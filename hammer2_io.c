#include "hammer2.h"

#define HAMMER2_DOP_READ	1
#define HAMMER2_DOP_NEW		2
#define HAMMER2_DOP_NEWNZ	3
#define HAMMER2_DOP_READQ	4

static void dio_write_stats_update(hammer2_io_t *dio, struct buf *bp);

static hammer2_io_t *hammer2_io_hash_lookup(hammer2_dev_t *hmp,
			hammer2_off_t pbase, uint64_t *refsp);
static hammer2_io_t *hammer2_io_hash_enter(hammer2_dev_t *hmp,
			hammer2_io_t *dio, uint64_t *refsp);
static void hammer2_io_hash_cleanup(hammer2_dev_t *hmp, int dio_limit);

void
hammer2_io_hash_init(hammer2_dev_t *hmp)
{
	hammer2_io_hash_t *hash;
	int i;

	for (i = 0; i < HAMMER2_IOHASH_SIZE; ++i) {
		hash = &hmp->iohash[i];
		hammer2_spin_init(&hash->spin, "h2iohash");
	}
}

#ifdef HAMMER2_IO_DEBUG

static __inline void
DIO_RECORD(hammer2_io_t *dio HAMMER2_IO_DEBUG_ARGS)
{
	int i;

	i = atomic_fetchadd_int(&dio->debug_index, 1) & HAMMER2_IO_DEBUG_MASK;

	dio->debug_file[i] = file;
	dio->debug_line[i] = line;
	dio->debug_refs[i] = dio->refs;
	dio->debug_td[i] = curthread;
}

#else

#define DIO_RECORD(dio)

#endif

static __inline
hammer2_io_t *
hammer2_io_alloc(hammer2_dev_t *hmp, hammer2_off_t data_off, uint8_t btype,
		 int createit, int *isgoodp)
{
	hammer2_io_t *dio;
	hammer2_io_t *xio;
	hammer2_off_t lbase;
	hammer2_off_t pbase;
	hammer2_off_t pmask;
	hammer2_volume_t *vol;
	uint64_t refs;
	int lsize;
	int psize;

	psize = HAMMER2_PBUFSIZE;
	pmask = ~(hammer2_off_t)(psize - 1);
	if ((int)(data_off & HAMMER2_OFF_MASK_RADIX))
		lsize = 1 << (int)(data_off & HAMMER2_OFF_MASK_RADIX);
	else
		lsize = 0;
	lbase = data_off & ~HAMMER2_OFF_MASK_RADIX;
	pbase = lbase & pmask;

	if (pbase == 0 || ((lbase + lsize - 1) & pmask) != pbase) {
		kprintf("Illegal: %016jx %016jx+%08x / %016jx\n",
			pbase, lbase, lsize, pmask);
	}
	KKASSERT(pbase != 0 && ((lbase + lsize - 1) & pmask) == pbase);
	*isgoodp = 0;

	dio = hammer2_io_hash_lookup(hmp, pbase, &refs);
	if (dio) {
		if (refs & HAMMER2_DIO_GOOD)
			*isgoodp = 1;
	} else if (createit) {
		refs = 0;
		vol = hammer2_get_volume(hmp, pbase);
		dio = kmalloc_obj(sizeof(*dio), hmp->mio, M_INTWAIT | M_ZERO);
		dio->hmp = hmp;
		dio->devvp = vol->dev->devvp;
		dio->dbase = vol->offset;
		KKASSERT((dio->dbase & HAMMER2_FREEMAP_LEVEL1_MASK) == 0);
		dio->pbase = pbase;
		dio->psize = psize;
		dio->btype = btype;
		dio->refs = refs + 1;
		dio->act = 5;
		xio = hammer2_io_hash_enter(hmp, dio, &refs);
		if (xio == NULL) {
			atomic_add_int(&hammer2_dio_count, 1);
		} else {
			if (refs & HAMMER2_DIO_GOOD)
				*isgoodp = 1;
			kfree_obj(dio, hmp->mio);
			dio = xio;
		}
	} else {
		return NULL;
	}
	dio->ticks = ticks;
	if (dio->act < 10)
		++dio->act;

	return dio;
}

hammer2_io_t *
_hammer2_io_getblk(hammer2_dev_t *hmp, int btype, off_t lbase,
		   int lsize, int op HAMMER2_IO_DEBUG_ARGS)
{
	hammer2_io_t *dio;
	hammer2_off_t dev_pbase;
	off_t peof;
	uint64_t orefs;
	uint64_t nrefs;
	int isgood;
	int error;
	int hce;
	int bflags;

	bflags = ((btype == HAMMER2_BREF_TYPE_DATA) ? B_NOTMETA : 0);
	bflags |= B_KVABIO;

	KKASSERT((1 << (int)(lbase & HAMMER2_OFF_MASK_RADIX)) == lsize);

	if (op == HAMMER2_DOP_READQ) {
		dio = hammer2_io_alloc(hmp, lbase, btype, 0, &isgood);
		if (dio == NULL)
			return NULL;
		op = HAMMER2_DOP_READ;
	} else {
		dio = hammer2_io_alloc(hmp, lbase, btype, 1, &isgood);
	}

	for (;;) {
		orefs = dio->refs;
		cpu_ccfence();

		if (orefs & HAMMER2_DIO_GOOD) {
			if (isgood == 0)
				cpu_mfence();
			bkvasync(dio->bp);

			switch(op) {
			case HAMMER2_DOP_NEW:
				bzero(hammer2_io_data(dio, lbase), lsize);
			case HAMMER2_DOP_NEWNZ:
				atomic_set_long(&dio->refs, HAMMER2_DIO_DIRTY);
				break;
			case HAMMER2_DOP_READ:
			default:
				break;
			}
			DIO_RECORD(dio HAMMER2_IO_DEBUG_CALL);
			return (dio);
		}

		if (orefs & HAMMER2_DIO_INPROG) {
			nrefs = orefs | HAMMER2_DIO_WAITING;
			tsleep_interlock(dio, 0);
			if (atomic_cmpset_64(&dio->refs, orefs, nrefs)) {
				tsleep(dio, PINTERLOCKED, "h2dio", hz);
			}
		} else {
			nrefs = orefs | HAMMER2_DIO_INPROG;
			if (atomic_cmpset_64(&dio->refs, orefs, nrefs)) {
				break;
			}
		}
	}

	KKASSERT(dio->bp == NULL);
	if (btype == HAMMER2_BREF_TYPE_DATA)
		hce = hammer2_cluster_data_read;
	else
		hce = hammer2_cluster_meta_read;

	error = 0;
	dev_pbase = dio->pbase - dio->dbase;
	if (dio->pbase == (lbase & ~HAMMER2_OFF_MASK_RADIX) &&
	    dio->psize == lsize) {
		switch(op) {
		case HAMMER2_DOP_NEW:
		case HAMMER2_DOP_NEWNZ:
			dio->bp = getblk(dio->devvp,
					 dev_pbase, dio->psize,
					 GETBLK_KVABIO, 0);
			if (op == HAMMER2_DOP_NEW) {
				bkvasync(dio->bp);
				bzero(dio->bp->b_data, dio->psize);
			}
			atomic_set_long(&dio->refs, HAMMER2_DIO_DIRTY);
			break;
		case HAMMER2_DOP_READ:
		default:
			KKASSERT(dio->bp == NULL);
			if (hce > 0) {
				peof = (dio->pbase + HAMMER2_SEGMASK64) &
				       ~HAMMER2_SEGMASK64;
				peof -= dio->dbase;
				error = cluster_readx(dio->devvp,
						     peof, dev_pbase,
						     dio->psize, bflags,
						     dio->psize,
						     HAMMER2_PBUFSIZE*hce,
						     &dio->bp);
			} else {
				error = breadnx(dio->devvp, dev_pbase,
						dio->psize, bflags,
					        NULL, NULL, 0, &dio->bp);
			}
			break;
		}
	} else {
		if (hce > 0) {
			peof = (dio->pbase + HAMMER2_SEGMASK64) &
			       ~HAMMER2_SEGMASK64;
			peof -= dio->dbase;
			error = cluster_readx(dio->devvp,
					      peof, dev_pbase, dio->psize,
					      bflags,
					      dio->psize, HAMMER2_PBUFSIZE*hce,
					      &dio->bp);
		} else {
			error = breadnx(dio->devvp, dev_pbase,
				        dio->psize, bflags,
					NULL, NULL, 0, &dio->bp);
		}
		if (dio->bp) {
			switch(op) {
			case HAMMER2_DOP_NEW:
				bkvasync(dio->bp);
				bzero(hammer2_io_data(dio, lbase), lsize);
			case HAMMER2_DOP_NEWNZ:
				atomic_set_long(&dio->refs, HAMMER2_DIO_DIRTY);
				break;
			case HAMMER2_DOP_READ:
			default:
				break;
			}

			switch(btype) {
			case HAMMER2_BREF_TYPE_DATA:
				dio->bp->b_flags |= B_NOTMETA;
				break;
			default:
				break;
			}
		}
	}

	if (dio->bp) {
		bkvasync(dio->bp);
		BUF_KERNPROC(dio->bp);
		dio->bp->b_flags &= ~B_AGE;
	}
	dio->error = error;

	for (;;) {
		orefs = dio->refs;
		cpu_ccfence();
		nrefs = orefs & ~(HAMMER2_DIO_INPROG | HAMMER2_DIO_WAITING);
		if (error == 0)
			nrefs |= HAMMER2_DIO_GOOD;
		if (atomic_cmpset_64(&dio->refs, orefs, nrefs)) {
			if (orefs & HAMMER2_DIO_WAITING)
				wakeup(dio);
			break;
		}
		cpu_pause();
	}

	DIO_RECORD(dio HAMMER2_IO_DEBUG_CALL);

	return dio;
}

void
_hammer2_io_putblk(hammer2_io_t **diop HAMMER2_IO_DEBUG_ARGS)
{
	hammer2_dev_t *hmp;
	hammer2_io_t *dio;
	struct buf *bp;
	off_t pbase;
	int psize;
	int dio_limit;
	uint64_t orefs;
	uint64_t nrefs;

	dio = *diop;
	*diop = NULL;
	hmp = dio->hmp;
	DIO_RECORD(dio HAMMER2_IO_DEBUG_CALL);

	KKASSERT((dio->refs & HAMMER2_DIO_MASK) != 0);

	for (;;) {
		orefs = dio->refs;
		cpu_ccfence();

		if ((orefs & HAMMER2_DIO_MASK) == 1 &&
		    (orefs & HAMMER2_DIO_INPROG) == 0) {
			nrefs = orefs - 1;
			nrefs &= ~(HAMMER2_DIO_GOOD | HAMMER2_DIO_DIRTY);
			nrefs |= HAMMER2_DIO_INPROG;
			if (atomic_cmpset_64(&dio->refs, orefs, nrefs))
				break;
		} else if ((orefs & HAMMER2_DIO_MASK) == 1) {
			nrefs = orefs | HAMMER2_DIO_WAITING;
			tsleep_interlock(dio, 0);
			if (atomic_cmpset_64(&dio->refs, orefs, nrefs)) {
				tsleep(dio, PINTERLOCKED, "h2dio", hz);
			}
		} else {
			nrefs = orefs - 1;
			if (atomic_cmpset_64(&dio->refs, orefs, nrefs))
				return;
		}
		cpu_pause();
	}

	pbase = dio->pbase;
	psize = dio->psize;
	bp = dio->bp;
	dio->bp = NULL;

	if ((orefs & HAMMER2_DIO_GOOD) && bp) {
		if (orefs & HAMMER2_DIO_DIRTY) {
			dio_write_stats_update(dio, bp);

			off_t peof;
			int hce;
			if (dio->refs & HAMMER2_DIO_FLUSH) {
				if ((hce = hammer2_cluster_write) != 0) {
					peof = (pbase + HAMMER2_SEGMASK64) &
					       ~HAMMER2_SEGMASK64;
					peof -= dio->dbase;
					bp->b_flags |= B_CLUSTEROK;
					cluster_write(bp, peof, psize, hce);
				} else {
					bp->b_flags &= ~B_CLUSTEROK;
					bawrite(bp);
				}
			} else {
				bp->b_flags &= ~B_CLUSTEROK;
				bdwrite(bp);
			}
		} else if (bp->b_flags & (B_ERROR | B_INVAL | B_RELBUF)) {
			brelse(bp);
		} else {
			bqrelse(bp);
		}
	} else if (bp) {
		brelse(bp);
	}

	hmp = dio->hmp;
	atomic_add_int(&hmp->iofree_count, 1);

	for (;;) {
		orefs = dio->refs;
		cpu_ccfence();
		nrefs = orefs & ~(HAMMER2_DIO_INPROG | HAMMER2_DIO_GOOD |
				  HAMMER2_DIO_WAITING | HAMMER2_DIO_FLUSH);
		if (atomic_cmpset_64(&dio->refs, orefs, nrefs)) {
			if (orefs & HAMMER2_DIO_WAITING)
				wakeup(dio);
			break;
		}
		cpu_pause();
	}

	dio_limit = hammer2_dio_limit;
	if (dio_limit < 256)
		dio_limit = 256;
	if (dio_limit > 1024*1024)
		dio_limit = 1024*1024;
	if (hmp->iofree_count > dio_limit)
		hammer2_io_hash_cleanup(hmp, dio_limit);
}

char *
hammer2_io_data(hammer2_io_t *dio, off_t lbase)
{
	struct buf *bp;
	int off;

	bp = dio->bp;
	KKASSERT(bp != NULL);
	bkvasync(bp);
	lbase -= dio->dbase;
	off = (lbase & ~HAMMER2_OFF_MASK_RADIX) - bp->b_loffset;
	KKASSERT(off >= 0 && off < bp->b_bufsize);
	return(bp->b_data + off);
}

int
hammer2_io_new(hammer2_dev_t *hmp, int btype, off_t lbase, int lsize,
	       hammer2_io_t **diop)
{
	*diop = hammer2_io_getblk(hmp, btype, lbase, lsize, HAMMER2_DOP_NEW);
	return ((*diop)->error);
}

int
hammer2_io_newnz(hammer2_dev_t *hmp, int btype, off_t lbase, int lsize,
		 hammer2_io_t **diop)
{
	*diop = hammer2_io_getblk(hmp, btype, lbase, lsize, HAMMER2_DOP_NEWNZ);
	return ((*diop)->error);
}

int
_hammer2_io_bread(hammer2_dev_t *hmp, int btype, off_t lbase, int lsize,
		hammer2_io_t **diop HAMMER2_IO_DEBUG_ARGS)
{
#ifdef HAMMER2_IO_DEBUG
	hammer2_io_t *dio;
#endif

	*diop = _hammer2_io_getblk(hmp, btype, lbase, lsize,
				   HAMMER2_DOP_READ HAMMER2_IO_DEBUG_CALL);
#ifdef HAMMER2_IO_DEBUG
	if ((dio = *diop) != NULL) {
#if 0
		int i = (dio->debug_index - 1) & HAMMER2_IO_DEBUG_MASK;
		dio->debug_data[i] = debug_data;
#endif
	}
#endif
	return ((*diop)->error);
}

hammer2_io_t *
_hammer2_io_getquick(hammer2_dev_t *hmp, off_t lbase,
		     int lsize HAMMER2_IO_DEBUG_ARGS)
{
	hammer2_io_t *dio;

	dio = _hammer2_io_getblk(hmp, 0, lbase, lsize,
				 HAMMER2_DOP_READQ HAMMER2_IO_DEBUG_CALL);
	return dio;
}

void
_hammer2_io_bawrite(hammer2_io_t **diop HAMMER2_IO_DEBUG_ARGS)
{
	atomic_set_64(&(*diop)->refs, HAMMER2_DIO_DIRTY |
				      HAMMER2_DIO_FLUSH);
	_hammer2_io_putblk(diop HAMMER2_IO_DEBUG_CALL);
}

void
_hammer2_io_bdwrite(hammer2_io_t **diop HAMMER2_IO_DEBUG_ARGS)
{
	atomic_set_64(&(*diop)->refs, HAMMER2_DIO_DIRTY);
	_hammer2_io_putblk(diop HAMMER2_IO_DEBUG_CALL);
}

int
_hammer2_io_bwrite(hammer2_io_t **diop HAMMER2_IO_DEBUG_ARGS)
{
	atomic_set_64(&(*diop)->refs, HAMMER2_DIO_DIRTY |
				      HAMMER2_DIO_FLUSH);
	_hammer2_io_putblk(diop HAMMER2_IO_DEBUG_CALL);
	return (0);
}

void
hammer2_io_setdirty(hammer2_io_t *dio)
{
	atomic_set_64(&dio->refs, HAMMER2_DIO_DIRTY);
}

void
hammer2_io_inval(hammer2_io_t *dio, hammer2_off_t data_off, u_int bytes)
{
}

void
_hammer2_io_brelse(hammer2_io_t **diop HAMMER2_IO_DEBUG_ARGS)
{
	_hammer2_io_putblk(diop HAMMER2_IO_DEBUG_CALL);
}

void
_hammer2_io_bqrelse(hammer2_io_t **diop HAMMER2_IO_DEBUG_ARGS)
{
	_hammer2_io_putblk(diop HAMMER2_IO_DEBUG_CALL);
}

void
hammer2_io_dedup_set(hammer2_dev_t *hmp, hammer2_blockref_t *bref)
{
	hammer2_io_t *dio;
	uint64_t mask;
	int lsize;
	int isgood;

	dio = hammer2_io_alloc(hmp, bref->data_off, bref->type, 1, &isgood);
	if ((int)(bref->data_off & HAMMER2_OFF_MASK_RADIX))
		lsize = 1 << (int)(bref->data_off & HAMMER2_OFF_MASK_RADIX);
	else
		lsize = 0;
	mask = hammer2_dedup_mask(dio, bref->data_off, lsize);
	atomic_clear_64(&dio->dedup_valid, mask);
	atomic_set_64(&dio->dedup_alloc, mask);
	hammer2_io_putblk(&dio);
}

void
hammer2_io_dedup_delete(hammer2_dev_t *hmp, uint8_t btype,
			hammer2_off_t data_off, u_int bytes)
{
	hammer2_io_t *dio;
	uint64_t mask;
	int isgood;

	if ((data_off & ~HAMMER2_OFF_MASK_RADIX) == 0)
		return;
	if (btype != HAMMER2_BREF_TYPE_DATA)
		return;
	dio = hammer2_io_alloc(hmp, data_off, btype, 0, &isgood);
	if (dio) {
		if (data_off < dio->pbase ||
		    (data_off & ~HAMMER2_OFF_MASK_RADIX) + bytes >
		    dio->pbase + dio->psize) {
			panic("hammer2_io_dedup_delete: DATAOFF BAD "
			      "%016jx/%d %016jx\n",
			      data_off, bytes, dio->pbase);
		}
		mask = hammer2_dedup_mask(dio, data_off, bytes);
		atomic_clear_64(&dio->dedup_alloc, mask);
		atomic_clear_64(&dio->dedup_valid, mask);
		hammer2_io_putblk(&dio);
	}
}

void
hammer2_io_dedup_assert(hammer2_dev_t *hmp, hammer2_off_t data_off, u_int bytes)
{
	hammer2_io_t *dio;
	int isgood;

	dio = hammer2_io_alloc(hmp, data_off, HAMMER2_BREF_TYPE_DATA,
			       0, &isgood);
	if (dio) {
		KASSERT((dio->dedup_alloc &
			  hammer2_dedup_mask(dio, data_off, bytes)) == 0,
			("hammer2_dedup_assert: %016jx/%d %016jx/%016jx",
			data_off,
			bytes,
			hammer2_dedup_mask(dio, data_off, bytes),
			dio->dedup_alloc));
		hammer2_io_putblk(&dio);
	}
}

static
void
dio_write_stats_update(hammer2_io_t *dio, struct buf *bp)
{
	if (bp->b_flags & B_DELWRI)
		return;
	hammer2_adjwritecounter(dio->btype, dio->psize);
}

void
hammer2_io_bkvasync(hammer2_io_t *dio)
{
	KKASSERT(dio->bp != NULL);
	bkvasync(dio->bp);
}

void
_hammer2_io_ref(hammer2_io_t *dio HAMMER2_IO_DEBUG_ARGS)
{
	DIO_RECORD(dio HAMMER2_IO_DEBUG_CALL);
	atomic_add_64(&dio->refs, 1);
}

static __inline hammer2_io_hash_t *
hammer2_io_hashv(hammer2_dev_t *hmp, hammer2_off_t pbase)
{
	int hv;

	hv = (int)pbase + (int)(pbase >> 16);
	return (&hmp->iohash[hv & HAMMER2_IOHASH_MASK]);
}

static hammer2_io_t *
hammer2_io_hash_lookup(hammer2_dev_t *hmp, hammer2_off_t pbase, uint64_t *refsp)
{
	hammer2_io_hash_t *hash;
	hammer2_io_t *dio;
	uint64_t refs;

	*refsp = 0;
	hash = hammer2_io_hashv(hmp, pbase);
	hammer2_spin_sh(&hash->spin);
	for (dio = hash->base; dio; dio = dio->next) {
		if (dio->pbase == pbase) {
			refs = atomic_fetchadd_64(&dio->refs, 1);
			if ((refs & HAMMER2_DIO_MASK) == 0)
				atomic_add_int(&dio->hmp->iofree_count, -1);
			*refsp = refs;
			break;
		}
	}
	hammer2_spin_unsh(&hash->spin);

	return dio;
}

static hammer2_io_t *
hammer2_io_hash_enter(hammer2_dev_t *hmp, hammer2_io_t *dio, uint64_t *refsp)
{
	hammer2_io_t *xio;
	hammer2_io_t **xiop;
	hammer2_io_hash_t *hash;
	uint64_t refs;

	*refsp = 0;
	hash = hammer2_io_hashv(hmp, dio->pbase);
	hammer2_spin_ex(&hash->spin);
	for (xiop = &hash->base; (xio = *xiop) != NULL; xiop = &xio->next) {
		if (xio->pbase == dio->pbase) {
			refs = atomic_fetchadd_64(&xio->refs, 1);
			if ((refs & HAMMER2_DIO_MASK) == 0)
				atomic_add_int(&xio->hmp->iofree_count, -1);
			*refsp = refs;
			goto done;
		}
	}
	dio->next = NULL;
	*xiop = dio;
done:
	hammer2_spin_unex(&hash->spin);

	return xio;
}

static void
hammer2_io_hash_cleanup(hammer2_dev_t *hmp, int dio_limit)
{
	hammer2_io_hash_t *hash;
	hammer2_io_t *dio;
	hammer2_io_t **diop;
	hammer2_io_t **cleanapp;
	hammer2_io_t *cleanbase;
	int count;
	int maxscan;
	int i;

	count = hmp->iofree_count - dio_limit + 32;
	if (count <= 0)
		return;
	cleanbase = NULL;
	cleanapp = &cleanbase;

	i = hmp->io_iterator++;
	maxscan = HAMMER2_IOHASH_SIZE;
	while (count > 0 && maxscan--) {
		hash = &hmp->iohash[i & HAMMER2_IOHASH_MASK];
		hammer2_spin_ex(&hash->spin);
		diop = &hash->base;
		while ((dio = *diop) != NULL) {
			if ((dio->refs & (HAMMER2_DIO_MASK |
					  HAMMER2_DIO_INPROG)) != 0)
			{
				diop = &dio->next;
				continue;
			}
			if (dio->act > 0) {
				int act;

				act = dio->act - (ticks - dio->ticks) / hz - 1;
				dio->act = (act < 0) ? 0 : act;
			}
			if (dio->act) {
				diop = &dio->next;
				continue;
			}
			KKASSERT(dio->bp == NULL);
			*diop = dio->next;
			dio->next = NULL;
			*cleanapp = dio;
			cleanapp = &dio->next;
			--count;
			atomic_add_int(&hmp->iofree_count, -1);
		}
		hammer2_spin_unex(&hash->spin);
		i = hmp->io_iterator++;
	}

	while ((dio = cleanbase) != NULL) {
		cleanbase = dio->next;
		dio->next = NULL;
		KKASSERT(dio->bp == NULL &&
		    (dio->refs & (HAMMER2_DIO_MASK |
				  HAMMER2_DIO_INPROG)) == 0);
		if (dio->refs & HAMMER2_DIO_DIRTY) {
			kprintf("hammer2_io_cleanup: Dirty buffer "
				"%016jx/%d (bp=%p)\n",
				dio->pbase, dio->psize, dio->bp);
		}
		kfree_obj(dio, hmp->mio);
		atomic_add_int(&hammer2_dio_count, -1);
	}
}

void
hammer2_io_hash_cleanup_all(hammer2_dev_t *hmp)
{
	hammer2_io_hash_t *hash;
	hammer2_io_t *dio;
	int i;

	for (i = 0; i < HAMMER2_IOHASH_SIZE; ++i) {
		hash = &hmp->iohash[i];

		while ((dio = hash->base) != NULL) {
			hash->base = dio->next;
			dio->next = NULL;
			KKASSERT(dio->bp == NULL &&
			    (dio->refs & (HAMMER2_DIO_MASK |
					  HAMMER2_DIO_INPROG)) == 0);
			if (dio->refs & HAMMER2_DIO_DIRTY) {
				kprintf("hammer2_io_cleanup: Dirty buffer "
					"%016jx/%d (bp=%p)\n",
					dio->pbase, dio->psize, dio->bp);
			}
			kfree_obj(dio, hmp->mio);
			atomic_add_int(&hammer2_dio_count, -1);
			atomic_add_int(&hmp->iofree_count, -1);
		}
	}
}
