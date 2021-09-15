/* tests on the getauxval() non-standard interface. */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/auxv.h>
#include <ccan/pipecmd/pipecmd.h>
#include <ccan/str/str.h>

#include <sneks/test.h>


#define AT_MAX 100


static bool auxp(int name) {
	errno = 0;
	return (getauxval(name), errno != ENOENT);
}


START_TEST(getauxval_basic)
{
	plan_tests(3);

	/* a definitely missing key should return 0 and set ENOENT. */
	errno = 0;
	ok1(getauxval(696969) == 0 && errno == ENOENT);

	/* there should be at least one key in the auxiliary vector, at all. */
	int valid_key = -1;
	for(int i=0; i < AT_MAX; i++) {
		errno = 0;
		if(getauxval(i) != 0 || errno != ENOENT) {
			valid_key = i;
			break;
		}
	}
	/* that's not an error however, it just makes the test weaker. */
	skip_start(valid_key < 0, 2, "no valid keys to getauxval()") {
		/* AT_PAGESZ should be what sysconf returns, or missing. */
		unsigned long v;
		errno = 0;
		imply_ok1((v = getauxval(AT_PAGESZ), errno != ENOENT),
			v == sysconf(_SC_PAGE_SIZE));

		/* if present, AT_EXECFN should be a zero-terminated string; in the
		 * userspace testsuite's case it should be shorter than 100 bytes.
		 */
		errno = 0;
		imply_ok1((v = getauxval(AT_EXECFN), errno != ENOENT),
			(char *)v != NULL && strnlen((char *)v, 100) < 100);
	} skip_end;
}
END_TEST

DECLARE_TEST("process:auxv", getauxval_basic);


/* semantics tests on the uid/gid values, and AT_SECURE. */
START_TEST(uids)
{
	plan_tests(4);

	/* the uid/euid/gid/egid values should be present or absent as a group. */
	iff_ok1(auxp(AT_UID), auxp(AT_GID) && auxp(AT_EUID) && auxp(AT_EGID));
	iff_ok1(!auxp(AT_UID), !auxp(AT_GID) && !auxp(AT_EUID) && !auxp(AT_EGID));

	bool ids_present = auxp(AT_UID) && auxp(AT_GID) && auxp(AT_EUID) && auxp(AT_EGID);
	skip_start(!ids_present, 2, "IDs not present") {
		/* AT_SECURE should be set when uid != euid ∨ gid != egid. */
		imply_ok1(getauxval(AT_UID) != getauxval(AT_EUID)
				|| getauxval(AT_GID) != getauxval(AT_EGID),
			getauxval(AT_SECURE));
		/* if AT_SECURE is present and not set, then the real ID should equal
		 * its effective counterpart.
		 */
		imply_ok1(auxp(AT_SECURE) && !getauxval(AT_SECURE),
			getauxval(AT_UID) == getauxval(AT_EUID) && getauxval(AT_GID) == getauxval(AT_EGID));
	} skip_end;
}
END_TEST

DECLARE_TEST("process:auxv", uids);


/* tests whether AT_RANDOM varies between child processes. */
START_TEST(random)
{
	if(!auxp(AT_RANDOM)) {
		plan_skip_all("AT_RANDOM not present");
		return;
	}

	plan_tests(5);

	const uint8_t *rndptr = (void *)getauxval(AT_RANDOM);
	ok1(rndptr != NULL);
	int child = fork_subtest_start("inherited thru fork") {
		plan(2);
		const uint8_t *child_rndptr = (void *)getauxval(AT_RANDOM);
		iff_ok1(child_rndptr != NULL, rndptr != NULL);
		imply_ok1(child_rndptr != NULL && rndptr != NULL,
			memcmp(child_rndptr, rndptr, 16) == 0);
	} fork_subtest_end;
	fork_subtest_ok1(child);

	/* it should be generated again by exec. start a helper that prints its
	 * value out, capture that, and compare.
	 */
	int cfd = -1;
	pid_t sub = pipecmd(NULL, &cfd, &cfd,
		TESTDIR "/user/test/tools/auxv_random_printer", NULL);
	skip_start(sub < 0, 3, "errno=%d", errno) {
		FILE *input = fdopen(cfd, "rb");
		fail_if(input == NULL);
		uint8_t cvals[18];
		char buf[20];
		int n = 0;
		while(n < 16 && fgets(buf, sizeof buf, input) != NULL) {
			long c = a64l(buf);
			cvals[n++] = c & 0xff;
			cvals[n++] = (c >> 8) & 0xff;
			cvals[n++] = (c >> 16) & 0xff;
		}
		fclose(input);

		imply_ok1(rndptr != NULL, n >= 16 && memcmp(rndptr, cvals, 16) != 0);

		int st, dead = waitpid(sub, &st, 0);
		ok(dead == sub, "waited on sub");
		ok(WIFEXITED(st), "sub exited normally");
	} skip_end;
}
END_TEST

DECLARE_TEST("process:auxv", random);


/* test that AT_EXECFN is always the terminal part of any exec'd path. */
START_TEST(execfn)
{
	plan_tests(5);

	const char *pathspec = TESTDIR "/user/test/tools/auxv_string_printer";
	char execfn_str[16];
	snprintf(execfn_str, sizeof execfn_str, "%d", AT_EXECFN);
	int cfd = -1;
	pid_t sub = pipecmd(NULL, &cfd, &cfd, pathspec, execfn_str, NULL);
	skip_start(!ok(sub > 0, "sub started"), 4, "errno=%d", errno) {
		FILE *input = fdopen(cfd, "rb");
		fail_if(input == NULL);
		char buf[200];
		if(fgets(buf, sizeof buf, input) == NULL) buf[0] = '\0';
		else while(buf[strlen(buf) - 1] == '\n') buf[strlen(buf) - 1] = '\0';
		fclose(input);

		int st, dead = waitpid(sub, &st, 0);
		ok(dead == sub, "waited on sub");
		ok(WIFEXITED(st), "sub exited normally");
		ok(WEXITSTATUS(st) == 0, "auxv %s was found", execfn_str);
		ok1(strends(buf, strrchr(pathspec, '/') + 1));
	} skip_end;
}
END_TEST

DECLARE_TEST("process:auxv", execfn);
