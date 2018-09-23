
# setcontext(3) and getcontext(3). perhaps makecontext(3) and swapcontext(3)
# as well, one day.

	.text
	.global getcontext
	.type getcontext, @function
	.align 16,0

# doesn't store eax, ecx, edx, because those are caller-saved.
getcontext:
	movl 4(%esp), %eax	# struct ucontext
	leal 4(%eax), %eax	# mcontext_t
	movl %ebx, 4(%eax)
	movl %esi, 16(%eax)
	movl %edi, 20(%eax)
	movl %ebp, 24(%eax)
	movl %esp, 28(%eax)
	movl (%esp), %ecx
	movl %ecx, 32(%eax)	# returning eip
	xorl %eax, %eax
	ret


	.text
	.global setcontext
	.type setcontext, @function
	.align 16,0

setcontext:
	movl 4(%esp), %eax	# struct ucontext
	leal 4(%eax), %eax	# mcontext_t
	movl 4(%eax), %ebx
	movl 16(%eax), %esi
	movl 20(%eax), %edi
	movl 24(%eax), %ebp
	movl 28(%eax), %esp
	addl $4, %esp
	movl 32(%eax), %edx
	movl (%eax), %eax
	jmp *%edx
