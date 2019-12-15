
#ifndef _UCONTEXT_H
#define _UCONTEXT_H

#include <stdint.h>


/* TODO: get this from an arch-specific header. */
typedef struct {
	/* format details follow __invoke_sig_slow()'s convenience. IPC-written
	 * registers (MRs, vs/as, ir, ec, tw[01], and CPU registers except %edi
	 * and %eip) are not preserved when signal handling is entered by
	 * interrupting a waiting IPC, since the kernel was already given
	 * permission to smash them.
	 *
	 * the foocontext family save and restore only callee-saved registers. the
	 * restoring side also sets BR0 accept untyped words only.
	 */
	uint32_t mrs[64], brs[33];
	uint32_t tw0, tw1, vsas, ir, xferto, ec, cpe;	/* UTCB vregs */
	uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax; /* pushal order */
	uint32_t _errno, eflags, eip;
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
