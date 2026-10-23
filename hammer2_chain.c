#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/types.h>
#include <sys/lock.h>
#include <sys/buf.h>

#include <crypto/sha2/sha2.h>

#include "hammer2.h"

static hammer2_chain_t *hammer2_chain_create_indirect(
		hammer2_chain_t *parent,
		hammer2_key_t key, int keybits,
		hammer2_tid_t mtid, int for_type, int *errorp);
static int hammer2_chain_delete_obref(hammer2_chain_t *parent,
		hammer2_chain_t *chain,
		hammer2_tid_t mtid, int flags,
		hammer2_blockref_t *obref);
static hammer2_chain_t *hammer2_combined_find(
		hammer2_chain_t *parent,
		hammer2_blockref_t *base, int count,
		hammer2_key_t *key_nextp,
		hammer2_key_t key_beg, hammer2_key_t key_end,
		hammer2_blockref_t **brefp);
static hammer2_chain_t *hammer2_chain_lastdrop(hammer2_chain_t *chain,
				int depth);
static struct krate krate_h2chk = { .freq = 5 };
static struct krate krate_h2me = { .freq = 1 };
static struct krate krate_h2em = { .freq = 1 };

RB_GENERATE(hammer2_chain_tree, hammer2_chain, rbnode, hammer2_chain_cmp);

int
hammer2_chain_cmp(hammer2_chain_t *chain1, hammer2_chain_t *chain2)
{
	hammer2_key_t c1_beg;
	hammer2_key_t c1_end;
	hammer2_key_t c2_beg;
	hammer2_key_t c2_end;

	c1_beg = chain1->bref.key;
	c1_end = c1_beg + ((hammer2_key_t)1 << chain1->bref.keybits) - 1;
	c2_beg = chain2->bref.key;
	c2_end = c2_beg + ((hammer2_key_t)1 << chain2->bref.keybits) - 1;

	if (c1_end < c2_beg)
		return(-1);
	if (c1_beg > c2_end)
		return(1);
	return(0);
}

static __inline void
hammer2_chain_assert_no_data(hammer2_chain_t *chain)
{
	KKASSERT(chain->dio == NULL);
	if (chain->bref.type != HAMMER2_BREF_TYPE_VOLUME &&
	    chain->bref.type != HAMMER2_BREF_TYPE_FREEMAP &&
	    chain->data) {
		panic("hammer2_chain_assert_no_data: chain %p still has data",
		    chain);
	}
}

void
hammer2_chain_setflush(hammer2_chain_t *chain)
{
	hammer2_chain_t *parent;

	if ((chain->flags & HAMMER2_CHAIN_ONFLUSH) == 0) {
		hammer2_spin_sh(&chain->core.spin);
		while ((chain->flags & HAMMER2_CHAIN_ONFLUSH) == 0) {
			atomic_set_int(&chain->flags, HAMMER2_CHAIN_ONFLUSH);
			if (chain->bref.type == HAMMER2_BREF_TYPE_INODE)
				break;
			if ((parent = chain->parent) == NULL)
				break;
			hammer2_spin_sh(&parent->core.spin);
			hammer2_spin_unsh(&chain->core.spin);
			chain = parent;
		}
		hammer2_spin_unsh(&chain->core.spin);
	}
}

hammer2_chain_t *
hammer2_chain_alloc(hammer2_dev_t *hmp, hammer2_pfs_t *pmp,
		    hammer2_blockref_t *bref)
{
	hammer2_chain_t *chain;
	u_int bytes;

	if ((int)(bref->data_off & HAMMER2_OFF_MASK_RADIX))
		bytes = 1U << (int)(bref->data_off & HAMMER2_OFF_MASK_RADIX);
	else
		bytes = 0;

	switch(bref->type) {
	case HAMMER2_BREF_TYPE_INODE:
	case HAMMER2_BREF_TYPE_INDIRECT:
	case HAMMER2_BREF_TYPE_DATA:
	case HAMMER2_BREF_TYPE_DIRENT:
	case HAMMER2_BREF_TYPE_FREEMAP_NODE:
	case HAMMER2_BREF_TYPE_FREEMAP_LEAF:
	case HAMMER2_BREF_TYPE_FREEMAP:
	case HAMMER2_BREF_TYPE_VOLUME:
		chain = kmalloc_obj(sizeof(*chain), hmp->mchain,
				    M_WAITOK | M_ZERO);
		atomic_add_long(&hammer2_chain_allocs, 1);
		break;
	case HAMMER2_BREF_TYPE_EMPTY:
	default:
		panic("hammer2_chain_alloc: unrecognized blockref type: %d",
		      bref->type);
		break;
	}

	if (pmp == hmp->spmp)
		chain->pmp = NULL;
	else
		chain->pmp = pmp;

	chain->hmp = hmp;
	chain->bref = *bref;
	chain->bytes = bytes;
	chain->refs = 1;
	chain->flags = HAMMER2_CHAIN_ALLOCATED;

	if (bref->flags & HAMMER2_BREF_FLAG_PFSROOT)
		atomic_set_int(&chain->flags, HAMMER2_CHAIN_PFSBOUNDARY);
	hammer2_chain_init(chain);

	return (chain);
}

void
hammer2_chain_init(hammer2_chain_t *chain)
{
	RB_INIT(&chain->core.rbtree);
	hammer2_mtx_init(&chain->lock, "h2chain");
	hammer2_spin_init(&chain->core.spin, "h2chain");
	lockinit(&chain->diolk, "chdio", 0, 0);
}

void
hammer2_chain_ref(hammer2_chain_t *chain)
{
	if (atomic_fetchadd_int(&chain->refs, 1) == 0) {
	}
}

void
hammer2_chain_ref_hold(hammer2_chain_t *chain)
{
	atomic_add_int(&chain->lockcnt, 1);
	hammer2_chain_ref(chain);
}

#define HAMMER2_CHAIN_INSERT_SPIN	0x0001
#define HAMMER2_CHAIN_INSERT_LIVE	0x0002
#define HAMMER2_CHAIN_INSERT_RACE	0x0004

static
int
hammer2_chain_insert(hammer2_chain_t *parent, hammer2_chain_t *chain,
		     int flags, int generation)
{
	hammer2_chain_t *xchain __debugvar;
	int error = 0;

	if (flags & HAMMER2_CHAIN_INSERT_SPIN)
		hammer2_spin_ex(&parent->core.spin);

	if ((flags & HAMMER2_CHAIN_INSERT_RACE) &&
	    parent->core.generation != generation) {
		error = HAMMER2_ERROR_EAGAIN;
		goto failed;
	}

	xchain = RB_INSERT(hammer2_chain_tree, &parent->core.rbtree, chain);
	KASSERT(xchain == NULL,
		("hammer2_chain_insert: collision %p %p (key=%016jx)",
		chain, xchain, chain->bref.key));
	atomic_set_int(&chain->flags, HAMMER2_CHAIN_ONRBTREE);
	chain->parent = parent;
	++parent->core.chain_count;
	++parent->core.generation;

	if (flags & HAMMER2_CHAIN_INSERT_LIVE)
		atomic_add_int(&parent->core.live_count, 1);
failed:
	if (flags & HAMMER2_CHAIN_INSERT_SPIN)
		hammer2_spin_unex(&parent->core.spin);
	return error;
}

void
hammer2_chain_drop(hammer2_chain_t *chain)
{
	u_int refs;

	KKASSERT(chain->refs > 0);

	while (chain) {
		refs = chain->refs;
		cpu_ccfence();
		KKASSERT(refs > 0);

		if (refs == 1) {
			if (hammer2_mtx_ex_try(&chain->lock) == 0)
				chain = hammer2_chain_lastdrop(chain, 0);
		} else {
			if (atomic_cmpset_int(&chain->refs, refs, refs - 1))
				break;
		}
		cpu_pause();
	}
}

void
hammer2_chain_unhold(hammer2_chain_t *chain)
{
	u_int lockcnt;
	int iter = 0;

	for (;;) {
		lockcnt = chain->lockcnt;
		cpu_ccfence();
		if (lockcnt > 1) {
			if (atomic_cmpset_int(&chain->lockcnt,
					      lockcnt, lockcnt - 1)) {
				break;
			}
		} else if (hammer2_mtx_ex_try(&chain->lock) == 0) {
			hammer2_chain_unlock(chain);
			break;
		} else {
			if (++iter > 1000) {
				if (iter > 1000 + hz) {
					kprintf("hammer2: h2race1 %p\n", chain);
					iter = 1000;
				}
				tsleep(&iter, 0, "h2race1", 1);
			}
			cpu_pause();
		}
	}
}

void
hammer2_chain_drop_unhold(hammer2_chain_t *chain)
{
	hammer2_chain_unhold(chain);
	hammer2_chain_drop(chain);
}

void
hammer2_chain_rehold(hammer2_chain_t *chain)
{
	hammer2_chain_lock(chain, HAMMER2_RESOLVE_SHARED);
	atomic_add_int(&chain->lockcnt, 1);
	hammer2_chain_unlock(chain);
}

static
hammer2_chain_t *
hammer2_chain_lastdrop(hammer2_chain_t *chain, int depth)
{
	hammer2_dev_t *hmp;
	hammer2_chain_t *parent;
	hammer2_chain_t *rdrop;

	hammer2_spin_ex(&chain->core.spin);

	if (chain->parent != NULL) {
		if (chain->flags & (HAMMER2_CHAIN_UPDATE |
				    HAMMER2_CHAIN_MODIFIED)) {
			if (atomic_cmpset_int(&chain->refs, 1, 0)) {
				hammer2_spin_unex(&chain->core.spin);
				hammer2_chain_assert_no_data(chain);
				hammer2_mtx_unlock(&chain->lock);
				chain = NULL;
			} else {
				hammer2_spin_unex(&chain->core.spin);
				hammer2_mtx_unlock(&chain->lock);
			}
			return (chain);
		}
	} else if (chain->bref.type == HAMMER2_BREF_TYPE_VOLUME ||
		   chain->bref.type == HAMMER2_BREF_TYPE_FREEMAP) {
		atomic_clear_int(&chain->flags, HAMMER2_CHAIN_UPDATE);
	} else {
		atomic_set_int(&chain->flags, HAMMER2_CHAIN_DESTROY);
		if (chain->flags & HAMMER2_CHAIN_UPDATE)
			atomic_clear_int(&chain->flags, HAMMER2_CHAIN_UPDATE);

		if (chain->core.chain_count) {
			hammer2_spin_unex(&chain->core.spin);
			hammer2_flush(chain, HAMMER2_FLUSH_TOP |
					     HAMMER2_FLUSH_ALL);
			hammer2_mtx_unlock(&chain->lock);

			return(chain);
		}

		if (chain->flags & HAMMER2_CHAIN_MODIFIED) {
			atomic_clear_int(&chain->flags, HAMMER2_CHAIN_MODIFIED);
			atomic_add_long(&hammer2_count_modified_chains, -1);
			if (chain->pmp)
				hammer2_pfs_memory_wakeup(chain->pmp, -1);
		}
	}

	if (chain->core.chain_count) {
		if (atomic_cmpset_int(&chain->refs, 1, 0)) {
			hammer2_spin_unex(&chain->core.spin);
			hammer2_chain_assert_no_data(chain);
			hammer2_mtx_unlock(&chain->lock);
			chain = NULL;
		} else {
			hammer2_spin_unex(&chain->core.spin);
			hammer2_mtx_unlock(&chain->lock);
		}
		return (chain);
	}

	hmp = chain->hmp;
	rdrop = NULL;

	parent = chain->parent;

	if (parent) {
		hammer2_spin_ex(&parent->core.spin);
		if (atomic_cmpset_int(&chain->refs, 1, 0) == 0) {
			hammer2_spin_unex(&parent->core.spin);
			hammer2_spin_unex(&chain->core.spin);
			hammer2_mtx_unlock(&chain->lock);

			return(chain);
		}

		if (chain->flags & HAMMER2_CHAIN_ONRBTREE) {
			if ((parent->flags & HAMMER2_CHAIN_COUNTEDBREFS) &&
			    (chain->flags & HAMMER2_CHAIN_BLKMAPPED) == 0) {
				atomic_add_int(&parent->core.live_count, -1);
			}
			RB_REMOVE(hammer2_chain_tree,
				  &parent->core.rbtree, chain);
			atomic_clear_int(&chain->flags, HAMMER2_CHAIN_ONRBTREE);
			--parent->core.chain_count;
			chain->parent = NULL;
		}

		if (parent->core.chain_count == 0) {
			rdrop = parent;
			atomic_add_int(&rdrop->refs, 1);
		}
		hammer2_spin_unex(&parent->core.spin);
		parent = NULL;
	} else {
		if (atomic_cmpset_int(&chain->refs, 1, 0) == 0) {
			hammer2_spin_unex(&chain->core.spin);
			hammer2_mtx_unlock(&chain->lock);

			return(chain);
		}
	}

	hammer2_spin_unex(&chain->core.spin);
	hammer2_chain_assert_no_data(chain);
	hammer2_mtx_unlock(&chain->lock);
	KKASSERT(RB_EMPTY(&chain->core.rbtree) &&
		 chain->core.chain_count == 0);

	KKASSERT((chain->flags & (HAMMER2_CHAIN_UPDATE |
				  HAMMER2_CHAIN_MODIFIED)) == 0);

	if (chain->flags & HAMMER2_CHAIN_ALLOCATED) {
		atomic_clear_int(&chain->flags, HAMMER2_CHAIN_ALLOCATED);
		chain->hmp = NULL;
		kfree_obj(chain, hmp->mchain);
		atomic_add_long(&hammer2_chain_allocs, -1);
	}

	return(rdrop);
}

static hammer2_io_t *
hammer2_chain_drop_data(hammer2_chain_t *chain)
{
	hammer2_io_t *dio;

	if ((dio = chain->dio) != NULL) {
		chain->dio = NULL;
		chain->data = NULL;
	} else {
		switch(chain->bref.type) {
		case HAMMER2_BREF_TYPE_VOLUME:
		case HAMMER2_BREF_TYPE_FREEMAP:
			break;
		default:
			if (chain->data != NULL) {
				hammer2_spin_unex(&chain->core.spin);
				panic("chain data not null: "
				      "chain %p bref %016jx.%02x "
				      "refs %d parent %p dio %p data %p",
				      chain, chain->bref.data_off,
				      chain->bref.type, chain->refs,
				      chain->parent,
				      chain->dio, chain->data);
			}
			KKASSERT(chain->data == NULL);
			break;
		}
	}
	return dio;
}

int
hammer2_chain_lock(hammer2_chain_t *chain, int how)
{
	KKASSERT(chain->refs > 0);

	if (how & HAMMER2_RESOLVE_NONBLOCK) {
		atomic_add_int(&chain->lockcnt, 1);
		if (how & HAMMER2_RESOLVE_SHARED) {
			if (how & HAMMER2_RESOLVE_LOCKAGAIN) {
				hammer2_mtx_sh_again(&chain->lock);
			} else {
				if (hammer2_mtx_sh_try(&chain->lock) != 0) {
					hammer2_chain_unhold(chain);
					return EAGAIN;
				}
			}
		} else {
			if (hammer2_mtx_ex_try(&chain->lock) != 0) {
				hammer2_chain_unhold(chain);
				return EAGAIN;
			}
		}
	} else {
		atomic_add_int(&chain->lockcnt, 1);
		if (how & HAMMER2_RESOLVE_SHARED) {
			if (how & HAMMER2_RESOLVE_LOCKAGAIN) {
				hammer2_mtx_sh_again(&chain->lock);
			} else {
				hammer2_mtx_sh(&chain->lock);
			}
		} else {
			hammer2_mtx_ex(&chain->lock);
		}
	}

	if (chain->data) {
		if (chain->dio)
			hammer2_io_bkvasync(chain->dio);
		return 0;
	}

	switch(how & HAMMER2_RESOLVE_MASK) {
	case HAMMER2_RESOLVE_NEVER:
		return 0;
	case HAMMER2_RESOLVE_MAYBE:
		if (chain->flags & HAMMER2_CHAIN_INITIAL)
			return 0;
		if (chain->bref.type == HAMMER2_BREF_TYPE_DATA)
			return 0;
#if 0
		if (chain->bref.type == HAMMER2_BREF_TYPE_FREEMAP_NODE)
			return 0;
		if (chain->bref.type == HAMMER2_BREF_TYPE_FREEMAP_LEAF)
			return 0;
#endif
	case HAMMER2_RESOLVE_ALWAYS:
	default:
		break;
	}

	hammer2_chain_load_data(chain);

	return 0;
}

#if 0
void
hammer2_chain_lock_unhold(hammer2_chain_t *chain, int how)
{
	hammer2_chain_lock(chain, how);
	atomic_add_int(&chain->lockcnt, -1);
}

void
hammer2_chain_lock_downgrade(hammer2_chain_t *chain)
{
	hammer2_mtx_downgrade(&chain->lock);
}
#endif

void
hammer2_chain_load_data(hammer2_chain_t *chain)
{
	hammer2_blockref_t *bref;
	hammer2_dev_t *hmp;
	hammer2_io_t *dio;
	char *bdata;
	int error;

	KKASSERT(chain->lock.mtx_lock & MTX_MASK);
	if (chain->data) {
		if (chain->dio)
			hammer2_io_bkvasync(chain->dio);
		return;
	}
	if ((chain->bref.data_off & ~HAMMER2_OFF_MASK_RADIX) == 0)
		return;

	hmp = chain->hmp;
	KKASSERT(hmp != NULL);

	for (;;) {
		u_int oflags;
		u_int nflags;

		oflags = chain->flags;
		cpu_ccfence();
		if (oflags & HAMMER2_CHAIN_IOINPROG) {
			nflags = oflags | HAMMER2_CHAIN_IOSIGNAL;
			tsleep_interlock(&chain->flags, 0);
			if (atomic_cmpset_int(&chain->flags, oflags, nflags)) {
				tsleep(&chain->flags, PINTERLOCKED,
					"h2iocw", 0);
			}
		} else {
			nflags = oflags | HAMMER2_CHAIN_IOINPROG;
			if (atomic_cmpset_int(&chain->flags, oflags, nflags)) {
				break;
			}
		}
	}

	if (chain->data) {
		if (chain->dio)
			hammer2_io_bkvasync(chain->dio);
		goto done;
	}

	bref = &chain->bref;

	if (chain->flags & HAMMER2_CHAIN_INITIAL) {
		error = hammer2_io_new(hmp, bref->type,
				       bref->data_off, chain->bytes,
				       &chain->dio);
	} else {
		error = hammer2_io_bread(hmp, bref->type,
					 bref->data_off, chain->bytes,
					 &chain->dio);
		hammer2_adjreadcounter(chain->bref.type, chain->bytes);
	}
	if (error) {
		chain->error = HAMMER2_ERROR_EIO;
		kprintf("hammer2_chain_load_data: I/O error %016jx: %d\n",
			(intmax_t)bref->data_off, error);
		hammer2_io_bqrelse(&chain->dio);
		goto done;
	}
	chain->error = 0;

	if ((dio = chain->dio) != NULL && dio->bp) {
		struct buf *bp = dio->bp;

		if (dio->psize == chain->bytes &&
		    (bp->b_flags & B_IOISSUED)) {
			atomic_clear_int(&chain->flags,
					 HAMMER2_CHAIN_TESTEDGOOD);
			bp->b_flags &= ~B_IOISSUED;
		}
	}

	bdata = hammer2_io_data(chain->dio, chain->bref.data_off);

	if (chain->flags & HAMMER2_CHAIN_INITIAL) {
		atomic_clear_int(&chain->flags, HAMMER2_CHAIN_INITIAL);
		atomic_set_int(&chain->flags, HAMMER2_CHAIN_NOTTESTED);
	} else if (chain->flags & HAMMER2_CHAIN_MODIFIED) {
	} else if ((chain->flags & HAMMER2_CHAIN_TESTEDGOOD) == 0) {
		if (hammer2_chain_testcheck(chain, bdata) == 0) {
			chain->error = HAMMER2_ERROR_CHECK;
		} else {
			atomic_set_int(&chain->flags, HAMMER2_CHAIN_TESTEDGOOD);
		}
	}

	switch (bref->type) {
	case HAMMER2_BREF_TYPE_VOLUME:
	case HAMMER2_BREF_TYPE_FREEMAP:
		panic("hammer2_chain_load_data: unresolved volume header");
		break;
	case HAMMER2_BREF_TYPE_DIRENT:
		KKASSERT(chain->bytes != 0);
	case HAMMER2_BREF_TYPE_INODE:
	case HAMMER2_BREF_TYPE_FREEMAP_LEAF:
	case HAMMER2_BREF_TYPE_INDIRECT:
	case HAMMER2_BREF_TYPE_DATA:
	case HAMMER2_BREF_TYPE_FREEMAP_NODE:
	default:
		chain->data = (void *)bdata;
		break;
	}

done:
	for (;;) {
		u_int oflags;
		u_int nflags;

		oflags = chain->flags;
		nflags = oflags & ~(HAMMER2_CHAIN_IOINPROG |
				    HAMMER2_CHAIN_IOSIGNAL);
		KKASSERT(oflags & HAMMER2_CHAIN_IOINPROG);
		if (atomic_cmpset_int(&chain->flags, oflags, nflags)) {
			if (oflags & HAMMER2_CHAIN_IOSIGNAL)
				wakeup(&chain->flags);
			break;
		}
	}
}

void
hammer2_chain_unlock(hammer2_chain_t *chain)
{
	hammer2_io_t *dio;
	u_int lockcnt;
	int iter = 0;

	for (;;) {
		lockcnt = chain->lockcnt;
		KKASSERT(lockcnt > 0);
		cpu_ccfence();
		if (lockcnt > 1) {
			if (atomic_cmpset_int(&chain->lockcnt,
					      lockcnt, lockcnt - 1)) {
				hammer2_mtx_unlock(&chain->lock);
				return;
			}
		} else if (hammer2_mtx_upgrade_try(&chain->lock) == 0) {
			if (atomic_cmpset_int(&chain->lockcnt, 1, 0))
				break;
		} else {
			if (++iter > 1000) {
				if (iter > 1000 + hz) {
					kprintf("hammer2: h2race2 %p\n", chain);
					iter = 1000;
				}
				tsleep(&iter, 0, "h2race2", 1);
			}
			cpu_pause();
		}
	}

	dio = hammer2_chain_drop_data(chain);
	if (dio)
		hammer2_io_bqrelse(&dio);
	hammer2_mtx_unlock(&chain->lock);
}

#if 0
void
hammer2_chain_unlock_hold(hammer2_chain_t *chain)
{
	atomic_add_int(&chain->lockcnt, 1);
	hammer2_chain_unlock(chain);
}
#endif

static
hammer2_blockref_t *
hammer2_chain_base_and_count(hammer2_chain_t *parent, int *countp)
{
	hammer2_blockref_t *base;
	int count;

	if (parent->flags & HAMMER2_CHAIN_INITIAL) {
		base = NULL;

		switch(parent->bref.type) {
		case HAMMER2_BREF_TYPE_INODE:
			count = HAMMER2_SET_COUNT;
			break;
		case HAMMER2_BREF_TYPE_INDIRECT:
		case HAMMER2_BREF_TYPE_FREEMAP_NODE:
			count = parent->bytes / sizeof(hammer2_blockref_t);
			break;
		case HAMMER2_BREF_TYPE_VOLUME:
			count = HAMMER2_SET_COUNT;
			break;
		case HAMMER2_BREF_TYPE_FREEMAP:
			count = HAMMER2_SET_COUNT;
			break;
		default:
			panic("hammer2_chain_base_and_count: "
			      "unrecognized blockref type: %d",
			      parent->bref.type);
			count = 0;
			break;
		}
	} else {
		switch(parent->bref.type) {
		case HAMMER2_BREF_TYPE_INODE:
			base = &parent->data->ipdata.u.blockset.blockref[0];
			count = HAMMER2_SET_COUNT;
			break;
		case HAMMER2_BREF_TYPE_INDIRECT:
		case HAMMER2_BREF_TYPE_FREEMAP_NODE:
			base = &parent->data->npdata[0];
			count = parent->bytes / sizeof(hammer2_blockref_t);
			break;
		case HAMMER2_BREF_TYPE_VOLUME:
			base = &parent->data->voldata.
					sroot_blockset.blockref[0];
			count = HAMMER2_SET_COUNT;
			break;
		case HAMMER2_BREF_TYPE_FREEMAP:
			base = &parent->data->blkset.blockref[0];
			count = HAMMER2_SET_COUNT;
			break;
		default:
			panic("hammer2_chain_base_and_count: "
			      "unrecognized blockref type: %d",
			      parent->bref.type);
			base = NULL;
			count = 0;
			break;
		}
	}
	*countp = count;

	return base;
}

void
hammer2_chain_countbrefs(hammer2_chain_t *chain,
			 hammer2_blockref_t *base, int count)
{
	hammer2_spin_ex(&chain->core.spin);
	if ((chain->flags & HAMMER2_CHAIN_COUNTEDBREFS) == 0) {
		if (base) {
			while (--count >= 0) {
				if (base[count].type != HAMMER2_BREF_TYPE_EMPTY)
					break;
			}
			chain->core.live_zero = count + 1;
			while (count >= 0) {
				if (base[count].type != HAMMER2_BREF_TYPE_EMPTY)
					atomic_add_int(&chain->core.live_count,
						       1);
				--count;
			}
		} else {
			chain->core.live_zero = 0;
		}
		atomic_set_int(&chain->flags, HAMMER2_CHAIN_COUNTEDBREFS);
	}
	hammer2_spin_unex(&chain->core.spin);
}

int
hammer2_chain_resize(hammer2_chain_t *chain,
		     hammer2_tid_t mtid, hammer2_off_t dedup_off,
		     int nradix, int flags)
{
	hammer2_dev_t *hmp;
	size_t obytes;
	size_t nbytes;
	int error;

	hmp = chain->hmp;

	KKASSERT(chain != &hmp->vchain);
	KKASSERT(chain->bref.type == HAMMER2_BREF_TYPE_DATA ||
		 chain->bref.type == HAMMER2_BREF_TYPE_INDIRECT ||
		 chain->bref.type == HAMMER2_BREF_TYPE_DIRENT);

	obytes = chain->bytes;
	nbytes = (nradix) ? (1U << nradix) : 0;
	if (obytes == nbytes)
		return (chain->error);

	error = hammer2_chain_modify(chain, mtid, dedup_off, 0);
	if (error)
		return error;

	error = hammer2_freemap_alloc(chain, nbytes);
	if (error)
		return error;

	chain->bytes = nbytes;

	if (chain->dio) {
		KKASSERT(chain->bref.type == HAMMER2_BREF_TYPE_DATA ||
			 chain->bref.type == HAMMER2_BREF_TYPE_DIRENT);
		hammer2_io_brelse(&chain->dio);
		chain->data = NULL;
	}
	return (chain->error);
}

int
hammer2_chain_modify(hammer2_chain_t *chain, hammer2_tid_t mtid,
		     hammer2_off_t dedup_off, int flags)
{
	hammer2_dev_t *hmp;
	hammer2_io_t *dio;
	int error;
	int wasinitial;
	int setmodified;
	int setupdate;
	int newmod;
	char *bdata;

	hmp = chain->hmp;
	KKASSERT(chain->lock.mtx_lock & MTX_EXCLUSIVE);

	if (chain->bref.type == HAMMER2_BREF_TYPE_FREEMAP_NODE ||
	    chain->bref.type == HAMMER2_BREF_TYPE_FREEMAP_LEAF) {
		KKASSERT((chain->flags & HAMMER2_CHAIN_INITIAL) ||
			 (flags & HAMMER2_MODIFY_OPTDATA) == 0);
	}

	if (chain->data == NULL && chain->bytes != 0 &&
	    (flags & HAMMER2_MODIFY_OPTDATA) == 0 &&
	    (chain->bref.data_off & ~HAMMER2_OFF_MASK_RADIX)) {
		hammer2_chain_load_data(chain);
		if (chain->error)
			return (chain->error);
	}
	error = 0;

	if ((chain->flags & HAMMER2_CHAIN_MODIFIED) == 0) {
		atomic_add_long(&hammer2_count_modified_chains, 1);
		atomic_set_int(&chain->flags, HAMMER2_CHAIN_MODIFIED);
		hammer2_pfs_memory_inc(chain->pmp);
		setmodified = 1;

		if ((chain->bref.type == HAMMER2_BREF_TYPE_DATA ||
		     chain->bref.type == HAMMER2_BREF_TYPE_DIRENT) &&
		    (chain->flags & HAMMER2_CHAIN_INITIAL) == 0 &&
		    (chain->flags & HAMMER2_CHAIN_DEDUPABLE) == 0 &&
		    HAMMER2_DEC_CHECK(chain->bref.methods) ==
		     HAMMER2_CHECK_NONE &&
		    chain->pmp &&
		    chain->bref.modify_tid >
		     chain->pmp->iroot->meta.pfs_lsnap_tid) {
			newmod = 0;
		} else if ((hmp->hflags & HMNT2_EMERG) &&
			   chain->pmp &&
			   chain->bref.modify_tid >
			    chain->pmp->iroot->meta.pfs_lsnap_tid) {
			newmod = 0;
		} else {
			newmod = 1;
		}
	} else if (chain->flags & HAMMER2_CHAIN_DEDUPABLE) {
		newmod = 1;
		setmodified = 0;
	} else {
		newmod = 0;
		setmodified = 0;
	}

	if ((chain->flags & HAMMER2_CHAIN_UPDATE) == 0) {
		atomic_set_int(&chain->flags, HAMMER2_CHAIN_UPDATE);
		setupdate = 1;
	} else {
		setupdate = 0;
	}

	lockmgr(&chain->diolk, LK_EXCLUSIVE);

	if (chain != &hmp->vchain && chain != &hmp->fchain &&
	    chain->bytes) {
		if ((chain->bref.data_off & ~HAMMER2_OFF_MASK_RADIX) == 0 ||
		    newmod
		) {
			if (dedup_off) {
				chain->bref.data_off = dedup_off;
				if ((int)(dedup_off & HAMMER2_OFF_MASK_RADIX))
					chain->bytes = 1 <<
						(int)(dedup_off &
						HAMMER2_OFF_MASK_RADIX);
				else
					chain->bytes = 0;
				chain->error = 0;
				atomic_clear_int(&chain->flags,
						 HAMMER2_CHAIN_MODIFIED);
				atomic_add_long(&hammer2_count_modified_chains,
						-1);
				if (chain->pmp) {
					hammer2_pfs_memory_wakeup(
						chain->pmp, -1);
				}
				hammer2_freemap_adjust(hmp, &chain->bref,
						HAMMER2_FREEMAP_DORECOVER);
				atomic_set_int(&chain->flags,
						HAMMER2_CHAIN_DEDUPABLE);
			} else {
				error = hammer2_freemap_alloc(chain,
							      chain->bytes);
				atomic_clear_int(&chain->flags,
						HAMMER2_CHAIN_DEDUPABLE);

				if (error && (hmp->hflags & HMNT2_EMERG)) {
					error = 0;
					chain->bref.flags |=
						HAMMER2_BREF_FLAG_EMERG_MIP;

					krateprintf(&krate_h2em,
					    "hammer2: Emergency Mode WARNING: "
					    "Operation will likely corrupt "
					    "related snapshot: "
					    "%016jx.%02x key=%016jx\n",
					    chain->bref.data_off,
					    chain->bref.type,
					    chain->bref.key);
				} else if (error == 0) {
					chain->bref.flags &=
						~HAMMER2_BREF_FLAG_EMERG_MIP;
				}
			}
		}
	}

	if (error) {
		if (setmodified) {
			atomic_clear_int(&chain->flags, HAMMER2_CHAIN_MODIFIED);
			atomic_add_long(&hammer2_count_modified_chains, -1);
			if (chain->pmp)
				hammer2_pfs_memory_wakeup(chain->pmp, -1);
		}
		if (setupdate) {
			atomic_clear_int(&chain->flags, HAMMER2_CHAIN_UPDATE);
		}
		lockmgr(&chain->diolk, LK_RELEASE);

		return error;
	}

	chain->bref.mirror_tid = hmp->voldata.mirror_tid + 1;
	if (mtid)
		chain->bref.modify_tid = mtid;

	if (chain->flags & HAMMER2_CHAIN_BLKMAPPED)
		atomic_set_int(&chain->flags, HAMMER2_CHAIN_BLKMAPUPD);

	if (chain->bref.type == HAMMER2_BREF_TYPE_DATA &&
	    (flags & HAMMER2_MODIFY_OPTDATA) &&
	    chain->data == NULL) {
		if (dedup_off == 0) {
			KKASSERT(chain->dio == NULL);
			goto skip2;
		}
	}

	if (chain->flags & HAMMER2_CHAIN_INITIAL) {
		atomic_clear_int(&chain->flags, HAMMER2_CHAIN_INITIAL);
		wasinitial = 1;
	} else {
		wasinitial = 0;
	}

	switch(chain->bref.type) {
	case HAMMER2_BREF_TYPE_VOLUME:
	case HAMMER2_BREF_TYPE_FREEMAP:
		KKASSERT(chain->dio == NULL);
		break;
	case HAMMER2_BREF_TYPE_DIRENT:
		if (chain->bytes == 0) {
			KKASSERT(chain->dio == NULL);
			break;
		}
	case HAMMER2_BREF_TYPE_INODE:
	case HAMMER2_BREF_TYPE_FREEMAP_LEAF:
	case HAMMER2_BREF_TYPE_DATA:
	case HAMMER2_BREF_TYPE_INDIRECT:
	case HAMMER2_BREF_TYPE_FREEMAP_NODE:
		KKASSERT(chain != &hmp->vchain && chain != &hmp->fchain);

		if (wasinitial && dedup_off == 0) {
			error = hammer2_io_new(hmp, chain->bref.type,
					       chain->bref.data_off,
					       chain->bytes, &dio);
		} else {
			error = hammer2_io_bread(hmp, chain->bref.type,
						 chain->bref.data_off,
						 chain->bytes, &dio);
		}
		hammer2_adjreadcounter(chain->bref.type, chain->bytes);

		if (error) {
			kprintf("hammer2_chain_modify: hmp=%p I/O error\n",
				hmp);
			chain->error = HAMMER2_ERROR_EIO;
			hammer2_io_brelse(&dio);
			hammer2_io_brelse(&chain->dio);
			chain->data = NULL;
			break;
		}
		chain->error = 0;
		bdata = hammer2_io_data(dio, chain->bref.data_off);

		if (chain->data) {
			KKASSERT(chain->dio != NULL);
			if (chain->data != (void *)bdata && dedup_off == 0) {
				bcopy(chain->data, bdata, chain->bytes);
			}
		} else if (wasinitial == 0 && dedup_off == 0) {
			panic("hammer2_chain_modify: having a COW %p\n",
			      chain);
		}

		{
			hammer2_io_t *tio;

			if ((tio = chain->dio) != NULL)
				hammer2_io_bqrelse(&tio);
			chain->data = (void *)bdata;
			chain->dio = dio;
			if (dedup_off == 0)
				hammer2_io_setdirty(dio);
		}
		break;
	default:
		panic("hammer2_chain_modify: illegal non-embedded type %d",
		      chain->bref.type);
		break;

	}
skip2:
	if (chain->parent)
		hammer2_chain_setflush(chain->parent);
	lockmgr(&chain->diolk, LK_RELEASE);

	return (chain->error);
}

int
hammer2_chain_modify_ip(hammer2_inode_t *ip, hammer2_chain_t *chain,
			hammer2_tid_t mtid, int flags)
{
	int error;

	hammer2_inode_modify(ip);
	error = hammer2_chain_modify(chain, mtid, 0, flags);

	return error;
}

struct hammer2_chain_find_info {
	hammer2_chain_t		*best;
	hammer2_key_t		key_beg;
	hammer2_key_t		key_end;
	hammer2_key_t		key_next;
};

static int hammer2_chain_find_cmp(hammer2_chain_t *child, void *data);
static int hammer2_chain_find_callback(hammer2_chain_t *child, void *data);

static
hammer2_chain_t *
hammer2_chain_find(hammer2_chain_t *parent, hammer2_key_t *key_nextp,
			  hammer2_key_t key_beg, hammer2_key_t key_end)
{
	struct hammer2_chain_find_info info;

	info.best = NULL;
	info.key_beg = key_beg;
	info.key_end = key_end;
	info.key_next = *key_nextp;

	RB_SCAN(hammer2_chain_tree, &parent->core.rbtree,
		hammer2_chain_find_cmp, hammer2_chain_find_callback,
		&info);
	*key_nextp = info.key_next;
#if 0
	kprintf("chain_find %p %016jx:%016jx next=%016jx\n",
		parent, key_beg, key_end, *key_nextp);
#endif

	return (info.best);
}

static
int
hammer2_chain_find_cmp(hammer2_chain_t *child, void *data)
{
	struct hammer2_chain_find_info *info = data;
	hammer2_key_t child_beg;
	hammer2_key_t child_end;

	child_beg = child->bref.key;
	child_end = child_beg + ((hammer2_key_t)1 << child->bref.keybits) - 1;

	if (child_end < info->key_beg)
		return(-1);
	if (child_beg > info->key_end)
		return(1);
	return(0);
}

static
int
hammer2_chain_find_callback(hammer2_chain_t *child, void *data)
{
	struct hammer2_chain_find_info *info = data;
	hammer2_chain_t *best;
	hammer2_key_t child_end;

	if ((best = info->best) == NULL) {
		info->best = child;
	} else if (best->bref.key <= info->key_beg &&
		   child->bref.key <= info->key_beg) {
		KKASSERT(0);
	} else if (child->bref.key < best->bref.key) {
		info->best = child;
		if (info->key_next > best->bref.key || info->key_next == 0)
			info->key_next = best->bref.key;
	} else if (child->bref.key == best->bref.key) {
		KKASSERT(0);
		info->best = child;
	} else {
		if (info->key_next > child->bref.key || info->key_next == 0)
			info->key_next = child->bref.key;
	}

	child_end = child->bref.key + ((hammer2_key_t)1 << child->bref.keybits);
	if (child_end && (info->key_next > child_end || info->key_next == 0))
		info->key_next = child_end;

	return(0);
}

hammer2_chain_t *
hammer2_chain_get(hammer2_chain_t *parent, int generation,
		  hammer2_blockref_t *bref, int how)
{
	hammer2_dev_t *hmp = parent->hmp;
	hammer2_chain_t *chain;
	int error;

	if (bref->flags & HAMMER2_BREF_FLAG_PFSROOT)
		chain = hammer2_chain_alloc(hmp, NULL, bref);
	else
		chain = hammer2_chain_alloc(hmp, parent->pmp, bref);

	atomic_set_int(&chain->flags, HAMMER2_CHAIN_BLKMAPPED);

	hammer2_chain_lock(chain, how);

	KKASSERT(parent->refs > 0);
	error = hammer2_chain_insert(parent, chain,
				     HAMMER2_CHAIN_INSERT_SPIN |
				     HAMMER2_CHAIN_INSERT_RACE,
				     generation);
	if (error) {
		KKASSERT((chain->flags & HAMMER2_CHAIN_ONRBTREE) == 0);
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
		chain = NULL;
	} else {
		KKASSERT(chain->flags & HAMMER2_CHAIN_ONRBTREE);
	}

	return (chain);
}

hammer2_chain_t *
hammer2_chain_lookup_init(hammer2_chain_t *parent, int flags)
{
	hammer2_chain_ref(parent);
	if (flags & HAMMER2_LOOKUP_SHARED) {
		hammer2_chain_lock(parent, HAMMER2_RESOLVE_ALWAYS |
					   HAMMER2_RESOLVE_SHARED);
	} else {
		hammer2_chain_lock(parent, HAMMER2_RESOLVE_ALWAYS);
	}
	return (parent);
}

void
hammer2_chain_lookup_done(hammer2_chain_t *parent)
{
	if (parent) {
		hammer2_chain_unlock(parent);
		hammer2_chain_drop(parent);
	}
}

hammer2_chain_t *
hammer2_chain_getparent(hammer2_chain_t *chain, int flags)
{
	hammer2_chain_t *parent;

	parent = chain->parent;
	if (parent == NULL)
		panic("hammer2_chain_getparent: no parent");
	hammer2_chain_ref(parent);
	if (hammer2_chain_lock(parent, flags|HAMMER2_RESOLVE_NONBLOCK) == 0)
		return parent;

	for (;;) {
		hammer2_chain_unlock(chain);
		hammer2_chain_lock(parent, flags);
		hammer2_chain_lock(chain, flags);

		if (chain->parent == parent)
			break;
		hammer2_chain_unlock(parent);
		hammer2_chain_drop(parent);
		cpu_ccfence();
		parent = chain->parent;
		if (parent == NULL)
			panic("hammer2_chain_getparent: no parent");
		hammer2_chain_ref(parent);
	}
	return parent;
}

hammer2_chain_t *
hammer2_chain_repparent(hammer2_chain_t **chainp, int flags)
{
	hammer2_chain_t *chain;
	hammer2_chain_t *parent;
	struct hammer2_reptrack reptrack;
	struct hammer2_reptrack **repp;

	chain = *chainp;
	parent = chain->parent;
	if (parent == NULL) {
		hammer2_spin_unex(&chain->core.spin);
		panic("hammer2_chain_repparent: no parent");
	}
	hammer2_chain_ref(parent);
	if (hammer2_chain_lock(parent, flags|HAMMER2_RESOLVE_NONBLOCK) == 0) {
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
		*chainp = parent;

		return parent;
	}

	hammer2_spin_init(&reptrack.spin, "h2reptrk");
	reptrack.chain = parent;
	hammer2_chain_ref(parent);

	hammer2_spin_ex(&parent->core.spin);
	reptrack.next = parent->core.reptrack;
	parent->core.reptrack = &reptrack;
	hammer2_spin_unex(&parent->core.spin);

	hammer2_chain_unlock(chain);
	hammer2_chain_drop(chain);
	chain = NULL;

	for (;;) {
		hammer2_chain_lock(parent, flags);

		if (reptrack.chain == parent)
			break;
		hammer2_chain_unlock(parent);
		hammer2_chain_drop(parent);

		kprintf("hammer2: debug REPTRACK %p->%p\n",
			parent, reptrack.chain);
		hammer2_spin_ex(&reptrack.spin);
		parent = reptrack.chain;
		hammer2_chain_ref(parent);
		hammer2_spin_unex(&reptrack.spin);
	}

	hammer2_spin_ex(&parent->core.spin);
	repp = &parent->core.reptrack;
	while (*repp != &reptrack)
		repp = &(*repp)->next;
	*repp = reptrack.next;
	hammer2_spin_unex(&parent->core.spin);

	hammer2_chain_drop(parent);
	*chainp = parent;

	return parent;
}

static
void
hammer2_chain_repchange(hammer2_chain_t *parent, hammer2_chain_t *chain)
{
	struct hammer2_reptrack *reptrack;

	KKASSERT(chain->core.live_count == 0 && RB_EMPTY(&chain->core.rbtree));
	while (chain->core.reptrack) {
		hammer2_spin_ex(&parent->core.spin);
		hammer2_spin_ex(&chain->core.spin);
		reptrack = chain->core.reptrack;
		if (reptrack == NULL) {
			hammer2_spin_unex(&chain->core.spin);
			hammer2_spin_unex(&parent->core.spin);
			break;
		}
		hammer2_spin_ex(&reptrack->spin);
		chain->core.reptrack = reptrack->next;
		reptrack->chain = parent;
		reptrack->next = parent->core.reptrack;
		parent->core.reptrack = reptrack;
		hammer2_chain_ref(parent);

		hammer2_spin_unex(&chain->core.spin);
		hammer2_spin_unex(&parent->core.spin);
		kprintf("hammer2: debug repchange %p %p->%p\n",
			reptrack, chain, parent);
		hammer2_chain_drop(chain);
	}
}

hammer2_chain_t *
hammer2_chain_lookup(hammer2_chain_t **parentp, hammer2_key_t *key_nextp,
		     hammer2_key_t key_beg, hammer2_key_t key_end,
		     int *errorp, int flags)
{
	hammer2_chain_t *parent;
	hammer2_chain_t *chain;
	hammer2_blockref_t *base;
	hammer2_blockref_t *bref;
	hammer2_blockref_t bsave;
	hammer2_key_t scan_beg;
	hammer2_key_t scan_end;
	int count = 0;
	int how_always = HAMMER2_RESOLVE_ALWAYS;
	int how_maybe = HAMMER2_RESOLVE_MAYBE;
	int how;
	int generation;
	int maxloops = 300000;

	if (flags & HAMMER2_LOOKUP_ALWAYS) {
		how_maybe = how_always;
		how = HAMMER2_RESOLVE_ALWAYS;
	} else if (flags & HAMMER2_LOOKUP_NODATA) {
		how = HAMMER2_RESOLVE_NEVER;
	} else {
		how = HAMMER2_RESOLVE_MAYBE;
	}
	if (flags & HAMMER2_LOOKUP_SHARED) {
		how_maybe |= HAMMER2_RESOLVE_SHARED;
		how_always |= HAMMER2_RESOLVE_SHARED;
		how |= HAMMER2_RESOLVE_SHARED;
	}

	parent = *parentp;
	*errorp = 0;

	while (parent->bref.type == HAMMER2_BREF_TYPE_INDIRECT ||
	       parent->bref.type == HAMMER2_BREF_TYPE_FREEMAP_NODE) {
		scan_beg = parent->bref.key;
		scan_end = scan_beg +
			   ((hammer2_key_t)1 << parent->bref.keybits) - 1;
		if ((parent->flags & HAMMER2_CHAIN_DELETED) == 0) {
			if (key_beg >= scan_beg && key_end <= scan_end)
				break;
		}
		parent = hammer2_chain_repparent(parentp, how_maybe);
	}
again:
	if (--maxloops == 0)
		panic("hammer2_chain_lookup: maxloops");

	switch(parent->bref.type) {
	case HAMMER2_BREF_TYPE_FREEMAP_NODE:
	case HAMMER2_BREF_TYPE_INDIRECT:
		if (flags & HAMMER2_LOOKUP_MATCHIND) {
			scan_beg = parent->bref.key;
			scan_end = scan_beg +
			       ((hammer2_key_t)1 << parent->bref.keybits) - 1;
			if (key_beg == scan_beg && key_end == scan_end) {
				chain = parent;
				hammer2_chain_ref(chain);
				hammer2_chain_lock(chain, how_maybe);
				*key_nextp = scan_end + 1;
				goto done;
			}
		}
		break;
	default:
		break;
	}

	if (parent->error) {
		*errorp = parent->error;
		return NULL;
	}

	switch(parent->bref.type) {
	case HAMMER2_BREF_TYPE_INODE:
		if (parent->data->ipdata.meta.op_flags &
		    HAMMER2_OPFLAG_DIRECTDATA) {
			if (flags & HAMMER2_LOOKUP_NODIRECT) {
				chain = NULL;
				*key_nextp = key_end + 1;
				goto done;
			}
			hammer2_chain_ref(parent);
			hammer2_chain_lock(parent, how_always |
						   HAMMER2_RESOLVE_LOCKAGAIN);
			*key_nextp = key_end + 1;
			return (parent);
		}
		base = &parent->data->ipdata.u.blockset.blockref[0];
		count = HAMMER2_SET_COUNT;
		break;
	case HAMMER2_BREF_TYPE_FREEMAP_NODE:
	case HAMMER2_BREF_TYPE_INDIRECT:
		if (parent->flags & HAMMER2_CHAIN_INITIAL) {
			base = NULL;
		} else {
			if (parent->data == NULL) {
				kprintf("hammer2: unexpected NULL data "
					"on %p\n", parent);
				while (1)
					tsleep(parent, 0, "xxx", 0);
			}
			base = &parent->data->npdata[0];
		}
		count = parent->bytes / sizeof(hammer2_blockref_t);
		break;
	case HAMMER2_BREF_TYPE_VOLUME:
		base = &parent->data->voldata.sroot_blockset.blockref[0];
		count = HAMMER2_SET_COUNT;
		break;
	case HAMMER2_BREF_TYPE_FREEMAP:
		base = &parent->data->blkset.blockref[0];
		count = HAMMER2_SET_COUNT;
		break;
	default:
		panic("hammer2_chain_lookup: unrecognized "
		      "blockref(B) type: %d",
		      parent->bref.type);
		base = NULL;
		count = 0;
		break;
	}

	if ((parent->flags & HAMMER2_CHAIN_COUNTEDBREFS) == 0)
		hammer2_chain_countbrefs(parent, base, count);

	hammer2_spin_ex(&parent->core.spin);
	chain = hammer2_combined_find(parent, base, count,
				      key_nextp,
				      key_beg, key_end,
				      &bref);
	generation = parent->core.generation;

	if (bref == NULL) {
		KKASSERT(chain == NULL);
		hammer2_spin_unex(&parent->core.spin);
		if (key_beg == key_end)
			return (NULL);

		if (parent->bref.type != HAMMER2_BREF_TYPE_INDIRECT &&
		    parent->bref.type != HAMMER2_BREF_TYPE_FREEMAP_NODE) {
			return (NULL);
		}

		key_beg = parent->bref.key +
			  ((hammer2_key_t)1 << parent->bref.keybits);
		if (key_beg == 0 || key_beg > key_end)
			return (NULL);
		parent = hammer2_chain_repparent(parentp, how_maybe);
		goto again;
	}

	bsave = *bref;
	if (chain == NULL) {
		hammer2_spin_unex(&parent->core.spin);
		if (bsave.type == HAMMER2_BREF_TYPE_INDIRECT ||
		    bsave.type == HAMMER2_BREF_TYPE_FREEMAP_NODE) {
			chain = hammer2_chain_get(parent, generation,
						  &bsave, how_maybe);
		} else {
			chain = hammer2_chain_get(parent, generation,
						  &bsave, how);
		}
		if (chain == NULL)
			goto again;
	} else {
		hammer2_chain_ref(chain);
		hammer2_spin_unex(&parent->core.spin);

		if (bsave.type == HAMMER2_BREF_TYPE_INDIRECT ||
		    bsave.type == HAMMER2_BREF_TYPE_FREEMAP_NODE) {
			hammer2_chain_lock(chain, how_maybe);
		} else {
			hammer2_chain_lock(chain, how);
		}
		KKASSERT(chain->parent == parent);
	}
	if (bcmp(&bsave, &chain->bref, sizeof(bsave)) ||
	    chain->parent != parent) {
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
		chain = NULL;
		goto again;
	}

	if (chain->flags & HAMMER2_CHAIN_DELETED) {
		kprintf("skip deleted chain %016jx.%02x key=%016jx\n",
			chain->bref.data_off, chain->bref.type,
			chain->bref.key);
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
		chain = NULL;
		key_beg = *key_nextp;
		if (key_beg == 0 || key_beg > key_end)
			return(NULL);
		goto again;
	}

	if (chain->bref.type == HAMMER2_BREF_TYPE_INDIRECT ||
	    chain->bref.type == HAMMER2_BREF_TYPE_FREEMAP_NODE) {
		hammer2_chain_unlock(parent);
		hammer2_chain_drop(parent);
		*parentp = parent = chain;
		chain = NULL;
		goto again;
	}
done:
	return (chain);
}

hammer2_chain_t *
hammer2_chain_next(hammer2_chain_t **parentp, hammer2_chain_t *chain,
		   hammer2_key_t *key_nextp,
		   hammer2_key_t key_beg, hammer2_key_t key_end,
		   int *errorp, int flags)
{
	hammer2_chain_t *parent;
	int how_maybe;

	how_maybe = HAMMER2_RESOLVE_MAYBE;
	if (flags & HAMMER2_LOOKUP_SHARED)
		how_maybe |= HAMMER2_RESOLVE_SHARED;

	parent = *parentp;
	*errorp = 0;

	if (chain) {
		key_beg = chain->bref.key +
			  ((hammer2_key_t)1 << chain->bref.keybits);
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);

		if (chain == parent)
			return(NULL);
		if (key_beg == 0 || key_beg > key_end)
			return(NULL);
		chain = NULL;
	} else if (parent->bref.type != HAMMER2_BREF_TYPE_INDIRECT &&
		   parent->bref.type != HAMMER2_BREF_TYPE_FREEMAP_NODE) {
		return (NULL);
	} else {
		key_beg = parent->bref.key +
			  ((hammer2_key_t)1 << parent->bref.keybits);
		if (key_beg == 0 || key_beg > key_end)
			return (NULL);
		parent = hammer2_chain_repparent(parentp, how_maybe);
	}

	return (hammer2_chain_lookup(parentp, key_nextp,
				     key_beg, key_end,
				     errorp, flags));
}

int
hammer2_chain_scan(hammer2_chain_t *parent, hammer2_chain_t **chainp,
		   hammer2_blockref_t *bref, int *firstp,
		   int flags)
{
	hammer2_blockref_t *base;
	hammer2_blockref_t *bref_ptr;
	hammer2_key_t key;
	hammer2_key_t next_key;
	hammer2_chain_t *chain = NULL;
	int count = 0;
	int how;
	int generation;
	int maxloops = 300000;
	int error;

	error = 0;

	if (flags & HAMMER2_LOOKUP_ALWAYS) {
		how = HAMMER2_RESOLVE_ALWAYS;
	} else if (flags & HAMMER2_LOOKUP_NODATA) {
		how = HAMMER2_RESOLVE_NEVER;
	} else {
		how = HAMMER2_RESOLVE_MAYBE;
	}
	if (flags & HAMMER2_LOOKUP_SHARED) {
		how |= HAMMER2_RESOLVE_SHARED;
	}

	if (*firstp) {
		key = 0;
		*firstp = 0;
	} else {
		key = bref->key + ((hammer2_key_t)1 << bref->keybits);
		if ((chain = *chainp) != NULL) {
			*chainp = NULL;
			hammer2_chain_unlock(chain);
			hammer2_chain_drop(chain);
			chain = NULL;
		}
		if (key == 0) {
			error |= HAMMER2_ERROR_EOF;
			goto done;
		}
	}

again:
	if (parent->error) {
		error = parent->error;
		goto done;
	}
	if (--maxloops == 0)
		panic("hammer2_chain_scan: maxloops");

	switch(parent->bref.type) {
	case HAMMER2_BREF_TYPE_INODE:
		if (parent->data->ipdata.meta.op_flags &
		    HAMMER2_OPFLAG_DIRECTDATA) {
			error |= HAMMER2_ERROR_EOF;
			goto done;
		}
		base = &parent->data->ipdata.u.blockset.blockref[0];
		count = HAMMER2_SET_COUNT;
		break;
	case HAMMER2_BREF_TYPE_FREEMAP_NODE:
	case HAMMER2_BREF_TYPE_INDIRECT:
		if (parent->flags & HAMMER2_CHAIN_INITIAL) {
			base = NULL;
		} else {
			if (parent->data == NULL)
				panic("parent->data is NULL");
			base = &parent->data->npdata[0];
		}
		count = parent->bytes / sizeof(hammer2_blockref_t);
		break;
	case HAMMER2_BREF_TYPE_VOLUME:
		base = &parent->data->voldata.sroot_blockset.blockref[0];
		count = HAMMER2_SET_COUNT;
		break;
	case HAMMER2_BREF_TYPE_FREEMAP:
		base = &parent->data->blkset.blockref[0];
		count = HAMMER2_SET_COUNT;
		break;
	default:
		panic("hammer2_chain_scan: unrecognized blockref type: %d",
		      parent->bref.type);
		base = NULL;
		count = 0;
		break;
	}

	if ((parent->flags & HAMMER2_CHAIN_COUNTEDBREFS) == 0)
		hammer2_chain_countbrefs(parent, base, count);

	next_key = 0;
	bref_ptr = NULL;
	hammer2_spin_ex(&parent->core.spin);
	chain = hammer2_combined_find(parent, base, count,
				      &next_key,
				      key, HAMMER2_KEY_MAX,
				      &bref_ptr);
	generation = parent->core.generation;

	if (bref_ptr == NULL) {
		hammer2_spin_unex(&parent->core.spin);
		KKASSERT(chain == NULL);
		error |= HAMMER2_ERROR_EOF;
		goto done;
	}

	*bref = *bref_ptr;

	if (chain == NULL) {
		switch(bref->type) {
		case HAMMER2_BREF_TYPE_INODE:
		case HAMMER2_BREF_TYPE_FREEMAP_NODE:
		case HAMMER2_BREF_TYPE_INDIRECT:
		case HAMMER2_BREF_TYPE_VOLUME:
		case HAMMER2_BREF_TYPE_FREEMAP:
			hammer2_spin_unex(&parent->core.spin);
			chain = hammer2_chain_get(parent, generation,
						  bref, how);
			if (chain == NULL)
				goto again;
			break;
		default:
			hammer2_spin_unex(&parent->core.spin);
			break;
		}
	} else {
		hammer2_chain_ref(chain);
		hammer2_spin_unex(&parent->core.spin);
		hammer2_chain_lock(chain, how);
	}
	if (chain &&
	    (bcmp(bref, &chain->bref, sizeof(*bref)) ||
	     chain->parent != parent)) {
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
		chain = NULL;
		goto again;
	}

	if (chain && (chain->flags & HAMMER2_CHAIN_DELETED)) {
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
		chain = NULL;

		key = next_key;
		if (key == 0) {
			error |= HAMMER2_ERROR_EOF;
			goto done;
		}
		goto again;
	}

done:
	if (chain)
		*chainp = chain;
	return (error);
}

int
hammer2_chain_create(hammer2_chain_t **parentp, hammer2_chain_t **chainp,
		     hammer2_dev_t *hmp, hammer2_pfs_t *pmp, int methods,
		     hammer2_key_t key, int keybits, int type, size_t bytes,
		     hammer2_tid_t mtid, hammer2_off_t dedup_off, int flags)
{
	hammer2_chain_t *chain;
	hammer2_chain_t *parent;
	hammer2_blockref_t *base;
	hammer2_blockref_t dummy;
	int allocated = 0;
	int error = 0;
	int count;
	int maxloops = 300000;

	parent = *parentp;
	if (parent) {
		KKASSERT(hammer2_mtx_owned(&parent->lock));
		KKASSERT(parent->error == 0);
		hmp = parent->hmp;
	}
	chain = *chainp;

	if (chain == NULL) {
		bzero(&dummy, sizeof(dummy));
		dummy.type = type;
		dummy.key = key;
		dummy.keybits = keybits;
		dummy.data_off = hammer2_getradix(bytes);

		if (methods == HAMMER2_METH_DEFAULT)
			dummy.methods = parent->bref.methods;
		else
			dummy.methods = (uint8_t)methods;

		if (type != HAMMER2_BREF_TYPE_DATA &&
		    HAMMER2_DEC_CHECK(dummy.methods) == HAMMER2_CHECK_NONE) {
			dummy.methods |=
				HAMMER2_ENC_CHECK(HAMMER2_CHECK_DEFAULT);
		}

		chain = hammer2_chain_alloc(hmp, pmp, &dummy);

		chain->lockcnt = 1;
		hammer2_mtx_ex(&chain->lock);
		allocated = 1;

		switch(type) {
		case HAMMER2_BREF_TYPE_VOLUME:
		case HAMMER2_BREF_TYPE_FREEMAP:
			panic("hammer2_chain_create: called with volume type");
			break;
		case HAMMER2_BREF_TYPE_INDIRECT:
			panic("hammer2_chain_create: cannot be used to"
			      "create indirect block");
			break;
		case HAMMER2_BREF_TYPE_FREEMAP_NODE:
			panic("hammer2_chain_create: cannot be used to"
			      "create freemap root or node");
			break;
		case HAMMER2_BREF_TYPE_FREEMAP_LEAF:
			KKASSERT(bytes == sizeof(chain->data->bmdata));
		case HAMMER2_BREF_TYPE_DIRENT:
		case HAMMER2_BREF_TYPE_INODE:
		case HAMMER2_BREF_TYPE_DATA:
		default:
			KKASSERT(chain->data == NULL);
			atomic_set_int(&chain->flags, HAMMER2_CHAIN_INITIAL);
			break;
		}
	} else {
		chain->bref.key = key;
		chain->bref.keybits = keybits;
		if (chain->flags & HAMMER2_CHAIN_DELETED)
			atomic_clear_int(&chain->flags, HAMMER2_CHAIN_DELETED);
		KKASSERT(chain->parent == NULL);
	}

	if (flags & HAMMER2_INSERT_PFSROOT)
		chain->bref.flags |= HAMMER2_BREF_FLAG_PFSROOT;

	if (parent == NULL)
		goto skip;

again:
	if (--maxloops == 0)
		panic("hammer2_chain_create: maxloops");

	switch(parent->bref.type) {
	case HAMMER2_BREF_TYPE_INODE:
		if ((parent->data->ipdata.meta.op_flags &
		     HAMMER2_OPFLAG_DIRECTDATA) != 0) {
			kprintf("hammer2: parent set for direct-data! "
				"pkey=%016jx ckey=%016jx\n",
				parent->bref.key,
				chain->bref.key);
	        }
		KKASSERT((parent->data->ipdata.meta.op_flags &
			  HAMMER2_OPFLAG_DIRECTDATA) == 0);
		KKASSERT(parent->data != NULL);
		base = &parent->data->ipdata.u.blockset.blockref[0];
		count = HAMMER2_SET_COUNT;
		break;
	case HAMMER2_BREF_TYPE_INDIRECT:
	case HAMMER2_BREF_TYPE_FREEMAP_NODE:
		if (parent->flags & HAMMER2_CHAIN_INITIAL)
			base = NULL;
		else
			base = &parent->data->npdata[0];
		count = parent->bytes / sizeof(hammer2_blockref_t);
		break;
	case HAMMER2_BREF_TYPE_VOLUME:
		KKASSERT(parent->data != NULL);
		base = &parent->data->voldata.sroot_blockset.blockref[0];
		count = HAMMER2_SET_COUNT;
		break;
	case HAMMER2_BREF_TYPE_FREEMAP:
		KKASSERT(parent->data != NULL);
		base = &parent->data->blkset.blockref[0];
		count = HAMMER2_SET_COUNT;
		break;
	default:
		panic("hammer2_chain_create: unrecognized blockref type: %d",
		      parent->bref.type);
		base = NULL;
		count = 0;
		break;
	}

	if ((parent->flags & HAMMER2_CHAIN_COUNTEDBREFS) == 0)
		hammer2_chain_countbrefs(parent, base, count);

	KASSERT(parent->core.live_count >= 0 &&
		parent->core.live_count <= count,
		("bad live_count %d/%d (%02x, %d)",
			parent->core.live_count, count,
			parent->bref.type, parent->bytes));

	if (parent->core.live_count == count) {
		hammer2_chain_t *nparent;

		KKASSERT((flags & HAMMER2_INSERT_SAMEPARENT) == 0);

		nparent = hammer2_chain_create_indirect(parent, key, keybits,
							mtid, type, &error);
		if (nparent == NULL) {
			if (allocated)
				hammer2_chain_drop(chain);
			chain = NULL;
			goto done;
		}
		if (parent != nparent) {
			hammer2_chain_unlock(parent);
			hammer2_chain_drop(parent);
			parent = *parentp = nparent;
		}
		goto again;
	}

skip:
	if (chain->flags & HAMMER2_CHAIN_DELETED)
		kprintf("Inserting deleted chain @%016jx\n",
			chain->bref.key);

	if (chain->parent != NULL)
		panic("hammer2: hammer2_chain_create: chain already connected");
	KKASSERT(chain->parent == NULL);
	if (parent) {
		KKASSERT(parent->core.live_count < count);
		hammer2_chain_insert(parent, chain,
				     HAMMER2_CHAIN_INSERT_SPIN |
				     HAMMER2_CHAIN_INSERT_LIVE,
				     0);
	}

	if (allocated) {
		switch(chain->bref.type) {
		case HAMMER2_BREF_TYPE_DATA:
		case HAMMER2_BREF_TYPE_FREEMAP_LEAF:
		case HAMMER2_BREF_TYPE_DIRENT:
		case HAMMER2_BREF_TYPE_INODE:
			error = hammer2_chain_modify(chain, mtid, dedup_off,
						     HAMMER2_MODIFY_OPTDATA);
			break;
		default:
			panic("hammer2_chain_create: bad type: %d",
			      chain->bref.type);
			break;
		}
	} else {
		if ((chain->flags & HAMMER2_CHAIN_UPDATE) == 0)
			atomic_set_int(&chain->flags, HAMMER2_CHAIN_UPDATE);
	}

	if (parent)
		hammer2_chain_setflush(parent);

done:
	*chainp = chain;

	return (error);
}

void
hammer2_chain_rename(hammer2_chain_t **parentp, hammer2_chain_t *chain,
		     hammer2_tid_t mtid, int flags)
{
	hammer2_blockref_t *bref;
	hammer2_chain_t *parent;

	KKASSERT(chain->parent == NULL);
	bref = &chain->bref;

	if (parentp && (parent = *parentp) != NULL) {
		KKASSERT(hammer2_mtx_owned(&parent->lock));
		KKASSERT(parent->refs > 0);
		KKASSERT(parent->error == 0);

		hammer2_chain_create(parentp, &chain, NULL, chain->pmp,
				     HAMMER2_METH_DEFAULT,
				     bref->key, bref->keybits, bref->type,
				     chain->bytes, mtid, 0, flags);
		KKASSERT(chain->flags & HAMMER2_CHAIN_UPDATE);
		hammer2_chain_setflush(*parentp);
	}
}

static void
hammer2_chain_rename_obref(hammer2_chain_t **parentp, hammer2_chain_t *chain,
			   hammer2_tid_t mtid, int flags,
			   hammer2_blockref_t *obref)
{
	hammer2_chain_rename(parentp, chain, mtid, flags);

	if (obref->type != HAMMER2_BREF_TYPE_EMPTY) {
		hammer2_blockref_t *tbase;
		int tcount;

		KKASSERT((chain->flags & HAMMER2_CHAIN_BLKMAPPED) == 0);
		hammer2_chain_modify(*parentp, mtid, 0, 0);
		tbase = hammer2_chain_base_and_count(*parentp, &tcount);
		hammer2_base_insert(*parentp, tbase, tcount, chain, obref);
		if (bcmp(obref, &chain->bref, sizeof(chain->bref))) {
			atomic_set_int(&chain->flags, HAMMER2_CHAIN_BLKMAPUPD |
						      HAMMER2_CHAIN_UPDATE);
		} else {
			atomic_clear_int(&chain->flags, HAMMER2_CHAIN_UPDATE);
		}
	}
}

static int
_hammer2_chain_delete_helper(hammer2_chain_t *parent, hammer2_chain_t *chain,
			     hammer2_tid_t mtid, int flags,
			     hammer2_blockref_t *obref)
{
	int error = 0;

	KKASSERT((chain->flags & HAMMER2_CHAIN_DELETED) == 0);
	KKASSERT(chain->parent == parent);

	if (chain->flags & HAMMER2_CHAIN_BLKMAPPED) {
		hammer2_blockref_t *base;
		int count;

		KKASSERT(parent != NULL);
		KKASSERT(parent->error == 0);
		KKASSERT((parent->flags & HAMMER2_CHAIN_INITIAL) == 0);
		error = hammer2_chain_modify(parent, mtid, 0, 0);
		if (error)
			goto done;

		KKASSERT(chain->flags & HAMMER2_CHAIN_ONRBTREE);
		hammer2_spin_ex(&chain->core.spin);
		hammer2_spin_ex(&parent->core.spin);

		atomic_set_int(&chain->flags, HAMMER2_CHAIN_DELETED);
		atomic_add_int(&parent->core.live_count, -1);
		++parent->core.generation;
		RB_REMOVE(hammer2_chain_tree, &parent->core.rbtree, chain);
		atomic_clear_int(&chain->flags, HAMMER2_CHAIN_ONRBTREE);
		--parent->core.chain_count;
		chain->parent = NULL;

		switch(parent->bref.type) {
		case HAMMER2_BREF_TYPE_INODE:
			if (parent->data &&
			    (parent->data->ipdata.meta.op_flags &
			     HAMMER2_OPFLAG_DIRECTDATA) == 0) {
				base =
				   &parent->data->ipdata.u.blockset.blockref[0];
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
			base = &parent->data->voldata.
					sroot_blockset.blockref[0];
			count = HAMMER2_SET_COUNT;
			break;
		case HAMMER2_BREF_TYPE_FREEMAP:
			base = &parent->data->blkset.blockref[0];
			count = HAMMER2_SET_COUNT;
			break;
		default:
			base = NULL;
			count = 0;
			panic("_hammer2_chain_delete_helper: "
			      "unrecognized blockref type: %d",
			      parent->bref.type);
			break;
		}

		if (base) {
			hammer2_base_delete(parent, base, count, chain, obref);
		}
		hammer2_spin_unex(&parent->core.spin);
		hammer2_spin_unex(&chain->core.spin);
	} else if (chain->flags & HAMMER2_CHAIN_ONRBTREE) {
		hammer2_spin_ex(&chain->core.spin);
		hammer2_spin_ex(&parent->core.spin);
		atomic_set_int(&chain->flags, HAMMER2_CHAIN_DELETED);
		atomic_add_int(&parent->core.live_count, -1);
		++parent->core.generation;
		RB_REMOVE(hammer2_chain_tree, &parent->core.rbtree, chain);
		atomic_clear_int(&chain->flags, HAMMER2_CHAIN_ONRBTREE);
		--parent->core.chain_count;
		chain->parent = NULL;
		hammer2_spin_unex(&parent->core.spin);
		hammer2_spin_unex(&chain->core.spin);
	} else {
		atomic_set_int(&chain->flags, HAMMER2_CHAIN_DELETED);
	}
done:
	return error;
}

static int hammer2_chain_indkey_freemap(hammer2_chain_t *parent,
				hammer2_key_t *keyp, int keybits,
				hammer2_blockref_t *base, int count);
static int hammer2_chain_indkey_file(hammer2_chain_t *parent,
				hammer2_key_t *keyp, int keybits,
				hammer2_blockref_t *base, int count,
				int ncount);
static int hammer2_chain_indkey_dir(hammer2_chain_t *parent,
				hammer2_key_t *keyp, int keybits,
				hammer2_blockref_t *base, int count,
				int ncount);
static
hammer2_chain_t *
hammer2_chain_create_indirect(hammer2_chain_t *parent,
			      hammer2_key_t create_key, int create_bits,
			      hammer2_tid_t mtid, int for_type, int *errorp)
{
	hammer2_dev_t *hmp;
	hammer2_blockref_t *base;
	hammer2_blockref_t *bref;
	hammer2_blockref_t bsave;
	hammer2_blockref_t dummy;
	hammer2_chain_t *chain;
	hammer2_chain_t *ichain;
	hammer2_key_t key = create_key;
	hammer2_key_t key_beg;
	hammer2_key_t key_end;
	hammer2_key_t key_next;
	int keybits = create_bits;
	int count;
	int ncount;
	int nbytes;
	int loops;
	int error;
	int reason;
	int generation;
	int maxloops = 300000;

	hmp = parent->hmp;
	KKASSERT(hammer2_mtx_owned(&parent->lock));

	*errorp = hammer2_chain_modify(parent, mtid, 0, 0);
	if (*errorp) {
		kprintf("hammer2_chain_create_indirect: error %08x %s\n",
			*errorp, hammer2_error_str(*errorp));
		return NULL;
	}
	KKASSERT((parent->flags & HAMMER2_CHAIN_INITIAL) == 0);

	base = hammer2_chain_base_and_count(parent, &count);

	if (for_type == HAMMER2_BREF_TYPE_FREEMAP_NODE ||
	    for_type == HAMMER2_BREF_TYPE_FREEMAP_LEAF) {
		nbytes = HAMMER2_FREEMAP_LEVELN_PSIZE;
	} else if (parent->bref.type == HAMMER2_BREF_TYPE_INODE) {
		if (parent->data->ipdata.meta.type ==
		    HAMMER2_OBJTYPE_DIRECTORY)
			nbytes = HAMMER2_IND_BYTES_MIN;
		else
			nbytes = HAMMER2_IND_BYTES_NOM;

	} else {
		nbytes = HAMMER2_IND_BYTES_NOM;
	}
	if (nbytes < count * sizeof(hammer2_blockref_t)) {
		KKASSERT(for_type != HAMMER2_BREF_TYPE_FREEMAP_NODE &&
			 for_type != HAMMER2_BREF_TYPE_FREEMAP_LEAF);
		nbytes = count * sizeof(hammer2_blockref_t);
	}
	ncount = nbytes / sizeof(hammer2_blockref_t);

	switch(for_type) {
	case HAMMER2_BREF_TYPE_FREEMAP_NODE:
	case HAMMER2_BREF_TYPE_FREEMAP_LEAF:
		keybits = hammer2_chain_indkey_freemap(parent, &key, keybits,
						       base, count);
		break;
	case HAMMER2_BREF_TYPE_DATA:
		keybits = hammer2_chain_indkey_file(parent, &key, keybits,
						    base, count, ncount);
		break;
	case HAMMER2_BREF_TYPE_DIRENT:
	case HAMMER2_BREF_TYPE_INODE:
		keybits = hammer2_chain_indkey_dir(parent, &key, keybits,
						   base, count, ncount);
		break;
	default:
		panic("illegal indirect block for bref type %d", for_type);
		break;
	}

	key &= ~(((hammer2_key_t)1 << keybits) - 1);

	bzero(&dummy, sizeof(dummy));
	if (for_type == HAMMER2_BREF_TYPE_FREEMAP_NODE ||
	    for_type == HAMMER2_BREF_TYPE_FREEMAP_LEAF) {
		dummy.type = HAMMER2_BREF_TYPE_FREEMAP_NODE;
	} else {
		dummy.type = HAMMER2_BREF_TYPE_INDIRECT;
	}
	dummy.key = key;
	dummy.keybits = keybits;
	dummy.data_off = hammer2_getradix(nbytes);
	dummy.methods =
		HAMMER2_ENC_CHECK(HAMMER2_DEC_CHECK(parent->bref.methods)) |
		HAMMER2_ENC_COMP(HAMMER2_COMP_NONE);

	ichain = hammer2_chain_alloc(hmp, parent->pmp, &dummy);
	atomic_set_int(&ichain->flags, HAMMER2_CHAIN_INITIAL);
	hammer2_chain_lock(ichain, HAMMER2_RESOLVE_MAYBE);

	*errorp = hammer2_chain_modify(ichain, mtid, 0, 0);
	if (*errorp) {
		kprintf("hammer2_chain_create_indirect: error %08x %s\n",
			*errorp, hammer2_error_str(*errorp));
		hammer2_chain_unlock(ichain);
		hammer2_chain_drop(ichain);
		return NULL;
	}
	KKASSERT((ichain->flags & HAMMER2_CHAIN_INITIAL) == 0);

	key_beg = 0;
	key_end = HAMMER2_KEY_MAX;
	key_next = 0;
	hammer2_spin_ex(&parent->core.spin);
	loops = 0;
	reason = 0;

	for (;;) {
		base = hammer2_chain_base_and_count(parent, &count);

		if (++loops > 100000) {
		    hammer2_spin_unex(&parent->core.spin);
		    panic("excessive loops r=%d p=%p base/count %p:%d %016jx\n",
			  reason, parent, base, count, key_next);
		}

		chain = hammer2_combined_find(parent, base, count,
					      &key_next,
					      key_beg, key_end,
					      &bref);
		generation = parent->core.generation;
		if (bref == NULL)
			break;
		key_next = bref->key + ((hammer2_key_t)1 << bref->keybits);

		if (rounddown2(key ^ bref->key, (hammer2_key_t)1 << keybits) != 0) {
			goto next_key_spinlocked;
		}

		bsave = *bref;
		if (chain) {
			hammer2_chain_ref(chain);
			hammer2_spin_unex(&parent->core.spin);
			hammer2_chain_lock(chain, HAMMER2_RESOLVE_NEVER);
		} else {
			hammer2_spin_unex(&parent->core.spin);
			chain = hammer2_chain_get(parent, generation, &bsave,
						  HAMMER2_RESOLVE_NEVER);
			if (chain == NULL) {
				reason = 1;
				hammer2_spin_ex(&parent->core.spin);
				continue;
			}
		}

		if (bcmp(&bsave, &chain->bref, sizeof(bsave)) ||
		    chain->parent != parent ||
		    (chain->flags & HAMMER2_CHAIN_DELETED)) {
			hammer2_chain_unlock(chain);
			hammer2_chain_drop(chain);
			if (hammer2_debug & 0x0040) {
				kprintf("LOST PARENT RETRY "
				"RETRY (%p,%p)->%p %08x\n",
				parent, chain->parent, chain, chain->flags);
			}
			hammer2_spin_ex(&parent->core.spin);
			continue;
		}

		error = hammer2_chain_delete_obref(parent, chain, mtid, 0,
						   &bsave);
		KKASSERT(error == 0);
		hammer2_chain_rename_obref(&ichain, chain, mtid, 0, &bsave);
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
		KKASSERT(parent->refs > 0);
		chain = NULL;
		base = NULL;
		hammer2_spin_ex(&parent->core.spin);
next_key_spinlocked:
		if (--maxloops == 0)
			panic("hammer2_chain_create_indirect: maxloops");
		reason = 4;
		if (key_next == 0 || key_next > key_end)
			break;
		key_beg = key_next;
	}
	hammer2_spin_unex(&parent->core.spin);

	base = NULL;
	KKASSERT((ichain->flags & HAMMER2_CHAIN_ONRBTREE) == 0);
	KKASSERT(parent->core.live_count < count);
	hammer2_chain_insert(parent, ichain,
			     HAMMER2_CHAIN_INSERT_SPIN |
			     HAMMER2_CHAIN_INSERT_LIVE,
			     0);

	hammer2_chain_setflush(ichain);
	hammer2_chain_setflush(parent);

	if (rounddown2(create_key ^ key, (hammer2_key_t)1 << keybits) != 0) {
		hammer2_chain_unlock(ichain);
		hammer2_chain_drop(ichain);
	} else {
		parent = ichain;
	}

	return(parent);
}

int
hammer2_chain_indirect_maintenance(hammer2_chain_t *parent,
				   hammer2_chain_t *chain)
{
	hammer2_blockref_t *chain_base;
	hammer2_blockref_t *base;
	hammer2_blockref_t *bref;
	hammer2_blockref_t bsave;
	hammer2_key_t key_next;
	hammer2_key_t key_beg;
	hammer2_key_t key_end;
	hammer2_chain_t *sub;
	int chain_count;
	int count;
	int error;
	int generation;

	if ((chain->flags & (HAMMER2_CHAIN_INITIAL |
			     HAMMER2_CHAIN_COUNTEDBREFS)) == 0) {
		base = &chain->data->npdata[0];
		count = chain->bytes / sizeof(hammer2_blockref_t);
		hammer2_chain_countbrefs(chain, base, count);
	}

	if (chain->core.live_count == 0 && RB_EMPTY(&chain->core.rbtree)) {
		hammer2_chain_delete(parent, chain,
				     chain->bref.modify_tid,
				     HAMMER2_DELETE_PERMANENT);
		hammer2_chain_repchange(parent, chain);
		return 1;
	}

	base = hammer2_chain_base_and_count(parent, &count);

	if ((parent->flags & (HAMMER2_CHAIN_INITIAL |
			     HAMMER2_CHAIN_COUNTEDBREFS)) == 0) {
		hammer2_chain_countbrefs(parent, base, count);
	}

	if (parent->core.live_count + chain->core.live_count - 1 > count)
		return 0;
	chain_count = chain->bytes / sizeof(hammer2_blockref_t);
	if (chain->core.live_count > chain_count * 3 / 4)
		return 0;

	error = hammer2_chain_modify(parent, 0, 0, 0);
	if (error) {
		krateprintf(&krate_h2me, "hammer2: indirect_maint: %s\n",
			    hammer2_error_str(error));
		return 0;
	}
	error = hammer2_chain_modify(chain, chain->bref.modify_tid, 0, 0);
	if (error) {
		krateprintf(&krate_h2me, "hammer2: indirect_maint: %s\n",
			    hammer2_error_str(error));
		return 0;
	}

	chain->bref.embed.stats.inode_count = 0;
	chain->bref.embed.stats.data_count = 0;
	error = hammer2_chain_delete(parent, chain,
				     chain->bref.modify_tid,
				     HAMMER2_DELETE_PERMANENT);
	KKASSERT(error == 0);

	hammer2_spin_ex(&chain->core.spin);

	key_next = 0;
	key_beg = 0;
	key_end = HAMMER2_KEY_MAX;
	for (;;) {
		chain_base = &chain->data->npdata[0];
		chain_count = chain->bytes / sizeof(hammer2_blockref_t);
		sub = hammer2_combined_find(chain, chain_base, chain_count,
					    &key_next,
					    key_beg, key_end,
					    &bref);
		generation = chain->core.generation;
		if (bref == NULL)
			break;
		key_next = bref->key + ((hammer2_key_t)1 << bref->keybits);

		bsave = *bref;
		if (sub) {
			hammer2_chain_ref(sub);
			hammer2_spin_unex(&chain->core.spin);
			hammer2_chain_lock(sub, HAMMER2_RESOLVE_NEVER);
		} else {
			hammer2_spin_unex(&chain->core.spin);
			sub = hammer2_chain_get(chain, generation, &bsave,
						HAMMER2_RESOLVE_NEVER);
			if (sub == NULL) {
				hammer2_spin_ex(&chain->core.spin);
				continue;
			}
		}
		if (bcmp(&bsave, &sub->bref, sizeof(bsave)) ||
		    sub->parent != chain ||
		    (sub->flags & HAMMER2_CHAIN_DELETED)) {
			hammer2_chain_unlock(sub);
			hammer2_chain_drop(sub);
			hammer2_spin_ex(&chain->core.spin);
			sub = NULL;
			continue;
		}
		error = hammer2_chain_delete_obref(chain, sub,
						   sub->bref.modify_tid, 0,
						   &bsave);
		KKASSERT(error == 0);
		hammer2_chain_rename_obref(&parent, sub,
				     sub->bref.modify_tid,
				     HAMMER2_INSERT_SAMEPARENT, &bsave);
		hammer2_chain_unlock(sub);
		hammer2_chain_drop(sub);
		hammer2_spin_ex(&chain->core.spin);

		if (key_next == 0)
			break;
		key_beg = key_next;
	}
	hammer2_spin_unex(&chain->core.spin);

	hammer2_chain_repchange(parent, chain);

	return 1;
}

int
hammer2_chain_indkey_freemap(hammer2_chain_t *parent, hammer2_key_t *keyp,
			     int keybits, hammer2_blockref_t *base, int count)
{
	hammer2_chain_t *chain;
	hammer2_blockref_t *bref;
	hammer2_key_t key;
	hammer2_key_t key_beg;
	hammer2_key_t key_end;
	hammer2_key_t key_next;
	int maxloops = 300000;

	key = *keyp;
	keybits = 64;

	key_beg = 0;
	key_end = HAMMER2_KEY_MAX;
	hammer2_spin_ex(&parent->core.spin);

	for (;;) {
		if (--maxloops == 0) {
			panic("indkey_freemap shit %p %p:%d\n",
			      parent, base, count);
		}
		chain = hammer2_combined_find(parent, base, count,
					      &key_next,
					      key_beg, key_end,
					      &bref);

		if (bref == NULL)
			break;

		if (chain && (chain->flags & HAMMER2_CHAIN_DELETED)) {
			if (key_next == 0 || key_next > key_end)
				break;
			key_beg = key_next;
			continue;
		}

		key_next = bref->key + ((hammer2_key_t)1 << bref->keybits);

		if (keybits > bref->keybits) {
			key = bref->key;
			keybits = bref->keybits;
		} else if (keybits == bref->keybits && bref->key < key) {
			key = bref->key;
		}
		if (key_next == 0)
			break;
		key_beg = key_next;
	}
	hammer2_spin_unex(&parent->core.spin);

	switch(keybits) {
	case HAMMER2_FREEMAP_LEVEL0_RADIX:
		keybits = HAMMER2_FREEMAP_LEVEL1_RADIX;
		break;
	case HAMMER2_FREEMAP_LEVEL1_RADIX:
		keybits = HAMMER2_FREEMAP_LEVEL2_RADIX;
		break;
	case HAMMER2_FREEMAP_LEVEL2_RADIX:
		keybits = HAMMER2_FREEMAP_LEVEL3_RADIX;
		break;
	case HAMMER2_FREEMAP_LEVEL3_RADIX:
		keybits = HAMMER2_FREEMAP_LEVEL4_RADIX;
		break;
	case HAMMER2_FREEMAP_LEVEL4_RADIX:
		keybits = HAMMER2_FREEMAP_LEVEL5_RADIX;
		break;
	case HAMMER2_FREEMAP_LEVEL5_RADIX:
		panic("hammer2_chain_indkey_freemap: level too high");
		break;
	default:
		panic("hammer2_chain_indkey_freemap: bad radix");
		break;
	}
	*keyp = key;

	return (keybits);
}

static int
hammer2_chain_indkey_file(hammer2_chain_t *parent, hammer2_key_t *keyp,
			    int keybits, hammer2_blockref_t *base, int count,
			    int ncount)
{
	hammer2_chain_t *chain;
	hammer2_blockref_t *bref;
	hammer2_key_t key;
	hammer2_key_t key_beg;
	hammer2_key_t key_end;
	hammer2_key_t key_next;
	int nradix;
	int maxloops = 300000;

	key = *keyp;
	keybits = 64;

	key_beg = 0;
	key_end = HAMMER2_KEY_MAX;
	hammer2_spin_ex(&parent->core.spin);

	for (;;) {
		if (--maxloops == 0) {
			panic("indkey_freemap shit %p %p:%d\n",
			      parent, base, count);
		}
		chain = hammer2_combined_find(parent, base, count,
					      &key_next,
					      key_beg, key_end,
					      &bref);

		if (bref == NULL)
			break;

		if (chain && (chain->flags & HAMMER2_CHAIN_DELETED)) {
			if (key_next == 0 || key_next > key_end)
				break;
			key_beg = key_next;
			continue;
		}

		key_next = bref->key + ((hammer2_key_t)1 << bref->keybits);

		if (keybits > bref->keybits) {
			key = bref->key;
			keybits = bref->keybits;
		} else if (keybits == bref->keybits && bref->key < key) {
			key = bref->key;
		}
		if (key_next == 0)
			break;
		key_beg = key_next;
	}
	hammer2_spin_unex(&parent->core.spin);

	*keyp = key;

	switch(ncount) {
	case HAMMER2_IND_COUNT_MIN:
		nradix = HAMMER2_IND_RADIX_MIN - HAMMER2_BLOCKREF_RADIX;
		break;
	case HAMMER2_IND_COUNT_NOM:
		nradix = HAMMER2_IND_RADIX_NOM - HAMMER2_BLOCKREF_RADIX;
		break;
	case HAMMER2_IND_COUNT_MAX:
		nradix = HAMMER2_IND_RADIX_MAX - HAMMER2_BLOCKREF_RADIX;
		break;
	default:
		panic("bad ncount %d\n", ncount);
		nradix = 0;
		break;
	}

	if (nradix >= 64)
		nradix = 63;

	return keybits + nradix;
}

#if 1

static int
hammer2_chain_indkey_dir(hammer2_chain_t *parent, hammer2_key_t *keyp,
			 int keybits, hammer2_blockref_t *base, int count,
			 int ncount)
{
	hammer2_blockref_t *bref;
	hammer2_chain_t	*chain;
	hammer2_key_t key_beg;
	hammer2_key_t key_end;
	hammer2_key_t key_next;
	hammer2_key_t key;
	int nkeybits;
	int locount;
	int hicount;
	int maxloops = 300000;

	key = *keyp;
	locount = 0;
	hicount = 0;

	key_beg = 0;
	key_end = HAMMER2_KEY_MAX;
	hammer2_spin_ex(&parent->core.spin);

	for (;;) {
		if (--maxloops == 0) {
			panic("indkey_freemap shit %p %p:%d\n",
			      parent, base, count);
		}
		chain = hammer2_combined_find(parent, base, count,
					      &key_next,
					      key_beg, key_end,
					      &bref);

		if (bref == NULL)
			break;

		if (chain && (chain->flags & HAMMER2_CHAIN_DELETED)) {
			if (key_next == 0 || key_next > key_end)
				break;
			key_beg = key_next;
			continue;
		}

		key_next = bref->key + ((hammer2_key_t)1 << bref->keybits);

		nkeybits = keybits;
		if (nkeybits < bref->keybits) {
			if (bref->keybits > 64) {
				kprintf("bad bref chain %p bref %p\n",
					chain, bref);
				Debugger("fubar");
			}
			nkeybits = bref->keybits;
		}
		while (nkeybits < 64 &&
		       rounddown2(key ^ bref->key, (hammer2_key_t)1 << nkeybits) != 0) {
			++nkeybits;
		}

		if (keybits != nkeybits) {
			if (((hammer2_key_t)1 << (nkeybits - 1)) & key) {
				hicount += locount;
				locount = 0;
			} else {
				locount += hicount;
				hicount = 0;
			}
			keybits = nkeybits;
		}

		if (((hammer2_key_t)1 << (nkeybits - 1)) & bref->key)
			++hicount;
		else
			++locount;

		if (key_next == 0)
			break;
		key_beg = key_next;
	}
	hammer2_spin_unex(&parent->core.spin);
	bref = NULL;

	--keybits;

	while (((hammer2_key_t)1 << keybits) < ncount) {
		++keybits;
		if (key & ((hammer2_key_t)1 << keybits)) {
			hicount += locount;
			locount = 0;
		} else {
			locount += hicount;
			hicount = 0;
		}
	}

	if (hicount > locount)
		key |= (hammer2_key_t)1 << keybits;
	else
		key &= ~(hammer2_key_t)1 << keybits;

	*keyp = key;

	return (keybits);
}

#else

static int
hammer2_chain_indkey_dir(hammer2_chain_t *parent, hammer2_key_t *keyp,
			 int keybits, hammer2_blockref_t *base, int count,
			 int ncount)
{
	hammer2_blockref_t *bref;
	hammer2_chain_t	*chain;
	hammer2_key_t key_beg;
	hammer2_key_t key_end;
	hammer2_key_t key_next;
	hammer2_key_t key;
	int nkeybits;
	int locount;
	int hicount;
	int maxloops = 300000;

	if (parent->bref.type == HAMMER2_BREF_TYPE_INODE) {
		return 63;
	}

	key = *keyp;
	locount = 0;
	hicount = 0;

	key_beg = 0;
	key_end = HAMMER2_KEY_MAX;
	hammer2_spin_ex(&parent->core.spin);

	for (;;) {
		if (--maxloops == 0) {
			panic("indkey_freemap shit %p %p:%d\n",
			      parent, base, count);
		}
		chain = hammer2_combined_find(parent, base, count,
					      &key_next,
					      key_beg, key_end,
					      &bref);

		if (bref == NULL)
			break;

		if (chain && (chain->flags & HAMMER2_CHAIN_DELETED)) {
			if (key_next == 0 || key_next > key_end)
				break;
			key_beg = key_next;
			continue;
		}

		key_next = bref->key + ((hammer2_key_t)1 << bref->keybits);

		nkeybits = keybits;
		if (nkeybits < bref->keybits) {
			if (bref->keybits > 64) {
				kprintf("bad bref chain %p bref %p\n",
					chain, bref);
				Debugger("fubar");
			}
			nkeybits = bref->keybits;
		}
		while (nkeybits < 64 &&
		       (~(((hammer2_key_t)1 << nkeybits) - 1) &
		        (key ^ bref->key)) != 0) {
			++nkeybits;
		}

		if (keybits != nkeybits) {
			if (((hammer2_key_t)1 << (nkeybits - 1)) & key) {
				hicount += locount;
				locount = 0;
			} else {
				locount += hicount;
				hicount = 0;
			}
			keybits = nkeybits;
		}

		if (((hammer2_key_t)1 << (nkeybits - 1)) & bref->key)
			++hicount;
		else
			++locount;

		if (key_next == 0)
			break;
		key_beg = key_next;
	}
	hammer2_spin_unex(&parent->core.spin);
	bref = NULL;

	--keybits;

	while (((hammer2_key_t)1 << keybits) < ncount) {
		++keybits;
		if (key & ((hammer2_key_t)1 << keybits)) {
			hicount += locount;
			locount = 0;
		} else {
			locount += hicount;
			hicount = 0;
		}
	}

	if (hicount > locount)
		key |= (hammer2_key_t)1 << keybits;
	else
		key &= ~(hammer2_key_t)1 << keybits;

	*keyp = key;

	return (keybits);
}

#endif

int
hammer2_chain_delete(hammer2_chain_t *parent, hammer2_chain_t *chain,
		     hammer2_tid_t mtid, int flags)
{
	int error = 0;

	KKASSERT(hammer2_mtx_owned(&chain->lock));

	if ((chain->flags & HAMMER2_CHAIN_DELETED) == 0) {
		KKASSERT((chain->flags & HAMMER2_CHAIN_DELETED) == 0 &&
			 chain->parent == parent);
		error = _hammer2_chain_delete_helper(parent, chain,
						     mtid, flags, NULL);
	}

	if (error == 0) {
		if (flags & HAMMER2_DELETE_PERMANENT) {
			atomic_set_int(&chain->flags, HAMMER2_CHAIN_DESTROY);
			hammer2_chain_setflush(chain);
		}
	}

	return error;
}

static int
hammer2_chain_delete_obref(hammer2_chain_t *parent, hammer2_chain_t *chain,
		     hammer2_tid_t mtid, int flags,
		     hammer2_blockref_t *obref)
{
	int error = 0;

	KKASSERT(hammer2_mtx_owned(&chain->lock));

	obref->type = HAMMER2_BREF_TYPE_EMPTY;
	if ((chain->flags & HAMMER2_CHAIN_DELETED) == 0) {
		KKASSERT((chain->flags & HAMMER2_CHAIN_DELETED) == 0 &&
			 chain->parent == parent);
		error = _hammer2_chain_delete_helper(parent, chain,
						     mtid, flags, obref);
	}

	if (error == 0) {
		if (flags & HAMMER2_DELETE_PERMANENT) {
			atomic_set_int(&chain->flags, HAMMER2_CHAIN_DESTROY);
			hammer2_chain_setflush(chain);
		}
	}

	return error;
}

static int
hammer2_base_find(hammer2_chain_t *parent,
		  hammer2_blockref_t *base, int count,
		  hammer2_key_t *key_nextp,
		  hammer2_key_t key_beg, hammer2_key_t key_end __unused)
{
	hammer2_blockref_t *scan;
	hammer2_key_t scan_end;
	int i;
	int limit;

	KKASSERT(parent->flags & HAMMER2_CHAIN_COUNTEDBREFS);

	if (count == 0 || base == NULL)
		return(count);

	i = parent->cache_index;
	cpu_ccfence();
	limit = parent->core.live_zero;
	if (i >= limit)
		i = limit - 1;
	if (i < 0)
		i = 0;
	KKASSERT(i < count);

	scan = &base[i];
	while (i > 0 && (scan->type == HAMMER2_BREF_TYPE_EMPTY ||
	    scan->key > key_beg)) {
		--scan;
		--i;
	}
	parent->cache_index = i;

	while (i < count) {
		if (scan->type != HAMMER2_BREF_TYPE_EMPTY) {
			scan_end = scan->key +
				   ((hammer2_key_t)1 << scan->keybits) - 1;
			if (scan->key > key_beg || scan_end >= key_beg)
				break;
		}
		if (i >= limit)
			return (count);
		++scan;
		++i;
	}
	if (i != count) {
		parent->cache_index = i;
		if (i >= limit) {
			i = count;
		} else {
			scan_end = scan->key +
				   ((hammer2_key_t)1 << scan->keybits);
			if (scan_end && (*key_nextp > scan_end ||
					 *key_nextp == 0)) {
				*key_nextp = scan_end;
			}
		}
	}
	return (i);
}

static hammer2_chain_t *
hammer2_combined_find(hammer2_chain_t *parent,
		      hammer2_blockref_t *base, int count,
		      hammer2_key_t *key_nextp,
		      hammer2_key_t key_beg, hammer2_key_t key_end,
		      hammer2_blockref_t **brefp)
{
	hammer2_blockref_t *bref;
	hammer2_chain_t *chain;
	int i;

	*key_nextp = key_end + 1;
	i = hammer2_base_find(parent, base, count, key_nextp,
			      key_beg, key_end);
	chain = hammer2_chain_find(parent, key_nextp, key_beg, key_end);

	if (i == count && chain == NULL) {
		*brefp = NULL;
		return(NULL);
	}

	if (i == count) {
		bref = &chain->bref;
		goto found;
	}

	if (chain == NULL) {
		bref = &base[i];
		goto found;
	}

	if ((chain->bref.key <= key_beg && base[i].key <= key_beg) ||
	    chain->bref.key == base[i].key) {
		KKASSERT(chain->bref.key == base[i].key);
		bref = &chain->bref;
		goto found;
	}

	if (chain->bref.key < base[i].key) {
		bref = &chain->bref;
	} else {
		bref = &base[i];
		chain = NULL;
	}

found:
	if (bref->key > key_end) {
		*brefp = NULL;
		chain = NULL;
	} else {
		*brefp = bref;
	}
	return(chain);
}

void
hammer2_base_delete(hammer2_chain_t *parent,
		    hammer2_blockref_t *base, int count,
		    hammer2_chain_t *chain,
		    hammer2_blockref_t *obref)
{
	hammer2_blockref_t *elm = &chain->bref;
	hammer2_blockref_t *scan;
	hammer2_key_t key_next;
	int i;

	key_next = 0;
	i = hammer2_base_find(parent, base, count, &key_next,
			      elm->key, elm->key);
	scan = &base[i];
	if (i == count || scan->type == HAMMER2_BREF_TYPE_EMPTY ||
	    scan->key != elm->key ||
	    ((chain->flags & HAMMER2_CHAIN_BLKMAPUPD) == 0 &&
	     scan->keybits != elm->keybits)) {
		hammer2_spin_unex(&parent->core.spin);
		panic("delete base %p element not found at %d/%d elm %p\n",
		      base, i, count, elm);
		return;
	}

	if ((int)(scan->data_off & HAMMER2_OFF_MASK_RADIX)) {
		parent->bref.embed.stats.data_count -= (hammer2_off_t)1 <<
				(int)(scan->data_off & HAMMER2_OFF_MASK_RADIX);
	}
	switch(scan->type) {
	case HAMMER2_BREF_TYPE_INODE:
		--parent->bref.embed.stats.inode_count;
	case HAMMER2_BREF_TYPE_DATA:
		if (parent->bref.leaf_count == HAMMER2_BLOCKREF_LEAF_MAX) {
			atomic_set_int(&chain->flags,
				       HAMMER2_CHAIN_HINT_LEAF_COUNT);
		} else {
			if (parent->bref.leaf_count)
				--parent->bref.leaf_count;
		}
	case HAMMER2_BREF_TYPE_INDIRECT:
		if (scan->type != HAMMER2_BREF_TYPE_DATA) {
			parent->bref.embed.stats.data_count -=
				scan->embed.stats.data_count;
			parent->bref.embed.stats.inode_count -=
				scan->embed.stats.inode_count;
		}
		if (scan->type == HAMMER2_BREF_TYPE_INODE)
			break;
		if (parent->bref.leaf_count == HAMMER2_BLOCKREF_LEAF_MAX) {
			atomic_set_int(&chain->flags,
				       HAMMER2_CHAIN_HINT_LEAF_COUNT);
		} else {
			if (parent->bref.leaf_count <= scan->leaf_count)
				parent->bref.leaf_count = 0;
			else
				parent->bref.leaf_count -= scan->leaf_count;
		}
		break;
	case HAMMER2_BREF_TYPE_DIRENT:
		if (parent->bref.leaf_count == HAMMER2_BLOCKREF_LEAF_MAX) {
			atomic_set_int(&chain->flags,
				       HAMMER2_CHAIN_HINT_LEAF_COUNT);
		} else {
			if (parent->bref.leaf_count)
				--parent->bref.leaf_count;
		}
	default:
		break;
	}

	if (obref)
		*obref = *scan;
	bzero(scan, sizeof(*scan));

	if (parent->core.live_zero == i + 1) {
		while (--i >= 0 && base[i].type == HAMMER2_BREF_TYPE_EMPTY)
			;
		parent->core.live_zero = i + 1;
	}

	atomic_clear_int(&chain->flags, HAMMER2_CHAIN_BLKMAPPED |
					HAMMER2_CHAIN_BLKMAPUPD);
}

void
hammer2_base_insert(hammer2_chain_t *parent,
		    hammer2_blockref_t *base, int count,
		    hammer2_chain_t *chain, hammer2_blockref_t *elm)
{
	hammer2_key_t key_next;
	hammer2_key_t xkey;
	int i;
	int j;
	int k;
	int l;
	int u = 1;

	key_next = 0;
	i = hammer2_base_find(parent, base, count, &key_next,
			      elm->key, elm->key);

	KKASSERT(i >= 0 && i <= count);

	if (chain)
		atomic_set_int(&chain->flags, HAMMER2_CHAIN_BLKMAPPED);

	if ((int)(elm->data_off & HAMMER2_OFF_MASK_RADIX)) {
		parent->bref.embed.stats.data_count += (hammer2_off_t)1 <<
				(int)(elm->data_off & HAMMER2_OFF_MASK_RADIX);
	}
	switch(elm->type) {
	case HAMMER2_BREF_TYPE_INODE:
		++parent->bref.embed.stats.inode_count;
	case HAMMER2_BREF_TYPE_DATA:
		if (parent->bref.leaf_count != HAMMER2_BLOCKREF_LEAF_MAX)
			++parent->bref.leaf_count;
	case HAMMER2_BREF_TYPE_INDIRECT:
		if (elm->type != HAMMER2_BREF_TYPE_DATA) {
			parent->bref.embed.stats.data_count +=
				elm->embed.stats.data_count;
			parent->bref.embed.stats.inode_count +=
				elm->embed.stats.inode_count;
		}
		if (elm->type == HAMMER2_BREF_TYPE_INODE)
			break;
		if (parent->bref.leaf_count + elm->leaf_count <
		    HAMMER2_BLOCKREF_LEAF_MAX) {
			parent->bref.leaf_count += elm->leaf_count;
		} else {
			parent->bref.leaf_count = HAMMER2_BLOCKREF_LEAF_MAX;
		}
		break;
	case HAMMER2_BREF_TYPE_DIRENT:
		if (parent->bref.leaf_count != HAMMER2_BLOCKREF_LEAF_MAX)
			++parent->bref.leaf_count;
		break;
	default:
		break;
	}

	if (i == count && parent->core.live_zero < count) {
		i = parent->core.live_zero++;
		base[i] = *elm;
		return;
	}

	xkey = elm->key + ((hammer2_key_t)1 << elm->keybits) - 1;
	if (i != count && (base[i].key < elm->key || xkey >= base[i].key)) {
		hammer2_spin_unex(&parent->core.spin);
		panic("insert base %p overlapping elements at %d elm %p\n",
		      base, i, elm);
	}

	j = i;
	k = i;
	while (j > 0 || k < count) {
		--j;
		if (j >= 0 && base[j].type == HAMMER2_BREF_TYPE_EMPTY) {
			if (j == i - 1) {
				base[j] = *elm;
			} else {
				bcopy(&base[j+1], &base[j],
				      (i - j - 1) * sizeof(*base));
				base[i - 1] = *elm;
			}
			goto validate;
		}
		++k;
		if (k < count && base[k].type == HAMMER2_BREF_TYPE_EMPTY) {
			bcopy(&base[i], &base[i+1],
			      (k - i) * sizeof(hammer2_blockref_t));
			base[i] = *elm;

			if (parent->core.live_zero <= k)
				parent->core.live_zero = k + 1;
			u = 2;
			goto validate;
		}
	}
	panic("hammer2_base_insert: no room!");

validate:
	key_next = 0;
	for (l = 0; l < count; ++l) {
		if (base[l].type != HAMMER2_BREF_TYPE_EMPTY) {
			key_next = base[l].key +
				   ((hammer2_key_t)1 << base[l].keybits) - 1;
			break;
		}
	}
	while (++l < count) {
		if (base[l].type != HAMMER2_BREF_TYPE_EMPTY) {
			if (base[l].key <= key_next)
				panic("base_insert %d %d,%d,%d fail %p:%d", u, i, j, k, base, l);
			key_next = base[l].key +
				   ((hammer2_key_t)1 << base[l].keybits) - 1;

		}
	}

}

void
hammer2_chain_setcheck(hammer2_chain_t *chain, void *bdata)
{
	atomic_clear_int(&chain->flags, HAMMER2_CHAIN_NOTTESTED);

	switch(HAMMER2_DEC_CHECK(chain->bref.methods)) {
	case HAMMER2_CHECK_NONE:
		break;
	case HAMMER2_CHECK_DISABLED:
		break;
	case HAMMER2_CHECK_ISCSI32:
		chain->bref.check.iscsi32.value =
			hammer2_icrc32(bdata, chain->bytes);
		break;
	case HAMMER2_CHECK_XXHASH64:
		chain->bref.check.xxhash64.value =
			XXH64(bdata, chain->bytes, XXH_HAMMER2_SEED);
		break;
	case HAMMER2_CHECK_SHA192:
		{
			SHA256_CTX hash_ctx;
			union {
				uint8_t digest[SHA256_DIGEST_LENGTH];
				uint64_t digest64[SHA256_DIGEST_LENGTH/8];
			} u;

			SHA256_Init(&hash_ctx);
			SHA256_Update(&hash_ctx, bdata, chain->bytes);
			SHA256_Final(u.digest, &hash_ctx);
			u.digest64[2] ^= u.digest64[3];
			bcopy(u.digest,
			      chain->bref.check.sha192.data,
			      sizeof(chain->bref.check.sha192.data));
		}
		break;
	case HAMMER2_CHECK_FREEMAP:
		chain->bref.check.freemap.icrc32 =
			hammer2_icrc32(bdata, chain->bytes);
		break;
	default:
		kprintf("hammer2_chain_setcheck: unknown check type %02x\n",
			chain->bref.methods);
		break;
	}
}

static void
hammer2_characterize_failed_chain(hammer2_chain_t *chain, uint64_t check,
				  int bits, void *bdata)
{
	hammer2_chain_t *lchain;
	hammer2_chain_t *ochain;
	int did;

	did = krateprintf(&krate_h2chk,
		"chain %016jx.%02x (%s) meth=%02x CHECK FAIL "
		"(flags=%08x, bref/data ",
		chain->bref.data_off,
		chain->bref.type,
		hammer2_bref_type_str(chain->bref.type),
		chain->bref.methods,
		chain->flags);
	if (did == 0)
		return;

	if (bits == 32) {
		kprintf("%08x/%08x)\n",
			chain->bref.check.iscsi32.value,
			(uint32_t)check);
	} else {
		kprintf("%016jx/%016jx)\n",
			chain->bref.check.xxhash64.value,
			check);
	}

	kprintf("   chain %p bdata %p dio %p bp %p ",
		chain,
		bdata,
		chain->dio,
		(chain->dio ? chain->dio->bp : NULL));

	if (chain->dio) {
		kprintf("bp_loff %016jx,%d bdata %p/%p",
			(intmax_t)chain->dio->bp->b_loffset,
			chain->dio->bp->b_bufsize,
			bdata,
			chain->dio->bp->b_data);
	}
	kprintf("\n");

	ochain = chain;
	lchain = chain;
	while (chain && chain->bref.type != HAMMER2_BREF_TYPE_INODE) {
		lchain = chain;
		chain = chain->parent;
	}

	if (chain && chain->bref.type == HAMMER2_BREF_TYPE_INODE &&
	    ((chain->bref.flags & HAMMER2_BREF_FLAG_PFSROOT) == 0 ||
	     (lchain->bref.key & HAMMER2_DIRHASH_VISIBLE))) {
		kprintf("   Resides at/in inode %ld\n",
			(long)chain->bref.key);
	} else if (chain && chain->bref.type == HAMMER2_BREF_TYPE_INODE) {
		kprintf("   Resides in inode index - CRITICAL!!!\n");
	} else {
		kprintf("   Resides in root index - CRITICAL!!!\n");
	}
	if (ochain->hmp) {
		const char *pfsname = "UNKNOWN";
		int i;

		if (ochain->pmp) {
			for (i = 0; i < HAMMER2_MAXCLUSTER; ++i) {
				if (ochain->pmp->pfs_hmps[i] == ochain->hmp &&
				    ochain->pmp->pfs_names[i]) {
					pfsname = ochain->pmp->pfs_names[i];
					break;
				}
			}
		}
		kprintf("   In pfs %s on device %s\n",
			pfsname, ochain->hmp->devrepname);
	}
}

int
hammer2_chain_testcheck(hammer2_chain_t *chain, void *bdata)
{
	uint32_t check32;
	uint64_t check64;
	int r;

	if (chain->flags & HAMMER2_CHAIN_NOTTESTED)
		return 1;

	switch(HAMMER2_DEC_CHECK(chain->bref.methods)) {
	case HAMMER2_CHECK_NONE:
		r = 1;
		break;
	case HAMMER2_CHECK_DISABLED:
		r = 1;
		break;
	case HAMMER2_CHECK_ISCSI32:
		check32 = hammer2_icrc32(bdata, chain->bytes);
		r = (chain->bref.check.iscsi32.value == check32);
		if (r == 0) {
			hammer2_characterize_failed_chain(chain, check32,
							  32, bdata);
		}
		hammer2_process_icrc32 += chain->bytes;
		break;
	case HAMMER2_CHECK_XXHASH64:
		check64 = XXH64(bdata, chain->bytes, XXH_HAMMER2_SEED);
		r = (chain->bref.check.xxhash64.value == check64);
		if (r == 0) {
			hammer2_characterize_failed_chain(chain, check64,
							  64, bdata);
		}
		hammer2_process_xxhash64 += chain->bytes;
		break;
	case HAMMER2_CHECK_SHA192:
		{
			SHA256_CTX hash_ctx;
			union {
				uint8_t digest[SHA256_DIGEST_LENGTH];
				uint64_t digest64[SHA256_DIGEST_LENGTH/8];
			} u;

			SHA256_Init(&hash_ctx);
			SHA256_Update(&hash_ctx, bdata, chain->bytes);
			SHA256_Final(u.digest, &hash_ctx);
			u.digest64[2] ^= u.digest64[3];
			if (bcmp(u.digest,
				 chain->bref.check.sha192.data,
			         sizeof(chain->bref.check.sha192.data)) == 0) {
				r = 1;
			} else {
				r = 0;
				krateprintf(&krate_h2chk,
					"chain %016jx.%02x meth=%02x "
					"CHECK FAIL\n",
					chain->bref.data_off,
					chain->bref.type,
					chain->bref.methods);
			}
		}
		break;
	case HAMMER2_CHECK_FREEMAP:
		r = (chain->bref.check.freemap.icrc32 ==
		     hammer2_icrc32(bdata, chain->bytes));
		if (r == 0) {
			int did;

			did = krateprintf(&krate_h2chk,
					  "chain %016jx.%02x meth=%02x "
					  "CHECK FAIL\n",
					  chain->bref.data_off,
					  chain->bref.type,
					  chain->bref.methods);
			if (did) {
				kprintf("freemap.icrc %08x icrc32 %08x (%d)\n",
					chain->bref.check.freemap.icrc32,
					hammer2_icrc32(bdata, chain->bytes),
					chain->bytes);
				if (chain->dio) {
					kprintf("dio %p buf %016jx,%d "
						"bdata %p/%p\n",
						chain->dio,
						(intmax_t)chain->dio->bp->b_loffset,
						chain->dio->bp->b_bufsize,
						bdata,
						chain->dio->bp->b_data);
				}
			}
		}
		break;
	default:
		kprintf("hammer2_chain_testcheck: unknown check type %02x\n",
			chain->bref.methods);
		r = 1;
		break;
	}
	return r;
}

int
hammer2_chain_inode_find(hammer2_pfs_t *pmp, hammer2_key_t inum,
			 int clindex, int flags,
			 hammer2_chain_t **parentp, hammer2_chain_t **chainp)
{
	hammer2_chain_t *parent;
	hammer2_chain_t *rchain;
	hammer2_key_t key_dummy;
	hammer2_inode_t *ip;
	int resolve_flags;
	int error;

	KKASSERT((flags & HAMMER2_LOOKUP_NODATA) == 0);

	resolve_flags = (flags & HAMMER2_LOOKUP_SHARED) ?
			HAMMER2_RESOLVE_SHARED : 0;

	if (*chainp) {
		hammer2_chain_unlock(*chainp);
		hammer2_chain_drop(*chainp);
		*chainp = NULL;
	}
	if (*parentp) {
		hammer2_chain_unlock(*parentp);
		hammer2_chain_drop(*parentp);
		*parentp = NULL;
	}

	ip = hammer2_inode_lookup(pmp, inum);
	if (ip) {
		*chainp = hammer2_inode_chain_and_parent(ip, clindex,
						       parentp,
						       resolve_flags);
		hammer2_inode_drop(ip);
		if (*chainp)
			return (*chainp)->error;
		if (*parentp) {
			hammer2_chain_unlock(*parentp);
			hammer2_chain_drop(*parentp);
			*parentp = NULL;
		}
	}

	parent = hammer2_inode_chain(pmp->iroot, clindex, resolve_flags);
	rchain = NULL;
	if (parent) {
		rchain = hammer2_chain_lookup(&parent, &key_dummy,
					      inum, inum,
					      &error, flags);
		if (error == 0 && rchain) {
			error = rchain->error;
			if (error == 0 && rchain->data) {
				if (inum != rchain->data->ipdata.meta.inum) {
					kprintf("hammer2_chain_inode_find: lookup inum %ld, "
						"got valid inode but with inum %ld\n",
						(long)inum, (long)rchain->data->ipdata.meta.inum);
					error = HAMMER2_ERROR_CHECK;
					rchain->error = error;
				}
			}
		}
	} else {
		error = HAMMER2_ERROR_EIO;
	}
	*parentp = parent;
	*chainp = rchain;

	return error;
}

hammer2_chain_t *
hammer2_chain_bulksnap(hammer2_dev_t *hmp)
{
	hammer2_chain_t *copy;

	copy = hammer2_chain_alloc(hmp, hmp->spmp, &hmp->vchain.bref);
	copy->data = kmalloc(sizeof(copy->data->voldata),
			     hmp->mmsg, M_WAITOK | M_ZERO);
	hammer2_voldata_lock(hmp);
	copy->data->voldata = hmp->volsync;
	hammer2_voldata_unlock(hmp);

	return copy;
}

void
hammer2_chain_bulkdrop(hammer2_chain_t *copy)
{
	KKASSERT(copy->bref.type == HAMMER2_BREF_TYPE_VOLUME);
	KKASSERT(copy->data);
	kfree(copy->data, copy->hmp->mmsg);
	copy->data = NULL;
	hammer2_chain_drop(copy);
}

int
hammer2_chain_dirent_test(hammer2_chain_t *chain, const char *name,
			  size_t name_len)
{
	const hammer2_inode_data_t *ripdata;

	if (chain->bref.type == HAMMER2_BREF_TYPE_INODE) {
		ripdata = &chain->data->ipdata;
		if (ripdata->meta.name_len == name_len &&
		    bcmp(ripdata->filename, name, name_len) == 0) {
			return 1;
		}
	}
	if (chain->bref.type == HAMMER2_BREF_TYPE_DIRENT &&
	    chain->bref.embed.dirent.namlen == name_len) {
		if (name_len > sizeof(chain->bref.check.buf) &&
		    bcmp(chain->data->buf, name, name_len) == 0) {
			return 1;
		}
		if (name_len <= sizeof(chain->bref.check.buf) &&
		    bcmp(chain->bref.check.buf, name, name_len) == 0) {
			return 1;
		}
	}
	return 0;
}

void
hammer2_dump_chain(hammer2_chain_t *chain, int tab, int bi, int *countp,
		   char pfx, u_int flags)
{
	hammer2_chain_t *scan;
	hammer2_chain_t *parent;

	--*countp;
	if (*countp == 0) {
		kprintf("%*.*s...\n", tab, tab, "");
		return;
	}
	if (*countp < 0)
		return;
	kprintf("%*.*s%c-chain %p %s.%-3d %016jx %016jx/%-2d mir=%016jx\n",
		tab, tab, "", pfx, chain,
		hammer2_bref_type_str(chain->bref.type), bi,
		chain->bref.data_off, chain->bref.key, chain->bref.keybits,
		chain->bref.mirror_tid);

	kprintf("%*.*s      [%08x] (%s) refs=%d",
		tab, tab, "",
		chain->flags,
		((chain->bref.type == HAMMER2_BREF_TYPE_INODE &&
		chain->data) ?  (char *)chain->data->ipdata.filename : "?"),
		chain->refs);

	parent = chain->parent;
	if (parent)
		kprintf("\n%*.*s      p=%p [pflags %08x prefs %d]",
			tab, tab, "",
			parent, parent->flags, parent->refs);
	if (RB_EMPTY(&chain->core.rbtree)) {
		kprintf("\n");
	} else {
		int bi = 0;
		kprintf(" {\n");
		RB_FOREACH(scan, hammer2_chain_tree, &chain->core.rbtree) {
			if ((scan->flags & flags) || flags == (u_int)-1) {
				hammer2_dump_chain(scan, tab + 4, bi, countp,
						   'a', flags);
			}
			bi++;
		}
		if (chain->bref.type == HAMMER2_BREF_TYPE_INODE && chain->data)
			kprintf("%*.*s}(%s)\n", tab, tab, "",
				chain->data->ipdata.filename);
		else
			kprintf("%*.*s}\n", tab, tab, "");
	}
}
