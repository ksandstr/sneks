
	.text
	.global _start

	.align 16
_start:
	leal __stack, %esp
	subl $128, %esp	# don't stomp <struct thrd>
	call __crt1_entry
	pushl %eax
	jmp __return_from_main

# two pages' worth should be well enough.
	.bss
	.align 8192
	.space 8192
__stack:
