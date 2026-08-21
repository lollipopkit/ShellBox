#!/usr/bin/env bash
#
# The gate: does a guest boot, run things, and answer correctly.
#
# Usage:  tests/integration/smoke.sh [-i <ish binary>] [-r <rootfs>]
#
# ## Offline on purpose
#
# Every case here runs against what the base rootfs already ships. Nothing
# installs a package, and nothing reaches the network — a gate that depends on
# dl-cdn.alpinelinux.org turns red when a mirror hiccups, and a red build that
# means nothing is worse than no build at all. Installing toolchains and
# compiling with them is the full tier's job (tests/integration/full.sh), where
# a network failure is a person reading a log rather than a blocked pull
# request.
#
# The rootfs is Alpine because that is what ServerBox ships. The other two are
# in the full tier.

set -uo pipefail
cd "$(dirname "$0")/../.."

ISH=""
ROOTFS=""
while getopts "i:r:h" opt; do
    case $opt in
        i) ISH="$OPTARG" ;;
        r) ROOTFS="$OPTARG" ;;
        h) sed -n '2,20p' "$0"; exit 0 ;;
        *) exit 2 ;;
    esac
done

if [ -z "$ISH" ]; then
    for cand in build-arm64-release/ish build/ish; do
        [ -x "$cand" ] && { ISH="$cand"; break; }
    done
fi
[ -n "$ISH" ] && [ -x "$ISH" ] || { echo "no iSH binary; build one or pass -i" >&2; exit 2; }
[ -n "$ROOTFS" ] || ROOTFS="$(tests/integration/rootfs.sh alpine)" || exit 1

# A rootfs is a directory on the host and it keeps whatever a run leaves in it.
# Fixed paths meant this suite collided with itself: full.sh's go case makes
# /tmp/g a directory, and the `unlink` case below then could not create /tmp/g
# as a file — a failure that had nothing to do with the code under test, and one
# that only appeared once both had been run. Everything writes under here, and
# it is removed at the end.
#
# The cases below name it as $SCRATCH inside single quotes, so it reaches the
# guest's shell unexpanded and is expanded there against the assignment the
# wrapper prepends. Expanding it here instead would work too; leaving it to the
# guest keeps the cases readable as the shell commands they are.
SCRATCH="/tmp/ish-smoke-$$"
cleanup() { "$ISH" -r "$ROOTFS" /bin/sh -c "rm -rf $SCRATCH" >/dev/null 2>&1 || true; }
trap cleanup EXIT

pass=0; fail=0

# Runs a shell command in the guest and compares stdout and the exit status.
# Both, because a case that asserts only on output passes when the guest prints
# the right thing on its way to dying.
check() {
    local name="$1" want_out="$2" want_status="$3" cmd="$4"
    local out status
    out="$("$ISH" -r "$ROOTFS" /bin/sh -c "SCRATCH=$SCRATCH; mkdir -p \$SCRATCH; $cmd" 2>/dev/null)"
    status=$?
    if [ "$out" = "$want_out" ] && [ "$status" = "$want_status" ]; then
        pass=$((pass+1)); printf '  PASS  %s\n' "$name"
    else
        fail=$((fail+1))
        printf '  FAIL  %s\n' "$name"
        printf '        output: %q\n        want:   %q\n' "$out" "$want_out"
        printf '        status: %s  want: %s\n' "$status" "$want_status"
    fi
}

echo "########## smoke ($ISH -r $ROOTFS) ##########"

# The guest runs at all, and the interpreter reports the architecture it is
# emulating rather than the one it is running on. Both are aarch64 here, which
# is the point of the fork, so this asserts the guest's own uname rather than
# a difference.
check "boot and print"        "hello"   0 'echo hello'
check "uname is aarch64"      "aarch64" 0 'uname -m'

# An exit status has to survive do_execve, task_start, the wait and the host
# process's own exit. 42 rather than 1: a status that arrives as "nonzero"
# would pass against 1 by accident.
check "exit status"           ""        42 'exit 42'
check "exit status from a child" ""     7  'sh -c "exit 7"'

# A shell pipeline is fork, exec, two pipes and a wait for each side. This is
# also the shape apt's method workers use, which is known not to work on the
# Ubuntu rootfs -- see the note in full.sh.
check "pipe between processes" "world" 0 'echo world | cat'
check "pipeline of three"      "3"     0 'printf "a\nb\nc\n" | wc -l | tr -d " "'

# Files, through realfs and the host filesystem underneath it.
check "write then read a file" "content" 0 'echo content > $SCRATCH/f && cat $SCRATCH/f'
check "unlink"                 "gone"    0 'echo x > $SCRATCH/g && rm $SCRATCH/g && { [ -e $SCRATCH/g ] || echo gone; }'
check "mkdir and list"         "in-dir"  0 'mkdir -p $SCRATCH/d && echo x > $SCRATCH/d/in-dir && ls $SCRATCH/d'

# The two flag values a review found wrong, from the guest side this time:
# O_DIRECTORY on a file, and a symlink opened without following it. tests/unit
# asserts these against generic_openat directly; this asserts that a real guest
# program sees the same answers.
check "O_DIRECTORY on a file"  "ok" 0 \
    'echo x > $SCRATCH/f2; cd $SCRATCH/f2 2>/dev/null && echo bad || echo ok'
check "symlink resolves"       "target" 0 \
    'echo target > $SCRATCH/t; ln -sf $SCRATCH/t $SCRATCH/l; cat $SCRATCH/l'

# Signals: a killed child is reported as killed, and the shell renders it as
# 128+signal. This is the wait(2) status encoding that waitid_decode_status
# reads, seen from the other end.
check "child killed by a signal" "" 143 'sh -c "kill -TERM \$\$"'

# Environment and argv survive execve.
check "argv"                   "a b c" 0 'sh -c "echo \$1 \$2 \$3" _ a b c'
check "environment"            "value" 0 'X=value sh -c "echo \$X"'

echo
if [ "$fail" = "0" ]; then
    echo "smoke: $pass passed"
    exit 0
fi
echo "smoke: $fail FAILED, $pass passed"
exit 1
