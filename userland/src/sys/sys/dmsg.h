#ifndef _SYS_DMSG_H_
#define _SYS_DMSG_H_

#include <stdint.h>
#include <sys/tree.h>
#include <uuid/uuid.h>

#define DMSG_TERMINATE_STRING(ary)	\
	do { (ary)[sizeof(ary) - 1] = 0; } while (0)

struct dmsg_hdr {
	uint16_t	magic;
	uint16_t	reserved02;
	uint32_t	salt;

	uint64_t	msgid;
	uint64_t	circuit;
	uint64_t	link_verifier;

	uint32_t	cmd;
	uint32_t	aux_crc;
	uint32_t	aux_bytes;
	uint32_t	error;
	uint64_t	aux_descr;
	uint32_t	reserved38;
	uint32_t	hdr_crc;
};

typedef struct dmsg_hdr dmsg_hdr_t;

#define DMSG_HDR_MAGIC		0x4832
#define DMSG_HDR_MAGIC_REV	0x3248
#define DMSG_HDR_CRCOFF		offsetof(dmsg_hdr_t, salt)
#define DMSG_HDR_CRCBYTES	(sizeof(dmsg_hdr_t) - DMSG_HDR_CRCOFF)

#define DMSG_HDR_MAX		2048
#define DMSG_AUX_MAX		(1024*1024)
#define DMSG_BUF_SIZE		(DMSG_HDR_MAX * 4)
#define DMSG_BUF_MASK		(DMSG_BUF_SIZE - 1)

#define DMSGF_CREATE		0x80000000U
#define DMSGF_DELETE		0x40000000U
#define DMSGF_REPLY		0x20000000U
#define DMSGF_ABORT		0x10000000U
#define DMSGF_REVTRANS		0x08000000U
#define DMSGF_REVCIRC		0x04000000U
#define DMSGF_FLAG1		0x02000000U
#define DMSGF_FLAG0		0x01000000U

#define DMSGF_FLAGS		0xFF000000U
#define DMSGF_PROTOS		0x00F00000U
#define DMSGF_CMDS		0x000FFF00U
#define DMSGF_SIZE		0x000000FFU

#define DMSGF_CMDF_EXPECT_ACK	0x00080000U

#define DMSGF_CMDSWMASK		(DMSGF_CMDS |	\
					 DMSGF_SIZE |	\
					 DMSGF_PROTOS |	\
					 DMSGF_REPLY)

#define DMSGF_BASECMDMASK	(DMSGF_CMDS |	\
					 DMSGF_SIZE |	\
					 DMSGF_PROTOS)

#define DMSGF_TRANSMASK		(DMSGF_CMDS |	\
					 DMSGF_SIZE |	\
					 DMSGF_PROTOS |	\
					 DMSGF_REPLY |	\
					 DMSGF_CREATE |	\
					 DMSGF_DELETE)

#define DMSGF_BASEFLAGS		(DMSGF_CREATE | DMSGF_DELETE | DMSGF_REPLY)

#define DMSG_PROTO_LNK		0x00000000U
#define DMSG_PROTO_DBG		0x00100000U
#define DMSG_PROTO_HM2		0x00200000U
#define DMSG_PROTO_XX3		0x00300000U
#define DMSG_PROTO_XX4		0x00400000U
#define DMSG_PROTO_BLK		0x00500000U
#define DMSG_PROTO_VOP		0x00600000U

#define DMSG_ALIGN		64
#define DMSG_ALIGNMASK		(DMSG_ALIGN - 1)
#define DMSG_DOALIGN(bytes)	(((bytes) + DMSG_ALIGNMASK) &		\
				 ~DMSG_ALIGNMASK)

#define DMSG_HDR_ENCODE(elm)	(((uint32_t)sizeof(struct elm) +	\
				  DMSG_ALIGNMASK) /			\
				 DMSG_ALIGN)

#define DMSG_LNK(cmd, elm)	(DMSG_PROTO_LNK |			\
					 ((cmd) << 8) | 		\
					 DMSG_HDR_ENCODE(elm))

#define DMSG_DBG(cmd, elm)	(DMSG_PROTO_DBG |			\
					 ((cmd) << 8) | 		\
					 DMSG_HDR_ENCODE(elm))

#define DMSG_HM2(cmd, elm)	(DMSG_PROTO_HM2 |			\
					 ((cmd) << 8) | 		\
					 DMSG_HDR_ENCODE(elm))

#define DMSG_BLK(cmd, elm)	(DMSG_PROTO_BLK |			\
					 ((cmd) << 8) | 		\
					 DMSG_HDR_ENCODE(elm))

#define DMSG_VOP(cmd, elm)	(DMSG_PROTO_VOP |			\
					 ((cmd) << 8) | 		\
					 DMSG_HDR_ENCODE(elm))

#define DMSG_LNK_PAD		DMSG_LNK(0x000, dmsg_hdr)
#define DMSG_LNK_PING		DMSG_LNK(0x001, dmsg_hdr)
#define DMSG_LNK_AUTH		DMSG_LNK(0x010, dmsg_lnk_auth)
#define DMSG_LNK_CONN		DMSG_LNK(0x011, dmsg_lnk_conn)
#define DMSG_LNK_SPAN		DMSG_LNK(0x012, dmsg_lnk_span)
#define DMSG_LNK_ERROR		DMSG_LNK(0xFFF, dmsg_hdr)

#define DMSG_LNK_CMD_HAMMER2_VOLCONF	0x20

#define DMSG_LABEL_SIZE		128

struct dmsg_lnk_auth {
	dmsg_hdr_t	head;
	char		dummy[64];
};

typedef struct dmsg_uuid {
	uuid_t uuid;
} dmsg_uuid_t;

struct dmsg_lnk_conn {
	dmsg_hdr_t	head;
	dmsg_uuid_t	media_id;
	dmsg_uuid_t	peer_id;
	dmsg_uuid_t	reserved01;
	uint64_t	peer_mask;
	uint8_t		peer_type;
	uint8_t		reserved02;
	uint16_t	proto_version;
	uint32_t	status;
	uint32_t	rnss;
	uint8_t		reserved03[8];
	uint32_t	reserved04[14];
	char		peer_label[DMSG_LABEL_SIZE];
};

typedef struct dmsg_lnk_conn dmsg_lnk_conn_t;

#define DMSG_PEER_NONE			0
#define DMSG_PEER_ROUTER		1
#define DMSG_PEER_BLOCK			2
#define DMSG_PEER_HAMMER2		3
#define DMSG_PEER_CLIENT		63
#define DMSG_PEER_MAX			64

#define DMSG_PFSTYPE_DEFAULT		0
#define DMSG_PFSTYPE_MASK		0x0F

struct dmsg_media_block {
	uint64_t	bytes;
	uint32_t	blksize;
	uint32_t	reserved01;
};

typedef struct dmsg_media_block dmsg_media_block_t;

struct dmsg_lnk_span {
	dmsg_hdr_t	head;
	dmsg_uuid_t	peer_id;
	dmsg_uuid_t	pfs_id;
	uint8_t		pfs_type;
	uint8_t		peer_type;
	uint16_t	proto_version;
	uint32_t	status;
	uint8_t		reserved02[8];
	uint32_t	dist;
	uint32_t	rnss;
	union {
		uint32_t	reserved03[14];
		dmsg_media_block_t block;
	} media;

	char		peer_label[DMSG_LABEL_SIZE];
	char		pfs_label[DMSG_LABEL_SIZE];
};

typedef struct dmsg_lnk_span dmsg_lnk_span_t;

#define DMSG_SPAN_PROTO_1	1

#define DMSG_DBG_SHELL		DMSG_DBG(0x001, dmsg_dbg_shell)

struct dmsg_dbg_shell {
	dmsg_hdr_t	head;
};
typedef struct dmsg_dbg_shell dmsg_dbg_shell_t;

#define DMSG_HM2_OPENPFS	DMSG_HM2(0x001, dmsg_hm2_openpfs)

#define DMSG_BLK_OPEN		DMSG_BLK(0x001, dmsg_blk_open)
#define DMSG_BLK_CLOSE		DMSG_BLK(0x002, dmsg_blk_open)
#define DMSG_BLK_READ		DMSG_BLK(0x003, dmsg_blk_read)
#define DMSG_BLK_WRITE		DMSG_BLK(0x004, dmsg_blk_write)
#define DMSG_BLK_FLUSH		DMSG_BLK(0x005, dmsg_blk_flush)
#define DMSG_BLK_FREEBLKS	DMSG_BLK(0x006, dmsg_blk_freeblks)
#define DMSG_BLK_ERROR		DMSG_BLK(0xFFF, dmsg_blk_error)

struct dmsg_blk_open {
	dmsg_hdr_t	head;
	uint32_t	modes;
	uint32_t	reserved01;
};

#define DMSG_BLKOPEN_RD		0x0001
#define DMSG_BLKOPEN_WR		0x0002

struct dmsg_blk_error {
	dmsg_hdr_t	head;
	uint64_t	keyid;
	uint32_t	resid;
	uint32_t	reserved02;
	char		buf[64];
};

struct dmsg_blk_read {
	dmsg_hdr_t	head;
	uint64_t	keyid;
	uint64_t	offset;
	uint32_t	bytes;
	uint32_t	flags;
	uint32_t	reserved01;
	uint32_t	reserved02;
};

struct dmsg_blk_write {
	dmsg_hdr_t	head;
	uint64_t	keyid;
	uint64_t	offset;
	uint32_t	bytes;
	uint32_t	flags;
	uint32_t	reserved01;
	uint32_t	reserved02;
};

struct dmsg_blk_flush {
	dmsg_hdr_t	head;
	uint64_t	keyid;
	uint64_t	offset;
	uint32_t	bytes;
	uint32_t	flags;
	uint32_t	reserved01;
	uint32_t	reserved02;
};

struct dmsg_blk_freeblks {
	dmsg_hdr_t	head;
	uint64_t	keyid;
	uint64_t	offset;
	uint32_t	bytes;
	uint32_t	flags;
	uint32_t	reserved01;
	uint32_t	reserved02;
};

typedef struct dmsg_blk_open		dmsg_blk_open_t;
typedef struct dmsg_blk_read		dmsg_blk_read_t;
typedef struct dmsg_blk_write		dmsg_blk_write_t;
typedef struct dmsg_blk_flush		dmsg_blk_flush_t;
typedef struct dmsg_blk_freeblks	dmsg_blk_freeblks_t;
typedef struct dmsg_blk_error		dmsg_blk_error_t;

#define DMSG_ERR_NOSUPP		0x20
#define DMSG_ERR_LOSTLINK	0x21
#define DMSG_ERR_IO		0x22
#define DMSG_ERR_PARAM		0x23
#define DMSG_ERR_CANTCIRC	0x24

union dmsg_any {
	char			buf[DMSG_HDR_MAX];
	dmsg_hdr_t		head;

	dmsg_lnk_conn_t		lnk_conn;
	dmsg_lnk_span_t		lnk_span;

	dmsg_blk_open_t		blk_open;
	dmsg_blk_error_t	blk_error;
	dmsg_blk_read_t		blk_read;
	dmsg_blk_write_t	blk_write;
	dmsg_blk_flush_t	blk_flush;
	dmsg_blk_freeblks_t	blk_freeblks;
};

typedef union dmsg_any dmsg_any_t;

#if defined(_KERNEL) || defined(_KERNEL_STRUCTURES)

struct hammer2_mount;
struct xa_softc;
struct kdmsg_iocom;
struct kdmsg_state;
struct kdmsg_msg;
struct kdmsg_data;

#define KDMSG_CLUSTERCTL_UNUSED01	0x00000001
#define KDMSG_CLUSTERCTL_KILLRX		0x00000002
#define KDMSG_CLUSTERCTL_KILLTX		0x00000004
#define KDMSG_CLUSTERCTL_SLEEPING	0x00000008

TAILQ_HEAD(kdmsg_state_list, kdmsg_state);

struct kdmsg_state {
	RB_ENTRY(kdmsg_state) rbnode;
	struct kdmsg_state	*scan;
	struct kdmsg_state_list	subq;
	TAILQ_ENTRY(kdmsg_state) entry;
	TAILQ_ENTRY(kdmsg_state) user_entry;
	struct kdmsg_iocom *iocom;
	struct kdmsg_state *parent;
	int		refs;
	uint32_t	icmd;
	uint32_t	txcmd;
	uint32_t	rxcmd;
	uint64_t	msgid;
	int		flags;
	int		error;
	void		*chain;
	int (*func)(struct kdmsg_state *, struct kdmsg_msg *);
	union {
		void *any;
		struct hammer2_mount *hmp;
		struct xa_softc *xa_sc;
	} any;
};

#define KDMSG_STATE_SUBINSERTED	0x0001
#define KDMSG_STATE_DYNAMIC	0x0002
#define KDMSG_STATE_UNUSED0004	0x0004
#define KDMSG_STATE_ABORTING	0x0008
#define KDMSG_STATE_OPPOSITE	0x0010
#define KDMSG_STATE_DYING	0x0020
#define KDMSG_STATE_INTERLOCK	0x0040
#define KDMSG_STATE_RBINSERTED	0x0080
#define KDMSG_STATE_SIGNAL	0x0400
#define KDMSG_STATE_NEW		0x0800

struct kdmsg_msg {
	TAILQ_ENTRY(kdmsg_msg) qentry;
	struct kdmsg_state *state;
	size_t		hdr_size;
	size_t		aux_size;
	char		*aux_data;
	uint32_t	flags;
	uint32_t	tcmd;
	dmsg_any_t	any;
};

struct kdmsg_data {
	char		*aux_data;
	size_t		aux_size;
	struct kdmsg_iocom *iocom;
};

#define KDMSG_FLAG_AUXALLOC	0x0001

typedef struct kdmsg_link kdmsg_link_t;
typedef struct kdmsg_state kdmsg_state_t;
typedef struct kdmsg_msg kdmsg_msg_t;
typedef struct kdmsg_data kdmsg_data_t;

struct kdmsg_state_tree;
int kdmsg_state_cmp(kdmsg_state_t *state1, kdmsg_state_t *state2);
RB_HEAD(kdmsg_state_tree, kdmsg_state);
RB_PROTOTYPE(kdmsg_state_tree, kdmsg_state, rbnode, kdmsg_state_cmp);

struct file;
struct malloc_type;

struct kdmsg_iocom {
	struct malloc_type	*mmsg;
	struct file		*msg_fp;
	thread_t		msgrd_td;
	thread_t		msgwr_td;
	int			msg_ctl;
	int			msg_seq;
	uint32_t		flags;
	struct lock		msglk;
	TAILQ_HEAD(, kdmsg_msg) msgq;
	void			*handle;
	void			(*auto_callback)(kdmsg_msg_t *);
	int			(*rcvmsg)(kdmsg_msg_t *);
	void			(*exit_func)(struct kdmsg_iocom *);
	struct kdmsg_state	state0;
	struct kdmsg_state	*conn_state;
	struct kdmsg_state	*freerd_state;
	struct kdmsg_state	*freewr_state;
	struct kdmsg_state_tree staterd_tree;
	struct kdmsg_state_tree statewr_tree;
	dmsg_lnk_conn_t		auto_lnk_conn;
	dmsg_lnk_span_t		auto_lnk_span;
};

typedef struct kdmsg_iocom	kdmsg_iocom_t;

#define KDMSG_IOCOMF_AUTOCONN	0x0001
#define KDMSG_IOCOMF_AUTORXSPAN	0x0002
#define KDMSG_IOCOMF_AUTOTXSPAN	0x0008
#define KDMSG_IOCOMF_EXITNOACC	0x8000

#define KDMSG_IOCOMF_AUTOANY	(KDMSG_IOCOMF_AUTOCONN |	\
				 KDMSG_IOCOMF_AUTORXSPAN |	\
				 KDMSG_IOCOMF_AUTOTXSPAN)

#endif

#ifdef _KERNEL

void kdmsg_iocom_init(kdmsg_iocom_t *iocom, void *handle, u_int32_t flags,
			struct malloc_type *mmsg,
			int (*rcvmsg)(kdmsg_msg_t *msg));
void kdmsg_iocom_reconnect(kdmsg_iocom_t *iocom, struct file *fp,
			const char *subsysname);
void kdmsg_iocom_autoinitiate(kdmsg_iocom_t *iocom,
			void (*conn_callback)(kdmsg_msg_t *msg));
void kdmsg_iocom_uninit(kdmsg_iocom_t *iocom);
void kdmsg_drain_msgq(kdmsg_iocom_t *iocom);

void kdmsg_msg_free(kdmsg_msg_t *msg);
kdmsg_msg_t *kdmsg_msg_alloc(kdmsg_state_t *state, uint32_t cmd,
				int (*func)(kdmsg_state_t *, kdmsg_msg_t *),
				void *data);
void kdmsg_msg_write(kdmsg_msg_t *msg);
void kdmsg_msg_reply(kdmsg_msg_t *msg, uint32_t error);
void kdmsg_msg_result(kdmsg_msg_t *msg, uint32_t error);
void kdmsg_state_reply(kdmsg_state_t *state, uint32_t error);
void kdmsg_state_result(kdmsg_state_t *state, uint32_t error);
void kdmsg_detach_aux_data(kdmsg_msg_t *msg, kdmsg_data_t *data);
void kdmsg_free_aux_data(kdmsg_data_t *data);

#endif

#endif
