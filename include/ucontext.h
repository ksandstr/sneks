#ifndef _UCONTEXT_H
#define _UCONTEXT_H

#include <signal.h>
#include <sys/ucontext.h>

extern int getcontext(ucontext_t *)
	__attribute__((__returns_twice__));
extern _Noreturn void setcontext(const ucontext_t *);

#endif
