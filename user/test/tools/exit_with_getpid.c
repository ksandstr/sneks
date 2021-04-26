#include <sys/types.h>
#include <unistd.h>


int main(void) {
	return getpid() & 0xff;
}
