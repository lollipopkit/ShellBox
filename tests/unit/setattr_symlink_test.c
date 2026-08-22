// realfs_setattr and AT_SYMLINK_NOFOLLOW.
//
// The same bug as realfs_utime's, in the sibling function and found the same
// way. The guest's flag was decoded into `follow_links`, `generic_setattrat`
// handed it to `path_normalize`, and the fs op took no such parameter — so
// `realfs_setattr` called `fchownat` with flags 0 and the host followed the
// last component whatever the guest had asked.
//
// `lchown` on a symlink therefore chowned its target, and on a symlink whose
// target does not exist yet it answered ENOENT for a call that should have
// succeeded. dpkg does exactly that: it lays down `<name>.dpkg-new` as a
// symlink pointing at a file it has not unpacked yet and then sets its
// ownership. `apt-get install libncurses6` failed with
//
//   error setting ownership of symlink '…/libform.so.6.dpkg-new':
//   No such file or directory
//
// so no package shipping a symlink would install — which is most libraries.
// A trivial package like `hello` has none, which is why an earlier check of
// "does apt work" said yes.
//
// Both layers are covered, because the fix changed two: the direct calls pin
// the one that dropped the flag, the generic_setattrat calls pin the dispatch
// above it that had to start passing `follow_links` on.

#define ISH_INTERNAL
#include "kernel/fs.h"
#include "kernel/init.h"
#include "fs/path.h"
#include "fs/real.h"
#include "kernel/errno.h"
#include "tests/unit/unit.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char tmpdir[PATH_MAX];
static struct mount mount;

// Our own uid. Chowning to it is permitted and changes nothing observable,
// which is the point: what is under test is *which path* the call resolves,
// not whether the ownership took. Anything else would need root and would be
// swallowed by the EPERM branch in realfs_setattr.
static uid_t me;

static void make_symlink(const char *target, const char *name) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", tmpdir, name);
    unlink(path);
    if (symlink(target, path) < 0) {
        perror("symlink");
        exit(2);
    }
}

// dpkg's case: a symlink laid down before the file it points at.
TEST(nofollow_chowns_a_dangling_symlink) {
    make_symlink("nothing-here", "dangling");
    CHECK_EQ_INT(
        realfs_setattr(&mount, "/dangling", make_attr(uid, me), false), 0);
}

// And the flag is not simply always set: following a dangling symlink has to
// keep answering ENOENT, or a "fix" that hardcoded NOFOLLOW would pass the
// case above while breaking every ordinary chown through a link.
TEST(follow_still_fails_on_a_dangling_symlink) {
    make_symlink("nothing-here", "dangling2");
    CHECK_EQ_INT(
        realfs_setattr(&mount, "/dangling2", make_attr(uid, me), true),
        _ENOENT);
}

// A live symlink resolves either way, so neither answer is an error.
TEST(a_live_symlink_is_chowned_either_way) {
    char target[PATH_MAX];
    snprintf(target, sizeof(target), "%s/live-target", tmpdir);
    int fd = open(target, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        perror("open");
        exit(2);
    }
    close(fd);
    make_symlink("live-target", "live");

    CHECK_EQ_INT(realfs_setattr(&mount, "/live", make_attr(uid, me), false), 0);
    CHECK_EQ_INT(realfs_setattr(&mount, "/live", make_attr(uid, me), true), 0);
}

// The same two questions through the dispatch, which is the half that fails if
// `generic_setattrat` stops handing `follow_links` to the fs op.
TEST(dispatch_nofollow_chowns_a_dangling_symlink) {
    make_symlink("nothing-here", "ddangling");
    CHECK_EQ_INT(
        generic_setattrat(AT_PWD, "/ddangling", make_attr(uid, me), false), 0);
}

TEST(dispatch_follow_still_fails_on_a_dangling_symlink) {
    make_symlink("nothing-here", "ddangling2");
    CHECK_EQ_INT(
        generic_setattrat(AT_PWD, "/ddangling2", make_attr(uid, me), true),
        _ENOENT);
}

int main(void) {
    me = getuid();
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/ish-setattr-test-%d", getpid());
    if (mkdir(tmpdir, 0755) < 0) {
        perror("mkdir");
        return 2;
    }
    mount.root_fd = open(tmpdir, O_RDONLY | O_DIRECTORY);
    if (mount.root_fd < 0) {
        perror("open tmpdir");
        return 2;
    }

    RUN(nofollow_chowns_a_dangling_symlink);
    RUN(follow_still_fails_on_a_dangling_symlink);
    RUN(a_live_symlink_is_chowned_either_way);

    int err = mount_root(&realfs, tmpdir);
    if (err < 0) {
        fprintf(stderr, "mount_root: %d\n", err);
        return 2;
    }
    if ((err = become_first_process()) < 0) {
        fprintf(stderr, "become_first_process: %d\n", err);
        return 2;
    }

    RUN(dispatch_nofollow_chowns_a_dangling_symlink);
    RUN(dispatch_follow_still_fails_on_a_dangling_symlink);

    close(mount.root_fd);
    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
    if (system(cmd) != 0)
        fprintf(stderr, "warning: could not remove %s\n", tmpdir);
    return UNIT_REPORT();
}
