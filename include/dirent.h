
#ifndef _DIRENT_H
#define _DIRENT_H 1

#include <sys/types.h>


/* values of dirent.type.
 *
 * TODO: once find-idl-defs.pl gets clever enough to look here, #include
 * <sneks/api/directory-defs.h> and pull these constants from there. (and do
 * the same for SEEK_FOO as well in <stdio.h> and <unistd.h>.)
 */
#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12


struct dirent {
	ino_t d_ino;
	off_t d_off;
	unsigned short d_reclen;
	unsigned char d_type;
	char d_name[];
};

/* presence of non-POSIX fields */
#define _DIRENT_HAVE_D_OFF 1
#define _DIRENT_HAVE_D_RECLEN 1
#define _DIRENT_HAVE_D_TYPE 1


struct __dirstream;
typedef struct __dirstream DIR;


extern DIR *opendir(const char *name);
extern int closedir(DIR *dirp);

extern DIR *fdopendir(int fd);
extern int dirfd(DIR *dirp);

extern struct dirent *readdir(DIR *dirp);
/* no readdir_r() due to deprecation. (it's a bad interface, is what.) */

extern void seekdir(DIR *dirp, long loc);
extern long telldir(DIR *dirp);
extern void rewinddir(DIR *dirp);

extern int scandir(const char *, struct dirent ***,
	int (*)(const struct dirent *),
	int (*)(const struct dirent **, const struct dirent **));

extern int alphasort(const struct dirent **, const struct dirent **);

#ifdef _GNU_SOURCE
/* TODO: scandirat(), versionsort() */
#endif

#endif
