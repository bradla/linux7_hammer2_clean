#ifndef _CPU_ATOMIC_H_
#define _CPU_ATOMIC_H_

#ifndef _SYS_TYPES_H_
#include <sys/types.h>
#endif

#define MPLOCKED	"lock ; "
#define XACQUIRE	"repne; "
#define XRELEASE	"repe; "

#define ATOMIC_ASM(NAME, TYPE, OP, CONS, V)		\
static __inline void					\
atomic_##NAME##_##TYPE(volatile u_##TYPE *p, u_##TYPE v)\
{							\
	__asm __volatile(MPLOCKED OP			\
			 : "+m" (*p)			\
			 : CONS (V)); 			\
}							\
static __inline void					\
atomic_##NAME##_##TYPE##_xacquire(volatile u_##TYPE *p, u_##TYPE v)\
{							\
	__asm __volatile(XACQUIRE MPLOCKED OP		\
			 : "+m" (*p)			\
			 : CONS (V)); 			\
}							\
static __inline void					\
atomic_##NAME##_##TYPE##_xrelease(volatile u_##TYPE *p, u_##TYPE v)\
{							\
	__asm __volatile(XRELEASE MPLOCKED OP		\
			 : "+m" (*p)			\
			 : CONS (V)); 			\
}							\
static __inline void					\
atomic_##NAME##_##TYPE##_nonlocked(volatile u_##TYPE *p, u_##TYPE v)\
{							\
	__asm __volatile(OP				\
			 : "+m" (*p)			\
			 : CONS (V)); 			\
}

ATOMIC_ASM(set,	     char,  "orb %b1,%0",  "iq",   v)
ATOMIC_ASM(clear,    char,  "andb %b1,%0", "iq",   ~v)
ATOMIC_ASM(add,	     char,  "addb %b1,%0", "iq",   v)
ATOMIC_ASM(subtract, char,  "subb %b1,%0", "iq",   v)

ATOMIC_ASM(set,	     short, "orw %w1,%0",  "iq",   v)
ATOMIC_ASM(clear,    short, "andw %w1,%0", "iq",  ~v)
ATOMIC_ASM(add,	     short, "addw %w1,%0", "iq",   v)
ATOMIC_ASM(subtract, short, "subw %w1,%0", "iq",   v)

ATOMIC_ASM(set,	     int,   "orl %1,%0",  "iq",   v)
ATOMIC_ASM(clear,    int,   "andl %1,%0", "iq",  ~v)
ATOMIC_ASM(add,	     int,   "addl %1,%0", "iq",   v)
ATOMIC_ASM(subtract, int,   "subl %1,%0", "iq",   v)

ATOMIC_ASM(set,	     long,  "orq %1,%0",  "r",   v)
ATOMIC_ASM(clear,    long,  "andq %1,%0", "r",  ~v)
ATOMIC_ASM(add,	     long,  "addq %1,%0", "r",   v)
ATOMIC_ASM(subtract, long,  "subq %1,%0", "r",   v)

static __inline u_long
atomic_readandclear_long(volatile u_long *addr)
{
	u_long res;

	res = 0;
	__asm __volatile(
	"	xchgq	%1,%0 ;		"
	"# atomic_readandclear_long"
	: "+r" (res),
	  "=m" (*addr)
	: "m" (*addr));

	return (res);
}

static __inline u_int
atomic_readandclear_int(volatile u_int *addr)
{
	u_int res;

	res = 0;
	__asm __volatile(
	"	xchgl	%1,%0 ;		"
	"# atomic_readandclear_int"
	: "+r" (res),
	  "=m" (*addr)
	: "m" (*addr));

	return (res);
}

static __inline int
atomic_swap_int(volatile int *addr, int value)
{
	__asm __volatile("xchgl %0, %1" :
	    "=r" (value), "=m" (*addr) : "0" (value) : "memory");
	return (value);
}

static __inline long
atomic_swap_long(volatile long *addr, long value)
{
	__asm __volatile("xchgq %0, %1" :
	    "=r" (value), "=m" (*addr) : "0" (value) : "memory");
	return (value);
}

static __inline void *
atomic_swap_ptr(volatile void **addr, void *value)
{
	__asm __volatile("xchgq %0, %1" :
	    "=r" (value), "=m" (*addr) : "0" (value) : "memory");
	return (value);
}

static __inline int
atomic_poll_acquire_int(volatile u_int *p)
{
	u_int data;

	__asm __volatile(MPLOCKED "btsl $0,%0; setnc %%al; andl $255,%%eax" : "+m" (*p), "=a" (data));
	return(data);
}

static __inline void
atomic_poll_release_int(volatile u_int *p)
{
	__asm __volatile(MPLOCKED "btrl $0,%0" : "+m" (*p));
}

typedef volatile int	__atomic_intr_t;

static __inline void
atomic_intr_init(__atomic_intr_t *p)
{
	*p = 0;
}

static __inline int
atomic_intr_handler_disable(__atomic_intr_t *p)
{
	int data;

	__asm __volatile(MPLOCKED "orl $0x40000000,%1; movl %1,%%eax; " \
				  "andl $0x80000000,%%eax" \
				  : "=a"(data) , "+m"(*p));
	return(data);
}

static __inline void
atomic_intr_handler_enable(__atomic_intr_t *p)
{
	__asm __volatile(MPLOCKED "andl $0xBFFFFFFF,%0" : "+m" (*p));
}

static __inline int
atomic_intr_handler_is_enabled(__atomic_intr_t *p)
{
	int data;

	__asm __volatile("movl %1,%%eax; andl $0x40000000,%%eax" \
			 : "=a"(data) : "m"(*p));
	return(data);
}

static __inline void
atomic_intr_cond_inc(__atomic_intr_t *p)
{
	__asm __volatile(MPLOCKED "incl %0" : "+m" (*p));
}

static __inline void
atomic_intr_cond_dec(__atomic_intr_t *p)
{
	__asm __volatile(MPLOCKED "decl %0" : "+m" (*p));
}

static __inline void
atomic_intr_cond_enter(__atomic_intr_t *p, void (*func)(void *), void *arg)
{
	__asm __volatile(MPLOCKED "btsl $31,%0; jnc 3f; " \
			 MPLOCKED "incl %0; " \
			 "1: ;" \
			 MPLOCKED "btsl $31,%0; jnc 2f; " \
			 "movq %2,%%rdi; call *%1; " \
			 "jmp 1b; " \
			 "2: ;" \
			 MPLOCKED "decl %0; " \
			 "3: ;" \
			 : "+m" (*p) \
			 : "r"(func), "m"(arg) \
			 : "ax", "cx", "dx", "rsi", "rdi", "r8", "r9", "r10", "r11");

}

static __inline int
atomic_intr_cond_try(__atomic_intr_t *p)
{
	int ret;

	__asm __volatile("subl %%eax,%%eax; "			\
			 MPLOCKED "btsl $31,%0; jnc 2f; "	\
			 "movl $1,%%eax;"			\
			 "2: ;"
			 : "+m" (*p), "=&a"(ret)
                         : : "cx", "dx");
	return (ret);
}

static __inline int
atomic_intr_cond_test(__atomic_intr_t *p)
{
	return((int)(*p & 0x80000000));
}

static __inline void
atomic_intr_cond_exit(__atomic_intr_t *p, void (*func)(void *), void *arg)
{
	__asm __volatile(MPLOCKED "btrl $31,%0; " \
			 "testl $0x3FFFFFFF,%0; jz 1f; " \
			 "movq %2,%%rdi; call *%1; " \
			 "1: ;" \
			 : "+m" (*p) \
			 : "r"(func), "m"(arg) \
			 : "ax", "cx", "dx", "rsi", "rdi", "r8", "r9", "r10", "r11");

}

static __inline int
atomic_cmpxchg_int(volatile u_int *_dst, u_int _old, u_int _new)
{
	u_int res = _old;

	__asm __volatile(MPLOCKED "cmpxchgl %2,%1; " \
			 : "+a" (res), "=m" (*_dst) \
			 : "r" (_new), "m" (*_dst) \
			 : "memory");
	return (res);
}

static __inline int
atomic_cmpxchg_long_test(volatile u_long *_dst, u_long _old, u_long _new)
{
	u_int res = _old;

	__asm __volatile(MPLOCKED "cmpxchgq %2,%1; "
				  " setz %%al;"
				  " movsbq %%al,%%rax" \
			 : "+a" (res), "=m" (*_dst) \
			 : "r" (_new), "m" (*_dst) \
			 : "memory");
	return (res);
}

static __inline int
atomic_cmpset_short(volatile u_short *_dst, u_short _old, u_short _new)
{
	u_short res = _old;

	__asm __volatile(MPLOCKED "cmpxchgw %w2,%1; " \
			 : "+a" (res), "=m" (*_dst) \
			 : "r" (_new), "m" (*_dst) \
			 : "memory");
	return (res == _old);
}

static __inline int
atomic_fcmpset_char(volatile u_char *_dst, u_char *_old, u_char _new)
{
	u_char res = *_old;

	__asm __volatile(MPLOCKED "cmpxchgb %2,%0; " \
			 : "+m" (*_dst),
			   "+a" (*_old)
			 : "r" (_new)
			 : "memory", "cc");
	return (res == *_old);
}

static __inline int
atomic_fcmpset_short(volatile u_short *_dst, u_short *_old, u_short _new)
{
	u_short res = *_old;

	__asm __volatile(MPLOCKED "cmpxchgw %2,%0; " \
			 : "+m" (*_dst),
			   "+a" (*_old)
			 : "r" (_new)
			 : "memory", "cc");
	return (res == *_old);
}

static __inline int
atomic_cmpset_int(volatile u_int *_dst, u_int _old, u_int _new)
{
	u_int res = _old;

	__asm __volatile(MPLOCKED "cmpxchgl %2,%1; " \
			 : "+a" (res), "=m" (*_dst) \
			 : "r" (_new), "m" (*_dst) \
			 : "memory");
	return (res == _old);
}

static __inline int
atomic_fcmpset_int(volatile u_int *_dst, u_int *_old, u_int _new)
{
	u_int res = *_old;

	__asm __volatile(MPLOCKED "cmpxchgl %2,%0; " \
			 : "+m" (*_dst),
			   "+a" (*_old)
			 : "r" (_new)
			 : "memory", "cc");
	return (res == *_old);
}

static __inline int
atomic_cmpset_int_xacquire(volatile u_int *_dst, u_int _old, u_int _new)
{
	u_int res = _old;

	__asm __volatile(XACQUIRE MPLOCKED "cmpxchgl %2,%1; " \
			 : "+a" (res), "=m" (*_dst) \
			 : "r" (_new), "m" (*_dst) \
			 : "memory");
	return (res == _old);
}

static __inline int
atomic_cmpset_int_xrelease(volatile u_int *_dst, u_int _old, u_int _new)
{
	u_int res = _old;

	__asm __volatile(XRELEASE MPLOCKED "cmpxchgl %2,%1; " \
			 : "+a" (res), "=m" (*_dst) \
			 : "r" (_new), "m" (*_dst) \
			 : "memory");
	return (res == _old);
}

static __inline int
atomic_cmpset_long(volatile u_long *_dst, u_long _old, u_long _new)
{
	u_long res = _old;

	__asm __volatile(MPLOCKED "cmpxchgq %2,%1; " \
			 : "+a" (res), "=m" (*_dst) \
			 : "r" (_new), "m" (*_dst) \
			 : "memory");
	return (res == _old);
}

static __inline int
atomic_fcmpset_long(volatile u_long *_dst, u_long *_old, u_long _new)
{
	u_long res = *_old;

	__asm __volatile(MPLOCKED "cmpxchgq %2,%0; " \
			 : "+m" (*_dst),
			   "+a" (*_old)
			 : "r" (_new)
			 : "memory", "cc");
	return (res == *_old);
}

static __inline int
atomic_cmpset_long_xacquire(volatile u_long *_dst, u_long _old, u_long _new)
{
	u_long res = _old;

	__asm __volatile(XACQUIRE MPLOCKED "cmpxchgq %2,%1; " \
			 : "+a" (res), "=m" (*_dst) \
			 : "r" (_new), "m" (*_dst) \
			 : "memory");
	return (res == _old);
}

static __inline int
atomic_cmpset_long_xrelease(volatile u_long *_dst, u_long _old, u_long _new)
{
	u_long res = _old;

	__asm __volatile(XRELEASE MPLOCKED "cmpxchgq %2,%1; " \
			 : "+a" (res), "=m" (*_dst) \
			 : "r" (_new), "m" (*_dst) \
			 : "memory");
	return (res == _old);
}

static inline void *
atomic_cas_ptr(volatile void *p, void *e, void *n)
{
	__asm volatile(MPLOCKED " cmpxchgq %2, %1"
	    : "=a" (n), "=m" (*(volatile unsigned long *)p)
	    : "r" (n), "a" (e), "m" (*(volatile unsigned long *)p));

	return (n);
}

static __inline u_int
atomic_fetchadd_int(volatile u_int *_p, u_int _v)
{
	__asm __volatile(MPLOCKED "xaddl %0,%1; " \
			 : "+r" (_v), "=m" (*_p)	\
			 : "m" (*_p)		\
			 : "memory");
	return (_v);
}

static __inline u_int
atomic_fetchadd_int_xacquire(volatile u_int *_p, u_int _v)
{
	__asm __volatile(XACQUIRE MPLOCKED "xaddl %0,%1; " \
			 : "+r" (_v), "=m" (*_p)	\
			 : "m" (*_p)		\
			 : "memory");
	return (_v);
}

static __inline u_int
atomic_fetchadd_int_xrelease(volatile u_int *_p, u_int _v)
{
	__asm __volatile(XRELEASE MPLOCKED "xaddl %0,%1; " \
			 : "+r" (_v), "=m" (*_p)	\
			 : "m" (*_p)		\
			 : "memory");
	return (_v);
}

static __inline u_long
atomic_fetchadd_long(volatile u_long *_p, u_long _v)
{
	__asm __volatile(MPLOCKED "xaddq %0,%1; " \
			 : "+r" (_v), "=m" (*_p)	\
			 : "m" (*_p)		\
			 : "memory");
	return (_v);
}

static __inline u_long
atomic_fetchadd_long_xacquire(volatile u_long *_p, u_long _v)
{
	__asm __volatile(XACQUIRE MPLOCKED "xaddq %0,%1; " \
			 : "+r" (_v), "=m" (*_p)	\
			 : "m" (*_p)		\
			 : "memory");
	return (_v);
}

static __inline u_long
atomic_fetchadd_long_xrelease(volatile u_long *_p, u_long _v)
{
	__asm __volatile(XRELEASE MPLOCKED "xaddq %0,%1; " \
			 : "+r" (_v), "=m" (*_p)	\
			 : "m" (*_p)		\
			 : "memory");
	return (_v);
}

static __inline int
atomic_testandset_int(volatile u_int *p, u_int v)
{
	u_char res;

	__asm __volatile(
	"	" MPLOCKED "		"
	"	btsl	%2,%1 ;		"
	"	setc	%0 ;		"
	"# atomic_testandset_int"
	: "=q" (res),
	  "+m" (*p)
	: "Ir" (v & 0x1f)
	: "cc");
	return (res);
}

static __inline int
atomic_testandset_long(volatile u_long *p, u_long v)
{
	u_char res;

	__asm __volatile(
	"	" MPLOCKED "		"
	"	btsq	%2,%1 ;		"
	"	setc	%0 ;		"
	"# atomic_testandset_long"
	: "=q" (res),
	  "+m" (*p)
	: "Ir" (v & 0x3f)
	: "cc");
	return (res);
}

static __inline int
atomic_testandclear_int(volatile u_int *p, u_int v)
{
	u_char res;

	__asm __volatile(
	"	" MPLOCKED "		"
	"	btrl	%2,%1 ;		"
	"	setc	%0 ;		"
	"# atomic_testandclear_int"
	: "=q" (res),
	  "+m" (*p)
	: "Ir" (v & 0x1f)
	: "cc");
	return (res);
}

static __inline int
atomic_testandclear_long(volatile u_long *p, u_long v)
{
	u_char res;

	__asm __volatile(
	"	" MPLOCKED "		"
	"	btrq	%2,%1 ;		"
	"	setc	%0 ;		"
	"# atomic_testandclear_long"
	: "=q" (res),
	  "+m" (*p)
	: "Ir" (v & 0x3f)
	: "cc");
	return (res);
}

#define ATOMIC_STORE_LOAD(TYPE, LOP, SOP)		\
static __inline u_##TYPE				\
atomic_load_acq_##TYPE(volatile u_##TYPE *p)		\
{							\
	u_##TYPE res;  	\
							\
	__asm __volatile(MPLOCKED LOP			\
	: "=a" (res),			 		\
	  "=m" (*p)			 		\
	: "m" (*p)			 		\
	: "memory");					\
							\
	return (res);					\
}							\
							\

							\
static __inline void					\
atomic_store_rel_##TYPE(volatile u_##TYPE *p, u_##TYPE v)\
{							\
	__asm __volatile(SOP				\
	: "=m" (*p),			 		\
	  "+r" (v)			 		\
	: "m" (*p));			 		\
}							\
struct __hack

ATOMIC_STORE_LOAD(char, "cmpxchgb %b0,%1", "xchgb %b1,%0");
ATOMIC_STORE_LOAD(short,"cmpxchgw %w0,%1", "xchgw %w1,%0");
ATOMIC_STORE_LOAD(int,  "cmpxchgl %0,%1",  "xchgl %1,%0");
ATOMIC_STORE_LOAD(long, "cmpxchgq %0,%1",  "xchgq %1,%0");

#undef ATOMIC_ASM
#undef ATOMIC_STORE_LOAD

#define	atomic_set_acq_char		atomic_set_char
#define	atomic_set_rel_char		atomic_set_char
#define	atomic_clear_acq_char		atomic_clear_char
#define	atomic_clear_rel_char		atomic_clear_char
#define	atomic_add_acq_char		atomic_add_char
#define	atomic_add_rel_char		atomic_add_char
#define	atomic_subtract_acq_char	atomic_subtract_char
#define	atomic_subtract_rel_char	atomic_subtract_char

#define	atomic_set_acq_short		atomic_set_short
#define	atomic_set_rel_short		atomic_set_short
#define	atomic_clear_acq_short		atomic_clear_short
#define	atomic_clear_rel_short		atomic_clear_short
#define	atomic_add_acq_short		atomic_add_short
#define	atomic_add_rel_short		atomic_add_short
#define	atomic_subtract_acq_short	atomic_subtract_short
#define	atomic_subtract_rel_short	atomic_subtract_short

#define	atomic_set_acq_int		atomic_set_int
#define	atomic_set_rel_int		atomic_set_int
#define	atomic_clear_acq_int		atomic_clear_int
#define	atomic_clear_rel_int		atomic_clear_int
#define	atomic_add_acq_int		atomic_add_int
#define	atomic_add_rel_int		atomic_add_int
#define	atomic_subtract_acq_int		atomic_subtract_int
#define	atomic_subtract_rel_int		atomic_subtract_int
#define	atomic_cmpset_acq_int		atomic_cmpset_int
#define	atomic_cmpset_rel_int		atomic_cmpset_int

#define	atomic_set_acq_long		atomic_set_long
#define	atomic_set_rel_long		atomic_set_long
#define	atomic_clear_acq_long		atomic_clear_long
#define	atomic_clear_rel_long		atomic_clear_long
#define	atomic_add_acq_long		atomic_add_long
#define	atomic_add_rel_long		atomic_add_long
#define	atomic_subtract_acq_long	atomic_subtract_long
#define	atomic_subtract_rel_long	atomic_subtract_long
#define	atomic_cmpset_acq_long		atomic_cmpset_long
#define	atomic_cmpset_rel_long		atomic_cmpset_long

#define atomic_set_cpumask		atomic_set_long
#define atomic_clear_cpumask		atomic_clear_long
#define atomic_cmpset_cpumask		atomic_cmpset_long
#define atomic_store_rel_cpumask	atomic_store_rel_long
#define atomic_load_acq_cpumask		atomic_load_acq_long

#define	atomic_set_8		atomic_set_char
#define	atomic_set_acq_8	atomic_set_acq_char
#define	atomic_set_rel_8	atomic_set_rel_char
#define	atomic_clear_8		atomic_clear_char
#define	atomic_clear_acq_8	atomic_clear_acq_char
#define	atomic_clear_rel_8	atomic_clear_rel_char
#define	atomic_add_8		atomic_add_char
#define	atomic_add_acq_8	atomic_add_acq_char
#define	atomic_add_rel_8	atomic_add_rel_char
#define	atomic_subtract_8	atomic_subtract_char
#define	atomic_subtract_acq_8	atomic_subtract_acq_char
#define	atomic_subtract_rel_8	atomic_subtract_rel_char
#define	atomic_load_acq_8	atomic_load_acq_char
#define	atomic_store_rel_8	atomic_store_rel_char
#define	atomic_fcmpset_8	atomic_fcmpset_char

#define	atomic_set_16		atomic_set_short
#define	atomic_set_acq_16	atomic_set_acq_short
#define	atomic_set_rel_16	atomic_set_rel_short
#define	atomic_clear_16		atomic_clear_short
#define	atomic_clear_acq_16	atomic_clear_acq_short
#define	atomic_clear_rel_16	atomic_clear_rel_short
#define	atomic_add_16		atomic_add_short
#define	atomic_add_acq_16	atomic_add_acq_short
#define	atomic_add_rel_16	atomic_add_rel_short
#define	atomic_subtract_16	atomic_subtract_short
#define	atomic_subtract_acq_16	atomic_subtract_acq_short
#define	atomic_subtract_rel_16	atomic_subtract_rel_short
#define	atomic_load_acq_16	atomic_load_acq_short
#define	atomic_store_rel_16	atomic_store_rel_short
#define	atomic_fcmpset_16	atomic_fcmpset_short

#define	atomic_set_32		atomic_set_int
#define	atomic_set_acq_32	atomic_set_acq_int
#define	atomic_set_rel_32	atomic_set_rel_int
#define	atomic_clear_32		atomic_clear_int
#define	atomic_clear_acq_32	atomic_clear_acq_int
#define	atomic_clear_rel_32	atomic_clear_rel_int
#define	atomic_add_32		atomic_add_int
#define	atomic_add_acq_32	atomic_add_acq_int
#define	atomic_add_rel_32	atomic_add_rel_int
#define	atomic_subtract_32	atomic_subtract_int
#define	atomic_subtract_acq_32	atomic_subtract_acq_int
#define	atomic_subtract_rel_32	atomic_subtract_rel_int
#define	atomic_load_acq_32	atomic_load_acq_int
#define	atomic_store_rel_32	atomic_store_rel_int
#define	atomic_cmpset_32	atomic_cmpset_int
#define	atomic_fcmpset_32	atomic_fcmpset_int
#define	atomic_cmpset_acq_32	atomic_cmpset_acq_int
#define	atomic_cmpset_rel_32	atomic_cmpset_rel_int
#define	atomic_readandclear_32	atomic_readandclear_int
#define	atomic_fetchadd_32	atomic_fetchadd_int

#define	atomic_load_acq_64	atomic_load_acq_long
#define	atomic_store_rel_64	atomic_store_rel_long
#define	atomic_swap_64		atomic_swap_long
#define	atomic_fetchadd_64	atomic_fetchadd_long
#define	atomic_add_64		atomic_add_long
#define atomic_cmpset_64	atomic_cmpset_long
#define atomic_fcmpset_64	atomic_fcmpset_long
#define atomic_set_64		atomic_set_long
#define atomic_clear_64		atomic_clear_long

#define atomic_set_ptr(p, v) \
	atomic_set_long((volatile u_long *)(p), (u_long)(v))
#define atomic_set_acq_ptr(p, v) \
	atomic_set_acq_long((volatile u_long *)(p), (u_long)(v))
#define atomic_set_rel_ptr(p, v) \
	atomic_set_rel_long((volatile u_long *)(p), (u_long)(v))
#define atomic_clear_ptr(p, v) \
	atomic_clear_long((volatile u_long *)(p), (u_long)(v))
#define atomic_clear_acq_ptr(p, v) \
	atomic_clear_acq_long((volatile u_long *)(p), (u_long)(v))
#define atomic_clear_rel_ptr(p, v) \
	atomic_clear_rel_long((volatile u_long *)(p), (u_long)(v))
#define atomic_add_ptr(p, v) \
	atomic_add_long((volatile u_long *)(p), (u_long)(v))
#define atomic_add_acq_ptr(p, v) \
	atomic_add_acq_long((volatile u_long *)(p), (u_long)(v))
#define atomic_add_rel_ptr(p, v) \
	atomic_add_rel_long((volatile u_long *)(p), (u_long)(v))
#define atomic_subtract_ptr(p, v) \
	atomic_subtract_long((volatile u_long *)(p), (u_long)(v))
#define atomic_subtract_acq_ptr(p, v) \
	atomic_subtract_acq_long((volatile u_long *)(p), (u_long)(v))
#define atomic_subtract_rel_ptr(p, v) \
	atomic_subtract_rel_long((volatile u_long *)(p), (u_long)(v))
#define atomic_load_acq_ptr(p) \
	atomic_load_acq_long((volatile u_long *)(p))
#define atomic_store_rel_ptr(p, v) \
	atomic_store_rel_long((volatile u_long *)(p), (v))
#define atomic_cmpset_ptr(dst, old, new) 				\
	atomic_cmpset_long((volatile u_long *)(dst), (u_long)(old),	\
				(u_long)(new))
#define atomic_fcmpset_ptr(dst, old, new) 				\
	atomic_fcmpset_long((volatile u_long *)(dst), (u_long *)(old),	\
				(u_long)(new))
#define atomic_cmpset_acq_ptr(dst, old, new)				\
	atomic_cmpset_acq_long((volatile u_long *)(dst), (u_long)(old), \
				(u_long)(new))
#define atomic_cmpset_rel_ptr(dst, old, new)				\
	atomic_cmpset_rel_long((volatile u_long *)(dst), (u_long)(old), \
				(u_long)(new))
#define atomic_readandclear_ptr(p)					\
	atomic_readandclear_long((volatile u_long *)(p))

#endif
