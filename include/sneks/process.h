#ifndef _SNEKS_PROCESS_H
#define _SNEKS_PROCESS_H

#include <stddef.h>
#include <l4/types.h>

#define SNEKS_MAX_PID 32767
#define SNEKS_MIN_SYSID 50000	/* for legibility */

/* on the interpretation of L4_Version(a global tid).
 *
 * an intended effect of the PID encoding is that tasks initially spawned by
 * the microkernel, having version=1 in their thread IDs, translate to PID 0
 * without special casing because their thread number field doesn't extend
 * past 64k. this includes the L4.X2 special interrupt thread encoding.
 */
#define SNEKS_PID_V_MASK 1	/* userspace tid iff (v & v_mask) == v_value */
#define SNEKS_PID_V_VALUE 1	/* ...1 */
#define SNEKS_SYS_V_MASK 3	/* systask -''- */
#define SNEKS_SYS_V_VALUE 2	/* ..10 */

/* the mask of bits from L4_ThreadNo(an userspace tid), and the amount of
 * right shift required to put them in the correct position within the
 * extracted process ID.
 */
#define SNEKS_PID_T_MASK 0x30000
#define SNEKS_PID_T_SHIFT 3

/* process lifecycle notification using MSGB_PROCESS_LIFECYCLE (via
 * <sneks/msg.h>).
 *
 * three things can be received notifications about. the first is process
 * creation via fork(2), the second is process image replacement via the
 * exec(2) family, and the third is process exit. the related message formats
 * are as follows:
 *   - fork: parent-pid, 0 | child-pid << 8
 *   - exec: process-pid, 1
 *   - exit: process-pid, 2 | signo << 8, waitstatus, exitcode
 *
 * note that process creation via spawn_NP(2) isn't signaled; rather, file
 * descriptor duplication etc. happens explicitly.
 */
#define MPL_FORK 0	/* these are low 8 bits of body[1] */
#define MPL_EXEC 1
#define MPL_EXIT 2

/* structure at the front of the process startup stack at startup. see
 * usr/crt/posix.c for details. (TODO: document those details here. also this
 * should be in IDL but isn't because LLVM.)
 */
struct sneks_startup_fd {
	size_t fd_flags;	/* fd in 15..0, others FF_* */
	size_t serv;		/* raw L4_ThreadId_t */
	size_t handle;
} __attribute__((packed));

#define FF_CWD (1 << 16)	/* this descriptor is __cwd_fd */

extern unsigned pidof_NP(L4_ThreadId_t tid);

/* execve() without the fork(). returns pidof_NP() of the created task's
 * main() TID.
 */
extern int spawn_NP(const char *filename, char *const argv[], char *const envp[]);

#endif
