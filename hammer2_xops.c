#include "hammer2.h"

static int
checkdirempty(hammer2_chain_t *oparent, hammer2_chain_t *ochain, int clindex)
{
	hammer2_chain_t *parent, *chain;
	hammer2_key_t key_next, inum;
	int error = 0, didunlock = 0;

	if (ochain->bref.type == HAMMER2_BREF_TYPE_DIRENT) {
		inum = ochain->bref.embed.dirent.inum;
		hammer2_chain_unlock(ochain);
		hammer2_chain_unlock(oparent);

		parent = NULL;
		chain = NULL;
		error = hammer2_chain_inode_find(ochain->pmp, inum, clindex, 0,
		    &parent, &chain);
		if (parent) {
			hammer2_chain_unlock(parent);
			hammer2_chain_drop(parent);
		}
		didunlock = 1;
	} else {

		chain = hammer2_chain_lookup_init(ochain, 0);
	}

	if (error == 0) {
		parent = chain;
		chain = NULL;
		if (parent)
			chain = hammer2_chain_lookup(&parent, &key_next,
			    HAMMER2_DIRHASH_VISIBLE, HAMMER2_KEY_MAX, &error,
			    0);
		if (chain) {
			error = HAMMER2_ERROR_ENOTEMPTY;
			hammer2_chain_unlock(chain);
			hammer2_chain_drop(chain);
		}
		hammer2_chain_lookup_done(parent);
	} else {
		if (chain) {
			hammer2_chain_unlock(chain);
			hammer2_chain_drop(chain);
			chain = NULL;
		}
	}

	if (didunlock) {
		hammer2_chain_lock(oparent, HAMMER2_RESOLVE_ALWAYS);
		hammer2_chain_lock(ochain, HAMMER2_RESOLVE_ALWAYS);
		if ((ochain->flags & HAMMER2_CHAIN_DELETED) ||
		    (oparent->flags & HAMMER2_CHAIN_DELETED) ||
		    ochain->parent != oparent) {
			hprintf("CHECKDIR inum %016llx RETRY\n",
			    (long long)inum);
			error = HAMMER2_ERROR_EAGAIN;
		}
	}
	return (error);
}

void
hammer2_xop_ipcluster(hammer2_xop_t *arg, void *scratch, int clindex)
{
	hammer2_xop_ipcluster_t *xop = &arg->xop_ipcluster;
	hammer2_chain_t *chain;
	int error;

	chain = hammer2_inode_chain(xop->head.ip1, clindex,
	    HAMMER2_RESOLVE_ALWAYS | HAMMER2_RESOLVE_SHARED);
	if (chain)
		error = chain->error;
	else
		error = HAMMER2_ERROR_EIO;

	hammer2_xop_feed(&xop->head, chain, clindex, error);
	if (chain) {
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
	}
}

static int
hammer2_xop_readdir_bref(hammer2_xop_readdir_t *xop, hammer2_chain_t **parentp,
    hammer2_key_t lkey, int clindex)
{
	hammer2_blockref_t bref;
	hammer2_chain_t *chain;
	hammer2_key_t key_next;
	int lflags = HAMMER2_LOOKUP_ALWAYS | HAMMER2_LOOKUP_SHARED;
	int error = 0, have;

	have = hammer2_chain_lookup_bref(parentp, &key_next, lkey, lkey, &bref,
	    &error, lflags);
	if (!have && error == 0)
		have = hammer2_chain_lookup_bref(parentp, &key_next, lkey,
		    HAMMER2_KEY_MAX, &bref, &error,
		    lflags | HAMMER2_LOOKUP_ITERATE);

	while (have && error == 0) {
		chain = NULL;
		if (bref.type != HAMMER2_BREF_TYPE_DIRENT ||
		    bref.embed.dirent.namlen > sizeof(bref.check.buf)) {

			chain = hammer2_chain_lookup(parentp, &key_next,
			    bref.key, bref.key, &error, lflags);
			if (error)
				break;
			if (chain == NULL) {

				have = hammer2_chain_next_bref(parentp,
				    &key_next, HAMMER2_KEY_MAX, &bref, &error,
				    lflags | HAMMER2_LOOKUP_ITERATE);
				continue;
			}
		}

		error = hammer2_xop_feed_bref(&xop->head, &bref, chain, clindex,
		    0);
		if (chain) {
			hammer2_chain_unlock(chain);
			hammer2_chain_drop(chain);
		}
		if (error)
			break;

		have = hammer2_chain_next_bref(parentp, &key_next,
		    HAMMER2_KEY_MAX, &bref, &error,
		    lflags | HAMMER2_LOOKUP_ITERATE);
	}
	return (error);
}

void
hammer2_xop_readdir(hammer2_xop_t *arg, void *scratch, int clindex)
{
	hammer2_xop_readdir_t *xop = &arg->xop_readdir;
	hammer2_chain_t *chain, *parent;
	hammer2_key_t lkey, key_next;
	int error = 0;

	lkey = xop->lkey;

	parent = hammer2_inode_chain(xop->head.ip1, clindex,
	    HAMMER2_RESOLVE_ALWAYS | HAMMER2_RESOLVE_SHARED);
	if (parent == NULL) {
		hprintf("NULL parent\n");
		goto done;
	}

	if (xop->head.flags & HAMMER2_XOP_FIFO_BREF) {
		error = hammer2_xop_readdir_bref(xop, &parent, lkey, clindex);
		hammer2_chain_unlock(parent);
		hammer2_chain_drop(parent);
		goto done;
	}

	chain = hammer2_chain_lookup(&parent, &key_next, lkey, lkey, &error,
	    HAMMER2_LOOKUP_ALWAYS | HAMMER2_LOOKUP_SHARED);
	if (chain == NULL)
		chain = hammer2_chain_lookup(&parent, &key_next, lkey,
		    HAMMER2_KEY_MAX, &error,
		    HAMMER2_LOOKUP_ALWAYS | HAMMER2_LOOKUP_SHARED |
		    HAMMER2_LOOKUP_ITERATE);
	while (chain) {
		error = hammer2_xop_feed(&xop->head, chain, clindex, 0);
		if (error)
			goto break2;
		chain = hammer2_chain_next(&parent, chain, &key_next,
		    HAMMER2_KEY_MAX, &error,
		    HAMMER2_LOOKUP_ALWAYS | HAMMER2_LOOKUP_SHARED |
		    HAMMER2_LOOKUP_ITERATE);
	}
break2:
	if (chain) {
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
	}
	hammer2_chain_unlock(parent);
	hammer2_chain_drop(parent);
done:
	hammer2_xop_feed(&xop->head, NULL, clindex, error);
}

void
hammer2_xop_nresolve(hammer2_xop_t *arg, void *scratch, int clindex)
{
	hammer2_xop_nresolve_t *xop = &arg->xop_nresolve;
	hammer2_chain_t *chain, *parent;
	hammer2_key_t lhc, key_next;
	const char *name;
	size_t name_len;
	int error;

	chain = NULL;
	parent = hammer2_inode_chain(xop->head.ip1, clindex,
	    HAMMER2_RESOLVE_ALWAYS | HAMMER2_RESOLVE_SHARED);
	if (parent == NULL) {
		hprintf("NULL parent\n");
		error = HAMMER2_ERROR_EIO;
		goto done;
	}
	name = xop->head.name1;
	name_len = xop->head.name1_len;

	lhc = hammer2_dirhash(name, name_len);
	chain = hammer2_chain_lookup(&parent, &key_next, lhc,
	    lhc + HAMMER2_DIRHASH_LOMASK, &error,
	    HAMMER2_LOOKUP_ALWAYS | HAMMER2_LOOKUP_SHARED);
	while (chain) {
		if (hammer2_chain_dirent_test(chain, name, name_len))
			break;
		chain = hammer2_chain_next(&parent, chain, &key_next,
		    lhc + HAMMER2_DIRHASH_LOMASK, &error,
		    HAMMER2_LOOKUP_ALWAYS | HAMMER2_LOOKUP_SHARED);
	}

	if (chain && chain->error == 0) {
		if (chain->bref.type == HAMMER2_BREF_TYPE_DIRENT) {
			lhc = chain->bref.embed.dirent.inum;
			error = hammer2_chain_inode_find(chain->pmp, lhc,
			    clindex, HAMMER2_LOOKUP_SHARED, &parent, &chain);
		}
	} else if (chain && error == 0) {
		error = chain->error;
	}
done:
	hammer2_xop_feed(&xop->head, chain, clindex, error);
	if (chain) {
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
	}
	if (parent) {
		hammer2_chain_unlock(parent);
		hammer2_chain_drop(parent);
	}
}

void
hammer2_xop_unlink(hammer2_xop_t *arg, void *scratch, int clindex)
{
	hammer2_xop_unlink_t *xop = &arg->xop_unlink;
	hammer2_chain_t *parent, *chain;
	hammer2_key_t key_next, lhc;
	const char *name;
	size_t name_len;
	uint8_t type;
	int error, error2, dopermanent, doforce;
again:

	parent = hammer2_inode_chain(xop->head.ip1, clindex,
	    HAMMER2_RESOLVE_ALWAYS);
	chain = NULL;
	if (parent == NULL) {
		hprintf("NULL parent\n");
		error = HAMMER2_ERROR_EIO;
		goto done;
	}
	name = xop->head.name1;
	name_len = xop->head.name1_len;

	lhc = hammer2_dirhash(name, name_len);
	chain = hammer2_chain_lookup(&parent, &key_next, lhc,
	    lhc + HAMMER2_DIRHASH_LOMASK, &error, HAMMER2_LOOKUP_ALWAYS);
	while (chain) {
		if (hammer2_chain_dirent_test(chain, name, name_len))
			break;
		chain = hammer2_chain_next(&parent, chain, &key_next,
		    lhc + HAMMER2_DIRHASH_LOMASK, &error,
		    HAMMER2_LOOKUP_ALWAYS);
	}

	if (chain && chain->error == 0) {
		dopermanent = xop->dopermanent & H2DOPERM_PERMANENT;
		doforce = xop->dopermanent & H2DOPERM_FORCE;

		if (chain->bref.type == HAMMER2_BREF_TYPE_DIRENT) {
			type = chain->bref.embed.dirent.type;
			dopermanent |= HAMMER2_DELETE_PERMANENT;
		} else {
			type = chain->data->ipdata.meta.type;
		}

		if (type == HAMMER2_OBJTYPE_DIRECTORY && doforce) {

			error = hammer2_chain_delete(parent, chain,
			    xop->head.mtid, dopermanent);
		} else if (type == HAMMER2_OBJTYPE_DIRECTORY &&
		    xop->isdir == 0) {
			error = HAMMER2_ERROR_EISDIR;
		} else if (type == HAMMER2_OBJTYPE_DIRECTORY &&
		    (error = checkdirempty(parent, chain, clindex)) != 0) {

			if (error == HAMMER2_ERROR_EAGAIN) {
				hammer2_chain_unlock(chain);
				hammer2_chain_drop(chain);
				hammer2_chain_unlock(parent);
				hammer2_chain_drop(parent);
				goto again;
			}
		} else if (type != HAMMER2_OBJTYPE_DIRECTORY &&
		    xop->isdir >= 1) {
			error = HAMMER2_ERROR_ENOTDIR;
		} else {

			error = hammer2_chain_delete(parent, chain,
			    xop->head.mtid, dopermanent);
			if (error == 0)
				error = chain->error;
		}
	} else {
		if (chain && error == 0)
			error = chain->error;
	}

	if (chain && chain->bref.type == HAMMER2_BREF_TYPE_DIRENT &&
	    (xop->dopermanent & H2DOPERM_IGNINO) == 0) {
		lhc = chain->bref.embed.dirent.inum;
		error2 = hammer2_chain_inode_find(chain->pmp, lhc, clindex, 0,
		    &parent, &chain);
		if (error2) {
			hprintf("lhc %016llx failed\n", (long long)lhc);
			error2 = 0;
		}
		if (error == 0)
			error = error2;
	}

done:
	hammer2_xop_feed(&xop->head, chain, clindex, error);
	if (chain) {
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
		chain = NULL;
	}
	if (parent) {
		hammer2_chain_unlock(parent);
		hammer2_chain_drop(parent);
		parent = NULL;
	}
}

void
hammer2_xop_nrename(hammer2_xop_t *arg, void *scratch, int clindex)
{
	hammer2_xop_nrename_t *xop = &arg->xop_nrename;
	hammer2_pfs_t *pmp;
	hammer2_chain_t *parent, *chain, *tmp;
	hammer2_inode_t *ip;
	hammer2_inode_data_t *wipdata;
	hammer2_key_t key_next, lhc;
	const char *name;
	size_t name_len;
	int e2, error;

	ip = xop->head.ip4;
	if (ip) {
		chain = hammer2_inode_chain(ip, clindex,
		    HAMMER2_RESOLVE_ALWAYS);
		if (chain == NULL) {
			error = HAMMER2_ERROR_EIO;
			parent = NULL;
			goto done;
		}
		if (chain->data->ipdata.meta.type == HAMMER2_OBJTYPE_DIRECTORY &&
		    (error = checkdirempty(NULL, chain, clindex)) != 0) {
			KKASSERT(error != HAMMER2_ERROR_EAGAIN);
			parent = NULL;
			goto done;
		}
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
	}

	ip = xop->head.ip2;
	pmp = ip->pmp;
	chain = NULL;
	error = 0;

	if (xop->ip_key & HAMMER2_DIRHASH_VISIBLE) {

		chain = hammer2_inode_chain(ip, clindex,
		    HAMMER2_RESOLVE_ALWAYS);
		if (chain == NULL) {
			error = HAMMER2_ERROR_EIO;
			parent = NULL;
			goto done;
		}
		if (ip->flags & HAMMER2_INODE_CREATING) {
			parent = NULL;
		} else {
			parent = hammer2_chain_getparent(chain,
			    HAMMER2_RESOLVE_ALWAYS);
			if (parent == NULL) {
				error = HAMMER2_ERROR_EIO;
				goto done;
			}
		}
	} else {

		parent = hammer2_inode_chain(xop->head.ip1, clindex,
		    HAMMER2_RESOLVE_ALWAYS);
		if (parent == NULL) {
			hprintf("NULL parent\n");
			error = HAMMER2_ERROR_EIO;
			goto done;
		}
		name = xop->head.name1;
		name_len = xop->head.name1_len;

		lhc = hammer2_dirhash(name, name_len);
		chain = hammer2_chain_lookup(&parent, &key_next, lhc,
		    lhc + HAMMER2_DIRHASH_LOMASK, &error,
		    HAMMER2_LOOKUP_ALWAYS);
		while (chain) {
			if (hammer2_chain_dirent_test(chain, name, name_len))
				break;
			chain = hammer2_chain_next(&parent, chain, &key_next,
			    lhc + HAMMER2_DIRHASH_LOMASK, &error,
			    HAMMER2_LOOKUP_ALWAYS);
		}
	}

	if (chain == NULL) {

		hprintf("\"%s\" -> \"%s\" ENOENT\n",
		    xop->head.name1, xop->head.name2);
		if (error == 0)
			error = HAMMER2_ERROR_ENOENT;
		goto done;
	}

	if (chain->error) {
		error = chain->error;
		goto done;
	}

	error = hammer2_chain_delete(parent, chain, xop->head.mtid, 0);
	if (parent) {
		hammer2_chain_unlock(parent);
		hammer2_chain_drop(parent);
		parent = NULL;
	} else {
		hprintf("NULL parent\n");
		error = EINVAL;
	}
	if (error)
		goto done;

	if (chain->bref.key != xop->lhc ||
	    xop->head.name1_len != xop->head.name2_len ||
	    bcmp(xop->head.name1, xop->head.name2, xop->head.name1_len) != 0) {
		if (chain->bref.type == HAMMER2_BREF_TYPE_INODE) {
			error = hammer2_chain_modify(chain, xop->head.mtid, 0, 0);
			if (error == 0) {
				wipdata = &chain->data->ipdata;
				bzero(wipdata->filename,
				    sizeof(wipdata->filename));
				bcopy(xop->head.name2, wipdata->filename,
				    xop->head.name2_len);
				wipdata->meta.name_key = xop->lhc;
				wipdata->meta.name_len = xop->head.name2_len;
			}
		}
		if (chain->bref.type == HAMMER2_BREF_TYPE_DIRENT) {
			if (xop->head.name2_len <=
			    sizeof(chain->bref.check.buf)) {

				error = hammer2_chain_resize(chain,
				    xop->head.mtid, 0, 0, 0);
				if (error == 0)
					error = hammer2_chain_modify(chain,
					    xop->head.mtid, 0, 0);
				if (error == 0) {
					bzero(chain->bref.check.buf,
					    sizeof(chain->bref.check.buf));
					bcopy(xop->head.name2,
					    chain->bref.check.buf,
					    xop->head.name2_len);
				}
			} else {

				error = hammer2_chain_resize(chain,
				    xop->head.mtid, 0,
				    hammer2_getradix(HAMMER2_ALLOC_MIN), 0);
				if (error == 0)
					error = hammer2_chain_modify(chain,
					    xop->head.mtid, 0, 0);
				if (error == 0) {
					bzero(chain->data->buf, chain->bytes);
					bcopy(xop->head.name2, chain->data->buf,
					    xop->head.name2_len);
				}
			}
			if (error == 0)
				chain->bref.embed.dirent.namlen =
				    xop->head.name2_len;
		}
	}

	if (chain->bref.type == HAMMER2_BREF_TYPE_INODE &&
	    chain->data->ipdata.meta.iparent != xop->head.ip3->meta.inum) {
		error = hammer2_chain_modify(chain, xop->head.mtid, 0, 0);
		if (error == 0) {
			wipdata = &chain->data->ipdata;
			wipdata->meta.iparent = xop->head.ip3->meta.inum;
		}
	}

	parent = hammer2_inode_chain(xop->head.ip3, clindex,
	    HAMMER2_RESOLVE_ALWAYS);
	if (parent == NULL) {
		error = HAMMER2_ERROR_EIO;
		goto done;
	}

	if (error == 0) {
		tmp = hammer2_chain_lookup(&parent, &key_next,
		    xop->lhc & ~HAMMER2_DIRHASH_LOMASK,
		    xop->lhc | HAMMER2_DIRHASH_LOMASK,
		    &error, HAMMER2_LOOKUP_ALWAYS);
		while (tmp) {
			if (hammer2_chain_dirent_test(tmp, xop->head.name2,
			    xop->head.name2_len)) {
				e2 = hammer2_chain_delete(parent, tmp,
				    xop->head.mtid, 0);
				if (error == 0 && e2)
					error = e2;
			}
			tmp = hammer2_chain_next(&parent, tmp, &key_next,
			    xop->lhc | HAMMER2_DIRHASH_LOMASK, &error,
			    HAMMER2_LOOKUP_ALWAYS);
		}
	}
	if (error == 0) {

		tmp = hammer2_chain_lookup(&parent, &key_next, xop->lhc,
		    xop->lhc, &error, 0);
		KKASSERT(tmp == NULL);
		error = hammer2_chain_create(&parent, &chain, NULL, pmp,
		    HAMMER2_METH_DEFAULT, xop->lhc, 0, HAMMER2_BREF_TYPE_INODE,
		    HAMMER2_INODE_BYTES, xop->head.mtid, 0, 0);
	}
done:
	hammer2_xop_feed(&xop->head, NULL, clindex, error);
	if (parent) {
		hammer2_chain_unlock(parent);
		hammer2_chain_drop(parent);
	}
	if (chain) {
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
	}
}

void
hammer2_xop_scanlhc(hammer2_xop_t *arg, void *scratch, int clindex)
{
	hammer2_xop_scanlhc_t *xop = &arg->xop_scanlhc;
	hammer2_chain_t *parent, *chain;
	hammer2_key_t key_next;
	int error = 0;

	parent = hammer2_inode_chain(xop->head.ip1, clindex,
	    HAMMER2_RESOLVE_ALWAYS | HAMMER2_RESOLVE_SHARED);
	if (parent == NULL) {
		hprintf("NULL parent\n");
		chain = NULL;
		error = HAMMER2_ERROR_EIO;
		goto done;
	}

	chain = hammer2_chain_lookup(&parent, &key_next, xop->lhc,
	    xop->lhc + HAMMER2_DIRHASH_LOMASK, &error,
	    HAMMER2_LOOKUP_ALWAYS | HAMMER2_LOOKUP_SHARED);
	while (chain) {
		error = hammer2_xop_feed(&xop->head, chain, clindex, 0);
		if (error) {
			hammer2_chain_unlock(chain);
			hammer2_chain_drop(chain);
			chain = NULL;
			goto done;
		}
		chain = hammer2_chain_next(&parent, chain, &key_next,
		    xop->lhc + HAMMER2_DIRHASH_LOMASK, &error,
		    HAMMER2_LOOKUP_ALWAYS | HAMMER2_LOOKUP_SHARED);
	}
done:
	hammer2_xop_feed(&xop->head, NULL, clindex, error);
	if (parent) {
		hammer2_chain_unlock(parent);
		hammer2_chain_drop(parent);
	}
}

void
hammer2_xop_lookup(hammer2_xop_t *arg, void *scratch, int clindex)
{
	hammer2_xop_lookup_t *xop = &arg->xop_lookup;
	hammer2_chain_t *chain, *parent;
	hammer2_key_t key_next;
	int error = 0;

	chain = NULL;
	parent = hammer2_inode_chain(xop->head.ip1, clindex,
	    HAMMER2_RESOLVE_ALWAYS | HAMMER2_RESOLVE_SHARED);
	if (parent == NULL) {
		hprintf("NULL parent\n");
		error = HAMMER2_ERROR_EIO;
		goto done;
	}

	chain = hammer2_chain_lookup(&parent, &key_next, xop->lhc, xop->lhc,
	    &error, HAMMER2_LOOKUP_ALWAYS | HAMMER2_LOOKUP_SHARED);
	if (error == 0) {
		if (chain)
			error = chain->error;
		else
			error = HAMMER2_ERROR_ENOENT;
	}
	hammer2_xop_feed(&xop->head, chain, clindex, error);
done:
	if (chain) {
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
	}
	if (parent) {
		hammer2_chain_unlock(parent);
		hammer2_chain_drop(parent);
	}
}

void
hammer2_xop_delete(hammer2_xop_t *arg, void *scratch, int clindex)
{
	hammer2_xop_lookup_t *xop = &arg->xop_lookup;
	hammer2_chain_t *parent, *chain;
	hammer2_key_t key_next;
	int error = 0;

	parent = hammer2_inode_chain(xop->head.ip1, clindex,
	    HAMMER2_RESOLVE_ALWAYS);
	chain = NULL;
	if (parent == NULL) {
		error = HAMMER2_ERROR_EIO;
		goto done;
	}

	chain = hammer2_chain_lookup(&parent, &key_next, xop->lhc, xop->lhc,
	    &error, HAMMER2_LOOKUP_NODATA);
	if (error == 0) {
		if (chain)
			error = chain->error;
		else
			error = HAMMER2_ERROR_ENOENT;
	}
	if (chain)
		error = hammer2_chain_delete(parent, chain, xop->head.mtid,
		    HAMMER2_DELETE_PERMANENT);
	hammer2_xop_feed(&xop->head, NULL, clindex, error);
done:
	if (chain) {
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
	}
	if (parent) {
		hammer2_chain_unlock(parent);
		hammer2_chain_drop(parent);
	}
}

void
hammer2_xop_scanall(hammer2_xop_t *arg, void *scratch, int clindex)
{
	hammer2_xop_scanall_t *xop = &arg->xop_scanall;
	hammer2_chain_t *parent, *chain;
	hammer2_key_t key_next;
	int error = 0;

	KKASSERT(xop->resolve_flags & HAMMER2_RESOLVE_SHARED);
	KKASSERT(xop->lookup_flags & HAMMER2_LOOKUP_SHARED);

	parent = hammer2_inode_chain(xop->head.ip1, clindex,
	    xop->resolve_flags);
	if (parent == NULL) {
		hprintf("NULL parent\n");
		goto done;
	}

	chain = hammer2_chain_lookup(&parent, &key_next, xop->key_beg,
	    xop->key_end, &error, xop->lookup_flags);
	while (chain) {
		error = hammer2_xop_feed(&xop->head, chain, clindex, 0);
		if (error)
			goto break2;
		chain = hammer2_chain_next(&parent, chain, &key_next,
		    xop->key_end, &error, xop->lookup_flags);
	}
break2:
	if (chain) {
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
	}
	hammer2_chain_unlock(parent);
	hammer2_chain_drop(parent);
done:
	hammer2_xop_feed(&xop->head, NULL, clindex, error);
}

void
hammer2_xop_inode_mkdirent(hammer2_xop_t *arg, void *scratch, int clindex)
{
	hammer2_xop_mkdirent_t *xop = &arg->xop_mkdirent;
	hammer2_chain_t *parent, *chain;
	hammer2_key_t key_next;
	size_t data_len;
	int error;

	parent = hammer2_inode_chain(xop->head.ip1, clindex,
	    HAMMER2_RESOLVE_ALWAYS);
	if (parent == NULL) {
		error = HAMMER2_ERROR_EIO;
		chain = NULL;
		goto fail;
	}

	chain = hammer2_chain_lookup(&parent, &key_next, xop->lhc, xop->lhc,
	    &error, 0);
	if (chain) {
		error = HAMMER2_ERROR_EEXIST;
		goto fail;
	}

	if (xop->dirent.namlen <= sizeof(chain->bref.check.buf))
		data_len = 0;
	else
		data_len = HAMMER2_ALLOC_MIN;
	error = hammer2_chain_create(&parent, &chain, NULL, xop->head.ip1->pmp,
	    HAMMER2_METH_DEFAULT, xop->lhc, 0, HAMMER2_BREF_TYPE_DIRENT,
	    data_len, xop->head.mtid, 0, 0);
	if (error == 0) {

		error = hammer2_chain_modify(chain, xop->head.mtid, 0, 0);
		if (error == 0) {
			chain->bref.embed.dirent = xop->dirent;
			if (xop->dirent.namlen <= sizeof(chain->bref.check.buf))
				bcopy(xop->head.name1, chain->bref.check.buf,
				    xop->dirent.namlen);
			else
				bcopy(xop->head.name1, chain->data->buf,
				    xop->dirent.namlen);
		}
	}
fail:
	if (parent) {
		hammer2_chain_unlock(parent);
		hammer2_chain_drop(parent);
	}
	hammer2_xop_feed(&xop->head, chain, clindex, error);
	if (chain) {
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
	}
}

void
hammer2_xop_inode_create(hammer2_xop_t *arg, void *scratch, int clindex)
{
	hammer2_xop_create_t *xop = &arg->xop_create;
	hammer2_chain_t *parent, *chain;
	hammer2_key_t key_next;
	int error;

	parent = hammer2_inode_chain(xop->head.ip1, clindex,
	    HAMMER2_RESOLVE_ALWAYS);
	if (parent == NULL) {
		error = HAMMER2_ERROR_EIO;
		chain = NULL;
		goto fail;
	}

	chain = hammer2_chain_lookup(&parent, &key_next, xop->lhc, xop->lhc,
	    &error, 0);
	if (chain) {
		error = HAMMER2_ERROR_EEXIST;
		goto fail;
	}

	error = hammer2_chain_create(&parent, &chain, NULL, xop->head.ip1->pmp,
	    HAMMER2_METH_DEFAULT, xop->lhc, 0, HAMMER2_BREF_TYPE_INODE,
	    HAMMER2_INODE_BYTES, xop->head.mtid, 0, xop->flags);
	if (error == 0) {
		error = hammer2_chain_modify(chain, xop->head.mtid, 0, 0);
		if (error == 0) {
			chain->data->ipdata.meta = xop->meta;
			if (xop->head.name1) {
				bcopy(xop->head.name1,
				    chain->data->ipdata.filename,
				    xop->head.name1_len);
				chain->data->ipdata.meta.name_len =
				    xop->head.name1_len;
			}
			chain->data->ipdata.meta.name_key = xop->lhc;
		}
	}
fail:
	if (parent) {
		hammer2_chain_unlock(parent);
		hammer2_chain_drop(parent);
	}
	hammer2_xop_feed(&xop->head, chain, clindex, error);
	if (chain) {
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
	}
}

void
hammer2_xop_inode_create_det(hammer2_xop_t *arg, void *scratch, int clindex)
{
	hammer2_xop_create_t *xop = &arg->xop_create;
	hammer2_chain_t *parent, *chain, *null_parent;
	hammer2_inode_t *pip, *iroot;
	hammer2_key_t key_next;
	int error;

	pip = xop->head.ip1;
	iroot = pip->pmp->iroot;

	parent = hammer2_inode_chain(iroot, clindex, HAMMER2_RESOLVE_ALWAYS);
	if (parent == NULL) {
		error = HAMMER2_ERROR_EIO;
		chain = NULL;
		goto fail;
	}

	chain = hammer2_chain_lookup(&parent, &key_next, xop->lhc, xop->lhc,
	    &error, 0);
	if (chain) {
		error = HAMMER2_ERROR_EEXIST;
		goto fail;
	}

	null_parent = NULL;
	error = hammer2_chain_create(&null_parent, &chain, parent->hmp,
	    pip->pmp, HAMMER2_ENC_COMP(pip->meta.comp_algo) +
	    HAMMER2_ENC_CHECK(pip->meta.check_algo), xop->lhc, 0,
	    HAMMER2_BREF_TYPE_INODE, HAMMER2_INODE_BYTES,
	    xop->head.mtid, 0, xop->flags);
	if (error == 0) {
		error = hammer2_chain_modify(chain, xop->head.mtid, 0, 0);
		if (error == 0) {
			chain->data->ipdata.meta = xop->meta;
			if (xop->head.name1) {
				bcopy(xop->head.name1,
				    chain->data->ipdata.filename,
				    xop->head.name1_len);
				chain->data->ipdata.meta.name_len =
				    xop->head.name1_len;
			}
			chain->data->ipdata.meta.name_key = xop->lhc;
		}
	}
fail:
	if (parent) {
		hammer2_chain_unlock(parent);
		hammer2_chain_drop(parent);
	}
	hammer2_xop_feed(&xop->head, chain, clindex, error);
	if (chain) {
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
	}
}

void
hammer2_xop_inode_create_ins(hammer2_xop_t *arg, void *scratch, int clindex)
{
	hammer2_xop_create_t *xop = &arg->xop_create;
	hammer2_chain_t *parent, *chain;
	hammer2_key_t key_next;
	int error;

	parent = hammer2_inode_chain(xop->head.ip1->pmp->iroot, clindex,
	    HAMMER2_RESOLVE_ALWAYS);
	if (parent == NULL) {
		error = HAMMER2_ERROR_EIO;
		chain = NULL;
		goto fail;
	}

	chain = hammer2_chain_lookup(&parent, &key_next, xop->lhc, xop->lhc,
	    &error, 0);
	if (chain) {
		error = HAMMER2_ERROR_EEXIST;
		goto fail;
	}

	chain = hammer2_inode_chain(xop->head.ip1, clindex,
	    HAMMER2_RESOLVE_ALWAYS);
	if (chain == NULL) {
		error = HAMMER2_ERROR_EIO;
		chain = NULL;
		goto fail;
	}

	error = hammer2_chain_create(&parent, &chain, NULL, xop->head.ip1->pmp,
	    HAMMER2_METH_DEFAULT, xop->lhc, 0, HAMMER2_BREF_TYPE_INODE,
	    HAMMER2_INODE_BYTES, xop->head.mtid, 0, xop->flags);
fail:
	if (parent) {
		hammer2_chain_unlock(parent);
		hammer2_chain_drop(parent);
	}
	hammer2_xop_feed(&xop->head, chain, clindex, error);
	if (chain) {
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
	}
}

void
hammer2_xop_inode_destroy(hammer2_xop_t *arg, void *scratch, int clindex)
{
	hammer2_xop_destroy_t *xop = &arg->xop_destroy;
	hammer2_chain_t *parent, *chain;
	hammer2_inode_t *ip;
	int error;

	ip = xop->head.ip1;

	chain = hammer2_inode_chain(ip, clindex, HAMMER2_RESOLVE_ALWAYS);
	if (chain == NULL) {
		parent = NULL;
		error = HAMMER2_ERROR_EIO;
		goto done;
	}

	if (ip->flags & HAMMER2_INODE_CREATING) {

		parent = NULL;
	} else {

		parent = hammer2_chain_getparent(chain, HAMMER2_RESOLVE_ALWAYS);
		if (parent == NULL) {
			error = HAMMER2_ERROR_EIO;
			goto done;
		}
	}
	KKASSERT(chain->parent == parent);

	hammer2_chain_delete(parent, chain, xop->head.mtid, 0);
	error = 0;
done:
	hammer2_xop_feed(&xop->head, NULL, clindex, error);
	if (parent) {
		hammer2_chain_unlock(parent);
		hammer2_chain_drop(parent);
	}
	if (chain) {
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
	}
}

void
hammer2_xop_inode_unlinkall(hammer2_xop_t *arg, void *scratch, int clindex)
{
	hammer2_xop_unlinkall_t *xop = &arg->xop_unlinkall;
	hammer2_chain_t *parent, *chain;
	hammer2_key_t key_next;
	int error;

	parent = hammer2_inode_chain(xop->head.ip1, clindex,
	    HAMMER2_RESOLVE_ALWAYS);
	chain = NULL;
	if (parent == NULL) {
		error = 0;
		goto done;
	}
	chain = hammer2_chain_lookup(&parent, &key_next, xop->key_beg,
	    xop->key_end, &error, HAMMER2_LOOKUP_ALWAYS);
	while (chain) {
		hammer2_chain_delete(parent, chain, xop->head.mtid,
		    HAMMER2_DELETE_PERMANENT);
		hammer2_xop_feed(&xop->head, chain, clindex, chain->error);

		chain = hammer2_chain_next(&parent, chain, &key_next,
		    xop->key_end, &error, HAMMER2_LOOKUP_ALWAYS);
	}
done:
	if (error == 0)
		error = HAMMER2_ERROR_ENOENT;
	hammer2_xop_feed(&xop->head, NULL, clindex, error);
	if (parent) {
		hammer2_chain_unlock(parent);
		hammer2_chain_drop(parent);
	}
	if (chain) {
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
	}
}

void
hammer2_xop_inode_connect(hammer2_xop_t *arg, void *scratch, int clindex)
{
	hammer2_xop_connect_t *xop = &arg->xop_connect;
	hammer2_pfs_t *pmp;
	hammer2_chain_t *parent, *chain;
	hammer2_inode_data_t *wipdata;
	hammer2_key_t key_dummy;
	int error;

	pmp = xop->head.ip1->pmp;
	parent = hammer2_inode_chain(xop->head.ip1, clindex,
	    HAMMER2_RESOLVE_ALWAYS);
	if (parent == NULL) {
		chain = NULL;
		error = HAMMER2_ERROR_EIO;
		goto fail;
	}

	chain = hammer2_chain_lookup(&parent, &key_dummy, xop->lhc, xop->lhc,
	    &error, 0);
	if (chain) {
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
		chain = NULL;
		error = HAMMER2_ERROR_EEXIST;
		goto fail;
	}
	if (error)
		goto fail;

	chain = hammer2_inode_chain(xop->head.ip2, clindex,
	    HAMMER2_RESOLVE_ALWAYS);
	error = hammer2_chain_modify(chain, xop->head.mtid, 0, 0);
	if (error)
		goto fail;

	wipdata = &chain->data->ipdata;

	hammer2_inode_modify(xop->head.ip2);
	if (xop->head.name1) {
		bzero(wipdata->filename, sizeof(wipdata->filename));
		bcopy(xop->head.name1, wipdata->filename, xop->head.name1_len);
		wipdata->meta.name_len = xop->head.name1_len;
	}
	wipdata->meta.name_key = xop->lhc;

	error = hammer2_chain_create(&parent, &chain, NULL, pmp,
	    HAMMER2_METH_DEFAULT, xop->lhc, 0, HAMMER2_BREF_TYPE_INODE,
	    HAMMER2_INODE_BYTES, xop->head.mtid, 0, 0);
fail:
	hammer2_xop_feed(&xop->head, NULL, clindex, error);
	if (parent) {
		hammer2_chain_unlock(parent);
		hammer2_chain_drop(parent);
	}
	if (chain) {
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
	}
}

void
hammer2_xop_inode_chain_sync(hammer2_xop_t *arg, void *scratch, int clindex)
{
	hammer2_xop_fsync_t *xop = &arg->xop_fsync;
	hammer2_chain_t *parent, *chain = NULL;
	hammer2_key_t lbase, key_next;
	int error = 0;

	parent = hammer2_inode_chain(xop->head.ip1, clindex,
	    HAMMER2_RESOLVE_ALWAYS);
	if (parent == NULL) {
		error = HAMMER2_ERROR_EIO;
		goto done;
	}
	if (parent->error) {
		error = parent->error;
		goto done;
	}

	if ((xop->ipflags & HAMMER2_INODE_RESIZED) == 0) {

	} else if (xop->meta.size < xop->osize) {

		lbase = (xop->meta.size + HAMMER2_PBUFMASK64) &
		    ~HAMMER2_PBUFMASK64;
		chain = hammer2_chain_lookup(&parent, &key_next, lbase,
		    HAMMER2_KEY_MAX, &error,
		    HAMMER2_LOOKUP_NODATA | HAMMER2_LOOKUP_NODIRECT);
		while (chain) {

			switch (chain->bref.type) {
			case HAMMER2_BREF_TYPE_DIRENT:
			case HAMMER2_BREF_TYPE_INODE:
				KKASSERT(0);
				break;
			case HAMMER2_BREF_TYPE_DATA:
				hammer2_chain_delete(parent, chain,
				    xop->head.mtid, HAMMER2_DELETE_PERMANENT);
				break;
			}
			chain = hammer2_chain_next(&parent, chain, &key_next,
			    HAMMER2_KEY_MAX, &error,
			    HAMMER2_LOOKUP_NODATA | HAMMER2_LOOKUP_NODIRECT);
		}

		if (parent->bref.type != HAMMER2_BREF_TYPE_INODE) {
			hammer2_chain_unlock(parent);
			hammer2_chain_drop(parent);
			parent = hammer2_inode_chain(xop->head.ip1, clindex,
			    HAMMER2_RESOLVE_ALWAYS);
			hprintf("truncate reset on '%s'\n",
			    parent->data->ipdata.filename);
		}
	}

	if (error == 0) {
		error = hammer2_chain_modify(parent, xop->head.mtid, 0, 0);
		if (error == 0) {
			parent->data->ipdata.meta = xop->meta;
			if (xop->clear_directdata) {
				bzero(&parent->data->ipdata.u.blockset,
				    sizeof(parent->data->ipdata.u.blockset));
			} else if ((xop->ipflags & HAMMER2_INODE_RESIZED) &&
			    (xop->meta.op_flags & HAMMER2_OPFLAG_DIRECTDATA) &&
			    xop->meta.size < HAMMER2_EMBEDDED_BYTES) {

				bzero(parent->data->ipdata.u.data + xop->meta.size,
				    HAMMER2_EMBEDDED_BYTES - xop->meta.size);
			}
		}
	}
done:
	if (chain) {
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
	}
	if (parent) {
		hammer2_chain_unlock(parent);
		hammer2_chain_drop(parent);
	}
	hammer2_xop_feed(&xop->head, NULL, clindex, error);
}

void
hammer2_xop_bmap(hammer2_xop_t *arg, void *scratch, int clindex)
{
	hammer2_xop_bmap_t *xop = &arg->xop_bmap;
	hammer2_inode_t *ip = xop->head.ip1;
	hammer2_chain_t *chain, *parent;
	hammer2_key_t lbase, key_dummy;
	int error = 0;

	chain = NULL;
	parent = hammer2_inode_chain(ip, clindex,
	    HAMMER2_RESOLVE_ALWAYS | HAMMER2_RESOLVE_SHARED);
	if (parent == NULL) {
		hprintf("NULL parent\n");
		error = HAMMER2_ERROR_EIO;
		goto done;
	}

	xop->offset = HAMMER2_OFF_MASK;
	lbase = (hammer2_key_t)xop->lbn * hammer2_get_logical();
	chain = hammer2_chain_lookup(&parent, &key_dummy, lbase, lbase,
	    &error, HAMMER2_LOOKUP_SHARED);
	if (error == 0) {
		if (chain) {
			error = chain->error;
			if (error == 0)
				xop->offset = chain->bref.data_off;
		} else {
			error = HAMMER2_ERROR_ENOENT;
		}
	}
done:
	hammer2_xop_feed(&xop->head, chain, clindex, error);
	if (chain) {
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
	}
	if (parent) {
		hammer2_chain_unlock(parent);
		hammer2_chain_drop(parent);
	}
}
