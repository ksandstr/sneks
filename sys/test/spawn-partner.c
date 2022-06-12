/* userspace collaborator program for systemspace tests. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ccan/array_size/array_size.h>
#include <ccan/str/str.h>
#include <l4/types.h>
#include <l4/ipc.h>
#include <sneks/api/io-defs.h>

static unsigned long envul(const char *name) {
	char *s = getenv(name);
	if(s == NULL) exit(1);
	return strtoull(s, NULL, 0);
}

static int send_status(int n) {
	L4_ThreadId_t dest = { .raw = envul("RETURN") };
	L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 1, .X.label = 0xfddb }.raw);
	L4_LoadMR(1, n);
	return L4_IpcSucceeded(L4_Send(dest)) ? 0 : L4_ErrorCode();
}

static int fork_and_exit(void) /* from uapi:lifecycle */
{
	int child = fork();
	if(child < 0) {
		perror("fork");
		return 1;
	} else if(child == 0) {
		exit(666);
	} else {
		int st, n = waitpid(child, &st, 0);
		if(n < 0) perror("waitpid");
		return child;
	}
}

static int dup_to_wildcard(void) /* from io:handle */
{
	L4_ThreadId_t server = { .raw = envul("SERVER") };
	int handle = envul("HANDLE");
	return send_status(__io_touch(server, handle));
}

int main(int argc, char *argv[])
{
	static const struct { const char *name; int (*fn)(void); } modes[] = {
		{ "fork-and-exit", &fork_and_exit },
		{ "dup-to-wildcard", &dup_to_wildcard },
	};
	for(int i=0; i < ARRAY_SIZE(modes) && argc > 1; i++) {
		if(streq(modes[i].name, argv[1])) return (*modes[i].fn)();
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
