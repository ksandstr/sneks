# copypasta'd from mung and permitted to diverge, then modified to match
# musl's jmp_buf format.

	.global setjmp
	.type setjmp, @function
	.align 16,0
setjmp:
	movl 4(%esp), %eax
	movl %ebx, (%eax)
	movl %esi, 4(%eax)
	movl %edi, 8(%eax)
	movl %ebp, 12(%eax)
	leal 4(%esp), %ecx
	movl %ecx, 16(%eax)
	movl (%esp), %ecx
	movl %ecx, 20(%eax)
	xorl %eax, %eax
	ret


	.global __longjmp_actual
	.type __longjmp_actual, @function
	.align 16,0
__longjmp_actual:
	movl 4(%esp), %edx
	movl 8(%esp), %eax
	cmpl $1, %eax
	adcb $0, %al
	movl (%edx), %ebx
	movl 4(%edx), %esi
	movl 8(%edx), %edi
	movl 12(%edx), %ebp
	movl 16(%edx), %esp
	jmp *20(%edx)
