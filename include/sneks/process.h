
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


/* for passing file descriptors through spawn and exec: the first fdlist
 * item's address is given in the startup routine's stack pointer, or 0 if
 * there are none. in any given fd list, ->fd appears in descending order, so
 * that the extent of the file descriptor table can be determined by reading
 * the first fdlist item; and no ->fd may appear twice.
 *
 * this should be defined in IDL so that Proc::spawn could accept a sequence
 * thereof, but since LLVM is a bit fucky wrt sizeof on struct types (i can't
 * pull a targetref from anywhere!), it's here instead.
 */
struct sneks_fdlist
{
	unsigned short next; /* # of bytes from start to next item, or 0 to end */
	unsigned short fd;
	L4_ThreadId_t serv;
	L4_Word_t cookie;
} __attribute__((packed));


static inline struct sneks_fdlist *sneks_fdlist_next(
	struct sneks_fdlist *cur)
{
	return (void *)cur + cur->next;
}

#endif


extern unsigned pidof_NP(L4_ThreadId_t tid);

/* execve() without the fork(). returns pidof_NP() of the created task's
 * main() TID.
 */
extern int spawn_NP(const char *filename,
	char *const argv[], char *const envp[]);

#endif
