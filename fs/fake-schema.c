// The schema every fakefs has, in a translation unit of its own.
//
// Not in fake-db.c, where it belongs by subject: that object also holds
// fake_db_init and friends, which call die() and printk(). tools/fakefsify
// links libfakefs without the kernel, so pulling in that object to reach the
// schema left the linker looking for symbols the tool does not have. A file
// with nothing but the string and one exec keeps the definition single without
// dragging the kernel behind it.
#include <sqlite3.h>
#include "kernel/errno.h"
#include "fs/fake-db.h"

static const char *fake_db_schema =
    "create table meta (id integer unique default 0, db_inode integer);"
    "insert into meta (db_inode) values (0);"
    "create table stats (inode integer primary key, stat blob);"
    "create table paths (path blob primary key, inode integer references stats(inode));"
    // no index is needed on stats, because the rows are ordered by the primary key
    "create index inode_to_path on paths (inode, path);"
    "pragma user_version=3;";

int fake_db_create_schema(sqlite3 *db) {
    return sqlite3_exec(db, fake_db_schema, NULL, NULL, NULL) == SQLITE_OK ? 0 : _EIO;
}
