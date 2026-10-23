#include "hammer2.h"

int
hammer2_msg_adhoc_input(kdmsg_msg_t *msg)
{
	kprintf("ADHOC INPUT MSG %08x\n", msg->any.head.cmd);
	return(0);
}

int
hammer2_msg_dbg_rcvmsg(kdmsg_msg_t *msg)
{
	switch(msg->any.head.cmd & DMSGF_CMDSWMASK) {
	case DMSG_DBG_SHELL:

		kdmsg_msg_reply(msg, DMSG_ERR_NOSUPP);
		break;
	case DMSG_DBG_SHELL | DMSGF_REPLY:

		if (msg->aux_data) {
			msg->aux_data[msg->aux_size - 1] = 0;
			kprintf("DEBUGMSG: %s\n", msg->aux_data);
		}
		break;
	default:

		kdmsg_msg_reply(msg, DMSG_ERR_NOSUPP);
		break;
	}
	return(0);
}
