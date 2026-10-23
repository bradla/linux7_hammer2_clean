#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/proc.h>
#include <sys/mount.h>
#include <vm/vm_kern.h>
#include <vm/vm_extern.h>

#include "hammer2.h"

typedef struct hammer2_chain_save {
	TAILQ_ENTRY(hammer2_chain_save)	entry;
	hammer2_chain_t	*chain;
} hammer2_chain_save_t;

TAILQ_HEAD(hammer2_chain_save_list, hammer2_chain_save);
typedef struct hammer2_chain_save_list hammer2_chain_save_list_t;

typedef struct hammer2_bulkfree_info {
	hammer2_dev_t		*hmp;
	kmem_anon_desc_t	kp;
	hammer2_off_t		sbase;
	hammer2_off_t		sstop;
	hammer2_bmap_data_t	*bmap;
	int			depth;
	long			count_10_00;
	long			count_11_10;
	long			count_00_11;
	long			count_01_11;
	long			count_10_11;
	long			count_l0cleans;
	long			count_linadjusts;
	long			count_inodes_scanned;
	long			count_dirents_scanned;
	long			count_dedup_factor;
	long			count_bytes_scanned;
	long			count_chains_scanned;
	long			count_chains_reported;
	long			bulkfree_calls;
	int			bulkfree_ticks;
	int			list_alert;
	hammer2_off_t		adj_free;
	hammer2_tid_t		mtid;
	time_t			save_time;
	hammer2_chain_save_list_t list;
	long			list_count;
	long			list_count_max;
	hammer2_chain_save_t	*backout;
	hammer2_dedup_t		*dedup;
	int			pri;
} hammer2_bulkfree_info_t;

static int h2_bulkfree_test(hammer2_bulkfree_info_t *info,
			hammer2_blockref_t *bref, int pri, int saved_error);
static uint32_t bigmask_get(hammer2_bmap_data_t *bmap);
static int bigmask_good(hammer2_bmap_data_t *bmap, uint32_t live_bigmask);

static
int
hammer2_bulkfree_scan(hammer2_chain_t *parent,
		  int (*func)(hammer2_bulkfree_info_t *info,
			      hammer2_blockref_t *bref),
		  hammer2_bulkfree_info_t *info)
{
	hammer2_blockref_t bref;
	hammer2_chain_t *chain;
	hammer2_chain_save_t *tail;
	hammer2_chain_save_t *save;
	int first = 1;
	int rup_error;
	int error;
	int e2;

	++info->pri;

	chain = NULL;
	rup_error = 0;
	error = 0;

	hammer2_chain_lock(parent, HAMMER2_RESOLVE_ALWAYS |
				   HAMMER2_RESOLVE_SHARED);

	tail = TAILQ_FIRST(&info->list);

	if (parent->error & HAMMER2_ERROR_CHECK) {
		error = parent->error;
		goto done;
	}

	if (parent->bref.type == HAMMER2_BREF_TYPE_INODE &&
	    (parent->bref.flags & HAMMER2_BREF_FLAG_PFSROOT)) {
		kprintf("hammer2_bulkfree: Scanning %s\n",
			parent->data->ipdata.filename);
	}

	for (;;) {
		error |= hammer2_chain_scan(parent, &chain, &bref, &first,
					    HAMMER2_LOOKUP_NODATA |
					    HAMMER2_LOOKUP_SHARED);

		if (error & ~HAMMER2_ERROR_CHECK)
			break;

		if (bref.type == HAMMER2_BREF_TYPE_DIRENT)
			++info->count_dirents_scanned;

		if ((bref.data_off & ~HAMMER2_OFF_MASK_RADIX) == 0)
			continue;

		++info->pri;
		e2 = h2_bulkfree_test(info, &bref, 1, 0);
		if (e2) {
			error |= e2 & ~HAMMER2_ERROR_EOF;
			continue;
		}

		if (bref.type == HAMMER2_BREF_TYPE_INODE)
			++info->count_inodes_scanned;

		error |= func(info, &bref);
		if (error & ~HAMMER2_ERROR_CHECK)
			break;

		if (chain == NULL)
			continue;

		info->count_bytes_scanned += chain->bytes;
		++info->count_chains_scanned;

		if (info->count_chains_scanned >=
		    info->count_chains_reported + 1000000 ||
		    (info->count_chains_scanned < 1000000 &&
		     info->count_chains_scanned >=
		     info->count_chains_reported + 100000)) {
			kprintf(" chains %-7ld inodes %-7ld "
				"dirents %-7ld bytes %5ldMB\n",
				info->count_chains_scanned,
				info->count_inodes_scanned,
				info->count_dirents_scanned,
				info->count_bytes_scanned / 1000000);
			info->count_chains_reported =
				info->count_chains_scanned;
		}

		switch(chain->bref.type) {
		case HAMMER2_BREF_TYPE_INODE:
		case HAMMER2_BREF_TYPE_FREEMAP_NODE:
		case HAMMER2_BREF_TYPE_INDIRECT:
		case HAMMER2_BREF_TYPE_VOLUME:
		case HAMMER2_BREF_TYPE_FREEMAP:
			++info->depth;
			if (chain->error & HAMMER2_ERROR_CHECK) {
			} else if (info->depth > 16 ||
				   info->backout ||
				   (info->depth > hammer2_limit_saved_depth &&
				   info->list_count >=
				    (hammer2_limit_saved_chains >> 2)))
			{
				if (info->list_count >
				     hammer2_limit_saved_chains &&
				    info->list_alert == 0)
				{
					kprintf("hammer2: during bulkfree, "
						"saved chains exceeded %ld "
						"at depth %d, "
						"backing off to less-efficient "
						"operation\n",
						hammer2_limit_saved_chains,
						info->depth);
					info->list_alert = 1;
				}

				save = kmalloc(sizeof(*save), M_HAMMER2,
					       M_WAITOK | M_ZERO);
				save->chain = chain;
				hammer2_chain_ref(chain);

				if (info->backout) {
					TAILQ_INSERT_AFTER(&info->list,
							   info->backout,
							   save, entry);
				} else {
					TAILQ_INSERT_HEAD(&info->list,
							  save, entry);
				}
				info->backout = save;
				++info->list_count;
				if (info->list_count_max < info->list_count)
					info->list_count_max = info->list_count;

				info->pri += 10;
			} else {
				int savepri = info->pri;

				hammer2_chain_unlock(chain);
				hammer2_chain_unlock(parent);
				info->pri = 0;
				rup_error |= hammer2_bulkfree_scan(chain,
								   func, info);
				info->pri += savepri;
				hammer2_chain_lock(parent,
						   HAMMER2_RESOLVE_ALWAYS |
						   HAMMER2_RESOLVE_SHARED);
				hammer2_chain_lock(chain,
						   HAMMER2_RESOLVE_ALWAYS |
						   HAMMER2_RESOLVE_SHARED);
			}
			--info->depth;
			break;
		case HAMMER2_BREF_TYPE_DATA:
			break;
		default:
			break;
		}
		if (rup_error & HAMMER2_ERROR_ABORTED)
			break;
	}
	if (chain) {
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
	}

	if (parent->bref.type == HAMMER2_BREF_TYPE_INODE &&
	    (parent->bref.flags & HAMMER2_BREF_FLAG_PFSROOT)) {
		for (;;) {
			int opri;

			save = TAILQ_FIRST(&info->list);
			if (save == tail)
				break;

			TAILQ_REMOVE(&info->list, save, entry);
			info->backout = NULL;
			--info->list_count;
			opri = info->pri;
			info->pri = 0;
			rup_error |= hammer2_bulkfree_scan(save->chain, func, info);
			hammer2_chain_drop(save->chain);
			kfree(save, M_HAMMER2);
			info->pri = opri;
		}
	}

	error |= rup_error;

	if (parent->bref.type == HAMMER2_BREF_TYPE_INODE &&
	    (parent->bref.flags & HAMMER2_BREF_FLAG_PFSROOT) &&
	    (error & ~HAMMER2_ERROR_EOF)) {
		kprintf("hammer2_bulkfree: Encountered errors (%08x) "
			"while scanning \"%s\"\n",
			error, parent->data->ipdata.filename);
	}

	h2_bulkfree_test(info, &parent->bref, info->pri + 1,
			 (error & ~HAMMER2_ERROR_EOF));

done:
	hammer2_chain_unlock(parent);

	return (error & ~HAMMER2_ERROR_EOF);
}

static void hammer2_bulkfree_thread(void *arg __unused);
static void cbinfo_bmap_init(hammer2_bulkfree_info_t *cbinfo, size_t size);
static int h2_bulkfree_callback(hammer2_bulkfree_info_t *cbinfo,
			hammer2_blockref_t *bref);
static int h2_bulkfree_sync(hammer2_bulkfree_info_t *cbinfo);
static void h2_bulkfree_sync_adjust(hammer2_bulkfree_info_t *cbinfo,
			hammer2_off_t data_off, hammer2_bmap_data_t *live,
			hammer2_bmap_data_t *bmap, hammer2_key_t alloc_base);

void
hammer2_bulkfree_init(hammer2_dev_t *hmp)
{
	hammer2_thr_create(&hmp->bfthr, NULL, hmp,
			   hmp->devrepname, -1, -1,
			   hammer2_bulkfree_thread);
}

void
hammer2_bulkfree_uninit(hammer2_dev_t *hmp)
{
	hammer2_thr_delete(&hmp->bfthr);
}

static void
hammer2_bulkfree_thread(void *arg)
{
	hammer2_thread_t *thr = arg;
	hammer2_ioc_bulkfree_t bfi;
	uint32_t flags;

	for (;;) {
		hammer2_thr_wait_any(thr,
				     HAMMER2_THREAD_STOP |
				     HAMMER2_THREAD_FREEZE |
				     HAMMER2_THREAD_UNFREEZE |
				     HAMMER2_THREAD_REMASTER,
				     hz * 60);

		flags = thr->flags;
		cpu_ccfence();
		if (flags & HAMMER2_THREAD_STOP)
			break;
		if (flags & HAMMER2_THREAD_FREEZE) {
			hammer2_thr_signal2(thr, HAMMER2_THREAD_FROZEN,
						 HAMMER2_THREAD_FREEZE);
			continue;
		}
		if (flags & HAMMER2_THREAD_UNFREEZE) {
			hammer2_thr_signal2(thr, 0,
						 HAMMER2_THREAD_FROZEN |
						 HAMMER2_THREAD_UNFREEZE);
			continue;
		}
		if (flags & HAMMER2_THREAD_FROZEN)
			continue;
		if (flags & HAMMER2_THREAD_REMASTER) {
			hammer2_thr_signal2(thr, 0, HAMMER2_THREAD_REMASTER);
			bzero(&bfi, sizeof(bfi));
			bfi.size = 8192 * 1024;
		}
	}
	thr->td = NULL;
	hammer2_thr_signal(thr, HAMMER2_THREAD_STOPPED);
}

int
hammer2_bulkfree_pass(hammer2_dev_t *hmp, hammer2_chain_t *vchain,
		      hammer2_ioc_bulkfree_t *bfi)
{
	hammer2_bulkfree_info_t cbinfo;
	hammer2_chain_save_t *save;
	hammer2_off_t incr;
	size_t size;
	int error;

	hammer2_dedup_clear(hmp);

	bzero(&cbinfo, sizeof(cbinfo));
	size = (bfi->size + HAMMER2_FREEMAP_LEVELN_PSIZE - 1) &
	       ~(size_t)(HAMMER2_FREEMAP_LEVELN_PSIZE - 1);

	if (size > kmem_lim_size() * 1024 * 1024 / 4) {
		size = kmem_lim_size() * 1024 * 1024 / 4;
		kprintf("hammer2: Warning: capping bulkfree buffer at %jdM\n",
			(intmax_t)size / (1024 * 1024));
	}

#define HAMMER2_FREEMAP_SIZEDIV	\
	(HAMMER2_FREEMAP_LEVEL1_SIZE / HAMMER2_FREEMAP_LEVELN_PSIZE)

	if (size > hmp->total_size / HAMMER2_FREEMAP_SIZEDIV)
		size = howmany(hmp->total_size, HAMMER2_FREEMAP_SIZEDIV);

	if (size < 1024 * 1024)
		size = 1024 * 1024;
	size = (size + HAMMER2_FREEMAP_LEVELN_PSIZE - 1) &
	       ~(size_t)(HAMMER2_FREEMAP_LEVELN_PSIZE - 1);

	cbinfo.hmp = hmp;
	cbinfo.bmap = kmem_alloc_swapbacked(&cbinfo.kp, size, VM_SUBSYS_HAMMER);
	cbinfo.dedup = kmalloc(sizeof(*cbinfo.dedup) * HAMMER2_DEDUP_HEUR_SIZE,
			       M_HAMMER2, M_WAITOK | M_ZERO);

	kprintf("hammer2: bulkfree buf=%jdM\n",
		(intmax_t)size / (1024 * 1024));

	cbinfo.sbase = bfi->sbase;
	if (cbinfo.sbase > hmp->total_size)
		cbinfo.sbase = hmp->total_size;
	cbinfo.sbase &= ~HAMMER2_FREEMAP_LEVEL1_MASK;
	TAILQ_INIT(&cbinfo.list);

	cbinfo.bulkfree_ticks = ticks;

	error = 0;
	while (cbinfo.sbase < hmp->total_size) {
		int allmedia;

		cbinfo_bmap_init(&cbinfo, size);
		bzero(cbinfo.dedup, sizeof(*cbinfo.dedup) *
				    HAMMER2_DEDUP_HEUR_SIZE);
		cbinfo.count_inodes_scanned = 0;
		cbinfo.count_dirents_scanned = 0;
		cbinfo.count_bytes_scanned = 0;
		cbinfo.count_chains_scanned = 0;
		cbinfo.count_chains_reported = 0;

		incr = size / HAMMER2_FREEMAP_LEVELN_PSIZE *
		       HAMMER2_FREEMAP_LEVEL1_SIZE;
		if (hmp->total_size - cbinfo.sbase <= incr) {
			cbinfo.sstop = hmp->total_size;
			allmedia = 1;
		} else {
			cbinfo.sstop = cbinfo.sbase + incr;
			allmedia = 0;
		}
		kprintf("hammer2: pass %016jx-%016jx ",
			(intmax_t)cbinfo.sbase,
			(intmax_t)cbinfo.sstop);
		if (allmedia && cbinfo.sbase == 0)
			kprintf("(all media)\n");
		else if (allmedia)
			kprintf("(remaining media)\n");
		else
			kprintf("(%jdGB of media)\n",
				(intmax_t)incr / (1024L*1024*1024));

#undef HAMMER2_BULKFREE_TRANS
#ifdef HAMMER2_BULKFREE_TRANS
		hammer2_trans_init(hmp->spmp, 0);
		cbinfo.mtid = hammer2_trans_sub(hmp->spmp);
#else
		cbinfo.mtid = 0;
#endif
		cbinfo.pri = 0;
		error |= hammer2_bulkfree_scan(vchain,
					       h2_bulkfree_callback, &cbinfo);

		while ((save = TAILQ_FIRST(&cbinfo.list)) != NULL &&
		       (error & ~HAMMER2_ERROR_CHECK) == 0) {
			TAILQ_REMOVE(&cbinfo.list, save, entry);
			--cbinfo.list_count;
			cbinfo.pri = 0;
			cbinfo.backout = NULL;
			error |= hammer2_bulkfree_scan(save->chain,
						       h2_bulkfree_callback,
						       &cbinfo);
			hammer2_chain_drop(save->chain);
			kfree(save, M_HAMMER2);
		}
		while (save) {
			TAILQ_REMOVE(&cbinfo.list, save, entry);
			--cbinfo.list_count;
			hammer2_chain_drop(save->chain);
			kfree(save, M_HAMMER2);
			save = TAILQ_FIRST(&cbinfo.list);
		}
		cbinfo.backout = NULL;

		if (error & ~HAMMER2_ERROR_CHECK) {
			kprintf("bulkfree lastdrop %d %d error=0x%04x\n",
				vchain->refs, vchain->core.chain_count, error);
		} else {
			if (error & HAMMER2_ERROR_CHECK) {
				kprintf("bulkfree lastdrop %d %d "
					"(with check errors)\n",
					vchain->refs, vchain->core.chain_count);
			} else {
				kprintf("bulkfree lastdrop %d %d\n",
					vchain->refs, vchain->core.chain_count);
			}

			error = h2_bulkfree_sync(&cbinfo);

			hammer2_voldata_lock(hmp);
			hammer2_voldata_modify(hmp);
			hmp->voldata.allocator_free += cbinfo.adj_free;
			hammer2_voldata_unlock(hmp);
		}

#ifdef HAMMER2_BULKFREE_TRANS
		hammer2_trans_done(hmp->spmp, 0);
#endif
		if (error & ~HAMMER2_ERROR_CHECK)
			break;
		cbinfo.sbase = cbinfo.sstop;
		cbinfo.adj_free = 0;
	}
	kmem_free_swapbacked(&cbinfo.kp);
	kfree(cbinfo.dedup, M_HAMMER2);
	cbinfo.dedup = NULL;

	bfi->sstop = cbinfo.sbase;

	incr = bfi->sstop / (hmp->total_size / 10000);
	if (incr > 10000)
		incr = 10000;

	kprintf("bulkfree pass statistics (%d.%02d%% storage processed):\n",
		(int)incr / 100,
		(int)incr % 100);

	if (error & ~HAMMER2_ERROR_CHECK) {
		kprintf("    bulkfree was aborted\n");
	} else {
		if (error & HAMMER2_ERROR_CHECK) {
			kprintf("    WARNING: bulkfree "
				"encountered CRC errors\n");
		}
		kprintf("    transition->free   %ld\n", cbinfo.count_10_00);
		kprintf("    transition->staged %ld\n", cbinfo.count_11_10);
		kprintf("    ERR(00)->allocated %ld\n", cbinfo.count_00_11);
		kprintf("    ERR(01)->allocated %ld\n", cbinfo.count_01_11);
		kprintf("    staged->allocated  %ld\n", cbinfo.count_10_11);
		kprintf("    ~4MB segs cleaned  %ld\n", cbinfo.count_l0cleans);
		kprintf("    linear adjusts     %ld\n",
			cbinfo.count_linadjusts);
		kprintf("    dedup factor       %ld\n",
			cbinfo.count_dedup_factor);
		kprintf("    max saved chains   %ld\n", cbinfo.list_count_max);
	}

	return error;
}

static void
cbinfo_bmap_init(hammer2_bulkfree_info_t *cbinfo, size_t size)
{
	hammer2_bmap_data_t *bmap = cbinfo->bmap;
	hammer2_key_t key = cbinfo->sbase;
	hammer2_key_t lokey;
	hammer2_key_t hikey;

	lokey = (cbinfo->hmp->voldata.allocator_beg + HAMMER2_SEGMASK64) &
		~HAMMER2_SEGMASK64;
	hikey = cbinfo->hmp->total_size & ~HAMMER2_SEGMASK64;

	bzero(bmap, size);
	while (size) {
		bzero(bmap, sizeof(*bmap));
		if (lokey < H2FMBASE(key, HAMMER2_FREEMAP_LEVEL1_RADIX))
			lokey = H2FMBASE(key, HAMMER2_FREEMAP_LEVEL1_RADIX);
		if (lokey < H2FMZONEBASE(key) + HAMMER2_ZONE_SEG64)
			lokey = H2FMZONEBASE(key) + HAMMER2_ZONE_SEG64;
		if (key < lokey || key >= hikey) {
			memset(bmap->bitmapq, -1,
			       sizeof(bmap->bitmapq));
			bmap->avail = 0;
			bmap->linear = HAMMER2_SEGSIZE;
		} else {
			bmap->avail = HAMMER2_FREEMAP_LEVEL0_SIZE;
		}
		size -= sizeof(*bmap);
		key += HAMMER2_FREEMAP_LEVEL0_SIZE;
		++bmap;
	}
}

static int
h2_bulkfree_callback(hammer2_bulkfree_info_t *cbinfo, hammer2_blockref_t *bref)
{
	hammer2_bmap_data_t *bmap;
	hammer2_off_t data_off;
	uint16_t class;
	size_t bytes;
	int radix;

	if (hammer2_signal_check(&cbinfo->save_time))
		return HAMMER2_ERROR_ABORTED;

	if (bref->type != HAMMER2_BREF_TYPE_DATA &&
	    bref->type != HAMMER2_BREF_TYPE_DIRENT) {
		++cbinfo->bulkfree_calls;
		if (cbinfo->bulkfree_calls > hammer2_bulkfree_tps) {
			int dticks = ticks - cbinfo->bulkfree_ticks;
			if (dticks < 0)
				dticks = 0;
			if (dticks < hz) {
				tsleep(&cbinfo->bulkfree_ticks, 0,
				       "h2bw", hz - dticks);
			}
			cbinfo->bulkfree_calls = 0;
			cbinfo->bulkfree_ticks = ticks;
		}
	}

	data_off = bref->data_off & ~HAMMER2_OFF_MASK_RADIX;
	if (data_off < cbinfo->sbase || data_off >= cbinfo->sstop)
		return 0;
	if (data_off < cbinfo->hmp->voldata.allocator_beg)
		return 0;
	if (data_off >= cbinfo->hmp->total_size)
		return 0;

	radix = (int)(bref->data_off & HAMMER2_OFF_MASK_RADIX);
	KKASSERT(radix != 0);
	bytes = (size_t)1 << radix;
	class = (bref->type << 8) | HAMMER2_PBUFRADIX;

	if (data_off + bytes > cbinfo->sstop) {
		kprintf("hammer2_bulkfree_scan: illegal 1GB boundary "
			"%016jx %016jx/%d\n",
			(intmax_t)bref->data_off,
			(intmax_t)bref->key,
			bref->keybits);
		bytes = cbinfo->sstop - data_off;
	}

	data_off -= cbinfo->sbase;
	bmap = cbinfo->bmap + (data_off >> HAMMER2_FREEMAP_LEVEL0_RADIX);

	data_off &= HAMMER2_FREEMAP_LEVEL0_MASK;
	if (data_off + bytes > HAMMER2_FREEMAP_LEVEL0_SIZE) {
		kprintf("hammer2_bulkfree_scan: illegal 4MB boundary "
			"%016jx %016jx/%d\n",
			(intmax_t)bref->data_off,
			(intmax_t)bref->key,
			bref->keybits);
		bytes = HAMMER2_FREEMAP_LEVEL0_SIZE - data_off;
	}

	if (bmap->class == 0) {
		bmap->class = class;
		bmap->avail = HAMMER2_FREEMAP_LEVEL0_SIZE;
	}

#if 0
	if (bmap->class != class) {
		kprintf("hammer2_bulkfree_scan: illegal mixed class "
			"%016jx %016jx/%d (%04x vs %04x)\n",
			(intmax_t)bref->data_off,
			(intmax_t)bref->key,
			bref->keybits,
			class, bmap->class);
	}
#endif

	if (bytes & HAMMER2_FREEMAP_BLOCK_MASK) {
		if (bmap->linear < (int32_t)data_off + (int32_t)bytes)
			bmap->linear = (int32_t)data_off + (int32_t)bytes;
	} else if (bmap->linear >= (int32_t)data_off &&
		   bmap->linear < (int32_t)data_off + (int32_t)bytes) {
		bmap->linear = (int32_t)data_off + (int32_t)bytes;
	}

	while (bytes > 0) {
		hammer2_bitmap_t bmask;
		int bindex;

		bindex = (int)data_off >> (HAMMER2_FREEMAP_BLOCK_RADIX +
					   HAMMER2_BMAP_INDEX_RADIX);
		bmask = (hammer2_bitmap_t)3 <<
			((((int)data_off & HAMMER2_BMAP_INDEX_MASK) >>
			 HAMMER2_FREEMAP_BLOCK_RADIX) << 1);

		if ((bmap->bitmapq[bindex] & bmask) == 0) {
			if (bytes < HAMMER2_FREEMAP_BLOCK_SIZE) {
				bmap->avail -= HAMMER2_FREEMAP_BLOCK_SIZE;
			} else {
				bmap->avail -= bytes;
			}
			bmap->bitmapq[bindex] |= bmask;
		}
		data_off += HAMMER2_FREEMAP_BLOCK_SIZE;
		if (bytes < HAMMER2_FREEMAP_BLOCK_SIZE)
			bytes = 0;
		else
			bytes -= HAMMER2_FREEMAP_BLOCK_SIZE;
	}
	return 0;
}

static int
h2_bulkfree_sync(hammer2_bulkfree_info_t *cbinfo)
{
	hammer2_off_t data_off;
	hammer2_key_t key;
	hammer2_key_t key_dummy;
	hammer2_bmap_data_t *bmap;
	hammer2_bmap_data_t *live;
	hammer2_chain_t *live_parent;
	hammer2_chain_t *live_chain;
	int bmapindex;
	int error;

	kprintf("hammer2_bulkfree - range ");

	if (cbinfo->sbase < cbinfo->hmp->voldata.allocator_beg)
		kprintf("%016jx-",
			(intmax_t)cbinfo->hmp->voldata.allocator_beg);
	else
		kprintf("%016jx-",
			(intmax_t)cbinfo->sbase);

	if (cbinfo->sstop > cbinfo->hmp->total_size)
		kprintf("%016jx\n",
			(intmax_t)cbinfo->hmp->total_size);
	else
		kprintf("%016jx\n",
			(intmax_t)cbinfo->sstop);

	data_off = cbinfo->sbase;
	bmap = cbinfo->bmap;

	live_parent = &cbinfo->hmp->fchain;
	hammer2_chain_ref(live_parent);
	hammer2_chain_lock(live_parent, HAMMER2_RESOLVE_ALWAYS);
	live_chain = NULL;
	error = 0;

	while (data_off < cbinfo->sstop) {

		if (data_off < cbinfo->hmp->voldata.allocator_beg)
			goto next;
		if (data_off >= cbinfo->hmp->total_size)
			goto next;

		key = (data_off & ~HAMMER2_FREEMAP_LEVEL1_MASK);

		if (live_chain == NULL || live_chain->bref.key != key) {
			if (live_chain) {
				hammer2_chain_unlock(live_chain);
				hammer2_chain_drop(live_chain);
			}
			live_chain = hammer2_chain_lookup(
					    &live_parent,
					    &key_dummy,
					    key,
					    key + HAMMER2_FREEMAP_LEVEL1_MASK,
					    &error,
					    HAMMER2_LOOKUP_ALWAYS);
			if (error) {
				kprintf("hammer2_bulkfree: freemap lookup "
					"error near %016jx, error %s\n",
					(intmax_t)data_off,
					hammer2_error_str(live_chain->error));
				break;
			}
		}
		if (live_chain == NULL) {
			if (bmap->class &&
			    bmap->avail != HAMMER2_FREEMAP_LEVEL0_SIZE) {
				kprintf("hammer2_bulkfree: cannot locate "
					"live leaf for allocated data "
					"near %016jx\n",
					(intmax_t)data_off);
			}
			goto next;
		}
		if (live_chain->error) {
			kprintf("hammer2_bulkfree: unable to access freemap "
				"near %016jx, error %s\n",
				(intmax_t)data_off,
				hammer2_error_str(live_chain->error));
			hammer2_chain_unlock(live_chain);
			hammer2_chain_drop(live_chain);
			live_chain = NULL;
			goto next;
		}

		bmapindex = (data_off & HAMMER2_FREEMAP_LEVEL1_MASK) >>
			    HAMMER2_FREEMAP_LEVEL0_RADIX;
		live = &live_chain->data->bmdata[bmapindex];

		if (bcmp(live->bitmapq, bmap->bitmapq,
			 sizeof(bmap->bitmapq)) == 0 &&
		    live->linear >= bmap->linear &&
		    (hammer2_aux_flags & 1) == 0 &&
		    bigmask_good(bmap, live_chain->bref.check.freemap.bigmask))
		{
			goto next;
		}
		if (hammer2_debug & 1) {
			kprintf("live %016jx %04d.%04x (avail=%d) "
				"bigmask %08x->%08x\n",
				data_off, bmapindex, live->class, live->avail,
				live_chain->bref.check.freemap.bigmask,
				live_chain->bref.check.freemap.bigmask |
				bigmask_get(bmap));
		}

		if (hammer2_chain_modify(live_chain, cbinfo->mtid, 0, 0)) {
			kprintf("hammer2_bulkfree: unable to modify freemap "
				"at %016jx for data-block %016jx, error %s\n",
				live_chain->bref.data_off,
				(intmax_t)data_off,
				hammer2_error_str(live_chain->error));
			hammer2_chain_unlock(live_chain);
			hammer2_chain_drop(live_chain);
			live_chain = NULL;
			goto next;
		}
		live_chain->bref.check.freemap.bigmask = -1;
		cbinfo->hmp->freemap_relaxed = 0;
		live = &live_chain->data->bmdata[bmapindex];

		h2_bulkfree_sync_adjust(cbinfo, data_off, live, bmap,
					live_chain->bref.key +
					bmapindex *
					HAMMER2_FREEMAP_LEVEL0_SIZE);
next:
		data_off += HAMMER2_FREEMAP_LEVEL0_SIZE;
		++bmap;
	}
	if (live_chain) {
		hammer2_chain_unlock(live_chain);
		hammer2_chain_drop(live_chain);
	}
	if (live_parent) {
		hammer2_chain_unlock(live_parent);
		hammer2_chain_drop(live_parent);
	}
	return error;
}

static
void
h2_bulkfree_sync_adjust(hammer2_bulkfree_info_t *cbinfo,
			hammer2_off_t data_off, hammer2_bmap_data_t *live,
			hammer2_bmap_data_t *bmap, hammer2_key_t alloc_base)
{
	int bindex;
	int scount;
	hammer2_off_t tmp_off;
	hammer2_bitmap_t lmask;
	hammer2_bitmap_t mmask;

	tmp_off = data_off;

	for (bindex = 0; bindex < HAMMER2_BMAP_ELEMENTS; ++bindex) {
		lmask = live->bitmapq[bindex];
		mmask = bmap->bitmapq[bindex];
		if (lmask == mmask) {
			tmp_off += HAMMER2_BMAP_INDEX_SIZE;
			continue;
		}

		for (scount = 0;
		     scount < HAMMER2_BMAP_BITS_PER_ELEMENT;
		     scount += 2) {
			if ((mmask & 3) == 0) {
				switch (lmask & 3) {
				case 0:
					break;
				case 1:
					kprintf("hammer2_bulkfree: cannot "
						"transition m=00/l=01\n");
					break;
				case 2:
					live->bitmapq[bindex] &=
					    ~((hammer2_bitmap_t)2 << scount);
					live->avail +=
						HAMMER2_FREEMAP_BLOCK_SIZE;
					if (live->avail >
					    HAMMER2_FREEMAP_LEVEL0_SIZE) {
						live->avail =
						    HAMMER2_FREEMAP_LEVEL0_SIZE;
					}
					cbinfo->adj_free +=
						HAMMER2_FREEMAP_BLOCK_SIZE;
					++cbinfo->count_10_00;
					hammer2_io_dedup_assert(
						cbinfo->hmp,
						tmp_off |
						HAMMER2_FREEMAP_BLOCK_RADIX,
						HAMMER2_FREEMAP_BLOCK_SIZE);
					break;
				case 3:
					live->bitmapq[bindex] &=
					    ~((hammer2_bitmap_t)1 << scount);
					++cbinfo->count_11_10;
					hammer2_io_dedup_delete(
						cbinfo->hmp,
						HAMMER2_BREF_TYPE_DATA,
						tmp_off |
						HAMMER2_FREEMAP_BLOCK_RADIX,
						HAMMER2_FREEMAP_BLOCK_SIZE);
					break;
				}
			} else if ((mmask & 3) == 3) {
				switch (lmask & 3) {
				case 0:
					kprintf("hammer2_bulkfree: "
						"00->11 critical freemap "
						"transition for datablock "
						"%016jx\n",
						tmp_off);
					++cbinfo->count_00_11;
					cbinfo->adj_free -=
						HAMMER2_FREEMAP_BLOCK_SIZE;
					live->avail -=
						HAMMER2_FREEMAP_BLOCK_SIZE;
					if ((int32_t)live->avail < 0)
						live->avail = 0;
					break;
				case 1:
					++cbinfo->count_01_11;
					break;
				case 2:
					++cbinfo->count_10_11;
					break;
				case 3:
					break;
				}
				live->bitmapq[bindex] |=
					((hammer2_bitmap_t)3 << scount);
			}
			mmask >>= 2;
			lmask >>= 2;
			tmp_off += HAMMER2_FREEMAP_BLOCK_SIZE;
		}
	}

	for (bindex = HAMMER2_BMAP_ELEMENTS - 1; bindex >= 0; --bindex) {
		if (live->bitmapq[bindex] != 0)
			break;
	}
	if (bindex < 0) {
#if 0
		kprintf("hammer2: cleanseg %016jx.%04x (%d)\n",
			alloc_base, live->class, live->avail);
#endif
		live->avail = HAMMER2_FREEMAP_LEVEL0_SIZE;
		live->class = 0;
		live->linear = 0;
		++cbinfo->count_l0cleans;
	} else if (bindex < 7) {
		if (live->linear < bmap->linear &&
		    ((live->linear ^ bmap->linear) &
		     ~HAMMER2_FREEMAP_BLOCK_MASK) == 0) {
			live->linear = bmap->linear;
			++cbinfo->count_linadjusts;
		} else {
			live->linear =
				(bmap->linear + HAMMER2_FREEMAP_BLOCK_MASK) &
				~HAMMER2_FREEMAP_BLOCK_MASK;
			++cbinfo->count_linadjusts;
		}
	} else {
		live->linear = HAMMER2_SEGSIZE;
	}

#if 0
	if (bmap->class) {
		kprintf("%016jx %04d.%04x (avail=%7d) "
			"%08x %08x %08x %08x %08x %08x %08x %08x\n",
			(intmax_t)data_off,
			(int)((data_off &
			       HAMMER2_FREEMAP_LEVEL1_MASK) >>
			      HAMMER2_FREEMAP_LEVEL0_RADIX),
			bmap->class,
			bmap->avail,
			bmap->bitmap[0], bmap->bitmap[1],
			bmap->bitmap[2], bmap->bitmap[3],
			bmap->bitmap[4], bmap->bitmap[5],
			bmap->bitmap[6], bmap->bitmap[7]);
	}
#endif
}

static
int
h2_bulkfree_test(hammer2_bulkfree_info_t *cbinfo, hammer2_blockref_t *bref,
		 int pri, int saved_error)
{
	hammer2_dedup_t *dedup;
	int best;
	int n;
	int i;

	n = hammer2_icrc32(&bref->data_off, sizeof(bref->data_off));
	dedup = cbinfo->dedup + (n & (HAMMER2_DEDUP_HEUR_MASK & ~7));

	for (i = best = 0; i < 8; ++i) {
		if (dedup[i].data_off == bref->data_off) {
			if (dedup[i].ticks < pri)
				dedup[i].ticks = pri;
			if (pri == 1)
				cbinfo->count_dedup_factor += dedup[i].ticks;
			return (dedup[i].saved_error | HAMMER2_ERROR_EOF);
		}
		if (dedup[i].ticks < dedup[best].ticks)
			best = i;
	}
	dedup[best].data_off = bref->data_off;
	dedup[best].ticks = pri;
	dedup[best].saved_error = saved_error;

	return 0;
}

static uint32_t
bigmask_get(hammer2_bmap_data_t *bmap)
{
	hammer2_bitmap_t mask;
	hammer2_bitmap_t scan;
	uint32_t bigmask;
	uint32_t radix_mask;
	int iter;
	int i;
	int j;

	bigmask = 0;
	for (i = 0; i < HAMMER2_BMAP_ELEMENTS; ++i) {
		mask = bmap->bitmapq[i];

		radix_mask = 1U << HAMMER2_FREEMAP_BLOCK_RADIX;
		radix_mask |= radix_mask - 1;
		iter = 2;
		while (iter <= HAMMER2_BMAP_BITS_PER_ELEMENT) {
			if (iter == HAMMER2_BMAP_BITS_PER_ELEMENT)
				scan = -1;
			else
				scan = (1LU << iter) - 1;
			j = 0;
			while (j < HAMMER2_BMAP_BITS_PER_ELEMENT) {
				if ((scan & mask) == 0) {
					bigmask |= radix_mask;
				}
				scan <<= iter;
				j += iter;
			}
			iter <<= 1;
			radix_mask = (radix_mask << 1) | 1;
		}
	}
	return bigmask;
}

static int
bigmask_good(hammer2_bmap_data_t *bmap, uint32_t live_bigmask)
{
	uint32_t bigmask;

	bigmask = bigmask_get(bmap);
	return ((live_bigmask & bigmask) == bigmask);
}
