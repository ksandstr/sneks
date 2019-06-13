
/* tests on mmap(2), munmap(), sbrk(), etc. */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <ccan/array_size/array_size.h>

#include <sneks/test.h>


/* error cases of mmap(2).
 *
 * TODO: add ones with a backing file once random-access files are available.
 */
START_TEST(mmap_errors)
{
	plan_tests(2);

	/* should reject MAP_PRIVATE | MAP_SHARED. */
	void *ptr = mmap(NULL, sysconf(_SC_PAGESIZE), PROT_READ | PROT_WRITE,
		MAP_ANONYMOUS | MAP_PRIVATE | MAP_SHARED, -1, 0);
	if(!ok(ptr == MAP_FAILED && errno == EINVAL, "both shared and private")) {
		diag("ptr=%p, errno=%d", ptr, errno);
	}

	/* and the absence of both. */
	ptr = mmap(NULL, sysconf(_SC_PAGESIZE), PROT_READ | PROT_WRITE,
		MAP_ANONYMOUS, -1, 0);
	if(!ok(ptr == MAP_FAILED && errno == EINVAL,
		"neither shared nor private"))
	{
		diag("ptr=%p, errno=%d", ptr, errno);
	}

	/* TODO: checks on validity of addr, length, and offset. */
}
END_TEST

DECLARE_TEST("process:memory", mmap_errors);


/* mmap(2), munmap() API basics. */
START_LOOP_TEST(mmap_basic, iter, 0, 3)
{
	const int page_size = sysconf(_SC_PAGESIZE);
	const bool addr_from_sbrk = !!(iter & 1), is_shared = !!(iter & 2);
	diag("page_size=%d, addr_from_sbrk=%s, is_shared=%s", page_size,
		btos(addr_from_sbrk), btos(is_shared));
	plan_tests(4);

	const int map_size = 16 * page_size;

	void *addr_hint = NULL;
	if(addr_from_sbrk) addr_hint = sbrk(0) + page_size;
	diag("addr_hint=%p", addr_hint);
	void *ptr = mmap(addr_hint, map_size, PROT_READ | PROT_WRITE,
		MAP_ANONYMOUS | (is_shared ? MAP_SHARED : MAP_PRIVATE), -1, 0);

	skip_start(!ok1(ptr != MAP_FAILED), 2, "no valid map") {
		diag("ptr=%p", ptr);

		strcpy(ptr, "ja sit teet mun matikanläksyt jouluspettariin asti");
		pass("write didn't break");

		int n = munmap(ptr, map_size);
		if(!ok(n == 0, "munmap")) diag("munmap errno=%d", errno);
	} skip_end;

	void *moer = sbrk(map_size);
	ok(moer != (void *)-1, "sbrk after unmap");
	sbrk(-map_size);
}
END_TEST

DECLARE_TEST("process:memory", mmap_basic);


/* MAP_FIXED to mmap(2). should overlap existing mappings. may fail at overlap
 * with sbrk().
 */
START_TEST(mmap_fixed)
{
	const int page_size = sysconf(_SC_PAGESIZE);
	diag("page_size=%d", page_size);
	plan_tests(5);
#ifdef __sneks__
	todo_start("not in sneks yet");
#endif

	void *base = sbrk(0);
	diag("base=%p", base);

	/* part 1: MAP_FIXED should succeed ahead of sbrk(0). */
	const size_t meg = 1024 * 1024;
	void *ptr = mmap(base + meg, meg, PROT_READ | PROT_WRITE,
		MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
	ok1(ptr != MAP_FAILED);
	ok1(ptr == base + meg);

	/* sbrk into it should fail. */
	void *next = sbrk(2 * meg);
	if(ok1(next == (void *)-1)) diag("errno=%d", errno);
	else {
		diag("undoing previous sbrk");
		sbrk(-2 * meg);
	}

	if(ptr != MAP_FAILED) munmap(ptr, meg);
	/* and work after munmap. */
	next = sbrk(2 * meg);
	if(ok1(next != (void *)-1)) sbrk(-2 * meg);

#ifndef __sneks__
	skip(1, "only sneks forbids mmap() over sbrk");
#else
	/* part 2: MAP_FIXED into the sbrk range should fail. */
	ptr = mmap(base - meg, page_size * 2, PROT_READ | PROT_WRITE,
		MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
	if(!ok1(ptr == MAP_FAILED)) {
		diag("ptr'=%p", ptr);
		munmap(ptr, page_size * 2);
	}
#endif
}
END_TEST

DECLARE_TEST("process:memory", mmap_fixed);


static sig_atomic_t poked = 0;

static void sync_poke(int signum) {
	poked = 1;
}

/* non-/survival of mmap(2) regions through fork(). */
START_LOOP_TEST(mmap_across_fork, iter, 0, 1)
{
	const bool is_private = !!(iter & 1);
	diag("is_private=%s", btos(is_private));
	plan_tests(2);

	char *area = mmap(NULL, sysconf(_SC_PAGESIZE), PROT_READ | PROT_WRITE,
		MAP_ANONYMOUS | (is_private ? MAP_PRIVATE : MAP_SHARED), -1, 0);
	diag("area=%p", area);
	skip_start(!ok(area != MAP_FAILED, "mmap(2)"), 1,
		"no mmap area, errno=%d", errno)
	{
		const char *ref = "mitä teille tulee mieleen sanasta hattivatti?";
		strcpy(area, ref);
		sigset_t usr1, old;
		sigemptyset(&usr1);
		sigaddset(&usr1, SIGUSR1);
		int n = sigprocmask(SIG_BLOCK, &usr1, &old);
		fail_unless(n == 0, "sigprocmask errno=%d", errno);

		int child = fork_subtest_start("child process") {
			plan_tests(1);
			struct sigaction act = { .sa_handler = &sync_poke };
			n = sigaction(SIGUSR1, &act, NULL);
			fail_unless(n == 0, "child sigaction failed");
			while(!poked) sigsuspend(&old);
			iff_ok1(is_private, strcmp(area, ref) == 0);
		} fork_subtest_end;

		strcpy(area, "siis hatti-vatti?");
		n = kill(child, SIGUSR1);
		fail_unless(n == 0, "kill(2) errno=%d", errno);

		fork_subtest_ok1(child);
	} skip_end;
}
END_TEST

DECLARE_TEST("process:memory", mmap_across_fork);


static void xmmap(
	void *addr, size_t sz, int prot, int flags,
	int fd, size_t offset)
{
	fail_unless(flags & MAP_FIXED);
	void *ptr = mmap(addr, sz, prot, flags, fd, offset);
	fail_if(ptr == MAP_FAILED, "mmap(2), errno=%d", errno);
	fail_unless(ptr == addr, "mmap(2) result is %p, wanted %p", ptr, addr);
}


START_LOOP_TEST(munmap_geometry_shrapnel, iter, 0, 63)
{
	static const int under_counts[] = { 0, 1, 3, 11 };
	const bool pad_front = !!(iter & 1), pad_rear = !!(iter & 2),
		lap_front = !!(iter & 4), lap_rear = !!(iter & 8);
	const int num_under = under_counts[(iter >> 4) & 3];
	diag("pad_front=%s, pad_rear=%s, lap_front=%s, lap_rear=%s, num_under=%d",
		btos(pad_front), btos(pad_rear), btos(lap_front), btos(lap_rear),
		num_under);
	plan_tests(1);

	const int page_size = sysconf(_SC_PAGESIZE);
	void *const base = sbrk(0),
		*const start = base + page_size * 1024,
		*const end = start + page_size * 32768;
	diag("base=%p, start=%p, end=%p", base, start, end);

	int segs[(num_under + 4) * 2], *sp = segs;
	if(pad_front) { *sp++ = -256; *sp++ = 4; }
	if(lap_front) { *sp++ = -4; *sp++ = 8; }
	for(int i=0, pos = 17; i < num_under; i++) {
		*sp++ = pos;
		*sp++ = 77;
		pos += 81;
	}
	const int last_page = (end - start) / page_size;
	if(lap_rear) { *sp++ = last_page - 4; *sp++ = 8; }
	if(pad_rear) { *sp++ = last_page + 123; *sp++ = 4; }
	fail_unless(sp <= &segs[ARRAY_SIZE(segs)]);

	for(int i=0; &segs[i] < sp; i+=2) {
		void *s = start + segs[i + 0] * page_size,
			*e = s + segs[i + 1] * page_size;
		diag("creating mmap [%p, %p)", s, e);
		xmmap(s, e - s, PROT_READ | PROT_WRITE,
			MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	}

	int n = munmap(start, end - start);
	if(!ok(n == 0, "munmap")) diag("errno=%d", errno);
	/* TODO: catch sigsegv to check that references are valid for the front
	 * and rear pads and front and rear overlapping maps, respectively; and
	 * invalid between start and end. also add a variable to fault some pages
	 * in first.
	 */
}
END_TEST

DECLARE_TEST("process:memory", munmap_geometry_shrapnel);


/* then the case which the previous test couldn't do, i.e. munmap from within
 * a larger lazy_mmap. variables are whether it's constructed out of one part
 * or several.
 */
START_LOOP_TEST(munmap_geometry_hotdog, iter, 0, 1)
{
	const bool from_parts = !!(iter & 1);
	diag("from_parts=%s", btos(from_parts));
	plan_tests(1);

	const int page_size = sysconf(_SC_PAGESIZE);
	void *const base = sbrk(0) + 1024 * page_size,
		*const start = base + 1024 * page_size,
		*const end = start + 4096 * page_size;
	int n_parts = from_parts ? 16 : 1;
	size_t sz = end - start, pc = sz / 16;
	for(int i=0; i < n_parts; i++) {
		diag("creating mmap [%p, %p)", start + pc * i, start + pc * i + pc);
		xmmap(start + pc * i, pc, PROT_READ | PROT_WRITE,
			MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	}

	int n = munmap(start + 23 * page_size, 77 * page_size);
	if(!ok(n == 0, "munmap")) diag("errno=%d", errno);

	/* TODO: probe segvs, add parameter for faulting things in before first
	 * unmap
	 */

	munmap(start, end - start);
}
END_TEST

DECLARE_TEST("process:memory", munmap_geometry_hotdog);


/* this is meant to hit the backward case in VM::brk.
 * TODO: catch sigsegv to probe the sbrk heap after the negative call.
 */
START_TEST(sbrk_backward)
{
	plan_tests(2);

	void *ptr = sbrk(1024 * 1024);
	memset(ptr, 0xad, 1024 * 1024);
	pass("didn't crash");

	sbrk(-1024 * 1024);
	pass("still didn't crash");
}
END_TEST

DECLARE_TEST("process:memory", sbrk_backward);
