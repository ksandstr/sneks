/* tests on the execve(2) family. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ccan/str/str.h>
#include <ccan/talloc/talloc.h>
#include <ccan/array_size/array_size.h>
#include <ccan/darray/darray.h>
#include <ccan/pipecmd/pipecmd.h>
#include <sneks/test.h>

/* TODO: this could be in an utility module, right? */
static int ord_str(const void *a, const void *b) {
	return strcmp(*(const char **)a, *(const char **)b);
}

/* foundation: that an assistant program's exit status can be examined. */
START_LOOP_TEST(exit_status, iter, 0, 2)
{
	const char *prgname;
	switch(iter) {
		case 0: prgname = "exit_with_0"; break;
		case 1: prgname = "exit_with_1"; break;
		case 2: prgname = "exit_with_getpid"; break;
		default: abort();
	}
	diag("prgname=`%s'", prgname);
	plan_tests(6);

	ok(chdir(TESTDIR) == 0, "chdir(TESTDIR)");

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
		default: abort();
	}
	diag("prgname=`%s'", prgname);
	plan_tests(4);

	ok(chdir(TESTDIR) == 0, "chdir(TESTDIR)");

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
	const bool use_vector = iter & 1;
	diag("use_vector=%s", btos(use_vector));
	plan_tests(13);

	ok(chdir(TESTDIR) == 0, "chdir(TESTDIR)");

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

	char *prgname = "user/test/tools/argdumper", *const env[] = { "FOO=bar", "PATH=/bin:/sbin:/usr/bin:/usr/sbin", "howl=gargl=gargl", NULL };
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

/* test that the current directory is inherited by the child image. */
static const char *cwd_dirs[] = { "", "/user/test", "/user/test/io/dir" };

START_LOOP_TEST(cwd, iter, 0, ARRAY_SIZE(cwd_dirs) - 1)
{
	const char *target = cwd_dirs[iter];
	diag("target=`%s'", target);
	plan_tests(9);

	TALLOC_CTX *tal = talloc_new(NULL);
	char *actual = talloc_asprintf(tal, "%s/%s", TESTDIR, target);
	ok1(chdir(actual) == 0);

	int pfd[2], n = pipe(pfd);
	assert(n == 0);

	pid_t child = fork();
	if(child == 0) {
		close(pfd[0]);
		dup2(pfd[1], STDOUT_FILENO);
		dup2(pfd[1], STDERR_FILENO);
		close(pfd[1]);
		execl(TESTDIR "/user/test/tools/cwdlister", "cwdlister", (char *)NULL);
		exit(errno | 0x80);
	}
	close(pfd[1]);
	diag("child=%d", child);
	int st, dead = waitpid(child, &st, 0), err = errno;
	skip_start(!ok(dead == child, "waitpid"), 2, "errno=%d", err) {
		ok1(WIFEXITED(st));
		ok1(WEXITSTATUS(st) == EXIT_SUCCESS);
	} skip_end;

	FILE *input = fdopen(pfd[0], "r");
	assert(input != NULL);
	bool got_input = false, malformed = false;
	char line[200];
	darray(char *) ents = darray_new();
	while(fgets(line, sizeof line, input) != NULL) {
		int len = strlen(line);
		while(len > 0 && line[len - 1] == '\n') line[--len] = '\0';
		if(len > 0) got_input = true;
		if(!strstarts(line, "ENT ")) {
			diag("malformed line `%s'", line);
			malformed = true;
			continue;
		}
		darray_push(ents, talloc_strdup(tal, line + 4));
	}
	fclose(input);

	/* repeat locally */
	darray(char *) local = darray_new();
	DIR *d = opendir(".");
	struct dirent *ent;
	while(errno = 0, ent = readdir(d), ent != NULL) {
		darray_push(local, talloc_strdup(tal, ent->d_name));
	}
	ok(errno == 0, "readdir");

	/* examine teh entrails */
	ok1(got_input);
	ok1(!malformed);
	skip_start(!ok1(local.size == ents.size), 1, "unequal results") {
		qsort(ents.item, ents.size, sizeof ents.item[0], &ord_str);
		qsort(local.item, local.size, sizeof local.item[0], &ord_str);
		bool all_same = true;
		for(size_t i=0; i < ents.size; i++) {
			if(!streq(ents.item[i], local.item[i])) {
				diag("item %u differs; local=`%s', other=`%s'",
					(unsigned)i, local.item[i], ents.item[i]);
				all_same = false;
			}
		}
		ok1(all_same);	/* arr rook same */
	} skip_end;

	darray_free(ents); darray_free(local);
	talloc_free(tal);
}
END_TEST

DECLARE_TEST("process:exec", cwd);

/* test that the program name is passed to a child process as it should be:
 * program_invocation_name being the full directory (where applicable), the
 * short_name being the terminal part of previous, and argv[0] being one of
 * the two or both when they're the same.
 *
 * variables:
 *   - [full_path] whether the collaborator program isn't specified with a
 *     relative path.
 */
START_LOOP_TEST(program_name, iter, 0, 1)
{
	const bool full_path = iter & 1;
	diag("full_path=%s", btos(full_path));
	void *tal = talloc_new(NULL);

	plan_tests(9);

	const char *dir = TESTDIR "/user/test/tools", *prg = "program_name_printer";
	if(full_path) {
		prg = talloc_asprintf(tal, "%s/%s", dir, prg);
		dir = "/";
	} else {
		setenv("PATH", ".", 1);
	}
	diag("dir=`%s', prg=`%s'", dir, prg);
	if(!ok(chdir(dir) == 0, "chdir")) diag("errno=%d", errno);

	int cfd = -1;
	pid_t child = pipecmd(NULL, &cfd, &cfd, prg, NULL);
	skip_start(!ok(child > 0, "pipecmd"), 7, "no child (errno=%d)", errno) {
		FILE *input = fdopen(cfd, "rb");
		fail_if(input == NULL);
		char *long_name = NULL, *short_name = NULL, *argv_0 = NULL, buf[400];
		while(fgets(buf, sizeof buf, input) != NULL) {
			int len = strlen(buf);
			if(len > 0 && buf[len - 1] == '\n') buf[--len] = '\0';
			if(len == 0) continue;
			char *sep = strchr(buf, '=');
			if(sep != NULL) *(sep++) = '\0';
			if(sep != NULL && streq(buf, "argv[0]")) {
				argv_0 = talloc_strdup(tal, sep);
			} else if(sep != NULL && streq(buf, "program_invocation_name")) {
				long_name = talloc_strdup(tal, sep);
			} else if(sep != NULL && streq(buf, "program_invocation_short_name")) {
				short_name = talloc_strdup(tal, sep);
			} else {
				diag("unrecognized output line `%s'", buf);
			}
		}
		fclose(input);

		diag("argv_0=%s$", argv_0);
		diag("long_name=%s$", long_name);
		diag("short_name=%s$", short_name);
		skip_start(!ok1(argv_0 != NULL && long_name != NULL && short_name != NULL),
			4, "didn't get all values")
		{
			imply_ok1(streq(long_name, short_name), streq(argv_0, long_name));
			imply_ok1(!streq(long_name, short_name),
				strends(long_name, talloc_asprintf(tal, "/%s", short_name)));
			ok(streq(argv_0, long_name) || streq(argv_0, short_name),
				"argv[0] is same as long or short name");
			ok1(strchr(short_name, '/') == NULL);
		} skip_end;

		int st, dead = waitpid(child, &st, 0);
		ok(dead == child, "waitpid");
		ok(WIFEXITED(st), "collaborator exited");
	} skip_end;
	talloc_free(tal);
}
END_TEST

DECLARE_TEST("process:exec", program_name);
