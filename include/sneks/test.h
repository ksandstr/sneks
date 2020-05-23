/* imitation of tap.h from libtap by Nik Clayton, plus some extras. interface
 * for lib/tap.c .
 */

#ifndef __SNEKS_TEST_H__
#define __SNEKS_TEST_H__

#include <stdbool.h>
#include <stdnoreturn.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ccan/autodata/autodata.h>


#define ok(cond, test, ...) \
	_gen_result(!!(cond), __func__, __FILE__, __LINE__, \
		test, ##__VA_ARGS__)

#define ok1(cond) \
	_gen_result(!!(cond), __func__, __FILE__, __LINE__, "%s", #cond)

/* >implying implications */
#define imply_ok1(left, right) \
	ok(!(left) || (right), "%s --> %s", #left, #right)

#define imply_ok(left, right, test, ...) \
	ok(!(left) || (right), test, ##__VA_ARGS__)

/* alias for left == right, printed as "iff". */
#define iff_ok1(left, right) \
	ok((left) == (right), "%s iff %s", #left, #right)

#define pass(test, ...) ok(true, (test), ##__VA_ARGS__)
#define fail(test, ...) ok(false, (test), ##__VA_ARGS__)

/* (note the unclosed do-while block.) */
#define skip_start(cond, n, fmt, ...) \
	do { \
		if((cond)) { skip((n), (fmt), ##__VA_ARGS__); continue; }

#define skip_end \
	} while(false)

#define subtest_f(test_fn, param, fmt, ...) \
	({ subtest_start(fmt, ##__VA_ARGS__); \
	   (*(test_fn))((param)); \
	   subtest_end(); \
	 })


/* utility macros. */
#define btos(x) (!!(x) ? "true" : "false")


/* vaguely post-Check-style systemspace unit testing things exported in, and
 * related to, sys/test/check.c .
 */

struct test_info
{
	void (*test_fn)(int);
	const char *name;
	int iter_low, iter_high;
};

#define START_TEST(NAME) \
	static void TEST_ ## NAME (int); \
	static const struct test_info NAME ## _info = { \
		.test_fn = &TEST_ ## NAME, .name = #NAME, \
	}; \
	static void TEST_ ## NAME (int _i) {
#define START_LOOP_TEST(NAME, VAR, LOW, HIGH) \
	static void LOOP_TEST_ ## NAME (int); \
	static const struct test_info NAME ## _info = { \
		.test_fn = &LOOP_TEST_ ## NAME, .name = #NAME, \
		.iter_low = (LOW), .iter_high = (HIGH), \
	}; \
	static void LOOP_TEST_ ## NAME (int _i) { \
		int VAR = _i;
#define END_TEST }


/* brave new world of linker magic test specs. these ones work for systests
 * only, and those have to be in a C source file in a t/ path.
 */

struct systest_spec {
	const char *path;
	const struct test_info *test;
};
AUTODATA_TYPE(all_systests, struct systest_spec);

/* @path is "prefix:group", or "group" for no prefix. */
#define SYSTEST(path_, test_) \
	static const struct systest_spec _ST_ ##test_ = { \
		.path = (path_), .test = &(test_## _info), \
	}; \
	AUTODATA(all_systests, &_ST_ ##test_);

/* a fully parsed test, as returned from the interface. */
struct systest {
	const char *prefix, *group, *name, *id;
	int low, high, pri;
	void (*fn)(int iter);
};


/* (this should be in sys/test/defs.h or some such.) */
extern void run_all_systests(void);
extern void run_systest_by_spec(char **specs, size_t n_specs);
extern void describe_all_systests(void);


/* these work for userspace tests. they're just like the ones for systests,
 * but without the "systest" nomenclature. this turns a class of obscure
 * malfunction into linker errors, which is nice.
 */

struct utest_spec {
	const char *path;
	const struct test_info *test;
};
AUTODATA_TYPE(all_utest_specs, struct utest_spec);

#define DECLARE_TEST(path_, test_) \
	static const struct utest_spec _UT_ ##test_ = { \
		.path = (path_), .test = &(test_## _info), \
	}; \
	AUTODATA(all_utest_specs, &_UT_ ##test_);


/* fail() inherited from the libtap imitation */

#define fail_unless(expr, ...) \
	_fail_unless((expr), __FILE__, __LINE__, \
		#expr, "" __VA_ARGS__, NULL)

#define fail_if(expr, ...) \
	_fail_unless(!(expr), __FILE__, __LINE__, \
		"!(" #expr ")", "" __VA_ARGS__, NULL)

/* explicit bails inspired by other TAP implementations. bails abort the
 * entire test; a bail() in a forked subtest will also bail the parent (and so
 * on) at join. this applies to the code part of the lives/dies tests as well
 * since they sugar over the forked subtest syntax.
 */
extern noreturn void bail(const char *fmt, ...);
extern noreturn void vbail(const char *fmt, va_list args);

#define BAIL_OUT(...) bail("" __VA_ARGS__, NULL)


/* same for test from __assert_failure() */
extern bool in_test(void);


/* from tap.c */

extern void _fail_unless(
	int result, const char *file, int line,
	const char *expr, const char *fmt, ...);

extern int _gen_result(
	bool ok,
	const char *func, const char *file, unsigned int line,
	const char *test_name, ...)
		__attribute__((format(printf, 5, 6)));

extern void tap_reset(void);	/* called by the test harness */

extern void plan_no_plan(void);
extern void plan_skip_all(const char *reason_fmt, ...)
	__attribute__((format(printf, 1, 2)));
extern void plan_tests(unsigned int num_tests);

/* short-form plan() inspired by libtap. @tests is either the number of tests,
 * NO_PLAN, or SKIP_ALL, with the same effect as plan_tests(), plan_no_plan(),
 * and plan_skip_all() respectively. also done_testing() to exit a test
 * routine early (completing a lazy plan), available in user/test and nowhere
 * else.
 *
 * NOTE: planf() can't take a printf attribute because of the extra NULL at
 * the end, which the compiler will whine about.
 */
extern void planf(int tests, const char *fmt, ...);

#define plan(...) planf(__VA_ARGS__, NULL)

#define NO_PLAN -1
#define SKIP_ALL -2

extern int diag(const char *fmt, ...);
extern int skip(unsigned int num_skip, const char *reason, ...);
extern void todo_start(const char *fmt, ...);
extern void todo_end(void);

extern void subtest_start(const char *fmt, ...);
extern int subtest_end(void);
extern char *subtest_pop(int *rc_p, void **freeptr_p);

extern int exit_status(void);
extern noreturn void done_testing(void);

extern void close_no_plan(void);	/* for harness.c impls */

/* forked subtests. obviously not available under sys/test .
 * #include <sys/wait.h> to make these compile.
 *
 * TODO: fetch test name from child somehow, display in fork_subtest_ok1().
 * this may require pipes or a shared memory segment or something.
 */
#define fork_subtest_start(_fmt, ...) ({ \
		int __stc = fork(); \
		if(__stc == 0) { \
			subtest_start(_fmt, ##__VA_ARGS__);

#define fork_subtest_end \
			int _rc; \
			char *_msg = subtest_pop(&_rc, NULL); \
			diag("(subtest name was `%s')", _msg); \
			exit(_rc); \
		} \
		__stc; \
	})

/* returns the waitpid() status, for WIFEXITED() and the like. when exit
 * status is 255, propagates bail-out by exit(255)'ing.
 */
#define fork_subtest_join(_child) ({ \
		int __st, __dead, __c = (_child); \
		do { \
			__dead = waitpid(__c, &__st, 0); \
		} while(__dead < 0 && errno == EINTR); \
		fail_unless(__dead == __c); \
		if(WIFEXITED(__st) && WEXITSTATUS(__st) == 255) { \
			exit(255); \
		} \
		__st; \
	})

/* joins the forked subtest as a test point in the parent. will eventually
 * print the test name in the ok-line. returns like ok1(), bails like
 * fork_subtest_join().
 */
#define fork_subtest_ok1(_child) ({ \
		int __st, __dead, __c = (_child); \
		do { \
			__dead = waitpid(__c, &__st, 0); \
		} while(__dead < 0 && errno == EINTR); \
		if(WIFEXITED(__st) && WEXITSTATUS(__st) == 255) { \
			exit(255); \
		} \
		ok(__dead == __c \
			&& WIFEXITED(__st) && WEXITSTATUS(__st) == 0, \
			"unknown subtest"); 	/* press f to pay respects */ \
	})

/* inspired by liptap: dies_ok() and lives_ok().
 *
 * TODO: add some kind of a proper died() predicate to these; plain
 * WIFEXITED() is surely insufficient and the status could be reported to a
 * greater extent.
 */
#define dies_ok(code, fmt, ...) ({ \
		int _child = fork_subtest_start((fmt), ##__VA_ARGS__) { \
			(code); \
			exit(0); \
		} fork_subtest_end; \
		int _status = fork_subtest_join(_child); \
		ok(!WIFEXITED(_status), (fmt), ##__VA_ARGS__); \
	})

#define lives_ok(code, fmt, ...) ({ \
		int _child = fork_subtest_start((fmt), ##__VA_ARGS__) { \
			(code); \
			exit(0); \
		} fork_subtest_end; \
		int _status = fork_subtest_join(_child); \
		ok(WIFEXITED(_status), (fmt), ##__VA_ARGS__); \
	})

#define dies_ok1(code) dies_ok((code), #code " dies")
#define lives_ok1(code) lives_ok((code), #code " lives")


#ifndef __sneks__
/* being nice to hostsuite <3 */
extern int strscpy(char *dest, const char *src, size_t n);
#endif

#endif
