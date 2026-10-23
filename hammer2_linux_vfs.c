#include "hammer2.h"

#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/fs_context.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/writeback.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/statfs.h>
#include <linux/uio.h>
#include <linux/namei.h>
#include <linux/version.h>
#include <linux/uaccess.h>

#define HAMMER2_PORT_VERSION	"0.47"
#define HAMMER2_PORT_BUILD	HAMMER2_PORT_VERSION " built " __DATE__ " " __TIME__

int hammer2_mount(struct mount *mp);
int hammer2_unmount(struct mount *mp, int mntflags);
int hammer2_root(struct mount *mp, int flags, struct inode **vpp);
int hammer2_statfs(struct mount *mp, struct h2statfs *sbp);
int hammer2_sync(struct mount *mp, int waitfor);
hammer2_tid_t hammer2_trans_newinum(hammer2_pfs_t *pmp);
struct vfsconf;
int hammer2_init(struct vfsconf *vfsp);
int hammer2_uninit(struct vfsconf *vfsp);

static const struct super_operations hammer2_super_ops;
static const struct inode_operations hammer2_dir_iops;
static const struct inode_operations hammer2_file_iops;
static const struct inode_operations hammer2_symlink_iops;
static const struct inode_operations hammer2_special_iops;
static const struct file_operations hammer2_dir_fops;
static const struct file_operations hammer2_file_fops;
static const struct address_space_operations hammer2_aops;

static umode_t
hammer2_objtype_to_ifmt(uint8_t type)
{
	switch (type) {
	case HAMMER2_OBJTYPE_DIRECTORY:	return S_IFDIR;
	case HAMMER2_OBJTYPE_REGFILE:	return S_IFREG;
	case HAMMER2_OBJTYPE_FIFO:	return S_IFIFO;
	case HAMMER2_OBJTYPE_CDEV:	return S_IFCHR;
	case HAMMER2_OBJTYPE_BDEV:	return S_IFBLK;
	case HAMMER2_OBJTYPE_SOFTLINK:	return S_IFLNK;
	case HAMMER2_OBJTYPE_SOCKET:	return S_IFSOCK;
	default:			return 0;
	}
}

static uint8_t
hammer2_ifmt_to_dtype(umode_t mode)
{
	if (S_ISDIR(mode))	return DT_DIR;
	if (S_ISREG(mode))	return DT_REG;
	if (S_ISLNK(mode))	return DT_LNK;
	if (S_ISCHR(mode))	return DT_CHR;
	if (S_ISBLK(mode))	return DT_BLK;
	if (S_ISFIFO(mode))	return DT_FIFO;
	if (S_ISSOCK(mode))	return DT_SOCK;
	return DT_UNKNOWN;
}

static void
hammer2_set_inode_ops(struct inode *inode)
{
	switch (inode->i_mode & S_IFMT) {
	case S_IFREG:
		inode->i_op = &hammer2_file_iops;
		inode->i_fop = &hammer2_file_fops;
		inode->i_mapping->a_ops = &hammer2_aops;

		mapping_set_folio_order_range(inode->i_mapping, 0,
		    HAMMER2_PBUFRADIX - PAGE_SHIFT);
		break;
	case S_IFDIR:
		inode->i_op = &hammer2_dir_iops;
		inode->i_fop = &hammer2_dir_fops;
		break;
	case S_IFLNK:
		inode->i_op = &hammer2_symlink_iops;
		inode_nohighmem(inode);
		break;
	default:
		inode->i_op = &hammer2_special_iops;
		init_special_inode(inode, inode->i_mode,
		    MKDEV(VTOI(inode)->meta.rmajor, VTOI(inode)->meta.rminor));
		break;
	}
}

static void
hammer2_install_meta(struct inode *inode, hammer2_inode_t *ip)
{
	struct timespec64 ts;

	inode->i_mode = (ip->meta.mode & (S_IALLUGO)) |
	    hammer2_objtype_to_ifmt(ip->meta.type);
	i_uid_write(inode, hammer2_inode_to_uid(ip));
	i_gid_write(inode, hammer2_inode_to_gid(ip));
	set_nlink(inode, ip->meta.nlinks);
	i_size_write(inode, ip->meta.size);
	inode->i_blkbits = HAMMER2_PBUFRADIX;

	hammer2_time_to_timespec(ip->meta.mtime, &ts);
	inode_set_mtime_to_ts(inode, ts);
	hammer2_time_to_timespec(ip->meta.atime, &ts);
	inode_set_atime_to_ts(inode, ts);
	hammer2_time_to_timespec(ip->meta.ctime, &ts);
	inode_set_ctime_to_ts(inode, ts);

	hammer2_set_inode_ops(inode);
}

static int
hammer2_iget_test(struct inode *inode, void *data)
{
	return inode->i_private == data;
}

static int
hammer2_iget_set(struct inode *inode, void *data)
{
	hammer2_inode_t *ip = data;

	inode->i_private = ip;
	inode->i_ino = (unsigned long)ip->meta.inum;
	ip->vp = inode;
	hammer2_inode_ref(ip);
	return 0;
}

struct inode *
hammer2_iget(struct super_block *sb, hammer2_inode_t *ip)
{
	struct inode *inode;

	inode = iget5_locked(sb, (unsigned long)ip->meta.inum,
	    hammer2_iget_test, hammer2_iget_set, ip);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode_state_read(inode) & I_NEW))
		return inode;

	hammer2_install_meta(inode, ip);
	unlock_new_inode(inode);
	return inode;
}

static int
hammer2_strategy_block(struct inode *inode, hammer2_key_t lbase, char *data,
    int iocmd, int nowait)
{
	struct vop_strategy_args ap;
	struct buf b;

	memset(&b, 0, sizeof(b));
	b.b_iocmd = iocmd;
	b.b_data = data;
	b.b_offset = lbase;
	b.b_bcount = HAMMER2_PBUFSIZE;
	b.b_bufsize = HAMMER2_PBUFSIZE;
	b.b_resid = HAMMER2_PBUFSIZE;
	b.b_lblkno = lbase / HAMMER2_PBUFSIZE;
	b.b_blkno = b.b_lblkno;
	b.b_flags = nowait ? B_NOWAIT : 0;

	ap.a_vp = inode;
	ap.a_bp = &b;
	hammer2_strategy(&ap);

	if (b.b_ioflags & BIO_ERROR)
		return b.b_error ? -b.b_error : -EIO;
	return 0;
}

static ssize_t __maybe_unused
hammer2_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	hammer2_inode_t *ip = VTOI(inode);
	loff_t pos = iocb->ki_pos;
	loff_t isize;
	size_t want = iov_iter_count(to);
	ssize_t total = 0;
	char *blk;
	int error = 0;

	if (S_ISDIR(inode->i_mode))
		return -EISDIR;

	hammer2_mtx_sh(&ip->lock);
	isize = ip->meta.size;
	hammer2_mtx_unlock(&ip->lock);

	if (want == 0 || pos >= isize)
		return 0;

	blk = kmalloc(HAMMER2_PBUFSIZE, GFP_KERNEL);
	if (!blk)
		return -ENOMEM;

	while (want > 0 && pos < isize) {
		hammer2_key_t lbase = pos & ~(hammer2_key_t)HAMMER2_PBUFMASK;
		int loff = (int)(pos - lbase);
		size_t n = HAMMER2_PBUFSIZE - loff;

		if (n > want)
			n = want;
		if ((loff_t)(pos + n) > isize)
			n = (size_t)(isize - pos);

		error = hammer2_strategy_block(inode, lbase, blk, BIO_READ, 0);
		if (error)
			break;
		if (copy_to_iter(blk + loff, n, to) != n) {
			error = -EFAULT;
			break;
		}
		pos += n;
		total += n;
		want -= n;
	}

	kfree(blk);
	iocb->ki_pos = pos;

	if (total > 0)
		file_accessed(iocb->ki_filp);
	return total ? total : error;
}

static const char *
hammer2_get_link(struct dentry *dentry, struct inode *inode,
    struct delayed_call *done)
{
	hammer2_inode_t *ip = VTOI(inode);
	loff_t len;
	char *blk, *target;
	int error;

	if (!dentry)
		return ERR_PTR(-ECHILD);

	hammer2_mtx_sh(&ip->lock);
	len = ip->meta.size;
	hammer2_mtx_unlock(&ip->lock);

	if (len < 0 || len >= HAMMER2_PBUFSIZE)
		return ERR_PTR(-EIO);

	blk = kmalloc(HAMMER2_PBUFSIZE, GFP_KERNEL);
	if (!blk)
		return ERR_PTR(-ENOMEM);

	error = hammer2_strategy_block(inode, 0, blk, BIO_READ, 0);
	if (error) {
		kfree(blk);
		return ERR_PTR(error);
	}
	blk[len] = '\0';

	target = blk;
	set_delayed_call(done, kfree_link, target);
	return target;
}

#define HAMMER2_NAME_MAX	(HAMMER2_INODE_MAXNAME - 1)

static struct dentry *
hammer2_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
	hammer2_inode_t *dip = VTOI(dir);
	hammer2_xop_nresolve_t *xop;
	hammer2_inode_t *ip;
	struct inode *inode = NULL;
	int error;

	if (dentry->d_name.len > HAMMER2_NAME_MAX)
		return ERR_PTR(-ENAMETOOLONG);

	hammer2_inode_lock(dip, HAMMER2_RESOLVE_SHARED);
	xop = hammer2_xop_alloc(dip, 0);
	hammer2_xop_setname(&xop->head, dentry->d_name.name, dentry->d_name.len);
	hammer2_xop_start(&xop->head, &hammer2_nresolve_desc);
	error = hammer2_xop_collect(&xop->head, 0);
	error = hammer2_error_to_errno(error);

	if (error == 0) {
		ip = hammer2_inode_get(dip->pmp, &xop->head, -1, -1);
		hammer2_inode_unlock(dip);
		if (ip) {
			inode = hammer2_iget(dir->i_sb, ip);
			hammer2_inode_unlock(ip);
			if (IS_ERR(inode)) {
				hammer2_xop_retire(&xop->head,
				    HAMMER2_XOPMASK_VOP);
				return ERR_CAST(inode);
			}
		}
	} else {
		hammer2_inode_unlock(dip);
		if (error != ENOENT) {
			hammer2_xop_retire(&xop->head, HAMMER2_XOPMASK_VOP);
			return ERR_PTR(-error);
		}
	}
	hammer2_xop_retire(&xop->head, HAMMER2_XOPMASK_VOP);

	return d_splice_alias(inode, dentry);
}

static inline const void *
hammer2_readdir_gdata(hammer2_xop_head_t *head, hammer2_chain_t *dchain)
{
	if (dchain)
		return hammer2_xop_gdata_chain(head, dchain);
	return hammer2_xop_gdata(head);
}

static int
hammer2_iterate(struct file *file, struct dir_context *ctx)
{
	struct inode *inode = file_inode(file);
	hammer2_inode_t *ip = VTOI(inode);
	hammer2_xop_readdir_t *xop;
	const hammer2_inode_data_t *ripdata;
	hammer2_blockref_t bref;
	hammer2_chain_t *dchain;
	hammer2_tid_t inum;
	off_t saveoff = ctx->pos;
	int dtype, error = 0, xflags;
	int bound, bmin, bmax, nemit, nemit_total = 0, full = 0;
	uint16_t namlen;
	const char *dname;

	if (!S_ISDIR(inode->i_mode))
		return -ENOTDIR;

	hammer2_inode_lock(ip, HAMMER2_RESOLVE_SHARED);

	if (saveoff == 0) {
		inum = ip->meta.inum & HAMMER2_DIRHASH_USERMSK;
		if (!dir_emit(ctx, ".", 1, inum, DT_DIR))
			goto done;
		ctx->pos = ++saveoff;
	}
	if (saveoff == 1) {
		inum = ip->meta.inum & HAMMER2_DIRHASH_USERMSK;
		if (ip != ip->pmp->iroot)
			inum = ip->meta.iparent & HAMMER2_DIRHASH_USERMSK;
		if (!dir_emit(ctx, "..", 2, inum, DT_DIR))
			goto done;
		ctx->pos = ++saveoff;
	}

	bmin = hammer2_readdir_bound_min;
	bmax = hammer2_readdir_bound_max;
	if (bmin < HAMMER2_XOPFIFO)
		bmin = HAMMER2_XOPFIFO;
	if (bmin > HAMMER2_READDIR_BOUND_LIMIT)
		bmin = HAMMER2_READDIR_BOUND_LIMIT;
	if (bmax > HAMMER2_READDIR_BOUND_LIMIT)
		bmax = HAMMER2_READDIR_BOUND_LIMIT;
	if (bmax < bmin)
		bmax = bmin;

	bound = (int)(unsigned long)file->private_data;
	if (bound < bmin)
		bound = bmin;
	else if (bound > bmax)
		bound = bmax;

	xflags = HAMMER2_XOP_FIFO_BOUND;
	if (hammer2_readdir_bref && ip->cluster.nchains == 1)
		xflags |= HAMMER2_XOP_FIFO_BREF;

	while (!full) {
		xop = hammer2_xop_alloc(ip, xflags);
		hammer2_xop_setbound(&xop->head, bound);
		xop->lkey = ctx->pos | HAMMER2_DIRHASH_VISIBLE;
		hammer2_xop_start(&xop->head, &hammer2_readdir_desc);

		nemit = 0;
		for (;;) {

			if (xflags & HAMMER2_XOP_FIFO_BREF) {
				error = hammer2_xop_collect_bref(&xop->head,
				    &bref, &dchain);
			} else {
				dchain = NULL;
				error = hammer2_xop_collect(&xop->head, 0);
				if (error == 0)
					hammer2_cluster_bref(
					    &xop->head.cluster, &bref);
			}
			error = hammer2_error_to_errno(error);
			if (error)
				break;

			if ((xflags & HAMMER2_XOP_FIFO_BREF) && dchain == NULL &&
			    (bref.type == HAMMER2_BREF_TYPE_INODE ||
			    (bref.type == HAMMER2_BREF_TYPE_DIRENT &&
			    bref.embed.dirent.namlen >
			    sizeof(bref.check.buf)))) {
				hprintf("bref entry %016llx type %d needs data "
				    "but has no chain\n",
				    (long long)bref.key, bref.type);
				continue;
			}

			if (bref.type == HAMMER2_BREF_TYPE_INODE) {
				ripdata = &((const hammer2_media_data_t *)
				    hammer2_readdir_gdata(&xop->head,
				    dchain))->ipdata;
				dtype = hammer2_get_dtype(ripdata->meta.type);
				saveoff = bref.key & HAMMER2_DIRHASH_USERMSK;
				if (!dir_emit(ctx,
				    (const char *)ripdata->filename,
				    ripdata->meta.name_len,
				    ripdata->meta.inum & HAMMER2_DIRHASH_USERMSK,
				    dtype)) {
					hammer2_xop_pdata(&xop->head);
					full = 1;
					break;
				}
				hammer2_xop_pdata(&xop->head);
			} else if (bref.type == HAMMER2_BREF_TYPE_DIRENT) {
				dtype = hammer2_get_dtype(bref.embed.dirent.type);
				saveoff = bref.key & HAMMER2_DIRHASH_USERMSK;
				namlen = bref.embed.dirent.namlen;
				if (namlen <= sizeof(bref.check.buf))
					dname = bref.check.buf;
				else
					dname = ((const hammer2_media_data_t *)
					    hammer2_readdir_gdata(&xop->head,
					    dchain))->buf;
				if (!dir_emit(ctx, dname, namlen,
				    bref.embed.dirent.inum, dtype)) {
					if (namlen > sizeof(bref.check.buf))
						hammer2_xop_pdata(&xop->head);
					full = 1;
					break;
				}
				if (namlen > sizeof(bref.check.buf))
					hammer2_xop_pdata(&xop->head);
			} else {
				hprintf("bad blockref type %d\n", bref.type);
				continue;
			}

			ctx->pos = (saveoff + 1) & ~HAMMER2_DIRHASH_VISIBLE;
			++nemit;
		}
		hammer2_xop_retire(&xop->head, HAMMER2_XOPMASK_VOP);

		if (error == ENOENT || error == -ENOENT)
			error = 0;
		nemit_total += nemit;
		if (error || nemit == 0)
			break;
		if (bound < bmax) {
			bound *= 2;
			if (bound > bmax)
				bound = bmax;
		}
	}

	if (nemit_total) {
		int slop = hammer2_readdir_slop;
		int hint;

		if (slop < 1)
			slop = 1;
		hint = nemit_total + slop;

		if (hint > bmax)
			hint = bmax;
		file->private_data = (void *)(unsigned long)hint;
	}
done:
	hammer2_inode_unlock(ip);
	return -error;
}

static int
hammer2_getattr(struct mnt_idmap *idmap, const struct path *path,
    struct kstat *stat, u32 request_mask, unsigned int query_flags)
{
	struct inode *inode = d_inode(path->dentry);
	hammer2_inode_t *ip = VTOI(inode);

	generic_fillattr(idmap, request_mask, inode, stat);
	stat->blksize = HAMMER2_PBUFSIZE;
	if (ip && ip->meta.type != HAMMER2_OBJTYPE_DIRECTORY)
		stat->blocks = (hammer2_inode_data_count(ip) + 511) / 512;
	return 0;
}

static void
hammer2_zero_extend_tail(struct inode *inode, loff_t osize, loff_t nsize)
{
	struct address_space *mapping = inode->i_mapping;
	struct folio *folio;
	loff_t pos, zend;

	if (nsize <= osize || (osize & HAMMER2_PBUFMASK) == 0)
		return;

	zend = (osize + HAMMER2_PBUFSIZE) & ~(loff_t)HAMMER2_PBUFMASK;
	if (zend > nsize)
		zend = nsize;

	for (pos = osize; pos < zend; ) {
		size_t off, len;

		folio = __filemap_get_folio(mapping, pos >> PAGE_SHIFT,
		    FGP_LOCK | FGP_CREAT, mapping_gfp_mask(mapping));
		if (IS_ERR(folio))
			break;
		if (!folio_test_uptodate(folio))
			folio_zero_range(folio, 0, folio_size(folio));
		off = offset_in_folio(folio, pos);
		len = folio_size(folio) - off;
		if (pos + (loff_t)len > zend)
			len = zend - pos;
		folio_zero_range(folio, off, len);
		folio_mark_uptodate(folio);
		filemap_dirty_folio(mapping, folio);
		folio_unlock(folio);
		folio_put(folio);
		pos += len;
	}
}

static void
hammer2_resize_meta(struct inode *inode, hammer2_inode_t *ip, loff_t nsize,
    loff_t zlimit)
{
	hammer2_pfs_t *pmp = ip->pmp;
	struct folio *folio = NULL;
	loff_t osize;
	int crossing;

	osize = i_size_read(inode);
	crossing = (osize > 0 && osize <= HAMMER2_EMBEDDED_BYTES &&
	    nsize > HAMMER2_EMBEDDED_BYTES);
	if (crossing) {
		folio = read_mapping_folio(inode->i_mapping, 0, NULL);
		if (IS_ERR(folio))
			folio = NULL;
	}

	hammer2_trans_init(pmp, 0);
	hammer2_mtx_ex(&ip->lock);
	osize = ip->meta.size;
	if (nsize == osize) {
		hammer2_mtx_unlock(&ip->lock);
		hammer2_trans_done(pmp, HAMMER2_TRANS_SIDEQ);
		return;
	}
	hammer2_inode_modify(ip);
	ip->osize = osize;
	ip->meta.size = nsize;

	if (nsize < osize ||
	    (osize <= HAMMER2_EMBEDDED_BYTES && nsize > HAMMER2_EMBEDDED_BYTES)) {
		atomic_set_int(&ip->flags, HAMMER2_INODE_RESIZED);
		hammer2_inode_chain_sync(ip);
	}
	hammer2_mtx_unlock(&ip->lock);
	hammer2_trans_done(pmp, HAMMER2_TRANS_SIDEQ);

	i_size_write(inode, nsize);

	if (nsize < osize)
		truncate_pagecache(inode, nsize);
	else
		hammer2_zero_extend_tail(inode, osize, min(nsize, zlimit));

	if (folio) {
		folio_lock(folio);
		if (folio->mapping == inode->i_mapping)
			filemap_dirty_folio(inode->i_mapping, folio);
		folio_unlock(folio);
		folio_put(folio);
	}
}

static int
hammer2_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
    struct iattr *iattr)
{
	struct inode *inode = d_inode(dentry);
	hammer2_inode_t *ip = VTOI(inode);
	hammer2_pfs_t *pmp = ip->pmp;
	uint64_t ctime;
	int error;

	if (pmp->rdonly)
		return -EROFS;

	error = setattr_prepare(idmap, dentry, iattr);
	if (error)
		return error;

	if ((iattr->ia_valid & ATTR_SIZE) && S_ISDIR(inode->i_mode))
		return -EISDIR;

	if ((iattr->ia_valid & ATTR_SIZE) && iattr->ia_size != i_size_read(inode))
		hammer2_resize_meta(inode, ip, iattr->ia_size, iattr->ia_size);

	hammer2_trans_init(pmp, 0);
	hammer2_mtx_ex(&ip->lock);
	hammer2_inode_modify(ip);
	hammer2_update_time(&ctime);

	if (iattr->ia_valid & ATTR_MODE) {
		ip->meta.mode = (ip->meta.mode & ~(S_IALLUGO)) |
		    (iattr->ia_mode & S_IALLUGO);
		ip->meta.ctime = ctime;
	}
	if (iattr->ia_valid & ATTR_UID)
		hammer2_guid_to_uuid(&ip->meta.uid,
		    from_kuid(&init_user_ns, iattr->ia_uid));
	if (iattr->ia_valid & ATTR_GID)
		hammer2_guid_to_uuid(&ip->meta.gid,
		    from_kgid(&init_user_ns, iattr->ia_gid));
	if (iattr->ia_valid & ATTR_MTIME)
		ip->meta.mtime = hammer2_timespec_to_time(&iattr->ia_mtime);
	if (iattr->ia_valid & ATTR_ATIME)
		ip->meta.atime = hammer2_timespec_to_time(&iattr->ia_atime);

	if (iattr->ia_valid & ATTR_SIZE)
		ip->meta.mtime = ctime;
	if (iattr->ia_valid & (ATTR_CTIME | ATTR_MODE | ATTR_SIZE))
		ip->meta.ctime = ctime;
	hammer2_mtx_unlock(&ip->lock);
	hammer2_trans_done(pmp, HAMMER2_TRANS_SIDEQ);

	setattr_copy(idmap, inode, iattr);

	if (iattr->ia_valid & ATTR_SIZE)
		inode_set_mtime_to_ts(inode, inode_set_ctime_current(inode));
	mark_inode_dirty(inode);
	return 0;
}

mempool_t hammer2_blk_pool;

static void *h2_blk_alloc(gfp_t gfp, void *d)
{
	return kvmalloc(HAMMER2_PBUFSIZE, gfp);
}

static void h2_blk_free(void *p, void *d)
{
	kvfree(p);
}

int hammer2_blk_pool_init(void)
{
	return mempool_init(&hammer2_blk_pool, 8, h2_blk_alloc, h2_blk_free, NULL);
}

void hammer2_blk_pool_exit(void)
{
	mempool_exit(&hammer2_blk_pool);
}

static int
hammer2_fill_folio(struct inode *inode, struct folio *folio)
{
	hammer2_inode_t *ip = VTOI(inode);
	loff_t fpos = folio_pos(folio);
	size_t fsize = folio_size(folio);
	loff_t isize;
	char *blk;
	size_t done = 0;
	int error = 0;

	hammer2_mtx_sh(&ip->lock);
	isize = ip->meta.size;
	hammer2_mtx_unlock(&ip->lock);

	blk = mempool_alloc(&hammer2_blk_pool, GFP_NOFS);
	if (!blk)
		return -ENOMEM;

	while (done < fsize) {
		loff_t pos = fpos + done;
		hammer2_key_t lbase = pos & ~(hammer2_key_t)HAMMER2_PBUFMASK;
		int loff = (int)(pos - lbase);
		size_t chunk = HAMMER2_PBUFSIZE - loff;
		size_t valid;

		if (chunk > fsize - done)
			chunk = fsize - done;

		if (pos >= isize) {
			folio_zero_range(folio, done, fsize - done);
			break;
		}

		error = hammer2_strategy_block(inode, lbase, blk, BIO_READ, 0);
		if (error)
			goto out;

		valid = chunk;
		if ((loff_t)(pos + chunk) > isize)
			valid = (size_t)(isize - pos);
		memcpy_to_folio(folio, done, blk + loff, valid);
		if (valid < chunk)
			folio_zero_range(folio, done + valid, chunk - valid);
		done += chunk;
	}
out:
	mempool_free(blk, &hammer2_blk_pool);

	return error;
}

static int
hammer2_read_folio(struct file *file, struct folio *folio)
{
	int error = hammer2_fill_folio(folio->mapping->host, folio);

	if (!error)
		folio_mark_uptodate(folio);
	folio_unlock(folio);
	return error;
}

static int
hammer2_write_begin(const struct kiocb *iocb, struct address_space *mapping,
    loff_t pos, unsigned len, struct folio **foliop, void **fsdata)
{
	struct inode *inode = mapping->host;
	struct folio *folio;
	int error;

	folio = __filemap_get_folio(mapping, pos >> PAGE_SHIFT,
	    FGP_WRITEBEGIN | fgf_set_order(len), mapping_gfp_mask(mapping));
	if (IS_ERR(folio))
		return PTR_ERR(folio);

	if (!folio_test_uptodate(folio) &&
	    !(offset_in_folio(folio, pos) == 0 && len >= folio_size(folio))) {
		error = hammer2_fill_folio(inode, folio);
		if (error) {
			folio_unlock(folio);
			folio_put(folio);
			return error;
		}
		folio_mark_uptodate(folio);
	}

	*foliop = folio;
	return 0;
}

static int
hammer2_write_end(const struct kiocb *iocb, struct address_space *mapping,
    loff_t pos, unsigned len, unsigned copied, struct folio *folio,
    void *fsdata)
{
	struct inode *inode = mapping->host;
	hammer2_inode_t *ip = VTOI(inode);
	loff_t end;

	if (!folio_test_uptodate(folio)) {
		if (copied < len)
			copied = 0;
		else
			folio_mark_uptodate(folio);
	}

	end = pos + copied;

	if (copied) {
		flush_dcache_folio(folio);
		folio_mark_dirty(folio);
	}
	folio_unlock(folio);
	folio_put(folio);

	if (end > i_size_read(inode)) {
		i_size_write(inode, end);
		hammer2_resize_meta(inode, ip, end, pos);
	}
	return copied;
}

static int
hammer2_writeback_folio(struct inode *inode, struct folio *folio, char *blk,
    int may_skip)
{
	hammer2_inode_t *ip = VTOI(inode);
	hammer2_pfs_t *pmp = ip->pmp;
	loff_t fpos = folio_pos(folio);
	size_t fsize = folio_size(folio);
	loff_t isize = i_size_read(inode);
	size_t done = 0;
	unsigned int stall = 0;
	int error = 0;

	while (done < fsize) {
		loff_t pos = fpos + done;
		hammer2_key_t lbase = pos & ~(hammer2_key_t)HAMMER2_PBUFMASK;
		int loff = (int)(pos - lbase);
		size_t chunk = HAMMER2_PBUFSIZE - loff;

		if (chunk > fsize - done)
			chunk = fsize - done;
		if (pos >= isize)
			break;

		hammer2_trans_init(pmp, HAMMER2_TRANS_BUFCACHE);
		hammer2_mtx_ex(&ip->rmw_lock);
		if (loff != 0 || chunk != HAMMER2_PBUFSIZE) {

			error = hammer2_strategy_block(inode, lbase, blk,
			    BIO_READ, 1);
			if (error == -EAGAIN)
				goto deferred;
			if (error != 0) {
				memset(blk, 0, HAMMER2_PBUFSIZE);
				error = 0;
			}
		} else {
			memset(blk, 0, HAMMER2_PBUFSIZE);
		}
		memcpy_from_folio(blk + loff, folio, done, chunk);

		error = hammer2_strategy_block(inode, lbase, blk, BIO_WRITE, 1);
		if (error == -EAGAIN)
			goto deferred;
		hammer2_mtx_unlock(&ip->rmw_lock);
		hammer2_trans_done(pmp, HAMMER2_TRANS_BUFCACHE);
		if (error)
			break;
		done += chunk;
		stall = 0;
		continue;
deferred:

		hammer2_mtx_unlock(&ip->rmw_lock);
		hammer2_trans_done(pmp, HAMMER2_TRANS_BUFCACHE);
		if (may_skip) {
			error = -EAGAIN;
			break;
		}
		error = 0;
		if (++stall > 16) {

			schedule_timeout_uninterruptible(1);
			if (stall > (16 + HZ * 30))
				hprintf("inode %llu block %llu: XOP interlock "
				    "busy for >30s during sync writeback\n",
				    (unsigned long long)ip->meta.inum,
				    (unsigned long long)lbase);
		} else {
			cond_resched();
		}
	}
	return error;
}

static int
hammer2_writepages(struct address_space *mapping,
    struct writeback_control *wbc)
{
	struct inode *inode = mapping->host;
	struct folio *folio = NULL;
	char *blk;
	int error = 0;
	int may_skip;
	unsigned int nofs_flags;

	nofs_flags = memalloc_nofs_save();
	blk = mempool_alloc(&hammer2_blk_pool, GFP_NOFS);

	if (!blk)
		return -ENOMEM;

	may_skip = (wbc->sync_mode == WB_SYNC_NONE);

	while ((folio = writeback_iter(mapping, wbc, folio, &error))) {
		folio_start_writeback(folio);
		folio_unlock(folio);
		error = hammer2_writeback_folio(inode, folio, blk, may_skip);
		if (error == -EAGAIN) {

			folio_end_writeback(folio);
			filemap_dirty_folio(mapping, folio);
			error = 0;
			continue;
		}
		folio_end_writeback(folio);
	}

	mempool_free(blk, &hammer2_blk_pool);
	memalloc_nofs_restore(nofs_flags);

	return error;
}

static ssize_t
hammer2_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	hammer2_inode_t *ip = VTOI(inode);
	hammer2_pfs_t *pmp = ip->pmp;
	struct timespec64 ts;
	uint64_t mtime;
	ssize_t ret;

	if (pmp->rdonly || (pmp->flags & HAMMER2_PMPF_EMERG))
		return -EROFS;

	hammer2_pfs_memory_wait(pmp);

	ret = generic_file_write_iter(iocb, from);

	if (ret > 0) {
		hammer2_trans_init(pmp, 0);
		hammer2_mtx_ex(&ip->lock);
		hammer2_update_time(&mtime);
		hammer2_inode_modify(ip);
		ip->meta.mtime = mtime;
		ip->meta.ctime = mtime;
		hammer2_mtx_unlock(&ip->lock);
		hammer2_trans_done(pmp, HAMMER2_TRANS_SIDEQ);

		hammer2_time_to_timespec(mtime, &ts);
		inode_set_mtime_to_ts(inode, ts);
		inode_set_ctime_to_ts(inode, ts);
	}
	return ret;
}

static struct inode *
hammer2_create_obj(struct inode *dir, struct dentry *dentry, umode_t mode,
    dev_t rdev, const char *symlink_target)
{
	hammer2_inode_t *dip = VTOI(dir);
	hammer2_pfs_t *pmp = dip->pmp;
	hammer2_inode_t *nip = NULL;
	struct inode *inode = NULL;
	struct vattr va;
	struct ucred cred;
	hammer2_tid_t inum;
	uint64_t mtime;
	int error;

	if (pmp->rdonly || (pmp->flags & HAMMER2_PMPF_EMERG))
		return ERR_PTR(-EROFS);
	if (dentry->d_name.len > HAMMER2_NAME_MAX)
		return ERR_PTR(-ENAMETOOLONG);

	memset(&cred, 0, sizeof(cred));
	cred.uid = from_kuid(&init_user_ns, current_fsuid());
	cred.gid = from_kgid(&init_user_ns, current_fsgid());

	if (dir->i_mode & S_ISGID) {
		cred.gid = from_kgid(&init_user_ns, dir->i_gid);
		if (S_ISDIR(mode))
			mode |= S_ISGID;
	}

	memset(&va, 0, sizeof(va));
	va.va_type = hammer2_ifmt_to_dtype(mode);
	va.va_mode = mode;
	va.va_uid = cred.uid;
	va.va_gid = cred.gid;
	va.va_rdev = rdev;

	hammer2_trans_init(pmp, 0);
	inum = hammer2_trans_newinum(pmp);

	hammer2_inode_lock(dip, 0);
	nip = hammer2_inode_create_normal(dip, &va, &cred, inum, &error);
	if (error)
		error = hammer2_error_to_errno(error);
	else
		error = hammer2_dirent_create(dip, dentry->d_name.name,
		    dentry->d_name.len, nip->meta.inum, nip->meta.type);

	if (error) {
		if (nip) {
			hammer2_inode_unlink_finisher(nip, NULL);
			hammer2_inode_unlock(nip);
			nip = NULL;
		}
		inode = ERR_PTR(-error);
	} else {
		hammer2_inode_depend(dip, nip);
		inode = hammer2_iget(dir->i_sb, nip);
		hammer2_inode_unlock(nip);
	}

	if (!IS_ERR_OR_NULL(inode) && symlink_target) {

		size_t tlen = strlen(symlink_target);
		char *blk = kzalloc(HAMMER2_PBUFSIZE, GFP_KERNEL);

		if (!blk) {
			error = ENOMEM;
			inode = ERR_PTR(-ENOMEM);
		} else {
			hammer2_mtx_ex(&nip->lock);
			hammer2_inode_modify(nip);
			nip->osize = nip->meta.size;
			nip->meta.size = tlen;
			if (tlen > HAMMER2_EMBEDDED_BYTES) {
				atomic_set_int(&nip->flags,
				    HAMMER2_INODE_RESIZED);
				hammer2_inode_chain_sync(nip);
			}
			hammer2_mtx_unlock(&nip->lock);

			memcpy(blk, symlink_target, tlen);
			hammer2_trans_init(pmp, HAMMER2_TRANS_BUFCACHE);
			hammer2_strategy_block(inode, 0, blk, BIO_WRITE, 0);
			hammer2_trans_done(pmp, HAMMER2_TRANS_BUFCACHE);
			kfree(blk);
			i_size_write(inode, tlen);
		}
	}

	if (!IS_ERR_OR_NULL(inode)) {
		hammer2_update_time(&mtime);
		hammer2_inode_modify(dip);
		dip->meta.mtime = mtime;
		dip->meta.ctime = mtime;
		if (S_ISDIR(mode) && dip->meta.nlinks != 1) {
			++dip->meta.nlinks;
			inc_nlink(dir);
		}
		inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	}

	hammer2_inode_unlock(dip);
	hammer2_trans_done(pmp, HAMMER2_TRANS_SIDEQ);
	return inode;
}

static int
hammer2_create(struct mnt_idmap *idmap, struct inode *dir,
    struct dentry *dentry, umode_t mode)
{
	struct inode *inode = hammer2_create_obj(dir, dentry, mode, 0, NULL);

	if (IS_ERR(inode))
		return PTR_ERR(inode);
	d_instantiate(dentry, inode);
	return 0;
}

static int
hammer2_mknod(struct mnt_idmap *idmap, struct inode *dir,
    struct dentry *dentry, umode_t mode, dev_t rdev)
{
	struct inode *inode = hammer2_create_obj(dir, dentry, mode, rdev, NULL);

	if (IS_ERR(inode))
		return PTR_ERR(inode);
	d_instantiate(dentry, inode);
	return 0;
}

static struct dentry *
hammer2_mkdir(struct mnt_idmap *idmap, struct inode *dir,
    struct dentry *dentry, umode_t mode)
{
	struct inode *inode = hammer2_create_obj(dir, dentry, mode | S_IFDIR,
	    0, NULL);

	if (IS_ERR(inode))
		return ERR_CAST(inode);
	d_instantiate(dentry, inode);
	return NULL;
}

static int
hammer2_symlink(struct mnt_idmap *idmap, struct inode *dir,
    struct dentry *dentry, const char *symname)
{
	struct inode *inode;

	if (strlen(symname) >= HAMMER2_PBUFSIZE)
		return -ENAMETOOLONG;
	inode = hammer2_create_obj(dir, dentry, S_IFLNK | 0777, 0, symname);
	if (IS_ERR(inode))
		return PTR_ERR(inode);
	d_instantiate(dentry, inode);
	return 0;
}

static int
hammer2_link(struct dentry *old_dentry, struct inode *dir,
    struct dentry *dentry)
{
	struct inode *inode = d_inode(old_dentry);
	hammer2_inode_t *tdip = VTOI(dir);
	hammer2_inode_t *ip = VTOI(inode);
	hammer2_pfs_t *pmp = tdip->pmp;
	uint64_t cmtime;
	int error;

	if (pmp->rdonly || (pmp->flags & HAMMER2_PMPF_EMERG))
		return -EROFS;
	if (dentry->d_name.len > HAMMER2_NAME_MAX)
		return -ENAMETOOLONG;
	if (ip->meta.nlinks >= HAMMER2_LINK_MAX)
		return -EMLINK;

	hammer2_trans_init(pmp, 0);
	hammer2_inode_lock(tdip, 0);
	hammer2_inode_lock(ip, 0);
	hammer2_update_time(&cmtime);

	error = hammer2_dirent_create(tdip, dentry->d_name.name,
	    dentry->d_name.len, ip->meta.inum, ip->meta.type);
	if (error == 0) {
		hammer2_inode_modify(ip);
		++ip->meta.nlinks;
		ip->meta.ctime = cmtime;
		hammer2_inode_modify(tdip);
		tdip->meta.mtime = cmtime;
		tdip->meta.ctime = cmtime;
	}
	hammer2_inode_unlock(ip);
	hammer2_inode_unlock(tdip);
	hammer2_trans_done(pmp, HAMMER2_TRANS_SIDEQ);

	if (error == 0) {
		inc_nlink(inode);
		inode_set_ctime_current(inode);

		inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
		ihold(inode);
		d_instantiate(dentry, inode);
	}
	return -error;
}

static int
hammer2_unlink_obj(struct inode *dir, struct dentry *dentry, int isdir)
{
	hammer2_inode_t *dip = VTOI(dir);
	struct inode *inode = d_inode(dentry);
	hammer2_inode_t *ip = VTOI(inode);
	hammer2_pfs_t *pmp = dip->pmp;
	hammer2_xop_unlink_t *xop;
	uint64_t mtime;
	int error;

	if (pmp->rdonly)
		return -EROFS;

	hammer2_trans_init(pmp, 0);
	hammer2_inode_lock(dip, 0);

	xop = hammer2_xop_alloc(dip, HAMMER2_XOP_MODIFYING);
	hammer2_xop_setname(&xop->head, dentry->d_name.name, dentry->d_name.len);
	xop->isdir = isdir;
	xop->dopermanent = 0;
	hammer2_xop_start(&xop->head, &hammer2_unlink_desc);
	error = hammer2_xop_collect(&xop->head, 0);
	error = hammer2_error_to_errno(error);

	if (error == 0) {
		ip = hammer2_inode_get(dip->pmp, &xop->head, -1, -1);
		hammer2_xop_retire(&xop->head, HAMMER2_XOPMASK_VOP);
		if (ip) {
			hammer2_inode_unlink_finisher(ip, NULL);
			hammer2_inode_depend(dip, ip);
			hammer2_inode_unlock(ip);
		}
	} else {
		hammer2_xop_retire(&xop->head, HAMMER2_XOPMASK_VOP);
	}

	if (error == 0) {
		hammer2_update_time(&mtime);
		hammer2_inode_modify(dip);
		dip->meta.mtime = mtime;
		dip->meta.ctime = mtime;
		if (isdir && dip->meta.nlinks != 1)
			--dip->meta.nlinks;
	}
	hammer2_inode_unlock(dip);
	hammer2_trans_done(pmp, HAMMER2_TRANS_SIDEQ);

	if (error == 0) {
		if (isdir) {
			clear_nlink(inode);
			if (dir->i_nlink > 2)
				drop_nlink(dir);
		} else {
			if (inode->i_nlink)
				drop_nlink(inode);
		}
		inode_set_ctime_current(inode);
		inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	}
	return -error;
}

static int
hammer2_unlink(struct inode *dir, struct dentry *dentry)
{
	return hammer2_unlink_obj(dir, dentry, 0);
}

static int
hammer2_rmdir(struct inode *dir, struct dentry *dentry)
{
	return hammer2_unlink_obj(dir, dentry, 1);
}

static int
hammer2_rename(struct mnt_idmap *idmap, struct inode *fdir,
    struct dentry *fdentry, struct inode *tdir, struct dentry *tdentry,
    unsigned int flags)
{
	hammer2_inode_t *fdip = VTOI(fdir);
	hammer2_inode_t *tdip = VTOI(tdir);
	hammer2_inode_t *fip = VTOI(d_inode(fdentry));
	hammer2_inode_t *tip = d_inode(tdentry) ? VTOI(d_inode(tdentry)) : NULL;
	hammer2_pfs_t *pmp = fdip->pmp;
	hammer2_xop_nrename_t *xop4;
	hammer2_xop_scanlhc_t *sxop;
	hammer2_key_t tlhc, lhcbase;
	uint64_t mtime;
	int error;

	if (flags & ~RENAME_NOREPLACE)
		return -EINVAL;
	if (pmp->rdonly || (pmp->flags & HAMMER2_PMPF_EMERG))
		return -EROFS;

	if (tdentry->d_name.len > HAMMER2_NAME_MAX ||
	    fdentry->d_name.len > HAMMER2_NAME_MAX)
		return -ENAMETOOLONG;

	hammer2_trans_init(pmp, 0);
	hammer2_inode_ref(fip);

	hammer2_inode_lock(fdip, 0);
	if (tdip != fdip)
		hammer2_inode_lock(tdip, 0);
	hammer2_inode_lock(fip, 0);
	if (tip)
		hammer2_inode_lock(tip, 0);

	tlhc = hammer2_dirhash(tdentry->d_name.name, tdentry->d_name.len);
	lhcbase = tlhc;
	sxop = hammer2_xop_alloc(tdip, HAMMER2_XOP_MODIFYING);
	sxop->lhc = tlhc;
	hammer2_xop_start(&sxop->head, &hammer2_scanlhc_desc);
	while ((error = hammer2_xop_collect(&sxop->head, 0)) == 0) {
		if (tlhc != sxop->head.cluster.focus->bref.key)
			break;
		++tlhc;
	}
	error = hammer2_error_to_errno(error);
	hammer2_xop_retire(&sxop->head, HAMMER2_XOPMASK_VOP);
	if (error == ENOENT || error == -ENOENT) {
		++tlhc;
		error = 0;
	}
	if (error == 0 && ((lhcbase ^ tlhc) & ~HAMMER2_DIRHASH_LOMASK))
		error = ENOSPC;

	if (error == 0) {
		xop4 = hammer2_xop_alloc(fdip, HAMMER2_XOP_MODIFYING);
		xop4->lhc = tlhc;
		xop4->ip_key = fip->meta.name_key;
		hammer2_xop_setip2(&xop4->head, fip);
		hammer2_xop_setip3(&xop4->head, tdip);
		if (tip && tip->meta.type == HAMMER2_OBJTYPE_DIRECTORY)
			hammer2_xop_setip4(&xop4->head, tip);
		hammer2_xop_setname(&xop4->head, fdentry->d_name.name,
		    fdentry->d_name.len);
		hammer2_xop_setname2(&xop4->head, tdentry->d_name.name,
		    tdentry->d_name.len);
		hammer2_xop_start(&xop4->head, &hammer2_nrename_desc);
		error = hammer2_xop_collect(&xop4->head, 0);
		error = hammer2_error_to_errno(error);
		hammer2_xop_retire(&xop4->head, HAMMER2_XOPMASK_VOP);
		if (error == ENOENT || error == -ENOENT)
			error = 0;

		if (error == 0 &&
		    (fip->meta.name_key & HAMMER2_DIRHASH_VISIBLE)) {
			hammer2_inode_modify(fip);
			fip->meta.name_len = tdentry->d_name.len;
			fip->meta.name_key = tlhc;
		}
		if (error == 0) {
			hammer2_inode_modify(fip);
			fip->meta.iparent = tdip->meta.inum;
		}
	}

	if (error == 0 && tip)
		hammer2_inode_unlink_finisher(tip, NULL);

	if (error == 0) {
		hammer2_update_time(&mtime);

		hammer2_inode_modify(fip);
		fip->meta.ctime = mtime;

		if (tip) {
			hammer2_inode_modify(tip);
			tip->meta.ctime = mtime;
			if (d_inode(tdentry))
				inode_set_ctime_current(d_inode(tdentry));
		}
		hammer2_inode_modify(fdip);
		fdip->meta.mtime = mtime;
		fdip->meta.ctime = mtime;
		if (fip->meta.type == HAMMER2_OBJTYPE_DIRECTORY &&
		    fdip->meta.nlinks != 1)
			--fdip->meta.nlinks;
		hammer2_inode_modify(tdip);
		tdip->meta.mtime = mtime;
		tdip->meta.ctime = mtime;
		inode_set_ctime_current(d_inode(fdentry));
		inode_set_mtime_to_ts(fdir, inode_set_ctime_current(fdir));
		if (fdir != tdir)
			inode_set_mtime_to_ts(tdir, inode_set_ctime_current(tdir));
		if (fip->meta.type == HAMMER2_OBJTYPE_DIRECTORY &&
		    tdip->meta.nlinks != 1)
			++tdip->meta.nlinks;
	}

	if (tip)
		hammer2_inode_unlock(tip);
	hammer2_inode_unlock(fip);
	if (tdip != fdip)
		hammer2_inode_unlock(tdip);
	hammer2_inode_unlock(fdip);
	hammer2_inode_drop(fip);
	hammer2_trans_done(pmp, HAMMER2_TRANS_SIDEQ);

	if (error == 0 && tip && d_inode(tdentry)) {
		struct inode *tinode = d_inode(tdentry);

		if (S_ISDIR(tinode->i_mode))
			clear_nlink(tinode);
		else if (tinode->i_nlink)
			drop_nlink(tinode);
	}
	if (error == 0 && fip->meta.type == HAMMER2_OBJTYPE_DIRECTORY &&
	    fdir != tdir) {
		if (fdir->i_nlink > 2)
			drop_nlink(fdir);
		inc_nlink(tdir);
	}
	return -error;
}

static int
hammer2_flush_devices(hammer2_pfs_t *pmp)
{
	hammer2_dev_t *hmp;
	struct block_device *bdev;
	int i, j, error = 0, e;

	for (i = 0; i < HAMMER2_MAXCLUSTER; ++i) {
		hmp = pmp->pfs_hmps[i];
		if (hmp == NULL)
			continue;
		for (j = 0; j < hmp->nvolumes; ++j) {
			if (hmp->volumes[j].dev == NULL)
				continue;
			bdev = hmp->volumes[j].dev->bdev;
			if (bdev == NULL)
				continue;
			e = sync_blockdev(bdev);
			if (e && error == 0)
				error = e;
			e = blkdev_issue_flush(bdev);
			if (e && error == 0)
				error = e;
		}
	}
	return error;
}

static int
hammer2_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
	struct inode *inode = file_inode(file);
	hammer2_inode_t *ip = VTOI(inode);
	int error, error2;

	error = file_write_and_wait_range(file, start, end);
	if (error)
		return error;

	hammer2_trans_init(ip->pmp, 0);
	hammer2_inode_lock(ip, 0);
	error = 0;
	if (ip->flags & (HAMMER2_INODE_RESIZED | HAMMER2_INODE_MODIFIED))
		error = hammer2_inode_chain_sync(ip);
	error2 = hammer2_inode_chain_flush(ip, HAMMER2_XOP_INODE_STOP);
	if (error2)
		error = error2;
	hammer2_inode_unlock(ip);
	hammer2_trans_done(ip->pmp, 0);

	if (error == 0 && hammer2_fsync_durable) {
		hammer2_vfs_sync_pmp(ip->pmp, MNT_WAIT);
		return -hammer2_flush_devices(ip->pmp);
	}

	return -hammer2_error_to_errno(error);
}

static long
hammer2_unlocked_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct inode *inode = file_inode(file);
	void __user *uarg = (void __user *)arg;
	unsigned int size = _IOC_SIZE(cmd);
	void *kdata = NULL;
	int fflag, error;

	if (size > PAGE_SIZE)
		return -EINVAL;
	if (size) {
		kdata = kzalloc(size, GFP_KERNEL);
		if (!kdata)
			return -ENOMEM;
		if ((_IOC_DIR(cmd) & _IOC_WRITE) &&
		    copy_from_user(kdata, uarg, size)) {
			kfree(kdata);
			return -EFAULT;
		}
	}

	fflag = ((file->f_mode & FMODE_READ) ? 1 : 0) |
		((file->f_mode & FMODE_WRITE) ? 2 : 0);

	error = hammer2_ioctl_linux(inode, cmd, kdata, fflag);
	if (error > 0)
		error = -error;

	if (error == 0 && size && (_IOC_DIR(cmd) & _IOC_READ) &&
	    copy_to_user(uarg, kdata, size))
		error = -EFAULT;

	kfree(kdata);
	return error;
}

static int
hammer2_linux_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb = dentry->d_sb;
	hammer2_pfs_t *pmp = sb->s_fs_info;
	struct h2statfs *h2;
	int error;

	h2 = kzalloc(sizeof(*h2), GFP_KERNEL);
	if (!h2)
		return -ENOMEM;
	error = hammer2_statfs(pmp->mp, h2);
	if (error) {
		kfree(h2);
		return -error;
	}

	buf->f_type = HAMMER2_VOLUME_ID_HBO;
	buf->f_bsize = HAMMER2_PBUFSIZE;
	buf->f_blocks = h2->f_blocks;
	buf->f_bfree = h2->f_bfree;
	buf->f_bavail = h2->f_bavail;
	buf->f_files = h2->f_files;

	if (!capable(CAP_SYS_RESOURCE)) {
		hammer2_dev_t *hmp = pmp->pfs_hmps[0];
		u64 reserve = hmp ? (hmp->free_reserved / HAMMER2_PBUFSIZE) : 0;

		if (buf->f_bavail > reserve)
			buf->f_bavail -= reserve;
		else
			buf->f_bavail = 0;
	}
	buf->f_ffree = h2->f_ffree;
	buf->f_namelen = HAMMER2_NAME_MAX;
	kfree(h2);
	return 0;
}

static int
hammer2_sync_fs(struct super_block *sb, int wait)
{
	hammer2_pfs_t *pmp = sb->s_fs_info;
	int error;

	error = hammer2_sync(pmp->mp, wait ? MNT_WAIT : MNT_NOWAIT);
	if (error == 0 && wait && hammer2_fsync_durable)
		error = hammer2_flush_devices(pmp);
	return -error;
}

static void
hammer2_evict_inode(struct inode *inode)
{
	hammer2_inode_t *ip = VTOI(inode);

	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);

	if (ip) {

		hammer2_inode_lock(ip, 0);
		if ((ip->flags & HAMMER2_INODE_ISUNLINKED) &&
		    !(ip->flags & HAMMER2_INODE_DELETING)) {
			atomic_set_int(&ip->flags, HAMMER2_INODE_DELETING);
			hammer2_inode_delayed_sideq(ip);
		}
		if (ip->vp == inode)
			ip->vp = NULL;
		inode->i_private = NULL;
		hammer2_inode_unlock(ip);
		hammer2_inode_drop(ip);
	}
}

static void
hammer2_put_super(struct super_block *sb)
{
	hammer2_pfs_t *pmp = sb->s_fs_info;
	struct mount *mp;

	if (!pmp)
		return;
	mp = pmp->mp;
	hammer2_unmount(mp, 0);
	sb->s_fs_info = NULL;
	if (mp) {
		kfree(mp->mnt_optnew);
		kfree(mp);
	}
}

static const struct super_operations hammer2_super_ops = {
	.statfs		= hammer2_linux_statfs,
	.sync_fs	= hammer2_sync_fs,
	.evict_inode	= hammer2_evict_inode,
	.put_super	= hammer2_put_super,
};

static int
hammer2_update_time_op(struct inode *inode, enum fs_update_time type,
    unsigned int flags)
{
	hammer2_inode_t *ip = VTOI(inode);
	hammer2_pfs_t *pmp = ip->pmp;
	struct timespec64 ts;

	if (pmp == NULL || pmp->rdonly || (pmp->flags & HAMMER2_PMPF_EMERG))
		return 0;

	generic_update_time(inode, type, flags);

	if (hammer2_mtx_ex_try(&ip->lock) != 0)
		return 0;
	hammer2_inode_modify(ip);

	if (type == FS_UPD_ATIME) {
		ts = inode_get_atime(inode);
		ip->meta.atime = hammer2_timespec_to_time(&ts);
	} else {
		ts = inode_get_mtime(inode);
		ip->meta.mtime = hammer2_timespec_to_time(&ts);
		ts = inode_get_ctime(inode);
		ip->meta.ctime = hammer2_timespec_to_time(&ts);
	}
	hammer2_mtx_unlock(&ip->lock);

	return 0;
}

static const struct inode_operations hammer2_dir_iops = {
	.update_time	= hammer2_update_time_op,
	.lookup		= hammer2_lookup,
	.create		= hammer2_create,
	.link		= hammer2_link,
	.unlink		= hammer2_unlink,
	.symlink	= hammer2_symlink,
	.mkdir		= hammer2_mkdir,
	.rmdir		= hammer2_rmdir,
	.mknod		= hammer2_mknod,
	.rename		= hammer2_rename,
	.getattr	= hammer2_getattr,
	.setattr	= hammer2_setattr,
};

static const struct inode_operations hammer2_file_iops = {
	.update_time	= hammer2_update_time_op,
	.getattr	= hammer2_getattr,
	.setattr	= hammer2_setattr,
};

static const struct inode_operations hammer2_symlink_iops = {
	.update_time	= hammer2_update_time_op,
	.get_link	= hammer2_get_link,
	.getattr	= hammer2_getattr,
	.setattr	= hammer2_setattr,
};

static const struct inode_operations hammer2_special_iops = {
	.update_time	= hammer2_update_time_op,
	.getattr	= hammer2_getattr,
	.setattr	= hammer2_setattr,
};

static const struct file_operations hammer2_dir_fops = {
	.read		= generic_read_dir,
	.iterate_shared	= hammer2_iterate,
	.llseek		= generic_file_llseek,
	.fsync		= hammer2_fsync,
	.unlocked_ioctl	= hammer2_unlocked_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

static const struct file_operations hammer2_file_fops = {
	.llseek		= generic_file_llseek,
	.read_iter	= generic_file_read_iter,
	.write_iter	= hammer2_write_iter,
	.mmap		= generic_file_mmap,
	.fsync		= hammer2_fsync,

	.splice_read	= filemap_splice_read,
	.splice_write	= iter_file_splice_write,
	.unlocked_ioctl	= hammer2_unlocked_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

static const struct address_space_operations hammer2_aops = {
	.read_folio	= hammer2_read_folio,
	.writepages	= hammer2_writepages,
	.write_begin	= hammer2_write_begin,
	.write_end	= hammer2_write_end,
	.dirty_folio	= filemap_dirty_folio,

	.migrate_folio	= filemap_migrate_folio,
};

static int
hammer2_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct h2_mount_optlist *ol;
	struct mount *mp;
	hammer2_pfs_t *pmp;
	struct inode *root_inode;
	char *devstr;
	int *hflags;
	int error;

	if (!fc->source)
		return -EINVAL;

	mp = kzalloc(sizeof(*mp), GFP_KERNEL);
	ol = kzalloc(sizeof(*ol), GFP_KERNEL);
	devstr = kstrdup(fc->source, GFP_KERNEL);
	hflags = kzalloc(sizeof(int), GFP_KERNEL);
	if (!mp || !ol || !devstr || !hflags) {
		error = -ENOMEM;
		goto fail_free;
	}

	ol->count = 3;
	ol->opts[0].name = "from";
	ol->opts[0].value = devstr;
	ol->opts[0].len = strlen(devstr) + 1;
	ol->opts[1].name = "fspath";
	ol->opts[1].value = devstr;
	ol->opts[1].len = strlen(devstr) + 1;
	ol->opts[2].name = "hflags";
	ol->opts[2].value = hflags;
	ol->opts[2].len = sizeof(int);

	mp->mnt_optnew = ol;
	mp->mnt_flag = (fc->sb_flags & SB_RDONLY) ? MNT_RDONLY : 0;
	mp->mnt_iosize_max = MAXPHYS;

	error = hammer2_mount(mp);
	if (error) {

		if (error > 0)
			error = -error;
		goto fail_free;
	}

	pmp = (hammer2_pfs_t *)mp->mnt_data;
	if (!pmp) {
		error = -EINVAL;
		goto fail_unmount;
	}

	sb->s_fs_info = pmp;
	pmp->sb = sb;
	pmp->mp = mp;

	sb->s_magic = HAMMER2_VOLUME_ID_HBO;
	sb->s_blocksize = HAMMER2_PBUFSIZE;
	sb->s_blocksize_bits = HAMMER2_PBUFRADIX;
	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_op = &hammer2_super_ops;
	sb->s_time_gran = 1000;

	error = super_setup_bdi(sb);
	if (error)
		goto fail_unmount;

	if (pmp->rdonly)
		sb->s_flags |= SB_RDONLY;

	error = hammer2_root(mp, 0, &root_inode);
	if (error) {
		if (error > 0)
			error = -error;
		goto fail_unmount;
	}

	sb->s_root = d_make_root(root_inode);
	if (!sb->s_root) {
		error = -ENOMEM;
		goto fail_unmount;
	}

	kfree(devstr);
	return 0;

fail_unmount:
	hammer2_unmount(mp, 0);
	sb->s_fs_info = NULL;
fail_free:
	kfree(mp);
	kfree(ol);
	kfree(devstr);
	kfree(hflags);
	return error;
}

static int
hammer2_get_tree(struct fs_context *fc)
{
	return get_tree_nodev(fc, hammer2_fill_super);
}

static int
hammer2_reconfigure(struct fs_context *fc)
{

	return 0;
}

static const struct fs_context_operations hammer2_context_ops = {
	.get_tree	= hammer2_get_tree,
	.reconfigure	= hammer2_reconfigure,
};

static int
hammer2_init_fs_context(struct fs_context *fc)
{
	fc->ops = &hammer2_context_ops;
	return 0;
}

static struct file_system_type hammer2_fs_type = {
	.owner			= THIS_MODULE,
	.name			= "hammer2",
	.init_fs_context	= hammer2_init_fs_context,
	.kill_sb		= kill_anon_super,
	.fs_flags		= 0,
};
MODULE_ALIAS_FS("hammer2");

static int __init
hammer2_module_init(void)
{
	int error;

	error = hammer2_init(NULL);
	if (error)
		return error;

	error = hammer2_blk_pool_init();
	if (error)
		return error;

	error = register_filesystem(&hammer2_fs_type);
	if (error) {
		pr_err("hammer2: register_filesystem failed: %d\n", error);
		hammer2_uninit(NULL);
		return error;
	}
	pr_info("hammer2: filesystem registered, version %s\n",
	    HAMMER2_PORT_BUILD);
	return 0;
}

static void __exit
hammer2_module_exit(void)
{
	unregister_filesystem(&hammer2_fs_type);

	hammer2_blk_pool_exit();

	rcu_barrier();
	hammer2_uninit(NULL);
	pr_info("hammer2: filesystem unregistered\n");
}

module_init(hammer2_module_init);
module_exit(hammer2_module_exit);

MODULE_INFO(version, HAMMER2_PORT_BUILD);
