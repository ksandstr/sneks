
/* exports within the root task program. */

#ifndef __ROOT_DEFS_H__
#define __ROOT_DEFS_H__

#include <stdbool.h>
#include <threads.h>
#include <l4/types.h>


#define THREAD_STACK_SIZE 4096


/* from mm.c. sends pre-heap sbrk()'d pages to sysmem and switches to sysmem
 * brk() handling.
 */
extern void mm_enable_sysmem(L4_ThreadId_t sysmem_tid);


/* from main.c. should be moved into mm.c, or boot.c, or some such. */

extern void send_phys_to_sysmem(
	L4_ThreadId_t sysmem_tid, bool self, L4_Word_t addr);


/* from thrd.c */

extern void rt_thrd_init(void);
extern void rt_thrd_tests(void);

/* FIXME: rename this to match the one in sys/crt (& later, userspace) */
extern L4_ThreadId_t thrd_tidof_NP(thrd_t t);


/* from uapi.c */

extern L4_ThreadId_t uapi_tid;	/* nil before uapi activation */

extern int add_task(int pid, L4_Fpage_t kip_area, L4_Fpage_t utcb_area);
extern L4_ThreadId_t allocate_thread(int pid, void **utcb_loc_p);
extern void free_thread(L4_ThreadId_t tid, void *utcb_loc);
extern void lock_uapi(void);
extern void unlock_uapi(void);

extern int uapi_loop(void *param_ptr);

extern void uapi_init(void);


#endif
