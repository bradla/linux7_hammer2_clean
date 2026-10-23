#ifndef LIBC_STRING_UTIL_H_
#define LIBC_STRING_UTIL_H_

#include <sys/types.h>

size_t	 strlcpy(char * __restrict, const char * __restrict, size_t);
size_t	 strlcat(char * __restrict, const char * __restrict, size_t);

#endif
