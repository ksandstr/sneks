
/* a vague facsimile of what could one day be posix_spawn() and that lot. */

#ifndef _SPAWN_H
#define _SPAWN_H 1

/* returns pid or -1, sets errno on failure */
extern int spawn_NP(
	const char *filename,
	char *const argv[], char *const envp[]);

#endif
