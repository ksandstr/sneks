#ifndef _THREADS_H
#define _THREADS_H

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>

typedef _Atomic uintptr_t mtx_t;
typedef _Atomic uintptr_t cnd_t;

enum {
	mtx_plain = 0,
	mtx_recursive = 1,
	mtx_timed = 2,
};

enum {
	thrd_success = 0,
	thrd_timedout,
	thrd_busy,
	thrd_error,
	thrd_nomem,
};

struct timespec;

typedef void (*tss_dtor_t)(void *);
typedef int tss_t;	/* may as well. */

typedef int (*thrd_start_t)(void *);
typedef int thrd_t;

#define ONCE_FLAG_INIT 0

typedef _Atomic int once_flag;
extern void call_once(once_flag *flag, void (*func)(void));

typedef int thrd_t;
extern thrd_t thrd_current(void);
extern int thrd_create(thrd_t *thr, thrd_start_t fn, void *arg);
extern _Noreturn void thrd_exit(int res);
extern int thrd_join(thrd_t thr, int *res_p);

extern int mtx_init(mtx_t *mtx, int type);
extern void mtx_destroy(mtx_t *mtx);
extern int mtx_lock(mtx_t *mtx);
extern int mtx_trylock(mtx_t *mtx);
extern int mtx_timedlock(mtx_t *mtx, const struct timespec *ts);
extern int mtx_unlock(mtx_t *mtx);

extern int cnd_init(cnd_t *cond);
extern int cnd_signal(cnd_t *cond);
extern int cnd_broadcast(cnd_t *cond);
extern int cnd_wait(cnd_t *cond, mtx_t *mutex);
extern int cnd_timedwait(cnd_t *cond, mtx_t *mutex, const struct timespec *timeo);
extern void cnd_destroy(cnd_t *cond);

extern int tss_create(tss_t *key, tss_dtor_t dtor);
extern void tss_delete(tss_t key);
extern void *tss_get(tss_t key);
extern void tss_set(tss_t key, void *ptr);

#endif
