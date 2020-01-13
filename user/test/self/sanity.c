
/* selftests on <sneks/sanity.h> gadgets. */

#include <stdint.h>
#include <sneks/sanity.h>
#include <sneks/test.h>


START_TEST(valid_addr_size)
{
	plan(NO_PLAN);
	const uintptr_t lim = ~(uintptr_t)0;

	/* mainly just the edge cases. however many there are. */
	ok1(VALID_ADDR_SIZE(lim, 0));
	ok1(!VALID_ADDR_SIZE(lim, 1));
	ok1(VALID_ADDR_SIZE(0, lim));
	ok1(!VALID_ADDR_SIZE(1, lim));
	ok1(VALID_ADDR_SIZE(0x10000, lim - 0x10000));
	ok1(!VALID_ADDR_SIZE(0x10000, lim - 0x10000 + 1));
	ok1(!VALID_ADDR_SIZE(0x800000, -65536));

	done_testing();
}
END_TEST

DECLARE_TEST("self:sanity", valid_addr_size);
