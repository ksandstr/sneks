
/* exports within the root task program. */

#ifndef __ROOT_DEFS_H__
#define __ROOT_DEFS_H__

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <threads.h>
#include <ccan/list/list.h>
#include <ccan/bitmap/bitmap.h>
#include <ccan/darray/darray.h>

#include <l4/types.h>
#include <l4/kip.h>

#include <sneks/devcookie.h>


#define THREAD_STACK_SIZE 4096

/* flags in <struct process>. comment describes behaviour when set. */
#define PF_WAIT_ANY 1	/* sleeping in passive waitid(P_ANY, ...) */
#define PF_SAVED_MASK 2	/* signal delivery is in sigsuspend mode */


struct rangealloc;

typedef darray(L4_ThreadId_t) tidlist;


/* common portion for the L4.X2 address space configuration and thread
 * management.
 */
struct task_common {
	L4_Fpage_t kip_area, utcb_area;
	bitmap *utcb_free;
	tidlist threads;	/* .size == 0 indicates zombie */
};


/* userspace processes. allocated in ra_process. */
struct process
{
	struct task_common task;

	unsigned flags;	/* mishmash of PF_* */
	int ppid;		/* parent's PID */
	__uid_t real_uid, eff_uid, saved_uid;
	L4_ThreadId_t wait_tid;	/* waiting TID on parent */
	/* waitid() output values */
	short signo, status;
	int code;
	struct list_node dead_link;	/* in parent's dead_list */

	/* signal delivery. */
	uintptr_t sigpage_addr;
	uint64_t ign_set, dfl_set, mask_set, pending_set;
	uint64_t saved_mask_set;	/* for Proc::sigsuspend, cf. PF_SAVED_MASK */
	L4_ThreadId_t sighelper_tid;
	void *sighelper_utcb;

	/* rarely used things later down */
	struct list_head dead_list;	/* zombie children via ->dead_link */
};


/* TODO: move this else-the-fucking-where */
static inline uint64_t rdtsc(void) {
	uint64_t output;
	asm volatile ("rdtsc" : "=A" (output));
	return output;
}


/* from mm.c. sends pre-heap sbrk()'d pages to sysmem and switches to sysmem
 * brk() handling.
 */
extern void mm_enable_sysmem(L4_ThreadId_t sysmem_tid);


/* from main.c . */

extern L4_ThreadId_t vm_tid, sysmsg_tid, initrd_tid;
extern L4_KernelInterfacePage_t *the_kip;
extern struct cookie_key device_cookie_key;

extern void send_phys_to_sysmem(
	L4_ThreadId_t sysmem_tid, bool self, L4_Fpage_t fp);

extern L4_ThreadId_t spawn_systask_from_initrd(const char *path, ...);


/* from thrd.c */

extern void rt_thrd_init(void);
extern void rt_thrd_tests(void);

/* FIXME: rename this to match the one in sys/crt (& later, userspace) */
extern L4_ThreadId_t thrd_tidof_NP(thrd_t t);

extern int next_early_utcb_slot;	/* for add_systask() */


/* from random.c */
extern void random_init(uint64_t x);
extern void add_entropy(uint64_t x);
extern void generate_u(void *outbuf, size_t length);


/* from sig.c */
/* may raise muidl::NoReply */
extern void sig_send(struct process *p, int sig, bool self);

/* IDL stuff */
extern void root_uapi_sigconfig(
	L4_Word_t sigpage_addr,
	uint8_t tail_data[static 1024], unsigned *tail_data_len,
	int *handler_offset_p);

extern uint64_t root_uapi_sigset(
	int set_name, uint64_t or_bits, uint64_t and_bits);

extern int root_uapi_sigsuspend(uint64_t mask);
extern int root_uapi_kill(int pid, int sig);


/* from uapi.c */

extern L4_ThreadId_t uapi_tid;
extern struct rangealloc *ra_process;

#define IS_SYSTASK(pid) ((pid) >= SNEKS_MIN_SYSID)

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

extern struct process *get_process(int pid);
extern void sig_remove_helper(struct process *p);
extern void zombify(struct process *p);

extern int uapi_loop(void *param_ptr);
extern void uapi_init(void);


/* from device.c */

extern L4_ThreadId_t devices_tid;

extern int devices_loop(void *param_ptr);
extern void devices_init(void);


/* from fsio.c */

extern L4_ThreadId_t __get_rootfs(void);
extern FILE *sfdopen_NP(L4_ThreadId_t server, L4_Word_t handle, const char *mode);

/* NOTE: while these two are analoguous to fileno(3), they should instead
 * convert POSIX fds to the values returned. however root isn't POSIX so this
 * isn't much of an issue.
 */
extern L4_ThreadId_t fserver_NP(FILE *f);	/* these two are like fileno(3) */
extern int fhandle_NP(FILE *f);


#endif
