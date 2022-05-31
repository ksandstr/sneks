/* libtap-like interface. a thoroughly rewritten version of libtap's tap.c,
 * which carries the following license statement:
 *-
 * Copyright (c) 2004 Nik Clayton
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *-
 * the aggregate work is hereby licensed with identical terms to the ones
 * above, excepting the copyright statement.
 */

#if !defined(__KERNEL__) || defined(ENABLE_SELFTEST)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdnoreturn.h>
#include <stdarg.h>
#include <assert.h>
#include <ccan/likely/likely.h>
#include <sneks/test.h>

struct tapctx {
	struct tapctx *succ;
	bool no_plan, have_plan, skip_all, todo, test_died, no_plan_closed;
	int depth, num_tests_run, expected_tests, failed_tests;
	char *todo_msg, testmsg[];
};

static struct tapctx firstctx = { }, *tap = &firstctx;

void tap_reset(void)
{
	while(tap != &firstctx) {
		struct tapctx *next = tap->succ;
		free(tap->todo_msg); free(tap);
		tap = next;
	}
	free(tap->todo_msg);
	*tap = (struct tapctx){ };
}

void subtest_start(const char *fmt, ...)
{
	va_list al; va_start(al, fmt);
	int msglen = vsnprintf(NULL, 0, fmt, al) + 1;
	va_end(al);
	struct tapctx *c = malloc(sizeof *c + msglen); if(c == NULL) abort();
	*c = (struct tapctx){ .succ = tap, .depth = tap->depth + 1 };
	if(tap->todo_msg != NULL) c->todo_msg = strdup(tap->todo_msg);
	va_start(al, fmt); vsnprintf(c->testmsg, msglen, fmt, al); va_end(al);
	tap = c;
}

char *subtest_pop(int *rc_p, void **freeptr_p)
{
	if(tap == &firstctx) { diag("tried to pop no subtest?"); abort(); }
	close_no_plan();
	int st = exit_status();
	if(st != 0) diag("subtest exit_status=%d", st);
	if(rc_p != NULL) *rc_p = st;
	struct tapctx *dead = tap; tap = dead->succ;
	free(dead->todo_msg);
	if(freeptr_p != NULL) {
		*freeptr_p = dead; return dead->testmsg;
	} else {
		free(dead); return NULL;
	}
}

int subtest_end(void)
{
	int rc; void *freeptr;
	char *msg = subtest_pop(&rc, &freeptr);
	ok(rc == 0, "%s", msg);
	free(freeptr);
	return rc;
}

static void print_sub_prefix(void) {
	for(int i=0; i < tap->depth; i++) printf("    ");
}

noreturn void vbail(const char *fmt, va_list args)
{
	fflush(stdout);
	/* no subtest prefix for bailouts; they fail the entire test.
	 * (this should be different for forked subtests which don't.)
	 */
	printf("Bail out!  "); vfprintf(stdout, fmt, args); printf("\n");
	fflush(stdout);
	exit(255);
}

noreturn void bail(const char *fmt, ...) { va_list al; va_start(al, fmt); vbail(fmt, al); }

#ifndef __KERNEL__
void _fail_unless(int result, const char *file, int line, const char *expr, const char *fmt, ...)
{
	if(likely(result)) return;
	else if(fmt == NULL) {
		printf("Bail out!  `%s' failed in %s:%d\n", expr, file, line);
	} else {
		va_list ap; va_start(ap, fmt);
		char buf[200]; vsnprintf(buf, sizeof buf, fmt, ap);
		va_end(ap);
		printf("Bail out!  %s: `%s' in %s:%d\n", buf, expr, file, line);
	}
	abort(); /* fail_if() and fail_unless() have assert nature, so abort() it is. */
}
#endif

int _gen_result(bool ok, const char *func, const char *file, unsigned int line, const char *test_name_fmt, ...)
{
	tap->num_tests_run++;
	char *test_name = NULL, name_buf[200];
	if(test_name_fmt != NULL) {
		va_list al; va_start(al, test_name_fmt);
		vsnprintf(name_buf, sizeof name_buf, test_name_fmt, al);
		va_end(al);
		test_name = name_buf;
		bool found_ok = false;
		for(int i=0; test_name[i] != '\0'; i++) {
			char c = test_name[i];
			if((c < '0' || c > '9') && c != ' ' && c != '\t' && c != '\n' && c != '\r') {
				found_ok = true;
				break;
			}
		}
		if(!found_ok) {
			diag("    You named your test `%s'. You shouldn't use numbers for your test names.", test_name);
			diag("    Very confusing.");
		}
	}
	/* core */
	print_sub_prefix();
	if(!ok) {
		printf("not ");
		tap->failed_tests++;
	}
	printf("ok %d", tap->num_tests_run);
	/* extension */
	if(test_name != NULL) {
		int len = strlen(test_name);
		char escbuf[len * 2 + 1];
		for(int i=0, p=0; i < len; i++) {
			if(test_name[i] != '#') escbuf[p++] = test_name[i];
			else {
				escbuf[p++] = '\\';
				escbuf[p++] = '#';
			}
			if(i + 1 == len) escbuf[p++] = '\0';
		}
		printf(" - %s", escbuf);
	}
	/* adornment */
	if(tap->todo) {
		printf(" # TODO %s", tap->todo_msg);
		if(!ok) tap->failed_tests--;
	}
	printf("\n");
	/* detail */
	if(!ok) diag("    Failed %stest (%s:%s() at line %d)", tap->todo ? "(TODO) " : "", file, func, line);
	return ok ? 1 : 0;
}

static void plan_once(void) {
	if(!tap->have_plan) return;
	fprintf(stderr, "You tried to plan twice!\n");
	tap->test_died = true;
	abort();
}

void plan_no_plan(void) {
	plan_once();
	tap->have_plan = true; tap->no_plan = true;
}

static void plan_skip_all_v(const char *fmt, va_list al)
{
	plan_once();
	tap->have_plan = true; tap->skip_all = true;
	print_sub_prefix();
	printf("1..0");
	if(fmt != NULL) {
		printf(" # Skip ");
		vfprintf(stdout, fmt, al);
	}
	printf("\n");
	/* TODO: nonlocal exit to the top level, for comfort? */
}

void plan_skip_all(const char *fmt, ...) {
	va_list al; va_start(al, fmt);
	plan_skip_all_v(fmt, al);
	va_end(al);
}

void plan_tests(unsigned int num_tests)
{
	plan_once();
	if(num_tests == 0) {
		fprintf(stderr, "You said to run 0 tests! You've got to run something.\n");
		tap->test_died = true;
		abort();
	}
	tap->have_plan = 1;
	print_sub_prefix();
	printf("1..%u\n", num_tests);
	tap->expected_tests = num_tests;
}

void planf(int tests, const char *fmt, ...)
{
	if(tests >= 0) plan_tests(tests);
	else if(tests == NO_PLAN) plan_no_plan();
	else {
		assert(tests == SKIP_ALL);
		va_list al; va_start(al, fmt);
		plan_skip_all_v(fmt, al);
		va_end(al);
	}
}

int diag(const char *fmt, ...)
{
	va_list al; va_start(al, fmt);
	char msg[200]; vsnprintf(msg, sizeof msg, fmt, al);
	va_end(al);
	char *s = msg, *lf;
	do {
		if(lf = strchr(s, '\n'), lf != NULL) *lf = '\0';
		print_sub_prefix();
		fprintf(stderr, "# %s\n", s);
		if(lf != NULL) s = lf + 1;
	} while(lf != NULL);
	return 0;
}

int skip(unsigned int num_skip, const char *reason, ...)
{
	va_list al; va_start(al, reason);
	char msg[200]; vsnprintf(msg, sizeof msg, reason, al);
	va_end(al);
	for(unsigned int i = 0; i < num_skip; i++) {
		tap->num_tests_run++;
		print_sub_prefix();
		printf("ok %d # skip %s\n", tap->num_tests_run, msg);
	}
	return 1;
}

void todo_start(const char *fmt, ...)
{
	va_list al; va_start(al, fmt);
	char msg[200]; int len = vsnprintf(msg, sizeof msg, fmt, al);
	va_end(al);
	/* TODO: this may leave todo_msg NULL. it should be replaced with a
	 * "tap.c malloc() issue" type message, as in libtap.
	 */
	free(tap->todo_msg);
	if(tap->todo_msg = malloc(len + 1), tap->todo_msg != NULL) memcpy(tap->todo_msg, msg, len + 1);
	tap->todo = true;
}

void todo_end(void) {
	tap->todo = false;
	free(tap->todo_msg); tap->todo_msg = NULL;
}

int exit_status(void)
{
	if(tap->no_plan || !tap->have_plan) return tap->failed_tests;
	else if(tap->expected_tests < tap->num_tests_run) {
		/* ran too many. return number of unplanned tests */
		return tap->num_tests_run - tap->expected_tests;
	} else {
		/* failed + didn't run */
		return tap->failed_tests + tap->expected_tests - tap->num_tests_run;
	}
}

void close_no_plan(void)
{
	if(tap->no_plan_closed) return;
	if(!tap->have_plan || tap->no_plan) {
		print_sub_prefix();
		printf("1..%d\n", tap->num_tests_run);
		tap->expected_tests = tap->num_tests_run;
		if(!tap->have_plan) {
			/* discourage an implicit lazy plan */
			diag("    No plan until end of test?");
			tap->have_plan = true;	/* shouldn't plan again. */
		}
	}
	tap->no_plan_closed = true;
}

#endif
