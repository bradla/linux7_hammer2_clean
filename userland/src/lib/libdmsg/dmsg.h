#include <sys/types.h>
#include <sys/queue.h>
#include <sys/tree.h>
#include <sys/dmsg.h>
#include <sys/dfly.h>

#include <pthread.h>

#include <sys/uio.h>

#if 0
#include <openssl/rsa.h>
#include <openssl/pem.h>
#endif
#include <openssl/err.h>
#include <openssl/evp.h>

#define DMSG_DEFAULT_DIR	"/etc/hammer2"
#define DMSG_PATH_REMOTE	DMSG_DEFAULT_DIR "/remote"

#define DMSG_LISTEN_PORT	987

#define dm_printf(level, ctl, ...)		\
	if (DMsgDebugOpt >= (level))		\
		fprintf(stderr, "libdmsg: " ctl, __VA_ARGS__)
#define dmx_printf(level, ctl, ...)		\
	if (DMsgDebugOpt >= (level))		\
		fprintf(stderr, ctl, __VA_ARGS__)
#define dmio_printf(iocom, level, ctl, ...)	\
	if (DMsgDebugOpt >= (level))		\
		fprintf(stderr, "libdmsg: " ctl, __VA_ARGS__)

struct dmsg_handshake {
	char pad1[8];
	uint16_t magic;
	uint16_t version;
	uint32_t flags;
	uint8_t sess[64];
	uint8_t verf[16];
	char quickmsg[32];
	char junk080[128];
	char pad2[8];
	char junk100[256-8];
};

typedef struct dmsg_handshake dmsg_handshake_t;

#define DMSG_CRYPTO_CHUNK_SIZE		DMSG_ALIGN
#define DMSG_MAX_IV_SIZE		32

#define DMSG_CRYPTO_GCM_IV_FIXED_SIZE	4
#define DMSG_CRYPTO_GCM_IV_SIZE		12
#define DMSG_CRYPTO_GCM_KEY_SIZE	32

#define DMSG_CRYPTO_ALGO_GCM_IDX	0

#define DMSG_CRYPTO_ALGO		DMSG_CRYPTO_ALGO_GCM_IDX

struct dmsg_iocom;
struct dmsg_state;
struct dmsg_msg;

TAILQ_HEAD(dmsg_state_queue, dmsg_state);
TAILQ_HEAD(dmsg_msg_queue, dmsg_msg);
RB_HEAD(dmsg_state_tree, dmsg_state);

struct h2span_link;
struct h2span_relay;
struct h2span_conn;

TAILQ_HEAD(dmsg_media_queue, dmsg_media);

struct dmsg_media {
	TAILQ_ENTRY(dmsg_media) entry;
	dmsg_uuid_t  media_id;
	int     refs;
	void	*usrhandle;
};

typedef struct dmsg_media dmsg_media_t;

struct dmsg_state {
	RB_ENTRY(dmsg_state) rbnode;
	struct dmsg_state	*scan;
	TAILQ_HEAD(, dmsg_state) subq;
	TAILQ_ENTRY(dmsg_state) entry;
	struct dmsg_iocom *iocom;
	struct dmsg_state *parent;
	struct dmsg_state *relay;
	uint32_t	icmd;
	uint32_t	txcmd;
	uint32_t	rxcmd;
	uint64_t	msgid;
	int		flags;
	int		error;
	unsigned int	refs;
	void (*func)(struct dmsg_msg *);
	union {
		void *any;
		struct h2span_link *link;
		struct h2span_conn *conn;
		struct h2span_relay *relay;
	} any;
	dmsg_media_t	*media;
};

#define DMSG_STATE_SUBINSERTED	0x0001
#define DMSG_STATE_DYNAMIC	0x0002
#define DMSG_STATE_UNUSED0004	0x0004
#define DMSG_STATE_ABORTING	0x0008
#define DMSG_STATE_OPPOSITE	0x0010
#define DMSG_STATE_CIRCUIT	0x0020
#define DMSG_STATE_DYING	0x0040
#define DMSG_STATE_RBINSERTED	0x0080
#define DMSG_STATE_NEW		0x0400
#define DMSG_STATE_ROOT		0x8000

struct dmsg_msg {
	TAILQ_ENTRY(dmsg_msg) qentry;
	struct dmsg_state *state;
	size_t		hdr_size;
	size_t		aux_size;
	char		*aux_data;
	uint32_t	tcmd;
	dmsg_any_t 	any;
};

typedef struct dmsg_state dmsg_state_t;
typedef struct dmsg_msg dmsg_msg_t;
typedef struct dmsg_msg_queue dmsg_msg_queue_t;

int dmsg_state_cmp(dmsg_state_t *state1, dmsg_state_t *state2);
RB_PROTOTYPE(dmsg_state_tree, dmsg_state, rbnode, dmsg_state_cmp);

struct dmsg_ioq {
	enum { DMSG_MSGQ_STATE_HEADER1,
	       DMSG_MSGQ_STATE_HEADER2,
	       DMSG_MSGQ_STATE_AUXDATA1,
	       DMSG_MSGQ_STATE_AUXDATA2,
	       DMSG_MSGQ_STATE_ERROR } state;
	size_t		fifo_beg;
	size_t		fifo_cdx;
	size_t		fifo_cdn;
	size_t		fifo_end;
	size_t		hbytes;
	size_t		abytes;
	size_t		unaligned_aux_size;
	int		error;
	int		seq;
	int		msgcount;
	EVP_CIPHER_CTX	*ctx;
	char		iv[DMSG_MAX_IV_SIZE];
	dmsg_msg_t	*msg;
	dmsg_msg_queue_t msgq;
	char		buf[DMSG_BUF_SIZE];
};

typedef struct dmsg_ioq dmsg_ioq_t;

#define DMSG_IOQ_ERROR_SYNC		1
#define DMSG_IOQ_ERROR_EOF		2
#define DMSG_IOQ_ERROR_SOCK		3
#define DMSG_IOQ_ERROR_FIELD		4
#define DMSG_IOQ_ERROR_HCRC		5
#define DMSG_IOQ_ERROR_XCRC		6
#define DMSG_IOQ_ERROR_ACRC		7
#define DMSG_IOQ_ERROR_STATE		8
#define DMSG_IOQ_ERROR_NOPEER		9
#define DMSG_IOQ_ERROR_NORKEY		10
#define DMSG_IOQ_ERROR_NOLKEY		11
#define DMSG_IOQ_ERROR_KEYXCHGFAIL	12
#define DMSG_IOQ_ERROR_KEYFMT		13
#define DMSG_IOQ_ERROR_BADURANDOM	14
#define DMSG_IOQ_ERROR_MSGSEQ		15
#define DMSG_IOQ_ERROR_EALREADY		16
#define DMSG_IOQ_ERROR_TRANS		17
#define DMSG_IOQ_ERROR_IVWRAP		18
#define DMSG_IOQ_ERROR_MACFAIL		19
#define DMSG_IOQ_ERROR_ALGO		20
#define DMSG_IOQ_ERROR_UNUSED21		21
#define DMSG_IOQ_ERROR_BAD_CIRCUIT	22
#define DMSG_IOQ_ERROR_UNUSED23		23
#define DMSG_IOQ_ERROR_ASSYM		24

#define DMSG_IOQ_MAXIOVEC    16

struct dmsg_iocom {
	char		*label;
	dmsg_ioq_t	ioq_rx;
	dmsg_ioq_t	ioq_tx;
	dmsg_state_t	state0;
	struct dmsg_state_tree  staterd_tree;
	struct dmsg_state_tree  statewr_tree;
	int	sock_fd;
	int	alt_fd;
	int	wakeupfds[2];
	unsigned int	flags;
	int	rxmisc;
	int	txmisc;
	void	(*signal_callback)(struct dmsg_iocom *);
	void	(*altmsg_callback)(struct dmsg_iocom *);
	void	(*rcvmsg_callback)(dmsg_msg_t *msg);
	void	(*usrmsg_callback)(dmsg_msg_t *msg, int unmanaged);
	dmsg_msg_queue_t txmsgq;
	struct h2span_conn *conn;
	uint64_t	conn_msgid;
	pthread_mutex_t	mtx;
};

typedef struct dmsg_iocom dmsg_iocom_t;

#define DMSG_IOCOMF_EOF		0x00000001
#define DMSG_IOCOMF_RREQ	0x00000002
#define DMSG_IOCOMF_WREQ	0x00000004
#define DMSG_IOCOMF_RWORK	0x00000008
#define DMSG_IOCOMF_WWORK	0x00000010
#define DMSG_IOCOMF_PWORK	0x00000020
#define DMSG_IOCOMF_ARWORK	0x00000040
#define DMSG_IOCOMF_AWWORK	0x00000080
#define DMSG_IOCOMF_SWORK	0x00000100
#define DMSG_IOCOMF_CRYPTED	0x00000200
#define DMSG_IOCOMF_CLOSEALT	0x00000400

typedef int (*algo_init_fn)(dmsg_ioq_t *, char *, int, char *, int, int);
typedef void (*algo_uninit_fn)(dmsg_ioq_t *);
typedef int (*algo_enc_fn)(dmsg_ioq_t *, char *, char *, int, int *);
typedef int (*algo_dec_fn)(dmsg_ioq_t *, char *, char *, int, int *);

struct crypto_algo {
	const char	*name;
	int		keylen;
	int		unused01;
	algo_init_fn	init;
	algo_uninit_fn	uninit;
	algo_enc_fn	enc_chunk;
	algo_dec_fn	dec_chunk;
};

struct dmsg_master_service_info {
	int	fd;
	int	altfd;
	int	noclosealt;
	int	detachme;
	char	*label;
	void	*handle;
	void	(*altmsg_callback)(dmsg_iocom_t *iocom);
	void	(*usrmsg_callback)(dmsg_msg_t *msg, int unmanaged);
	void	(*exit_callback)(void *handle);
};

typedef struct dmsg_master_service_info dmsg_master_service_info_t;

struct iovec;

#define DMSG_NODEOP_ADD		1
#define DMSG_NODEOP_DEL		2

#define dmsg_icrc32(buf, size)		iscsi_crc32((buf), (size))
#define dmsg_icrc32c(buf, size, crc)	iscsi_crc32_ext((buf), (size), (crc))

const char *dmsg_basecmd_str(uint32_t cmd);
const char *dmsg_msg_str(dmsg_msg_t *msg);

void *dmsg_alloc(size_t bytes);
void dmsg_free(void *ptr);
const char *dmsg_uuid_to_str(dmsg_uuid_t *uuid, char **strp);
const char *dmsg_peer_type_to_str(uint8_t type);
int dmsg_connect(const char *hostname);

void dmsg_bswap_head(dmsg_hdr_t *head);
void dmsg_ioq_init(dmsg_iocom_t *iocom, dmsg_ioq_t *ioq);
void dmsg_ioq_done(dmsg_iocom_t *iocom, dmsg_ioq_t *ioq);
void dmsg_iocom_init(dmsg_iocom_t *iocom, int sock_fd, int alt_fd,
			void (*state_func)(dmsg_iocom_t *iocom),
			void (*rcvmsg_func)(dmsg_msg_t *msg),
			void (*usrmsg_func)(dmsg_msg_t *msg, int unmanaged),
			void (*altmsg_func)(dmsg_iocom_t *iocom));
void dmsg_iocom_restate(dmsg_iocom_t *iocom,
			void (*state_func)(dmsg_iocom_t *iocom),
			void (*rcvmsg_func)(dmsg_msg_t *msg));
void dmsg_iocom_label(dmsg_iocom_t *iocom, const char *ctl, ...);
void dmsg_iocom_signal(dmsg_iocom_t *iocom);
void dmsg_iocom_done(dmsg_iocom_t *iocom);
dmsg_msg_t *dmsg_msg_alloc(dmsg_state_t *state, size_t aux_size,
			uint32_t cmd,
			void (*func)(dmsg_msg_t *), void *data);
dmsg_msg_t *dmsg_msg_alloc_locked(dmsg_state_t *state, size_t aux_size,
			uint32_t cmd,
			void (*func)(dmsg_msg_t *), void *data);
void dmsg_msg_reply(dmsg_msg_t *msg, uint32_t error);
void dmsg_msg_result(dmsg_msg_t *msg, uint32_t error);
void dmsg_state_reply(dmsg_state_t *state, uint32_t error);
void dmsg_state_result(dmsg_state_t *state, uint32_t error);

void dmsg_msg_free(dmsg_msg_t *msg);

void dmsg_iocom_core(dmsg_iocom_t *iocom);
dmsg_msg_t *dmsg_ioq_read(dmsg_iocom_t *iocom);
void dmsg_msg_write(dmsg_msg_t *msg);

void dmsg_iocom_drain(dmsg_iocom_t *iocom);
void dmsg_iocom_flush1(dmsg_iocom_t *iocom);
void dmsg_iocom_flush2(dmsg_iocom_t *iocom);

void dmsg_state_relay(dmsg_msg_t *msg);
void dmsg_state_cleanuprx(dmsg_iocom_t *iocom, dmsg_msg_t *msg);
void dmsg_state_hold(dmsg_state_t *state);
void dmsg_state_drop(dmsg_state_t *state);

void dmsg_msg_lnk_signal(dmsg_iocom_t *iocom);
void dmsg_msg_lnk(dmsg_msg_t *msg);
void dmsg_msg_dbg(dmsg_msg_t *msg);
void dmsg_shell_tree(dmsg_iocom_t *iocom, char *cmdbuf __unused);
int dmsg_debug_findspan(uint64_t msgid, dmsg_state_t **statep);
dmsg_state_t *dmsg_findspan(const char *label);

void dmsg_crypto_setup(void);
void dmsg_crypto_negotiate(dmsg_iocom_t *iocom);
void dmsg_crypto_terminate(dmsg_iocom_t *iocom);
void dmsg_crypto_decrypt(dmsg_iocom_t *iocom, dmsg_ioq_t *ioq);
int dmsg_crypto_encrypt(dmsg_iocom_t *iocom, dmsg_ioq_t *ioq,
			struct iovec *iov, int n, size_t *nactp);

void *dmsg_master_service(void *data);
void dmsg_printf(dmsg_iocom_t *iocom, const char *ctl, ...) __printflike(2, 3);

extern int DMsgDebugOpt;

void dmsg_uuid_create(dmsg_uuid_t *uuid);
int dmsg_uuid_from_string(const char *str, dmsg_uuid_t *uuid);
int dmsg_uuid_to_string(const dmsg_uuid_t *uuid, char **str);
int dmsg_uuid_compare(const dmsg_uuid_t *uuid1, const dmsg_uuid_t *uuid2);
int dmsg_uuid_is_nil(dmsg_uuid_t *uuid);

#define bswap16(x) bswap_16(x)
#define bswap32(x) bswap_32(x)
#define bswap64(x) bswap_64(x)
