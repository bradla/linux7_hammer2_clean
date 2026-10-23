#include "hammer2.h"

typedef struct hammer2_deferred_ip {
	struct hammer2_deferred_ip *next;
	hammer2_inode_t	*ip;
} hammer2_deferred_ip_t;

typedef struct hammer2_deferred_list {
	hammer2_deferred_ip_t	*base;
	int			count;
} hammer2_deferred_list_t;

#define HAMMER2_SYNCHRO_DEBUG 1

static int hammer2_sync_slaves(hammer2_thread_t *thr, hammer2_inode_t *ip,
				hammer2_deferred_list_t *list, int isroot);
#if 0
static void hammer2_update_pfs_status(hammer2_thread_t *thr, uint32_t flags);
				nerror = hammer2_sync_insert(
						thr, &parent, &chain,
						focus->bref.modify_tid,
						idx, focus);
#endif
static int hammer2_sync_insert(hammer2_thread_t *thr,
			hammer2_chain_t **parentp, hammer2_chain_t **chainp,
			hammer2_tid_t modify_tid, int idx,
			hammer2_xop_head_t *xop, hammer2_chain_t *focus);
static int hammer2_sync_destroy(hammer2_thread_t *thr,
			hammer2_chain_t **parentp, hammer2_chain_t **chainp,
			hammer2_tid_t mtid, int idx);
static int hammer2_sync_replace(hammer2_thread_t *thr,
			hammer2_chain_t *parent, hammer2_chain_t *chain,
			hammer2_tid_t mtid, int idx,
			hammer2_xop_head_t *xop, hammer2_chain_t *focus,
			int isroot);

void
hammer2_primary_sync_thread(void *arg)
{
	hammer2_thread_t *thr = arg;
	hammer2_pfs_t *pmp;
	hammer2_deferred_list_t list;
	hammer2_deferred_ip_t *defer;
	int error;
	uint32_t flags;
	uint32_t nflags;

	pmp = thr->pmp;
	bzero(&list, sizeof(list));

	for (;;) {
		flags = thr->flags;
		cpu_ccfence();

		if (flags & HAMMER2_THREAD_STOP)
			break;

		if (flags & HAMMER2_THREAD_FREEZE) {
			nflags = (flags & ~(HAMMER2_THREAD_FREEZE |
					    HAMMER2_THREAD_WAITING)) |
				 HAMMER2_THREAD_FROZEN;
			if (!atomic_cmpset_int(&thr->flags, flags, nflags))
				continue;
			if (flags & HAMMER2_THREAD_WAITING)
				wakeup(&thr->flags);
			continue;
		}

		if (flags & HAMMER2_THREAD_UNFREEZE) {
			nflags = flags & ~(HAMMER2_THREAD_UNFREEZE |
					   HAMMER2_THREAD_FROZEN |
					   HAMMER2_THREAD_WAITING);
			if (!atomic_cmpset_int(&thr->flags, flags, nflags))
				continue;
			if (flags & HAMMER2_THREAD_WAITING)
				wakeup(&thr->flags);
			continue;
		}

		if (flags & HAMMER2_THREAD_FROZEN) {
			nflags = flags | HAMMER2_THREAD_WAITING;

			tsleep_interlock(&thr->flags, 0);
			if (atomic_cmpset_int(&thr->flags, flags, nflags))
				tsleep(&thr->flags, PINTERLOCKED, "frozen", 0);
			continue;
		}

		if (thr->flags & HAMMER2_THREAD_REMASTER) {
			nflags = flags & ~HAMMER2_THREAD_REMASTER;
			if (atomic_cmpset_int(&thr->flags, flags, nflags)) {
			}
			continue;
		}

		if (hammer2_debug & 0x8000)
			kprintf("sync_slaves pfs %s clindex %d\n",
				pmp->pfs_names[thr->clindex], thr->clindex);
		hammer2_trans_init(pmp, 0);

		hammer2_inode_ref(pmp->iroot);

		for (;;) {
			int didbreak = 0;
			error = hammer2_sync_slaves(thr, pmp->iroot, &list, 1);
			if (hammer2_debug & 0x8000) {
				kprintf("sync_slaves error %d defer %p\n",
					error, list.base);
			}
			if (error != HAMMER2_ERROR_EAGAIN)
				break;
			while ((defer = list.base) != NULL) {
				hammer2_inode_t *nip;

				nip = defer->ip;
				error = hammer2_sync_slaves(thr, nip, &list,
							(nip == pmp->iroot));
				if (error &&
				    error != HAMMER2_ERROR_EAGAIN &&
				    error != HAMMER2_ERROR_ENOENT) {
					break;
				}
				if (hammer2_thr_break(thr)) {
					didbreak = 1;
					break;
				}

				if (defer == list.base) {
					--list.count;
					list.base = defer->next;
					kfree(defer, M_HAMMER2);
					defer = NULL;
					hammer2_inode_drop(nip);
				}
			}

			if (didbreak ||
			    (error && error != HAMMER2_ERROR_EAGAIN)) {
				kprintf("didbreak\n");
				while ((defer = list.base) != NULL) {
					--list.count;
					hammer2_inode_drop(defer->ip);
					list.base = defer->next;
					kfree(defer, M_HAMMER2);
				}
				if (error == 0 || error == HAMMER2_ERROR_EAGAIN)
					error = HAMMER2_ERROR_EINPROGRESS;
				break;
			}
		}

		hammer2_inode_drop(pmp->iroot);
		hammer2_trans_done(pmp, 0);

		if (error && error != HAMMER2_ERROR_EINPROGRESS)
			kprintf("hammer2_sync_slaves: error %d\n", error);

		nflags = flags | HAMMER2_THREAD_WAITING;
		tsleep_interlock(&thr->flags, 0);
		if (atomic_cmpset_int(&thr->flags, flags, nflags)) {
			tsleep(&thr->flags, 0, "h2idle", hz * 5);
		}
	}
	thr->td = NULL;
	hammer2_thr_signal(thr, HAMMER2_THREAD_STOPPED);
}

#if 0
static
void
hammer2_update_pfs_status(hammer2_thread_t *thr, uint32_t flags)
{
	hammer2_pfs_t *pmp = thr->pmp;

	flags &= HAMMER2_CLUSTER_ZFLAGS;
	if (pmp->cluster_flags == flags)
		return;
	pmp->cluster_flags = flags;

	kprintf("pfs %p", pmp);
	if (flags & HAMMER2_CLUSTER_MSYNCED)
		kprintf(" masters-all-good");
	if (flags & HAMMER2_CLUSTER_SSYNCED)
		kprintf(" slaves-all-good");

	if (flags & HAMMER2_CLUSTER_WRHARD)
		kprintf(" quorum/rw");
	else if (flags & HAMMER2_CLUSTER_RDHARD)
		kprintf(" quorum/ro");

	if (flags & HAMMER2_CLUSTER_UNHARD)
		kprintf(" out-of-sync-masters");
	else if (flags & HAMMER2_CLUSTER_NOHARD)
		kprintf(" no-masters-visible");

	if (flags & HAMMER2_CLUSTER_WRSOFT)
		kprintf(" soft/rw");
	else if (flags & HAMMER2_CLUSTER_RDSOFT)
		kprintf(" soft/ro");

	if (flags & HAMMER2_CLUSTER_UNSOFT)
		kprintf(" out-of-sync-slaves");
	else if (flags & HAMMER2_CLUSTER_NOSOFT)
		kprintf(" no-slaves-visible");
	kprintf("\n");
}
#endif

#if 0
static
void
dumpcluster(const char *label,
	    hammer2_cluster_t *cparent, hammer2_cluster_t *cluster)
{
	hammer2_chain_t *chain;
	int i;

	if ((hammer2_debug & 1) == 0)
		return;

	kprintf("%s\t", label);
	KKASSERT(cparent->nchains == cluster->nchains);
	for (i = 0; i < cparent->nchains; ++i) {
		if (i)
			kprintf("\t");
		kprintf("%d ", i);
		if ((chain = cparent->array[i].chain) != NULL) {
			kprintf("%016jx%s ",
				chain->bref.key,
				((cparent->array[i].flags &
				  HAMMER2_CITEM_INVALID) ? "(I)" : "   ")
			);
		} else {
			kprintf("      NULL      %s ", "   ");
		}
		if ((chain = cluster->array[i].chain) != NULL) {
			kprintf("%016jx%s ",
				chain->bref.key,
				((cluster->array[i].flags &
				  HAMMER2_CITEM_INVALID) ? "(I)" : "   ")
			);
		} else {
			kprintf("      NULL      %s ", "   ");
		}
		kprintf("\n");
	}
}
#endif

static
int
hammer2_sync_slaves(hammer2_thread_t *thr, hammer2_inode_t *ip,
		    hammer2_deferred_list_t *list, int isroot)
{
	hammer2_xop_scanall_t *xop;
	hammer2_chain_t *parent;
	hammer2_chain_t *chain;
	hammer2_pfs_t *pmp;
	hammer2_key_t key_next;
	hammer2_tid_t sync_tid;
	int needrescan;
	int want_update;
	int serror;
	int merror;
	int nerror;
	int idx;
	int n;

	pmp = ip->pmp;
	idx = thr->clindex;
	needrescan = 0;
	want_update = 0;
	sync_tid = 0;
	chain = NULL;
	parent = NULL;

#if 0
	if (pmp->cluster_flags & HAMMER2_CLUSTER_SSYNCED)
		return(0);
	if ((pmp->cluster_flags & HAMMER2_CLUSTER_RDHARD) == 0)
		return(HAMMER2_ERROR_INCOMPLETE);
#endif

	merror = 0;

	{
		hammer2_xop_ipcluster_t *xop2;
		hammer2_chain_t *focus;

		hammer2_inode_lock(ip, HAMMER2_RESOLVE_SHARED);
		xop2 = hammer2_xop_alloc(ip, HAMMER2_XOP_MODIFYING);
		hammer2_xop_start_except(&xop2->head, &hammer2_ipcluster_desc,
					 idx);
		hammer2_inode_unlock(ip);
		merror = hammer2_xop_collect(&xop2->head, 0);
		if (merror == 0 && (focus = xop2->head.cluster.focus) != NULL) {
			sync_tid = focus->bref.modify_tid;
			chain = hammer2_inode_chain_and_parent(ip, idx,
						    &parent,
						    HAMMER2_RESOLVE_ALWAYS |
						    HAMMER2_RESOLVE_SHARED);
			want_update = (chain->bref.modify_tid != sync_tid);
			if (chain) {
				hammer2_chain_unlock(chain);
				hammer2_chain_drop(chain);
				chain = NULL;
			}
			if (parent) {
				hammer2_chain_unlock(parent);
				hammer2_chain_drop(parent);
				parent = NULL;
			}
		}
		hammer2_xop_retire(&xop2->head, HAMMER2_XOPMASK_VOP);
	}

	if (want_update == 0)
		return(0);

	hammer2_inode_lock(ip, HAMMER2_RESOLVE_SHARED);
	xop = hammer2_xop_alloc(ip, HAMMER2_XOP_MODIFYING);
	xop->key_beg = HAMMER2_KEY_MIN;
	xop->key_end = HAMMER2_KEY_MAX;
	xop->resolve_flags = HAMMER2_RESOLVE_SHARED |
			     HAMMER2_RESOLVE_ALWAYS;
	xop->lookup_flags = HAMMER2_LOOKUP_SHARED |
			    HAMMER2_LOOKUP_NODIRECT |
			    HAMMER2_LOOKUP_ALWAYS;
	hammer2_xop_start_except(&xop->head, &hammer2_scanall_desc, idx);
	parent = hammer2_inode_chain(ip, idx,
				     HAMMER2_RESOLVE_ALWAYS |
				     HAMMER2_RESOLVE_SHARED);
	hammer2_inode_unlock(ip);

	chain = hammer2_chain_lookup(&parent, &key_next,
				     HAMMER2_KEY_MIN, HAMMER2_KEY_MAX,
				     &serror,
				     HAMMER2_LOOKUP_SHARED |
				     HAMMER2_LOOKUP_NODIRECT |
				     HAMMER2_LOOKUP_NODATA);
	merror = hammer2_xop_collect(&xop->head, 0);
	if (hammer2_debug & 0x8000) {
		kprintf("START_SCAN IP=%016jx chain=%p (%016jx)\n",
			ip->meta.name_key, chain,
			(chain ? chain->bref.key : -1));
	}

	for (;;) {
		int advance_local = 0;
		int advance_xop = 0;
		int dodefer = 0;
		hammer2_chain_t *focus;

		if (chain == NULL && merror == HAMMER2_ERROR_ENOENT)
			break;
		if (merror && merror != HAMMER2_ERROR_ENOENT)
			break;

		if (chain && merror == HAMMER2_ERROR_ENOENT) {
			n = -1;
			focus = NULL;
		} else if (chain == NULL) {
			n = 1;
			focus = xop->head.cluster.focus;
		} else {
			focus = xop->head.cluster.focus;
			n = hammer2_chain_cmp(chain, focus);
		}

		if (n < 0) {
			nerror = hammer2_sync_destroy(thr, &parent, &chain,
						      0, idx);
		} else if (n == 0 && chain->bref.modify_tid !=
				     focus->bref.modify_tid) {
			if (focus->bref.type == HAMMER2_BREF_TYPE_INODE) {
				nerror = hammer2_sync_replace(
						thr, parent, chain,
						0,
						idx, &xop->head, focus, 0);
				dodefer = 1;
			} else {
				nerror = hammer2_sync_replace(
						thr, parent, chain,
						focus->bref.modify_tid,
						idx, &xop->head, focus, 0);
			}
			advance_local = 1;
			advance_xop = 1;
		} else if (n == 0) {
			advance_local = 1;
			advance_xop = 1;
			nerror = 0;
		} else if (n > 0) {
			if (focus->bref.type == HAMMER2_BREF_TYPE_INODE) {
				nerror = hammer2_sync_insert(
						thr, &parent, &chain,
						0,
						idx, &xop->head, focus);
				dodefer = 2;
			} else {
				nerror = hammer2_sync_insert(
						thr, &parent, &chain,
						focus->bref.modify_tid,
						idx, &xop->head, focus);
			}
			advance_local = 1;
			advance_xop = 1;
		}

		if (merror == 0 && dodefer) {
			hammer2_inode_t *nip;
			hammer2_deferred_ip_t *defer;

			KKASSERT(focus->bref.type == HAMMER2_BREF_TYPE_INODE);

			defer = kmalloc(sizeof(*defer), M_HAMMER2,
					M_WAITOK | M_ZERO);
			KKASSERT(xop->head.cluster.array[idx].chain == NULL);
			xop->head.cluster.array[idx].flags =
							HAMMER2_CITEM_INVALID;
			xop->head.cluster.array[idx].chain = chain;
			nip = hammer2_inode_get(pmp, &xop->head, -1, idx);
			xop->head.cluster.array[idx].chain = NULL;

			hammer2_inode_ref(nip);
			hammer2_inode_unlock(nip);

			defer->next = list->base;
			defer->ip = nip;
			list->base = defer;
			++list->count;
			needrescan = 1;
		}

		if (needrescan && list->count > 1000)
			break;

		if (advance_xop) {
			merror = hammer2_xop_collect(&xop->head, 0);
		}
		if (advance_local) {
			chain = hammer2_chain_next(&parent, chain, &key_next,
						   key_next, HAMMER2_KEY_MAX,
						   &serror,
						   HAMMER2_LOOKUP_SHARED |
						   HAMMER2_LOOKUP_NODIRECT |
						   HAMMER2_LOOKUP_NODATA);
		}
	}
	hammer2_xop_retire(&xop->head, HAMMER2_XOPMASK_VOP);
	if (chain) {
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
	}
	if (parent) {
		hammer2_chain_unlock(parent);
		hammer2_chain_drop(parent);
	}

	if ((merror == 0 || merror == HAMMER2_ERROR_ENOENT) && needrescan)
		merror = HAMMER2_ERROR_EAGAIN;

	if (merror == 0 || merror == HAMMER2_ERROR_ENOENT) {
		hammer2_xop_ipcluster_t *xop2;
		hammer2_chain_t *focus;

		hammer2_inode_lock(ip, HAMMER2_RESOLVE_SHARED);
		xop2 = hammer2_xop_alloc(ip, HAMMER2_XOP_MODIFYING);
		hammer2_xop_start_except(&xop2->head, &hammer2_ipcluster_desc,
					 idx);
		hammer2_inode_unlock(ip);
		merror = hammer2_xop_collect(&xop2->head, 0);
		if (merror == 0) {
			focus = xop2->head.cluster.focus;
			if ((hammer2_debug & 0x8000) && focus) {
				const char *filename;

				filename = hammer2_xop_gdata(&xop2->head)->
						ipdata.filename;
				kprintf("syncthr: update inode %p (%s)\n",
					focus, filename);
				hammer2_xop_pdata(&xop2->head);
			}
			chain = hammer2_inode_chain_and_parent(ip, idx,
						    &parent,
						    HAMMER2_RESOLVE_ALWAYS |
						    HAMMER2_RESOLVE_SHARED);

			KKASSERT(parent != NULL);
			nerror = hammer2_sync_replace(
					thr, parent, chain,
					sync_tid,
					idx, &xop2->head, focus, isroot);
			hammer2_chain_unlock(chain);
			hammer2_chain_drop(chain);
			hammer2_chain_unlock(parent);
			hammer2_chain_drop(parent);
		}
		hammer2_xop_retire(&xop2->head, HAMMER2_XOPMASK_VOP);
	}

	return merror;
}

static
int
hammer2_sync_insert(hammer2_thread_t *thr,
		    hammer2_chain_t **parentp, hammer2_chain_t **chainp,
		    hammer2_tid_t mtid, int idx, hammer2_xop_head_t *xop,
		    hammer2_chain_t *focus)
{
	hammer2_chain_t *chain;
	hammer2_key_t dummy;
	int error;

#if HAMMER2_SYNCHRO_DEBUG
	if (hammer2_debug & 1)
	kprintf("insert rec par=%p/%d.%016jx slave %d %d.%016jx mod=%016jx\n",
		*parentp,
		(*parentp)->bref.type,
		(*parentp)->bref.key,
		idx,
		focus->bref.type, focus->bref.key, mtid);
#endif

	if (*chainp) {
		hammer2_chain_unlock(*chainp);
		hammer2_chain_drop(*chainp);
		*chainp = NULL;
	}
	hammer2_chain_unlock(*parentp);
	hammer2_chain_lock(*parentp, HAMMER2_RESOLVE_ALWAYS);

	chain = hammer2_chain_lookup(parentp, &dummy,
				     focus->bref.key, focus->bref.key,
				     &error,
				     HAMMER2_LOOKUP_NODIRECT |
				     HAMMER2_LOOKUP_ALWAYS);
	KKASSERT(chain == NULL);

	chain = NULL;
	error = hammer2_chain_create(parentp, &chain, NULL, thr->pmp,
				     focus->bref.methods,
				     focus->bref.key, focus->bref.keybits,
				     focus->bref.type, focus->bytes,
				     mtid, 0, 0);
	if (error == 0) {
		const hammer2_media_data_t *data;

		error = hammer2_chain_modify(chain, mtid, 0, 0);
		if (error)
			goto failed;

		chain->bref.methods = focus->bref.methods;
		chain->bref.vradix = focus->bref.vradix;
		KKASSERT(chain->bref.modify_tid == mtid);
		chain->bref.flags = focus->bref.flags;

		switch(chain->bref.type) {
		case HAMMER2_BREF_TYPE_INODE:
			data = hammer2_xop_gdata(xop);

			if ((data->ipdata.meta.op_flags &
			     HAMMER2_OPFLAG_DIRECTDATA) == 0) {
				bcopy(data, chain->data,
				      offsetof(hammer2_inode_data_t, u));
				hammer2_xop_pdata(xop);
				break;
			}
			hammer2_xop_pdata(xop);
		case HAMMER2_BREF_TYPE_DATA:
			data = hammer2_xop_gdata(xop);
			bcopy(data, chain->data, chain->bytes);
			hammer2_chain_setcheck(chain, chain->data);
			hammer2_xop_pdata(xop);
			break;
		case HAMMER2_BREF_TYPE_DIRENT:
			if (chain->bytes) {
				data = hammer2_xop_gdata(xop);
				bcopy(data, chain->data, chain->bytes);
				hammer2_chain_setcheck(chain, chain->data);
				hammer2_xop_pdata(xop);
			} else {
				chain->bref.check = focus->bref.check;
			}
			chain->bref.embed = focus->bref.embed;
			break;
		default:
			KKASSERT(0);
			break;
		}
	}

failed:
	if (chain)
		hammer2_chain_unlock(chain);
	*chainp = chain;

	hammer2_chain_unlock(*parentp);
	hammer2_chain_lock(*parentp, HAMMER2_RESOLVE_SHARED |
				     HAMMER2_RESOLVE_ALWAYS);
	if (chain) {
		hammer2_chain_lock(chain, HAMMER2_RESOLVE_SHARED |
					  HAMMER2_RESOLVE_ALWAYS);
		error = chain->error;
	}

	return error;
}

static
int
hammer2_sync_destroy(hammer2_thread_t *thr,
		     hammer2_chain_t **parentp, hammer2_chain_t **chainp,
		     hammer2_tid_t mtid, int idx)
{
	hammer2_chain_t *chain;
	hammer2_key_t key_next;
	hammer2_key_t save_key;
	int error;

	chain = *chainp;

#if HAMMER2_SYNCHRO_DEBUG
	if (hammer2_debug & 1)
	kprintf("destroy rec %p/%p slave %d %d.%016jx\n",
		*parentp, chain,
		idx, chain->bref.type, chain->bref.key);
#endif

	save_key = chain->bref.key;
	if (save_key != HAMMER2_KEY_MAX)
		++save_key;

	hammer2_chain_unlock(chain);
	hammer2_chain_unlock(*parentp);
	hammer2_chain_lock(*parentp, HAMMER2_RESOLVE_ALWAYS);
	hammer2_chain_lock(chain, HAMMER2_RESOLVE_NEVER);

	hammer2_chain_delete(*parentp, chain, mtid, HAMMER2_DELETE_PERMANENT);
	hammer2_chain_unlock(chain);
	hammer2_chain_drop(chain);
	chain = NULL;

	hammer2_chain_unlock(*parentp);
	hammer2_chain_lock(*parentp, HAMMER2_RESOLVE_SHARED |
				     HAMMER2_RESOLVE_ALWAYS);
	*chainp = hammer2_chain_lookup(parentp, &key_next,
				     save_key, HAMMER2_KEY_MAX,
				     &error,
				     HAMMER2_LOOKUP_SHARED |
				     HAMMER2_LOOKUP_NODIRECT |
				     HAMMER2_LOOKUP_NODATA);
	return error;
}

static
int
hammer2_sync_replace(hammer2_thread_t *thr,
		     hammer2_chain_t *parent, hammer2_chain_t *chain,
		     hammer2_tid_t mtid, int idx,
		     hammer2_xop_head_t *xop, hammer2_chain_t *focus,
		     int isroot)
{
	uint8_t otype;
	int nradix;
	int error;

#if HAMMER2_SYNCHRO_DEBUG
	if (hammer2_debug & 1)
	kprintf("replace rec %p slave %d %d.%016jx mod=%016jx\n",
		chain,
		idx,
		focus->bref.type, focus->bref.key, mtid);
#endif
	hammer2_chain_unlock(chain);
	hammer2_chain_lock(chain, HAMMER2_RESOLVE_ALWAYS);
	error = chain->error;
	if (error == 0) {
		const hammer2_media_data_t *data;

		if (chain->bytes != focus->bytes) {
			nradix = hammer2_getradix(chain->bytes);
			error = hammer2_chain_resize(chain, mtid, 0, nradix, 0);
			if (error)
				goto failed;
		}
		error = hammer2_chain_modify(chain, mtid, 0, 0);
		if (error)
			goto failed;
		otype = chain->bref.type;
		data = hammer2_xop_gdata(xop);
		chain->bref.type = focus->bref.type;
		chain->bref.methods = focus->bref.methods;
		chain->bref.keybits = focus->bref.keybits;
		chain->bref.vradix = focus->bref.vradix;
		KKASSERT(mtid == 0 || chain->bref.modify_tid == mtid);
		chain->bref.flags = focus->bref.flags;

		switch(chain->bref.type) {
		case HAMMER2_BREF_TYPE_INODE:
			if (isroot) {
				chain->data->ipdata.meta.uflags =
					data->ipdata.meta.uflags;
				chain->data->ipdata.meta.rmajor =
					data->ipdata.meta.rmajor;
				chain->data->ipdata.meta.rminor =
					data->ipdata.meta.rminor;
				chain->data->ipdata.meta.ctime =
					data->ipdata.meta.ctime;
				chain->data->ipdata.meta.mtime =
					data->ipdata.meta.mtime;
				chain->data->ipdata.meta.atime =
					data->ipdata.meta.atime;
				chain->data->ipdata.meta.uid =
					data->ipdata.meta.uid;
				chain->data->ipdata.meta.gid =
					data->ipdata.meta.gid;
				chain->data->ipdata.meta.mode =
					data->ipdata.meta.mode;
				chain->data->ipdata.meta.ncopies =
					data->ipdata.meta.ncopies;
				chain->data->ipdata.meta.comp_algo =
					data->ipdata.meta.comp_algo;
				chain->data->ipdata.meta.check_algo =
					data->ipdata.meta.check_algo;
				chain->data->ipdata.meta.data_quota =
					data->ipdata.meta.data_quota;
				chain->data->ipdata.meta.inode_quota =
					data->ipdata.meta.inode_quota;

				if (chain->data->ipdata.meta.pfs_lsnap_tid <
				    data->ipdata.meta.pfs_lsnap_tid) {
					chain->data->ipdata.meta.pfs_lsnap_tid =
					data->ipdata.meta.pfs_lsnap_tid;
				}

				hammer2_chain_setcheck(chain, chain->data);
				break;
			}

			if ((data->ipdata.meta.op_flags &
			     HAMMER2_OPFLAG_DIRECTDATA) == 0) {
				if (otype != HAMMER2_BREF_TYPE_INODE ||
				    (chain->data->ipdata.meta.op_flags &
				     HAMMER2_OPFLAG_DIRECTDATA)) {
					kprintf("chain inode trans "
						"away from dd\n");
					bzero(&chain->data->ipdata.u,
					      sizeof(chain->data->ipdata.u));
				}
				bcopy(data, chain->data,
				      offsetof(hammer2_inode_data_t, u));
				hammer2_chain_setcheck(chain, chain->data);
				break;
			}
		case HAMMER2_BREF_TYPE_DATA:
			bcopy(data, chain->data, chain->bytes);
			hammer2_chain_setcheck(chain, chain->data);
			break;
		case HAMMER2_BREF_TYPE_DIRENT:
			if (chain->bytes) {
				bcopy(data, chain->data, chain->bytes);
				hammer2_chain_setcheck(chain, chain->data);
			} else {
				chain->bref.check = focus->bref.check;
			}
			chain->bref.embed = focus->bref.embed;
			break;
		default:
			KKASSERT(0);
			break;
		}
		hammer2_xop_pdata(xop);
	}

failed:
	hammer2_chain_unlock(chain);
	hammer2_chain_lock(chain, HAMMER2_RESOLVE_SHARED |
				  HAMMER2_RESOLVE_MAYBE);

	return error;
}
