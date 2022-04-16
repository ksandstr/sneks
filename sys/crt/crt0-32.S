	.text
	.global _start

	.align 16
_start:
	leal __stktop - 128, %esp	# don't stomp <struct thrd>
	xorl %ebp, %ebp			# terminate frame chain
	call __crt1_entry
	movl %eax, (%esp)
	call exit
	int $3

# TODO: arrange __crt1_entry to call main() on a separate stack, which isn't
# the initialization stack; then reduce the latter (this one here) to like 512
# bytes or something.
	.global __stktop
	.bss
	.align 64*1024
	.space 64*1024
__stktop:
