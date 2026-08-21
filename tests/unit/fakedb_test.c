// fake_db_init / fake_db_deinit, against real meta.db files.
//
// These are the paths a mount takes, and until recently a failure in any of
// them was a die() — so there was nothing to assert about. What is asserted
// here is what replaced it: an error fails the mount and leaves nothing
// behind, and doing it twice is safe.
//
// A leak is not visible from inside the process, so the proxy used throughout
// is the host file descriptor count: an sqlite handle that was not closed
// holds at least one, and one held after deinit is exactly the bug these
// paths had. Checked as a delta over a repeated operation, which is what
// distinguishes a leak from a one-off allocation.

#include "fs/fake-db.h"
#include "kernel/errno.h"
#include "tests/unit/unit.h"

#include <dirent.h>
#include <fcntl.h>
#include <sqlite3.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static char tmpdir[256];

// The schema tools/fakefsify writes, at the version fakefs_migrate considers
// current. Kept here rather than shared so that a change to the real one shows
// up as a failing migration test instead of both moving together.
static const char *schema =
    "create table meta (id integer unique default 0, db_inode integer);"
    "insert into meta (db_inode) values (0);"
    "create table stats (inode integer primary key, stat blob);"
    "create table paths (path blob primary key, inode integer references stats(inode));"
    "create index inode_to_path on paths (inode, path);"
    "pragma user_version=3;";

// The same schema at version 0, before any migration has run.
static const char *schema_v0 =
    "create table meta (id integer unique default 0, db_inode integer);"
    "insert into meta (db_inode) values (0);"
    "create table stats (inode integer primary key, stat blob);"
    "create table paths (path blob primary key, inode integer);";

static int open_fd_count(void) {
    // Counting entries under /dev/fd is portable enough for macOS and Linux,
    // which is where this test runs.
    DIR *dir = opendir("/dev/fd");
    if (dir == NULL)
        return -1;
    int n = 0;
    while (readdir(dir) != NULL)
        n++;
    closedir(dir);
    return n;
}

static void make_db(const char *path, const char *sql) {
    unlink(path);
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "could not create %s: %s\n", path, sqlite3_errmsg(db));
        exit(2);
    }
    if (sql != NULL) {
        rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "could not populate %s: %s\n", path, sqlite3_errmsg(db));
            exit(2);
        }
    }
    sqlite3_close(db);
}

static void db_path(char *out, size_t out_size, const char *name) {
    snprintf(out, out_size, "%s/%s", tmpdir, name);
}

// A current-schema database opens, and closes without leaving a descriptor.
TEST(init_then_deinit_is_clean) {
    char path[512];
    db_path(path, sizeof(path), "ok.db");
    make_db(path, schema);

    int before = open_fd_count();
    struct fakefs_db fs;
    int err = fake_db_init(&fs, path, AT_FDCWD);
    CHECK_EQ_INT(err, 0);
    if (err < 0)
        return;
    CHECK(fs.db != NULL);
    CHECK(fs.lock != NULL);
    CHECK(fs.stmt.path_get_inode != NULL);

    int rc = fake_db_deinit(&fs);
    CHECK_EQ_INT(rc, SQLITE_OK);
    // Cleared, so a second call has nothing to double-finalize and the
    // struct can take another init.
    CHECK(fs.db == NULL);
    CHECK(fs.lock == NULL);
    CHECK(fs.stmt.path_get_inode == NULL);

    int after = open_fd_count();
    CHECK_EQ_INT(after, before);
}

// Calling deinit again must not double-finalize or double-free. It used to
// walk the same statement pointers a second time.
TEST(deinit_is_idempotent) {
    char path[512];
    db_path(path, sizeof(path), "twice.db");
    make_db(path, schema);

    struct fakefs_db fs;
    CHECK_EQ_INT(fake_db_init(&fs, path, AT_FDCWD), 0);
    CHECK_EQ_INT(fake_db_deinit(&fs), SQLITE_OK);
    CHECK_EQ_INT(fake_db_deinit(&fs), SQLITE_OK);
    CHECK_EQ_INT(fake_db_deinit(&fs), SQLITE_OK);
}

// The struct is not zeroed by its owner — it lives inside a malloc'd struct
// mount — so init has to start from whatever was there. Filling it with a
// pattern first is what the heap does in the case this was found in.
TEST(init_does_not_read_uninitialized_fields) {
    char path[512];
    db_path(path, sizeof(path), "dirty.db");
    make_db(path, schema);

    struct fakefs_db fs;
    memset(&fs, 0xba, sizeof(fs));
    CHECK_EQ_INT(fake_db_init(&fs, path, AT_FDCWD), 0);
    CHECK(fs.db != NULL);
    CHECK_EQ_INT(fake_db_deinit(&fs), SQLITE_OK);

    // And on a failing init, which is the path that reads them.
    char missing[512];
    db_path(missing, sizeof(missing), "does-not-exist.db");
    unlink(missing);
    memset(&fs, 0xba, sizeof(fs));
    CHECK(fake_db_init(&fs, missing, AT_FDCWD) < 0);
    CHECK(fs.db == NULL);
    CHECK(fs.lock == NULL);
}

// A database that is not one fails the mount and returns, rather than aborting
// the process — which is what this whole path was changed for. Repeated, so a
// descriptor held by a handle that was not closed shows up as a delta.
TEST(bad_database_fails_without_leaking) {
    char path[512];
    db_path(path, sizeof(path), "garbage.db");

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        UNIT_FAIL("could not write %s", path);
        return;
    }
    // Not an SQLite file: the header check fails on the first statement.
    for (int i = 0; i < 4096; i++)
        fputc(0x41, f);
    fclose(f);

    struct fakefs_db fs;
    // Once to settle any first-call allocation, then measured.
    CHECK(fake_db_init(&fs, path, AT_FDCWD) < 0);
    int before = open_fd_count();
    for (int i = 0; i < 8; i++) {
        int err = fake_db_init(&fs, path, AT_FDCWD);
        CHECK(err < 0);
        CHECK(fs.db == NULL);
    }
    int after = open_fd_count();
    CHECK_EQ_INT(after, before);
}

// A missing file, likewise: a mount of a container that is not there.
TEST(missing_database_fails_without_leaking) {
    char path[512];
    db_path(path, sizeof(path), "absent.db");
    unlink(path);

    struct fakefs_db fs;
    CHECK(fake_db_init(&fs, path, AT_FDCWD) < 0);
    int before = open_fd_count();
    for (int i = 0; i < 8; i++)
        CHECK(fake_db_init(&fs, path, AT_FDCWD) < 0);
    int after = open_fd_count();
    CHECK_EQ_INT(after, before);
}

// An open database missing the tables the cached statements name: every
// PREPARE_OR_FAIL past the first fails, which is the partial-teardown case.
// sqlite3_close answers BUSY while a statement is alive, so this used to drop
// the last reference to a handle that was still open.
TEST(missing_tables_fails_without_leaking) {
    char path[512];
    db_path(path, sizeof(path), "empty.db");

    struct fakefs_db fs;
    make_db(path, "create table meta (id integer unique default 0, db_inode integer);"
                  "insert into meta (db_inode) values (0);"
                  "pragma user_version=3;");
    CHECK(fake_db_init(&fs, path, AT_FDCWD) < 0);
    CHECK(fs.db == NULL);
    CHECK(fs.lock == NULL);

    int before = open_fd_count();
    for (int i = 0; i < 8; i++) {
        make_db(path, "create table meta (id integer unique default 0, db_inode integer);"
                      "insert into meta (db_inode) values (0);"
                      "pragma user_version=3;");
        CHECK(fake_db_init(&fs, path, AT_FDCWD) < 0);
    }
    int after = open_fd_count();
    CHECK_EQ_INT(after, before);
}

// A version-0 database migrates on the way in, and the migration is what used
// to die() on any SQLite error. Asserted through the user_version it leaves
// behind and through the mount succeeding.
TEST(migration_runs_and_is_not_repeated) {
    char path[512];
    db_path(path, sizeof(path), "v0.db");
    make_db(path, schema_v0);

    struct fakefs_db fs;
    CHECK_EQ_INT(fake_db_init(&fs, path, AT_FDCWD), 0);
    CHECK_EQ_INT(fake_db_deinit(&fs), SQLITE_OK);

    sqlite3 *db = NULL;
    CHECK_EQ_INT(sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    sqlite3_stmt *stmt = NULL;
    CHECK_EQ_INT(sqlite3_prepare_v2(db, "pragma user_version", -1, &stmt, NULL), SQLITE_OK);
    CHECK_EQ_INT(sqlite3_step(stmt), SQLITE_ROW);
    CHECK_EQ_INT(sqlite3_column_int(stmt, 0), 3);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    // Opening it again finds nothing left to do and still succeeds.
    CHECK_EQ_INT(fake_db_init(&fs, path, AT_FDCWD), 0);
    CHECK_EQ_INT(fake_db_deinit(&fs), SQLITE_OK);
}

// A migration that cannot run rolls back rather than leaving the schema half
// changed. Version 2 rebuilds the paths table; a stray table by the name it
// creates makes that step fail.
TEST(failed_migration_rolls_back) {
    char path[512];
    db_path(path, sizeof(path), "v0-blocked.db");
    make_db(path, schema_v0);
    {
        sqlite3 *db = NULL;
        CHECK_EQ_INT(sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL), SQLITE_OK);
        CHECK_EQ_INT(sqlite3_exec(db, "create table paths_new (x integer)",
                                  NULL, NULL, NULL), SQLITE_OK);
        sqlite3_close(db);
    }

    struct fakefs_db fs;
    // Fails the mount instead of aborting the process.
    CHECK(fake_db_init(&fs, path, AT_FDCWD) < 0);
    CHECK(fs.db == NULL);

    // And left the version where it was, so nothing thinks the migration ran.
    sqlite3 *db = NULL;
    CHECK_EQ_INT(sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    sqlite3_stmt *stmt = NULL;
    CHECK_EQ_INT(sqlite3_prepare_v2(db, "pragma user_version", -1, &stmt, NULL), SQLITE_OK);
    CHECK_EQ_INT(sqlite3_step(stmt), SQLITE_ROW);
    CHECK_EQ_INT(sqlite3_column_int(stmt, 0), 0);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

// The statements a mount actually uses, exercised once each through a
// transaction, so a prepare that succeeded but bound the wrong shape shows up.
TEST(statements_work_after_init) {
    char path[512];
    db_path(path, sizeof(path), "use.db");
    make_db(path, schema);

    struct fakefs_db fs;
    CHECK_EQ_INT(fake_db_init(&fs, path, AT_FDCWD), 0);

    struct ish_stat stat = {.mode = 0100644, .uid = 501, .gid = 20, .rdev = 0};
    db_begin_write(&fs);
    inode_t inode = path_create(&fs, "/hello", &stat);
    db_commit(&fs);
    CHECK(inode != 0);

    db_begin_read(&fs);
    struct ish_stat read_back;
    inode_t read_inode = 0;
    bool found = path_read_stat(&fs, "/hello", &read_back, &read_inode);
    db_commit(&fs);
    CHECK(found);
    CHECK_EQ_INT(read_inode, inode);
    CHECK_EQ(read_back.mode, stat.mode);
    CHECK_EQ_INT(read_back.uid, stat.uid);

    db_begin_read(&fs);
    CHECK(!path_read_stat(&fs, "/nothing-here", NULL, NULL));
    db_commit(&fs);

    // path_delete_tree takes the row and everything under it. '/' is 0x2f and
    // '0' is 0x30, so the range is exactly the children — a sibling whose name
    // merely starts with the same characters must survive.
    db_begin_write(&fs);
    path_create(&fs, "/dir", &stat);
    path_create(&fs, "/dir/a", &stat);
    path_create(&fs, "/dir/b/c", &stat);
    path_create(&fs, "/dirty", &stat);
    db_commit(&fs);

    db_begin_write(&fs);
    path_delete_tree(&fs, "/dir");
    db_commit(&fs);

    db_begin_read(&fs);
    CHECK(!path_read_stat(&fs, "/dir", NULL, NULL));
    CHECK(!path_read_stat(&fs, "/dir/a", NULL, NULL));
    CHECK(!path_read_stat(&fs, "/dir/b/c", NULL, NULL));
    CHECK(path_read_stat(&fs, "/dirty", NULL, NULL));
    CHECK(path_read_stat(&fs, "/hello", NULL, NULL));
    db_commit(&fs);

    CHECK_EQ_INT(fake_db_deinit(&fs), SQLITE_OK);
}

int main(void) {
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/ish-fakedb-test-%d", (int) getpid());
    if (mkdir(tmpdir, 0700) < 0) {
        perror("mkdir");
        return 2;
    }

    RUN(init_then_deinit_is_clean);
    RUN(deinit_is_idempotent);
    RUN(init_does_not_read_uninitialized_fields);
    RUN(bad_database_fails_without_leaking);
    RUN(missing_database_fails_without_leaking);
    RUN(missing_tables_fails_without_leaking);
    RUN(migration_runs_and_is_not_repeated);
    RUN(failed_migration_rolls_back);
    RUN(statements_work_after_init);

    int status = UNIT_REPORT();
    // Left behind on failure, so the databases can be looked at.
    if (status == 0) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
        if (system(cmd) != 0)
            fprintf(stderr, "note: could not remove %s\n", tmpdir);
    } else {
        fprintf(stderr, "databases left in %s\n", tmpdir);
    }
    return status;
}
