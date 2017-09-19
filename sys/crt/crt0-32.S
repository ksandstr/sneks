
	.text
	.global _start

	.align 16
_start:
	leal __stack, %esp
	pushl $0	# argc
	pushl $0	# argv
	pushl $0	# envp
	call main
	addl $8, %esp
	movl %eax, (%esp)
	jmp __return_from_main

# two pages' worth should be well enough.
	.bss
	.space 8192
__stack:
