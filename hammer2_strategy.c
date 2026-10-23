#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/buf.h>
#include <sys/proc.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/objcache.h>

#include "hammer2.h"
#include "hammer2_lz4.h"

#include "zlib/hammer2_zlib.h"

struct objcache *cache_buffer_read;
struct objcache *cache_buffer_write;

static int hammer2_strategy_read(struct vop_strategy_args *ap);
static int hammer2_strategy_write(struct vop_strategy_args *ap);
static void hammer2_strategy_read_completion(hammer2_chain_t *focus,
				const char *data, struct bio *bio);

static hammer2_off_t hammer2_dedup_lookup(hammer2_dev_t *hmp,
			char **datap, int pblksize);

int
hammer2_vop_strategy(struct vop_strategy_args *ap)
{
	struct bio *biop;
	struct buf *bp;
	int error;

	biop = ap->a_bio;
	bp = biop->bio_buf;

	switch(bp->b_cmd) {
	case BUF_CMD_READ:
		error = hammer2_strategy_read(ap);
		break;
	case BUF_CMD_WRITE:
		error = hammer2_strategy_write(ap);
		break;
	default:
		bp->b_error = error = EINVAL;
		bp->b_flags |= B_ERROR;
		biodone(biop);
		break;
	}
	return (error);
}

static int
hammer2_vop_bmap_impl(struct vop_bmap_args *ap)
{
	hammer2_xop_bmap_t *xop;
	hammer2_inode_t *ip;
	int error;

	ip = VTOI(ap->a_vp);

	if (ap->a_doffsetp == NULL)
		return (0);
	if (ap->a_runp)
		*ap->a_runp = 0;
	if (ap->a_runb)
		*ap->a_runb = 0;

	xop = hammer2_xop_alloc(ip, 0);
	xop->loffset = ap->a_loffset;
	hammer2_xop_start(&xop->head, &hammer2_bmap_desc);
	error = hammer2_xop_collect(&xop->head, 0);
	error = hammer2_error_to_errno(error);
	if (error) {
		if (error == ENOENT)
			error = 0;
		*ap->a_doffsetp = NOOFFSET;
	} else {
		KKASSERT(xop->offset != HAMMER2_OFF_MASK);
		*ap->a_doffsetp = xop->offset;
	}
	hammer2_xop_retire(&xop->head, HAMMER2_XOPMASK_VOP);

	return (error);
}

int
hammer2_vop_bmap(struct vop_bmap_args *ap)
{
	if (ap->a_cmd == BUF_CMD_SEEK)
		return (hammer2_vop_bmap_impl(ap));

	*ap->a_doffsetp = NOOFFSET;
	if (ap->a_runp)
		*ap->a_runp = 0;
	if (ap->a_runb)
		*ap->a_runb = 0;
	return (EOPNOTSUPP);
}

static
void
hammer2_decompress_LZ4_callback(const char *data, u_int bytes, struct bio *bio)
{
	struct buf *bp;
	char *compressed_buffer;
	int compressed_size;
	int result;

	bp = bio->bio_buf;

#if 0
	if bio->bio_caller_info2.index &&
	      bio->bio_caller_info1.uvalue32 !=
	      crc32(bp->b_data, bp->b_bufsize) --- return error
#endif

	KKASSERT(bp->b_bufsize <= HAMMER2_PBUFSIZE);
	compressed_size = *(const int *)data;
	KKASSERT((uint32_t)compressed_size <= bytes - sizeof(int));

	compressed_buffer = objcache_get(cache_buffer_read, M_INTWAIT);
	result = LZ4_decompress_safe(__DECONST(char *, &data[sizeof(int)]),
				     compressed_buffer,
				     compressed_size,
				     bp->b_bufsize);
	if (result < 0) {
		kprintf("READ PATH: Error during decompression."
			"bio %016jx/%d\n",
			(intmax_t)bio->bio_offset, bytes);
		bzero(compressed_buffer, bp->b_bufsize);
	}
	KKASSERT(result <= bp->b_bufsize);
	bcopy(compressed_buffer, bp->b_data, bp->b_bufsize);
	if (result < bp->b_bufsize)
		bzero(bp->b_data + result, bp->b_bufsize - result);
	objcache_put(cache_buffer_read, compressed_buffer);
	bp->b_resid = 0;
	bp->b_flags |= B_AGE;
}

static
void
hammer2_decompress_ZLIB_callback(const char *data, u_int bytes, struct bio *bio)
{
	struct buf *bp;
	char *compressed_buffer;
	z_stream strm_decompress;
	int result;
	int ret;

	bp = bio->bio_buf;

	KKASSERT(bp->b_bufsize <= HAMMER2_PBUFSIZE);
	bzero(&strm_decompress, sizeof(strm_decompress));
	strm_decompress.avail_in = 0;
	strm_decompress.next_in = Z_NULL;

	ret = inflateInit(&strm_decompress);

	if (ret != Z_OK)
		kprintf("HAMMER2 ZLIB: Fatal error in inflateInit.\n");

	compressed_buffer = objcache_get(cache_buffer_read, M_INTWAIT);
	strm_decompress.next_in = __DECONST(char *, data);

	strm_decompress.avail_in = bytes;
	strm_decompress.next_out = compressed_buffer;
	strm_decompress.avail_out = bp->b_bufsize;

	ret = inflate(&strm_decompress, Z_FINISH);
	if (ret != Z_STREAM_END) {
		kprintf("HAMMER2 ZLIB: Fatal error during decompression.\n");
		bzero(compressed_buffer, bp->b_bufsize);
	}
	bcopy(compressed_buffer, bp->b_data, bp->b_bufsize);
	result = bp->b_bufsize - strm_decompress.avail_out;
	if (result < bp->b_bufsize)
		bzero(bp->b_data + result, strm_decompress.avail_out);
	objcache_put(cache_buffer_read, compressed_buffer);
	ret = inflateEnd(&strm_decompress);

	bp->b_resid = 0;
	bp->b_flags |= B_AGE;
}

static
int
hammer2_strategy_read(struct vop_strategy_args *ap)
{
	hammer2_xop_strategy_t *xop;
	struct bio *bio;
	hammer2_inode_t *ip;
	hammer2_key_t lbase;

	bio = ap->a_bio;
	ip = VTOI(ap->a_vp);

	lbase = bio->bio_offset;
	KKASSERT(((int)lbase & HAMMER2_PBUFMASK) == 0);

	xop = hammer2_xop_alloc(ip, HAMMER2_XOP_STRATEGY);
	xop->finished = 0;
	xop->bio = bio;
	xop->lbase = lbase;
	hammer2_mtx_init(&xop->lock, "h2bior");
	hammer2_xop_start(&xop->head, &hammer2_strategy_read_desc);

	return(0);
}

void
hammer2_xop_strategy_read(hammer2_xop_t *arg, void *scratch, int clindex)
{
	hammer2_xop_strategy_t *xop = &arg->xop_strategy;
	hammer2_chain_t *parent;
	hammer2_chain_t *chain;
	hammer2_chain_t *focus;
	hammer2_key_t key_dummy;
	hammer2_key_t lbase;
	struct bio *bio;
	struct buf *bp;
	const char *data;
	int error;

	lbase = xop->lbase;

	parent = hammer2_inode_chain(xop->head.ip1, clindex,
				     HAMMER2_RESOLVE_ALWAYS |
				     HAMMER2_RESOLVE_SHARED);
	if (parent) {
		chain = hammer2_chain_lookup(&parent, &key_dummy,
					     lbase, lbase,
					     &error,
					     HAMMER2_LOOKUP_ALWAYS |
					     HAMMER2_LOOKUP_SHARED);
		if (chain)
			error = chain->error;
	} else {
		error = HAMMER2_ERROR_EIO;
		chain = NULL;
	}
	hammer2_xop_feed(&xop->head, chain, clindex, error);
	if (chain) {
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
	}
	if (parent) {
		hammer2_chain_unlock(parent);
		hammer2_chain_drop(parent);
	}
	chain = NULL;
	parent = NULL;

	if (xop->finished)
		return;
	hammer2_mtx_ex(&xop->lock);
	if (xop->finished) {
		hammer2_mtx_unlock(&xop->lock);
		return;
	}
	bio = xop->bio;
	bp = bio->bio_buf;
	bkvasync(bp);

	error = hammer2_xop_collect(&xop->head, HAMMER2_XOP_COLLECT_NOWAIT);

	switch(error) {
	case 0:
		xop->finished = 1;
		hammer2_mtx_unlock(&xop->lock);
		bp->b_flags |= B_NOTMETA;
		focus = xop->head.cluster.focus;
		data = hammer2_xop_gdata(&xop->head)->buf;
		hammer2_strategy_read_completion(focus, data, xop->bio);
		hammer2_xop_pdata(&xop->head);
		biodone(bio);
		hammer2_xop_retire(&xop->head, HAMMER2_XOPMASK_VOP);
		break;
	case HAMMER2_ERROR_ENOENT:
		xop->finished = 1;
		hammer2_mtx_unlock(&xop->lock);
		bp->b_flags |= B_NOTMETA;
		bp->b_resid = 0;
		bp->b_error = 0;
		bzero(bp->b_data, bp->b_bcount);
		biodone(bio);
		hammer2_xop_retire(&xop->head, HAMMER2_XOPMASK_VOP);
		break;
	case HAMMER2_ERROR_EINPROGRESS:
		hammer2_mtx_unlock(&xop->lock);
		break;
	default:
		kprintf("xop_strategy_read: error %08x loff=%016jx\n",
			error, (intmax_t)bp->b_loffset);
		xop->finished = 1;
		hammer2_mtx_unlock(&xop->lock);
		bp->b_flags |= B_ERROR;
		bp->b_error = EIO;
		biodone(bio);
		hammer2_xop_retire(&xop->head, HAMMER2_XOPMASK_VOP);
		break;
	}
}

static
void
hammer2_strategy_read_completion(hammer2_chain_t *focus, const char *data,
				 struct bio *bio)
{
	struct buf *bp = bio->bio_buf;

	if (focus->bref.type == HAMMER2_BREF_TYPE_INODE) {
		bcopy(((const hammer2_inode_data_t *)data)->u.data,
		      bp->b_data, HAMMER2_EMBEDDED_BYTES);
		bzero(bp->b_data + HAMMER2_EMBEDDED_BYTES,
		      bp->b_bcount - HAMMER2_EMBEDDED_BYTES);
		bp->b_resid = 0;
		bp->b_error = 0;
	} else if (focus->bref.type == HAMMER2_BREF_TYPE_DATA) {
		hammer2_dedup_record(focus, NULL, data);
		atomic_set_int(&focus->flags, HAMMER2_CHAIN_RELEASE);

		switch (HAMMER2_DEC_COMP(focus->bref.methods)) {
		case HAMMER2_COMP_LZ4:
			hammer2_decompress_LZ4_callback(data, focus->bytes,
							bio);
			break;
		case HAMMER2_COMP_ZLIB:
			hammer2_decompress_ZLIB_callback(data, focus->bytes,
							 bio);
			break;
		case HAMMER2_COMP_NONE:
			KKASSERT(focus->bytes <= bp->b_bcount);
			bcopy(data, bp->b_data, focus->bytes);
			if (focus->bytes < bp->b_bcount) {
				bzero(bp->b_data + focus->bytes,
				      bp->b_bcount - focus->bytes);
			}
			bp->b_resid = 0;
			bp->b_error = 0;
			break;
		default:
			panic("hammer2_strategy_read_completion: "
			      "unknown compression type");
		}
	} else {
		panic("hammer2_strategy_read_completion: unknown bref type");
	}
}

static void hammer2_write_file_core(char *data, hammer2_inode_t *ip,
				hammer2_chain_t **parentp,
				hammer2_key_t lbase, int ioflag, int pblksize,
				hammer2_tid_t mtid, int *errorp);
static void hammer2_compress_and_write(char *data, hammer2_inode_t *ip,
				hammer2_chain_t **parentp,
				hammer2_key_t lbase, int ioflag, int pblksize,
				hammer2_tid_t mtid, int *errorp,
				int comp_algo, int check_algo);
static void hammer2_zero_check_and_write(char *data, hammer2_inode_t *ip,
				hammer2_chain_t **parentp,
				hammer2_key_t lbase, int ioflag, int pblksize,
				hammer2_tid_t mtid, int *errorp,
				int check_algo);
static int test_block_zeros(const char *buf, size_t bytes);
static void zero_write(char *data, hammer2_inode_t *ip,
				hammer2_chain_t **parentp,
				hammer2_key_t lbase,
				hammer2_tid_t mtid, int *errorp);
static void hammer2_write_bp(hammer2_chain_t *chain, char *data,
				int ioflag, int pblksize,
				hammer2_tid_t mtid, int *errorp,
				int check_algo);

int
hammer2_strategy_write(struct vop_strategy_args *ap)
{
	hammer2_xop_strategy_t *xop;
	hammer2_pfs_t *pmp;
	struct bio *bio;
	hammer2_inode_t *ip;

	bio = ap->a_bio;
	ip = VTOI(ap->a_vp);
	pmp = ip->pmp;

	atomic_set_int(&ip->flags, HAMMER2_INODE_DIRTYDATA);
	hammer2_lwinprog_ref(pmp);
	hammer2_trans_assert_strategy(pmp);
	hammer2_trans_init(pmp, HAMMER2_TRANS_BUFCACHE);

	xop = hammer2_xop_alloc(ip, HAMMER2_XOP_MODIFYING |
				    HAMMER2_XOP_STRATEGY);
	xop->finished = 0;
	xop->bio = bio;
	xop->lbase = bio->bio_offset;
	hammer2_mtx_init(&xop->lock, "h2biow");
	hammer2_xop_start(&xop->head, &hammer2_strategy_write_desc);

	hammer2_lwinprog_wait(pmp, hammer2_flush_pipe);

	return(0);
}

void
hammer2_xop_strategy_write(hammer2_xop_t *arg, void *scratch, int clindex)
{
	hammer2_xop_strategy_t *xop = &arg->xop_strategy;
	hammer2_chain_t *parent;
	hammer2_key_t lbase;
	hammer2_inode_t *ip;
	struct bio *bio;
	struct buf *bp;
	int error;
	int lblksize;
	int pblksize;
	char *bio_data;

	if (xop->finished)
		return;
	hammer2_mtx_sh(&xop->lock);
	if (xop->finished) {
		hammer2_mtx_unlock(&xop->lock);
		return;
	}

	lbase = xop->lbase;
	bio = xop->bio;
	bp = bio->bio_buf;
	ip = xop->head.ip1;
	bio_data = scratch;

	lblksize = hammer2_calc_logical(ip, bio->bio_offset, &lbase, NULL);
	pblksize = hammer2_calc_physical(ip, lbase);
	bkvasync(bp);
	KKASSERT(lblksize <= MAXPHYS);
	bcopy(bp->b_data, bio_data, lblksize);

	hammer2_mtx_unlock(&xop->lock);
	bp = NULL;
	bio = NULL;

	parent = hammer2_inode_chain(ip, clindex, HAMMER2_RESOLVE_ALWAYS);
	hammer2_write_file_core(bio_data, ip, &parent,
				lbase, IO_ASYNC, pblksize,
				xop->head.mtid, &error);
	if (parent) {
		hammer2_chain_unlock(parent);
		hammer2_chain_drop(parent);
		parent = NULL;
	}
	hammer2_xop_feed(&xop->head, NULL, clindex, error);

	if (xop->finished)
		return;
	hammer2_mtx_ex(&xop->lock);
	if (xop->finished) {
		hammer2_mtx_unlock(&xop->lock);
		return;
	}

	error = hammer2_xop_collect(&xop->head, HAMMER2_XOP_COLLECT_NOWAIT);

	if (error == HAMMER2_ERROR_EINPROGRESS) {
		hammer2_mtx_unlock(&xop->lock);
		return;
	}

	xop->finished = 1;
	hammer2_mtx_unlock(&xop->lock);

	bio = xop->bio;
	bp = bio->bio_buf;

	if (error == HAMMER2_ERROR_ENOENT || error == 0) {
		bp->b_flags |= B_NOTMETA;
		bp->b_resid = 0;
		bp->b_error = 0;
		biodone(bio);
	} else {
		kprintf("xop_strategy_write: error %d loff=%016jx\n",
			error, (intmax_t)bp->b_loffset);
		bp->b_flags |= B_ERROR;
		bp->b_error = EIO;
		biodone(bio);
	}
	hammer2_xop_retire(&xop->head, HAMMER2_XOPMASK_VOP);
	hammer2_trans_assert_strategy(ip->pmp);
	hammer2_lwinprog_drop(ip->pmp);
	hammer2_trans_done(ip->pmp, HAMMER2_TRANS_BUFCACHE);
}

void
hammer2_bioq_sync(hammer2_pfs_t *pmp)
{
	hammer2_lwinprog_wait(pmp, 0);
}

static
hammer2_chain_t *
hammer2_assign_physical(hammer2_inode_t *ip, hammer2_chain_t **parentp,
			hammer2_key_t lbase, int pblksize,
			hammer2_tid_t mtid, char **datap, int *errorp)
{
	hammer2_chain_t *chain;
	hammer2_key_t key_dummy;
	hammer2_off_t dedup_off;
	int pradix = hammer2_getradix(pblksize);

	KKASSERT(pblksize >= HAMMER2_ALLOC_MIN);

	chain = hammer2_chain_lookup(parentp, &key_dummy,
				     lbase, lbase,
				     errorp,
				     HAMMER2_LOOKUP_NODATA);

	if (chain && (chain->flags & HAMMER2_CHAIN_DELETED) &&
	    chain->bref.type != HAMMER2_BREF_TYPE_INODE) {
		kprintf("assign physical deleted chain @ "
			"%016jx (%016jx.%02x) ip %016jx\n",
			lbase, chain->bref.data_off, chain->bref.type,
			ip->meta.inum);
		Debugger("bleh");
	}

	if (chain == NULL) {
		dedup_off = hammer2_dedup_lookup((*parentp)->hmp, datap,
						 pblksize);
		*errorp |= hammer2_chain_create(parentp, &chain, NULL, ip->pmp,
				       HAMMER2_ENC_CHECK(ip->meta.check_algo) |
				       HAMMER2_ENC_COMP(HAMMER2_COMP_NONE),
					        lbase, HAMMER2_PBUFRADIX,
					        HAMMER2_BREF_TYPE_DATA,
					        pblksize, mtid,
					        dedup_off, 0);
		if (chain == NULL)
			goto failed;
	} else if (chain->error == 0) {
		switch (chain->bref.type) {
		case HAMMER2_BREF_TYPE_INODE:
			*errorp |= hammer2_chain_modify_ip(ip, chain, mtid, 0);
			break;
		case HAMMER2_BREF_TYPE_DATA:
			dedup_off = hammer2_dedup_lookup(chain->hmp, datap,
							 pblksize);
			if (chain->bytes != pblksize) {
				*errorp |= hammer2_chain_resize(chain,
						     mtid, dedup_off,
						     pradix,
						     HAMMER2_MODIFY_OPTDATA);
				if (*errorp)
					break;
			}

			*errorp |= hammer2_chain_modify(chain, mtid, dedup_off,
						        HAMMER2_MODIFY_OPTDATA);
			break;
		default:
			panic("hammer2_assign_physical: bad type");
			break;
		}
	} else {
		*errorp = chain->error;
	}
	atomic_set_int(&ip->flags, HAMMER2_INODE_DIRTYDATA);
failed:
	return (chain);
}

static
void
hammer2_write_file_core(char *data, hammer2_inode_t *ip,
			hammer2_chain_t **parentp,
			hammer2_key_t lbase, int ioflag, int pblksize,
			hammer2_tid_t mtid, int *errorp)
{
	hammer2_chain_t *chain;
	char *bdata;

	*errorp = 0;

	switch(HAMMER2_DEC_ALGO(ip->meta.comp_algo)) {
	case HAMMER2_COMP_NONE:
		bdata = data;
		chain = hammer2_assign_physical(ip, parentp, lbase, pblksize,
						mtid, &bdata, errorp);
		if (*errorp) {
		} else if (chain->bref.type == HAMMER2_BREF_TYPE_INODE) {
			hammer2_inode_data_t *wipdata;

			wipdata = &chain->data->ipdata;
			KKASSERT(wipdata->meta.op_flags &
				 HAMMER2_OPFLAG_DIRECTDATA);
			bcopy(data, wipdata->u.data, HAMMER2_EMBEDDED_BYTES);
			++hammer2_iod_file_wembed;
		} else if (bdata == NULL) {
			chain->bref.methods =
				HAMMER2_ENC_COMP(HAMMER2_COMP_NONE) +
				HAMMER2_ENC_CHECK(ip->meta.check_algo);
			hammer2_chain_setcheck(chain, data);
			atomic_clear_int(&chain->flags, HAMMER2_CHAIN_INITIAL);
		} else {
			hammer2_write_bp(chain, data, ioflag, pblksize,
					 mtid, errorp, ip->meta.check_algo);
		}
		if (chain) {
			hammer2_chain_unlock(chain);
			hammer2_chain_drop(chain);
		}
		break;
	case HAMMER2_COMP_AUTOZERO:
		hammer2_zero_check_and_write(data, ip, parentp,
					     lbase, ioflag, pblksize,
					     mtid, errorp,
					     ip->meta.check_algo);
		break;
	case HAMMER2_COMP_LZ4:
	case HAMMER2_COMP_ZLIB:
	default:
		hammer2_compress_and_write(data, ip, parentp,
					   lbase, ioflag, pblksize,
					   mtid, errorp,
					   ip->meta.comp_algo,
					   ip->meta.check_algo);
		break;
	}
}

static
void
hammer2_compress_and_write(char *data, hammer2_inode_t *ip,
	hammer2_chain_t **parentp,
	hammer2_key_t lbase, int ioflag, int pblksize,
	hammer2_tid_t mtid, int *errorp, int comp_algo, int check_algo)
{
	hammer2_chain_t *chain;
	int comp_size;
	int comp_block_size;
	char *comp_buffer;
	char *bdata;

	if (check_algo != HAMMER2_CHECK_NONE &&
	    test_block_zeros(data, pblksize)) {
		zero_write(data, ip, parentp, lbase, mtid, errorp);
		return;
	}

	comp_size = 0;
	comp_buffer = NULL;

	KKASSERT(pblksize / 2 <= 32768);

	if (ip->comp_heuristic < 8 || (ip->comp_heuristic & 7) == 0 ||
	    hammer2_always_compress) {
		z_stream strm_compress;
		int comp_level;
		int ret;

		switch(HAMMER2_DEC_ALGO(comp_algo)) {
		case HAMMER2_COMP_LZ4:
			comp_buffer = objcache_get(cache_buffer_write,
						   M_INTWAIT);
			comp_size = LZ4_compress_limitedOutput(
					data,
					&comp_buffer[sizeof(int)],
					pblksize,
					pblksize / 2 - sizeof(int64_t));
			*(int *)comp_buffer = comp_size;
			if (comp_size)
				comp_size += sizeof(int);
			break;
		case HAMMER2_COMP_ZLIB:
			comp_level = HAMMER2_DEC_LEVEL(comp_algo);
			if (comp_level == 0)
				comp_level = 6;
			else if (comp_level < 6)
				comp_level = 6;
			else if (comp_level > 9)
				comp_level = 9;
			bzero(&strm_compress, sizeof(strm_compress));
			ret = deflateInit(&strm_compress, comp_level);
			if (ret != Z_OK) {
				kprintf("HAMMER2 ZLIB: fatal error "
					"on deflateInit.\n");
			}

			comp_buffer = objcache_get(cache_buffer_write,
						   M_INTWAIT);
			strm_compress.next_in = data;
			strm_compress.avail_in = pblksize;
			strm_compress.next_out = comp_buffer;
			strm_compress.avail_out = pblksize / 2;
			ret = deflate(&strm_compress, Z_FINISH);
			if (ret == Z_STREAM_END) {
				comp_size = pblksize / 2 -
					    strm_compress.avail_out;
			} else {
				comp_size = 0;
			}
			ret = deflateEnd(&strm_compress);
			break;
		default:
			kprintf("Error: Unknown compression method.\n");
			kprintf("Comp_method = %d.\n", comp_algo);
			break;
		}
	}

	if (comp_size == 0) {
		comp_block_size = pblksize;
		if (++ip->comp_heuristic > 128)
			ip->comp_heuristic = 8;
	} else {
		ip->comp_heuristic = 0;
		if (comp_size <= 1024) {
			comp_block_size = 1024;
		} else if (comp_size <= 2048) {
			comp_block_size = 2048;
		} else if (comp_size <= 4096) {
			comp_block_size = 4096;
		} else if (comp_size <= 8192) {
			comp_block_size = 8192;
		} else if (comp_size <= 16384) {
			comp_block_size = 16384;
		} else if (comp_size <= 32768) {
			comp_block_size = 32768;
		} else {
			panic("hammer2: WRITE PATH: "
			      "Weird comp_size value.");
			comp_block_size = pblksize;
		}

		if (comp_size < comp_block_size) {
			bzero(comp_buffer + comp_size,
			      comp_block_size - comp_size);
		}
	}

	bdata = comp_size ? comp_buffer : data;
	chain = hammer2_assign_physical(ip, parentp, lbase, comp_block_size,
					mtid, &bdata, errorp);

	if (*errorp) {
		goto done;
	}

	if (chain->bref.type == HAMMER2_BREF_TYPE_INODE) {
		hammer2_inode_data_t *wipdata;

		*errorp = hammer2_chain_modify_ip(ip, chain, mtid, 0);
		if (*errorp == 0) {
			wipdata = &chain->data->ipdata;
			KKASSERT(wipdata->meta.op_flags &
				 HAMMER2_OPFLAG_DIRECTDATA);
			bcopy(data, wipdata->u.data, HAMMER2_EMBEDDED_BYTES);
			++hammer2_iod_file_wembed;
		}
	} else if (bdata == NULL) {
		if (comp_size) {
			chain->bref.methods =
				HAMMER2_ENC_COMP(comp_algo) +
				HAMMER2_ENC_CHECK(check_algo);
		} else {
			chain->bref.methods =
				HAMMER2_ENC_COMP(
					HAMMER2_COMP_NONE) +
				HAMMER2_ENC_CHECK(check_algo);
		}
		bdata = comp_size ? comp_buffer : data;
		hammer2_chain_setcheck(chain, bdata);
		atomic_clear_int(&chain->flags, HAMMER2_CHAIN_INITIAL);
	} else {
		hammer2_io_t *dio;

		KKASSERT(chain->flags & HAMMER2_CHAIN_MODIFIED);

		switch(chain->bref.type) {
		case HAMMER2_BREF_TYPE_INODE:
			panic("hammer2_compress_and_write: unexpected inode\n");
			break;
		case HAMMER2_BREF_TYPE_DATA:
			*errorp = hammer2_io_newnz(chain->hmp,
						   chain->bref.type,
						   chain->bref.data_off,
						   chain->bytes,
						   &dio);
			if (*errorp) {
				hammer2_io_brelse(&dio);
				kprintf("hammer2: WRITE PATH: "
					"dbp bread error\n");
				break;
			}
			bdata = hammer2_io_data(dio, chain->bref.data_off);

			if (comp_size) {
				chain->bref.methods =
					HAMMER2_ENC_COMP(comp_algo) +
					HAMMER2_ENC_CHECK(check_algo);
				bcopy(comp_buffer, bdata, comp_block_size);
			} else {
				chain->bref.methods =
					HAMMER2_ENC_COMP(
						HAMMER2_COMP_NONE) +
					HAMMER2_ENC_CHECK(check_algo);
				bcopy(data, bdata, pblksize);
			}

			hammer2_chain_setcheck(chain, bdata);

			atomic_clear_int(&chain->flags, HAMMER2_CHAIN_INITIAL);
			hammer2_dedup_record(chain, dio, bdata);

			if (ioflag & IO_SYNC) {
				hammer2_io_bwrite(&dio);
			} else if (ioflag & IO_ASYNC) {
				hammer2_io_bawrite(&dio);
			} else {
				hammer2_io_bdwrite(&dio);
			}
			break;
		default:
			panic("hammer2_compress_and_write: bad chain type %d\n",
				chain->bref.type);
			break;
		}
	}
done:
	if (chain) {
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
	}
	if (comp_buffer)
		objcache_put(cache_buffer_write, comp_buffer);
}

static
void
hammer2_zero_check_and_write(char *data, hammer2_inode_t *ip,
	hammer2_chain_t **parentp,
	hammer2_key_t lbase, int ioflag, int pblksize,
	hammer2_tid_t mtid, int *errorp,
	int check_algo)
{
	hammer2_chain_t *chain;
	char *bdata;

	if (check_algo != HAMMER2_CHECK_NONE &&
	    test_block_zeros(data, pblksize)) {
		zero_write(data, ip, parentp, lbase, mtid, errorp);
	} else {
		bdata = data;
		chain = hammer2_assign_physical(ip, parentp, lbase, pblksize,
						mtid, &bdata, errorp);
		if (*errorp) {
		} else if (bdata) {
			hammer2_write_bp(chain, data, ioflag, pblksize,
					 mtid, errorp, check_algo);
		} else {
			chain->bref.methods =
				HAMMER2_ENC_COMP(HAMMER2_COMP_NONE) +
				HAMMER2_ENC_CHECK(check_algo);
			hammer2_chain_setcheck(chain, data);
			atomic_clear_int(&chain->flags, HAMMER2_CHAIN_INITIAL);
		}
		if (chain) {
			hammer2_chain_unlock(chain);
			hammer2_chain_drop(chain);
		}
	}
}

static
int
test_block_zeros(const char *buf, size_t bytes)
{
	size_t i;

	for (i = 0; i < bytes; i += sizeof(long)) {
		if (*(const long *)(buf + i) != 0)
			return (0);
	}
	return (1);
}

static
void
zero_write(char *data, hammer2_inode_t *ip,
	   hammer2_chain_t **parentp,
	   hammer2_key_t lbase, hammer2_tid_t mtid, int *errorp)
{
	hammer2_chain_t *chain;
	hammer2_key_t key_dummy;

	chain = hammer2_chain_lookup(parentp, &key_dummy,
				     lbase, lbase,
				     errorp,
				     HAMMER2_LOOKUP_NODATA);
	if (chain) {
		if (chain->bref.type == HAMMER2_BREF_TYPE_INODE) {
			hammer2_inode_data_t *wipdata;

			if (*errorp == 0) {
				*errorp = hammer2_chain_modify_ip(ip, chain,
								  mtid, 0);
			}
			if (*errorp == 0) {
				wipdata = &chain->data->ipdata;
				KKASSERT(wipdata->meta.op_flags &
					 HAMMER2_OPFLAG_DIRECTDATA);
				bzero(wipdata->u.data, HAMMER2_EMBEDDED_BYTES);
				++hammer2_iod_file_wembed;
			}
		} else {
			hammer2_chain_delete(*parentp, chain,
					     mtid, HAMMER2_DELETE_PERMANENT);
			++hammer2_iod_file_wzero;
		}
		atomic_set_int(&ip->flags, HAMMER2_INODE_DIRTYDATA);
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
	} else {
		++hammer2_iod_file_wzero;
	}
}

static
void
hammer2_write_bp(hammer2_chain_t *chain, char *data, int ioflag,
		 int pblksize,
		 hammer2_tid_t mtid, int *errorp, int check_algo)
{
	hammer2_inode_data_t *wipdata;
	hammer2_io_t *dio;
	char *bdata;
	int error;

	error = 0;

	KKASSERT(chain->flags & HAMMER2_CHAIN_MODIFIED);

	switch(chain->bref.type) {
	case HAMMER2_BREF_TYPE_INODE:
		wipdata = &chain->data->ipdata;
		KKASSERT(wipdata->meta.op_flags & HAMMER2_OPFLAG_DIRECTDATA);
		bcopy(data, wipdata->u.data, HAMMER2_EMBEDDED_BYTES);
		error = 0;
		++hammer2_iod_file_wembed;
		break;
	case HAMMER2_BREF_TYPE_DATA:
		error = hammer2_io_newnz(chain->hmp,
					 chain->bref.type,
					 chain->bref.data_off,
					 chain->bytes, &dio);
		if (error) {
			hammer2_io_bqrelse(&dio);
			kprintf("hammer2: WRITE PATH: "
				"dbp bread error\n");
			break;
		}
		bdata = hammer2_io_data(dio, chain->bref.data_off);

		chain->bref.methods = HAMMER2_ENC_COMP(HAMMER2_COMP_NONE) +
				      HAMMER2_ENC_CHECK(check_algo);
		bcopy(data, bdata, chain->bytes);

		hammer2_chain_setcheck(chain, bdata);

		atomic_clear_int(&chain->flags, HAMMER2_CHAIN_INITIAL);
		hammer2_dedup_record(chain, dio, bdata);

		if (ioflag & IO_SYNC) {
			hammer2_io_bwrite(&dio);
		} else if (ioflag & IO_ASYNC) {
			hammer2_io_bawrite(&dio);
		} else {
			hammer2_io_bdwrite(&dio);
		}
		break;
	default:
		panic("hammer2_write_bp: bad chain type %d\n",
		      chain->bref.type);
		error = 0;
		break;
	}
	*errorp = error;
}

void
hammer2_dedup_record(hammer2_chain_t *chain, hammer2_io_t *dio,
		     const char *data)
{
	hammer2_dev_t *hmp;
	hammer2_dedup_t *dedup;
	uint64_t crc;
	uint64_t mask;
	int best = 0;
	int i;
	int dticks;

	if (hammer2_dedup_enable == 0)
		return;
	if (dio == NULL) {
		dio = chain->dio;
		if (dio == NULL)
			return;
	}

	hmp = chain->hmp;

	switch(HAMMER2_DEC_CHECK(chain->bref.methods)) {
	case HAMMER2_CHECK_ISCSI32:
#if 0
		crc = (uint64_t)(uint32_t)chain->bref.check.iscsi32.value;
#endif
		crc = XXH64(data, chain->bytes, XXH_HAMMER2_SEED);
		break;
	case HAMMER2_CHECK_XXHASH64:
		crc = chain->bref.check.xxhash64.value;
		break;
	case HAMMER2_CHECK_SHA192:
#if 0
		crc = ((uint64_t *)chain->bref.check.sha192.data)[0] ^
		      ((uint64_t *)chain->bref.check.sha192.data)[1] ^
		      ((uint64_t *)chain->bref.check.sha192.data)[2];
#endif
		crc = XXH64(data, chain->bytes, XXH_HAMMER2_SEED);
		break;
	default:
		return;
	}

	atomic_set_int(&chain->flags, HAMMER2_CHAIN_DEDUPABLE);

	dedup = &hmp->heur_dedup[crc & (HAMMER2_DEDUP_HEUR_MASK & ~3)];
	for (i = 0; i < 4; ++i) {
		if (dedup[i].data_crc == crc) {
			best = i;
			break;
		}
		dticks = (int)(dedup[i].ticks - dedup[best].ticks);
		if (dticks < 0 || dticks > hz * 60 * 30)
			best = i;
	}
	dedup += best;
	if (hammer2_debug & 0x40000) {
		kprintf("REC %04x %016jx %016jx\n",
			(int)(dedup - hmp->heur_dedup),
			crc,
			chain->bref.data_off);
	}
	dedup->ticks = ticks;
	dedup->data_off = chain->bref.data_off;
	dedup->data_crc = crc;

	mask = hammer2_dedup_mask(dio, chain->bref.data_off, chain->bytes);
	atomic_set_64(&dio->dedup_valid, mask);

#if 0
	if (chain->flags & HAMMER2_CHAIN_MODIFIED) {
		atomic_clear_int(&chain->flags, HAMMER2_CHAIN_MODIFIED);
		atomic_add_long(&hammer2_count_modified_chains, -1);
		if (chain->pmp)
			hammer2_pfs_memory_wakeup(chain->pmp, -1);
	}
#endif
}

static
hammer2_off_t
hammer2_dedup_lookup(hammer2_dev_t *hmp, char **datap, int pblksize)
{
	hammer2_dedup_t *dedup;
	hammer2_io_t *dio;
	hammer2_off_t off;
	uint64_t crc;
	uint64_t mask;
	char *data;
	char *dtmp;
	int i;

	if (hammer2_dedup_enable == 0)
		return 0;
	data = *datap;
	if (data == NULL)
		return 0;

	crc = XXH64(data, pblksize, XXH_HAMMER2_SEED);
	dedup = &hmp->heur_dedup[crc & (HAMMER2_DEDUP_HEUR_MASK & ~3)];

	if (hammer2_debug & 0x40000) {
		kprintf("LOC %04x/4 %016jx\n",
			(int)(dedup - hmp->heur_dedup),
			crc);
	}

	for (i = 0; i < 4; ++i) {
		off = dedup[i].data_off;
		cpu_ccfence();
		if (dedup[i].data_crc != crc)
			continue;
		if ((1 << (int)(off & HAMMER2_OFF_MASK_RADIX)) != pblksize)
			continue;
		dio = hammer2_io_getquick(hmp, off, pblksize);
		if (dio) {
			dtmp = hammer2_io_data(dio, off),
			mask = hammer2_dedup_mask(dio, off, pblksize);
			if ((dio->dedup_alloc & mask) == mask &&
			    (dio->dedup_valid & mask) == mask &&
			    bcmp(data, dtmp, pblksize) == 0) {
				if (hammer2_debug & 0x40000) {
					kprintf("DEDUP SUCCESS %016jx\n",
						(intmax_t)off);
				}
				hammer2_io_putblk(&dio);
				*datap = NULL;
				dedup[i].ticks = ticks;
				atomic_add_long(&hammer2_iod_file_wdedup,
						pblksize);

				return off;
			}
			hammer2_io_putblk(&dio);
		}
	}
	return 0;
}

void
hammer2_dedup_clear(hammer2_dev_t *hmp)
{
	int i;

	for (i = 0; i < HAMMER2_DEDUP_HEUR_SIZE; ++i) {
		hmp->heur_dedup[i].data_off = 0;
		hmp->heur_dedup[i].ticks = ticks - 1;
	}
}
