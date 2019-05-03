
/* straightforward sleeping routines for L4.X2. */

#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>

#include <l4/types.h>
#include <l4/ipc.h>


/* TODO: change these to receive-phase sleeps and make them explicitly
 * interruptable.
 */
unsigned int sleep(unsigned int seconds)
{
	/* TODO: use a TimePoint instead once those become available. */
	L4_Clock_t start = L4_SystemClock();
	L4_MsgTag_t tag = L4_Send_Timeout(L4_MyGlobalId(),
		L4_TimePeriod((uint64_t)seconds * 1000000));
	assert(L4_IpcFailed(tag));
	if(L4_ErrorCode() == 2) return 0;	/* send phase timeout */
	else {
		assert(L4_ErrorCode() == 6);	/* canceled in send phase */
		L4_Clock_t end = L4_SystemClock();
		return (end.raw - start.raw + 500000) / 1000000;
	}
}


/* see TODO for sleep() */
int usleep(__useconds_t usec)
{
	L4_MsgTag_t tag = L4_Send_Timeout(L4_MyGlobalId(), L4_TimePeriod(usec));
	assert(L4_IpcFailed(tag));
	if(L4_ErrorCode() == 2) return 0;	/* send phase timeout */
	else {
		assert(L4_ErrorCode() == 6);	/* canceled in send phase */
		errno = EINTR;
		return -1;
	}
}
