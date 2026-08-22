// realfs_utime and AT_SYMLINK_NOFOLLOW.
//
// The guest's flag was decoded correctly and then dropped one layer down:
// sys_utime_common derived `follow_links`, generic_utime handed it to
// path_normalize, and realfs_utime called the host with flags hardcoded to 0.
// So the host followed the last component whatever the guest had asked, and
// two things went wrong that look unrelated — a symlink was never the thing
// stamped, and a *dangling* symlink answered an error for a call that should
// have succeeded.
//
// It surfaced as no package with a build id being installable on a glibc
// distribution: rpm lays down /usr/lib/.build-id/** as symlinks and stamps
// each one with AT_SYMLINK_NOFOLLOW, which failed the whole unpack.
//
// Two layers are covered, because the fix changed two. The direct calls to
// realfs_utime pin the layer that dropped the flag. The generic_utime calls
// pin the dispatch above it, which had to start passing `follow_links` to
// `mount->fs->utime` — back that half out alone and every direct test here
// still passes.
//
// realfs both times, and not fakefs. fakefs points `.utime` at the same
// realfs_utime, so the callback is not a second thing to reach; and its
// symlink model stores the target in a *regular file* with an S_IFLNK bit in
// meta.db (fs/fake.c), so the host has nothing to follow and neither value of
// the flag can change the answer. A fakefs symlink case would assert
// something that cannot tell the bug from the fix.

#define ISH_INTERNAL
#include "kernel/fs.h"
#include "kernel/init.h"
#include "fs/path.h"
#include "fs/real.h"
#include "tests/unit/unit.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// Far enough from now that nothing else could have set it, and old enough to
// be representable everywhere.
#define STAMP 1000000

static char tmpdir[PATH_MAX];
static struct mount mount;

static struct timespec at(long sec) {
    return (struct timespec) {.tv_sec = sec, .tv_nsec = 0};
}

// The guest path, which realfs_utime resolves against mount.root_fd.
static int utime_guest(const char *guest_path, long sec, bool follow_links) {
    return realfs_utime(&mount, guest_path, at(sec), at(sec), follow_links);
}

static long lmtime(const char *name) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", tmpdir, name);
    struct stat st;
    if (lstat(path, &st) < 0)
        return -1;
    return (long) st.st_mtime;
}

static long mtime(const char *name) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", tmpdir, name);
    struct stat st;
    if (stat(path, &st) < 0)
        return -1;
    return (long) st.st_mtime;
}

static void make_symlink(const char *target, const char *name) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", tmpdir, name);
    unlink(path);
    if (symlink(target, path) < 0) {
        perror("symlink");
        exit(2);
    }
}

static void make_file(const char *name) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", tmpdir, name);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        perror("open");
        exit(2);
    }
    close(fd);
}

// The rpm case. A build-id symlink is written before the package carrying its
// target, so the link points at nothing when it is stamped.
TEST(nofollow_stamps_a_dangling_symlink) {
    make_symlink("nothing-here", "dangling");
    CHECK_EQ_INT(utime_guest("/dangling", STAMP, false), 0);
    CHECK_EQ_INT(lmtime("dangling"), STAMP);
}

// NOFOLLOW means the link itself, and never what it points at. Both halves are
// asserted: with the flag dropped the target was stamped instead, which a test
// looking only at the link would have read as an untouched link and reported
// as the same failure for the wrong reason.
TEST(nofollow_stamps_the_link_not_the_target) {
    make_file("target");
    make_symlink("target", "live");

    CHECK_EQ_INT(utime_guest("/live", STAMP, false), 0);
    CHECK_EQ_INT(lmtime("live"), STAMP);
    CHECK(mtime("target") != STAMP);
}

// And the other way, which has to keep working: without the flag the target is
// the thing stamped, and the link is left alone.
TEST(follow_stamps_the_target_not_the_link) {
    make_file("target2");
    make_symlink("target2", "live2");

    CHECK_EQ_INT(utime_guest("/live2", STAMP, true), 0);
    CHECK_EQ_INT(mtime("target2"), STAMP);
    CHECK(lmtime("live2") != STAMP);
}

// An ordinary file answers the same either way. Here so that a fix which
// simply inverted the flag does not pass on the strength of the two above.
TEST(a_plain_file_is_stamped_either_way) {
    make_file("plain");
    CHECK_EQ_INT(utime_guest("/plain", STAMP, false), 0);
    CHECK_EQ_INT(mtime("plain"), STAMP);

    make_file("plain2");
    CHECK_EQ_INT(utime_guest("/plain2", STAMP, true), 0);
    CHECK_EQ_INT(mtime("plain2"), STAMP);
}

// The same two questions through the dispatch, with tmpdir mounted as the
// guest root. This is the half that fails if `generic_utime` stops handing
// `follow_links` to the fs op while realfs_utime keeps accepting it.
TEST(dispatch_nofollow_stamps_the_link_not_the_target) {
    make_file("dtarget");
    make_symlink("dtarget", "dlive");

    CHECK_EQ_INT(generic_utime(AT_PWD, "/dlive", at(STAMP), at(STAMP), false), 0);
    CHECK_EQ_INT(lmtime("dlive"), STAMP);
    CHECK(mtime("dtarget") != STAMP);
}

TEST(dispatch_follow_stamps_the_target_not_the_link) {
    make_file("dtarget2");
    make_symlink("dtarget2", "dlive2");

    CHECK_EQ_INT(generic_utime(AT_PWD, "/dlive2", at(STAMP), at(STAMP), true), 0);
    CHECK_EQ_INT(mtime("dtarget2"), STAMP);
    CHECK(lmtime("dlive2") != STAMP);
}

TEST(dispatch_nofollow_stamps_a_dangling_symlink) {
    make_symlink("nothing-here", "ddangling");
    CHECK_EQ_INT(generic_utime(AT_PWD, "/ddangling", at(STAMP), at(STAMP), false), 0);
    CHECK_EQ_INT(lmtime("ddangling"), STAMP);
}

int main(void) {
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/ish-utime-test-%d", getpid());
    if (mkdir(tmpdir, 0755) < 0) {
        perror("mkdir");
        return 2;
    }
    mount.root_fd = open(tmpdir, O_RDONLY | O_DIRECTORY);
    if (mount.root_fd < 0) {
        perror("open tmpdir");
        return 2;
    }

    RUN(nofollow_stamps_a_dangling_symlink);
    RUN(nofollow_stamps_the_link_not_the_target);
    RUN(follow_stamps_the_target_not_the_link);
    RUN(a_plain_file_is_stamped_either_way);

    // The dispatch needs a mount table and a task to resolve a path against.
    int err = mount_root(&realfs, tmpdir);
    if (err < 0) {
        fprintf(stderr, "mount_root: %d\n", err);
        return 2;
    }
    if ((err = become_first_process()) < 0) {
        fprintf(stderr, "become_first_process: %d\n", err);
        return 2;
    }

    RUN(dispatch_nofollow_stamps_the_link_not_the_target);
    RUN(dispatch_follow_stamps_the_target_not_the_link);
    RUN(dispatch_nofollow_stamps_a_dangling_symlink);

    close(mount.root_fd);
    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
    if (system(cmd) != 0)
        fprintf(stderr, "warning: could not remove %s\n", tmpdir);
    return UNIT_REPORT();
}
