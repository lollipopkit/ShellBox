// lock_fchdir / unlock_fchdir.
//
// The contract is a *temporary* change of the host process's working
// directory, and nothing used to change it back: one realfs_mknod of a FIFO
// moved the cwd to a mount's root permanently, and every later relative host
// syscall — in any thread, in any mount, including host code outside this
// kernel — resolved against it. The tests here are about the two halves of
// that contract: the directory comes back, and a failure does not take the
// lock with it.

#include "util/fchdir.h"
#include "tests/unit/unit.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static char tmpdir[PATH_MAX];
static char cwd_before[PATH_MAX];

static void record_cwd(char *out) {
    if (getcwd(out, PATH_MAX) == NULL) {
        perror("getcwd");
        exit(2);
    }
}

TEST(cwd_is_restored) {
    char before[PATH_MAX], during[PATH_MAX], after[PATH_MAX];
    record_cwd(before);

    int dirfd = open(tmpdir, O_RDONLY | O_DIRECTORY);
    CHECK(dirfd >= 0);
    if (dirfd < 0)
        return;

    if (lock_fchdir(dirfd) != 0) {
        UNIT_FAIL("lock_fchdir on a real directory failed");
        close(dirfd);
        return;
    }
    record_cwd(during);
    unlock_fchdir();
    record_cwd(after);

    CHECK_EQ_INT(strcmp(after, before), 0);
    // And it really did move while it was held, so the restore is doing work.
    CHECK(strcmp(during, before) != 0);
    close(dirfd);
}

// A descriptor that is not a directory: fchdir fails, and the lock must not be
// left held — the caller does relative host operations on the strength of a
// zero return, and every later lock_fchdir would block forever.
TEST(bad_dirfd_fails_without_holding_the_lock) {
    char before[PATH_MAX], after[PATH_MAX];
    record_cwd(before);

    char file[PATH_MAX];
    snprintf(file, sizeof(file), "%s/a-file", tmpdir);
    int filefd = open(file, O_RDWR | O_CREAT | O_TRUNC, 0600);
    CHECK(filefd >= 0);
    if (filefd < 0)
        return;

    CHECK_EQ_INT(lock_fchdir(filefd), -1);
    record_cwd(after);
    CHECK_EQ_INT(strcmp(after, before), 0);
    close(filefd);

    // The lock is free: this would hang instead of returning if it were not.
    int dirfd = open(tmpdir, O_RDONLY | O_DIRECTORY);
    CHECK(dirfd >= 0);
    if (dirfd >= 0) {
        CHECK_EQ_INT(lock_fchdir(dirfd), 0);
        unlock_fchdir();
        close(dirfd);
    }
}

// A descriptor that was never open. Not a *closed* one: lock_fchdir opens the
// current directory first, and on a closed descriptor that open takes the
// number that was just freed — so fchdir then succeeds, on the cwd.
TEST(unopened_dirfd_fails_without_holding_the_lock) {
    // -1, rather than some number that merely happens to be free: this has to
    // be invalid by definition, not by circumstance.
    CHECK_EQ_INT(lock_fchdir(-1), -1);

    int good = open(tmpdir, O_RDONLY | O_DIRECTORY);
    CHECK(good >= 0);
    if (good >= 0) {
        // Held only if it answered 0; unlocking one that was never taken, or
        // leaving one held, would hang whatever runs next.
        if (lock_fchdir(good) == 0)
            unlock_fchdir();
        else
            UNIT_FAIL("the lock was left held by the failing call above");
        close(good);
    }
}

// Repeated use does not accumulate descriptors: the saved cwd is opened on
// every lock and has to be closed on every unlock.
TEST(repeated_use_does_not_leak) {
    int dirfd = open(tmpdir, O_RDONLY | O_DIRECTORY);
    CHECK(dirfd >= 0);
    if (dirfd < 0)
        return;

    // A probe descriptor's number is a usable stand-in for how many are open:
    // the lowest free number does not climb if nothing is being kept.
    if (lock_fchdir(dirfd) != 0) {
        UNIT_FAIL("lock_fchdir on a real directory failed");
        close(dirfd);
        return;
    }
    unlock_fchdir();
    int probe_before = open("/dev/null", O_RDONLY);
    close(probe_before);

    for (int i = 0; i < 64; i++) {
        if (lock_fchdir(dirfd) != 0) {
            UNIT_FAIL("lock_fchdir failed on iteration %d", i);
            close(dirfd);
            return;
        }
        unlock_fchdir();
    }
    int probe_after = open("/dev/null", O_RDONLY);
    close(probe_after);
    CHECK_EQ_INT(probe_after, probe_before);
    close(dirfd);
}

int main(void) {
    record_cwd(cwd_before);
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/ish-fchdir-test-%d", (int) getpid());
    if (mkdir(tmpdir, 0700) < 0) {
        perror("mkdir");
        return 2;
    }

    RUN(cwd_is_restored);
    RUN(bad_dirfd_fails_without_holding_the_lock);
    RUN(unopened_dirfd_fails_without_holding_the_lock);
    RUN(repeated_use_does_not_leak);

    // Whatever the tests did, the process ends where it started — which is
    // itself the property under test.
    char cwd_after[PATH_MAX];
    record_cwd(cwd_after);
    if (strcmp(cwd_after, cwd_before) != 0)
        UNIT_FAIL("cwd moved from %s to %s", cwd_before, cwd_after);

    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
    if (system(cmd) != 0)
        fprintf(stderr, "note: could not remove %s\n", tmpdir);
    return UNIT_REPORT();
}
