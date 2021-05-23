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

# two pages' worth should be well enough for whatever systasks main()s do.
	.global __stktop
	.bss
	.align 8192
	.space 8192
__stktop:
