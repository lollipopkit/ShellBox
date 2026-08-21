#!/usr/bin/env bash
#
# Turns the instruction word from an "illegal instruction" report into a
# mnemonic.
#
# When a guest hits an encoding the decoder has no case for, kernel/calls.c
# prints
#
#     <pid> illegal instruction at 0x<pc>: insn=0x<word>
#
# and delivers SIGILL. That word is the whole of the evidence, and until now
# turning it into something a person can act on meant finding a disassembler and
# getting the byte order right. AArch64 is little-endian, so the word has to be
# reversed before a disassembler will take it — which is the step this exists to
# stop people getting wrong.
#
# Usage:
#   tools/decode-insn.sh 0x0e216800
#   tools/decode-insn.sh 0e216800 4ea01800     # several at once, 0x optional
#   ish -r rootfs prog 2>&1 | tools/decode-insn.sh   # or straight from a log
#
# Reads stdin when given no arguments, and picks the words out of whatever it
# is handed, so a whole crash log can be piped in.

set -uo pipefail

MC="${LLVM_MC:-}"
if [ -z "$MC" ]; then
    for cand in llvm-mc /opt/homebrew/opt/llvm/bin/llvm-mc /usr/local/opt/llvm/bin/llvm-mc; do
        command -v "$cand" >/dev/null 2>&1 && { MC="$cand"; break; }
    done
fi
if [ -z "$MC" ]; then
    echo "no llvm-mc found; brew install llvm, or set \$LLVM_MC" >&2
    exit 2
fi

decode() {
    local word="${1#0x}"
    # 32 bits, zero-padded: a report may print without leading zeros.
    word="$(printf '%08s' "$word" | tr ' ' 0)"
    local b0="${word:6:2}" b1="${word:4:2}" b2="${word:2:2}" b3="${word:0:2}"
    local out
    out="$("$MC" --disassemble -triple=aarch64 <<< "0x$b0 0x$b1 0x$b2 0x$b3" 2>&1 |
           grep -v '^\s*\.text' | sed -n 's/^\s*//p' | head -1)"
    if [ -z "$out" ] || printf '%s' "$out" | grep -qi 'invalid\|error'; then
        printf '0x%s   <not a valid AArch64 encoding>\n' "$word"
    else
        printf '0x%s   %s\n' "$word" "$out"
    fi
}

if [ $# -gt 0 ]; then
    for word in "$@"; do decode "$word"; done
    exit 0
fi

# From a log: every 0x-prefixed 8-digit word, and every `insn=` value.
grep -oE '(insn=)?0x[0-9a-fA-F]{8}' | sed 's/^insn=//' | sort -u | while read -r word; do
    decode "$word"
done
