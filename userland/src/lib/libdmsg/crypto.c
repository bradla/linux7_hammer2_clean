#define _GNU_SOURCE

#include "dmsg_local.h"

static pthread_mutex_t *crypto_locks;
int crypto_count;

static int dmsg_crypto_gcm_init(dmsg_ioq_t *, char *, int, char *, int, int);
static void dmsg_crypto_gcm_uninit(dmsg_ioq_t *);
static int dmsg_crypto_gcm_encrypt_chunk(dmsg_ioq_t *, char *, char *, int, int *);
static int dmsg_crypto_gcm_decrypt_chunk(dmsg_ioq_t *, char *, char *, int, int *);

static struct crypto_algo crypto_algos[] = {
	{
		.name      = "aes-256-gcm",
		.keylen    = DMSG_CRYPTO_GCM_KEY_SIZE,
		.unused01  = 0,
		.init      = dmsg_crypto_gcm_init,
		.uninit    = dmsg_crypto_gcm_uninit,
		.enc_chunk = dmsg_crypto_gcm_encrypt_chunk,
		.dec_chunk = dmsg_crypto_gcm_decrypt_chunk
	},
	{ NULL, 0, 0, NULL, NULL, NULL, NULL }
};

static
unsigned long
dmsg_crypto_id_callback(void)
{
	return ((unsigned long)(uintptr_t)pthread_self());
}

static
void
dmsg_crypto_locking_callback(int mode, int type,
				const char *file __unused, int line __unused)
{
	assert(type >= 0 && type < crypto_count);
	if (mode & CRYPTO_LOCK) {
		pthread_mutex_lock(&crypto_locks[type]);
	} else {
		pthread_mutex_unlock(&crypto_locks[type]);
	}
}

void
dmsg_crypto_setup(void)
{
	crypto_count = CRYPTO_num_locks();
	crypto_locks = calloc(crypto_count, sizeof(crypto_locks[0]));
	CRYPTO_set_id_callback(dmsg_crypto_id_callback);
	CRYPTO_set_locking_callback(dmsg_crypto_locking_callback);
}

static
int
dmsg_crypto_gcm_init(dmsg_ioq_t *ioq, char *key, int klen,
			char *iv_fixed, int ivlen, int enc)
{
	int i, ok;

	if (klen < DMSG_CRYPTO_GCM_KEY_SIZE ||
	    ivlen < DMSG_CRYPTO_GCM_IV_FIXED_SIZE) {
		dm_printf(1, "%s\n", "Not enough key or iv material");
		return -1;
	}

	dm_printf(6, "%s key: ", enc ? "Encryption" : "Decryption");
	for (i = 0; i < DMSG_CRYPTO_GCM_KEY_SIZE; ++i)
		dmx_printf(6, "%02x", (unsigned char)key[i]);
	dmx_printf(6, "%s\n", "");

	dm_printf(6, "%s iv:  ", enc ? "Encryption" : "Decryption");
	for (i = 0; i < DMSG_CRYPTO_GCM_IV_FIXED_SIZE; ++i)
		dmx_printf(6, "%02x", (unsigned char)iv_fixed[i]);
	dmx_printf(6, "%s\n", " (fixed part only)");

	memset(ioq->iv, 0, DMSG_CRYPTO_GCM_IV_SIZE);
	memcpy(ioq->iv, iv_fixed, DMSG_CRYPTO_GCM_IV_FIXED_SIZE);

	ioq->ctx = EVP_CIPHER_CTX_new();

	if (enc)
		ok = EVP_EncryptInit_ex(ioq->ctx, EVP_aes_256_gcm(), NULL,
					(unsigned char*)key, ioq->iv);
	else
		ok = EVP_DecryptInit_ex(ioq->ctx, EVP_aes_256_gcm(), NULL,
					(unsigned char*)key, ioq->iv);
	if (!ok)
		goto fail;

	ok = EVP_CIPHER_CTX_ctrl(ioq->ctx, EVP_CTRL_GCM_SET_IVLEN,
				 DMSG_CRYPTO_GCM_IV_SIZE, NULL);
	if (!ok)
		goto fail;

	EVP_CIPHER_CTX_set_padding(ioq->ctx, 0);

	return 0;

fail:
	dm_printf(1, "%s\n", "Error during _gcm_init");
	return -1;
}

static
void
dmsg_crypto_gcm_uninit(dmsg_ioq_t *ioq)
{
	EVP_CIPHER_CTX_free(ioq->ctx);
	ioq->ctx = NULL;
}

static
int
_gcm_iv_increment(char *iv)
{

	uint64_t *c = (uint64_t *)(&iv[DMSG_CRYPTO_GCM_IV_FIXED_SIZE]);

	*c = htobe64(be64toh(*c)+1);

	return (*c == 0) ? 0 : 1;
}

static
int
dmsg_crypto_gcm_encrypt_chunk(dmsg_ioq_t *ioq, char *ct, char *pt,
				 int in_size, int *out_size)
{
	int ok;
	int u_len;

	*out_size = 0;

	ok = 0;

	if (!ok)
		goto fail;

	u_len = 0;
	ok = EVP_EncryptUpdate(ioq->ctx, (unsigned char*)ct, &u_len,
		(unsigned char*)pt, in_size);
	if (!ok)
		goto fail;

	ok = _gcm_iv_increment(ioq->iv);
	if (!ok) {
		ioq->error = DMSG_IOQ_ERROR_IVWRAP;
		goto fail_out;
	}

	*out_size = u_len;

	return 0;

fail:
	ioq->error = DMSG_IOQ_ERROR_ALGO;
fail_out:
	dm_printf(1, "%s\n", "error during encrypt_chunk");
	return -1;
}

static
int
dmsg_crypto_gcm_decrypt_chunk(dmsg_ioq_t *ioq, char *ct, char *pt,
				 int out_size, int *consume_size)
{
	int ok;
	int u_len;

	*consume_size = 0;

	ok = 0;

	if (!ok) {
		ioq->error = DMSG_IOQ_ERROR_ALGO;
		goto fail_out;
	}

	ok = EVP_DecryptUpdate(ioq->ctx, (unsigned char*)pt, &u_len,
		(unsigned char*)ct, out_size);
	if (!ok)
		goto fail;

	ok = _gcm_iv_increment(ioq->iv);
	if (!ok) {
		ioq->error = DMSG_IOQ_ERROR_IVWRAP;
		goto fail_out;
	}

	*consume_size = u_len;

	return 0;

fail:
	ioq->error = DMSG_IOQ_ERROR_MACFAIL;
fail_out:
	dm_printf(1, "%s\n",
		  "error during decrypt_chunk "
		  "(likely authentication error)");
	return -1;
}

typedef union {
	struct sockaddr sa;
	struct sockaddr_in sa_in;
	struct sockaddr_in6 sa_in6;
} sockaddr_any_t;

void
dmsg_crypto_negotiate(dmsg_iocom_t *iocom)
{
	sockaddr_any_t sa;
	socklen_t salen = sizeof(sa);
	char peername[128];
	char realname[128];
	dmsg_handshake_t handtx;
	dmsg_handshake_t handrx;
	char buf1[sizeof(handtx)];
	char buf2[sizeof(handtx)];
	char *ptr;
	char *path = NULL;
	struct stat st;
	FILE *fp;
	RSA *keys[3] = { NULL, NULL, NULL };
	size_t i;
	size_t blksize;
	size_t blkmask;
	ssize_t n;
	int fd;
	int error;

	if (getpeername(iocom->sock_fd, &sa.sa, &salen) < 0) {
		iocom->ioq_rx.error = DMSG_IOQ_ERROR_NOPEER;
		atomic_set_int(&iocom->flags, DMSG_IOCOMF_EOF);
		dm_printf(1, "%s\n", "accept: getpeername() failed");
		goto done;
	}
	if (getnameinfo(&sa.sa, salen, peername, sizeof(peername),
			NULL, 0, NI_NUMERICHOST) < 0) {
		iocom->ioq_rx.error = DMSG_IOQ_ERROR_NOPEER;
		atomic_set_int(&iocom->flags, DMSG_IOCOMF_EOF);
		dm_printf(1, "%s\n", "accept: cannot decode sockaddr");
		goto done;
	}
	if (DMsgDebugOpt) {
		if (realhostname_sa(realname, sizeof(realname),
				    &sa.sa, salen) == HOSTNAME_FOUND) {
			dm_printf(1, "accept from %s (%s)\n",
				  peername, realname);
		} else {
			dm_printf(1, "accept from %s\n", peername);
		}
	}

	asprintf(&path, "%s/%s.pub", DMSG_PATH_REMOTE, peername);
	if ((fp = fopen(path, "r")) == NULL) {
		free(path);
		asprintf(&path, "%s/%s.none",
			 DMSG_PATH_REMOTE, peername);
		if (stat(path, &st) < 0) {
			iocom->ioq_rx.error = DMSG_IOQ_ERROR_NORKEY;
			atomic_set_int(&iocom->flags, DMSG_IOCOMF_EOF);
			dm_printf(1, "%s\n", "auth failure: unknown host");
			goto done;
		}
		dm_printf(1, "%s\n", "auth succeeded, unencrypted link");
		goto done;
	}
	if (fp) {
		keys[0] = PEM_read_RSA_PUBKEY(fp, NULL, NULL, NULL);
		fclose(fp);
		if (keys[0] == NULL) {
			iocom->ioq_rx.error = DMSG_IOQ_ERROR_KEYFMT;
			atomic_set_int(&iocom->flags, DMSG_IOCOMF_EOF);
			dm_printf(1, "%s\n", "auth failure: bad key format");
			goto done;
		}
	}

	free(path);
	asprintf(&path, DMSG_DEFAULT_DIR "/rsa.pub");
	if ((fp = fopen(path, "r")) == NULL) {
		iocom->ioq_rx.error = DMSG_IOQ_ERROR_NOLKEY;
		atomic_set_int(&iocom->flags, DMSG_IOCOMF_EOF);
		goto done;
	}
	keys[1] = PEM_read_RSA_PUBKEY(fp, NULL, NULL, NULL);
	fclose(fp);
	if (keys[1] == NULL) {
		iocom->ioq_rx.error = DMSG_IOQ_ERROR_KEYFMT;
		atomic_set_int(&iocom->flags, DMSG_IOCOMF_EOF);
		dm_printf(1, "%s\n", "auth failure: bad host key format");
		goto done;
	}

	free(path);
	asprintf(&path, DMSG_DEFAULT_DIR "/rsa.prv");
	if ((fp = fopen(path, "r")) == NULL) {
		iocom->ioq_rx.error = DMSG_IOQ_ERROR_NOLKEY;
		atomic_set_int(&iocom->flags, DMSG_IOCOMF_EOF);
		dm_printf(1, "%s\n", "auth failure: bad host key format");
		goto done;
	}
	keys[2] = PEM_read_RSAPrivateKey(fp, NULL, NULL, NULL);
	fclose(fp);
	if (keys[2] == NULL) {
		iocom->ioq_rx.error = DMSG_IOQ_ERROR_KEYFMT;
		atomic_set_int(&iocom->flags, DMSG_IOCOMF_EOF);
		dm_printf(1, "%s\n", "auth failure: bad host key format");
		goto done;
	}
	free(path);
	path = NULL;

	if (keys[0]) {
		blksize = (size_t)RSA_size(keys[0]);
		if (blksize != (size_t)RSA_size(keys[1]) ||
		    blksize != (size_t)RSA_size(keys[2]) ||
		    sizeof(handtx) % blksize != 0) {
			iocom->ioq_rx.error = DMSG_IOQ_ERROR_KEYFMT;
			atomic_set_int(&iocom->flags, DMSG_IOCOMF_EOF);
			dm_printf(1, "%s\n",
				  "auth failure: key size mismatch");
			goto done;
		}
	} else {
		blksize = sizeof(handtx);
	}
	blkmask = blksize - 1;

	bzero(&handrx, sizeof(handrx));
	bzero(&handtx, sizeof(handtx));

	fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0 ||
	    fstat(fd, &st) < 0 ||
	    S_ISREG(st.st_mode) ||
	    read(fd, &handtx, sizeof(handtx)) != sizeof(handtx)) {
urandfail:
		if (fd >= 0)
			close(fd);
		iocom->ioq_rx.error = DMSG_IOQ_ERROR_BADURANDOM;
		atomic_set_int(&iocom->flags, DMSG_IOCOMF_EOF);
		dm_printf(1, "%s\n", "auth failure: bad rng");
		goto done;
	}
	if (bcmp(&handrx, &handtx, sizeof(handtx)) == 0)
		goto urandfail;
	close(fd);

	snprintf(handtx.quickmsg, sizeof(handtx.quickmsg), "Testing 1 2 3");
	handtx.magic = DMSG_HDR_MAGIC;
	handtx.version = 1;
	handtx.flags = 0;
	assert(sizeof(handtx.verf) * 4 == sizeof(handtx.sess));
	bzero(handtx.verf, sizeof(handtx.verf));

	handtx.pad1[0] &= 0x3f;
	handtx.pad2[0] &= 0x3f;

	for (i = 0; i < sizeof(handtx.sess); ++i)
		handtx.verf[i / 4] ^= handtx.sess[i];

	for (i = 0; i < sizeof(handtx); i += blksize) {
		ptr = (char *)&handtx + i;
		if (keys[0]) {

			do {
				++*(int *)(ptr + 4);
				if (RSA_private_encrypt(blksize,
					    (unsigned char*)ptr,
					    (unsigned char*)buf1,
					    keys[2], RSA_NO_PADDING) < 0) {
					iocom->ioq_rx.error =
						DMSG_IOQ_ERROR_KEYXCHGFAIL;
				}
			} while (buf1[0] & 0xC0);

			if (RSA_public_encrypt(blksize,
					    (unsigned char*)buf1,
					    (unsigned char*)buf2,
					    keys[0], RSA_NO_PADDING) < 0) {
				iocom->ioq_rx.error =
					DMSG_IOQ_ERROR_KEYXCHGFAIL;
			}
		}
		if (write(iocom->sock_fd, buf2, blksize) != (ssize_t)blksize) {
			dmio_printf(iocom, 1, "%s\n", "WRITE ERROR");
		}
	}
	if (iocom->ioq_rx.error) {
		atomic_set_int(&iocom->flags, DMSG_IOCOMF_EOF);
		dmio_printf(iocom, 1, "%s\n",
			    "auth failure: key exchange failure "
			    "during encryption");
		goto done;
	}

	i = 0;
	while (i < sizeof(handrx)) {
		ptr = (char *)&handrx + i;
		n = read(iocom->sock_fd, ptr, blksize - (i & blkmask));
		if (n <= 0)
			break;
		ptr -= (i & blkmask);
		i += n;
		if (keys[0] && (i & blkmask) == 0) {
			if (RSA_private_decrypt(blksize,
					   (unsigned char*)ptr,
					   (unsigned char*)buf1,
					   keys[2], RSA_NO_PADDING) < 0)
				iocom->ioq_rx.error =
						DMSG_IOQ_ERROR_KEYXCHGFAIL;
			if (RSA_public_decrypt(blksize,
					   (unsigned char*)buf1,
					   (unsigned char*)ptr,
					   keys[0], RSA_NO_PADDING) < 0)
				iocom->ioq_rx.error =
						DMSG_IOQ_ERROR_KEYXCHGFAIL;
		}
	}
	if (iocom->ioq_rx.error) {
		atomic_set_int(&iocom->flags, DMSG_IOCOMF_EOF);
		dmio_printf(iocom, 1, "%s\n",
			    "auth failure: key exchange failure "
			    "during decryption");
		goto done;
	}

	if (i != sizeof(handrx)) {
keyxchgfail:
		iocom->ioq_rx.error = DMSG_IOQ_ERROR_KEYXCHGFAIL;
		atomic_set_int(&iocom->flags, DMSG_IOCOMF_EOF);
		dmio_printf(iocom, 1, "%s\n",
			    "auth failure: key exchange failure");
		goto done;
	}

	if (handrx.magic == DMSG_HDR_MAGIC_REV) {
		handrx.version = bswap16(handrx.version);
		handrx.flags = bswap32(handrx.flags);
	}
	for (i = 0; i < sizeof(handrx.sess); ++i)
		handrx.verf[i / 4] ^= handrx.sess[i];
	n = 0;
	for (i = 0; i < sizeof(handrx.verf); ++i)
		n += handrx.verf[i];
	if (handrx.version != 1)
		++n;
	if (n != 0)
		goto keyxchgfail;

	error = crypto_algos[DMSG_CRYPTO_ALGO].init(&iocom->ioq_rx,
	    (char*)handrx.sess,
	    crypto_algos[DMSG_CRYPTO_ALGO].keylen,
	    (char*)handrx.sess + crypto_algos[DMSG_CRYPTO_ALGO].keylen,
	    sizeof(handrx.sess) - crypto_algos[DMSG_CRYPTO_ALGO].keylen,
	    0  );
	if (error)
		goto keyxchgfail;

	error = crypto_algos[DMSG_CRYPTO_ALGO].init(&iocom->ioq_tx,
	    (char*)handtx.sess,
	    crypto_algos[DMSG_CRYPTO_ALGO].keylen,
	    (char*)handtx.sess + crypto_algos[DMSG_CRYPTO_ALGO].keylen,
	    sizeof(handtx.sess) - crypto_algos[DMSG_CRYPTO_ALGO].keylen,
	    1  );
	if (error)
		goto keyxchgfail;

	atomic_set_int(&iocom->flags, DMSG_IOCOMF_CRYPTED);

	dmio_printf(iocom, 1, "auth success: %s\n", handrx.quickmsg);
done:
	if (path)
		free(path);
	if (keys[0])
		RSA_free(keys[0]);
	if (keys[1])
		RSA_free(keys[1]);
	if (keys[2])
		RSA_free(keys[2]);
}

void
dmsg_crypto_terminate(dmsg_iocom_t *iocom)
{
	crypto_algos[DMSG_CRYPTO_ALGO].uninit(&iocom->ioq_rx);
	crypto_algos[DMSG_CRYPTO_ALGO].uninit(&iocom->ioq_tx);
}

void
dmsg_crypto_decrypt(dmsg_iocom_t *iocom __unused, dmsg_ioq_t *ioq)
{
	int p_len;
	int used;
	__unused int error;
	char buf[512];

	p_len = ioq->fifo_end - ioq->fifo_cdn;

	if (p_len == 0)
		return;

	while (p_len >= DMSG_CRYPTO_CHUNK_SIZE) {
		bcopy(ioq->buf + ioq->fifo_cdn, buf, DMSG_CRYPTO_CHUNK_SIZE);
		error = crypto_algos[DMSG_CRYPTO_ALGO].dec_chunk(
		    ioq, buf,
		    ioq->buf + ioq->fifo_cdx,
		    DMSG_CRYPTO_CHUNK_SIZE,
		    &used);
#ifdef CRYPTO_DEBUG
		dmio_printf(iocom, 5,
			    "dec: p_len: %d, used: %d, "
			    "fifo_cdn: %ju, fifo_cdx: %ju\n",
			     p_len, used,
			     ioq->fifo_cdn, ioq->fifo_cdx);
#endif
		p_len -= used;
		ioq->fifo_cdn += used;
		ioq->fifo_cdx += DMSG_CRYPTO_CHUNK_SIZE;
#ifdef CRYPTO_DEBUG
		dmio_printf(iocom, 5,
			    "dec: p_len: %d, used: %d, "
			    "fifo_cdn: %ju, fifo_cdx: %ju\n",
			    p_len, used, ioq->fifo_cdn, ioq->fifo_cdx);
#endif
	}
}

int
dmsg_crypto_encrypt(dmsg_iocom_t *iocom __unused, dmsg_ioq_t *ioq,
		    struct iovec *iov, int n, size_t *nactp)
{
	int p_len, used, ct_used;
	int i;
	__unused int error;
	size_t nmax;

	nmax = sizeof(ioq->buf) - ioq->fifo_end;

	*nactp = 0;
	for (i = 0; i < n && nmax; ++i) {
		used = 0;
		p_len = iov[i].iov_len;
		assert((p_len & DMSG_ALIGNMASK) == 0);

		while (p_len >= DMSG_CRYPTO_CHUNK_SIZE &&
		    nmax >= DMSG_CRYPTO_CHUNK_SIZE)
		{
			error = crypto_algos[DMSG_CRYPTO_ALGO].enc_chunk(
			    ioq,
			    ioq->buf + ioq->fifo_cdx,
			    (char *)iov[i].iov_base + used,
			    DMSG_CRYPTO_CHUNK_SIZE, &ct_used);
#ifdef CRYPTO_DEBUG
			dmio_printf(iocom, 5,
				    "nactp: %ju, p_len: %d, "
				    "ct_used: %d, used: %d, nmax: %ju\n",
				    *nactp, p_len, ct_used, used, nmax);
#endif

			*nactp += (size_t)DMSG_CRYPTO_CHUNK_SIZE;
			used += DMSG_CRYPTO_CHUNK_SIZE;
			p_len -= DMSG_CRYPTO_CHUNK_SIZE;

			ioq->fifo_cdx += (size_t)ct_used;
			ioq->fifo_cdn += (size_t)ct_used;
			ioq->fifo_end += (size_t)ct_used;
			nmax -= (size_t)ct_used;
#ifdef CRYPTO_DEBUG
			dmio_printf(iocom, 5,
				    "nactp: %ju, p_len: %d, "
				    "ct_used: %d, used: %d, nmax: %ju\n",
				    *nactp, p_len, ct_used, used, nmax);
#endif
		}
	}
	iov[0].iov_base = ioq->buf + ioq->fifo_beg;
	iov[0].iov_len = ioq->fifo_cdx - ioq->fifo_beg;

	return (1);
}
