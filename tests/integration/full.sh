#!/usr/bin/env bash
#
# The matrix: install real software in a real userland and use it.
#
# Usage:  tests/integration/full.sh [-i <ish>] [-d alpine,rocky,ubuntu] [-s node,go,rust,cc]
#
# Everything here needs the network, which is why it is not the pull-request
# gate — see tests/integration/smoke.sh. Run it by hand or from the nightly
# workflow, and read a failure as "this suite has an opinion", not "the branch
# is broken", until the log says which.
#
# ## What is under test
#
# Not the package manager's authors' work. The package manager *is* the test:
# apk, dnf and apt each fork and exec their way through hundreds of processes,
# hit the filesystem constantly, and use syscalls no benchmark thinks to. That
# a toolchain then compiles and runs a program is the second half.
#
# ## Known failures, so a run reads as a result rather than a mess
#
# The /dev gap that stopped both glibc distributions outright is fixed: `ish -r`
# mounts a small fakefs at /dev and creates the nodes in it. What is left:
#
#   rocky / dnf    Resolves, downloads and computes the transaction, then rpm
#                  fails to unpack it: "failed to open dir usr of /usr/lib/:
#                  cpio: open failed - Bad file descriptor". rpm holds directory
#                  descriptors and openat()s through them, and one of those
#                  comes back EBADF.
#
#   ubuntu / apt   `apt-get update` reaches the mirror and fails on the fetch
#                  itself. It no longer fails with "Method ... did not start
#                  correctly", which is what it said before /dev existed.
#
# Fixed along the way, and listed here because the failures they caused looked
# like distribution problems rather than emulator ones:
#
#   setsockopt(IPPROTO_IP, IP_RECVERR) answered EINVAL, and glibc treats that as
#   fatal in its resolver — socket, setsockopt, close, and not one DNS query on
#   the wire. Every glibc distribution could not resolve a name.
#
#   syncfs was a stub returning ENOSYS, which ended a dnf transaction that had
#   already downloaded everything.
#
# Nothing is skipped on account of either remaining failure. A known failure
# that stops being run is a known failure that never gets fixed.
#
# ## The cache is not pristine
#
# Packages installed by a run stay in the tree, so running this twice locally
# does not test installation the second time — the compile and the run still
# are. CI gets a fresh runner, which is where installation is really under
# test. To reset by hand: rm -rf build/integration/<name> (chmod -R u+w first;
# a rootfs ships 555 directories).

set -uo pipefail
cd "$(dirname "$0")/../.."

ISH=""
DISTROS="alpine,rocky,ubuntu"
SUITES="cc,node,go,rust"
while getopts "i:d:s:h" opt; do
    case $opt in
        i) ISH="$OPTARG" ;;
        d) DISTROS="$OPTARG" ;;
        s) SUITES="$OPTARG" ;;
        h) sed -n '2,30p' "$0"; exit 0 ;;
        *) exit 2 ;;
    esac
done

if [ -z "$ISH" ]; then
    for cand in build-arm64-release/ish build/ish; do
        [ -x "$cand" ] && { ISH="$cand"; break; }
    done
fi
[ -n "$ISH" ] && [ -x "$ISH" ] || { echo "no iSH binary; build one or pass -i" >&2; exit 2; }

# Per-run, for the reason smoke.sh gives at greater length: the rootfs keeps
# what a run leaves in it, and two cases writing /tmp/g collided.
SCRATCH="/tmp/ish-full-$$"

pass=0; fail=0; failed_names=""

# Runs a script in the guest and requires a marker in its output. A marker
# rather than the whole of stdout, because a package manager prints progress
# nobody should have to predict; the marker is printed by the thing under test
# after it has worked.
run_case() {
    local distro="$1" name="$2" marker="$3" script="$4"
    local rootfs out started elapsed
    rootfs="$(tests/integration/rootfs.sh "$distro")" || { fail=$((fail+1)); return; }
    started=$SECONDS
    out="$("$ISH" -r "$rootfs" /bin/sh -c "mkdir -p $SCRATCH; $script; rm -rf $SCRATCH" 2>&1)"
    elapsed=$((SECONDS - started))
    if printf '%s' "$out" | grep -q "$marker"; then
        pass=$((pass+1)); printf '  PASS  %-8s %-12s %3ds\n' "$distro" "$name" "$elapsed"
    else
        fail=$((fail+1)); failed_names="$failed_names $distro/$name"
        printf '  FAIL  %-8s %-12s %3ds\n' "$distro" "$name" "$elapsed"
        printf '%s\n' "$out" | tail -12 | sed 's/^/        | /'
    fi
}

# How each distro installs things. The commands differ, the assertion does not.
install_cmd() {
    case "$1" in
        alpine) echo "apk add --no-progress -q" ;;
        rocky)  echo "dnf install -y -q" ;;
        ubuntu) echo "apt-get update >/dev/null 2>&1; apt-get install -y -q" ;;
    esac
}

# Package names, which are the same idea under three spellings.
pkg_for() {
    case "$1/$2" in
        alpine/cc)   echo "gcc musl-dev" ;;
        */cc)        echo "gcc glibc-devel" ;;
        alpine/node) echo "nodejs" ;;
        rocky/node)  echo "nodejs" ;;
        ubuntu/node) echo "nodejs" ;;
        */go)        echo "go" ;;
        */rust)      echo "rust cargo" ;;
    esac
}

for distro in ${DISTROS//,/ }; do
    echo
    echo "########## $distro ##########"
    install="$(install_cmd "$distro")"

    for suite in ${SUITES//,/ }; do
        pkg="$(pkg_for "$distro" "$suite")"
        # rocky spells glibc's headers differently and has no `go` in the base
        # repos; a missing name is a failure of this table, so say so rather
        # than reporting the suite as broken.
        [ -n "$pkg" ] || { echo "  SKIP  $distro $suite (no package name in this table)"; continue; }

        case "$suite" in
            cc)
                # A quoted heredoc, not printf: the source travels through
                # bash and then the guest's shell, and a %d in it is a format
                # specifier to both of them before it is ever C.
                run_case "$distro" cc "CC-OK 42" "
                    $install $pkg >/dev/null 2>&1
                    cat > $SCRATCH/h.c <<'SRC'
#include <stdio.h>
int main(void) { printf(\"CC-OK %d\\n\", 6 * 7); return 0; }
SRC
                    gcc -O2 -o $SCRATCH/h $SCRATCH/h.c && $SCRATCH/h"
                ;;
            node)
                run_case "$distro" node "NODE-OK" "
                    $install $pkg >/dev/null 2>&1
                    node -e 'console.log(\"NODE-OK\", process.version)'"
                ;;
            go)
                run_case "$distro" go "GO-OK" "
                    $install $pkg >/dev/null 2>&1
                    mkdir -p $SCRATCH/g && cd $SCRATCH/g
                    cat > main.go <<'SRC'
package main

import \"fmt\"

func main() { fmt.Println(\"GO-OK\", 6*7) }
SRC
                    go mod init x >/dev/null 2>&1
                    # No network for modules: the program imports only the
                    # standard library, so the toolchain is what is being
                    # tested, not the module proxy.
                    GOFLAGS=-mod=mod GOPROXY=off go build -o $SCRATCH/g/x . && $SCRATCH/g/x"
                ;;
            rust)
                run_case "$distro" rust "RUST-OK" "
                    $install $pkg >/dev/null 2>&1
                    cat > $SCRATCH/r.rs <<'SRC'
fn main() { println!(\"RUST-OK {}\", 6 * 7); }
SRC
                    rustc -O -o $SCRATCH/r $SCRATCH/r.rs && $SCRATCH/r"
                ;;
            *)
                echo "  SKIP  unknown suite $suite"
                ;;
        esac
    done
done

echo
if [ "$fail" = "0" ]; then
    echo "full: $pass passed"
    exit 0
fi
echo "full: $fail FAILED, $pass passed —$failed_names"
exit 1
