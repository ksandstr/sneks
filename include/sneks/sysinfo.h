/* the Sneks system information page, similar in function to the commpage of
 * Mach and suchlike.
 */
#ifndef _SNEKS_SYSINFO_H
#define _SNEKS_SYSINFO_H

#include <stdint.h>
#include <assert.h>
#include <l4/types.h>
#include <l4/kip.h>
#include <sneks/mm.h>

#define SNEKS_SYSINFO_MAGIC 0xbadc0de5	/* please send me them */

/* no pointers, this should be relocable. */
struct __sysinfo
{
	L4_Word32_t magic;
	L4_Word8_t sysinfo_size_log2;
	L4_Word8_t __pad0[3];
	L4_Word32_t __pad1[3];

	/* basic system services and misc API detail. */
	struct {
		L4_ThreadId_t proc;	/* Sneks::Proc */
		L4_ThreadId_t vm;	/* Sneks::VM (generally same as L4_Pager()) */
		L4_ThreadId_t rootfs;	/* Sneks::Path, Sneks::Filesystem */
		L4_Word_t __reserved[29];
	} api;

	struct {
		uint8_t page_size_log2;		/* system preferred page size */
		uint8_t biggest_page_log2;	/* maximum bigpage size */
		uint8_t __pad0[2];
		L4_Word32_t __reserved[15];
	} memory;

	/* services implementing specific POSIX interfaces. */
	struct {
		L4_ThreadId_t pipe;	/* Sneks::Pipe */
		L4_Word32_t __reserved[63];
	} posix;
} __attribute__((__packed__));

#if defined(PAGE_SIZE)
/* in userspace, sysinfopage is always located after the kernel information
 * page. this makes it easy to find using simple maths. note that the return
 * value is still read-only despite not being marked const.
 */
static inline struct __sysinfo *__get_sysinfo(L4_KernelInterfacePage_t *kip) {
	uintptr_t base = ((uintptr_t)kip + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
	struct __sysinfo *si = (struct __sysinfo *)(base + L4_KipAreaSize(kip));
	assert(si->magic == SNEKS_SYSINFO_MAGIC);
	return si;
}
#endif

#endif
