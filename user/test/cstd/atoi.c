/* string-to-whatever conversions and back, except printf/scanf family */
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#include <assert.h>
#include <errno.h>
#include <ccan/str/str.h>
#include <ccan/array_size/array_size.h>
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

DECLARE_TEST("cstd:atoi", l64a_static);

START_TEST(a64l_static)
{
	plan(4);
	ok1(a64l("v/") == 123);
	ok1(a64l("zzzzz/") == INT32_MAX);
	ok1(a64l(".....0") == INT32_MIN);
	ok1(a64l("zzzzz1") == ~0ul);
}
END_TEST

DECLARE_TEST("cstd:atoi", a64l_static);

/* static test cases of strtoul(3). */
START_TEST(strtoul_static)
{
	plan_tests(16);
	char *end;
	ok1(strtoul("-1", NULL, 10) == -1);
	ok1(strtoul("+1", NULL, 10) == 1);
	ok1(strtoul("  -1", NULL, 10) == -1);
	ok1(strtoul("   1", NULL, 10) == 1);
	ok1(strtoul("123s", &end, 0) == 123 && end != NULL && streq(end, "s"));
	ok1(strtoul("123\000456s", &end, 0) == 123 && end != NULL && *end == '\0');
	ok1(strtoul("0777", NULL, 8) == 0777);
	ok1(strtoul("0777", NULL, 0) == 0777);
	ok1(strtoul("0777", NULL, 10) == 777);
	ok1(strtoul("0xbabe", NULL, 16) == 0xbabe);
	ok1(strtoul("0xf00d", NULL, 0) == 0xf00d);
	ok1(strtoul("0XB0A7", NULL, 0) == 0xb0a7);
	ok1(strtoul("1001", NULL, 2) == 9);
	ok1(strtoul("z", NULL, 36) == 35);
	ok1(strtoul("4294967295", NULL, 10) == (1ull << 32) - 1);
	ok1(strtoul("-4294967295", NULL, 10) == 1);	/* two's complemagic */
}
END_TEST

DECLARE_TEST("cstd:atoi", strtoul_static);

/* error cases. this one isn't LP64 clean. */
START_TEST(strtoul_error)
{
	plan_tests(10);
	errno = 0; ok1(strtoul("", NULL, 10) == 0 && (errno == 0 || errno == EINVAL));
	errno = 0; ok1(strtoul("-zzzzzzzzzzzzzzzz", NULL, 36) == ULONG_MAX && errno == ERANGE);
	errno = 0; ok1(strtoul("zzzzzzzzzzzzzzzz", NULL, 36) == ULONG_MAX && errno == ERANGE);
	errno = 0; ok1(strtoul("4294967296", NULL, 10) == ULONG_MAX && errno == ERANGE);
	errno = 0; ok1(strtoul("123", NULL, 1) == 0 && errno == EINVAL);
	errno = 0; ok1(strtoul("123", NULL, 37) == 0 && errno == EINVAL);
	errno = 0; ok1(strtoul("123", NULL, -10) == 0 && errno == EINVAL);
	char *end;
	end = NULL; ok1(strtoul("+ 123", &end, 10) == 0 && end != NULL && streq(end, "+ 123"));
	end = NULL; ok1(strtoul("- 123", &end, 10) == 0 && end != NULL && streq(end, "- 123"));
	end = NULL; ok1(strtoul("+0 123", &end, 10) == 0 && end != NULL && streq(end, " 123"));
}
END_TEST

DECLARE_TEST("cstd:atoi", strtoul_error);

static void ultos(char buf[static 65], unsigned long val, int base)
{
	assert(base >= 2 && base <= 36);
	if(val == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
	int pos = 0;
	while(val > 0) {
		buf[pos++] = "0123456789abcdefghijklmnopqrstuvwxyz"[val % base];
		val /= base;
	}
	for(int i=0; i < pos / 2; i++) {
		char *a = &buf[i], *b = &buf[pos - 1 - i], t = *a;
		*a = *b;
		*b = t;
	}
	buf[pos] = '\0';
}

START_TEST(ultos_validation)
{
	plan_tests(7);
#define try(ref, val, base) ({ char buf[65]; ultos(buf, (val), (base)); streq((ref), buf); })
	ok1(try("0", 0, 2));
	ok1(try("0", 0, 36));
	ok1(try("10", 10, 10));
	ok1(try("1f", 31, 16));
	ok1(try("1001", 9, 2));
	ok1(try("777", 0777, 8));
	ok1(try("z", 35, 36));
#undef try
}
END_TEST

DECLARE_TEST("cstd:atoi", ultos_validation);

/* all bases between 2 and 36 are belong to us. */
START_LOOP_TEST(strtoul_aybabtu, base, 2, 36)
{
	const unsigned long cases[] = { 0, 1, base - 1, base, base + 1, base * 2 - 1, base * 2, base * 2 + 1, 12345, ULONG_MAX };
	plan_tests(ARRAY_SIZE(cases));
	for(int i=0; i < ARRAY_SIZE(cases); i++) {
		char buf[65], *end;
		ultos(buf, cases[i], base);
		if(!ok(strtoul(buf, &end, base) == cases[i], "case=%lu", cases[i])) diag("buf=`%s'", buf);
	}
}
END_TEST

DECLARE_TEST("cstd:atoi", strtoul_aybabtu);
