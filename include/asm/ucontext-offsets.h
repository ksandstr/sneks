#ifndef _ASM_UCONTEXT_OFFSETS_H
#define _ASM_UCONTEXT_OFFSETS_H

#ifndef __i386__
#error "sneks <asm/ucontext-offsets.h> isn't valid for non-i386"
#endif

/* positions within ucontext_t */
#define o_uc_flags 0
#define o_uc_link 0x4
#define o_uc_stack 0x8
#define o_uc_mcontext 0x14
#define o_uc_sigmask 0x6c
#define o___fpregs_mem 0xec
#define ucontext_t_size (0xec + 0x70)

/* positions within mcontext_t */
#define o_gregs 0
#define o_fpregs 0x4c
#define o_oldmask 0x50
#define o_cr2 0x54

/* offsets into gregset_t */
#define __regoffs(n) ((n) * 4)
#define oGS __regoffs(0)
#define oFS __regoffs(1)
#define oES __regoffs(2)
#define oDS __regoffs(3)
#define oEDI __regoffs(4)
#define oESI __regoffs(5)
#define oEBP __regoffs(6)
#define oESP __regoffs(7)
#define oEBX __regoffs(8)
#define oEDX __regoffs(9)
#define oECX __regoffs(10)
#define oEAX __regoffs(11)
#define oTRAPNO __regoffs(12)
#define oERR __regoffs(13)
#define oEIP __regoffs(14)
#define oCS __regoffs(15)
#define oEFL __regoffs(16)
#define oUESP __regoffs(17)
#define oSS __regoffs(18)


#endif
