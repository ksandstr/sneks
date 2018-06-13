
/* odds and ends without a great enough theme to warrant a module. */

#include <errno.h>
#include <sneks/process.h>


int spawn_NP(const char *filename,
	char *const argv[], char *const envp[])
{
	errno = ENOSYS;
	return -1;
}
