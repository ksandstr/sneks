
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ccan/minmax/minmax.h>
#include <ccan/compiler/compiler.h>
#include <ccan/array_size/array_size.h>

#include <l4/types.h>
#include <l4/ipc.h>

#include "defs.h"


static L4_Fpage_t next_phys_page(L4_ThreadId_t root, L4_Word_t *addr_hi)
{
	L4_Accept(L4_UntypedWordsAcceptor);
	L4_MsgTag_t tag = L4_Receive(root);
	if(L4_IpcFailed(tag)) goto ipcfail;
	L4_Fpage_t page;
	L4_StoreMR(1, &page.raw);
	L4_StoreMR(2, addr_hi);
	if(L4_IsNilFpage(page)) {
		/* termination. leave the chain open. */
	} else {
		L4_Accept(L4_MapGrantItems(page));
		L4_LoadMR(0, 0);
		tag = L4_Call(root);
		if(L4_IpcFailed(tag)) goto ipcfail;
	}

	return page;

ipcfail:
	printf("%s: ipc failed, ec=%lu\n", __func__, L4_ErrorCode());
	abort();
}


COLD L4_Fpage_t *init_protocol(int *n_phys_p, L4_ThreadId_t *peer_tid_p)
{
	L4_ThreadId_t sender;
	L4_Accept(L4_UntypedWordsAcceptor);
	L4_MsgTag_t tag = L4_Wait(&sender);
	if(L4_IpcFailed(tag)) {
		printf("vm: %s: can't get init message, ec=%#lx\n", __func__,
			L4_ErrorCode());
		abort();
	}
	*peer_tid_p = sender;
	L4_Word_t total_pages = 0, max_phys = 0;
	L4_StoreMR(1, &total_pages);
	L4_StoreMR(2, &max_phys);
	printf("vm: total_pages=%lu, max_phys=%#lx\n", total_pages, max_phys);
	/* TODO: use vmaux to deal with total_pages too large to fit a 1G dlmalloc
	 * heap below the microkernel reservation.
	 */

	L4_LoadMR(0, 0);
	L4_Reply(sender);

	/* TODO: come up with a more fancy system for recording arbitrary amounts
	 * of physical memory, such as one of thems that cares about high address
	 * bits. this one only goes to 4G, and 200 pages should easily record that
	 * much.
	 */
	L4_Fpage_t temp_phys[200];
	L4_Word_t addr_hi, n_phys = 0;
	L4_Fpage_t pg;
	while(pg = next_phys_page(sender, &addr_hi), !L4_IsNilFpage(pg)) {
		if(n_phys == ARRAY_SIZE(temp_phys)) {
			printf("vm: %s: too many physical fpages!\n", __func__);
			abort();
		}
		temp_phys[n_phys++] = pg;
	}

	/* this can only be done after the grant loop, or program break would
	 * inhibit memory transfer.
	 */
	extern char _end;
	int n = brk(max((void *)max_phys + 1, (void *)&_end + 1));
	if(n != 0) {
		printf("vm: brk failed, errno=%d\n", errno);
		abort();
	}

	*n_phys_p = n_phys;
	L4_Fpage_t *phys = malloc(sizeof *phys * n_phys);
	memcpy(phys, temp_phys, sizeof *phys * n_phys);
	return phys;
}
