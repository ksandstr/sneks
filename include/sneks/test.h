/* imitation of tap.h from libtap by Nik Clayton, plus some extras. interface
 * for lib/tap.c .
 */

#ifndef __SNEKS_TEST_H__
#define __SNEKS_TEST_H__

#include <stdbool.h>
#include <ccan/compiler/compiler.h>
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
	static void NAME (int); \
	static const struct test_info NAME ## _info = { \
		.test_fn = &NAME, .name = #NAME, \
	}; \
	static void NAME (int _i) {
#define START_LOOP_TEST(NAME, VAR, LOW, HIGH) \
	static void NAME (int); \
	static const struct test_info NAME ## _info = { \
		.test_fn = &NAME, .name = #NAME, \
		.iter_low = (LOW), .iter_high = (HIGH), \
	}; \
	static void NAME (int _i) { \
		int VAR = _i;
#define END_TEST }

/* brave new world of linker magic test specs */

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


extern void run_all_systests(void);
extern void run_systest_by_spec(char **specs, size_t n_specs);
extern void describe_all_systests(void);


/* fail() inherited from the libtap imitation */

#define fail_unless(expr, ...) \
	_fail_unless((expr), __FILE__, __LINE__, \
		"Assertion `" #expr "' failed", ## __VA_ARGS__, NULL)

#define fail_if(expr, ...) \
	_fail_unless(!(expr), __FILE__, __LINE__, \
		"Failure `" #expr "' occurred", ## __VA_ARGS__, NULL)


/* internal API for test exit from _fail_unless() */
extern NORETURN void exit_on_fail(void);

/* same for test from __assert_failure() */
extern bool in_test(void);


/* from tap.c */

extern void _fail_unless(
	int result, const char *file, int line,
	const char *expr, ...);

extern int _gen_result(
	bool ok,
	const char *func, const char *file, unsigned int line,
	const char *test_name, ...);

extern void tap_reset(void);	/* called by the test harness */

extern void plan_no_plan(void);
extern void plan_skip_all(const char *reason);
extern void plan_tests(unsigned int num_tests);

extern int diag(const char *fmt, ...);
extern int skip(unsigned int num_skip, const char *reason, ...);
extern void todo_start(const char *fmt, ...);
extern void todo_end(void);

extern void subtest_start(const char *fmt, ...);
extern int subtest_end(void);

extern int exit_status(void);

#endif
