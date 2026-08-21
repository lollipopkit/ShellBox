// The guest's open(2) flag values, and the two checks in generic_openat that
// read them.
//
// The guest is AArch64, and AArch64 overrides the asm-generic values for three
// flags (arch/arm64/include/uapi/asm/fcntl.h, for AArch32 compat). This tree
// carried x86's instead, so:
//
//   * O_DIRECTORY_ was 1<<16. No aarch64 guest ever sets that bit for
//     O_DIRECTORY, so the ENOTDIR check below never fired — and 1<<16 *is*
//     O_DIRECT on aarch64, so an O_DIRECT open was read as an O_DIRECTORY one
//     and failed on any non-directory. GNU cp over an existing file died with
//     "cannot create regular file: Not a directory".
//   * O_NOFOLLOW_ had no value at all, so it could not be honored.
//
// A wrong constant is only visible against something that does not move with
// it, so the values are asserted literally rather than against each other, and
// the behaviour is asserted through generic_openat with a real root mounted.

#include "kernel/calls.h"
#include "kernel/fs.h"
#include "kernel/init.h"
#include "fs/fd.h"
#include "fs/real.h"
#include "tests/unit/unit.h"

#include <fcntl.h>
#include <stdlib.h>
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static char tmpdir[PATH_MAX];

// From arch/arm64/include/uapi/asm/fcntl.h and asm-generic/fcntl.h. Written as
// literals on purpose: repeating the definition under test proves nothing.
TEST(flag_values_are_the_aarch64_ones) {
    CHECK_EQ(O_DIRECTORY_, 0x4000);
    CHECK_EQ(O_NOFOLLOW_,  0x8000);
    CHECK_EQ(O_DIRECT_,    0x10000);
    CHECK_EQ(O_LARGEFILE_, 0x20000);
    CHECK_EQ(O_NOATIME_,   0x40000);
    CHECK_EQ(O_CLOEXEC_,   0x80000);
    CHECK_EQ(O_PATH_,      0x200000);

    // The asm-generic ones AArch64 does not override, so that a future edit
    // cannot quietly shift the whole block.
    CHECK_EQ(O_CREAT_,     0x40);
    CHECK_EQ(O_TRUNC_,     0x200);
    CHECK_EQ(O_APPEND_,    0x400);
    CHECK_EQ(O_NONBLOCK_,  0x800);
}

// The shape of the original bug: two flags sharing a bit. Nothing above would
// catch a *new* flag colliding with an existing one, and that is the same
// mistake in a different place.
TEST(no_two_flags_share_a_bit) {
    static const struct { const char *name; int value; } flags[] = {
        {"O_RDONLY_", O_RDONLY_}, {"O_WRONLY_", O_WRONLY_}, {"O_RDWR_", O_RDWR_},
        {"O_CREAT_", O_CREAT_}, {"O_EXCL_", O_EXCL_}, {"O_NOCTTY_", O_NOCTTY_},
        {"O_TRUNC_", O_TRUNC_}, {"O_APPEND_", O_APPEND_}, {"O_NONBLOCK_", O_NONBLOCK_},
        {"O_DIRECTORY_", O_DIRECTORY_}, {"O_NOFOLLOW_", O_NOFOLLOW_},
        {"O_DIRECT_", O_DIRECT_}, {"O_LARGEFILE_", O_LARGEFILE_},
        {"O_NOATIME_", O_NOATIME_}, {"O_CLOEXEC_", O_CLOEXEC_}, {"O_PATH_", O_PATH_},
    };
    for (size_t i = 0; i < sizeof(flags)/sizeof(flags[0]); i++) {
        // O_RDONLY_ is zero, and an access mode is not a bit anyway.
        if (flags[i].value == 0)
            continue;
        for (size_t j = i + 1; j < sizeof(flags)/sizeof(flags[0]); j++) {
            if (flags[j].value == 0)
                continue;
            if ((flags[i].value & flags[j].value) != 0)
                UNIT_FAIL("%s (%#x) and %s (%#x) share a bit",
                          flags[i].name, flags[i].value,
                          flags[j].name, flags[j].value);
        }
    }
}

static int open_err(const char *path, int flags) {
    struct fd *fd = generic_open(path, flags, 0);
    if (IS_ERR(fd))
        return (int) PTR_ERR(fd);
    fd_close(fd);
    return 0;
}

// The check the wrong constant disabled. With O_DIRECTORY_ at 1<<16 a guest
// asking for a directory got whatever was there.
TEST(o_directory_rejects_a_file) {
    CHECK_EQ_INT(open_err("/file", O_RDONLY_ | O_DIRECTORY_), _ENOTDIR);
    CHECK_EQ_INT(open_err("/dir", O_RDONLY_ | O_DIRECTORY_), 0);
}

// The other half of the same bug, and the one users actually hit: O_DIRECT on
// a regular file must open it, not be mistaken for a demand that it be a
// directory. Nothing forwards O_DIRECT to the host — Darwin has no equivalent
// — so the only thing under test is that it is not misread.
TEST(o_direct_on_a_file_is_not_o_directory) {
    CHECK_EQ_INT(open_err("/file", O_RDONLY_ | O_DIRECT_), 0);
}

// O_NOFOLLOW cannot be handed to the host open(): path_normalize resolves the
// final component itself, so by the time any fs open() runs the symlink is
// already gone and the host sees the target. It has to change the resolution,
// which is what makes ELOOP reachable.
TEST(o_nofollow_refuses_a_symlink) {
    CHECK_EQ_INT(open_err("/link", O_RDONLY_ | O_NOFOLLOW_), _ELOOP);
    // Without it the same path opens the target, so the test above is about
    // the flag and not about the link being broken.
    CHECK_EQ_INT(open_err("/link", O_RDONLY_), 0);
    // A non-link is unaffected: O_NOFOLLOW is about the final component being
    // a symlink, not about refusing to resolve anything.
    CHECK_EQ_INT(open_err("/file", O_RDONLY_ | O_NOFOLLOW_), 0);
}

// A symlink to a directory, opened with both. O_NOFOLLOW is checked first in
// Linux too: the error is ELOOP, not ENOTDIR.
TEST(o_nofollow_beats_o_directory_on_a_link) {
    CHECK_EQ_INT(open_err("/dirlink", O_RDONLY_ | O_NOFOLLOW_ | O_DIRECTORY_), _ELOOP);
}

static int make_tree(void) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/file", tmpdir);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -1;
    close(fd);
    snprintf(path, sizeof(path), "%s/dir", tmpdir);
    if (mkdir(path, 0700) < 0)
        return -1;
    snprintf(path, sizeof(path), "%s/link", tmpdir);
    if (symlink("file", path) < 0)
        return -1;
    snprintf(path, sizeof(path), "%s/dirlink", tmpdir);
    if (symlink("dir", path) < 0)
        return -1;
    return 0;
}

int main(void) {
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/ish-openflags-test-%d", (int) getpid());
    if (mkdir(tmpdir, 0700) < 0) {
        perror("mkdir");
        return 2;
    }
    if (make_tree() < 0) {
        perror("setting up the test tree");
        return 2;
    }

    // realfs rather than fakefs: the checks under test are in generic_openat,
    // above any mount, and realfs needs no metadata database to stand up.
    int err = mount_root(&realfs, tmpdir);
    if (err < 0) {
        fprintf(stderr, "mount_root: %d\n", err);
        return 2;
    }
    if ((err = become_first_process()) < 0) {
        fprintf(stderr, "become_first_process: %d\n", err);
        return 2;
    }

    RUN(flag_values_are_the_aarch64_ones);
    RUN(no_two_flags_share_a_bit);
    RUN(o_directory_rejects_a_file);
    RUN(o_direct_on_a_file_is_not_o_directory);
    RUN(o_nofollow_refuses_a_symlink);
    RUN(o_nofollow_beats_o_directory_on_a_link);

    int status = UNIT_REPORT();
    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
    if (system(cmd) != 0)
        fprintf(stderr, "note: could not remove %s\n", tmpdir);
    return status;
}
