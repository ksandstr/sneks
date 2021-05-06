	.text
	.global _start

	.align 16
_start:
	movl $__stktop - 128, %esp	# don't stomp <struct thrd>
	call __crt1_entry
	pushl %eax
	call exit
	int $3

# two pages' worth should be well enough.
	.global __stktop
	.bss
	.align 8192
	.space 8192
__stktop:
