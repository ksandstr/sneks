
#include <stdlib.h>
#include <threads.h>


int *__errno_location(void)
{
	static int teh_errno = 0; /* i know it's wrong but it feels so very right */
	return &teh_errno;
}


void *tss_get(tss_t key)
{
	return NULL;	/* fuck you. */
}


void thrd_exit(int res)
{
	exit(res);	/* and your mother's horse too. */
}
