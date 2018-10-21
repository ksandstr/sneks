
# __invoke_sig_fast and __invoke_sig_slow, as used from sig_bottom() in
# user/crt/sigaction.c .

	.text
	.global __invoke_sig_fast
	.type __invoke_sig_fast, @function
	.align 16,0

__invoke_sig_fast:
	# TODO: just like the slow side, but without the MR/BR saving stuff.
	# the clue is that we're running on top of an aborted IPC operation,
	# which invalidates both MRs and BRs -- so they need not be reloaded
	# even if the signal handler does IPC.
	jmp __invoke_sig_slow


	.text
	.global __invoke_sig_slow
	.type __invoke_sig_slow, @function
	.align 16,0

# this routine is invoked with the parameters in front of the return address,
# rather than after, so that it can popf;ret for an epilogue, thereby restoring
# user context.
__invoke_sig_slow:	# (sig, flags, ip)
	pushal
	movl 32(%esp), %eax	# sig# for first regparm
	# store MRs and BRs on stack.
	subl $0x184, %esp	# 64 + 33 words
	movl %gs:0, %ebx	# UTCB
	movl %ebx, %esi		# address of MR0..MR63
	movl %esp, %edi		# storage area
	movl $64, %ecx
	rep movsl (%esi), (%edi)
	movl $33, %ecx
	leal -192(%ebx), %esi	# address of BR32..BR0
	rep movsl (%esi), (%edi)
	call __sig_invoke
	# restore MRs and BRs.
	movl %ebx, %edi
	movl %esp, %esi
	movl $64, %ecx
	rep movsl (%esi), (%edi)
	leal -192(%ebx), %edi
	movl $33, %ecx
	rep movsl (%esi), (%edi)
	addl $0x184, %esp
	popal
	addl $4, %esp	# drop sig
	popf
	ret

