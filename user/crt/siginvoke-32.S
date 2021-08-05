
#include <asm/ucontext-offsets.h>

# __invoke_sig_fast and __invoke_sig_slow, as used from sig_bottom() in
# user/crt/sigaction.c, but also __invoke_sig_sync() for direct calling
# when sig_bottom() runs from e.g. sigprocmask(SIG_UNBLOCK, ...).


	.text
	.global __invoke_sig_sync
	.type __invoke_sig_sync, @function
	.align 64,0

# given regparms %eax=sig, produce (sig, flags, ip) on the stack and
# proceed as though asynchronous signal delivery had occurred.
__invoke_sig_sync:
	pushf
	pushl %eax
	jmp __invoke_sig_fast


	.global __invoke_sig_fast
	.type __invoke_sig_fast, @function
	.align 16,0

__invoke_sig_fast:
	# TODO: just like the slow side, but without the MR/BR saving stuff.
	# the clue is that we're running on top of an aborted IPC operation,
	# which invalidates both MRs and BRs, and also the per-thread errno;
	# so they need not be reloaded even if the signal handler does IPC
	# or causes errno smash via syscall.
	jmp __invoke_sig_slow


	.text
	.global __invoke_sig_slow
	.type __invoke_sig_slow, @function
	.align 64,0

# this routine is invoked with the parameters in front of the return address,
# rather than after, so that it can popf;ret for an epilogue.
__invoke_sig_slow:	# (0:sig, 4:flags, 8:ip)
	subl $ucontext_t_size, %esp	# allomacate us an ucontext
	movl %edi, o_uc_mcontext + o_gregs + oEDI(%esp)
	movl %esi, o_uc_mcontext + o_gregs + oESI(%esp)
	movl %ebp, o_uc_mcontext + o_gregs + oEBP(%esp)
	leal ucontext_t_size + 12(%esp), %esi # pop params from stored %esp
	movl %esi, o_uc_mcontext + o_gregs + oESP(%esp)
	movl %ebx, o_uc_mcontext + o_gregs + oEBX(%esp)
	movl %edx, o_uc_mcontext + o_gregs + oEDX(%esp)
	movl %ecx, o_uc_mcontext + o_gregs + oECX(%esp)
	movl %eax, o_uc_mcontext + o_gregs + oEAX(%esp)
	movl ucontext_t_size + 8(%esp), %esi
	movl %esi, o_uc_mcontext + o_gregs + oEIP(%esp)
	movl $0, o_uc_link(%esp)
	# TODO: fill rest of ucontext_t here
	movl %gs:0, %ebx	# UTCB
	# stash errno at very bottom.
	call __errno_location
	movl %esp, %edx	# second regparm: ucontext_t ptr
	pushl (%eax)
	movl ucontext_t_size+4(%esp), %eax # first regparm: signum
	# then vregs in ascending order.
	pushl -40(%ebx)	# cop/preempt flags
	pushl -36(%ebx)	# errorcode
	pushl -32(%ebx) # xfertimeouts
	pushl -28(%ebx) # intendedreceiver
	pushl -24(%ebx) # virtualsender/actualsender
	pushl -20(%ebx) # threadword1
	pushl -16(%ebx) # threadword0
	# store MRs and BRs.
	subl $0x184, %esp	# 64 + 33 words
	movl %ebx, %esi		# address of MR0..MR63
	movl %esp, %edi		# start of storage area
	movl $64, %ecx
	rep movsl (%esi), (%edi)
	leal -192(%ebx), %esi	# address of BR32..BR0
	movl $33, %ecx
	rep movsl (%esi), (%edi)
	# invoke signal handling
	call __sig_invoke
	# restore MRs and BRs.
	# NOTE: retention of UTCB pointer in %ebx means that userspace threads
	# may not return from __sig_invoke on a different kernel thread than
	# it was on at entry. longjmp() and setcontext() are fine.
	movl %ebx, %edi
	movl %esp, %esi
	movl $64, %ecx
	rep movsl (%esi), (%edi)
	leal -192(%ebx), %edi
	movl $33, %ecx
	rep movsl (%esi), (%edi)
	addl $0x184, %esp
	# restore vregs.
	popl -16(%ebx)
	popl -20(%ebx)
	popl -24(%ebx)
	popl -28(%ebx)
	popl -32(%ebx)
	popl -36(%ebx)
	popl -40(%ebx)
	# restore errno.
	call __errno_location
	popl (%eax)
	# restore GPRs. construct flags+eip in context %esp to permit
	# swapcontext() funny business.
	movl o_uc_mcontext + o_gregs + oESP(%esp), %eax
	movl ucontext_t_size + 4(%esp), %edi
	movl o_uc_mcontext + o_gregs + oEIP(%esp), %esi
	subl $8, %eax
	movl %esi, 4(%eax)	# ucontext_t eip
	movl %edi, (%eax)	# flags
	movl o_uc_mcontext + o_gregs + oEDI(%esp), %edi
	movl o_uc_mcontext + o_gregs + oESI(%esp), %esi
	movl o_uc_mcontext + o_gregs + oEBP(%esp), %ebp
	movl o_uc_mcontext + o_gregs + oEBX(%esp), %ebx
	movl o_uc_mcontext + o_gregs + oEDX(%esp), %edx
	movl o_uc_mcontext + o_gregs + oECX(%esp), %ecx
	movl o_uc_mcontext + o_gregs + oEAX(%esp), %esp
	xchgl %esp, %eax
	# return w/ flags
	popf
	ret
