
#include <stdio.h>
#include <stdlib.h>


int main(int argc, char *argv[], char *envp[])
{
	for(int i=0; i < argc; i++) {
		printf("ARG %d %s\n", i, argv[i]);
	}
	/* well, shucks... */
	for(int i=0; envp[i] != NULL; i++) {
		printf("ENV %d %s\n", i, envp[i]);
	}
	return EXIT_SUCCESS;
}
