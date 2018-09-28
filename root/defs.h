
/* exports within the root task program. */

#ifndef __ROOT_DEFS_H__
#define __ROOT_DEFS_H__

#include <stdbool.h>
#include <threads.h>

#include <l4/types.h>
#include <l4/kip.h>


#define THREAD_STACK_SIZE 4096


/* from mm.c. sends pre-heap sbrk()'d pages to sysmem and switches to sysmem
 * brk() handling.
 */
extern void mm_enable_sysmem(L4_ThreadId_t sysmem_tid);


/* from main.c. should be moved into mm.c, or boot.c, or some such. */

extern L4_KernelInterfacePage_t *the_kip;

extern void send_phys_to_sysmem(
	L4_ThreadId_t sysmem_tid, bool self, L4_Fpage_t fp);


/* from thrd.c */

extern void rt_thrd_init(void);
extern void rt_thrd_tests(void);

/* FIXME: rename this to match the one in sys/crt (& later, userspace) */
extern L4_ThreadId_t thrd_tidof_NP(thrd_t t);


/* from uapi.c */

extern L4_ThreadId_t uapi_tid;
extern L4_ThreadId_t vm_tid;

/* adds tracking structures for systasks as they're either registered (as for
 * root and sysmem) or spawned (for filesystems, drivers, and so forth; after
 * UAPI has been launched). creates nothing in the microkernel or sysmem.
 * return value is negative errno on failure, @pid if >= SNEKS_MIN_SYSID, or a
 * freshly-allocated systask PID when @pid < 0.
 *
 * contains some special behaviour for root and sysmem bring-up, respectively.
 * when @pid is the minimal sysid, the caller thread is added to the created
 * task's thread list. if @pid is the same as the current thread's pager's
 * PID, the pager is added to the created task's thread list.
 */
extern int add_systask(int pid, L4_Fpage_t kip_area, L4_Fpage_t utcb_area);

extern L4_ThreadId_t allocate_thread(int pid, void **utcb_loc_p);
extern void lock_uapi(void);
extern void unlock_uapi(void);

extern int uapi_loop(void *param_ptr);

extern void uapi_init(void);


#endif
