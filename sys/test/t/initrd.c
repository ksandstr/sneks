/* tests on access to initrd data from within the main systest program. */
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <ccan/str/str.h>
#include <ccan/minmax/minmax.h>

#include <l4/types.h>
#include <l4/thread.h>
#include <l4/ipc.h>

#include <sneks/process.h>
#include <sneks/ipc.h>
#include <sneks/test.h>

/* open(2) a file and read its contents. */
START_LOOP_TEST(open_file_and_read, iter, 0, 1)
{
	const bool two_pieces = !!(iter & 1);
	diag("two_pieces=%s", btos(two_pieces));
	plan_tests(4);

	int fd = open("/initrd/systest/sys/test/hello.txt", O_RDONLY);
	skip_start(!ok(fd > 0, "open(2)"), 4, "open(2) errno=%d", errno) {
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
		if(!ok(n >= 0, "read(2)")) diag("errno=%d", errno);
		buffer[max(n, 0)] = '\0';
		if(!ok1(streq(buffer, "hello, world\n"))) {
			diag("n=%d, buffer=`%s'", n, buffer);
		}

		n = close(fd);
		if(!ok(n == 0, "close(2)")) {
			diag("close(2) failed, errno=%d", errno);
		}
	} skip_end;
}
END_TEST

SYSTEST("systest:initrd", open_file_and_read);

/* test spawning of userspace programs. */
START_TEST(spawn_into_userspace)
{
	plan_tests(3);

	char my_id[100];
	snprintf(my_id, sizeof my_id, "%#lx", L4_Myself().raw);
	char *argv[] = { "initrd_spawn_partner", my_id, NULL }, *envp[] = { NULL };
	int cpid = spawn_NP("/initrd/systest/sys/test/initrd_spawn_partner", argv, envp);
	if(!ok(cpid > 0, "spawn succeeded")) diag("errno=%d", errno);

	L4_ThreadId_t sender = L4_nilthread;
	bool child_died = false;
	L4_MsgTag_t tag = L4_Wait_Timeout(L4_TimePeriod(50 * 1000), &sender);
	L4_Word_t ec = L4_ErrorCode();
	if(!ok(L4_IpcSucceeded(tag), "got IPC")) {
		diag("ec=%#lx", ec);
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
	/* clean up */
	if(cpid > 0 && !child_died) {
		while(wait_until_gone(sender, L4_Never) != 0) { /* rave, repeat */ }
	}
}
END_TEST

SYSTEST("systest:initrd", spawn_into_userspace);
