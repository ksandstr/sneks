
#ifndef _SYS_CHRDEV_PRIVATE_H
#define _SYS_CHRDEV_PRIVATE_H

#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>

#include <sneks/chrdev.h>


struct chrdev_callbacks
{
	int (*get_status)(chrfile_t *);
	int (*dead_client)(pid_t);
	int (*read)(chrfile_t *, uint8_t *, unsigned);
	int (*write)(chrfile_t *, const uint8_t *, unsigned);
	void (*confirm)(chrfile_t *, unsigned, bool);
	int (*close)(chrfile_t *);
	int (*ioctl)(chrfile_t *, unsigned long, va_list args);
	int (*fork)(chrfile_t *, chrfile_t *);
	int (*pipe)(chrfile_t *, chrfile_t *, int);
};


/* from func.c */

extern struct chrdev_callbacks callbacks;


#endif
