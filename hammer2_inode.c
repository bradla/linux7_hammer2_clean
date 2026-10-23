#include "hammer2.h"
#include <linux/cred.h>
#include <linux/capability.h>

static void hammer2_inode_repoint(hammer2_inode_t *, hammer2_cluster_t *);
static void hammer2_inode_repoint_one(hammer2_inode_t *, hammer2_cluster_t *,
    int);

void
hammer2_inum_hash_init(hammer2_pfs_t *pmp)
{
	hammer2_inum_hash_t *hash;
	int i;

	for (i = 0; i < HAMMER2_INUMHASH_SIZE; ++i) {
		hash = &pmp->inumhash[i];
		hammer2_spin_init(&hash->spin, "h2mp_inum");
	}
}

void
hammer2_inum_hash_destroy(hammer2_pfs_t *pmp)
{
	hammer2_inum_hash_t *hash;
	int i;

	for (i = 0; i < HAMMER2_INUMHASH_SIZE; ++i) {
		hash = &pmp->inumhash[i];
		hammer2_spin_destroy(&hash->spin);
	}
}

static hammer2_depend_t *
hammer2_inode_setdepend_locked(hammer2_inode_t *ip, hammer2_depend_t *depend)
{
	hammer2_pfs_t *pmp = ip->pmp;
	hammer2_depend_t *dtmp;
	hammer2_inode_t *iptmp;
	int sanitychk = 0;

	if (ip->flags & HAMMER2_INODE_SYNCQ) {
		if (depend == (void *)-1 || depend == NULL)
			return ((void *)-1);
		depend->pass2 = 1;
		hammer2_trans_setflags(pmp, HAMMER2_TRANS_RESCAN);
		return (depend);
	}

	if (ip->flags & HAMMER2_INODE_SIDEQ) {

		if (depend == NULL) {
			depend = ip->depend;
		} else if (depend == (void *)-1) {
			depend = ip->depend;
			depend->pass2 = 1;
		} else if (depend != ip->depend) {
			dtmp = ip->depend;
			while ((iptmp = list_first_entry_or_null(&dtmp->sideq.head,
			    hammer2_inode_t, qentry)) != NULL) {
				if (iptmp == ip)
					sanitychk = 1;
				TAILQ_REMOVE(&dtmp->sideq, iptmp, qentry);
				TAILQ_INSERT_TAIL(&depend->sideq, iptmp, qentry);
				iptmp->depend = depend;
			}
			KKASSERT(sanitychk == 1);
			depend->count += dtmp->count;
			depend->pass2 |= dtmp->pass2;
			TAILQ_REMOVE(&pmp->depq, dtmp, entry);
			dtmp->count = 0;
			dtmp->pass2 = 0;
		}
	} else {

		hammer2_inode_ref(ip);
		atomic_set_int(&ip->flags, HAMMER2_INODE_SIDEQ);
		if (depend == NULL) {
			depend = &ip->depend_static;
			TAILQ_INSERT_TAIL(&pmp->depq, depend, entry);
		} else if (depend == (void *)-1) {
			depend = &ip->depend_static;
			depend->pass2 = 1;
			TAILQ_INSERT_TAIL(&pmp->depq, depend, entry);
		}
		TAILQ_INSERT_TAIL(&depend->sideq, ip, qentry);
		ip->depend = depend;
		++depend->count;
		++pmp->sideq_count;
	}

	if (ip->flags & HAMMER2_INODE_SYNCQ_PASS2)
		depend->pass2 = 1;
	if (depend->pass2)
		hammer2_trans_setflags(pmp, HAMMER2_TRANS_RESCAN);

	return (depend);
}

void
hammer2_inode_delayed_sideq(hammer2_inode_t *ip)
{
	hammer2_pfs_t *pmp = ip->pmp;

	if ((ip->flags & (HAMMER2_INODE_SYNCQ | HAMMER2_INODE_SIDEQ)) == 0) {
		hammer2_spin_ex(&pmp->list_spin);
		hammer2_inode_setdepend_locked(ip, NULL);
		hammer2_spin_unex(&pmp->list_spin);
	}
}

void
hammer2_inode_lock(hammer2_inode_t *ip, int how)
{
	hammer2_pfs_t *pmp;
	int error __unused;

	hammer2_inode_ref(ip);
	pmp = ip->pmp;

	if (how & HAMMER2_RESOLVE_SHARED) {
		hammer2_mtx_sh(&ip->lock);
		return;
	}

	hammer2_mtx_ex(&ip->lock);
	if (hammer2_mtx_refs(&ip->lock) > 1)
		return;
	hammer2_mtx_assert(&ip->lock, SA_XLOCKED | SA_NOTRECURSED);
	while ((ip->flags & HAMMER2_INODE_SYNCQ) && pmp) {
		hammer2_spin_ex(&pmp->list_spin);
		if (ip->flags & HAMMER2_INODE_SYNCQ) {
			atomic_set_int(&ip->flags, HAMMER2_INODE_SYNCQ_WAKEUP);
			TAILQ_REMOVE(&pmp->syncq, ip, qentry);
			TAILQ_INSERT_HEAD(&pmp->syncq, ip, qentry);
			hammer2_spin_unex(&pmp->list_spin);

			error = hammer2_mtx_sleep(&ip->flags, &ip->lock,
			    "h2sync", hz / 100);
			debug_hprintf("inum %016llx wokeup error %d\n",
			    (long long)ip->meta.inum, error);
			continue;
		}
		hammer2_spin_unex(&pmp->list_spin);
		hammer2_mtx_assert_ex(&ip->lock);
		break;
	}
}

void
hammer2_inode_lock4(hammer2_inode_t *ip1, hammer2_inode_t *ip2,
    hammer2_inode_t *ip3, hammer2_inode_t *ip4)
{
	hammer2_inode_t *ips[4], *iptmp, *ipslp;
	hammer2_depend_t *depend;
	hammer2_pfs_t *pmp;
	size_t count, i;

	pmp = ip1->pmp;
	KKASSERT(pmp == ip2->pmp);

	ips[0] = ip1;
	ips[1] = ip2;
	if (ip3 == NULL) {
		count = 2;
	} else if (ip4 == NULL) {
		count = 3;
		ips[2] = ip3;
		KKASSERT(pmp == ip3->pmp);
	} else {
		count = 4;
		ips[2] = ip3;
		ips[3] = ip4;
		KKASSERT(pmp == ip3->pmp);
		KKASSERT(pmp == ip4->pmp);
	}

	for (i = 0; i < count; ++i)
		hammer2_inode_ref(ips[i]);
restart:

	for (i = 0; i < count; ++i)
		hammer2_mtx_ex(&ips[i]->lock);

	hammer2_spin_ex(&pmp->list_spin);
	depend = NULL;
	ipslp = NULL;
	for (i = 0; i < count; ++i) {
		iptmp = ips[i];
		depend = hammer2_inode_setdepend_locked(iptmp, depend);
		if (iptmp->flags & HAMMER2_INODE_SYNCQ) {
			TAILQ_REMOVE(&pmp->syncq, iptmp, qentry);
			TAILQ_INSERT_HEAD(&pmp->syncq, iptmp, qentry);
			if (ipslp == NULL)
				ipslp = iptmp;
		}
	}
	hammer2_spin_unex(&pmp->list_spin);

	if (ipslp) {
		for (i = 0; i < count; ++i)
			hammer2_mtx_unlock(&ips[i]->lock);
		tsleep(&ipslp->flags, 0, "h2sync", 2);
		goto restart;
	}
}

void
hammer2_inode_unlock(hammer2_inode_t *ip)
{

	hammer2_spin_ex(&ip->pmp->list_spin);
	if (ip->flags & HAMMER2_INODE_SYNCQ_WAKEUP) {
		atomic_clear_int(&ip->flags, HAMMER2_INODE_SYNCQ_WAKEUP);
		hammer2_mtx_unlock(&ip->lock);
		hammer2_mtx_wakeup(&ip->flags);
	} else {
		hammer2_mtx_unlock(&ip->lock);
	}
	hammer2_spin_unex(&ip->pmp->list_spin);
	hammer2_inode_drop(ip);
}

void
hammer2_inode_depend(hammer2_inode_t *ip1, hammer2_inode_t *ip2)
{
	hammer2_pfs_t *pmp;
	hammer2_depend_t *depend;

	pmp = ip1->pmp;
	hammer2_spin_ex(&pmp->list_spin);
	depend = hammer2_inode_setdepend_locked(ip1, NULL);
	depend = hammer2_inode_setdepend_locked(ip2, depend);
	hammer2_spin_unex(&pmp->list_spin);
}

hammer2_chain_t *
hammer2_inode_chain(hammer2_inode_t *ip, int clindex, int how)
{
	hammer2_chain_t *chain;
	hammer2_cluster_t *cluster;

	hammer2_spin_sh(&ip->cluster_spin);
	cluster = &ip->cluster;
	if (clindex >= cluster->nchains)
		chain = NULL;
	else
		chain = cluster->array[clindex].chain;
	if (chain) {
		hammer2_chain_ref(chain);
		hammer2_spin_unsh(&ip->cluster_spin);
		hammer2_chain_lock(chain, how);
	} else {
		hammer2_spin_unsh(&ip->cluster_spin);
	}

	return (chain);
}

hammer2_chain_t *
hammer2_inode_chain_and_parent(hammer2_inode_t *ip, int clindex,
    hammer2_chain_t **parentp, int how)
{
	hammer2_chain_t *chain, *parent;

	for (;;) {
		hammer2_spin_sh(&ip->cluster_spin);
		if (clindex >= ip->cluster.nchains)
			chain = NULL;
		else
			chain = ip->cluster.array[clindex].chain;
		if (chain) {
			hammer2_chain_ref(chain);
			hammer2_spin_unsh(&ip->cluster_spin);
			hammer2_chain_lock(chain, how);
		} else {
			hammer2_spin_unsh(&ip->cluster_spin);
		}

		parent = chain->parent;
		if (parent) {
			hammer2_chain_ref(parent);
			hammer2_chain_unlock(chain);
			hammer2_chain_lock(parent, how);
			hammer2_chain_lock(chain, how);
		}
		if (ip->cluster.array[clindex].chain == chain &&
		    chain->parent == parent)
			break;

		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
		if (parent) {
			hammer2_chain_unlock(parent);
			hammer2_chain_drop(parent);
		}
	}
	*parentp = parent;

	return (chain);
}

static int
hammer2_inode_lock_temp_release(hammer2_inode_t *ip)
{
	return (hammer2_mtx_temp_release(&ip->lock));
}

static void
hammer2_inode_lock_temp_restore(hammer2_inode_t *ip, int ostate)
{
	hammer2_mtx_temp_restore(&ip->lock, ostate);
}

static __inline hammer2_inum_hash_t *
inumhash(hammer2_pfs_t *pmp, hammer2_tid_t inum)
{
	int hv;

	hv = (int)inum;
	return (&pmp->inumhash[hv & HAMMER2_INUMHASH_MASK]);
}

hammer2_inode_t *
hammer2_inode_lookup(hammer2_pfs_t *pmp, hammer2_tid_t inum)
{
	hammer2_inum_hash_t *hash;
	hammer2_inode_t *ip;

	KKASSERT(pmp);
	if (pmp->spmp_hmp) {
		ip = NULL;
	} else {
		hash = inumhash(pmp, inum);
		hammer2_spin_sh(&hash->spin);
		for (ip = hash->base; ip; ip = ip->next) {
			if (ip->meta.inum == inum) {
				hammer2_inode_ref(ip);
				break;
			}
		}
		hammer2_spin_unsh(&hash->spin);
	}

	return (ip);
}

void
hammer2_inode_ref(hammer2_inode_t *ip)
{
	atomic_add_int(&ip->refs, 1);
}

void
hammer2_inode_drop(hammer2_inode_t *ip)
{
	hammer2_pfs_t *pmp;
	hammer2_inum_hash_t *hash;
	hammer2_inode_t **xipp;
	unsigned int refs;

	while (ip) {
		refs = ip->refs;
		cpu_ccfence();
		if (refs == 1) {

			pmp = ip->pmp;
			KKASSERT(pmp);
			hash = inumhash(pmp, ip->meta.inum);

			hammer2_spin_ex(&hash->spin);
			if (atomic_cmpset_int(&ip->refs, 1, 0)) {
				KKASSERT(hammer2_mtx_refs(&ip->lock) == 0);
				if (ip->flags & HAMMER2_INODE_ONHASH) {
					xipp = &hash->base;
					while (*xipp != ip)
						xipp = &(*xipp)->next;
					*xipp = ip->next;
					ip->next = NULL;
					atomic_clear_int(&ip->flags,
					    HAMMER2_INODE_ONHASH);
				}
				hammer2_spin_unex(&hash->spin);
				ip->pmp = NULL;

				hammer2_inode_repoint(ip, NULL);
				hammer2_mtx_destroy(&ip->lock);
				hammer2_mtx_destroy(&ip->truncate_lock);
				hammer2_mtx_destroy(&ip->rmw_lock);
				hammer2_spin_destroy(&ip->cluster_spin);

				uma_zfree(hammer2_zone_inode, ip);
				atomic_add_int(&hammer2_count_inode_allocated,
				    -1);
				ip = NULL;
			} else {
				hammer2_spin_unex(&hash->spin);
			}
		} else {

			if (atomic_cmpset_int(&ip->refs, refs, refs - 1))
				break;
		}
	}
}

int
hammer2_igetv(hammer2_inode_t *ip, int flags, void *vpp)
{
	struct inode *inode;

	(void)flags;
	if (vpp)
		*(void **)vpp = NULL;
	if (ip == NULL || ip->pmp == NULL || ip->pmp->sb == NULL)
		return -EINVAL;

	inode = hammer2_iget(ip->pmp->sb, ip);
	if (IS_ERR(inode))
		return PTR_ERR(inode);
	if (vpp)
		*(struct inode **)vpp = inode;
	return 0;
}

hammer2_inode_t *
hammer2_inode_get(hammer2_pfs_t *pmp, hammer2_xop_head_t *xop,
    hammer2_tid_t inum, int idx)
{
	hammer2_inum_hash_t *hash;
	hammer2_inode_t *nip, *xip, **xipp;
	const hammer2_inode_data_t *iptmp, *nipdata;

	KKASSERT(xop == NULL ||
	    hammer2_cluster_type(&xop->cluster) == HAMMER2_BREF_TYPE_INODE);
	KKASSERT(pmp);

	if (xop) {
		iptmp = &((const hammer2_media_data_t *)hammer2_xop_gdata(xop))->ipdata;
		inum = iptmp->meta.inum;
		hammer2_xop_pdata(xop);
	}
again:
	nip = hammer2_inode_lookup(pmp, inum);
	if (nip) {

		if (xop) {
			if (hammer2_mtx_ex_try(&nip->lock) != 0) {
				hammer2_cluster_unhold(&xop->cluster);
				hammer2_mtx_ex(&nip->lock);
				hammer2_cluster_rehold(&xop->cluster);
			}
		} else {
			hammer2_mtx_ex(&nip->lock);
		}

		if (pmp->spmp_hmp == NULL &&
		    (nip->flags & HAMMER2_INODE_ONHASH) == 0) {
			hammer2_mtx_unlock(&nip->lock);
			hammer2_inode_drop(nip);
			goto again;
		}
		if (xop) {
			if (idx >= 0)
				hammer2_inode_repoint_one(nip, &xop->cluster,
				    idx);
			else
				hammer2_inode_repoint(nip, &xop->cluster);
		}
		return (nip);
	}

	nip = uma_zalloc(hammer2_zone_inode, M_WAITOK | M_ZERO);
	atomic_add_int(&hammer2_count_inode_allocated, 1);
	hammer2_spin_init(&nip->cluster_spin, "h2ip_cl");

	nip->cluster.pmp = pmp;
	if (xop) {
		nipdata = &((const hammer2_media_data_t *)hammer2_xop_gdata(xop))->ipdata;
		nip->meta = nipdata->meta;
		hammer2_xop_pdata(xop);
		hammer2_inode_repoint(nip, &xop->cluster);
	} else {
		nip->meta.inum = inum;
	}

	nip->pmp = pmp;

	nip->ipdep_idx = nip->meta.inum % HAMMER2_IHASH_SIZE;
	KKASSERT(nip->ipdep_idx >= 0 && nip->ipdep_idx < HAMMER2_IHASH_SIZE);

	nip->refs = 1;
	hammer2_mtx_init_recurse(&nip->lock, "h2ip");
	hammer2_mtx_init(&nip->truncate_lock, "h2ip_tr");
	hammer2_mtx_init(&nip->rmw_lock, "h2ip_rmw");
	hammer2_mtx_ex(&nip->lock);
	TAILQ_INIT(&nip->depend_static.sideq);

	if (pmp->spmp_hmp == NULL) {
		hash = inumhash(pmp, nip->meta.inum);
		hammer2_spin_ex(&hash->spin);
		for (xipp = &hash->base;
		    (xip = *xipp) != NULL;
		    xipp = &xip->next) {
			if (xip->meta.inum == nip->meta.inum) {
				hammer2_spin_unex(&hash->spin);
				hammer2_mtx_unlock(&nip->lock);
				hammer2_inode_drop(nip);
				goto again;
			}
		}
		nip->next = NULL;
		*xipp = nip;
		atomic_set_int(&nip->flags, HAMMER2_INODE_ONHASH);
		hammer2_spin_unex(&hash->spin);
	}

	return (nip);
}

hammer2_inode_t *
hammer2_inode_create_pfs(hammer2_pfs_t *spmp, const char *name, size_t name_len,
    int *errorp)
{
	hammer2_xop_create_t *xop;
	hammer2_xop_scanlhc_t *sxop;
	hammer2_inode_t *pip, *nip;
	hammer2_tid_t pip_inum;
	hammer2_key_t lhc, lhcbase;
	uint8_t pip_comp_algo, pip_check_algo;
	int error;

	pip = spmp->iroot;
	nip = NULL;

	lhc = hammer2_dirhash(name, name_len);
	*errorp = 0;

	hammer2_inode_lock(pip, 0);

	pip_comp_algo = pip->meta.comp_algo;
	pip_check_algo = pip->meta.check_algo;
	pip_inum = (pip == pip->pmp->iroot) ? 1 : pip->meta.inum;

	lhcbase = lhc;
	sxop = hammer2_xop_alloc(pip, HAMMER2_XOP_MODIFYING);
	sxop->lhc = lhc;
	hammer2_xop_start(&sxop->head, &hammer2_scanlhc_desc);
	while ((error = hammer2_xop_collect(&sxop->head, 0)) == 0) {
		if (lhc != sxop->head.cluster.focus->bref.key)
			break;
		++lhc;
	}
	hammer2_xop_retire(&sxop->head, HAMMER2_XOPMASK_VOP);
	if (error) {
		if (error != HAMMER2_ERROR_ENOENT)
			goto done2;
		++lhc;
		error = 0;
	}
	if ((lhcbase ^ lhc) & ~HAMMER2_DIRHASH_LOMASK) {
		error = HAMMER2_ERROR_ENOSPC;
		goto done2;
	}

	xop = hammer2_xop_alloc(pip, HAMMER2_XOP_MODIFYING);
	xop->lhc = lhc;
	xop->flags = HAMMER2_INSERT_PFSROOT;
	bzero(&xop->meta, sizeof(xop->meta));
	xop->meta.type = HAMMER2_OBJTYPE_DIRECTORY;
	xop->meta.inum = 1;
	xop->meta.iparent = pip_inum;

	xop->meta.comp_algo = pip_comp_algo;
	xop->meta.check_algo = pip_check_algo;
	xop->meta.version = HAMMER2_INODE_VERSION_ONE;
	hammer2_update_time(&xop->meta.ctime);
	xop->meta.mtime = xop->meta.ctime;
	xop->meta.atime = xop->meta.ctime;
	xop->meta.btime = xop->meta.ctime;
	xop->meta.mode = 0755;
	xop->meta.nlinks = 1;
	hammer2_xop_setname(&xop->head, name, name_len);
	xop->meta.name_len = name_len;
	xop->meta.name_key = lhc;
	KKASSERT(name_len < HAMMER2_INODE_MAXNAME);
	hammer2_xop_start(&xop->head, &hammer2_inode_create_desc);
	error = hammer2_xop_collect(&xop->head, 0);
	if (error) {
		*errorp = error;
		goto done;
	}

	nip = hammer2_inode_get(pip->pmp, &xop->head, -1, -1);
	nip->comp_heuristic = 0;
done:
	hammer2_xop_retire(&xop->head, HAMMER2_XOPMASK_VOP);
done2:
	hammer2_inode_unlock(pip);

	return (nip);
}

static uid_t
vop_helper_create_uid(void *mp, mode_t dmode, uid_t duid,
    struct ucred *cred, mode_t *modep)
{
	(void)mp;
#ifdef SUIDDIR
	if (0   &&
	    (dmode & S_ISUID) &&
	    duid != cred->cr_uid && duid) {
		*modep &= ~07111;
		return (duid);
	}
#endif
	return (cred->cr_uid);
}

hammer2_inode_t *
hammer2_inode_create_normal(hammer2_inode_t *pip, struct vattr *vap,
    struct ucred *cred, hammer2_key_t inum, int *errorp)
{
	hammer2_xop_create_t *xop;
	hammer2_inode_t *dip, *nip;
	hammer2_tid_t pip_inum;
	struct uuid pip_uid, pip_gid;
	uint32_t pip_mode;
	uint8_t pip_comp_algo, pip_check_algo;
	uid_t xuid;
	int error;

	dip = pip->pmp->iroot;
	KKASSERT(dip != NULL);

	*errorp = 0;

	pip_uid = pip->meta.uid;
	pip_gid = pip->meta.gid;
	pip_mode = pip->meta.mode;
	pip_comp_algo = pip->meta.comp_algo;
	pip_check_algo = pip->meta.check_algo;
	pip_inum = (pip == pip->pmp->iroot) ? 1 : pip->meta.inum;

	nip = hammer2_inode_get(dip->pmp, NULL, inum, -1);
	nip->comp_heuristic = 0;
	KKASSERT((nip->flags & HAMMER2_INODE_CREATING) == 0 &&
	    nip->cluster.nchains == 0);
	atomic_set_int(&nip->flags, HAMMER2_INODE_CREATING);

	nip->meta.type = hammer2_get_obj_type(vap->va_type);

	switch (nip->meta.type) {
	case HAMMER2_OBJTYPE_CDEV:
	case HAMMER2_OBJTYPE_BDEV:
		nip->meta.rmajor = major(vap->va_rdev);
		nip->meta.rminor = minor(vap->va_rdev);
		break;
	default:
		break;
	}

	KKASSERT(nip->meta.inum == inum);
	nip->meta.iparent = pip_inum;

	nip->meta.comp_algo = pip_comp_algo;
	nip->meta.check_algo = pip_check_algo;
	nip->meta.version = HAMMER2_INODE_VERSION_ONE;
	hammer2_update_time(&nip->meta.ctime);
	nip->meta.mtime = nip->meta.ctime;
	nip->meta.atime = nip->meta.ctime;
	nip->meta.btime = nip->meta.ctime;
	nip->meta.mode = vap->va_mode;

	nip->meta.nlinks = 1;

	if ((nip->meta.mode & S_ISGID) && !S_ISDIR(vap->va_mode) &&
	    (nip->meta.mode & S_IXGRP) && !capable(CAP_FSETID) &&
	    !in_group_p(make_kgid(&init_user_ns, vap->va_gid)))
		nip->meta.mode &= ~S_ISGID;

	xuid = hammer2_to_unix_xid(&pip_uid);
	xuid = vop_helper_create_uid(dip->pmp->mp, pip_mode, xuid, cred,
	    &vap->va_mode);
	if (vap->va_uid != (uid_t)VNOVAL)
		hammer2_guid_to_uuid(&nip->meta.uid, vap->va_uid);
	else
		hammer2_guid_to_uuid(&nip->meta.uid, xuid);

	if (vap->va_gid != (gid_t)VNOVAL)
		hammer2_guid_to_uuid(&nip->meta.gid, vap->va_gid);
	else
		nip->meta.gid = pip_gid;

	if (nip->meta.type == HAMMER2_OBJTYPE_REGFILE ||
	    nip->meta.type == HAMMER2_OBJTYPE_SOFTLINK)
		nip->meta.op_flags |= HAMMER2_OPFLAG_DIRECTDATA;

	xop = hammer2_xop_alloc(pip, HAMMER2_XOP_MODIFYING);
	xop->lhc = inum;
	xop->flags = 0;
	xop->meta = nip->meta;
	xop->meta.name_len = hammer2_xop_setname_inum(&xop->head, inum);
	xop->meta.name_key = inum;
	nip->meta.name_len = xop->meta.name_len;
	nip->meta.name_key = xop->meta.name_key;
	hammer2_inode_modify(nip);

	hammer2_xop_start(&xop->head, &hammer2_inode_create_det_desc);
	error = hammer2_xop_collect(&xop->head, 0);
	if (error) {
		*errorp = error;
		goto done;
	}

	hammer2_inode_repoint(nip, &xop->head.cluster);
done:
	hammer2_xop_retire(&xop->head, HAMMER2_XOPMASK_VOP);

	return (nip);
}

int
hammer2_dirent_create(hammer2_inode_t *dip, const char *name, size_t name_len,
    hammer2_key_t inum, uint8_t type)
{
	hammer2_xop_mkdirent_t *xop;
	hammer2_xop_scanlhc_t *sxop;
	hammer2_key_t lhc, lhcbase;
	int error = 0;

	KKASSERT(name != NULL);
	lhc = hammer2_dirhash(name, name_len);

	hammer2_inode_modify(dip);

	lhcbase = lhc;
	sxop = hammer2_xop_alloc(dip, HAMMER2_XOP_MODIFYING);
	sxop->lhc = lhc;
	hammer2_xop_start(&sxop->head, &hammer2_scanlhc_desc);
	while ((error = hammer2_xop_collect(&sxop->head, 0)) == 0) {
		if (lhc != sxop->head.cluster.focus->bref.key)
			break;
		++lhc;
	}
	hammer2_xop_retire(&sxop->head, HAMMER2_XOPMASK_VOP);
	if (error) {
		if (error != HAMMER2_ERROR_ENOENT)
			goto done2;
		++lhc;
		error = 0;
	}
	if ((lhcbase ^ lhc) & ~HAMMER2_DIRHASH_LOMASK) {
		error = HAMMER2_ERROR_ENOSPC;
		goto done2;
	}

	xop = hammer2_xop_alloc(dip, HAMMER2_XOP_MODIFYING);
	xop->lhc = lhc;
	bzero(&xop->dirent, sizeof(xop->dirent));
	xop->dirent.inum = inum;
	xop->dirent.type = type;
	xop->dirent.namlen = name_len;
	KKASSERT(name_len < HAMMER2_INODE_MAXNAME);
	hammer2_xop_setname(&xop->head, name, name_len);
	hammer2_xop_start(&xop->head, &hammer2_inode_mkdirent_desc);
	error = hammer2_xop_collect(&xop->head, 0);
	hammer2_xop_retire(&xop->head, HAMMER2_XOPMASK_VOP);
done2:
	return (hammer2_error_to_errno(error));
}

static void
hammer2_inode_repoint(hammer2_inode_t *ip, hammer2_cluster_t *cluster)
{
	hammer2_chain_t *dropch[HAMMER2_MAXCLUSTER];
	hammer2_chain_t *ochain, *nchain;
	int i;

	bzero(dropch, sizeof(dropch));

	hammer2_spin_ex(&ip->cluster_spin);
	for (i = 0; i < ip->ccache_nchains; ++i) {
		dropch[i] = ip->ccache[i].chain;
		ip->ccache[i].flags = 0;
		ip->ccache[i].chain = NULL;
	}
	ip->ccache_nchains = 0;
	hammer2_spin_unex(&ip->cluster_spin);

	while (--i >= 0)
		if (dropch[i]) {
			hammer2_chain_drop(dropch[i]);
			dropch[i] = NULL;
		}

	hammer2_spin_ex(&ip->cluster_spin);
	for (i = 0; cluster && i < cluster->nchains; ++i) {

		nchain = cluster->array[i].chain;
		if (i < ip->cluster.nchains) {
			ochain = ip->cluster.array[i].chain;
			if (ochain == nchain)
				continue;
		} else {
			ochain = NULL;
		}

		ip->cluster.array[i].chain = nchain;
		if (nchain)
			hammer2_chain_ref(nchain);
		dropch[i] = ochain;
	}

	while (i < ip->cluster.nchains) {
		nchain = ip->cluster.array[i].chain;
		if (nchain)
			ip->cluster.array[i].chain = NULL;
		dropch[i] = nchain;
		++i;
	}

	if (cluster) {
		ip->cluster.nchains = cluster->nchains;
		ip->cluster.focus = cluster->focus;
		hammer2_assert_cluster(&ip->cluster);
	} else {
		ip->cluster.nchains = 0;
		ip->cluster.focus = NULL;
	}

	hammer2_spin_unex(&ip->cluster_spin);

	while (--i >= 0)
		if (dropch[i])
			hammer2_chain_drop(dropch[i]);
}

static void
hammer2_inode_repoint_one(hammer2_inode_t *ip, hammer2_cluster_t *cluster,
    int idx)
{
	hammer2_chain_t *dropch[HAMMER2_MAXCLUSTER];
	hammer2_chain_t *ochain, *nchain;
	int i;

	hammer2_spin_ex(&ip->cluster_spin);
	for (i = 0; i < ip->ccache_nchains; ++i) {
		dropch[i] = ip->ccache[i].chain;
		ip->ccache[i].chain = NULL;
	}
	ip->ccache_nchains = 0;
	hammer2_spin_unex(&ip->cluster_spin);

	while (--i >= 0)
		if (dropch[i])
			hammer2_chain_drop(dropch[i]);

	hammer2_spin_ex(&ip->cluster_spin);
	KKASSERT(idx < cluster->nchains);
	if (idx < ip->cluster.nchains) {
		ochain = ip->cluster.array[idx].chain;
		nchain = cluster->array[idx].chain;
	} else {
		ochain = NULL;
		nchain = cluster->array[idx].chain;
		for (i = ip->cluster.nchains; i <= idx; ++i)
			bzero(&ip->cluster.array[i],
			    sizeof(ip->cluster.array[i]));
		ip->cluster.nchains = idx + 1;
		hammer2_assert_cluster(&ip->cluster);
	}
	if (ochain != nchain) {

		ip->cluster.array[idx].chain = nchain;
	}
	hammer2_spin_unex(&ip->cluster_spin);

	if (ochain != nchain) {
		if (nchain)
			hammer2_chain_ref(nchain);
		if (ochain)
			hammer2_chain_drop(ochain);
	}
}

hammer2_key_t
hammer2_inode_data_count(const hammer2_inode_t *ip)
{
	hammer2_chain_t *chain;
	hammer2_key_t count = 0;
	int i;

	for (i = 0; i < ip->cluster.nchains; ++i) {
		chain = ip->cluster.array[i].chain;
		if (chain == NULL)
			continue;
		if (count < chain->bref.embed.stats.data_count)
			count = chain->bref.embed.stats.data_count;
	}

	return (count);
}

hammer2_key_t
hammer2_inode_inode_count(const hammer2_inode_t *ip)
{
	hammer2_chain_t *chain;
	hammer2_key_t count = 0;
	int i;

	for (i = 0; i < ip->cluster.nchains; ++i) {
		chain = ip->cluster.array[i].chain;
		if (chain == NULL)
			continue;
		if (count < chain->bref.embed.stats.inode_count)
			count = chain->bref.embed.stats.inode_count;
	}

	return (count);
}

int
hammer2_inode_unlink_finisher(hammer2_inode_t *ip, struct inode **vprecyclep)
{
	struct inode *vp;
	uint64_t ctime;
	int has_links;

	has_links = ip->meta.type != HAMMER2_OBJTYPE_DIRECTORY &&
	    (int64_t)ip->meta.nlinks > 1;

	if (has_links) {
		hammer2_update_time(&ctime);
	} else {
		atomic_set_int(&ip->flags, HAMMER2_INODE_ISUNLINKED);

		vp = ip->vp;
		cpu_ccfence();

		if (vp == NULL) {
			if ((ip->flags & HAMMER2_INODE_DELETING) == 0) {
				atomic_set_int(&ip->flags,
				    HAMMER2_INODE_DELETING);
				hammer2_inode_delayed_sideq(ip);
			}
			return (0);
		}

		KKASSERT(vprecyclep == NULL);
	}

	hammer2_inode_modify(ip);
	if (has_links) {
		--ip->meta.nlinks;
		ip->meta.ctime = ctime;
	} else {
		ip->meta.nlinks = 0;
	}

	return (0);
}

void
hammer2_inode_modify(hammer2_inode_t *ip)
{
	atomic_set_int(&ip->flags, HAMMER2_INODE_MODIFIED);

	if (ip->pmp && (ip->flags & HAMMER2_INODE_NOSIDEQ) == 0)
		hammer2_inode_delayed_sideq(ip);
}

int
hammer2_inode_chain_sync(hammer2_inode_t *ip)
{
	hammer2_xop_fsync_t *xop;
	int error = 0;
	int cleared_dd = 0, saved_ipflags = 0;

	if (ip->flags & (HAMMER2_INODE_RESIZED | HAMMER2_INODE_MODIFIED)) {
		xop = hammer2_xop_alloc(ip, HAMMER2_XOP_MODIFYING);
		xop->clear_directdata = 0;
		if (ip->flags & HAMMER2_INODE_RESIZED) {
			if ((ip->meta.op_flags & HAMMER2_OPFLAG_DIRECTDATA) &&
			    ip->meta.size > HAMMER2_EMBEDDED_BYTES) {
				ip->meta.op_flags &= ~HAMMER2_OPFLAG_DIRECTDATA;
				xop->clear_directdata = 1;
			}
			xop->osize = ip->osize;
		} else {
			xop->osize = ip->meta.size;
		}
		xop->ipflags = ip->flags;
		xop->meta = ip->meta;
		saved_ipflags = ip->flags;
		atomic_clear_int(&ip->flags,
		    HAMMER2_INODE_RESIZED | HAMMER2_INODE_MODIFIED);
		hammer2_xop_start(&xop->head, &hammer2_inode_chain_sync_desc);
		error = hammer2_xop_collect(&xop->head, 0);
		cleared_dd = xop->clear_directdata;
		hammer2_xop_retire(&xop->head, HAMMER2_XOPMASK_VOP);
		if (error == HAMMER2_ERROR_ENOENT)
			error = 0;
		if (error) {
			hprintf("unable to fsync inode %016llx (error %02x)\n",
			    (long long)ip->meta.inum, error);

			if (cleared_dd)
				ip->meta.op_flags |= HAMMER2_OPFLAG_DIRECTDATA;
			atomic_set_int(&ip->flags, saved_ipflags &
			    (HAMMER2_INODE_RESIZED | HAMMER2_INODE_MODIFIED));
			hammer2_inode_delayed_sideq(ip);
		}
	}

	return (error);
}

int
hammer2_inode_chain_ins(hammer2_inode_t *ip)
{
	hammer2_xop_create_t *xop;
	int error = 0;

	if (ip->flags & HAMMER2_INODE_CREATING) {
		atomic_clear_int(&ip->flags, HAMMER2_INODE_CREATING);
		xop = hammer2_xop_alloc(ip, HAMMER2_XOP_MODIFYING);
		xop->lhc = ip->meta.inum;
		xop->flags = 0;
		hammer2_xop_start(&xop->head, &hammer2_inode_create_ins_desc);
		error = hammer2_xop_collect(&xop->head, 0);
		hammer2_xop_retire(&xop->head, HAMMER2_XOPMASK_VOP);
		if (error == HAMMER2_ERROR_ENOENT)
			error = 0;
		if (error) {
			hprintf("backend unable to insert inum %016llx (error %02x)\n",
			    (long long)ip->meta.inum, error);

			atomic_set_int(&ip->flags, HAMMER2_INODE_CREATING);
			hammer2_inode_delayed_sideq(ip);
		}
	}
	return (error);
}

int
hammer2_inode_chain_des(hammer2_inode_t *ip)
{
	hammer2_xop_destroy_t *xop;
	int error = 0;

	if (ip->flags & HAMMER2_INODE_DELETING) {
		atomic_clear_int(&ip->flags,
		    HAMMER2_INODE_DELETING | HAMMER2_INODE_ISUNLINKED);
		xop = hammer2_xop_alloc(ip, HAMMER2_XOP_MODIFYING);
		hammer2_xop_start(&xop->head, &hammer2_inode_destroy_desc);
		error = hammer2_xop_collect(&xop->head, 0);
		hammer2_xop_retire(&xop->head, HAMMER2_XOPMASK_VOP);
		if (error == HAMMER2_ERROR_ENOENT)
			error = 0;
		if (error) {
			hprintf("backend unable to delete inode %016llx (error %02x)\n",
			    (long long)ip->meta.inum, error);

			atomic_set_int(&ip->flags, HAMMER2_INODE_DELETING);
			hammer2_inode_delayed_sideq(ip);
		}
	}
	return (error);
}

int
hammer2_inode_chain_flush(hammer2_inode_t *ip, int flags)
{
	hammer2_xop_flush_t *xop;
	int error;

	atomic_clear_int(&ip->flags, HAMMER2_INODE_DIRTYDATA);
	xop = hammer2_xop_alloc(ip, HAMMER2_XOP_MODIFYING | flags);
	hammer2_xop_start(&xop->head, &hammer2_inode_flush_desc);
	error = hammer2_xop_collect(&xop->head, HAMMER2_XOP_COLLECT_WAITALL);
	hammer2_xop_retire(&xop->head, HAMMER2_XOPMASK_VOP);
	if (error == HAMMER2_ERROR_ENOENT)
		error = 0;

	return (error);
}

int
hammer2_checkpath(const hammer2_inode_t *dip, hammer2_inode_t *tdip)
{
	hammer2_xop_lookup_t *xop;
	hammer2_chain_t *chain;
	hammer2_inode_t *ip;
	const hammer2_inode_data_t *ipdata;
	hammer2_tid_t inum = tdip->meta.inum;
	int error;

	KKASSERT(dip != tdip);
	KKASSERT(dip->meta.type == HAMMER2_OBJTYPE_DIRECTORY);
	KKASSERT(tdip->meta.type == HAMMER2_OBJTYPE_DIRECTORY);

	while (inum != 1) {
		if (inum == tdip->meta.inum) {
			ip = tdip;
			hammer2_inode_ref(ip);
		} else {
			ip = hammer2_inode_lookup(dip->pmp, inum);
		}
		if (ip) {
			if (dip->meta.inum == ip->meta.iparent) {
				hammer2_inode_drop(ip);
				return (EINVAL);
			}
			inum = ip->meta.iparent;
			hammer2_inode_drop(ip);
			ip = NULL;
			continue;
		}
		if (inum == tdip->meta.inum) {
			chain = hammer2_inode_chain(tdip, 0,
			    HAMMER2_RESOLVE_ALWAYS | HAMMER2_RESOLVE_SHARED);
			if (chain) {
				ipdata = &chain->data->ipdata;
				if (dip->meta.inum == ipdata->meta.iparent) {
					hammer2_chain_unlock(chain);
					hammer2_chain_drop(chain);
					return (EINVAL);
				}
				inum = ipdata->meta.iparent;
				hammer2_chain_unlock(chain);
				hammer2_chain_drop(chain);
			} else {
				return (EIO);
			}
		} else {
			xop = hammer2_xop_alloc(dip->pmp->iroot, 0);
			xop->lhc = inum;
			hammer2_xop_start(&xop->head, &hammer2_lookup_desc);
			error = hammer2_xop_collect(&xop->head, 0);
			if (error == 0) {
				ipdata = &((const hammer2_media_data_t *)
				    hammer2_xop_gdata(&xop->head))->ipdata;
				if (dip->meta.inum == ipdata->meta.iparent) {
					hammer2_xop_pdata(&xop->head);
					hammer2_xop_retire(&xop->head,
					    HAMMER2_XOPMASK_VOP);
					return (EINVAL);
				}
				inum = ipdata->meta.iparent;
				hammer2_xop_pdata(&xop->head);
			}
			hammer2_xop_retire(&xop->head, HAMMER2_XOPMASK_VOP);
			if (error) {
				error = hammer2_error_to_errno(error);
				switch (error) {
				case ENOENT:
					hprintf("inum %016llx chain not found\n",
					    (long long)inum);
					return (0);
				default:
					return (error);
				}
			}
		}
	}

	return (0);
}
