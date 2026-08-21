// Unmounting a tmpfs, which used to be `TODO("tmpfs umount")` — that is,
// `die()`. Standalone that aborts the process; in an embedder that installs a
// die_handler it parks the calling thread, and if that thread is the one
// driving the host's UI the whole app stops with no crash and nothing logged.
// ServerBox mounted one at `/dev/shm` for every Linux system it attached, so
// the first detach froze it.
//
// Running this under `-Db_sanitize=address` is the point of the tree below:
// the release order is the part that is easy to get wrong, and a child
// releasing its parent while that parent's list of children is being walked
// reads as a use-after-free rather than as a failed assertion.
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/init.h"
#include "kernel/task.h"
#include "fs/fd.h"
#include "fs/path.h"
#include "fs/real.h"
#include "unit.h"

static void make_dir(const char *path) {
    int err = generic_mkdirat(AT_PWD, path, 0755);
    if (err < 0 && err != _EEXIST)
        fprintf(stderr, "mkdir %s: %d\n", path, err);
}

static void make_file(const char *path) {
    struct fd *fd = generic_open(path, O_WRONLY_ | O_CREAT_, 0644);
    if (IS_ERR(fd)) {
        fprintf(stderr, "open %s: %d\n", path, (int) PTR_ERR(fd));
        return;
    }
    fd_close(fd);
}

TEST(an_empty_tmpfs_unmounts) {
    make_dir("/empty");
    CHECK_EQ_INT(do_mount(&tmpfs, "tmp", "/empty", "", 0), 0);
    CHECK_EQ_INT(do_umount("/empty"), 0);
}

TEST(a_tmpfs_with_a_tree_in_it_unmounts) {
    make_dir("/tree");
    CHECK_EQ_INT(do_mount(&tmpfs, "tmp", "/tree", "", 0), 0);

    // Deep and wide, because the release walks both: a child holds a
    // reference on its directory, so the order the two come apart in is what
    // this is here to pin.
    make_dir("/tree/a");
    make_dir("/tree/a/b");
    make_dir("/tree/a/b/c");
    make_file("/tree/a/b/c/deep");
    make_file("/tree/a/one");
    make_file("/tree/a/two");
    make_dir("/tree/sibling");
    make_file("/tree/sibling/file");
    make_file("/tree/top");

    CHECK_EQ_INT(do_umount("/tree"), 0);
}

TEST(unmounting_twice_is_not_a_crash) {
    make_dir("/twice");
    CHECK_EQ_INT(do_mount(&tmpfs, "tmp", "/twice", "", 0), 0);
    CHECK_EQ_INT(do_umount("/twice"), 0);
    // Nothing there any more, which do_umount reports as `_EINVAL` rather than
    // by taking the process with it.
    CHECK_EQ_INT(do_umount("/twice"), _EINVAL);
}

TEST(a_mount_still_in_use_is_refused_and_survives_it) {
    make_dir("/busy");
    CHECK_EQ_INT(do_mount(&tmpfs, "tmp", "/busy", "", 0), 0);
    make_file("/busy/held");
    struct fd *fd = generic_open("/busy/held", O_RDONLY_, 0);
    CHECK(!IS_ERR(fd));
    CHECK_EQ_INT(do_umount("/busy"), _EBUSY);
    // And once it is let go, the same mount comes apart normally — the
    // refused attempt must not have released anything on its way out.
    fd_close(fd);
    CHECK_EQ_INT(do_umount("/busy"), 0);
}

int main(void) {
    // A realfs root, not a tmpfs one. `mount_root` runs before
    // `become_first_process`, and tmpfs allocates its root inode through
    // `tmp_inode_new`, which reads `current->euid` — there is no `current`
    // that early, and the mount segfaults. realfs does not need one. It also
    // matches what an embedder does: the tmpfs under test is a sub-mount, the
    // way `/dev/shm` is.
    char tmpdir[] = "/tmp/tmpfs_umount_test.XXXXXX";
    if (mkdtemp(tmpdir) == NULL) {
        perror("mkdtemp");
        return 2;
    }
    int err = mount_root(&realfs, tmpdir);
    if (err < 0) {
        fprintf(stderr, "mount_root: %d\n", err);
        return 2;
    }
    if ((err = become_first_process()) < 0) {
        fprintf(stderr, "become_first_process: %d\n", err);
        return 2;
    }
    struct fd *root = generic_open("/", O_RDONLY_, 0);
    if (IS_ERR(root)) {
        fprintf(stderr, "open /: %d\n", (int) PTR_ERR(root));
        return 2;
    }
    fs_chdir(current->fs, root);

    RUN(an_empty_tmpfs_unmounts);
    RUN(a_tmpfs_with_a_tree_in_it_unmounts);
    RUN(unmounting_twice_is_not_a_crash);
    RUN(a_mount_still_in_use_is_refused_and_survives_it);
    int status = UNIT_REPORT();
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
    if (system(cmd) != 0)
        fprintf(stderr, "note: could not remove %s\n", tmpdir);
    return status;
}
