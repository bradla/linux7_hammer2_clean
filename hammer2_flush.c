#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/types.h>
#include <sys/lock.h>
#include <sys/vnode.h>
#include <sys/buf.h>

#include "hammer2.h"

#define HAMMER2_FLUSH_DEPTH_LIMIT	60

struct hammer2_flush_info {
	hammer2_chain_t *parent;
	int		depth;
	int		error;
	int		flags;
#ifdef HAMMER2_SCAN_DEBUG
	long		scan_count;
	long		scan_mod_count;
	long		scan_upd_count;
	long		scan_onf_count;
	long		scan_del_count;
	long		scan_btype[7];
#endif
};

typedef struct hammer2_flush_info hammer2_flush_info_t;

static int hammer2_flush_core(hammer2_flush_info_t *info,
				hammer2_chain_t *chain, int flags);
static int hammer2_flush_recurse(hammer2_chain_t *child, void *data);

void
hammer2_trans_manage_init(hammer2_pfs_t *pmp)
{
}

void
hammer2_trans_init(hammer2_pfs_t *pmp, uint32_t flags)
{
	uint32_t oflags;
	uint32_t nflags;
	int dowait;

	for (;;) {
		oflags = pmp->trans.flags;
		cpu_ccfence();
		dowait = 0;

		if (flags & HAMMER2_TRANS_ISFLUSH) {
			if (oflags & HAMMER2_TRANS_ISFLUSH) {
				nflags = oflags | HAMMER2_TRANS_WAITING;
				dowait = 1;
			} else {
				nflags = (oflags | flags) + 1;
			}
		} else if (flags & HAMMER2_TRANS_BUFCACHE) {
			nflags = (oflags | flags) + 1;
		} else {
			nflags = (oflags | flags) + 1;
		}
		if (dowait)
			tsleep_interlock(&pmp->trans.sync_wait, 0);
		if (atomic_cmpset_int(&pmp->trans.flags, oflags, nflags)) {
			if (dowait == 0)
				break;
			tsleep(&pmp->trans.sync_wait, PINTERLOCKED,
			       "h2trans", hz);
		} else {
			cpu_pause();
		}
	}

#if 0
	while ((flags & HAMMER2_TRANS_ISFLUSH) &&
	       (flags & HAMMER2_TRANS_COPYQ)) {
		oflags = pmp->trans.flags;
		cpu_ccfence();
		if ((oflags & HAMMER2_TRANS_MASK) == 1)
			break;
		nflags = oflags | HAMMER2_TRANS_WAITING;
		tsleep_interlock(&pmp->trans.sync_wait, 0);
		if (atomic_cmpset_int(&pmp->trans.flags, oflags, nflags)) {
			tsleep(&pmp->trans.sync_wait, PINTERLOCKED,
			       "h2trans2", hz);
		}
	}
#endif
}

hammer2_tid_t
hammer2_trans_sub(hammer2_pfs_t *pmp)
{
	hammer2_tid_t mtid;

	mtid = atomic_fetchadd_64(&pmp->modify_tid, 1);

	return (mtid);
}

void
hammer2_trans_setflags(hammer2_pfs_t *pmp, uint32_t flags)
{
	atomic_set_int(&pmp->trans.flags, flags);
}

void
hammer2_trans_clearflags(hammer2_pfs_t *pmp, uint32_t flags)
{
	uint32_t oflags;
	uint32_t nflags;

	for (;;) {
		oflags = pmp->trans.flags;
		cpu_ccfence();
		nflags = oflags & ~flags;
		if (atomic_cmpset_int(&pmp->trans.flags, oflags, nflags)) {
			if ((oflags ^ nflags) & HAMMER2_TRANS_WAITING)
				wakeup(&pmp->trans.sync_wait);
			break;
		}
		cpu_pause();
	}
}

void
hammer2_trans_done(hammer2_pfs_t *pmp, uint32_t flags)
{
	uint32_t oflags;
	uint32_t nflags;

#if 0
	if ((flags & HAMMER2_TRANS_SIDEQ) &&
	    pmp->sideq_count > hammer2_limit_dirty_inodes / 2 &&
	    pmp->sideq_count > (pmp->inum_count >> 3) &&
	    pmp->mp) {
		speedup_syncer(pmp->mp);
	}
#endif

	for (;;) {
		oflags = pmp->trans.flags;
		cpu_ccfence();
		KKASSERT(oflags & HAMMER2_TRANS_MASK);

		nflags = (oflags - 1) & ~flags;
		if (flags & HAMMER2_TRANS_ISFLUSH) {
			nflags &= ~HAMMER2_TRANS_WAITING;
		}
		if ((oflags & (HAMMER2_TRANS_ISFLUSH|HAMMER2_TRANS_MASK)) ==
		    (HAMMER2_TRANS_ISFLUSH|2)) {
			nflags &= ~HAMMER2_TRANS_WAITING;
		}
		if (atomic_cmpset_int(&pmp->trans.flags, oflags, nflags)) {
			if ((oflags ^ nflags) & HAMMER2_TRANS_WAITING)
				wakeup(&pmp->trans.sync_wait);
			break;
		}
		cpu_pause();
	}
}

hammer2_tid_t
hammer2_trans_newinum(hammer2_pfs_t *pmp)
{
	hammer2_tid_t tid;

	tid = atomic_fetchadd_64(&pmp->inode_tid, 1);

	return tid;
}

void
hammer2_trans_assert_strategy(hammer2_pfs_t *pmp)
{
#if 0
	KKASSERT((pmp->trans.flags & HAMMER2_TRANS_ISFLUSH) == 0);
#endif
}

int
hammer2_flush(hammer2_chain_t *chain, int flags)
{
	hammer2_flush_info_t info;
	int loops;

	bzero(&info, sizeof(info));
	info.flags = flags & ~HAMMER2_FLUSH_TOP;

	if ((info.parent = chain->parent) != NULL)
		hammer2_chain_ref(info.parent);

	hammer2_chain_ref(chain);
	loops = 0;

	for (;;) {
		if (info.parent != chain->parent) {
			if (hammer2_debug & 0x0040) {
				kprintf("LOST CHILD4 %p->%p "
					"(actual parent %p)\n",
					info.parent, chain, chain->parent);
			}
			hammer2_chain_drop(info.parent);
			info.parent = chain->parent;
			hammer2_chain_ref(info.parent);
		}
		if (hammer2_flush_core(&info, chain, flags) == 0)
			break;

		if (++loops % 1000 == 0) {
			kprintf("hammer2_flush: excessive loops on %p\n",
				chain);
			if (hammer2_debug & 0x100000)
				Debugger("hell4");
		}
	}
#ifdef HAMMER2_SCAN_DEBUG
	if (info.scan_count >= 10)
	kprintf("hammer2_flush: scan_count %ld (%ld,%ld,%ld,%ld) "
		"bt(%ld,%ld,%ld,%ld,%ld,%ld)\n",
		info.scan_count,
		info.scan_mod_count,
		info.scan_upd_count,
		info.scan_onf_count,
		info.scan_del_count,
		info.scan_btype[1],
		info.scan_btype[2],
		info.scan_btype[3],
		info.scan_btype[4],
		info.scan_btype[5],
		info.scan_btype[6]);
#endif
	hammer2_chain_drop(chain);
	if (info.parent)
		hammer2_chain_drop(info.parent);
	return (info.error);
}

static int
hammer2_flush_core(hammer2_flush_info_t *info, hammer2_chain_t *chain,
		   int flags)
{
	hammer2_chain_t *parent;
	hammer2_dev_t *hmp;
	int save_error;
	int retry;

	retry = 0;

	if ((chain->flags & HAMMER2_CHAIN_FLUSH_MASK) == 0)
		return 0;

	hmp = chain->hmp;

	parent = info->parent;
	KKASSERT(chain->parent == parent);

	if ((chain->flags & HAMMER2_CHAIN_PFSBOUNDARY) &&
	    (flags & HAMMER2_FLUSH_ALL) == 0 &&
	    (flags & HAMMER2_FLUSH_TOP) == 0 &&
	    chain->pmp && chain->pmp->mp) {
		if (chain->flags & (HAMMER2_CHAIN_ONFLUSH |
				    HAMMER2_CHAIN_DESTROY |
				    HAMMER2_CHAIN_MODIFIED)) {
			hammer2_chain_setflush(parent);
		}
		goto done;
	} else if (chain->bref.type == HAMMER2_BREF_TYPE_INODE &&
		   (flags & HAMMER2_FLUSH_INODE_STOP) &&
		   (flags & HAMMER2_FLUSH_ALL) == 0 &&
		   (flags & HAMMER2_FLUSH_TOP) == 0 &&
		   chain->pmp && chain->pmp->mp) {
		goto done;
#if 0
		if (chain->flags & (HAMMER2_CHAIN_ONFLUSH |
				    HAMMER2_CHAIN_DESTROY |
				    HAMMER2_CHAIN_MODIFIED)) {
			if (parent)
				hammer2_chain_setflush(parent);
		}
#endif
	} else if (info->depth == HAMMER2_FLUSH_DEPTH_LIMIT) {
		panic("hammer2: flush depth limit");
	} else if (chain->flags & (HAMMER2_CHAIN_ONFLUSH |
				   HAMMER2_CHAIN_DESTROY)) {
		atomic_clear_int(&chain->flags, HAMMER2_CHAIN_ONFLUSH);
		save_error = info->error;
		info->error = 0;
		info->parent = chain;

		hammer2_spin_ex(&chain->core.spin);
		RB_SCAN(hammer2_chain_tree, &chain->core.rbtree,
			NULL, hammer2_flush_recurse, info);
		if (chain->flags & HAMMER2_CHAIN_ONFLUSH) {
			atomic_clear_int(&chain->flags, HAMMER2_CHAIN_ONFLUSH);
			RB_SCAN(hammer2_chain_tree, &chain->core.rbtree,
				NULL, hammer2_flush_recurse, info);
		}
		hammer2_spin_unex(&chain->core.spin);
		info->parent = parent;

		if (info->error)
			hammer2_chain_setflush(chain);
		info->error |= save_error;

		if (chain->parent != parent) {
			kprintf("LOST CHILD2 %p->%p (actual parent %p)\n",
				parent, chain, chain->parent);
			goto done;
		}
	}

	hammer2_chain_ref_hold(chain);
	hammer2_chain_unlock(chain);
	if (parent)
		hammer2_chain_lock(parent, HAMMER2_RESOLVE_ALWAYS);
	hammer2_chain_lock(chain, HAMMER2_RESOLVE_MAYBE);
	hammer2_chain_drop_unhold(chain);

	if ((parent && parent->error) || chain->error) {
		kprintf("hammer2: chain error during flush\n");
		info->error |= chain->error;
		if (parent) {
			info->error |= parent->error;
			hammer2_chain_unlock(parent);
		}
		goto done;
	}

	if (chain->parent != parent) {
		if (hammer2_debug & 0x0040) {
			kprintf("LOST CHILD3 %p->%p (actual parent %p)\n",
				parent, chain, chain->parent);
		}
		KKASSERT(parent != NULL);
		hammer2_chain_unlock(parent);
		retry = 1;
		goto done;
	}

	if (parent && (parent->flags & HAMMER2_CHAIN_DESTROY))
		atomic_set_int(&chain->flags, HAMMER2_CHAIN_DESTROY);

	if (chain->flags & HAMMER2_CHAIN_MODIFIED) {
		KKASSERT((chain->flags & HAMMER2_CHAIN_UPDATE) ||
			 chain->parent == NULL);
		atomic_clear_int(&chain->flags, HAMMER2_CHAIN_MODIFIED);
		atomic_add_long(&hammer2_count_modified_chains, -1);

		if (chain->pmp)
			hammer2_pfs_memory_wakeup(chain->pmp, -1);

#if 0
		if ((chain->flags & HAMMER2_CHAIN_UPDATE) == 0 &&
		    chain != &hmp->vchain &&
		    chain != &hmp->fchain) {
			atomic_set_int(&chain->flags, HAMMER2_CHAIN_UPDATE);
		}
#endif

		if (hammer2_debug & 0x1000) {
			kprintf("Flush %p.%d %016jx/%d data=%016jx\n",
				chain, chain->bref.type,
				(uintmax_t)chain->bref.key,
				chain->bref.keybits,
				(uintmax_t)chain->bref.data_off);
		}

		switch(chain->bref.type) {
		case HAMMER2_BREF_TYPE_FREEMAP:
			KKASSERT(hmp->vchain.flags & HAMMER2_CHAIN_MODIFIED);
			KKASSERT(chain == &hmp->fchain);
			hmp->voldata.freemap_tid = chain->bref.mirror_tid;
			if (hammer2_debug & 0x8000) {
				kprintf("sync freemap mirror_tid %08jx\n",
					(intmax_t)chain->bref.mirror_tid);
			}

			++hmp->voldata.mirror_tid;
			break;
		case HAMMER2_BREF_TYPE_VOLUME:
			hammer2_chain_lock(&hmp->fchain,
					   HAMMER2_RESOLVE_ALWAYS);
			hammer2_voldata_lock(hmp);
			if (hammer2_debug & 0x8000) {
				kprintf("sync volume  mirror_tid %08jx\n",
					(intmax_t)chain->bref.mirror_tid);
			}

			if (hmp->voldata.mirror_tid < chain->bref.mirror_tid) {
				hmp->voldata.mirror_tid =
					chain->bref.mirror_tid;
			}

			KKASSERT(chain->data != NULL);
			KKASSERT(chain->dio == NULL);

			hmp->voldata.icrc_sects[HAMMER2_VOL_ICRC_SECT1]=
				hammer2_icrc32(
					(char *)&hmp->voldata +
					 HAMMER2_VOLUME_ICRC1_OFF,
					HAMMER2_VOLUME_ICRC1_SIZE);
			hmp->voldata.icrc_sects[HAMMER2_VOL_ICRC_SECT0]=
				hammer2_icrc32(
					(char *)&hmp->voldata +
					 HAMMER2_VOLUME_ICRC0_OFF,
					HAMMER2_VOLUME_ICRC0_SIZE);
			hmp->voldata.icrc_volheader =
				hammer2_icrc32(
					(char *)&hmp->voldata +
					 HAMMER2_VOLUME_ICRCVH_OFF,
					HAMMER2_VOLUME_ICRCVH_SIZE);

			if (hammer2_debug & 0x8000) {
				kprintf("syncvolhdr %016jx %016jx\n",
					hmp->voldata.mirror_tid,
					hmp->vchain.bref.mirror_tid);
			}
			hmp->volsync = hmp->voldata;
			atomic_set_int(&chain->flags, HAMMER2_CHAIN_VOLUMESYNC);
			hammer2_voldata_unlock(hmp);
			hammer2_chain_unlock(&hmp->fchain);
			break;
		case HAMMER2_BREF_TYPE_DATA:
			break;
		case HAMMER2_BREF_TYPE_INDIRECT:
		case HAMMER2_BREF_TYPE_FREEMAP_NODE:
		case HAMMER2_BREF_TYPE_FREEMAP_LEAF:
			hammer2_chain_setcheck(chain, chain->data);
			break;
		case HAMMER2_BREF_TYPE_DIRENT:
			if (chain->bytes)
				hammer2_chain_setcheck(chain, chain->data);
			break;
		case HAMMER2_BREF_TYPE_INODE:
			if (chain->data->ipdata.meta.op_flags &
			    HAMMER2_OPFLAG_PFSROOT) {
				hammer2_inode_data_t *ipdata;

				hammer2_io_setdirty(chain->dio);
				ipdata = &chain->data->ipdata;
				if (chain->pmp) {
					ipdata->meta.pfs_inum =
						chain->pmp->inode_tid;
				}
			} else {
			}

			hammer2_chain_setcheck(chain, chain->data);
			break;
		default:
			panic("hammer2_flush_core: unsupported "
			      "embedded bref %d",
			      chain->bref.type);
		}

		if (chain->flags & HAMMER2_CHAIN_DESTROY) {
			hammer2_io_dedup_delete(hmp,
						chain->bref.type,
						chain->bref.data_off,
						chain->bytes);
#if 0
			hammer2_io_t *dio;
			if (chain->dio) {
				hammer2_io_inval(chain->dio,
						 chain->bref.data_off,
						 chain->bytes);
			} else if ((dio = hammer2_io_getquick(hmp,
						  chain->bref.data_off,
						  chain->bytes,
						  1)) != NULL) {
				hammer2_io_inval(dio,
						 chain->bref.data_off,
						 chain->bytes);
				hammer2_io_putblk(&dio);
			}
#endif
		}
	}

	if ((chain->flags & HAMMER2_CHAIN_UPDATE) && parent == NULL)
		atomic_clear_int(&chain->flags, HAMMER2_CHAIN_UPDATE);

	if (chain->bref.type == HAMMER2_BREF_TYPE_INODE &&
	    (flags & HAMMER2_FLUSH_INODE_STOP) &&
	    (flags & HAMMER2_FLUSH_FSSYNC) == 0 &&
	    (flags & HAMMER2_FLUSH_ALL) == 0 &&
	    chain->pmp && chain->pmp->mp) {
#ifdef HAMMER2_DEBUG_SYNC
		kprintf("inum %ld do not update parent, non-fssync\n",
			(long)chain->bref.key);
#endif
		goto skipupdate;
	}
#ifdef HAMMER2_DEBUG_SYNC
	if (chain->bref.type == HAMMER2_BREF_TYPE_INODE)
		kprintf("inum %ld update parent\n", (long)chain->bref.key);
#endif

	if (chain->flags & HAMMER2_CHAIN_UPDATE) {
		hammer2_blockref_t *base;
		int count;

		atomic_clear_int(&chain->flags, HAMMER2_CHAIN_UPDATE);

		if (parent->flags & HAMMER2_CHAIN_DESTROY) {
			if (parent->bref.modify_tid < chain->bref.modify_tid) {
				parent->bref.modify_tid =
					chain->bref.modify_tid;
			}
			atomic_clear_int(&chain->flags, HAMMER2_CHAIN_BLKMAPPED |
							HAMMER2_CHAIN_BLKMAPUPD);
			goto skipupdate;
		}

		if (chain->bref.type == HAMMER2_BREF_TYPE_INDIRECT) {
			if (hammer2_chain_indirect_maintenance(parent, chain))
				goto skipupdate;
		}

		save_error = hammer2_chain_modify(parent, 0, 0, 0);
		if (save_error) {
			info->error |= save_error;
			kprintf("hammer2_flush: %016jx.%02x error=%08x\n",
				parent->bref.data_off, parent->bref.type,
				save_error);
			atomic_set_int(&chain->flags, HAMMER2_CHAIN_UPDATE);
			goto skipupdate;
		}
		if (parent->bref.modify_tid < chain->bref.modify_tid)
			parent->bref.modify_tid = chain->bref.modify_tid;

		switch(parent->bref.type) {
		case HAMMER2_BREF_TYPE_INODE:
			if (parent->data &&
			    (parent->data->ipdata.meta.op_flags &
			     HAMMER2_OPFLAG_DIRECTDATA) == 0) {
				base = &parent->data->
					ipdata.u.blockset.blockref[0];
			} else {
				base = NULL;
			}
			count = HAMMER2_SET_COUNT;
			break;
		case HAMMER2_BREF_TYPE_INDIRECT:
		case HAMMER2_BREF_TYPE_FREEMAP_NODE:
			if (parent->data)
				base = &parent->data->npdata[0];
			else
				base = NULL;
			count = parent->bytes / sizeof(hammer2_blockref_t);
			break;
		case HAMMER2_BREF_TYPE_VOLUME:
			base = &chain->hmp->voldata.sroot_blockset.blockref[0];
			count = HAMMER2_SET_COUNT;
			break;
		case HAMMER2_BREF_TYPE_FREEMAP:
			base = &parent->data->npdata[0];
			count = HAMMER2_SET_COUNT;
			break;
		default:
			base = NULL;
			count = 0;
			panic("hammer2_flush_core: "
			      "unrecognized blockref type: %d",
			      parent->bref.type);
			break;
		}

		if (base && (chain->flags & HAMMER2_CHAIN_BLKMAPUPD)) {
			if (chain->flags & HAMMER2_CHAIN_BLKMAPPED) {
				hammer2_spin_ex(&parent->core.spin);
				hammer2_base_delete(parent, base, count, chain,
						    NULL);
				hammer2_spin_unex(&parent->core.spin);
			} else {
				atomic_clear_int(&chain->flags,
						 HAMMER2_CHAIN_BLKMAPUPD);
			}
		}
		if (base && (chain->flags & HAMMER2_CHAIN_BLKMAPPED) == 0) {
			hammer2_spin_ex(&parent->core.spin);
			hammer2_base_insert(parent, base, count,
					    chain, &chain->bref);
			hammer2_spin_unex(&parent->core.spin);
		}
	}
skipupdate:
	if (parent)
		hammer2_chain_unlock(parent);

done:
	KKASSERT(chain->refs > 0);

	return retry;
}

static int
hammer2_flush_recurse(hammer2_chain_t *child, void *data)
{
	hammer2_flush_info_t *info = data;
	hammer2_chain_t *parent = info->parent;

#ifdef HAMMER2_SCAN_DEBUG
	++info->scan_count;
	if (child->flags & HAMMER2_CHAIN_MODIFIED)
		++info->scan_mod_count;
	if (child->flags & HAMMER2_CHAIN_UPDATE)
		++info->scan_upd_count;
	if (child->flags & HAMMER2_CHAIN_ONFLUSH)
		++info->scan_onf_count;
#endif

	hammer2_chain_ref(child);
	hammer2_spin_unex(&parent->core.spin);

	hammer2_chain_ref_hold(parent);
	hammer2_chain_unlock(parent);
	hammer2_chain_lock(child, HAMMER2_RESOLVE_MAYBE);
	if (child->parent != parent) {
		kprintf("LOST CHILD1 %p->%p (actual parent %p)\n",
			parent, child, child->parent);
		goto done;
	}
	if (child->error) {
		kprintf("CHILD ERROR DURING FLUSH LOCK %p->%p\n",
			parent, child);
		info->error |= child->error;
		goto done;
	}

	if (parent && (parent->flags & HAMMER2_CHAIN_DESTROY))
		atomic_set_int(&child->flags, HAMMER2_CHAIN_DESTROY);
#ifdef HAMMER2_SCAN_DEBUG
	if (child->flags & HAMMER2_CHAIN_DESTROY)
		++info->scan_del_count;
#endif
	if ((parent->bref.flags & HAMMER2_BREF_FLAG_PFSROOT) &&
	    (child->flags & HAMMER2_CHAIN_DESTROY) == 0 &&
	    parent->bref.type == HAMMER2_BREF_TYPE_INODE &&
	    (info->flags & HAMMER2_FLUSH_FSSYNC) == 0) {
		if ((child->bref.key & HAMMER2_DIRHASH_VISIBLE) == 0) {
			if (child->flags & HAMMER2_CHAIN_FLUSH_MASK) {
				hammer2_chain_setflush(parent);
			}
			goto done;
		}
	}

	if (child->flags & HAMMER2_CHAIN_FLUSH_MASK) {
#ifdef HAMMER2_SCAN_DEBUG
		if (child->bref.type < 7)
			++info->scan_btype[child->bref.type];
#endif
		++info->depth;
		hammer2_flush_core(info, child, info->flags);
		--info->depth;
	}

done:
	hammer2_chain_unlock(child);
	hammer2_chain_lock(parent, HAMMER2_RESOLVE_MAYBE);
	hammer2_chain_drop_unhold(parent);
	if (parent->error) {
		kprintf("PARENT ERROR DURING FLUSH LOCK %p->%p\n",
			parent, child);
		info->error |= parent->error;
	}
	hammer2_chain_drop(child);
	KKASSERT(info->parent == parent);
	hammer2_spin_ex(&parent->core.spin);

	return (0);
}

void
hammer2_xop_inode_flush(hammer2_xop_t *arg, void *scratch __unused, int clindex)
{
	hammer2_xop_flush_t *xop = &arg->xop_flush;
	hammer2_chain_t *chain;
	hammer2_inode_t *ip;
	hammer2_dev_t *hmp;
	hammer2_pfs_t *pmp;
	hammer2_devvp_t *e;
	struct vnode *devvp;
	int flush_error = 0;
	int fsync_error = 0;
	int total_error = 0;
	int j;
	int xflags;
	int ispfsroot = 0;

	xflags = HAMMER2_FLUSH_TOP;
	if (xop->head.flags & HAMMER2_XOP_INODE_STOP)
		xflags |= HAMMER2_FLUSH_INODE_STOP;
	if (xop->head.flags & HAMMER2_XOP_FSSYNC)
		xflags |= HAMMER2_FLUSH_FSSYNC;

	ip = xop->head.ip1;
	pmp = ip->pmp;
	chain = hammer2_inode_chain(ip, clindex, HAMMER2_RESOLVE_ALWAYS);
	if (chain) {
		hmp = chain->hmp;
		if (chain->flags & HAMMER2_CHAIN_FLUSH_MASK) {
			if (chain->parent)
				hammer2_chain_setflush(chain->parent);
			hammer2_flush(chain, xflags);

			if (ip == pmp->iroot && pmp != hmp->spmp) {
				hammer2_spin_ex(&pmp->blockset_spin);
				pmp->pfs_iroot_blocksets[clindex] =
					chain->data->ipdata.u.blockset;
				hammer2_spin_unex(&pmp->blockset_spin);
			}

#if 0
			if ((xop->head.flags & HAMMER2_XOP_PARENTONFLUSH) ||
			    chain->bref.type != HAMMER2_BREF_TYPE_INODE) {
				parent = chain->parent;
				if (parent)
					hammer2_chain_setflush(parent);
			}
#endif
		}
		if (chain->flags & HAMMER2_CHAIN_PFSBOUNDARY)
			ispfsroot = 1;
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
		chain = NULL;
	} else {
		hmp = NULL;
	}

	if ((xop->head.flags & HAMMER2_XOP_VOLHDR) == 0)
		goto skip;
	if (ispfsroot == 0)
		goto skip;

	for (j = clindex - 1; j >= 0; --j) {
		if ((chain = ip->cluster.array[j].chain) != NULL) {
			if (chain->hmp == hmp) {
				chain = NULL;
				goto skip;
			}
		}
	}
	chain = NULL;

	hammer2_trans_init(hmp->spmp, HAMMER2_TRANS_ISFLUSH);

	if (hmp->spmp->iroot) {
		chain = hmp->spmp->iroot->cluster.array[0].chain;
		if (chain) {
			hammer2_chain_ref(chain);
			hammer2_chain_lock(chain, HAMMER2_RESOLVE_ALWAYS);
			flush_error |=
				hammer2_flush(chain,
					      HAMMER2_FLUSH_TOP |
					      HAMMER2_FLUSH_INODE_STOP |
					      HAMMER2_FLUSH_FSSYNC);
			hammer2_chain_unlock(chain);
			hammer2_chain_drop(chain);
		}
	}

	hammer2_chain_ref(&hmp->vchain);
	hammer2_chain_lock(&hmp->vchain, HAMMER2_RESOLVE_ALWAYS);
	hammer2_chain_ref(&hmp->fchain);
	hammer2_chain_lock(&hmp->fchain, HAMMER2_RESOLVE_ALWAYS);
	if (hmp->fchain.flags & HAMMER2_CHAIN_FLUSH_MASK) {
		hammer2_voldata_modify(hmp);
		chain = &hmp->fchain;
		flush_error |= hammer2_flush(chain, HAMMER2_FLUSH_TOP);
		KKASSERT(chain == &hmp->fchain);
	}
	hammer2_chain_unlock(&hmp->fchain);
	hammer2_chain_unlock(&hmp->vchain);
	hammer2_chain_drop(&hmp->fchain);

	hammer2_chain_lock(&hmp->vchain, HAMMER2_RESOLVE_ALWAYS);
	if (hmp->vchain.flags & HAMMER2_CHAIN_FLUSH_MASK) {
		chain = &hmp->vchain;
		flush_error |= hammer2_flush(chain, HAMMER2_FLUSH_TOP);
		KKASSERT(chain == &hmp->vchain);
	}
	hammer2_chain_unlock(&hmp->vchain);
	hammer2_chain_drop(&hmp->vchain);

	TAILQ_FOREACH(e, &hmp->devvpl, entry) {
		devvp = e->devvp;
		KKASSERT(devvp);
		vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
		fsync_error = VOP_FSYNC(devvp, MNT_WAIT, 0);
		vn_unlock(devvp);
		if (fsync_error || flush_error) {
			kprintf("hammer2: sync error fsync=%d h2flush=0x%04x dev=%s\n",
				fsync_error, flush_error, e->path);
		}
	}

	if (fsync_error == 0 && flush_error == 0 &&
	    (hmp->vchain.flags & HAMMER2_CHAIN_VOLUMESYNC)) {
		struct buf *bp;
		int vol_error = 0;

		bp = getpbuf(NULL);
		bp->b_bio1.bio_offset = 0;
		bp->b_bufsize = 0;
		bp->b_bcount = 0;
		bp->b_cmd = BUF_CMD_FLUSH;
		bp->b_bio1.bio_done = biodone_sync;
		bp->b_bio1.bio_flags |= BIO_SYNC;
		vn_strategy(hmp->devvp, &bp->b_bio1);
		fsync_error = biowait(&bp->b_bio1, "h2vol");
		relpbuf(bp, NULL);

		j = hmp->volhdrno + 1;
		if (j < 0)
			j = 0;
		if (j >= HAMMER2_NUM_VOLHDRS)
			j = 0;
		if (j * HAMMER2_ZONE_BYTES64 + HAMMER2_SEGSIZE >
		    hmp->volsync.volu_size) {
			j = 0;
		}
		if (hammer2_debug & 0x8000) {
			kprintf("sync volhdr %d %jd\n",
				j, (intmax_t)hmp->volsync.volu_size);
		}
		bp = getblk(hmp->devvp, j * HAMMER2_ZONE_BYTES64,
			    HAMMER2_VOLUME_BYTES, GETBLK_KVABIO, 0);
		atomic_clear_int(&hmp->vchain.flags,
				 HAMMER2_CHAIN_VOLUMESYNC);
		bkvasync(bp);
		bcopy(&hmp->volsync, bp->b_data, HAMMER2_VOLUME_BYTES);
		vol_error = bwrite(bp);
		hmp->volhdrno = j;
		if (vol_error)
			fsync_error = vol_error;
	}
	if (flush_error)
		total_error = flush_error;
	if (fsync_error)
		total_error = hammer2_errno_to_error(fsync_error);

	hammer2_trans_done(hmp->spmp, HAMMER2_TRANS_ISFLUSH);
skip:
	hammer2_xop_feed(&xop->head, NULL, clindex, total_error);
}
