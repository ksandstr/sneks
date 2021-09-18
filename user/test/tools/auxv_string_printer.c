#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/auxv.h>
int main(int argc, char *argv[]) {
	errno = 0;
	unsigned long value = getauxval(strtoul(argv[1], NULL, 0));
	if(errno == ENOENT) return 1;
	else {
		puts((char *)value);
		return 0;
	}
}
