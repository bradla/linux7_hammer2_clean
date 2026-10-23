#include <sys/types.h>
#include <errno.h>

int
sysctlbyname(const char *name, void *oldp, size_t *oldlenp,
    const void *newp, size_t newlen)
{
	errno = ENOTSUP;
	return(-1);
}
