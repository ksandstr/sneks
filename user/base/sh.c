
/* not a proper sh(1), but enough to resolve a relative hashbang
 * interpreter as required by execve.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <ccan/str/str.h>
#include <ccan/array_size/array_size.h>


int main(int argc, char *argv[])
{
	if(argc < 1 || !streq(argv[1], "-c")) {
		fprintf(stderr, "sh: you must be looking for a smarter /bin/sh\n");
		return EXIT_FAILURE;
	} else if(argc < 2) {
		fprintf(stderr, "sh: must have command_file\n");
		return EXIT_FAILURE;
	}

	const char *cmdfile = argv[2], *args[argc];
	int nargs = 0;
	args[nargs++] = argc >= 3 ? argv[3] : cmdfile;
	for(int i=4; i <= argc; i++) args[nargs++] = argv[i];
	args[nargs++] = NULL;
	assert(nargs <= ARRAY_SIZE(args));
	execvp(cmdfile, (char **)args);
	fprintf(stderr, "sh: can't execute `%s', errno=%d\n", cmdfile, errno);
	return EXIT_FAILURE;
}
