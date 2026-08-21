// The x86 vector ops that emu/arch/x86/vec.c gets to decide on its own: what
// happens when the two operands are the same register, how a tie between +0
// and -0 is broken, and what a float that does not fit an int32 converts to.
// Each is a case the interpreter reaches through a legal encoding, and each
// was wrong in a way that produced a plausible number rather than a crash.
//
// These are pure functions over xmm registers, so the expected values here are
// the ISA's, not this implementation's. Where a case is subtle the reference
// is named in the comment.

#include "emu/cpu.h"
#include "emu/arch/x86/vec.h"
#include "tests/unit/unit.h"

#include <math.h>

static union xmm_reg xmm_u32(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    union xmm_reg r;
    r.u32[0] = a; r.u32[1] = b; r.u32[2] = c; r.u32[3] = d;
    return r;
}

static void check_u32x4(const union xmm_reg *r,
        uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    CHECK_EQ(r->u32[0], a);
    CHECK_EQ(r->u32[1], b);
    CHECK_EQ(r->u32[2], c);
    CHECK_EQ(r->u32[3], d);
}

// SHUFPS builds its result from the *original* operands. Two ways that goes
// wrong if the destination is written as it is read, and both are reachable:
//   - the second low lane can select the first, which by then holds the
//     first's result rather than its input. Nothing about the operands has to
//     be unusual for this.
//   - with src == dst (SHUFPS xmm0, xmm0, imm is a legal encoding) the high
//     lanes read a source whose low half is already the result.
TEST(shufps_lane_written_then_selected) {
    union xmm_reg src = xmm_u32(0xaa, 0xbb, 0xcc, 0xdd);
    union xmm_reg dst = xmm_u32(0x11, 0x22, 0x33, 0x44);
    // imm 0b00_00_00_01: low lanes select dst[1] then dst[0], high lanes
    // select src[0] twice.
    vec_shuffle_ps128(NULL, &src, &dst, 0x01);
    check_u32x4(&dst, 0x22, 0x11, 0xaa, 0xaa);
}

TEST(shufps_same_register) {
    union xmm_reg r = xmm_u32(0x11, 0x22, 0x33, 0x44);
    // imm 0b01_00_10_11: dst[3], dst[2], then src[0], src[1] — which are the
    // lanes just overwritten if the result is not taken from a copy.
    vec_shuffle_ps128(NULL, &r, &r, 0x4b);
    check_u32x4(&r, 0x44, 0x33, 0x11, 0x22);
}

TEST(shufpd_same_register) {
    union xmm_reg r;
    r.qw[0] = 0x1111111111111111ull;
    r.qw[1] = 0x2222222222222222ull;
    // imm 0b01: low half takes dst[1], high half takes src[0] — again the
    // half that was just written.
    vec_shuffle_pd128(NULL, &r, &r, 0x01);
    CHECK_EQ(r.qw[0], 0x2222222222222222ull);
    CHECK_EQ(r.qw[1], 0x1111111111111111ull);
}

// PACKSSWB xmm0, xmm0 packs the same register's words into both halves of the
// result. Written in place, the source's low eight bytes are the packed result
// by the time the high half reads them.
TEST(packss_same_register) {
    union xmm_reg r;
    for (int i = 0; i < 8; i++)
        r.u16[i] = (uint16_t) (i + 1);
    vec_packss_w128(NULL, &r, &r);
    // Both halves are the same eight saturated bytes: 1..8 twice.
    for (int i = 0; i < 8; i++) {
        CHECK_EQ(r.u8[i], i + 1);
        CHECK_EQ(r.u8[i + 8], i + 1);
    }
}

TEST(packss_saturates_signed) {
    union xmm_reg src, dst;
    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));
    dst.u16[0] = 0x0080;         // +128, one past what a signed byte holds
    dst.u16[1] = (uint16_t) -129;
    dst.u16[2] = 0x007f;         // +127, the largest that fits
    dst.u16[3] = (uint16_t) -128;
    vec_packss_w128(NULL, &src, &dst);
    CHECK_EQ(dst.u8[0], 0x7f);
    CHECK_EQ(dst.u8[1], 0x80);
    CHECK_EQ(dst.u8[2], 0x7f);
    CHECK_EQ(dst.u8[3], 0x80);
}

// PACKUSWB saturates to an *unsigned* byte, so anything negative becomes 0.
TEST(packsu_saturates_unsigned) {
    union xmm_reg src, dst;
    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));
    dst.u16[0] = 0x0100;         // 256, one past a byte
    dst.u16[1] = (uint16_t) -1;
    dst.u16[2] = 0x00ff;
    vec_packsu_w128(NULL, &src, &dst);
    CHECK_EQ(dst.u8[0], 0xff);
    CHECK_EQ(dst.u8[1], 0x00);
    CHECK_EQ(dst.u8[2], 0xff);
}

// PADDSB / PSUBSB saturate each byte to the signed range. The boundaries on
// either side of the clamp matter: a sum of exactly -128 or +127 is
// representable and must come through unchanged.
TEST(addss_b_saturation_boundaries) {
    union xmm_reg src, dst;
    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));
    dst.u8[0] = 100;  src.u8[0] = 100;                   // +200 -> +127
    dst.u8[1] = 100;  src.u8[1] = 27;                    // +127 exactly
    dst.u8[2] = (uint8_t) -100; src.u8[2] = (uint8_t) -100; // -200 -> -128
    dst.u8[3] = (uint8_t) -100; src.u8[3] = (uint8_t) -28;  // -128 exactly
    dst.u8[4] = (uint8_t) -100; src.u8[4] = (uint8_t) -29;  // -129 -> -128
    vec_addss_b128(NULL, &src, &dst);
    CHECK_EQ(dst.u8[0], 0x7f);
    CHECK_EQ(dst.u8[1], 0x7f);
    CHECK_EQ(dst.u8[2], 0x80);
    CHECK_EQ(dst.u8[3], 0x80);
    CHECK_EQ(dst.u8[4], 0x80);
}

// SSE MIN/MAX are defined as `dst = (dst OP src) ? dst : src`. That decides
// the two cases a comparison alone cannot, and both take the *source*: an
// unordered operand, and a tie. +0 and -0 compare equal, so MINSD of them
// answers with whichever one src holds — visible through signbit and through
// any bitwise use of the result.
TEST(minmax_tie_takes_source) {
    double dst = 0.0, src = -0.0;
    vec_single_fmin64(NULL, &src, &dst);
    CHECK(signbit(dst));

    dst = -0.0; src = 0.0;
    vec_single_fmin64(NULL, &src, &dst);
    CHECK(!signbit(dst));

    dst = 0.0; src = -0.0;
    vec_single_fmax64(NULL, &src, &dst);
    CHECK(signbit(dst));

    float fdst = 0.0f, fsrc = -0.0f;
    vec_single_fmin32(NULL, &fsrc, &fdst);
    CHECK(signbit(fdst));
}

TEST(minmax_nan_takes_source) {
    double nan_val = nan("");

    // dst is NaN: every comparison is false, so the source wins.
    double dst = nan_val, src = 1.0;
    vec_single_fmin64(NULL, &src, &dst);
    CHECK_EQ_INT(dst == 1.0, 1);

    // src is NaN: the comparison is false again, and the source still wins.
    dst = 1.0; src = nan_val;
    vec_single_fmin64(NULL, &src, &dst);
    CHECK(isnan(dst));

    dst = nan_val; src = 1.0;
    vec_single_fmax64(NULL, &src, &dst);
    CHECK_EQ_INT(dst == 1.0, 1);
}

TEST(minmax_ordinary_values) {
    double dst = 3.0, src = 1.0;
    vec_single_fmin64(NULL, &src, &dst);
    CHECK_EQ_INT(dst == 1.0, 1);

    dst = 3.0; src = 1.0;
    vec_single_fmax64(NULL, &src, &dst);
    CHECK_EQ_INT(dst == 3.0, 1);

    float fdst = 3.0f, fsrc = 1.0f;
    vec_single_fmax32(NULL, &fsrc, &fdst);
    CHECK_EQ_INT(fdst == 3.0f, 1);
}

// A float that does not fit an int32 converts to the "integer indefinite"
// value, 0x80000000, and so does a NaN. That is the ISA's answer and it is not
// what the host's own conversion gives: on arm64 FCVTZS saturates to INT32_MAX
// instead, so the guest saw a different number depending on the host.
TEST(cvt_out_of_range_is_indefinite) {
    int32_t out;
    double big = 1e300;
    vec_cvttsd2si64(NULL, &big, &out);
    CHECK_EQ((uint32_t) out, 0x80000000u);

    double small = -1e300;
    vec_cvttsd2si64(NULL, &small, &out);
    CHECK_EQ((uint32_t) out, 0x80000000u);

    double nan_val = nan("");
    vec_cvttsd2si64(NULL, &nan_val, &out);
    CHECK_EQ((uint32_t) out, 0x80000000u);

    // 2^31 is the first value out of range; 2^31 - 1 is the last one in.
    double edge = 2147483648.0;
    vec_cvttsd2si64(NULL, &edge, &out);
    CHECK_EQ((uint32_t) out, 0x80000000u);
    double in_range = 2147483647.0;
    vec_cvttsd2si64(NULL, &in_range, &out);
    CHECK_EQ((uint32_t) out, 2147483647u);
    // -2^31 is representable, and must not be mistaken for the indefinite
    // value's *cause* — it is the correct answer here.
    double low_edge = -2147483648.0;
    vec_cvttsd2si64(NULL, &low_edge, &out);
    CHECK_EQ((uint32_t) out, 0x80000000u);

    float fbig = 1e30f;
    vec_cvttss2si32(NULL, &fbig, &out);
    CHECK_EQ((uint32_t) out, 0x80000000u);
}

// CVTSD2SS converts between float formats. Its NaN answer is a NaN — the
// indefinite integer belongs to the conversions that produce an integer, and
// sharing one test across both wrote -2147483648.0f into a float destination.
TEST(cvt_float_to_float_keeps_nan) {
    double nan_val = nan("");
    float out = 0.0f;
    vec_cvtsd2ss64(NULL, &nan_val, &out);
    CHECK(isnan(out));

    float fnan = nanf("");
    double dout = 0.0;
    vec_cvtss2sd32(NULL, &fnan, &dout);
    CHECK(isnan(dout));

    double in = 1.5;
    vec_cvtsd2ss64(NULL, &in, &out);
    CHECK_EQ_INT(out == 1.5f, 1);
}

// The packed conversions clear the lanes above the ones they write, and read
// their source before doing so.
TEST(cvt_packed_clears_high_lanes) {
    union xmm_reg r;
    r.f64[0] = 1.0;
    r.f64[1] = 2.0;
    vec_cvttpd2dq64(NULL, &r, &r);
    CHECK_EQ(r.u32[0], 1);
    CHECK_EQ(r.u32[1], 2);
    CHECK_EQ(r.u32[2], 0);
    CHECK_EQ(r.u32[3], 0);
}

int main(void) {
    RUN(shufps_lane_written_then_selected);
    RUN(shufps_same_register);
    RUN(shufpd_same_register);
    RUN(packss_same_register);
    RUN(packss_saturates_signed);
    RUN(packsu_saturates_unsigned);
    RUN(addss_b_saturation_boundaries);
    RUN(minmax_tie_takes_source);
    RUN(minmax_nan_takes_source);
    RUN(minmax_ordinary_values);
    RUN(cvt_out_of_range_is_indefinite);
    RUN(cvt_float_to_float_keeps_nan);
    RUN(cvt_packed_clears_high_lanes);
    return UNIT_REPORT();
}
