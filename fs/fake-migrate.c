#include "kernel/fs.h"
#include "debug.h"
#include "kernel/errno.h"
#include "fs/fake-db.h"
#include "fs/sqlutil.h"

// A migration that fails fails the mount, rather than the app: see the note in
// sqlutil.h. Every macro below lands on the sql_err label at the end of
// fakefs_migrate.
#define HANDLE_ERR(db) do { \
    printk("fakefs migrate: sqlite error: %s\n", sqlite3_errmsg(db)); \
    goto sql_err; \
} while (0)

// The value of the user_version pragma is used to decide what needs migrating.

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
static struct migration {
    const char *sql;
    void (*migrate)(struct fakefs_db *fs);
} migrations[] = {
    // version 1: add another index
    {
        "create index inode_to_path on paths (inode, path);"
    },
    // version 2: add foreign key constraint on paths, create trigger to automatically cleanup stats
    {
        "create table paths_new (path blob primary key, inode integer references stats(inode));"
        "insert into paths_new select * from paths where exists (select 1 from stats where inode = paths.inode);"
        "drop table paths; alter table paths_new rename to paths;"
        "create index inode_to_path on paths (inode, path);"
        "delete from stats where not exists (select 1 from paths where inode = stats.inode);"
        "create trigger delete_path after delete on paths "
        "when not exists (select 1 from paths where inode = old.inode) "
        "begin "
            "delete from stats where not exists (select 1 from paths where inode = old.inode) and inode = old.inode; "
        "end;"
    },
    // version 3: the trigger was a mistake
    {
        "drop trigger delete_path"
    },
};

int fakefs_migrate(struct fakefs_db *fs, int UNUSED(root_fd)) {
    sqlite3 *db = fs->db;
    int err;
    // Both are declared and cleared up here because HANDLE_ERR jumps forward
    // to sql_err from any point in the function, including from above their
    // first assignment.
    sqlite3_stmt *user_version = NULL;
    char *pragma_user_version = NULL;

    user_version = PREPARE("pragma user_version");
    STEP(user_version);
    int version = sqlite3_column_int(user_version, 0);
    FINALIZE(user_version);
    user_version = NULL;

    EXEC("begin");
    int versions = sizeof(migrations)/sizeof(migrations[0]);
    while (version < versions) {
        struct migration m = migrations[version];
        if (m.sql != NULL)
            EXEC(m.sql);
        if (m.migrate != NULL)
            m.migrate(fs);
        version++;
    }
    // for some reason placeholders aren't allowed in pragmas
    pragma_user_version = sqlite3_mprintf("pragma user_version = %d", version);
    if (pragma_user_version == NULL)
        goto sql_err;
    EXEC(pragma_user_version);
    sqlite3_free(pragma_user_version);
    pragma_user_version = NULL;
    EXEC("commit");

    return 0;

sql_err:
    // Anything half-applied goes back: a migration is a set of schema changes
    // that only mean anything together, and user_version is only bumped once
    // they have all run. The rollback is unconditional because the failure may
    // have come from before `begin`, where it is a no-op.
    sqlite3_free(pragma_user_version);
    sqlite3_finalize(user_version);
    sqlite3_exec(db, "rollback", NULL, NULL, NULL);
    return _EIO;
}
