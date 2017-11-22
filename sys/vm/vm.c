
/* systemspace POSIX-like virtual memory server. */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <ccan/minmax/minmax.h>
#include <ccan/array_size/array_size.h>

#include <l4/types.h>
#include <l4/ipc.h>

#include <ukernel/rangealloc.h>
#include <sneks/mm.h>
#include <sneks/bitops.h>

#include "nbsl.h"
#include "epoch.h"
#include "defs.h"


struct pl;


/* physical memory. has a short name because it's very common. this structure
 * has the read-only parts of the pp/pl split; the only field that may be
 * concurrently written is ->link, which may be swapped for NULL to take the
 * page off a list (depublication) once the corresponding <struct pl> has been
 * depublished.
 */
struct pp
{
	/* frame number per rangealloc in pp_ra. */
	struct pl *_Atomic link;

	/* pagecache pages only */
	unsigned long fsid;
	uint64_t ino;
	uint32_t offset;	/* limits files to 16 TiB each */
};


/* link to a <struct pp> by physical page number. */
struct pl {
	struct nbsl_node nn;
	uint32_t page_num;	/* constant */
	_Atomic uint32_t status;
};


static size_t pp_first;
static struct rangealloc *pp_ra;

static struct nbsl free_list = NBSL_LIST_INIT(free_list);


static COLD void init_phys(L4_Fpage_t *phys, int n_phys)
{
	size_t p_min = ~0ul, p_max = 0;
	for(int i=0; i < n_phys; i++) {
		p_min = min_t(size_t, p_min, L4_Address(phys[i]) >> PAGE_BITS);
		p_max = max_t(size_t, p_max,
			(L4_Address(phys[i]) + L4_Size(phys[i])) >> PAGE_BITS);
	}
	size_t p_total = p_max - p_min;
	printf("vm: allocating %lu <struct pp> (first is %lu)\n",
		(unsigned long)p_total, (unsigned long)p_min);
	pp_first = p_min;

	int eck = e_begin();
	pp_ra = RA_NEW(struct pp, p_total);
	for(int i=0; i < n_phys; i++) {
		int base = (L4_Address(phys[i]) >> PAGE_BITS) - pp_first;
		assert(base >= 0);
		for(int o=0; o < L4_Size(phys[i]) >> PAGE_BITS; o++) {
			struct pp *pp = ra_alloc(pp_ra, base + o);
			assert(ra_ptr2id(pp_ra, pp) == base + o);
			assert(ra_id2ptr(pp_ra, base + o) == pp);
			struct pl *link = malloc(sizeof *link);
			link->page_num = base + o;
			atomic_store_explicit(&link->status, 0, memory_order_relaxed);
			atomic_store(&pp->link, link);
			struct nbsl_node *top;
			do {
				top = nbsl_top(&free_list);
			} while(!nbsl_push(&free_list, top, &link->nn));
		}
	}
	e_end(eck);
}


int main(int argc, char *argv[])
{
	printf("vm sez hello!\n");
	int n_phys = 0;
	L4_Fpage_t *phys = init_protocol(&n_phys);
	printf("vm: init protocol done.\n");

	init_phys(phys, n_phys);
	free(phys);
	printf("vm: physical memory tracking initialized.\n");

	return 0;
}
