#ifndef _SYS_STAT_H
#define _SYS_STAT_H

#include <time.h>
#include <sys/types.h>
#include <sneks/api/path-defs.h>

#define S_IFMT SNEKS_PATH_S_IFMT
#define S_IFDIR SNEKS_PATH_S_IFDIR
#define S_IFCHR SNEKS_PATH_S_IFCHR
#define S_IFBLK SNEKS_PATH_S_IFBLK
#define S_IFREG SNEKS_PATH_S_IFREG
#define S_IFIFO SNEKS_PATH_S_IFIFO
#define S_IFLNK SNEKS_PATH_S_IFLNK
#define S_IFSOCK SNEKS_PATH_S_IFSOCK

#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m) (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

/* mode bits
 *
 * (TODO: spec these out via path.idl, though for realsies these bits are Unix
 * canon since forever.)
 */
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRWXU (S_IRUSR | S_IWUSR | S_IXUSR)

#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IXGRP 0010
#define S_IRWXG (S_IRGRP | S_IWGRP | S_IXGRP)

#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IXOTH 0001	/* beware his fiery breath! */
#define S_IRWXO (S_IROTH | S_IWOTH | S_IXOTH)


struct stat
{
	/* TODO: get <struct sneks_io_statbuf> from api/io-defs.h using a "sole
	 * import" preprocessor method, stick it in here, and #define all st_foo
	 * fields as "_statbuf.foo".
	 */
	dev_t st_dev;
	ino_t st_ino;
	mode_t st_mode;
	nlink_t st_nlink;
	uid_t st_uid;
	gid_t st_gid;
	dev_t st_rdev;
	off_t st_size;
	blksize_t st_blksize;
	blkcnt_t st_blocks;
	struct timespec st_atim, st_mtim, st_ctim;

#define st_atime st_atim.tv_sec
#define st_mtime st_mtim.tv_sec
#define st_ctime st_ctim.tv_sec
};


extern int stat(const char *pathname, struct stat *statbuf);
extern int fstat(int fd, struct stat *statbuf);
extern int lstat(const char *pathname, struct stat *statbuf);

extern int fstatat(int dirfd, const char *pathname, struct stat *statbuf, int flags);

#endif
