	.text
	.global _start

	.align 16
_start:
	movl %esp, %ebx	# store fdlist pointer
	leal __stack, %esp
	subl $128, %esp	# don't stomp <struct thrd>
	pushl %ebx
	call __crt1_entry
	pushl %eax
	call exit
	int $3

# two pages' worth should be well enough.
	.bss
	.align 8192
	.space 8192
__stack:
