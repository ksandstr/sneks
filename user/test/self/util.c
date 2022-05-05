
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


#ifdef __sneks__
#include <sneks/crtprivate.h>


static void set_int_fn(void *ptr) {
	*(int *)ptr = 1;
}


START_TEST(crt_thread_basic)
{
	plan_tests(2);

	int *value = malloc(sizeof *value);
	*value = 0;

	L4_ThreadId_t tid;
	int n = __crt_thread_create(&tid, &set_int_fn, value);
	if(!ok(n == 0, "__crt_thread_create")) diag("n=%d", n);
	diag("tid=%lu:%lu", L4_ThreadNo(tid), L4_Version(tid));

	__crt_thread_join(tid);
	ok(*value, "value was altered");

	free(value);
}
END_TEST

DECLARE_TEST("self:crt", crt_thread_basic);

#endif
