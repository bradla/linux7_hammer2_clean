#define MEMORY_USAGE 14

#define HEAPMODE 1

#if (defined(__x86_64__) || defined(_M_X64))
#  define LZ4_ARCH64 1
#else
#  define LZ4_ARCH64 0
#endif

#if defined (__STDC_VERSION__) && __STDC_VERSION__ >= 199901L

#else
#  define restrict
#endif

#include "hammer2.h"
#include "hammer2_lz4.h"

#undef likely
#undef unlikely
#define likely(x)	(__builtin_expect(!!(x), 1))
#define unlikely(x)	(__builtin_expect(!!(x), 0))
#define expect(expr, value) (__builtin_expect((expr), (value)))

#define M_HAMMER2_LZ4	NULL

typedef uint8_t  BYTE;
typedef uint16_t U16;
typedef uint32_t U32;
typedef  int32_t S32;
typedef uint64_t U64;

#if defined(__GNUC__)  && !defined(LZ4_FORCE_UNALIGNED_ACCESS)
#  define _PACKED __attribute__ ((packed))
#else
#  define _PACKED
#endif

#if !defined(LZ4_FORCE_UNALIGNED_ACCESS) && !defined(__GNUC__)
#  pragma pack(push, 1)
#endif

typedef struct _U16_S { U16 v; } _PACKED U16_S;
typedef struct _U32_S { U32 v; } _PACKED U32_S;
typedef struct _U64_S { U64 v; } _PACKED U64_S;

#if !defined(LZ4_FORCE_UNALIGNED_ACCESS) && !defined(__GNUC__)
#  pragma pack(pop)
#endif

#define A64(x) (((U64_S *)(x))->v)
#define A32(x) (((U32_S *)(x))->v)
#define A16(x) (((U16_S *)(x))->v)

#define HASHTABLESIZE (1 << MEMORY_USAGE)

#define MINMATCH 4

#define COPYLENGTH 8
#define LASTLITERALS 5
#define MFLIMIT (COPYLENGTH+MINMATCH)
#define MINLENGTH (MFLIMIT+1)

#define LZ4_64KLIMIT ((1<<16) + (MFLIMIT-1))
#define SKIPSTRENGTH 6

#define MAXD_LOG 16
#define MAX_DISTANCE ((1 << MAXD_LOG) - 1)

#define ML_BITS  4
#define ML_MASK  ((1U<<ML_BITS)-1)
#define RUN_BITS (8-ML_BITS)
#define RUN_MASK ((1U<<RUN_BITS)-1)

#if LZ4_ARCH64
#  define STEPSIZE 8
#  define UARCH U64
#  define AARCH A64
#  define LZ4_COPYSTEP(s,d)       A64(d) = A64(s); d+=8; s+=8;
#  define LZ4_COPYPACKET(s,d)     LZ4_COPYSTEP(s,d)
#  define LZ4_SECURECOPY(s,d,e)   if (d<e) LZ4_WILDCOPY(s,d,e)
#  define HTYPE                   U32
#  define INITBASE(base)          BYTE* base = ip
#else
#  define STEPSIZE 4
#  define UARCH U32
#  define AARCH A32
#  define LZ4_COPYSTEP(s,d)       A32(d) = A32(s); d+=4; s+=4;
#  define LZ4_COPYPACKET(s,d)     LZ4_COPYSTEP(s,d); LZ4_COPYSTEP(s,d);
#  define LZ4_SECURECOPY          LZ4_WILDCOPY
#  define HTYPE                   BYTE*
#  define INITBASE(base)          int base = 0
#endif

#if (defined(LZ4_BIG_ENDIAN) && !defined(BIG_ENDIAN_NATIVE_BUT_INCOMPATIBLE))
#  define LZ4_READ_LITTLEENDIAN_16(d,s,p) { U16 v = A16(p);
											v = lz4_bswap16(v);
											d = (s) - v; }
#  define LZ4_WRITE_LITTLEENDIAN_16(p,i)  { U16 v = (U16)(i);
											v = lz4_bswap16(v);
											A16(p) = v;
											p+=2; }
#else
#  define LZ4_READ_LITTLEENDIAN_16(d,s,p) { d = (s) - A16(p); }
#  define LZ4_WRITE_LITTLEENDIAN_16(p,v)  { A16(p) = v; p+=2; }
#endif

#define LZ4_WILDCOPY(s,d,e)     do { LZ4_COPYPACKET(s,d) } while (d<e);
#define LZ4_BLINDCOPY(s,d,l)    { BYTE* e=(d)+(l); LZ4_WILDCOPY(s,d,e); d=e; }

#if LZ4_ARCH64

static
inline
int
LZ4_NbCommonBytes (register U64 val)
{
#if defined(LZ4_BIG_ENDIAN)
    #if defined(_MSC_VER) && !defined(LZ4_FORCE_SW_BITCOUNT)
    unsigned long r = 0;
    _BitScanReverse64( &r, val );
    return (int)(r>>3);
    #elif defined(__GNUC__) && (GCC_VERSION >= 304) && !defined(LZ4_FORCE_SW_BITCOUNT)
    return (__builtin_clzll(val) >> 3);
    #else
    int r;
    if (!(val>>32)) { r=4; } else { r=0; val>>=32; }
    if (!(val>>16)) { r+=2; val>>=8; } else { val>>=24; }
    r += (!val);
    return r;
    #endif
#else
    #if defined(_MSC_VER) && !defined(LZ4_FORCE_SW_BITCOUNT)
    unsigned long r = 0;
    _BitScanForward64( &r, val );
    return (int)(r>>3);
    #elif defined(__GNUC__) && (GCC_VERSION >= 304) && !defined(LZ4_FORCE_SW_BITCOUNT)
    return (__builtin_ctzll(val) >> 3);
    #else
    static int DeBruijnBytePos[64] = {
		0, 0, 0, 0, 0, 1, 1, 2, 0, 3,
		1, 3, 1, 4, 2, 7, 0, 2, 3, 6,
		1, 5, 3, 5, 1, 3, 4, 4, 2, 5,
		6, 7, 7, 0, 1, 2, 3, 3, 4, 6,
		2, 6, 5, 5, 3, 4, 5, 6, 7, 1,
		2, 4, 6, 4, 4, 5, 7, 2, 6, 5,
		7, 6, 7, 7 };
    return DeBruijnBytePos[((U64)((val & -val) * 0x0218A392CDABBD3F)) >> 58];
    #endif
#endif
}

#else

static
inline
int
LZ4_NbCommonBytes (register U32 val)
{
#if defined(LZ4_BIG_ENDIAN)
#  if defined(_MSC_VER) && !defined(LZ4_FORCE_SW_BITCOUNT)
    unsigned long r = 0;
    _BitScanReverse( &r, val );
    return (int)(r>>3);
#  elif defined(__GNUC__) && (GCC_VERSION >= 304) && !defined(LZ4_FORCE_SW_BITCOUNT)
    return (__builtin_clz(val) >> 3);
#  else
    int r;
    if (!(val>>16)) { r=2; val>>=8; } else { r=0; val>>=24; }
    r += (!val);
    return r;
#  endif
#else
#  if defined(_MSC_VER) && !defined(LZ4_FORCE_SW_BITCOUNT)
    unsigned long r;
    _BitScanForward( &r, val );
    return (int)(r>>3);
#  elif defined(__GNUC__) && (GCC_VERSION >= 304) && !defined(LZ4_FORCE_SW_BITCOUNT)
    return (__builtin_ctz(val) >> 3);
#  else
    static int DeBruijnBytePos[32] = {
		0, 0, 3, 0, 3, 1, 3, 0, 3, 2,
		2, 1, 3, 2, 0, 1, 3, 3, 1, 2,
		2, 2, 2, 0, 3, 1, 2, 0, 1, 0,
		1, 1 };
    return DeBruijnBytePos[((U32)((val & -(S32)val) * 0x077CB531U)) >> 27];
#  endif
#endif
}

#endif

#include "hammer2_lz4_encoder.h"

void*
LZ4_create(void)
{
#ifdef __KERNEL__
	return hmalloc(HASHTABLESIZE, M_HAMMER2_LZ4, M_WAITOK);
#else
	return malloc(HASHTABLESIZE);
#endif
}

int
LZ4_free(void* ctx)
{
#ifdef __KERNEL__
	hfree(ctx, M_HAMMER2_LZ4, HASHTABLESIZE);
#else
	free(ctx);
#endif
	return 0;
}

int
LZ4_compress_limitedOutput(char* source, char* dest, int inputSize, int maxOutputSize)
{
    void* ctx = LZ4_create();
    int result;
    if (ctx == NULL) return 0;
    if (inputSize < LZ4_64KLIMIT)
        result = LZ4_compress64k_heap_limitedOutput(ctx, source, dest,
			inputSize, maxOutputSize);
    else result = LZ4_compress_heap_limitedOutput(ctx, source, dest,
			inputSize, maxOutputSize);
    LZ4_free(ctx);
    return result;
}

typedef enum { noPrefix = 0, withPrefix = 1 } prefix64k_directive;
typedef enum { endOnOutputSize = 0, endOnInputSize = 1 } end_directive;
typedef enum { full = 0, partial = 1 } exit_directive;

static
inline
int LZ4_decompress_generic(
                 char* source,
                 char* dest,
                 int inputSize,
                 int outputSize,

                 int endOnInput,
                 int prefix64k,
                 int partialDecoding,
                 int targetOutputSize
                 )
{

    BYTE* restrict ip = (BYTE*) source;
    BYTE* ref;
    BYTE* iend = ip + inputSize;

    BYTE* op = (BYTE*) dest;
    BYTE* oend = op + outputSize;
    BYTE* cpy;
    BYTE* oexit = op + targetOutputSize;

    size_t dec32table[] = {0, 3, 2, 3, 0, 0, 0, 0};
#if LZ4_ARCH64
    size_t dec64table[] = {0, 0, 0, (size_t)-1, 0, 1, 2, 3};
#endif

    if ((partialDecoding) && (oexit> oend-MFLIMIT)) oexit = oend-MFLIMIT;

    if unlikely(outputSize==0) goto _output_error;

    while (1)
    {
        unsigned token;
        size_t length;

        token = *ip++;
        if ((length=(token>>ML_BITS)) == RUN_MASK)
        {
            unsigned s=255;
            while (((endOnInput)?ip<iend:1) && (s==255))
            {
                s = *ip++;
                length += s;
            }
        }

        cpy = op+length;
        if (((endOnInput) && ((cpy>(partialDecoding?oexit:oend-MFLIMIT))
			|| (ip+length>iend-(2+1+LASTLITERALS))) )
            || ((!endOnInput) && (cpy>oend-COPYLENGTH)))
        {
            if (partialDecoding)
            {
                if (cpy > oend) goto _output_error;

                if ((endOnInput) && (ip+length > iend)) goto _output_error;

            }
            else
            {
                if ((!endOnInput) && (cpy != oend)) goto _output_error;

                if ((endOnInput) && ((ip+length != iend) || (cpy > oend)))
					goto _output_error;

            }
            memcpy(op, ip, length);
            ip += length;
            op += length;
            break;

        }
        LZ4_WILDCOPY(ip, op, cpy); ip -= (op-cpy); op = cpy;

        LZ4_READ_LITTLEENDIAN_16(ref,cpy,ip); ip+=2;
        if ((prefix64k==noPrefix) && unlikely(ref < (BYTE*)dest))
			goto _output_error;

        if ((length=(token&ML_MASK)) == ML_MASK)
        {
            while (endOnInput ? ip<iend-(LASTLITERALS+1) : 1)

            {
                unsigned s = *ip++;
                length += s;
                if (s==255) continue;
                break;
            }
        }

        if unlikely((op-ref)<STEPSIZE)
        {
#if LZ4_ARCH64
            size_t dec64 = dec64table[op-ref];
#else
            const size_t dec64 = 0;
#endif
            op[0] = ref[0];
            op[1] = ref[1];
            op[2] = ref[2];
            op[3] = ref[3];
            op += 4, ref += 4; ref -= dec32table[op-ref];
            A32(op) = A32(ref);
            op += STEPSIZE-4; ref -= dec64;
        } else { LZ4_COPYSTEP(ref,op); }
        cpy = op + length - (STEPSIZE-4);

        if unlikely(cpy>oend-(COPYLENGTH)-(STEPSIZE-4))
        {
            if (cpy > oend-LASTLITERALS) goto _output_error;

            LZ4_SECURECOPY(ref, op, (oend-COPYLENGTH));
            while(op<cpy) *op++=*ref++;
            op=cpy;
            continue;
        }
        LZ4_WILDCOPY(ref, op, cpy);
        op=cpy;
    }

    if (endOnInput)
       return (int) (((char*)op)-dest);
    else
       return (int) (((char*)ip)-source);

_output_error:
    return (int) (-(((char*)ip)-source))-1;
}

int
LZ4_decompress_safe(char* source, char* dest, int inputSize, int maxOutputSize)
{
    return LZ4_decompress_generic(source, dest, inputSize, maxOutputSize,
							endOnInputSize, noPrefix, full, 0);
}
