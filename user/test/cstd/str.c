#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <ccan/str/str.h>
#include <sneks/test.h>

/* a case of known breakage from mung of yore, iterated over various
 * alignments of the first input.
 */
START_LOOP_TEST(strcmp_breakage_TyTiPtva, iter, 0, 7)
{
	const char *teststr = "TyTiPtva";
	char copybuf[32], othbuf[32], *copy = &copybuf[iter];
	strscpy(copy, teststr, sizeof(copybuf) - iter);
	diag("align=%d, copy=%p, teststr=%p", iter, copy, teststr);
	plan_tests(1 + 8);
	ok1(strcmp(copy, teststr) == 0);
	for(int oth_align = 0; oth_align < 8; oth_align++) {
		char *oth = &othbuf[oth_align];
		strscpy(oth, teststr, sizeof(othbuf) - oth_align);
		if(!ok(strcmp(copy, oth) == 0, "copy == oth [align=%d]", oth_align)) diag("oth=%p", oth);
	}
}
END_TEST

DECLARE_TEST("cstd:str", strcmp_breakage_TyTiPtva);

START_TEST(strscpy_basic)
{
	plan_tests(7);
	char small[10];
	ok1(strscpy(small, "very large string", sizeof small) == -E2BIG);
	ok1(small[sizeof small - 1] == '\0');
	ok1(strlen(small) == sizeof small - 1);
	ok1(streq(small, "very larg"));
	char large[64];
	ok1(strscpy(large, "smaller string", sizeof large) == 14);
	ok1(large[14] == '\0');
	ok1(streq(large, "smaller string"));
}
END_TEST

DECLARE_TEST("cstd:str", strscpy_basic);
