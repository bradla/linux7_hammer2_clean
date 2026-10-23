#include "hammer2.h"
#include "hammer2_lz4.h"
#include "hammer2_xxhash.h"
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/zlib.h>

static int hammer2_strategy_read(struct vop_strategy_args *);
static int hammer2_strategy_write(struct vop_strategy_args *);
static void hammer2_strategy_read_completion(hammer2_chain_t *, const char *,
    struct buf *);
static void hammer2_dedup_record(hammer2_chain_t *, hammer2_io_t *,
    const char *);
static hammer2_off_t hammer2_dedup_lookup(hammer2_dev_t *, char **, int);

int
hammer2_strategy(struct vop_strategy_args *ap)
{
	struct inode *vp = ap->a_vp;
	struct buf *bp = ap->a_bp;

	if (S_ISBLK(vp->i_mode) || S_ISCHR(vp->i_mode))
		hpanic("spec %o", vp->i_mode);

	switch (bp->b_iocmd) {
	case BIO_READ:
		hammer2_strategy_read(ap);
		break;
	case BIO_WRITE:
		hammer2_strategy_write(ap);
		break;
	default:
		bp->b_error = EOPNOTSUPP;
		bp->b_ioflags |= BIO_ERROR;
		bufdone(bp);
		break;
	}

	return (0);
}

static void
hammer2_decompress_LZ4_callback(const char *data, unsigned int bytes,
    struct buf *bp)
{

	int compressed_size = *(const int *)data;
	int result;

	KKASSERT(bp->b_bufsize <= HAMMER2_PBUFSIZE);

	KKASSERT((uint32_t)compressed_size <= bytes - sizeof(int));

	result = LZ4_decompress_safe(__DECONST(char *, &data[sizeof(int)]), bp->b_data, compressed_size, bp->b_bufsize);

	if (result < 0) {
		hprintf("error during decompression: buf %016llx/%016llx/%d\n",
		    (long long)bp->b_lblkno, (long long)bp->b_blkno, bytes);

		result = 0;
	}

	if (result < bp->b_bufsize)
		bzero(bp->b_data + result, bp->b_bufsize - result);

}

static void
hammer2_decompress_ZLIB_callback(const char *data, unsigned int bytes,
    struct buf *bp)
{

	z_stream strm_decompress;
	int result;

	KKASSERT(bp->b_bufsize <= HAMMER2_PBUFSIZE);
	bzero(&strm_decompress, sizeof(strm_decompress));
	strm_decompress.avail_in = 0;
	strm_decompress.next_in = Z_NULL;

	strm_decompress.workspace =
	    kvmalloc(zlib_inflate_workspacesize(), GFP_KERNEL);
	if (strm_decompress.workspace == NULL) {
		hprintf("no memory for zlib inflate workspace\n");
		goto fail;

	}

	result = inflateInit(&strm_decompress);
	if (result != Z_OK) {
		hprintf("fatal error in inflateInit\n");
		goto fail_ws;
	}

	strm_decompress.next_in = __DECONST(char *, data);
	strm_decompress.avail_in = bytes;
	strm_decompress.next_out = bp->b_data;
	strm_decompress.avail_out = bp->b_bufsize;

	result = inflate(&strm_decompress, Z_FINISH);
	if (result != Z_STREAM_END) {
		hprintf("fatal error during decompression\n");
		inflateEnd(&strm_decompress);
		goto fail_ws;

	}

	if (strm_decompress.avail_out)
		bzero(bp->b_data + bp->b_bufsize - strm_decompress.avail_out,
		    strm_decompress.avail_out);
	inflateEnd(&strm_decompress);
	kvfree(strm_decompress.workspace);

	bp->b_resid = 0;
	return;
fail_ws:
	kvfree(strm_decompress.workspace);
fail:
	bzero(bp->b_data, bp->b_bufsize);
	bp->b_error = EIO;
	bp->b_ioflags |= BIO_ERROR;
	bp->b_resid = 0;
}

static int
hammer2_strategy_read(struct vop_strategy_args *ap)
{
	hammer2_xop_strategy_t *xop;
	hammer2_inode_t *ip = VTOI(ap->a_vp);
	struct buf *bp = ap->a_bp;
	hammer2_key_t lbase;

	lbase = bp->b_offset;
	KKASSERT(((int)lbase & HAMMER2_PBUFMASK) == 0);

	xop = hammer2_xop_alloc(ip, HAMMER2_XOP_STRATEGY);
	xop->bp = bp;
	xop->lbase = lbase;
	hammer2_xop_start(&xop->head, &hammer2_strategy_read_desc);
	hammer2_xop_retire(&xop->head, HAMMER2_XOPMASK_VOP);

	return (0);
}

void
hammer2_xop_strategy_read(hammer2_xop_t *arg, void *scratch, int clindex)
{
	hammer2_xop_strategy_t *xop = &arg->xop_strategy;
	hammer2_chain_t *chain, *parent;
	hammer2_key_t lbase = xop->lbase, key_dummy;
	struct buf *bp;
	const char *data;
	int error;

	parent = hammer2_inode_chain(xop->head.ip1, clindex,
	    HAMMER2_RESOLVE_ALWAYS | HAMMER2_RESOLVE_SHARED);
	if (parent) {
		chain = hammer2_chain_lookup(&parent, &key_dummy, lbase, lbase,
		    &error, HAMMER2_LOOKUP_ALWAYS | HAMMER2_LOOKUP_SHARED);
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

	bp = xop->bp;

	do {
		error = hammer2_xop_collect(&xop->head,
		    HAMMER2_XOP_COLLECT_NOWAIT);
	} while (error == HAMMER2_ERROR_EINPROGRESS);
	switch (error) {
	case 0:
		data = ((const hammer2_media_data_t *)hammer2_xop_gdata(&xop->head))->buf;
		hammer2_strategy_read_completion(xop->head.cluster.focus, data,
		    xop->bp);
		hammer2_xop_pdata(&xop->head);
		bufdone(bp);
		break;
	case HAMMER2_ERROR_ENOENT:
		bp->b_resid = 0;
		bp->b_error = 0;
		bzero(bp->b_data, bp->b_bcount);
		bufdone(bp);
		break;
	default:
		hprintf("error %08x at blkno %016llx/%016llx\n",
		    error, (long long)bp->b_lblkno, (long long)bp->b_blkno);
		bp->b_error = EIO;
		bp->b_ioflags |= BIO_ERROR;
		bufdone(bp);
		break;
	}
}

static void
hammer2_strategy_read_completion(hammer2_chain_t *focus, const char *data,
    struct buf *bp)
{
	if (focus->bref.type == HAMMER2_BREF_TYPE_INODE) {

		bcopy(((const hammer2_inode_data_t *)data)->u.data, bp->b_data,
		    HAMMER2_EMBEDDED_BYTES);
		bzero(bp->b_data + HAMMER2_EMBEDDED_BYTES,
		    bp->b_bcount - HAMMER2_EMBEDDED_BYTES);
		bp->b_resid = 0;
		bp->b_error = 0;
	} else if (focus->bref.type == HAMMER2_BREF_TYPE_DATA) {

		hammer2_dedup_record(focus, NULL, data);
		atomic_set_int(&focus->flags, HAMMER2_CHAIN_RELEASE);

		switch (HAMMER2_DEC_COMP(focus->bref.methods)) {
		case HAMMER2_COMP_LZ4:
			hammer2_decompress_LZ4_callback(data, focus->bytes, bp);

			break;
		case HAMMER2_COMP_ZLIB:
			hammer2_decompress_ZLIB_callback(data, focus->bytes, bp);

			break;
		case HAMMER2_COMP_NONE:
			KKASSERT(focus->bytes <= (unsigned int)bp->b_bcount);
			bcopy(data, bp->b_data, focus->bytes);
			if (focus->bytes < (unsigned int)bp->b_bcount)
				bzero(bp->b_data + focus->bytes,
				    bp->b_bcount - focus->bytes);
			bp->b_resid = 0;
			bp->b_error = 0;
			break;
		default:
			hpanic("bad comp type %02x", focus->bref.methods);
			break;
		}
	} else {
		hpanic("bad blockref type %d", focus->bref.type);
	}
}

static void hammer2_write_file_core(char *, hammer2_inode_t *,
    hammer2_chain_t **, hammer2_key_t, int, int, hammer2_tid_t, int *);
static void hammer2_compress_and_write(char *, hammer2_inode_t *,
    hammer2_chain_t **, hammer2_key_t, int, int, hammer2_tid_t, int *, int,
    int);
static void hammer2_zero_check_and_write(char *, hammer2_inode_t *,
    hammer2_chain_t **, hammer2_key_t, int, int, hammer2_tid_t, int *, int);
static int test_block_zeros(const char *, size_t);
static void zero_write(char *, hammer2_inode_t *, hammer2_chain_t **,
    hammer2_key_t, hammer2_tid_t, int *);
static void hammer2_write_bp(hammer2_chain_t *, char *, int, int, hammer2_tid_t,
    int *, int);

static int
hammer2_strategy_write(struct vop_strategy_args *ap)
{
	hammer2_xop_strategy_t *xop;
	hammer2_inode_t *ip = VTOI(ap->a_vp);
	hammer2_pfs_t *pmp = ip->pmp;
	struct buf *bp = ap->a_bp;

	atomic_set_int(&ip->flags, HAMMER2_INODE_DIRTYDATA);
	hammer2_trans_assert_strategy(pmp);
	hammer2_trans_init(pmp, HAMMER2_TRANS_BUFCACHE);

	xop = hammer2_xop_alloc(ip,
	    HAMMER2_XOP_MODIFYING | HAMMER2_XOP_STRATEGY |
	    ((bp->b_flags & B_NOWAIT) ? HAMMER2_XOP_NONBLOCK : 0));
	xop->bp = bp;
	xop->lbase = bp->b_offset;
	hammer2_xop_start(&xop->head, &hammer2_strategy_write_desc);

	if (xop->head.flags & HAMMER2_XOP_ABORTED) {
		bp->b_error = EAGAIN;
		bp->b_ioflags |= BIO_ERROR;
		bufdone(bp);
	}
	hammer2_xop_retire(&xop->head, HAMMER2_XOPMASK_VOP);

	return (0);
}

void
hammer2_xop_strategy_write(hammer2_xop_t *arg, void *scratch, int clindex)
{
	hammer2_xop_strategy_t *xop = &arg->xop_strategy;
	hammer2_chain_t *parent;
	hammer2_inode_t *ip = xop->head.ip1;
	hammer2_key_t lbase = xop->lbase;
	struct buf *bp = xop->bp;
	char *bio_data = scratch;
	int error, lblksize, pblksize;

	lblksize = hammer2_calc_logical(ip, lbase, &lbase, NULL);
	pblksize = hammer2_calc_physical(ip, lbase);
	KKASSERT(lblksize <= MAXPHYS);
	bcopy(bp->b_data, bio_data, lblksize);
	bp = NULL;

	parent = hammer2_inode_chain(ip, clindex, HAMMER2_RESOLVE_ALWAYS);
	hammer2_write_file_core(bio_data, ip, &parent, lbase, IO_ASYNC,
	    pblksize, xop->head.mtid, &error);
	if (parent) {
		hammer2_chain_unlock(parent);
		hammer2_chain_drop(parent);
		parent = NULL;
	}
	hammer2_xop_feed(&xop->head, NULL, clindex, error);

	do {
		error = hammer2_xop_collect(&xop->head,
		    HAMMER2_XOP_COLLECT_NOWAIT);
	} while (error == HAMMER2_ERROR_EINPROGRESS);
	bp = xop->bp;
	if (error == HAMMER2_ERROR_ENOENT || error == 0) {
		bp->b_resid = 0;
		bp->b_error = 0;
		bufdone(bp);
	} else {
		hprintf("error %08x at blkno %016llx/%016llx\n",
		    error, (long long)bp->b_lblkno, (long long)bp->b_blkno);
		bp->b_error = EIO;
		bp->b_ioflags |= BIO_ERROR;
		bufdone(bp);
	}
	hammer2_trans_assert_strategy(ip->pmp);
	hammer2_trans_done(ip->pmp, HAMMER2_TRANS_BUFCACHE);
}

void
hammer2_bioq_sync(hammer2_pfs_t *pmp)
{
}

static hammer2_chain_t *
hammer2_assign_physical(hammer2_inode_t *ip, hammer2_chain_t **parentp,
    hammer2_key_t lbase, int pblksize, hammer2_tid_t mtid, char **datap,
    int *errorp)
{
	hammer2_chain_t *chain;
	hammer2_key_t key_dummy;
	hammer2_off_t dedup_off;
	int pradix;

	KKASSERT(pblksize >= HAMMER2_ALLOC_MIN);
	pradix = hammer2_getradix(pblksize);

	chain = hammer2_chain_lookup(parentp, &key_dummy, lbase, lbase, errorp,
	    HAMMER2_LOOKUP_NODATA);

	if (chain && (chain->flags & HAMMER2_CHAIN_DELETED) &&
	    chain->bref.type != HAMMER2_BREF_TYPE_INODE)
		hpanic("assign physical deleted %s chain %016llx/%d inum "
		    "%016llx lbase %016llx",
		    hammer2_breftype_to_str(chain->bref.type),
		    (long long)chain->bref.key, chain->bref.keybits,
		    (long long)ip->meta.inum, (long long)lbase);

	if (chain == NULL) {

		dedup_off = hammer2_dedup_lookup((*parentp)->hmp, datap,
		    pblksize);
		*errorp |= hammer2_chain_create(parentp, &chain, NULL, ip->pmp,
		    HAMMER2_ENC_CHECK(ip->meta.check_algo) |
		    HAMMER2_ENC_COMP(HAMMER2_COMP_NONE), lbase,
		    HAMMER2_PBUFRADIX, HAMMER2_BREF_TYPE_DATA, pblksize, mtid,
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
			if (chain->bytes != (unsigned int)pblksize) {
				*errorp |= hammer2_chain_resize(chain, mtid,
				    dedup_off, pradix, HAMMER2_MODIFY_OPTDATA);
				if (*errorp)
					break;
			}

			*errorp |= hammer2_chain_modify(chain, mtid, dedup_off,
			    HAMMER2_MODIFY_OPTDATA);
			break;
		default:
			hpanic("bad blockref type %d", chain->bref.type);
			break;
		}
	} else {
		*errorp = chain->error;
	}
	atomic_set_int(&ip->flags, HAMMER2_INODE_DIRTYDATA);
failed:
	return (chain);
}

static void
hammer2_write_file_core(char *data, hammer2_inode_t *ip,
    hammer2_chain_t **parentp, hammer2_key_t lbase, int ioflag, int pblksize,
    hammer2_tid_t mtid, int *errorp)
{
	hammer2_chain_t *chain;
	hammer2_inode_data_t *wipdata;
	char *bdata;

	*errorp = 0;

	switch (HAMMER2_DEC_ALGO(ip->meta.comp_algo)) {
	case HAMMER2_COMP_NONE:

		bdata = data;
		chain = hammer2_assign_physical(ip, parentp, lbase, pblksize,
		    mtid, &bdata, errorp);
		if (*errorp) {

		} else if (chain->bref.type == HAMMER2_BREF_TYPE_INODE) {
			wipdata = &chain->data->ipdata;
			KKASSERT(wipdata->meta.op_flags &
			    HAMMER2_OPFLAG_DIRECTDATA);
			bcopy(data, wipdata->u.data, HAMMER2_EMBEDDED_BYTES);
		} else if (bdata == NULL) {

			chain->bref.methods =
			    HAMMER2_ENC_COMP(HAMMER2_COMP_NONE) +
			    HAMMER2_ENC_CHECK(ip->meta.check_algo);
			hammer2_chain_setcheck(chain, data);
			atomic_clear_int(&chain->flags, HAMMER2_CHAIN_INITIAL);
		} else {
			hammer2_write_bp(chain, data, ioflag, pblksize, mtid,
			    errorp, ip->meta.check_algo);
		}
		if (chain) {
			hammer2_chain_unlock(chain);
			hammer2_chain_drop(chain);
		}
		break;
	case HAMMER2_COMP_AUTOZERO:

		hammer2_zero_check_and_write(data, ip, parentp, lbase, ioflag,
		    pblksize, mtid, errorp, ip->meta.check_algo);
		break;
	case HAMMER2_COMP_LZ4:
	case HAMMER2_COMP_ZLIB:
	default:

		hammer2_compress_and_write(data, ip, parentp, lbase, ioflag,
		    pblksize, mtid, errorp, ip->meta.comp_algo,
		    ip->meta.check_algo);
		break;
	}
}

static void
hammer2_compress_and_write(char *data, hammer2_inode_t *ip,
    hammer2_chain_t **parentp, hammer2_key_t lbase, int ioflag, int pblksize,
    hammer2_tid_t mtid, int *errorp, int comp_algo, int check_algo)
{
	hammer2_chain_t *chain;
	hammer2_inode_data_t *wipdata;
	hammer2_io_t *dio;
	z_stream strm_compress;
	char *comp_buffer, *bdata;
	int comp_size, comp_block_size, comp_level, ret;

	KKASSERT(pblksize / 2 <= 32768);
	KKASSERT(pblksize / 2 <= HAMMER2_PBUFSIZE / 2);

	if (check_algo != HAMMER2_CHECK_NONE &&
	    test_block_zeros(data, pblksize)) {
		zero_write(data, ip, parentp, lbase, mtid, errorp);
		return;
	}

	comp_size = 0;
	comp_buffer = NULL;

	if (ip->comp_heuristic < 8 || (ip->comp_heuristic & 7) == 0 ||
	    hammer2_always_compress) {
		switch (HAMMER2_DEC_ALGO(comp_algo)) {
		case HAMMER2_COMP_LZ4:

			comp_buffer = uma_zalloc(hammer2_zone_wbuf, M_WAITOK);
			comp_size = LZ4_compress_limitedOutput(data,
			    &comp_buffer[sizeof(int)], pblksize,
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

			strm_compress.workspace = kvmalloc(
			    zlib_deflate_workspacesize(MAX_WBITS,
			    MAX_MEM_LEVEL), GFP_KERNEL);
			if (strm_compress.workspace == NULL) {
				comp_size = 0;
				break;
			}
			ret = deflateInit(&strm_compress, comp_level);
			if (ret != Z_OK)
				hprintf("fatal error on deflateInit\n");

			comp_buffer = uma_zalloc(hammer2_zone_wbuf, M_WAITOK);
			strm_compress.next_in = data;
			strm_compress.avail_in = pblksize;
			strm_compress.next_out = comp_buffer;
			strm_compress.avail_out = pblksize / 2;
			ret = deflate(&strm_compress, Z_FINISH);
			if (ret == Z_STREAM_END)
				comp_size = pblksize / 2 -
				    strm_compress.avail_out;
			else
				comp_size = 0;
			ret = deflateEnd(&strm_compress);
			kvfree(strm_compress.workspace);
			break;
		default:
			hprintf("unknown compression method %d\n", comp_algo);
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
			hpanic("weird comp_size value");

		}

		if (comp_size < comp_block_size)
			bzero(comp_buffer + comp_size,
			    comp_block_size - comp_size);
	}

	bdata = comp_size ? comp_buffer : data;
	chain = hammer2_assign_physical(ip, parentp, lbase, comp_block_size,
	    mtid, &bdata, errorp);
	if (*errorp)
		goto done;
	if (chain->bref.type == HAMMER2_BREF_TYPE_INODE) {
		*errorp = hammer2_chain_modify_ip(ip, chain, mtid, 0);
		if (*errorp == 0) {
			wipdata = &chain->data->ipdata;
			KKASSERT(wipdata->meta.op_flags &
			    HAMMER2_OPFLAG_DIRECTDATA);
			bcopy(data, wipdata->u.data, HAMMER2_EMBEDDED_BYTES);
		}
	} else if (bdata == NULL) {

		if (comp_size)
			chain->bref.methods =
			    HAMMER2_ENC_COMP(comp_algo) +
			    HAMMER2_ENC_CHECK(check_algo);
		else
			chain->bref.methods =
			    HAMMER2_ENC_COMP(HAMMER2_COMP_NONE) +
			    HAMMER2_ENC_CHECK(check_algo);
		bdata = comp_size ? comp_buffer : data;
		hammer2_chain_setcheck(chain, bdata);
		atomic_clear_int(&chain->flags, HAMMER2_CHAIN_INITIAL);
	} else {
		KKASSERT(chain->flags & HAMMER2_CHAIN_MODIFIED);
		switch (chain->bref.type) {
		case HAMMER2_BREF_TYPE_INODE:
			hpanic("unexpected inode");
			break;
		case HAMMER2_BREF_TYPE_DATA:

			*errorp = hammer2_io_newnz(chain->hmp, chain->bref.type,
			    chain->bref.data_off, chain->bytes, &dio);
			if (*errorp) {
				hammer2_io_brelse(&dio);
				hprintf("getblk error %d\n", *errorp);
				break;
			}
			bdata = hammer2_io_data(dio, chain->bref.data_off);

			hammer2_mtx_ex(&dio->lock);

			if (comp_size) {
				chain->bref.methods =
				    HAMMER2_ENC_COMP(comp_algo) +
				    HAMMER2_ENC_CHECK(check_algo);
				bcopy(comp_buffer, bdata, comp_block_size);
			} else {
				chain->bref.methods =
				    HAMMER2_ENC_COMP(HAMMER2_COMP_NONE) +
				    HAMMER2_ENC_CHECK(check_algo);
				bcopy(data, bdata, pblksize);
			}

			hammer2_chain_setcheck(chain, bdata);
			hammer2_mtx_unlock(&dio->lock);

			atomic_clear_int(&chain->flags, HAMMER2_CHAIN_INITIAL);
			hammer2_dedup_record(chain, dio, bdata);

			if (ioflag & IO_SYNC)
				hammer2_io_bwrite(&dio);
			else if (ioflag & IO_ASYNC)
				hammer2_io_bawrite(&dio);
			else
				hammer2_io_bdwrite(&dio);
			break;
		default:
			hpanic("bad blockref type %d", chain->bref.type);
			break;
		}
	}
done:
	if (chain) {
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
	}
	if (comp_buffer)
		uma_zfree(hammer2_zone_wbuf, comp_buffer);
}

static void
hammer2_zero_check_and_write(char *data, hammer2_inode_t *ip,
    hammer2_chain_t **parentp, hammer2_key_t lbase, int ioflag, int pblksize,
    hammer2_tid_t mtid, int *errorp, int check_algo)
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
			hammer2_write_bp(chain, data, ioflag, pblksize, mtid,
			    errorp, check_algo);
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

static int
test_block_zeros(const char *buf, size_t bytes)
{
	size_t i;

	for (i = 0; i < bytes; i += sizeof(long)) {
		if (*(const long *)(buf + i) != 0)
			return (0);
	}
	return (1);
}

static void
zero_write(char *data, hammer2_inode_t *ip, hammer2_chain_t **parentp,
    hammer2_key_t lbase, hammer2_tid_t mtid, int *errorp)
{
	hammer2_chain_t *chain;
	hammer2_inode_data_t *wipdata;
	hammer2_key_t key_dummy;

	chain = hammer2_chain_lookup(parentp, &key_dummy, lbase, lbase, errorp,
	    HAMMER2_LOOKUP_NODATA);
	if (chain) {
		if (chain->bref.type == HAMMER2_BREF_TYPE_INODE) {
			if (*errorp == 0)
				*errorp = hammer2_chain_modify_ip(ip, chain,
				    mtid, 0);
			if (*errorp == 0) {
				wipdata = &chain->data->ipdata;
				KKASSERT(wipdata->meta.op_flags &
				    HAMMER2_OPFLAG_DIRECTDATA);
				bzero(wipdata->u.data, HAMMER2_EMBEDDED_BYTES);
			}
		} else {

			hammer2_chain_delete(*parentp, chain, mtid,
			    HAMMER2_DELETE_PERMANENT);
		}
		atomic_set_int(&ip->flags, HAMMER2_INODE_DIRTYDATA);
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
	}
}

static void
hammer2_write_bp(hammer2_chain_t *chain, char *data, int ioflag, int pblksize,
    hammer2_tid_t mtid, int *errorp, int check_algo)
{
	hammer2_inode_data_t *wipdata;
	hammer2_io_t *dio;
	char *bdata;
	int error = 0;

	KKASSERT(chain->flags & HAMMER2_CHAIN_MODIFIED);

	switch (chain->bref.type) {
	case HAMMER2_BREF_TYPE_INODE:
		wipdata = &chain->data->ipdata;
		KKASSERT(wipdata->meta.op_flags & HAMMER2_OPFLAG_DIRECTDATA);
		bcopy(data, wipdata->u.data, HAMMER2_EMBEDDED_BYTES);
		error = 0;
		break;
	case HAMMER2_BREF_TYPE_DATA:
		error = hammer2_io_newnz(chain->hmp, chain->bref.type,
		    chain->bref.data_off, chain->bytes, &dio);
		if (error) {
			hammer2_io_bqrelse(&dio);
			hprintf("getblk error %d\n", error);
			break;
		}
		bdata = hammer2_io_data(dio, chain->bref.data_off);
		chain->bref.methods =
		    HAMMER2_ENC_COMP(HAMMER2_COMP_NONE) +
		    HAMMER2_ENC_CHECK(check_algo);

		hammer2_mtx_ex(&dio->lock);
		bcopy(data, bdata, chain->bytes);

		hammer2_chain_setcheck(chain, bdata);
		hammer2_mtx_unlock(&dio->lock);

		atomic_clear_int(&chain->flags, HAMMER2_CHAIN_INITIAL);
		hammer2_dedup_record(chain, dio, bdata);

		if (ioflag & IO_SYNC)
			hammer2_io_bwrite(&dio);
		else if (ioflag & IO_ASYNC)
			hammer2_io_bawrite(&dio);
		else
			hammer2_io_bdwrite(&dio);
		break;
	default:
		hpanic("bad blockref type %d", chain->bref.type);
		break;
	}
	*errorp = error;
}

static void
hammer2_dedup_record(hammer2_chain_t *chain, hammer2_io_t *dio,
    const char *data)
{
	hammer2_dev_t *hmp = chain->hmp;
	hammer2_dedup_t *dedup;
	uint64_t crc, mask;
	int i, dticks, best = 0;

	if (hammer2_dedup_enable == 0)
		return;
	if (dio == NULL) {
		dio = chain->dio;
		if (dio == NULL)
			return;
	}
	hammer2_mtx_assert_unlocked(&dio->lock);

	switch (HAMMER2_DEC_CHECK(chain->bref.methods)) {
	case HAMMER2_CHECK_ISCSI32:

		crc = XXH64(data, chain->bytes, XXH_HAMMER2_SEED);
		break;
	case HAMMER2_CHECK_XXHASH64:
		crc = chain->bref.check.xxhash64.value;
		break;
	case HAMMER2_CHECK_SHA192:

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
	dedup->ticks = getticks();
	dedup->data_off = chain->bref.data_off;
	dedup->data_crc = crc;

	mask = hammer2_dedup_mask(dio, chain->bref.data_off, chain->bytes);
	hammer2_mtx_ex(&dio->lock);
	dio->dedup_valid |= mask;
	hammer2_mtx_unlock(&dio->lock);
}

static hammer2_off_t
hammer2_dedup_lookup(hammer2_dev_t *hmp, char **datap, int pblksize)
{
	hammer2_dedup_t *dedup;
	hammer2_io_t *dio;
	hammer2_off_t off;
	char *data, *dtmp;
	uint64_t crc, mask;
	int i;

	if (hammer2_dedup_enable == 0)
		return (0);
	data = *datap;
	if (data == NULL)
		return (0);

	crc = XXH64(data, pblksize, XXH_HAMMER2_SEED);
	dedup = &hmp->heur_dedup[crc & (HAMMER2_DEDUP_HEUR_MASK & ~3)];

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
			hammer2_mtx_assert_unlocked(&dio->lock);
			hammer2_mtx_ex(&dio->lock);
			if ((dio->dedup_alloc & mask) == mask &&
			    (dio->dedup_valid & mask) == mask &&
			    bcmp(data, dtmp, pblksize) == 0) {
				hammer2_mtx_unlock(&dio->lock);
				hammer2_io_putblk(&dio);
				*datap = NULL;
				dedup[i].ticks = getticks();
				return (off);
			}
			hammer2_mtx_unlock(&dio->lock);
			hammer2_io_putblk(&dio);
		}
	}
	return (0);
}

void
hammer2_dedup_clear(hammer2_dev_t *hmp)
{
	int i;

	for (i = 0; i < HAMMER2_DEDUP_HEUR_SIZE; ++i) {
		hmp->heur_dedup[i].data_off = 0;
		hmp->heur_dedup[i].ticks = getticks() - 1;
	}
}
