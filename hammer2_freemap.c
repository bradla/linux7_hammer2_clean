#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/proc.h>
#include <sys/mount.h>

#include "hammer2.h"

#define FREEMAP_DEBUG	0

struct hammer2_fiterate {
	hammer2_off_t	bpref;
	hammer2_off_t	bnext;
	int		loops;
	int		relaxed;
};

typedef struct hammer2_fiterate hammer2_fiterate_t;

static int hammer2_freemap_try_alloc(hammer2_chain_t **parentp,
			hammer2_blockref_t *bref, int radix,
			hammer2_fiterate_t *iter, hammer2_tid_t mtid);
static void hammer2_freemap_init(hammer2_dev_t *hmp,
			hammer2_key_t key, hammer2_chain_t *chain);
static int hammer2_bmap_alloc(hammer2_dev_t *hmp,
			hammer2_bmap_data_t *bmap, uint16_t class,
			int n, int sub_key, int radix, hammer2_key_t *basep);
static int hammer2_freemap_iterate(hammer2_chain_t **parentp,
			hammer2_chain_t **chainp,
			hammer2_fiterate_t *iter);

static
int
hammer2_freemap_reserve(hammer2_chain_t *chain, int radix)
{
	hammer2_blockref_t *bref = &chain->bref;
	hammer2_off_t off;
	int index;
	int index_inc;
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

	switch(bref->keybits) {
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
		panic("freemap: bad radix(2) %p %d\n", bref, bref->keybits);
		off = (hammer2_off_t)-1;
		break;
	}
	bref->data_off = off | radix;
#if FREEMAP_DEBUG
	kprintf("FREEMAP BLOCK TYPE %d %016jx/%d DATA_OFF=%016jx\n",
		bref->type, bref->key, bref->keybits, bref->data_off);
#endif
	return (0);
}

int
hammer2_freemap_alloc(hammer2_chain_t *chain, size_t bytes)
{
	hammer2_dev_t *hmp = chain->hmp;
	hammer2_blockref_t *bref = &chain->bref;
	hammer2_chain_t *parent;
	hammer2_tid_t mtid;
	int radix;
	int error;
	unsigned int hindex;
	hammer2_fiterate_t iter;

	if (bytes == 0) {
		chain->bref.data_off = 0;
		return 0;
	}

	KKASSERT(hmp->spmp);
	mtid = hammer2_trans_sub(hmp->spmp);

	radix = hammer2_getradix(bytes);
	KKASSERT((size_t)1 << radix == bytes);

	if (bref->type == HAMMER2_BREF_TYPE_FREEMAP_NODE ||
	    bref->type == HAMMER2_BREF_TYPE_FREEMAP_LEAF) {
		error = hammer2_freemap_reserve(chain, radix);

		return error;
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

	while (error == HAMMER2_ERROR_EAGAIN) {
		error = hammer2_freemap_try_alloc(&parent, bref, radix,
						  &iter, mtid);
	}
	hmp->freemap_relaxed |= iter.relaxed;
	hmp->heur_freemap[hindex] = iter.bnext;
	hammer2_chain_unlock(parent);
	hammer2_chain_drop(parent);

	return (error);
}

static int
hammer2_freemap_try_alloc(hammer2_chain_t **parentp,
			  hammer2_blockref_t *bref, int radix,
			  hammer2_fiterate_t *iter, hammer2_tid_t mtid)
{
	hammer2_dev_t *hmp = (*parentp)->hmp;
	hammer2_off_t l0size;
	hammer2_off_t l1size;
	hammer2_off_t l1mask;
	hammer2_key_t key_dummy;
	hammer2_chain_t *chain;
	hammer2_off_t key;
	size_t bytes;
	uint16_t class;
	int error;

	bytes = (size_t)1 << radix;
	class = (bref->type << 8) | HAMMER2_PBUFRADIX;

	key = H2FMBASE(iter->bnext, HAMMER2_FREEMAP_LEVEL1_RADIX);
	l0size = HAMMER2_FREEMAP_LEVEL0_SIZE;
	l1size = HAMMER2_FREEMAP_LEVEL1_SIZE;
	l1mask = l1size - 1;

	chain = hammer2_chain_lookup(parentp, &key_dummy, key, key + l1mask,
				     &error,
				     HAMMER2_LOOKUP_ALWAYS |
				     HAMMER2_LOOKUP_MATCHIND);

	if (chain == NULL) {
#if 0
		kprintf("freemap create L1 @ %016jx bpref %016jx\n",
			key, iter->bpref);
#endif
		error = hammer2_chain_create(parentp, &chain, NULL, hmp->spmp,
				     HAMMER2_METH_DEFAULT,
				     key, HAMMER2_FREEMAP_LEVEL1_RADIX,
				     HAMMER2_BREF_TYPE_FREEMAP_LEAF,
				     HAMMER2_FREEMAP_LEVELN_PSIZE,
				     mtid, 0, 0);
		KKASSERT(error == 0);
		if (error == 0) {
			error = hammer2_chain_modify(chain, mtid, 0, 0);
			if (error == 0) {
				bzero(&chain->data->bmdata[0],
				      HAMMER2_FREEMAP_LEVELN_PSIZE);
				chain->bref.check.freemap.bigmask =
					(uint32_t)-1;
				chain->bref.check.freemap.avail = l1size;

				hammer2_freemap_init(hmp, key, chain);
			}
		}
	} else if (chain->error) {
		kprintf("hammer2_freemap_try_alloc: %016jx: error %s\n",
			(intmax_t)bref->data_off,
			hammer2_error_str(chain->error));
		error = HAMMER2_ERROR_EIO;
	} else if ((chain->bref.check.freemap.bigmask &
		   ((size_t)1 << radix)) == 0) {
		error = HAMMER2_ERROR_ENOSPC;
	} else {
		error = hammer2_chain_modify(chain, mtid, 0, 0);
	}

	if (error == 0) {
		hammer2_bmap_data_t *bmap;
		hammer2_key_t base_key;
		int count;
		int start;
		int n;

		KKASSERT(chain->bref.type == HAMMER2_BREF_TYPE_FREEMAP_LEAF);
		start = (int)((iter->bnext - key) >>
			      HAMMER2_FREEMAP_LEVEL0_RADIX);
		KKASSERT(start >= 0 && start < HAMMER2_FREEMAP_COUNT);

		error = hammer2_chain_modify(chain, mtid, 0, 0);
		if (error)
			goto skip_scan;

		error = HAMMER2_ERROR_ENOSPC;
		for (count = 0; count < HAMMER2_FREEMAP_COUNT; ++count) {
			int availchk;

			if (start + count >= HAMMER2_FREEMAP_COUNT &&
			    start - count < 0) {
				break;
			}

			n = start + count;
			bmap = &chain->data->bmdata[n];

			if (n >= HAMMER2_FREEMAP_COUNT) {
				availchk = 0;
			} else if (bmap->avail) {
				availchk = 1;
			} else if (radix < HAMMER2_FREEMAP_BLOCK_RADIX &&
			          (bmap->linear & HAMMER2_FREEMAP_BLOCK_MASK)) {
				availchk = 1;
			} else {
				availchk = 0;
			}

			if (availchk &&
			    (bmap->class == 0 || bmap->class == class ||
			     iter->relaxed)) {
				base_key = key + n * l0size;
				error = hammer2_bmap_alloc(hmp, bmap,
							   class, n,
							   (int)bref->key,
							   radix,
							   &base_key);
				if (error != HAMMER2_ERROR_ENOSPC) {
					key = base_key;
					break;
				}
			}

			n = start - count;
			bmap = &chain->data->bmdata[n];
			if (n < 0) {
				availchk = 0;
			} else if (bmap->avail) {
				availchk = 1;
			} else if (radix < HAMMER2_FREEMAP_BLOCK_RADIX &&
			          (bmap->linear & HAMMER2_FREEMAP_BLOCK_MASK)) {
				availchk = 1;
			} else {
				availchk = 0;
			}

			if (availchk &&
			    (bmap->class == 0 || bmap->class == class ||
			    iter->relaxed)) {
				base_key = key + n * l0size;
				error = hammer2_bmap_alloc(hmp, bmap,
							   class, n,
							   (int)bref->key,
							   radix,
							   &base_key);
				if (error != HAMMER2_ERROR_ENOSPC) {
					key = base_key;
					break;
				}
			}
		}

		if (error == HAMMER2_ERROR_ENOSPC &&
		    start == 0 &&
		    iter->relaxed)
		{
			chain->bref.check.freemap.bigmask &=
				(uint32_t)~((size_t)1 << radix);
		}
skip_scan:
		;
	}

	if (error == 0) {
		KKASSERT(key >= hmp->voldata.allocator_beg &&
			 key + bytes <= hmp->total_size);
		KKASSERT((key & HAMMER2_ZONE_MASK64) >= HAMMER2_ZONE_SEG);
		bref->data_off = key | radix;

		if (bref->type == HAMMER2_BREF_TYPE_DATA)
			hammer2_io_dedup_set(hmp, bref);
#if 0
		kprintf("alloc cp=%p %016jx %016jx using %016jx\n",
			chain,
			bref->key, bref->data_off, chain->bref.data_off);
#endif
	} else if (error == HAMMER2_ERROR_ENOSPC) {
		error = hammer2_freemap_iterate(parentp, &chain, iter);
	}

	if (chain) {
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
	}
	return (error);
}

static
int
hammer2_bmap_alloc(hammer2_dev_t *hmp, hammer2_bmap_data_t *bmap,
		   uint16_t class, int n, int sub_key,
		   int radix, hammer2_key_t *basep)
{
	size_t size;
	size_t bgsize;
	int bmradix;
	hammer2_bitmap_t bmmask;
	int offset;
	int i;
	int j;

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
		KKASSERT(bmap->linear >= 0 &&
			 bmap->linear + size <= HAMMER2_SEGSIZE &&
			 (bmap->linear & (HAMMER2_ALLOC_MIN - 1)) == 0);
		offset = bmap->linear;
		i = offset / (HAMMER2_SEGSIZE / HAMMER2_BMAP_ELEMENTS);
		j = (offset / (HAMMER2_FREEMAP_BLOCK_SIZE / 2)) & 62;
		bmmask = (bmradix == HAMMER2_BMAP_BITS_PER_ELEMENT) ?
			 HAMMER2_BMAP_ALLONES :
			 ((hammer2_bitmap_t)1 << bmradix) - 1;
		bmmask <<= j;
		bmap->linear = offset + size;
	} else {
		i = -1;
		j = -1;

		switch(class >> 8) {
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
		case HAMMER2_BREF_TYPE_INODE:
			break;
		default:
			break;
		}
		if (i >= 0) {
			KKASSERT(i < HAMMER2_BMAP_ELEMENTS &&
				 j < 2 * HAMMER2_BMAP_BLOCKS_PER_ELEMENT);
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
				 HAMMER2_BMAP_ALLONES :
				 ((hammer2_bitmap_t)1 << bmradix) - 1;
			for (j = 0;
			     j < HAMMER2_BMAP_BITS_PER_ELEMENT;
			     j += bmradix) {
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

	if ((bmap->bitmapq[i] & bmmask) == 0 &&
	    HAMMER2_PBUFSIZE != size) {
		size_t psize = HAMMER2_PBUFSIZE;
		hammer2_off_t pmask = (hammer2_off_t)psize - 1;
		int pbmradix = (hammer2_bitmap_t)2 <<
					(HAMMER2_PBUFRADIX -
			       HAMMER2_FREEMAP_BLOCK_RADIX);
		hammer2_bitmap_t pbmmask;
		int pradix = hammer2_getradix(psize);

		pbmmask = (pbmradix == HAMMER2_BMAP_BITS_PER_ELEMENT) ?
			HAMMER2_BMAP_ALLONES :
			((hammer2_bitmap_t)1 << pbmradix) - 1;
		while ((pbmmask & bmmask) == 0)
			pbmmask <<= pbmradix;

#if 0
		kprintf("%016jx mask %016jx %016jx %016jx (%zd/%zd)\n",
			*basep + offset, bmap->bitmapq[i],
			pbmmask, bmmask, size, psize);
#endif

		if ((bmap->bitmapq[i] & pbmmask) == 0) {
			hammer2_io_t *dio;

			hammer2_io_newnz(hmp, class >> 8,
					(*basep + (offset & ~pmask)) |
					pradix, psize, &dio);
			hammer2_io_putblk(&dio);
		}
	}

#if 0
	if (size == HAMMER2_INODE_BYTES) {
		if (n & 1) {
			if (bmap[-1].radix == 0 && bmap[-1].avail)
				bmap[-1].radix = radix;
		} else {
			if (bmap[1].radix == 0 && bmap[1].avail)
				bmap[1].radix = radix;
		}
	}
#endif
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

	return(0);
}

static
void
hammer2_freemap_init(hammer2_dev_t *hmp, hammer2_key_t key,
		     hammer2_chain_t *chain)
{
	hammer2_off_t lokey;
	hammer2_off_t hikey;
	hammer2_bmap_data_t *bmap;
	int count;

	lokey = (hmp->voldata.allocator_beg + HAMMER2_SEGMASK64) &
		~HAMMER2_SEGMASK64;
	if (lokey < H2FMBASE(key, HAMMER2_FREEMAP_LEVEL1_RADIX))
		lokey = H2FMBASE(key, HAMMER2_FREEMAP_LEVEL1_RADIX);

	if (lokey < H2FMZONEBASE(key) + HAMMER2_ZONE_SEG64)
		lokey = H2FMZONEBASE(key) + HAMMER2_ZONE_SEG64;

	hikey = key + HAMMER2_FREEMAP_LEVEL1_SIZE;
	if (hikey > hmp->total_size) {
		hikey = hmp->total_size & ~HAMMER2_SEGMASK64;
	}

	chain->bref.check.freemap.avail = HAMMER2_FREEMAP_LEVEL1_SIZE;
	bmap = &chain->data->bmdata[0];

	for (count = 0; count < HAMMER2_FREEMAP_COUNT; ++count) {
		if (key < lokey || key >= hikey) {
			memset(bmap->bitmapq, -1,
			       sizeof(bmap->bitmapq));
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
	return(HAMMER2_ERROR_EAGAIN);
}

void
hammer2_freemap_adjust(hammer2_dev_t *hmp, hammer2_blockref_t *bref,
		       int how)
{
	hammer2_off_t data_off = bref->data_off;
	hammer2_chain_t *chain;
	hammer2_chain_t *parent;
	hammer2_bmap_data_t *bmap;
	hammer2_key_t key;
	hammer2_key_t key_dummy;
	hammer2_off_t l1size;
	hammer2_off_t l1mask;
	hammer2_tid_t mtid;
	hammer2_bitmap_t *bitmap;
	const hammer2_bitmap_t bmmask00 = 0;
	hammer2_bitmap_t bmmask11;
	size_t bytes;
	uint16_t class;
	int radix;
	int start;
	int count;
	int modified = 0;
	int error;
	size_t bgsize = 0;

	KKASSERT(how == HAMMER2_FREEMAP_DORECOVER);

	KKASSERT(hmp->spmp);
	mtid = hammer2_trans_sub(hmp->spmp);

	radix = (int)data_off & HAMMER2_OFF_MASK_RADIX;
	KKASSERT(radix != 0);
	data_off &= ~HAMMER2_OFF_MASK_RADIX;
	KKASSERT(radix <= HAMMER2_RADIX_MAX);

	if (radix)
		bytes = (size_t)1 << radix;
	else
		bytes = 0;
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
				     &error,
				     HAMMER2_LOOKUP_ALWAYS |
				     HAMMER2_LOOKUP_MATCHIND);

	if (chain == NULL && how != HAMMER2_FREEMAP_DORECOVER) {
		kprintf("hammer2_freemap_adjust: %016jx: no chain\n",
			(intmax_t)bref->data_off);
		goto done;
	}
	if (chain->error) {
		kprintf("hammer2_freemap_adjust: %016jx: error %s\n",
			(intmax_t)bref->data_off,
			hammer2_error_str(chain->error));
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
		chain = NULL;
		goto done;
	}

	if (chain == NULL && how == HAMMER2_FREEMAP_DORECOVER) {
		error = hammer2_chain_create(&parent, &chain, NULL, hmp->spmp,
				     HAMMER2_METH_DEFAULT,
				     key, HAMMER2_FREEMAP_LEVEL1_RADIX,
				     HAMMER2_BREF_TYPE_FREEMAP_LEAF,
				     HAMMER2_FREEMAP_LEVELN_PSIZE,
				     mtid, 0, 0);

		if (hammer2_debug & 0x0040) {
			kprintf("fixup create chain %p %016jx:%d\n",
				chain, chain->bref.key, chain->bref.keybits);
		}

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

#if FREEMAP_DEBUG
	kprintf("FREEMAP ADJUST TYPE %d %016jx/%d DATA_OFF=%016jx\n",
		chain->bref.type, chain->bref.key,
		chain->bref.keybits, chain->bref.data_off);
#endif

	start = ((int)(data_off >> HAMMER2_FREEMAP_BLOCK_RADIX) & 15) * 2;
	bmmask11 = (hammer2_bitmap_t)3 << start;

	if (radix < HAMMER2_FREEMAP_BLOCK_RADIX) {
		count = 1;
#if 0
		if (how == HAMMER2_FREEMAP_DOREALFREE)
			how = HAMMER2_FREEMAP_DOMAYFREE;
#endif
	} else {
		count = 1 << (radix - HAMMER2_FREEMAP_BLOCK_RADIX);
	}

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
				if (hammer2_debug & 0x0040) {
					kprintf("hammer2_freemap_adjust: "
						"fixup type=%02x "
						"block=%016jx/%zd\n",
						bref->type, data_off, bytes);
				}
			} else {
			}
		}
#if 0
		else if ((*bitmap & bmmask11) == bmmask11) {
			if (!modified) {
				hammer2_chain_modify(chain, 0);
				modified = 1;
				goto again;
			}
			if (how == HAMMER2_FREEMAP_DOREALFREE)
				*bitmap &= ~bmmask11;
			else
				*bitmap = (*bitmap & ~bmmask11) | bmmask10;
		} else if ((*bitmap & bmmask11) == bmmask10) {
			if (how == HAMMER2_FREEMAP_DOREALFREE) {
				if (!modified) {
					hammer2_chain_modify(chain, 0);
					modified = 1;
					goto again;
				}
				*bitmap &= ~bmmask11;
			}
		} else {
			panic("hammer2_freemap_adjust: "
			      "Illegal state %08x(%08x)",
			      *bitmap, *bitmap & bmmask11);
		}
#endif
		--count;
		bmmask11 <<= 2;
	}
#if 0
#if HAMMER2_BMAP_ELEMENTS != 8
#error "hammer2_freemap.c: HAMMER2_BMAP_ELEMENTS expected to be 8"
#endif
	if (how == HAMMER2_FREEMAP_DOREALFREE && modified) {
		bmap->avail += 1 << radix;
		KKASSERT(bmap->avail <= HAMMER2_SEGSIZE);
		if (bmap->avail == HAMMER2_SEGSIZE &&
		    bmap->bitmapq[0] == 0 &&
		    bmap->bitmapq[1] == 0 &&
		    bmap->bitmapq[2] == 0 &&
		    bmap->bitmapq[3] == 0 &&
		    bmap->bitmapq[4] == 0 &&
		    bmap->bitmapq[5] == 0 &&
		    bmap->bitmapq[6] == 0 &&
		    bmap->bitmapq[7] == 0) {
			key = H2FMBASE(data_off, HAMMER2_FREEMAP_LEVEL0_RADIX);
			kprintf("Freeseg %016jx\n", (intmax_t)key);
			bmap->class = 0;
		}
	}
#endif

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
