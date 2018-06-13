
/* tests on access to initrd data from within systests. */

#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <ccan/minmax/minmax.h>

#include <l4/types.h>
#include <l4/thread.h>
#include <l4/ipc.h>

#include <sneks/process.h>
#include <sneks/test.h>


START_LOOP_TEST(open_file_and_read, iter, 0, 1)
{
	const bool two_pieces = !!(iter & 1);
	diag("two_pieces=%s", btos(two_pieces));
	plan_tests(4);

	int fd = open("/initrd/systest/sys/test/hello.txt", O_RDONLY);
	if(!ok1(fd > 0)) {
		diag("open(2) failed, errno=%d", errno);
	}

	char buffer[200];
	int n;
	if(!two_pieces) n = read(fd, buffer, sizeof buffer - 1);
	else {
		n = read(fd, buffer, 6);
		if(n == 6) {
			int m = read(fd, &buffer[6], sizeof buffer - 7);
			if(m < 0) n = m; else n += m;
		}
	}
	if(!ok1(n >= 0)) {
		diag("read(2) failed, errno=%d", errno);
	}
	buffer[max(n, 0)] = '\0';
	if(!ok1(strcmp(buffer, "hello, world\n") == 0)) {
		diag("n=%d, buffer=`%s'", n, buffer);
	}

	n = close(fd);
	if(!ok(n == 0, "close(2)")) {
		diag("close(2) failed, errno=%d", errno);
	}
}
END_TEST


START_TEST(spawn_into_userspace)
{
	plan_tests(3);
	todo_start("no implementation");

	char my_id[100];
	snprintf(my_id, sizeof my_id, "%#lx", L4_Myself().raw);
	char *argv[] = { "initrd_spawn_partner", my_id, NULL }, *envp[] = { NULL };
	int cpid = spawn_NP("/initrd/systest/sys/test/initrd_spawn_partner",
		argv, envp);
	if(!ok(cpid > 0, "spawn succeeded")) diag("errno=%d", errno);

	L4_ThreadId_t sender = L4_nilthread;
	bool child_died = false;
	L4_MsgTag_t tag = L4_Wait_Timeout(L4_TimePeriod(50 * 1000), &sender);
	if(!ok(L4_IpcSucceeded(tag), "got IPC")) {
		diag("ec=%#lx", L4_ErrorCode());
		child_died = true;
	}
	if(!ok(pidof_NP(sender) == cpid, "IPC was from child")) {
		if(L4_IpcSucceeded(tag)) {
			diag("sender=%lu:%lu (pid %d)",
				L4_ThreadNo(sender), L4_Version(sender), pidof_NP(sender));
		}
	}

	if(L4_IpcSucceeded(tag)) {
		L4_LoadMR(0, 0);
		tag = L4_Reply(sender);
		if(L4_IpcFailed(tag)) {
			diag("reply failed, ec=%#lx", L4_ErrorCode());
		}
	}

	if(cpid > 0 && !child_died) {
		/* systasks can't wait on children so instead we'll sleep until the
		 * partner's main thread has gone.
		 */
		do {
			tag = L4_Receive_Timeout(sender, L4_Never);
		} while(!L4_IpcFailed(tag) || L4_ErrorCode() != 5);
	}
}
END_TEST


SYSTEST("systest:initrd", open_file_and_read);
SYSTEST("systest:initrd", spawn_into_userspace);
