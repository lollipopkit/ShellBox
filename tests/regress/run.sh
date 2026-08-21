#!/bin/bash
# Build and run the fs/wait syscall regression tests inside iSH.
#
# These pin the behaviour of five fixes so a regression fails here rather than
# in a user's shell:
#   #30  O_DIRECTORY flag value + O_NOFOLLOW honored at path resolution
#   #31  fakefs stat/fstat st_dev (nlink, blksize) agreement
#   #29  waitpid not reporting its internal 1s timeout as EINTR
#        waitid si_status/si_code decoding
#        waitid si_uid
#
# Usage:
#   tests/regress/run.sh [-i <ish binary>] [-r <rootfs>] [-f <fakefs path>]
#
#   -i  iSH binary to test. Default: the first of build-arm64-release/ish,
#       build/ish that exists.
#   -r  rootfs to run against, passed to iSH as -r. Default alpine-arm64-321.
#   -m  how to mount the rootfs: -r (default) or -f. Takes the flag itself as
#       its argument, so it is written `-m -f`.
#   -f  a file under a fakefs mount point (e.g. /var/minis/shared/doc.txt) to
#       additionally check stat/fstat agreement on. Only meaningful when the
#       rootfs is mounted with -f and a path-translate hook is installed, so it
#       is optional; without it the #31 assertions still run against the rootfs.
#
# The guest test binary is built with a cross-compiler (aarch64-linux-musl-gcc
# by default, override with $CC_GUEST). If none is available the script skips
# rather than failing, since the toolchain is not part of the repo.

set -u
cd "$(dirname "$0")/../.."

ISH=""
ROOTFS="alpine-arm64-321"
FAKEFS_PATH=""
MOUNT_FLAG="-r"

while getopts "i:r:f:m:h" opt; do
    case $opt in
        i) ISH="$OPTARG" ;;
        r) ROOTFS="$OPTARG" ;;
        f) FAKEFS_PATH="$OPTARG" ;;
        m) MOUNT_FLAG="$OPTARG" ;;
        h) sed -n '2,26p' "$0"; exit 0 ;;
        *) exit 2 ;;
    esac
done

if [ -z "$ISH" ]; then
    for cand in build-arm64-release/ish build/ish; do
        if [ -x "$cand" ]; then ISH="$cand"; break; fi
    done
fi
if [ -z "$ISH" ] || [ ! -x "$ISH" ]; then
    echo "no iSH binary found; build one or pass -i <path>" >&2
    exit 2
fi
if [ ! -d "$ROOTFS" ]; then
    echo "rootfs $ROOTFS not found; pass -r <path>" >&2
    exit 2
fi

CC_GUEST="${CC_GUEST:-aarch64-linux-musl-gcc}"
have_cc=1
if ! command -v "$CC_GUEST" >/dev/null 2>&1; then
    # Only the compiled cases need it. The cp cases run /bin/sh out of the
    # rootfs, so exiting here skipped assertions that had everything they
    # needed -- and did it with status 0, which reads as "passed".
    have_cc=0
    echo "SKIP: no guest cross-compiler ($CC_GUEST); running the cp cases only." >&2
    echo "      Install one or set CC_GUEST to build the compiled cases." >&2
fi

SRC=tests/regress/regress_syscall.c
ASIMD_SRC="tests/regress/regress_asimd_fcvt.c tests/regress/regress_asimd_fcvt.S"
# With -f the guest's / lives under <rootfs>/data; with -r it is <rootfs> itself.
GUEST_ROOT="$ROOTFS"
if [ "$MOUNT_FLAG" = "-f" ]; then
    GUEST_ROOT="$ROOTFS/data"
fi
# Everything this run stages goes in a directory of its own, named for the pid.
# Fixed names under the rootfs's /tmp were overwritten on the way in and deleted
# on the way out, so a rootfs that happened to hold a file called
# regress_syscall lost it to a test run.
GUEST_ART="/tmp/ish-regress-$$"
HOST_ART="$GUEST_ROOT$GUEST_ART"
mkdir -p "$HOST_ART"
trap 'rm -rf "$HOST_ART"' EXIT

BIN="$HOST_ART/regress_syscall"
ASIMD_BIN="$HOST_ART/regress_asimd_fcvt"
if [ "$have_cc" = "1" ]; then
    echo "building $SRC with $CC_GUEST"
    if ! "$CC_GUEST" -static -O0 -Wall -o "$BIN" "$SRC"; then
        echo "failed to build $SRC" >&2
        exit 1
    fi
    # The AdvSIMD conversions, which nothing else in the tree exercises: the
    # gadgets they cover are only reachable by executing guest code.
    echo "building $ASIMD_SRC with $CC_GUEST"
    if ! "$CC_GUEST" -static -O0 -Wall -o "$ASIMD_BIN" $ASIMD_SRC; then
        echo "failed to build $ASIMD_SRC" >&2
        exit 1
    fi
fi
cp tests/regress/regress_cp.sh "$HOST_ART/regress_cp.sh"

# A fakefs keeps its own metadata in meta.db, so files dropped straight into
# data/ are invisible to the guest. Register them by copying through the guest
# itself; the shell script is small enough for this to be cheap, but the test
# binary is not, so -f mode runs only the cp cases and expects the syscall
# binary to be run separately under -r (where every one of these assertions is
# equally valid -- none of them depend on the mount type).
if [ "$MOUNT_FLAG" = "-f" ]; then
    rm -f "$BIN" "$ASIMD_BIN"
    b64_sh=$(base64 < tests/regress/regress_cp.sh | tr -d '\n')
    rm -f "$HOST_ART/regress_cp.sh"
    if ! "$ISH" "$MOUNT_FLAG" "$ROOTFS" /bin/sh -c \
        "mkdir -p $GUEST_ART && printf %s '$b64_sh' | base64 -d > $GUEST_ART/regress_cp.sh"; then
        echo "failed to stage regress_cp.sh into the fakefs" >&2
        exit 1
    fi
fi

status=0

if [ "$MOUNT_FLAG" = "-f" ] || [ "$have_cc" = "0" ]; then
    echo
    echo "########## syscall + AdvSIMD regressions ##########"
    if [ "$have_cc" = "0" ]; then
        echo "  SKIP  no guest cross-compiler."
    else
        echo "  SKIP  staging a static binary into a fakefs is impractical;"
        echo "        run these with -r <rootfs> (assertions are mount-independent)."
    fi
else
    echo
    echo "########## syscall regressions ($ISH $MOUNT_FLAG $ROOTFS) ##########"
    "$ISH" "$MOUNT_FLAG" "$ROOTFS" "$GUEST_ART/regress_syscall" ${FAKEFS_PATH:+"$FAKEFS_PATH"} || status=1

    echo
    echo "########## AdvSIMD FCVT regressions ##########"
    "$ISH" "$MOUNT_FLAG" "$ROOTFS" "$GUEST_ART/regress_asimd_fcvt" || status=1
fi

echo
echo "########## cp regressions ##########"
"$ISH" "$MOUNT_FLAG" "$ROOTFS" /bin/sh "$GUEST_ART/regress_cp.sh" ${FAKEFS_PATH:+"$FAKEFS_PATH"} || status=1

echo
if [ "$status" != "0" ]; then
    echo "regress: FAILURES (see above)"
    exit "$status"
fi
# Not "all cases passed" when most of them did not run. Every wait, signal and
# waitid assertion lives in the compiled binary, so a run without it establishes
# the cp cases and nothing else — and saying otherwise is how a suite comes to
# be trusted for something it never checked.
if [ "$have_cc" = "0" ] || [ "$MOUNT_FLAG" = "-f" ]; then
    echo "regress: the cp cases passed; the compiled cases did not run"
    exit 0
fi
echo "regress: all cases passed"
exit 0
