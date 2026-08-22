// Giving up a controlling terminal without waiting for a reaper.
//
// A session leader that opens a terminal takes it as its controlling one, and
// `tty_set_controlling` holds a reference for the group. That reference is
// given back in `task_leave_session`, which runs when the task is reaped.
//
// An embedder does not reap. Nothing here waits for a task, and the auto-reap
// path needs a parent that ignores SIGCHLD, which init does not — so a session
// that is hung up leaves its group behind as a zombie holding the terminal,
// for the life of the process.
//
// What that cost, end to end: the pty stayed allocated, so `/dev/pts` could
// never be unmounted, so a filesystem any session had ever been opened in
// could never be detached. ServerBox surfaced it as a Linux system that could
// not be deleted until the app was restarted, and as a two-second retry loop
// that was waiting for something that was never going to happen.

#define ISH_INTERNAL
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/init.h"
#include "kernel/task.h"
#include "fs/devices.h"
#include "fs/fd.h"
#include "fs/real.h"
#include "fs/tty.h"
#include "tests/unit/unit.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int test_tty_write(struct tty *tty, const void *buf, size_t len,
                          bool blocking) {
    (void) tty; (void) buf; (void) blocking;
    return (int) len;
}

static const struct tty_driver_ops test_tty_ops = {
    .write = test_tty_write,
};

DEFINE_TTY_DRIVER(test_pty, &test_tty_ops, TTY_PSEUDO_SLAVE_MAJOR, 4);

static void cleanup(const char *tmpdir) {
    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
    if (system(cmd) != 0)
        fprintf(stderr, "warning: could not remove %s\n", tmpdir);
}

// What an embedder does per session: a pty, and stdio opened onto it by a task
// that is a session leader — which is what makes the open take it as the
// controlling terminal.
static struct fd *open_session_tty(struct tty **out) {
    struct tty *tty = pty_open_fake(&test_pty);
    if (IS_ERR(tty))
        return ERR_PTR(PTR_ERR(tty));
    struct fd *fd = adhoc_fd_create(&tty_dev.fd);
    if (fd == NULL)
        return ERR_PTR(_ENOMEM);
    tty_open(tty, fd);
    *out = tty;
    return fd;
}

TEST(opening_a_terminal_as_a_session_leader_takes_it) {
    // The premise. If this stopped being true the rest of the file would be
    // testing something that no longer happens.
    struct tty *tty = NULL;
    struct fd *fd = open_session_tty(&tty);
    if (IS_ERR(fd)) {
        UNIT_FAIL("could not open a session tty: %d", (int) PTR_ERR(fd));
        return;
    }

    CHECK(current->group->tty == tty);
    CHECK_EQ_INT(tty->session, current->group->sid);

    tty_disown(current->group);
    fd_close(fd);
}

TEST(disowning_gives_the_reference_back) {
    struct tty *tty = NULL;
    struct fd *fd = open_session_tty(&tty);
    if (IS_ERR(fd)) {
        UNIT_FAIL("could not open a session tty: %d", (int) PTR_ERR(fd));
        return;
    }

    // Counted rather than inferred: the whole failure was one reference that
    // nobody gave back, and a test that only checked `group->tty == NULL`
    // would pass while still leaking it.
    lock(&tty->lock);
    int held = tty->refcount;
    unlock(&tty->lock);

    tty_disown(current->group);

    lock(&tty->lock);
    int after = tty->refcount;
    unlock(&tty->lock);

    CHECK_EQ_INT(after, held - 1);
    CHECK(current->group->tty == NULL);
    // And the terminal no longer claims a session, so another leader may take
    // it — `tty_set_controlling` refuses one that is already spoken for.
    CHECK_EQ_INT(tty->session, 0);

    fd_close(fd);
}

TEST(disowning_twice_is_not_two_references) {
    struct tty *tty = NULL;
    struct fd *fd = open_session_tty(&tty);
    if (IS_ERR(fd)) {
        UNIT_FAIL("could not open a session tty: %d", (int) PTR_ERR(fd));
        return;
    }

    tty_disown(current->group);
    lock(&tty->lock);
    int after_first = tty->refcount;
    unlock(&tty->lock);

    // The call sites cannot always know whether a session took a terminal —
    // a one-shot command opens no interactive shell — so this has to be safe
    // to call regardless. Releasing a second time would free a live tty.
    tty_disown(current->group);
    lock(&tty->lock);
    int after_second = tty->refcount;
    unlock(&tty->lock);

    CHECK_EQ_INT(after_second, after_first);

    fd_close(fd);
}

TEST(disowning_a_group_that_holds_nothing_does_nothing) {
    // The ordinary case for a non-interactive session, and the one that would
    // crash if this dereferenced without checking.
    CHECK(current->group->tty == NULL);
    tty_disown(current->group);
    CHECK(current->group->tty == NULL);
}

int main(void) {
    char tmpdir[] = "/tmp/ish-tty-disown-test-XXXXXX";
    if (mkdtemp(tmpdir) == NULL) {
        perror("mkdtemp");
        return 2;
    }
    int err = mount_root(&realfs, tmpdir);
    if (err < 0) {
        fprintf(stderr, "mount_root: %d\n", err);
        cleanup(tmpdir);
        return 2;
    }
    if ((err = become_first_process()) < 0) {
        fprintf(stderr, "become_first_process: %d\n", err);
        cleanup(tmpdir);
        return 2;
    }

    RUN(disowning_a_group_that_holds_nothing_does_nothing);
    RUN(opening_a_terminal_as_a_session_leader_takes_it);
    RUN(disowning_gives_the_reference_back);
    RUN(disowning_twice_is_not_two_references);

    cleanup(tmpdir);
    return UNIT_REPORT();
}
