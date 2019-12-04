
#include <stdio.h>
#include <errno.h>
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


START_TEST(lives_dies_ok)
{
	plan_tests(4);

	void *ptr = malloc(16);
	fail_unless(ptr != NULL);
	dies_ok({ free(ptr); free(ptr); }, "free ptr twice dies");
	dies_ok1({ free(ptr); free(ptr); });

	lives_ok({ free(ptr); }, "free ptr once lives");
	lives_ok1({ free(ptr); });
	free(ptr);
}
END_TEST

DECLARE_TEST("self:subtest", lives_dies_ok);


START_TEST(short_form_plan)
{
	plan_tests(5);

	int child = fork_subtest_start("regular plan") {
		plan(1);
		ok1(true);
	} fork_subtest_end;
	fork_subtest_ok1(child);

	child = fork_subtest_start("no-plan (lazy) plan") {
		plan(NO_PLAN);
		ok1(true);
	} fork_subtest_end;
	fork_subtest_ok1(child);

	child = fork_subtest_start("plan omitted") {
		ok1(true);
	} fork_subtest_end;
	fork_subtest_ok1(child);

	child = fork_subtest_start("skip-all plan (terse)") {
		plan(SKIP_ALL);
	} fork_subtest_end;
	fork_subtest_ok1(child);

	child = fork_subtest_start("skip-all plan (verbose)") {
		plan(SKIP_ALL, "nothing here but us chickens%c", '!');
	} fork_subtest_end;
	fork_subtest_ok1(child);
}
END_TEST

DECLARE_TEST("self:syntax", short_form_plan);


static void fuck_stdout(void)
{
	fclose(stdout); stdout = fmemopen(NULL, 64 * 1024, "wb");
	fclose(stderr); stderr = fmemopen(NULL, 64 * 1024, "wb");
}


/* three kinds of bails & their reports.
 *
 * to avoid confusing the reporting script, we redirect child stdout and
 * stderr to the fuck-it bucket. that's fine since the only thing we care
 * about is the exit code.
 *
 * NOTE: the cleaner way would set a "no bail!" flag in the child, and bail()
 * would then output something about how cute little kittens would never bail
 * this test sequence out, and if they did it'd never be recognized as such by
 * the reporting script. then the forking stuff should
 */
START_TEST(bails)
{
	plan_tests(3);

	int child = fork();
	if(child == 0) {
		fuck_stdout();
		bail("can't go on no mo%c", '\'');
		abort();
	}
	int st, dead = waitpid(child, &st, 0);
	if(dead != child) diag("waitpid: dead=%d, errno=%d", dead, errno);
	ok(WIFEXITED(st) && WEXITSTATUS(st) == 255, "bail()'d");

	child = fork();
	if(child == 0) { fuck_stdout(); BAIL_OUT(); abort(); }
	dead = waitpid(child, &st, 0);
	if(dead != child) diag("waitpid: dead=%d, errno=%d", dead, errno);
	ok(WIFEXITED(st) && WEXITSTATUS(st) == 255, "BAIL_OUT()'d");

	child = fork();
	if(child == 0) {
		fuck_stdout();
		BAIL_OUT("woo hoo here we go%c", '!');
		abort();
	}
	dead = waitpid(child, &st, 0);
	if(dead != child) diag("waitpid: dead=%d, errno=%d", dead, errno);
	ok(WIFEXITED(st) && WEXITSTATUS(st) == 255, "BAIL_OUT(text)'d");
}
END_TEST

DECLARE_TEST("self:syntax", bails);
