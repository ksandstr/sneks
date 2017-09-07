
/* exports within the root task program. */

#ifndef __ROOT_DEFS_H__
#define __ROOT_DEFS_H__

#include <stdbool.h>
#include <l4/types.h>


/* from mm.c. sends pre-heap sbrk()'d pages to sysmem and switches to sysmem
 * brk() handling.
 */
extern void mm_enable_sysmem(L4_ThreadId_t sysmem_tid);


/* from main.c. should be moved into mm.c, or boot.c, or some such. */

extern void send_phys_to_sysmem(
	L4_ThreadId_t sysmem_tid, bool self, L4_Word_t addr);


#endif
