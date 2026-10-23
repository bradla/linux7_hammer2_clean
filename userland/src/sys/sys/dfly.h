#ifndef _SYS_DFLY_H_
#define _SYS_DFLY_H_

#include <sys/cdefs.h>

#define LH1_VERSION_STRING	"v0.2.0"

#if __GNUC_PREREQ(2, 7)

#define __unused	__attribute__((unused))
#define __dead2		__attribute__((__noreturn__))
#define __packed	__attribute__((__packed__))
#else
#define __unused
#define __dead2
#define __packed
#endif

#if !__GNUC_PREREQ(2, 7)
#define	__printflike(fmtarg, firstvararg)
#elif __GNUC_PREREQ(3, 0)
#define	__printflike(fmtarg, firstvararg) \
            __attribute__((__nonnull__(fmtarg), \
			  __format__ (__printf__, fmtarg, firstvararg)))
#else
#define	__printflike(fmtarg, firstvararg) \
	    __attribute__((__format__ (__printf__, fmtarg, firstvararg)))
#endif

#endif
