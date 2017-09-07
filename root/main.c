
#include <stdio.h>
#include <stdlib.h>


int main(void)
{
	printf("hello, world!\n");
	void *foo = malloc(128);
	printf("foo=%p\n", foo);
	free(foo);
	return 0;
}
