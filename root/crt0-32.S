	.text
	.global _start

	.align 16
_start:
	leal __stack - 80, %esp
	xorl %ebp, %ebp
	call main
__return_from_main:
	movl $1, %ebx
	int $1
	jmp __return_from_main

# FIXME: duplicated in root/main.c THREAD_STACK_SIZE
	.bss
	.align 4096
	.space 4096
__stack:
