// fakefs stat/fstat agreement on a hook-routed path.
//
// A path routed through the path-translate hook does not go through meta.db —
// that is the point of the hook — so fakefs_stat answers it from a host stat()
// and fills the guest statbuf by hand. It used to fill only st_ino, leaving
// dev, nlink and blksize zero, while fakefs_fstat on the same file went
// through realfs.fstat/copy_stat and got real values for all of them.
//
// Two calls describing the same file have to agree. coreutils checks exactly
// this: cp calls stat() on the source and fstat() on the descriptor it opened,
// and psame_inode() compares st_dev and st_ino. A dev of 0 from one and a real
// one from the other reads as "the file was replaced while being copied", and
// cp fails.
//
// The assertions here are agreement between the two calls, not particular
// values: what a fake dev number should be is a policy this test has no
// business pinning, but that both callers see the same one is the contract.

// fs/fake.h is guarded as kernel-internal, which is what a test of a kernel
// internal is.
#define ISH_INTERNAL
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "kernel/init.h"
#include "fs/fake.h"
#include "fs/path.h"
#include "fs/fd.h"
#include "fs/stat.h"
#include "tests/unit/unit.h"

#include <fcntl.h>
#include <stdlib.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char tmpdir[PATH_MAX];   // holds data/, meta.db and host/
static char hostdir[PATH_MAX];  // the hook's target, outside the fakefs
static char bounddir[PATH_MAX]; // a bind mount's target, ditto

// Guest paths under here are routed to hostdir. Everything else falls through
// to the ordinary meta.db lookup, which is what an unhooked path must keep
// doing — see stat_still_works_off_the_hooked_path below.
#define HOOK_PREFIX "/hooked"

static bool translate(const char *guest_path, uint64_t fs_context,
                      char *out_host_path, size_t out_size) {
    (void) fs_context;
    size_t prefix_len = strlen(HOOK_PREFIX);
    if (strncmp(guest_path, HOOK_PREFIX, prefix_len) != 0)
        return false;
    if (guest_path[prefix_len] != '\0' && guest_path[prefix_len] != '/')
        return false;
    snprintf(out_host_path, out_size, "%s%s", hostdir, guest_path + prefix_len);
    return true;
}

// The counterpart, and not optional: fakefs_fstat's hook fallback asks
// realfs_getpath() what guest path the descriptor came from, and that answer
// comes from F_GETPATH — a *host* path — put back through this. Without it
// the descriptor looks like an ordinary fakefs one with no meta.db row, which
// is an error rather than a hook-routed file.
static bool reverse(const char *host_path, char *out_guest_path, size_t out_size) {
    size_t host_len = strlen(hostdir);
    if (strncmp(host_path, hostdir, host_len) != 0)
        return false;
    if (host_path[host_len] != '\0' && host_path[host_len] != '/')
        return false;
    snprintf(out_guest_path, out_size, "%s%s", HOOK_PREFIX, host_path + host_len);
    return true;
}

static int fstat_via_fd(const char *path, struct statbuf *out) {
    struct fd *fd = generic_open(path, O_RDONLY_, 0);
    if (IS_ERR(fd))
        return (int) PTR_ERR(fd);
    int err = fd->mount->fs->fstat(fd, out);
    fd_close(fd);
    return err;
}

// The pair cp compares. This is the whole bug.
TEST(stat_and_fstat_agree_on_a_hooked_file) {
    struct statbuf by_path = {}, by_fd = {};
    CHECK_EQ_INT(generic_statat(AT_PWD, HOOK_PREFIX "/doc.txt", &by_path, true), 0);
    CHECK_EQ_INT(fstat_via_fd(HOOK_PREFIX "/doc.txt", &by_fd), 0);

    CHECK_EQ(by_path.dev, by_fd.dev);
    CHECK_EQ(by_path.inode, by_fd.inode);
    CHECK_EQ_INT(by_path.nlink, by_fd.nlink);
    CHECK_EQ_INT(by_path.blksize, by_fd.blksize);
}

// Agreement on two zeroes would satisfy the test above. These are the fields
// the fix added, and a file that exists has none of them at zero.
TEST(the_added_fields_are_populated) {
    struct statbuf st = {};
    CHECK_EQ_INT(generic_statat(AT_PWD, HOOK_PREFIX "/doc.txt", &st, true), 0);
    CHECK(st.dev != 0);
    CHECK(st.nlink != 0);
    CHECK(st.blksize != 0);
    CHECK_EQ(st.size, 5);
}

// A directory reached through the hook takes the same path through
// fakefs_stat, and cp -r stats those too.
TEST(stat_and_fstat_agree_on_a_hooked_directory) {
    struct statbuf by_path = {}, by_fd = {};
    CHECK_EQ_INT(generic_statat(AT_PWD, HOOK_PREFIX, &by_path, true), 0);
    CHECK_EQ_INT(fstat_via_fd(HOOK_PREFIX, &by_fd), 0);

    CHECK_EQ(by_path.dev, by_fd.dev);
    CHECK_EQ(by_path.inode, by_fd.inode);
    CHECK(S_ISDIR(by_path.mode));
}

// The other branch of fakefs_stat that answers from a host stat(): a path
// registered in the bind-mount table rather than routed by the hook. It reads
// the host stat for size, nlink and the timestamps, and takes inode/mode/owner
// from meta.db — but dev, blksize and blocks are the host's to give, and
// leaving them alone put the same stat/fstat disagreement here that the
// hook-routed path had.
TEST(stat_and_fstat_agree_on_a_bind_mounted_file) {
    struct statbuf by_path = {}, by_fd = {};
    // The first stat of a bind-mounted path takes a different branch: no
    // meta.db row exists yet, so it is created and answered from a full
    // realfs.stat. The branch under test is the one every stat after that
    // takes, where the row is found and only some fields are refreshed from
    // the host.
    CHECK_EQ_INT(generic_statat(AT_PWD, "/bound/doc.txt", &by_path, true), 0);
    memset(&by_path, 0, sizeof(by_path));
    CHECK_EQ_INT(generic_statat(AT_PWD, "/bound/doc.txt", &by_path, true), 0);
    CHECK_EQ_INT(fstat_via_fd("/bound/doc.txt", &by_fd), 0);

    CHECK_EQ(by_path.dev, by_fd.dev);
    CHECK_EQ_INT(by_path.blksize, by_fd.blksize);
    CHECK_EQ_INT(by_path.blocks, by_fd.blocks);
    CHECK_EQ_INT(by_path.nlink, by_fd.nlink);
    CHECK(by_path.dev != 0);
}

// The hook is consulted before the meta.db table, not instead of it. A path it
// declines has to keep resolving the ordinary way, or this fix would trade one
// broken case for another.
TEST(stat_still_works_off_the_hooked_path) {
    struct statbuf st = {};
    CHECK_EQ_INT(generic_statat(AT_PWD, "/", &st, true), 0);
    CHECK(S_ISDIR(st.mode));
}

// ── setting up a fakefs by hand ─────────────────────────────────────
// tools/fakefsify would do this from a tarball, but it needs libarchive and a
// rootfs; what is needed here is a mountable empty tree.

static const char *schema =
    "create table meta (id integer unique default 0, db_inode integer);"
    "insert into meta (db_inode) values (0);"
    "create table stats (inode integer primary key, stat blob);"
    "create table paths (path blob primary key, inode integer references stats(inode));"
    "create index inode_to_path on paths (inode, path);"
    "pragma user_version=3;";

static int make_fakefs(void) {
    char path[PATH_MAX];

    snprintf(path, sizeof(path), "%s/data", tmpdir);
    if (mkdir(path, 0755) < 0)
        return -1;

    snprintf(path, sizeof(path), "%s/meta.db", tmpdir);
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK)
        return -1;
    if (sqlite3_exec(db, schema, NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }

    // The root, and nothing else: fakefs stores "/" as the empty blob, and
    // every other path in this test is reached through the hook.
    struct ish_stat root = { .mode = S_IFDIR | 0755 };
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "insert into stats (inode, stat) values (1, ?)", -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    sqlite3_bind_blob(stmt, 1, &root, sizeof(root), SQLITE_STATIC);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return -1;
    }
    sqlite3_finalize(stmt);
    if (sqlite3_exec(db, "insert into paths (path, inode) values (x'', 1);",
                     NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    sqlite3_close(db);

    // The hook's target. Outside the fakefs on purpose: a host directory the
    // guest reaches only because the hook says so is what a bind mount is.
    if (mkdir(hostdir, 0755) < 0)
        return -1;
    if (mkdir(bounddir, 0755) < 0)
        return -1;
    snprintf(path, sizeof(path), "%s/doc.txt", bounddir);
    int bfd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (bfd < 0)
        return -1;
    if (write(bfd, "bound", 5) != 5) {
        close(bfd);
        return -1;
    }
    close(bfd);
    snprintf(path, sizeof(path), "%s/doc.txt", hostdir);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    if (write(fd, "hello", 5) != 5) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

int main(void) {
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/ish-fakefs-stat-test-%d", (int) getpid());
    snprintf(hostdir, sizeof(hostdir), "%s/host", tmpdir);
    snprintf(bounddir, sizeof(bounddir), "%s/bound", tmpdir);
    if (mkdir(tmpdir, 0700) < 0) {
        perror("mkdir");
        return 2;
    }
    if (make_fakefs() < 0) {
        perror("building the fakefs");
        return 2;
    }

    // F_GETPATH answers with the resolved path, and on macOS /tmp is a symlink
    // to /private/tmp — so the reverse hook has to be told the name the kernel
    // will actually hand it, not the one this test used to create the tree.
    char resolved[PATH_MAX];
    if (realpath(hostdir, resolved) == NULL) {
        perror("realpath");
        return 2;
    }
    snprintf(hostdir, sizeof(hostdir), "%s", resolved);
    if (realpath(bounddir, resolved) == NULL) {
        perror("realpath");
        return 2;
    }
    snprintf(bounddir, sizeof(bounddir), "%s", resolved);

    fakefs_set_path_translate_hook(translate);
    fakefs_set_path_reverse_hook(reverse);

    char data[PATH_MAX];
    snprintf(data, sizeof(data), "%s/data", tmpdir);
    int err = mount_root(&fakefs, data);
    if (err < 0) {
        fprintf(stderr, "mount_root: %d\n", err);
        return 2;
    }
    if ((err = become_first_process()) < 0) {
        fprintf(stderr, "become_first_process: %d\n", err);
        return 2;
    }

    // Registered after the root is mounted, which is when an embedder does it.
    if (fakefs_bind_mount("/bound", bounddir, false) < 0) {
        fprintf(stderr, "fakefs_bind_mount failed\n");
        return 2;
    }

    RUN(stat_and_fstat_agree_on_a_hooked_file);
    RUN(the_added_fields_are_populated);
    RUN(stat_and_fstat_agree_on_a_hooked_directory);
    RUN(stat_and_fstat_agree_on_a_bind_mounted_file);
    RUN(stat_still_works_off_the_hooked_path);

    int status = UNIT_REPORT();
    if (status == 0) {
        char cmd[PATH_MAX + 16];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
        if (system(cmd) != 0)
            fprintf(stderr, "note: could not remove %s\n", tmpdir);
    } else {
        fprintf(stderr, "fakefs left in %s\n", tmpdir);
    }
    return status;
}
