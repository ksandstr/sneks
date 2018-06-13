
#ifndef _SNEKS_PROCESS_H
#define _SNEKS_PROCESS_H

#include <l4/types.h>


/* namespace pollution guard. in a standard environment, userspace programs
 * shouldn't be impacted by this stuff.
 */
#ifdef __SNEKS__

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

#endif


extern unsigned pidof_NP(L4_ThreadId_t tid);

/* execve() without the fork(). returns pidof_NP() of the created task's
 * main() TID.
 */
extern int spawn_NP(const char *filename,
	char *const argv[], char *const envp[]);

#endif
