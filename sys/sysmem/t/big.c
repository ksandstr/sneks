
/* tests on allocating lots of memory within systest, giving sysmem's memory
 * compression support a bit of a workout.
 */

#include <stdlib.h>
#include <string.h>
#include <ccan/array_size/array_size.h>
#include <sneks/test.h>


#define LARGE_SIZE (4 * 1024 * 1024)
#define HUGE_SIZE (32 * 1024 * 1024)


START_TEST(plain_alloc)
{
	plan_tests(2);

	void *ptr = malloc(LARGE_SIZE);
	ok(ptr != NULL, "alloc of %u bytes", LARGE_SIZE);
	free(ptr);

	ptr = malloc(HUGE_SIZE);
	ok(ptr != NULL, "alloc of %u bytes", HUGE_SIZE);
	free(ptr);
}
END_TEST


START_TEST(fill_constant)
{
	static unsigned scales[] = { LARGE_SIZE, HUGE_SIZE };
	plan_tests(ARRAY_SIZE(scales));

	for(int i=0; i < ARRAY_SIZE(scales); i++) {
		void *ptr = malloc(scales[i]);
		memset(ptr, 1, scales[i]);
		pass("running after writing %u bytes", scales[i]);
		free(ptr);
	}
}
END_TEST


SYSTEST("mem:big", plain_alloc);
SYSTEST("mem:big", fill_constant);
/* TODO: add one that stores a repeating sequence of 0..255 */
