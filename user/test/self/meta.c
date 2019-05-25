
#include <sys/types.h>
#include <sys/wait.h>
#include <sneks/test.h>


START_TEST(simple_fork_subtest)
{
	plan_tests(1);

	int child = fork_subtest_start("subtest") {
		plan_tests(1);
		ok1(true);
	} fork_subtest_end;
	fork_subtest_ok1(child);
}
END_TEST

DECLARE_TEST("self:subtest", simple_fork_subtest);


START_LOOP_TEST(nested_fork_subtests, iter, 0, 1)
{
	const bool inner_ok = !(iter & 1);
	diag("inner_ok=%s", btos(inner_ok));
	plan_tests(2);

	int outer = fork_subtest_start("outer subtest") {
		plan_tests(1);
		int inner = fork_subtest_start("inner subtest") {
			plan_tests(1);
			ok1(inner_ok);
		} fork_subtest_end;
		fork_subtest_ok1(inner);
	} fork_subtest_end;

	int st = fork_subtest_join(outer);
	ok1(WIFEXITED(st));
	iff_ok1(inner_ok, WEXITSTATUS(st) == 0);
}
END_TEST

DECLARE_TEST("self:subtest", nested_fork_subtests);
