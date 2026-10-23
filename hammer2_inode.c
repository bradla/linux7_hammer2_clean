#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/types.h>
#include <sys/lock.h>
#include <sys/uuid.h>
#include <sys/vnode.h>

#include "hammer2.h"

#define INODE_DEBUG	0

void
hammer2_inum_hash_init(hammer2_pfs_t *pmp)
{
	hammer2_inum_hash_t *hash;
	int i;

	for (i = 0; i < HAMMER2_INUMHASH_SIZE; ++i) {
		hash = &pmp->inumhash[i];
		hammer2_spin_init(&hash->spin, "h2inum");
	}
}

static __noinline
hammer2_depend_t *
hammer2_inode_setdepend_locked(hammer2_inode_t *ip, hammer2_depend_t *depend)
{
	hammer2_pfs_t *pmp = ip->pmp;
	hammer2_depend_t *dtmp;
	hammer2_inode_t *iptmp;

	if (ip->flags & HAMMER2_INODE_SYNCQ) {
		if (depend == (void *)-1 ||
		    depend == NULL) {
			return ((void *)-1);
		}
		depend->pass2 = 1;
		hammer2_trans_setflags(pmp, HAMMER2_TRANS_RESCAN);

		return depend;
	}

	if (ip->flags & HAMMER2_INODE_SIDEQ) {
		if (depend == NULL) {
			depend = ip->depend;
		} else if (depend == (void *)-1) {
			depend = ip->depend;
			depend->pass2 = 1;
		} else if (depend != ip->depend) {
#ifdef INVARIANTS
			int sanitychk = 0;
#endif
			dtmp = ip->depend;
			while ((iptmp = TAILQ_FIRST(&dtmp->sideq)) != NULL) {
#ifdef INVARIANTS
				if (iptmp == ip)
					sanitychk = 1;
#endif
				TAILQ_REMOVE(&dtmp->sideq, iptmp, entry);
				TAILQ_INSERT_TAIL(&depend->sideq, iptmp, entry);
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
		TAILQ_INSERT_TAIL(&depend->sideq, ip, entry);
		ip->depend = depend;
		++depend->count;
		++pmp->sideq_count;
	}

	if (ip->flags & HAMMER2_INODE_SYNCQ_PASS2)
		depend->pass2 = 1;
	if (depend->pass2)
		hammer2_trans_setflags(pmp, HAMMER2_TRANS_RESCAN);

	return depend;
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

	hammer2_inode_ref(ip);
	pmp = ip->pmp;

	if (how & HAMMER2_RESOLVE_SHARED) {
		hammer2_mtx_sh(&ip->lock);
		return;
	}

	hammer2_mtx_ex(&ip->lock);
	if (hammer2_mtx_refs(&ip->lock) > 1)
		return;
	while ((ip->flags & HAMMER2_INODE_SYNCQ) && pmp) {
		hammer2_spin_ex(&pmp->list_spin);
		if (ip->flags & HAMMER2_INODE_SYNCQ) {
			tsleep_interlock(&ip->flags, 0);
			atomic_set_int(&ip->flags, HAMMER2_INODE_SYNCQ_WAKEUP);
			TAILQ_REMOVE(&pmp->syncq, ip, entry);
			TAILQ_INSERT_HEAD(&pmp->syncq, ip, entry);
			hammer2_spin_unex(&pmp->list_spin);
			hammer2_mtx_unlock(&ip->lock);
			tsleep(&ip->flags, PINTERLOCKED, "h2sync", 0);
			hammer2_mtx_ex(&ip->lock);
			continue;
		}
		hammer2_spin_unex(&pmp->list_spin);
		break;
	}
}

void
hammer2_inode_lock4(hammer2_inode_t *ip1, hammer2_inode_t *ip2,
		    hammer2_inode_t *ip3, hammer2_inode_t *ip4)
{
	hammer2_inode_t *ips[4];
	hammer2_inode_t *iptmp;
	hammer2_inode_t *ipslp;
	hammer2_depend_t *depend;
	hammer2_pfs_t *pmp;
	size_t count;
	size_t i;

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
	for (i = 0; i < count; ++i) {
		hammer2_mtx_ex(&ips[i]->lock);
	}

	hammer2_spin_ex(&pmp->list_spin);
	depend = NULL;
	ipslp = NULL;
	for (i = 0; i < count; ++i) {
		iptmp = ips[i];
		depend = hammer2_inode_setdepend_locked(iptmp, depend);
		if (iptmp->flags & HAMMER2_INODE_SYNCQ) {
			TAILQ_REMOVE(&pmp->syncq, iptmp, entry);
			TAILQ_INSERT_HEAD(&pmp->syncq, iptmp, entry);
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
	if (ip->flags & HAMMER2_INODE_SYNCQ_WAKEUP) {
		atomic_clear_int(&ip->flags, HAMMER2_INODE_SYNCQ_WAKEUP);
		hammer2_mtx_unlock(&ip->lock);
		wakeup(&ip->flags);
	} else {
		hammer2_mtx_unlock(&ip->lock);
	}
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
	return chain;
}

hammer2_chain_t *
hammer2_inode_chain_and_parent(hammer2_inode_t *ip, int clindex,
			       hammer2_chain_t **parentp, int how)
{
	hammer2_chain_t *chain;
	hammer2_chain_t *parent;

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
		    chain->parent == parent) {
			break;
		}

		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
		if (parent) {
			hammer2_chain_unlock(parent);
			hammer2_chain_drop(parent);
		}
	}
	*parentp = parent;

	return chain;
}

hammer2_mtx_state_t
hammer2_inode_lock_temp_release(hammer2_inode_t *ip)
{
	return hammer2_mtx_temp_release(&ip->lock);
}

void
hammer2_inode_lock_temp_restore(hammer2_inode_t *ip, hammer2_mtx_state_t ostate)
{
	hammer2_mtx_temp_restore(&ip->lock, ostate);
}

int
hammer2_inode_lock_upgrade(hammer2_inode_t *ip)
{
	int wasexclusive;

	if (mtx_islocked_ex(&ip->lock)) {
		wasexclusive = 1;
	} else {
		hammer2_mtx_unlock(&ip->lock);
		hammer2_mtx_ex(&ip->lock);
		wasexclusive = 0;
	}
	return wasexclusive;
}

void
hammer2_inode_lock_downgrade(hammer2_inode_t *ip, int wasexclusive)
{
	if (wasexclusive == 0)
		hammer2_mtx_downgrade(&ip->lock);
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
	return(ip);
}

void
hammer2_inode_ref(hammer2_inode_t *ip)
{
	atomic_add_int(&ip->refs, 1);
	if (hammer2_debug & 0x80000) {
		kprintf("INODE+1 %p (%d->%d)\n", ip, ip->refs - 1, ip->refs);
		print_backtrace(8);
	}
}

void
hammer2_inode_drop(hammer2_inode_t *ip)
{
	hammer2_pfs_t *pmp;
	u_int refs;

	while (ip) {
		if (hammer2_debug & 0x80000) {
			kprintf("INODE-1 %p (%d->%d)\n",
				ip, ip->refs, ip->refs - 1);
			print_backtrace(8);
		}
		refs = ip->refs;
		cpu_ccfence();
		if (refs == 1) {
			hammer2_inum_hash_t *hash;
			hammer2_inode_t **xipp;

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
					atomic_add_long(&pmp->inum_count, -1);
					atomic_clear_int(&ip->flags,
						     HAMMER2_INODE_ONHASH);
				}
				hammer2_spin_unex(&hash->spin);

				ip->pmp = NULL;

				hammer2_inode_repoint(ip, NULL);

				kfree_obj(ip, pmp->minode);
				atomic_add_long(&pmp->inmem_inodes, -1);
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

struct vnode *
hammer2_igetv(hammer2_inode_t *ip, int *errorp)
{
	hammer2_pfs_t *pmp;
	struct vnode *vp;

	pmp = ip->pmp;
	KKASSERT(pmp != NULL);
	*errorp = 0;

	for (;;) {
		int wasexclusive;

		vp = ip->vp;
		if (vp) {
			hammer2_mtx_state_t ostate;

			vhold(vp);
			ostate = hammer2_inode_lock_temp_release(ip);
			if (vget(vp, LK_EXCLUSIVE)) {
				vdrop(vp);
				hammer2_inode_lock_temp_restore(ip, ostate);
				continue;
			}
			hammer2_inode_lock_temp_restore(ip, ostate);
			vdrop(vp);
			if (ip->vp != vp) {
				kprintf("hammer2: igetv race %p/%p\n",
					ip->vp, vp);
				vput(vp);
				continue;
			}
			*errorp = 0;
			break;
		}

		*errorp = getnewvnode(VT_HAMMER2, pmp->mp, &vp, 0, 0);
		if (*errorp) {
			kprintf("hammer2: igetv getnewvnode failed %d\n",
				*errorp);
			vp = NULL;
			break;
		}

		wasexclusive = hammer2_inode_lock_upgrade(ip);
		if (ip->vp != NULL) {
			vp->v_type = VBAD;
			vx_put(vp);
			hammer2_inode_lock_downgrade(ip, wasexclusive);
			continue;
		}

		switch (ip->meta.type) {
		case HAMMER2_OBJTYPE_DIRECTORY:
			vp->v_type = VDIR;
			break;
		case HAMMER2_OBJTYPE_REGFILE:
			vp->v_type = VREG;
			vsetflags(vp, VKVABIO);
			vinitvmio(vp, ip->meta.size,
				  HAMMER2_LBUFSIZE,
				  (int)ip->meta.size & HAMMER2_LBUFMASK);
			break;
		case HAMMER2_OBJTYPE_SOFTLINK:
			vp->v_type = VLNK;
			vsetflags(vp, VKVABIO);
			vinitvmio(vp, ip->meta.size,
				  HAMMER2_LBUFSIZE,
				  (int)ip->meta.size & HAMMER2_LBUFMASK);
			break;
		case HAMMER2_OBJTYPE_CDEV:
			vp->v_type = VCHR;
		case HAMMER2_OBJTYPE_BDEV:
			vp->v_ops = &pmp->mp->mnt_vn_spec_ops;
			if (ip->meta.type != HAMMER2_OBJTYPE_CDEV)
				vp->v_type = VBLK;
			addaliasu(vp,
				  ip->meta.rmajor,
				  ip->meta.rminor);
			break;
		case HAMMER2_OBJTYPE_FIFO:
			vp->v_type = VFIFO;
			vp->v_ops = &pmp->mp->mnt_vn_fifo_ops;
			break;
		case HAMMER2_OBJTYPE_SOCKET:
			vp->v_type = VSOCK;
			break;
		default:
			panic("hammer2: unhandled objtype %d",
			      ip->meta.type);
			break;
		}

		if (ip == pmp->iroot)
			vsetflags(vp, VROOT);

		vp->v_data = ip;
		ip->vp = vp;
		hammer2_inode_ref(ip);
		hammer2_inode_lock_downgrade(ip, wasexclusive);
		vx_downgrade(vp);
		break;
	}

	if (hammer2_debug & 0x0002) {
		kprintf("igetv vp %p refs 0x%08x aux 0x%08x\n",
			vp, vp->v_refcnt, vp->v_auxrefs);
	}
	return (vp);
}

hammer2_inode_t *
hammer2_inode_get(hammer2_pfs_t *pmp, hammer2_xop_head_t *xop,
		  hammer2_tid_t inum, int idx)
{
	hammer2_inode_t *nip;
	const hammer2_inode_data_t *iptmp;
	const hammer2_inode_data_t *nipdata;

	KKASSERT(xop == NULL ||
		 hammer2_cluster_type(&xop->cluster) ==
		 HAMMER2_BREF_TYPE_INODE);
	KKASSERT(pmp);

	if (xop) {
		iptmp = &hammer2_xop_gdata(xop)->ipdata;
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
		return nip;
	}

	nip = kmalloc_obj(sizeof(*nip), pmp->minode, M_WAITOK | M_ZERO);
	hammer2_spin_init(&nip->cluster_spin, "h2clspin");
	atomic_add_long(&pmp->inmem_inodes, 1);

	{
		hammer2_inode_t *nnip = nip;
		nip->ihash = (int)hammer2_icrc32(&nnip, sizeof(nnip));
	}

	nip->cluster.refs = 1;
	nip->cluster.pmp = pmp;
	nip->cluster.flags |= HAMMER2_CLUSTER_INODE;
	if (xop) {
		nipdata = &hammer2_xop_gdata(xop)->ipdata;
		nip->meta = nipdata->meta;
		hammer2_xop_pdata(xop);
		hammer2_inode_repoint(nip, &xop->cluster);
	} else {
		nip->meta.inum = inum;
	}

	nip->pmp = pmp;

	nip->refs = 1;
	hammer2_mtx_init(&nip->lock, "h2inode");
	hammer2_mtx_init(&nip->truncate_lock, "h2trunc");
	hammer2_mtx_ex(&nip->lock);
	TAILQ_INIT(&nip->depend_static.sideq);

	if (pmp->spmp_hmp == NULL) {
		hammer2_inum_hash_t *hash;
		hammer2_inode_t *xip;
		hammer2_inode_t **xipp;

		hash = inumhash(pmp, nip->meta.inum);
		hammer2_spin_ex(&hash->spin);
		for (xipp = &hash->base;
		     (xip = *xipp) != NULL;
		     xipp = &xip->next)
		{
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
		atomic_add_long(&pmp->inum_count, 1);
		hammer2_spin_unex(&hash->spin);
	}
	return (nip);
}

hammer2_inode_t *
hammer2_inode_create_pfs(hammer2_pfs_t *spmp,
		     const char *name, size_t name_len,
		     int *errorp)
{
	hammer2_xop_create_t *xop;
	hammer2_inode_t *pip;
	hammer2_inode_t *nip;
	int error;
	uint8_t pip_comp_algo;
	uint8_t pip_check_algo;
	hammer2_tid_t pip_inum;
	hammer2_key_t lhc;

	pip = spmp->iroot;
	nip = NULL;

	lhc = hammer2_dirhash(name, name_len);
	*errorp = 0;

	hammer2_inode_lock(pip, 0);

	pip_comp_algo = pip->meta.comp_algo;
	pip_check_algo = pip->meta.check_algo;
	pip_inum = (pip == pip->pmp->iroot) ? 1 : pip->meta.inum;

	{
		hammer2_xop_scanlhc_t *sxop;
		hammer2_key_t lhcbase;

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
	xop->meta.mode = 0755;
	xop->meta.nlinks = 1;

	hammer2_xop_setname(&xop->head, name, name_len);
	xop->meta.name_len = name_len;
	xop->meta.name_key = lhc;
	KKASSERT(name_len < HAMMER2_INODE_MAXNAME);

	hammer2_xop_start(&xop->head, &hammer2_inode_create_desc);

	error = hammer2_xop_collect(&xop->head, 0);
#if INODE_DEBUG
	kprintf("CREATE INODE %*.*s\n",
		(int)name_len, (int)name_len, name);
#endif

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

hammer2_inode_t *
hammer2_inode_create_normal(hammer2_inode_t *pip,
			    struct vattr *vap, struct ucred *cred,
			    hammer2_key_t inum, int *errorp)
{
	hammer2_xop_create_t *xop;
	hammer2_inode_t *dip;
	hammer2_inode_t *nip;
	int error;
	uid_t xuid;
	uuid_t pip_uid;
	uuid_t pip_gid;
	uint32_t pip_mode;
	uint8_t pip_comp_algo;
	uint8_t pip_check_algo;
	hammer2_tid_t pip_inum;

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
		nip->meta.rmajor = vap->va_rmajor;
		nip->meta.rminor = vap->va_rminor;
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
	nip->meta.mode = vap->va_mode;
	nip->meta.nlinks = 1;

	xuid = hammer2_to_unix_xid(&pip_uid);
	xuid = vop_helper_create_uid(dip->pmp->mp, pip_mode,
				     xuid, cred,
				     &vap->va_mode);
	if (vap->va_vaflags & VA_UID_UUID_VALID)
		nip->meta.uid = vap->va_uid_uuid;
	else if (vap->va_uid != (uid_t)VNOVAL)
		hammer2_guid_to_uuid(&nip->meta.uid, vap->va_uid);
	else
		hammer2_guid_to_uuid(&nip->meta.uid, xuid);

	if (vap->va_vaflags & VA_GID_UUID_VALID)
		nip->meta.gid = vap->va_gid_uuid;
	else if (vap->va_gid != (gid_t)VNOVAL)
		hammer2_guid_to_uuid(&nip->meta.gid, vap->va_gid);
	else
		nip->meta.gid = pip_gid;

	if (nip->meta.type == HAMMER2_OBJTYPE_REGFILE ||
	    nip->meta.type == HAMMER2_OBJTYPE_SOFTLINK) {
		nip->meta.op_flags |= HAMMER2_OPFLAG_DIRECTDATA;
	}

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
#if INODE_DEBUG
	kprintf("create inode type %d error %d\n", nip->meta.type, error);
#endif

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
	hammer2_key_t lhc;
	int error;

	lhc = 0;
	error = 0;

	KKASSERT(name != NULL);
	lhc = hammer2_dirhash(name, name_len);

	hammer2_inode_modify(dip);

	{
		hammer2_xop_scanlhc_t *sxop;
		hammer2_key_t lhcbase;

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
	error = hammer2_error_to_errno(error);

	return error;
}

void
hammer2_inode_repoint(hammer2_inode_t *ip, hammer2_cluster_t *cluster)
{
	hammer2_chain_t *dropch[HAMMER2_MAXCLUSTER];
	hammer2_chain_t *ochain;
	hammer2_chain_t *nchain;
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

	while (--i >= 0) {
		if (dropch[i]) {
			hammer2_chain_drop(dropch[i]);
			dropch[i] = NULL;
		}
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
		ip->cluster.array[i].flags &= ~HAMMER2_CITEM_INVALID;
		ip->cluster.array[i].flags |= cluster->array[i].flags &
					      HAMMER2_CITEM_INVALID;
		if (nchain)
			hammer2_chain_ref(nchain);
		dropch[i] = ochain;
	}

	while (i < ip->cluster.nchains) {
		nchain = ip->cluster.array[i].chain;
		if (nchain) {
			ip->cluster.array[i].chain = NULL;
			ip->cluster.array[i].flags |= HAMMER2_CITEM_INVALID;
		}
		dropch[i] = nchain;
		++i;
	}

	if (cluster) {
		ip->cluster.nchains = cluster->nchains;
		ip->cluster.focus = cluster->focus;
		ip->cluster.flags = cluster->flags & ~HAMMER2_CLUSTER_LOCKED;
	} else {
		ip->cluster.nchains = 0;
		ip->cluster.focus = NULL;
		ip->cluster.flags &= ~HAMMER2_CLUSTER_ZFLAGS;
	}

	hammer2_spin_unex(&ip->cluster_spin);

	while (--i >= 0) {
		if (dropch[i])
			hammer2_chain_drop(dropch[i]);
	}
}

void
hammer2_inode_repoint_one(hammer2_inode_t *ip, hammer2_cluster_t *cluster,
			  int idx)
{
	hammer2_chain_t *dropch[HAMMER2_MAXCLUSTER];
	hammer2_chain_t *ochain;
	hammer2_chain_t *nchain;
	int i;

	hammer2_spin_ex(&ip->cluster_spin);
	for (i = 0; i < ip->ccache_nchains; ++i) {
		dropch[i] = ip->ccache[i].chain;
		ip->ccache[i].chain = NULL;
	}
	ip->ccache_nchains = 0;
	hammer2_spin_unex(&ip->cluster_spin);

	while (--i >= 0) {
		if (dropch[i])
			hammer2_chain_drop(dropch[i]);
	}

	hammer2_spin_ex(&ip->cluster_spin);
	KKASSERT(idx < cluster->nchains);
	if (idx < ip->cluster.nchains) {
		ochain = ip->cluster.array[idx].chain;
		nchain = cluster->array[idx].chain;
	} else {
		ochain = NULL;
		nchain = cluster->array[idx].chain;
		for (i = ip->cluster.nchains; i <= idx; ++i) {
			bzero(&ip->cluster.array[i],
			      sizeof(ip->cluster.array[i]));
			ip->cluster.array[i].flags |= HAMMER2_CITEM_INVALID;
		}
		ip->cluster.nchains = idx + 1;
	}
	if (ochain != nchain) {
		ip->cluster.array[idx].chain = nchain;
		ip->cluster.array[idx].flags &= ~HAMMER2_CITEM_INVALID;
		ip->cluster.array[idx].flags |= cluster->array[idx].flags &
						HAMMER2_CITEM_INVALID;
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
		if ((chain = ip->cluster.array[i].chain) != NULL) {
			if (count < chain->bref.embed.stats.data_count)
				count = chain->bref.embed.stats.data_count;
		}
	}
	return count;
}

hammer2_key_t
hammer2_inode_inode_count(const hammer2_inode_t *ip)
{
	hammer2_chain_t *chain;
	hammer2_key_t count = 0;
	int i;

	for (i = 0; i < ip->cluster.nchains; ++i) {
		if ((chain = ip->cluster.array[i].chain) != NULL) {
			if (count < chain->bref.embed.stats.inode_count)
				count = chain->bref.embed.stats.inode_count;
		}
	}
	return count;
}

int
hammer2_inode_unlink_finisher(hammer2_inode_t *ip, struct vnode **vprecyclep)
{
	struct vnode *vp;
	uint64_t ctime;

	if ((int64_t)ip->meta.nlinks > 1) {
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
			return 0;
		}

		if (vprecyclep) {
			vhold(vp);
			*vprecyclep = vp;
		}
	}

	hammer2_inode_modify(ip);
	if ((int64_t)ip->meta.nlinks > 1) {
		--ip->meta.nlinks;
		ip->meta.ctime = ctime;
	} else {
		ip->meta.nlinks = 0;
	}

	return 0;
}

void
hammer2_inode_vprecycle(struct vnode *vp)
{
	if (vget(vp, LK_EXCLUSIVE) == 0) {
		vfinalize(vp);
		hammer2_knote(vp, NOTE_DELETE);
		vdrop(vp);
		vput(vp);
	} else {
		vdrop(vp);
	}
}

void
hammer2_inode_modify(hammer2_inode_t *ip)
{
	atomic_set_int(&ip->flags, HAMMER2_INODE_MODIFIED);
	if (ip->vp)
		vsetisdirty(ip->vp);
	if (ip->pmp && (ip->flags & HAMMER2_INODE_NOSIDEQ) == 0)
		hammer2_inode_delayed_sideq(ip);
}

int
hammer2_inode_chain_sync(hammer2_inode_t *ip)
{
	int error;

	error = 0;
	if (ip->flags & (HAMMER2_INODE_RESIZED | HAMMER2_INODE_MODIFIED)) {
		hammer2_xop_fsync_t *xop;

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

		atomic_clear_int(&ip->flags, HAMMER2_INODE_RESIZED |
					     HAMMER2_INODE_MODIFIED);
		hammer2_xop_start(&xop->head, &hammer2_inode_chain_sync_desc);
		error = hammer2_xop_collect(&xop->head, 0);
		hammer2_xop_retire(&xop->head, HAMMER2_XOPMASK_VOP);
		if (error == HAMMER2_ERROR_ENOENT)
			error = 0;
		if (error) {
			kprintf("hammer2: unable to fsync inode %p\n", ip);
		}
	}
	return error;
}

int
hammer2_inode_chain_ins(hammer2_inode_t *ip)
{
	int error;

	error = 0;
	if (ip->flags & HAMMER2_INODE_CREATING) {
		hammer2_xop_create_t *xop;

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
			kprintf("hammer2: backend unable to "
				"insert inode %p %ld\n", ip, (long)ip->meta.inum);
		}
	}
	return error;
}

int
hammer2_inode_chain_des(hammer2_inode_t *ip)
{
	int error;

	error = 0;
	if (ip->flags & HAMMER2_INODE_DELETING) {
		hammer2_xop_destroy_t *xop;

		atomic_clear_int(&ip->flags, HAMMER2_INODE_DELETING |
					     HAMMER2_INODE_ISUNLINKED);
		xop = hammer2_xop_alloc(ip, HAMMER2_XOP_MODIFYING);
		hammer2_xop_start(&xop->head, &hammer2_inode_destroy_desc);
		error = hammer2_xop_collect(&xop->head, 0);
		hammer2_xop_retire(&xop->head, HAMMER2_XOPMASK_VOP);

		if (error == HAMMER2_ERROR_ENOENT)
			error = 0;
		if (error) {
			kprintf("hammer2: backend unable to "
				"delete inode %p %ld\n", ip, (long)ip->meta.inum);
		}
	}
	return error;
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

	return error;
}
