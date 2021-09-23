/* tests on the process environment. that's distinct from environment
 * variables, though they are included.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <ccan/str/str.h>
#include <ccan/pipecmd/pipecmd.h>

#include <sneks/test.h>
#ifdef __sneks__
#include <sneks/crtprivate.h>
#endif


#ifdef __sneks__

struct sp_out {
	size_t a_foo, a_start, botsize;
};


/* closes @outfd. */
static int get_prober_output(struct sp_out *out, int outfd)
{
	*out = (struct sp_out){ };
	FILE *input = fdopen(outfd, "rb");
	if(input == NULL) return -errno;
	char buf[100];
	errno = 0;
	while(fgets(buf, sizeof buf, input) != NULL) {
		int len = strlen(buf);
		if(len > 0 && buf[len - 1] == '\n') buf[--len] = '\0';
		if(len == 0) continue;
		char *sep = strchr(buf, '=');
		if(sep != NULL) *(sep++) = '\0';
		if(sep != NULL && streq(buf, "&foo")) {
			out->a_foo = strtoul(sep, NULL, 0);
		} else if(sep != NULL && streq(buf, "&_start")) {
			out->a_start = strtoul(sep, NULL, 0);
		} else if(sep != NULL && streq(buf, "botsize")) {
			out->botsize = strtoul(sep, NULL, 0);
		} else {
			diag("unrecognized stanza `%s'", buf);
		}
	}
	fclose(input);

	return -errno;
}


/* test that a RLIMIT_STACK larger than fits under a collaborator's process
 * image will be allocated above it, and a smaller will be allocated beneath.
 *
 * variables:
 *   - [rlimit_large] stack size: 128k or 8192k
 */
START_LOOP_TEST(stack_location, iter, 0, 1)
{
	const bool rlimit_large = !!(iter & 1);
	diag("rlimit_large=%s", btos(rlimit_large));

	plan_tests(7);

	const int stksize = (rlimit_large ? 8192 : 128) * 1024;
	diag("stksize=%#x", stksize);
	int n = setrlimit(RLIMIT_STACK,
		&(struct rlimit){ .rlim_cur = stksize, .rlim_max = stksize });
	if(!ok(n == 0, "setrlimit")) diag("errno=%d", errno);

	int cfd = -1;
	pid_t child = pipecmd(NULL, &cfd, &cfd, TESTDIR "/user/test/tools/stack_prober", NULL);
	skip_start(!ok(child > 0, "pipecmd"), 5, "no subordinate (errno=%d)", errno) {
		struct sp_out sp;
		int n = get_prober_output(&sp, cfd);
		fail_if(n != 0, "get_prober_output() failed, n=%d", n);

		size_t a_foo = sp.a_foo, a_start = sp.a_start, botsize = sp.botsize;
		diag("&_foo=%#lx", (unsigned long)a_foo);
		diag("&_start=%#lx", (unsigned long)a_start);
		diag("botsize=%#lx", (unsigned long)botsize);
		skip_start(!ok(a_foo > 0 && a_start > 0 && botsize > 0, "have values"), 2, "no values") {
			imply_ok1(botsize < stksize, a_foo > a_start);
			imply_ok1(a_foo < a_start, botsize >= stksize);
		} skip_end;

		int st, dead = waitpid(child, &st, 0);
		ok(dead == child, "waitpid");
		ok(WIFEXITED(st), "collaborator exited normally");
	} skip_end;
}
END_TEST

DECLARE_TEST("process:env", stack_location);


/* searches for the forbidden low zone by repeatedly starting a program to
 * access a byte, then observing whether it crashes.
 */
START_TEST(forbidden_zone)
{
	const char *prober = TESTDIR "/user/test/tools/stack_prober";
	plan_tests(6);

	int n = setrlimit(RLIMIT_STACK, &(struct rlimit){ 256 * 1024, 256 * 1024 });
	if(!ok(n == 0, "setrlimit(stack, 256k)")) diag("errno=%d", errno);

	size_t high = 0;
	{
		subtest_start("get stack high address");
		plan(3);
		int cfd = -1;
		pid_t child = pipecmd(NULL, &cfd, &cfd, prober, NULL);
		skip_start(!ok(child > 0, "pipecmd"), 2, "no subordinate (errno=%d)", errno) {
			struct sp_out sp;
			int n = get_prober_output(&sp, cfd);
			fail_if(n != 0, "get_prober_output() failed, n=%d", n);

			high = sp.a_foo;

			int st, dead = waitpid(child, &st, 0);
			ok(dead == child, "waitpid");
			ok(WIFEXITED(st), "collaborator exited normally");
		} skip_end;
		subtest_end();
	}

	const size_t page_size = sysconf(_SC_PAGE_SIZE);
	high = (high + page_size - 1) & ~(page_size - 1);
	size_t low = 0;
	bool crash_signaled = true, sig_segv = true;
	while(high > low + page_size) {
		size_t mid = (high + low) / 2;
		//diag("probing mid=%#x", (unsigned)mid);
		int cfd = -1;
		char address[40];
		snprintf(address, sizeof address, "%#x", (unsigned)mid);
		pid_t child = pipecmd(NULL, &cfd, &cfd, prober, address, NULL);
		fail_if(child <= 0, "pipecmd failed, errno=%d", errno);
		struct sp_out sp;
		int n = get_prober_output(&sp, cfd);
		fail_if(n != 0, "get_prober_output() failed, n=%d", n);

		int st, dead = waitpid(child, &st, 0);
		fail_unless(dead == child, "dead=%d, errno=%d", dead, errno);
		if(WIFEXITED(st)) {
			high = mid;
		} else {
			if(!WIFSIGNALED(st)) {
				diag("crash wasn't signaled");
				crash_signaled = false;
			} else if(WTERMSIG(st) != SIGSEGV) {
				diag("unexpected termsig=%d", WTERMSIG(st));
				sig_segv = false;
			}
			low = mid;
		}
	}
	ok1(crash_signaled);
	ok1(sig_segv);

	ok1(high != low);
	if(!ok1(high == 0x10000)) diag("high=%#x", (unsigned)high);
}
END_TEST

DECLARE_TEST("process:env", forbidden_zone);
#endif
