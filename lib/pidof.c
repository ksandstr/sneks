
/* the pidof_NP() routine, specifying the way sneks assigns process and thread
 * IDs on a 32-bit target.
 */

#include <string.h>
#include <l4/types.h>
#include <l4/thread.h>
#include <ccan/likely/likely.h>

#include <sneks/process.h>


unsigned pidof_NP(L4_ThreadId_t tid)
{
	if(unlikely(L4_IsLocalId(tid))) tid = L4_MyGlobalId();
	L4_Word_t v = L4_Version(tid), n = L4_ThreadNo(tid);
	if(likely((v & SNEKS_PID_V_MASK) == SNEKS_PID_V_VALUE)) {
		/* userspace PIDs. */
		return (v >> (ffsl(~SNEKS_PID_V_MASK) - 1))
			| ((n & SNEKS_PID_T_MASK) >> SNEKS_PID_T_SHIFT);
	} else if((v & SNEKS_SYS_V_MASK) == SNEKS_SYS_V_VALUE) {
		/* systask IDs with simple encoding. */
		return SNEKS_MIN_SYSID + (v >> (ffsl(~SNEKS_SYS_V_MASK) - 1));
	} else {
		/* other suffixes would go here: ...100, etc. */
		return 0;
	}
}
