#include "hammer2.h"

#define H2XOPDESCRIPTOR(label)					\
	hammer2_xop_desc_t hammer2_##label##_desc = {		\
		.storage_func = hammer2_xop_##label,		\
		.id = #label					\
	}

H2XOPDESCRIPTOR(ipcluster);
H2XOPDESCRIPTOR(readdir);
H2XOPDESCRIPTOR(nresolve);
H2XOPDESCRIPTOR(unlink);
H2XOPDESCRIPTOR(nrename);
H2XOPDESCRIPTOR(scanlhc);
H2XOPDESCRIPTOR(scanall);
H2XOPDESCRIPTOR(lookup);
H2XOPDESCRIPTOR(delete);
H2XOPDESCRIPTOR(inode_mkdirent);
H2XOPDESCRIPTOR(inode_create);
H2XOPDESCRIPTOR(inode_create_det);
H2XOPDESCRIPTOR(inode_create_ins);
H2XOPDESCRIPTOR(inode_destroy);
H2XOPDESCRIPTOR(inode_chain_sync);
H2XOPDESCRIPTOR(inode_unlinkall);
H2XOPDESCRIPTOR(inode_connect);
H2XOPDESCRIPTOR(inode_flush);
H2XOPDESCRIPTOR(strategy_read);
H2XOPDESCRIPTOR(strategy_write);
H2XOPDESCRIPTOR(bmap);

static void
hammer2_xop_fifo_alloc(hammer2_xop_fifo_t *fifo, size_t new_nmemb,
    size_t old_nmemb, int want_brefs)
{
	int flags = M_WAITOK | M_ZERO;
	size_t new_size, old_size;

	KKASSERT(new_nmemb > old_nmemb);
	if (old_nmemb == 0)
		KKASSERT(!fifo->array && !fifo->errors);
	else
		KKASSERT(fifo->array && fifo->errors);

	KKASSERT((new_nmemb & (new_nmemb - 1)) == 0);
	KKASSERT(new_nmemb >= HAMMER2_XOPFIFO);

	new_size = new_nmemb * sizeof(hammer2_chain_t *);
	old_size = old_nmemb * sizeof(hammer2_chain_t *);
	if (!fifo->array)
		fifo->array = hmalloc(new_size, M_HAMMER2, flags);
	else
		fifo->array = hrealloc(fifo->array, new_size, M_HAMMER2, flags);
	KKASSERT(fifo->array);
	adjust_malloc_leak(-old_size, M_HAMMER2);

	new_size = new_nmemb * sizeof(int);
	old_size = old_nmemb * sizeof(int);
	if (!fifo->errors)
		fifo->errors = hmalloc(new_size, M_HAMMER2, flags);
	else
		fifo->errors = hrealloc(fifo->errors, new_size, M_HAMMER2,
		    flags);
	KKASSERT(fifo->errors);
	adjust_malloc_leak(-old_size, M_HAMMER2);

	if (want_brefs) {
		new_size = new_nmemb * sizeof(hammer2_blockref_t);
		old_size = old_nmemb * sizeof(hammer2_blockref_t);
		if (!fifo->brefs)
			fifo->brefs = hmalloc(new_size, M_HAMMER2, flags);
		else
			fifo->brefs = hrealloc(fifo->brefs, new_size, M_HAMMER2,
			    flags);
		KKASSERT(fifo->brefs);
		adjust_malloc_leak(-old_size, M_HAMMER2);
	}
}

void *
hammer2_xop_alloc(hammer2_inode_t *ip, int flags)
{
	hammer2_xop_t *xop;

	xop = uma_zalloc(hammer2_zone_xops, M_WAITOK | M_ZERO);
	KKASSERT(xop->head.cluster.array[0].chain == NULL);

	xop->head.ip1 = ip;
	xop->head.flags = flags;

	if (flags & HAMMER2_XOP_MODIFYING)
		xop->head.mtid = hammer2_trans_sub(ip->pmp);
	else
		xop->head.mtid = 0;

	xop->head.cluster.nchains = ip->cluster.nchains;
	xop->head.cluster.pmp = ip->pmp;
	hammer2_assert_cluster(&ip->cluster);

	xop->head.run_mask = HAMMER2_XOPMASK_VOP;

	hammer2_xop_fifo_t *fifo = &xop->head.collect[0];
	xop->head.fifo_size = HAMMER2_XOPFIFO;
	hammer2_xop_fifo_alloc(fifo, xop->head.fifo_size, 0,
	    flags & HAMMER2_XOP_FIFO_BREF);

	hammer2_inode_ref(ip);

	return (xop);
}

void
hammer2_xop_setbound(hammer2_xop_head_t *xop, int bound)
{
	hammer2_xop_fifo_t *fifo = &xop->collect[0];
	hammer2_chain_t **narray;
	hammer2_blockref_t *nbrefs;
	int *nerrors;
	int want;

	KKASSERT(xop->flags & HAMMER2_XOP_FIFO_BOUND);
	KKASSERT(bound > 0);

	want = HAMMER2_XOPFIFO;
	while (want < bound)
		want <<= 1;

	if (want > xop->fifo_size) {
		narray = hrealloc(fifo->array,
		    want * sizeof(hammer2_chain_t *), M_HAMMER2, M_WAITOK);
		if (narray) {
			fifo->array = narray;
			nerrors = hrealloc(fifo->errors, want * sizeof(int),
			    M_HAMMER2, M_WAITOK);
			if (nerrors) {
				fifo->errors = nerrors;

				if (xop->flags & HAMMER2_XOP_FIFO_BREF) {
					nbrefs = hrealloc(fifo->brefs,
					    want * sizeof(hammer2_blockref_t),
					    M_HAMMER2, M_WAITOK);
					if (nbrefs) {
						fifo->brefs = nbrefs;
						xop->fifo_size = want;
					}
				} else {
					xop->fifo_size = want;
				}
			}
		}
	}

	if (bound > xop->fifo_size)
		bound = xop->fifo_size;
	xop->fifo_bound = bound;
}

void
hammer2_xop_setname(hammer2_xop_head_t *xop, const char *name, size_t name_len)
{
	xop->name1 = hmalloc(name_len + 1, M_HAMMER2, M_WAITOK | M_ZERO);
	xop->name1_len = name_len;
	bcopy(name, xop->name1, name_len);
}

void
hammer2_xop_setname2(hammer2_xop_head_t *xop, const char *name, size_t name_len)
{
	xop->name2 = hmalloc(name_len + 1, M_HAMMER2, M_WAITOK | M_ZERO);
	xop->name2_len = name_len;
	bcopy(name, xop->name2, name_len);
}

size_t
hammer2_xop_setname_inum(hammer2_xop_head_t *xop, hammer2_key_t inum)
{
	const size_t name_len = 18;

	xop->name1 = hmalloc(name_len + 1, M_HAMMER2, M_WAITOK | M_ZERO);
	xop->name1_len = name_len;
	snprintf(xop->name1, name_len + 1, "0x%016llx", (long long)inum);

	return (name_len);
}

void
hammer2_xop_setip2(hammer2_xop_head_t *xop, hammer2_inode_t *ip2)
{
	xop->ip2 = ip2;
	hammer2_inode_ref(ip2);
}

void
hammer2_xop_setip3(hammer2_xop_head_t *xop, hammer2_inode_t *ip3)
{
	xop->ip3 = ip3;
	hammer2_inode_ref(ip3);
}

void
hammer2_xop_setip4(hammer2_xop_head_t *xop, hammer2_inode_t *ip4)
{
	xop->ip4 = ip4;
	hammer2_inode_ref(ip4);
}

static __inline int
hammer2_xop_active(const hammer2_xop_head_t *xop)
{
	if (xop->run_mask & HAMMER2_XOPMASK_VOP)
		return (1);
	else
		return (0);
}

static int
xop_testset_ipdep(hammer2_inode_t *ip, int idx)
{
	hammer2_ipdep_list_t *ipdep;
	hammer2_inode_t *iptmp;

	hammer2_lk_assert_ex(&ip->pmp->xop_lock[idx]);

	ipdep = &ip->pmp->ipdep_lists[idx];
	LIST_FOREACH(iptmp, ipdep, ientry)
		if (iptmp == ip)
			return (1);

	LIST_INSERT_HEAD(ipdep, ip, ientry);
	return (0);
}

static void
xop_unset_ipdep(hammer2_inode_t *ip, int idx)
{
	hammer2_ipdep_list_t *ipdep;
	hammer2_inode_t *iptmp;

	hammer2_lk_assert_ex(&ip->pmp->xop_lock[idx]);

	ipdep = &ip->pmp->ipdep_lists[idx];
	LIST_FOREACH(iptmp, ipdep, ientry)
		if (iptmp == ip) {
			LIST_REMOVE(ip, ientry);
			return;
		}
}

static void
hammer2_xop_testset_ipdep(hammer2_inode_t *ip)
{
	hammer2_pfs_t *pmp = ip->pmp;
	hammer2_lk_t *mtx;
	hammer2_lkc_t *cv;

	mtx = &pmp->xop_lock[ip->ipdep_idx];
	cv = &pmp->xop_cv[ip->ipdep_idx];

	hammer2_lk_ex(mtx);
again:
	if (xop_testset_ipdep(ip, ip->ipdep_idx)) {
		pmp->flags |= HAMMER2_PMPF_WAITING;
		hammer2_lkc_sleep(cv, mtx, "h2pmp_xop", 0);
		goto again;
	}
	hammer2_lk_unlock(mtx);
}

static int
hammer2_xop_testset_ipdep_try(hammer2_inode_t *ip)
{
	hammer2_pfs_t *pmp = ip->pmp;
	hammer2_lk_t *mtx = &pmp->xop_lock[ip->ipdep_idx];
	int collision;

	hammer2_lk_ex(mtx);
	collision = xop_testset_ipdep(ip, ip->ipdep_idx);
	hammer2_lk_unlock(mtx);

	return (collision);
}

static void
hammer2_xop_unset_ipdep(hammer2_inode_t *ip)
{
	hammer2_pfs_t *pmp = ip->pmp;
	hammer2_lk_t *mtx;
	hammer2_lkc_t *cv;

	mtx = &pmp->xop_lock[ip->ipdep_idx];
	cv = &pmp->xop_cv[ip->ipdep_idx];

	hammer2_lk_ex(mtx);
	xop_unset_ipdep(ip, ip->ipdep_idx);

	pmp->flags &= ~HAMMER2_PMPF_WAITING;
	hammer2_lkc_wakeup(cv);

	hammer2_lk_unlock(mtx);
}

#ifdef HAMMER2_INVARIANTS

static __inline void
xop_storage_func(hammer2_xop_head_t *xop, hammer2_inode_t *ip, void *scratch,
    int i)
{
#ifdef XOP_ADMIN_DEBUG
	hprintf("xop_%s inum %016llx index %d\n",
	    xop->desc->id, (long long)ip->meta.inum, i);
#endif
	xop->desc->storage_func((hammer2_xop_t *)xop, scratch, i);
#ifdef XOP_ADMIN_DEBUG
	hprintf("xop_%s inum %016llx index %d done\n",
	    xop->desc->id, (long long)ip->meta.inum, i);
#endif
}
#else
#define xop_storage_func(xop, ip, scratch, i)	\
	xop->desc->storage_func((hammer2_xop_t *)xop, scratch, i)
#endif

void
hammer2_xop_start(hammer2_xop_head_t *xop, hammer2_xop_desc_t *desc)
{
	hammer2_inode_t *ip = xop->ip1;
	uint32_t mask;
	int i;

	KKASSERT(ip);
	hammer2_assert_cluster(&ip->cluster);
	xop->desc = desc;

	if (desc == &hammer2_strategy_write_desc)
		xop->scratch = hmalloc(hammer2_get_logical(), M_HAMMER2,
		    M_WAITOK | M_ZERO);

	for (i = 0; i < ip->cluster.nchains; ++i) {
		mask = 1LLU << i;
		if (ip->cluster.array[i].chain) {
			atomic_set_32(&xop->run_mask, mask);
			atomic_set_32(&xop->chk_mask, mask);
		} else {
			continue;
		}

		if (hammer2_xop_active(xop)) {

			if (xop->flags & HAMMER2_XOP_NONBLOCK) {
				hammer2_inode_t *got2 = NULL, *got3 = NULL;
				int collision, got1;

				collision = hammer2_xop_testset_ipdep_try(ip);
				got1 = !collision;
				if (!collision && xop->ip2) {
					collision = hammer2_xop_testset_ipdep_try(xop->ip2);
					if (!collision)
						got2 = xop->ip2;
				}
				if (!collision && xop->ip3 && xop->ip3 != xop->ip1) {
					collision = hammer2_xop_testset_ipdep_try(xop->ip3);
					if (!collision)
						got3 = xop->ip3;
				}
				if (!collision && xop->ip4 && xop->ip4 != xop->ip2)
					collision = hammer2_xop_testset_ipdep_try(xop->ip4);
				if (collision) {
					if (got3)
						hammer2_xop_unset_ipdep(got3);
					if (got2)
						hammer2_xop_unset_ipdep(got2);

					if (got1)
						hammer2_xop_unset_ipdep(ip);
					xop->flags |= HAMMER2_XOP_ABORTED;
					hammer2_xop_feed(xop, NULL, i, EAGAIN);
					hammer2_xop_retire(xop, mask);
					continue;
				}
			} else {
				hammer2_xop_testset_ipdep(ip);
				if (xop->ip2)
					hammer2_xop_testset_ipdep(xop->ip2);
				if (xop->ip3 && xop->ip3 != xop->ip1)
					hammer2_xop_testset_ipdep(xop->ip3);
				if (xop->ip4 && xop->ip4 != xop->ip2)
					hammer2_xop_testset_ipdep(xop->ip4);
			}
			xop_storage_func(xop, ip, xop->scratch, i);
			hammer2_xop_retire(xop, mask);
		} else {
			hammer2_xop_feed(xop, NULL, i, ECONNABORTED);
			hammer2_xop_retire(xop, mask);
		}
	}
}

void
hammer2_xop_start_except(hammer2_xop_head_t *xop, hammer2_xop_desc_t *desc,
			 int notidx)
{
	hammer2_inode_t *ip = xop->ip1;
	uint32_t mask;
	int i;

	KKASSERT(ip);
	hammer2_assert_cluster(&ip->cluster);
	xop->desc = desc;

	if (desc == &hammer2_strategy_write_desc)
		xop->scratch = hmalloc(hammer2_get_logical(), M_HAMMER2,
		    M_WAITOK | M_ZERO);

	for (i = 0; i < ip->cluster.nchains; ++i) {
		if (i == notidx)
			continue;
		mask = 1LLU << i;
		if (ip->cluster.array[i].chain) {
			atomic_set_32(&xop->run_mask, mask);
			atomic_set_32(&xop->chk_mask, mask);
		} else {
			continue;
		}

		if (hammer2_xop_active(xop)) {

			if (xop->flags & HAMMER2_XOP_NONBLOCK) {
				hammer2_inode_t *got2 = NULL, *got3 = NULL;
				int collision, got1;

				collision = hammer2_xop_testset_ipdep_try(ip);
				got1 = !collision;
				if (!collision && xop->ip2) {
					collision = hammer2_xop_testset_ipdep_try(xop->ip2);
					if (!collision)
						got2 = xop->ip2;
				}
				if (!collision && xop->ip3 && xop->ip3 != xop->ip1) {
					collision = hammer2_xop_testset_ipdep_try(xop->ip3);
					if (!collision)
						got3 = xop->ip3;
				}
				if (!collision && xop->ip4 && xop->ip4 != xop->ip2)
					collision = hammer2_xop_testset_ipdep_try(xop->ip4);
				if (collision) {
					if (got3)
						hammer2_xop_unset_ipdep(got3);
					if (got2)
						hammer2_xop_unset_ipdep(got2);

					if (got1)
						hammer2_xop_unset_ipdep(ip);
					xop->flags |= HAMMER2_XOP_ABORTED;
					hammer2_xop_feed(xop, NULL, i, EAGAIN);
					hammer2_xop_retire(xop, mask);
					continue;
				}
			} else {
				hammer2_xop_testset_ipdep(ip);
				if (xop->ip2)
					hammer2_xop_testset_ipdep(xop->ip2);
				if (xop->ip3 && xop->ip3 != xop->ip1)
					hammer2_xop_testset_ipdep(xop->ip3);
				if (xop->ip4 && xop->ip4 != xop->ip2)
					hammer2_xop_testset_ipdep(xop->ip4);
			}
			xop_storage_func(xop, ip, xop->scratch, i);
			hammer2_xop_retire(xop, mask);
		} else {
			hammer2_xop_feed(xop, NULL, i, ECONNABORTED);
			hammer2_xop_retire(xop, mask);
		}
	}
}

void
hammer2_xop_retire(hammer2_xop_head_t *xop, uint32_t mask)
{
	hammer2_chain_t *chain, *dropch[HAMMER2_MAXCLUSTER];
	hammer2_inode_t *ip;
	hammer2_xop_fifo_t *fifo;
	uint32_t omask;
	int prior_nchains, i;

	KASSERTMSG(xop->run_mask & mask,
	    "run_mask %x vs mask %x", xop->run_mask, mask);
	omask = atomic_fetchadd_32(&xop->run_mask, -mask);

	if ((omask & HAMMER2_XOPMASK_ALLDONE) != mask)
		return;

	if (xop->ip1) {

		ip = xop->ip1;
		hammer2_spin_ex(&ip->cluster_spin);
		prior_nchains = ip->ccache_nchains;
		for (i = 0; i < prior_nchains; ++i) {
			dropch[i] = ip->ccache[i].chain;
			ip->ccache[i].chain = NULL;
		}
		for (i = 0; i < xop->cluster.nchains; ++i) {
			ip->ccache[i] = xop->cluster.array[i];
			if (ip->ccache[i].chain)
				hammer2_chain_ref(ip->ccache[i].chain);
		}
		ip->ccache_nchains = i;
		hammer2_spin_unex(&ip->cluster_spin);

		for (i = 0; i < prior_nchains; ++i) {
			chain = dropch[i];
			if (chain)
				hammer2_chain_drop(chain);
		}
	}

	for (i = 0; i < xop->cluster.nchains; ++i) {
		xop->cluster.array[i].flags = 0;
		chain = xop->cluster.array[i].chain;
		if (chain) {
			xop->cluster.array[i].chain = NULL;
			hammer2_chain_drop_unhold(chain);
		}
	}

	mask = xop->chk_mask;
	for (i = 0; mask && i < HAMMER2_MAXCLUSTER; ++i) {
		fifo = &xop->collect[i];
		while (fifo->ri != fifo->wi) {
			chain = fifo->array[fifo->ri & fifo_mask(xop)];
			if (chain)
				hammer2_chain_drop_unhold(chain);
			++fifo->ri;
		}
		mask &= ~(1U << i);
	}

	if (xop->flags & HAMMER2_XOP_ABORTED) {
		if (xop->ip1) { hammer2_inode_drop(xop->ip1); xop->ip1 = NULL; }
		if (xop->ip2) { hammer2_inode_drop(xop->ip2); xop->ip2 = NULL; }
		if (xop->ip3) { hammer2_inode_drop(xop->ip3); xop->ip3 = NULL; }
		if (xop->ip4) { hammer2_inode_drop(xop->ip4); xop->ip4 = NULL; }
	}
	if (xop->ip1) {
		hammer2_xop_unset_ipdep(xop->ip1);
		hammer2_inode_drop(xop->ip1);
		xop->ip1 = NULL;
	}
	if (xop->ip2) {
		hammer2_xop_unset_ipdep(xop->ip2);
		hammer2_inode_drop(xop->ip2);
		xop->ip2 = NULL;
	}
	if (xop->ip3) {
		if (xop->ip3 && xop->ip3 != xop->ip1)
			hammer2_xop_unset_ipdep(xop->ip3);
		hammer2_inode_drop(xop->ip3);
		xop->ip3 = NULL;
	}
	if (xop->ip4) {
		if (xop->ip4 && xop->ip4 != xop->ip2)
			hammer2_xop_unset_ipdep(xop->ip4);
		hammer2_inode_drop(xop->ip4);
		xop->ip4 = NULL;
	}

	if (xop->name1) {
		hfree(xop->name1, M_HAMMER2, strlen(xop->name1) + 1);
		xop->name1 = NULL;
		xop->name1_len = 0;
	}
	if (xop->name2) {
		hfree(xop->name2, M_HAMMER2, strlen(xop->name2) + 1);
		xop->name2 = NULL;
		xop->name2_len = 0;
	}

	for (i = 0; i < xop->cluster.nchains; ++i) {
		fifo = &xop->collect[i];
		KKASSERT(fifo->array == NULL || xop->fifo_size > 0);
		KKASSERT(fifo->errors == NULL || xop->fifo_size > 0);
		hfree(fifo->array, M_HAMMER2,
		    xop->fifo_size * sizeof(hammer2_chain_t *));
		hfree(fifo->errors, M_HAMMER2, xop->fifo_size * sizeof(int));
		if (fifo->brefs) {
			hfree(fifo->brefs, M_HAMMER2,
			    xop->fifo_size * sizeof(hammer2_blockref_t));
			fifo->brefs = NULL;
		}
	}

	if (xop->scratch)
		hfree(xop->scratch, M_HAMMER2, hammer2_get_logical());

	uma_zfree(hammer2_zone_xops, xop);
}

static int
hammer2_xop_feed_slot(hammer2_xop_head_t *xop, hammer2_chain_t *chain,
    const hammer2_blockref_t *bref, int clindex, int error)
{
	hammer2_xop_fifo_t *fifo;
	size_t old_fifo_size;

	if (hammer2_xop_active(xop) == 0) {
		error = HAMMER2_ERROR_ABORTED;
		goto done;
	}

	fifo = &xop->collect[clindex];

	if ((chain || bref) && (xop->flags & HAMMER2_XOP_FIFO_BOUND)) {
		int bound = xop->fifo_bound ? xop->fifo_bound :
		    HAMMER2_XOPFIFO_BOUND;
		if (fifo->wi >= bound) {
			error = HAMMER2_ERROR_ABORTED;
			goto done;
		}
	}

	while (fifo->ri == fifo->wi - xop->fifo_size) {
		if ((xop->run_mask & HAMMER2_XOPMASK_VOP) == 0) {
			error = HAMMER2_ERROR_ABORTED;
			goto done;
		}

		if ((xop->flags & HAMMER2_XOP_FIFO_BOUND) &&
		    xop->fifo_size >= (xop->fifo_bound ? xop->fifo_bound :
		    HAMMER2_XOPFIFO_BOUND)) {
			error = HAMMER2_ERROR_ABORTED;
			goto done;
		}
		old_fifo_size = xop->fifo_size;
		xop->fifo_size *= 2;
		hammer2_xop_fifo_alloc(fifo, xop->fifo_size, old_fifo_size,
		    xop->flags & HAMMER2_XOP_FIFO_BREF);
	}

	if (chain)
		hammer2_chain_ref_hold(chain);
	if (error == 0 && chain)
		error = chain->error;
	fifo->errors[fifo->wi & fifo_mask(xop)] = error;
	fifo->array[fifo->wi & fifo_mask(xop)] = chain;
	if (bref) {
		fifo->brefs[fifo->wi & fifo_mask(xop)] = *bref;
	} else if (xop->flags & HAMMER2_XOP_FIFO_BREF) {

		fifo->brefs[fifo->wi & fifo_mask(xop)].type =
		    HAMMER2_BREF_TYPE_EMPTY;
	}
	++fifo->wi;

	error = 0;
done:
	return (error);
}

int
hammer2_xop_feed(hammer2_xop_head_t *xop, hammer2_chain_t *chain, int clindex,
    int error)
{
	return (hammer2_xop_feed_slot(xop, chain, NULL, clindex, error));
}

int
hammer2_xop_feed_bref(hammer2_xop_head_t *xop, const hammer2_blockref_t *bref,
    hammer2_chain_t *chain, int clindex, int error)
{
	KKASSERT(xop->flags & HAMMER2_XOP_FIFO_BREF);
	KKASSERT(bref != NULL && bref->type != HAMMER2_BREF_TYPE_EMPTY);

	return (hammer2_xop_feed_slot(xop, chain, bref, clindex, error));
}

int
hammer2_xop_collect_bref(hammer2_xop_head_t *xop, hammer2_blockref_t *brefp,
    hammer2_chain_t **chainp)
{
	hammer2_xop_fifo_t *fifo = &xop->collect[0];
	int error, i;

	KKASSERT(xop->flags & HAMMER2_XOP_FIFO_BREF);
	KKASSERT(xop->cluster.nchains == 1);

	*chainp = NULL;

	if (xop->cluster.array[0].chain) {
		hammer2_chain_drop_unhold(xop->cluster.array[0].chain);
		xop->cluster.array[0].chain = NULL;
	}

	if (fifo->ri == fifo->wi)
		return (HAMMER2_ERROR_ENOENT);

	i = fifo->ri & fifo_mask(xop);
	error = fifo->errors[i];
	*chainp = fifo->array[i];
	*brefp = fifo->brefs[i];
	xop->cluster.array[0].chain = *chainp;
	++fifo->ri;

	if (brefp->type == HAMMER2_BREF_TYPE_EMPTY) {
		if (error == 0 || error == HAMMER2_ERROR_ABORTED)
			return (HAMMER2_ERROR_ENOENT);
		return (error);
	}
	return (error);
}

int
hammer2_xop_collect(hammer2_xop_head_t *xop, int flags)
{
	hammer2_xop_fifo_t *fifo;
	hammer2_chain_t *chain;
	hammer2_key_t lokey;
	int i, keynull, adv, error;

	lokey = HAMMER2_KEY_MAX;
	keynull = HAMMER2_CHECK_NULL;

	for (i = 0; i < xop->cluster.nchains; ++i) {
		chain = xop->cluster.array[i].chain;
		if (chain == NULL) {
			adv = 1;
		} else if (chain->bref.key < xop->collect_key) {
			adv = 1;
		} else {
			keynull &= ~HAMMER2_CHECK_NULL;
			if (lokey > chain->bref.key)
				lokey = chain->bref.key;
			adv = 0;
		}
		if (adv == 0)
			continue;

		if (chain)
			hammer2_chain_drop_unhold(chain);

		fifo = &xop->collect[i];
		if (fifo->ri != fifo->wi) {
			chain = fifo->array[fifo->ri & fifo_mask(xop)];
			error = fifo->errors[fifo->ri & fifo_mask(xop)];
			++fifo->ri;
			xop->cluster.array[i].chain = chain;
			xop->cluster.array[i].error = error;
			if (chain == NULL)
				xop->cluster.array[i].flags |=
				    HAMMER2_CITEM_NULL;
			--i;
		} else {

			xop->cluster.array[i].chain = NULL;

		}
	}

	error = hammer2_cluster_check(&xop->cluster, lokey, keynull);

	if (lokey == HAMMER2_KEY_MAX)
		xop->collect_key = lokey;
	else
		xop->collect_key = lokey + 1;

	return (error);
}
