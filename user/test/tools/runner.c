
/* tool program that runs commands from an input script. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <ccan/str/str.h>


int main(int argc, char *argv[])
{
	if(argc < 1) {
		fprintf(stderr, "%s: no argument!\n", argv[0]);
		return EXIT_FAILURE;
	}

	int fd = open(argv[1], O_RDONLY);
	FILE *f = fd >= 0 ? fdopen(fd, "r") : NULL;
	if(f == NULL) {
		fprintf(stderr, "%s: can't open `%s': errno=%d\n", argv[0],
			argv[1], errno);
		return EXIT_FAILURE;
	}

	char line[100];
	while(fgets(line, sizeof line, f) != NULL) {
		if(line[0] == '#') continue;

		line[sizeof line - 1] = '\0';
		int len = strlen(line);
		while(len > 0 && line[len - 1] == '\n') line[--len] = '\0';

		char *brk = strpbrk(line, " \t\r\n");
		if(brk != NULL) *(brk++) = '\0';
		if(streq(line, "exit")) {
			int rc = 0;
			if(brk != NULL) rc = strtoul(brk, NULL, 10);
			exit(rc);
		} else if(streq(line, "exec")) {
			if(brk == NULL) {
				fprintf(stderr, "no parameter for exec?\n");
				continue;
			}
			execlp(brk, brk, NULL);
			fprintf(stderr, "exec failed, errno=%d\n", errno);
		} else {
			fprintf(stderr, "unknown command `%s'\n", line);
			/* and proceed for the lulz */
		}
	}

	fclose(f);
	return 0;
}
