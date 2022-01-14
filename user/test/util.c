#include <sys/stat.h>
#include <sneks/test.h>

static unsigned fac_worker(unsigned acc, unsigned x) {
	if(x == 0) return acc; else return fac_worker(acc * x, x - 1);
}

unsigned factorial(unsigned x) {
	if(x == 0) return 0;
	return fac_worker(1, x);
}

void gen_perm(unsigned *buf, unsigned n, unsigned perm)
{
	if(n == 0) return;
	for(unsigned i=0; i < n; i++) buf[i] = i;
	for(unsigned i=0; i < n - 1; i++) {
		unsigned x = perm % (n - i);
		perm /= (n - i);
		unsigned t = buf[i];
		buf[i] = buf[i + x];
		buf[i + x] = t;
	}
}

bool test_e(const char *pathspec) {
	return stat(pathspec, &(struct stat){ }) == 0;
}
