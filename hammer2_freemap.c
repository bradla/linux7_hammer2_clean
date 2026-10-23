#include "hammer2.h"

struct hammer2_fiterate {
	hammer2_off_t	bpref;
	hammer2_off_t	bnext;
	int		loops;
	int		relaxed;
};

typedef struct hammer2_fiterate hammer2_fiterate_t;

static int hammer2_freemap_try_alloc(hammer2_chain_t **, hammer2_blockref_t *,
    int, hammer2_fiterate_t *, hammer2_tid_t);
static void hammer2_freemap_init(hammer2_dev_t *, hammer2_key_t,
    hammer2_chain_t *);
static int hammer2_bmap_alloc(hammer2_dev_t *, hammer2_bmap_data_t *, uint16_t,
    int, int, int, hammer2_key_t *);
static int hammer2_freemap_iterate(hammer2_chain_t **, hammer2_chain_t **,
    hammer2_fiterate_t *);

static int
hammer2_freemap_reserve(hammer2_chain_t *chain, int radix)
{
	hammer2_blockref_t *bref = &chain->bref;
	hammer2_off_t off;
	int index, index_inc;
	size_t bytes;

	bytes = (size_t)1 << radix;

	if ((bref->data_off & ~HAMMER2_OFF_MASK_RADIX) == 0) {
		index = 0;
	} else {
		off = bref->data_off & ~HAMMER2_OFF_MASK_RADIX &
		    HAMMER2_SEGMASK;
		off = off / HAMMER2_PBUFSIZE;
		KKASSERT(off >= HAMMER2_ZONE_FREEMAP_00 &&
		    off < HAMMER2_ZONE_FREEMAP_END);
		index = (int)(off - HAMMER2_ZONE_FREEMAP_00) /
		    HAMMER2_ZONE_FREEMAP_INC;
		KKASSERT(index >= 0 && index < HAMMER2_NFREEMAPS);
		if (++index == HAMMER2_NFREEMAPS)
			index = 0;
	}

	index_inc = index * HAMMER2_ZONE_FREEMAP_INC;

	switch (bref->keybits) {

	case HAMMER2_FREEMAP_LEVEL5_RADIX:
		KKASSERT(bref->type == HAMMER2_BREF_TYPE_FREEMAP_NODE);
		KKASSERT(bytes == HAMMER2_FREEMAP_LEVELN_PSIZE);
		off = H2FMBASE(bref->key, HAMMER2_FREEMAP_LEVEL5_RADIX) +
		    (index_inc + HAMMER2_ZONE_FREEMAP_00 +
		    HAMMER2_ZONEFM_LEVEL5) * HAMMER2_PBUFSIZE;
		break;
	case HAMMER2_FREEMAP_LEVEL4_RADIX:
		KKASSERT(bref->type == HAMMER2_BREF_TYPE_FREEMAP_NODE);
		KKASSERT(bytes == HAMMER2_FREEMAP_LEVELN_PSIZE);
		off = H2FMBASE(bref->key, HAMMER2_FREEMAP_LEVEL4_RADIX) +
		    (index_inc + HAMMER2_ZONE_FREEMAP_00 +
		    HAMMER2_ZONEFM_LEVEL4) * HAMMER2_PBUFSIZE;
		break;
	case HAMMER2_FREEMAP_LEVEL3_RADIX:
		KKASSERT(bref->type == HAMMER2_BREF_TYPE_FREEMAP_NODE);
		KKASSERT(bytes == HAMMER2_FREEMAP_LEVELN_PSIZE);
		off = H2FMBASE(bref->key, HAMMER2_FREEMAP_LEVEL3_RADIX) +
		    (index_inc + HAMMER2_ZONE_FREEMAP_00 +
		    HAMMER2_ZONEFM_LEVEL3) * HAMMER2_PBUFSIZE;
		break;
	case HAMMER2_FREEMAP_LEVEL2_RADIX:
		KKASSERT(bref->type == HAMMER2_BREF_TYPE_FREEMAP_NODE);
		KKASSERT(bytes == HAMMER2_FREEMAP_LEVELN_PSIZE);
		off = H2FMBASE(bref->key, HAMMER2_FREEMAP_LEVEL2_RADIX) +
		    (index_inc + HAMMER2_ZONE_FREEMAP_00 +
		    HAMMER2_ZONEFM_LEVEL2) * HAMMER2_PBUFSIZE;
		break;
	case HAMMER2_FREEMAP_LEVEL1_RADIX:
		KKASSERT(bref->type == HAMMER2_BREF_TYPE_FREEMAP_LEAF);
		KKASSERT(bytes == HAMMER2_FREEMAP_LEVELN_PSIZE);
		off = H2FMBASE(bref->key, HAMMER2_FREEMAP_LEVEL1_RADIX) +
		    (index_inc + HAMMER2_ZONE_FREEMAP_00 +
		    HAMMER2_ZONEFM_LEVEL1) * HAMMER2_PBUFSIZE;
		break;
	default:
		hpanic("bad radix %d", bref->keybits);
		break;
	}
	bref->data_off = off | radix;

	return (0);
}

int
hammer2_freemap_alloc(hammer2_chain_t *chain, size_t bytes)
{
	hammer2_dev_t *hmp = chain->hmp;
	hammer2_blockref_t *bref = &chain->bref;
	hammer2_chain_t *parent;
	hammer2_tid_t mtid;
	hammer2_fiterate_t iter;
	int radix, error;
	unsigned int hindex;

	if (bytes == 0) {
		bref->data_off = 0;
		return (0);
	}

	KKASSERT(hmp->spmp);
	mtid = hammer2_trans_sub(hmp->spmp);

	radix = hammer2_getradix(bytes);
	KKASSERT((size_t)1 << radix == bytes);

	if (bref->type == HAMMER2_BREF_TYPE_FREEMAP_NODE ||
	    bref->type == HAMMER2_BREF_TYPE_FREEMAP_LEAF) {

		return (hammer2_freemap_reserve(chain, radix));
	}

	KKASSERT(bytes >= HAMMER2_ALLOC_MIN && bytes <= HAMMER2_ALLOC_MAX);

	hindex = HAMMER2_PBUFRADIX - HAMMER2_LBUFRADIX;
	KKASSERT(hindex < HAMMER2_FREEMAP_HEUR_NRADIX);
	hindex += bref->type * HAMMER2_FREEMAP_HEUR_NRADIX;
	hindex &= HAMMER2_FREEMAP_HEUR_TYPES * HAMMER2_FREEMAP_HEUR_NRADIX - 1;
	KKASSERT(hindex < HAMMER2_FREEMAP_HEUR_SIZE);

	iter.bpref = hmp->heur_freemap[hindex];
	iter.relaxed = hmp->freemap_relaxed;

	if (iter.bpref > hmp->total_size)
		iter.bpref = hmp->total_size - 1;

	parent = &hmp->fchain;
	hammer2_chain_ref(parent);
	hammer2_chain_lock(parent, HAMMER2_RESOLVE_ALWAYS);
	error = HAMMER2_ERROR_EAGAIN;
	iter.bnext = iter.bpref;
	iter.loops = 0;

	while (error == HAMMER2_ERROR_EAGAIN)
		error = hammer2_freemap_try_alloc(&parent, bref, radix, &iter,
		    mtid);
	hmp->freemap_relaxed |= iter.relaxed;
	hmp->heur_freemap[hindex] = iter.bnext;
	hammer2_chain_unlock(parent);
	hammer2_chain_drop(parent);

	return (error);
}

static int
hammer2_freemap_try_alloc(hammer2_chain_t **parentp, hammer2_blockref_t *bref,
    int radix, hammer2_fiterate_t *iter, hammer2_tid_t mtid)
{
	hammer2_dev_t *hmp = (*parentp)->hmp;
	hammer2_off_t l0size, l1size, l1mask, key;
	hammer2_key_t key_dummy, base_key;
	hammer2_chain_t *chain;
	hammer2_bmap_data_t *bmap;
	uint16_t class;
	int error, count, start, n, availchk;
	size_t bytes;

	bytes = (size_t)1 << radix;
	class = (bref->type << 8) | HAMMER2_PBUFRADIX;

	key = H2FMBASE(iter->bnext, HAMMER2_FREEMAP_LEVEL1_RADIX);
	l0size = HAMMER2_FREEMAP_LEVEL0_SIZE;
	l1size = HAMMER2_FREEMAP_LEVEL1_SIZE;
	l1mask = l1size - 1;

	chain = hammer2_chain_lookup(parentp, &key_dummy, key, key + l1mask,
	    &error, HAMMER2_LOOKUP_ALWAYS | HAMMER2_LOOKUP_MATCHIND);
	if (chain == NULL) {

		error = hammer2_chain_create(parentp, &chain, NULL, hmp->spmp,
		    HAMMER2_METH_DEFAULT, key, HAMMER2_FREEMAP_LEVEL1_RADIX,
		    HAMMER2_BREF_TYPE_FREEMAP_LEAF, HAMMER2_FREEMAP_LEVELN_PSIZE,
		    mtid, 0, 0);

		if (error)
			hprintf("chain_create failed: error %02x\n", error);
		if (error == 0) {
			hammer2_chain_modify(chain, mtid, 0, 0);
			bzero(&chain->data->bmdata[0],
			    HAMMER2_FREEMAP_LEVELN_PSIZE);
			chain->bref.check.freemap.bigmask = (uint32_t)-1;
			chain->bref.check.freemap.avail = l1size;

			hammer2_freemap_init(hmp, key, chain);
		}
	} else if (chain->error) {

		hprintf_rl("error %d at data_off %016llx\n",
		    chain->error, (long long)bref->data_off);
		error = HAMMER2_ERROR_EIO;
	} else if ((chain->bref.check.freemap.bigmask &
	    ((size_t)1 << radix)) == 0) {

		error = HAMMER2_ERROR_ENOSPC;
	} else {

		error = hammer2_chain_modify(chain, mtid, 0, 0);
	}

	if (error == 0) {
		KKASSERT(chain->bref.type == HAMMER2_BREF_TYPE_FREEMAP_LEAF);
		start = (int)((iter->bnext - key) >>
		    HAMMER2_FREEMAP_LEVEL0_RADIX);
		KKASSERT(start >= 0 && start < HAMMER2_FREEMAP_COUNT);

		error = hammer2_chain_modify(chain, mtid, 0, 0);
		if (error == 0)
			error = HAMMER2_ERROR_ENOSPC;
		for (count = 0; error == HAMMER2_ERROR_ENOSPC &&
		    count < HAMMER2_FREEMAP_COUNT; ++count) {
			if (start + count >= HAMMER2_FREEMAP_COUNT &&
			    start - count < 0)
				break;

			n = start + count;
			bmap = &chain->data->bmdata[n];
			if (n >= HAMMER2_FREEMAP_COUNT)
				availchk = 0;
			else if (bmap->avail)
				availchk = 1;
			else if (radix < HAMMER2_FREEMAP_BLOCK_RADIX &&
			    (bmap->linear & HAMMER2_FREEMAP_BLOCK_MASK))
				availchk = 1;
			else
				availchk = 0;

			if (availchk && (bmap->class == 0 ||
			    bmap->class == class || iter->relaxed)) {
				base_key = key + n * l0size;
				error = hammer2_bmap_alloc(hmp, bmap, class, n,
				    (int)bref->key, radix, &base_key);
				if (error != HAMMER2_ERROR_ENOSPC) {
					key = base_key;
					break;
				}
			}

			n = start - count;
			bmap = &chain->data->bmdata[n];
			if (n < 0)
				availchk = 0;
			else if (bmap->avail)
				availchk = 1;
			else if (radix < HAMMER2_FREEMAP_BLOCK_RADIX &&
			    (bmap->linear & HAMMER2_FREEMAP_BLOCK_MASK))
				availchk = 1;
			else
				availchk = 0;

			if (availchk && (bmap->class == 0 ||
			    bmap->class == class || iter->relaxed)) {
				base_key = key + n * l0size;
				error = hammer2_bmap_alloc(hmp, bmap, class, n,
				    (int)bref->key, radix, &base_key);
				if (error != HAMMER2_ERROR_ENOSPC) {
					key = base_key;
					break;
				}
			}
		}

		if (error == HAMMER2_ERROR_ENOSPC && start == 0 &&
		    iter->relaxed)
			chain->bref.check.freemap.bigmask &=
			    (uint32_t)~((size_t)1 << radix);

	}

	if (error == 0) {

		KKASSERT(key >= hmp->voldata.allocator_beg &&
		    key + bytes <= hmp->total_size);
		KKASSERT((key & HAMMER2_ZONE_MASK64) >= HAMMER2_ZONE_SEG);
		bref->data_off = key | radix;

		if (bref->type == HAMMER2_BREF_TYPE_DATA)
			hammer2_io_dedup_set(hmp, bref);
	} else if (error == HAMMER2_ERROR_ENOSPC) {

		error = hammer2_freemap_iterate(parentp, &chain, iter);
	}

	if (chain) {
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
	}
	return (error);
}

static int
hammer2_bmap_alloc(hammer2_dev_t *hmp, hammer2_bmap_data_t *bmap,
    uint16_t class, int n, int sub_key, int radix, hammer2_key_t *basep)
{
	hammer2_bitmap_t bmmask, pbmmask;
	hammer2_io_t *dio;
	size_t size, bgsize;
	int bmradix, offset, i, j, pbmradix;

	size = (size_t)1 << radix;

	if (radix <= HAMMER2_FREEMAP_BLOCK_RADIX) {
		bmradix = 2;

	} else {
		bmradix = (hammer2_bitmap_t)2 <<
		    (radix - HAMMER2_FREEMAP_BLOCK_RADIX);

	}

	if (((uint32_t)bmap->linear & HAMMER2_FREEMAP_BLOCK_MASK) + size <=
	    HAMMER2_FREEMAP_BLOCK_SIZE &&
	    (bmap->linear & HAMMER2_FREEMAP_BLOCK_MASK) &&
	    bmap->linear < HAMMER2_SEGSIZE) {

		KKASSERT(bmap->linear >= 0);
		KKASSERT(bmap->linear + size <= HAMMER2_SEGSIZE);
		KKASSERT((bmap->linear & (HAMMER2_ALLOC_MIN - 1)) == 0);
		offset = bmap->linear;
		i = offset / (HAMMER2_SEGSIZE / HAMMER2_BMAP_ELEMENTS);
		j = (offset / (HAMMER2_FREEMAP_BLOCK_SIZE / 2)) & 62;
		bmmask = (bmradix == HAMMER2_BMAP_BITS_PER_ELEMENT) ?
		    HAMMER2_BMAP_ALLONES : ((hammer2_bitmap_t)1 << bmradix) - 1;
		bmmask <<= j;
		bmap->linear = offset + size;
	} else {

		i = -1;
		j = -1;

		switch (class >> 8) {
		case HAMMER2_BREF_TYPE_DATA:
			if (radix >= HAMMER2_FREEMAP_BLOCK_RADIX) {
				i = (sub_key & HAMMER2_BMAP_MASK) /
				    (HAMMER2_BMAP_SIZE / HAMMER2_BMAP_ELEMENTS);
				j = (sub_key & HAMMER2_BMAP_INDEX_MASK) /
				    (HAMMER2_BMAP_INDEX_SIZE /
				    HAMMER2_BMAP_BLOCKS_PER_ELEMENT);
				j = j * 2;
			}
			break;
		default:
			break;
		}
		if (i >= 0) {
			KKASSERT(i < HAMMER2_BMAP_ELEMENTS);
			KKASSERT(j < 2 * HAMMER2_BMAP_BLOCKS_PER_ELEMENT);
			KKASSERT(j + bmradix <= HAMMER2_BMAP_BITS_PER_ELEMENT);
			bmmask = (bmradix == HAMMER2_BMAP_BITS_PER_ELEMENT) ?
			    HAMMER2_BMAP_ALLONES :
			    ((hammer2_bitmap_t)1 << bmradix) - 1;
			bmmask <<= j;

			if ((bmap->bitmapq[i] & bmmask) == 0)
				goto success;
		}

		for (i = 0; i < HAMMER2_BMAP_ELEMENTS; ++i) {
			bmmask = (bmradix == HAMMER2_BMAP_BITS_PER_ELEMENT) ?
			    HAMMER2_BMAP_ALLONES : ((hammer2_bitmap_t)1 << bmradix) - 1;
			for (j = 0; j < HAMMER2_BMAP_BITS_PER_ELEMENT; j += bmradix) {
				if ((bmap->bitmapq[i] & bmmask) == 0)
					goto success;
				bmmask <<= bmradix;
			}
		}

		return (HAMMER2_ERROR_ENOSPC);
success:
		offset = i * (HAMMER2_SEGSIZE / HAMMER2_BMAP_ELEMENTS) +
		    (j * (HAMMER2_FREEMAP_BLOCK_SIZE / 2));
		if (size & HAMMER2_FREEMAP_BLOCK_MASK)
			bmap->linear = offset + size;
	}

	KKASSERT(i >= 0 && i < HAMMER2_BMAP_ELEMENTS);

	if ((bmap->bitmapq[i] & bmmask) == 0 && HAMMER2_PBUFSIZE != size) {
		pbmradix = (hammer2_bitmap_t)2 <<
		    (HAMMER2_PBUFRADIX - HAMMER2_FREEMAP_BLOCK_RADIX);
		pbmmask = (pbmradix == HAMMER2_BMAP_BITS_PER_ELEMENT) ?
		    HAMMER2_BMAP_ALLONES : ((hammer2_bitmap_t)1 << pbmradix) - 1;

		while ((pbmmask & bmmask) == 0)
			pbmmask <<= pbmradix;

		if ((bmap->bitmapq[i] & pbmmask) == 0) {
			hammer2_io_newnz(hmp, class >> 8,
			    (*basep + (offset & ~HAMMER2_PBUFMASK)) |
			    hammer2_getradix(HAMMER2_PBUFSIZE),
			    HAMMER2_PBUFSIZE, &dio);
			hammer2_io_putblk(&dio);
		}
	}

	if (radix < HAMMER2_FREEMAP_BLOCK_RADIX) {
		if (bmap->bitmapq[i] & bmmask)
			bgsize = 0;
		else
			bgsize = HAMMER2_FREEMAP_BLOCK_SIZE;
	} else {
		bgsize = size;
	}

	bmap->bitmapq[i] |= bmmask;
	if (bmap->class == 0)
		bmap->class = class;
	bmap->avail -= bgsize;
	*basep += offset;

	if (bgsize) {
		hammer2_voldata_lock(hmp);
		hammer2_voldata_modify(hmp);
		hmp->voldata.allocator_free -= bgsize;
		hammer2_voldata_unlock(hmp);
	}

	return (0);
}

static void
hammer2_freemap_init(hammer2_dev_t *hmp, hammer2_key_t key,
    hammer2_chain_t *chain)
{
	hammer2_off_t lokey, hikey;
	hammer2_bmap_data_t *bmap;
	int count;

	lokey = (hmp->voldata.allocator_beg + HAMMER2_SEGMASK64) &
	    ~HAMMER2_SEGMASK64;
	if (lokey < H2FMBASE(key, HAMMER2_FREEMAP_LEVEL1_RADIX))
		lokey = H2FMBASE(key, HAMMER2_FREEMAP_LEVEL1_RADIX);

	if (lokey < H2FMZONEBASE(key) + HAMMER2_ZONE_SEG64)
		lokey = H2FMZONEBASE(key) + HAMMER2_ZONE_SEG64;

	hikey = key + HAMMER2_FREEMAP_LEVEL1_SIZE;
	if (hikey > hmp->total_size)
		hikey = hmp->total_size & ~HAMMER2_SEGMASK64;

	chain->bref.check.freemap.avail = HAMMER2_FREEMAP_LEVEL1_SIZE;
	bmap = &chain->data->bmdata[0];

	for (count = 0; count < HAMMER2_FREEMAP_COUNT; ++count) {
		if (key < lokey || key >= hikey) {
			memset(bmap->bitmapq, -1, sizeof(bmap->bitmapq));
			bmap->avail = 0;
			bmap->linear = HAMMER2_SEGSIZE;
			chain->bref.check.freemap.avail -=
			    HAMMER2_FREEMAP_LEVEL0_SIZE;
		} else {
			bmap->avail = HAMMER2_FREEMAP_LEVEL0_SIZE;
		}
		key += HAMMER2_FREEMAP_LEVEL0_SIZE;
		++bmap;
	}
}

static int
hammer2_freemap_iterate(hammer2_chain_t **parentp, hammer2_chain_t **chainp,
    hammer2_fiterate_t *iter)
{
	hammer2_dev_t *hmp = (*parentp)->hmp;

	iter->bnext &= ~HAMMER2_FREEMAP_LEVEL1_MASK;
	iter->bnext += HAMMER2_FREEMAP_LEVEL1_SIZE;

	if (iter->bnext >= hmp->total_size) {
		iter->bnext = 0;
		if (++iter->loops >= 2) {
			if (iter->relaxed == 0)
				iter->relaxed = 1;
			else
				return (HAMMER2_ERROR_ENOSPC);
		}
	}
	return (HAMMER2_ERROR_EAGAIN);
}

void
hammer2_freemap_adjust(hammer2_dev_t *hmp, hammer2_blockref_t *bref, int how)
{
	hammer2_off_t data_off = bref->data_off;
	hammer2_chain_t *chain, *parent;
	hammer2_bmap_data_t *bmap;
	hammer2_key_t key, key_dummy;
	hammer2_off_t l1size, l1mask;
	hammer2_tid_t mtid;
	hammer2_bitmap_t *bitmap;
	const hammer2_bitmap_t bmmask00 = 0;

	hammer2_bitmap_t bmmask11;
	uint16_t class;
	int error, radix, start, count, modified = 0;
	size_t bgsize = 0;

	KKASSERT(how == HAMMER2_FREEMAP_DORECOVER);

	KKASSERT(hmp->spmp);
	mtid = hammer2_trans_sub(hmp->spmp);

	radix = (int)data_off & HAMMER2_OFF_MASK_RADIX;
	KKASSERT(radix != 0);
	KKASSERT(radix <= HAMMER2_RADIX_MAX);

	data_off &= ~HAMMER2_OFF_MASK_RADIX;

	class = (bref->type << 8) | HAMMER2_PBUFRADIX;

	if (data_off < hmp->voldata.allocator_beg)
		return;

	KKASSERT((data_off & HAMMER2_ZONE_MASK64) >= HAMMER2_ZONE_SEG);

	key = H2FMBASE(data_off, HAMMER2_FREEMAP_LEVEL1_RADIX);
	l1size = HAMMER2_FREEMAP_LEVEL1_SIZE;
	l1mask = l1size - 1;

	parent = &hmp->fchain;
	hammer2_chain_ref(parent);
	hammer2_chain_lock(parent, HAMMER2_RESOLVE_ALWAYS);

	chain = hammer2_chain_lookup(&parent, &key_dummy, key, key + l1mask,
	    &error, HAMMER2_LOOKUP_ALWAYS | HAMMER2_LOOKUP_MATCHIND);

	if (chain == NULL && how != HAMMER2_FREEMAP_DORECOVER) {
		hprintf_rl("no chain at data_off %016llx\n",
		    (long long)bref->data_off);
		goto done;
	}

	if (chain && chain->error) {
		hprintf_rl("error %d at data_off %016llx\n",
		    chain->error, (long long)bref->data_off);
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
		chain = NULL;
		goto done;
	}

	if (chain == NULL && how == HAMMER2_FREEMAP_DORECOVER) {
		error = hammer2_chain_create(&parent, &chain, NULL, hmp->spmp,
		    HAMMER2_METH_DEFAULT, key, HAMMER2_FREEMAP_LEVEL1_RADIX,
		    HAMMER2_BREF_TYPE_FREEMAP_LEAF,
		    HAMMER2_FREEMAP_LEVELN_PSIZE, mtid, 0, 0);
		if (error == 0) {
			error = hammer2_chain_modify(chain, mtid, 0, 0);
			KKASSERT(error == 0);
			bzero(&chain->data->bmdata[0],
			    HAMMER2_FREEMAP_LEVELN_PSIZE);
			chain->bref.check.freemap.bigmask = (uint32_t)-1;
			chain->bref.check.freemap.avail = l1size;

			hammer2_freemap_init(hmp, key, chain);
		}

	}

	start = ((int)(data_off >> HAMMER2_FREEMAP_BLOCK_RADIX) & 15) * 2;

	bmmask11 = (hammer2_bitmap_t)3 << start;

	if (radix < HAMMER2_FREEMAP_BLOCK_RADIX)
		count = 1;
	else
		count = 1 << (radix - HAMMER2_FREEMAP_BLOCK_RADIX);

again:
	bmap = &chain->data->bmdata[(int)(data_off >> HAMMER2_SEGRADIX) &
	    (HAMMER2_FREEMAP_COUNT - 1)];
	bitmap = &bmap->bitmapq[(int)(data_off >> (HAMMER2_SEGRADIX - 3)) & 7];

	if (modified)
		bmap->linear = 0;

	while (count) {
		KKASSERT(bmmask11);
		if (how == HAMMER2_FREEMAP_DORECOVER) {

			if ((*bitmap & bmmask11) != bmmask11) {
				if (modified == 0) {
					hammer2_chain_modify(chain, mtid, 0, 0);
					modified = 1;
					goto again;
				}
				if ((*bitmap & bmmask11) == bmmask00) {
					bmap->avail -=
					    HAMMER2_FREEMAP_BLOCK_SIZE;
					bgsize += HAMMER2_FREEMAP_BLOCK_SIZE;
				}
				if (bmap->class == 0)
					bmap->class = class;
				*bitmap |= bmmask11;
			}
		}
		--count;

		bmmask11 <<= 2;
	}

	if (modified) {
		chain->bref.check.freemap.bigmask = -1;
		hmp->freemap_relaxed = 0;
	}

	hammer2_chain_unlock(chain);
	hammer2_chain_drop(chain);
done:
	hammer2_chain_unlock(parent);
	hammer2_chain_drop(parent);

	if (bgsize) {
		hammer2_voldata_lock(hmp);
		hammer2_voldata_modify(hmp);
		hmp->voldata.allocator_free -= bgsize;
		hammer2_voldata_unlock(hmp);
	}
}
