#include "hammer2.h"

static int hammer2_rcvdmsg(kdmsg_msg_t *msg);
static void hammer2_autodmsg(kdmsg_msg_t *msg);
static int hammer2_lnk_span_reply(kdmsg_state_t *state, kdmsg_msg_t *msg);

void
hammer2_iocom_init(hammer2_dev_t *hmp)
{

	kdmsg_iocom_init(&hmp->iocom, hmp,
			 KDMSG_IOCOMF_AUTOCONN |
			 KDMSG_IOCOMF_AUTORXSPAN,
			 hmp->mmsg, hammer2_rcvdmsg);
}

void
hammer2_iocom_uninit(hammer2_dev_t *hmp)
{

	if (hmp->iocom.mmsg)
		kdmsg_iocom_uninit(&hmp->iocom);
}

void
hammer2_cluster_reconnect(hammer2_dev_t *hmp, struct file *fp)
{

	kdmsg_iocom_reconnect(&hmp->iocom, fp, "hammer2");

	bzero(&hmp->iocom.auto_lnk_conn.peer_id,
	      sizeof(hmp->iocom.auto_lnk_conn.peer_id));

	hmp->iocom.auto_lnk_conn.proto_version = DMSG_SPAN_PROTO_1;
#if 0
	hmp->iocom.auto_lnk_conn.peer_type = hmp->voldata.peer_type;
#endif
	hmp->iocom.auto_lnk_conn.peer_type = DMSG_PEER_HAMMER2;

	hmp->iocom.auto_lnk_conn.peer_mask = 1LLU << DMSG_PEER_HAMMER2;

#if 0
	switch (ipdata->meta.pfs_type) {
	case DMSG_PFSTYPE_CLIENT:
		hmp->iocom.auto_lnk_conn.peer_mask &=
				~(1LLU << DMSG_PFSTYPE_CLIENT);
		break;
	default:
		break;
	}
#endif

	bzero(&hmp->iocom.auto_lnk_conn.peer_label,
	      sizeof(hmp->iocom.auto_lnk_conn.peer_label));
	ksnprintf(hmp->iocom.auto_lnk_conn.peer_label,
		  sizeof(hmp->iocom.auto_lnk_conn.peer_label),
		  "%s/%s",
		  hostname, "hammer2-mount");
	kdmsg_iocom_autoinitiate(&hmp->iocom, hammer2_autodmsg);
}

static int
hammer2_rcvdmsg(kdmsg_msg_t *msg)
{
	kprintf("RCVMSG %08x\n", msg->tcmd);

	switch(msg->tcmd) {
	case DMSG_DBG_SHELL:

		kdmsg_msg_result(msg, DMSG_ERR_NOSUPP);
		break;
	case DMSG_DBG_SHELL | DMSGF_REPLY:

		if (msg->aux_data) {
			msg->aux_data[msg->aux_size - 1] = 0;
			kprintf("HAMMER2 DBG: %s\n", msg->aux_data);
		}
		break;
	default:

		if (msg->any.head.cmd & DMSGF_CREATE)
			kdmsg_msg_reply(msg, DMSG_ERR_NOSUPP);
		break;
	}
	return(0);
}

static void hammer2_update_spans(hammer2_dev_t *hmp, kdmsg_state_t *state);

static void
hammer2_autodmsg(kdmsg_msg_t *msg)
{
	hammer2_dev_t *hmp = msg->state->iocom->handle;
	int copyid;

	switch(msg->tcmd) {
	case DMSG_LNK_CONN | DMSGF_CREATE:
	case DMSG_LNK_CONN | DMSGF_CREATE | DMSGF_DELETE:
	case DMSG_LNK_CONN | DMSGF_DELETE:

		break;
	case DMSG_LNK_CONN | DMSGF_CREATE | DMSGF_REPLY:
	case DMSG_LNK_CONN | DMSGF_CREATE | DMSGF_DELETE | DMSGF_REPLY:

		if (msg->any.head.cmd & DMSGF_CREATE) {

			(void)copyid;
			kprintf("HAMMER2: INITIATE SPANs\n");
			hammer2_update_spans(hmp, msg->state);
		}
		if ((msg->any.head.cmd & DMSGF_DELETE) &&
		    msg->state && (msg->state->txcmd & DMSGF_DELETE) == 0) {
			kprintf("HAMMER2: CONN WAS TERMINATED\n");
		}
		break;
	case DMSG_LNK_SPAN | DMSGF_CREATE:

		if (msg->any.lnk_span.peer_type != DMSG_PEER_HAMMER2) {
			kdmsg_msg_reply(msg, 0);
			break;
		}
		if (msg->any.lnk_span.proto_version != DMSG_SPAN_PROTO_1) {
			kdmsg_msg_reply(msg, 0);
			break;
		}
		DMSG_TERMINATE_STRING(msg->any.lnk_span.peer_label);
		if (hammer2_debug & 0x0100) {
			kprintf("H2 +RXSPAN cmd=%08x (%-20s) cl=",
				msg->any.head.cmd,
				msg->any.lnk_span.peer_label);
			printf_uuid(&msg->any.lnk_span.peer_id);
			kprintf(" fs=");
			printf_uuid(&msg->any.lnk_span.pfs_id);
			kprintf(" type=%d\n", msg->any.lnk_span.pfs_type);
		}
		kdmsg_msg_result(msg, 0);
		break;
	case DMSG_LNK_SPAN | DMSGF_DELETE:

		if (hammer2_debug & 0x0100)
			kprintf("H2 -RXSPAN\n");
		break;
	default:
		break;
	}
}

static void
hammer2_update_spans(hammer2_dev_t *hmp, kdmsg_state_t *state)
{
	const hammer2_inode_data_t *ripdata;
	hammer2_chain_t *parent;
	hammer2_chain_t *chain;
	hammer2_pfs_t *spmp;
	hammer2_key_t key_next;
	kdmsg_msg_t *rmsg;
	size_t name_len;
	int error;

	spmp = hmp->spmp;
	hammer2_inode_lock(spmp->iroot, 0);
	error = 0;

	parent = hammer2_inode_chain(spmp->iroot, 0, HAMMER2_RESOLVE_ALWAYS);
	chain = NULL;
	if (parent == NULL)
		goto done;
	chain = hammer2_chain_lookup(&parent, &key_next,
				     HAMMER2_KEY_MIN, HAMMER2_KEY_MAX,
				     &error, 0);
	while (chain) {
		if (chain->bref.type != HAMMER2_BREF_TYPE_INODE)
			continue;
		ripdata = &chain->data->ipdata;
#if 0
		kprintf("UPDATE SPANS: %s\n", ripdata->filename);
#endif

		rmsg = kdmsg_msg_alloc(&hmp->iocom.state0,
				       DMSG_LNK_SPAN | DMSGF_CREATE,
				       hammer2_lnk_span_reply, NULL);

		memcpy(&rmsg->any.lnk_span.peer_id, &ripdata->meta.pfs_clid,
		       sizeof(rmsg->any.lnk_span.peer_id));
		memcpy(&rmsg->any.lnk_span.pfs_id, &ripdata->meta.pfs_fsid,
		       sizeof(rmsg->any.lnk_span.pfs_id));
		rmsg->any.lnk_span.pfs_type = ripdata->meta.pfs_type;
		rmsg->any.lnk_span.peer_type = DMSG_PEER_HAMMER2;
		rmsg->any.lnk_span.proto_version = DMSG_SPAN_PROTO_1;
		name_len = ripdata->meta.name_len;
		if (name_len >= sizeof(rmsg->any.lnk_span.peer_label))
			name_len = sizeof(rmsg->any.lnk_span.peer_label) - 1;
		bcopy(ripdata->filename,
		      rmsg->any.lnk_span.peer_label,
		      name_len);

		kdmsg_msg_write(rmsg);

		chain = hammer2_chain_next(&parent, chain, &key_next,
					       HAMMER2_KEY_MAX,
					       &error, 0);
	}
	hammer2_inode_unlock(spmp->iroot);

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

static
int
hammer2_lnk_span_reply(kdmsg_state_t *state, kdmsg_msg_t *msg)
{
	if ((state->txcmd & DMSGF_DELETE) == 0 &&
	    (msg->any.head.cmd & DMSGF_DELETE)) {
		kdmsg_msg_reply(msg, 0);
	}
	return 0;
}

void
hammer2_volconf_update(hammer2_dev_t *hmp, int index)
{
	(void)hmp;
	(void)index;
}
