#include <sys/mount.h>
#include <sneks/sysinfo.h>
#include <sneks/api/namespace-defs.h>
#include "private.h"

#define OR_EMPTY(str) ((str) != NULL ? (str) : "")

int mount(const char *source, const char *target, const char *fstype, unsigned long mountflags, const void *data) {
	return NTOERR(__ns_mount(__the_sysinfo->api.rootfs, OR_EMPTY(source), OR_EMPTY(target), OR_EMPTY(fstype), mountflags, OR_EMPTY(data)));
}

int umount(const char *target) {
	return umount2(target, 0);
}

int umount2(const char *target, int flags) {
	return NTOERR(__ns_umount(__the_sysinfo->api.rootfs, OR_EMPTY(target), flags));
}
