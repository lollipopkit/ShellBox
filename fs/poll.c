#include "kernel/task.h"
#include <string.h>
#include <poll.h>
#include <fcntl.h>
#include <limits.h>
#include <time.h>
#include "misc.h"
#include "util/list.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "fs/fd.h"
#include "fs/poll.h"
#include "fs/real.h"

#include "fs/sockrestart.h"

// From kernel/calls.h - avoid circular include
extern _Noreturn void do_exit_group(int status);

#if defined(__linux__)
#include <sys/epoll.h>
#define HAVE_EPOLL 1
#elif defined(__APPLE__)
#include <sys/event.h>
#define HAVE_KQUEUE 1
#endif

static int real_poll_init(struct real_poll *real);
static void real_poll_close(struct real_poll *real);
struct real_poll_event {
#if HAVE_EPOLL
    struct epoll_event real;
#elif HAVE_KQUEUE
    struct kevent real;
#endif
};
static void *rpe_data(struct real_poll_event *rpe);
static int rpe_events(struct real_poll_event *rpe);
static int real_poll_wait(struct real_poll *real, struct real_poll_event *events, int max, struct timespec *timeout);
static int real_poll_update(struct real_poll *real, int fd, int types, void *data);

/// Safe close that skips fds 0-2 (stdin/stdout/stderr).
/// iOS guards these descriptors — closing them triggers EXC_GUARD and kills the app.
/// The iSH kernel operates on host fds that should never be 0/1/2, but race
/// conditions during fd reuse can occasionally produce them.
static inline void safe_close(int fd) {
    if (fd > 2)
        close(fd);
}

static uint64_t poll_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// lock order: fd, then poll

struct poll *poll_create(void) {
    struct poll *poll = malloc(sizeof(struct poll));
    if (poll == NULL)
        return ERR_PTR(_ENOMEM);
    int err = real_poll_init(&poll->real);
    if (err < 0) {
        // errno_map() reads errno, so it goes first: free() is not required to
        // leave errno alone.
        int mapped = errno_map();
        free(poll);
        return ERR_PTR(mapped);
    }
    poll->waiters = 0;
    poll->notify_pipe[0] = -1;
    poll->notify_pipe[1] = -1;
    list_init(&poll->poll_fds);
    list_init(&poll->pollfd_freelist);
    lock_init(&poll->lock);
    return poll;
}

static inline bool poll_fd_is_real(struct poll_fd *pollfd) {
    return pollfd->fd->ops->poll == realfs_poll;
}

// does not do its own locking
// Match on the (struct fd, guest fd number) pair — see fd_no in fs/poll.h.
static struct poll_fd *poll_find_fd(struct poll *poll, struct fd *fd, int fd_no) {
    struct poll_fd *poll_fd, *tmp;
    list_for_each_entry_safe(&poll->poll_fds, poll_fd, tmp, fds) {
        if (poll_fd->fd == fd && poll_fd->fd_no == fd_no)
            return poll_fd;
    }
    return NULL;
}

// Recompute the host (real) registration for `fd` in this poll as the UNION
// of the event types of every registration referring to it. Several guest
// registrations (different fd numbers, same description after dup) share one
// host registration, since the host backend is keyed by real_fd. Passing 0
// types deletes the host registration. poll->lock must be held.
static int poll_real_refresh(struct poll *poll, struct fd *fd) {
    int types = 0;
    struct poll_fd *pf;
    list_for_each_entry(&poll->poll_fds, pf, fds) {
        if (pf->fd == fd && !pf->oneshot_fired)
            types |= pf->types;
    }
    // The payload names the *description*, not one of the registrations of
    // it. One host registration stands for all of them, so there is no first
    // among them to speak for the rest — and naming one meant only that one
    // had its edge state cleared when the host reported an event.
    return real_poll_update(&poll->real, fd->real_fd, types, fd);
}

// See comment on pollfd_freelist for context
static void poll_fd_free(struct poll_fd *poll_fd) {
    struct poll *poll = poll_fd->poll;
    memset(poll_fd, 0xba, sizeof(*poll_fd));
    poll_fd->poll = NULL; // used to mark it as free
    list_add(&poll->pollfd_freelist, &poll_fd->fds);
}

bool poll_has_fd(struct poll *poll, struct fd *fd, int fd_no) {
    return poll_find_fd(poll, fd, fd_no) != NULL;
}

int poll_add_fd(struct poll *poll, struct fd *fd, int fd_no, int types, union poll_fd_info info) {
    int err;
    lock(&fd->poll_lock);
    lock(&poll->lock);

    // Checked here, under the lock that the insertion below also holds.
    // sys_epoll_ctl used to ask poll_has_fd first and then call this, so two
    // threads adding the same descriptor could both find it absent and both
    // add it.
    if (poll_find_fd(poll, fd, fd_no) != NULL) {
        err = _EEXIST;
        goto out;
    }

    struct poll_fd *poll_fd;
    if (!list_empty(&poll->pollfd_freelist)) {
        poll_fd = list_first_entry(&poll->pollfd_freelist, struct poll_fd, fds);
        list_remove(&poll_fd->fds);
    } else {
        poll_fd = malloc(sizeof(struct poll_fd));
        if (poll_fd == NULL) {
            err = _ENOMEM;
            goto out;
        }
    }
    poll_fd->fd = fd;
    poll_fd->fd_no = fd_no;
    poll_fd->poll = poll;
    poll_fd->types = types;
    poll_fd->info = info;
    poll_fd->triggered_types = 0;
    poll_fd->oneshot_fired = false;

    // Link first, then refresh the (shared) host registration to the union of
    // all registrations of this description in this poll.
    list_add(&fd->poll_fds, &poll_fd->polls);
    list_add(&poll->poll_fds, &poll_fd->fds);

    if (poll_fd_is_real(poll_fd)) {
        err = poll_real_refresh(poll, fd);
        if (err < 0) {
            err = errno_map();
            list_remove(&poll_fd->polls);
            list_remove(&poll_fd->fds);
            poll_fd_free(poll_fd);
            // A failed update leaves the description unregistered, including
            // for the registrations that were already there — the host
            // registration is shared. Rebuild it from what is left.
            poll_real_refresh(poll, fd);
            goto out;
        }
    }

    err = 0;
out:
    unlock(&poll->lock);
    unlock(&fd->poll_lock);
    return err;
}

int poll_del_fd(struct poll *poll, struct fd *fd, int fd_no) {
    int err;
    lock(&fd->poll_lock);
    lock(&poll->lock);
    struct poll_fd *poll_fd = poll_find_fd(poll, fd, fd_no);
    if (poll_fd == NULL) {
        err = _ENOENT;
        goto out;
    }

    bool is_real = poll_fd_is_real(poll_fd);
    list_remove(&poll_fd->polls);
    list_remove(&poll_fd->fds);
    poll_fd_free(poll_fd);

    // Refresh (or delete, if this was the last registration) the shared host
    // registration to the union of the remaining registrations.
    //
    // The removal itself cannot fail and has already happened, so this answers
    // 0 either way: reporting an error would tell the guest the descriptor is
    // still in the set when it is not, and EPOLL_CTL_DEL has nowhere to put
    // that. A host registration left behind costs a spurious wakeup, which the
    // readiness scan at the top of poll_wait discards.
    if (is_real) {
        if (poll_real_refresh(poll, fd) < 0)
            printk("poll_del_fd: host registration for real_fd %d not updated: %s\n",
                   fd->real_fd, strerror(errno));
    }

    err = 0;
out:
    unlock(&poll->lock);
    unlock(&fd->poll_lock);
    return err;
}

int poll_mod_fd(struct poll *poll, struct fd *fd, int fd_no, int types, union poll_fd_info info) {
    int err;
    lock(&fd->poll_lock);
    lock(&poll->lock);
    struct poll_fd *poll_fd = poll_find_fd(poll, fd, fd_no);
    if (poll_fd == NULL) {
        err = _ENOENT;
        goto out;
    }

    int old_types = poll_fd->types;
    union poll_fd_info old_info = poll_fd->info;
    int old_triggered = poll_fd->triggered_types;
    bool old_oneshot = poll_fd->oneshot_fired;

    poll_fd->types = types;
    poll_fd->info = info;
    poll_fd->triggered_types &= types;
    poll_fd->oneshot_fired = false; // EPOLL_CTL_MOD re-arms a fired oneshot

    if (poll_fd_is_real(poll_fd)) {
        err = poll_real_refresh(poll, fd);
        if (err < 0) {
            err = errno_map();
            // These four fields are what poll_real_refresh reads, so leaving
            // the new ones in place would describe a host registration that
            // was never made. Put them back and re-apply — the failed update
            // took the description's registration out entirely, including the
            // part that belonged to the other registrations sharing it.
            poll_fd->types = old_types;
            poll_fd->info = old_info;
            poll_fd->triggered_types = old_triggered;
            poll_fd->oneshot_fired = old_oneshot;
            poll_real_refresh(poll, fd);
            goto out;
        }
    }

    err = 0;
out:
    unlock(&poll->lock);
    unlock(&fd->poll_lock);
    return err;
}

void poll_cleanup_fd(struct fd *fd) {
    lock(&fd->poll_lock);
    struct poll_fd *poll_fd, *tmp;
    list_for_each_entry_safe(&fd->poll_fds, poll_fd, tmp, polls) {
        lock(&poll_fd->poll->lock);
        if (poll_fd_is_real(poll_fd))
            real_poll_update(&poll_fd->poll->real, fd->real_fd, 0, poll_fd);
        list_remove(&poll_fd->polls);
        list_remove(&poll_fd->fds);
        unlock(&poll_fd->poll->lock);
        poll_fd_free(poll_fd);
    }
    unlock(&fd->poll_lock);
}

// True if any registration in this set is ready right now, by the same rules
// the wait loop applies. This is what an epoll fd answers its own poll
// operation with, so that poll() and select() can wait on one.
//
// Takes poll->lock, and is called from the wait loop of a *different* poll
// with that one's lock held. The only edge is therefore outer to inner, and
// there is no path back: epoll_ctl refuses to put an epoll set inside another,
// so the members walked here are never epoll fds, and a poll() set is not
// something an fd can refer to at all.
bool poll_any_ready(struct poll *poll) {
    bool ready = false;
    lock(&poll->lock);
    struct poll_fd *poll_fd;
    list_for_each_entry(&poll->poll_fds, poll_fd, fds) {
        if (poll_fd->oneshot_fired)
            continue;
        struct fd *fd = poll_fd->fd;
        if (fd->ops->poll == NULL)
            continue;
        int types = fd->ops->poll(fd) & (poll_fd->types | POLL_HUP | POLL_ERR);
        if (poll_fd->types & POLL_EDGETRIGGERED)
            types &= ~poll_fd->triggered_types;
        if (types) {
            ready = true;
            break;
        }
    }
    unlock(&poll->lock);
    return ready;
}

// Diagnostic: dump each member fd of a poll set with its current poll state.
void poll_dump_members(struct poll *poll) {
    lock(&poll->lock);
    struct poll_fd *poll_fd;
    list_for_each_entry(&poll->poll_fds, poll_fd, fds) {
        struct fd *fd = poll_fd->fd;
        int ready = (fd->ops && fd->ops->poll) ? fd->ops->poll(fd) : -1;
        fprintf(stderr, "  [epoll-member] fd_no=%d type=%#x want=%#x ready=%#x triggered=%#x real_fd=%d\n",
                poll_fd->fd_no, fd->type, poll_fd->types, ready,
                poll_fd->triggered_types, fd->real_fd);
    }
    unlock(&poll->lock);
}

void poll_wakeup(struct fd *fd, int events) {
    struct poll_fd *poll_fd;
    lock(&fd->poll_lock);
    list_for_each_entry(&fd->poll_fds, poll_fd, polls) {
        struct poll *poll = poll_fd->poll;
        lock(&poll->lock);
        if (poll_fd->oneshot_fired) {
            unlock(&poll->lock);
            continue;
        }
        if (poll_fd->types & POLL_EDGETRIGGERED)
            poll_fd->triggered_types &= ~events;
        if (poll->notify_pipe[1] != -1)
            write(poll->notify_pipe[1], "", 1);
        unlock(&poll->lock);
        // oneshot?
    }
    unlock(&fd->poll_lock);
}

// Give up this thread's claim on the notify pipe, closing it if it was the
// last. poll->lock must be held.
static void poll_release_wait(struct poll *poll) {
    if (--poll->waiters == 0) {
        real_poll_update(&poll->real, poll->notify_pipe[0], 0, NULL);
        safe_close(poll->notify_pipe[0]);
        safe_close(poll->notify_pipe[1]);
        poll->notify_pipe[0] = -1;
        poll->notify_pipe[1] = -1;
    }
}

int poll_wait(struct poll *poll_, poll_callback_t callback, void *context, struct timespec *timeout) {
    lock(&poll_->lock);

    // acquire the pipe
    if (poll_->waiters++ == 0) {
        assert(poll_->notify_pipe[0] == -1 && poll_->notify_pipe[1] == -1);
        if (pipe(poll_->notify_pipe) < 0) {
            // Rolled back. Leaving waiters at one made the next caller take
            // the "someone else already made the pipe" path and register a
            // notify_pipe still holding -1, so one transient EMFILE poisoned
            // the poll object for the rest of its life.
            poll_->waiters--;
            unlock(&poll_->lock);
            return errno_map();
        }
        fcntl(poll_->notify_pipe[0], F_SETFL, O_NONBLOCK);
        fcntl(poll_->notify_pipe[1], F_SETFL, O_NONBLOCK);
        if (real_poll_update(&poll_->real, poll_->notify_pipe[0], POLL_READ, NULL) < 0) {
            // This registration is the only thing that turns a poll_wakeup
            // into a wakeup. Ignoring a failure here left the wait blind to
            // every notification, so it only ever came round on its one-second
            // slice — a poll that answers late rather than one that fails.
            int mapped = errno_map();
            safe_close(poll_->notify_pipe[0]);
            safe_close(poll_->notify_pipe[1]);
            poll_->notify_pipe[0] = -1;
            poll_->notify_pipe[1] = -1;
            poll_->waiters--;
            unlock(&poll_->lock);
            return mapped;
        }
    }

    // The guest's timeout as a deadline, fixed once. Each pass below waits at
    // most a second so signals and group exit stay responsive, and used to
    // deduct a flat second per *timed-out* pass — which left every other way
    // of going round the loop free. A wakeup that turned out not to be for us
    // restarted the whole second, so a stream of unrelated wakeups held a
    // timed wait open indefinitely. The caller's struct is no longer written
    // through either.
    bool timed = timeout != NULL;
    uint64_t deadline_ns = 0;
    if (timed)
        deadline_ns = poll_now_ns() +
            (uint64_t)timeout->tv_sec * 1000000000ULL + (uint64_t)timeout->tv_nsec;

    int res = 0;
    while (true) {
        // check if any fds are ready
        struct poll_fd *poll_fd, *tmp;
        list_for_each_entry_safe(&poll_->poll_fds, poll_fd, tmp, fds) {
            if (poll_fd->oneshot_fired)
                continue;
            struct fd *fd = poll_fd->fd;
            int poll_types = 0;
            if (fd->ops->poll)
                poll_types = fd->ops->poll(fd);
            poll_types &= poll_fd->types | POLL_HUP | POLL_ERR;
            if (poll_fd->types & POLL_EDGETRIGGERED) {
                poll_types &= ~poll_fd->triggered_types;
            }
            if (poll_types) {
                // A callback that answers 0 did not take the event — the
                // epoll batch is full. Everything below marks the event as
                // delivered, so running it anyway disarmed a EPOLLONESHOT
                // registration, and consumed an edge, for something the guest
                // was never told about. Leave it for the next call.
                if (callback(context, poll_types, poll_fd->info) != 1)
                    continue;
                res++;

                // The real poll does not actually get the FDs set as oneshot.
                // But this loop is done while holding the lock, so only one
                // thread can get each oneshot event. This doesn't solve the
                // thundering herd problem at all, but at least the semantics
                // are right. I'll just leave that as a TODO.
                if (poll_fd->types & POLL_ONESHOT) {
                    // Disable in place; do NOT unlink. We hold only
                    // poll->lock here, but poll_fd->polls is walked by
                    // poll_wakeup under fd->poll_lock — unlinking/freeing
                    // raced it and crashed the tty input thread (SIGSEGV in
                    // poll_wakeup when bun registers stdin with
                    // EPOLLONESHOT). Linux semantics also keep a fired
                    // oneshot registered, disabled until EPOLL_CTL_MOD.
                    poll_fd->oneshot_fired = true;
                    if (poll_fd_is_real(poll_fd)) {
                        // Refresh the shared host registration to the union
                        // of the still-armed registrations.
                        poll_real_refresh(poll_, fd);
                    }
                }

                if (poll_fd->types & POLL_EDGETRIGGERED) {
                    poll_fd->triggered_types |= poll_types;
                }
            }
        }
        if (res > 0)
            break;

        bool signal_pending = false;
        if (current->sighand != NULL) {
            lock(&current->sighand->lock);
            signal_pending = !!(current->pending & ~current->blocked);
            unlock(&current->sighand->lock);
        }
        if (signal_pending) {
            res = _EINTR;
            break;
        }

        // Checked here, not after the unlock below: every other way out of
        // this loop leaves with poll_->lock held and with no outstanding
        // sockrestart_begin_listen_wait, and the code after it expects both.
        struct timespec slice = {.tv_sec = 1, .tv_nsec = 0};
        if (timed) {
            uint64_t now = poll_now_ns();
            if (now >= deadline_ns)
                break; // the guest's timeout is up; res stays 0
            uint64_t remaining = deadline_ns - now;
            if (remaining < 1000000000ULL) {
                slice.tv_sec = 0;
                slice.tv_nsec = (long)remaining;
            }
        }

        // wait for a ready notification
        list_for_each_entry(&poll_->poll_fds, poll_fd, fds) {
            sockrestart_begin_listen_wait(poll_fd->fd);
        }
        unlock(&poll_->lock);
        int err;
        int saved_errno;
        struct real_poll_event e[4];
        // Use a bounded timeout to avoid indefinite blocks. Guest signal
        // delivery via SIGUSR1 wakes kevent, but if the signal arrives
        // between our pending check and kevent entry, we'd block forever.
        // A 5-second cap ensures we re-check signals periodically.
        // Cap all waits to 1 second to avoid macOS condvar issues.
        // pthread_cond_timedwait_relative_np can block forever under
        // thread contention. Short caps ensure we re-check periodically.
        struct timespec *wait_timeout = &slice;
        current->blocking = true;
        do {
            err = real_poll_wait(&poll_->real, e, sizeof(e)/sizeof(e[0]), wait_timeout);
            saved_errno = errno;  // save immediately before anything clobbers it
        } while (saved_errno == EINTR && sockrestart_should_restart_listen_wait());
        current->blocking = false;
        // Only update last_unblocked_ns when actual events were received.
        // Timeout returns (err==0) don't count as real progress — the poll_wait
        // loop is just cycling. This prevents the deadlock detector from being
        // fooled by idle poll_wait loops during exit cleanup.
        if (err > 0) {
            struct timespec _ts;
            clock_gettime(CLOCK_MONOTONIC, &_ts);
            uint64_t now = (uint64_t)_ts.tv_sec * 1000000000ULL + _ts.tv_nsec;
            current->last_unblocked_ns = now;
            atomic_store_explicit(&current->group->last_progress_ns, now,
                                  memory_order_relaxed);
        }
        // Timed out on this pass' slice. Loop back to re-check fd readiness
        // and pending signals; the deadline check at the top of the wait is
        // what ends a timed wait.
        if (err == 0) {
            lock(&poll_->lock);
            list_for_each_entry(&poll_->poll_fds, poll_fd, fds) {
                sockrestart_end_listen_wait(poll_fd->fd);
            }
            // Check for group exit so blocking threads unblock promptly
            if (current->group->doing_group_exit) {
                res = _EINTR;
                break;
            }
            // Safety valve: if no thread in this process group has
            // done real work for >60s and there are no live child
            // processes, force exit. Catches V8/libuv exit cleanup
            // hangs where the event loop spins idle forever.
            //
            // Exceptions:
            //   - pid 1 (init): legitimately idles, killing halts the system
            //   - processes with a controlling TTY: interactive shells idle
            //     waiting for user input and must not be killed
            if (current->pid != 1 && current->group->tty == NULL) {
                struct timespec _now;
                clock_gettime(CLOCK_MONOTONIC, &_now);
                uint64_t now_ns = (uint64_t)_now.tv_sec * 1000000000ULL + _now.tv_nsec;
                uint64_t last = atomic_load_explicit(
                    &current->group->last_progress_ns, memory_order_relaxed);
                int64_t idle_s = (int64_t)(now_ns - last) / 1000000000LL;
                if (idle_s >= 60) {
                    bool has_live_children = false;
                    int thread_count = 0;
                    lock(&pids_lock);
                    lock(&current->group->lock);
                    struct task *t_iter;
                    list_for_each_entry(&current->group->threads, t_iter, group_links) {
                        thread_count++;
                        struct task *child;
                        list_for_each_entry(&t_iter->children, child, siblings) {
                            if (child->group != current->group && !child->zombie)
                                has_live_children = true;
                        }
                    }
                    unlock(&current->group->lock);
                    unlock(&pids_lock);
                    if (!has_live_children) {
                        if (ish_exec_trace())
                            printk("SAFETY-VALVE[poll]: pid=%d idle %llds, %d threads → exit_group\n",
                                   current->pid, (long long)idle_s, thread_count);
                        // do_exit_group does not come back, so this wait has
                        // to be given up here. Leaving it counted kept the
                        // notify pipe open for the life of the poll object,
                        // and the next poll_wait on it tripped the assert
                        // that the pipe is -1 when the first waiter arrives.
                        // The lock has to go too — exit runs fd closes, and
                        // one of them may be this poll.
                        poll_release_wait(poll_);
                        unlock(&poll_->lock);
                        do_exit_group(0);
                    }
                }
            }
            continue;
        }
        lock(&poll_->lock);
        list_for_each_entry(&poll_->poll_fds, poll_fd, fds) {
            sockrestart_end_listen_wait(poll_fd->fd);
        }

        if (err < 0) {
            errno = saved_errno;
            res = errno_map();
            break;
        }

        // deal with any edge-triggered notifications: every registration of
        // the description the host named, not just the one that happened to
        // come first in the list. A second EPOLLET registration of the same
        // descriptor used to keep an edge it had already been told about and
        // stop reporting readiness for good.
        for (int i = 0; i < err; i++) {
            struct fd *triggered_fd = rpe_data(&e[i]);
            if (triggered_fd == NULL)
                continue; // the notify pipe, which registers no payload
            int triggered_events = rpe_events(&e[i]);
            struct poll_fd *edge_pf;
            list_for_each_entry(&poll_->poll_fds, edge_pf, fds) {
                if (edge_pf->fd == triggered_fd &&
                        (edge_pf->types & POLL_EDGETRIGGERED))
                    edge_pf->triggered_types &= ~triggered_events;
            }
        }

        char fuck;
        if (read(poll_->notify_pipe[0], &fuck, 1) < 0 && errno != EAGAIN) {
            res = errno_map();
            break;
        }
    }

    // release the pipe
    poll_release_wait(poll_);

    unlock(&poll_->lock);
    return res;
}

void poll_destroy(struct poll *poll) {
    struct poll_fd *poll_fd;
    struct poll_fd *tmp;

    // The lock order is fd, then poll, so the member list cannot be walked
    // with poll->lock held — and walking it with no lock at all is what this
    // used to do, while poll_cleanup_fd removed entries from the same list
    // under poll->lock. Instead: read the head under poll->lock, drop it, take
    // the two in order, and look the entry up again, since it may be gone.
    //
    // No poll_wait can be running: the epoll fd's refcount reached zero to get
    // here, and a thread inside epoll_wait holds a syscall reference to it.
    //
    // TODO: one window is left. If the member fd's last reference goes between
    // the unlock and the lock below, poll_cleanup_fd runs and frees it, and
    // fd->poll_lock is taken on freed memory. Closing that needs a reference
    // held across the gap, which needs struct fd's refcount to be atomic —
    // it is a plain int today.
    for (;;) {
        lock(&poll->lock);
        if (list_empty(&poll->poll_fds)) {
            unlock(&poll->lock);
            break;
        }
        poll_fd = list_first_entry(&poll->poll_fds, struct poll_fd, fds);
        struct fd *fd = poll_fd->fd;
        int fd_no = poll_fd->fd_no;
        unlock(&poll->lock);

        lock(&fd->poll_lock);
        lock(&poll->lock);
        poll_fd = poll_find_fd(poll, fd, fd_no);
        if (poll_fd != NULL) {
            list_remove(&poll_fd->polls);
            list_remove(&poll_fd->fds);
            // Onto the freelist rather than freed here, so there is one place
            // below that frees, and so a stale pointer finds 0xba rather than
            // a live registration.
            poll_fd_free(poll_fd);
        }
        unlock(&poll->lock);
        unlock(&fd->poll_lock);
    }

    list_for_each_entry_safe(&poll->pollfd_freelist, poll_fd, tmp, fds) {
        list_remove(&poll_fd->fds);
        free(poll_fd);
    }

    real_poll_close(&poll->real);
    free(poll);
}

// Platform-specific real_poll implementations

#if HAVE_EPOLL

static int real_poll_init(struct real_poll *real) {
    real->fd = epoll_create1(0);
    if (real->fd < 0)
        return -1;
    return 0;
}

static int real_poll_wait(struct real_poll *real, struct real_poll_event *events, int max, struct timespec *timeout) {
    int timeout_millis = -1;
    if (timeout != NULL)
        timeout_millis = timeout->tv_sec * 1000 + timeout->tv_nsec / 1000000;
    return epoll_wait(real->fd, (struct epoll_event *) events, max, timeout_millis);
}

static int real_poll_update(struct real_poll *real, int fd, int types, void *data) {
    types &= ~EPOLLONESHOT;
    if (types == 0)
        return epoll_ctl(real->fd, EPOLL_CTL_DEL, fd, NULL);
    struct epoll_event epevent = {.events = types, .data.ptr = data};
    int err = epoll_ctl(real->fd, EPOLL_CTL_MOD, fd, &epevent);
    if (err < 0 && errno == ENOENT)
        err = epoll_ctl(real->fd, EPOLL_CTL_ADD, fd, &epevent);
    return err;
}

static void *rpe_data(struct real_poll_event *rpe) {
    return rpe->real.data.ptr;
}
static int rpe_events(struct real_poll_event *rpe) {
    return rpe->real.events;
}

#elif HAVE_KQUEUE

static int real_poll_init(struct real_poll *real) {
    real->fd = kqueue();
    if (real->fd < 0)
        return -1;
    return 0;
}

static int real_poll_update(struct real_poll *real, int fd, int types, void *data) {
    struct kevent e[3] = {
        {.filter = EVFILT_READ, .flags = types & (POLL_READ | POLL_HUP) ? EV_ADD : EV_DELETE},
        {.filter = EVFILT_WRITE, .flags = types & POLL_WRITE ? EV_ADD : EV_DELETE},
        {.filter = EVFILT_EXCEPT, .flags = types & POLL_ERR ? EV_ADD : EV_DELETE},
    };
    if (!(types & POLL_READ) && types & POLL_HUP) {
        // Set the low water mark really high so we'll only get woken up on a hangup
        e[0].fflags = NOTE_LOWAT;
        e[0].data = INT_MAX;
    }
    for (int i = 0; i < 3; i++) {
        e[i].ident = fd;
        e[i].udata = data;
        e[i].flags |= EV_RECEIPT;
        if (types & POLL_EDGETRIGGERED)
            e[i].flags |= EV_CLEAR;
    }

    // Receipts land in their own array: the returned events overwrite what
    // was submitted, so keeping them apart is what lets the check below know
    // which change each result belongs to.
    struct kevent receipts[3];
    int n = kevent(real->fd, e, 3, receipts, 3, NULL);
    if (n < 0)
        return -1;
    // EV_RECEIPT makes every change report its result in `data`, and
    // returning the raw count called all three applied whatever they said —
    // so a registration the host refused looked installed and no wakeup ever
    // arrived. Only the ADDs are worth failing for: EV_DELETE is issued
    // unconditionally to mean "no longer interested", so ENOENT is its
    // ordinary answer, and EVFILT_EXCEPT is best-effort on descriptions that
    // do not support it.
    for (int i = 0; i < n && i < 3; i++) {
        if (!(receipts[i].flags & EV_ERROR) || receipts[i].data == 0)
            continue;
        if (!(e[i].flags & EV_ADD) || e[i].filter == EVFILT_EXCEPT)
            continue;
        int failed_errno = (int) receipts[i].data;
        // kevent applies each change independently, so the filters that did
        // succeed are registered even though this call is about to report
        // failure — and the caller reads that as "nothing is registered".
        // There is no earlier state to put back, because the changes that
        // applied have already overwritten it; take all three out so the
        // description really is unregistered, which is what the caller's own
        // rollback then rebuilds from.
        struct kevent undo[3], undo_receipts[3];
        for (int j = 0; j < 3; j++)
            EV_SET(&undo[j], fd, e[j].filter, EV_DELETE | EV_RECEIPT, 0, 0, NULL);
        kevent(real->fd, undo, 3, undo_receipts, 3, NULL);
        errno = failed_errno;
        return -1;
    }
    return n;
}

static int real_poll_wait(struct real_poll *real, struct real_poll_event *events, int max, struct timespec *timeout) {
    return kevent(real->fd, NULL, 0, (struct kevent *) events, max, timeout);
}

static void *rpe_data(struct real_poll_event *rpe) {
    return rpe->real.udata;
}
static int rpe_events(struct real_poll_event *rpe) {
    if (rpe->real.filter == EVFILT_READ) {
        int events = 0;
        if (rpe->real.data > 0)
            events |= POLL_READ;
        if (rpe->real.flags & EV_EOF)
            events |= POLL_HUP;
        return events;
    }
    if (rpe->real.filter == EVFILT_WRITE) return POLL_WRITE;
    if (rpe->real.filter == EVFILT_EXCEPT) return POLL_ERR;
    return 0;
}

#endif

static void real_poll_close(struct real_poll *real) {
    safe_close(real->fd);
}

