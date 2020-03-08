
#include <stdlib.h>
#include <string.h>
#include <l4/types.h>
#include <l4/thread.h>
#include <sneks/test.h>
#include <sneks/mm.h>

#include "sysmem-defs.h"


/* set some flags with Sysmem::alter_flags, then clear them. this test aims
 * for a particular assertion in the sysmem fault handler; see changelog for
 * deets.
 */
START_TEST(set_and_clear_flags)
{
	const int brksize = 64 * 1024;
	diag("brksize=%d", brksize);
	plan(3);

	void *brkpos = sbrk(brksize);
	diag("brkpos=%p", brkpos);
	int n = __sysmem_alter_flags(L4_Pager(), L4_nilthread.raw,
		L4_FpageLog2((L4_Word_t)brkpos, 12), SMATTR_PIN, ~0ul);
	if(!ok(n == 0, "alter_flags to set PIN")) {
		diag("n=%d", n);
	}

	/* then clear them. */
	n = __sysmem_alter_flags(L4_Pager(), L4_nilthread.raw,
		L4_FpageLog2((L4_Word_t)brkpos, 12), 0, ~(L4_Word_t)SMATTR_PIN);
	if(!ok(n == 0, "alter_flags to clear PIN")) {
		diag("n=%d", n);
	}

	/* now poke the memory. */
	memset(brkpos, 0xd5, brksize);
	pass("memset didn't die");

	sbrk(-brksize);
}
END_TEST

SYSTEST("mem:flags", set_and_clear_flags);
