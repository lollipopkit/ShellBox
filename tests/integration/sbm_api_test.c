// The interface an embedder links, driven the way an embedder drives it.
//
// Usage:  sbm_api_test <rootfs>
//
// Everything else in tests/ goes in through the CLI, and the CLI is not what
// ships: ServerBox links libish.a, libish_emu.a and libfakefs.a and calls into
// them from its own code (ios/Runner/ish/sbm_ish.c). That path — boot an init,
// open a pty session under it, write a command in, read the output back, learn
// the exit code — had no test at all. The 39 symbols it takes from the
// libraries were checked for existence with nm and for nothing else.
//
// This is the same sequence, cut down to what a test needs:
//
//   mount_root(&realfs)            an ordinary directory tree
//   become_first_process()         pid 1
//   generic_open("/") + fs_chdir   a working directory to resolve against
//   do_mount(&devptsfs, /dev/pts)  so a session's slave has somewhere to appear
//   do_execve + task_start         init, which must exist and must not exit
//   ...then per session:
//   become_new_init_child()        a task under init, on this thread
//   pty_open_fake(&driver)         a tty whose writes land in our buffer
//   open /dev/pts/N as fd 0,1,2    what makes isatty and /dev/stdout work
//   do_execve + task_start         the command, on a thread of its own
//
// What is deliberately not copied from ServerBox: profiles (its machine root
// holds several trees; here the rootfs is the root), the change-event consumer,
// and the crash handler.
//
// One thing this copies that is worth naming rather than fixing: nothing reaps
// a finished session. Its task stays a zombie, because its parent is an init
// that sleeps and never waits. ServerBox does the same — sbm_ish_close marks
// the slot free and sends SIGHUP, and no one calls wait — so a harness that
// reaped would be modelling something the embedder does not do. Over an app's
// lifetime that is a task, a tgroup and a pid per session, never returned.

#define ISH_INTERNAL
#include "kernel/calls.h"
#include "kernel/init.h"
#include "kernel/task.h"
#include "fs/dev.h"
#include "fs/devices.h"
#include "fs/fd.h"
#include "fs/path.h"
#include "fs/real.h"
#include "fs/tty.h"
#include "tests/unit/unit.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// — What a session prints ————————————————————————————————————————————
//
// A ring in ServerBox, because a terminal nobody reads must not block the
// guest. A flat buffer here: a test knows how much output it asked for, and an
// overflow should fail loudly rather than silently drop the evidence.

#define OUTPUT_MAX 65536
static struct {
    struct tty *tty;
    char data[OUTPUT_MAX];
    size_t length;
    bool overflowed;
    int exit_code;
    bool exited;
    pid_t_ pid;
} session;

static pthread_mutex_t session_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t session_wrote = PTHREAD_COND_INITIALIZER;

static int console_write(struct tty *tty, const void *buffer, size_t length, bool blocking) {
    (void) blocking;
    pthread_mutex_lock(&session_lock);
    if (tty == session.tty) {
        if (session.length + length > sizeof(session.data))
            session.overflowed = true;
        else {
            memcpy(session.data + session.length, buffer, length);
            session.length += length;
        }
    }
    pthread_cond_broadcast(&session_wrote);
    pthread_mutex_unlock(&session_lock);
    return (int) length;
}

static int console_nop(struct tty *tty) { (void) tty; return 0; }
static void console_cleanup(struct tty *tty) { (void) tty; }

static const struct tty_driver_ops console_ops = {
    .init = console_nop,
    .open = console_nop,
    .close = console_nop,
    .write = console_write,
    .cleanup = console_cleanup,
};

static struct tty *harness_ttys[8];
static struct tty_driver harness_driver = {
    .ops = &console_ops,
    .major = TTY_PSEUDO_SLAVE_MAJOR,
    .ttys = harness_ttys,
    .limit = 8,
};

// Called by the kernel as each task exits. The session's task is the one whose
// status this test is waiting for.
static void guest_exited(struct task *task, int code) {
    pthread_mutex_lock(&session_lock);
    if (task->pid == session.pid) {
        // The hook is handed the raw wait(2) status: an exit code in the high
        // byte, or a signal in the low seven bits. Rendered here the way the
        // CLI's exit_handler does it, which is the shell's convention.
        //
        // Worth knowing for anyone reading this as a model: ServerBox does only
        // `code >> 8`, so a session killed by a signal reports 0 there — the
        // same answer as success.
        session.exit_code = (code & 0x7f) ? 128 + (code & 0x7f) : (code >> 8);
        session.exited = true;
    }
    pthread_cond_broadcast(&session_wrote);
    pthread_mutex_unlock(&session_lock);
}

// — /dev ——————————————————————————————————————————————————————————
//
// A fakefs, which is what ServerBox mounts here and for the same reason: it is
// the only filesystem in the tree that can hold a device node. realfs refuses —
// mknod needs root on the host — and tmpfs has neither mknod nor symlink, so a
// tmpfs /dev cannot even carry /dev/stdout. fakefs keeps rdev in sqlite, so it
// can carry both.
//
// Built in a temporary directory rather than in the rootfs, so a run leaves the
// tree on disk exactly as it found it. Without a /dev at all — which is what
// `ish -r` gives a guest today, see issue #8 — a command redirecting to
// /dev/null does not write to a device, it creates a regular file at that path,
// and the next run inherits it.

static char devfs_dir[PATH_MAX];

// Every way out of this program is _exit — main's, so that a guest thread
// inside the interpreter is not carried through exit handlers, and the timeout
// path's — so no atexit handler runs and each of them has to call this.
static void remove_devfs(void) {
    if (devfs_dir[0] == '\0')
        return;
    char command[PATH_MAX + 16];
    snprintf(command, sizeof(command), "rm -rf '%s'", devfs_dir);
    if (system(command) != 0)
        fprintf(stderr, "note: could not remove %s\n", devfs_dir);
    devfs_dir[0] = '\0';
}

// Removed rather than kept: this file used to build the database by hand —
// schema, root row, the lot — because there was no API for it. There is now,
// added for the CLI's /dev in the same change, and a second copy of a schema is
// a database two programs can disagree about.
// The nodes a shell and the programs it runs expect to find. The same list the
// CLI creates for a fakefs root, which is where it was taken from.
static int make_dev(void) {
    snprintf(devfs_dir, sizeof(devfs_dir), "/tmp/ish-sbm-dev-%d", (int) getpid());
    // Creates the directory, data/ and meta.db with its root row.
    int err = fake_db_create(devfs_dir);
    if (err < 0)
        return err;

    char source[PATH_MAX];
    snprintf(source, sizeof(source), "%s/data", devfs_dir);
    generic_mkdirat(AT_PWD, "/dev", 0755);
    err = do_mount(&fakefs, source, "/dev", "", 0);
    if (err < 0)
        return err;

    generic_mknodat(AT_PWD, "/dev/null", S_IFCHR | 0666, dev_make(MEM_MAJOR, DEV_NULL_MINOR));
    generic_mknodat(AT_PWD, "/dev/zero", S_IFCHR | 0666, dev_make(MEM_MAJOR, DEV_ZERO_MINOR));
    generic_mknodat(AT_PWD, "/dev/full", S_IFCHR | 0666, dev_make(MEM_MAJOR, DEV_FULL_MINOR));
    generic_mknodat(AT_PWD, "/dev/random", S_IFCHR | 0666, dev_make(MEM_MAJOR, DEV_RANDOM_MINOR));
    generic_mknodat(AT_PWD, "/dev/urandom", S_IFCHR | 0666, dev_make(MEM_MAJOR, DEV_URANDOM_MINOR));
    generic_mknodat(AT_PWD, "/dev/tty", S_IFCHR | 0666, dev_make(TTY_ALTERNATE_MAJOR, DEV_TTY_MINOR));
    generic_mknodat(AT_PWD, "/dev/console", S_IFCHR | 0666, dev_make(TTY_ALTERNATE_MAJOR, DEV_CONSOLE_MINOR));
    generic_mknodat(AT_PWD, "/dev/ptmx", S_IFCHR | 0666, dev_make(TTY_ALTERNATE_MAJOR, DEV_PTMX_MINOR));

    // /dev/stdout and friends point into /proc, so they resolve only once it is
    // mounted; the mount happens in boot() before this is called.
    generic_symlinkat("/proc/self/fd/0", AT_PWD, "/dev/stdin");
    generic_symlinkat("/proc/self/fd/1", AT_PWD, "/dev/stdout");
    generic_symlinkat("/proc/self/fd/2", AT_PWD, "/dev/stderr");

    generic_mkdirat(AT_PWD, "/dev/pts", 0755);
    return do_mount(&devptsfs, "devpts", "/dev/pts", "", 0);
}

// — Boot ——————————————————————————————————————————————————————————

// argv and envp reach do_execve as a run of NUL-terminated strings with a
// second NUL at the end, which is the shape execve already has them in.
static size_t pack(char *out, size_t out_size, const char *const *parts, size_t count) {
    size_t position = 0;
    for (size_t i = 0; i < count; i++) {
        size_t length = strlen(parts[i]) + 1;
        if (position + length >= out_size)
            return 0;
        memcpy(out + position, parts[i], length);
        position += length;
    }
    out[position] = '\0';
    return count;
}

static const char *const environment_parts[] = {
    "HOME=/root",
    "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
    "TERM=xterm-256color",
};

static int boot(const char *rootfs) {
    int err = mount_root(&realfs, rootfs);
    if (err < 0)
        return err;
    err = become_first_process();
    if (err < 0)
        return err;
    current->thread = pthread_self();

    // Before anything resolves a relative path, which generic_mkdirat below
    // does.
    struct fd *root = generic_open("/", O_RDONLY_, 0);
    if (IS_ERR(root))
        return (int) PTR_ERR(root);
    fs_chdir(current->fs, root);

    exit_hook = guest_exited;

    // A session's slave is /dev/pts/N, and devptsfs is what makes that path
    // exist. It is a mount rather than a device node, which is why this works
    // on a realfs root at all: realfs cannot mknod without host root, so the
    // rest of /dev is missing here — see issue #8.
    // /proc before /dev: the /dev/std* symlinks point into it.
    generic_mkdirat(AT_PWD, "/proc", 0755);
    err = do_mount(&procfs, "proc", "/proc", "", 0);
    if (err < 0)
        return err;
    if ((err = make_dev()) < 0)
        return err;

    // Init has to exist and must not exit: kernel/exit.c ends the host process
    // when it does. It also must not read a terminal — it has none — so it
    // sleeps. ServerBox's does the same, and its comment records what happened
    // when it was a shell instead.
    char argv[256];
    const char *const parts[] = { "/bin/sh", "-c", "while :; do sleep 2147483647; done" };
    size_t count = pack(argv, sizeof(argv), parts, 3);
    char environment[512];
    pack(environment, sizeof(environment), environment_parts, 3);
    if (count == 0)
        return _E2BIG;

    err = do_execve("/bin/sh", (int) count, argv, environment);
    if (err < 0)
        return err;
    task_start(current);
    return 0;
}

// — A session ——————————————————————————————————————————————————————

// Points fds 0, 1 and 2 at the session's own pty, by opening the slave rather
// than by create_stdio. ServerBox's comment on this is the long version: the
// open is what routes through dev_open and tty_open, which is what claims a
// controlling terminal and makes isatty and /dev/stdout true.
static int attach_stdio(struct tty *tty) {
    char slave[64];
    snprintf(slave, sizeof(slave), "/dev/pts/%d", tty->num);
    struct fd *fd = generic_open(slave, O_RDWR_, 0);
    if (IS_ERR(fd))
        return (int) PTR_ERR(fd);
    if (!S_ISCHR(fd->type)) {
        fd_close(fd);
        return _ENOTTY;
    }
    fd->refcount = 0;
    current->files->files[0] = fd_retain(fd);
    current->files->files[1] = fd_retain(fd);
    current->files->files[2] = fd_retain(fd);
    return 0;
}

// Runs one command in a session of its own and waits for it. Returns the exit
// code, or a negative error from the setup.
static int run(const char *command, char *out, size_t out_size, int timeout_ms) {
    pthread_mutex_lock(&session_lock);
    memset(&session, 0, sizeof(session));
    session.exit_code = -1;
    pthread_mutex_unlock(&session_lock);

    int err = become_new_init_child();
    if (err < 0)
        return err;

    struct tty *tty = pty_open_fake(&harness_driver);
    if (IS_ERR(tty))
        return (int) PTR_ERR(tty);
    tty_set_winsize(tty, (struct winsize_) { .col = 80, .row = 25 });

    // A terminal answers with CRLF and echoes what is written to it. A test
    // comparing output would have to undo both, so turn them off — which is
    // what ServerBox does for a non-interactive session, and for the same
    // reason.
    tty->termios.oflags &= ~OPOST_;
    tty->termios.lflags &= ~ECHO_;

    pthread_mutex_lock(&session_lock);
    session.tty = tty;
    session.pid = current->pid;
    pthread_mutex_unlock(&session_lock);

    if ((err = attach_stdio(tty)) < 0)
        return err;

    char argv[4096];
    const char *const parts[] = { "/bin/sh", "-c", command };
    size_t count = pack(argv, sizeof(argv), parts, 3);
    if (count == 0)
        return _E2BIG;
    char environment[512];
    pack(environment, sizeof(environment), environment_parts, 3);

    if ((err = do_execve("/bin/sh", (int) count, argv, environment)) < 0)
        return err;
    task_start(current);

    // Wait for the exit hook. A deadline rather than a plain wait: a guest that
    // hangs should fail this test rather than the run that contains it.
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long) (timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    int code;
    pthread_mutex_lock(&session_lock);
    while (!session.exited) {
        if (pthread_cond_timedwait(&session_wrote, &session_lock, &deadline) != 0)
            break;
    }
    bool exited = session.exited;
    code = session.exit_code;
    size_t length = session.length;
    if (length >= out_size)
        length = out_size - 1;
    memcpy(out, session.data, length);
    out[length] = '\0';
    bool overflowed = session.overflowed;
    pthread_mutex_unlock(&session_lock);

    if (overflowed)
        return _E2BIG;
    if (!exited) {
        // The task is still running, on its own thread, holding the tty this
        // struct points at — and the next case memsets that struct and hands
        // the same fields to a new session. Nothing here can stop a guest
        // thread inside the interpreter, so carrying on would report results
        // from two sessions at once. Say what happened and stop.
        fprintf(stderr, "\nFAIL: `%s` did not finish within %dms; the guest task "
                        "is still running, so the rest of this test would be "
                        "meaningless\n", command, timeout_ms);
        fflush(stderr);
        remove_devfs();
        _exit(2);
    }
    return code;
}

// Output with the trailing newline removed, which every one of these commands
// ends with and none of the assertions is about.
static void chomp(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r'))
        s[--n] = '\0';
}

// — The contract ——————————————————————————————————————————————————

TEST(a_session_runs_a_command_and_reports_its_output) {
    char out[4096];
    int code = run("echo hello from the guest", out, sizeof(out), 15000);
    chomp(out);
    CHECK_EQ_INT(code, 0);
    CHECK_EQ_INT(strcmp(out, "hello from the guest"), 0);
}

// The value ServerBox surfaces as sbm_ish_exit_code. It arrives through the
// exit hook rather than through wait(), which is the part worth pinning: an
// embedder has no parent process to wait in.
TEST(the_exit_code_comes_back) {
    char out[256];
    CHECK_EQ_INT(run("exit 0", out, sizeof(out), 15000), 0);
    CHECK_EQ_INT(run("exit 42", out, sizeof(out), 15000), 42);
    CHECK_EQ_INT(run("false", out, sizeof(out), 15000), 1);
}

// stderr is the same terminal, so a session sees both streams in order — which
// is what a caller reading one buffer expects.
TEST(stderr_and_stdout_share_the_terminal) {
    char out[4096];
    int code = run("echo one; echo two >&2; echo three", out, sizeof(out), 15000);
    chomp(out);
    CHECK_EQ_INT(code, 0);
    CHECK_EQ_INT(strcmp(out, "one\ntwo\nthree"), 0);
}

// The session really is a terminal, not a pipe. This is what attach_stdio
// exists for: opening /dev/pts/N rather than handing over an adhoc fd is what
// makes isatty true, and ServerBox found that out the hard way.
TEST(the_session_is_a_tty) {
    char out[256];
    int code = run("[ -t 1 ] && echo tty || echo not-a-tty", out, sizeof(out), 15000);
    chomp(out);
    CHECK_EQ_INT(code, 0);
    CHECK_EQ_INT(strcmp(out, "tty"), 0);
}

// /dev/stdout is a symlink into /proc/self/fd, so it resolves only if the
// session's descriptors are in the tables procfs lists.
TEST(dev_stdout_resolves) {
    char out[256];
    int code = run("echo through-dev-stdout > /dev/stdout", out, sizeof(out), 15000);
    chomp(out);
    CHECK_EQ_INT(code, 0);
    CHECK_EQ_INT(strcmp(out, "through-dev-stdout"), 0);
}

// The window size an embedder sets is what the guest reads back. ServerBox
// passes the terminal view's size here, and a shell that gets it wrong wraps
// its own output.
TEST(the_window_size_reaches_the_guest) {
    char out[256];
    int code = run("stty size 2>/dev/null || echo no-stty", out, sizeof(out), 15000);
    chomp(out);
    CHECK_EQ_INT(code, 0);
    // busybox stty prints "rows cols".
    if (strcmp(out, "no-stty") != 0)
        CHECK_EQ_INT(strcmp(out, "25 80"), 0);
}

// Sessions are independent: one exiting does not take another's state, and the
// pid each reports is its own. Run in sequence rather than concurrently —
// concurrency is worth a test of its own and this one is about isolation.
TEST(sessions_do_not_leak_into_each_other) {
    char first[256], second[256];
    CHECK_EQ_INT(run("echo first", first, sizeof(first), 15000), 0);
    CHECK_EQ_INT(run("echo second", second, sizeof(second), 15000), 0);
    chomp(first); chomp(second);
    CHECK_EQ_INT(strcmp(first, "first"), 0);
    CHECK_EQ_INT(strcmp(second, "second"), 0);
}

// A command killed by a signal reports the way a shell does, which is where
// waitid_decode_status and the exit hook meet.
TEST(a_signalled_command_is_reported) {
    char out[256];
    int code = run("sh -c 'kill -TERM $$'", out, sizeof(out), 15000);
    CHECK_EQ_INT(code, 143);
}

// The device nodes a fakefs /dev can hold and a realfs one cannot. This is the
// half of issue #8 that an embedder does not suffer from: ServerBox mounts a
// fakefs here, so its guests have these, while `ish -r` guests do not — a
// redirect to /dev/null there creates a regular file.
TEST(the_device_nodes_work) {
    char out[256];
    int code = run("echo swallowed > /dev/null; echo after-null", out, sizeof(out), 15000);
    chomp(out);
    CHECK_EQ_INT(code, 0);
    CHECK_EQ_INT(strcmp(out, "after-null"), 0);

    // An entropy source that answers is what dnf's libstdc++ wanted and did not
    // get on a realfs root.
    code = run("head -c 8 /dev/urandom | wc -c | tr -d ' '", out, sizeof(out), 15000);
    chomp(out);
    CHECK_EQ_INT(code, 0);
    CHECK_EQ_INT(strcmp(out, "8"), 0);

    // /dev/tty is the session's own terminal, reached by name rather than by
    // descriptor — it resolves only because attach_stdio claimed one.
    code = run("echo via-dev-tty > /dev/tty", out, sizeof(out), 15000);
    chomp(out);
    CHECK_EQ_INT(code, 0);
    CHECK_EQ_INT(strcmp(out, "via-dev-tty"), 0);
}

// init outlives every session. If it had exited, kernel/exit.c would have ended
// this process and nothing after it would run — so reaching here at all is half
// the assertion; the other half is that a session still works afterwards.
TEST(init_is_still_up_at_the_end) {
    char out[256];
    CHECK_EQ_INT(run("echo still-here", out, sizeof(out), 15000), 0);
    chomp(out);
    CHECK_EQ_INT(strcmp(out, "still-here"), 0);
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <rootfs>\n", argv[0]);
        return 2;
    }

    int err = boot(argv[1]);
    if (err < 0) {
        fprintf(stderr, "boot failed: %d\n", err);
        return 2;
    }

    RUN(a_session_runs_a_command_and_reports_its_output);
    RUN(the_exit_code_comes_back);
    RUN(stderr_and_stdout_share_the_terminal);
    RUN(the_session_is_a_tty);
    RUN(dev_stdout_resolves);
    RUN(the_window_size_reaches_the_guest);
    RUN(sessions_do_not_leak_into_each_other);
    RUN(a_signalled_command_is_reported);
    RUN(the_device_nodes_work);
    RUN(init_is_still_up_at_the_end);

    int status = UNIT_REPORT();

    remove_devfs();

    // _exit, not return: init is still running on a thread of its own and
    // returning from main would take the whole process through exit handlers
    // while a guest thread is inside the interpreter.
    fflush(stdout);
    fflush(stderr);
    _exit(status);
}
