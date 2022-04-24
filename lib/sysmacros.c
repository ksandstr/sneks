#include <sys/types.h>
#include <sys/sysmacros.h>

unsigned _sneks_major(dev_t dev) {
	return (dev >> 15) & 0x7fff;
}

unsigned _sneks_minor(dev_t dev) {
	return dev & 0x7fff;
}

dev_t _sneks_makedev(unsigned major, unsigned minor) {
	return (major & 0x7fff) << 15 | (minor & 0x7fff);
}
