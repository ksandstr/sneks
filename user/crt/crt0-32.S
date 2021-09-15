	.text
	.weak _DYNAMIC
	.hidden _DYNAMIC

	.global _start
_start:
	popl %ecx	# stkwant
	xorl %ebp, %ebp # stack frame list sentinel
	movl %edx, %esi
	movl %esp, %edi	# p
	andl $-16, %esp
	push %edi	# trash for alignment
	push %edi
	# GOT address for relocable binaries
	call 1f
1:	addl $_DYNAMIC-1b, (%esp)
	movl (%esp), %ebx
	# POSIX layer initialization
	push %ecx
	push %edi
	call __sneks_posix_init
	# switch to designated stack, reinitialize stack frame, enter crt.
	movl %eax, %esp
	andl $-16, %esp
	push %edi
	push %edi
	push %ebx
	# register startup %edx w/ atexit unless null
	testl %esi, %esi
	jz 2f
	push %esi
	call atexit
	addl $4, %esp
2:	push %edi
	call _start_c
	hlt
