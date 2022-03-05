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

# NOTE: same as STACK_SIZE in root/thrd.c
	.bss
	.align 8192
	.space 8192
__stack:
