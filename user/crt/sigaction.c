
#include <stdbool.h>
#include <signal.h>
#include <errno.h>

#include <l4/types.h>
#include <l4/ipc.h>


int sigaction(
	int signum,
	const struct sigaction *act,
	struct sigaction *oldact)
{
	errno = ENOSYS;
	return -1;
}


sighandler_t signal(int signum, sighandler_t handler)
{
	/* BSD semantics, please. just like glibc 2 and later. */
	struct sigaction old, act = {
		.sa_handler = handler,
		.sa_flags = SA_RESTART,
	};
	int n = sigaction(signum, &act, &old);
	return n < 0 ? SIG_ERR : old.sa_handler;
}
