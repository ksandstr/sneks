
/* main test program for userspace, i.e. subject to, and able to test, IPC
 * redirection and scheduling. once those things appear.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include <ccan/opt/opt.h>
#include <ccan/darray/darray.h>

#include <sneks/test.h>

#include "defs.h"


/* TODO: change do_describe to false by default once the test reporting script
 * learns to use both --describe and DESCRIBE=1 depending on context. for now,
 * always printing desc lines makes up for the misunderstanding.
 */
static bool no_test = false, do_describe = true;
static darray(char *) only_ids = darray_new();


static char *add_run_only(const char *testspec, void *unused)
{
	char *base = strdup(testspec), *arg = base, *sep;
	do {
		sep = strchr(arg, '+');
		if(sep != NULL) *sep = '\0';
		darray_push(only_ids, strdup(arg));
		if(sep != NULL) arg = sep + 1;
	} while(sep != NULL);
	free(base);
	return NULL;
}


static void ignore_opt_error(const char *fmt, ...) {
	/* what it says on teh tin */
}


static const struct opt_table opts[] = {
	OPT_WITHOUT_ARG("--describe", &opt_set_bool, &do_describe,
		"print desc lines for all tests before executing them"),
	OPT_WITHOUT_ARG("--no-test", &opt_set_bool, &no_test,
		"don't run any tests"),
	OPT_WITH_ARG("--run-only=ids", &add_run_only, NULL, NULL,
		"run only specified tests, separated by `+' signs"),
	OPT_ENDTABLE
};


int main(int argc, char *argv[])
{
	setvbuf(stdout, NULL, _IONBF, 0);
	opt_register_table(opts, NULL);
	if(!opt_parse(&argc, argv, &ignore_opt_error)) {
		printf("*** option parsing failed!\n");
		return 1;
	}
	char *envargs = getenv("UTEST_OPTS");
	if(envargs != NULL) {
		/* accept more ampersand-separated arguments from UTEST_OPTS. */
		envargs = strdup(envargs);	/* leak like a boss */
		darray(char *) other_argv = darray_new();
		darray_push(other_argv, argv[0]);
		while(*envargs != '\0') {
			char *sep = envargs + strcspn(envargs, "&");
			if(*sep == '&') *(sep++) = '\0';
			darray_push(other_argv, envargs);
			envargs = sep;
		}
		darray_push(other_argv, NULL);	/* for CCAN opt */
		int other_argc = other_argv.size - 1;
		if(!opt_parse(&other_argc, other_argv.item, &ignore_opt_error)) {
			printf("*** UTEST_OPTS parsing failed!\n");
			return 1;
		}
		darray_free(other_argv);
	}

	if(do_describe) describe_all_tests();
	if(!no_test) {
		if(darray_empty(only_ids)) run_all_tests();
		else run_test_by_spec(only_ids.item, only_ids.size);
	}

	printf("*** testsuite completed.\n");
	return 0;
}
