
/* C11 condition variables. */

#include <threads.h>
#include <stdatomic.h>


int cnd_init(cnd_t *cond)
{
	return thrd_error;
}


void cnd_destroy(cnd_t *cond)
{
}


int cnd_signal(cnd_t *cond)
{
	return thrd_error;
}


int cnd_broadcast(cnd_t *cond)
{
	return thrd_error;
}


int cnd_wait(cnd_t *cond, mtx_t *mutex)
{
	return thrd_error;
}


int cnd_timedwait( cnd_t *cond, mtx_t *mutex, const struct timespec *timeo)
{
	return thrd_error;
}
