// do_uname, against a host name longer than the field it goes in.
//
// struct uname's fields are 65 bytes and the host's is not: macOS gives
// utsname.nodename 256, Linux gives it 65 including the terminator, and an
// embedder can set uname_hostname_override to anything at all. This used to be
// a plain strcpy, which on macOS compiles to __builtin___strcpy_chk against the
// field's known size — so a machine with a long enough name did not get a
// truncated uname, it got SIGTRAP out of __chk_fail_overflow before the guest
// saw a thing. Every command run on such a host died.
//
// It was a CI runner that was named that, and it was the integration suite that
// noticed. This test is here so the next one is caught before it ships.

#include "kernel/calls.h"
#include "tests/unit/unit.h"

#include <stdlib.h>
#include <string.h>

// Set by do_uname's caller in the product; a test is the other caller.
extern const char *uname_hostname_override;

static const char *long_name =
    "a-host-with-a-name-far-longer-than-sixty-five-bytes-which-is-all-the-room-"
    "the-field-has-and-then-quite-a-lot-more-besides";

TEST(a_long_hostname_is_truncated_not_fatal) {
    uname_hostname_override = long_name;
    struct uname uts;
    // The bug was that this call did not return.
    do_uname(&uts);

    // Truncated to fit, and terminated inside the field: a caller doing
    // strlen() on it must not run into the next one.
    CHECK_EQ_INT(strlen(uts.hostname), sizeof(uts.hostname) - 1);
    CHECK_EQ_INT(uts.hostname[sizeof(uts.hostname) - 1], 0);
    // What did fit is the front of the name, not something else.
    CHECK_EQ_INT(strncmp(uts.hostname, long_name, sizeof(uts.hostname) - 1), 0);
}

// The field is 65 bytes, so 64 characters is the longest name that survives
// whole. Off-by-one country, and the only length worth naming explicitly.
TEST(a_name_that_exactly_fits_is_not_truncated) {
    char exact[65];
    memset(exact, 'x', 64);
    exact[64] = '\0';
    uname_hostname_override = exact;

    struct uname uts;
    do_uname(&uts);
    CHECK_EQ_INT(strlen(uts.hostname), 64);
    CHECK_EQ_INT(strcmp(uts.hostname, exact), 0);
}

// The rest of the struct is still filled, and the guest is still told it is
// Linux on aarch64 — a truncating copy that scribbled over its neighbours
// would show up here.
TEST(the_other_fields_survive) {
    uname_hostname_override = long_name;
    struct uname uts;
    do_uname(&uts);

    CHECK_EQ_INT(strcmp(uts.system, "Linux"), 0);
    CHECK_EQ_INT(strcmp(uts.arch, "aarch64"), 0);
    CHECK_EQ_INT(strcmp(uts.release, "4.20.69-ish"), 0);
    CHECK_EQ_INT(strcmp(uts.domain, "(none)"), 0);
    CHECK(strlen(uts.version) > 0);
    CHECK(strlen(uts.version) < sizeof(uts.version));
}

// Without an override it reports the machine's own name, whatever that is, and
// still fits.
TEST(the_real_hostname_fits) {
    uname_hostname_override = NULL;
    struct uname uts;
    do_uname(&uts);
    CHECK(strlen(uts.hostname) < sizeof(uts.hostname));
}

int main(void) {
    RUN(a_long_hostname_is_truncated_not_fatal);
    RUN(a_name_that_exactly_fits_is_not_truncated);
    RUN(the_other_fields_survive);
    RUN(the_real_hostname_fits);
    uname_hostname_override = NULL;
    return UNIT_REPORT();
}
