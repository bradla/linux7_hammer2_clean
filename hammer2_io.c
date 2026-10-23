#include "hammer2.h"

#define HAMMER2_DOP_READ	1
#define HAMMER2_DOP_NEW		2
#define HAMMER2_DOP_NEWNZ	3
#define HAMMER2_DOP_READQ	4

static hammer2_io_t *hammer2_io_hash_lookup(hammer2_dev_t *, hammer2_off_t,
    uint64_t *);
static hammer2_io_t *hammer2_io_hash_enter(hammer2_dev_t *, hammer2_io_t *,
    uint64_t *);
static void hammer2_io_hash_cleanup(hammer2_dev_t *, int);

static __inline void
hammer2_assert_io_refs(hammer2_io_t *dio)
{
	KKASSERT(dio);
	hammer2_mtx_assert_ex(&dio->lock);
	KKASSERT((dio->refs & HAMMER2_DIO_MASK) != 0);
}

void
hammer2_io_hash_init(hammer2_dev_t *hmp)
{
	hammer2_io_hash_t *hash;
	int i;

	for (i = 0; i < HAMMER2_IOHASH_SIZE; ++i) {
		hash = &hmp->iohash[i];
		hammer2_spin_init(&hash->spin, "h2dev_io");
	}
}

void
hammer2_io_hash_destroy(hammer2_dev_t *hmp)
{
	hammer2_io_hash_t *hash;
	int i;

	for (i = 0; i < HAMMER2_IOHASH_SIZE; ++i) {
		hash = &hmp->iohash[i];
		hammer2_spin_destroy(&hash->spin);
	}
}

static hammer2_io_t *
hammer2_io_alloc(hammer2_dev_t *hmp, hammer2_off_t data_off, uint8_t btype,
    int createit)
{
	hammer2_volume_t *vol;
	hammer2_io_t *dio, *xio;
	hammer2_off_t lbase, pbase, pmask;
	int lsize, psize;

	hammer2_mtx_assert_ex(&hmp->iohash_lock);

	psize = HAMMER2_PBUFSIZE;
	pmask = ~(hammer2_off_t)(psize - 1);
	if ((int)(data_off & HAMMER2_OFF_MASK_RADIX))
		lsize = 1 << (int)(data_off & HAMMER2_OFF_MASK_RADIX);
	else
		lsize = 0;
	lbase = data_off & ~HAMMER2_OFF_MASK_RADIX;
	pbase = lbase & pmask;

	if (pbase == 0 || ((lbase + lsize - 1) & pmask) != pbase)
		hpanic("illegal base: %016llx %016llx+%08x / %016llx",
		    (long long)pbase, (long long)lbase, lsize,
		    (long long)pmask);

	dio = hammer2_io_hash_lookup(hmp, pbase, NULL);
	if (dio) {

	} else if (createit) {
		vol = hammer2_get_volume(hmp, pbase);
		dio = hmalloc(sizeof(*dio), M_HAMMER2, M_WAITOK | M_ZERO);
		dio->hmp = hmp;
		dio->bdev = vol->dev->bdev;
		dio->dbase = vol->offset;
		KKASSERT((dio->dbase & HAMMER2_FREEMAP_LEVEL1_MASK) == 0);
		dio->pbase = pbase;
		dio->psize = psize;
		dio->btype = btype;
		dio->refs = 1;
		dio->act = 5;
		hammer2_mtx_init(&dio->lock, "h2io");
		hammer2_mtx_ex(&dio->lock);
		xio = hammer2_io_hash_enter(hmp, dio, NULL);
		if (xio == NULL) {
			atomic_add_int(&hammer2_count_dio_allocated, 1);
		} else {
			hammer2_mtx_unlock(&dio->lock);
			hammer2_mtx_destroy(&dio->lock);
			hfree(dio, M_HAMMER2, sizeof(*dio));
			dio = xio;
			hammer2_mtx_ex(&dio->lock);
		}
	} else {
		return (NULL);
	}

	dio->ticks = getticks();
	if (dio->act < 10)
		++dio->act;

	hammer2_assert_io_refs(dio);

	return (dio);
}

int
hammer2_dev_bread(struct block_device *bdev, loff_t byteoff, void *buf,
    int bytes)
{
	struct buffer_head *bh;
	sector_t blk = (sector_t)(byteoff >> PAGE_SHIFT);
	int i, n = bytes >> PAGE_SHIFT;

	for (i = 0; i < n; ++i) {
		bh = __bread(bdev, blk + i, PAGE_SIZE);
		if (!bh)
			return (-EIO);
		memcpy((char *)buf + ((size_t)i << PAGE_SHIFT), bh->b_data,
		    PAGE_SIZE);
		brelse(bh);
	}
	return (0);
}

int
hammer2_dev_bwrite(struct block_device *bdev, loff_t byteoff, const void *buf,
    int bytes, int sync)
{

	struct buffer_head *bhs[HAMMER2_PBUFSIZE >> PAGE_SHIFT];
	sector_t blk = (sector_t)(byteoff >> PAGE_SHIFT);
	int i, n = bytes >> PAGE_SHIFT;
	int error = 0;

	if (n > ARRAY_SIZE(bhs))
		return -EINVAL;

	for (i = 0; i < n; ++i) {

		struct buffer_head *bh = __getblk(bdev, blk + i, PAGE_SIZE);
		if (!bh) {
			error = -EIO;
			n = i;
			break;
		}
		lock_buffer(bh);
		memcpy(bh->b_data, (const char *)buf + ((size_t)i << PAGE_SHIFT),
		    PAGE_SIZE);
		set_buffer_uptodate(bh);
		mark_buffer_dirty(bh);
		unlock_buffer(bh);
		bhs[i] = bh;

	}
	if (sync) {
		for (i = 0; i < n; ++i)
			write_dirty_buffer(bhs[i], REQ_SYNC);
		for (i = 0; i < n; ++i) {
			wait_on_buffer(bhs[i]);
			if (!buffer_uptodate(bhs[i]))
				error = -EIO;
		}
	}
	for (i = 0; i < n; ++i)
		brelse(bhs[i]);
	return error;

}

static int
hammer2_bread(hammer2_dev_t *hmp, hammer2_io_t *dio, daddr_t lblkno, int hce)
{
	int error;

	(void)lblkno;
	(void)hce;

	if (dio->data == NULL) {
		dio->data = kvmalloc(dio->psize, GFP_KERNEL);
		if (dio->data == NULL)
			return (-ENOMEM);
	}
	error = hammer2_dev_bread(dio->bdev, dio->pbase - dio->dbase, dio->data,
	    dio->psize);
	hammer2_inc_iostat(&hmp->iostat_read, dio->btype, dio->psize);

	return (error);
}

hammer2_io_t *
hammer2_io_getblk(hammer2_dev_t *hmp, int btype, hammer2_off_t lbase, int lsize,
    int op)
{
	hammer2_io_t *dio;
	int error, hce;

	KKASSERT((1 << (int)(lbase & HAMMER2_OFF_MASK_RADIX)) == lsize);

	hammer2_mtx_ex(&hmp->iohash_lock);
	if (op == HAMMER2_DOP_READQ) {
		dio = hammer2_io_alloc(hmp, lbase, btype, 0);
		if (dio == NULL) {

			hammer2_mtx_unlock(&hmp->iohash_lock);
			return (NULL);
		}
		op = HAMMER2_DOP_READ;
	} else {
		dio = hammer2_io_alloc(hmp, lbase, btype, 1);
	}
	KKASSERT(dio);
	hammer2_assert_io_refs(dio);
	hammer2_mtx_unlock(&hmp->iohash_lock);

	if (dio->refs & HAMMER2_DIO_GOOD) {
		switch (op) {
		case HAMMER2_DOP_NEW:
			bzero(hammer2_io_data(dio, lbase), lsize);

		case HAMMER2_DOP_NEWNZ:
			atomic_set_32(&dio->refs, HAMMER2_DIO_DIRTY);
			break;
		default:
			break;
		}
		hammer2_mtx_unlock(&dio->lock);
		return (dio);
	}

	if (btype == HAMMER2_BREF_TYPE_DATA)
		hce = hammer2_cluster_data_read;
	else
		hce = hammer2_cluster_meta_read;

	error = 0;

	if (dio->pbase == (lbase & ~HAMMER2_OFF_MASK_RADIX) &&
	    dio->psize == lsize) {
		switch (op) {
		case HAMMER2_DOP_NEW:
		case HAMMER2_DOP_NEWNZ:

			dio->data = kvmalloc(dio->psize, GFP_KERNEL);
			if (dio->data == NULL) {
				error = -ENOMEM;
				break;
			}
			if (op == HAMMER2_DOP_NEW)
				bzero(dio->data, dio->psize);
			atomic_set_32(&dio->refs, HAMMER2_DIO_DIRTY);
			break;
		default:
			error = hammer2_bread(hmp, dio, 0, hce);
			break;
		}
	} else {
		error = hammer2_bread(hmp, dio, 0, hce);
		if (dio->data && error == 0) {
			switch (op) {
			case HAMMER2_DOP_NEW:
				bzero(hammer2_io_data(dio, lbase), lsize);

			case HAMMER2_DOP_NEWNZ:
				atomic_set_32(&dio->refs, HAMMER2_DIO_DIRTY);
				break;
			default:
				break;
			}
		}
	}

	dio->error = error;
	if (error == 0)
		atomic_set_32(&dio->refs, HAMMER2_DIO_GOOD);
	else
		atomic_add_int(&hmp->iofail_count, 1);

	hammer2_mtx_unlock(&dio->lock);

	return (dio);
}

void
hammer2_io_putblk(hammer2_io_t **diop)
{
	hammer2_dev_t *hmp;
	hammer2_io_t *dio;
	hammer2_off_t peof;
	struct buffer_head *bp;
	uint64_t orefs;
	int dio_limit, hce;

	dio = *diop;

	if (dio == NULL)
		return;
	*diop = NULL;

	hammer2_mtx_ex(&dio->lock);
	if ((dio->refs & HAMMER2_DIO_MASK) == 0) {
		hammer2_mtx_unlock(&dio->lock);
		return;
	}
	hammer2_assert_io_refs(dio);

	for (;;) {
		orefs = dio->refs;
		KKASSERT((orefs & HAMMER2_DIO_MASK) != 0);
		if ((orefs & HAMMER2_DIO_MASK) == 1) {

			if (atomic_cmpset_32(&dio->refs, orefs,
			    (orefs - 1) & ~(HAMMER2_DIO_DIRTY |
					    HAMMER2_DIO_FLUSH)))
				break;
		} else {
			if (atomic_cmpset_32(&dio->refs, orefs, orefs - 1)) {
				hammer2_mtx_unlock(&dio->lock);
				return;
			}
		}
	}

	hmp = dio->hmp;
	bp = dio->bh;
	dio->bh = NULL;

	(void)peof;
	(void)hce;
	(void)bp;
	if ((orefs & HAMMER2_DIO_GOOD) && dio->data) {
		if (orefs & HAMMER2_DIO_DIRTY) {
			hammer2_dev_bwrite(dio->bdev, dio->pbase - dio->dbase,
			    dio->data, dio->psize,
			    (orefs & HAMMER2_DIO_FLUSH) ? 1 : 0);
			hammer2_inc_iostat(&hmp->iostat_write, dio->btype,
			    dio->psize);
		}
	}

	if (dio->data == NULL)
		atomic_clear_32(&dio->refs, HAMMER2_DIO_GOOD);

	atomic_add_int(&hmp->iofree_count, 1);

	hammer2_mtx_unlock(&dio->lock);

	hammer2_mtx_ex(&hmp->iohash_lock);
	dio_limit = hammer2_dio_limit;
	if (dio_limit < 256)
		dio_limit = 256;
	if (dio_limit > 1024*1024)
		dio_limit = 1024*1024;
	if (hmp->iofree_count > dio_limit)
		hammer2_io_hash_cleanup(hmp, dio_limit);
	hammer2_mtx_unlock(&hmp->iohash_lock);
}

char *
hammer2_io_data(hammer2_io_t *dio, hammer2_off_t lbase)
{
	int off;

	KASSERTMSG(dio->data != NULL, "NULL dio buf");

	lbase -= dio->dbase;
	off = (lbase & ~HAMMER2_OFF_MASK_RADIX) -
	    (hammer2_off_t)(dio->pbase - dio->dbase);

	KASSERTMSG(off >= 0, "bad offset not 0x%x >= 0x%x", off, 0);
	KASSERTMSG(off < dio->psize, "bad offset not 0x%x < 0x%x",
	    off, dio->psize);

	return (dio->data + off);
}

int
hammer2_io_new(hammer2_dev_t *hmp, int btype, hammer2_off_t lbase, int lsize,
    hammer2_io_t **diop)
{
	*diop = hammer2_io_getblk(hmp, btype, lbase, lsize, HAMMER2_DOP_NEW);
	return ((*diop)->error);
}

int
hammer2_io_newnz(hammer2_dev_t *hmp, int btype, hammer2_off_t lbase, int lsize,
    hammer2_io_t **diop)
{
	*diop = hammer2_io_getblk(hmp, btype, lbase, lsize, HAMMER2_DOP_NEWNZ);
	return ((*diop)->error);
}

int
hammer2_io_bread(hammer2_dev_t *hmp, int btype, hammer2_off_t lbase, int lsize,
    hammer2_io_t **diop)
{
	*diop = hammer2_io_getblk(hmp, btype, lbase, lsize, HAMMER2_DOP_READ);
	return ((*diop)->error);
}

hammer2_io_t *
hammer2_io_getquick(hammer2_dev_t *hmp, off_t lbase, int lsize)
{
	return (hammer2_io_getblk(hmp, 0, lbase, lsize, HAMMER2_DOP_READQ));
}

void
hammer2_io_bawrite(hammer2_io_t **diop)
{
	atomic_set_32(&(*diop)->refs, HAMMER2_DIO_DIRTY | HAMMER2_DIO_FLUSH);
	hammer2_io_putblk(diop);
}

void
hammer2_io_bdwrite(hammer2_io_t **diop)
{
	atomic_set_32(&(*diop)->refs, HAMMER2_DIO_DIRTY);
	hammer2_io_putblk(diop);
}

int
hammer2_io_bwrite(hammer2_io_t **diop)
{
	atomic_set_32(&(*diop)->refs, HAMMER2_DIO_DIRTY | HAMMER2_DIO_FLUSH);
	hammer2_io_putblk(diop);

	return (0);
}

void
hammer2_io_setdirty(hammer2_io_t *dio)
{
	atomic_set_32(&dio->refs, HAMMER2_DIO_DIRTY);
}

void
hammer2_io_brelse(hammer2_io_t **diop)
{
	hammer2_io_putblk(diop);
}

void
hammer2_io_bqrelse(hammer2_io_t **diop)
{
	hammer2_io_putblk(diop);
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

	hammer2_mtx_assert_ex(&hmp->iohash_lock);

	if (refsp)
		*refsp = 0;

	hash = hammer2_io_hashv(hmp, pbase);

	for (dio = hash->base; dio; dio = dio->next) {
		if (dio->pbase == pbase) {
			hammer2_mtx_ex(&dio->lock);

			refs = (uint32_t)atomic_fetchadd_32(&dio->refs, 1);
			if ((refs & HAMMER2_DIO_MASK) == 0)
				atomic_add_int(&dio->hmp->iofree_count, -1);
			if (refsp)
				*refsp = refs;
			break;
		}
	}

	if (dio)
		hammer2_assert_io_refs(dio);
	return (dio);
}

static hammer2_io_t *
hammer2_io_hash_enter(hammer2_dev_t *hmp, hammer2_io_t *dio, uint64_t *refsp)
{
	hammer2_io_hash_t *hash;
	hammer2_io_t *xio, **xiop;
	uint64_t refs;

	hammer2_mtx_assert_ex(&hmp->iohash_lock);
	hammer2_assert_io_refs(dio);

	if (refsp)
		*refsp = 0;

	hash = hammer2_io_hashv(hmp, dio->pbase);

	for (xiop = &hash->base; (xio = *xiop) != NULL; xiop = &xio->next) {
		if (xio->pbase == dio->pbase) {
			refs = xio->refs++;
			if ((refs & HAMMER2_DIO_MASK) == 0)
				atomic_add_int(&xio->hmp->iofree_count, -1);
			if (refsp)
				*refsp = refs;
			goto done;
		}
	}
	dio->next = NULL;
	*xiop = dio;
done:

	return (xio);
}

static void
hammer2_io_hash_cleanup(hammer2_dev_t *hmp, int dio_limit)
{
	hammer2_io_hash_t *hash;
	hammer2_io_t *dio, **diop, *cleanbase, **cleanapp;
	int count, maxscan, act, i;

	hammer2_mtx_assert_ex(&hmp->iohash_lock);

	count = hmp->iofree_count - dio_limit + 32;
	if (count <= 0)
		return;

	cleanbase = NULL;
	cleanapp = &cleanbase;
	i = hmp->io_iterator++;
	maxscan = HAMMER2_IOHASH_SIZE;

	while (count > 0 && maxscan--) {
		hash = &hmp->iohash[i & HAMMER2_IOHASH_MASK];

		diop = &hash->base;
		while ((dio = *diop) != NULL) {
			if ((dio->refs & HAMMER2_DIO_MASK) != 0) {
				diop = &dio->next;
				continue;
			}
			if (dio->act > 0) {
				act = dio->act - (getticks() - dio->ticks) / hz - 1;
				dio->act = (act < 0) ? 0 : act;
			}
			if (dio->act) {
				diop = &dio->next;
				continue;
			}
			KKASSERT(dio->bh == NULL);
			*diop = dio->next;
			dio->next = NULL;
			*cleanapp = dio;
			cleanapp = &dio->next;
			--count;

			atomic_add_int(&hammer2_count_dio_allocated, -1);
			atomic_add_int(&hmp->iofree_count, -1);
		}

		i = hmp->io_iterator++;
	}

	while ((dio = cleanbase) != NULL) {
		cleanbase = dio->next;
		dio->next = NULL;
		KKASSERT(dio->bh == NULL &&
		    (dio->refs & HAMMER2_DIO_MASK) == 0);
		if (dio->refs & HAMMER2_DIO_DIRTY)
			hprintf("dirty buffer %016llx/%d\n",
			    (long long)dio->pbase, dio->psize);
		if (dio->data) {
			kvfree(dio->data);
			dio->data = NULL;
		}
		hammer2_mtx_destroy(&dio->lock);
		hfree(dio, M_HAMMER2, sizeof(*dio));
	}
}

void
hammer2_io_hash_cleanup_all(hammer2_dev_t *hmp)
{
	hammer2_io_hash_t *hash;
	hammer2_io_t *dio;
	int i;

	hammer2_mtx_assert_ex(&hmp->iohash_lock);

	for (i = 0; i < HAMMER2_IOHASH_SIZE; ++i) {
		hash = &hmp->iohash[i];
		while ((dio = hash->base) != NULL) {
			hash->base = dio->next;
			dio->next = NULL;
			KKASSERT(dio->bh == NULL &&
			    (dio->refs & HAMMER2_DIO_MASK) == 0);
			if (dio->refs & HAMMER2_DIO_DIRTY)
				hprintf("dirty buffer %016llx/%d\n",
				    (long long)dio->pbase, dio->psize);
			if (dio->data) {
				kvfree(dio->data);
				dio->data = NULL;
			}
			hammer2_mtx_destroy(&dio->lock);
			hfree(dio, M_HAMMER2, sizeof(*dio));
			atomic_add_int(&hammer2_count_dio_allocated, -1);
			atomic_add_int(&hmp->iofree_count, -1);
		}
	}
}

#define HAMMER2_DEDUP_FRAG	(HAMMER2_PBUFSIZE / 64)
#define HAMMER2_DEDUP_FRAGRADIX	(HAMMER2_PBUFRADIX - 6)

uint64_t
hammer2_dedup_mask(hammer2_io_t *dio, hammer2_off_t data_off, u_int bytes)
{
	int bbeg, bits;
	uint64_t mask;

	bbeg = (int)((data_off & ~HAMMER2_OFF_MASK_RADIX) - dio->pbase) >>
	    HAMMER2_DEDUP_FRAGRADIX;
	bits = (int)((bytes + (HAMMER2_DEDUP_FRAG - 1)) >>
	    HAMMER2_DEDUP_FRAGRADIX);

	if (bbeg + bits == 64)
		mask = (uint64_t)-1;
	else
		mask = ((uint64_t)1 << (bbeg + bits)) - 1;
	mask &= ~(((uint64_t)1 << bbeg) - 1);

	return (mask);
}

void
hammer2_io_dedup_set(hammer2_dev_t *hmp, hammer2_blockref_t *bref)
{
	hammer2_io_t *dio;
	uint64_t mask;
	int lsize;

	hammer2_mtx_ex(&hmp->iohash_lock);
	dio = hammer2_io_alloc(hmp, bref->data_off, bref->type, 1);
	KKASSERT(dio);
	hammer2_assert_io_refs(dio);
	hammer2_mtx_unlock(&hmp->iohash_lock);

	if ((int)(bref->data_off & HAMMER2_OFF_MASK_RADIX))
		lsize = 1 << (int)(bref->data_off & HAMMER2_OFF_MASK_RADIX);
	else
		lsize = 0;

	mask = hammer2_dedup_mask(dio, bref->data_off, lsize);
	dio->dedup_valid &= ~mask;
	dio->dedup_alloc |= mask;

	hammer2_mtx_unlock(&dio->lock);
	hammer2_io_putblk(&dio);
}

void
hammer2_io_dedup_delete(hammer2_dev_t *hmp, uint8_t btype,
    hammer2_off_t data_off, unsigned int bytes)
{
	hammer2_io_t *dio;
	uint64_t mask;

	if ((data_off & ~HAMMER2_OFF_MASK_RADIX) == 0)
		return;
	if (btype != HAMMER2_BREF_TYPE_DATA)
		return;

	hammer2_mtx_ex(&hmp->iohash_lock);
	dio = hammer2_io_alloc(hmp, data_off, btype, 0);
	if (dio) {
		hammer2_assert_io_refs(dio);
		hammer2_mtx_unlock(&hmp->iohash_lock);

		if (data_off < (hammer2_off_t)dio->pbase ||
		    (data_off & ~HAMMER2_OFF_MASK_RADIX) +
		    (hammer2_off_t)bytes >
		    (hammer2_off_t)dio->pbase + dio->psize)
			hpanic("bad data_off %016llx/%d %016llx",
			    (long long)data_off, bytes, (long long)dio->pbase);

		mask = hammer2_dedup_mask(dio, data_off, bytes);
		dio->dedup_alloc &= ~mask;
		dio->dedup_valid &= ~mask;

		hammer2_mtx_unlock(&dio->lock);
		hammer2_io_putblk(&dio);
	} else {
		hammer2_mtx_unlock(&hmp->iohash_lock);
	}
}

void
hammer2_io_dedup_assert(hammer2_dev_t *hmp, hammer2_off_t data_off,
    unsigned int bytes)
{
	hammer2_io_t *dio;

	hammer2_mtx_ex(&hmp->iohash_lock);
	dio = hammer2_io_alloc(hmp, data_off, HAMMER2_BREF_TYPE_DATA, 0);
	if (dio) {
		hammer2_assert_io_refs(dio);
		hammer2_mtx_unlock(&hmp->iohash_lock);

		KASSERTMSG((dio->dedup_alloc &
		    hammer2_dedup_mask(dio, data_off, bytes)) == 0,
		    "%016llx/%d %016llx/%016llx",
		    (long long)data_off, bytes,
		    (long long)hammer2_dedup_mask(dio, data_off, bytes),
		    (long long)dio->dedup_alloc);

		hammer2_mtx_unlock(&dio->lock);
		hammer2_io_putblk(&dio);
	} else {
		hammer2_mtx_unlock(&hmp->iohash_lock);
	}
}
