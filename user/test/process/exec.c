
/* tests on the execve(2) family. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ccan/str/str.h>
#include <ccan/talloc/talloc.h>
#include <ccan/darray/darray.h>

#include <sneks/test.h>


/* foundation: that an assistant program's exit status can be examined. */
START_LOOP_TEST(exit_status, iter, 0, 2)
{
	const char *prgname;
	switch(iter) {
		case 0: prgname = "exit_with_0"; break;
		case 1: prgname = "exit_with_1"; break;
		case 2: prgname = "exit_with_getpid"; break;
		default: assert(false);
	}
	diag("prgname=`%s'", prgname);
	plan_tests(6);

	ok(chdir(TESTDIR) == 0, "chdir(TESTDIR)");

#ifdef __sneks__
	todo_start("no implementation");
#endif

	int child = fork();
	if(child == 0) {
		char *fullpath = talloc_asprintf(NULL, "user/test/tools/%s", prgname);
		execl(fullpath, prgname, (char *)NULL);
		talloc_free(fullpath);
		int err = errno;
		diag("execl failed, errno=%d", err);
		exit(err ^ (child & 0xff));
	}
	diag("child=%d", child);

	int st, n = waitpid(child, &st, 0);
	skip_start(!ok(n == child, "waitpid"), 4, "errno=%d", errno) {
		ok1(WIFEXITED(st));
		diag("WEXITSTATUS(st)=%d", WEXITSTATUS(st));
		imply_ok1(strends(prgname, "with_0"), WEXITSTATUS(st) == 0);
		imply_ok1(strends(prgname, "with_1"), WEXITSTATUS(st) == 1);
		imply_ok1(strends(prgname, "with_getpid"),
			WEXITSTATUS(st) == (child & 0xff));
	} skip_end;
}
END_TEST

DECLARE_TEST("process:exec", exit_status);


/* kinds of executable, positive: regular programs, scripts run by a regular
 * program, and scripts run by another script.
 */
START_LOOP_TEST(kinds_positive, iter, 0, 2)
{
	const char *prgname;
	switch(iter) {
		case 0: prgname = "tools/exit_with_0"; break;
		case 1: prgname = "scripts/first_order_script"; break;
		case 2: prgname = "scripts/second_order_script"; break;
		default: assert(false);
	}
	diag("prgname=`%s'", prgname);
	plan_tests(4);

	ok(chdir(TESTDIR) == 0, "chdir(TESTDIR)");

#ifdef __sneks__
	todo_start("no implementation");
#endif

	int child = fork();
	if(child == 0) {
		char *fullpath = talloc_asprintf(NULL, "user/test/%s", prgname);
		execl(fullpath, prgname, (char *)NULL);
		talloc_free(fullpath);
		int err = errno;
		diag("execl failed, errno=%d", err);
		exit(err | 0x80);
	}
	diag("child=%d", child);

	int st, n = waitpid(child, &st, 0);
	skip_start(!ok(n == child, "waitpid"), 2, "errno=%d", errno) {
		ok1(WIFEXITED(st));
		ok1(WEXITSTATUS(st) == 0);
	} skip_end;
}
END_TEST

DECLARE_TEST("process:exec", kinds_positive);


/* passing of file descriptors, arguments, and the environment. */
static const char *find_env(char *const envp[], const char *key);

START_LOOP_TEST(fd_arg_env_passing, iter, 0, 1)
{
	const bool use_vector = !!(iter & 1);
	diag("use_vector=%s", btos(use_vector));
	plan_tests(13);

	ok(chdir(TESTDIR) == 0, "chdir(TESTDIR)");

#ifdef __sneks__
	todo_start("no implementation");
#endif

	TALLOC_CTX *tal = talloc_new(NULL);
	int pipe_out[2], n = pipe(pipe_out), err = errno;
	if(!ok(n == 0, "pipe created")) {
		/* TODO: it'd be nice to have a do-while(false) wrapper so tests could
		 * break out, and also a skip_rest() form that chucks the rest of the
		 * plan _or_ runs plan_skip_all() if no plan was declared.
		 */
		skip(10, "errno=%d", err);
		goto end;
	}

	char *prgname = "user/test/tools/argdumper",
		*const env[] = { "FOO=bar", "PATH=/bin:/sbin:/usr/bin:/usr/sbin",
			"howl=gargl=gargl", NULL };

	fflush(stdout);
	int child = fork();
	if(child == 0) {
		/* pres butan */
		close(pipe_out[0]);
		n = dup2(pipe_out[1], STDOUT_FILENO);
		if(n >= 0) n = dup2(pipe_out[1], STDERR_FILENO);
		if(n >= 0) {
			close(pipe_out[1]);
			char *mypid = talloc_asprintf(tal, "%d", getpid());
			if(use_vector) {
				char *const args[] = { prgname, "woop shoop", mypid, NULL };
				execve(prgname, args, env);
			} else {
				execle(prgname, prgname, "woop shoop", mypid, (char *)NULL, env);
			}
		}
		/* can't report no error, man */
		exit(errno | 0x80);
	}
	diag("child=%d", child);

	/* receev bacon */
	close(pipe_out[1]);
	FILE *input = fdopen(pipe_out[0], "r");
	assert(input != NULL);
	darray(char *) args = darray_new(), envs = darray_new();
	bool got_input = false, args_match = true, envs_match = true, nothing_weird = true;
	char line[200];
	while(fgets(line, sizeof line, input) != NULL) {
		int len = strlen(line);
		while(len > 0 && line[len - 1] == '\n') line[--len] = '\0';
		if(len > 0) got_input = true;
		char *kind = line, *num = strchr(line, ' '),
			*rest = num != NULL ? strchr(num + 1, ' ') : NULL;
		if(num != NULL) *(num++) = '\0';
		if(rest != NULL) *(rest++) = '\0';
		diag("kind=`%s', num=`%s', rest=`%s'", kind, num, rest);
		if(streq(kind, "ARG")) {
			if(strtoul(num, NULL, 0) != args.size && args_match) {
				diag("args.size=%d didn't match", (int)args.size);
				args_match = false;
			}
			darray_push(args, talloc_strdup(tal, rest));
		} else if(streq(kind, "ENV")) {
			if(strtoul(num, NULL, 0) != envs.size && envs_match) {
				diag("envs.size=%d didn't match", (int)envs.size);
			}
			darray_push(envs, talloc_strdup(tal, rest));
		} else {
			diag("weird kind=`%s'", kind);
			nothing_weird = false;
		}
	}
	fclose(input);

	int st;
	n = waitpid(child, &st, 0);
	skip_start(!ok(n == child, "waitpid"), 2, "errno=%d", errno) {
		ok1(WIFEXITED(st));
		if(!ok1(WEXITSTATUS(st) == 0)) {
			diag("exitstatus=%d (child errno=%d)", WEXITSTATUS(st),
				WEXITSTATUS(st) & ~0x80);
		}
	} skip_end;

	/* enjoy bacon */
	ok1(got_input);
	ok1(args_match);
	ok1(envs_match);
	ok1(nothing_weird);

	ok1(args.size >= 1 && strends(args.item[0], "/argdumper"));
	ok1(args.size >= 2 && streq(args.item[1], "woop shoop"));
	ok1(args.size >= 3 && strtoul(args.item[2], NULL, 0) == child);

	darray_push(envs, NULL);
	envs_match = true;
	for(int i=0; env[i] != NULL; i++) {
		char *key = talloc_strdup(tal, env[i]),
			*brk = strchr(key, '=');
		assert(brk != NULL);
		*(brk++) = '\0';
		const char *val = find_env(envs.item, key);
		if(val == NULL || !streq(val, brk)) {
			diag("for key=`%s' val=`%s' (expected `%s')",
				key, val, brk);
			envs_match = false;
		}
	}
	ok(envs_match, "env values found");

	darray_free(args);
	darray_free(envs);

end:
	talloc_free(tal);
}
END_TEST

DECLARE_TEST("process:exec", fd_arg_env_passing);


static const char *find_env(char *const envp[], const char *key)
{
	for(int i=0; envp[i] != NULL; i++) {
		char *brk = strchr(envp[i], '=');
		if(brk == NULL) continue;
		char tmp[brk - envp[i] + 1];
		memcpy(tmp, envp[i], brk - envp[i]);
		tmp[sizeof tmp - 1] = '\0';
		if(streq(tmp, key)) return brk + 1;
	}
	return NULL;
}
