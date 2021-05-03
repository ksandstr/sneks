
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <ccan/str/str.h>
#include <ccan/array_size/array_size.h>
#include <ccan/darray/darray.h>

#include <sneks/test.h>


/* test exec*p*() search thru parts of $PATH.
 *
 * variables:
 *   - [terminal] which terminal case $PATH has (including none)
 *   - [skippable] whether $PATH doesn't have skippable cases, or which
 *     permutation of 3 it has
 *   - [last_fail] whether the last $PATH part is a failure case distinct from
 *     the terminal case
 */

struct skipcase {
	const char *part, *name;
};

struct termcase {
	int exst;
	const char *part, *name;
};


static const struct skipcase all_skips[] = {
	{ "user/test/io/dir", "not found" },
	/* TODO: replace these two with cases where "exit_with_0" doesn't have
	 * execute permission, and where "exit_with_0"'s hashbang interpreter
	 * doesn't have execute permission.
	 */
	{ "user", "not found (user)" },
	{ TESTDIR, "not found (root)" },
};


static const struct termcase terminals[] = {
	{ .exst = 0, .part = "user/test/tools", .name = "success" },
	{ ENOENT, NULL, "none" },
	{ EACCES, "user/test/exec/isdir", "prg is directory" },
	{ EACCES, "user/test/exec/xnotset", "+x not set" },
	{ ENOENT, "user/test/foobie/bletch", "missing directory" },
	{ ENOENT, "user/test/exec/noexec_script", "interpreter doesn't exist" },
	/* TODO: add one where search access (+x) is denied (EACCES) */
	/* TODO: add one where cpu/abi aren't supported (EINVAL) */
	/* TODO: add one where path component exceeds NAME_MAX (ENAMETOOLONG) */
	/* TODO: add one where aggregate path exceeds PATH_MAX (ENAMETOOLONG) */
};


START_LOOP_TEST(path_search, iter, 0,
	2 * ARRAY_SIZE(terminals) * (6 + 1) - 1)	/* 6 is factorial(len(all_skips)) */
{
	const char *prg = "exit_with_0";
	diag("prg=`%s'", prg);

	const bool last_fail = !!(iter & 1);
	const int term_index = (iter >> 1) % ARRAY_SIZE(terminals),
		skip_perm = (iter >> 1) / ARRAY_SIZE(terminals);

	const struct termcase *term = &terminals[term_index];
	diag("last_fail=%s, terminal=%d `%s'",
		btos(last_fail), term_index, term->name);

	/* generate either a permutation of all_skips, or nothing.
	 *
	 * TODO: it'd be better to generate all k-permutations of all_skips for
	 * k <= len(all_skips), but since that's dicky and provides little value
	 * we'll leave that for someone else later on.
	 */
	const struct skipcase *skips[ARRAY_SIZE(all_skips)];
	int n_skips;
	darray(char) skipstr = darray_new();
	darray_append(skipstr, '[');
	if(skip_perm == 0) n_skips = 0;
	else {
		assert(skip_perm - 1 < factorial(ARRAY_SIZE(all_skips)));
		n_skips = ARRAY_SIZE(all_skips);
		unsigned ixes[n_skips];
		gen_perm(ixes, n_skips, skip_perm - 1);
		for(int i=0; i < n_skips; i++) {
			skips[i] = &all_skips[ixes[i]];
			if(i > 0) darray_appends(skipstr, ',', ' ');
			darray_append_string(skipstr, skips[i]->name);
		}
	}
	darray_appends(skipstr, ']', '\0');
	diag("skip_perm=%d, skips=%s", skip_perm, skipstr.item);
	darray_free(skipstr);

	plan_tests(7);
	ok1(chdir(TESTDIR) == 0);

	darray(char) pathvar = darray_new();
	for(int i=0; i < n_skips; i++) {
		if(i > 0) darray_appends(pathvar, ':');
		darray_append_string(pathvar, skips[i]->part);
	}
	if(term->part != NULL) {
		if(n_skips > 0) darray_appends(pathvar, ':');
		darray_append_string(pathvar, term->part);
	}
	const struct termcase *failer = NULL;
	if(last_fail) {
		for(int i=0; i < ARRAY_SIZE(terminals); i++) {
			if(term == &terminals[i]) continue;	/* must be different */
			if(terminals[i].exst == 0) continue; /* ... and fail */
			failer = &terminals[i];
			break;
		}
		assert(failer != NULL);
		diag("failer=`%s'", failer->name);
		if(n_skips > 0 || term->part != NULL) darray_appends(pathvar, ':');
		if(failer->part != NULL) darray_append_string(pathvar, failer->part);
	}
	darray_appends(pathvar, '\0');
#ifdef __sneks__
	diag("pathvar.item=`%s'", pathvar.item);
#endif

	int child = fork();
	assert(child >= 0);
	if(child == 0) {
		if(pathvar.size == 0) unsetenv("PATH"); else setenv("PATH", pathvar.item, 1);
		execlp(prg, prg, (char *)NULL);
		diag("child errno=%d", errno);
		exit(0x80 | errno);
	}

	int st, dead = waitpid(child, &st, 0);
	ok1(dead == child);

	skip_start(!ok1(WIFEXITED(st)), 4, "didn't exit") {
		int exst = WEXITSTATUS(st);
		iff_ok1(exst & 0x80, term->exst != 0);
		int exec_err = exst & ~0x80;
		if(!imply_ok1(term->part != NULL && term->exst != 0, exec_err == term->exst)) {
			diag("exec_err=%d, term->exst=%d", exec_err, term->exst);
		}
		if(!imply_ok1(term->part == NULL && failer != NULL, exec_err == failer->exst)) {
			diag("exec_err=%d, failer->exst=%d", exec_err, failer->exst);
		}
		if(!imply_ok1(term->part == NULL && failer == NULL, exec_err == ENOENT)) {
			diag("exec_err=%d", exec_err);
		}
	} skip_end;

	darray_free(pathvar);
}
END_TEST

DECLARE_TEST("process:exec", path_search);
