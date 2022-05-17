#ifndef _SNEKS_THRD_H
#define _SNEKS_THRD_H

#include <ccan/list/list.h>
#include <l4/types.h>

/* per-thread wait structure for cnd.c & mtx.c */
struct thrd_wait {
	L4_ThreadId_t tid;
	union {
		struct { struct list_node link; /* in __mtx_gubbins.waits */ } mtx;
		union { L4_Word_t next; } cnd;
	};
};

/* per-runtime interfacey bit for lib/thrd.c */
extern int __thrd_new(L4_ThreadId_t *);
extern int __thrd_destroy(L4_ThreadId_t);
extern const int __thrd_stksize_log2;

/* lib/thrd.c initializer */
extern void __thrd_init(void);

/* lib/tss.c interface to thrd, named according to defining module */
extern void __tss_on_exit(void *);
extern void *__thrd_get_tss(void);
extern void __thrd_set_tss(void *);

/* same for lib/{cnd,mtx}.c */
extern struct thrd_wait *__thrd_get_wait(void);
extern void __thrd_put_wait(struct thrd_wait *);

#endif
