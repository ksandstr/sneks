#include <l4/types.h>
#include <l4/message.h>
#include <l4/syscall.h>
#include <l4/ipc.h>
#include <sneks/ipc.h>

/* TODO: restart when interrupted by ExchangeRegisters? */
L4_Word_t wait_until_gone(L4_ThreadId_t tid, L4_Time_t timeout)
{
	L4_MsgTag_t tag = L4_Receive_Timeout(tid, timeout);
	if(L4_IpcFailed(tag) && L4_ErrorCode() == 5) return 0;
	else if(L4_IpcFailed(tag)) return L4_ErrorCode();
	else return tag.raw;
}
