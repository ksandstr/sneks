
/* tests on the getenv(3) family, being the various modes of access to the
 * per-process environment variables, which systasks can use to receive
 * parameters.
 *
 * TODO: share this with userspace tests as well.
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <ccan/str/str.h>
#include <ccan/talloc/talloc.h>
#include <ccan/darray/darray.h>

#include <sneks/test.h>


extern char **environ;


START_TEST(basic_get_set)
{
	const char *varname = "BASIC_TESTVARIABLE";
	plan_tests(3);

	ok1(getenv(varname) == NULL);
	if(!ok1(setenv(varname, "TESTVALUE", 0) == 0)) {
		diag("errno=%d", errno);
	}
	const char *value_after = getenv(varname);
	if(!ok1(value_after != NULL && streq(value_after, "TESTVALUE"))) {
		diag("value_after=%p (`%s')", value_after,
			value_after != NULL ? value_after : "(N/A)");
	}
}
END_TEST

SYSTEST("crt:env", basic_get_set);


START_TEST(setenv_invalid_name)
{
	plan_tests(3);

	/* invalid values for `name'. */
	int n = setenv(NULL, "TESTVALUE", 0);
	ok(n < 0 && errno == EINVAL, "EINVAL when name is NULL");
	n = setenv("", "TESTVALUE", 0);
	ok(n < 0 && errno == EINVAL, "EINVAL when name is empty");
	n = setenv("TEST=VARIABLE", "TESTVALUE", 0);
	ok(n < 0 && errno == EINVAL, "EINVAL when name has '='");
}
END_TEST

SYSTEST("crt:env", setenv_invalid_name);


/* test that overwriting happens iff requested. */
START_LOOP_TEST(setenv_overwrite, iter, 0, 1)
{
	char varname[64];
	snprintf(varname, sizeof varname, "OVERWRITE_TESTVARIABLE%d", iter);
	const int overwrite = (iter & 1);
	diag("overwrite=%d", overwrite);

	plan_tests(5);

	ok1(getenv(varname) == NULL);
	int n = setenv(varname, "OLD", 0);
	ok(n == 0, "1st setenv");
	const char *oldval = getenv(varname);
	if(oldval == NULL) oldval = "";
	ok(strcmp(oldval, "OLD") == 0, "getenv");
	n = setenv(varname, "NEW", overwrite);
	ok(n == 0, "2nd setenv");
	const char *newval = getenv(varname);
	if(newval == NULL) newval = "";
	iff_ok1(overwrite, strcmp(newval, "NEW") == 0);
}
END_TEST

SYSTEST("crt:env", setenv_overwrite);


START_TEST(basic_unset)
{
	const char *varname = "UNSET_TESTVARIABLE";
	plan_tests(5);

	ok1(getenv(varname) == NULL);
	int n = setenv(varname, "OLD", 0);
	ok(n == 0, "setenv");
	char *value = getenv(varname);
	ok1(value != NULL && streq(value, "OLD"));
	n = unsetenv(varname);
	ok(n == 0, "unsetenv");
	ok1(getenv(varname) == NULL);
}
END_TEST

SYSTEST("crt:env", basic_unset);


START_TEST(basic_clear)
{
	const char *var1 = "CLEAR_TEST0", *var2 = "CLEAR_TEST1";
	plan_tests(7);

	ok1(getenv(var1) == NULL);
	ok1(getenv(var2) == NULL);
	int n = setenv(var1, "xx", 0);
	ok(n == 0, "setenv %s", var1);
	n = setenv(var2, "yy", 0);
	ok(n == 0, "setenv %s", var2);
	n = clearenv();
	ok(n == 0, "clearenv");
	ok1(getenv(var1) == NULL);
	ok1(getenv(var2) == NULL);
}
END_TEST

SYSTEST("crt:env", basic_clear);


/* basic positive functionality test for putenv(3). */
START_TEST(basic_put)
{
	char *data = strdup("PUT_TESTVARIABLE=TESTVALUE");
	assert(data != NULL);
	plan_tests(6);

	/* putenv changes the environment. */
	ok1(getenv("PUT_TESTVARIABLE") == NULL);
	int n = putenv(data);
	ok(n == 0, "putenv");
	char *value = getenv("PUT_TESTVARIABLE");
	ok1(value != NULL && streq(value, "TESTVALUE"));

	/* changing putenv's parameter afterward also changes the environment. */
	char *sep = strchr(data, '=');
	sep[2] = 'U';
	value = getenv("PUT_TESTVARIABLE");
	ok1(value != NULL && streq(value, "TUSTVALUE"));

	/* putenv variables are removed by unsetenv. */
	n = unsetenv("PUT_TESTVARIABLE");
	ok(n == 0, "unsetenv");
	ok1(getenv("PUT_TESTVARIABLE") == NULL);

	free(data);
}
END_TEST

SYSTEST("crt:env", basic_put);


/* putenv() edge cases:
 *   - no equals sign in `name'
 *   - equals sign at position 0
 *   - empty string
 *
 * none of these should change the environment.
 */
START_TEST(edge_put)
{
	const char *prefix = "EDGE_PUT";
	plan_tests(9);

	void *talctx = talloc_new(NULL);
	static int count = 0;

	/* empty string. */
	char *value = getenv("");
	skip_start(!ok(value == NULL, "no value for \"\""), 2, "unclean") {
		int n = putenv(talloc_strdup(talctx, ""));
		ok(n == 0, "putenv for the empty string");
		value = getenv("");
		if(!ok1(value == NULL)) {
			diag("value=%p (`%s')", value, value);
		}
	} skip_end;

	/* no equals sign. */
	char *var = talloc_asprintf(talctx, "%s%d", prefix, count++);
	value = getenv(var);
	ok(value == NULL, "no value for `%s'", var);
	int n = putenv(var);
	ok(n == 0, "putenv for `%s'", var);
	value = getenv(var);
	ok1(value == NULL);

	/* equals sign at position 0. */
	value = getenv("");
	skip_start(!ok(value == NULL, "no value for \"\""), 2, "unclean") {
		n = putenv(talloc_strdup(talctx, "=EDGE_PUT_VALUE"));
		ok(n == 0, "putenv for leading =");
		value = getenv("");
		if(!ok1(value == NULL)) {
			diag("value=%p (`%s')", value, value);
		}
	} skip_end;

	talloc_free(talctx);
}
END_TEST

SYSTEST("crt:env", edge_put);


/* putenv() should overwrite environment variables created by both setenv()
 * and putenv().
 */
START_LOOP_TEST(overwrite_put, iter, 0, 1)
{
	const char *varname = "OVERWRITE_TESTVARIABLE";
	const bool first_put = !!(iter & 1);
	diag("first_put=%s", btos(first_put));
	plan_tests(7);

	void *talctx = talloc_new(NULL);

	/* setup: place varname=OLD in the environment and verify. */
	ok1(getenv(varname) == NULL);
	int n;
	if(!first_put) n = setenv(varname, "OLD", 0);
	else {
		char *var = talloc_asprintf(talctx, "%s=OLD", varname);
		n = putenv(var);
	}
	ok(n == 0, "old value set ok");
	char *value = getenv(varname);
	ok1(value != NULL && streq(value, "OLD"));

	/* overwrite it with putenv(3). */
	char *var = talloc_asprintf(talctx, "%s=NEW", varname);
	n = putenv(var);
	ok(n == 0, "putenv (overwriting)");
	value = getenv(varname);
	ok1(value != NULL && streq(value, "NEW"));

	/* remove it with unsetenv. */
	n = unsetenv(varname);
	ok(n == 0, "unsetenv");
	value = getenv(varname);
	if(!ok(value == NULL, "removed")) {
		diag("value=%p `%s'", value, value);
	}

	talloc_free(talctx);
}
END_TEST

SYSTEST("crt:env", overwrite_put);


/* environ should be changed by calls to setenv() and putenv(). */
static bool search_environ(const char *name, const char *value);

START_TEST(environ_alter)
{
	plan_tests(4);

	ok1(setenv("ARGLE", "bargle", 1) == 0);
	ok1(search_environ("ARGLE", "bargle"));

	ok1(putenv("QWERT=uiop") == 0);
	ok1(search_environ("QWERT", "uiop"));
}
END_TEST

SYSTEST("crt:env", environ_alter);


static bool search_environ(const char *name, const char *value)
{
	int nl = strlen(name), vl = strlen(value);
	char cmp[nl + vl + 2];
	snprintf(cmp, sizeof cmp, "%s=%s", name, value);
	for(int i=0; environ != NULL && environ[i] != NULL; i++) {
		if(streq(environ[i], cmp)) return true;
	}
	return false;
}


/* clear environ like it says on the manual page; subsequently getenv() should
 * return NULLs for everything previously entered.
 */
START_LOOP_TEST(environ_reset, iter, 0, 1)
{
	const bool use_clearenv = !!(iter & 1);
	diag("use_clearenv=%s", btos(use_clearenv));
	plan_tests(5);

	todo_start("unimplemented");

	ok1(setenv("FOO", "foo", 1) == 0);
	ok1(setenv("BAR", "bar", 1) == 0);
	if(use_clearenv) {
		clearenv();
		ok(true, "clearenv() didn't die");
	} else {
		environ = NULL;
		skip(1, "not calling clearenv()");
	}
	ok1(getenv("FOO") == NULL);
	ok1(getenv("BAR") == NULL);
}
END_TEST

SYSTEST("crt:env", environ_reset);


/* reset environ to a darray we construct locally. */
START_TEST(environ_replace)
{
	plan_tests(11);

	todo_start("unimplemented");

	ok1(setenv("FOO", "foo", 1) == 0);
	ok1(setenv("BAR", "bar", 1) == 0);

	darray(char *) newenv = darray_new();
	darray_appends(newenv, "ZOT=zot", "ASDF=sdfg", "QWERT=yuiop", NULL);
	environ = newenv.item;
	diag("environ changed");

	/* access of replaced environ */
	ok1(getenv("FOO") == NULL);
	ok1(getenv("BAR") == NULL);
	ok1(getenv("ZOT") != NULL && streq(getenv("ZOT"), "zot"));
	ok1(getenv("ASDF") != NULL && streq(getenv("ASDF"), "sdfg"));
	ok1(getenv("QWERT") != NULL && streq(getenv("QWERT"), "yuiop"));

	/* modification */
	ok1(unsetenv("ZOT") == 0);
	ok1(setenv("ASDF", "fdsa", 1) == 0);
	ok1(getenv("ZOT") == NULL);
	ok1(getenv("ASDF") != NULL && streq(getenv("ASDF"), "fdsa"));
}
END_TEST

SYSTEST("crt:env", environ_replace);
