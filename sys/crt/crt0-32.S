
	.text
	.global _start

	.align 16
_start:
	leal __stack, %esp
	call __crt1_entry
	pushl %eax
	jmp __return_from_main

# two pages' worth should be well enough.
	.bss
	.space 8192
__stack:
