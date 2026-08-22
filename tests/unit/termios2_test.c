// TCGETS2 and its three setters.
//
// glibc has used the `2` forms for tcgetattr/tcsetattr since it grew support
// for arbitrary baud rates. This kernel implemented only the originals, so on
// a distribution whose glibc is new enough every one of those calls answered
// ENOTTY — and `isatty`, which is `tcgetattr` succeeding and nothing else,
// therefore said "not a terminal" about a perfectly good pty.
//
// What that looks like is not an error. A shell that cannot tell it has a
// terminal does not fail; it declines to be interactive. Measured on
// ubuntu-base 26.04 (bash 5.3.9, glibc 2.42) under `ish -r`, driven through a
// pty:
//
//     $-        hBs         no i, no m, no H
//     PS1       empty
//     prompt    none at all
//
// and on Rocky 9 (bash 5.1.8, glibc 2.34) through the same pty, `himBHs` and
// `sh-5.1#`. Two glibc distributions on one engine, differing only in whether
// their libc had made the switch. Alpine was never affected: musl's isatty
// still uses TCGETS.
//
// The strace line that named it:
//
//     ioctl(0, 0x802c542a, 0xffffd930) = 0xffffffe7
//
// 0x802c542a is TCGETS2 and 0xffffffe7 is -25, ENOTTY.

#define ISH_INTERNAL
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/init.h"
#include "fs/tty.h"
#include "fs/fd.h"
#include "fs/real.h"
#include "fs/devices.h"
#include "tests/unit/unit.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// A driver of this test's own, the way an embedder writes one — see
// `sbm_pty_driver` in ServerBox's sbm_ish.c. `pty_slave`'s own init expects a
// master on the other end, which a session opened by an embedder does not
// have; `pty_open_fake` exists for exactly that case and points whatever
// driver it is given at the pty slave table.
static int test_tty_write(struct tty *tty, const void *buf, size_t len,
                          bool blocking) {
    (void) tty; (void) buf; (void) blocking;
    return (int) len;  // discarded: nothing here reads what a tty echoes
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

static struct fd *open_pty(void) {
    struct tty *tty = pty_open_fake(&test_pty);
    if (IS_ERR(tty))
        return ERR_PTR(PTR_ERR(tty));
    struct fd *fd = adhoc_fd_create(&tty_dev.fd);
    if (fd == NULL)
        return ERR_PTR(_ENOMEM);
    tty_open(tty, fd);
    return fd;
}

static int ioctl_on(struct fd *fd, int cmd, void *arg) {
    return tty_dev.fd.ioctl(fd, cmd, arg);
}

TEST(tcgets2_answers_rather_than_enotty) {
    struct fd *fd = open_pty();
    if (IS_ERR(fd)) {
        UNIT_FAIL("could not open a pty: %d", (int) PTR_ERR(fd));
        return;
    }

    struct termios2_ got;
    memset(&got, 0, sizeof(got));
    // ENOTTY here is the whole bug: it is what makes isatty answer false.
    CHECK_EQ_INT(ioctl_on(fd, TCGETS2_, &got), 0);

    // And it describes the same terminal TCGETS does, rather than zeroes.
    struct termios_ plain;
    memset(&plain, 0, sizeof(plain));
    CHECK_EQ_INT(ioctl_on(fd, TCGETS_, &plain), 0);
    CHECK_EQ(got.iflags, plain.iflags);
    CHECK_EQ(got.oflags, plain.oflags);
    CHECK_EQ(got.lflags, plain.lflags);
    CHECK_EQ_INT(memcmp(got.cc, plain.cc, sizeof(plain.cc)), 0);

    // The two views have to agree about the line. A pty has none, so what is
    // reported is what Linux reports for one: 38400, and cflags saying the
    // same thing rather than B0, which means "hang up".
    CHECK_EQ(got.cflags, plain.cflags);
    CHECK_EQ(got.cflags & CSIZE_, CS8_);
    CHECK(got.cflags & CREAD_);
    CHECK_EQ(got.cflags & CBAUD_MASK_, B38400_);
    CHECK_EQ(got.ispeed, 38400u);
    CHECK_EQ(got.ospeed, 38400u);

    fd_close(fd);
}

TEST(tcsets2_round_trips_what_it_was_given) {
    struct fd *fd = open_pty();
    if (IS_ERR(fd)) {
        UNIT_FAIL("could not open a pty: %d", (int) PTR_ERR(fd));
        return;
    }

    struct termios2_ set;
    memset(&set, 0, sizeof(set));
    CHECK_EQ_INT(ioctl_on(fd, TCGETS2_, &set), 0);
    // What a shell does on the way in: turn off canonical mode and echo.
    set.lflags &= ~(ICANON_ | ECHO_);
    set.ispeed = 115200;
    set.ospeed = 115200;
    CHECK_EQ_INT(ioctl_on(fd, TCSETS2_, &set), 0);

    struct termios2_ got;
    memset(&got, 0, sizeof(got));
    CHECK_EQ_INT(ioctl_on(fd, TCGETS2_, &got), 0);
    CHECK_EQ(got.lflags, set.lflags);
    CHECK_EQ(got.ispeed, 115200);
    CHECK_EQ(got.ospeed, 115200);

    // The flags a `2` call sets are the same flags the original form reads.
    // Two views of one terminal, not two terminals.
    struct termios_ plain;
    memset(&plain, 0, sizeof(plain));
    CHECK_EQ_INT(ioctl_on(fd, TCGETS_, &plain), 0);
    CHECK_EQ(plain.lflags, set.lflags);

    fd_close(fd);
}

TEST(the_flush_and_drain_forms_are_accepted_too) {
    // glibc's tcsetattr picks one of the three by its `optional_actions`
    // argument, so implementing only TCSETS2 would leave tcsetattr(TCSAFLUSH)
    // — which is what a shell uses — still answering ENOTTY.
    struct fd *fd = open_pty();
    if (IS_ERR(fd)) {
        UNIT_FAIL("could not open a pty: %d", (int) PTR_ERR(fd));
        return;
    }

    struct termios2_ t;
    memset(&t, 0, sizeof(t));
    CHECK_EQ_INT(ioctl_on(fd, TCGETS2_, &t), 0);
    CHECK_EQ_INT(ioctl_on(fd, TCSETSW2_, &t), 0);
    CHECK_EQ_INT(ioctl_on(fd, TCSETSF2_, &t), 0);

    fd_close(fd);
}

// The size table decides whether the ioctl is reached at all: sys_ioctl asks
// for a size first and answers ENOTTY when there is none, so a command missing
// from it never gets as far as the switch that would handle it. Asserted
// through the same fd ops the kernel uses, because calling the handler
// directly — as the cases above do — steps over exactly this.
TEST(the_size_table_admits_the_new_commands) {
    static const int cmds[] = {TCGETS2_, TCSETS2_, TCSETSW2_, TCSETSF2_};
    for (unsigned i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        CHECK_EQ_INT(tty_dev.fd.ioctl_size(cmds[i]),
                     (ssize_t) sizeof(struct termios2_));
    }
}

TEST(the_commands_are_the_numbers_linux_uses) {
    // Written out rather than derived from the header's own macros. A guest
    // issues the number its kernel headers gave it, so what matters is that
    // these equal Linux's — and a test that checked TCGETS2_ against fields
    // of TCGETS2_ would pass just as happily with the direction, type or
    // number changed. Only the size would have been pinned, which is the one
    // part a wrong constant is least likely to get wrong.
    CHECK_EQ(TCGETS2_, 0x802c542a);
    CHECK_EQ(TCSETS2_, 0x402c542b);
    CHECK_EQ(TCSETSW2_, 0x402c542c);
    CHECK_EQ(TCSETSF2_, 0x402c542d);

    // And the struct is the size those numbers encode, since the kernel copies
    // that many bytes to and from the guest.
    CHECK_EQ(sizeof(struct termios2_), 44u);
}

TEST(termios2_is_termios_with_the_speeds_appended) {
    // Layout, not just total size: a struct that packed differently could be
    // 44 bytes and still put every field somewhere the guest does not expect.
    CHECK_EQ(offsetof(struct termios2_, iflags), offsetof(struct termios_, iflags));
    CHECK_EQ(offsetof(struct termios2_, oflags), offsetof(struct termios_, oflags));
    CHECK_EQ(offsetof(struct termios2_, cflags), offsetof(struct termios_, cflags));
    CHECK_EQ(offsetof(struct termios2_, lflags), offsetof(struct termios_, lflags));
    CHECK_EQ(offsetof(struct termios2_, line), offsetof(struct termios_, line));
    CHECK_EQ(offsetof(struct termios2_, cc), offsetof(struct termios_, cc));
    // Linux puts them at 36 and 40, immediately after cc[19] plus its padding.
    CHECK_EQ(offsetof(struct termios2_, ispeed), 36u);
    CHECK_EQ(offsetof(struct termios2_, ospeed), 40u);
}

int main(void) {
    // A root, because opening a pty reaches the filesystem and mount_find
    // asserts on an empty mount table. Nothing here reads or writes it.
    // mkdtemp, not a name built from the pid: a run interrupted before its
    // cleanup would otherwise leave a directory that makes the next run with
    // the same pid fail at mkdir. Removed on every path out, including the
    // two setup failures below, which used to return leaving it behind.
    char tmpdir[] = "/tmp/ish-termios2-test-XXXXXX";
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

    RUN(tcgets2_answers_rather_than_enotty);
    RUN(tcsets2_round_trips_what_it_was_given);
    RUN(the_flush_and_drain_forms_are_accepted_too);
    RUN(the_size_table_admits_the_new_commands);
    RUN(the_commands_are_the_numbers_linux_uses);
    RUN(termios2_is_termios_with_the_speeds_appended);

    cleanup(tmpdir);
    return UNIT_REPORT();
}
