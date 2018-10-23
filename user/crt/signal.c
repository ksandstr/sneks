
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>

#include <l4/types.h>
#include <l4/ipc.h>

#include "proc-defs.h"
#include "private.h"


int pause(void)
{
	return 0;
}


int kill(int pid, int signum)
{
	errno = ENOSYS;
	return -1;
}
