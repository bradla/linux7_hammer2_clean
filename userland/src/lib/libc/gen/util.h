#ifndef LIBC_GEN_UTIL_H_
#define LIBC_GEN_UTIL_H_

#include <sys/types.h>

#define GETDEVPATH_RAWDEV	0x0001

#define _PATH_DEVTAB_PATHS \
	"/usr/local/etc:/etc:/etc/defaults"

char *getdevpath(const char *devname, int flags);
int sysctlbyname(const char *name, void *oldp, size_t *oldlenp,
		const void *newp, size_t newlen);
void setproctitle(const char *fmt, ...);

#endif
