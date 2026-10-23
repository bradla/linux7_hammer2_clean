#ifndef _HAMMER2_COMPAT_H_
#define _HAMMER2_COMPAT_H_

#include <linux/kernel.h>
#include <linux/bug.h>
#include <linux/string.h>
#include <linux/sched.h>
#include <linux/jiffies.h>
#include <linux/types.h>
#include <linux/limits.h>

#ifndef UINT32_MAX
#define UINT32_MAX	(0xffffffffU)
#endif
#ifndef UINT64_MAX
#define UINT64_MAX	(0xffffffffffffffffULL)
#endif
#ifndef INT32_MAX
#define INT32_MAX	(0x7fffffff)
#endif
#ifndef INT64_MAX
#define INT64_MAX	(0x7fffffffffffffffLL)
#endif
#include <asm/barrier.h>
#include <asm/processor.h>

struct uuid {
	uint32_t	time_low;
	uint16_t	time_mid;
	uint16_t	time_hi_and_version;
	uint8_t		clock_seq_hi_and_reserved;
	uint8_t		clock_seq_low;
	uint8_t		node[6];
};

#ifndef bzero
#define bzero(p, n)	memset((p), 0, (n))
#endif
#ifndef bcopy
#define bcopy(src, dst, n) memmove((dst), (src), (n))
#endif
#ifndef bcmp
#define bcmp(a, b, n)	memcmp((a), (b), (n))
#endif

#include <linux/slab.h>

#define M_WAITOK	GFP_KERNEL
#define M_NOWAIT	GFP_ATOMIC
#define M_ZERO		0x80000000U
#define M_NULL		0
#define M_HAMMER	NULL
#define M_HAMMER2	NULL
#define M_TEMP		NULL
#define M_FORCE		0
#define M_VOLHDRS	NULL
#define M_IGNINO	0
#define M_PERMANENT	0
#define M_END		0
#define M_LEVEL		0
#define M_X		0

static inline void *
hammer2_kmalloc(size_t sz, void *tag, unsigned int flags)
{
	gfp_t gfp = (flags & ~M_ZERO) ? (gfp_t)(flags & ~M_ZERO) : GFP_KERNEL;

	if (flags & M_ZERO)
		return kvzalloc(sz, gfp);
	return kvmalloc(sz, gfp);
}

static inline void
hammer2_kfree(void *p, void *tag)
{
	kfree(p);
}

#define hmalloc(sz, tag, fl)		hammer2_kmalloc((sz), (tag), (fl))

#define hfree(p, ...)			kvfree((p))

static inline void *
hammer2_krealloc(void *p, size_t sz, void *tag, unsigned int flags)
{
	gfp_t gfp = (flags & ~M_ZERO) ? (gfp_t)(flags & ~M_ZERO) : GFP_KERNEL;
	void *np = krealloc(p, sz, gfp);

	return np;
}
#define hrealloc(p, sz, tag, fl)	hammer2_krealloc((p), (sz), (tag), (fl))

#define adjust_malloc_leak(delta, tag)	do { (void)(delta); } while (0)

#define MALLOC_DEFINE(tag, short_descr, long_descr) \
	struct __hammer2_malloc_define_##tag { int dummy; }
#define SYSCTL_NODE(parent, nbr, name, access, handler, descr) \
	struct __hammer2_sysctl_node_##name { int dummy; }
#define SYSCTL_INT(parent, nbr, name, access, ptr, val, descr) \
	struct __hammer2_sysctl_int_##name { int dummy; }
#define SYSCTL_LONG(parent, nbr, name, access, ptr, val, descr) \
	struct __hammer2_sysctl_long_##name { int dummy; }
#define OID_AUTO	0
#define CTLFLAG_RW	0
#define CTLFLAG_RD	0
#define UMA_ALIGN_PTR	0

#ifndef MNT_UPDATE
#define MNT_UPDATE	0x10000
#endif
#ifndef MNT_LOCAL
#define MNT_LOCAL	0x1000
#endif
#ifndef MNAMELEN
#define MNAMELEN	1024
#endif

static const int nbuf = 1024;
static const int desiredvnodes = 1024;
static const int maxphys = (1 << 20);

static inline unsigned long *hashinit(int elements, void *type,
				      unsigned long *hashmask)
{
	int i, sz = roundup_pow_of_two(elements);
	struct list_head *t = kzalloc(sz * sizeof(struct list_head), GFP_KERNEL);

	if (t)
		for (i = 0; i < sz; ++i)
			INIT_LIST_HEAD(&t[i]);
	if (hashmask)
		*hashmask = sz - 1;
	(void)type;
	return (unsigned long *)t;
}
static inline void hashdestroy(void *table, void *type, unsigned long hashmask)
{
	(void)hashmask;
	(void)type;
	kfree(table);
}
#define HASH_LIST	0
#define M_HAMMER2_HASH	NULL

static inline char *hstrdup(const char *s) { return kstrdup(s, GFP_KERNEL); }
static inline void hstrfree(char *s)       { kfree(s); }

#define TAILQ_EMPTY(hp)		list_empty(&(hp)->head)

struct h2_mount_opt {
	const char	*name;
	void		*value;
	int		len;
};
struct h2_mount_optlist {
	int			count;
	struct h2_mount_opt	opts[8];
};
static inline int vfs_getopt(void *optsv, const char *name,
			     void **buf, int *len)
{
	struct h2_mount_optlist *ol = optsv;
	int i;

	if (ol) {
		for (i = 0; i < ol->count; ++i) {
			if (strcmp(ol->opts[i].name, name) == 0) {
				if (buf)
					*buf = ol->opts[i].value;
				if (len)
					*len = ol->opts[i].len;
				return 0;
			}
		}
	}
	if (buf) *buf = NULL;
	if (len) *len = 0;
	return ENOENT;
}
static inline int vfs_filteropt(void *opts, const char **legal)
{
	(void)opts; (void)legal;
	return 0;
}

#define MPTOPMP(mp)	((hammer2_pfs_t *)((struct mount *)(mp))->mnt_data)

struct h2statfs {
	long			f_iosize;
	long			f_bsize;
	char			f_mntfromname[MNAMELEN];
	char			f_mntonname[MNAMELEN];
	struct { int val[2]; } f_fsid;
	uint64_t		f_blocks;
	uint64_t		f_bfree;
	uint64_t		f_bavail;
	uint64_t		f_files;
	uint64_t		f_ffree;
};

struct mount {
	int				mnt_flag;
	int				mnt_kern_flag;
	void				*mnt_data;
	void				*mnt_optnew;
	struct h2statfs			mnt_stat;
	void				*mnt_vfc;
	long				mnt_iosize_max;
};

#define MNT_ILOCK(mp)		do { (void)(mp); } while (0)
#define MNT_IUNLOCK(mp)		do { (void)(mp); } while (0)
#define MNT_KERN_LOOKUP_SHARED	0
#define MNT_KERN_EXTENDED_SHARED 0

#ifndef MNTK_LOOKUP_SHARED
#define MNTK_LOOKUP_SHARED	0
#endif
#ifndef MNTK_EXTENDED_SHARED
#define MNTK_EXTENDED_SHARED	0
#endif
#ifndef MNTK_USES_BCACHE
#define MNTK_USES_BCACHE	0
#endif
#ifndef FORCECLOSE
#define FORCECLOSE	0
#endif

typedef long *qaddr_t;
#ifndef caddr_t
typedef char *caddr_t;
#endif

#define curthread	((struct thread *)current)
struct thread;

typedef struct thread *thread_t;

#ifndef ksnprintf
#define ksnprintf	snprintf
#endif
#include <linux/utsname.h>
#ifndef hostname
#define hostname	(init_utsname()->nodename)
#endif
static inline void
printf_uuid(const void *uuid)
{

	if (uuid)
		printk(KERN_CONT "%pUb", uuid);
}

#ifndef LOCKENTER
#define LOCKENTER	do { } while (0)
#endif
#ifndef LOCKEXIT
#define LOCKEXIT	do { } while (0)
#endif

struct fid {
	unsigned short	fid_len;
	unsigned short	fid_reserved;
	char		fid_data[28];
};

#ifndef LK_EXCLUSIVE
#define LK_EXCLUSIVE	0
#define LK_SHARED	0
#define LK_NOWAIT	0
#define LK_RETRY	0
#define LK_INTERLOCK	0
#define LK_NOWITNESS	0
#endif
#ifndef LK_RELEASE
#define LK_RELEASE	0x100
#endif

struct lock {
	struct rw_semaphore	lk_rw;
};
#define lockinit(lk, name, timo, flags)	init_rwsem(&(lk)->lk_rw)
#define lockuninit(lk)			do { } while (0)
#define lockmgr(lk, flags)						\
	do {								\
		if ((flags) & LK_RELEASE)				\
			up_write(&(lk)->lk_rw);				\
		else							\
			down_write(&(lk)->lk_rw);			\
	} while (0)

#ifndef PCATCH
#define PCATCH	0
#define PRIBIO	0
#endif

static inline int vflush(struct mount *mp, int rootrefs, int flags, void *td)
{
	(void)mp; (void)rootrefs; (void)flags; (void)td;
	return 0;
}
static inline void vfs_mountedfrom(struct mount *mp, const char *name)
{
	(void)mp; (void)name;
}
static inline int vfs_flagopt(void *opts, const char *name, int *flagp, int flag)
{
	(void)opts; (void)name; (void)flagp; (void)flag;
	return 0;
}

struct vfsconf;
struct vfsops {
	int  (*vfs_init)(struct vfsconf *);
	int  (*vfs_uninit)(struct vfsconf *);
	int  (*vfs_mount)(struct mount *);
	int  (*vfs_unmount)(struct mount *, int);
	int  (*vfs_sync)(struct mount *, int);
	int  (*vfs_vget)(struct mount *, ino_t, int, struct inode **);
	int  (*vfs_root)(struct mount *, int, struct inode **);
	int  (*vfs_statfs)(struct mount *, struct h2statfs *);
	int  (*vfs_fhtovp)(struct mount *, struct fid *, int, struct inode **);
};
#define VFS_SET(ops, name, flag) \
	static int __maybe_unused __h2_vfs_set_##name = 0
#define MODULE_VERSION(name, ver) \
	static int __maybe_unused __h2_modver_##name = (ver)

#include <linux/fs.h>

#define VBAD	0
#define VNON	0
#define VDIR	DT_DIR
#define VREG	DT_REG
#define VFIFO	DT_FIFO
#define VCHR	DT_CHR
#define VBLK	DT_BLK
#define VLNK	DT_LNK
#define VSOCK	DT_SOCK

#define hprintf(fmt, ...)		pr_info_ratelimited("hammer2: " fmt, ##__VA_ARGS__)
#define debug_hprintf(fmt, ...)		pr_debug("hammer2: " fmt, ##__VA_ARGS__)

#define hprintf_rl(fmt, ...)		pr_info_ratelimited("hammer2: " fmt, ##__VA_ARGS__)

#define hprintf_diag(fmt, ...)		pr_info("hammer2: " fmt, ##__VA_ARGS__)

#define printf(fmt, ...)		pr_info(fmt, ##__VA_ARGS__)

#ifndef howmany
#define howmany(x, y)	(((x) + ((y) - 1)) / (y))
#endif

#ifndef rounddown2
#define rounddown2(x, y)	((x) & ~((y) - 1))
#endif

#ifndef DEV_BSIZE
#define DEV_BSIZE	512
#endif

#ifndef MNT_RDONLY
#define MNT_RDONLY	0x00000001
#endif
#ifndef MNT_WAIT
#define MNT_WAIT	1
#endif
#ifndef MNT_NOWAIT
#define MNT_NOWAIT	2
#endif

#include <linux/uaccess.h>
static inline int hammer2_copyout(const void *kern, void __user *user, size_t n)
{
	return copy_to_user(user, kern, n) ? -EFAULT : 0;
}
static inline int hammer2_copyin(const void __user *user, void *kern, size_t n)
{
	return copy_from_user(kern, user, n) ? -EFAULT : 0;
}
#define copyout(kern, user, n)	hammer2_copyout((kern), (user), (n))
#define copyin(user, kern, n)	hammer2_copyin((user), (kern), (n))

struct vattr {
	mode_t			va_mode;
	uid_t			va_uid;
	gid_t			va_gid;
	dev_t			va_rdev;
	uint8_t			va_type;
	uint64_t		va_fsid;
	uint64_t		va_fileid;
	uint64_t		va_nlink;
	uint64_t		va_size;
	uint32_t		va_flags;
	struct timespec64	va_ctime;
	struct timespec64	va_mtime;
	struct timespec64	va_atime;
	struct timespec64	va_birthtime;
	uint64_t		va_gen;
	uint32_t		va_blocksize;
	uint64_t		va_bytes;
	uint64_t		va_filerev;
	int			va_vaflags;
};

#include <linux/socket.h>
#define cr_uid		uid
#define cr_gid		gid

static inline int groupmember(gid_t gid, const struct ucred *cred)
{
	(void)gid;
	(void)cred;
	return 0;
}

static inline int priv_check_cred(const struct ucred *cred, int priv)
{
	(void)cred;
	(void)priv;
	return EPERM;
}
#define PRIV_VFS_RETAINSUGID	0
#define PRIV_VFS_CHOWN		0
#define PRIV_VFS_SETGID		0
#define PRIV_VFS_STICKYFILE	0
#define PRIV_VFS_SYSFLAGS	0

#ifndef ALLPERMS
#define ALLPERMS	(S_ISUID | S_ISGID | S_ISVTX | 0777)
#endif
#ifndef S_ISTXT
#define S_ISTXT		S_ISVTX
#endif
#ifndef EFTYPE
#define EFTYPE		EINVAL
#endif
#ifndef NODEV
#define NODEV		((dev_t)(-1))
#endif

#include <linux/kdev_t.h>
#ifndef major
#define major(d)	MAJOR((dev_t)(d))
#endif
#ifndef minor
#define minor(d)	MINOR((dev_t)(d))
#endif

#ifndef VNOVAL
#define VNOVAL	(-1)
#endif

#ifndef SA_XLOCKED
#define SA_XLOCKED	0x01
#endif
#ifndef SA_NOTRECURSED
#define SA_NOTRECURSED	0x02
#endif
#define hammer2_mtx_assert(p, flags) \
	WARN_ON(!mutex_is_locked(&(p)->lock))

struct buf {
	int		b_iocmd;
	int		b_error;
	int		b_ioflags;
	int		b_flags;
	long		b_resid;
	long		b_bufsize;
	long		b_bcount;
	off_t		b_offset;
	char		*b_data;
	sector_t	b_lblkno;
	sector_t	b_blkno;
};

struct vop_strategy_args {
	struct inode		*a_vp;
	struct buf		*a_bp;
};

#define BIO_READ	0
#define BIO_WRITE	1
#define BIO_ERROR	0x0001
#define B_CLUSTEROK	0x0100
#define B_INVAL		0x0200
#define B_RELBUF	0x0400
#define B_NOWAIT	0x0800

static inline void bufdone(struct buf *bp) { (void)bp; }
#define __DECONST(t, v)		((t)(uintptr_t)(v))

#ifndef Z_NULL
#define Z_NULL		NULL
#endif
#define inflateInit	zlib_inflateInit
#define inflate		zlib_inflate
#define inflateEnd	zlib_inflateEnd
#define deflateInit	zlib_deflateInit
#define deflate		zlib_deflate
#define deflateEnd	zlib_deflateEnd

#ifndef IO_SYNC
#define IO_SYNC		0x0080
#endif
#ifndef IO_ASYNC
#define IO_ASYNC	0x0200
#endif

#ifndef MAXPHYS
#define MAXPHYS		(1 << 20)
#endif

#ifndef FIOSEEKDATA
#define FIOSEEKDATA	SEEK_DATA
#endif
#ifndef FIOSEEKHOLE
#define FIOSEEKHOLE	SEEK_HOLE
#endif

#include <linux/random.h>
#include <linux/uuid.h>
static inline void
kern_uuidgen(struct uuid *u, int count)
{
	int i;
	for (i = 0; i < count; ++i)
		get_random_bytes(&u[i], sizeof(u[i]));
}

#include <crypto/sha2.h>

#define SHA256_DIGEST_LENGTH	SHA256_DIGEST_SIZE
typedef struct sha256_ctx SHA256_CTX;
static inline void SHA256_Init(SHA256_CTX *c)            { sha256_init(c); }
static inline void SHA256_Update(SHA256_CTX *c, const void *d, size_t n)
                                                         { sha256_update(c, d, n); }
static inline void SHA256_Final(uint8_t *out, SHA256_CTX *c)
                                                         { sha256_final(c, out); }

struct hammer2_zone {
	size_t		elem_size;
	const char	*name;
};
typedef struct hammer2_zone *uma_zone_t;

static inline uma_zone_t
uma_zcreate(const char *name, size_t sz,
	    void *ctor, void *dtor, void *init, void *fini,
	    int align, unsigned int flags)
{
	struct hammer2_zone *z = kzalloc(sizeof(*z), GFP_KERNEL);
	if (z) {
		z->elem_size = sz;
		z->name = name;
	}
	return z;
}

static inline void
uma_zdestroy(uma_zone_t z)
{
	kfree(z);
}

static inline void *
uma_zalloc(uma_zone_t z, unsigned int flags)
{
	gfp_t gfp = (flags & ~M_ZERO) ? (gfp_t)(flags & ~M_ZERO) : GFP_KERNEL;
	if (!z)
		return NULL;
	if (flags & M_ZERO)
		return kzalloc(z->elem_size, gfp);
	return kmalloc(z->elem_size, gfp);
}

static inline void
uma_zfree(uma_zone_t z, void *p)
{
	kfree(p);
}

#define atomic_add_int(p, v) \
	atomic_add((int)(v), (atomic_t *)(p))
#define atomic_clear_int(p, v) \
	atomic_andnot((int)(v), (atomic_t *)(p))
#define atomic_set_int(p, v) \
	atomic_or((int)(v), (atomic_t *)(p))
#define atomic_cmpset_int(p, o, n) \
	(atomic_cmpxchg((atomic_t *)(p), (int)(o), (int)(n)) == (int)(o))
#define atomic_fetchadd_int(p, v) \
	atomic_fetch_add((int)(v), (atomic_t *)(p))

#define atomic_set_32(p, v) \
	atomic_or((int)(v), (atomic_t *)(p))
#define atomic_clear_32(p, v) \
	atomic_andnot((int)(v), (atomic_t *)(p))
#define atomic_fetchadd_32(p, v) \
	atomic_fetch_add((int)(v), (atomic_t *)(p))
#define atomic_cmpset_32(p, o, n) \
	(atomic_cmpxchg((atomic_t *)(p), (int)(o), (int)(n)) == (int)(o))

#define atomic_set_64(p, v) \
	atomic64_or((s64)(v), (atomic64_t *)(p))
#define atomic_clear_64(p, v) \
	atomic64_andnot((s64)(v), (atomic64_t *)(p))
#define atomic_fetchadd_64(p, v) \
	atomic64_fetch_add((s64)(v), (atomic64_t *)(p))

#ifndef KASSERT
#define KASSERT(exp, msg) \
    do { \
        if (unlikely(!(exp))) { \
            printk(KERN_ERR "HAMMER2: assertion failed: " msg "\n"); \
            BUG(); \
        } \
    } while (0)
#endif

#define KASSERTMSG(exp, msg, ...) \
    do { \
        if (unlikely(!(exp))) { \
            printk(KERN_ERR "HAMMER2: assertion failed: " msg "\n", ##__VA_ARGS__); \
            BUG(); \
        } \
    } while (0)

#define KKASSERT(exp) \
    KASSERTMSG(exp, \
        "assertion \"%s\" failed in %s at %s:%d", \
        #exp, __func__, __FILE__, __LINE__)

#define cpu_pause()     cpu_relax()

#define cpu_ccfence()   barrier()

#define getticks()      (jiffies)

#define TICKS_PER_SECOND    HZ
#define ticks_to_seconds(t) ((t) / HZ)
#define seconds_to_ticks(s) ((s) * HZ)

#ifdef HAMMER2_DEBUG
#define DPRINTK(fmt, ...) \
    printk(KERN_DEBUG "HAMMER2: " fmt, ##__VA_ARGS__)
#else
#define DPRINTK(fmt, ...) \
    do { } while (0)
#endif

#define DPRINTK_INFO(fmt, ...) \
    printk(KERN_INFO "HAMMER2: " fmt, ##__VA_ARGS__)

#define DPRINTK_WARN(fmt, ...) \
    printk(KERN_WARNING "HAMMER2: " fmt, ##__VA_ARGS__)

#define DPRINTK_ERR(fmt, ...) \
    printk(KERN_ERR "HAMMER2: " fmt, ##__VA_ARGS__)

#define hammer2_panic(fmt, ...) \
    do { \
        printk(KERN_EMERG "HAMMER2: panic: " fmt, ##__VA_ARGS__); \
        BUG(); \
    } while (0)

#define hammer2_unreachable() \
    do { \
        printk(KERN_ERR "HAMMER2: unreachable code at %s:%d\n", \
               __FILE__, __LINE__); \
        unreachable(); \
    } while (0)

#endif
