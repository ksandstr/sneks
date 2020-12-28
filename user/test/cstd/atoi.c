
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <ccan/str/str.h>
#include <sneks/test.h>


START_TEST(l64a_static)
{
	plan(4);

	ok1(streq(l64a(123), "v/"));
	ok1(streq(l64a(INT32_MAX), "zzzzz/"));

	/* the part where we violate POSIX. */
#ifndef __sneks__
	skip_start(true, 2, "l64a(x < 0) unspecified") {
#endif

	ok1(streq(l64a(INT32_MIN), ".....0"));	/* aka 0x80000000 */
	ok1(streq(l64a(~0ul), "zzzzz1"));

#ifndef __sneks__
	} skip_end;
#endif
}
END_TEST

DECLARE_TEST("cstd:a64l", l64a_static);


START_TEST(a64l_static)
{
	plan(4);
	ok1(a64l("v/") == 123);
	ok1(a64l("zzzzz/") == INT32_MAX);
	ok1(a64l(".....0") == INT32_MIN);
	ok1(a64l("zzzzz1") == ~0ul);
}
END_TEST

DECLARE_TEST("cstd:a64l", a64l_static);
