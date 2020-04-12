
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <l4/types.h>
#include <l4/ipc.h>


int main(int argc, char *argv[])
{
	/* mode of uapi:lifecycle:notify_fork_exit */
	if(argc > 1 && strcmp(argv[1], "fork-and-exit") == 0) {
		int child = fork();
		if(child < 0) {
			fprintf(stderr, "fork failed, errno=%d\n", errno);
			return 1;
		} else if(child == 0) {
			exit(666);
		} else {
			int st, n = waitpid(child, &st, 0);
			if(n < 0) fprintf(stderr, "waitpid failed, errno=%d\n", errno);
			return child;
		}
	}

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
