
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <dirent.h>


int main(void)
{
	DIR *d = opendir(".");
	if(d == NULL) exit(errno ^ 0xc0);
	struct dirent *ent;
	while(errno = 0, ent = readdir(d), ent != NULL) {
		printf("ENT %s\n", ent->d_name);
	}
	int err = errno;
	closedir(d);
	return err == 0 ? 0 : err ^ 0xc0;
}
