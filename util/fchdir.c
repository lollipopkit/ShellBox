#include <fcntl.h>
#include <unistd.h>
#include "util/sync.h"

static lock_t fchdir_lock = LOCK_INITIALIZER;
// Guarded by fchdir_lock, so only the thread holding it ever touches this.
static int fchdir_saved_cwd = -1;

int lock_fchdir(int dirfd) {
    lock(&fchdir_lock);
    // The contract here is a *temporary* directory change, and nothing used
    // to change it back: one realfs_mknod of a FIFO moved the host process'
    // working directory to a mount's root permanently, and every later
    // relative host syscall — in any thread, in any mount, including the
    // host integrations outside this kernel — resolved against it.
    fchdir_saved_cwd = open(".", O_RDONLY | O_CLOEXEC);
    if (fchdir_saved_cwd < 0) {
        // Without somewhere to come back to, changing directory is not
        // temporary — and the caller does relative host operations on the
        // strength of it. Refuse instead.
        unlock(&fchdir_lock);
        return -1;
    }
    if (fchdir(dirfd) < 0) {
        close(fchdir_saved_cwd);
        fchdir_saved_cwd = -1;
        unlock(&fchdir_lock);
        return -1;
    }
    return 0;
}

void unlock_fchdir() {
    if (fchdir_saved_cwd >= 0) {
        fchdir(fchdir_saved_cwd);
        close(fchdir_saved_cwd);
        fchdir_saved_cwd = -1;
    }
    unlock(&fchdir_lock);
}
