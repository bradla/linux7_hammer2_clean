#include "hammer2.h"
#include "hammer2_ioctl.h"
#include "hammer2_mount.h"
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/uaccess.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>

static int
hammer2_is_rdonly(const struct mount *mp)
{
	if (mp && (mp->mnt_flag & MNT_RDONLY)) {
		hprintf("read-only mounted\n");
		return 1;
	}
	return 0;
}

static int
hammer2_ioctl_version_get(hammer2_inode_t *ip, void *data)
{
	hammer2_ioc_version_t *v = data;
	hammer2_dev_t *hmp = ip->pmp->pfs_hmps[0];

	if (hmp == NULL)
		return (EINVAL);

	if (hmp)
		v->version = hmp->voldata.version;
	else
		v->version = -1;

	return (0);
}

static int
hammer2_ioctl_recluster(hammer2_inode_t *ip, void *data)
{
	hammer2_ioc_recluster_t *recl = data;
	hammer2_cluster_t *cluster;
	struct file *fp;
	int error;

	fp = fget(recl->fd);
	if (fp == NULL)
		return (EINVAL);

	cluster = &ip->pmp->iroot->cluster;
	if (cluster->focus != NULL) {
		hammer2_cluster_reconnect(cluster->focus->hmp, fp);
		error = 0;
	} else if (cluster->nchains == 1 &&
		   cluster->array[0].chain != NULL) {
		hammer2_cluster_reconnect(cluster->array[0].chain->hmp, fp);
		error = 0;
	} else {
		hprintf("recluster: nchains=%d focus=%p\n",
			cluster->nchains, cluster->focus);
		fput(fp);
		error = EINVAL;
	}
	return (error);
}

static int
hammer2_ioctl_pfs_get(hammer2_inode_t *ip, void *data)
{
	hammer2_ioc_pfs_t *pfs = data;
	hammer2_dev_t *hmp = ip->pmp->pfs_hmps[0];
	const hammer2_inode_data_t *ripdata;
	hammer2_chain_t *chain, *parent;
	hammer2_key_t key_next, save_key;
	int error = 0;

	if (hmp == NULL)
		return (EINVAL);

	save_key = pfs->name_key;

	if (save_key == (hammer2_key_t)-1) {
		hammer2_inode_lock(ip->pmp->iroot, 0);
		parent = NULL;
		chain = hammer2_inode_chain(ip->pmp->iroot, 0,
		    HAMMER2_RESOLVE_ALWAYS | HAMMER2_RESOLVE_SHARED);
	} else {
		hammer2_inode_lock(hmp->spmp->iroot, 0);
		parent = hammer2_inode_chain(hmp->spmp->iroot, 0,
		    HAMMER2_RESOLVE_ALWAYS | HAMMER2_RESOLVE_SHARED);
		chain = hammer2_chain_lookup(&parent, &key_next, pfs->name_key,
		    HAMMER2_KEY_MAX, &error, HAMMER2_LOOKUP_SHARED);
	}

	while (chain) {
		if (chain->bref.type == HAMMER2_BREF_TYPE_INODE)
			break;
		if (parent == NULL) {
			hammer2_chain_unlock(chain);
			hammer2_chain_drop(chain);
			chain = NULL;
			break;
		}
		chain = hammer2_chain_next(&parent, chain, &key_next,
		    HAMMER2_KEY_MAX, &error, HAMMER2_LOOKUP_SHARED);
	}
	error = hammer2_error_to_errno(error);

	if (chain && chain->error == 0) {
		ripdata = &chain->data->ipdata;
		pfs->name_key = ripdata->meta.name_key;
		pfs->pfs_type = ripdata->meta.pfs_type;
		pfs->pfs_subtype = ripdata->meta.pfs_subtype;
		pfs->pfs_clid = ripdata->meta.pfs_clid;
		pfs->pfs_fsid = ripdata->meta.pfs_fsid;
		KKASSERT(ripdata->meta.name_len < sizeof(pfs->name));
		bcopy(ripdata->filename, pfs->name, ripdata->meta.name_len);
		pfs->name[ripdata->meta.name_len] = 0;
		ripdata = NULL;

		if (parent == NULL) {
			pfs->name_next = (hammer2_key_t)-1;
		} else {
			chain = hammer2_chain_next(&parent, chain, &key_next,
			    HAMMER2_KEY_MAX, &error, HAMMER2_LOOKUP_SHARED);
			if (chain)
				pfs->name_next = chain->bref.key;
			else
				pfs->name_next = (hammer2_key_t)-1;
		}
	} else {
		pfs->name_next = (hammer2_key_t)-1;
		error = ENOENT;
	}

	if (chain) {
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
	}
	if (parent) {
		hammer2_chain_unlock(parent);
		hammer2_chain_drop(parent);
	}

	if (save_key == (hammer2_key_t)-1)
		hammer2_inode_unlock(ip->pmp->iroot);
	else
		hammer2_inode_unlock(hmp->spmp->iroot);

	return (error);
}

static int
hammer2_ioctl_pfs_lookup(hammer2_inode_t *ip, void *data)
{
	hammer2_ioc_pfs_t *pfs = data;
	hammer2_dev_t *hmp = ip->pmp->pfs_hmps[0];
	const hammer2_inode_data_t *ripdata;
	hammer2_chain_t *chain, *parent;
	hammer2_key_t key_next, lhc;
	size_t len;
	int error = 0;

	if (hmp == NULL)
		return (EINVAL);

	hammer2_inode_lock(hmp->spmp->iroot, HAMMER2_RESOLVE_SHARED);
	parent = hammer2_inode_chain(hmp->spmp->iroot, 0,
	    HAMMER2_RESOLVE_ALWAYS | HAMMER2_RESOLVE_SHARED);

	pfs->name[sizeof(pfs->name) - 1] = 0;
	len = strlen(pfs->name);
	lhc = hammer2_dirhash(pfs->name, len);

	chain = hammer2_chain_lookup(&parent, &key_next, lhc,
	    lhc + HAMMER2_DIRHASH_LOMASK, &error, HAMMER2_LOOKUP_SHARED);
	while (chain) {
		if (hammer2_chain_dirent_test(chain, pfs->name, len))
			break;
		chain = hammer2_chain_next(&parent, chain, &key_next,
		    lhc + HAMMER2_DIRHASH_LOMASK, &error,
		    HAMMER2_LOOKUP_SHARED);
	}
	error = hammer2_error_to_errno(error);

	if (chain && chain->error == 0) {
		KKASSERT(chain->bref.type == HAMMER2_BREF_TYPE_INODE);
		ripdata = &chain->data->ipdata;
		pfs->name_key = ripdata->meta.name_key;
		pfs->pfs_type = ripdata->meta.pfs_type;
		pfs->pfs_subtype = ripdata->meta.pfs_subtype;
		pfs->pfs_clid = ripdata->meta.pfs_clid;
		pfs->pfs_fsid = ripdata->meta.pfs_fsid;

		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
	} else if (error == 0) {
		error = ENOENT;
	}
	if (parent) {
		hammer2_chain_unlock(parent);
		hammer2_chain_drop(parent);
	}

	hammer2_inode_unlock(hmp->spmp->iroot);

	return (error);
}

static int
hammer2_ioctl_pfs_create(hammer2_inode_t *ip, void *data)
{
	hammer2_inode_data_t *nipdata;
	hammer2_chain_t *nchain;
	hammer2_dev_t *hmp, *force_local;
	hammer2_ioc_pfs_t *pfs = data;
	hammer2_inode_t *nip = NULL;
	hammer2_tid_t mtid;
	int error;

	hmp = ip->pmp->pfs_hmps[0];
	if (hmp == NULL)
		return (EINVAL);

	if (pfs->name[0] == 0)
		return (EINVAL);
	pfs->name[sizeof(pfs->name) - 1] = 0;

	if (hammer2_is_rdonly(ip->pmp->mp))
		return (EROFS);

	if (hammer2_ioctl_pfs_lookup(ip, pfs) == 0)
		return (EEXIST);

	if (pfs->pfs_type != HAMMER2_PFSTYPE_MASTER)
		return (EOPNOTSUPP);

	hammer2_trans_init(hmp->spmp, HAMMER2_TRANS_ISFLUSH);
	mtid = hammer2_trans_sub(hmp->spmp);
	nip = hammer2_inode_create_pfs(hmp->spmp, pfs->name, strlen(pfs->name),
	    &error);
	if (error == 0) {
		atomic_set_int(&nip->flags, HAMMER2_INODE_NOSIDEQ);
		hammer2_inode_modify(nip);
		nchain = hammer2_inode_chain(nip, 0, HAMMER2_RESOLVE_ALWAYS);
		error = hammer2_chain_modify(nchain, mtid, 0, 0);
		KKASSERT(error == 0);
		nipdata = &nchain->data->ipdata;

		nip->meta.pfs_type = pfs->pfs_type;
		nip->meta.pfs_subtype = pfs->pfs_subtype;
		nip->meta.pfs_clid = pfs->pfs_clid;
		nip->meta.pfs_fsid = pfs->pfs_fsid;
		nip->meta.op_flags |= HAMMER2_OPFLAG_PFSROOT;

		nip->meta.comp_algo =
		    HAMMER2_ENC_ALGO(HAMMER2_COMP_DEFAULT);
		nip->meta.check_algo =
		    HAMMER2_ENC_ALGO(HAMMER2_CHECK_DEFAULT);

		if (strcasecmp(pfs->name, "boot") == 0)
			nip->meta.comp_algo =
			    HAMMER2_ENC_ALGO(HAMMER2_COMP_AUTOZERO);

		hammer2_chain_unlock(nchain);
		hammer2_inode_ref(nip);
		hammer2_inode_unlock(nip);
		hammer2_inode_chain_sync(nip);
		hammer2_inode_chain_flush(nip,
		    HAMMER2_XOP_INODE_STOP | HAMMER2_XOP_FSSYNC);
		hammer2_inode_drop(nip);

		force_local = (hmp->hflags & HMNT2_LOCAL) ? hmp : NULL;

		hammer2_chain_lock(nchain, HAMMER2_RESOLVE_ALWAYS);
		nipdata = &nchain->data->ipdata;
		debug_hprintf("ADD LOCAL PFS (IOCTL): %s\n", nipdata->filename);
		hammer2_pfsalloc(nchain, nipdata, force_local);

		hammer2_chain_unlock(nchain);
		hammer2_chain_drop(nchain);
	}
	hammer2_trans_done(hmp->spmp,
	    HAMMER2_TRANS_ISFLUSH | HAMMER2_TRANS_SIDEQ);

	return (error);
}

static int
hammer2_ioctl_pfs_delete(hammer2_inode_t *ip, void *data)
{
	hammer2_ioc_pfs_t *pfs = data;
	hammer2_dev_t *hmp;
	hammer2_pfs_t *spmp, *pmp;
	hammer2_xop_unlink_t *xop;
	hammer2_inode_t *dip;
	int error, i;

	hmp = ip->pmp->pfs_hmps[0];
	if (hmp == NULL)
		return (EINVAL);

	if (pfs->name[0] == 0)
		return (EINVAL);
	pfs->name[sizeof(pfs->name) - 1] = 0;

	if (hammer2_is_rdonly(ip->pmp->mp))
		return (EROFS);

	hammer2_lk_ex(&hammer2_mntlk);

	TAILQ_FOREACH(pmp, &hammer2_pfslist, mntentry) {
		for (i = 0; i < HAMMER2_MAXCLUSTER; ++i) {
			if (pmp->pfs_hmps[i] != hmp)
				continue;
			if (pmp->pfs_names[i] &&
			    strcmp(pmp->pfs_names[i], pfs->name) == 0)
				break;
		}
		if (i != HAMMER2_MAXCLUSTER)
			break;
	}

	if (pmp == NULL) {
		hammer2_lk_unlock(&hammer2_mntlk);
		return (ENOENT);
	}
	if (pmp->mp) {
		hammer2_lk_unlock(&hammer2_mntlk);
		return (EBUSY);
	}

	debug_hprintf("FOUND PFS %s CLINDEX %d\n", pfs->name, i);
	hammer2_pfsdealloc(pmp, i, 1);

	hammer2_lk_unlock(&hammer2_mntlk);

	spmp = hmp->spmp;
	dip = spmp->iroot;
	hammer2_trans_init(spmp, 0);
	hammer2_inode_lock(dip, 0);

	xop = hammer2_xop_alloc(dip, HAMMER2_XOP_MODIFYING);
	hammer2_xop_setname(&xop->head, pfs->name, strlen(pfs->name));
	xop->isdir = 2;
	xop->dopermanent = H2DOPERM_PERMANENT | H2DOPERM_FORCE;
	hammer2_xop_start(&xop->head, &hammer2_unlink_desc);
	error = hammer2_xop_collect(&xop->head, 0);
	error = hammer2_error_to_errno(error);
	hammer2_xop_retire(&xop->head, HAMMER2_XOPMASK_VOP);

	hammer2_inode_unlock(dip);
	hammer2_trans_done(spmp, HAMMER2_TRANS_SIDEQ);

	return (error);
}

static int
hammer2_ioctl_pfs_snapshot(hammer2_inode_t *ip, void *data)
{
	hammer2_ioc_pfs_t *pfs = data;
	hammer2_dev_t *hmp, *force_local;
	hammer2_pfs_t *pmp;
	hammer2_chain_t *chain, *nchain;
	hammer2_inode_t *nip;
	hammer2_inode_data_t *wipdata;
	hammer2_tid_t mtid, starting_inum;
	int error;

	pmp = ip->pmp;
	ip = pmp->iroot;

	hmp = pmp->pfs_hmps[0];
	if (hmp == NULL)
		return (EINVAL);

	if (pfs->name[0] == 0)
		return (EINVAL);
	pfs->name[sizeof(pfs->name) - 1] = 0;

	if (hammer2_is_rdonly(ip->pmp->mp))
		return (EROFS);

	hammer2_lk_ex(&hmp->bulklk);

	if (pfs->pfs_flags & HAMMER2_PFSFLAGS_NOSYNC) {
		hammer2_trans_init(pmp, 0);
	} else {
		hammer2_sync(pmp->mp, MNT_WAIT);
		hammer2_trans_init(pmp, HAMMER2_TRANS_ISFLUSH);
	}
	mtid = hammer2_trans_sub(pmp);
	hammer2_inode_lock(ip, 0);
	hammer2_inode_modify(ip);
	ip->meta.pfs_lsnap_tid = mtid;

	chain = hammer2_inode_chain(ip, 0, HAMMER2_RESOLVE_ALWAYS);
	hmp = chain->hmp;

	hammer2_chain_unlock(chain);
	nip = hammer2_inode_create_pfs(hmp->spmp, pfs->name, strlen(pfs->name),
	    &error);
	hammer2_chain_lock(chain, HAMMER2_RESOLVE_ALWAYS);

	if (nip) {
		atomic_set_int(&nip->flags, HAMMER2_INODE_NOSIDEQ);
		hammer2_inode_modify(nip);
		nchain = hammer2_inode_chain(nip, 0, HAMMER2_RESOLVE_ALWAYS);
		error = hammer2_chain_modify(nchain, mtid, 0, 0);
		KKASSERT(error == 0);
		wipdata = &nchain->data->ipdata;

		starting_inum = ip->pmp->inode_tid + 1;
		nip->meta.pfs_inum = starting_inum;
		nip->meta.pfs_type = HAMMER2_PFSTYPE_MASTER;
		nip->meta.pfs_subtype = HAMMER2_PFSSUBTYPE_SNAPSHOT;
		nip->meta.op_flags |= HAMMER2_OPFLAG_PFSROOT;
		nip->meta.pfs_lsnap_tid = mtid;
		nchain->bref.embed.stats = chain->bref.embed.stats;

		kern_uuidgen(&nip->meta.pfs_fsid, 1);
		kern_uuidgen(&nip->meta.pfs_clid, 1);
		nchain->bref.flags |= HAMMER2_BREF_FLAG_PFSROOT;

		wipdata->meta = nip->meta;
		hammer2_spin_ex(&pmp->blockset_spin);
		wipdata->u.blockset = pmp->pfs_iroot_blocksets[0];
		hammer2_spin_unex(&pmp->blockset_spin);

		KKASSERT(wipdata == &nchain->data->ipdata);

		hammer2_chain_unlock(nchain);
		hammer2_inode_ref(nip);
		hammer2_inode_unlock(nip);
		hammer2_inode_chain_sync(nip);
		hammer2_inode_chain_flush(nip,
		    HAMMER2_XOP_INODE_STOP | HAMMER2_XOP_FSSYNC);

		hammer2_inode_drop(nip);

		force_local = (hmp->hflags & HMNT2_LOCAL) ? hmp : NULL;

		hammer2_chain_lock(nchain, HAMMER2_RESOLVE_ALWAYS);
		wipdata = &nchain->data->ipdata;
		debug_hprintf("SNAPSHOT LOCAL PFS (IOCTL): %s\n",
		    wipdata->filename);
		hammer2_pfsalloc(nchain, wipdata, force_local);
		nchain->pmp->inode_tid = starting_inum;

		hammer2_chain_unlock(nchain);
		hammer2_chain_drop(nchain);
	}

	hammer2_chain_unlock(chain);
	hammer2_chain_drop(chain);

	hammer2_inode_unlock(ip);
	if (pfs->pfs_flags & HAMMER2_PFSFLAGS_NOSYNC)
		hammer2_trans_done(pmp, 0);
	else
		hammer2_trans_done(pmp,
		    HAMMER2_TRANS_ISFLUSH | HAMMER2_TRANS_SIDEQ);

	hammer2_lk_unlock(&hmp->bulklk);

	return (hammer2_error_to_errno(error));
}

static int
hammer2_ioctl_inode_get(hammer2_inode_t *ip, void *data)
{
	hammer2_ioc_inode_t *ino = data;

	hammer2_inode_lock(ip, HAMMER2_RESOLVE_SHARED);

	ino->data_count = hammer2_inode_data_count(ip);
	ino->inode_count = hammer2_inode_inode_count(ip);
	bzero(&ino->ip_data, sizeof(ino->ip_data));
	ino->ip_data.meta = ip->meta;

	hammer2_inode_unlock(ip);

	return (0);
}

static int
hammer2_ioctl_inode_set(hammer2_inode_t *ip, void *data)
{
	hammer2_ioc_inode_t *ino = data;

	if (hammer2_is_rdonly(ip->pmp->mp))
		return (EROFS);

	hammer2_trans_init(ip->pmp, 0);
	hammer2_inode_lock(ip, 0);

	if ((ino->flags & HAMMER2IOC_INODE_FLAG_CHECK) &&
	    ip->meta.check_algo != ino->ip_data.meta.check_algo) {
		hammer2_inode_modify(ip);
		ip->meta.check_algo = ino->ip_data.meta.check_algo;
	}
	if ((ino->flags & HAMMER2IOC_INODE_FLAG_COMP) &&
	    ip->meta.comp_algo != ino->ip_data.meta.comp_algo) {
		hammer2_inode_modify(ip);
		ip->meta.comp_algo = ino->ip_data.meta.comp_algo;
	}

	if ((ino->flags & HAMMER2IOC_INODE_FLAG_IQUOTA) &&
	    ip->meta.inode_quota != ino->ip_data.meta.inode_quota) {
		hammer2_inode_modify(ip);
		ip->meta.inode_quota = ino->ip_data.meta.inode_quota;
	}
	if ((ino->flags & HAMMER2IOC_INODE_FLAG_DQUOTA) &&
	    ip->meta.data_quota != ino->ip_data.meta.data_quota) {
		hammer2_inode_modify(ip);
		ip->meta.data_quota = ino->ip_data.meta.data_quota;
	}
	if ((ino->flags & HAMMER2IOC_INODE_FLAG_COPIES) &&
	    ip->meta.ncopies != ino->ip_data.meta.ncopies) {
		hammer2_inode_modify(ip);
		ip->meta.ncopies = ino->ip_data.meta.ncopies;
	}

	hammer2_inode_unlock(ip);
	hammer2_trans_done(ip->pmp, HAMMER2_TRANS_SIDEQ);

	return (0);
}

static int
hammer2_ioctl_debug_dump(hammer2_inode_t *ip, unsigned int flags __unused)
{
#ifdef HAMMER2_INVARIANTS
	hammer2_chain_t *chain;
	int i;

	for (i = 0; i < ip->cluster.nchains; ++i) {
		chain = ip->cluster.array[i].chain;
		if (chain) {
			hprintf("cluster #%d\n", i);
			hammer2_dump_chain(chain, 0, 0, -1, 'i');
		}
	}

	return (0);
#else
	return (EOPNOTSUPP);
#endif
}

static int
hammer2_ioctl_emerg_mode(hammer2_inode_t *ip, u_int mode)
{
	hammer2_pfs_t *pmp = ip->pmp;
	hammer2_dev_t *hmp;
	int i;

	if (mode) {
		hprintf("WARNING: Emergency mode enabled\n");
		atomic_set_int(&pmp->flags, HAMMER2_PMPF_EMERG);
	} else {
		hprintf("WARNING: Emergency mode disabled\n");
		atomic_clear_int(&pmp->flags, HAMMER2_PMPF_EMERG);
	}

	for (i = 0; i < HAMMER2_MAXCLUSTER; ++i) {
		hmp = pmp->pfs_hmps[i];
		if (hmp == NULL)
			continue;
		if (mode)
			atomic_set_int(&hmp->hflags, HMNT2_EMERG);
		else
			atomic_clear_int(&hmp->hflags, HMNT2_EMERG);
	}

	return (0);
}

static int
hammer2_ioctl_bulkfree_scan(hammer2_inode_t *ip, void *data)
{
	hammer2_ioc_bulkfree_t *bfi = data;
	hammer2_dev_t *hmp;
	hammer2_pfs_t *pmp;
	hammer2_chain_t *vchain;
	int error = 0, didsnap, etmp, i;

	ip = ip->pmp->iroot;

	hmp = ip->pmp->pfs_hmps[0];
	if (hmp == NULL)
		return (EINVAL);
	if (bfi == NULL)
		return (EINVAL);

	if (hammer2_is_rdonly(ip->pmp->mp))
		return (EROFS);

	hammer2_lk_ex(&hmp->bflk);

	hammer2_lk_ex(&hammer2_mntlk);
	TAILQ_FOREACH(pmp, &hammer2_pfslist, mntentry) {
		for (i = 0; i < HAMMER2_MAXCLUSTER; ++i) {
			if (pmp->pfs_hmps[i] != hmp)
				continue;
			etmp = hammer2_vfs_sync_pmp(pmp, MNT_WAIT);
			if (etmp && (error == 0 || error == ENOSPC))
				error = etmp;
			break;
		}
	}
	hammer2_lk_unlock(&hammer2_mntlk);

	if (error && error != ENOSPC)
		goto failed;

	if (error) {
		hprintf("WARNING: bulkfree forced to use live topology due to "
		    "ENOSPC\n");
		vchain = &hmp->vchain;
		hammer2_chain_ref(vchain);
		didsnap = 0;
	} else {
		vchain = hammer2_chain_bulksnap(hmp);
		didsnap = 1;
	}

	if (didsnap == 0)
		hammer2_trans_init(hmp->spmp, HAMMER2_TRANS_ISFLUSH);
	error = hammer2_bulkfree_pass(hmp, vchain, bfi);
	if (didsnap) {
		hammer2_chain_bulkdrop(vchain);
	} else {
		hammer2_chain_drop(vchain);
		hammer2_trans_done(hmp->spmp, HAMMER2_TRANS_ISFLUSH |
		    HAMMER2_TRANS_SIDEQ);
	}
	error = hammer2_error_to_errno(error);

failed:
	hammer2_lk_unlock(&hmp->bflk);
	return (error);
}

static int
hammer2_ioctl_destroy(hammer2_inode_t *ip, void *data)
{
#ifdef HAMMER2_INVARIANTS
	hammer2_ioc_destroy_t *iocd = data;
	hammer2_pfs_t *pmp = ip->pmp;
	int error;

	if (pmp->rdonly)
		return (EROFS);
	if (hammer2_is_rdonly(ip->pmp->mp))
		return (EROFS);

	switch (iocd->cmd) {
	case HAMMER2_DELETE_FILE:

		{
		hammer2_xop_unlink_t *xop;

		if (iocd->path[sizeof(iocd->path)-1]) {
			error = EINVAL;
			break;
		}
		if (ip->meta.type != HAMMER2_OBJTYPE_DIRECTORY) {
			error = EINVAL;
			break;
		}
		hammer2_trans_init(pmp, 0);
		hammer2_inode_lock(ip, 0);

		xop = hammer2_xop_alloc(ip, HAMMER2_XOP_MODIFYING);
		hammer2_xop_setname(&xop->head, iocd->path, strlen(iocd->path));
		xop->isdir = -1;
		xop->dopermanent =
		    H2DOPERM_PERMANENT | H2DOPERM_FORCE | H2DOPERM_IGNINO;
		hammer2_xop_start(&xop->head, &hammer2_unlink_desc);
		error = hammer2_xop_collect(&xop->head, 0);
		error = hammer2_error_to_errno(error);
		hammer2_xop_retire(&xop->head, HAMMER2_XOPMASK_VOP);

		hammer2_inode_unlock(ip);
		hammer2_trans_done(pmp, HAMMER2_TRANS_SIDEQ);
		}
		break;
	case HAMMER2_DELETE_INUM:

		{
		hammer2_xop_lookup_t *xop;

		if (iocd->inum < 1) {
			error = EINVAL;
			break;
		}
		hammer2_trans_init(pmp, 0);

		xop = hammer2_xop_alloc(pmp->iroot, HAMMER2_XOP_MODIFYING);
		xop->lhc = iocd->inum;
		hammer2_xop_start(&xop->head, &hammer2_delete_desc);
		error = hammer2_xop_collect(&xop->head, 0);
		error = hammer2_error_to_errno(error);
		hammer2_xop_retire(&xop->head, HAMMER2_XOPMASK_VOP);

		hammer2_trans_done(pmp, HAMMER2_TRANS_SIDEQ);
		}
		break;
	default:
		error = EINVAL;
		break;
	}

	return (error);
#else
	return (EOPNOTSUPP);
#endif
}

static int
hammer2_ioctl_growfs(hammer2_inode_t *ip, void *data)
{
	hammer2_ioc_growfs_t *grow = data;
	hammer2_dev_t *hmp;
	hammer2_off_t size, delta;
	hammer2_tid_t mtid;
	const struct g_consumer *cp;
	struct buf *bp;
	int i, error;
	daddr_t blkno;

	hmp = ip->pmp->pfs_hmps[0];
	if (hmp == NULL)
		return (EINVAL);

	if (hammer2_is_rdonly(ip->pmp->mp))
		return (EROFS);

	if (hmp->nvolumes > 1) {
		hprintf("growfs currently unsupported with multiple volumes\n");
		return (EOPNOTSUPP);
	}
	KKASSERT(hmp->total_size == hmp->voldata.volu_size);

	{
		struct block_device *bdev = hmp->volumes[0].dev
			? hmp->volumes[0].dev->bdev : NULL;
		KASSERTMSG(bdev, "NULL bdev");
		size = bdev_nr_bytes(bdev);
	}
	hprintf("growfs partition-auto to %016llx\n", (long long)size);

	grow->modified = 0;
	if (grow->size == 0) {
		grow->size = size;
	} else if (grow->size > size) {
		hprintf("growfs size %016llx exceeds device size %016llx\n",
		    (long long)grow->size, (long long)size);
		return (EINVAL);
	}

	grow->size &= ~HAMMER2_VOLUME_ALIGNMASK64;
	delta = grow->size - hmp->voldata.volu_size;

	if (grow->size > 0x7FFFFFFFFFFFFFFFLU) {
		hprintf("growfs failure, limit is 2^63 - 1 bytes\n");
		return (EINVAL);
	}

	if (grow->size < hmp->voldata.volu_size) {
		hprintf("growfs failure, would shrink from %016llx to %016llx\n",
		    (long long)hmp->voldata.volu_size, (long long)grow->size);
		return (EINVAL);
	}

	if (delta == 0) {
		hprintf("growfs - size did not change\n");
		return (0);
	}

	for (i = 0; i < HAMMER2_NUM_VOLHDRS; ++i) {
		if (i * HAMMER2_ZONE_BYTES64 < hmp->voldata.volu_size)
			continue;
		if (i * HAMMER2_ZONE_BYTES64 >= grow->size)
			break;
		hprintf("growfs - clear volhdr %d\n", i);
		{
		struct block_device *bdev = hmp->volumes[0].dev
			? hmp->volumes[0].dev->bdev : NULL;
		struct buffer_head *bh;

		blkno = i * HAMMER2_ZONE_BYTES64 / HAMMER2_VOLUME_BYTES;
		bh = __bread(bdev, blkno, HAMMER2_VOLUME_BYTES);
		if (!bh) {
			hprintf("growfs: __bread failed at zone %d\n", i);
			return EIO;
		}
		bzero(bh->b_data, HAMMER2_VOLUME_BYTES);
		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);
		brelse(bh);
		}
	}

	hammer2_trans_init(hmp->spmp, HAMMER2_TRANS_ISFLUSH);
	mtid = hammer2_trans_sub(hmp->spmp);

	hprintf("growfs - expand by %016llx to %016llx mtid %016llx\n",
	    (long long)delta, (long long)grow->size, (long long)mtid);

	hammer2_voldata_lock(hmp);
	hammer2_voldata_modify(hmp);

	hmp->voldata.volu_size = grow->size;
	hmp->voldata.total_size += delta;
	hmp->voldata.allocator_size += delta;
	hmp->voldata.allocator_free += delta;
	hmp->total_size += delta;
	hmp->volumes[0].size += delta;

	hammer2_voldata_unlock(hmp);

	hammer2_trans_done(hmp->spmp,
	    HAMMER2_TRANS_ISFLUSH | HAMMER2_TRANS_SIDEQ);
	grow->modified = 1;

	hammer2_sync(ip->pmp->mp, MNT_WAIT);

	return (0);
}

static int
hammer2_ioctl_volume_list(hammer2_inode_t *ip, void *data)
{
	hammer2_ioc_volume_list_t *vollist = data;
	hammer2_ioc_volume_t entry;
	hammer2_volume_t *vol;
	hammer2_dev_t *hmp = ip->pmp->pfs_hmps[0];
	int i, error = 0, cnt = 0;

	if (hmp == NULL)
		return (EINVAL);

	for (i = 0; i < hmp->nvolumes; ++i) {
		if (cnt >= vollist->nvolumes)
			break;
		vol = &hmp->volumes[i];
		bzero(&entry, sizeof(entry));

		entry.id = vol->id;
		bcopy(vol->dev->path, entry.path, sizeof(entry.path));
		entry.offset = vol->offset;
		entry.size = vol->size;
		error = copyout(&entry, &vollist->volumes[cnt], sizeof(entry));
		if (error)
			return (error);
		cnt++;
	}
	vollist->nvolumes = cnt;
	vollist->version = hmp->voldata.version;
	bcopy(ip->pmp->pfs_names[0], vollist->pfs_name,
	    sizeof(vollist->pfs_name));

	return (error);
}

static int
hammer2_ioctl_volume_list2(hammer2_inode_t *ip, void *data)
{
	hammer2_ioc_volume_list2_t *vollist = data;
	hammer2_ioc_volume2_t *entry;
	hammer2_volume_t *vol;
	hammer2_dev_t *hmp = ip->pmp->pfs_hmps[0];
	int i, error = 0, cnt = 0;

	if (hmp == NULL)
		return (EINVAL);

	for (i = 0; i < hmp->nvolumes; ++i) {
		if (cnt >= vollist->nvolumes)
			break;
		vol = &hmp->volumes[i];
		entry = &vollist->volumes[cnt];

		entry->id = vol->id;
		bcopy(vol->dev->path, entry->path, sizeof(entry->path));
		entry->offset = vol->offset;
		entry->size = vol->size;
		cnt++;
	}
	vollist->nvolumes = cnt;
	vollist->version = hmp->voldata.version;
	bcopy(ip->pmp->pfs_names[0], vollist->pfs_name,
	    sizeof(vollist->pfs_name));

	return (error);
}

int
hammer2_ioctl_impl(struct vnode *vp, unsigned long com, void *data, int fflag,
    struct ucred *cred)
{
	hammer2_inode_t *ip = VTOI(vp);
	int error;

	switch (com) {
	case HAMMER2IOC_VERSION_GET:
		error = hammer2_ioctl_version_get(ip, data);
		break;
	case HAMMER2IOC_RECLUSTER:
		error = hammer2_ioctl_recluster(ip, data);
		break;
	case HAMMER2IOC_PFS_GET:
		error = hammer2_ioctl_pfs_get(ip, data);
		break;
	case HAMMER2IOC_PFS_LOOKUP:
		error = hammer2_ioctl_pfs_lookup(ip, data);
		break;
	case HAMMER2IOC_PFS_CREATE:
		error = hammer2_ioctl_pfs_create(ip, data);
		break;
	case HAMMER2IOC_PFS_DELETE:
		error = hammer2_ioctl_pfs_delete(ip, data);
		break;
	case HAMMER2IOC_PFS_SNAPSHOT:
		error = hammer2_ioctl_pfs_snapshot(ip, data);
		break;
	case HAMMER2IOC_INODE_GET:
		error = hammer2_ioctl_inode_get(ip, data);
		break;
	case HAMMER2IOC_INODE_SET:
		error = hammer2_ioctl_inode_set(ip, data);
		break;
	case HAMMER2IOC_DEBUG_DUMP:
		error = hammer2_ioctl_debug_dump(ip, *(unsigned int *)data);
		break;
	case HAMMER2IOC_EMERG_MODE:
		error = hammer2_ioctl_emerg_mode(ip, *(unsigned int *)data);
		break;
	case HAMMER2IOC_BULKFREE_SCAN:
		error = hammer2_ioctl_bulkfree_scan(ip, data);
		break;
	case HAMMER2IOC_DESTROY:
		error = hammer2_ioctl_destroy(ip, data);
		break;
	case HAMMER2IOC_GROWFS:
		error = hammer2_ioctl_growfs(ip, data);
		break;
	case HAMMER2IOC_VOLUME_LIST:
		error = hammer2_ioctl_volume_list(ip, data);
		break;
	case HAMMER2IOC_VOLUME_LIST2:
		error = hammer2_ioctl_volume_list2(ip, data);
		break;
	case FIOSEEKDATA:
	case FIOSEEKHOLE:

		KKASSERT(ip->in_seek == 0);
		error = -EOPNOTSUPP;
		break;
	default:
		error = EOPNOTSUPP;
		break;
	}

	KKASSERT(ip->in_seek == 0);

	return (error);
}

int
hammer2_ioctl_linux(struct inode *inode, unsigned long com, void *data,
    int fflag)
{

	return (hammer2_ioctl_impl((void *)inode, com, data, fflag, NULL));
}
