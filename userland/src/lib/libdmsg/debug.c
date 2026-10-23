#include "dmsg_local.h"

const char *
dmsg_basecmd_str(uint32_t cmd)
{
	static char buf[64];
	char protobuf[32];
	char cmdbuf[32];
	const char *protostr;
	const char *cmdstr;

	switch(cmd & DMSGF_PROTOS) {
	case DMSG_PROTO_LNK:
		protostr = "LNK_";
		break;
	case DMSG_PROTO_DBG:
		protostr = "DBG_";
		break;
	case DMSG_PROTO_HM2:
		protostr = "HM2_";
		break;
	case DMSG_PROTO_BLK:
		protostr = "BLK_";
		break;
	case DMSG_PROTO_VOP:
		protostr = "VOP_";
		break;
	default:
		snprintf(protobuf, sizeof(protobuf), "%x_",
			(cmd & DMSGF_PROTOS) >> 20);
		protostr = protobuf;
		break;
	}

	switch(cmd & (DMSGF_PROTOS |
		      DMSGF_CMDS |
		      DMSGF_SIZE)) {
	case DMSG_LNK_PAD:
		cmdstr = "PAD";
		break;
	case DMSG_LNK_PING:
		cmdstr = "PING";
		break;
	case DMSG_LNK_AUTH:
		cmdstr = "AUTH";
		break;
	case DMSG_LNK_CONN:
		cmdstr = "CONN";
		break;
	case DMSG_LNK_SPAN:
		cmdstr = "SPAN";
		break;
	case DMSG_LNK_ERROR:
		if (cmd & DMSGF_DELETE)
			cmdstr = "RETURN";
		else
			cmdstr = "RESULT";
		break;
	case DMSG_DBG_SHELL:
		cmdstr = "SHELL";
		break;
	default:
		snprintf(cmdbuf, sizeof(cmdbuf),
			 "%06x", (cmd & (DMSGF_PROTOS |
					 DMSGF_CMDS |
					 DMSGF_SIZE)));
		cmdstr = cmdbuf;
		break;
	}
	snprintf(buf, sizeof(buf), "%s%s", protostr, cmdstr);
	return (buf);
}

const char *
dmsg_msg_str(dmsg_msg_t *msg)
{
	dmsg_state_t *state;
	static char buf[256];
	char errbuf[16];
	char statebuf[64];
	char flagbuf[64];
	const char *statestr;
	const char *errstr;
	uint32_t basecmd;
	int i;

	if ((state = msg->state) != NULL) {
		basecmd = (state->rxcmd & DMSGF_REPLY) ?
			  state->txcmd : state->rxcmd;
		snprintf(statebuf, sizeof(statebuf),
			 " %s=%s,L=%s%s,R=%s%s",
			 ((state->txcmd & DMSGF_REPLY) ?
				"rcvcmd" : "sndcmd"),
			 dmsg_basecmd_str(basecmd),
			 ((state->txcmd & DMSGF_CREATE) ? "C" : ""),
			 ((state->txcmd & DMSGF_DELETE) ? "D" : ""),
			 ((state->rxcmd & DMSGF_CREATE) ? "C" : ""),
			 ((state->rxcmd & DMSGF_DELETE) ? "D" : "")
		);
		statestr = statebuf;
	} else {
		statestr = "";
	}

	switch(msg->any.head.error) {
	case 0:
		errstr = "";
		break;
	case DMSG_IOQ_ERROR_SYNC:
		errstr = "err=IOQ:NOSYNC";
		break;
	case DMSG_IOQ_ERROR_EOF:
		errstr = "err=IOQ:STREAMEOF";
		break;
	case DMSG_IOQ_ERROR_SOCK:
		errstr = "err=IOQ:SOCKERR";
		break;
	case DMSG_IOQ_ERROR_FIELD:
		errstr = "err=IOQ:BADFIELD";
		break;
	case DMSG_IOQ_ERROR_HCRC:
		errstr = "err=IOQ:BADHCRC";
		break;
	case DMSG_IOQ_ERROR_XCRC:
		errstr = "err=IOQ:BADXCRC";
		break;
	case DMSG_IOQ_ERROR_ACRC:
		errstr = "err=IOQ:BADACRC";
		break;
	case DMSG_IOQ_ERROR_STATE:
		errstr = "err=IOQ:BADSTATE";
		break;
	case DMSG_IOQ_ERROR_NOPEER:
		errstr = "err=IOQ:PEERCONFIG";
		break;
	case DMSG_IOQ_ERROR_NORKEY:
		errstr = "err=IOQ:BADRKEY";
		break;
	case DMSG_IOQ_ERROR_NOLKEY:
		errstr = "err=IOQ:BADLKEY";
		break;
	case DMSG_IOQ_ERROR_KEYXCHGFAIL:
		errstr = "err=IOQ:BADKEYXCHG";
		break;
	case DMSG_IOQ_ERROR_KEYFMT:
		errstr = "err=IOQ:BADFMT";
		break;
	case DMSG_IOQ_ERROR_BADURANDOM:
		errstr = "err=IOQ:BADRANDOM";
		break;
	case DMSG_IOQ_ERROR_MSGSEQ:
		errstr = "err=IOQ:BADSEQ";
		break;
	case DMSG_IOQ_ERROR_EALREADY:
		errstr = "err=IOQ:DUPMSG";
		break;
	case DMSG_IOQ_ERROR_TRANS:
		errstr = "err=IOQ:BADTRANS";
		break;
	case DMSG_IOQ_ERROR_IVWRAP:
		errstr = "err=IOQ:IVWRAP";
		break;
	case DMSG_IOQ_ERROR_MACFAIL:
		errstr = "err=IOQ:MACFAIL";
		break;
	case DMSG_IOQ_ERROR_ALGO:
		errstr = "err=IOQ:ALGOFAIL";
		break;
	case DMSG_ERR_NOSUPP:
		errstr = "err=NOSUPPORT";
		break;
	default:
		snprintf(errbuf, sizeof(errbuf),
			 " err=%d", msg->any.head.error);
		errstr = errbuf;
		break;
	}

	i = 0;
	if (msg->any.head.cmd & (DMSGF_CREATE | DMSGF_DELETE |
				 DMSGF_ABORT | DMSGF_REPLY)) {
		flagbuf[i++] = '|';
		if (msg->any.head.cmd & DMSGF_CREATE)
			flagbuf[i++] = 'C';
		if (msg->any.head.cmd & DMSGF_DELETE)
			flagbuf[i++] = 'D';
		if (msg->any.head.cmd & DMSGF_REPLY)
			flagbuf[i++] = 'R';
		if (msg->any.head.cmd & DMSGF_ABORT)
			flagbuf[i++] = 'A';
	}
	flagbuf[i] = 0;

	snprintf(buf, sizeof(buf),
		"msg=%s%s %s %s hcrc=%08x id=%016jx",
		 dmsg_basecmd_str(msg->any.head.cmd),
		 flagbuf,
		 errstr,
		 statestr,
		 msg->any.head.hdr_crc,
		 msg->any.head.msgid);

	return(buf);
}
