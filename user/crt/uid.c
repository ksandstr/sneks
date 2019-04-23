
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>


__uid_t getuid(void) {
	return 666;		/* vade retro! */
}


__uid_t geteuid(void) {
	return 667;
}


int setuid(__uid_t uid) {
	errno = ENOSYS;
	return -1;
}


int seteuid(__uid_t euid) {
	errno = ENOSYS;
	return -1;
}


int setreuid(__uid_t real_uid, __uid_t eff_uid) {
	errno = ENOSYS;
	return -1;
}


int setresuid(__uid_t real_uid, __uid_t eff_uid, __uid_t saved_uid) {
	errno = ENOSYS;
	return -1;
}
