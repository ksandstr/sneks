
#include <stdio.h>
#include <stdlib.h>

#include <l4/types.h>
#include <l4/ipc.h>


int main(int argc, char *argv[])
{
	L4_ThreadId_t oth = { .raw = strtoul(argv[1], NULL, 0) };
	if(L4_IsNilThread(oth)) {
		printf("oth parsed to nil?\n");
		return 1;
	}

	L4_LoadMR(0, 0);
	L4_MsgTag_t tag = L4_Call_Timeouts(oth, L4_TimePeriod(3000), L4_Never);
	if(L4_IpcFailed(tag)) {
		printf("call failed, ec=%lu\n", L4_ErrorCode());
		return 1;
	}

	return 0;
}
