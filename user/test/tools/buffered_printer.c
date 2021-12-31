/* collaborator of cstd/buf. */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <assert.h>
#include <ccan/str/str.h>

int main(int argc, char *argv[])
{
	if(argc < 3) return EXIT_FAILURE;
	const char *buftype = argv[1], *altreg = argv[2];
	//fprintf(stderr, "buftype=`%s', altreg=`%s'\n", buftype, altreg);
	int mode;
	if(streq(buftype, "block")) mode = _IOFBF;
	else if(streq(buftype, "line")) mode = _IOLBF;
	else if(streq(buftype, "none")) mode = _IONBF;
	else {
		assert(streq(buftype, "default"));
		mode = -1;
	}
	if(mode >= 0) {
		if(streq(altreg, "alt")) {
			char *buffer = malloc(BUFSIZ);
			assert(buffer != NULL);
			switch(mode) {
				case _IOFBF: setbuf(stdout, buffer); break;
				case _IOLBF: setlinebuf(stdout); break;
				case _IONBF: setbuf(stdout, NULL); break;
				default: assert(false);
			}
		} else {
			int n = setvbuf(stdout, NULL, mode, 0);
			if(n < 0) {
				fprintf(stderr, "setvbuf() failed w/ errno=%d\n", errno);
				return EXIT_FAILURE;
			}
		}
	}

	printf("first ");
	char c; read(STDIN_FILENO, &c, 1);
	printf("second\n");
	read(STDIN_FILENO, &c, 1);

	return EXIT_SUCCESS;
}
