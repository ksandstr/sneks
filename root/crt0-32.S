

	.text
	.global _start

	.align 16
_start:
	leal __stack, %esp
	subl $80, %esp
	pushl $__return_from_main
	jmp main


__return_from_main:
	movl $1, %ebx
	int $1
	jmp __return_from_main

# FIXME: get this from same source as main.c!
	.bss
	.align 4096
	.space 4096
__stack:
