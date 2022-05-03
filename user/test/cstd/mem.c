/* some quick and dirty tests of memchr(3) and the like. some of these should
 * be redundant with tests from mung, since lib/string.c is copypasta'd
 * between the two projects and only occasionally put back in sync.
 *
 * TODO:
 *   - test about finding the first `c' when there are more than one.
 *   - variations for:
 *     - other instances after, at, and before `s'.
 *     - `s' at 0 thru 7 bytes' alignment.
 *     - same for the correct instance.
 */
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>
#include <ccan/array_size/array_size.h>
#include <sneks/test.h>

/* basic positive and negative operation.
 * this is intended to cover handling of the `c' parameter as a byte value
 * despite passing it as `int', but it might not. it may further be redundant
 * with some part of the memchr_positive/_negative tests.
 */
START_TEST(memchr_basic)
{
	plan_tests(2);

	unsigned char allbytes[UCHAR_MAX + 1];
	diag("allbytes=%p", allbytes);
	for(unsigned i=0; i <= 0xff; i++) allbytes[i] = i;

	bool bytes_found = true;
	for(int i=0; i <= UCHAR_MAX; i++) {
		void *p = memchr(allbytes, i, UCHAR_MAX + 1);
		if(p != &allbytes[i]) {
			diag("for i=%d, p=%p (should be %p)", i, p, &allbytes[i]);
			bytes_found = false;
		}
	}
	ok1(bytes_found);

	bool not_found = true;
	for(int i=0; i <= UCHAR_MAX; i++) {
		if(i == 0) allbytes[i]++; else allbytes[i]--;
		void *p = memchr(allbytes, i, UCHAR_MAX + 1);
		if(p != NULL) {
			diag("for i=%d, p=%p (should be nil)", i, p);
			not_found = false;
		}
		if(i != 0) allbytes[i]++; else allbytes[i]--;
	}
	ok1(not_found);
}
END_TEST

DECLARE_TEST("cstd:mem", memchr_basic);

/* positive match at various positions.
 *
 * iter variables:
 *   - starting offset within a page-aligned buffer
 *   - high bit of test character
 *   - swapping test and fill character ('\0')
 */
START_LOOP_TEST(memchr_positive, iter, 0, 15)
{
	static const int pos[] = { 0, 1, 2, 256, 777, 1021, 1022, 1023 };
	const int start_offset = iter & 3,
		actual_char = ~iter & 4 ? 'w' : 0xf7,
		test_char = ~iter & 8 ? '\0' : actual_char,
		fill_char = iter & 8 ? '\0' : actual_char;
	diag("start_offset=%d, test_char=%#02x, fill_char=%#02x", start_offset, test_char, fill_char);
	plan_tests(ARRAY_SIZE(pos));
	unsigned char *raw = aligned_alloc(1 << 12, 8 * 1024), *mem = raw + start_offset;
	for(int i=0; i < ARRAY_SIZE(pos); i++) {
		subtest_start("pos[%d]=%d, mem=%p", i, pos[i], mem);
			plan(6);
			memset(raw, fill_char, 2 * 1024);
			ok(memchr(mem, test_char, 1024) == NULL, "not found before set");
			mem[pos[i]] = test_char; /* set one */
			void *ret = memchr(mem, test_char, 1024);
			if(!ok(ret == mem + pos[i], "found after one set")) diag("ret=%p", ret);
			imply_ok1(ret != NULL, *(unsigned char *)ret == test_char);
			mem[pos[i] + 1024] = test_char; /* set another, see if it confuses */
			ret = memchr(mem, test_char, 1024);
			if(!ok(ret == mem + pos[i], "found after two set")) diag("ret=%p", ret);
			imply_ok1(ret != NULL, *(unsigned char *)ret == test_char);
			mem[pos[i]] = fill_char; /* unset first */
			if(!ok((ret = memchr(mem, test_char, 1024), ret == NULL), "not found after first reset")) diag("ret=%p", ret);
		subtest_end();
	}
	free(raw);
}
END_TEST

DECLARE_TEST("cstd:mem", memchr_positive);

START_LOOP_TEST(memchr_negative, iter, 0, 15)
{
	const int start_offset = iter & 3,
		actual_char = ~iter & 4 ? 'w' : 0xf7,
		test_char = ~iter & 8 ? '\0' : actual_char,
		fill_char = iter & 8 ? '\0' : actual_char;
	diag("start_offset=%d, actual_char=%#02x, test_char=%#02x, fill_char=%#02x", start_offset, actual_char, test_char, fill_char);
	plan_tests(4);
	unsigned char *raw = aligned_alloc(1 << 12, 8 * 1024), *mem = raw + 4096 + start_offset;
	diag("raw=%p, mem=%p", raw, mem);
	memset(raw, fill_char, 8 * 1024);
	ok(memchr(mem, test_char, 512) == NULL, "null when not set");
	mem[-1] = test_char;
	void *ret = memchr(mem, test_char, 512);
	if(!ok(ret == NULL, "null when set before")) diag("ret=%p off=%d (%#02x)", ret, (int)(ret - (void *)mem), *(unsigned char *)ret);
	mem[-1] = fill_char; mem[512] = test_char;
	ret = memchr(mem, test_char, 512);
	if(!ok(ret == NULL, "null when set after")) diag("ret=%p off=%d (%#02x)", ret, (int)(ret - (void *)mem), *(unsigned char *)ret);
	mem[-1] = test_char;
	ret = memchr(mem, test_char, 512);
	if(!ok(ret == NULL, "null when both set")) diag("ret=%p off=%d (%#02x)", ret, (int)(ret - (void *)mem), *(unsigned char *)ret);
	free(raw);
}
END_TEST

DECLARE_TEST("cstd:mem", memchr_negative);
