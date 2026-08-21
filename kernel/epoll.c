#include <stdatomic.h>
#include <sys/stat.h>
#include "kernel/calls.h"
#include "fs/poll.h"

static struct fd_ops epoll_ops;

fd_t sys_epoll_create(int_t flags) {
    STRACE("epoll_create(%#x)", flags);
    if (flags & ~(O_CLOEXEC_))
        return _EINVAL;

    struct fd *fd = adhoc_fd_create(&epoll_ops);
    if (fd == NULL)
        return _ENOMEM;
    struct poll *poll = poll_create();
    if (IS_ERR(poll)) {
        // fd_create zeroes the struct, so epollfd.poll is NULL here and
        // epoll_close has nothing to destroy.
        fd_close(fd);
        return PTR_ERR(poll);
    }
    fd->epollfd.poll = poll;
    return f_install(fd, flags);
}
fd_t sys_epoll_create0(void) {
    return sys_epoll_create(0);
}

struct epoll_event_ {
    uint32_t events;
#ifdef GUEST_ARM64
    uint32_t __pad;
#endif
    uint64_t data;
}
#ifndef GUEST_ARM64
__attribute__((packed))
#endif
;

#define EPOLL_CTL_ADD_ 1
#define EPOLL_CTL_DEL_ 2
#define EPOLL_CTL_MOD_ 3
#define EPOLLET_ (1 << 31)
#define EPOLLONESHOT_ (1 << 30)

// The most events one epoll_wait will return, and so the size of the array it
// puts on the stack. Well past what any real event loop asks for — Node and
// libuv use 1024, musl's own wrappers less.
#define EPOLL_MAX_EVENTS 1024

int_t sys_epoll_ctl(fd_t epoll_f, int_t op, fd_t f, addr_t event_addr) {
    STRACE("epoll_ctl(%d, %d, %d, %#x)", epoll_f, op, f, event_addr);
    struct fd *epoll = f_get(epoll_f);
    if (epoll == NULL)
        return _EBADF;
    if (epoll->ops != &epoll_ops)
        return _EINVAL;
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;

    // An epoll set inside an epoll set is not supported, and saying so is the
    // point: epoll_ops has no .poll, so poll_wait's readiness scan read the
    // inner set as never ready and the registration silently never fired. A
    // guest that probes for nested epoll can fall back on an error; it cannot
    // fall back on an event loop that just stops.
    //
    // TODO: support it. Two pieces are missing, and the second is the reason
    // this is a refusal rather than a partial implementation:
    //   - a .poll on epoll_ops that scans the inner set's members. On its own
    //     this only gets readiness noticed on poll_wait's one-second slice,
    //     since the inner set's descriptors are not in the outer host queue.
    //   - propagating poll_wakeup up the nesting. That has to run outside
    //     poll->lock: the readiness scan walks outer→inner, so waking
    //     inner→outer with locks held gives two orders over the same pair.
    // Cycle and depth rejection (Linux: EINVAL for self, ELOOP beyond four)
    // comes with it, and is load-bearing rather than tidiness — a cycle would
    // make either of the walks above recurse forever.
    //
    // An epoll fd passed to poll() or select() is a different case and is not
    // rejected: epoll_ops has a .poll now, so those report readiness. It is
    // this refusal that makes that safe, by keeping the nesting one level
    // deep and free of cycles.
    if (op == EPOLL_CTL_ADD_ && fd->ops == &epoll_ops) {
        // Said once. A guest that probes for nested epoll does so from a loop,
        // and the point of the message is that the refusal is deliberate —
        // which one line makes as well as thousands. Exchanged rather than
        // tested and set: epoll_ctl runs on every thread, and a plain bool
        // read-then-write is a data race whose whole purpose is to be read
        // concurrently.
        static _Atomic bool said;
        if (!atomic_exchange(&said, true)) {
            printk("epoll_ctl: refusing to nest epoll set %d inside %d (pid=%d): "
                   "unsupported, reported once\n", f, epoll_f, current->pid);
        }
        return _EINVAL;
    }

    // Regular files (and directories) are not pollable: they are always
    // "ready", so Linux rejects adding them to an epoll set with EPERM. iSH
    // used to accept them, which makes bun's fs.WriteStream take its polling
    // path (instead of the EPERM fallback) and then throw EEXIST on the second
    // registration of the same fd. Match Linux and reject with EPERM.
    if (op == EPOLL_CTL_ADD_ && (S_ISREG(fd->type) || S_ISDIR(fd->type)))
        return _EPERM;

    if (op == EPOLL_CTL_DEL_)
        return poll_del_fd(epoll->epollfd.poll, fd, f);
    // Anything that is not ADD, DEL or MOD used to fall through to MOD.
    if (op != EPOLL_CTL_ADD_ && op != EPOLL_CTL_MOD_)
        return _EINVAL;

    struct epoll_event_ event;
    if (user_get(event_addr, event))
        return _EFAULT;
    STRACE(" {events: %#x, data: %#x}", event.events, event.data);

    if (op == EPOLL_CTL_ADD_) {
        // poll_add_fd answers EEXIST itself, holding the poll lock across the
        // check and the insertion. Asking poll_has_fd first and then adding
        // let two threads registering the same descriptor both find it absent
        // and both add it.
        return poll_add_fd(epoll->epollfd.poll, fd, f, event.events, (union poll_fd_info) event.data);
    } else {
        return poll_mod_fd(epoll->epollfd.poll, fd, f, event.events, (union poll_fd_info) event.data);
    }
}

struct epoll_context {
    struct epoll_event_ *events;
    int n;
    int max_events;
};

static int epoll_callback(void *context, int types, union poll_fd_info info) {
    struct epoll_context *c = context;
    if (c->n >= c->max_events)
        return 0;
    c->events[c->n++] = (struct epoll_event_) {.events = types, .data = info.num};
    return 1;
}

int_t sys_epoll_wait(fd_t epoll_f, addr_t events_addr, int_t max_events, int_t timeout) {
    STRACE("epoll_wait(%d, %#x, %d, %d)", epoll_f, events_addr, max_events, timeout);
    struct fd *epoll = f_get(epoll_f);
    if (epoll == NULL)
        return _EBADF;
    if (epoll->ops != &epoll_ops)
        return _EINVAL;

    struct timespec timeout_ts;
    if (timeout >= 0) {
        timeout_ts.tv_sec = timeout / 1000;
        timeout_ts.tv_nsec = (timeout % 1000) * 1000000;
    }
    // Clamped, not rejected: epoll_wait may always return fewer events than
    // asked for, so a guest that passes a large max_events is answered
    // correctly with a smaller batch. It sized a stack array before, with
    // nothing but the guest's word for how big.
    if (max_events > EPOLL_MAX_EVENTS)
        max_events = EPOLL_MAX_EVENTS;
    if (max_events <= 0)
        return _EINVAL;
    struct epoll_event_ events[max_events];

    struct epoll_context context = {.events = events, .n = 0, .max_events = max_events};
    STRACE("...\n");
    { extern char *getenv(const char *);
      static addr_t wa = 1;
      if (wa == 1) { const char *e = getenv("ISH_EPOLL_WATCH_MUTEX"); wa = e ? strtoull(e,NULL,16) : 0; }
      if (wa) {
          dword_t mv = 0; dword_t *vp = mem_ptr(current->mem, wa, MEM_READ); if (vp) mv = *vp;
          if (mv != 0) {  // only log when the watched mutex is held
              fprintf(stderr, "[epoll] pid=%d enters epoll_wait timeout=%d while mutex 0x%llx = 0x%x; members:\n",
                      current->pid, timeout, (unsigned long long)wa, mv);
              extern void poll_dump_members(struct poll *p);
              poll_dump_members(epoll->epollfd.poll);
          }
      } }
    int res = poll_wait(epoll->epollfd.poll, epoll_callback, &context, timeout < 0 ? NULL : &timeout_ts);
    STRACE("%d end epoll_wait", current->pid);
    if (res >= 0) {
        for (int i = 0; i < res; i++) {
            STRACE(" {events: %#x, data: %#x}", events[i].events, events[i].data);
        }
        if (user_write(events_addr, events, sizeof(struct epoll_event_) * res))
            return _EFAULT;
    } else if (res != _EINTR) {
        // Host-level errors from kqueue/kevent should not propagate to the guest
        // as unexpected errnos. libuv asserts that epoll_pwait only fails with
        // EINTR; anything else (EBADF from kqueue race, etc.) crashes the guest.
        // Convert to EINTR so the guest's event loop retries gracefully.
        printk("epoll_wait: converting error %d to EINTR (pid=%d)\n", res, current->pid);
        res = _EINTR;
    }
    return res;
}

int_t sys_epoll_pwait(fd_t epoll_f, addr_t events_addr, int_t max_events, int_t timeout, addr_t sigmask_addr, dword_t sigsetsize) {
    sigset_t_ mask;
    if (sigmask_addr != 0) {
        int err = user_get_sigset(sigmask_addr, sigsetsize, &mask);
        if (err)
            return err;
        sigmask_set_temp(mask);
    }

    return sys_epoll_wait(epoll_f, events_addr, max_events, timeout);
}

static int epoll_close(struct fd *fd) {
    // NULL when sys_epoll_create failed between making the fd and making the
    // poll object.
    if (fd->epollfd.poll != NULL)
        poll_destroy(fd->epollfd.poll);
    return 0;
}

// An epoll set is readable when any of its registrations is ready. Linux
// allows poll() and select() on an epoll fd; without this, epoll_ops had no
// poll operation at all, so the wait loop's readiness scan read the set as
// never ready and a guest waiting on one waited forever.
//
// Bounded at one level and free of cycles because epoll_ctl refuses to put an
// epoll set inside another: the members poll_any_ready walks are never epoll
// fds, so the only lock edge is outer poll to inner poll with no path back.
//
// TODO: nothing wakes the outer wait when the inner set becomes ready — the
// inner members are not registered in the outer host queue. A poll whose only
// event source is an epoll fd therefore notices on poll_wait's one-second
// slice rather than at once; in a mixed set any other activity brings the scan
// round and it is seen immediately.
//
// Closing that means poll_wakeup travelling up the nesting. Two things that
// look like blockers are not:
//   - Lock order works out. The house order is fd before poll, and the walk
//     would be member->poll_lock, inner->lock, release inner, owner->poll_lock,
//     outer->lock — which keeps it. The readiness scan's edge is outer->lock to
//     inner->lock and it takes no fd lock, so the two do not close a cycle.
//   - The owner cannot be freed underneath it. fd_close on an epoll fd reaches
//     free() only through epoll_close and poll_destroy, and poll_destroy takes
//     each member's poll_lock — the very lock the walk is holding.
// What makes it a change of its own rather than a line here is cost and the
// bookkeeping that avoids it. poll_wakeup runs on every socket write, pipe
// write and tty byte, for every fd in every process. Propagating unconditionally
// adds a lock round-trip per wakeup for every fd in any epoll set — which under
// Node, bun or Python is all of them — to reach an epoll fd that is almost
// never itself watched. Avoiding that needs a "this set is watched" flag kept
// in step across poll_add_fd, poll_del_fd and poll_cleanup_fd, and a stale one
// leaves a freed struct fd * in the hottest path in the emulator. That wants a
// stress test, which this tree has no harness for.
static int epoll_poll(struct fd *fd) {
    if (fd->epollfd.poll == NULL)
        return 0;
    return poll_any_ready(fd->epollfd.poll) ? POLL_READ : 0;
}

static struct fd_ops epoll_ops = {
    .close = epoll_close,
    .poll = epoll_poll,
};
