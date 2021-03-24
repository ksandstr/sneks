
#include <errno.h>
#include <sys/types.h>
#include <dirent.h>


struct __stdio_dir {
	char dummy;
};


DIR *opendir(const char *name)
{
	errno = ENOSYS;
	return NULL;
}


int closedir(DIR *dirp)
{
	errno = ENOSYS;
	return -1;
}


DIR *fdopendir(int fd)
{
	errno = ENOSYS;
	return NULL;
}


int dirfd(DIR *dirp)
{
	errno = ENOSYS;
	return -1;
}


struct dirent *readdir(DIR *dirp)
{
	errno = ENOSYS;
	return NULL;
}


void seekdir(DIR *dirp, long loc)
{
	/* empty */
}


long telldir(DIR *dirp)
{
	errno = ENOSYS;
	return -1;
}


void rewinddir(DIR *dirp)
{
	/* empty */
}


int scandir(const char *dir_name, struct dirent ***namelist,
	int (*filter)(const struct dirent *),
	int (*compar)(const struct dirent **, const struct dirent **))
{
	errno = ENOSYS;
	return -1;
}


int alphasort(const struct dirent **a, const struct dirent **b)
{
	errno = ENOSYS;
	return -1;
}
