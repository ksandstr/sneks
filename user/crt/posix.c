#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#include <l4/thread.h>
#include <l4/kip.h>
#include <l4/kdebug.h>

#include <sneks/elf.h>
#include <sneks/process.h>
#include <sneks/sysinfo.h>

#include "private.h"


/* NOTE: this runs on a 4k stack. */
void *__sneks_posix_init(size_t *p, size_t stkwant)
{
	/* FIXME: replace this with Elf32_auxv_t here and in crt1.c, removing our
	 * local auxv_t. it's silly.
	 */
	static_assert(sizeof(auxv_t) == sizeof(Elf32_auxv_t));
	static_assert(offsetof(auxv_t, a_type) == offsetof(Elf32_auxv_t, a_type));
	static_assert(offsetof(auxv_t, a_val) == offsetof(Elf32_auxv_t, a_un.a_val));

	__the_kip = L4_GetKernelInterface();
	__the_sysinfo = __get_sysinfo(__the_kip);
	__main_tid = L4_MyLocalId();

	/* start heap on next page after program image end. */
	extern char _end;
	uintptr_t cand = max((uintptr_t)&_end,
		(uintptr_t)__the_sysinfo + (1u << __the_sysinfo->sysinfo_size_log2));
	const size_t page_size = 1u << __the_sysinfo->memory.page_size_log2;
	brk((void *)((cand + page_size - 1) & ~(page_size - 1)));

	__file_init((size_t *)PAGE_CEIL((uintptr_t)p - PAGE_SIZE * 2));

	size_t stkmax = (size_t)&stkwant - 0x10000;
	if(stkmax - 32 >= stkwant) {
		/* use existing startup stack. */
		return p;
	} else {
		/* reserve a different stack for main(). */
		void *newstk = sbrk(stkwant);
		if(newstk == NULL) abort();
		/* TODO: mark pages [0x10000, p-0x1000) for runtime use, e.g. slabs
		 * for file descriptors. (and move __file_init() down here for that.)
		 */
		return (void *)newstk + stkwant;
	}
}
