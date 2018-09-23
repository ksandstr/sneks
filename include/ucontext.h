
#ifndef _UCONTEXT_H
#define _UCONTEXT_H

#include <stdint.h>


/* TODO: get this from an arch-specific header. */
typedef struct {
	uint32_t eax, ebx, ecx, edx, esi, edi, ebp, esp, eip;
} __attribute__((__packed__)) mcontext_t;


typedef struct ucontext
{
	struct ucontext *link;
	mcontext_t mcontext;
} __attribute__((__packed__)) ucontext_t;


extern int getcontext(ucontext_t *ucp);
extern int setcontext(const ucontext_t *ucp)
	__attribute__((__noreturn__));

#endif
