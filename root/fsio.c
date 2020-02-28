
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>


FILE *fopen(const char *path, const char *mode)
{
	errno = ENOSYS;
	return NULL;
}


#ifdef BUILD_SELFTEST

#include <sneks/test.h>
#include <sneks/systask.h>


START_TEST(fopen_trivial)
{
	plan(7);
	todo_start("no impl");

	FILE *f = fopen("/initrd/systest/root/fsiohello.txt", "rb");
	if(f == NULL) diag("fopen errno=%d", errno);
	skip_start(!ok(f != NULL, "fopen"), 6, "no file") {
		char str[100] = "";
		ok(fgets(str, sizeof str, f) != NULL, "fgets");
		ok(strcmp(str, "hello, root:fsio test\n") == 0, "line data");
		/* TODO: ok1(feof(f)) */

		ok(fseek(f, -5, SEEK_CUR) == 0, "fseek");
		memset(str, 0, sizeof str);
		ok(fgets(str, sizeof str, f) != NULL, "fgets");
		if(!ok(strcmp(str, "test\n") == 0, "short line data")) {
			diag("str=`%s'", str);
		}
		/* TODO: ok1(feof(f)) */

		if(!ok(fclose(f) == 0, "fclose")) diag("fclose errno=%d", errno);
	} skip_end;
}
END_TEST

SYSTASK_SELFTEST("root:fsio", fopen_trivial);

#endif
