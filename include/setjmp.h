#ifndef _SETJMP_H
#define _SETJMP_H

#ifdef __i386__
typedef unsigned long __jmp_buf[6];
#else
#error "<setjmp.h> not defined for this target architecture"
#endif

/* TODO: use a <bits/__sigset_t.h> or some such for sigset_t __ss */
typedef struct __jmp_buf_tag {
	__jmp_buf __jb;
	unsigned long __fl;
	unsigned long __ss[128 / sizeof(long)];	/* same # as sigset_t size */
} jmp_buf[1];


extern int setjmp(jmp_buf env);
extern _Noreturn void longjmp(jmp_buf env, int val);

#endif
