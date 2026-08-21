// waitid_decode_status: the wait(2) status word, read the way waitid(2)
// reports it.
//
// do_wait produces the packed status word wait4(2) copies out verbatim.
// waitid(2) does not take that: it reports the *decoded* value in si_status
// and says which kind of event it was in si_code. sys_waitid used to hand the
// raw word straight through, so a child that exited 3 was reported as having
// exited 0x300, and si_code was left at zero — no CLD_* value at all, which is
// not one of the answers waitid is allowed to give.
//
// The encodings come from the places that write child.status:
//
//   sys_exit / sys_exit_group   do_exit(status << 8)    low byte 0
//   deliver SIGNAL_KILL         do_exit_group(sig)      low byte is the signal
//   deliver SIGNAL_STOP         sig << 8 | 0x7f
//   ptrace stop                 signal << 8 | 0x7f
//
// Each case below is one of those, written as the producing expression rather
// than as a hex literal, so a change to how a status is packed shows up here.

#include "kernel/calls.h"
#include "kernel/signal.h"
#include "tests/unit/unit.h"

#include <stdio.h>

static struct siginfo_ decode(dword_t raw_status) {
    struct siginfo_ info = {};
    info.child.status = raw_status;
    waitid_decode_status(&info);
    return info;
}

// Every decode reports SIGCHLD, whatever the event was. waitid fills a
// siginfo_t for the signal the parent would have received.
TEST(always_reports_sigchld) {
    CHECK_EQ_INT(decode(3 << 8).sig, SIGCHLD_);
    CHECK_EQ_INT(decode(SIGKILL_).sig, SIGCHLD_);
    CHECK_EQ_INT(decode(SIGSTOP_ << 8 | 0x7f).sig, SIGCHLD_);
}

TEST(normal_exit) {
    struct siginfo_ info = decode(3 << 8);
    CHECK_EQ_INT(info.code, CLD_EXITED_);
    CHECK_EQ_INT(info.child.status, 3);

    // The two ends of the range. Zero is the case that looked right before the
    // fix and hid it: a raw 0 decodes to 0 either way.
    CHECK_EQ_INT(decode(0).code, CLD_EXITED_);
    CHECK_EQ_INT(decode(0).child.status, 0);
    CHECK_EQ_INT(decode(255 << 8).code, CLD_EXITED_);
    CHECK_EQ_INT(decode(255 << 8).child.status, 255);
}

TEST(killed_by_a_signal) {
    struct siginfo_ info = decode(SIGKILL_);
    CHECK_EQ_INT(info.code, CLD_KILLED_);
    CHECK_EQ_INT(info.child.status, SIGKILL_);

    info = decode(SIGSEGV_);
    CHECK_EQ_INT(info.code, CLD_KILLED_);
    CHECK_EQ_INT(info.child.status, SIGSEGV_);
}

// Nothing sets the 0x80 core-dump bit today, so this is the one case that is
// unreachable from a running system. It is decoded anyway, and asserted here,
// so that implementing core dumps does not silently report them as plain
// kills.
TEST(dumped_is_distinguished_from_killed) {
    struct siginfo_ info = decode(SIGSEGV_ | 0x80);
    CHECK_EQ_INT(info.code, CLD_DUMPED_);
    // The signal, without the dump bit: si_status is a signal number.
    CHECK_EQ_INT(info.child.status, SIGSEGV_);
}

TEST(stopped) {
    struct siginfo_ info = decode(SIGSTOP_ << 8 | 0x7f);
    CHECK_EQ_INT(info.code, CLD_STOPPED_);
    CHECK_EQ_INT(info.child.status, SIGSTOP_);

    // A ptrace stop packs the same way, with the trap signal.
    info = decode(SIGTRAP_ << 8 | 0x7f);
    CHECK_EQ_INT(info.code, CLD_STOPPED_);
    CHECK_EQ_INT(info.child.status, SIGTRAP_);
}

// A ptrace stop and a group stop pack into the same status word. do_wait is
// the only place that knows which one it read, so it marks the ptrace one and
// the decode has to leave that mark alone — Linux reports CLD_TRAPPED there,
// not CLD_STOPPED. Driving a real tracee is out of reach from a unit test; what
// is testable, and what the decode is responsible for, is not overwriting it.
TEST(a_marked_ptrace_stop_stays_trapped) {
    struct siginfo_ info = {};
    info.child.status = SIGTRAP_ << 8 | 0x7f;
    info.code = CLD_TRAPPED_;
    waitid_decode_status(&info);
    CHECK_EQ_INT(info.code, CLD_TRAPPED_);
    // The status still decodes: the mark says which stop it was, not what to
    // do with the word.
    CHECK_EQ_INT(info.child.status, SIGTRAP_);
    CHECK_EQ_INT(info.sig, SIGCHLD_);

    // And an unmarked stop is still an ordinary one.
    CHECK_EQ_INT(decode(SIGTRAP_ << 8 | 0x7f).code, CLD_STOPPED_);
}

TEST(continued) {
    struct siginfo_ info = decode(0xffff);
    CHECK_EQ_INT(info.code, CLD_CONTINUED_);
    CHECK_EQ_INT(info.child.status, SIGCONT_);
}

// The orderings that matter. 0xffff also satisfies "low byte is 0x7f" and
// "low 7 bits are non-zero", and a signal-death status satisfies neither of
// the two before it — so each case has to be tried in the right order, and
// this pins that rather than the arithmetic.
TEST(overlapping_encodings_resolve_to_the_right_case) {
    // Stopped, not "killed by signal 0x7f": 0x7f is not a signal number, and
    // reading it as one would report SIGRTMAX-ish nonsense.
    CHECK_EQ_INT(decode(SIGSTOP_ << 8 | 0x7f).code, CLD_STOPPED_);
    // Continued, not stopped-with-signal-255.
    CHECK_EQ_INT(decode(0xffff).code, CLD_CONTINUED_);
    // A high byte on a signal death does not make it an exit: only the low
    // seven bits being zero does.
    CHECK_EQ_INT(decode(3 << 8 | SIGKILL_).code, CLD_KILLED_);
    CHECK_EQ_INT(decode(3 << 8 | SIGKILL_).child.status, SIGKILL_);
}

int main(void) {
    RUN(always_reports_sigchld);
    RUN(normal_exit);
    RUN(killed_by_a_signal);
    RUN(dumped_is_distinguished_from_killed);
    RUN(stopped);
    RUN(a_marked_ptrace_stop_stays_trapped);
    RUN(continued);
    RUN(overlapping_encodings_resolve_to_the_right_case);
    return UNIT_REPORT();
}
