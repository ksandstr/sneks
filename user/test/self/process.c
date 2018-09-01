
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>

#include <sneks/test.h>


START_TEST(getpid)
{
	plan_tests(1);
	ok1(getpid() > 0);
}
END_TEST


DECLARE_TEST("self:getpid", getpid);
