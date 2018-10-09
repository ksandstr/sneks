
#include <stdlib.h>
#include <errno.h>


char *getenv(const char *name)
{
	return NULL;
}


int setenv(const char *name, const char *value, int overwrite)
{
	errno = ENOMEM;
	return -1;
}


int unsetenv(const char *name)
{
	return 0;
}


int putenv(char *string)
{
	return 0;
}


int clearenv(void)
{
	return 0;
}
