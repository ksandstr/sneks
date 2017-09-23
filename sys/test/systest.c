/* main test program for systasks.
 *
 * executes tests concerning system tasks in a system task, giving them access
 * to system space as though they were legit system code. this is distinct
 * from userspace things, which will be restricted by IPC redirection.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <ccan/opt/opt.h>
#include <ccan/darray/darray.h>

#include <sneks/test.h>


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


/* CCAN opt is hilariously OP for this purpose. but since this is systest,
 * binary size doesn't really matter.
 */
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
	opt_register_table(opts, NULL);
	if(!opt_parse(&argc, argv, &ignore_opt_error)) {
		printf("*** option parsing failed!\n");
		return 1;
	}

	if(do_describe) describe_all_systests();
	if(!no_test) {
		if(darray_empty(only_ids)) run_all_systests();
		else run_systest_by_spec(only_ids.item, only_ids.size);
	}

	printf("*** systest completed.\n");
	return 0;
}
