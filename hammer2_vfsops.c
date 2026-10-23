#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/nlookup.h>
#include <sys/vnode.h>
#include <sys/mount.h>
#include <sys/fcntl.h>
#include <sys/vfsops.h>
#include <sys/sysctl.h>
#include <sys/socket.h>
#include <sys/objcache.h>
#include <sys/proc.h>
#include <sys/lock.h>
#include <sys/file.h>

#include "hammer2.h"

TAILQ_HEAD(hammer2_mntlist, hammer2_dev);
static struct hammer2_mntlist hammer2_mntlist;

struct hammer2_pfslist hammer2_pfslist;
struct hammer2_pfslist hammer2_spmplist;
struct lock hammer2_mntlk;

int hammer2_supported_version = HAMMER2_VOL_VERSION_DEFAULT;
int hammer2_debug;
int hammer2_aux_flags;
int hammer2_xop_nthreads;
int hammer2_xop_sgroups;
int hammer2_xop_xgroups;
int hammer2_xop_xbase;
int hammer2_xop_mod;
long hammer2_debug_inode;
int hammer2_cluster_meta_read = 1;
int hammer2_cluster_data_read = 4;
int hammer2_cluster_write = 0;
int hammer2_dedup_enable = 1;
int hammer2_always_compress = 0;
int hammer2_flush_pipe = 100;
int hammer2_dio_count;
int hammer2_dio_limit = 256;
int hammer2_bulkfree_tps = 5000;
int hammer2_spread_workers;
int hammer2_limit_saved_depth;
long hammer2_chain_allocs;
long hammer2_limit_saved_chains;
long hammer2_limit_dirty_chains;
long hammer2_limit_dirty_inodes;
long hammer2_count_modified_chains;
long hammer2_iod_file_read;
long hammer2_iod_meta_read;
long hammer2_iod_indr_read;
long hammer2_iod_fmap_read;
long hammer2_iod_volu_read;
long hammer2_iod_file_write;
long hammer2_iod_file_wembed;
long hammer2_iod_file_wzero;
long hammer2_iod_file_wdedup;
long hammer2_iod_meta_write;
long hammer2_iod_indr_write;
long hammer2_iod_fmap_write;
long hammer2_iod_volu_write;
static long hammer2_iod_inode_creates;
static long hammer2_iod_inode_deletes;

long hammer2_process_icrc32;
long hammer2_process_xxhash64;

MALLOC_DECLARE(M_HAMMER2_CBUFFER);
MALLOC_DEFINE(M_HAMMER2_CBUFFER, "HAMMER2-compbuffer",
		"Buffer used for compression.");

MALLOC_DECLARE(M_HAMMER2_DEBUFFER);
MALLOC_DEFINE(M_HAMMER2_DEBUFFER, "HAMMER2-decompbuffer",
		"Buffer used for decompression.");

SYSCTL_NODE(_vfs, OID_AUTO, hammer2, CTLFLAG_RW, 0, "HAMMER2 filesystem");

SYSCTL_INT(_vfs_hammer2, OID_AUTO, supported_version, CTLFLAG_RD,
	   &hammer2_supported_version, 0, "");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, aux_flags, CTLFLAG_RW,
	   &hammer2_aux_flags, 0, "");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, debug, CTLFLAG_RW,
	   &hammer2_debug, 0, "");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, debug_inode, CTLFLAG_RW,
	   &hammer2_debug_inode, 0, "");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, spread_workers, CTLFLAG_RW,
	   &hammer2_spread_workers, 0, "");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, cluster_meta_read, CTLFLAG_RW,
	   &hammer2_cluster_meta_read, 0, "");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, cluster_data_read, CTLFLAG_RW,
	   &hammer2_cluster_data_read, 0, "");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, cluster_write, CTLFLAG_RW,
	   &hammer2_cluster_write, 0, "");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, dedup_enable, CTLFLAG_RW,
	   &hammer2_dedup_enable, 0, "");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, always_compress, CTLFLAG_RW,
	   &hammer2_always_compress, 0, "");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, flush_pipe, CTLFLAG_RW,
	   &hammer2_flush_pipe, 0, "");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, bulkfree_tps, CTLFLAG_RW,
	   &hammer2_bulkfree_tps, 0, "");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, chain_allocs, CTLFLAG_RD,
	   &hammer2_chain_allocs, 0, "");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, limit_saved_chains, CTLFLAG_RW,
	   &hammer2_limit_saved_chains, 0, "");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, limit_saved_depth, CTLFLAG_RW,
	   &hammer2_limit_saved_depth, 0, "");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, limit_dirty_chains, CTLFLAG_RW,
	   &hammer2_limit_dirty_chains, 0, "");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, limit_dirty_inodes, CTLFLAG_RW,
	   &hammer2_limit_dirty_inodes, 0, "");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, count_modified_chains, CTLFLAG_RD,
	   &hammer2_count_modified_chains, 0, "");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, dio_count, CTLFLAG_RD,
	   &hammer2_dio_count, 0, "");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, dio_limit, CTLFLAG_RW,
	   &hammer2_dio_limit, 0, "");

SYSCTL_LONG(_vfs_hammer2, OID_AUTO, iod_file_read, CTLFLAG_RD,
	   &hammer2_iod_file_read, 0, "");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, iod_meta_read, CTLFLAG_RD,
	   &hammer2_iod_meta_read, 0, "");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, iod_indr_read, CTLFLAG_RD,
	   &hammer2_iod_indr_read, 0, "");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, iod_fmap_read, CTLFLAG_RD,
	   &hammer2_iod_fmap_read, 0, "");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, iod_volu_read, CTLFLAG_RD,
	   &hammer2_iod_volu_read, 0, "");

SYSCTL_LONG(_vfs_hammer2, OID_AUTO, iod_file_write, CTLFLAG_RD,
	   &hammer2_iod_file_write, 0, "");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, iod_file_wembed, CTLFLAG_RD,
	   &hammer2_iod_file_wembed, 0, "");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, iod_file_wzero, CTLFLAG_RD,
	   &hammer2_iod_file_wzero, 0, "");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, iod_file_wdedup, CTLFLAG_RD,
	   &hammer2_iod_file_wdedup, 0, "");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, iod_meta_write, CTLFLAG_RD,
	   &hammer2_iod_meta_write, 0, "");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, iod_indr_write, CTLFLAG_RD,
	   &hammer2_iod_indr_write, 0, "");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, iod_fmap_write, CTLFLAG_RD,
	   &hammer2_iod_fmap_write, 0, "");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, iod_volu_write, CTLFLAG_RD,
	   &hammer2_iod_volu_write, 0, "");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, iod_inode_creates, CTLFLAG_RD,
	   &hammer2_iod_inode_creates, 0, "");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, iod_inode_deletes, CTLFLAG_RD,
	   &hammer2_iod_inode_deletes, 0, "");

SYSCTL_LONG(_vfs_hammer2, OID_AUTO, process_icrc32, CTLFLAG_RD,
	   &hammer2_process_icrc32, 0, "");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, process_xxhash64, CTLFLAG_RD,
	   &hammer2_process_xxhash64, 0, "");

static int hammer2_vfs_init(struct vfsconf *conf);
static int hammer2_vfs_uninit(struct vfsconf *vfsp);
static int hammer2_vfs_mount(struct mount *mp, char *path, caddr_t data,
				struct ucred *cred);
static int hammer2_remount(hammer2_dev_t *, struct mount *, char *,
				struct ucred *);
static int hammer2_recovery(hammer2_dev_t *hmp);
static int hammer2_vfs_unmount(struct mount *mp, int mntflags);
static int hammer2_vfs_root(struct mount *mp, struct vnode **vpp);
static int hammer2_vfs_statfs(struct mount *mp, struct statfs *sbp,
				struct ucred *cred);
static int hammer2_vfs_statvfs(struct mount *mp, struct statvfs *sbp,
				struct ucred *cred);
static int hammer2_vfs_fhtovp(struct mount *mp, struct vnode *rootvp,
				struct fid *fhp, struct vnode **vpp);
static int hammer2_vfs_vptofh(struct vnode *vp, struct fid *fhp);
static int hammer2_vfs_checkexp(struct mount *mp, struct sockaddr *nam,
				int *exflagsp, struct ucred **credanonp);
static int hammer2_vfs_modifying(struct mount *mp);

static void hammer2_update_pmps(hammer2_dev_t *hmp);

static void hammer2_mount_helper(struct mount *mp, hammer2_pfs_t *pmp);
static void hammer2_unmount_helper(struct mount *mp, hammer2_pfs_t *pmp,
				hammer2_dev_t *hmp);
static int hammer2_fixup_pfses(hammer2_dev_t *hmp);

static struct vfsops hammer2_vfsops = {
	.vfs_flags	= 0,
	.vfs_init	= hammer2_vfs_init,
	.vfs_uninit	= hammer2_vfs_uninit,
	.vfs_sync	= hammer2_vfs_sync,
	.vfs_mount	= hammer2_vfs_mount,
	.vfs_unmount	= hammer2_vfs_unmount,
	.vfs_root	= hammer2_vfs_root,
	.vfs_statfs	= hammer2_vfs_statfs,
	.vfs_statvfs	= hammer2_vfs_statvfs,
	.vfs_vget	= hammer2_vfs_vget,
	.vfs_vptofh	= hammer2_vfs_vptofh,
	.vfs_fhtovp	= hammer2_vfs_fhtovp,
	.vfs_checkexp	= hammer2_vfs_checkexp,
	.vfs_modifying	= hammer2_vfs_modifying
};

MALLOC_DEFINE(M_HAMMER2, "HAMMER2-mount", "");

VFS_SET(hammer2_vfsops, hammer2, VFCF_MPSAFE);
MODULE_VERSION(hammer2, 1);

static
int
hammer2_vfs_init(struct vfsconf *conf)
{
	static struct objcache_malloc_args margs_read;
	static struct objcache_malloc_args margs_write;
	static struct objcache_malloc_args margs_vop;

	int error;
	int mod;

	error = 0;
	kmalloc_raise_limit(M_HAMMER2, 0);

	mod = ncpus;
	hammer2_xop_mod = mod;
	hammer2_xop_nthreads = mod * 2;
	while (hammer2_xop_nthreads / mod < HAMMER2_XOPGROUPS_MIN ||
	       hammer2_xop_nthreads < HAMMER2_XOPTHREADS_MIN)
	{
		hammer2_xop_nthreads += mod;
	}
	hammer2_xop_sgroups = hammer2_xop_nthreads / mod / 2;
	hammer2_xop_xgroups = hammer2_xop_nthreads / mod - hammer2_xop_sgroups;
	hammer2_xop_xbase = hammer2_xop_sgroups * mod;

	hammer2_dio_limit = nbuf * 2;
	if (hammer2_dio_limit > 100000)
		hammer2_dio_limit = 100000;

	if (HAMMER2_BLOCKREF_BYTES != sizeof(struct hammer2_blockref))
		error = EINVAL;
	if (HAMMER2_INODE_BYTES != sizeof(struct hammer2_inode_data))
		error = EINVAL;
	if (HAMMER2_VOLUME_BYTES != sizeof(struct hammer2_volume_data))
		error = EINVAL;

	if (error)
		kprintf("HAMMER2 structure size mismatch; cannot continue.\n");

	margs_read.objsize = 65536;
	margs_read.mtype = M_HAMMER2_DEBUFFER;

	margs_write.objsize = 32768;
	margs_write.mtype = M_HAMMER2_CBUFFER;

	margs_vop.objsize = sizeof(hammer2_xop_t);
	margs_vop.mtype = M_HAMMER2;

	cache_buffer_read = objcache_create(margs_read.mtype->ks_shortdesc,
				0, 1, NULL, NULL, NULL,
				objcache_malloc_alloc,
				objcache_malloc_free,
				&margs_read);
	cache_buffer_write = objcache_create(margs_write.mtype->ks_shortdesc,
				0, 1, NULL, NULL, NULL,
				objcache_malloc_alloc,
				objcache_malloc_free,
				&margs_write);
	cache_xops = objcache_create(margs_vop.mtype->ks_shortdesc,
				0, 1, NULL, NULL, NULL,
				objcache_malloc_alloc_zero,
				objcache_malloc_free,
				&margs_vop);

	lockinit(&hammer2_mntlk, "mntlk", 0, 0);
	TAILQ_INIT(&hammer2_mntlist);
	TAILQ_INIT(&hammer2_pfslist);
	TAILQ_INIT(&hammer2_spmplist);

	hammer2_limit_dirty_chains = maxvnodes / 10;
	if (hammer2_limit_dirty_chains > HAMMER2_LIMIT_DIRTY_CHAINS)
		hammer2_limit_dirty_chains = HAMMER2_LIMIT_DIRTY_CHAINS;
	if (hammer2_limit_dirty_chains < 1000)
		hammer2_limit_dirty_chains = 1000;

	hammer2_limit_dirty_inodes = maxvnodes / 25;
	if (hammer2_limit_dirty_inodes < 100)
		hammer2_limit_dirty_inodes = 100;
	if (hammer2_limit_dirty_inodes > HAMMER2_LIMIT_DIRTY_INODES)
		hammer2_limit_dirty_inodes = HAMMER2_LIMIT_DIRTY_INODES;

	hammer2_limit_saved_chains = hammer2_limit_dirty_chains * 5;

	return (error);
}

static
int
hammer2_vfs_uninit(struct vfsconf *vfsp __unused)
{
	objcache_destroy(cache_buffer_read);
	objcache_destroy(cache_buffer_write);
	objcache_destroy(cache_xops);
	return 0;
}

hammer2_pfs_t *
hammer2_pfsalloc(hammer2_chain_t *chain,
		 const hammer2_inode_data_t *ripdata,
		 hammer2_dev_t *force_local)
{
	hammer2_pfs_t *pmp;
	hammer2_inode_t *iroot;
	int count;
	int i;
	int j;

	pmp = NULL;

	if (ripdata) {
		TAILQ_FOREACH(pmp, &hammer2_pfslist, mntentry) {
			if (force_local != pmp->force_local)
				continue;
			if (force_local == NULL &&
			    bcmp(&pmp->pfs_clid, &ripdata->meta.pfs_clid,
				 sizeof(pmp->pfs_clid)) == 0) {
				break;
			} else if (force_local && pmp->pfs_names[0] &&
			    strcmp(pmp->pfs_names[0], (const char *)ripdata->filename) == 0) {
				break;
			}
		}
	}

	if (pmp == NULL) {
		pmp = kmalloc(sizeof(*pmp), M_HAMMER2, M_WAITOK | M_ZERO);
		pmp->force_local = force_local;
		hammer2_trans_manage_init(pmp);
		kmalloc_create_obj(&pmp->minode, "HAMMER2-inodes",
				   sizeof(struct hammer2_inode));
		lockinit(&pmp->lock, "pfslk", 0, 0);
		hammer2_spin_init(&pmp->blockset_spin, "h2blkset");
		hammer2_inum_hash_init(pmp);
		hammer2_spin_init(&pmp->xop_spin, "h2xop");
		TAILQ_INIT(&pmp->syncq);
		TAILQ_INIT(&pmp->depq);
		hammer2_spin_init(&pmp->list_spin, "h2pfsalloc_list");

		if (ripdata) {
			pmp->pfs_clid = ripdata->meta.pfs_clid;
			TAILQ_INSERT_TAIL(&hammer2_pfslist, pmp, mntentry);
		} else {
			pmp->flags |= HAMMER2_PMPF_SPMP;
			TAILQ_INSERT_TAIL(&hammer2_spmplist, pmp, mntentry);
		}

	}

	if ((iroot = pmp->iroot) == NULL) {
		iroot = hammer2_inode_get(pmp, NULL, 1, -1);
		if (ripdata)
			iroot->meta = ripdata->meta;
		pmp->iroot = iroot;
		hammer2_inode_ref(iroot);
		hammer2_inode_unlock(iroot);
	}

	if (chain == NULL)
		goto done;

	hammer2_inode_ref(iroot);
	hammer2_mtx_ex(&iroot->lock);
	j = iroot->cluster.nchains;

	if (j == HAMMER2_MAXCLUSTER) {
		kprintf("hammer2_pfsalloc: cluster full!\n");
	} else {
		KKASSERT(chain->pmp == NULL);
		chain->pmp = pmp;
		hammer2_chain_ref(chain);
		iroot->cluster.array[j].chain = chain;
		if (force_local)
			pmp->pfs_types[j] = HAMMER2_PFSTYPE_MASTER;
		else
			pmp->pfs_types[j] = ripdata->meta.pfs_type;
		pmp->pfs_names[j] = kstrdup((const char *)ripdata->filename, M_HAMMER2);
		pmp->pfs_hmps[j] = chain->hmp;
		hammer2_spin_ex(&pmp->blockset_spin);
		pmp->pfs_iroot_blocksets[j] = chain->data->ipdata.u.blockset;
		hammer2_spin_unex(&pmp->blockset_spin);

		if (pmp->mp)
			++chain->hmp->mount_count;

		if (chain->flags & HAMMER2_CHAIN_MODIFIED)
			hammer2_pfs_memory_inc(pmp);
		++j;
	}
	iroot->cluster.nchains = j;

	if (ripdata && pmp->pfs_nmasters < ripdata->meta.pfs_nmasters) {
		pmp->pfs_nmasters = ripdata->meta.pfs_nmasters;
	}

	count = 0;
	for (i = 0; i < iroot->cluster.nchains; ++i) {
		if (pmp->pfs_types[i] == HAMMER2_PFSTYPE_MASTER)
			++count;
	}
	if (pmp->pfs_nmasters < count)
		pmp->pfs_nmasters = count;

	for (i = 0; i < iroot->cluster.nchains; ++i) {
		if (pmp->pfs_nmasters <= 1 &&
		    pmp->pfs_types[i] == HAMMER2_PFSTYPE_MASTER) {
			continue;
		}

		if (pmp->sync_thrs[i].td == NULL) {
			hammer2_thr_create(&pmp->sync_thrs[i], pmp, NULL,
					   "h2nod", i, -1,
					   hammer2_primary_sync_thread);
		}
	}

	if (pmp->mp || iroot->cluster.nchains >= 2)
		hammer2_xop_helper_create(pmp);

	hammer2_mtx_unlock(&iroot->lock);
	hammer2_inode_drop(iroot);
done:
	return pmp;
}

void
hammer2_pfsdealloc(hammer2_pfs_t *pmp, int clindex, int destroying)
{
	hammer2_inode_t *iroot;
	hammer2_chain_t *chain;
	int j;

	iroot = pmp->iroot;
	if (iroot) {
		hammer2_thr_delete(&pmp->sync_thrs[clindex]);

		hammer2_mtx_ex(&iroot->lock);
		chain = iroot->cluster.array[clindex].chain;
		iroot->cluster.array[clindex].chain = NULL;

		switch(pmp->pfs_types[clindex]) {
		case HAMMER2_PFSTYPE_MASTER:
			if (destroying && pmp->pfs_nmasters > 0)
				--pmp->pfs_nmasters;
			break;
		default:
			break;
		}
		pmp->pfs_types[clindex] = HAMMER2_PFSTYPE_NONE;

		hammer2_mtx_unlock(&iroot->lock);

		if (chain) {
			atomic_set_int(&chain->flags, HAMMER2_CHAIN_RELEASE);
			hammer2_chain_drop(chain);
		}

		if (pmp->xop_groups) {
			for (j = 0; j < hammer2_xop_nthreads; ++j) {
				hammer2_thr_delete(
					&pmp->xop_groups[j].thrs[clindex]);
			}
		}
	}
}

static void
hammer2_pfsfree(hammer2_pfs_t *pmp)
{
	hammer2_inode_t *iroot;
	hammer2_chain_t *chain;
	int chains_still_present = 0;
	int i;
	int j;

	if (pmp->flags & HAMMER2_PMPF_SPMP)
		TAILQ_REMOVE(&hammer2_spmplist, pmp, mntentry);
	else
		TAILQ_REMOVE(&hammer2_pfslist, pmp, mntentry);

	iroot = pmp->iroot;
	if (iroot) {
		for (i = 0; i < iroot->cluster.nchains; ++i) {
			hammer2_thr_delete(&pmp->sync_thrs[i]);
			if (pmp->xop_groups) {
				for (j = 0; j < hammer2_xop_nthreads; ++j)
					hammer2_thr_delete(
						&pmp->xop_groups[j].thrs[i]);
			}
			chain = iroot->cluster.array[i].chain;
			if (chain && !RB_EMPTY(&chain->core.rbtree)) {
				kprintf("hammer2: Warning pmp %p still "
					"has active chains\n", pmp);
				chains_still_present = 1;
			}
		}
		KASSERT(iroot->refs == 1,
			("PMP->IROOT %p REFS WRONG %d", iroot, iroot->refs));

		hammer2_inode_drop(iroot);
		pmp->iroot = NULL;
	}

	if (chains_still_present) {
		kprintf("hammer2: cannot free pmp %p, still in use\n", pmp);
	} else {
		kmalloc_destroy_obj(&pmp->minode);
		kfree(pmp, M_HAMMER2);
	}
}

static void
hammer2_pfsfree_scan(hammer2_dev_t *hmp, int which)
{
	hammer2_pfs_t *pmp;
	hammer2_inode_t *iroot;
	hammer2_chain_t *rchain;
	int i;
	int j;
	struct hammer2_pfslist *wlist;

	if (which == 0)
		wlist = &hammer2_pfslist;
	else
		wlist = &hammer2_spmplist;
again:
	TAILQ_FOREACH(pmp, wlist, mntentry) {
		if ((iroot = pmp->iroot) == NULL)
			continue;

		for (i = 0; i < HAMMER2_MAXCLUSTER; ++i) {
			if (pmp->pfs_hmps[i] == hmp)
				break;
		}
		if (i == HAMMER2_MAXCLUSTER)
			continue;

		hammer2_vfs_sync_pmp(pmp, MNT_WAIT);

		for (i = 0; i < HAMMER2_MAXCLUSTER; ++i) {
			if (pmp->pfs_hmps[i] == NULL)
				continue;
			hammer2_thr_freeze_async(&pmp->sync_thrs[i]);
			if (pmp->xop_groups) {
				for (j = 0; j < hammer2_xop_nthreads; ++j) {
					hammer2_thr_freeze_async(
						&pmp->xop_groups[j].thrs[i]);
				}
			}
		}
		for (i = 0; i < HAMMER2_MAXCLUSTER; ++i) {
			if (pmp->pfs_hmps[i] == NULL)
				continue;
			hammer2_thr_freeze(&pmp->sync_thrs[i]);
			if (pmp->xop_groups) {
				for (j = 0; j < hammer2_xop_nthreads; ++j) {
					hammer2_thr_freeze(
						&pmp->xop_groups[j].thrs[i]);
				}
			}
		}

		hammer2_mtx_ex(&iroot->lock);

		for (i = 0; i < HAMMER2_MAXCLUSTER; ++i) {
			if (pmp->pfs_hmps[i] != hmp)
				continue;
			hammer2_thr_delete(&pmp->sync_thrs[i]);
			if (pmp->xop_groups) {
				for (j = 0; j < hammer2_xop_nthreads; ++j) {
					hammer2_thr_delete(
						&pmp->xop_groups[j].thrs[i]);
				}
			}
			rchain = iroot->cluster.array[i].chain;
			iroot->cluster.array[i].chain = NULL;
			pmp->pfs_types[i] = HAMMER2_PFSTYPE_NONE;
			if (pmp->pfs_names[i]) {
				kfree(pmp->pfs_names[i], M_HAMMER2);
				pmp->pfs_names[i] = NULL;
			}
			if (rchain) {
				hammer2_chain_drop(rchain);
				if (iroot->cluster.focus == rchain)
					iroot->cluster.focus = NULL;
			}
			pmp->pfs_hmps[i] = NULL;
		}
		hammer2_mtx_unlock(&iroot->lock);

		for (i = HAMMER2_MAXCLUSTER - 1; i >= 0; --i) {
			if (pmp->pfs_hmps[i])
				break;
		}
		iroot->cluster.nchains = i + 1;

		if (iroot->cluster.nchains == 0) {
			if (hmp->spmp == pmp) {
				hmp->spmp = NULL;
				hmp->vchain.pmp = NULL;
				hmp->fchain.pmp = NULL;
			}

			KKASSERT(TAILQ_EMPTY(&pmp->syncq));
			KKASSERT(TAILQ_EMPTY(&pmp->depq));
			hammer2_pfsfree(pmp);
			goto again;
		}

		for (i = 0; i < HAMMER2_MAXCLUSTER; ++i) {
			if (pmp->pfs_hmps[i] == NULL)
				continue;
			hammer2_thr_remaster(&pmp->sync_thrs[i]);
			hammer2_thr_unfreeze(&pmp->sync_thrs[i]);
			if (pmp->xop_groups) {
				for (j = 0; j < hammer2_xop_nthreads; ++j) {
					hammer2_thr_remaster(
						&pmp->xop_groups[j].thrs[i]);
					hammer2_thr_unfreeze(
						&pmp->xop_groups[j].thrs[i]);
				}
			}
		}
	}
}

static
int
hammer2_vfs_mount(struct mount *mp, char *path, caddr_t data,
		  struct ucred *cred)
{
	struct hammer2_mount_info info;
	hammer2_pfs_t *pmp;
	hammer2_pfs_t *spmp;
	hammer2_dev_t *hmp, *hmp_tmp;
	hammer2_dev_t *force_local;
	hammer2_key_t key_next;
	hammer2_key_t key_dummy;
	hammer2_key_t lhc;
	hammer2_chain_t *parent;
	hammer2_chain_t *chain;
	const hammer2_inode_data_t *ripdata;
	hammer2_devvp_list_t devvpl;
	hammer2_devvp_t *e, *e_tmp;
	struct file *fp;
	char devstr[MNAMELEN];
	size_t size;
	size_t done;
	char *label;
	int ronly = ((mp->mnt_flag & MNT_RDONLY) != 0);
	int error;
	int i;

	hmp = NULL;
	pmp = NULL;
	label = NULL;
	bzero(&info, sizeof(info));

	if (path) {
		error = copyin(data, &info, sizeof(info));
		if (error)
			return (error);
	}

	if (mp->mnt_flag & MNT_UPDATE) {
		hammer2_cluster_t *cluster;

		error = 0;
		pmp = MPTOPMP(mp);
		pmp->hflags = info.hflags;
		cluster = &pmp->iroot->cluster;
		for (i = 0; i < cluster->nchains; ++i) {
			if (cluster->array[i].chain == NULL)
				continue;
			hmp = cluster->array[i].chain->hmp;
			error = hammer2_remount(hmp, mp, path, cred);
			if (error)
				break;
		}

		return error;
	}

	if (path == NULL) {
		info.cluster_fd = -1;
		ksnprintf(devstr, sizeof(devstr), "%s",
			  mp->mnt_stat.f_mntfromname);
		done = strlen(devstr) + 1;
		kprintf("hammer2_mount: root devstr=\"%s\"\n", devstr);
	} else {
		error = copyinstr(info.volume, devstr, MNAMELEN - 1, &done);
		if (error)
			return (error);
		kprintf("hammer2_mount: devstr=\"%s\"\n", devstr);
	}

	label = strchr(devstr, '@');
	if (label && ((label + 1) - devstr) > done) {
		kprintf("hammer2_mount: bad label %s/%zd\n", devstr, done);
		return (EINVAL);
	}
	if (label == NULL || label[1] == 0) {
		char slice;

		if (label == NULL)
			label = devstr + strlen(devstr);
		else
			*label = '\0';

		slice = label[-1];
		switch(slice) {
		case 'a':
			label = "BOOT";
			break;
		case 'd':
			label = "ROOT";
			break;
		default:
			label = "DATA";
			break;
		}
	} else {
		*label = '\0';
		label++;
	}

	kprintf("hammer2_mount: device=\"%s\" label=\"%s\" rdonly=%d\n",
		devstr, label, ronly);

	TAILQ_INIT(&devvpl);
	error = hammer2_init_devvp(devstr, path == NULL, &devvpl);
	if (error) {
		kprintf("hammer2: failed to initialize devvp in %s\n", devstr);
		hammer2_cleanup_devvp(&devvpl);
		return error;
	}

	lockmgr(&hammer2_mntlk, LK_EXCLUSIVE);
	if (!TAILQ_EMPTY(&devvpl)) {
		TAILQ_FOREACH(hmp_tmp, &hammer2_mntlist, mntentry) {
			TAILQ_FOREACH(e_tmp, &hmp_tmp->devvpl, entry) {
				int devvp_found = 0;
				TAILQ_FOREACH(e, &devvpl, entry) {
					KKASSERT(e->devvp);
					if (e_tmp->devvp == e->devvp)
						devvp_found = 1;
					if (e_tmp->devvp->v_rdev &&
					    e_tmp->devvp->v_rdev == e->devvp->v_rdev)
						devvp_found = 1;
				}
				if (!devvp_found)
					goto next_hmp;
			}
			hmp = hmp_tmp;
			kprintf("hammer2_mount: hmp=%p matched\n", hmp);
			break;
next_hmp:
			continue;
		}

		if (hmp == NULL) {
			TAILQ_FOREACH(e, &devvpl, entry) {
				struct vnode *devvp = e->devvp;
				KKASSERT(devvp);
				error = vfs_mountedon(devvp);
				if (error) {
					kprintf("hammer2_mount: %s mounted %d\n",
						e->path, error);
					hammer2_cleanup_devvp(&devvpl);
					lockmgr(&hammer2_mntlk, LK_RELEASE);
					return error;
				}
			}
		}
	} else {
		TAILQ_FOREACH(pmp, &hammer2_pfslist, mntentry) {
			for (i = 0; i < HAMMER2_MAXCLUSTER; ++i) {
				if (pmp->pfs_names[i] &&
				    strcmp(pmp->pfs_names[i], label) == 0) {
					hmp = pmp->pfs_hmps[i];
					break;
				}
			}
			if (hmp)
				break;
		}
		if (hmp == NULL) {
			kprintf("hammer2_mount: PFS label \"%s\" not found\n",
				label);
			hammer2_cleanup_devvp(&devvpl);
			lockmgr(&hammer2_mntlk, LK_RELEASE);
			return ENOENT;
		}
	}

	if (hmp == NULL) {
		hammer2_chain_t *schain;
		hammer2_xop_head_t xop;

		KKASSERT(!TAILQ_EMPTY(&devvpl));
		error = hammer2_open_devvp(&devvpl, ronly);
		if (error) {
			hammer2_close_devvp(&devvpl, ronly);
			hammer2_cleanup_devvp(&devvpl);
			lockmgr(&hammer2_mntlk, LK_RELEASE);
			return error;
		}

		hmp = kmalloc(sizeof(*hmp), M_HAMMER2, M_WAITOK | M_ZERO);
		hmp->devvp = NULL;
		error = hammer2_init_volumes(mp, &devvpl, hmp->volumes,
					     &hmp->voldata, &hmp->volhdrno,
					     &hmp->devvp);
		if (error) {
			hammer2_close_devvp(&devvpl, ronly);
			hammer2_cleanup_devvp(&devvpl);
			lockmgr(&hammer2_mntlk, LK_RELEASE);
			kfree(hmp, M_HAMMER2);
			return error;
		}
		if (!hmp->devvp) {
			kprintf("hammer2: failed to initialize root volume\n");
			hammer2_unmount_helper(mp, NULL, hmp);
			lockmgr(&hammer2_mntlk, LK_RELEASE);
			hammer2_vfs_unmount(mp, MNT_FORCE);
			return EINVAL;
		}

		ksnprintf(hmp->devrepname, sizeof(hmp->devrepname), "%s", devstr);
		hmp->ronly = ronly;
		hmp->hflags = info.hflags & HMNT2_DEVFLAGS;
		kmalloc_create_obj(&hmp->mchain, "HAMMER2-chains",
				   sizeof(struct hammer2_chain));
		kmalloc_create_obj(&hmp->mio, "HAMMER2-dio",
				   sizeof(struct hammer2_io));
		kmalloc_create(&hmp->mmsg, "HAMMER2-msg");
		TAILQ_INSERT_TAIL(&hammer2_mntlist, hmp, mntentry);
		hammer2_io_hash_init(hmp);
		hammer2_spin_init(&hmp->list_spin, "h2mount_list");

		lockinit(&hmp->vollk, "h2vol", 0, 0);
		lockinit(&hmp->bulklk, "h2bulk", 0, 0);
		lockinit(&hmp->bflock, "h2bflk", 0, 0);

		hmp->vchain.hmp = hmp;
		hmp->vchain.refs = 1;
		hmp->vchain.data = (void *)&hmp->voldata;
		hmp->vchain.bref.type = HAMMER2_BREF_TYPE_VOLUME;
		hmp->vchain.bref.data_off = 0 | HAMMER2_PBUFRADIX;
		hammer2_chain_init(&hmp->vchain);

		hmp->fchain.hmp = hmp;
		hmp->fchain.refs = 1;
		hmp->fchain.data = (void *)&hmp->voldata.freemap_blockset;
		hmp->fchain.bref.type = HAMMER2_BREF_TYPE_FREEMAP;
		hmp->fchain.bref.data_off = 0 | HAMMER2_PBUFRADIX;
		hmp->fchain.bref.methods =
			HAMMER2_ENC_CHECK(HAMMER2_CHECK_FREEMAP) |
			HAMMER2_ENC_COMP(HAMMER2_COMP_NONE);
		hammer2_chain_init(&hmp->fchain);

		KKASSERT(hmp->voldata.magic == HAMMER2_VOLUME_ID_HBO ||
			 hmp->voldata.magic == HAMMER2_VOLUME_ID_ABO);
		hmp->volsync = hmp->voldata;
		hmp->free_reserved = hmp->voldata.allocator_size / 20;
		if (hmp->voldata.version >= HAMMER2_VOL_VERSION_MULTI_VOLUMES) {
			hmp->nvolumes = hmp->voldata.nvolumes;
			hmp->total_size = hmp->voldata.total_size;
		} else {
			hmp->nvolumes = 1;
			hmp->total_size = hmp->voldata.volu_size;
		}
		KKASSERT(hmp->nvolumes > 0);

		TAILQ_INIT(&hmp->devvpl);
		while ((e = TAILQ_FIRST(&devvpl)) != NULL) {
			TAILQ_REMOVE(&devvpl, e, entry);
			TAILQ_INSERT_TAIL(&hmp->devvpl, e, entry);
		}
		KKASSERT(TAILQ_EMPTY(&devvpl));
		KKASSERT(!TAILQ_EMPTY(&hmp->devvpl));

		hmp->spmp = hammer2_pfsalloc(NULL, NULL, NULL);
		spmp = hmp->spmp;
		spmp->pfs_hmps[0] = hmp;

		hmp->vchain.bref.mirror_tid = hmp->voldata.mirror_tid;
		hmp->vchain.bref.modify_tid = hmp->vchain.bref.mirror_tid;
		hmp->vchain.pmp = spmp;
		hmp->fchain.bref.mirror_tid = hmp->voldata.freemap_tid;
		hmp->fchain.bref.modify_tid = hmp->fchain.bref.mirror_tid;
		hmp->fchain.pmp = spmp;

		parent = hammer2_chain_lookup_init(&hmp->vchain, 0);
		schain = hammer2_chain_lookup(&parent, &key_dummy,
				      HAMMER2_SROOT_KEY, HAMMER2_SROOT_KEY,
				      &error, 0);
		hammer2_chain_lookup_done(parent);
		if (schain == NULL) {
			kprintf("hammer2_mount: invalid super-root\n");
			hammer2_unmount_helper(mp, NULL, hmp);
			lockmgr(&hammer2_mntlk, LK_RELEASE);
			hammer2_vfs_unmount(mp, MNT_FORCE);
			return EINVAL;
		}
		if (schain->error) {
			kprintf("hammer2_mount: error %s reading super-root\n",
				hammer2_error_str(schain->error));
			hammer2_chain_unlock(schain);
			hammer2_chain_drop(schain);
			schain = NULL;
			hammer2_unmount_helper(mp, NULL, hmp);
			lockmgr(&hammer2_mntlk, LK_RELEASE);
			hammer2_vfs_unmount(mp, MNT_FORCE);
			return EINVAL;
		}

		spmp->inode_tid = 1;
		spmp->modify_tid = schain->bref.modify_tid + 1;

		ripdata = &schain->data->ipdata;
		KKASSERT(schain->pmp == NULL);
		spmp->pfs_clid = ripdata->meta.pfs_clid;

		hammer2_dummy_xop_from_chain(&xop, schain);
		hammer2_inode_drop(spmp->iroot);
		spmp->iroot = hammer2_inode_get(spmp, &xop, -1, -1);
		spmp->spmp_hmp = hmp;
		spmp->pfs_types[0] = ripdata->meta.pfs_type;
		spmp->ronly = ronly;
		hammer2_inode_ref(spmp->iroot);
		hammer2_inode_unlock(spmp->iroot);
		hammer2_cluster_unlock(&xop.cluster);
		hammer2_chain_drop(schain);
		schain = NULL;

		if (!hmp->ronly) {
			error = hammer2_recovery(hmp);
			if (error == 0)
				error |= hammer2_fixup_pfses(hmp);
		}
		hammer2_update_pmps(hmp);
		hammer2_iocom_init(hmp);
		hammer2_bulkfree_init(hmp);

		if (info.cluster_fd >= 0) {
			fp = holdfp(curthread, info.cluster_fd, -1);
			if (fp) {
				hammer2_cluster_reconnect(hmp, fp);
			} else {
				kprintf("hammer2_mount: bad cluster_fd!\n");
			}
		}
	} else {
		hammer2_cleanup_devvp(&devvpl);
		spmp = hmp->spmp;
		if (info.hflags & HMNT2_DEVFLAGS) {
			kprintf("hammer2_mount: Warning: mount flags pertaining "
				"to the whole device may only be specified "
				"on the first mount of the device: %08x\n",
				info.hflags & HMNT2_DEVFLAGS);
		}
	}

	force_local = (hmp->hflags & HMNT2_LOCAL) ? hmp : NULL;

	hammer2_inode_lock(spmp->iroot, 0);
	parent = hammer2_inode_chain(spmp->iroot, 0, HAMMER2_RESOLVE_ALWAYS);
	lhc = hammer2_dirhash(label, strlen(label));
	chain = hammer2_chain_lookup(&parent, &key_next,
				     lhc, lhc + HAMMER2_DIRHASH_LOMASK,
				     &error, 0);
	while (chain) {
		if (chain->bref.type == HAMMER2_BREF_TYPE_INODE &&
		    strcmp(label, (char *)chain->data->ipdata.filename) == 0) {
			break;
		}
		chain = hammer2_chain_next(&parent, chain, &key_next,
					    key_next,
					    lhc + HAMMER2_DIRHASH_LOMASK,
					    &error, 0);
	}
	if (parent) {
		hammer2_chain_unlock(parent);
		hammer2_chain_drop(parent);
	}
	hammer2_inode_unlock(spmp->iroot);

	if (chain == NULL) {
		hammer2_unmount_helper(mp, NULL, hmp);
		lockmgr(&hammer2_mntlk, LK_RELEASE);
		hammer2_vfs_unmount(mp, MNT_FORCE);

		if (error) {
			kprintf("hammer2_mount: PFS label I/O error\n");
			return EINVAL;
		} else {
			kprintf("hammer2_mount: PFS label \"%s\" not found\n",
				label);
			return ENOENT;
		}
	}

	if (chain->error) {
		kprintf("hammer2_mount: PFS label I/O error\n");
	} else {
		ripdata = &chain->data->ipdata;
		pmp = hammer2_pfsalloc(NULL, ripdata, force_local);
	}
	hammer2_chain_unlock(chain);
	hammer2_chain_drop(chain);

	if (pmp == NULL) {
		kprintf("hammer2_mount: Failed to acquire PFS structure\n");
		hammer2_unmount_helper(mp, NULL, hmp);
		lockmgr(&hammer2_mntlk, LK_RELEASE);
		hammer2_vfs_unmount(mp, MNT_FORCE);
		return EINVAL;
	}

	kprintf("hammer2_mount: hmp=%p pmp=%p\n", hmp, pmp);

	if (pmp->mp) {
		kprintf("hammer2_mount: PFS already mounted!\n");
		hammer2_unmount_helper(mp, NULL, hmp);
		lockmgr(&hammer2_mntlk, LK_RELEASE);
		hammer2_vfs_unmount(mp, MNT_FORCE);
		return EBUSY;
	}

	pmp->ronly = ronly;
	pmp->hflags = info.hflags;
	mp->mnt_flag |= MNT_LOCAL;
	mp->mnt_kern_flag |= MNTK_ALL_MPSAFE;
	mp->mnt_kern_flag |= MNTK_THR_SYNC;

	mp->mnt_stat.f_iosize = HAMMER2_PBUFSIZE;
	mp->mnt_stat.f_bsize = HAMMER2_PBUFSIZE;

	mp->mnt_vstat.f_frsize = HAMMER2_PBUFSIZE;
	mp->mnt_vstat.f_bsize = HAMMER2_PBUFSIZE;

	mp->mnt_iosize_max = MAXPHYS;

	hammer2_mount_helper(mp, pmp);
	lockmgr(&hammer2_mntlk, LK_RELEASE);

	vfs_getnewfsid(mp);
	vfs_add_vnodeops(mp, &hammer2_vnode_vops, &mp->mnt_vn_norm_ops);
	vfs_add_vnodeops(mp, &hammer2_spec_vops, &mp->mnt_vn_spec_ops);
	vfs_add_vnodeops(mp, &hammer2_fifo_vops, &mp->mnt_vn_fifo_ops);

	if (path) {
		copyinstr(info.volume, mp->mnt_stat.f_mntfromname,
			  MNAMELEN - 1, &size);
		bzero(mp->mnt_stat.f_mntfromname + size, MNAMELEN - size);
	}

	bzero(mp->mnt_stat.f_mntonname, sizeof(mp->mnt_stat.f_mntonname));
	if (path) {
		copyinstr(path, mp->mnt_stat.f_mntonname,
			  sizeof(mp->mnt_stat.f_mntonname) - 1,
			  &size);
	} else {
		mp->mnt_stat.f_mntonname[0] = '/';
	}

	hammer2_vfs_statfs(mp, &mp->mnt_stat, cred);

	return 0;
}

static
void
hammer2_update_pmps(hammer2_dev_t *hmp)
{
	const hammer2_inode_data_t *ripdata;
	hammer2_chain_t *parent;
	hammer2_chain_t *chain;
	hammer2_dev_t *force_local;
	hammer2_pfs_t *spmp;
	hammer2_key_t key_next;
	int error;

	force_local = (hmp->hflags & HMNT2_LOCAL) ? hmp : NULL;

	spmp = hmp->spmp;
	hammer2_inode_lock(spmp->iroot, 0);
	parent = hammer2_inode_chain(spmp->iroot, 0, HAMMER2_RESOLVE_ALWAYS);
	chain = hammer2_chain_lookup(&parent, &key_next,
					 HAMMER2_KEY_MIN, HAMMER2_KEY_MAX,
					 &error, 0);
	while (chain) {
		if (chain->error) {
			kprintf("I/O error scanning PFS labels\n");
		} else if (chain->bref.type != HAMMER2_BREF_TYPE_INODE) {
			kprintf("Non inode chain type %d under super-root\n",
				chain->bref.type);
		} else {
			ripdata = &chain->data->ipdata;
			hammer2_pfsalloc(chain, ripdata, force_local);
		}
		chain = hammer2_chain_next(&parent, chain, &key_next,
					   key_next, HAMMER2_KEY_MAX,
					   &error, 0);
	}
	if (parent) {
		hammer2_chain_unlock(parent);
		hammer2_chain_drop(parent);
	}
	hammer2_inode_unlock(spmp->iroot);
}

static
int
hammer2_remount(hammer2_dev_t *hmp, struct mount *mp, char *path __unused,
		struct ucred *cred)
{
	hammer2_pfs_t *pmp;
	hammer2_volume_t *vol;
	struct vnode *devvp;
	int i, ronly_save, error, result = 0;

	pmp = MPTOPMP(mp);
	ronly_save = pmp->ronly;
	if (pmp->ronly == 1 && (mp->mnt_kern_flag & MNTK_WANTRDWR))
		pmp->ronly = 0;
	else if (pmp->ronly == 0 && (mp->mnt_flag & MNT_RDONLY))
		pmp->ronly = 1;

	if (!(hmp->ronly && (mp->mnt_kern_flag & MNTK_WANTRDWR)))
		return 0;

	for (i = 0; i < hmp->nvolumes; ++i) {
		vol = &hmp->volumes[i];
		devvp = vol->dev->devvp;
		KKASSERT(devvp);
		vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
		VOP_OPEN(devvp, FREAD | FWRITE, FSCRED, NULL);
		vn_unlock(devvp);
		error = 0;
		if (vol->id == HAMMER2_ROOT_VOLUME) {
			error = hammer2_recovery(hmp);
			if (error == 0)
				error |= hammer2_fixup_pfses(hmp);
		}
		vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
		if (error == 0) {
			VOP_CLOSE(devvp, FREAD, NULL);
		} else {
			VOP_CLOSE(devvp, FREAD | FWRITE, NULL);
		}
		vn_unlock(devvp);
		result |= error;
	}
	if (result == 0) {
		kprintf("hammer2: enable read/write\n");
		hmp->ronly = 0;
		hmp->spmp->ronly = 0;
	} else {
		pmp->ronly = ronly_save;
	}

	return result;
}

static
int
hammer2_vfs_unmount(struct mount *mp, int mntflags)
{
	hammer2_pfs_t *pmp;
	int flags;
	int error = 0;

	pmp = MPTOPMP(mp);

	if (pmp == NULL)
		return(0);

	lockmgr(&hammer2_mntlk, LK_EXCLUSIVE);

	if (mntflags & MNT_FORCE)
		flags = FORCECLOSE;
	else
		flags = 0;
	if (pmp->iroot) {
		error = vflush(mp, 0, flags);
		if (error)
			goto failed;
		hammer2_vfs_sync(mp, MNT_WAIT);
		hammer2_vfs_sync(mp, MNT_WAIT);
		hammer2_vfs_sync(mp, MNT_WAIT);
	}

	hammer2_xop_helper_cleanup(pmp);

	if (pmp->mp)
		hammer2_unmount_helper(mp, pmp, NULL);

	error = 0;
failed:
	lockmgr(&hammer2_mntlk, LK_RELEASE);

	return (error);
}

static
void
hammer2_mount_helper(struct mount *mp, hammer2_pfs_t *pmp)
{
	hammer2_cluster_t *cluster;
	hammer2_chain_t *rchain;
	int i;

	mp->mnt_data = (qaddr_t)pmp;
	pmp->mp = mp;

	cluster = &pmp->iroot->cluster;
	for (i = 0; i < cluster->nchains; ++i) {
		rchain = cluster->array[i].chain;
		if (rchain == NULL)
			continue;
		++rchain->hmp->mount_count;
	}

	hammer2_xop_helper_create(pmp);
}

static
void
hammer2_unmount_helper(struct mount *mp, hammer2_pfs_t *pmp, hammer2_dev_t *hmp)
{
	hammer2_cluster_t *cluster;
	hammer2_chain_t *rchain;
	int i;

	if (pmp) {
		KKASSERT(hmp == NULL);
		KKASSERT(MPTOPMP(mp) == pmp);
		pmp->mp = NULL;
		mp->mnt_data = NULL;

		cluster = &pmp->iroot->cluster;
		for (i = 0; i < cluster->nchains; ++i) {
			rchain = cluster->array[i].chain;
			if (rchain == NULL)
				continue;
			--rchain->hmp->mount_count;
		}
again:
		TAILQ_FOREACH(hmp, &hammer2_mntlist, mntentry) {
			if (hmp->mount_count == 0) {
				hammer2_unmount_helper(NULL, NULL, hmp);
				goto again;
			}
		}
		return;
	}

	if (hmp->mount_count)
		return;

	hammer2_iocom_uninit(hmp);

	hammer2_bulkfree_uninit(hmp);
	hammer2_pfsfree_scan(hmp, 0);

#if 0
	hammer2_voldata_lock(hmp);
	hammer2_voldata_unlock(hmp);
#endif

	hammer2_chain_lock(&hmp->vchain, HAMMER2_RESOLVE_ALWAYS);
	hammer2_chain_lock(&hmp->fchain, HAMMER2_RESOLVE_ALWAYS);

	if (hmp->fchain.flags & HAMMER2_CHAIN_FLUSH_MASK) {
		hammer2_voldata_modify(hmp);
		hammer2_flush(&hmp->fchain, HAMMER2_FLUSH_TOP |
					    HAMMER2_FLUSH_ALL);
	}
	hammer2_chain_unlock(&hmp->fchain);

	if (hmp->vchain.flags & HAMMER2_CHAIN_FLUSH_MASK) {
		hammer2_flush(&hmp->vchain, HAMMER2_FLUSH_TOP |
					    HAMMER2_FLUSH_ALL);
	}
	hammer2_chain_unlock(&hmp->vchain);

	if ((hmp->vchain.flags | hmp->fchain.flags) &
	    HAMMER2_CHAIN_FLUSH_MASK) {
		kprintf("hammer2_unmount: chains left over after final sync\n");
		kprintf("    vchain %08x\n", hmp->vchain.flags);
		kprintf("    fchain %08x\n", hmp->fchain.flags);

		if (hammer2_debug & 0x0010)
			Debugger("entered debugger");
	}

	hammer2_pfsfree_scan(hmp, 1);

	KKASSERT(hmp->spmp == NULL);

	if (!TAILQ_EMPTY(&hmp->devvpl)) {
		hammer2_close_devvp(&hmp->devvpl, hmp->ronly);
		hammer2_cleanup_devvp(&hmp->devvpl);
	}
	KKASSERT(TAILQ_EMPTY(&hmp->devvpl));

	if (hmp->vchain.flags & HAMMER2_CHAIN_MODIFIED) {
		atomic_add_long(&hammer2_count_modified_chains, -1);
		atomic_clear_int(&hmp->vchain.flags, HAMMER2_CHAIN_MODIFIED);
		hammer2_pfs_memory_wakeup(hmp->vchain.pmp, -1);
	}
	if (hmp->vchain.flags & HAMMER2_CHAIN_UPDATE) {
		atomic_clear_int(&hmp->vchain.flags, HAMMER2_CHAIN_UPDATE);
	}

	if (hmp->fchain.flags & HAMMER2_CHAIN_MODIFIED) {
		atomic_add_long(&hammer2_count_modified_chains, -1);
		atomic_clear_int(&hmp->fchain.flags, HAMMER2_CHAIN_MODIFIED);
		hammer2_pfs_memory_wakeup(hmp->fchain.pmp, -1);
	}
	if (hmp->fchain.flags & HAMMER2_CHAIN_UPDATE) {
		atomic_clear_int(&hmp->fchain.flags, HAMMER2_CHAIN_UPDATE);
	}

#if 0
	{
		int dumpcnt;

		dumpcnt = 50;
		hammer2_dump_chain(&hmp->vchain, 0, 0, &dumpcnt, 'v', (u_int)-1);
		dumpcnt = 50;
		hammer2_dump_chain(&hmp->fchain, 0, 0, &dumpcnt, 'f', (u_int)-1);
	}
#endif

	hammer2_chain_drop(&hmp->fchain);

	hammer2_chain_drop(&hmp->vchain);

	hammer2_io_hash_cleanup_all(hmp);
	if (hmp->iofree_count) {
		kprintf("io_cleanup: %d I/O's left hanging\n",
			hmp->iofree_count);
	}

	TAILQ_REMOVE(&hammer2_mntlist, hmp, mntentry);
	kmalloc_destroy_obj(&hmp->mchain);
	kmalloc_destroy_obj(&hmp->mio);
	kmalloc_destroy(&hmp->mmsg);
	kfree(hmp, M_HAMMER2);
}

int
hammer2_vfs_vget(struct mount *mp, struct vnode *dvp,
		 ino_t ino, struct vnode **vpp)
{
	hammer2_xop_lookup_t *xop;
	hammer2_pfs_t *pmp;
	hammer2_inode_t *ip;
	hammer2_tid_t inum;
	int error;

	inum = (hammer2_tid_t)ino & HAMMER2_DIRHASH_USERMSK;

	error = 0;
	pmp = MPTOPMP(mp);

	ip = hammer2_inode_lookup(pmp, inum);
	if (ip) {
		hammer2_inode_lock(ip, HAMMER2_RESOLVE_SHARED);
		*vpp = hammer2_igetv(ip, &error);
		hammer2_inode_unlock(ip);
		hammer2_inode_drop(ip);

		return error;
	}

	xop = hammer2_xop_alloc(pmp->iroot, 0);
	xop->lhc = inum;
	hammer2_xop_start(&xop->head, &hammer2_lookup_desc);
	error = hammer2_xop_collect(&xop->head, 0);

	if (error == 0)
		ip = hammer2_inode_get(pmp, &xop->head, -1, -1);
	hammer2_xop_retire(&xop->head, HAMMER2_XOPMASK_VOP);

	if (ip) {
		*vpp = hammer2_igetv(ip, &error);
		hammer2_inode_unlock(ip);
	} else {
		*vpp = NULL;
		error = ENOENT;
	}
	return (error);
}

static
int
hammer2_vfs_root(struct mount *mp, struct vnode **vpp)
{
	hammer2_pfs_t *pmp;
	struct vnode *vp;
	int error;

	pmp = MPTOPMP(mp);
	if (pmp->iroot == NULL) {
		kprintf("hammer2 (%s): no root inode\n",
			mp->mnt_stat.f_mntfromname);
		*vpp = NULL;
		return EINVAL;
	}

	error = 0;
	hammer2_inode_lock(pmp->iroot, HAMMER2_RESOLVE_SHARED);

	while (pmp->inode_tid == 0) {
		hammer2_xop_ipcluster_t *xop;
		const hammer2_inode_meta_t *meta;

		xop = hammer2_xop_alloc(pmp->iroot, HAMMER2_XOP_MODIFYING);
		hammer2_xop_start(&xop->head, &hammer2_ipcluster_desc);
		error = hammer2_xop_collect(&xop->head, 0);

		if (error == 0) {
			meta = &hammer2_xop_gdata(&xop->head)->ipdata.meta;
			pmp->iroot->meta = *meta;
			pmp->inode_tid = meta->pfs_inum + 1;
			hammer2_xop_pdata(&xop->head);

			if (pmp->inode_tid < HAMMER2_INODE_START)
				pmp->inode_tid = HAMMER2_INODE_START;
			pmp->modify_tid =
				xop->head.cluster.focus->bref.modify_tid + 1;
#if 0
			kprintf("PFS: Starting inode %jd\n",
				(intmax_t)pmp->inode_tid);
			kprintf("PMP focus good set nextino=%ld mod=%016jx\n",
				pmp->inode_tid, pmp->modify_tid);
#endif
			wakeup(&pmp->iroot);

			hammer2_xop_retire(&xop->head, HAMMER2_XOPMASK_VOP);

			hammer2_vfs_statfs(mp, &mp->mnt_stat, NULL);
			break;
		}

		hammer2_xop_retire(&xop->head, HAMMER2_XOPMASK_VOP);
		hammer2_inode_unlock(pmp->iroot);
		error = tsleep(&pmp->iroot, PCATCH, "h2root", hz);
		hammer2_inode_lock(pmp->iroot, HAMMER2_RESOLVE_SHARED);
		if (error == EINTR)
			break;
	}

	if (error) {
		hammer2_inode_unlock(pmp->iroot);
		*vpp = NULL;
	} else {
		vp = hammer2_igetv(pmp->iroot, &error);
		hammer2_inode_unlock(pmp->iroot);
		*vpp = vp;
	}

	return (error);
}

static
int
hammer2_vfs_statfs(struct mount *mp, struct statfs *sbp, struct ucred *cred)
{
	hammer2_pfs_t *pmp;
	hammer2_dev_t *hmp;
	hammer2_blockref_t bref;
	struct statfs tmp;
	int i;

	pmp = MPTOPMP(mp);

	bzero(&tmp, sizeof(tmp));

	for (i = 0; i < pmp->iroot->cluster.nchains; ++i) {
		hmp = pmp->pfs_hmps[i];
		if (hmp == NULL)
			continue;
		if (pmp->iroot->cluster.array[i].chain)
			bref = pmp->iroot->cluster.array[i].chain->bref;
		else
			bzero(&bref, sizeof(bref));

		tmp.f_files = bref.embed.stats.inode_count;
		tmp.f_ffree = 0;
		tmp.f_blocks = hmp->voldata.allocator_size /
			       mp->mnt_vstat.f_bsize;
		tmp.f_bfree = hmp->voldata.allocator_free /
			      mp->mnt_vstat.f_bsize;
		tmp.f_bavail = tmp.f_bfree;

		if (cred && cred->cr_uid != 0) {
			uint64_t adj;

			adj = hmp->free_reserved / mp->mnt_vstat.f_bsize;
			tmp.f_blocks -= adj;
			tmp.f_bfree -= adj;
			tmp.f_bavail -= adj;
		}

		mp->mnt_stat.f_blocks = tmp.f_blocks;
		mp->mnt_stat.f_bfree = tmp.f_bfree;
		mp->mnt_stat.f_bavail = tmp.f_bavail;
		mp->mnt_stat.f_files = tmp.f_files;
		mp->mnt_stat.f_ffree = tmp.f_ffree;

		*sbp = mp->mnt_stat;
	}
	return (0);
}

static
int
hammer2_vfs_statvfs(struct mount *mp, struct statvfs *sbp, struct ucred *cred)
{
	hammer2_pfs_t *pmp;
	hammer2_dev_t *hmp;
	hammer2_blockref_t bref;
	struct statvfs tmp;
	int i;

	pmp = MPTOPMP(mp);
	bzero(&tmp, sizeof(tmp));

	for (i = 0; i < pmp->iroot->cluster.nchains; ++i) {
		hmp = pmp->pfs_hmps[i];
		if (hmp == NULL)
			continue;
		if (pmp->iroot->cluster.array[i].chain)
			bref = pmp->iroot->cluster.array[i].chain->bref;
		else
			bzero(&bref, sizeof(bref));

		tmp.f_files = bref.embed.stats.inode_count;
		tmp.f_ffree = 0;
		tmp.f_blocks = hmp->voldata.allocator_size /
			       mp->mnt_vstat.f_bsize;
		tmp.f_bfree = hmp->voldata.allocator_free /
			      mp->mnt_vstat.f_bsize;
		tmp.f_bavail = tmp.f_bfree;

		if (cred && cred->cr_uid != 0) {
			uint64_t adj;

			adj = hmp->free_reserved / mp->mnt_vstat.f_bsize;
			tmp.f_blocks -= adj;
			tmp.f_bfree -= adj;
			tmp.f_bavail -= adj;
		}

		mp->mnt_vstat.f_blocks = tmp.f_blocks;
		mp->mnt_vstat.f_bfree = tmp.f_bfree;
		mp->mnt_vstat.f_bavail = tmp.f_bavail;
		mp->mnt_vstat.f_files = tmp.f_files;
		mp->mnt_vstat.f_ffree = tmp.f_ffree;

		*sbp = mp->mnt_vstat;
	}
	return (0);
}

struct hammer2_recovery_elm {
	TAILQ_ENTRY(hammer2_recovery_elm) entry;
	hammer2_chain_t *chain;
	hammer2_tid_t sync_tid;
};

TAILQ_HEAD(hammer2_recovery_list, hammer2_recovery_elm);

struct hammer2_recovery_info {
	struct hammer2_recovery_list list;
	hammer2_tid_t	mtid;
	int	depth;
};

static int hammer2_recovery_scan(hammer2_dev_t *hmp,
			hammer2_chain_t *parent,
			struct hammer2_recovery_info *info,
			hammer2_tid_t sync_tid);

#define HAMMER2_RECOVERY_MAXDEPTH	10

static
int
hammer2_recovery(hammer2_dev_t *hmp)
{
	struct hammer2_recovery_info info;
	struct hammer2_recovery_elm *elm;
	hammer2_chain_t *parent;
	hammer2_tid_t sync_tid;
	hammer2_tid_t mirror_tid;
	int error;

	hammer2_trans_init(hmp->spmp, 0);

	sync_tid = hmp->voldata.freemap_tid;
	mirror_tid = hmp->voldata.mirror_tid;

	kprintf("hammer2_mount: \"%s\": ", hmp->devrepname);
	if (sync_tid >= mirror_tid) {
		kprintf("no recovery needed\n");
	} else {
		kprintf("freemap recovery %016jx-%016jx\n",
			sync_tid + 1, mirror_tid);
	}

	TAILQ_INIT(&info.list);
	info.depth = 0;
	parent = hammer2_chain_lookup_init(&hmp->vchain, 0);
	error = hammer2_recovery_scan(hmp, parent, &info, sync_tid);
	hammer2_chain_lookup_done(parent);

	while ((elm = TAILQ_FIRST(&info.list)) != NULL) {
		TAILQ_REMOVE(&info.list, elm, entry);
		parent = elm->chain;
		sync_tid = elm->sync_tid;
		kfree(elm, M_HAMMER2);

		hammer2_chain_lock(parent, HAMMER2_RESOLVE_ALWAYS);
		error |= hammer2_recovery_scan(hmp, parent, &info,
					      hmp->voldata.freemap_tid);
		hammer2_chain_unlock(parent);
		hammer2_chain_drop(parent);
	}

	hammer2_trans_done(hmp->spmp, 0);

	return error;
}

static
int
hammer2_recovery_scan(hammer2_dev_t *hmp, hammer2_chain_t *parent,
		      struct hammer2_recovery_info *info,
		      hammer2_tid_t sync_tid)
{
	const hammer2_inode_data_t *ripdata;
	hammer2_chain_t *chain;
	hammer2_blockref_t bref;
	int tmp_error;
	int rup_error;
	int error;
	int first;

	if (parent->bref.type != HAMMER2_BREF_TYPE_VOLUME) {
		hammer2_freemap_adjust(hmp, &parent->bref,
				       HAMMER2_FREEMAP_DORECOVER);
	}

	switch(parent->bref.type) {
	case HAMMER2_BREF_TYPE_VOLUME:
		break;
	case HAMMER2_BREF_TYPE_INODE:
		hammer2_chain_lock(parent, HAMMER2_RESOLVE_ALWAYS);
		ripdata = &parent->data->ipdata;
		if (ripdata->meta.op_flags & HAMMER2_OPFLAG_DIRECTDATA) {
			hammer2_chain_unlock(parent);
			return 0;
		}
		hammer2_chain_unlock(parent);
		break;
	case HAMMER2_BREF_TYPE_INDIRECT:
		hammer2_chain_lock(parent, HAMMER2_RESOLVE_ALWAYS);
		hammer2_chain_unlock(parent);
		break;
	case HAMMER2_BREF_TYPE_DIRENT:
	case HAMMER2_BREF_TYPE_DATA:
	case HAMMER2_BREF_TYPE_FREEMAP:
	case HAMMER2_BREF_TYPE_FREEMAP_NODE:
	case HAMMER2_BREF_TYPE_FREEMAP_LEAF:
		return 0;
		break;
	default:
		return HAMMER2_ERROR_BADBREF;
	}

	if (info->depth >= HAMMER2_RECOVERY_MAXDEPTH) {
		struct hammer2_recovery_elm *elm;

		elm = kmalloc(sizeof(*elm), M_HAMMER2, M_ZERO | M_WAITOK);
		elm->chain = parent;
		elm->sync_tid = sync_tid;
		hammer2_chain_ref(parent);
		TAILQ_INSERT_TAIL(&info->list, elm, entry);

		return(0);
	}

	chain = NULL;
	first = 1;
	rup_error = 0;
	error = 0;

	for (;;) {
		error |= hammer2_chain_scan(parent, &chain, &bref,
					    &first,
					    HAMMER2_LOOKUP_NODATA);

		if (error)
			break;

		if (chain == NULL) {
			if (bref.mirror_tid > sync_tid) {
				hammer2_freemap_adjust(hmp, &bref,
						     HAMMER2_FREEMAP_DORECOVER);
			}
			continue;
		}

		atomic_set_int(&chain->flags, HAMMER2_CHAIN_RELEASE);
		if (bref.mirror_tid > sync_tid) {
			++info->depth;
			tmp_error = hammer2_recovery_scan(hmp, chain,
							   info, sync_tid);
			--info->depth;
		} else {
			tmp_error = 0;
		}

		if (tmp_error == 0 &&
		    (bref.flags & HAMMER2_BREF_FLAG_PFSROOT) &&
		    (chain->flags & HAMMER2_CHAIN_ONFLUSH)) {
			hammer2_flush(chain, HAMMER2_FLUSH_TOP |
					     HAMMER2_FLUSH_ALL);
		}
		rup_error |= tmp_error;
	}
	return ((error | rup_error) & ~HAMMER2_ERROR_EOF);
}

static
int
hammer2_fixup_pfses(hammer2_dev_t *hmp)
{
	const hammer2_inode_data_t *ripdata;
	hammer2_chain_t *parent;
	hammer2_chain_t *chain;
	hammer2_key_t key_next;
	hammer2_pfs_t *spmp;
	int error;

	error = 0;

	spmp = hmp->spmp;
	hammer2_inode_lock(spmp->iroot, 0);
	parent = hammer2_inode_chain(spmp->iroot, 0, HAMMER2_RESOLVE_ALWAYS);
	chain = hammer2_chain_lookup(&parent, &key_next,
					 HAMMER2_KEY_MIN, HAMMER2_KEY_MAX,
					 &error, 0);
	while (chain) {
		if (chain->bref.type != HAMMER2_BREF_TYPE_INODE)
			continue;
		if (chain->error) {
			kprintf("I/O error scanning PFS labels\n");
			error |= chain->error;
		} else if ((chain->bref.flags &
			    HAMMER2_BREF_FLAG_PFSROOT) == 0) {
			int error2;

			ripdata = &chain->data->ipdata;
			hammer2_trans_init(hmp->spmp, 0);
			error2 = hammer2_chain_modify(chain,
						      chain->bref.modify_tid,
						      0, 0);
			if (error2 == 0) {
				kprintf("hammer2: Correct mis-flagged PFS %s\n",
					ripdata->filename);
				chain->bref.flags |= HAMMER2_BREF_FLAG_PFSROOT;
			} else {
				error |= error2;
			}
			hammer2_flush(chain, HAMMER2_FLUSH_TOP |
					     HAMMER2_FLUSH_ALL);
			hammer2_trans_done(hmp->spmp, 0);
		}
		chain = hammer2_chain_next(&parent, chain, &key_next,
					   key_next, HAMMER2_KEY_MAX,
					   &error, 0);
	}
	if (parent) {
		hammer2_chain_unlock(parent);
		hammer2_chain_drop(parent);
	}
	hammer2_inode_unlock(spmp->iroot);

	return error;
}

int
hammer2_vfs_sync(struct mount *mp, int waitfor)
{
	int error;

	error = hammer2_vfs_sync_pmp(MPTOPMP(mp), waitfor);

	return error;
}

int
hammer2_vfs_sync_pmp(hammer2_pfs_t *pmp, int waitfor)
{
	hammer2_inode_t *ip;
	hammer2_depend_t *depend;
	hammer2_depend_t *depend_next;
	struct vnode *vp;
	uint32_t pass2;
	int error;
	int wakecount;
	int dorestart;

	hammer2_trans_init(pmp, HAMMER2_TRANS_ISFLUSH);
#ifdef HAMMER2_DEBUG_SYNC
	kprintf("FILESYSTEM SYNC BOUNDARY\n");
#endif
	dorestart = 0;

restart:
#ifdef HAMMER2_DEBUG_SYNC
	kprintf("FILESYSTEM SYNC RESTART (%d)\n", dorestart);
#endif
	hammer2_trans_setflags(pmp, 0 );
	hammer2_trans_clearflags(pmp, HAMMER2_TRANS_RESCAN);

	hammer2_spin_ex(&pmp->list_spin);
	depend_next = TAILQ_FIRST(&pmp->depq);
	wakecount = 0;

	while ((depend = depend_next) != NULL) {
		depend_next = TAILQ_NEXT(depend, entry);
		if (dorestart && depend->pass2 == 0)
			continue;
		TAILQ_FOREACH(ip, &depend->sideq, entry) {
			KKASSERT(ip->flags & HAMMER2_INODE_SIDEQ);
			atomic_set_int(&ip->flags, HAMMER2_INODE_SYNCQ);
			atomic_clear_int(&ip->flags, HAMMER2_INODE_SIDEQ);
			ip->depend = NULL;
		}

		TAILQ_CONCAT(&pmp->syncq, &depend->sideq, entry);

		depend->count = 0;
		depend->pass2 = 0;
		TAILQ_REMOVE(&pmp->depq, depend, entry);
	}

	hammer2_spin_unex(&pmp->list_spin);
	hammer2_trans_clearflags(pmp,
				      HAMMER2_TRANS_WAITING);
	dorestart = 0;

	hammer2_pfs_memory_wakeup(pmp, 0);

	hammer2_spin_ex(&pmp->list_spin);
	while ((ip = TAILQ_FIRST(&pmp->syncq)) != NULL) {
		pass2 = ip->flags;
		cpu_ccfence();
		if (atomic_cmpset_int(&ip->flags,
			      pass2,
			      (pass2 & ~(HAMMER2_INODE_SYNCQ |
					 HAMMER2_INODE_SYNCQ_WAKEUP)) |
			      HAMMER2_INODE_SYNCQ_PASS2) == 0)
		{
			continue;
		}
		TAILQ_REMOVE(&pmp->syncq, ip, entry);
		--pmp->sideq_count;
		hammer2_spin_unex(&pmp->list_spin);

		if (pass2 & HAMMER2_INODE_SYNCQ_WAKEUP)
			wakeup(&ip->flags);
		if (++wakecount >= hammer2_limit_dirty_inodes / 20 + 1) {
			wakecount = 0;
			hammer2_pfs_memory_wakeup(pmp, 0);
		}

		hammer2_mtx_ex(&ip->lock);

		vp = ip->vp;
		if (vp) {
			if (vget(vp, LK_EXCLUSIVE|LK_NOWAIT)) {
				vp = NULL;
				dorestart |= 1;
#ifdef HAMMER2_DEBUG_SYNC
				kprintf("inum %ld (sync delayed by vnode)\n",
					(long)ip->meta.inum);
#endif
				hammer2_inode_delayed_sideq(ip);

				hammer2_mtx_unlock(&ip->lock);
				hammer2_inode_drop(ip);

				if (pass2 & HAMMER2_INODE_SYNCQ_PASS2)
					dorestart |= 2;
				hammer2_spin_ex(&pmp->list_spin);
				continue;
			}
		} else {
			vp = NULL;
		}

		if (ip->flags & HAMMER2_INODE_SIDEQ) {
			hammer2_mtx_unlock(&ip->lock);
			hammer2_inode_drop(ip);
			if (vp)
				vput(vp);
			dorestart |= 1;
			hammer2_spin_ex(&pmp->list_spin);
			continue;
		}

		if (vp) {
			vfsync(vp, MNT_WAIT, 1, NULL, NULL);
			bio_track_wait(&vp->v_track_write, 0, 0);
		}

		if (ip->flags & HAMMER2_INODE_DELETING) {
#ifdef HAMMER2_DEBUG_SYNC
			kprintf("inum %ld destroy\n", (long)ip->meta.inum);
#endif
			hammer2_inode_chain_des(ip);
			atomic_add_long(&hammer2_iod_inode_deletes, 1);
		} else if (ip->flags & HAMMER2_INODE_CREATING) {
#ifdef HAMMER2_DEBUG_SYNC
			kprintf("inum %ld insert\n", (long)ip->meta.inum);
#endif
			hammer2_inode_chain_ins(ip);
			atomic_add_long(&hammer2_iod_inode_creates, 1);
		}
#ifdef HAMMER2_DEBUG_SYNC
		kprintf("inum %ld chain-sync\n", (long)ip->meta.inum);
#endif

		hammer2_inode_chain_sync(ip);
		if (ip == pmp->iroot) {
			hammer2_inode_chain_flush(ip, HAMMER2_XOP_INODE_STOP);
		} else {
			hammer2_inode_chain_flush(ip, HAMMER2_XOP_INODE_STOP |
						      HAMMER2_XOP_FSSYNC);
		}
		if (vp) {
			lwkt_gettoken(&vp->v_token);
			if ((ip->flags & (HAMMER2_INODE_MODIFIED |
					  HAMMER2_INODE_RESIZED |
					  HAMMER2_INODE_DIRTYDATA)) == 0 &&
			    RB_EMPTY(&vp->v_rbdirty_tree) &&
			    !bio_track_active(&vp->v_track_write))
			{
				vclrisdirty(vp);
			} else {
				hammer2_inode_delayed_sideq(ip);
			}
			lwkt_reltoken(&vp->v_token);
			vput(vp);
			vp = NULL;
		}
		atomic_clear_int(&ip->flags, HAMMER2_INODE_SYNCQ_PASS2);
		hammer2_inode_unlock(ip);

		hammer2_spin_ex(&pmp->list_spin);
	}
	hammer2_spin_unex(&pmp->list_spin);
	hammer2_pfs_memory_wakeup(pmp, 0);

	if (dorestart || (pmp->trans.flags & HAMMER2_TRANS_RESCAN)) {
		if (dorestart & 2) {
			wakeup(&pmp->inmem_dirty_chains);
			tsleep(&dorestart, 0, "h2syndel", 2);
		}
#ifdef HAMMER2_DEBUG_SYNC
		kprintf("FILESYSTEM SYNC STAGE 1 RESTART\n");
#endif
		dorestart = 1;
		goto restart;
	}
#ifdef HAMMER2_DEBUG_SYNC
	kprintf("FILESYSTEM SYNC STAGE 2 BEGIN\n");
#endif

	if ((ip = pmp->iroot) != NULL) {
		hammer2_inode_ref(ip);
		hammer2_mtx_ex(&ip->lock);
		hammer2_inode_chain_sync(ip);
		hammer2_inode_chain_flush(ip, HAMMER2_XOP_INODE_STOP |
					      HAMMER2_XOP_FSSYNC |
					      HAMMER2_XOP_VOLHDR);
		hammer2_inode_unlock(ip);
	}
#ifdef HAMMER2_DEBUG_SYNC
	kprintf("FILESYSTEM SYNC STAGE 2 DONE\n");
#endif

	hammer2_bioq_sync(pmp);

	error = 0;
	hammer2_trans_done(pmp, HAMMER2_TRANS_ISFLUSH);

	return (error);
}

static
int
hammer2_vfs_vptofh(struct vnode *vp, struct fid *fhp)
{
	hammer2_inode_t *ip;

	KKASSERT(MAXFIDSZ >= 16);
	ip = VTOI(vp);
	fhp->fid_len = offsetof(struct fid, fid_data[16]);
	fhp->fid_ext = 0;
	((hammer2_tid_t *)fhp->fid_data)[0] = ip->meta.inum;
	((hammer2_tid_t *)fhp->fid_data)[1] = 0;

	return 0;
}

static
int
hammer2_vfs_fhtovp(struct mount *mp, struct vnode *rootvp,
	       struct fid *fhp, struct vnode **vpp)
{
	hammer2_tid_t inum;
	int error;

	inum = ((hammer2_tid_t *)fhp->fid_data)[0] & HAMMER2_DIRHASH_USERMSK;
	if (vpp) {
		if (inum == 1)
			error = hammer2_vfs_root(mp, vpp);
		else
			error = hammer2_vfs_vget(mp, NULL, inum, vpp);
	} else {
		error = 0;
	}
	return error;
}

static
int
hammer2_vfs_checkexp(struct mount *mp, struct sockaddr *nam,
		 int *exflagsp, struct ucred **credanonp)
{
	hammer2_pfs_t *pmp;
	struct netcred *np;
	int error;

	pmp = MPTOPMP(mp);
	np = vfs_export_lookup(mp, &pmp->export, nam);
	if (np) {
		*exflagsp = np->netc_exflags;
		*credanonp = &np->netc_anon;
		error = 0;
	} else {
		error = EACCES;
	}
	return error;
}

void
hammer2_lwinprog_ref(hammer2_pfs_t *pmp)
{
	atomic_add_int(&pmp->count_lwinprog, 1);
}

void
hammer2_lwinprog_drop(hammer2_pfs_t *pmp)
{
	int lwinprog;

	lwinprog = atomic_fetchadd_int(&pmp->count_lwinprog, -1);
	if ((lwinprog & HAMMER2_LWINPROG_WAITING) &&
	    (lwinprog & HAMMER2_LWINPROG_MASK) <= hammer2_flush_pipe * 2 / 3) {
		atomic_clear_int(&pmp->count_lwinprog,
				 HAMMER2_LWINPROG_WAITING);
		wakeup(&pmp->count_lwinprog);
	}
	if ((lwinprog & HAMMER2_LWINPROG_WAITING0) &&
	    (lwinprog & HAMMER2_LWINPROG_MASK) <= 0) {
		atomic_clear_int(&pmp->count_lwinprog,
				 HAMMER2_LWINPROG_WAITING0);
		wakeup(&pmp->count_lwinprog);
	}
}

void
hammer2_lwinprog_wait(hammer2_pfs_t *pmp, int flush_pipe)
{
	int lwinprog;
	int lwflag = (flush_pipe) ? HAMMER2_LWINPROG_WAITING :
				    HAMMER2_LWINPROG_WAITING0;

	for (;;) {
		lwinprog = pmp->count_lwinprog;
		cpu_ccfence();
		if ((lwinprog & HAMMER2_LWINPROG_MASK) <= flush_pipe)
			break;
		tsleep_interlock(&pmp->count_lwinprog, 0);
		atomic_set_int(&pmp->count_lwinprog, lwflag);
		lwinprog = pmp->count_lwinprog;
		if ((lwinprog & HAMMER2_LWINPROG_MASK) <= flush_pipe)
			break;
		tsleep(&pmp->count_lwinprog, PINTERLOCKED, "h2wpipe", hz);
	}
}

static int
hammer2_vfs_modifying(struct mount *mp)
{
	if (mp->mnt_flag & MNT_RDONLY)
		return EROFS;
	hammer2_pfs_memory_wait(MPTOPMP(mp));

	return 0;
}

void
hammer2_pfs_memory_wait(hammer2_pfs_t *pmp)
{
	uint32_t waiting;
	int pcatch;
	int error;
	int started;

	if (pmp == NULL || pmp->mp == NULL)
		return;

	started = 0;

	for (;;) {
		waiting = pmp->inmem_dirty_chains & HAMMER2_DIRTYCHAIN_MASK;
		cpu_ccfence();

		if (waiting > hammer2_limit_dirty_chains / 2 ||
		    pmp->sideq_count > hammer2_limit_dirty_inodes / 2)
		{
			trigger_syncer(pmp->mp);
		}

		if (waiting < hammer2_limit_dirty_chains &&
		    pmp->sideq_count < hammer2_limit_dirty_inodes)
		{
			break;
		}

		if (started == 0) {
			trigger_syncer_start(pmp->mp);
			started = 1;
		}

		pcatch = curthread->td_proc ? PCATCH : 0;
		tsleep_interlock(&pmp->inmem_dirty_chains, pcatch);

		atomic_set_int(&pmp->inmem_dirty_chains,
			       HAMMER2_DIRTYCHAIN_WAITING);

		if (waiting < hammer2_limit_dirty_chains &&
		    pmp->sideq_count < hammer2_limit_dirty_inodes) {
			break;
		}
		error = tsleep(&pmp->inmem_dirty_chains,
			       PINTERLOCKED | pcatch,
			       "h2memw", hz);
		if (error == ERESTART)
			break;
	}
	if (started)
		trigger_syncer_stop(pmp->mp);
}

void
hammer2_pfs_memory_wakeup(hammer2_pfs_t *pmp, int count)
{
	uint32_t waiting;

	if (pmp) {
		waiting = atomic_fetchadd_int(&pmp->inmem_dirty_chains, count);

		if ((waiting & HAMMER2_DIRTYCHAIN_WAITING) &&
		    (pmp->inmem_dirty_chains & HAMMER2_DIRTYCHAIN_MASK) <=
		    hammer2_limit_dirty_chains * 2 / 3 &&
		    pmp->sideq_count <= hammer2_limit_dirty_inodes * 2 / 3) {
			atomic_clear_int(&pmp->inmem_dirty_chains,
					 HAMMER2_DIRTYCHAIN_WAITING);
			wakeup(&pmp->inmem_dirty_chains);
		}
	}
}

void
hammer2_pfs_memory_inc(hammer2_pfs_t *pmp)
{
	if (pmp) {
		atomic_add_int(&pmp->inmem_dirty_chains, 1);
	}
}

void
hammer2_voldata_lock(hammer2_dev_t *hmp)
{
	lockmgr(&hmp->vollk, LK_EXCLUSIVE);
}

void
hammer2_voldata_unlock(hammer2_dev_t *hmp)
{
	lockmgr(&hmp->vollk, LK_RELEASE);
}

void
hammer2_voldata_modify(hammer2_dev_t *hmp)
{
	if ((hmp->vchain.flags & HAMMER2_CHAIN_MODIFIED) == 0) {
		atomic_add_long(&hammer2_count_modified_chains, 1);
		atomic_set_int(&hmp->vchain.flags, HAMMER2_CHAIN_MODIFIED);
		hammer2_pfs_memory_inc(hmp->vchain.pmp);
		hmp->vchain.bref.mirror_tid = hmp->voldata.mirror_tid + 1;
	}
}

int
hammer2_vfs_enospace(hammer2_inode_t *ip, off_t bytes, struct ucred *cred)
{
	hammer2_pfs_t *pmp;
	hammer2_dev_t *hmp;
	hammer2_off_t free_reserved;
	hammer2_off_t free_nominal;
	int i;

	pmp = ip->pmp;

	if (pmp->free_ticks == 0 || pmp->free_ticks != ticks) {
		free_reserved = HAMMER2_SEGSIZE;
		free_nominal = 0x7FFFFFFFFFFFFFFFLLU;
		for (i = 0; i < pmp->iroot->cluster.nchains; ++i) {
			hmp = pmp->pfs_hmps[i];
			if (hmp == NULL)
				continue;
			if (pmp->pfs_types[i] != HAMMER2_PFSTYPE_MASTER &&
			    pmp->pfs_types[i] != HAMMER2_PFSTYPE_SOFT_MASTER)
				continue;

			if (free_nominal > hmp->voldata.allocator_free)
				free_nominal = hmp->voldata.allocator_free;
			if (free_reserved < hmp->free_reserved)
				free_reserved = hmp->free_reserved;
		}

		pmp->free_reserved = free_reserved;
		pmp->free_nominal = free_nominal;
		pmp->free_ticks = ticks;
	} else {
		free_reserved = pmp->free_reserved;
		free_nominal = pmp->free_nominal;
	}
	if (cred && cred->cr_uid != 0) {
		if ((int64_t)(free_nominal - bytes) <
		    (int64_t)free_reserved) {
			return 2;
		}
	} else {
		if ((int64_t)(free_nominal - bytes) <
		    (int64_t)free_reserved / 2) {
			return 2;
		}
	}
	if ((int64_t)(free_nominal - bytes) < (int64_t)free_reserved * 2)
		return 1;
	return 0;
}
