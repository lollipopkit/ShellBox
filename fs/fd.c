#include <stdlib.h>
#include <string.h>
#include "debug.h"
#include "kernel/calls.h"
#include "kernel/resource.h"
#include "kernel/fs.h"
#include "fs/poll.h"
#include "fs/fd.h"
#include "fs/inode.h"

struct fd *fd_create(const struct fd_ops *ops) {
    struct fd *fd = malloc(sizeof(struct fd));
    if (fd == NULL)
        return NULL;
    *fd = (struct fd) {};
    fd->ops = ops;
    fd->refcount = 1;
    fd->flags = 0;
    fd->mount = NULL;
    fd->offset = 0;
    list_init(&fd->poll_fds);
    list_init(&fd->pidfd_links);
    lock_init(&fd->poll_lock);
    lock_init(&fd->lock);
    cond_init(&fd->cond);
    return fd;
}

struct fd *fd_retain(struct fd *fd) {
    fd->refcount++;
    return fd;
}

int fd_close(struct fd *fd) {
    int err = 0;
    if (--fd->refcount == 0) {
        poll_cleanup_fd(fd);
        if (fd->ops->close)
            err = fd->ops->close(fd);
        // see comment in close in kernel/fs.h
        if (fd->mount && fd->mount->fs->close && fd->mount->fs->close != fd->ops->close) {
            int new_err = fd->mount->fs->close(fd);
            if (new_err < 0)
                err = new_err;
        }

        if (fd->inode)
            inode_release(fd->inode);
        if (fd->mount)
            mount_release(fd->mount);
        free(fd->change_path);
        free(fd);
    }
    return err;
}

static int fdtable_resize(struct fdtable *table, unsigned size);

struct fdtable *fdtable_new(int size) {
    struct fdtable *fdt = malloc(sizeof(struct fdtable));
    if (fdt == NULL)
        return ERR_PTR(_ENOMEM);
    fdt->refcount = 1;
    fdt->size = 0;
    fdt->files = NULL;
    fdt->cloexec = NULL;
    lock_init(&fdt->lock);
    int err = fdtable_resize(fdt, size);
    if (err < 0) {
        free(fdt);
        return ERR_PTR(err);
    }
    return fdt;
}

static int fdtable_close(struct fdtable *table, fd_t f);

// FIXME this looks like it has the classic refcount UAF
void fdtable_release(struct fdtable *table) {
    lock(&table->lock);
    if (--table->refcount == 0) {
        for (fd_t f = 0; (unsigned) f < table->size; f++)
            fdtable_close(table, f);
        free(table->files);
        free(table->cloexec);
        unlock(&table->lock);
        free(table);
    } else {
        unlock(&table->lock);
    }
}

static int fdtable_resize(struct fdtable *table, unsigned size) {
    // currently the only legitimate use of this is to expand the table
    assert(size > table->size);

    struct fd **files = malloc(sizeof(struct fd *) * size);
    if (files == NULL)
        return _ENOMEM;
    memset(files, 0, sizeof(struct fd *) * size);
    if (table->files)
        memcpy(files, table->files, sizeof(struct fd *) * table->size);

    bits_t *cloexec = malloc(BITS_SIZE(size));
    if (cloexec == NULL) {
        free(files);
        return _ENOMEM;
    }
    memset(cloexec, 0, BITS_SIZE(size));
    if (table->cloexec)
        memcpy(cloexec, table->cloexec, BITS_SIZE(table->size));

    free(table->files);
    table->files = files;
    free(table->cloexec);
    table->cloexec = cloexec;
    table->size = size;
    return 0;
}

struct fdtable *fdtable_copy(struct fdtable *table) {
    lock(&table->lock);
    int size = table->size;
    struct fdtable *new_table = fdtable_new(size);
    if (IS_ERR(new_table)) {
        unlock(&table->lock);
        return new_table;
    }
    memcpy(new_table->files, table->files, sizeof(struct fd *) * size);
    for (fd_t f = 0; f < size; f++)
        if (new_table->files[f])
            new_table->files[f]->refcount++;
    memcpy(new_table->cloexec, table->cloexec, BITS_SIZE(size));
    unlock(&table->lock);
    return new_table;
}

static int fdtable_expand(struct fdtable *table, fd_t max) {
    unsigned size = max + 1;
    if (size > rlimit(RLIMIT_NOFILE_))
        return _EMFILE;
    if (table->size >= size)
        return 0;
    return fdtable_resize(table, max + 1);
}

struct fd *fdtable_get(struct fdtable *table, fd_t f) {
    // `table`, not current->files. Callers pass a table that is not the
    // current task's — fdtable_copy and the /proc/pid/fd readers do — and
    // bounding the index by the wrong table's size read past the end of the
    // one actually indexed.
    if (f < 0 || (unsigned) f >= table->size)
        return NULL;
    return table->files[f];
}

// Records a reference in the calling task's per-syscall list, so it can be
// released in one place when the syscall returns — see syscall_refs_drain.
// Returns false only if the list could not grow.
static bool syscall_ref_hold(struct fd *fd) {
    struct task *task = current;
    // One entry per description, however many times the syscall looks it up.
    // Without this a loop over the guest's own count — sys_recvmmsg calling
    // sys_recvmsg, sys_poll over a large set — would grow the list by an
    // entry per iteration; deduplicated, it cannot exceed the number of open
    // descriptors. n is one in almost every syscall, so the scan is a
    // comparison.
    for (unsigned i = 0; i < task->syscall_refs_n; i++)
        if (task->syscall_refs[i] == fd)
            return true;
    if (task->syscall_refs_n == task->syscall_refs_cap) {
        unsigned cap = task->syscall_refs_cap ? task->syscall_refs_cap * 2 : 8;
        struct fd **refs = realloc(task->syscall_refs, cap * sizeof(*refs));
        if (refs == NULL)
            return false;
        task->syscall_refs = refs;
        task->syscall_refs_cap = cap;
    }
    task->syscall_refs[task->syscall_refs_n++] = fd_retain(fd);
    return true;
}

void syscall_refs_drain(void) {
    struct task *task = current;
    if (task == NULL)
        return;
    for (unsigned i = 0; i < task->syscall_refs_n; i++)
        fd_close(task->syscall_refs[i]);
    task->syscall_refs_n = 0;
}

// Only the array. The references themselves are gone by now: a syscall that
// returns is drained by the dispatcher and one that does not — do_exit and
// what it is reached from — drains on the way in. This runs from task_destroy
// with pids_lock held, which is no place to be calling ops->close.
void syscall_refs_free(struct task *task) {
    free(task->syscall_refs);
    task->syscall_refs = NULL;
    task->syscall_refs_n = task->syscall_refs_cap = 0;
}

// Takes a reference under the same lock fdtable_close clears the slot with,
// so the description cannot be freed between the lookup and the use. Without
// it every caller worked on a descriptor another thread was free to close
// underneath it: the syscall blocks in ops->read or ops->poll, close(2)
// arrives on the last reference, and the struct fd is freed while in use.
//
// The reference is released when the syscall returns rather than by each
// caller. There are sixty-odd of those, most with several ways out, and one
// missed release is a descriptor that never closes while one release too many
// is the use-after-free this exists to prevent. Holding until the syscall
// ends is also exactly the interval the pointer is live for.
//
// A caller that hands the reference on — to f_install, say — takes its own
// with fd_retain first; the drain still owns this one.
struct fd *f_get(fd_t f) {
    lock(&current->files->lock);
    struct fd *fd = fdtable_get(current->files, f);
    if (fd != NULL && !syscall_ref_hold(fd)) {
        // Out of memory growing the list. The pointer is no worse than it was
        // before any of this, and failing the syscall outright would be a new
        // way for an ordinary read to fail.
        unlock(&current->files->lock);
        return fd;
    }
    unlock(&current->files->lock);
    return fd;
}

static fd_t f_install_start(struct fd *fd, fd_t start) {
    assert(start >= 0);
    struct fdtable *table = current->files;
    unsigned size = rlimit(RLIMIT_NOFILE_);
    if (size > table->size)
        size = table->size;

    fd_t f;
    for (f = start; (unsigned) f < size; f++)
        if (table->files[f] == NULL)
            break;
    if ((unsigned) f >= size) {
        int err = fdtable_expand(table, f);
        if (err < 0)
            f = err;
    }

    if (f >= 0) {
        table->files[f] = fd;
        bit_clear(f, table->cloexec);
    } else {
        fd_close(fd);
    }
    return f;
}

fd_t f_install(struct fd *fd, int flags) {
    lock(&current->files->lock);
    fd_t f = f_install_start(fd, 0);
    if (f >= 0) {
        if (flags & O_CLOEXEC_)
            bit_set(f, current->files->cloexec);
        if (flags & O_NONBLOCK_)
            fd_setflags(fd, O_NONBLOCK_);
    }
    unlock(&current->files->lock);
    return f;
}

static int fdtable_close(struct fdtable *table, fd_t f) {
    struct fd *fd = fdtable_get(table, f);
    if (fd == NULL)
        return _EBADF;
    if (fd->inode != NULL) // temporary hack for files like sockets that right now don't have inodes but will eventually
        file_lock_remove_owned_by(fd, table);
    int err = fd_close(fd);
    table->files[f] = NULL;
    bit_clear(f, table->cloexec);
    return err;
}

int f_close(fd_t f) {
    lock(&current->files->lock);
    int err = fdtable_close(current->files, f);
    unlock(&current->files->lock);
    return err;
}

dword_t sys_close(fd_t f) {
    STRACE("close(%d)", f);
    return f_close(f);
}

void fdtable_do_cloexec(struct fdtable *table) {
    lock(&table->lock);
    for (fd_t f = 0; (unsigned) f < table->size; f++)
        if (bit_test(f, table->cloexec))
            fdtable_close(table, f);
    unlock(&table->lock);
}

#define F_DUPFD_ 0
#define F_GETFD_ 1
#define F_SETFD_ 2
#define F_GETFL_ 3
#define F_SETFL_ 4

#define F_GETLK_ 5
#define F_SETLK_ 6
#define F_SETLKW_ 7
#define F_GETLK64_ 12
#define F_SETLK64_ 13
#define F_SETLKW64_ 14

#define F_DUPFD_CLOEXEC_ 1030

dword_t sys_dup(fd_t f) {
    STRACE("dup(%d)", f);
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    // f_install consumes a reference, and the one f_get took belongs to the
    // syscall's drain. This is the table's.
    fd_retain(fd);
    return f_install(fd, 0);
}

dword_t sys_dup3(fd_t f, fd_t new_f, int_t flags) {
    STRACE("dup3(%d, %d, %d)", f, new_f, flags);
    struct fdtable *table = current->files;
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    int err = fdtable_expand(table, new_f);
    if (err < 0)
        return err;
    fd_retain(fd);
    f_close(new_f);
    table->files[new_f] = fd;
    if (flags & O_CLOEXEC_)
        bit_set(new_f, table->cloexec);
    return new_f;
}

dword_t sys_dup2(fd_t f, fd_t new_f) {
    return sys_dup3(f, new_f, 0);
}

int fd_getflags(struct fd *fd) {
    if (fd->ops->getflags)
        return fd->ops->getflags(fd);
    return fd->flags;
}

#define FD_ALLOWED_FLAGS (O_APPEND_ | O_NONBLOCK_)
int fd_setflags(struct fd *fd, int flags) {
    if (fd->ops->setflags)
        return fd->ops->setflags(fd, flags);
    fd->flags = (fd->flags & ~FD_ALLOWED_FLAGS) | (flags & FD_ALLOWED_FLAGS);
    return 0;
}

dword_t sys_fcntl(fd_t f, dword_t cmd, addr_t arg) {
    struct fdtable *table = current->files;
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    struct flock32_ flock32;
    struct flock_ flock;
    fd_t new_f;
    int err;
    switch (cmd) {
        case F_DUPFD_:
            STRACE("fcntl(%d, F_DUPFD, %d)", f, arg);
            fd_retain(fd); // see sys_dup
            return f_install_start(fd, arg);

        case F_DUPFD_CLOEXEC_:
            STRACE("fcntl(%d, F_DUPFD_CLOEXEC, %d)", f, arg);
            fd_retain(fd); // see sys_dup
            new_f = f_install_start(fd, arg);
            bit_set(new_f, table->cloexec);
            return new_f;

        case F_GETFD_:
            STRACE("fcntl(%d, F_GETFD)", f);
            return bit_test(f, table->cloexec);
        case F_SETFD_:
            STRACE("fcntl(%d, F_SETFD, 0x%x)", f, arg);
            if (arg & 1)
                bit_set(f, table->cloexec);
            else
                bit_clear(f, table->cloexec);
            return 0;

        case F_GETFL_:
            STRACE("fcntl(%d, F_GETFL)", f);
            return fd_getflags(fd);
        case F_SETFL_:
            STRACE("fcntl(%d, F_SETFL, %#x)", f, arg);
            return fd_setflags(fd, arg);

        case F_GETLK_:
            STRACE("fcntl(%d, F_GETLK, %#x)", f, arg);
            if (user_read(arg, &flock32, sizeof(flock32)))
                return _EFAULT;
            flock.type = flock32.type;
            flock.whence = flock32.whence;
            flock.start = flock32.start;
            flock.len = flock32.len;
            flock.pid = flock32.pid;
            err = fcntl_getlk(fd, &flock);
            if (err >= 0) {
                flock32.type = flock.type;
                flock32.whence = flock.whence;
                flock32.start = flock.start;
                flock32.len = flock.len;
                flock32.pid = flock.pid;
                if (user_write(arg, &flock32, sizeof(flock32)))
                    return _EFAULT;
            }
            return err;

        case F_GETLK64_:
            STRACE("fcntl(%d, F_GETLK64, %#x)", f, arg);
            if (user_read(arg, &flock, sizeof(flock)))
                return _EFAULT;
            err = fcntl_getlk(fd, &flock);
            if (err >= 0)
                if (user_write(arg, &flock, sizeof(flock)))
                    return _EFAULT;
            return err;

        case F_SETLK_:
        case F_SETLKW_:
            STRACE("fcntl(%d, F_SETLK%*s, %#x)", f, cmd == F_SETLKW_, "W", arg);
            if (user_read(arg, &flock32, sizeof(flock32)))
                return _EFAULT;
            flock.type = flock32.type;
            flock.whence = flock32.whence;
            flock.start = flock32.start;
            flock.len = flock32.len;
            flock.pid = flock32.pid;
            return fcntl_setlk(fd, &flock, cmd == F_SETLKW64_);

        case F_SETLK64_:
        case F_SETLKW64_:
            STRACE("fcntl(%d, F_SETLK%*s64, %#x)", f, cmd == F_SETLKW_, "W", arg);
            if (user_read(arg, &flock, sizeof(flock)))
                return _EFAULT;
            return fcntl_setlk(fd, &flock, cmd == F_SETLKW_);

        default:
            STRACE("fcntl(%d, %d)", f, cmd);
            return _EINVAL;
    }
}

dword_t sys_fcntl32(fd_t fd, dword_t cmd, addr_t arg) {
    switch (cmd) {
        case F_GETLK64_:
        case F_SETLK64_:
        case F_SETLKW64_:
            return _EINVAL;
    }
    return sys_fcntl(fd, cmd, arg);
}

// close_range(unsigned first, unsigned last, unsigned flags) — Linux 5.9+ syscall #436.
// Closes all open fds in [first, last]. Used by Anthropic claude-cli, openssh,
// systemd, etc.; without this, claude-cli loops on missing-syscall ENOSYS.
// CLOSE_RANGE_UNSHARE (0x02) and CLOSE_RANGE_CLOEXEC (0x04) are defined in
// kernel headers but we treat them as no-ops here — guests that need the
// CLOEXEC variant will fall back to F_SETFD anyway.
#define CLOSE_RANGE_CLOEXEC_ 0x04
dword_t sys_close_range(uint32_t first, uint32_t last, uint32_t flags) {
    STRACE("close_range(%u, %u, 0x%x)", first, last, flags);
    if (first > last)
        return _EINVAL;
    lock(&current->files->lock);
    fd_t lim = current->files->size;
    if ((fd_t)last >= lim) last = lim - 1;
    int err = 0;
    for (fd_t f = (fd_t)first; f <= (fd_t)last; f++) {
        if (current->files->files[f] == NULL) continue;
        if (flags & CLOSE_RANGE_CLOEXEC_) {
            // Mark CLOEXEC instead of actually closing.
            bit_set(f, current->files->cloexec);
            continue;
        }
        int e = fdtable_close(current->files, f);
        if (e != 0 && err == 0) err = e;
    }
    unlock(&current->files->lock);
    return 0;  // Linux always returns 0 on success even if some close failed
}
