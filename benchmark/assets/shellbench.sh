#!/bin/sh
# Guest-side benchmark — runs INSIDE iSH, measures with /proc/uptime
# Outputs: "category|name|milliseconds" per line
# No eval, no bash-isms — pure POSIX sh

# Millisecond timer: prefer date +%s%N (nanosecond), fall back to /proc/uptime.
_date_ns=$(date +%s%N 2>/tmp/_null || echo "")
case "$_date_ns" in
    *[!0-9]*|"")
        _date_ns=
        ;;
esac
if [ -n "$_date_ns" ] && [ ${#_date_ns} -ge 18 ]; then
    _ms() { echo $(( $(date +%s%N) / 1000000 )); }
else
    _ms() { awk '{printf "%.0f", $1 * 1000}' /proc/uptime; }
fi
unset _date_ns

_t0=0
_start() { _t0=$(_ms); }
_end()   { local t1; t1=$(_ms); echo "$1|$2|$(( t1 - _t0 ))"; }

# ── System ──────────────────────────────────────────────────
_start; i=0; while [ $i -lt 1000 ]; do echo Hello World >/tmp/_null 2>&1; i=$((i+1)); done; _end System "echo x1000"
_start; i=0; while [ $i -lt 100 ]; do uname -a >/tmp/_null 2>&1; i=$((i+1)); done; _end System "uname -a x100"
_start; i=0; while [ $i -lt 100 ]; do ls /bin >/tmp/_null 2>&1; i=$((i+1)); done; _end System "ls /bin x100"
_start; i=0; while [ $i -lt 200 ]; do cat /etc/os-release >/tmp/_null 2>&1; i=$((i+1)); done; _end System "cat file x200"
_start; i=0; while [ $i -lt 200 ]; do ls /bin | wc -l >/tmp/_null 2>&1; i=$((i+1)); done; _end System "wc -l x200"
_start; i=0; while [ $i -lt 200 ]; do date >/tmp/_null 2>&1; i=$((i+1)); done; _end System "date x200"
_start; i=0; while [ $i -lt 100 ]; do env >/tmp/_null 2>&1; i=$((i+1)); done; _end System "env x100"

# ── Compute ─────────────────────────────────────────────────
_start; i=0; while [ $i -lt 1000 ]; do i=$((i+1)); done;    _end Compute "loop 1000"
_start; i=0; while [ $i -lt 5000 ]; do i=$((i+1)); done;    _end Compute "loop 5000"
_start; i=0; while [ $i -lt 10000 ]; do i=$((i+1)); done;   _end Compute "loop 10000"
_start; seq 1 10000 | awk '{s+=$1} END{print s}' >/tmp/_null 2>&1;  _end Compute "seq+awk 10K"
_start; seq 1 50000 | awk '{s+=$1} END{print s}' >/tmp/_null 2>&1;  _end Compute "seq+awk 50K"
_start; seq 1 100000 | awk '{s+=$1} END{print s}' >/tmp/_null 2>&1; _end Compute "seq+awk 100K"
_start; i=0; while [ $i -lt 500 ]; do i=$(expr $i + 1); done;     _end Compute "expr loop 500"
_start; i=0; while [ $i -lt 50 ]; do echo 'scale=100; sqrt(2)' | bc -l >/tmp/_null 2>&1; i=$((i+1)); done; _end Compute "bc sqrt x50"
_start; i=0; while [ $i -lt 50 ]; do echo 'scale=50; 4*a(1)' | bc -l >/tmp/_null 2>&1; i=$((i+1)); done; _end Compute "bc pi x50"

# ── Text ────────────────────────────────────────────────────
_start; i=0; while [ $i -lt 200 ]; do echo hello | sed 's/hello/world/g' >/tmp/_null 2>&1; i=$((i+1)); done; _end Text "sed replace x200"
_start; seq 1 1000 | sort -nr | tail -1 >/tmp/_null 2>&1;    _end Text "sort 1K"
_start; seq 1 5000 | sort -nr | tail -1 >/tmp/_null 2>&1;    _end Text "sort 5K"
_start; seq 1 5000 | sort | uniq -c | wc -l >/tmp/_null 2>&1; _end Text "uniq count 5K"
_start; seq 1 10000 | grep -c 5 >/tmp/_null 2>&1;            _end Text "grep count"
_start; seq 1 10000 | tr '0-9' 'a-j' | wc -l >/tmp/_null 2>&1; _end Text "tr lowercase 10K"

# ── File I/O ────────────────────────────────────────────────
_start; for i in $(seq 1 100); do echo x > /tmp/_b$i; done; rm -f /tmp/_b*; _end File-IO "create 100"
_start; for i in $(seq 1 500); do echo x > /tmp/_b$i; done; rm -f /tmp/_b*; _end File-IO "create 500"
_start; i=0; while [ $i -lt 20 ]; do find /bin -type f 2>/tmp/_null | wc -l >/tmp/_null; i=$((i+1)); done; _end File-IO "find /bin x20"
_start; i=0; while [ $i -lt 50 ]; do dd if=/dev/zero of=/tmp/_dd bs=1048576 count=4 2>/tmp/_null; rm -f /tmp/_dd; i=$((i+1)); done; _end File-IO "dd 4MB x50"

# ── Crypto ──────────────────────────────────────────────────
_start; i=0; while [ $i -lt 100 ]; do echo test | md5sum >/tmp/_null 2>&1; i=$((i+1)); done; _end Crypto "md5sum x100"
_start; i=0; while [ $i -lt 100 ]; do echo test | sha256sum >/tmp/_null 2>&1; i=$((i+1)); done; _end Crypto "sha256sum x100"

# ── Process ─────────────────────────────────────────────────
_start; for i in $(seq 1 100); do /bin/true; done;           _end Process "fork+exec 100"
_start; for i in $(seq 1 300); do /bin/true; done;           _end Process "fork+exec 300"
_start; i=0; while [ $i -lt 100 ]; do seq 1 1000 | grep 5 | sort -n | wc -l >/tmp/_null 2>&1; i=$((i+1)); done; _end Process "pipe chain x100"

# ── Python (if available) ──────────────────────────────────
if command -v python3 >/tmp/_null 2>&1; then
    _start; python3 -c 'pass';                               _end Python "startup"
    _start; python3 -c 'print(sum(range(1000000)))' >/tmp/_null; _end Python "sum(1M)"
    _start
    python3 -c '
def f(n):
 if n<2: return n
 return f(n-1)+f(n-2)
print(f(30))' >/tmp/_null
    _end Python "fib(30)"

    _start
    python3 -c '
s=""
for i in range(10000): s+=str(i)
print(len(s))' >/tmp/_null
    _end Python "str concat 10K"

    _start
    python3 -c '
import json
d=[{"id":i,"v":i*3.14} for i in range(5000)]
for _ in range(5): d=json.loads(json.dumps(d))
print(len(d))' >/tmp/_null
    _end Python "json roundtrip"

    _start; python3 -c 'import hashlib; print(hashlib.sha256(b"x"*1000000).hexdigest()[:8])' >/tmp/_null; _end Python "sha256 1MB"

    _start
    python3 -c '
import re
t="abc123def456"*5000
print(len(re.findall(r"\d+",t)))' >/tmp/_null
    _end Python "regex 50K"

    _start
    python3 -c '
import random; random.seed(42)
a=[random.randint(0,999999) for _ in range(100000)]
a.sort()
print(a[0])' >/tmp/_null
    _end Python "sort 100K"
fi

# ── C benchmark (prefer pre-compiled binary, fallback to gcc) ──
# Run BEFORE Node.js/Go so that platforms with incomplete runtime support
# (e.g. x86 iSH missing io_uring syscall used by Node 22) still produce C data.
# Prebuilt binaries pushed to /tmp/cbench_prebuilt by the host runner.
# On native macOS, the script directory is probed for cbench_lite_macos.
_cbench=""
_script_dir=$(dirname "$0" 2>/dev/null)
if [ -x /tmp/cbench_prebuilt ]; then
    _cbench=/tmp/cbench_prebuilt
elif [ -x "$_script_dir/cbench_lite_macos" ]; then
    _cbench="$_script_dir/cbench_lite_macos"
elif command -v gcc >/tmp/_null 2>&1 && [ -f /tmp/cbench_lite.c ]; then
    _start; gcc -O2 -o /tmp/cbench_compiled /tmp/cbench_lite.c -lm 2>/tmp/_null; _end C "gcc compile"
    [ -x /tmp/cbench_compiled ] && _cbench=/tmp/cbench_compiled
fi
if [ -n "$_cbench" ]; then
    $_cbench 2>/tmp/_null | while IFS= read -r line; do
        name=$(echo "$line" | awk '{print $1}')
        ms=$(echo "$line" | awk '{print $2}')
        [ -n "$name" ] && [ -n "$ms" ] && echo "C|$name|$ms"
    done
fi

# ── Go (if available) ──────────────────────────────────────
if command -v go >/tmp/_null 2>&1; then
    _start; timeout 30 go version >/tmp/_null 2>&1;           _end Go "version"
    _start; timeout 30 go env GOROOT >/tmp/_null 2>&1;        _end Go "env"
fi

# ── Node.js (if available) ─────────────────────────────────
# Each test wrapped in timeout so x86 iSH (missing io_uring syscall 425)
# can't hang the whole bench script.
if command -v node >/tmp/_null 2>&1; then
    _start; timeout 30 node -e 'process.exit(0)';            _end Node.js "startup"
    _start; timeout 30 node -e 'let s=0;for(let i=0;i<1e6;i++)s+=i;console.log(s)' >/tmp/_null 2>/tmp/_null; _end Node.js "sum 1M"
    _start
    timeout 30 node -e '
const d=Array.from({length:10000},(_,i)=>({id:i,v:i*3.14}));
for(let r=0;r<5;r++)JSON.parse(JSON.stringify(d));
console.log(d.length)' >/tmp/_null 2>/tmp/_null
    _end Node.js "JSON 10K"
    _start; timeout 30 node -e 'console.log(require("crypto").createHash("sha256").update("x".repeat(1000000)).digest("hex").slice(0,8))' >/tmp/_null 2>/tmp/_null; _end Node.js "sha256"
fi
