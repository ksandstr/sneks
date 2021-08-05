# setcontext(3) and getcontext(3) parts that deal with mcontext_t, which is
# otherwise dicky to pull out of ucontext_t.

#include <asm/ucontext-offsets.h>

	.text
	.global getcontext
	.type getcontext, @function
	.align 16,0

getcontext:
	movl 4(%esp), %eax	# ucontext_t *parameter
	addl $(o_uc_mcontext + o_gregs), %eax	# mcontext_t.gregs therein
	movl %edi, oEDI(%eax)
	movl %esi, oESI(%eax)
	movl %ebp, oEBP(%eax)
	leal 4(%esp), %ecx
	movl %ecx, oESP(%eax)
	movl %ebx, oEBX(%eax)
	movl (%esp), %ecx
	movl %ecx, oEIP(%eax)
	ret


	.text
	.global setcontext
	.type setcontext, @function
	.align 16,0

setcontext:
	movl 4(%esp), %eax	# ucontext_t *parameter
	addl $(o_uc_mcontext + o_gregs), %eax	# mcontext_t.gregs therein
	movl oEDI(%eax), %edi
	movl oESI(%eax), %esi
	movl oEBP(%eax), %ebp
	movl oESP(%eax), %esp
	movl oEBX(%eax), %ebx
	jmp *oEIP(%eax)
