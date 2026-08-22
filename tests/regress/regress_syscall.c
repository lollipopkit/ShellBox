// Regression tests for the fs/wait syscall fixes landed on fix/ish-issue-batch.
//
// Each block below pins the observable behaviour of one bug so a future change
// that reintroduces it fails here instead of in a user's shell. The comment on
// each block records what the bug actually looked like before the fix.
//
// Build for the guest (static, so no rootfs libc is required):
//   aarch64-linux-musl-gcc -static -O0 -o regress_syscall regress_syscall.c
// Run inside iSH:
//   ish -r <rootfs> /tmp/regress_syscall
//
// Exit status is 0 when every case passes, 1 otherwise. See tests/regress/run.sh.

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int pass, fail;

static void check(int ok, const char *what) {
    if (ok) {
        pass++;
        printf("  PASS  %s\n", what);
    } else {
        fail++;
        printf("  FAIL  %s   (errno=%d %s)\n", what, errno, strerror(errno));
    }
}

static void section(const char *title) {
    printf("\n=== %s ===\n", title);
}

static double now_sec(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void nap(double seconds) {
    struct timespec t = {(time_t)seconds, (long)((seconds - (int)seconds) * 1e9)};
    nanosleep(&t, NULL);
}

// Burn CPU rather than sleeping, so the child is genuinely running (this is
// what cc1 does, and it is the shape that triggered the waitpid bug).
static void burn(double seconds) {
    double t0 = now_sec();
    volatile unsigned x = 0;
    while (now_sec() - t0 < seconds)
        for (int i = 0; i < 200000; i++)
            x += i;
}

// ---------------------------------------------------------------------------
// #30 — O_DIRECTORY carried the x86 value (1<<16) instead of aarch64's 0x4000,
// so the ENOTDIR check in generic_openat() never fired for any guest, and
// 1<<16 (arm64's O_DIRECT) was misread as O_DIRECTORY. GNU cp probes its
// destination with open(dst, O_PATH|O_DIRECTORY); the bogus success made it
// treat an existing regular file as a directory and fail with
// "cannot create regular file '<dst>/<src>': Not a directory".
// ---------------------------------------------------------------------------
static void test_o_directory(const char *file, const char *dir) {
    section("#30 O_DIRECTORY flag value");

    errno = 0;
    int fd = open(file, O_RDONLY | O_DIRECTORY);
    check(fd < 0 && errno == ENOTDIR, "open(regular file, O_DIRECTORY) -> ENOTDIR");
    if (fd >= 0)
        close(fd);

    errno = 0;
    fd = open(file, O_PATH | O_DIRECTORY);
    check(fd < 0 && errno == ENOTDIR, "open(regular file, O_PATH|O_DIRECTORY) -> ENOTDIR");
    if (fd >= 0)
        close(fd);

    // Guard the other direction: the fix must not break real directories.
    fd = open(dir, O_RDONLY | O_DIRECTORY);
    check(fd >= 0, "open(directory, O_DIRECTORY) still succeeds");
    if (fd >= 0)
        close(fd);

    DIR *dp = opendir(dir);
    check(dp != NULL, "opendir(directory) still succeeds");
    if (dp != NULL) {
        int entries = 0;
        while (readdir(dp) != NULL)
            entries++;
        check(entries > 0, "readdir() returns entries");
        closedir(dp);
    }

    errno = 0;
    fd = open(dir, O_WRONLY);
    check(fd < 0 && errno == EISDIR, "open(directory, O_WRONLY) -> EISDIR");
    if (fd >= 0)
        close(fd);

    // 1<<16 is O_DIRECT on aarch64. It must not be mistaken for O_DIRECTORY.
    errno = 0;
    fd = open(file, O_RDONLY | O_DIRECT);
    check(fd >= 0, "open(regular file, O_DIRECT) succeeds (not read as O_DIRECTORY)");
    if (fd >= 0)
        close(fd);

    check(O_DIRECTORY == 0x4000, "guest O_DIRECTORY == 0x4000 (aarch64 ABI)");
    check((O_DIRECTORY & O_DIRECT) == 0, "O_DIRECTORY does not overlap O_DIRECT");
    check((O_DIRECTORY & O_NOFOLLOW) == 0, "O_DIRECTORY does not overlap O_NOFOLLOW");
    check((O_DIRECTORY & O_PATH) == 0, "O_DIRECTORY does not overlap O_PATH");
}

// ---------------------------------------------------------------------------
// #30 follow-up — O_NOFOLLOW was translated to the host open(), but
// generic_openat() resolves the final path component itself before the
// filesystem's open() runs, so the host flag was a no-op and
// open(symlink, O_NOFOLLOW) wrongly returned an fd to the link target.
// ---------------------------------------------------------------------------
static void test_o_nofollow(const char *file, const char *link) {
    section("#30 O_NOFOLLOW honored during path resolution");

    unlink(link);
    if (symlink(file, link) != 0) {
        printf("  SKIP  cannot create symlink %s: %s\n", link, strerror(errno));
        return;
    }

    errno = 0;
    int fd = open(link, O_RDONLY | O_NOFOLLOW);
    check(fd < 0 && errno == ELOOP, "open(symlink, O_NOFOLLOW) -> ELOOP");
    if (fd >= 0)
        close(fd);

    fd = open(file, O_RDONLY | O_NOFOLLOW);
    check(fd >= 0, "open(regular file, O_NOFOLLOW) still succeeds");
    if (fd >= 0)
        close(fd);

    // Without O_NOFOLLOW the symlink must still resolve.
    fd = open(link, O_RDONLY);
    check(fd >= 0, "open(symlink) without O_NOFOLLOW still follows");
    if (fd >= 0)
        close(fd);

    unlink(link);
}

// ---------------------------------------------------------------------------
// #31 — fakefs_stat()'s hook-routed branch memset the statbuf and then filled
// fields by hand, but never set dev (nor nlink/blksize). fstat() went through
// realfs/copy_stat and did set it, so stat() and fstat() on one file reported
// the same inode with different st_dev. GNU coreutils' psame_inode() reads
// that as "the file was replaced while being copied" and cp refuses to copy.
//
// Runs against whatever path it is given, so it is meaningful on a plain
// rootfs and on a fakefs mount point alike.
// ---------------------------------------------------------------------------
static void test_stat_fstat_agreement(const char *path) {
    section("#31 stat/fstat agreement (psame_inode)");
    printf("  path: %s\n", path);

    struct stat s, f;
    if (stat(path, &s) != 0) {
        printf("  SKIP  stat(%s): %s\n", path, strerror(errno));
        return;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("  SKIP  open(%s): %s\n", path, strerror(errno));
        return;
    }
    if (fstat(fd, &f) != 0) {
        printf("  SKIP  fstat(%s): %s\n", path, strerror(errno));
        close(fd);
        return;
    }

    check(s.st_dev == f.st_dev, "stat.st_dev == fstat.st_dev");
    check(s.st_ino == f.st_ino, "stat.st_ino == fstat.st_ino");
    check(s.st_dev != 0, "stat.st_dev != 0");
    check(s.st_nlink > 0, "stat.st_nlink > 0 (0 reads as an unlinked file)");
    check(s.st_blksize > 0, "stat.st_blksize > 0 (0 breaks cp's buffer sizing)");
    // Fields that were already correct before the fix must stay correct.
    check(s.st_size == f.st_size, "stat.st_size == fstat.st_size");
    check(s.st_mode == f.st_mode, "stat.st_mode == fstat.st_mode");
    check(s.st_mtime == f.st_mtime, "stat.st_mtime == fstat.st_mtime");
    close(fd);
}

// ---------------------------------------------------------------------------
// #29 — do_wait() waits with a 1-second bounded timeout (a workaround for
// macOS condvars). It tested `if (wait_for(...))`, which is true for both
// _EINTR and _ETIMEDOUT, so every expiry of that internal timeout was
// reported to the guest as a signal and waitpid returned EINTR. Any child
// running longer than a second broke its parent; gcc died with
// "failed to get exit status: Interrupted system call" at ~1.01s.
// ---------------------------------------------------------------------------
static void test_waitpid_across_timeout(void) {
    section("#29 waitpid across the 1s internal timeout");

    // Straddle the boundary: the bug appears the moment a child outlives it.
    static const double points[] = {0.2, 0.9, 1.0, 1.1, 1.5, 2.0};
    int bad = 0;
    for (unsigned i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
        pid_t p = fork();
        if (p == 0) {
            // Alternate sleeping and burning so both blocked and runnable
            // children are covered.
            if (i % 2)
                burn(points[i]);
            else
                nap(points[i]);
            _exit(40 + i);
        }
        int st = 0;
        errno = 0;
        pid_t r = waitpid(p, &st, 0);
        int ok = (r == p && WIFEXITED(st) && WEXITSTATUS(st) == (int)(40 + i));
        if (!ok) {
            bad++;
            printf("  child %.1fs -> waitpid=%d errno=%d(%s) exited=%d code=%d\n",
                   points[i], (int)r, errno, r < 0 ? strerror(errno) : "-",
                   WIFEXITED(st), WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        }
    }
    check(bad == 0, "children at 0.2/0.9/1.0/1.1/1.5/2.0s all reaped, no EINTR");

    // The zombie must actually be gone once waitpid returns.
    pid_t p = fork();
    if (p == 0) {
        nap(1.2);
        _exit(44);
    }
    int st = 0;
    check(waitpid(p, &st, 0) == p && WEXITSTATUS(st) == 44, "slow child reaped");
    errno = 0;
    check(waitpid(p, &st, WNOHANG) < 0 && errno == ECHILD,
          "no leftover zombie (second wait -> ECHILD)");

    // Several concurrent slow children, reaped through wait().
    pid_t kids[3];
    for (int i = 0; i < 3; i++) {
        kids[i] = fork();
        if (kids[i] == 0) {
            burn(1.1 + 0.2 * i);
            _exit(50 + i);
        }
    }
    int reaped = 0;
    for (int i = 0; i < 3; i++) {
        int s2;
        pid_t r = wait(&s2);
        if (r > 0 && WIFEXITED(s2) && WEXITSTATUS(s2) >= 50 && WEXITSTATUS(s2) <= 52)
            reaped++;
    }
    check(reaped == 3, "3 concurrent slow children all reaped via wait()");
    errno = 0;
    check(wait(&st) < 0 && errno == ECHILD, "wait() -> ECHILD when no children remain");
}

// ---------------------------------------------------------------------------
// A real signal must still interrupt waitpid. The #29 fix narrows got_signal
// to _EINTR only, so this guards against over-correcting it away.
// ---------------------------------------------------------------------------
static volatile sig_atomic_t handler_ran;
static void on_usr1(int sig) { (void)sig; handler_ran = 1; }

static void test_signal_still_interrupts(void) {
    section("#29 real signals still interrupt waitpid");

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;
    sigaction(SIGUSR1, &sa, NULL);
    handler_ran = 0;

    pid_t child = fork();
    if (child == 0) {
        nap(4);
        _exit(7);
    }
    pid_t killer = fork();
    if (killer == 0) {
        nap(0.3);
        kill(getppid(), SIGUSR1);
        _exit(0);
    }

    double t0 = now_sec();
    int st = 0;
    errno = 0;
    pid_t r = waitpid(child, &st, 0);
    double elapsed = now_sec() - t0;

    check(r < 0 && errno == EINTR, "SIGUSR1 during waitpid -> EINTR");
    check(handler_ran == 1, "signal handler ran");
    // If this fires at ~1s it means the internal timeout was misreported again.
    check(elapsed < 1.0, "interrupted at the signal, not at the 1s timeout");
    check(waitpid(child, &st, 0) == child && WEXITSTATUS(st) == 7,
          "child still reapable after EINTR");
    waitpid(killer, NULL, 0);
}

// ---------------------------------------------------------------------------
// waitid siginfo — do_wait() produces the raw wait(2) status word, which is
// what wait4 wants. sys_waitid handed that straight to userspace instead of
// decoding it, so si_status was 7936 for exit(31) (31<<8) and si_code was
// always 0, leaving a killed child indistinguishable from an exited one.
// si_uid was never assigned at all on the wait path.
// ---------------------------------------------------------------------------
// Whether `p` is a child, recording a failure when it is not.
//
// A fork that failed is not a pid, and -1 has a meaning of its own: `kill(-1,
// …)` is "every process this caller may signal". Two of the probes below send
// SIGSTOP and SIGKILL, so an unchecked fork does not fail a case here — it
// signals the whole guest, this binary and the shell that started it included,
// and the run ends stopped or dead rather than reporting anything.
//
// The guest, not the host: this is built for the guest and runs under `ish`,
// so the -1 reaches ish's own sys_kill and the tasks it knows about. That
// bounds the damage without making it acceptable.
static int forked_ok(pid_t p, const char *what) {
    if (p >= 0)
        return 1;
    check(0, what);
    return 0;
}

static void test_waitid_siginfo(void) {
    section("waitid si_status / si_code / si_uid");

    uid_t me = getuid();

    // exit(N) -> CLD_EXITED / N, including both ends of the byte.
    static const int codes[] = {0, 31, 255};
    for (unsigned i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        pid_t p = fork();
        if (p == 0)
            _exit(codes[i]);
        if (!forked_ok(p, "fork for the exit-status case"))
            continue;
        siginfo_t si;
        memset(&si, 0, sizeof si);
        int r = waitid(P_PID, p, &si, WEXITED);
        char msg[96];
        snprintf(msg, sizeof msg, "exit(%d) -> CLD_EXITED, si_status=%d", codes[i], codes[i]);
        check(r == 0 && si.si_code == CLD_EXITED && si.si_status == codes[i], msg);
        if (i == 1) {
            check(si.si_signo == SIGCHLD, "si_signo == SIGCHLD");
            check(si.si_pid == p, "si_pid == child pid");
            check(si.si_uid == me, "si_uid == child uid");
        }
    }

    // Killed by a signal -> CLD_KILLED / signal number.
    static const int sigs[] = {SIGKILL, SIGTERM};
    for (unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
        pid_t p = fork();
        if (p == 0) {
            nap(10);
            _exit(0);
        }
        if (!forked_ok(p, "fork for the killed-child case"))
            continue;
        nap(0.3);
        kill(p, sigs[i]);
        siginfo_t si;
        memset(&si, 0, sizeof si);
        int r = waitid(P_PID, p, &si, WEXITED);
        char msg[96];
        snprintf(msg, sizeof msg, "killed by %d -> CLD_KILLED, si_status=%d", sigs[i], sigs[i]);
        check(r == 0 && si.si_code == CLD_KILLED && si.si_status == sigs[i], msg);
        if (i == 0)
            check(si.si_uid == me, "si_uid set on CLD_KILLED too");
    }

    // Stopped -> CLD_STOPPED / stopping signal.
    {
        pid_t p = fork();
        if (p == 0) {
            nap(10);
            _exit(0);
        }
        if (forked_ok(p, "fork for the stopped-child case")) {
            nap(0.3);
            kill(p, SIGSTOP);
            siginfo_t si;
            memset(&si, 0, sizeof si);
            int r = waitid(P_PID, p, &si, WSTOPPED);
            check(r == 0 && si.si_code == CLD_STOPPED && si.si_status == SIGSTOP,
                  "SIGSTOP -> CLD_STOPPED, si_status=SIGSTOP");
            kill(p, SIGCONT);
            kill(p, SIGKILL);
            siginfo_t reap;
            waitid(P_PID, p, &reap, WEXITED);
        }
    }

    // A child that drops privileges must report its own uid, not the parent's.
    // This is the only case that can distinguish the si_uid bug, since
    // everything else here runs as a single uid.
    {
        pid_t p = fork();
        if (p == 0) {
            if (setuid(1000) != 0)
                _exit(99);
            _exit(12);
        }
        if (!forked_ok(p, "fork for the setuid case"))
            return;
        siginfo_t si;
        memset(&si, 0, sizeof si);
        int r = waitid(P_PID, p, &si, WEXITED);
        if (r == 0 && si.si_status == 99) {
            printf("  SKIP  setuid(1000) not permitted here; cannot test uid change\n");
        } else {
            check(r == 0 && si.si_status == 12, "child reached exit(12) after setuid(1000)");
            check(si.si_uid == 1000, "si_uid == 1000 (child's uid, not the parent's)");
        }
    }

    // WNOHANG with nothing ready must not report a scanned candidate's uid.
    {
        pid_t p = fork();
        if (p == 0) {
            nap(1.0);
            _exit(13);
        }
        nap(0.2);
        siginfo_t si;
        memset(&si, 0, sizeof si);
        int r = waitid(P_PID, p, &si, WEXITED | WNOHANG);
        check(r == 0 && si.si_pid == 0, "WNOHANG reports si_pid=0 while the child runs");
        check(si.si_uid == 0, "WNOHANG leaves si_uid=0 (no stale uid)");
        siginfo_t reap;
        waitid(P_PID, p, &reap, WEXITED);
    }

    // wait4/waitpid must keep seeing the *raw* encoding — only waitid decodes.
    {
        pid_t p = fork();
        if (p == 0)
            _exit(31);
        int st = 0;
        pid_t r = waitpid(p, &st, 0);
        check(r == p && WIFEXITED(st) && WEXITSTATUS(st) == 31,
              "waitpid still sees raw status (WIFEXITED/WEXITSTATUS)");

        p = fork();
        if (p == 0) {
            nap(10);
            _exit(0);
        }
        nap(0.3);
        kill(p, SIGKILL);
        st = 0;
        r = waitpid(p, &st, 0);
        check(r == p && WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL,
              "waitpid still sees raw status (WIFSIGNALED/WTERMSIG)");
    }
}

// ---------------------------------------------------------------------------
// A parent that says it will never wait does not get zombies.
//
// POSIX gives two ways to say it — SIGCHLD set to SIG_IGN, or SA_NOCLDWAIT —
// and both mean a child is released as it exits rather than kept for a wait
// that is not coming. Neither was implemented: do_exit marked every child a
// zombie without ever consulting the parent's disposition, and the only path
// that released one ran inside wait(2). `signal(SIGCHLD, SIG_IGN); fork();` —
// the ordinary shape of a forking server that does not collect its children —
// therefore held a task, a tgroup and a pid per child for the parent's life.
//
// The probe is waitpid, not /proc: this procfs answers for live tasks only, so
// a zombie is already invisible there and the obvious test passes against the
// bug.
// ---------------------------------------------------------------------------
// What a SIGCHLD handler is handed, and what the guest's siginfo_t looks like.
//
// #15: si_code was SI_KERNEL (128) and si_status the raw wait(2) word — 7936
// for a child that exited 31 — because do_exit built the siginfo by hand while
// waitid had been taught to decode it. #16: si_utime and si_stime were 32-bit
// at offsets 28 and 32, where an AArch64 guest reads 64-bit ones at 32 and 40,
// so a handler read si_stime as si_utime. The two hid each other: the field was
// at the wrong offset and nothing filled it.
static volatile sig_atomic_t chld_seen;
static int chld_code, chld_status, chld_pid;
static long chld_utime;

static void on_sigchld(int sig, siginfo_t *si, void *ctx) {
    (void) sig; (void) ctx;
    chld_code = si->si_code;
    chld_status = si->si_status;
    chld_pid = si->si_pid;
    chld_utime = (long) si->si_utime;
    chld_seen = 1;
}

static void test_sigchld_siginfo(void) {
    section("SIGCHLD siginfo: CLD_* code, decoded status, 64-bit times");

    struct sigaction sa, saved;
    check(sigaction(SIGCHLD, NULL, &saved) == 0, "read the SIGCHLD disposition");

    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_sigchld;
    sa.sa_flags = SA_SIGINFO;
    check(sigaction(SIGCHLD, &sa, NULL) == 0, "install an SA_SIGINFO handler");

    chld_seen = 0;
    pid_t p = fork();
    if (p == 0) {
        volatile double x = 0;
        for (long i = 0; i < 2000000; i++)
            x += (double) i;
        _exit(31);
    }
    check(p > 0, "fork for the SIGCHLD case");
    for (int i = 0; i < 150 && !chld_seen; i++)
        usleep(20000);
    check(chld_seen, "the handler ran");
    if (chld_seen) {
        check(chld_pid == p, "si_pid is the child");
        check(chld_code == CLD_EXITED, "si_code is CLD_EXITED, not SI_KERNEL");
        check(chld_status == 31, "si_status is 31, not the raw 31<<8");
        // The child spent real time; reading zero means the offset is wrong
        // again, since do_exit does fill this one.
        check(chld_utime > 0, "si_utime is where the guest reads it");
    }
    int st = 0;
    waitpid(p, &st, 0);

    // 128 bytes, which is what the guest's libc allocates. A short write leaves
    // the caller's own bytes in the tail.
    check(sizeof(siginfo_t) == 128, "the guest siginfo_t is 128 bytes");

    check(sigaction(SIGCHLD, &saved, NULL) == 0, "SIGCHLD disposition restored");
}

// ---------------------------------------------------------------------------
// #17: signal 64 is SIGRTMAX on AArch64, and NUM_SIGS made the range 1..63.
static void test_sigrtmax(void) {
    section("signal 64 (SIGRTMAX) is usable");

    check(SIGRTMAX == 64, "the guest libc says SIGRTMAX is 64");
    for (int sig = 62; sig <= SIGRTMAX; sig++) {
        struct sigaction sa, saved;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = SIG_IGN;
        char msg[64];
        snprintf(msg, sizeof msg, "sigaction(%d)", sig);
        int ok = sigaction(sig, NULL, &saved) == 0 && sigaction(sig, &sa, NULL) == 0;
        check(ok, msg);
        snprintf(msg, sizeof msg, "kill(self, %d)", sig);
        check(kill(getpid(), sig) == 0, msg);
        if (ok)
            sigaction(sig, &saved, NULL);
    }
}

// ---------------------------------------------------------------------------
// #18: what waitid is required to reject, report and preserve.
// ---------------------------------------------------------------------------
// #11: an absolute path makes the dirfd irrelevant.
//
// Linux ignores dirfd when the path is absolute rather than validating it.
// Every *at syscall here checked the descriptor first, so `openat(-1, "/", ...)`
// answered EBADF where Linux opens the root. rpm's file-state machine does
// exactly that, and every package in a dnf transaction failed to unpack.
static void test_at_ignores_dirfd_for_absolute_paths(void) {
    section("*at syscalls ignore dirfd for an absolute path");

    int fd = openat(-1, "/", O_RDONLY | O_DIRECTORY);
    check(fd >= 0, "openat(-1, \"/\", O_DIRECTORY)");
    if (fd >= 0)
        close(fd);

    // A descriptor that is merely closed, rather than -1.
    int closed = open("/", O_RDONLY | O_DIRECTORY);
    if (closed >= 0)
        close(closed);
    fd = openat(closed, "/etc", O_RDONLY | O_DIRECTORY);
    check(fd >= 0, "openat(<closed fd>, \"/etc\", O_DIRECTORY)");
    if (fd >= 0)
        close(fd);

    // And the rule is not open()'s alone.
    struct stat st;
    check(fstatat(-1, "/", &st, 0) == 0, "fstatat(-1, \"/\")");

    // A *relative* path with a bad descriptor is still EBADF.
    errno = 0;
    check(openat(-1, "etc", O_RDONLY | O_DIRECTORY) == -1 && errno == EBADF,
          "openat(-1, \"etc\") is still EBADF");
}

static void test_waitid_conformance(void) {
    section("waitid: option and id validation, WNOWAIT, WCONTINUED, si_utime");

    siginfo_t si;
    pid_t p = fork();
    if (p == 0) _exit(3);
    check(p > 0, "fork");
    nap(0.3);

    // An option set that selects no event is EINVAL, not a wait.
    memset(&si, 0, sizeof si);
    errno = 0;
    check(waitid(P_PID, p, &si, WNOHANG) == -1 && errno == EINVAL,
          "waitid with no WEXITED/WSTOPPED/WCONTINUED is EINVAL");

    // P_ALL takes no id, and P_PID takes a real one.
    errno = 0;
    check(waitid(P_ALL, 12345, &si, WEXITED | WNOHANG) == -1 && errno == EINVAL,
          "waitid(P_ALL) with an id is EINVAL");
    errno = 0;
    check(waitid(P_PID, 0, &si, WEXITED | WNOHANG) == -1 && errno == EINVAL,
          "waitid(P_PID, 0) is EINVAL");

    // The child is still there, and WNOWAIT leaves it there.
    memset(&si, 0, sizeof si);
    check(waitid(P_PID, p, &si, WEXITED | WNOWAIT) == 0 && si.si_status == 3,
          "WNOWAIT reports the exited child");
    memset(&si, 0, sizeof si);
    check(waitid(P_PID, p, &si, WEXITED) == 0 && si.si_status == 3,
          "and leaves it for the next wait");

    // P_PGID with id 0 means the caller's own group.
    p = fork();
    if (p == 0) _exit(5);
    nap(0.3);
    memset(&si, 0, sizeof si);
    check(waitid(P_PGID, 0, &si, WEXITED | WNOHANG) == 0 && si.si_pid == p,
          "waitid(P_PGID, 0) means this process group");

    // The times, which waitid reports in the siginfo rather than a rusage.
    p = fork();
    if (p == 0) {
        volatile double x = 0;
        for (long i = 0; i < 2000000; i++)
            x += (double) i;
        _exit(0);
    }
    nap(0.7);
    memset(&si, 0, sizeof si);
    waitid(P_PID, p, &si, WEXITED);
    check((long) si.si_utime > 0, "waitid fills si_utime");

    // A stop, reported twice under WNOWAIT, and then a continue.
    p = fork();
    if (p == 0) { for (;;) pause(); }
    nap(0.3);
    kill(p, SIGSTOP);
    nap(0.3);
    memset(&si, 0, sizeof si);
    int r1 = waitid(P_PID, p, &si, WSTOPPED | WNOWAIT | WNOHANG);
    int first = si.si_status;
    memset(&si, 0, sizeof si);
    int r2 = waitid(P_PID, p, &si, WSTOPPED | WNOHANG);
    check(r1 == 0 && first == SIGSTOP && r2 == 0 && si.si_status == SIGSTOP,
          "WNOWAIT leaves a stop for the next wait");

    kill(p, SIGCONT);
    nap(0.4);
    memset(&si, 0, sizeof si);
    check(waitid(P_PID, p, &si, WCONTINUED | WNOHANG) == 0 &&
          si.si_code == CLD_CONTINUED,
          "WCONTINUED reports a continued child");
    kill(p, SIGKILL);
    waitpid(p, NULL, 0);
}

static void *regress_worker(void *arg) {
    (void) arg;
    usleep(400000);
    return NULL;
}

static void test_no_zombies_when_parent_will_not_wait(void) {
    section("SIGCHLD ignored / SA_NOCLDWAIT release children");

    struct sigaction sa, saved;
    // Saved and put back at the end. Every case after this one forks and waits,
    // and SIG_DFL is not necessarily what was here — assuming it is makes this
    // test a thing that changes the ones after it.
    check(sigaction(SIGCHLD, NULL, &saved) == 0, "read the SIGCHLD disposition");

    // Set it, rather than assume it: if this program was launched by something
    // that had already ignored SIGCHLD, the control below would be auto-reaped
    // and would fail against a correct kernel. A suite has to own the state it
    // makes assertions about.
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_DFL;
    check(sigaction(SIGCHLD, &sa, NULL) == 0, "SIGCHLD forced to SIG_DFL first");

    // The control. A parent that has not said anything keeps its zombie, and
    // must: it is still entitled to wait for it.
    pid_t child = fork();
    if (child == 0)
        _exit(0);
    nap(0.3);
    int status = 0;
    check(waitpid(-1, &status, WNOHANG) == child,
          "default disposition still keeps the zombie");

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    check(sigaction(SIGCHLD, &sa, NULL) == 0, "sigaction(SIGCHLD, SIG_IGN)");
    child = fork();
    if (child == 0)
        _exit(0);
    nap(0.3);
    errno = 0;
    check(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD,
          "SIG_IGN releases the child (wait answers ECHILD)");

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_DFL;
    sa.sa_flags = SA_NOCLDWAIT;
    check(sigaction(SIGCHLD, &sa, NULL) == 0, "sigaction(SIGCHLD, SA_NOCLDWAIT)");
    child = fork();
    if (child == 0)
        _exit(0);
    nap(0.3);
    errno = 0;
    check(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD,
          "SA_NOCLDWAIT releases the child (wait answers ECHILD)");

    // The times a released child spent are still the parent's to read. Nothing
    // reaps it, so the accounting reap_if_zombie does had to be done where the
    // release is — and was not, at first.
    {
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = SIG_IGN;
        sigaction(SIGCHLD, &sa, NULL);

        struct rusage before, after;
        getrusage(RUSAGE_CHILDREN, &before);
        pid_t p = fork();
        if (p == 0) {
            volatile double x = 0;
            for (long i = 0; i < 3000000; i++)
                x += (double) i;
            _exit(0);
        }
        nap(1.0);
        getrusage(RUSAGE_CHILDREN, &after);
        long delta = (long)(after.ru_utime.tv_sec - before.ru_utime.tv_sec) * 1000000
                   + (after.ru_utime.tv_usec - before.ru_utime.tv_usec);
        check(delta > 0, "a released child's CPU time reaches RUSAGE_CHILDREN");
    }

    // A group whose leader exits first still has to be released whole. The
    // release names the leader rather than the last thread out for this reason:
    // naming the last one freed the group and left the leader's task and pid
    // waitable for the rest of the parent's life.
    {
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = SIG_IGN;
        sigaction(SIGCHLD, &sa, NULL);

        pid_t p = fork();
        if (p == 0) {
            pthread_t t;
            if (pthread_create(&t, NULL, regress_worker, NULL) != 0)
                _exit(1);
            pthread_exit(NULL);   // the leader goes first; the worker is last
        }
        check(p > 0, "fork for the leader-exits-first case");
        if (p > 0) {
            nap(1.5);
            errno = 0;
            int st = 0;
            check(waitpid(-1, &st, WNOHANG) == -1 && errno == ECHILD,
                  "a group whose leader exited first is released whole");
        }
    }

    check(sigaction(SIGCHLD, &saved, NULL) == 0, "SIGCHLD disposition restored");
}

// pselect6 with a null sigmask.
//
// The 6th argument is optional and null means "no mask", exactly as the
// timeout does. sys_pselect read it regardless, and `user_get(0, ...)` fails —
// so the call answered EFAULT without ever looking at a descriptor.
//
// It reached only glibc programs, which is why it survived so long: glibc has
// no `select` syscall to use on arm64 and passes NULL here, while musl passes
// a pointer to {0, _NSIG/8}. Alpine was unaffected and every glibc
// distribution had a `select` that could not succeed. apt reads the failure as
// its download method having died at startup and kills a healthy one, so
// `apt-get update` could not fetch anything.
//
// Called through syscall() rather than select(), so this pins the kernel's
// contract rather than whichever libc built the test.
static void test_pselect6_null_sigmask(void) {
    section("pselect6 with a null sigmask");

    int p[2];
    if (pipe(p) != 0) {
        check(0, "pipe for the pselect test");
        return;
    }

    fd_set r;
    FD_ZERO(&r);
    FD_SET(p[0], &r);
    struct timespec ts = {0, 0};   // poll and return, so this cannot hang

    // No mask: the argument glibc's select() passes.
    //
    // The pipe is empty and the timeout is zero, so the whole correct answer
    // is "nothing ready" — asserting only `rc >= 0` would have accepted a
    // spurious readiness just as happily. Nothing is written to the pipe until
    // both of these have run, so there is nothing for a wrong answer to be
    // right about.
    long rc = syscall(SYS_pselect6, p[0] + 1, &r, NULL, NULL, &ts, NULL);
    check(rc == 0 && !FD_ISSET(p[0], &r), "pselect6 accepts a null sigmask");

    // A packed struct whose *inner* pointer is null. A distinct path from the
    // one above — the argument is read, and only then found to carry no mask —
    // and not a mask case: the kernel skips user_get_sigset entirely, as Linux
    // does. test_pselect6_sigmask below is what covers a mask being applied.
    struct {
        const void *mask;
        unsigned long size;
    } sig = {NULL, 8};
    FD_ZERO(&r);
    FD_SET(p[0], &r);
    ts = (struct timespec) {0, 0};
    rc = syscall(SYS_pselect6, p[0] + 1, &r, NULL, NULL, &ts, &sig);
    check(rc == 0 && !FD_ISSET(p[0], &r), "pselect6 accepts a packed struct holding a null mask");

    // A readable descriptor is reported, so the call is doing its job and not
    // just returning 0 for everything.
    write(p[1], "x", 1);
    FD_ZERO(&r);
    FD_SET(p[0], &r);
    ts = (struct timespec) {0, 0};
    rc = syscall(SYS_pselect6, p[0] + 1, &r, NULL, NULL, &ts, NULL);
    check(rc == 1 && FD_ISSET(p[0], &r), "pselect6 reports a readable pipe");

    close(p[0]);
    close(p[1]);
}

// A no-op handler. SIG_IGN would not do: an ignored signal does not interrupt
// a wait either, so the control case below could not tell a mask that works
// from one that was never applied.
static volatile sig_atomic_t pselect_usr1_count;

static void pselect_sigusr1(int sig) { (void) sig; pselect_usr1_count++; }

// Sends SIGUSR1 to this process, but not before the caller says it is about to
// wait. `*go_fd` is the write end to say it on. Returns the child, or -1, which
// every caller checks — an unnoticed fork failure would make both cases below
// wait out their full timeout, and the control one would then "fail" for a
// reason that has nothing to do with masks.
//
// A bare sleep was a race rather than a barrier. Nothing stopped the child
// reaching kill() while the parent was still between fork() and the syscall,
// and a signal landing there is delivered *unmasked*: the wait that follows is
// then never challenged, times out, and every assertion in the masked case
// passes having tested nothing. The caller also asserts none arrived before
// entry, which is what closes the remaining gap between the two.
static pid_t pselect_signal_on_go(int *go_fd) {
    int ready[2];
    if (pipe(ready) != 0)
        return -1;
    pid_t parent = getpid();
    pid_t p = fork();
    if (p < 0) {
        close(ready[0]);
        close(ready[1]);
        return -1;
    }
    if (p == 0) {
        close(ready[1]);
        char go;
        while (read(ready[0], &go, 1) < 0 && errno == EINTR)
            continue;
        struct timespec t = {0, 200 * 1000 * 1000};
        nanosleep(&t, NULL);
        kill(parent, SIGUSR1);
        _exit(0);
    }
    close(ready[0]);
    *go_fd = ready[1];
    return p;
}

// The mask in the 6th argument is applied for the duration of the wait.
//
// Separate from the null-sigmask cases above because it asserts the opposite
// thing: those are about the argument being *optional*, this is about it being
// *used*. A test that only checked a masked call returns >= 0 would pass on a
// kernel that ignored the mask entirely, so each case here is paired with its
// control and the two must come out differently.
//
// Real timeouts rather than the zero-length ones above, so this also covers a
// wait that has to actually elapse — which is the shape apt uses.
static void test_pselect6_sigmask(void) {
    section("pselect6 applies the mask it is given");

    struct sigaction sa, old;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = pselect_sigusr1;    // no SA_RESTART: the wait must report EINTR
    if (sigaction(SIGUSR1, &sa, &old) != 0) {
        check(0, "install a SIGUSR1 handler");
        return;
    }

    int p[2];
    if (pipe(p) != 0) {
        check(0, "pipe for the sigmask test");
        sigaction(SIGUSR1, &old, NULL);
        return;
    }

    // The sender is a child, so its exit raises SIGCHLD at about the moment
    // SIGUSR1 arrives. Blocked for the duration, so an interrupted wait below
    // can only be attributable to SIGUSR1. The masked case cannot rely on this
    // one — sigmask_set_temp *replaces* the mask rather than adding to it — so
    // it names SIGCHLD in the mask it passes instead.
    sigset_t chld, oldset;
    sigemptyset(&chld);
    sigaddset(&chld, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &chld, &oldset) != 0) {
        check(0, "block SIGCHLD for the sigmask test");
        close(p[0]);
        close(p[1]);
        sigaction(SIGUSR1, &old, NULL);
        return;
    }

    // The go-ahead below is a write to a pipe the sender is reading. If that
    // sender ever dies first the write raises SIGPIPE, whose default action
    // would take the whole regression binary down — no result line, no failed
    // assertion, just an exit status. A test harness should report a broken
    // fixture, not disappear because of one.
    struct sigaction pipe_ign, pipe_old;
    memset(&pipe_ign, 0, sizeof(pipe_ign));
    pipe_ign.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &pipe_ign, &pipe_old);

    fd_set r;
    long rc;

    // Control: no mask, so the signal must cut the wait short.
    pselect_usr1_count = 0;
    int go = -1;
    pid_t killer = pselect_signal_on_go(&go);
    if (killer < 0) {
        check(0, "fork the signal sender (control)");
    } else {
        FD_ZERO(&r);
        FD_SET(p[0], &r);
        struct timespec ts = {5, 0};
        check(pselect_usr1_count == 0, "no SIGUSR1 before the control wait");
        if (write(go, "g", 1) != 1)
            check(0, "signal the control sender to go");

        double t0 = now_sec();
        rc = syscall(SYS_pselect6, p[0] + 1, &r, NULL, NULL, &ts, NULL);
        double waited = now_sec() - t0;
        check(rc < 0 && errno == EINTR, "an unmasked signal interrupts pselect6");
        check(waited < 2.0, "the interrupted wait returned early");
        check(pselect_usr1_count == 1, "the control's SIGUSR1 was delivered");
        close(go);
        waitpid(killer, NULL, 0);
    }

    // The same again with SIGUSR1 blocked through the packed argument. The
    // wait has to survive the signal and run to its own timeout.
    pselect_usr1_count = 0;
    go = -1;
    killer = pselect_signal_on_go(&go);
    if (killer < 0) {
        check(0, "fork the signal sender (masked)");
    } else {
        // The guest's sigset is one word. SIGCHLD for the reason above.
        uint64_t mask = (1ull << (SIGUSR1 - 1)) | (1ull << (SIGCHLD - 1));
        struct {
            const void *mask;
            unsigned long size;
        } sig = {&mask, sizeof(mask)};
        FD_ZERO(&r);
        FD_SET(p[0], &r);
        struct timespec ts = {1, 0};
        // Nothing has been delivered yet, so the arrival asserted below
        // happened while the mask was on rather than before it went on. The
        // handshake keeps the sender behind this point; this is what says so
        // rather than assuming it.
        check(pselect_usr1_count == 0, "no SIGUSR1 before the masked wait");
        if (write(go, "g", 1) != 1)
            check(0, "signal the masked sender to go");

        double t0 = now_sec();
        rc = syscall(SYS_pselect6, p[0] + 1, &r, NULL, NULL, &ts, &sig);
        double waited = now_sec() - t0;
        check(rc == 0, "a masked signal does not interrupt pselect6");
        check(waited > 0.8, "the masked wait ran to its own timeout");
        // And the signal really was sent. Without this the two checks above
        // pass just as well when the sender never ran or its kill() failed —
        // a wait nobody interrupted also reaches its timeout — so the case
        // could report a mask as effective having never tested one.
        //
        // It arrives once the wait puts the old mask back, which is the other
        // half of what a temporary mask means.
        check(pselect_usr1_count == 1, "the masked SIGUSR1 arrived once the wait ended");
        close(go);
        waitpid(killer, NULL, 0);
    }

    close(p[0]);
    close(p[1]);
    sigaction(SIGPIPE, &pipe_old, NULL);
    sigprocmask(SIG_SETMASK, &oldset, NULL);
    sigaction(SIGUSR1, &old, NULL);
}

int main(int argc, char **argv) {
    // Optional extra path to check stat/fstat on, e.g. a fakefs mount point.
    const char *extra_path = argc > 1 ? argv[1] : NULL;

    const char *file = "/tmp/regress_file.txt";
    const char *dir = "/tmp/regress_dir";
    const char *link = "/tmp/regress_link";

    unlink(file);
    unlink(link);
    rmdir(dir);
    int fd = open(file, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        fprintf(stderr, "setup: cannot create %s: %s\n", file, strerror(errno));
        return 2;
    }
    write(fd, "regression\n", 11);
    close(fd);
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "setup: cannot create %s: %s\n", dir, strerror(errno));
        return 2;
    }

    test_o_directory(file, dir);
    test_o_nofollow(file, link);
    test_stat_fstat_agreement(file);
    if (extra_path != NULL)
        test_stat_fstat_agreement(extra_path);
    test_waitpid_across_timeout();
    test_signal_still_interrupts();
    test_waitid_siginfo();
    test_at_ignores_dirfd_for_absolute_paths();
    test_waitid_conformance();
    test_sigchld_siginfo();
    test_sigrtmax();
    test_no_zombies_when_parent_will_not_wait();
    test_pselect6_null_sigmask();
    test_pselect6_sigmask();

    printf("\n================ RESULT ================\n");
    printf("  PASS: %d    FAIL: %d\n", pass, fail);

    unlink(file);
    unlink(link);
    rmdir(dir);
    return fail ? 1 : 0;
}
