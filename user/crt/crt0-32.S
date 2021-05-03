	.text
	.global _start

	.align 16
_start:
	movl %esp, %ebx	# capture argpos
	movl $__crt0_stktop - 128, %esp	# don't stomp <struct thrd>
	xorl %ebp, %ebp # stack frame list sentinel
	movl %ebx, (%esp)
	call __crt1_entry
	movl %eax, (%esp)
	call exit
	int $3

# two pages' worth is enough for crt1.c to set up us the heap and switch to a
# malloc'd stack. one page would likely do.
	.global __crt0_stktop
	.bss
	.align 8192
	.space 8192
__crt0_stktop:
