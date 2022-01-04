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
 *
 * covers handling of the `c' parameter as a byte value despite passing
 * it as `int'.
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
