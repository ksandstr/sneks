
#ifndef SEEN_FAKE_CLIB_SETJMP_H
#define SEEN_FAKE_CLIB_SETJMP_H

#include <stdint.h>


typedef struct jmp_buf_s {
	/* NOTE: %esp points to the frame return address for longjmp convenience.
	 * transferring these registers to a mcontext_t requires compensation for
	 * this.
	 */
	uint32_t regs[6];		/* ebx, esi, edi, ebp, eip, esp */
} jmp_buf[1];

extern int setjmp(jmp_buf env);
extern _Noreturn void longjmp(jmp_buf env, int val);


#endif
