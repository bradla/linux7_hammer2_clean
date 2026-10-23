#ifndef _VFS_HAMMER2_HAMMER2_H_
#define _VFS_HAMMER2_HAMMER2_H_

#ifdef _KERNEL
#include <sys/param.h>
#endif
#include <sys/types.h>
#ifdef _KERNEL
#include <sys/kernel.h>
#endif
#include <sys/conf.h>
#ifdef _KERNEL
#include <sys/systm.h>
#endif
#include <sys/diskslice.h>
#include <sys/tree.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/proc.h>
#include <sys/caps.h>
#include <sys/stat.h>
#include <sys/thread.h>
#include <sys/lockf.h>
#include <sys/buf.h>
#include <sys/queue.h>
#include <sys/limits.h>
#include <sys/dmsg.h>
#include <sys/mutex.h>
#include <sys/lock.h>
#include <sys/file.h>
#include <sys/filio.h>
#include <sys/objcache.h>

#ifdef _KERNEL
#include <sys/signal2.h>
#include <sys/buf2.h>
#include <sys/mutex2.h>
#include <sys/spinlock2.h>
#endif

#include "hammer2_xxhash.h"
#include "hammer2_disk.h"
#include "hammer2_mount.h"
#include "hammer2_ioctl.h"

struct hammer2_io;
struct hammer2_chain;
struct hammer2_inode;
struct hammer2_depend;
struct hammer2_dev;
struct hammer2_pfs;
union hammer2_xop;

typedef mtx_t				hammer2_mtx_t;
typedef mtx_state_t			hammer2_mtx_state_t;

typedef struct spinlock			hammer2_spin_t;

#define hammer2_mtx_ex			mtx_lock_ex_quick
#define hammer2_mtx_ex_try		mtx_lock_ex_try
#define hammer2_mtx_sh			mtx_lock_sh_quick
#define hammer2_mtx_sh_again		mtx_lock_sh_again
#define hammer2_mtx_sh_try		mtx_lock_sh_try
#define hammer2_mtx_unlock		mtx_unlock
#define hammer2_mtx_upgrade_try		mtx_upgrade_try
#define hammer2_mtx_downgrade		mtx_downgrade
#define hammer2_mtx_owned		mtx_owned
#define hammer2_mtx_init		mtx_init
#define hammer2_mtx_temp_release	mtx_lock_temp_release
#define hammer2_mtx_temp_restore	mtx_lock_temp_restore
#define hammer2_mtx_refs		mtx_lockrefs

#define hammer2_spin_init		spin_init
#define hammer2_spin_sh			spin_lock_shared
#define hammer2_spin_ex			spin_lock
#define hammer2_spin_unsh		spin_unlock_shared
#define hammer2_spin_unex		spin_unlock
#define hammer2_spin_lock_update	spin_lock_update
#define hammer2_spin_unlock_update	spin_unlock_update

TAILQ_HEAD(hammer2_xop_list, hammer2_xop_head);
TAILQ_HEAD(hammer2_chain_list, hammer2_chain);

typedef struct hammer2_xop_list	hammer2_xop_list_t;

#define HAMMER2_LIMIT_DIRTY_CHAINS	(1024*1024)
#define HAMMER2_LIMIT_DIRTY_INODES	(65536)

#define HAMMER2_IOHASH_SIZE		32768
#define HAMMER2_IOHASH_MASK		(HAMMER2_IOHASH_SIZE - 1)

#define HAMMER2_INUMHASH_SIZE		32768
#define HAMMER2_INUMHASH_MASK		(HAMMER2_IOHASH_SIZE - 1)

RB_HEAD(hammer2_chain_tree, hammer2_chain);

struct hammer2_reptrack {
	hammer2_spin_t	spin;
	struct hammer2_reptrack *next;
	struct hammer2_chain	*chain;
};

struct hammer2_chain_core {
	hammer2_spin_t	spin;
	struct hammer2_reptrack *reptrack;
	struct hammer2_chain_tree rbtree;
	int		live_zero;
	u_int		live_count;
	u_int		chain_count;
	int		generation;
};

typedef struct hammer2_chain_core hammer2_chain_core_t;

#ifdef HAMMER2_IO_DEBUG
#define HAMMER2_IO_DEBUG_ARGS	, const char *file, int line
#define HAMMER2_IO_DEBUG_CALL	, file, line
#define HAMMER2_IO_DEBUG_COUNT	2048
#define HAMMER2_IO_DEBUG_MASK	(HAMMER2_IO_DEBUG_COUNT - 1)
#else
#define HAMMER2_IO_DEBUG_ARGS
#define HAMMER2_IO_DEBUG_CALL
#endif

struct hammer2_io {
	struct hammer2_io  *next;
	struct hammer2_dev *hmp;
	struct vnode	*devvp;
	struct buf	*bp;
	off_t		dbase;
	off_t		pbase;
	uint64_t	refs;
	int		psize;
	int		act;
	int		btype;
	int		ticks;
	int		error;
#ifdef HAMMER2_IO_DEBUG
	int		debug_index;
#else
	int		unused01;
#endif
	uint64_t	dedup_valid;
	uint64_t	dedup_alloc;
#ifdef HAMMER2_IO_DEBUG
	const char	*debug_file[HAMMER2_IO_DEBUG_COUNT];
	void		*debug_td[HAMMER2_IO_DEBUG_COUNT];
	int		debug_line[HAMMER2_IO_DEBUG_COUNT];
	uint64_t	debug_refs[HAMMER2_IO_DEBUG_COUNT];
#endif
};

typedef struct hammer2_io hammer2_io_t;

struct hammer2_io_hash {
	hammer2_spin_t		spin;
	struct hammer2_io	*base;
};

typedef struct hammer2_io_hash	hammer2_io_hash_t;

#define HAMMER2_DIO_INPROG	0x8000000000000000LLU
#define HAMMER2_DIO_GOOD	0x4000000000000000LLU
#define HAMMER2_DIO_WAITING	0x2000000000000000LLU
#define HAMMER2_DIO_DIRTY	0x1000000000000000LLU
#define HAMMER2_DIO_FLUSH	0x0800000000000000LLU

#define HAMMER2_DIO_MASK	0x00FFFFFFFFFFFFFFLLU

struct hammer2_inum_hash {
	hammer2_spin_t		spin;
	struct hammer2_inode	*base;
};

typedef struct hammer2_inum_hash hammer2_inum_hash_t;

struct hammer2_chain {
	hammer2_mtx_t		lock;
	hammer2_chain_core_t	core;
	RB_ENTRY(hammer2_chain) rbnode;
	hammer2_blockref_t	bref;
	struct hammer2_chain	*parent;
	struct hammer2_dev	*hmp;
	struct hammer2_pfs	*pmp;

	struct lock	diolk;
	hammer2_io_t	*dio;
	hammer2_media_data_t *data;
	u_int		bytes;
	u_int		flags;
	u_int		refs;
	u_int		lockcnt;
	int		error;
	int		cache_index;
};

typedef struct hammer2_chain hammer2_chain_t;

int hammer2_chain_cmp(hammer2_chain_t *chain1, hammer2_chain_t *chain2);
RB_PROTOTYPE(hammer2_chain_tree, hammer2_chain, rbnode, hammer2_chain_cmp);

#define HAMMER2_METH_DEFAULT		-1

#define HAMMER2_CHAIN_MODIFIED		0x00000001
#define HAMMER2_CHAIN_ALLOCATED		0x00000002
#define HAMMER2_CHAIN_DESTROY		0x00000004
#define HAMMER2_CHAIN_DEDUPABLE		0x00000008
#define HAMMER2_CHAIN_DELETED		0x00000010
#define HAMMER2_CHAIN_INITIAL		0x00000020
#define HAMMER2_CHAIN_UPDATE		0x00000040
#define HAMMER2_CHAIN_NOTTESTED		0x00000080
#define HAMMER2_CHAIN_TESTEDGOOD	0x00000100
#define HAMMER2_CHAIN_ONFLUSH		0x00000200
#define HAMMER2_CHAIN_UNUSED0400	0x00000400
#define HAMMER2_CHAIN_VOLUMESYNC	0x00000800
#define HAMMER2_CHAIN_UNUSED1000	0x00001000
#define HAMMER2_CHAIN_COUNTEDBREFS	0x00002000
#define HAMMER2_CHAIN_ONRBTREE		0x00004000
#define HAMMER2_CHAIN_UNUSED8000	0x00008000
#define HAMMER2_CHAIN_UNUSED10000	0x00010000
#define HAMMER2_CHAIN_RELEASE		0x00020000
#define HAMMER2_CHAIN_BLKMAPPED		0x00040000
#define HAMMER2_CHAIN_BLKMAPUPD		0x00080000
#define HAMMER2_CHAIN_IOINPROG		0x00100000
#define HAMMER2_CHAIN_IOSIGNAL		0x00200000
#define HAMMER2_CHAIN_PFSBOUNDARY	0x00400000
#define HAMMER2_CHAIN_HINT_LEAF_COUNT	0x00800000
#define HAMMER2_CHAIN_UNUSED1000000	0x01000000

#define HAMMER2_CHAIN_FLUSH_MASK	(HAMMER2_CHAIN_MODIFIED |	\
					 HAMMER2_CHAIN_UPDATE |		\
					 HAMMER2_CHAIN_ONFLUSH |	\
					 HAMMER2_CHAIN_DESTROY)

#define HAMMER2_ERROR_NONE		0
#define HAMMER2_ERROR_EIO		0x00000001
#define HAMMER2_ERROR_CHECK		0x00000002
#define HAMMER2_ERROR_INCOMPLETE	0x00000004
#define HAMMER2_ERROR_DEPTH		0x00000008
#define HAMMER2_ERROR_BADBREF		0x00000010
#define HAMMER2_ERROR_ENOSPC		0x00000020
#define HAMMER2_ERROR_ENOENT		0x00000040
#define HAMMER2_ERROR_ENOTEMPTY		0x00000080
#define HAMMER2_ERROR_EAGAIN		0x00000100
#define HAMMER2_ERROR_ENOTDIR		0x00000200
#define HAMMER2_ERROR_EISDIR		0x00000400
#define HAMMER2_ERROR_EINPROGRESS	0x00000800
#define HAMMER2_ERROR_ABORTED		0x00001000
#define HAMMER2_ERROR_EOF		0x00002000
#define HAMMER2_ERROR_EINVAL		0x00004000
#define HAMMER2_ERROR_EEXIST		0x00008000
#define HAMMER2_ERROR_EDEADLK		0x00010000
#define HAMMER2_ERROR_ESRCH		0x00020000
#define HAMMER2_ERROR_ETIMEDOUT		0x00040000

#define HAMMER2_LOOKUP_UNUSED0001	0x00000001
#define HAMMER2_LOOKUP_NODATA		0x00000002
#define HAMMER2_LOOKUP_NODIRECT		0x00000004
#define HAMMER2_LOOKUP_SHARED		0x00000100
#define HAMMER2_LOOKUP_MATCHIND		0x00000200
#define HAMMER2_LOOKUP_UNUSED0400	0x00000400
#define HAMMER2_LOOKUP_ALWAYS		0x00000800
#define HAMMER2_LOOKUP_UNUSED1000	0x00001000

#define HAMMER2_MODIFY_OPTDATA		0x00000002

#define HAMMER2_RESOLVE_NEVER		1
#define HAMMER2_RESOLVE_MAYBE		2
#define HAMMER2_RESOLVE_ALWAYS		3
#define HAMMER2_RESOLVE_MASK		0x0F

#define HAMMER2_RESOLVE_SHARED		0x10
#define HAMMER2_RESOLVE_LOCKAGAIN	0x20
#define HAMMER2_RESOLVE_UNUSED40	0x40
#define HAMMER2_RESOLVE_NONBLOCK	0x80

#define HAMMER2_DELETE_PERMANENT	0x0001

#define HAMMER2_INSERT_PFSROOT		0x0004
#define HAMMER2_INSERT_SAMEPARENT	0x0008

#define HAMMER2_FREEMAP_DORECOVER	1
#if 0
#define HAMMER2_FREEMAP_DOMAYFREE	2
#define HAMMER2_FREEMAP_DOREALFREE	3
#endif

#define HAMMER2_XOPFIFO		16
#define HAMMER2_XOPFIFO_MASK	(HAMMER2_XOPFIFO - 1)
#define HAMMER2_XOPTHREADS_MIN	32
#define HAMMER2_XOPGROUPS_MIN	4

#define HAMMER2_MAXCLUSTER	8
#define HAMMER2_XOPMASK_CLUSTER	((uint64_t)((1LLU << HAMMER2_MAXCLUSTER) - 1))
#define HAMMER2_XOPMASK_VOP	((uint64_t)0x0000000080000000LLU)
#define HAMMER2_XOPMASK_FIFOW	((uint64_t)0x0000000040000000LLU)
#define HAMMER2_XOPMASK_WAIT	((uint64_t)0x0000000020000000LLU)
#define HAMMER2_XOPMASK_FEED	((uint64_t)0x0000000100000000LLU)

#define HAMMER2_XOPMASK_ALLDONE	(HAMMER2_XOPMASK_VOP | HAMMER2_XOPMASK_CLUSTER)

struct hammer2_cluster_item {
	hammer2_chain_t		*chain;
	int			error;
	uint32_t		flags;
};

typedef struct hammer2_cluster_item hammer2_cluster_item_t;

#define HAMMER2_CITEM_INVALID	0x00000001
#define HAMMER2_CITEM_FEMOD	0x00000002
#define HAMMER2_CITEM_NULL	0x00000004

struct hammer2_cluster {
	int			refs;
	int			ddflag;
	struct hammer2_pfs	*pmp;
	uint32_t		flags;
	int			nchains;
	int			error;
	int			focus_index;
	hammer2_chain_t		*focus;
	hammer2_cluster_item_t	array[HAMMER2_MAXCLUSTER];
};

typedef struct hammer2_cluster	hammer2_cluster_t;

#define HAMMER2_CLUSTER_INODE	0x00000001
#define HAMMER2_CLUSTER_UNUSED2	0x00000002
#define HAMMER2_CLUSTER_LOCKED	0x00000004
#define HAMMER2_CLUSTER_WRHARD	0x00000100
#define HAMMER2_CLUSTER_RDHARD	0x00000200
#define HAMMER2_CLUSTER_UNHARD	0x00000400
#define HAMMER2_CLUSTER_NOHARD	0x00000800
#define HAMMER2_CLUSTER_WRSOFT	0x00001000
#define HAMMER2_CLUSTER_RDSOFT	0x00002000
#define HAMMER2_CLUSTER_UNSOFT	0x00004000
#define HAMMER2_CLUSTER_NOSOFT	0x00008000
#define HAMMER2_CLUSTER_MSYNCED	0x00010000
#define HAMMER2_CLUSTER_SSYNCED	0x00020000

#define HAMMER2_CLUSTER_ANYDATA	( HAMMER2_CLUSTER_RDHARD |	\
				  HAMMER2_CLUSTER_RDSOFT |	\
				  HAMMER2_CLUSTER_RDSLAVE)
#if 0
#define HAMMER2_CLUSTER_RDOK	( HAMMER2_CLUSTER_RDHARD |	\
				  HAMMER2_CLUSTER_RDSOFT)

#define HAMMER2_CLUSTER_WROK	( HAMMER2_CLUSTER_WRHARD |	\
				  HAMMER2_CLUSTER_WRSOFT)
#endif
#define HAMMER2_CLUSTER_ZFLAGS	( HAMMER2_CLUSTER_WRHARD |	\
				  HAMMER2_CLUSTER_RDHARD |	\
				  HAMMER2_CLUSTER_WRSOFT |	\
				  HAMMER2_CLUSTER_RDSOFT |	\
				  HAMMER2_CLUSTER_MSYNCED |	\
				  HAMMER2_CLUSTER_SSYNCED)

TAILQ_HEAD(inoq_head, hammer2_inode);
TAILQ_HEAD(depq_head, hammer2_depend);

struct hammer2_depend {
	TAILQ_ENTRY(hammer2_depend) entry;
	struct inoq_head	sideq;
	long			count;
	int			pass2;
	int			unused01;
};

typedef struct hammer2_depend hammer2_depend_t;

struct hammer2_inode {
	struct hammer2_inode	*next;
	TAILQ_ENTRY(hammer2_inode) entry;
	hammer2_depend_t	*depend;
	hammer2_depend_t	depend_static;
	hammer2_mtx_t		lock;
	hammer2_mtx_t		truncate_lock;
	struct hammer2_pfs	*pmp;
	struct vnode		*vp;
	hammer2_spin_t		cluster_spin;
	hammer2_cluster_t	cluster;
	hammer2_cluster_item_t	ccache[HAMMER2_MAXCLUSTER];
	int			ccache_nchains;
	u_int			flags;
	u_int			refs;
	int			ihash;
	uint8_t			comp_heuristic;
	hammer2_inode_meta_t	meta;
	hammer2_off_t		osize;
};

typedef struct hammer2_inode hammer2_inode_t;

#define HAMMER2_INODE_MODIFIED		0x0001
#define HAMMER2_INODE_UNUSED0002	0x0002
#define HAMMER2_INODE_UNUSED0004	0x0004
#define HAMMER2_INODE_ONHASH		0x0008
#define HAMMER2_INODE_RESIZED		0x0010
#define HAMMER2_INODE_UNUSED0020	0x0020
#define HAMMER2_INODE_ISUNLINKED	0x0040
#define HAMMER2_INODE_UNUSED0080	0x0080
#define HAMMER2_INODE_SIDEQ		0x0100
#define HAMMER2_INODE_NOSIDEQ		0x0200
#define HAMMER2_INODE_DIRTYDATA		0x0400
#define HAMMER2_INODE_SYNCQ		0x0800
#define HAMMER2_INODE_DELETING		0x1000
#define HAMMER2_INODE_CREATING		0x2000
#define HAMMER2_INODE_SYNCQ_WAKEUP	0x4000
#define HAMMER2_INODE_SYNCQ_PASS2	0x8000

#define HAMMER2_INODE_DIRTY		(HAMMER2_INODE_MODIFIED |	\
					 HAMMER2_INODE_DIRTYDATA |	\
					 HAMMER2_INODE_DELETING |	\
					 HAMMER2_INODE_CREATING)

struct hammer2_trans {
	uint32_t		flags;
	uint32_t		sync_wait;
};

typedef struct hammer2_trans hammer2_trans_t;

#define HAMMER2_TRANS_ISFLUSH		0x80000000
#define HAMMER2_TRANS_BUFCACHE		0x40000000
#define HAMMER2_TRANS_SIDEQ		0x20000000
#define HAMMER2_TRANS_UNUSED10		0x10000000
#define HAMMER2_TRANS_WAITING		0x08000000
#define HAMMER2_TRANS_RESCAN		0x04000000
#define HAMMER2_TRANS_MASK		0x00FFFFFF

#define HAMMER2_FREEMAP_HEUR_NRADIX	4
#define HAMMER2_FREEMAP_HEUR_TYPES	8
#define HAMMER2_FREEMAP_HEUR_SIZE	(HAMMER2_FREEMAP_HEUR_NRADIX * \
					 HAMMER2_FREEMAP_HEUR_TYPES)

#define HAMMER2_DEDUP_HEUR_SIZE		(65536 * 4)
#define HAMMER2_DEDUP_HEUR_MASK		(HAMMER2_DEDUP_HEUR_SIZE - 1)

#define HAMMER2_FLUSH_TOP		0x0001
#define HAMMER2_FLUSH_ALL		0x0002
#define HAMMER2_FLUSH_INODE_STOP	0x0004
#define HAMMER2_FLUSH_FSSYNC		0x0008

struct hammer2_thread {
	struct hammer2_pfs *pmp;
	struct hammer2_dev *hmp;
	hammer2_xop_list_t xopq;
	thread_t	td;
	uint32_t	flags;
	int		clindex;
	int		repidx;
	char		*scratch;
};

typedef struct hammer2_thread hammer2_thread_t;

#define HAMMER2_THREAD_UNMOUNTING	0x0001
#define HAMMER2_THREAD_DEV		0x0002
#define HAMMER2_THREAD_WAITING		0x0004
#define HAMMER2_THREAD_REMASTER		0x0008
#define HAMMER2_THREAD_STOP		0x0010
#define HAMMER2_THREAD_FREEZE		0x0020
#define HAMMER2_THREAD_FROZEN		0x0040
#define HAMMER2_THREAD_XOPQ		0x0080
#define HAMMER2_THREAD_STOPPED		0x0100
#define HAMMER2_THREAD_UNFREEZE		0x0200

#define HAMMER2_THREAD_WAKEUP_MASK	(HAMMER2_THREAD_UNMOUNTING |	\
					 HAMMER2_THREAD_REMASTER |	\
					 HAMMER2_THREAD_STOP |		\
					 HAMMER2_THREAD_FREEZE |	\
					 HAMMER2_THREAD_XOPQ)

struct hammer2_dedup {
	hammer2_off_t	data_off;
	uint64_t	data_crc;
	uint32_t	ticks;
	uint32_t	saved_error;
};

typedef struct hammer2_dedup hammer2_dedup_t;

typedef void (*hammer2_xop_func_t)(union hammer2_xop *xop, void *scratch,
				   int clindex);

struct hammer2_xop_desc {
	hammer2_xop_func_t	storage_func;
	hammer2_xop_func_t	dmsg_dispatch;
	hammer2_xop_func_t	dmsg_process;
	const char		*id;
};

typedef struct hammer2_xop_desc hammer2_xop_desc_t;

struct hammer2_xop_fifo {
	TAILQ_ENTRY(hammer2_xop_head) entry;
	hammer2_chain_t		*array[HAMMER2_XOPFIFO];
	int			errors[HAMMER2_XOPFIFO];
	int			ri;
	int			wi;
	int			flags;
	hammer2_thread_t	*thr;
};

typedef struct hammer2_xop_fifo hammer2_xop_fifo_t;

#define HAMMER2_XOP_FIFO_RUN	0x0001
#define HAMMER2_XOP_FIFO_STALL	0x0002

struct hammer2_xop_head {
	hammer2_xop_desc_t	*desc;
	hammer2_tid_t		mtid;
	struct hammer2_inode	*ip1;
	struct hammer2_inode	*ip2;
	struct hammer2_inode	*ip3;
	struct hammer2_inode	*ip4;
	uint64_t		run_mask;
	uint64_t		chk_mask;
	int			flags;
	int			state;
	int			error;
	hammer2_key_t		collect_key;
	char			*name1;
	size_t			name1_len;
	char			*name2;
	size_t			name2_len;
	hammer2_xop_fifo_t	collect[HAMMER2_MAXCLUSTER];
	hammer2_cluster_t	cluster;
	hammer2_io_t		*focus_dio;
};

typedef struct hammer2_xop_head hammer2_xop_head_t;

struct hammer2_xop_ipcluster {
	hammer2_xop_head_t	head;
};

struct hammer2_xop_strategy {
	hammer2_xop_head_t	head;
	hammer2_key_t		lbase;
	int			finished;
	hammer2_mtx_t		lock;
	struct bio		*bio;
};

struct hammer2_xop_readdir {
	hammer2_xop_head_t	head;
	hammer2_key_t		lkey;
};

struct hammer2_xop_nresolve {
	hammer2_xop_head_t	head;
	hammer2_key_t		lhc;
};

struct hammer2_xop_unlink {
	hammer2_xop_head_t	head;
	int			isdir;
	int			dopermanent;
};

#define H2DOPERM_PERMANENT	0x01
#define H2DOPERM_FORCE		0x02
#define H2DOPERM_IGNINO		0x04

struct hammer2_xop_nrename {
	hammer2_xop_head_t	head;
	hammer2_tid_t		lhc;
	int			ip_key;
};

struct hammer2_xop_scanlhc {
	hammer2_xop_head_t	head;
	hammer2_key_t		lhc;
};

struct hammer2_xop_scanall {
	hammer2_xop_head_t	head;
	hammer2_key_t		key_beg;
	hammer2_key_t		key_end;
	int			resolve_flags;
	int			lookup_flags;
};

struct hammer2_xop_lookup {
	hammer2_xop_head_t	head;
	hammer2_key_t		lhc;
};

struct hammer2_xop_mkdirent {
	hammer2_xop_head_t	head;
	hammer2_dirent_head_t	dirent;
	hammer2_key_t		lhc;
};

struct hammer2_xop_create {
	hammer2_xop_head_t	head;
	hammer2_inode_meta_t	meta;
	hammer2_key_t		lhc;
	int			flags;
};

struct hammer2_xop_destroy {
	hammer2_xop_head_t	head;
};

struct hammer2_xop_fsync {
	hammer2_xop_head_t	head;
	hammer2_inode_meta_t	meta;
	hammer2_off_t		osize;
	u_int			ipflags;
	int			clear_directdata;
};

struct hammer2_xop_unlinkall {
	hammer2_xop_head_t	head;
	hammer2_key_t		key_beg;
	hammer2_key_t		key_end;
};

struct hammer2_xop_connect {
	hammer2_xop_head_t	head;
	hammer2_key_t		lhc;
};

struct hammer2_xop_flush {
	hammer2_xop_head_t	head;
};

struct hammer2_xop_bmap {
	hammer2_xop_head_t	head;
	hammer2_off_t		loffset;
	hammer2_off_t		offset;
};

typedef struct hammer2_xop_readdir hammer2_xop_readdir_t;
typedef struct hammer2_xop_nresolve hammer2_xop_nresolve_t;
typedef struct hammer2_xop_unlink hammer2_xop_unlink_t;
typedef struct hammer2_xop_nrename hammer2_xop_nrename_t;
typedef struct hammer2_xop_ipcluster hammer2_xop_ipcluster_t;
typedef struct hammer2_xop_strategy hammer2_xop_strategy_t;
typedef struct hammer2_xop_mkdirent hammer2_xop_mkdirent_t;
typedef struct hammer2_xop_create hammer2_xop_create_t;
typedef struct hammer2_xop_destroy hammer2_xop_destroy_t;
typedef struct hammer2_xop_fsync hammer2_xop_fsync_t;
typedef struct hammer2_xop_unlinkall hammer2_xop_unlinkall_t;
typedef struct hammer2_xop_scanlhc hammer2_xop_scanlhc_t;
typedef struct hammer2_xop_scanall hammer2_xop_scanall_t;
typedef struct hammer2_xop_lookup hammer2_xop_lookup_t;
typedef struct hammer2_xop_connect hammer2_xop_connect_t;
typedef struct hammer2_xop_flush hammer2_xop_flush_t;
typedef struct hammer2_xop_bmap hammer2_xop_bmap_t;

union hammer2_xop {
	hammer2_xop_head_t	head;
	hammer2_xop_ipcluster_t	xop_ipcluster;
	hammer2_xop_readdir_t	xop_readdir;
	hammer2_xop_nresolve_t	xop_nresolve;
	hammer2_xop_unlink_t	xop_unlink;
	hammer2_xop_nrename_t	xop_nrename;
	hammer2_xop_strategy_t	xop_strategy;
	hammer2_xop_mkdirent_t	xop_mkdirent;
	hammer2_xop_create_t	xop_create;
	hammer2_xop_destroy_t	xop_destroy;
	hammer2_xop_fsync_t	xop_fsync;
	hammer2_xop_unlinkall_t	xop_unlinkall;
	hammer2_xop_scanlhc_t	xop_scanlhc;
	hammer2_xop_scanall_t	xop_scanall;
	hammer2_xop_lookup_t	xop_lookup;
	hammer2_xop_flush_t	xop_flush;
	hammer2_xop_connect_t	xop_connect;
	hammer2_xop_bmap_t	xop_bmap;
};

typedef union hammer2_xop hammer2_xop_t;

struct hammer2_xop_group {
	hammer2_thread_t	thrs[HAMMER2_MAXCLUSTER];
};

typedef struct hammer2_xop_group hammer2_xop_group_t;

#define HAMMER2_XOP_COLLECT_NOWAIT	0x00000001
#define HAMMER2_XOP_COLLECT_WAITALL	0x00000002

#define HAMMER2_XOP_MODIFYING		0x00000001
#define HAMMER2_XOP_STRATEGY		0x00000002
#define HAMMER2_XOP_INODE_STOP		0x00000004
#define HAMMER2_XOP_VOLHDR		0x00000008
#define HAMMER2_XOP_FSSYNC		0x00000010

struct hammer2_devvp {
	TAILQ_ENTRY(hammer2_devvp) entry;
	struct vnode	*devvp;
	char		*path;
	int		open;
};

typedef struct hammer2_devvp hammer2_devvp_t;

TAILQ_HEAD(hammer2_devvp_list, hammer2_devvp);

typedef struct hammer2_devvp_list hammer2_devvp_list_t;

struct hammer2_volume {
	hammer2_devvp_t *dev;
	int		id;
	hammer2_off_t	offset;
	hammer2_off_t	size;
};

typedef struct hammer2_volume hammer2_volume_t;

struct hammer2_dev {
	struct vnode	*devvp;
	int		ronly;
	int		mount_count;
	TAILQ_ENTRY(hammer2_dev) mntentry;

	struct malloc_type *mchain_obj;
	struct malloc_type *mio_obj;
	struct malloc_type *mmsg;
	kdmsg_iocom_t	iocom;
	hammer2_io_hash_t iohash[HAMMER2_IOHASH_SIZE];
	int		iofree_count;
	int		io_iterator;
	int		freemap_relaxed;
	int		unused01;
	hammer2_chain_t vchain;
	hammer2_chain_t fchain;
	hammer2_spin_t	list_spin;
	struct hammer2_pfs *spmp;
	struct lock	vollk;
	struct lock	bulklk;
	struct lock	bflock;
	hammer2_off_t	heur_freemap[HAMMER2_FREEMAP_HEUR_SIZE];
	hammer2_dedup_t heur_dedup[HAMMER2_DEDUP_HEUR_SIZE];
	int		volhdrno;
	uint32_t	hflags;
	hammer2_off_t	free_reserved;
	hammer2_off_t	total_size;
	int		nvolumes;
	hammer2_thread_t bfthr;
	char		devrepname[64];
	hammer2_volume_data_t voldata;
	hammer2_volume_data_t volsync;

	hammer2_devvp_list_t devvpl;
	hammer2_volume_t volumes[HAMMER2_MAX_VOLUMES];
};

typedef struct hammer2_dev hammer2_dev_t;

struct hammer2_pfs {
	struct mount		*mp;
	TAILQ_ENTRY(hammer2_pfs) mntentry;
	uuid_t			pfs_clid;
	hammer2_dev_t		*spmp_hmp;
	hammer2_dev_t		*force_local;
	hammer2_inode_t		*iroot;
	uint8_t			pfs_types[HAMMER2_MAXCLUSTER];
	char			*pfs_names[HAMMER2_MAXCLUSTER];
	hammer2_dev_t		*pfs_hmps[HAMMER2_MAXCLUSTER];
	hammer2_blockset_t	pfs_iroot_blocksets[HAMMER2_MAXCLUSTER];
	hammer2_spin_t          blockset_spin;
	hammer2_trans_t		trans;
	struct lock		lock;
	struct netexport	export;
	int			unused00;
	int			ronly;
	int			hflags;
	struct malloc_type	*minode_obj;
	hammer2_inum_hash_t inumhash[HAMMER2_INUMHASH_SIZE];
	long			inum_count;
	int			flags;
	hammer2_tid_t		modify_tid;
	hammer2_tid_t		inode_tid;
	uint8_t			pfs_nmasters;
	uint8_t			pfs_mode;
	uint8_t			unused01;
	uint8_t			unused02;
	int			free_ticks;
	long			inmem_inodes;
	hammer2_off_t		free_reserved;
	hammer2_off_t		free_nominal;
	uint32_t		inmem_dirty_chains;
	int			count_lwinprog;
	hammer2_spin_t		list_spin;
	struct inoq_head	syncq;
	struct depq_head	depq;
	long			sideq_count;
	hammer2_thread_t	sync_thrs[HAMMER2_MAXCLUSTER];
	uint32_t		cluster_flags;
	int			has_xop_threads;
	hammer2_spin_t		xop_spin;
	hammer2_xop_group_t	*xop_groups;
};

typedef struct hammer2_pfs hammer2_pfs_t;

TAILQ_HEAD(hammer2_pfslist, hammer2_pfs);

#define HAMMER2_PMPF_SPMP	0x00000001
#define HAMMER2_PMPF_EMERG	0x00000002

#define HAMMER2_DIRTYCHAIN_WAITING	0x80000000
#define HAMMER2_DIRTYCHAIN_MASK		0x7FFFFFFF

#define HAMMER2_LWINPROG_WAITING	0x80000000
#define HAMMER2_LWINPROG_WAITING0	0x40000000
#define HAMMER2_LWINPROG_MASK		0x3FFFFFFF

#define HAMMER2_CHECK_NULL	0x00000001

#if defined(_KERNEL) || defined(_KERNEL_STRUCTURES)
#define VTOI(vp)	((hammer2_inode_t *)(vp)->v_data)
#endif

#if defined(_KERNEL)

#ifdef MALLOC_DECLARE
MALLOC_DECLARE(M_HAMMER2);
#endif

static __inline
hammer2_pfs_t *
MPTOPMP(struct mount *mp)
{
	return ((hammer2_pfs_t *)mp->mnt_data);
}

#define HAMMER2_DEDUP_FRAG      (HAMMER2_PBUFSIZE / 64)
#define HAMMER2_DEDUP_FRAGRADIX (HAMMER2_PBUFRADIX - 6)

static __inline
uint64_t
hammer2_dedup_mask(hammer2_io_t *dio, hammer2_off_t data_off, u_int bytes)
{
	int bbeg;
	int bits;
	uint64_t mask;

	bbeg = (int)((data_off & ~HAMMER2_OFF_MASK_RADIX) - dio->pbase) >>
	       HAMMER2_DEDUP_FRAGRADIX;
	bits = (int)((bytes + (HAMMER2_DEDUP_FRAG - 1)) >>
	       HAMMER2_DEDUP_FRAGRADIX);
	if (bbeg + bits == 64)
		mask = (uint64_t)-1;
	else
		mask = ((uint64_t)1 << (bbeg + bits)) - 1;

	mask &= ~(((uint64_t)1 << bbeg) - 1);

	return mask;
}

static __inline
int
hammer2_error_to_errno(int error)
{
	if (error) {
		if (error & HAMMER2_ERROR_EIO)
			error = EIO;
		else if (error & HAMMER2_ERROR_CHECK)
			error = EDOM;
		else if (error & HAMMER2_ERROR_ABORTED)
			error = EINTR;
		else if (error & HAMMER2_ERROR_BADBREF)
			error = EIO;
		else if (error & HAMMER2_ERROR_ENOSPC)
			error = ENOSPC;
		else if (error & HAMMER2_ERROR_ENOENT)
			error = ENOENT;
		else if (error & HAMMER2_ERROR_ENOTEMPTY)
			error = ENOTEMPTY;
		else if (error & HAMMER2_ERROR_EAGAIN)
			error = EAGAIN;
		else if (error & HAMMER2_ERROR_ENOTDIR)
			error = ENOTDIR;
		else if (error & HAMMER2_ERROR_EISDIR)
			error = EISDIR;
		else if (error & HAMMER2_ERROR_EINPROGRESS)
			error = EINPROGRESS;
		else if (error & HAMMER2_ERROR_EEXIST)
			error = EEXIST;
		else if (error & HAMMER2_ERROR_EINVAL)
			error = EINVAL;
		else if (error & HAMMER2_ERROR_EDEADLK)
			error = EDEADLK;
		else if (error & HAMMER2_ERROR_ESRCH)
			error = ESRCH;
		else if (error & HAMMER2_ERROR_ETIMEDOUT)
			error = ETIMEDOUT;
		else
			error = EDOM;
	}
	return error;
}

static __inline
int
hammer2_errno_to_error(int error)
{
	switch(error) {
	case 0:
		return 0;
	case EIO:
		return HAMMER2_ERROR_EIO;
	case EDOM:
		return HAMMER2_ERROR_CHECK;
	case EINTR:
		return HAMMER2_ERROR_ABORTED;
	case ENOSPC:
		return HAMMER2_ERROR_ENOSPC;
	case ENOENT:
		return HAMMER2_ERROR_ENOENT;
	case ENOTEMPTY:
		return HAMMER2_ERROR_ENOTEMPTY;
	case EAGAIN:
		return HAMMER2_ERROR_EAGAIN;
	case ENOTDIR:
		return HAMMER2_ERROR_ENOTDIR;
	case EISDIR:
		return HAMMER2_ERROR_EISDIR;
	case EINPROGRESS:
		return HAMMER2_ERROR_EINPROGRESS;
	case EEXIST:
		return HAMMER2_ERROR_EEXIST;
	case EINVAL:
		return HAMMER2_ERROR_EINVAL;
	case EDEADLK:
		return HAMMER2_ERROR_EDEADLK;
	case ESRCH:
		return HAMMER2_ERROR_ESRCH;
	case ETIMEDOUT:
		return HAMMER2_ERROR_ETIMEDOUT;
	default:
		return HAMMER2_ERROR_EINVAL;
	}
}

extern struct vop_ops hammer2_vnode_vops;
extern struct vop_ops hammer2_spec_vops;
extern struct vop_ops hammer2_fifo_vops;
extern struct hammer2_pfslist hammer2_pfslist;
extern struct lock hammer2_mntlk;

extern int hammer2_aux_flags;
extern int hammer2_debug;
extern int hammer2_xop_nthreads;
extern int hammer2_xop_sgroups;
extern int hammer2_xop_xgroups;
extern int hammer2_xop_xbase;
extern int hammer2_xop_mod;
extern long hammer2_debug_inode;
extern int hammer2_cluster_meta_read;
extern int hammer2_cluster_data_read;
extern int hammer2_cluster_write;
extern int hammer2_dedup_enable;
extern int hammer2_always_compress;
extern int hammer2_flush_pipe;
extern int hammer2_dio_count;
extern int hammer2_dio_limit;
extern int hammer2_bulkfree_tps;
extern int hammer2_spread_workers;
extern int hammer2_limit_saved_depth;
extern long hammer2_chain_allocs;
extern long hammer2_limit_saved_chains;
extern long hammer2_limit_dirty_chains;
extern long hammer2_limit_dirty_inodes;
extern long hammer2_count_modified_chains;
extern long hammer2_iod_file_read;
extern long hammer2_iod_meta_read;
extern long hammer2_iod_indr_read;
extern long hammer2_iod_fmap_read;
extern long hammer2_iod_volu_read;
extern long hammer2_iod_file_write;
extern long hammer2_iod_file_wembed;
extern long hammer2_iod_file_wzero;
extern long hammer2_iod_file_wdedup;
extern long hammer2_iod_meta_write;
extern long hammer2_iod_indr_write;
extern long hammer2_iod_fmap_write;
extern long hammer2_iod_volu_write;

extern long hammer2_process_icrc32;
extern long hammer2_process_xxhash64;

extern struct objcache *cache_buffer_read;
extern struct objcache *cache_buffer_write;
extern struct objcache *cache_xops;

#define hammer2_icrc32(buf, size)	iscsi_crc32((buf), (size))
#define hammer2_icrc32c(buf, size, crc)	iscsi_crc32_ext((buf), (size), (crc))

int hammer2_signal_check(time_t *timep);
const char *hammer2_error_str(int error);
const char *hammer2_bref_type_str(int btype);

int hammer2_get_dtype(uint8_t type);
int hammer2_get_vtype(uint8_t type);
uint8_t hammer2_get_obj_type(enum vtype vtype);
void hammer2_time_to_timespec(uint64_t xtime, struct timespec *ts);
uint64_t hammer2_timespec_to_time(const struct timespec *ts);
uint32_t hammer2_to_unix_xid(const uuid_t *uuid);
void hammer2_guid_to_uuid(uuid_t *uuid, uint32_t guid);

hammer2_key_t hammer2_dirhash(const char *aname, size_t len);
int hammer2_getradix(size_t bytes);

int hammer2_calc_logical(hammer2_inode_t *ip, hammer2_off_t uoff,
			hammer2_key_t *lbasep, hammer2_key_t *leofp);
int hammer2_calc_physical(hammer2_inode_t *ip, hammer2_key_t lbase);
void hammer2_update_time(uint64_t *timep);
void hammer2_adjreadcounter(int btype, size_t bytes);
void hammer2_adjwritecounter(int btype, size_t bytes);

void hammer2_inum_hash_init(hammer2_pfs_t *pmp);
struct vnode *hammer2_igetv(hammer2_inode_t *ip, int *errorp);
hammer2_inode_t *hammer2_inode_lookup(hammer2_pfs_t *pmp,
			hammer2_tid_t inum);
hammer2_inode_t *hammer2_inode_get(hammer2_pfs_t *pmp,
			hammer2_xop_head_t *xop, hammer2_tid_t inum, int idx);
void hammer2_inode_ref(hammer2_inode_t *ip);
void hammer2_inode_drop(hammer2_inode_t *ip);
void hammer2_inode_repoint(hammer2_inode_t *ip, hammer2_cluster_t *cluster);
void hammer2_inode_repoint_one(hammer2_inode_t *ip, hammer2_cluster_t *cluster,
			int idx);
hammer2_key_t hammer2_inode_data_count(const hammer2_inode_t *ip);
hammer2_key_t hammer2_inode_inode_count(const hammer2_inode_t *ip);
void hammer2_inode_modify(hammer2_inode_t *ip);
void hammer2_inode_delayed_sideq(hammer2_inode_t *ip);
void hammer2_inode_lock(hammer2_inode_t *ip, int how);
void hammer2_inode_lock4(hammer2_inode_t *ip1, hammer2_inode_t *ip2,
			hammer2_inode_t *ip3, hammer2_inode_t *ip4);
void hammer2_inode_unlock(hammer2_inode_t *ip);
void hammer2_inode_depend(hammer2_inode_t *ip1, hammer2_inode_t *ip2);
hammer2_chain_t *hammer2_inode_chain(hammer2_inode_t *ip, int clindex, int how);
hammer2_chain_t *hammer2_inode_chain_and_parent(hammer2_inode_t *ip,
			int clindex, hammer2_chain_t **parentp, int how);
hammer2_mtx_state_t hammer2_inode_lock_temp_release(hammer2_inode_t *ip);
void hammer2_inode_lock_temp_restore(hammer2_inode_t *ip,
			hammer2_mtx_state_t ostate);
int hammer2_inode_lock_upgrade(hammer2_inode_t *ip);
void hammer2_inode_lock_downgrade(hammer2_inode_t *ip, int);

hammer2_inode_t *hammer2_inode_create_normal(hammer2_inode_t *pip,
			struct vattr *vap, struct ucred *cred,
			hammer2_key_t inum, int *errorp);
hammer2_inode_t *hammer2_inode_create_pfs(hammer2_pfs_t *spmp,
			const char *name, size_t name_len,
			int *errorp);
int hammer2_inode_chain_ins(hammer2_inode_t *ip);
int hammer2_inode_chain_des(hammer2_inode_t *ip);
int hammer2_inode_chain_sync(hammer2_inode_t *ip);
int hammer2_inode_chain_flush(hammer2_inode_t *ip, int flags);
int hammer2_inode_unlink_finisher(hammer2_inode_t *ip, struct vnode **vpp);
void hammer2_inode_vprecycle(struct vnode *vp);
int hammer2_dirent_create(hammer2_inode_t *dip, const char *name,
			size_t name_len, hammer2_key_t inum, uint8_t type);

hammer2_chain_t *hammer2_chain_alloc(hammer2_dev_t *hmp,
				hammer2_pfs_t *pmp,
				hammer2_blockref_t *bref);
void hammer2_chain_init(hammer2_chain_t *chain);
void hammer2_chain_ref(hammer2_chain_t *chain);
void hammer2_chain_ref_hold(hammer2_chain_t *chain);
void hammer2_chain_drop(hammer2_chain_t *chain);
void hammer2_chain_drop_unhold(hammer2_chain_t *chain);
void hammer2_chain_unhold(hammer2_chain_t *chain);
void hammer2_chain_rehold(hammer2_chain_t *chain);
int hammer2_chain_lock(hammer2_chain_t *chain, int how);
void hammer2_chain_load_data(hammer2_chain_t *chain);

int hammer2_chain_inode_find(hammer2_pfs_t *pmp, hammer2_key_t inum,
				int clindex, int flags,
				hammer2_chain_t **parentp,
				hammer2_chain_t **chainp);
int hammer2_chain_modify(hammer2_chain_t *chain, hammer2_tid_t mtid,
				hammer2_off_t dedup_off, int flags);
int hammer2_chain_modify_ip(hammer2_inode_t *ip, hammer2_chain_t *chain,
				hammer2_tid_t mtid, int flags);
int hammer2_chain_resize(hammer2_chain_t *chain,
				hammer2_tid_t mtid, hammer2_off_t dedup_off,
				int nradix, int flags);
void hammer2_chain_unlock(hammer2_chain_t *chain);
hammer2_chain_t *hammer2_chain_get(hammer2_chain_t *parent, int generation,
				hammer2_blockref_t *bref, int how);
hammer2_chain_t *hammer2_chain_lookup_init(hammer2_chain_t *parent, int flags);
void hammer2_chain_lookup_done(hammer2_chain_t *parent);
hammer2_chain_t *hammer2_chain_getparent(hammer2_chain_t *chain, int flags);
hammer2_chain_t *hammer2_chain_repparent(hammer2_chain_t **chainp, int flags);
hammer2_chain_t *hammer2_chain_lookup(hammer2_chain_t **parentp,
				hammer2_key_t *key_nextp,
				hammer2_key_t key_beg, hammer2_key_t key_end,
				int *errorp, int flags);
hammer2_chain_t *hammer2_chain_next(hammer2_chain_t **parentp,
				hammer2_chain_t *chain,
				hammer2_key_t *key_nextp,
				hammer2_key_t key_beg, hammer2_key_t key_end,
				int *errorp, int flags);
int hammer2_chain_scan(hammer2_chain_t *parent,
				hammer2_chain_t **chainp,
				hammer2_blockref_t *bref,
				int *firstp, int flags);

int hammer2_chain_create(hammer2_chain_t **parentp, hammer2_chain_t **chainp,
				hammer2_dev_t *hmp, hammer2_pfs_t *pmp,
				int methods, hammer2_key_t key, int keybits,
				int type, size_t bytes, hammer2_tid_t mtid,
				hammer2_off_t dedup_off, int flags);
void hammer2_chain_rename(hammer2_chain_t **parentp,
				hammer2_chain_t *chain,
				hammer2_tid_t mtid, int flags);
int hammer2_chain_delete(hammer2_chain_t *parent, hammer2_chain_t *chain,
				hammer2_tid_t mtid, int flags);
int hammer2_chain_indirect_maintenance(hammer2_chain_t *parent,
				hammer2_chain_t *chain);
void hammer2_chain_setflush(hammer2_chain_t *chain);
void hammer2_chain_countbrefs(hammer2_chain_t *chain,
				hammer2_blockref_t *base, int count);
hammer2_chain_t *hammer2_chain_bulksnap(hammer2_dev_t *hmp);
void hammer2_chain_bulkdrop(hammer2_chain_t *copy);

void hammer2_chain_setcheck(hammer2_chain_t *chain, void *bdata);
int hammer2_chain_testcheck(hammer2_chain_t *chain, void *bdata);
int hammer2_chain_dirent_test(hammer2_chain_t *chain, const char *name,
				size_t name_len);

void hammer2_base_delete(hammer2_chain_t *parent,
				hammer2_blockref_t *base, int count,
				hammer2_chain_t *chain,
				hammer2_blockref_t *obref);
void hammer2_base_insert(hammer2_chain_t *parent,
				hammer2_blockref_t *base, int count,
				hammer2_chain_t *chain,
				hammer2_blockref_t *elm);
void hammer2_dump_chain(hammer2_chain_t *chain, int tab, int bi, int *countp,
				char pfx, u_int flags);

void hammer2_trans_manage_init(hammer2_pfs_t *pmp);
int hammer2_flush(hammer2_chain_t *chain, int istop);
void hammer2_trans_init(hammer2_pfs_t *pmp, uint32_t flags);
void hammer2_trans_setflags(hammer2_pfs_t *pmp, uint32_t flags);
void hammer2_trans_clearflags(hammer2_pfs_t *pmp, uint32_t flags);
hammer2_tid_t hammer2_trans_sub(hammer2_pfs_t *pmp);
void hammer2_trans_done(hammer2_pfs_t *pmp, uint32_t flags);
hammer2_tid_t hammer2_trans_newinum(hammer2_pfs_t *pmp);
void hammer2_trans_assert_strategy(hammer2_pfs_t *pmp);

int hammer2_ioctl(hammer2_inode_t *ip, u_long com, void *data,
				int fflag, struct ucred *cred);

void hammer2_io_hash_init(hammer2_dev_t *hmp);
void hammer2_io_inval(hammer2_io_t *dio, hammer2_off_t data_off, u_int bytes);
void hammer2_io_hash_cleanup_all(hammer2_dev_t *hmp);
char *hammer2_io_data(hammer2_io_t *dio, off_t lbase);
void hammer2_io_bkvasync(hammer2_io_t *dio);
void hammer2_io_dedup_set(hammer2_dev_t *hmp, hammer2_blockref_t *bref);
void hammer2_io_dedup_delete(hammer2_dev_t *hmp, uint8_t btype,
				hammer2_off_t data_off, u_int bytes);
void hammer2_io_dedup_assert(hammer2_dev_t *hmp, hammer2_off_t data_off,
				u_int bytes);
int hammer2_io_new(hammer2_dev_t *hmp, int btype, off_t lbase, int lsize,
				hammer2_io_t **diop);
int hammer2_io_newnz(hammer2_dev_t *hmp, int btype, off_t lbase, int lsize,
				hammer2_io_t **diop);
int _hammer2_io_bread(hammer2_dev_t *hmp, int btype, off_t lbase, int lsize,
				hammer2_io_t **diop HAMMER2_IO_DEBUG_ARGS);
void hammer2_io_setdirty(hammer2_io_t *dio);

hammer2_io_t *_hammer2_io_getblk(hammer2_dev_t *hmp, int btype, off_t lbase,
				int lsize, int op HAMMER2_IO_DEBUG_ARGS);
hammer2_io_t *_hammer2_io_getquick(hammer2_dev_t *hmp, off_t lbase,
				int lsize HAMMER2_IO_DEBUG_ARGS);
void _hammer2_io_putblk(hammer2_io_t **diop HAMMER2_IO_DEBUG_ARGS);
int _hammer2_io_bwrite(hammer2_io_t **diop HAMMER2_IO_DEBUG_ARGS);
void _hammer2_io_bawrite(hammer2_io_t **diop HAMMER2_IO_DEBUG_ARGS);
void _hammer2_io_bdwrite(hammer2_io_t **diop HAMMER2_IO_DEBUG_ARGS);
void _hammer2_io_brelse(hammer2_io_t **diop HAMMER2_IO_DEBUG_ARGS);
void _hammer2_io_bqrelse(hammer2_io_t **diop HAMMER2_IO_DEBUG_ARGS);
void _hammer2_io_ref(hammer2_io_t *dio HAMMER2_IO_DEBUG_ARGS);

#ifndef HAMMER2_IO_DEBUG

#define hammer2_io_getblk(hmp, btype, lbase, lsize, op)			\
	_hammer2_io_getblk((hmp), (btype), (lbase), (lsize), (op))
#define hammer2_io_getquick(hmp, lbase, lsize)				\
	_hammer2_io_getquick((hmp), (lbase), (lsize))
#define hammer2_io_putblk(diop)						\
	_hammer2_io_putblk(diop)
#define hammer2_io_bwrite(diop)						\
	_hammer2_io_bwrite((diop))
#define hammer2_io_bawrite(diop)					\
	_hammer2_io_bawrite((diop))
#define hammer2_io_bdwrite(diop)					\
	_hammer2_io_bdwrite((diop))
#define hammer2_io_brelse(diop)						\
	_hammer2_io_brelse((diop))
#define hammer2_io_bqrelse(diop)					\
	_hammer2_io_bqrelse((diop))
#define hammer2_io_ref(dio)						\
	_hammer2_io_ref((dio))

#define hammer2_io_bread(hmp, btype, lbase, lsize, diop)		\
	_hammer2_io_bread((hmp), (btype), (lbase), (lsize), (diop))

#else

#define hammer2_io_getblk(hmp, btype, lbase, lsize, op)			\
	_hammer2_io_getblk((hmp), (btype), (lbase), (lsize), (op),	\
	__FILE__, __LINE__)

#define hammer2_io_getquick(hmp, lbase, lsize)				\
	_hammer2_io_getquick((hmp), (lbase), (lsize), __FILE__, __LINE__)

#define hammer2_io_putblk(diop)						\
	_hammer2_io_putblk(diop, __FILE__, __LINE__)

#define hammer2_io_bwrite(diop)						\
	_hammer2_io_bwrite((diop), __FILE__, __LINE__)
#define hammer2_io_bawrite(diop)					\
	_hammer2_io_bawrite((diop), __FILE__, __LINE__)
#define hammer2_io_bdwrite(diop)					\
	_hammer2_io_bdwrite((diop), __FILE__, __LINE__)
#define hammer2_io_brelse(diop)						\
	_hammer2_io_brelse((diop), __FILE__, __LINE__)
#define hammer2_io_bqrelse(diop)					\
	_hammer2_io_bqrelse((diop), __FILE__, __LINE__)
#define hammer2_io_ref(dio)						\
	_hammer2_io_ref((dio), __FILE__, __LINE__)

#define hammer2_io_bread(hmp, btype, lbase, lsize, diop)		\
	_hammer2_io_bread((hmp), (btype), (lbase), (lsize), (diop),	\
			  __FILE__, __LINE__)

#endif

void hammer2_thr_signal(hammer2_thread_t *thr, uint32_t flags);
void hammer2_thr_signal2(hammer2_thread_t *thr,
			uint32_t pflags, uint32_t nflags);
void hammer2_thr_wait(hammer2_thread_t *thr, uint32_t flags);
void hammer2_thr_wait_neg(hammer2_thread_t *thr, uint32_t flags);
int hammer2_thr_wait_any(hammer2_thread_t *thr, uint32_t flags, int timo);
void hammer2_thr_create(hammer2_thread_t *thr,
			hammer2_pfs_t *pmp, hammer2_dev_t *hmp,
			const char *id, int clindex, int repidx,
			void (*func)(void *arg));
void hammer2_thr_delete(hammer2_thread_t *thr);
void hammer2_thr_remaster(hammer2_thread_t *thr);
void hammer2_thr_freeze_async(hammer2_thread_t *thr);
void hammer2_thr_freeze(hammer2_thread_t *thr);
void hammer2_thr_unfreeze(hammer2_thread_t *thr);
int hammer2_thr_break(hammer2_thread_t *thr);
void hammer2_primary_xops_thread(void *arg);

void *hammer2_xop_alloc(hammer2_inode_t *ip, int flags);
void hammer2_xop_setname(hammer2_xop_head_t *xop,
				const char *name, size_t name_len);
void hammer2_xop_setname2(hammer2_xop_head_t *xop,
				const char *name, size_t name_len);
size_t hammer2_xop_setname_inum(hammer2_xop_head_t *xop, hammer2_key_t inum);
void hammer2_xop_setip2(hammer2_xop_head_t *xop, hammer2_inode_t *ip2);
void hammer2_xop_setip3(hammer2_xop_head_t *xop, hammer2_inode_t *ip3);
void hammer2_xop_setip4(hammer2_xop_head_t *xop, hammer2_inode_t *ip4);
void hammer2_xop_reinit(hammer2_xop_head_t *xop);
void hammer2_xop_helper_create(hammer2_pfs_t *pmp);
void hammer2_xop_helper_cleanup(hammer2_pfs_t *pmp);
void hammer2_xop_start(hammer2_xop_head_t *xop, hammer2_xop_desc_t *desc);
void hammer2_xop_start_except(hammer2_xop_head_t *xop, hammer2_xop_desc_t *desc,
				int notidx);
int hammer2_xop_collect(hammer2_xop_head_t *xop, int flags);
void hammer2_xop_retire(hammer2_xop_head_t *xop, uint64_t mask);
int hammer2_xop_active(hammer2_xop_head_t *xop);
int hammer2_xop_feed(hammer2_xop_head_t *xop, hammer2_chain_t *chain,
				int clindex, int error);

void hammer2_primary_sync_thread(void *arg);

void hammer2_xop_ipcluster(hammer2_xop_t *xop, void *scratch, int clindex);
void hammer2_xop_readdir(hammer2_xop_t *xop, void *scratch, int clindex);
void hammer2_xop_nresolve(hammer2_xop_t *xop, void *scratch, int clindex);
void hammer2_xop_unlink(hammer2_xop_t *xop, void *scratch, int clindex);
void hammer2_xop_nrename(hammer2_xop_t *xop, void *scratch, int clindex);
void hammer2_xop_scanlhc(hammer2_xop_t *xop, void *scratch, int clindex);
void hammer2_xop_scanall(hammer2_xop_t *xop, void *scratch, int clindex);
void hammer2_xop_lookup(hammer2_xop_t *xop, void *scratch, int clindex);
void hammer2_xop_delete(hammer2_xop_t *xop, void *scratch, int clindex);
void hammer2_xop_inode_mkdirent(hammer2_xop_t *xop, void *scratch, int clindex);
void hammer2_xop_inode_create(hammer2_xop_t *xop, void *scratch, int clindex);
void hammer2_xop_inode_create_det(hammer2_xop_t *xop,
				void *scratch, int clindex);
void hammer2_xop_inode_create_ins(hammer2_xop_t *xop,
				void *scratch, int clindex);
void hammer2_xop_inode_destroy(hammer2_xop_t *xop, void *scratch, int clindex);
void hammer2_xop_inode_chain_sync(hammer2_xop_t *xop, void *scratch,
				int clindex);
void hammer2_xop_inode_unlinkall(hammer2_xop_t *xop, void *scratch,
				int clindex);
void hammer2_xop_inode_connect(hammer2_xop_t *xop, void *scratch, int clindex);
void hammer2_xop_inode_flush(hammer2_xop_t *xop, void *scratch, int clindex);
void hammer2_xop_strategy_read(hammer2_xop_t *xop, void *scratch, int clindex);
void hammer2_xop_strategy_write(hammer2_xop_t *xop, void *scratch, int clindex);
void hammer2_xop_bmap(hammer2_xop_t *xop, void *scratch, int clindex);

void hammer2_dmsg_ipcluster(hammer2_xop_t *xop, void *scratch, int clindex);
void hammer2_dmsg_readdir(hammer2_xop_t *xop, void *scratch, int clindex);
void hammer2_dmsg_nresolve(hammer2_xop_t *xop, void *scratch, int clindex);
void hammer2_dmsg_unlink(hammer2_xop_t *xop, void *scratch, int clindex);
void hammer2_dmsg_nrename(hammer2_xop_t *xop, void *scratch, int clindex);
void hammer2_dmsg_scanlhc(hammer2_xop_t *xop, void *scratch, int clindex);
void hammer2_dmsg_scanall(hammer2_xop_t *xop, void *scratch, int clindex);
void hammer2_dmsg_lookup(hammer2_xop_t *xop, void *scratch, int clindex);
void hammer2_dmsg_inode_mkdirent(hammer2_xop_t *xop, void *scratch,
				int clindex);
void hammer2_dmsg_inode_create(hammer2_xop_t *xop, void *scratch, int clindex);
void hammer2_dmsg_inode_destroy(hammer2_xop_t *xop, void *scratch, int clindex);
void hammer2_dmsg_inode_chain_sync(hammer2_xop_t *xop, void *scratch,
				int clindex);
void hammer2_dmsg_inode_unlinkall(hammer2_xop_t *xop, void *scratch,
				int clindex);
void hammer2_dmsg_inode_connect(hammer2_xop_t *xop, void *scratch, int clindex);
void hammer2_dmsg_inode_flush(hammer2_xop_t *xop, void *scratch, int clindex);
void hammer2_dmsg_strategy_read(hammer2_xop_t *xop, void *scratch, int clindex);
void hammer2_dmsg_strategy_write(hammer2_xop_t *xop, void *scratch,
				int clindex);

void hammer2_rmsg_ipcluster(hammer2_xop_t *xop, void *scratch, int clindex);
void hammer2_rmsg_readdir(hammer2_xop_t *xop, void *scratch, int clindex);
void hammer2_rmsg_nresolve(hammer2_xop_t *xop, void *scratch, int clindex);
void hammer2_rmsg_unlink(hammer2_xop_t *xop, void *scratch, int clindex);
void hammer2_rmsg_nrename(hammer2_xop_t *xop, void *scratch, int clindex);
void hammer2_rmsg_scanlhc(hammer2_xop_t *xop, void *scratch, int clindex);
void hammer2_rmsg_scanall(hammer2_xop_t *xop, void *scratch, int clindex);
void hammer2_rmsg_lookup(hammer2_xop_t *xop, void *scratch, int clindex);
void hammer2_rmsg_inode_mkdirent(hammer2_xop_t *xop, void *scratch,
				int clindex);
void hammer2_rmsg_inode_create(hammer2_xop_t *xop, void *scratch, int clindex);
void hammer2_rmsg_inode_destroy(hammer2_xop_t *xop, void *scratch, int clindex);
void hammer2_rmsg_inode_chain_sync(hammer2_xop_t *xop, void *scratch,
				int clindex);
void hammer2_rmsg_inode_unlinkall(hammer2_xop_t *xop, void *scratch,
				int clindex);
void hammer2_rmsg_inode_connect(hammer2_xop_t *xop, void *scratch, int clindex);
void hammer2_rmsg_inode_flush(hammer2_xop_t *xop, void *scratch, int clindex);
void hammer2_rmsg_strategy_read(hammer2_xop_t *xop, void *scratch, int clindex);
void hammer2_rmsg_strategy_write(hammer2_xop_t *xop, void *scratch,
				int clindex);

extern hammer2_xop_desc_t hammer2_ipcluster_desc;
extern hammer2_xop_desc_t hammer2_readdir_desc;
extern hammer2_xop_desc_t hammer2_nresolve_desc;
extern hammer2_xop_desc_t hammer2_unlink_desc;
extern hammer2_xop_desc_t hammer2_nrename_desc;
extern hammer2_xop_desc_t hammer2_scanlhc_desc;
extern hammer2_xop_desc_t hammer2_scanall_desc;
extern hammer2_xop_desc_t hammer2_lookup_desc;
extern hammer2_xop_desc_t hammer2_delete_desc;
extern hammer2_xop_desc_t hammer2_inode_mkdirent_desc;
extern hammer2_xop_desc_t hammer2_inode_create_desc;
extern hammer2_xop_desc_t hammer2_inode_create_det_desc;
extern hammer2_xop_desc_t hammer2_inode_create_ins_desc;
extern hammer2_xop_desc_t hammer2_inode_destroy_desc;
extern hammer2_xop_desc_t hammer2_inode_chain_sync_desc;
extern hammer2_xop_desc_t hammer2_inode_unlinkall_desc;
extern hammer2_xop_desc_t hammer2_inode_connect_desc;
extern hammer2_xop_desc_t hammer2_inode_flush_desc;
extern hammer2_xop_desc_t hammer2_strategy_read_desc;
extern hammer2_xop_desc_t hammer2_strategy_write_desc;
extern hammer2_xop_desc_t hammer2_bmap_desc;

int hammer2_msg_dbg_rcvmsg(kdmsg_msg_t *msg);
int hammer2_msg_adhoc_input(kdmsg_msg_t *msg);

int hammer2_vfs_sync(struct mount *mp, int waitflags);
int hammer2_vfs_sync_pmp(hammer2_pfs_t *pmp, int waitfor);
int hammer2_vfs_enospace(hammer2_inode_t *ip, off_t bytes, struct ucred *cred);

hammer2_pfs_t *hammer2_pfsalloc(hammer2_chain_t *chain,
				const hammer2_inode_data_t *ripdata,
				hammer2_dev_t *force_local);
void hammer2_pfsdealloc(hammer2_pfs_t *pmp, int clindex, int destroying);
int hammer2_vfs_vget(struct mount *mp, struct vnode *dvp,
				ino_t ino, struct vnode **vpp);

void hammer2_lwinprog_ref(hammer2_pfs_t *pmp);
void hammer2_lwinprog_drop(hammer2_pfs_t *pmp);
void hammer2_lwinprog_wait(hammer2_pfs_t *pmp, int pipe);

void hammer2_pfs_memory_wait(hammer2_pfs_t *pmp);
void hammer2_pfs_memory_inc(hammer2_pfs_t *pmp);
void hammer2_pfs_memory_wakeup(hammer2_pfs_t *pmp, int count);

void hammer2_voldata_lock(hammer2_dev_t *hmp);
void hammer2_voldata_unlock(hammer2_dev_t *hmp);
void hammer2_voldata_modify(hammer2_dev_t *hmp);

int hammer2_freemap_alloc(hammer2_chain_t *chain, size_t bytes);
void hammer2_freemap_adjust(hammer2_dev_t *hmp,
				hammer2_blockref_t *bref, int how);

uint8_t hammer2_cluster_type(hammer2_cluster_t *cluster);
void hammer2_cluster_bref(hammer2_cluster_t *cluster, hammer2_blockref_t *bref);
void hammer2_cluster_ref(hammer2_cluster_t *cluster);
void hammer2_cluster_drop(hammer2_cluster_t *cluster);
void hammer2_cluster_unhold(hammer2_cluster_t *cluster);
void hammer2_cluster_rehold(hammer2_cluster_t *cluster);
void hammer2_cluster_lock(hammer2_cluster_t *cluster, int how);
int hammer2_cluster_check(hammer2_cluster_t *cluster, hammer2_key_t lokey,
			int flags);
void hammer2_cluster_unlock(hammer2_cluster_t *cluster);

void hammer2_bulkfree_init(hammer2_dev_t *hmp);
void hammer2_bulkfree_uninit(hammer2_dev_t *hmp);
int hammer2_bulkfree_pass(hammer2_dev_t *hmp, hammer2_chain_t *vchain,
			struct hammer2_ioc_bulkfree *bfi);
void hammer2_dummy_xop_from_chain(hammer2_xop_head_t *xop,
			hammer2_chain_t *chain);

void hammer2_iocom_init(hammer2_dev_t *hmp);
void hammer2_iocom_uninit(hammer2_dev_t *hmp);
void hammer2_cluster_reconnect(hammer2_dev_t *hmp, struct file *fp);
void hammer2_volconf_update(hammer2_dev_t *hmp, int index);

int hammer2_vop_strategy(struct vop_strategy_args *ap);
int hammer2_vop_bmap(struct vop_bmap_args *ap);
void hammer2_bioq_sync(hammer2_pfs_t *pmp);
void hammer2_dedup_record(hammer2_chain_t *chain, hammer2_io_t *dio,
				const char *data);
void hammer2_dedup_clear(hammer2_dev_t *hmp);

int hammer2_open_devvp(const hammer2_devvp_list_t *devvpl, int ronly);
int hammer2_close_devvp(const hammer2_devvp_list_t *devvpl, int ronly);
int hammer2_init_devvp(const char *blkdevs, int rootmount,
			hammer2_devvp_list_t *devvpl);
void hammer2_cleanup_devvp(hammer2_devvp_list_t *devvpl);
int hammer2_init_volumes(struct mount *mp, const hammer2_devvp_list_t *devvpl,
			hammer2_volume_t *volumes,
			hammer2_volume_data_t *rootvoldata,
			int *rootvolzone,
			struct vnode **rootvoldevvp);
hammer2_volume_t *hammer2_get_volume(hammer2_dev_t *hmp, hammer2_off_t offset);

#define hammer2_xop_gdata(xop)	_hammer2_xop_gdata((xop), __FILE__, __LINE__)

static __inline
const hammer2_media_data_t *
_hammer2_xop_gdata(hammer2_xop_head_t *xop, const char *file, int line)
{
	hammer2_chain_t *focus;
	const void *data;

	focus = xop->cluster.focus;
	if (focus->dio) {
		lockmgr(&focus->diolk, LK_SHARED);
		if ((xop->focus_dio = focus->dio) != NULL) {
			_hammer2_io_ref(xop->focus_dio HAMMER2_IO_DEBUG_CALL);
			hammer2_io_bkvasync(xop->focus_dio);
		}
		data = focus->data;
		lockmgr(&focus->diolk, LK_RELEASE);
	} else {
		data = focus->data;
	}

	return data;
}

#define hammer2_xop_pdata(xop)	_hammer2_xop_pdata((xop), __FILE__, __LINE__)

static __inline
void
_hammer2_xop_pdata(hammer2_xop_head_t *xop, const char *file, int line)
{
	if (xop->focus_dio)
		_hammer2_io_putblk(&xop->focus_dio HAMMER2_IO_DEBUG_CALL);
}

static __inline
void
hammer2_knote(struct vnode *vp, int flags)
{
	if (flags)
		KNOTE(&vp->v_pollinfo.vpi_kqinfo.ki_note, flags);
}

#endif
#endif
