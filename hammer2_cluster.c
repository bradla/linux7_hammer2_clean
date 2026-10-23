#include "hammer2.h"

uint8_t
hammer2_cluster_type(const hammer2_cluster_t *cluster)
{
	if (cluster->error == 0) {
		KKASSERT(cluster->focus != NULL);
		return (cluster->focus->bref.type);
	}

	return (0);
}

void
hammer2_cluster_bref(const hammer2_cluster_t *cluster, hammer2_blockref_t *bref)
{
	if (cluster->error == 0) {
		KKASSERT(cluster->focus != NULL);
		*bref = cluster->focus->bref;
		bref->data_off = 0;
	} else {
		bzero(bref, sizeof(*bref));
	}
}

void
hammer2_dummy_xop_from_chain(hammer2_xop_head_t *xop, hammer2_chain_t *chain)
{
	hammer2_cluster_t *cluster = &xop->cluster;

	bzero(xop, sizeof(*xop));

	cluster->array[0].chain = chain;
	cluster->nchains = 1;
	cluster->focus = chain;
	cluster->pmp = chain->pmp;
	cluster->error = chain->error;

	hammer2_assert_cluster(cluster);
}

void
hammer2_cluster_unhold(hammer2_cluster_t *cluster)
{
	hammer2_chain_t *chain;
	int i;

	for (i = 0; i < cluster->nchains; ++i) {
		chain = cluster->array[i].chain;
		if (chain)
			hammer2_chain_unhold(chain);
	}
}

void
hammer2_cluster_rehold(hammer2_cluster_t *cluster)
{
	hammer2_chain_t *chain;
	int i;

	for (i = 0; i < cluster->nchains; ++i) {
		chain = cluster->array[i].chain;
		if (chain)
			hammer2_chain_rehold(chain);
	}
}

int
hammer2_cluster_check(hammer2_cluster_t *cluster, hammer2_key_t key, int flags)
{
	hammer2_pfs_t *pmp;
	hammer2_chain_t *chain;
	int i, error;

	cluster->focus = NULL;
	cluster->error = 0;
	hammer2_assert_cluster(cluster);

	pmp = cluster->pmp;
	KKASSERT(pmp != NULL);

	for (i = 0; i < cluster->nchains; ++i) {
		chain = cluster->array[i].chain;
		error = cluster->array[i].error;

		switch (pmp->pfs_types[i]) {
		case HAMMER2_PFSTYPE_MASTER:
		case HAMMER2_PFSTYPE_SUPROOT:
			cluster->focus = chain;
			cluster->error = error;
			break;
		case HAMMER2_PFSTYPE_NONE:
			break;
		default:
			hpanic("invalid PFS type %d", pmp->pfs_types[i]);
			break;
		}
	}

	if (flags & HAMMER2_CHECK_NULL) {
		if (cluster->error == 0)
			cluster->error = HAMMER2_ERROR_ENOENT;
		return (cluster->error);
	}

	if (cluster->focus == NULL)
		return (HAMMER2_ERROR_EIO);

	for (i = 0; i < cluster->nchains; ++i) {
		chain = cluster->array[i].chain;
		if (i == 0) {
			KKASSERT(chain != NULL);
			KKASSERT(chain == cluster->focus);
			KKASSERT(chain->bref.key == key);
		} else {
			KKASSERT(chain == NULL);
		}
	}

	return (cluster->error);
}
