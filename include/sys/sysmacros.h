#ifndef _SYS_SYSMACROS_H
#define _SYS_SYSMACROS_H

#include <sys/types.h>

#define major(d) _sneks_major((d))
#define minor(d) _sneks_minor((d))
#define makedev(maj, min) _sneks_makedev((maj), (min))

extern unsigned _sneks_major(dev_t dev);
extern unsigned _sneks_minor(dev_t dev);
extern dev_t _sneks_makedev(unsigned major, unsigned minor);

#endif
