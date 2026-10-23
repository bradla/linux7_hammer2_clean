#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/types.h>

#include "hammer2.h"

uint8_t
hammer2_cluster_type(hammer2_cluster_t *cluster)
{
	if (cluster->error == 0) {
		KKASSERT(cluster->focus != NULL);
		return(cluster->focus->bref.type);
	}
	return 0;
}

void
hammer2_cluster_bref(hammer2_cluster_t *cluster, hammer2_blockref_t *bref)
{
	if (cluster->error == 0) {
		KKASSERT(cluster->focus != NULL);
		*bref = cluster->focus->bref;
		bref->data_off = 0;
	} else {
		bzero(bref, sizeof(*bref));
	}
}

void
hammer2_dummy_xop_from_chain(hammer2_xop_head_t *xop, hammer2_chain_t *chain)
{
	hammer2_cluster_t *cluster;

	bzero(xop, sizeof(*xop));

	cluster = &xop->cluster;
	cluster->array[0].chain = chain;
	cluster->array[0].flags = HAMMER2_CITEM_FEMOD;
	cluster->nchains = 1;
	cluster->focus = chain;
	cluster->focus_index = 0;
	cluster->pmp = chain->pmp;
	cluster->refs = 1;
	cluster->error = chain->error;
	cluster->flags = HAMMER2_CLUSTER_LOCKED |
			 HAMMER2_CLUSTER_WRHARD |
			 HAMMER2_CLUSTER_RDHARD |
			 HAMMER2_CLUSTER_MSYNCED |
			 HAMMER2_CLUSTER_SSYNCED;
}

void
hammer2_cluster_ref(hammer2_cluster_t *cluster)
{
	atomic_add_int(&cluster->refs, 1);
}

void
hammer2_cluster_drop(hammer2_cluster_t *cluster)
{
	hammer2_chain_t *chain;
	int i;

	KKASSERT(cluster->refs > 0);
	if (atomic_fetchadd_int(&cluster->refs, -1) == 1) {
		cluster->focus = NULL;
		cluster->focus_index = 0;

		for (i = 0; i < cluster->nchains; ++i) {
			chain = cluster->array[i].chain;
			if (chain) {
				hammer2_chain_drop(chain);
				cluster->array[i].chain = NULL;
			}
		}
		cluster->nchains = 0;

		kfree(cluster, M_HAMMER2);
	}
}

void
hammer2_cluster_lock(hammer2_cluster_t *cluster, int how)
{
	hammer2_chain_t *chain;
	int i;

	KKASSERT(cluster->refs > 0);
	KKASSERT((cluster->flags & HAMMER2_CLUSTER_INODE) == 0);
	if (cluster->flags & HAMMER2_CLUSTER_LOCKED) {
		panic("hammer2_cluster_lock: cluster %p already locked!\n",
			cluster);
	}
	atomic_set_int(&cluster->flags, HAMMER2_CLUSTER_LOCKED);

	for (i = 0; i < cluster->nchains; ++i) {
		chain = cluster->array[i].chain;
		if (chain == NULL)
			continue;
		hammer2_chain_lock(chain, how);
	}
}

void
hammer2_cluster_unhold(hammer2_cluster_t *cluster)
{
	hammer2_chain_t *chain;
	int i;

	for (i = 0; i < cluster->nchains; ++i) {
		chain = cluster->array[i].chain;
		if (chain == NULL)
			continue;
		hammer2_chain_unhold(chain);
	}
}

void
hammer2_cluster_rehold(hammer2_cluster_t *cluster)
{
	hammer2_chain_t *chain;
	int i;

	for (i = 0; i < cluster->nchains; ++i) {
		chain = cluster->array[i].chain;
		if (chain == NULL)
			continue;
		hammer2_chain_rehold(chain);
	}
}

int
hammer2_cluster_check(hammer2_cluster_t *cluster, hammer2_key_t key, int flags)
{
	hammer2_chain_t *chain;
	hammer2_chain_t *focus;
	hammer2_pfs_t *pmp;
	hammer2_tid_t quorum_tid;
	hammer2_tid_t last_best_quorum_tid;
	uint32_t nflags;
	int ttlmasters;
	int ttlslaves;
	int nmasters;
	int nmasters_keymatch;
	int nslaves;
	int nquorum;
	int umasters;
	int error;
	int i;

	cluster->error = 0;
	cluster->focus = NULL;

	pmp = cluster->pmp;
	KKASSERT(pmp != NULL || cluster->nchains == 0);

	nquorum = pmp ? pmp->pfs_nmasters / 2 + 1 : 0;
	nflags = 0;
	ttlmasters = 0;
	ttlslaves = 0;

	for (i = 0; i < cluster->nchains; ++i) {
		cluster->array[i].flags &= ~HAMMER2_CITEM_FEMOD;
		cluster->array[i].flags |= HAMMER2_CITEM_INVALID;

		chain = cluster->array[i].chain;
		error = cluster->array[i].error;
		if (chain && error) {
			if (cluster->focus == NULL || cluster->focus == chain) {
			}

			switch (cluster->pmp->pfs_types[i]) {
			case HAMMER2_PFSTYPE_SUPROOT:
			case HAMMER2_PFSTYPE_MASTER:
				++ttlmasters;
				break;
			case HAMMER2_PFSTYPE_SLAVE:
				++ttlslaves;
				break;
			}
			continue;
		}
		switch (cluster->pmp->pfs_types[i]) {
		case HAMMER2_PFSTYPE_MASTER:
			++ttlmasters;
			break;
		case HAMMER2_PFSTYPE_SLAVE:
			++ttlslaves;
			break;
		case HAMMER2_PFSTYPE_SOFT_MASTER:
			nflags |= HAMMER2_CLUSTER_WRSOFT;
			nflags |= HAMMER2_CLUSTER_RDSOFT;
			break;
		case HAMMER2_PFSTYPE_SOFT_SLAVE:
			nflags |= HAMMER2_CLUSTER_RDSOFT;
			break;
		case HAMMER2_PFSTYPE_SUPROOT:
			++ttlmasters;
			nflags |= HAMMER2_CLUSTER_WRHARD;
			nflags |= HAMMER2_CLUSTER_RDHARD;
			cluster->focus_index = i;
			cluster->focus = chain;
			cluster->error = error;
			break;
		default:
			break;
		}
	}

	last_best_quorum_tid = HAMMER2_TID_MAX;
	umasters = 0;
	nmasters = 0;
	nmasters_keymatch = 0;
	quorum_tid = 0;

	while (nmasters < nquorum && last_best_quorum_tid != 0) {
		umasters = 0;
		nmasters = 0;
		nmasters_keymatch = 0;
		quorum_tid = 0;

		for (i = 0; i < cluster->nchains; ++i) {
			switch(cluster->pmp->pfs_types[i]) {
			case HAMMER2_PFSTYPE_MASTER:
			case HAMMER2_PFSTYPE_SUPROOT:
				break;
			default:
				continue;
			}

			chain = cluster->array[i].chain;
			error = cluster->array[i].error;

			if (chain == NULL && (cluster->array[i].flags &
					      HAMMER2_CITEM_NULL) == 0) {
				++umasters;
				continue;
			}

			if (flags & HAMMER2_CHECK_NULL) {
				if (chain == NULL) {
					++nmasters;
					++nmasters_keymatch;
					if (cluster->error == 0)
						cluster->error = error;
				}
			} else if (chain &&
				   (key == (hammer2_key_t)-1 ||
				    chain->bref.key == key)) {
				++nmasters_keymatch;

				if (chain->bref.modify_tid <
				     last_best_quorum_tid &&
				    quorum_tid < chain->bref.modify_tid) {
					nmasters = 0;
					quorum_tid = chain->bref.modify_tid;
				}
				if (quorum_tid == chain->bref.modify_tid) {
					++nmasters;
					if (chain->error == 0) {
						cluster->focus = chain;
						cluster->focus_index = i;
					}
				}
			}
		}
		if (nmasters >= nquorum)
			break;
		last_best_quorum_tid = quorum_tid;
	}

	if (nmasters < nquorum) {
		if (nmasters + umasters >= nquorum)
			return HAMMER2_ERROR_EINPROGRESS;
		if (nmasters_keymatch < nquorum)
			return HAMMER2_ERROR_ESRCH;
		return HAMMER2_ERROR_EDEADLK;
	}

	if (flags & HAMMER2_CHECK_NULL) {
		if (cluster->error == 0)
			cluster->error = HAMMER2_ERROR_ENOENT;
		return cluster->error;
	}

	if (cluster->focus == NULL)
		return HAMMER2_ERROR_EIO;

	nslaves = 0;
	cluster->error = 0;

	for (i = 0; i < cluster->nchains; ++i) {
		chain = cluster->array[i].chain;
		error = cluster->array[i].error;
		if (chain == NULL ||
		    chain->bref.key != key ||
		    chain->bref.modify_tid != quorum_tid) {
			continue;
		}

		if (cluster->error == 0)
			cluster->error = error;

		switch (cluster->pmp->pfs_types[i]) {
		case HAMMER2_PFSTYPE_MASTER:
			cluster->array[i].flags |= HAMMER2_CITEM_FEMOD;
			cluster->array[i].flags &= ~HAMMER2_CITEM_INVALID;
			nflags |= HAMMER2_CLUSTER_WRHARD;
			nflags |= HAMMER2_CLUSTER_RDHARD;
			break;
		case HAMMER2_PFSTYPE_SLAVE:
			cluster->array[i].flags &= ~HAMMER2_CITEM_INVALID;
			nflags |= HAMMER2_CLUSTER_RDHARD;
			++nslaves;
			break;
		case HAMMER2_PFSTYPE_SOFT_MASTER:
			cluster->array[i].flags |= HAMMER2_CITEM_FEMOD;
			cluster->array[i].flags &= ~HAMMER2_CITEM_INVALID;
			break;
		case HAMMER2_PFSTYPE_SOFT_SLAVE:
			cluster->array[i].flags &= ~HAMMER2_CITEM_INVALID;
			break;
		case HAMMER2_PFSTYPE_SUPROOT:
			cluster->array[i].flags |= HAMMER2_CITEM_FEMOD;
			cluster->array[i].flags &= ~HAMMER2_CITEM_INVALID;
			nflags |= HAMMER2_CLUSTER_WRHARD;
			nflags |= HAMMER2_CLUSTER_RDHARD;
			break;
		default:
			break;
		}
	}

	focus = cluster->focus;
	if (focus) {
		cluster->ddflag =
			(cluster->focus->bref.type == HAMMER2_BREF_TYPE_INODE);
	} else {
		cluster->ddflag = 0;
		goto skip4;
	}
	if (cluster->focus->flags & HAMMER2_CHAIN_PFSBOUNDARY)
		goto skip4;

	for (i = 0; i < cluster->nchains; ++i) {
		int ddflag;

		chain = cluster->array[i].chain;

		if (chain == NULL)
			continue;
		if (chain == focus)
			continue;
		if (cluster->array[i].flags & HAMMER2_CITEM_INVALID)
			continue;

		ddflag = (chain->bref.type == HAMMER2_BREF_TYPE_INODE);
		if (chain->bref.type != focus->bref.type ||
		    chain->bref.key != focus->bref.key ||
		    chain->bref.keybits != focus->bref.keybits ||
		    chain->bref.modify_tid != focus->bref.modify_tid ||
		    chain->bytes != focus->bytes ||
		    ddflag != cluster->ddflag) {
			cluster->array[i].flags |= HAMMER2_CITEM_INVALID;
			if (hammer2_debug & 1)
			kprintf("cluster_check: matching modify_tid failed "
				"bref test: idx=%d type=%02x/%02x "
				"key=%016jx/%d-%016jx/%d "
				"mod=%016jx/%016jx bytes=%u/%u\n",
				i,
				chain->bref.type, focus->bref.type,
				chain->bref.key, chain->bref.keybits,
				focus->bref.key, focus->bref.keybits,
				chain->bref.modify_tid, focus->bref.modify_tid,
				chain->bytes, focus->bytes);
			if (hammer2_debug & 0x4000)
				panic("cluster_check");
		}
	}
skip4:

	if (ttlslaves == 0)
		nflags |= HAMMER2_CLUSTER_NOSOFT;
	if (ttlmasters == 0)
		nflags |= HAMMER2_CLUSTER_NOHARD;

	if (nslaves == ttlslaves)
		nflags |= HAMMER2_CLUSTER_SSYNCED;
	if (nmasters == ttlmasters)
		nflags |= HAMMER2_CLUSTER_MSYNCED;

	atomic_set_int(&cluster->flags, nflags);
	atomic_clear_int(&cluster->flags, HAMMER2_CLUSTER_ZFLAGS & ~nflags);

	return cluster->error;
}

void
hammer2_cluster_unlock(hammer2_cluster_t *cluster)
{
	hammer2_chain_t *chain;
	int i;

	if ((cluster->flags & HAMMER2_CLUSTER_LOCKED) == 0) {
		kprintf("hammer2_cluster_unlock: cluster %p not locked\n",
			cluster);
	}
	KKASSERT(cluster->flags & HAMMER2_CLUSTER_LOCKED);
	KKASSERT(cluster->refs > 0);
	atomic_clear_int(&cluster->flags, HAMMER2_CLUSTER_LOCKED);

	for (i = 0; i < cluster->nchains; ++i) {
		chain = cluster->array[i].chain;
		if (chain)
			hammer2_chain_unlock(chain);
	}
}
