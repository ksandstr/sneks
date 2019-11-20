
/* tests of various utility things provided in the sneks and mung trees. */

#include <sneks/bitops.h>
#include <sneks/test.h>


START_TEST(msb_bitness)
{
	plan_tests(3);

	ok1(MSB(1) == 0);
	ok1(MSB(1 << 15) == 15);
	ok1(MSB(1 << 31) == 31);
}
END_TEST

DECLARE_TEST("self:util", msb_bitness);


START_TEST(msbll_bitness)
{
	plan_tests(5);

	ok1(MSBLL(1) == 0);
	ok1(MSBLL(1 << 15) == 15);
	ok1(MSBLL(1ull << 31) == 31);
	ok1(MSBLL(1ull << 32) == 32);
	ok1(MSBLL(1ull << 63) == 63);
}
END_TEST

DECLARE_TEST("self:util", msbll_bitness);
