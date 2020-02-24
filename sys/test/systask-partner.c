
/* like spawn-partner.c, but for systasks and root's selftest stuff.
 * could be made conditional on BUILD_SELFTEST, but so far no.
 */

#include <l4/types.h>
#include <l4/ipc.h>


int main(void)
{
	L4_ThreadId_t sender;
	L4_Accept(L4_UntypedWordsAcceptor);
	L4_MsgTag_t tag = L4_Wait(&sender);
	if(L4_IpcFailed(tag)) return 1;

	L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 1 }.raw);
	L4_LoadMR(1, 0x87654321);
	tag = L4_Reply(sender);
	return L4_IpcFailed(tag) ? 1 : 0;
}
