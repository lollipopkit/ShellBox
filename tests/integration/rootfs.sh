#!/usr/bin/env bash
#
# Puts a guest filesystem on disk and prints the path to it.
#
# Usage:  tests/integration/rootfs.sh <alpine|rocky|ubuntu>
#
# Each tree is unpacked once into build/integration/<name>/ and reused. `build/`
# is ignored by git, so this leaves no trace in the checkout.
#
# ## Why these three
#
# They are the three userland stacks a guest can be, and each exercises a
# different half of the kernel:
#
#   alpine   musl,  apk    — what ServerBox actually ships on iOS
#   rocky    glibc, dnf    — glibc's syscall use differs from musl's throughout,
#                            and dnf is Python, so the package manager is itself
#                            a large workload
#   ubuntu   glibc, apt    — dpkg's maintainer scripts fork and exec far more
#                            than the other two
#
# Ubuntu stands in for Debian, which has no plain rootfs tarball to download:
# debuerreotype publishes only metadata and an OCI layout on its artifact
# branches, and the Docker Hub route needs a registry token. Ubuntu base is the
# same apt/dpkg/glibc stack behind a versioned URL, which is what this needed.
#
# ## Digests
#
# Pinned, and checked before anything is unpacked. This downloads a root
# filesystem and then runs its binaries, so "it came from the right host over
# TLS" is not the standard being applied — the same standard ServerBox holds its
# own rootfs to.
#
# To move one: change the URL and the digest together, and say in the commit
# message which upstream release it is.

set -euo pipefail

cd "$(dirname "$0")/../.."
CACHE="build/integration"

case "${1:-}" in
  alpine)
    URL="https://dl-cdn.alpinelinux.org/alpine/v3.22/releases/aarch64/alpine-minirootfs-3.22.5-aarch64.tar.gz"
    SHA=3fbc6285032ed46821b511292633d7b2a6306a2e254f590e92bdafff56cf2f70
    LAYOUT=plain
    ;;
  rocky)
    # `.latest.` in the name, because Rocky publishes the versioned file with an
    # `.oci.` infix and a build date this would have to chase. The digest below
    # is what that URL served on 2026-08-21; when Rocky publishes a new build it
    # stops matching, and the failure says so rather than running an unknown
    # tree.
    URL="https://dl.rockylinux.org/pub/rocky/9/images/aarch64/Rocky-9-Container-Base.latest.aarch64.tar.xz"
    SHA=254dc06377bb63a5ab390cea33c2f26d71c4e0ff6ae1bc8a0fb0fbb86d992e89
    LAYOUT=oci
    ;;
  ubuntu)
    URL="https://cdimage.ubuntu.com/ubuntu-base/releases/24.04/release/ubuntu-base-24.04.3-base-arm64.tar.gz"
    SHA=7b2dced6dd56ad5e4a813fa25c8de307b655fdabc6ea9213175a92c48dabb048
    LAYOUT=plain
    ;;
  *)
    echo "usage: $0 <alpine|rocky|ubuntu>" >&2
    exit 2
    ;;
esac

NAME="$1"
TREE="$CACHE/$NAME"
TARBALL="$CACHE/$(basename "$URL")"

# The marker, not the directory: an interrupted unpack leaves a directory that
# looks finished. Written last, and named for the digest so that changing the
# pin above re-unpacks rather than reusing the previous tree.
STAMP="$TREE/.rootfs-sha"

if [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$SHA" ]; then
  echo "$PWD/$TREE"
  exit 0
fi

mkdir -p "$CACHE"

if [ ! -f "$TARBALL" ] || [ "$(shasum -a 256 "$TARBALL" | cut -d' ' -f1)" != "$SHA" ]; then
  echo "fetching $NAME" >&2
  curl -fsSL --retry 3 -o "$TARBALL.part" "$URL"
  got="$(shasum -a 256 "$TARBALL.part" | cut -d' ' -f1)"
  if [ "$got" != "$SHA" ]; then
    rm -f "$TARBALL.part"
    echo "error: $NAME digest mismatch" >&2
    echo "       expected $SHA" >&2
    echo "       got      $got" >&2
    echo "       If upstream published a new build, update the pin in $0." >&2
    exit 1
  fi
  mv "$TARBALL.part" "$TARBALL"
fi

# chmod first: a rootfs ships directories at 555, and rm cannot unlink from a
# directory it cannot write. Without this the re-unpack after a digest change
# fails halfway and leaves a tree that is neither the old one nor the new one.
[ -d "$TREE" ] && chmod -R u+w "$TREE" 2>/dev/null
rm -rf "$TREE"
mkdir -p "$TREE"

case "$LAYOUT" in
  plain)
    # Device nodes are skipped without root, which is what is wanted: the guest
    # builds its own /dev.
    tar xf "$TARBALL" -C "$TREE" 2>/dev/null || true
    ;;
  oci)
    # An OCI image layout rather than a filesystem: index.json names a manifest,
    # the manifest names layers, and the layers are the tree. Applied in order,
    # because a multi-layer image is only correct that way even when today's
    # base image has one.
    echo "unpacking the OCI layout" >&2
    OCI="$CACHE/$NAME.oci"
    rm -rf "$OCI"; mkdir -p "$OCI"
    tar xf "$TARBALL" -C "$OCI"
    python3 - "$OCI" "$TREE" <<'PY'
import json, os, subprocess, sys
oci, tree = sys.argv[1], sys.argv[2]

def blob(digest):
    return os.path.join(oci, "blobs", *digest.split(":"))

index = json.load(open(os.path.join(oci, "index.json")))
manifest = json.load(open(blob(index["manifests"][0]["digest"])))
layers = manifest["layers"]
print("%d layer(s)" % len(layers), file=sys.stderr)
for layer in layers:
    # tar rather than tarfile: whiteouts and hardlinks in a real image are the
    # kind of thing a hand-rolled extractor gets subtly wrong, and GNU/bsdtar
    # already knows. Device nodes fail without root and are skipped.
    subprocess.run(["tar", "xf", blob(layer["digest"]), "-C", tree],
                   stderr=subprocess.DEVNULL, check=False)
PY
    rm -rf "$OCI"
    ;;
esac

if [ ! -x "$TREE/bin/sh" ] && [ ! -L "$TREE/bin/sh" ]; then
  echo "error: $NAME unpacked without a /bin/sh" >&2
  exit 1
fi

# The guest resolves names with this, and every tree ships either nothing or a
# placeholder. Copied from the host, which is the only name server this machine
# is known to be able to reach.
if [ -f /etc/resolv.conf ]; then
  mkdir -p "$TREE/etc"
  grep -E '^nameserver' /etc/resolv.conf > "$TREE/etc/resolv.conf" || true
fi

printf '%s\n' "$SHA" > "$STAMP"
echo "$PWD/$TREE"
