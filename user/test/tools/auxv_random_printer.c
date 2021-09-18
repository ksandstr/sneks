#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <sys/auxv.h>
int main(void) {
	errno = 0;
	const uint8_t *r = (void *)getauxval(AT_RANDOM);
	if(errno == ENOENT || r == NULL) return 0;
	for(int i=0; i < 16; i+=3) {
		long l = r[i];
		if(i + 1 < 16) l |= (long)r[i + 1] << 8;
		if(i + 2 < 16) l |= (long)r[i + 2] << 16;
		puts(l64a(l));
	}
	return 0;
}
