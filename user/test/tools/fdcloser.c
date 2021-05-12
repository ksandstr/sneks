
/* file descriptor closer program, collaborates with cloexec of process:exec */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>

#ifdef __sneks__
#include <l4/types.h>
#include <sneks/api/io-defs.h>
#endif


int main(int argc, char *argv[])
{
	char *var;
	int rc = 0x40, n;

#ifdef __sneks__
	var = getenv("TESTSNEKSIOHANDLE");
	if(var == NULL) {
		printf("no TESTSNEKSIOHANDLE?\n");
		return EXIT_FAILURE;
	}

	/* TODO: use sscanf() */
	char *colon = strchr(var, ':'),
		*comma = strchr(colon != NULL ? colon + 1 : var, ',');
	if(colon != NULL) *colon = '\0';
	if(comma != NULL) *comma = '\0';
	unsigned long tno = strtoul(var, NULL, 0),
		version = colon != NULL ? strtoul(colon + 1, NULL, 0) : 0,
		handle = comma != NULL ? strtoul(comma + 1, NULL, 0) : 0;
	L4_ThreadId_t server = L4_GlobalId(tno, version);
	if(L4_IsLocalId(server)) {
		printf("%s: UwU what's this?\n", argv[0] != NULL ? argv[0] : "(wut)");
		return EXIT_FAILURE;
	}
	int newh;
	n = __io_dup(server, &newh, handle, 0);
	if(n != 0 && n != -EBADF) {
		printf("fdcloser: IO/dup returned unexpected n=%d", n);
	}
	rc |= n == 0 ? 2 : 0;
	if(n == 0) {
		n = __io_close(server, newh);
		if(n != 0) printf("fdcloser: IO/close returned unexpected n=%d", n);
	}
#endif

	var = getenv("TESTFD");
	if(var == NULL) {
		printf("no TESTFD?\n");
		return EXIT_FAILURE;
	}
	errno = 0;
	int fd = strtoul(var, NULL, 0);
	if(errno != 0) {
		printf("strtoul() failed, errno=%d\n", errno);
		return EXIT_FAILURE;
	}
	n = close(fd);
	rc |= n == 0 ? 1 : 0;
	if(n != 0 && errno != EBADF) {
		printf("fdclose: close(2) returned unexpected errno=%d\n", errno);
	}

	return rc;
}
