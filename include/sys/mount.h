/* not unlike Linux <sys/mount.h> */
#ifndef _SYS_MOUNT_H
#define _SYS_MOUNT_H

#define MS_RDONLY 1
#define MS_NOSUID 2
#define MS_NODEV 4
/* TODO: other MS_* */

/* nonstandard, per Linux. umount(x) = umount2(x, 0). */
extern int umount(const char *target);
extern int umount2(const char *target, int flags);
/* TODO: flags MNT_* for umount2() */
extern int mount(const char *source, const char *target, const char *fstype, unsigned long mountflags, const void *data);

#endif
