#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <syslog.h>
#include <sys/stat.h>
#include <ftw.h>
#include "kernel/init.h"
#include "kernel/fs.h"
#include "fs/devices.h"
#include "fs/dev.h"
#include "fs/path.h"
#include "fs/real.h"
#include "kernel/native_offload.h"
#ifdef __APPLE__
#include <sys/resource.h>
#define IOPOL_TYPE_VFS_HFS_CASE_SENSITIVITY 1
#define IOPOL_VFS_HFS_CASE_SENSITIVITY_FORCE_CASE_SENSITIVE 1
#endif

void real_tty_reset_term(void);

// Where the fakefs backing /dev lives, when one was needed. Empty otherwise.
static char dev_fakefs_dir[PATH_MAX];

static int unlink_entry(const char *path, const struct stat *info, int type, struct FTW *ftw) {
    (void) info; (void) ftw;
    if (remove(path) != 0 && type != FTW_NS)
        fprintf(stderr, "note: could not remove %s\n", path);
    return 0;
}

// A few kilobytes of sqlite in the system temp directory. Removed on the way
// out rather than left for /tmp's own housekeeping — and removed here, in the
// exit handler, because the process leaves through _exit and runs no atexit
// handler. A run killed outright still leaves one behind; it is named for the
// pid, so it is identifiable.
//
// nftw rather than system("rm -rf ..."): the path is built from TMPDIR, and a
// TMPDIR holding a quote or a semicolon would have been a command rather than a
// directory name. FTW_DEPTH visits a directory after its contents, which is the
// order remove() needs; FTW_PHYS keeps it from following a symlink out of the
// tree it is deleting.
static void remove_dev_fakefs(void) {
    if (dev_fakefs_dir[0] == '\0')
        return;
    if (nftw(dev_fakefs_dir, unlink_entry, 8, FTW_DEPTH | FTW_PHYS) != 0)
        fprintf(stderr, "note: could not remove %s\n", dev_fakefs_dir);
    dev_fakefs_dir[0] = '\0';
}

static void exit_handler(struct task *task, int code) {
    if (task->parent != NULL)
        return;
    real_tty_reset_term();
    remove_dev_fakefs();
    if (code & 0xff) {
        // Guest died from a signal. Don't raise on the host (our crash_handler
        // would intercept it). Just exit with the conventional 128+signal code.
        _exit(128 + (code & 0xff));
    }
    exit(code >> 8);
}

// this function parses command line arguments and initializes global
// data structures. thanks programming discussions discord server for the name.
// https://discord.gg/9zT7NHP
static inline int xX_main_Xx(int argc, char *const argv[], const char *envp) {
#ifdef __APPLE__
    // Enable case-sensitive filesystem mode on macOS, if possible.
    // In order for this to succeed, either we need to be running as root, or
    // be given the com.apple.private.iopol.case_sensitivity entitlement. The
    // second option isn't possible so you'll need to give iSH the setuid root
    // bit. In that case it's important to drop root permissions ASAP.
    // https://worthdoingbadly.com/casesensitive-iossim/
    int iopol_err = setiopolicy_np(IOPOL_TYPE_VFS_HFS_CASE_SENSITIVITY,
            IOPOL_SCOPE_PROCESS,
            IOPOL_VFS_HFS_CASE_SENSITIVITY_FORCE_CASE_SENSITIVE);
    if (iopol_err != 0 && errno != EPERM)
        perror("could not enable case sensitivity");
    setgid(getgid());
    setuid(getuid());
#endif

    // parse cli options
    int opt;
    const char *root = NULL;
    const char *workdir = NULL;
    const struct fs_ops *fs = &realfs;
    const char *console = "/dev/tty1";
    while ((opt = getopt(argc, argv, "+r:f:d:c:n:")) != -1) {
        switch (opt) {
            case 'r':
            case 'f':
                root = optarg;
                if (opt == 'f')
                    fs = &fakefs;
                break;
            case 'd':
                workdir = optarg;
                break;
            case 'c':
                console = optarg;
                break;
            case 'n':
                if (native_offload_add(optarg) < 0)
                    fprintf(stderr, "warning: ignoring -n %s\n", optarg);
                break;
        }
    }

    openlog(argv[0], 0, LOG_USER);

    char root_realpath[MAX_PATH + 1] = "/";
    if (root != NULL && realpath(root, root_realpath) == NULL) {
        perror(root);
        exit(1);
    }
    if (fs == &fakefs)
        strcat(root_realpath, "/data");
    int err = mount_root(fs, root_realpath);
    if (err < 0)
        return err;

    become_first_process();
    current->thread = pthread_self();

    // /dev has to be a filesystem that can hold a device node, and the root
    // is not always one: realfs needs root on the host to mknod, and tmpfs has
    // no mknod at all. So when the root is an ordinary directory, /dev gets a
    // small fakefs of its own — which is what an embedder does too, and for the
    // same reason (ServerBox's make_dev).
    //
    // Without it a guest has no /dev whatsoever, and the effect is worse than
    // things being absent: a shell redirecting to /dev/null does not write to a
    // device, it creates a regular file at that path in the rootfs. dnf died on
    // std::random_device with no /dev/urandom to open, apt could not start its
    // download methods, and every `2>/dev/null` left a file behind.
    if (fs == &realfs) {
        const char *tmp = getenv("TMPDIR");
        if (tmp == NULL || tmp[0] == '\0')
            tmp = "/tmp";
        snprintf(dev_fakefs_dir, sizeof(dev_fakefs_dir), "%s/ish-dev-%d", tmp, getpid());
        // Trailing slashes in TMPDIR are ordinary on macOS and would give a
        // path with a double slash, which is harmless, and a mount source whose
        // basename fakefs checks, which is not.
        int err = fake_db_create(dev_fakefs_dir);
        if (err < 0) {
            fprintf(stderr, "warning: no /dev for the guest (%d)\n", err);
            // It fails after creating the directory as readily as before: the
            // database is the last step. Remove whatever got made.
            remove_dev_fakefs();
        } else {
            char source[PATH_MAX];
            snprintf(source, sizeof(source), "%s/data", dev_fakefs_dir);
            generic_mkdirat(AT_PWD, "/dev", 0755);
            err = do_mount(&fakefs, source, "/dev", "", 0);
            if (err < 0) {
                fprintf(stderr, "warning: /dev did not mount (%d)\n", err);
                remove_dev_fakefs();
            }
        }
    }

    // Create essential device nodes
    if (fs != &realfs || dev_fakefs_dir[0] != '\0') {
        generic_mknodat(AT_PWD, "/dev/null", S_IFCHR|0666, dev_make(MEM_MAJOR, DEV_NULL_MINOR));
        generic_mknodat(AT_PWD, "/dev/zero", S_IFCHR|0666, dev_make(MEM_MAJOR, DEV_ZERO_MINOR));
        generic_mknodat(AT_PWD, "/dev/full", S_IFCHR|0666, dev_make(MEM_MAJOR, DEV_FULL_MINOR));
        generic_mknodat(AT_PWD, "/dev/random", S_IFCHR|0666, dev_make(MEM_MAJOR, DEV_RANDOM_MINOR));
        generic_mknodat(AT_PWD, "/dev/urandom", S_IFCHR|0666, dev_make(MEM_MAJOR, DEV_URANDOM_MINOR));
        generic_mknodat(AT_PWD, "/dev/tty", S_IFCHR|0666, dev_make(TTY_ALTERNATE_MAJOR, DEV_TTY_MINOR));
        generic_mknodat(AT_PWD, "/dev/console", S_IFCHR|0666, dev_make(TTY_ALTERNATE_MAJOR, DEV_CONSOLE_MINOR));
        generic_mknodat(AT_PWD, "/dev/ptmx", S_IFCHR|0666, dev_make(TTY_ALTERNATE_MAJOR, DEV_PTMX_MINOR));
        // /dev/stdout and its siblings are symlinks into /proc, which is
        // already mounted by the time this runs. Without them a shell
        // redirecting to /dev/stdout does not write to its own output, it
        // creates a file called stdout — the same shape of bug as the missing
        // /dev this block exists for.
        //
        // They resolve when the descriptor behind them has a path: a tty, or a
        // file the guest opened. They do not when it does not — a piped stdio
        // reads back as `unknown:[…]` — because this procfs answers /proc/PID/fd
        // with ordinary symlinks rather than Linux's re-openable magic ones.
        // The redirect then fails, which is a better answer than the file named
        // "stdout" it used to leave behind.
        generic_symlinkat("/proc/self/fd/0", AT_PWD, "/dev/stdin");
        generic_symlinkat("/proc/self/fd/1", AT_PWD, "/dev/stdout");
        generic_symlinkat("/proc/self/fd/2", AT_PWD, "/dev/stderr");
    }

    char cwd[MAX_PATH + 1];
    if (root == NULL && workdir == NULL) {
        getcwd(cwd, sizeof(cwd));
        workdir = cwd;
    }
    if (workdir != NULL) {
        struct fd *pwd = generic_open(workdir, O_RDONLY_, 0);
        if (IS_ERR(pwd)) {
            fprintf(stderr, "error opening working dir: %ld\n", PTR_ERR(pwd));
            // Past the mount, so the backing directory is this function's to
            // clean up: nothing later runs on this path.
            remove_dev_fakefs();
            return 1;
        }
        fs_chdir(current->fs, pwd);
    }

    char argv_copy[4096];
    int i = optind;
    size_t p = 0;
    size_t exec_argc = 0;
    if (argv[optind] == NULL) {
        // Every failure from here to the end of this function has to clean up
        // for itself: exit_hook, which is what removes this on a normal exit,
        // is not installed until the last line.
        remove_dev_fakefs();
        return _ENOENT;
    }
    // Inject V8 flags for node to work around scope corruption in emulation.
    // --jitless: disable JIT (avoids V8 code gen incompatible with our JIT)
    // --predictable: disable concurrent GC/compilation (avoids race conditions)
    // --no-lazy: eager compilation (avoids Zone reuse patterns that corrupt scopes)
    // --single-generation: skip young gen (reduces GC-triggered Zone resets)
    // This is needed here because the initial do_execve bypasses sys_execve.
    {
        const char *base = strrchr(argv[optind], '/');
        base = base ? base + 1 : argv[optind];
        if (strcmp(base, "node") == 0) {
            // Copy argv[0] first
            strcpy(&argv_copy[p], argv[optind]);
            p += strlen(argv[optind]) + 1;
            exec_argc++;
            // Inject V8 flags to work around scope corruption
            static const char *v8_flags[] = {
                "--jitless",
                "--no-lazy",
                "--no-expose-wasm",
                "--max-old-space-size=512",
                // Force single-threaded V8: in jitless mode worker threads
                // have no JIT to do, but V8 still creates them and they
                // block on futex/epoll waiting for the slow main thread.
                // Cuts `node -e 0` from 2.3s → 0.65s on iSH ARM64.
                // Validated 2026-05-05 via syscall profile.
                "--no-concurrent-marking",
                "--no-concurrent-recompilation",
                "--no-lazy-compile-dispatcher",
                // --predictable forces V8 into deterministic single-threaded
                // mode: disables concurrent GC, parallel scavenger, and
                // background compilation. Empirically yields the biggest
                // single speedup on iSH ARM64 (reduces wall time another
                // 30-40% on top of the no-concurrent-* flags above).
                "--predictable",
            };
            for (int fi = 0; fi < (int)(sizeof(v8_flags)/sizeof(v8_flags[0])); fi++) {
                strcpy(&argv_copy[p], v8_flags[fi]);
                p += strlen(v8_flags[fi]) + 1;
                exec_argc++;
            }
            // Copy remaining args (skip argv[optind])
            for (i = optind + 1; i < argc; i++) {
                strcpy(&argv_copy[p], argv[i]);
                p += strlen(argv[i]) + 1;
                exec_argc++;
            }
            argv_copy[p] = '\0';
            goto do_exec;
        }
    }
    while (i < argc) {
        strcpy(&argv_copy[p], argv[i]);
        p += strlen(argv[i]) + 1;
        exec_argc++;
        i++;
    }
    argv_copy[p] = '\0';
do_exec:
    // Inject environment variables for the initial exec.
    // sys_execve has its own injection, but the initial do_execve bypasses it.
    {
        static char envp_buf[4096];
        size_t ep = 0;
        if (envp != NULL) {
            const char *e = envp;
            while (*e) {
                size_t len = strlen(e) + 1;
                if (ep + len < sizeof(envp_buf) - 256) {
                    memcpy(&envp_buf[ep], e, len);
                    ep += len;
                }
                e += len;
            }
        }
        // Inject env vars that sys_execve would normally add.
        // These are injected only if not already present.
        static const char *inject_vars[] = {
            "GODEBUG=asyncpreemptoff=1",  // Disable Go async preemption
            "GOMAXPROCS=2",               // Limit Go thread count
            "NO_COLOR=1",                 // Disable color output
            "PYTHONMALLOC=malloc",        // Bypass pymalloc arenas
            "PYTHONDONTWRITEBYTECODE=1",  // Skip .pyc generation
            // Reduce libuv worker pool from 4→1: short scripts never use them
            // but pay the futex sync cost. Saves ~1s on `node -e 0`.
            "UV_THREADPOOL_SIZE=1",
        };
        for (size_t vi = 0; vi < sizeof(inject_vars)/sizeof(inject_vars[0]); vi++) {
            // Check if already present (search by prefix up to '=')
            const char *eq = strchr(inject_vars[vi], '=');
            size_t prefix_len = eq ? (size_t)(eq - inject_vars[vi] + 1) : strlen(inject_vars[vi]);
            int found = 0;
            const char *scan = envp_buf;
            const char *scan_end = envp_buf + ep;
            while (scan < scan_end && *scan) {
                if (strncmp(scan, inject_vars[vi], prefix_len) == 0) {
                    found = 1;
                    break;
                }
                scan += strlen(scan) + 1;
            }
            if (!found) {
                size_t vlen = strlen(inject_vars[vi]) + 1;
                if (ep + vlen < sizeof(envp_buf) - 64) {
                    memcpy(&envp_buf[ep], inject_vars[vi], vlen);
                    ep += vlen;
                }
            }
        }
        // Node-specific: LD_PRELOAD for zero_free.so
        const char *base2 = strrchr(argv[optind], '/');
        base2 = base2 ? base2 + 1 : argv[optind];
        if (strcmp(base2, "node") == 0) {
            static const char *ld_preload = "LD_PRELOAD=/lib/zero_free.so";
            size_t plen = strlen(ld_preload) + 1;
            if (ep + plen < sizeof(envp_buf) - 2) {
                memcpy(&envp_buf[ep], ld_preload, plen);
                ep += plen;
            }
        }
        envp_buf[ep] = '\0';
        err = do_execve(argv[optind], exec_argc, argv_copy, envp_buf);
    }
    if (err < 0) {
        remove_dev_fakefs();
        return err;
    }
    tty_drivers[TTY_CONSOLE_MAJOR] = &real_tty_driver;
    if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) {
        err = create_stdio(console, TTY_CONSOLE_MAJOR, 1);
        if (err < 0) {
            remove_dev_fakefs();
            return err;
        }
    } else {
        err = create_piped_stdio();
        if (err < 0) {
            remove_dev_fakefs();
            return err;
        }
    }
    exit_hook = exit_handler;
    return 0;
}
