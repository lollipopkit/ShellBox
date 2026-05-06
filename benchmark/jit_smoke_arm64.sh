#!/bin/bash
# ARM64 JIT semantic smoke tests for narrow peephole optimizations.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ASSETS_DIR="$SCRIPT_DIR/assets"
ISH_ARM64="$PROJECT_DIR/build-arm64-release/ish"
FAKEFS_ARM64="$PROJECT_DIR/alpine-arm64-fakefs"

if [ ! -x "$ISH_ARM64" ]; then
    echo "error: ARM64 binary not found: $ISH_ARM64" >&2
    exit 1
fi
if [ ! -d "$FAKEFS_ARM64" ]; then
    echo "error: ARM64 fakefs not found: $FAKEFS_ARM64" >&2
    exit 1
fi

cat "$ASSETS_DIR/arm64_bl_lr_test.c" |
  "$ISH_ARM64" -f "$FAKEFS_ARM64" /bin/sh -c "cat > /tmp/arm64_bl_lr_test.c"
cat "$ASSETS_DIR/arm64_leaf_add.S" |
  "$ISH_ARM64" -f "$FAKEFS_ARM64" /bin/sh -c "cat > /tmp/arm64_leaf_add.S"

"$ISH_ARM64" -f "$FAKEFS_ARM64" /bin/sh -c '
set -e
cc -O2 -fno-pie -no-pie \
  -o /tmp/arm64_bl_lr_test \
  /tmp/arm64_bl_lr_test.c \
  /tmp/arm64_leaf_add.S
/tmp/arm64_bl_lr_test
'
