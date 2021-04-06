
#ifndef _SYS_CHRDEV_PRIVATE_H
#define _SYS_CHRDEV_PRIVATE_H

#include <sneks/chrdev.h>


struct chrdev_callbacks
{
	/* constructors */
	int (*pipe)(chrfile_t *, chrfile_t *, int);
	int (*dev_open)(chrfile_t *, char, int, int, int);

	/* dtor for chrdev_rollback() */
	int (*close)(chrfile_t *);
};


/* from func.c */

extern struct chrdev_callbacks chrdev_callbacks;


#endif
