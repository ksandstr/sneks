#ifndef _SNEKS_IPC_H
#define _SNEKS_IPC_H

#include <l4/types.h>

/* from lib/ipcutil.c */
/* wait until @tid is on longer a valid Ipc.fromspec. returns 0 if so,
 * L4_ErrorCode() on failure, and raw MsgTag on out-of-band reception.
 */
extern L4_Word_t wait_until_gone(L4_ThreadId_t tid, L4_Time_t timeout);

#endif
