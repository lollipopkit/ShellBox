// The control-message walk and the iovec bound in fs/sock.h.
//
// The control-message walk: sys_sendmsg iterates a guest-supplied
// buffer with CMSG_NXTHDR_, so the only thing between a malformed cmsghdr and
// an unbounded loop or a read past the end is what these macros answer. Every
// field they look at comes from the guest.
//
// fs/sock.h needs the guest word types but nothing else from the kernel, so
// this test compiles on its own.

#include "misc.h"
#include "fs/sock.h"
#include "tests/unit/unit.h"

#include <stdint.h>

// One cmsghdr_ at the front of a buffer, with room after it to walk into.
#define BUF_SIZE 256
static uint8_t buf[BUF_SIZE];

static struct cmsghdr_ *first(uint32_t len, uint32_t level, uint32_t type) {
    memset(buf, 0, sizeof(buf));
    struct cmsghdr_ *cmsg = (void *) buf;
    cmsg->len = len;
    cmsg->level = level;
    cmsg->type = type;
    return cmsg;
}

// A length is rounded up to a multiple of the guest's word size. Done in the
// guest's own 32-bit width, a length near UINT32_MAX wraps to zero: CMSG_NEXT_
// then advances by nothing and the caller's loop never moves off the first
// header. The arithmetic is in size_t now, and the second guard makes every
// step strictly forward whatever the rounding produced.
TEST(len_near_uint32_max_does_not_wrap) {
    uint8_t *end = buf + sizeof(buf);
    struct cmsghdr_ *cmsg = first(UINT32_MAX, SOL_SOCKET_, SCM_RIGHTS_);
    // The rounded length is larger than a 32-bit type can hold, so it is
    // certainly past the end of the buffer: the walk stops.
    CHECK(CMSG_LEN_(cmsg) > UINT32_MAX);
    CHECK(CMSG_NXTHDR_(cmsg, end) == NULL);

    // The exact value that used to round to zero.
    cmsg = first(0xfffffffd, SOL_SOCKET_, SCM_RIGHTS_);
    CHECK(CMSG_LEN_(cmsg) != 0);
    CHECK(CMSG_NXTHDR_(cmsg, end) == NULL);
}

// A length that rounds to no more than the header itself would leave the
// cursor where it is even without any wrapping.
TEST(len_no_larger_than_header_stops) {
    uint8_t *end = buf + sizeof(buf);
    struct cmsghdr_ *cmsg = first(sizeof(struct cmsghdr_), SOL_SOCKET_, SCM_RIGHTS_);
    CHECK(CMSG_NXTHDR_(cmsg, end) == NULL);
}

// Shorter than a header at all: there is nothing to read a length out of.
TEST(len_shorter_than_header_stops) {
    uint8_t *end = buf + sizeof(buf);
    struct cmsghdr_ *cmsg = first(1, SOL_SOCKET_, SCM_RIGHTS_);
    CHECK(CMSG_NXTHDR_(cmsg, end) == NULL);
    cmsg = first(0, SOL_SOCKET_, SCM_RIGHTS_);
    CHECK(CMSG_NXTHDR_(cmsg, end) == NULL);
}

// A step that would land at or past the end stops, so the caller never reads a
// header that is not wholly inside the buffer.
TEST(step_past_end_stops) {
    uint8_t *end = buf + sizeof(buf);
    struct cmsghdr_ *cmsg = first(BUF_SIZE, SOL_SOCKET_, SCM_RIGHTS_);
    CHECK(CMSG_NXTHDR_(cmsg, end) == NULL);
    // One header short of the end: there is no room for the next header's
    // own fields, so this stops too.
    cmsg = first(BUF_SIZE - (uint32_t) sizeof(struct cmsghdr_), SOL_SOCKET_, SCM_RIGHTS_);
    CHECK(CMSG_NXTHDR_(cmsg, end) == NULL);
}

// A well-formed pair walks once and then stops, and the walk terminates.
TEST(well_formed_walk_advances_then_ends) {
    uint8_t *end = buf + sizeof(buf);
    uint32_t len = (uint32_t) (sizeof(struct cmsghdr_) + sizeof(int));
    struct cmsghdr_ *cmsg = first(len, SOL_SOCKET_, SCM_RIGHTS_);

    struct cmsghdr_ *next = CMSG_NXTHDR_(cmsg, end);
    CHECK(next != NULL);
    CHECK((uint8_t *) next > (uint8_t *) cmsg);
    CHECK((uint8_t *) next + sizeof(struct cmsghdr_) <= end);

    // The second header is zeroed, so it is shorter than a header and ends
    // the walk rather than continuing into the rest of the buffer.
    CHECK(CMSG_NXTHDR_(next, end) == NULL);
}

// Every step is strictly forward, for any length the guest can name. This is
// the property sys_sendmsg's loop depends on to terminate; the cases above are
// the specific ways it was broken.
TEST(every_step_is_forward) {
    uint8_t *end = buf + sizeof(buf);
    static const uint32_t lens[] = {
        0, 1, 3, 4, 7, 8, 12, 13,
        BUF_SIZE / 2, BUF_SIZE - 1, BUF_SIZE, BUF_SIZE + 1,
        0x7ffffffd, 0x7fffffff, 0x80000000, 0xfffffffc, 0xfffffffd,
        0xfffffffe, UINT32_MAX,
    };
    for (unsigned i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
        struct cmsghdr_ *cmsg = first(lens[i], SOL_SOCKET_, SCM_RIGHTS_);
        struct cmsghdr_ *next = CMSG_NXTHDR_(cmsg, end);
        if (next == NULL)
            continue;
        if ((uint8_t *) next <= (uint8_t *) cmsg)
            UNIT_FAIL("len %#x: next %p did not advance past %p",
                      lens[i], (void *) next, (void *) cmsg);
        if ((uint8_t *) next + sizeof(struct cmsghdr_) > end)
            UNIT_FAIL("len %#x: next header is not wholly in the buffer", lens[i]);
    }
}

// A guest names its own iovec lengths, and each one becomes a host malloc of
// exactly that size. iov_len_ok is what stands between that and 1024
// unbounded allocations for one syscall. It has to reject on the running sum,
// not only per vector, and it must not overflow doing so.
TEST(iov_single_vector_bound) {
    size_t total = 0;
    CHECK(iov_len_ok(0, &total));
    CHECK_EQ_INT(total, 0);
    CHECK(iov_len_ok(1, &total));
    CHECK_EQ_INT(total, 1);

    total = 0;
    CHECK(iov_len_ok(IOV_TOTAL_MAX, &total));
    CHECK_EQ_INT(total, IOV_TOTAL_MAX);

    // One past the cap, on its own. A rejection leaves the running sum alone.
    total = 0;
    CHECK(!iov_len_ok((uint64_t) IOV_TOTAL_MAX + 1, &total));
    CHECK_EQ_INT(total, 0);

    // What the 64-bit guest layout can name, which is well past any size_t
    // arithmetic that would stay meaningful.
    total = 0;
    CHECK(!iov_len_ok(UINT64_MAX, &total));
    CHECK(!iov_len_ok(0x100000000ull, &total));
    CHECK(!iov_len_ok(UINT32_MAX, &total));
    CHECK_EQ_INT(total, 0);
}

TEST(iov_running_total_bound) {
    // Each vector fits on its own; together they do not. This is the case a
    // per-vector check alone lets through.
    size_t total = 0;
    uint64_t half = IOV_TOTAL_MAX / 2;
    CHECK(iov_len_ok(half, &total));
    CHECK(iov_len_ok(half, &total));
    CHECK(iov_len_ok(1, &total));   // exactly the cap, since the cap is odd
    CHECK(!iov_len_ok(1, &total));  // one more is refused
    CHECK_EQ_INT(total, IOV_TOTAL_MAX);

    // The same boundary from the other side.
    total = IOV_TOTAL_MAX - 1;
    CHECK(iov_len_ok(1, &total));
    CHECK_EQ_INT(total, IOV_TOTAL_MAX);
    CHECK(!iov_len_ok(1, &total));
}

// A full array of the largest count a guest may pass, each vector as large as
// it may be: the total is refused long before that much is allocated, and the
// arithmetic does not wrap on the way there.
TEST(iov_full_array_is_refused) {
    size_t total = 0;
    int accepted = 0;
    for (int i = 0; i < UIO_MAXIOV_; i++) {
        if (!iov_len_ok(UINT32_MAX, &total))
            break;
        accepted++;
    }
    CHECK(accepted < UIO_MAXIOV_);
    CHECK(total <= IOV_TOTAL_MAX);
}

int main(void) {
    RUN(iov_single_vector_bound);
    RUN(iov_running_total_bound);
    RUN(iov_full_array_is_refused);
    RUN(len_near_uint32_max_does_not_wrap);
    RUN(len_no_larger_than_header_stops);
    RUN(len_shorter_than_header_stops);
    RUN(step_past_end_stops);
    RUN(well_formed_walk_advances_then_ends);
    RUN(every_step_is_forward);
    return UNIT_REPORT();
}
