
#include <stdbool.h>
#include <errno.h>
#include <signal.h>


static inline bool sig_valid(int signum) {
	return signum >= 1 && signum <= 64;
}


int sigemptyset(sigset_t *set)
{
	*set = 0;
	return 0;
}


int sigfillset(sigset_t *set)
{
	*set = ~0ull;	/* all aboard, choo choo */
	return 0;
}


int sigaddset(sigset_t *set, int signum)
{
	if(!sig_valid(signum)) {
		errno = EINVAL;
		return -1;
	}
	*set |= 1ull << (signum - 1);
	return 0;
}


int sigdelset(sigset_t *set, int signum)
{
	if(!sig_valid(signum)) {
		errno = EINVAL;
		return -1;
	}
	*set &= ~(1ull << (signum - 1));
	return 0;
}


int sigismember(sigset_t *set, int signum)
{
	if(!sig_valid(signum)) {
		errno = EINVAL;
		return -1;
	}
	return (*set >> (signum - 1)) & 1;
}
