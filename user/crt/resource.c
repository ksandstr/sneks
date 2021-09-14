#include <stddef.h>
#include <assert.h>
#include <errno.h>
#include <sys/resource.h>

#include <sneks/sysinfo.h>
#include <sneks/api/proc-defs.h>

#include "private.h"


int getrlimit(int resource, struct rlimit *rlim)
{
	static_assert(sizeof(struct rlimit) == sizeof(struct sneks_proc_rlimit));
	static_assert(offsetof(struct rlimit, rlim_cur) == offsetof(struct rlimit, rlim_cur));
	static_assert(offsetof(struct rlimit, rlim_max) == offsetof(struct rlimit, rlim_max));
	int n = __proc_prlimit(__the_sysinfo->api.proc, 0, resource | 0x80000000,
		&(struct sneks_proc_rlimit){ }, (void *)rlim);
	return NTOERR(n);
}


int setrlimit(int resource, const struct rlimit *rlim)
{
	int n = __proc_prlimit(__the_sysinfo->api.proc, 0, resource,
		(void *)rlim, &(struct sneks_proc_rlimit){ });
	return NTOERR(n);
}
