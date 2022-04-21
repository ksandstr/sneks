/* tests on vsnprintf(). */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <ccan/compiler/compiler.h>
#include <ccan/str/str.h>
#include <ccan/minmax/minmax.h>
#include <sneks/test.h>

#define fmterr(fmt, ...) (snprintf(NULL, 0, (fmt), ##__VA_ARGS__) < 0)

static bool test_fmt(const char *expect, const char *fmt, ...) PRINTF_FMT(2, 3);
static bool trunc_ok(size_t prefix, const char *fmt, ...) PRINTF_FMT(2, 3);

static bool has_bitwidth(void) {
	/* weak decision: if wN is unsupported for %d, it's the same for %n. */
	char buf[20]; snprintf(buf, sizeof buf, "%w64d", (int64_t)~LONG_MAX);
	return !streq(buf, "%w64d");
}

static bool is_sneks(void) {	/* TODO: generally usable, move to util */
#ifdef __sneks__
	return true;
#else
	return false;
#endif
}

static bool test_fmt(const char *expect, const char *fmt, ...)
{
	va_list al; va_start(al, fmt);
	size_t n = max(vsnprintf(NULL, 0, fmt, al), 10);
	va_end(al); va_start(al, fmt);
	char buf[n + 16]; memset(buf, 0xff, sizeof buf - 1); buf[sizeof buf - 1] = '\0';
	n = vsnprintf(buf, n + 1, fmt, al);
	va_end(al);
	bool res = buf[n] == '\0' && n == strlen(expect) && streq(buf, expect);
	if(!res) diag("expect=`%s' n=%zu buf[n]=0x%02x buf=`%s'", expect, n, (unsigned)buf[n], buf);
	return res;
}

static bool trunc_ok(size_t prefix, const char *fmt, ...)
{
	subtest_start("trunc prefix=%zu fmt=`%s'", prefix, fmt);
	plan(7);
	va_list al; va_start(al, fmt);
	/* self-validation: @prefix should be less than what the format yields. */
	char head[prefix + 1];
	int n_head = vsnprintf(head, sizeof head, fmt, al);
	va_end(al);
	if(!ok1(n_head >= 0)) diag("fmt=`%s', errno=%d", fmt, errno);
	if(!ok1(n_head > strlen(head))) diag("n_head=%d, head=`%s' (len=%d)", n_head, head, strlen(head));
	ok1(prefix == strlen(head));
	/* test that the output is same when produced into both excessively large
	 * and exactly-sized buffers.
	 */
	char excess[n_head + max(n_head, 100) + 1];
	va_start(al, fmt);
	int n_excess = vsnprintf(excess, sizeof excess, fmt, al);
	va_end(al);
	if(!ok1(n_excess == n_head)) diag("n_excess=%d, n_head=%d", n_excess, n_head);
	ok1(strstarts(excess, head));
	char exact[max(n_excess, n_head) + 1];
	va_start(al, fmt);
	int n_exact = vsnprintf(exact, sizeof exact, fmt, al);
	va_end(al);
	if(!ok1(n_exact == n_head)) diag("n_exact=%d, n_head=%d", n_exact, n_head);
	ok1(streq(exact, excess));
	diag("head=`%s', exact=`%s'", head, exact);
	return subtest_end() == 0;
}

/* basic format processing. */
START_TEST(basic)
{
	plan_tests(10);
	ok1(test_fmt("", ""));
	ok1(test_fmt("foo", "foo"));
	ok1(test_fmt("", "%s", ""));
	ok1(test_fmt("foo", "%s", "foo"));
	ok1(test_fmt("5", "%d", 5));
	ok1(test_fmt("5", "%c", '5'));
	ok1(test_fmt("%", "%%"));
	ok1(test_fmt("foo%5", "foo%%%d", 5));
	ok(!test_fmt("bar", "foo"), "validate test_fmt()");
	ok(!test_fmt("foo", "foo%"), "validate test_fmt()");
}
END_TEST

DECLARE_TEST("cstd:print", basic);

/* errors */
START_TEST(error)
{
	plan_tests(30);
	ok(!fmterr("foo"), "validate fmterr()");
#define _(x, ...) ok1(fmterr(x, ##__VA_ARGS__));
	_("%"); _("foo%"); _("%l"); _("%h"); _("%.");	/* premature ends */
	_("%1"); _("%.1");	/* incomplete width/prec spec */
	skip_start(!is_sneks(), 6, "strict sneks length modifiers") {	/* undefined in spec, sneks pops errors. */
		_("%llld", 1ll); _("%hhhd", 0);	/* too much longness/shortness */
		_("%llhd", 1ll); _("%hld", 1); _("%lhd", 1); _("%jld", 1);	/* invalid combinations */
	} skip_end;
	skip_start(!has_bitwidth(), 16, "no support for C23 bit-width modifiers") {
		_("%w", 0); _("%wf", 0); _("%wd", 0); _("%wfd", 0);	/* incomplete bitwidth spec */
		_("%w7d", 0); _("%wf7d", 0); _("%w65d", 0ll); _("%wf65d", 0ll); /* invalid bit width in bitwidth spec */
		_("%w4d", 0); _("%wf4d", 0); _("%w128d", 0ll, 0ll); _("%wf128d", 0ll, 0ll);	/* ibid */
		_("%w16hd", 0); _("%wf16hd", 0); _("%w8ld", 0); _("%wf8ld", 0); /* bitwidth and longness at same time */
	} skip_end;
#undef _
}
END_TEST

DECLARE_TEST("cstd:print", error);

/* concatenation */
START_TEST(concat)
{
	plan_tests(10);
	ok1(test_fmt("foo%", "foo%%"));
	ok1(test_fmt("%foo", "%%foo"));
	ok1(test_fmt("foo%bar", "foo%%bar"));
	ok1(test_fmt("fooXX", "foo%s", "XX"));
	ok1(test_fmt("XXfoo", "%sfoo", "XX"));
	ok1(test_fmt("fooXXbar", "foo%sbar", "XX"));
	ok1(test_fmt("foo5", "foo%d", 5));
	ok1(test_fmt("5foo", "%dfoo", 5));
	ok1(test_fmt("foo5bar", "foo%dbar", 5));
	ok1(test_fmt("fooXXbar5zot", "foo%sbar%dzot", "XX", 5));
}
END_TEST

DECLARE_TEST("cstd:print", concat);

/* huge numbers. huge */
START_TEST(huge)
{
	const int big = 2 * 1024 * 1024;	/* cDonald's theorem */
	diag("big=%d", big);
	plan_tests(8);
#ifdef __sneks__
	todo_start("foolish, foolish");
#endif
	ok1(snprintf(NULL, 0, "%*d", big, 123) == big);
	ok1(snprintf(NULL, 0, "%.*d", big, 123) == big);
	ok1(snprintf(NULL, 0, "%-*d", big, 123) == big);
	ok1(snprintf(NULL, 0, "%-.*d", big, 123) == big);
	char pref[5];
	ok1(snprintf(pref, sizeof pref, "%*d", big, 123) == big && streq(pref, "    "));
	ok1(snprintf(pref, sizeof pref, "%.*d", big, 123) == big && streq(pref, "0000"));
	ok1(snprintf(pref, sizeof pref, "%-*d", big, 123) == big && streq(pref, "123 "));
	ok1(snprintf(pref, sizeof pref, "%-.*d", big, 123) == big && streq(pref, "0000"));
}
END_TEST

DECLARE_TEST("cstd:print", huge);

/* %n */
START_TEST(percent_n)
{
	plan_tests(19);
	{ int d = -1; ok1(test_fmt("", "%n", &d) && d == 0); }
	{ int d = -1; ok1(test_fmt("z", "%c%n", 'z', &d) && d == 1); }
	{ int d = -1; ok1(test_fmt("zZ", "%c%n%s", 'z', &d, "Z") && d == 1); }
	{ int d = -1; ok1(test_fmt("foo", "%s%n", "foo", &d) && d == 3); }
	{ char hh = 'z'; ok1(test_fmt("foo", "%s%hhn", "foo", &hh) && hh == 3); }
	{ short h = -1; ok1(test_fmt("foo", "%s%hn", "foo", &h) && h == 3); }
	{ long l = -1; ok1(test_fmt("foo", "%s%ln", "foo", &l) && l == 3); }
	{ long long ll = -1; ok1(test_fmt("foo", "%s%lln", "foo", &ll) && ll == 3); }
	{ intmax_t j = -1; ok1(test_fmt("foo", "%s%jn", "foo", &j) && j == 3); }
	{ ssize_t z = -1; ok1(test_fmt("foo", "%s%zn", "foo", &z) && z == 3); }
	{ ptrdiff_t t = -1; ok1(test_fmt("foo", "%s%tn", "foo", &t) && t == 3); }
	skip_start(!has_bitwidth(), 8, "target doesn't support C23 wN length modifier") {
		{ int8_t i8 = -1; ok1(test_fmt("foo", "%s%w8n", "foo", &i8) && i8 == 3); }
		{ int16_t i16 = -1; ok1(test_fmt("foo", "%s%w16n", "foo", &i16) && i16 == 3); }
		{ int32_t i32 = -1; ok1(test_fmt("foo", "%s%w32n", "foo", &i32) && i32 == 3); }
		{ int64_t i64 = -1; ok1(test_fmt("foo", "%s%w64n", "foo", &i64) && i64 == 3); }
		{ int_fast8_t if8 = -1; ok1(test_fmt("foo", "%s%wf8n", "foo", &if8) && if8 == 3); }
		{ int_fast16_t if16 = -1; ok1(test_fmt("foo", "%s%wf16n", "foo", &if16) && if16 == 3); }
		{ int_fast32_t if32 = -1; ok1(test_fmt("foo", "%s%wf32n", "foo", &if32) && if32 == 3); }
		{ int_fast64_t if64 = -1; ok1(test_fmt("foo", "%s%wf64n", "foo", &if64) && if64 == 3); }
	} skip_end;
}
END_TEST

DECLARE_TEST("cstd:print", percent_n);

/* %s */
START_TEST(percent_s)
{
	plan_tests(24);
	ok1(snprintf(NULL, 0, "%s", "foo") == 3);
	ok1(snprintf(NULL, 0, "%.2s", "foo") == 2);
	ok1(snprintf(NULL, 0, "%.*s", 2, "foo") == 2);
	ok1(snprintf(NULL, 0, "%.*s", -2, "foo") == 3);
	ok1(snprintf(NULL, 0, "%.123s", "foo") == 3);
	ok1(snprintf(NULL, 0, "%.*s", 123, "foo") == 3);
	ok1(snprintf(NULL, 0, "%123s", "foo") == 123);
	ok1(snprintf(NULL, 0, "%*s", 123, "foo") == 123);
	ok1(snprintf(NULL, 0, "%123.2s", "foo") == 123);
	ok1(snprintf(NULL, 0, "%123.*s", 2, "foo") == 123);
	ok1(snprintf(NULL, 0, "%*.2s", 123, "foo") == 123);
	ok1(snprintf(NULL, 0, "%*.*s", 123, 2, "foo") == 123);
	/* TODO: make these as thorough as those before */
	ok1(test_fmt("foo", "%s", "foo"));
	ok1(test_fmt("fo", "%.2s", "foo"));
	ok1(test_fmt("  foo", "%5s", "foo"));
	ok1(test_fmt("   fo", "%5.2s", "foo"));
	ok1(test_fmt("  foo", "%5.123s", "foo"));
	ok1(test_fmt("  foo", "%*.*s", 5, 123, "foo"));
	ok1(test_fmt("  foo", "%*.*s", 5, -1, "foo"));
	ok1(test_fmt("   fo", "%*.*s", 5, 2, "foo"));
	ok1(test_fmt("foobar", "%*s", 5, "foobar"));
	trunc_ok(4, "%s", "foobar");
	trunc_ok(4, "%10.4s", "foobar");
	trunc_ok(6, "%10.4s", "foobar");
}
END_TEST

DECLARE_TEST("cstd:print", percent_s);

/* %d */
START_TEST(percent_d)
{
	plan_tests(50);
	ok1(test_fmt("0", "%d", 0));
	ok1(test_fmt("1", "%d", 1));
	ok1(test_fmt("-1", "%d", -1));
	ok1(test_fmt("3", "%.d", 3));
	ok1(test_fmt("2147483647", "%d", INT_MAX));
	ok1(test_fmt("-2147483648", "%d", INT_MIN));
	/* TODO: make LP64 clean */
	ok1(test_fmt("2147483647", "%ld", LONG_MAX));
	ok1(test_fmt("-2147483648", "%ld", LONG_MIN));
	ok1(test_fmt("9223372036854775807", "%lld", LLONG_MAX));
	ok1(test_fmt("-9223372036854775808", "%lld", LLONG_MIN));
	ok1(test_fmt("4294967295", "%u", UINT_MAX));
	ok1(test_fmt("4294967295", "%lu", ULONG_MAX));
	ok1(test_fmt("18446744073709551615", "%llu", ULLONG_MAX));
	ok1(test_fmt("  666", "%5d", 666));
	ok1(test_fmt(" -666", "%5d", -666));
	ok1(test_fmt("  666", "%*d", 5, 666));
	ok1(test_fmt("00666", "%05d", 666));
	ok1(test_fmt("00666", "%0*d", 5, 666));
	ok1(test_fmt("666  ", "%-5d", 666));
	ok1(test_fmt("-666 ", "%-5d", -666));
	ok1(test_fmt("666  ", "%0-5d", 666));
	ok1(test_fmt("666  ", "%-05d", 666));
	ok1(test_fmt("666  ", "%-*d", -5, 666));
	ok1(test_fmt("666  ", "%*d", -5, 666));
	ok1(test_fmt("+666", "%+d", 666));
	ok1(test_fmt("-666", "%+d", -666));
	ok1(test_fmt("666", "%+u", 666));
	ok1(test_fmt("666", "% u", 666));
	ok1(test_fmt(" 666", "% d", 666));
	ok1(test_fmt("-666", "% d", -666));
	ok1(test_fmt("666", "%.2d", 666));
	ok1(test_fmt("00666", "%.5d", 666));
	ok1(test_fmt("00666", "%.*d", 5, 666));
	ok1(test_fmt("-00666", "%.5d", -666));
	skip_start(test_fmt("1000000", "%'d", 1000000), 4, "host does not support SuS grouping modifier") {
		ok1(test_fmt("1_000_000_000", "%'d", 1000000000));
		ok1(test_fmt("2_147_483_647", "%'d", INT_MAX));
		ok1(test_fmt("-2_147_483_648", "%'d", INT_MIN));
		ok1(test_fmt("18_446_744_073_709_551_615", "%ll'u", ULLONG_MAX));
	} skip_end;
	skip_start(!has_bitwidth(), 12, "no support for C23 bitwidth modifiers") {
		ok1(test_fmt("-128", "%w8d", (int8_t)INT8_MIN));
		ok1(test_fmt("127", "%w8d", (int8_t)INT8_MAX));
		ok1(test_fmt("255", "%w8u", (uint8_t)UINT8_MAX));
		ok1(test_fmt("-32768", "%w16d", (int16_t)INT16_MIN));
		ok1(test_fmt("32767", "%w16d", (int16_t)INT16_MAX));
		ok1(test_fmt("65535", "%w16u", (uint16_t)UINT16_MAX));
		ok1(test_fmt("-2147483648", "%w32d", (int32_t)INT32_MIN));
		ok1(test_fmt("2147483647", "%w32d", (int32_t)INT32_MAX));
		ok1(test_fmt("4294967295", "%w32u", (uint32_t)UINT32_MAX));
		ok1(test_fmt("-9223372036854775808", "%w64d", (int64_t)INT64_MIN));
		ok1(test_fmt("9223372036854775807", "%w64d", (int64_t)INT64_MAX));
		ok1(test_fmt("18446744073709551615", "%w64u", (uint64_t)UINT64_MAX));
	} skip_end;
}
END_TEST

DECLARE_TEST("cstd:print", percent_d);

/* truncation of integer conversions */
START_TEST(truncated_integer)
{
	plan_tests(8);
	trunc_ok(2, "%d", 1234);	/* mid-conversion */
	trunc_ok(2, "%6d", 12);		/* mid-pad */
	trunc_ok(4, "%6d", 1234);	/* after pad, within main */
	trunc_ok(2, "%.6d", 1234);	/* mid-prefix */
	trunc_ok(4, "%.6d", 1234);	/* after prefix, within main */
	trunc_ok(2, "%9.6d", 123);	/* mid-pad, before precision */
	trunc_ok(5, "%9.6d", 123);	/* after pad, within precision, before main */
	trunc_ok(7, "%9.6d", 123);	/* after pad, after precision, within main */
}
END_TEST

DECLARE_TEST("cstd:print", truncated_integer);

/* non-decimal unsigned conversions */
START_TEST(nondecimal_u)
{
	plan_tests(37);
	ok1(test_fmt("0", "%o", 0));
	ok1(test_fmt("777", "%o", 0777));
	ok1(test_fmt("0777", "%#o", 0777));
	ok1(test_fmt("0077", "%#04o", 077));
	ok1(test_fmt("077", "%#o", 077));
	ok1(test_fmt("0077", "%#.4o", 077));
	ok1(test_fmt("0777", "%#.2o", 0777));
	ok1(test_fmt("  0077", "%#6.4o", 077));
	ok1(test_fmt(" 01777", "%#6.4o", 01777));
	ok1(test_fmt("0077  ", "%-#6.4o", 077));
	ok1(test_fmt("01777 ", "%-#6.4o", 01777));
	ok1(test_fmt("0", "%#o", 0));
	ok1(test_fmt("1", "% o", 1));
	ok1(test_fmt("1", "%+o", 1));
	ok1(test_fmt("0", "%x", 0));
	ok1(test_fmt("ffff", "%x", 0xffff));
	ok1(test_fmt("0xffff", "%#x", 0xffff));
	ok1(test_fmt("0XFFFF", "%#X", 0xffff));
	ok1(test_fmt("0x0000ffff", "%#010x", 0xffff));
	ok1(test_fmt("    0xffff", "%#10x", 0xffff));
	ok1(test_fmt("0xffff    ", "%-#10x", 0xffff));
	ok1(test_fmt("0xffff    ", "%-#010x", 0xffff));
	ok1(test_fmt("0x0000ffff", "%#.8x", 0xffff));
	ok1(test_fmt("0xffff", "%#.3x", 0xffff));
	ok1(test_fmt("  0x0000ffff", "%#12.8x", 0xffff));
	ok1(test_fmt("  0x0000ffff", "%#*.*x", 12, 8, 0xffff));
	ok1(test_fmt("0", "%#x", 0));
	ok1(test_fmt("1", "% x", 1));
	ok1(test_fmt("1", "%+x", 1));
	skip_start(test_fmt("%b", "%b", 0), 8, "no support for C23 %%b conversion") {
		ok1(test_fmt("0", "%b", 0));
		ok1(test_fmt("10010110", "%b", 0x96));
		ok1(test_fmt("0b1101001", "%#b", 0x69));
		ok1(test_fmt("0b01101001", "%#010b", 0x69));
		ok1(test_fmt("0b01101001", "%#.8b", 0x69));
		ok1(test_fmt("0", "%#b", 0));
		ok1(test_fmt("10", "% b", 2));
		ok1(test_fmt("10", "%+b", 2));
	} skip_end;
}
END_TEST

DECLARE_TEST("cstd:print", nondecimal_u);
