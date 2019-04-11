
#include <stdbool.h>
#include <errno.h>
#include <signal.h>


static bool sig_valid(int signum) {
	if(signum <= 0 || signum > 63) return false;
	return true;
}


int sigemptyset(sigset_t *set)
{
	*set = 0;
	return 0;
}


int sigfillset(sigset_t *set)
{
	*set = ~0ull;	/* should we exclude invalid signals? */
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
