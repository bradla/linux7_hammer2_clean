#include "dmsg_local.h"

static void master_auth_signal(dmsg_iocom_t *iocom);
static void master_auth_rxmsg(dmsg_msg_t *msg);
static void master_link_signal(dmsg_iocom_t *iocom);
static void master_link_rxmsg(dmsg_msg_t *msg);

void *
dmsg_master_service(void *data)
{
	dmsg_master_service_info_t *info = data;
	dmsg_iocom_t iocom;

	if (info->detachme)
		pthread_detach(pthread_self());

	dmsg_iocom_init(&iocom,
			info->fd,
			(info->altmsg_callback ? info->altfd : -1),
			master_auth_signal,
			master_auth_rxmsg,
			info->usrmsg_callback,
			info->altmsg_callback);
	if (info->noclosealt)
		iocom.flags &= ~DMSG_IOCOMF_CLOSEALT;
	if (info->label) {
		dmsg_iocom_label(&iocom, "%s", info->label);
		free(info->label);
		info->label = NULL;
	}
	dmsg_iocom_core(&iocom);
	dmsg_iocom_done(&iocom);

	dmio_printf(iocom, 1,
		    "iocom on fd %d terminated error rx=%d, tx=%d\n",
		    info->fd, iocom.ioq_rx.error, iocom.ioq_tx.error);
	close(info->fd);
	info->fd = -1;
	if (info->exit_callback)
		info->exit_callback(info->handle);
	free(info);

	return (NULL);
}

static void master_auth_conn_rx(dmsg_msg_t *msg);

static
void
master_auth_signal(dmsg_iocom_t *iocom)
{
	dmsg_msg_t *msg;

	msg = dmsg_msg_alloc(&iocom->state0, 0,
			     DMSG_LNK_CONN | DMSGF_CREATE,
			     master_auth_conn_rx, NULL);
	msg->any.lnk_conn.peer_mask = (uint64_t)-1;
	msg->any.lnk_conn.peer_type = DMSG_PEER_ROUTER;
	msg->any.lnk_conn.peer_mask = (uint64_t)-1;

	dmsg_msg_write(msg);

	dmsg_iocom_restate(iocom, master_link_signal, master_link_rxmsg);
}

static
void
master_auth_conn_rx(dmsg_msg_t *msg)
{
	if (msg->any.head.cmd & DMSGF_DELETE)
		dmsg_msg_reply(msg, 0);
}

static
void
master_auth_rxmsg(dmsg_msg_t *msg __unused)
{
}

static
void
master_link_signal(dmsg_iocom_t *iocom)
{
	dmsg_msg_lnk_signal(iocom);
}

static
void
master_link_rxmsg(dmsg_msg_t *msg)
{
	dmsg_state_t *state;
	dmsg_iocom_t *iocom;
	uint32_t cmd;

	state = msg->state;
	iocom = state->iocom;
	cmd = (state != &iocom->state0) ? state->icmd : msg->any.head.cmd;

	if (state->func) {

		assert(state != &iocom->state0);
		state->func(msg);
	} else {

		switch(cmd & DMSGF_PROTOS) {
		case DMSG_PROTO_LNK:
			dmsg_msg_lnk(msg);
			break;
		case DMSG_PROTO_DBG:
			dmsg_msg_dbg(msg);
			break;
		default:
			iocom->usrmsg_callback(msg, 1);
			break;
		}
	}
}

void
dmsg_msg_dbg(dmsg_msg_t *msg)
{
	dmsg_iocom_t *iocom = msg->state->iocom;

	switch(msg->tcmd & DMSGF_CMDSWMASK) {
	case DMSG_DBG_SHELL:

		if (msg->aux_data)
			msg->aux_data[msg->aux_size - 1] = 0;
		iocom->usrmsg_callback(msg, 0);
		break;
	case DMSG_DBG_SHELL | DMSGF_REPLY:

		iocom->usrmsg_callback(msg, 0);
		break;
	default:
		iocom->usrmsg_callback(msg, 1);
		break;
	}
}

void
dmsg_printf(dmsg_iocom_t *iocom, const char *ctl, ...)
{
	dmsg_msg_t *rmsg;
	va_list va;
	char buf[1024];
	size_t len;

	va_start(va, ctl);
	vsnprintf(buf, sizeof(buf), ctl, va);
	va_end(va);
	len = strlen(buf) + 1;

	rmsg = dmsg_msg_alloc(&iocom->state0, len,
			      DMSG_DBG_SHELL | DMSGF_REPLY,
			      NULL, NULL);
	bcopy(buf, rmsg->aux_data, len);

	dmsg_msg_write(rmsg);
}
