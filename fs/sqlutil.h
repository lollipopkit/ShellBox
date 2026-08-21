#ifndef FS_SQLUTIL_H
#define FS_SQLUTIL_H
#include <sqlite3.h>

// Some nice sqlite macros for anything outside of fs/fake.c
//
// Each macro assumes `sqlite3 *db` and `int err` are in scope, and reports a
// failure by expanding HANDLE_ERR(db) — which the file including this header
// has to define, before its first use of any of them. There is deliberately no
// default: this used to be die(), so a meta.db that was corrupt, busy, or on a
// volume iOS had not unlocked yet took the whole app down from inside a
// migration. What a caller should do instead depends on what it can unwind,
// which only the caller knows.
//
//   fs/fake-migrate.c, fs/fake-rebuild.c  goto sql_err, roll back, return _EIO
//   tools/fakefs.c                        fill in a struct error and return false

#define Q(...) #__VA_ARGS__

#define CHECK_ERR() \
    if (err != SQLITE_OK && err != SQLITE_ROW && err != SQLITE_DONE) \
        HANDLE_ERR(db)
#define EXEC(sql) \
    err = sqlite3_exec(db, sql, NULL, NULL, NULL); \
    CHECK_ERR();
#define PREPARE(sql) ({ \
    sqlite3_stmt *stmt; \
    err = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL); \
    CHECK_ERR(); \
    stmt; \
})
#define STEP(stmt) ({ \
    err = sqlite3_step(stmt); \
    CHECK_ERR(); \
    err == SQLITE_ROW; \
})
#define RESET(stmt) \
    err = sqlite3_reset(stmt); \
    CHECK_ERR()
#define STEP_RESET(stmt) \
    STEP(stmt); \
    RESET(stmt)
#define FINALIZE(stmt) \
    err = sqlite3_finalize(stmt); \
    CHECK_ERR()

#endif
