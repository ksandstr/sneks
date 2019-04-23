
/* UID, and later GID, management through the getuid/setuid family. */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>

#include "private.h"
#include "proc-defs.h"


/* on the basis that other processes cannot change this process' UID or GID,
 * we'll cache the current values here. spawn/exec initialization calls
 * Proc::getresuid to take initial values, and fork carries them through
 * correctly.
 *
 * TODO: should reload these in the setuid, setgid families.
 */
struct cached_creds {
	__uid_t u_real, u_eff, u_saved;
	/* also __gid_t g_real, g_eff, g_saved; */
};


static struct cached_creds cc;


static int reload_cc(void) {
	__gid_t g_dummy;
	return __proc_getresugid(__the_sysinfo->api.proc,
		&cc.u_real, &cc.u_eff, &cc.u_saved,
		&g_dummy, &g_dummy, &g_dummy);
}


void __init_crt_cached_creds(void)
{
	int n = reload_cc();
	if(n != 0) {
		/* CRT initialization likely shouldn't abort a process, but if Proc
		 * isn't responding the system has already made a fucky wucky.
		 */
		fprintf(stderr, "%s: Proc::getresuid failed, n=%d\n", __func__, n);
		abort();
	}
}


__uid_t getuid(void) {
	return cc.u_real;
}


__uid_t geteuid(void) {
	return cc.u_eff;
}


static int NTOERR(int n) {
	if(n == 0) return 0;
	else {
		errno = n > 0 ? EIO : -n;
		return -1;
	}
}


int setuid(__uid_t uid) {
	int n = __proc_setresugid(__the_sysinfo->api.proc,
		1, uid, -1, -1, -1, -1, -1);
	if(n == 0) n = reload_cc();
	return NTOERR(n);
}


int seteuid(__uid_t euid) {
	return setresuid(-1, euid, -1);
}


int setreuid(__uid_t real_uid, __uid_t eff_uid) {
	int n = __proc_setresugid(__the_sysinfo->api.proc,
		2, real_uid, eff_uid, -1, -1, -1, -1);
	if(n == 0) n = reload_cc();
	return NTOERR(n);
}


int setresuid(__uid_t real_uid, __uid_t eff_uid, __uid_t saved_uid) {
	int n = __proc_setresugid(__the_sysinfo->api.proc,
		3, real_uid, eff_uid, saved_uid, -1, -1, -1);
	if(n == 0) n = reload_cc();
	return NTOERR(n);
}
