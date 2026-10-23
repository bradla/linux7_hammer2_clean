#include "hammer2.h"
#include "hammer2_mount.h"
#include <linux/fs.h>
#include <linux/statfs.h>
#include <linux/seq_file.h>
#include <linux/parser.h>
#include <linux/blkdev.h>
#include <linux/sched.h>
#include <linux/sched/debug.h>

int hammer2_unmount(struct mount *, int);
static int hammer2_recovery(hammer2_dev_t *);
static int hammer2_fixup_pfses(hammer2_dev_t *);
static int hammer2_remount_impl(hammer2_dev_t *);
static int hammer2_remount(hammer2_dev_t *, struct mount *);
int hammer2_statfs(struct mount *, struct h2statfs *);
static void hammer2_update_pmps(hammer2_dev_t *);
static void hammer2_mount_helper(struct mount *, hammer2_pfs_t *);
static void hammer2_unmount_helper(struct mount *, hammer2_pfs_t *,
    hammer2_dev_t *);

MALLOC_DEFINE(M_HAMMER2, "hammer2_mount", "HAMMER2 mount structure");
uma_zone_t hammer2_zone_inode;
uma_zone_t hammer2_zone_xops;
uma_zone_t hammer2_zone_rbuf;
uma_zone_t hammer2_zone_wbuf;

TAILQ_HEAD(hammer2_mntlist, hammer2_dev);
typedef struct hammer2_mntlist hammer2_mntlist_t;
static hammer2_mntlist_t hammer2_mntlist;

hammer2_pfslist_t hammer2_pfslist;
static hammer2_pfslist_t hammer2_spmplist;

hammer2_lk_t hammer2_mntlk;

static struct task_struct *hammer2_mntlk_owner;
static const char *hammer2_mntlk_site = "none";

#define hammer2_mntlk_ex()	hammer2_mntlk_ex_at(__func__)
#define hammer2_mntlk_unlock()	hammer2_mntlk_unlock_at()

static void
hammer2_mntlk_ex_at(const char *site)
{
	unsigned long t0 = getticks();
	struct task_struct *owner;
	int reported = 0;

	while (!down_write_trylock(&hammer2_mntlk)) {
		schedule_timeout_uninterruptible(HZ / 10);
		if (reported || time_before(getticks(), t0 + 30 * HZ))
			continue;
		reported = 1;
		owner = READ_ONCE(hammer2_mntlk_owner);
		if (owner == current) {
			hprintf("mntlk: SELF-DEADLOCK in %s -- this task already "
			    "holds the lock, taken in %s\n", site,
			    READ_ONCE(hammer2_mntlk_site));
		} else if (owner) {
			hprintf("mntlk: %s blocked >30s; holder pid %d (%s) "
			    "took it in %s, state 0x%x\n", site,
			    owner->pid, owner->comm,
			    READ_ONCE(hammer2_mntlk_site),
			    (unsigned int)READ_ONCE(owner->__state));
			sched_show_task(owner);
		} else {
			hprintf("mntlk: %s blocked >30s with NO recorded holder "
			    "(lock leaked by an unbalanced release?)\n", site);
		}
	}
	WRITE_ONCE(hammer2_mntlk_owner, current);
	WRITE_ONCE(hammer2_mntlk_site, site);
}

static void
hammer2_mntlk_unlock_at(void)
{
	WRITE_ONCE(hammer2_mntlk_owner, NULL);
	WRITE_ONCE(hammer2_mntlk_site, "none");
	hammer2_lk_unlock(&hammer2_mntlk);
}

static int hammer2_supported_version = HAMMER2_VOL_VERSION_DEFAULT;

module_param(hammer2_supported_version, int, 0444);
MODULE_PARM_DESC(hammer2_supported_version,
	"Highest HAMMER2 volume version supported by this module (read-only)");
int hammer2_cluster_meta_read = 1;
int hammer2_cluster_data_read = 4;
int hammer2_cluster_write;
int hammer2_fsync_durable = 1;

module_param(hammer2_fsync_durable, int, 0644);
MODULE_PARM_DESC(hammer2_fsync_durable,
	"fsync() flushes topology+volume header+device (1=durable, 0=fast but loses data on power failure)");

int hammer2_dedup_enable = 1;

module_param(hammer2_dedup_enable, int, 0644);
MODULE_PARM_DESC(hammer2_dedup_enable,
	"Enable block deduplication on write (1=on, 0=off)");
int hammer2_count_inode_allocated;
int hammer2_count_chain_allocated;
int hammer2_count_chain_modified;
int hammer2_count_dio_allocated;
long hammer2_limit_dirty_chains;
module_param(hammer2_limit_dirty_chains, long, 0644);
MODULE_PARM_DESC(hammer2_limit_dirty_chains,
	"Dirty chains allowed before a writer is throttled (0 = auto at mount)");
int hammer2_dio_limit = 256;
module_param(hammer2_dio_limit, int, 0644);
MODULE_PARM_DESC(hammer2_dio_limit,
	"Cached 64KiB device buffers to keep for reuse (memory ~= limit * 64KiB)");

int hammer2_readdir_bound_min = HAMMER2_READDIR_BOUND_MIN;
module_param(hammer2_readdir_bound_min, int, 0644);
MODULE_PARM_DESC(hammer2_readdir_bound_min,
	"readdir: smallest directory scan, in entries (first call on an open)");

int hammer2_readdir_bound_max = HAMMER2_READDIR_BOUND_MAX;
module_param(hammer2_readdir_bound_max, int, 0644);
MODULE_PARM_DESC(hammer2_readdir_bound_max,
	"readdir: largest single scan round, in entries (peak pinned memory ~= value * 404 bytes)");

int hammer2_readdir_slop = HAMMER2_READDIR_SLOP;
module_param(hammer2_readdir_slop, int, 0644);
MODULE_PARM_DESC(hammer2_readdir_slop,
	"readdir: entries to over-scan past the previous call, so one round both fills the buffer and detects it is full");

int hammer2_readdir_bref;
module_param(hammer2_readdir_bref, int, 0644);
MODULE_PARM_DESC(hammer2_readdir_bref,
	"readdir: 1 = scan blockrefs without instantiating a chain per entry (default 0)");
int hammer2_bulkfree_tps = 5000;
int hammer2_limit_scan_depth;
int hammer2_limit_saved_chains;
int hammer2_always_compress;

int malloc_leak_m_hammer2;
int malloc_leak_m_hammer2_lz4;
int malloc_leak_m_temp;

SYSCTL_NODE(_vfs, OID_AUTO, hammer2, CTLFLAG_RW, 0, "HAMMER2 filesystem");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, supported_version, CTLFLAG_RD,
    &hammer2_supported_version, 0, "Highest supported HAMMER2 version");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, cluster_meta_read, CTLFLAG_RW,
    &hammer2_cluster_meta_read, 0, "Cluster read count for meta data");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, cluster_data_read, CTLFLAG_RW,
    &hammer2_cluster_data_read, 0, "Cluster read count for user data");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, cluster_write, CTLFLAG_RW,
    &hammer2_cluster_write, 0, "Cluster write count for device vnode");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, dedup_enable, CTLFLAG_RW,
    &hammer2_dedup_enable, 0, "Enable dedup");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, inode_allocated, CTLFLAG_RD,
    &hammer2_count_inode_allocated, 0, "Number of inode allocated");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, chain_allocated, CTLFLAG_RD,
    &hammer2_count_chain_allocated, 0, "Number of chain allocated");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, chain_modified, CTLFLAG_RD,
    &hammer2_count_chain_modified, 0, "Number of chain modified");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, dio_allocated, CTLFLAG_RD,
    &hammer2_count_dio_allocated, 0, "Number of dio allocated");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, dio_limit, CTLFLAG_RW,
    &hammer2_dio_limit, 0, "Number of dio to keep for reuse");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, bulkfree_tps, CTLFLAG_RW,
    &hammer2_bulkfree_tps, 0, "Bulkfree maximum scan rate");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, limit_scan_depth, CTLFLAG_RW,
    &hammer2_limit_scan_depth, 0, "Bulkfree scan depth limit");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, limit_saved_chains, CTLFLAG_RW,
    &hammer2_limit_saved_chains, 0, "Bulkfree saved chains limit");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, always_compress, CTLFLAG_RW,
    &hammer2_always_compress, 0, "Always try to compress write buffer");

static const char *hammer2_opts[] = {
	"async", "export", "force", "from", "noatime", "noexec", "noclusterr",
	"noclusterw", "nosuid", "nosymfollow", "suiddir", "sync", "hflags",
	NULL,
};

static int
hammer2_assert_clean(void)
{
	int error = 0;

	if (hammer2_count_inode_allocated > 0) {
		hprintf("%d inode left\n", hammer2_count_inode_allocated);
		error = EINVAL;
	}
	if (hammer2_count_chain_allocated > 0) {
		hprintf("%d chain left\n", hammer2_count_chain_allocated);
		error = EINVAL;
	}
	if (hammer2_count_chain_modified > 0) {
		hprintf("%d modified chain left\n",
		    hammer2_count_chain_modified);
		error = EINVAL;
	}
	if (hammer2_count_dio_allocated > 0) {
		hprintf("%d dio left\n", hammer2_count_dio_allocated);
		error = EINVAL;
	}

	return (error);
}

int
hammer2_init(struct vfsconf *vfsp)
{
	hammer2_assert_clean();

	hammer2_dio_limit = nbuf * 2;
	if (hammer2_dio_limit > 100000 || hammer2_dio_limit < 0)
		hammer2_dio_limit = 100000;

	KKASSERT(hammer2_zone_inode == NULL);
	hammer2_zone_inode = uma_zcreate("h2inozone", sizeof(hammer2_inode_t),
	    NULL, NULL, NULL, NULL, UMA_ALIGN_PTR, 0);
	KKASSERT(hammer2_zone_inode);

	KKASSERT(hammer2_zone_xops == NULL);
	hammer2_zone_xops = uma_zcreate("h2xopszone", sizeof(hammer2_xop_t),
	    NULL, NULL, NULL, NULL, UMA_ALIGN_PTR, 0);
	KKASSERT(hammer2_zone_xops);

	KKASSERT(hammer2_zone_rbuf == NULL);
	hammer2_zone_rbuf = uma_zcreate("h2rbufzone", HAMMER2_PBUFSIZE,
	    NULL, NULL, NULL, NULL, UMA_ALIGN_PTR, 0);
	KKASSERT(hammer2_zone_rbuf);

	KKASSERT(hammer2_zone_wbuf == NULL);
	hammer2_zone_wbuf = uma_zcreate("h2wbufzone", HAMMER2_PBUFSIZE / 2,
	    NULL, NULL, NULL, NULL, UMA_ALIGN_PTR, 0);
	KKASSERT(hammer2_zone_wbuf);

	hammer2_lk_init(&hammer2_mntlk, "h2_mnt");

	TAILQ_INIT(&hammer2_mntlist);
	TAILQ_INIT(&hammer2_pfslist);
	TAILQ_INIT(&hammer2_spmplist);

	hammer2_limit_dirty_chains = desiredvnodes / 10;
	if (hammer2_limit_dirty_chains > HAMMER2_LIMIT_DIRTY_CHAINS)
		hammer2_limit_dirty_chains = HAMMER2_LIMIT_DIRTY_CHAINS;
	if (hammer2_limit_dirty_chains < 1000)
		hammer2_limit_dirty_chains = 1000;
	hammer2_limit_saved_chains = hammer2_limit_dirty_chains * 5;

	return (0);
}

int
hammer2_uninit(struct vfsconf *vfsp)
{
	hammer2_lk_destroy(&hammer2_mntlk);

	if (hammer2_zone_inode) {
		uma_zdestroy(hammer2_zone_inode);
		hammer2_zone_inode = NULL;
	}
	if (hammer2_zone_xops) {
		uma_zdestroy(hammer2_zone_xops);
		hammer2_zone_xops = NULL;
	}
	if (hammer2_zone_rbuf) {
		uma_zdestroy(hammer2_zone_rbuf);
		hammer2_zone_rbuf = NULL;
	}
	if (hammer2_zone_wbuf) {
		uma_zdestroy(hammer2_zone_wbuf);
		hammer2_zone_wbuf = NULL;
	}

	hammer2_assert_clean();

	KKASSERT(TAILQ_EMPTY(&hammer2_mntlist));
	KKASSERT(TAILQ_EMPTY(&hammer2_pfslist));
	KKASSERT(TAILQ_EMPTY(&hammer2_spmplist));

	return (0);
}

void
hammer2_throttle_workfn(struct work_struct *work)
{
	hammer2_pfs_t *pmp = container_of(work, hammer2_pfs_t, throttle_work);

	hammer2_vfs_sync_pmp(pmp, MNT_WAIT);
	wake_up_all(&pmp->throttle_wq);
}

hammer2_pfs_t *
hammer2_pfsalloc(hammer2_chain_t *chain, const hammer2_inode_data_t *ripdata,
    hammer2_dev_t *force_local)
{
	hammer2_pfs_t *pmp = NULL;
	hammer2_inode_t *iroot;
	int i, j;

	if (ripdata) {
		TAILQ_FOREACH(pmp, &hammer2_pfslist, mntentry) {
			if (force_local != pmp->force_local)
				continue;
			if (force_local == NULL &&
			    bcmp(&pmp->pfs_clid, &ripdata->meta.pfs_clid,
			    sizeof(pmp->pfs_clid)) == 0)
				break;
			else if (force_local && pmp->pfs_names[0] &&
			    strcmp(pmp->pfs_names[0],
			    (const char *)ripdata->filename) == 0)
				break;
		}
	}

	if (pmp == NULL) {
		pmp = hmalloc(sizeof(*pmp), M_HAMMER2, M_WAITOK | M_ZERO);

		INIT_WORK(&pmp->throttle_work, hammer2_throttle_workfn);
		init_waitqueue_head(&pmp->throttle_wq);
		pmp->force_local = force_local;
		hammer2_trans_manage_init(pmp);
		hammer2_spin_init(&pmp->blockset_spin, "h2mp_bset");
		hammer2_spin_init(&pmp->list_spin, "h2mp_list");
		for (i = 0; i < HAMMER2_IHASH_SIZE; i++) {
			hammer2_lk_init(&pmp->xop_lock[i], "h2mp_xop");
			hammer2_lkc_init(&pmp->xop_cv[i], "h2mp_xop_cv");
		}
		hammer2_lk_init(&pmp->trans_lock, "h2mp_tx");
		hammer2_lkc_init(&pmp->trans_cv, "h2mp_tx_cv");
		TAILQ_INIT(&pmp->syncq);
		TAILQ_INIT(&pmp->depq);
		hammer2_inum_hash_init(pmp);

		KKASSERT((HAMMER2_IHASH_SIZE & (HAMMER2_IHASH_SIZE - 1)) == 0);
		pmp->ipdep_lists = (hammer2_ipdep_list_t *)hashinit(
		    HAMMER2_IHASH_SIZE, M_HAMMER2, &pmp->ipdep_mask);
		KKASSERT(HAMMER2_IHASH_SIZE == pmp->ipdep_mask + 1);

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
		hprintf("cluster full\n");

	} else {
		KKASSERT(chain->pmp == NULL);
		chain->pmp = pmp;
		hammer2_chain_ref(chain);
		iroot->cluster.array[j].chain = chain;
		if (force_local)
			pmp->pfs_types[j] = HAMMER2_PFSTYPE_MASTER;
		else
			pmp->pfs_types[j] = ripdata->meta.pfs_type;
		pmp->pfs_names[j] = hstrdup((const char *)ripdata->filename);
		pmp->pfs_hmps[j] = chain->hmp;
		hammer2_spin_ex(&pmp->blockset_spin);
		pmp->pfs_iroot_blocksets[j] = chain->data->ipdata.u.blockset;
		hammer2_spin_unex(&pmp->blockset_spin);

		if (pmp->mp)
			++chain->hmp->mount_count;
		++j;
	}
	iroot->cluster.nchains = j;
	hammer2_assert_cluster(&iroot->cluster);

	hammer2_mtx_unlock(&iroot->lock);
	hammer2_inode_drop(iroot);
done:
	return (pmp);
}

void
hammer2_pfsdealloc(hammer2_pfs_t *pmp, int clindex, int destroying __unused)
{
	hammer2_inode_t *iroot;
	hammer2_chain_t *chain;

	iroot = pmp->iroot;
	if (iroot) {

		hammer2_mtx_ex(&iroot->lock);
		chain = iroot->cluster.array[clindex].chain;
		iroot->cluster.array[clindex].chain = NULL;
		pmp->pfs_types[clindex] = HAMMER2_PFSTYPE_NONE;
		hammer2_mtx_unlock(&iroot->lock);

		if (chain) {
			atomic_set_int(&chain->flags, HAMMER2_CHAIN_RELEASE);
			hammer2_chain_drop(chain);
		}
	}
}

static void
hammer2_pfsfree(hammer2_pfs_t *pmp)
{
	hammer2_inode_t *iroot;
	hammer2_chain_t *chain;
	int i, chains_still_present = 0;

	KKASSERT(!(pmp->flags & HAMMER2_PMPF_WAITING));

	if (pmp->flags & HAMMER2_PMPF_SPMP)
		TAILQ_REMOVE(&hammer2_spmplist, pmp, mntentry);
	else
		TAILQ_REMOVE(&hammer2_pfslist, pmp, mntentry);

	iroot = pmp->iroot;
	if (iroot) {
		for (i = 0; i < iroot->cluster.nchains; ++i) {
			chain = iroot->cluster.array[i].chain;
			if (chain && !RB_EMPTY(&chain->core.rbtree))
				chains_still_present = 1;
		}
		KASSERTMSG(iroot->refs == 1,
		    "iroot inum %016llx refs %d not 1",
		    (long long)iroot->meta.inum, iroot->refs);
		hammer2_inode_drop(iroot);
		pmp->iroot = NULL;
	}

	if (chains_still_present) {
		KKASSERT(pmp->mp);
		hprintf("PFS still in use\n");
	} else {
		hammer2_spin_destroy(&pmp->blockset_spin);
		hammer2_spin_destroy(&pmp->list_spin);
		for (i = 0; i < HAMMER2_IHASH_SIZE; i++) {
			hammer2_lk_destroy(&pmp->xop_lock[i]);
			hammer2_lkc_destroy(&pmp->xop_cv[i]);
		}
		hammer2_lk_destroy(&pmp->trans_lock);
		hammer2_lkc_destroy(&pmp->trans_cv);
		hammer2_inum_hash_destroy(pmp);
		hashdestroy(pmp->ipdep_lists, M_HAMMER2, pmp->ipdep_mask);

		cancel_work_sync(&pmp->throttle_work);

		hfree(pmp, M_HAMMER2, sizeof(*pmp));
	}
}

static void
hammer2_pfsfree_scan(hammer2_dev_t *hmp, int which)
{
	hammer2_pfs_t *pmp;
	hammer2_inode_t *iroot;
	hammer2_chain_t *rchain;
	struct hammer2_pfslist *wlist;
	int i;

	if (which == 0)
		wlist = &hammer2_pfslist;
	else
		wlist = &hammer2_spmplist;
again:
	TAILQ_FOREACH(pmp, wlist, mntentry) {
		if ((iroot = pmp->iroot) == NULL)
			continue;

		for (i = 0; i < HAMMER2_MAXCLUSTER; ++i)
			if (pmp->pfs_hmps[i] == hmp)
				break;
		if (i == HAMMER2_MAXCLUSTER)
			continue;

		hammer2_vfs_sync_pmp(pmp, MNT_WAIT);

		hammer2_mtx_ex(&iroot->lock);

		for (i = 0; i < HAMMER2_MAXCLUSTER; ++i) {
			if (pmp->pfs_hmps[i] != hmp)
				continue;
			rchain = iroot->cluster.array[i].chain;
			iroot->cluster.array[i].chain = NULL;
			pmp->pfs_types[i] = HAMMER2_PFSTYPE_NONE;
			if (pmp->pfs_names[i]) {
				hstrfree(pmp->pfs_names[i]);
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

		for (i = HAMMER2_MAXCLUSTER - 1; i >= 0; --i)
			if (pmp->pfs_hmps[i])
				break;
		iroot->cluster.nchains = i + 1;

		if (iroot->cluster.nchains == 0) {

			if (hmp->spmp == pmp) {
				hmp->spmp = NULL;
				hmp->vchain.pmp = NULL;
				hmp->fchain.pmp = NULL;
			}

			hammer2_pfsfree(pmp);
			goto again;
		}
	}
}

int
hammer2_mount(struct mount *mp)
{
	struct vfsoptlist *opts = mp->mnt_optnew;
	hammer2_dev_t *hmp = NULL, *hmp_tmp, *force_local;
	hammer2_pfs_t *pmp = NULL, *spmp;
	hammer2_key_t key_next, key_dummy, lhc;
	hammer2_chain_t *chain, *parent;
	const hammer2_inode_data_t *ripdata;
	hammer2_devvp_list_t devvpl;
	hammer2_devvp_t *e, *e_tmp;
	hammer2_chain_t *schain;
	hammer2_xop_head_t *xop;
	hammer2_cluster_t *cluster;
	char devstr[MNAMELEN] = {0};
	char *fspec = NULL, *mntpt = NULL, *label = NULL;
	int rdonly = (mp->mnt_flag & MNT_RDONLY) != 0;
	int i, hflags, len, error, devvp_found;
	int *hflagsp = NULL;

	if (vfs_filteropt(opts, hammer2_opts))
		return (EINVAL);

	if (mp->mnt_flag & MNT_UPDATE) {

		error = 0;
		pmp = MPTOPMP(mp);
		cluster = &pmp->iroot->cluster;
		for (i = 0; i < cluster->nchains; ++i) {
			if (cluster->array[i].chain == NULL)
				continue;
			hmp = cluster->array[i].chain->hmp;
			error = hammer2_remount(hmp, mp);
			if (error)
				break;
		}
		return (error);
	}

	error = vfs_getopt(opts, "from", (void **)&fspec, &len);
	if (error)
		return (EINVAL);
	if (!fspec || fspec[len - 1] != '\0')
		return (EINVAL);

	error = vfs_getopt(opts, "fspath", (void **)&mntpt, NULL);
	if (error)
		return (EINVAL);

	error = vfs_getopt(opts, "hflags", (void **)&hflagsp, NULL);
	if (error)
		return (EINVAL);
	hflags = *hflagsp;

	if (hflags & HMNT2_LOCAL) {
		hprintf("HMNT2_LOCAL is not allowed\n");
		return (EINVAL);
	}
	KKASSERT((hflags & HMNT2_LOCAL) == 0);

	bcopy(fspec, devstr, MNAMELEN - 1);
	debug_hprintf("devstr \"%s\" mntpt \"%s\"\n", devstr, mntpt);

	label = strchr(devstr, '@');
	if (label == NULL || label[1] == 0) {

		label = "DATA";
	} else {
		*label = '\0';
		label++;
	}

	debug_hprintf("device \"%s\" label \"%s\" rdonly %d\n",
	    devstr, label, rdonly);

	TAILQ_INIT(&devvpl);
	error = hammer2_init_devvp(mp, devstr, &devvpl);
	if (error) {
		hprintf("failed to initialize devvp in %s\n", devstr);
		hammer2_cleanup_devvp(&devvpl);
		return (error);
	}

	error = hammer2_open_devvp(mp, &devvpl);
	if (error) {
		hammer2_close_devvp(&devvpl);
		hammer2_cleanup_devvp(&devvpl);
		return (error);
	}

	hammer2_mntlk_ex();
	if (!TAILQ_EMPTY(&devvpl)) {

		TAILQ_FOREACH(hmp_tmp, &hammer2_mntlist, mntentry) {
			TAILQ_FOREACH(e_tmp, &hmp_tmp->devvp_list, entry) {
				devvp_found = 0;
				TAILQ_FOREACH(e, &devvpl, entry) {
					KKASSERT(e->bdev);
					if (e_tmp->bdev == e->bdev)
						devvp_found = 1;
				}
				if (!devvp_found)
					goto next_hmp;
			}
			hmp = hmp_tmp;
			debug_hprintf("hmp matched\n");
			break;
next_hmp:
			continue;
		}

		if (hmp == NULL) {
			TAILQ_FOREACH(e, &devvpl, entry) {
				KKASSERT(e->bdev);
				error = 0;
				if (error) {
					hprintf("%s mounted %d\n", e->path,
					    error);
					hammer2_cleanup_devvp(&devvpl);
					hammer2_mntlk_unlock();
					return (error);
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
			hprintf("PFS label \"%s\" not found\n", label);
			hammer2_cleanup_devvp(&devvpl);
			hammer2_mntlk_unlock();
			return (ENOENT);
		}
	}

	if (hmp == NULL) {

		KKASSERT(!TAILQ_EMPTY(&devvpl));

		hmp = hmalloc(sizeof(*hmp), M_HAMMER2, M_WAITOK | M_ZERO);
		hmp->devvp = NULL;
		error = hammer2_init_volumes(&devvpl, hmp->volumes,
		    &hmp->voldata, &hmp->volhdrno, &hmp->devvp);
		if (error) {
			hammer2_close_devvp(&devvpl);
			hammer2_cleanup_devvp(&devvpl);
			hammer2_mntlk_unlock();
			hfree(hmp, M_HAMMER2, sizeof(*hmp));
			return (error);
		}
		if (!hmp->devvp) {
			hprintf("failed to initialize root volume\n");
			hammer2_unmount_helper(mp, NULL, hmp);
			hammer2_mntlk_unlock();
			hammer2_unmount(mp, MNT_FORCE);
			return (EINVAL);
		}

		hmp->rdonly = rdonly;
		hmp->hflags = hflags & HMNT2_DEVFLAGS;

		TAILQ_INSERT_TAIL(&hammer2_mntlist, hmp, mntentry);
		hammer2_mtx_init(&hmp->iohash_lock, "h2dev_iohash");
		hammer2_io_hash_init(hmp);

		hammer2_lk_init(&hmp->vollk, "h2dev_vol");
		hammer2_lk_init(&hmp->bulklk, "h2dev_bulk");
		hammer2_lk_init(&hmp->bflk, "h2dev_bf");

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

		TAILQ_INIT(&hmp->devvp_list);
		while ((e = list_first_entry_or_null(&devvpl.head,
		    hammer2_devvp_t, entry)) != NULL) {
			TAILQ_REMOVE(&devvpl, e, entry);
			TAILQ_INSERT_TAIL(&hmp->devvp_list, e, entry);
		}
		KKASSERT(TAILQ_EMPTY(&devvpl));
		KKASSERT(!TAILQ_EMPTY(&hmp->devvp_list));

		hmp->spmp = hammer2_pfsalloc(NULL, NULL, hmp);
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
		    HAMMER2_SROOT_KEY, HAMMER2_SROOT_KEY, &error, 0);
		hammer2_chain_lookup_done(parent);
		if (schain == NULL) {
			hprintf("invalid super-root\n");
			hammer2_unmount_helper(mp, NULL, hmp);
			hammer2_mntlk_unlock();
			hammer2_unmount(mp, MNT_FORCE);
			return (EINVAL);
		}
		if (schain->error) {
			hprintf("chain error %08x reading super-root\n",
			    schain->error);
			hammer2_chain_unlock(schain);
			hammer2_chain_drop(schain);
			schain = NULL;
			hammer2_unmount_helper(mp, NULL, hmp);
			hammer2_mntlk_unlock();
			hammer2_unmount(mp, MNT_FORCE);
			return (EINVAL);
		}

		spmp->inode_tid = 1;
		spmp->modify_tid = schain->bref.modify_tid + 1;

		ripdata = &schain->data->ipdata;
		KKASSERT(schain->pmp == NULL);
		spmp->pfs_clid = ripdata->meta.pfs_clid;

		xop = uma_zalloc(hammer2_zone_xops, M_WAITOK | M_ZERO);
		hammer2_dummy_xop_from_chain(xop, schain);
		hammer2_inode_drop(spmp->iroot);
		spmp->iroot = hammer2_inode_get(spmp, xop, -1, -1);
		spmp->spmp_hmp = hmp;
		spmp->pfs_types[0] = ripdata->meta.pfs_type;
		spmp->rdonly = rdonly;
		hammer2_inode_ref(spmp->iroot);
		hammer2_inode_unlock(spmp->iroot);
		hammer2_chain_unlock(schain);
		hammer2_chain_drop(schain);
		schain = NULL;
		uma_zfree(hammer2_zone_xops, xop);

		if (!hmp->rdonly) {
			error = hammer2_recovery(hmp);
			if (error == 0)
				error |= hammer2_fixup_pfses(hmp);

		}

		hammer2_update_pmps(hmp);
		hammer2_iocom_init(hmp);
		hammer2_bulkfree_init(hmp);
	} else {

		hammer2_cleanup_devvp(&devvpl);
		spmp = hmp->spmp;
		if (hflags & HMNT2_DEVFLAGS)
			hprintf("WARNING: mount flags pertaining to the whole "
			    "device may only be specified on the first mount "
			    "of the device: %08x\n",
			    hflags & HMNT2_DEVFLAGS);
	}

	force_local = (hmp->hflags & HMNT2_LOCAL) ? hmp : NULL;

	hammer2_inode_lock(spmp->iroot, 0);
	parent = hammer2_inode_chain(spmp->iroot, 0, HAMMER2_RESOLVE_ALWAYS);
	lhc = hammer2_dirhash(label, strlen(label));
	chain = hammer2_chain_lookup(&parent, &key_next, lhc,
	    lhc + HAMMER2_DIRHASH_LOMASK, &error, 0);
	while (chain) {
		if (chain->bref.type == HAMMER2_BREF_TYPE_INODE &&
		    strcmp(label, (char *)chain->data->ipdata.filename) == 0)
			break;
		chain = hammer2_chain_next(&parent, chain, &key_next,
		    lhc + HAMMER2_DIRHASH_LOMASK, &error, 0);
	}
	if (parent) {
		hammer2_chain_unlock(parent);
		hammer2_chain_drop(parent);
	}
	hammer2_inode_unlock(spmp->iroot);

	if (chain == NULL) {
		hammer2_unmount_helper(mp, NULL, hmp);
		hammer2_mntlk_unlock();
		hammer2_unmount(mp, MNT_FORCE);

		if (error) {
			hprintf("PFS label \"%s\" error %08x\n", label, error);
			return (EINVAL);
		} else {
			hprintf("PFS label \"%s\" not found\n", label);
			return (ENOENT);
		}
	}

	if (chain->error) {
		hprintf("PFS label \"%s\" chain error %08x\n",
		    label, chain->error);
	} else {
		ripdata = &chain->data->ipdata;
		pmp = hammer2_pfsalloc(NULL, ripdata, force_local);
	}
	hammer2_chain_unlock(chain);
	hammer2_chain_drop(chain);

	if (pmp == NULL) {
		hprintf("failed to acquire PFS structure\n");
		hammer2_unmount_helper(mp, NULL, hmp);
		hammer2_mntlk_unlock();
		hammer2_unmount(mp, MNT_FORCE);
		return (EINVAL);
	}

	if (pmp->mp) {
		hprintf("PFS already mounted!\n");
		hammer2_unmount_helper(mp, NULL, hmp);
		hammer2_mntlk_unlock();
		hammer2_unmount(mp, MNT_FORCE);
		return (EBUSY);
	}

	if (hammer2_getnewfsid(mp)) {
		hprintf("failed to get new fsid\n");
		hammer2_unmount_helper(mp, NULL, hmp);
		hammer2_mntlk_unlock();
		hammer2_unmount(mp, MNT_FORCE);
		return (EINVAL);
	}

	MNT_ILOCK(mp);
	mp->mnt_flag |= MNT_LOCAL;
	mp->mnt_kern_flag |= MNTK_LOOKUP_SHARED | MNTK_EXTENDED_SHARED |
	    MNTK_USES_BCACHE;
	MNT_IUNLOCK(mp);

	mp->mnt_stat.f_iosize = HAMMER2_PBUFSIZE;
	mp->mnt_stat.f_bsize = HAMMER2_PBUFSIZE;

	hammer2_mount_helper(mp, pmp);

	pmp->rdonly = rdonly;
	if (hmp->rdonly && !pmp->rdonly) {
		error = hammer2_remount_impl(hmp);
		if (error) {
			hprintf("failed to update to rw\n");
			hammer2_unmount_helper(mp, pmp, NULL);
			hammer2_mntlk_unlock();
			hammer2_unmount(mp, MNT_FORCE);
			return (error);
		}
	}
	hammer2_mntlk_unlock();

	hammer2_statfs(mp, &mp->mnt_stat);

	vfs_mountedfrom(mp, fspec);

	return (0);
}

static void
hammer2_update_pmps(hammer2_dev_t *hmp)
{
	hammer2_dev_t *force_local;
	hammer2_pfs_t *spmp;
	const hammer2_inode_data_t *ripdata;
	hammer2_chain_t *parent;
	hammer2_chain_t *chain;
	hammer2_key_t key_next;
	int error;

	force_local = (hmp->hflags & HMNT2_LOCAL) ? hmp : NULL;

	spmp = hmp->spmp;
	hammer2_inode_lock(spmp->iroot, 0);
	parent = hammer2_inode_chain(spmp->iroot, 0, HAMMER2_RESOLVE_ALWAYS);
	chain = hammer2_chain_lookup(&parent, &key_next, HAMMER2_KEY_MIN,
	    HAMMER2_KEY_MAX, &error, 0);
	while (chain) {
		if (chain->error) {
			hprintf("chain error %08x reading PFS root\n",
			    chain->error);
		} else if (chain->bref.type != HAMMER2_BREF_TYPE_INODE) {
			hprintf("non inode chain type %d under super-root\n",
			    chain->bref.type);
		} else {
			ripdata = &chain->data->ipdata;
			hammer2_pfsalloc(chain, ripdata, force_local);
		}
		chain = hammer2_chain_next(&parent, chain, &key_next,
		    HAMMER2_KEY_MAX, &error, 0);
	}
	if (parent) {
		hammer2_chain_unlock(parent);
		hammer2_chain_drop(parent);
	}
	hammer2_inode_unlock(spmp->iroot);
}

int
hammer2_unmount(struct mount *mp, int mntflags)
{
	hammer2_pfs_t *pmp = MPTOPMP(mp);
	int flags = 0, error = 0;

	if (pmp == NULL)
		return (0);

	hammer2_mntlk_ex();

	if (mntflags & MNT_FORCE)
		flags |= FORCECLOSE;
	if (pmp->iroot) {
		error = vflush(mp, 0, flags, curthread);
		if (error) {
			hprintf("vflush failed %d\n", error);
			goto failed;
		}
		hammer2_sync(mp, MNT_WAIT);
		hammer2_sync(mp, MNT_WAIT);
		hammer2_sync(mp, MNT_WAIT);
	} else {
		debug_hprintf("no root inode");
	}

	hammer2_unmount_helper(mp, pmp, NULL);
failed:
	hammer2_mntlk_unlock();

	if (TAILQ_EMPTY(&hammer2_mntlist))
		hammer2_assert_clean();

	return (error);
}

static void
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
}

static void
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
		mp->mnt_flag &= ~MNT_LOCAL;

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

	if (hmp->mount_count) {
		hprintf("%d PFS mounts still exist\n", hmp->mount_count);
		return;
	}

	hammer2_iocom_uninit(hmp);
	hammer2_bulkfree_uninit(hmp);
	hammer2_pfsfree_scan(hmp, 0);

	hammer2_chain_lock(&hmp->vchain, HAMMER2_RESOLVE_ALWAYS);
	hammer2_chain_lock(&hmp->fchain, HAMMER2_RESOLVE_ALWAYS);

	if (hmp->fchain.flags & HAMMER2_CHAIN_FLUSH_MASK) {
		hammer2_voldata_modify(hmp);
		hammer2_flush(&hmp->fchain,
		    HAMMER2_FLUSH_TOP | HAMMER2_FLUSH_ALL);
	}
	hammer2_chain_unlock(&hmp->fchain);

	if (hmp->vchain.flags & HAMMER2_CHAIN_FLUSH_MASK)
		hammer2_flush(&hmp->vchain,
		    HAMMER2_FLUSH_TOP | HAMMER2_FLUSH_ALL);
	hammer2_chain_unlock(&hmp->vchain);

	if ((hmp->vchain.flags | hmp->fchain.flags) &
	    HAMMER2_CHAIN_FLUSH_MASK) {

		hprintf("chains left over after final sync "
		    "vchain %08x fchain %08x (unmounting anyway)\n",
		    hmp->vchain.flags, hmp->fchain.flags);
	}

	hammer2_pfsfree_scan(hmp, 1);
	KKASSERT(hmp->spmp == NULL);

	if (!TAILQ_EMPTY(&hmp->devvp_list)) {
		hammer2_close_devvp(&hmp->devvp_list);
		hammer2_cleanup_devvp(&hmp->devvp_list);
	}
	KKASSERT(TAILQ_EMPTY(&hmp->devvp_list));

	if (hmp->vchain.flags & HAMMER2_CHAIN_MODIFIED) {
		atomic_add_int(&hammer2_count_chain_modified, -1);
		atomic_clear_int(&hmp->vchain.flags, HAMMER2_CHAIN_MODIFIED);
	}
	if (hmp->vchain.flags & HAMMER2_CHAIN_UPDATE)
		atomic_clear_int(&hmp->vchain.flags, HAMMER2_CHAIN_UPDATE);

	if (hmp->fchain.flags & HAMMER2_CHAIN_MODIFIED) {
		atomic_add_int(&hammer2_count_chain_modified, -1);
		atomic_clear_int(&hmp->fchain.flags, HAMMER2_CHAIN_MODIFIED);
	}
	if (hmp->fchain.flags & HAMMER2_CHAIN_UPDATE)
		atomic_clear_int(&hmp->fchain.flags, HAMMER2_CHAIN_UPDATE);

#ifdef HAMMER2_INVARIANTS
	hammer2_dump_chain(&hmp->vchain, 0, 0, -1, 'v');
	hammer2_dump_chain(&hmp->fchain, 0, 0, -1, 'f');
#endif

	hammer2_chain_drop(&hmp->vchain);
	hammer2_chain_drop(&hmp->fchain);

	hammer2_mtx_ex(&hmp->iohash_lock);
	hammer2_io_hash_cleanup_all(hmp);
	if (hmp->iofree_count) {
		hprintf("XXX %d I/O's left hanging\n", hmp->iofree_count);

	}
	hammer2_mtx_unlock(&hmp->iohash_lock);

	TAILQ_REMOVE(&hammer2_mntlist, hmp, mntentry);
	hammer2_mtx_destroy(&hmp->iohash_lock);
	hammer2_io_hash_destroy(hmp);

	hammer2_lk_destroy(&hmp->vollk);
	hammer2_lk_destroy(&hmp->bulklk);
	hammer2_lk_destroy(&hmp->bflk);

	hammer2_print_iostat(&hmp->iostat_read, "read");
	hammer2_print_iostat(&hmp->iostat_write, "write");

	hfree(hmp, M_HAMMER2, sizeof(*hmp));

	if (TAILQ_EMPTY(&hammer2_mntlist)) {
		if (malloc_leak_m_hammer2)
			hprintf("XXX M_HAMMER2 %d bytes leaked\n",
			    malloc_leak_m_hammer2);
		if (malloc_leak_m_hammer2_lz4)
			hprintf("XXX M_HAMMER2_LZ4 %d bytes leaked\n",
			    malloc_leak_m_hammer2_lz4);
		if (malloc_leak_m_temp)
			hprintf("XXX M_TEMP %d bytes leaked\n",
			    malloc_leak_m_temp);
	}
}

struct hammer2_recovery_elm {
	TAILQ_ENTRY(hammer2_recovery_elm) entry;
	hammer2_chain_t *chain;
	hammer2_tid_t sync_tid;
};

TAILQ_HEAD(hammer2_recovery_list, hammer2_recovery_elm);

struct hammer2_recovery_info {
	struct hammer2_recovery_list list;
	hammer2_tid_t mtid;
	int depth;
};

static int hammer2_recovery_scan(hammer2_dev_t *, hammer2_chain_t *,
    struct hammer2_recovery_info *, hammer2_tid_t);

#define HAMMER2_RECOVERY_MAXDEPTH	10

static int
hammer2_recovery(hammer2_dev_t *hmp)
{
	struct hammer2_recovery_info info;
	struct hammer2_recovery_elm *elm;
	hammer2_chain_t *parent;
	hammer2_tid_t sync_tid, mirror_tid;
	int error;

	hammer2_trans_init(hmp->spmp, 0);

	sync_tid = hmp->voldata.freemap_tid;
	mirror_tid = hmp->voldata.mirror_tid;

	if (sync_tid >= mirror_tid)
		debug_hprintf("no recovery needed\n");
	else
		hprintf("freemap recovery %016llx-%016llx\n",
		    (long long)sync_tid + 1, (long long)mirror_tid);

	TAILQ_INIT(&info.list);
	info.depth = 0;
	parent = hammer2_chain_lookup_init(&hmp->vchain, 0);
	error = hammer2_recovery_scan(hmp, parent, &info, sync_tid);
	hammer2_chain_lookup_done(parent);

	while ((elm = list_first_entry_or_null(&info.list.head,
	    struct hammer2_recovery_elm, entry)) != NULL) {
		TAILQ_REMOVE(&info.list, elm, entry);
		parent = elm->chain;
		sync_tid = elm->sync_tid;
		hfree(elm, M_HAMMER2, sizeof(*elm));

		hammer2_chain_lock(parent, HAMMER2_RESOLVE_ALWAYS);
		error |= hammer2_recovery_scan(hmp, parent, &info,
		    hmp->voldata.freemap_tid);
		hammer2_chain_unlock(parent);
		hammer2_chain_drop(parent);
	}

	hammer2_trans_done(hmp->spmp, 0);

	return (error);
}

static int
hammer2_recovery_scan(hammer2_dev_t *hmp, hammer2_chain_t *parent,
    struct hammer2_recovery_info *info, hammer2_tid_t sync_tid)
{
	hammer2_chain_t *chain;
	hammer2_blockref_t bref;
	struct hammer2_recovery_elm *elm;
	const hammer2_inode_data_t *ripdata;
	int tmp_error, rup_error, error, first;

	if (parent->bref.type != HAMMER2_BREF_TYPE_VOLUME)
		hammer2_freemap_adjust(hmp, &parent->bref,
		    HAMMER2_FREEMAP_DORECOVER);

	switch (parent->bref.type) {
	case HAMMER2_BREF_TYPE_VOLUME:

		break;
	case HAMMER2_BREF_TYPE_INODE:

		hammer2_chain_lock(parent, HAMMER2_RESOLVE_ALWAYS);
		ripdata = &parent->data->ipdata;
		if (ripdata->meta.op_flags & HAMMER2_OPFLAG_DIRECTDATA) {

			hammer2_chain_unlock(parent);
			return (0);
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

		return (0);
		break;
	default:
		return (HAMMER2_ERROR_BADBREF);
	}

	if (info->depth >= HAMMER2_RECOVERY_MAXDEPTH) {
		elm = hmalloc(sizeof(*elm), M_HAMMER2, M_ZERO | M_WAITOK);
		elm->chain = parent;
		elm->sync_tid = sync_tid;
		hammer2_chain_ref(parent);
		TAILQ_INSERT_TAIL(&info->list, elm, entry);

		return (0);
	}

	chain = NULL;
	first = 1;
	rup_error = 0;
	error = 0;

	for (;;) {
		error |= hammer2_chain_scan(parent, &chain, &bref, &first,
		    HAMMER2_LOOKUP_NODATA);

		if (error)
			break;

		if (chain == NULL) {
			if (bref.mirror_tid > sync_tid)
				hammer2_freemap_adjust(hmp, &bref,
				    HAMMER2_FREEMAP_DORECOVER);
			continue;
		}

		atomic_set_int(&chain->flags, HAMMER2_CHAIN_RELEASE);
		if (bref.mirror_tid > sync_tid) {
			++info->depth;
			tmp_error = hammer2_recovery_scan(hmp, chain, info,
			    sync_tid);
			--info->depth;
		} else {
			tmp_error = 0;
		}

		if (tmp_error == 0 &&
		    (bref.flags & HAMMER2_BREF_FLAG_PFSROOT) &&
		    (chain->flags & HAMMER2_CHAIN_ONFLUSH))
			hammer2_flush(chain,
			    HAMMER2_FLUSH_TOP | HAMMER2_FLUSH_ALL);
		rup_error |= tmp_error;
	}
	return ((error | rup_error) & ~HAMMER2_ERROR_EOF);
}

static int
hammer2_fixup_pfses(hammer2_dev_t *hmp)
{
	const hammer2_inode_data_t *ripdata;
	hammer2_chain_t *parent, *chain;
	hammer2_key_t key_next;
	hammer2_pfs_t *spmp;
	int error = 0, error2;

	spmp = hmp->spmp;
	hammer2_inode_lock(spmp->iroot, 0);
	parent = hammer2_inode_chain(spmp->iroot, 0, HAMMER2_RESOLVE_ALWAYS);
	chain = hammer2_chain_lookup(&parent, &key_next, HAMMER2_KEY_MIN,
	    HAMMER2_KEY_MAX, &error, 0);

	while (chain) {
		if (chain->bref.type != HAMMER2_BREF_TYPE_INODE)
			continue;
		if (chain->error) {
			hprintf("I/O error scanning PFS labels\n");
			error |= chain->error;
		} else if ((chain->bref.flags & HAMMER2_BREF_FLAG_PFSROOT) == 0) {
			ripdata = &chain->data->ipdata;
			hammer2_trans_init(hmp->spmp, 0);
			error2 = hammer2_chain_modify(chain,
			    chain->bref.modify_tid, 0, 0);
			if (error2 == 0) {
				hprintf("correct mis-flagged PFS %s\n",
				    ripdata->filename);
				chain->bref.flags |= HAMMER2_BREF_FLAG_PFSROOT;
			} else {
				error |= error2;
			}
			hammer2_flush(chain,
			    HAMMER2_FLUSH_TOP | HAMMER2_FLUSH_ALL);
			hammer2_trans_done(hmp->spmp, 0);
		}
		chain = hammer2_chain_next(&parent, chain, &key_next,
		    HAMMER2_KEY_MAX, &error, 0);
	}

	if (parent) {
		hammer2_chain_unlock(parent);
		hammer2_chain_drop(parent);
	}
	hammer2_inode_unlock(spmp->iroot);

	return (error);
}

static int
hammer2_remount_impl(hammer2_dev_t *hmp)
{
	hammer2_volume_t *vol;
	int i, j, error;

	for (i = 0; i < hmp->nvolumes; ++i) {
		vol = &hmp->volumes[i];
		error = hammer2_access_devvp(vol->dev->bdev, 0);
		if (error)
			return (error);
	}

	for (i = 0; i < hmp->nvolumes; ++i) {
		vol = &hmp->volumes[i];
		error = hammer2_getw_devvp(vol->dev->bdev);
		if (error) {
			for (j = 0; j < i; ++j) {
				vol = &hmp->volumes[j];
				hammer2_putw_devvp(vol->dev->bdev);
			}
			return (error);
		}
	}

	for (i = 0; i < hmp->nvolumes; ++i) {
		vol = &hmp->volumes[i];
		if (vol->id == HAMMER2_ROOT_VOLUME) {
			error = hammer2_recovery(hmp);
			if (error == 0)
				error |= hammer2_fixup_pfses(hmp);
			if (error)
				return (hammer2_error_to_errno(error));
		}
	}
	hmp->rdonly = 0;
	hmp->spmp->rdonly = 0;

	return (0);
}

static int
hammer2_remount(hammer2_dev_t *hmp, struct mount *mp)
{
	struct vfsoptlist *opts = mp->mnt_optnew;
	hammer2_pfs_t *pmp = MPTOPMP(mp);
	int error;

	if (pmp->rdonly == 0 && vfs_flagopt(opts, "ro", NULL, 0)) {

		vfs_flagopt(opts, "ro", &mp->mnt_flag, MNT_RDONLY);
		pmp->rdonly = 1;
	} else if (pmp->rdonly == 1 && !vfs_flagopt(opts, "ro", NULL, 0)) {
		if (hmp->rdonly) {
			error = hammer2_remount_impl(hmp);
			if (error)
				return (error);
		}
		MNT_ILOCK(mp);
		mp->mnt_flag &= ~MNT_RDONLY;
		MNT_IUNLOCK(mp);
		pmp->rdonly = 0;
	}

	debug_hprintf("ro %d rw %d MNT_RDONLY %d rdonly %d/%d/%d\n",
	    vfs_flagopt(opts, "ro", NULL, 0),
	    vfs_flagopt(opts, "rw", NULL, 0),
	    (mp->mnt_flag & MNT_RDONLY) ? 1 : 0,
	    hmp->rdonly, hmp->spmp->rdonly, pmp->rdonly);

	return (0);
}

int
hammer2_sync(struct mount *mp, int waitfor)
{
	return (hammer2_vfs_sync_pmp(MPTOPMP(mp), waitfor));
}

void
hammer2_pfs_memory_wait(hammer2_pfs_t *pmp)
{
	if (pmp == NULL || pmp->mp == NULL)
		return;

	if (current->flags & PF_LOCAL_THROTTLE)
		return;

	if (hammer2_count_chain_modified >= hammer2_limit_dirty_chains)
		return;

	queue_work(system_unbound_wq, &pmp->throttle_work);
	wait_event_timeout(pmp->throttle_wq,
	    hammer2_count_chain_modified < hammer2_limit_dirty_chains,
	    HZ / 10);
}

static int
hammer2_pmp_iofail_count(hammer2_pfs_t *pmp)
{
	int i, sum = 0;

	for (i = 0; i < HAMMER2_MAXCLUSTER; ++i) {
		if (pmp->pfs_hmps[i])
			sum += pmp->pfs_hmps[i]->iofail_count;
	}
	return (sum);
}

int
hammer2_vfs_sync_pmp(hammer2_pfs_t *pmp, int waitfor __unused)
{
	hammer2_inode_t *ip;
	hammer2_depend_t *depend, *depend_next;
	struct inode *vp;
	uint32_t pass2;
	int error, dorestart;
	int iofail, iofail_passes = 0, syncfail = 0;

	hammer2_trans_init(pmp, HAMMER2_TRANS_ISFLUSH);
	debug_hprintf("FILESYSTEM SYNC BOUNDARY\n");
	dorestart = 0;

restart:
	debug_hprintf("FILESYSTEM SYNC RESTART (%d)\n", dorestart);
	iofail = hammer2_pmp_iofail_count(pmp);
	hammer2_trans_setflags(pmp, 0);
	hammer2_trans_clearflags(pmp, HAMMER2_TRANS_RESCAN);

	hammer2_spin_ex(&pmp->list_spin);
	depend_next = list_first_entry_or_null(&pmp->depq.head,
	    hammer2_depend_t, entry);

	while ((depend = depend_next) != NULL) {
		depend_next = list_next_entry(depend, entry);
		if (&depend_next->entry == &pmp->depq.head)
			depend_next = NULL;
		if (dorestart && depend->pass2 == 0)
			continue;
		TAILQ_FOREACH(ip, &depend->sideq, qentry) {
			KKASSERT(ip->flags & HAMMER2_INODE_SIDEQ);
			atomic_set_int(&ip->flags, HAMMER2_INODE_SYNCQ);
			atomic_clear_int(&ip->flags, HAMMER2_INODE_SIDEQ);
			ip->depend = NULL;
		}

		TAILQ_CONCAT(&pmp->syncq, &depend->sideq, qentry);

		depend->count = 0;
		depend->pass2 = 0;
		TAILQ_REMOVE(&pmp->depq, depend, entry);
	}

	hammer2_spin_unex(&pmp->list_spin);
	hammer2_trans_clearflags(pmp, HAMMER2_TRANS_WAITING);
	dorestart = 0;

	hammer2_spin_ex(&pmp->list_spin);
	while ((ip = list_first_entry_or_null(&pmp->syncq.head,
	    hammer2_inode_t, qentry)) != NULL) {

		pass2 = ip->flags;
		cpu_ccfence();
		if (atomic_cmpset_int(&ip->flags, pass2,
		    (pass2 & ~(HAMMER2_INODE_SYNCQ | HAMMER2_INODE_SYNCQ_WAKEUP)) |
		    HAMMER2_INODE_SYNCQ_PASS2) == 0)
			continue;
		TAILQ_REMOVE(&pmp->syncq, ip, qentry);
		--pmp->sideq_count;
		hammer2_spin_unex(&pmp->list_spin);

		if (pass2 & HAMMER2_INODE_SYNCQ_WAKEUP)
			hammer2_mtx_wakeup(&ip->flags);

		vp = ip->vp;

		if (vp) {
			if (!igrab(vp)) {
				vp = NULL;
				dorestart |= 1;
				hammer2_mtx_ex(&ip->lock);
				hammer2_inode_delayed_sideq(ip);
				hammer2_mtx_unlock(&ip->lock);
				hammer2_inode_drop(ip);
				if (pass2 & HAMMER2_INODE_SYNCQ_PASS2)
					dorestart |= 2;
				hammer2_spin_ex(&pmp->list_spin);
				continue;
			}
		}

		if (hammer2_mtx_ex_try(&ip->lock) != 0) {
			hammer2_inode_delayed_sideq(ip);
			dorestart |= 1;
			if (pass2 & HAMMER2_INODE_SYNCQ_PASS2)
				dorestart |= 2;
			hammer2_inode_drop(ip);
			if (vp)
				iput(vp);
			hammer2_spin_ex(&pmp->list_spin);
			continue;
		}

		if (ip->flags & HAMMER2_INODE_SIDEQ) {
			hammer2_mtx_unlock(&ip->lock);
			hammer2_inode_drop(ip);
			if (vp)
				iput(vp);
			dorestart |= 1;
			hammer2_spin_ex(&pmp->list_spin);
			continue;
		}

		if (vp && !S_ISCHR(vp->i_mode)) {
			error = write_inode_now(vp, 1);
			if (error) {
				hprintf("inum %016llx vnode flush failed %d\n",
				    (long long)ip->meta.inum, error);
				error = 0;
			}
		}

		if (ip->flags & HAMMER2_INODE_DELETING) {
			debug_hprintf("inum %016llx destroy\n",
			    (long long)ip->meta.inum);
			hammer2_inode_chain_des(ip);
		} else if (ip->flags & HAMMER2_INODE_CREATING) {
			debug_hprintf("inum %016llx insert\n",
			    (long long)ip->meta.inum);
			hammer2_inode_chain_ins(ip);
		}

		debug_hprintf("inum %016llx pinum %016llx chain-sync\n",
		    (long long)ip->meta.inum, (long long)ip->meta.iparent);
		hammer2_inode_chain_sync(ip);

		if (ip == pmp->iroot)
			hammer2_inode_chain_flush(ip, HAMMER2_XOP_INODE_STOP);
		else
			hammer2_inode_chain_flush(ip,
			    HAMMER2_XOP_INODE_STOP | HAMMER2_XOP_FSSYNC);
		if (vp) {
			if ((ip->flags & (HAMMER2_INODE_MODIFIED |
			    HAMMER2_INODE_RESIZED |
			    HAMMER2_INODE_DIRTYDATA)) == 0) {

			} else {
				hammer2_inode_delayed_sideq(ip);
			}
			iput(vp);
			vp = NULL;
		}
		atomic_clear_int(&ip->flags, HAMMER2_INODE_SYNCQ_PASS2);
		hammer2_inode_unlock(ip);

		hammer2_spin_ex(&pmp->list_spin);
	}
	hammer2_spin_unex(&pmp->list_spin);

	if (hammer2_pmp_iofail_count(pmp) != iofail &&
	    ++iofail_passes >= HAMMER2_SYNC_IOFAIL_MAX) {
		hprintf("giving up sync after %d consecutive passes with I/O "
		    "errors (device gone?)\n", iofail_passes);
		syncfail = 1;
	} else if (hammer2_pmp_iofail_count(pmp) == iofail) {
		iofail_passes = 0;
	}

	if (!syncfail && (dorestart ||
	    (pmp->trans.flags & HAMMER2_TRANS_RESCAN))) {

		if (dorestart & 2)
			tsleep(&dorestart, 0, "h2syndel", 2);
		debug_hprintf("FILESYSTEM SYNC STAGE 1 RESTART\n");
		dorestart = 1;
		goto restart;
	}
	debug_hprintf("FILESYSTEM SYNC STAGE 2 BEGIN\n");

	if ((ip = pmp->iroot) != NULL) {
		hammer2_inode_ref(ip);
		hammer2_mtx_ex(&ip->lock);
		hammer2_inode_chain_sync(ip);
		hammer2_inode_chain_flush(ip,
		    HAMMER2_XOP_INODE_STOP | HAMMER2_XOP_FSSYNC |
		    HAMMER2_XOP_VOLHDR);
		hammer2_inode_unlock(ip);
	}
	debug_hprintf("FILESYSTEM SYNC STAGE 2 DONE\n");

	hammer2_bioq_sync(pmp);

	error = syncfail ? EIO : 0;
	hammer2_trans_done(pmp, HAMMER2_TRANS_ISFLUSH);

	return (error);
}

static int
hammer2_vget(struct mount *mp, ino_t ino, int flags, struct inode **vpp)
{
	hammer2_pfs_t *pmp = MPTOPMP(mp);
	hammer2_inode_t *ip;
	hammer2_xop_lookup_t *xop;
	hammer2_tid_t inum;
	int error;

	inum = (hammer2_tid_t)ino & HAMMER2_DIRHASH_USERMSK;

	ip = hammer2_inode_lookup(pmp, inum);
	if (ip) {
		hammer2_inode_lock(ip, HAMMER2_RESOLVE_SHARED);
		error = hammer2_igetv(ip, flags, vpp);
		hammer2_inode_unlock(ip);
		hammer2_inode_drop(ip);
		return (error);
	}

	xop = hammer2_xop_alloc(pmp->iroot, 0);
	xop->lhc = inum;
	hammer2_xop_start(&xop->head, &hammer2_lookup_desc);
	error = hammer2_xop_collect(&xop->head, 0);
	if (error == 0)
		ip = hammer2_inode_get(pmp, &xop->head, -1, -1);
	hammer2_xop_retire(&xop->head, HAMMER2_XOPMASK_VOP);

	if (ip) {
		error = hammer2_igetv(ip, flags, vpp);
		hammer2_inode_unlock(ip);
	} else {
		*vpp = NULL;
		error = ENOENT;
	}

	return (error);
}

int
hammer2_root(struct mount *mp, int flags, struct inode **vpp)
{
	hammer2_pfs_t *pmp = MPTOPMP(mp);
	hammer2_xop_ipcluster_t *xop;
	const hammer2_inode_meta_t *meta;
	int error = 0;

	if (pmp->iroot == NULL) {
		hprintf("%s has no root inode\n", mp->mnt_stat.f_mntfromname);
		*vpp = NULL;
		return (EINVAL);
	}

	hammer2_inode_lock(pmp->iroot, HAMMER2_RESOLVE_SHARED);

	while (pmp->inode_tid == 0) {
		xop = hammer2_xop_alloc(pmp->iroot, HAMMER2_XOP_MODIFYING);
		hammer2_xop_start(&xop->head, &hammer2_ipcluster_desc);
		error = hammer2_xop_collect(&xop->head, 0);

		if (error == 0) {
			meta = &((const hammer2_media_data_t *)
			    hammer2_xop_gdata(&xop->head))->ipdata.meta;
			pmp->iroot->meta = *meta;
			pmp->inode_tid = meta->pfs_inum + 1;
			hammer2_xop_pdata(&xop->head);

			if (pmp->inode_tid < HAMMER2_INODE_START)
				pmp->inode_tid = HAMMER2_INODE_START;
			pmp->modify_tid =
			    xop->head.cluster.focus->bref.modify_tid + 1;
			debug_hprintf("PFS nextino %016llx mod %016llx\n",
			    (long long)pmp->inode_tid,
			    (long long)pmp->modify_tid);

			wakeup(&pmp->iroot);
			hammer2_xop_retire(&xop->head, HAMMER2_XOPMASK_VOP);

			hammer2_statfs(mp, &mp->mnt_stat);
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
		error = hammer2_igetv(pmp->iroot, LK_EXCLUSIVE, vpp);
		hammer2_inode_unlock(pmp->iroot);
	}

	return (error);
}

int
hammer2_statfs(struct mount *mp, struct h2statfs *sbp)
{
	hammer2_pfs_t *pmp = MPTOPMP(mp);
	hammer2_dev_t *hmp;
	hammer2_blockref_t bref;
	struct ucred *cred = NULL;
	struct h2statfs tmp;
	uint64_t adj;
	int i;

	KKASSERT(mp->mnt_stat.f_iosize > 0);
	KKASSERT(mp->mnt_stat.f_bsize > 0);

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
		    mp->mnt_stat.f_bsize;
		tmp.f_bfree = hmp->voldata.allocator_free /
		    mp->mnt_stat.f_bsize;
		tmp.f_bavail = tmp.f_bfree;

		if (cred && cred->cr_uid != 0) {

			adj = hmp->free_reserved / mp->mnt_stat.f_bsize;
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
		sbp->f_iosize = mp->mnt_stat.f_iosize;
		sbp->f_bsize = mp->mnt_stat.f_bsize;
	}
	return (0);
}

static int
hammer2_fhtovp(struct mount *mp, struct fid *fhp, int flags, struct inode **vpp)
{
	hammer2_inode_t *ip;
	hammer2_tid_t inum;
	int error;

#if __FreeBSD_version < FREEBSD_READDIR_COOKIES_64
	return (EOPNOTSUPP);
#endif
	inum = ((hammer2_tid_t *)fhp->fid_data)[0] & HAMMER2_DIRHASH_USERMSK;
	if (vpp) {
		if (inum == 1)
			error = hammer2_root(mp, LK_EXCLUSIVE, vpp);
		else
			error = hammer2_vget(mp, inum, LK_EXCLUSIVE, vpp);
	} else {
		error = 0;
	}

	ip = VTOI(*vpp);
	(void)ip;

	return (error);
}

void
hammer2_voldata_lock(hammer2_dev_t *hmp)
{
	hammer2_lk_ex(&hmp->vollk);
}

void
hammer2_voldata_unlock(hammer2_dev_t *hmp)
{
	hammer2_lk_unlock(&hmp->vollk);
}

void
hammer2_voldata_modify(hammer2_dev_t *hmp)
{
	if ((hmp->vchain.flags & HAMMER2_CHAIN_MODIFIED) == 0) {
		atomic_add_int(&hammer2_count_chain_modified, 1);
		atomic_set_int(&hmp->vchain.flags, HAMMER2_CHAIN_MODIFIED);
		hmp->vchain.bref.mirror_tid = hmp->voldata.mirror_tid + 1;
	}
}

int
hammer2_vfs_enospace(hammer2_inode_t *ip, off_t bytes, struct ucred *cred)
{
	hammer2_dev_t *hmp;
	hammer2_pfs_t *pmp = ip->pmp;
	hammer2_off_t free_reserved, free_nominal;
	int i;

	if (pmp->free_ticks == 0 || pmp->free_ticks != getticks()) {
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
		pmp->free_ticks = getticks();
	} else {
		free_reserved = pmp->free_reserved;
		free_nominal = pmp->free_nominal;
	}

	if (cred && cred->cr_uid != 0) {
		if ((int64_t)(free_nominal - bytes) < (int64_t)free_reserved)
			return (2);
	} else {
		if ((int64_t)(free_nominal - bytes) < (int64_t)free_reserved / 2)
			return (2);
	}

	if ((int64_t)(free_nominal - bytes) < (int64_t)free_reserved * 2)
		return (1);

	return (0);
}

static struct vfsops hammer2_vfsops = {
	.vfs_init	= hammer2_init,
	.vfs_uninit	= hammer2_uninit,
	.vfs_mount	= hammer2_mount,
	.vfs_unmount	= hammer2_unmount,
	.vfs_sync       = hammer2_sync,
	.vfs_vget	= hammer2_vget,
	.vfs_root	= hammer2_root,
	.vfs_statfs	= hammer2_statfs,
	.vfs_fhtovp	= hammer2_fhtovp,
};

VFS_SET(hammer2_vfsops, hammer2, 0);
MODULE_VERSION(hammer2, 1);
