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

# NOTE: same as 1 << __thrd_stksize_log2 in root/thrd.c; keep in sync!
	.bss
	.align 16384
	.space 16384
__stack:
