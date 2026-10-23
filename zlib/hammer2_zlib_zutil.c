#include "hammer2_zlib_zutil.h"

#ifndef NO_DUMMY_DECL
struct internal_state      {int dummy;};
#endif

z_const char * const z_errmsg[10] = {
"need dictionary",
"stream end",
"",
"file error",
"stream error",
"data error",
"insufficient memory",
"buffer error",
"incompatible version",
""};

const char * zlibVersion(void);
uLong zlibCompileFlags(void);
const char * zError(int err);

const
char*
zlibVersion(void)
{
    return ZLIB_VERSION;
}

uLong
zlibCompileFlags(void)
{
    uLong flags;

    flags = 0;
    switch ((int)(sizeof(uInt))) {
    case 2:     break;
    case 4:     flags += 1;     break;
    case 8:     flags += 2;     break;
    default:    flags += 3;
    }
    switch ((int)(sizeof(uLong))) {
    case 2:     break;
    case 4:     flags += 1 << 2;        break;
    case 8:     flags += 2 << 2;        break;
    default:    flags += 3 << 2;
    }
    switch ((int)(sizeof(voidpf))) {
    case 2:     break;
    case 4:     flags += 1 << 4;        break;
    case 8:     flags += 2 << 4;        break;
    default:    flags += 3 << 4;
    }
    switch ((int)(sizeof(z_off_t))) {
    case 2:     break;
    case 4:     flags += 1 << 6;        break;
    case 8:     flags += 2 << 6;        break;
    default:    flags += 3 << 6;
    }
#ifdef H2_ZLIB_DEBUG
    flags += 1 << 8;
#endif
#if defined(ASMV) || defined(ASMINF)
    flags += 1 << 9;
#endif
#ifdef ZLIB_WINAPI
    flags += 1 << 10;
#endif
#ifdef BUILDFIXED
    flags += 1 << 12;
#endif
#ifdef DYNAMIC_CRC_TABLE
    flags += 1 << 13;
#endif
#ifdef NO_GZCOMPRESS
    flags += 1L << 16;
#endif
#ifdef NO_GZIP
    flags += 1L << 17;
#endif
#ifdef PKZIP_BUG_WORKAROUND
    flags += 1L << 20;
#endif
#ifdef FASTEST
    flags += 1L << 21;
#endif
#if defined(STDC) || defined(Z_HAVE_STDARG_H)
#  ifdef NO_vsnprintf
    flags += 1L << 25;
#    ifdef HAS_vsprintf_void
    flags += 1L << 26;
#    endif
#  else
#    ifdef HAS_vsnprintf_void
    flags += 1L << 26;
#    endif
#  endif
#else
    flags += 1L << 24;
#  ifdef NO_snprintf
    flags += 1L << 25;
#    ifdef HAS_sprintf_void
    flags += 1L << 26;
#    endif
#  else
#    ifdef HAS_snprintf_void
    flags += 1L << 26;
#    endif
#  endif
#endif
    return flags;
}

#ifdef H2_ZLIB_DEBUG

#  ifndef verbose
#    define verbose 0
#  endif
int ZLIB_INTERNAL z_verbose = verbose;

void ZLIB_INTERNAL z_error (char *m)
{
#if defined(_KERNEL)
    panic("h2 %s: %s", __func__, m);
#else
    fprintf(stderr, "%s\n", m);
    exit(1);
#endif
}
#endif

const
char*
zError(int err)
{
    return ERR_MSG(err);
}

#ifndef HAVE_MEMCPY

void
ZLIB_INTERNAL
zmemcpy(Bytef* dest, const Bytef* source, uInt  len)
{
    if (len == 0) return;
    do {
        *dest++ = *source++;
    } while (--len != 0);
}

int
ZLIB_INTERNAL
zmemcmp(const Bytef* s1, const Bytef* s2, uInt len)
{
    uInt j;

    for (j = 0; j < len; j++) {
        if (s1[j] != s2[j]) return 2*(s1[j] > s2[j])-1;
    }
    return 0;
}

void
ZLIB_INTERNAL
zmemzero(Bytef* dest, uInt len)
{
    if (len == 0) return;
    do {
        *dest++ = 0;
    } while (--len != 0);
}
#endif
