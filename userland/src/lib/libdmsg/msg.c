#define _GNU_SOURCE

#include "dmsg_local.h"

#define DMSG_BLOCK_DEBUG

int DMsgDebugOpt;
static unsigned int dmsg_state_count;
#ifdef DMSG_BLOCK_DEBUG
static unsigned int biocount;
#endif

static int dmsg_state_msgrx(dmsg_msg_t *msg, int mstate);
static void dmsg_state_cleanuptx(dmsg_iocom_t *iocom, dmsg_msg_t *msg);
static void dmsg_msg_free_locked(dmsg_msg_t *msg);
static void dmsg_state_free(dmsg_state_t *state);
static void dmsg_subq_delete(dmsg_state_t *state);
static void dmsg_simulate_failure(dmsg_state_t *state, int meto, int error);
static void dmsg_state_abort(dmsg_state_t *state);
static void dmsg_state_dying(dmsg_state_t *state);

RB_GENERATE(dmsg_state_tree, dmsg_state, rbnode, dmsg_state_cmp);

int
dmsg_state_cmp(dmsg_state_t *state1, dmsg_state_t *state2)
{
	if (state1->msgid < state2->msgid)
		return(-1);
	if (state1->msgid > state2->msgid)
		return(1);
	return(0);
}

void
dmsg_ioq_init(dmsg_iocom_t *iocom __unused, dmsg_ioq_t *ioq)
{
	bzero(ioq, sizeof(*ioq));
	ioq->state = DMSG_MSGQ_STATE_HEADER1;
	TAILQ_INIT(&ioq->msgq);
}

void
dmsg_ioq_done(dmsg_iocom_t *iocom __unused, dmsg_ioq_t *ioq)
{
	dmsg_msg_t *msg;

	while ((msg = TAILQ_FIRST(&ioq->msgq)) != NULL) {
		assert(0);
		TAILQ_REMOVE(&ioq->msgq, msg, qentry);
		dmsg_msg_free(msg);
	}
	if ((msg = ioq->msg) != NULL) {
		ioq->msg = NULL;
		dmsg_msg_free(msg);
	}
}

void
dmsg_iocom_init(dmsg_iocom_t *iocom, int sock_fd, int alt_fd,
		   void (*signal_func)(dmsg_iocom_t *iocom),
		   void (*rcvmsg_func)(dmsg_msg_t *msg),
		   void (*usrmsg_func)(dmsg_msg_t *msg, int unmanaged),
		   void (*altmsg_func)(dmsg_iocom_t *iocom))
{
	struct stat st;

	bzero(iocom, sizeof(*iocom));

	asprintf(&iocom->label, "iocom-%p", iocom);
	iocom->signal_callback = signal_func;
	iocom->rcvmsg_callback = rcvmsg_func;
	iocom->altmsg_callback = altmsg_func;
	iocom->usrmsg_callback = usrmsg_func;

	{
		pthread_mutexattr_t _mattr;
		pthread_mutexattr_init(&_mattr);
		pthread_mutexattr_settype(&_mattr, PTHREAD_MUTEX_RECURSIVE);
		pthread_mutex_init(&iocom->mtx, &_mattr);
		pthread_mutexattr_destroy(&_mattr);
	}
	RB_INIT(&iocom->staterd_tree);
	RB_INIT(&iocom->statewr_tree);
	TAILQ_INIT(&iocom->txmsgq);
	iocom->sock_fd = sock_fd;
	iocom->alt_fd = alt_fd;
	iocom->flags = DMSG_IOCOMF_RREQ | DMSG_IOCOMF_CLOSEALT;
	if (signal_func)
		iocom->flags |= DMSG_IOCOMF_SWORK;
	dmsg_ioq_init(iocom, &iocom->ioq_rx);
	dmsg_ioq_init(iocom, &iocom->ioq_tx);
	iocom->state0.refs = 1;
	iocom->state0.iocom = iocom;
	iocom->state0.parent = &iocom->state0;
	iocom->state0.flags = DMSG_STATE_ROOT;
	TAILQ_INIT(&iocom->state0.subq);

	if (pipe(iocom->wakeupfds) < 0)
		assert(0);
	fcntl(iocom->wakeupfds[0], F_SETFL, O_NONBLOCK);
	fcntl(iocom->wakeupfds[1], F_SETFL, O_NONBLOCK);

	if (fstat(sock_fd, &st) < 0)
		assert(0);
	if (S_ISSOCK(st.st_mode)) {
		int sdomain = 0;
		socklen_t dlen = sizeof(sdomain);

		if (getsockopt(sock_fd, SOL_SOCKET, SO_DOMAIN,
			       &sdomain, &dlen) < 0)
			sdomain = AF_INET;
		if (sdomain != AF_UNIX && sdomain != AF_LOCAL)
			dmsg_crypto_negotiate(iocom);
	}

	if (sock_fd >= 0)
		fcntl(sock_fd, F_SETFL, O_NONBLOCK);
#if 0

	if (alt_fd >= 0)
		fcntl(alt_fd, F_SETFL, O_NONBLOCK);
#endif
}

void
dmsg_iocom_label(dmsg_iocom_t *iocom, const char *ctl, ...)
{
	va_list va;
	char *optr;

	va_start(va, ctl);
	optr = iocom->label;
	vasprintf(&iocom->label, ctl, va);
	va_end(va);
	if (optr)
		free(optr);
}

void
dmsg_iocom_restate(dmsg_iocom_t *iocom,
		   void (*signal_func)(dmsg_iocom_t *),
		   void (*rcvmsg_func)(dmsg_msg_t *msg))
{
	pthread_mutex_lock(&iocom->mtx);
	iocom->signal_callback = signal_func;
	iocom->rcvmsg_callback = rcvmsg_func;
	if (signal_func)
		atomic_set_int(&iocom->flags, DMSG_IOCOMF_SWORK);
	else
		atomic_clear_int(&iocom->flags, DMSG_IOCOMF_SWORK);
	pthread_mutex_unlock(&iocom->mtx);
}

void
dmsg_iocom_signal(dmsg_iocom_t *iocom)
{
	pthread_mutex_lock(&iocom->mtx);
	if (iocom->signal_callback)
		atomic_set_int(&iocom->flags, DMSG_IOCOMF_SWORK);
	pthread_mutex_unlock(&iocom->mtx);
}

void
dmsg_iocom_done(dmsg_iocom_t *iocom)
{
	dmsg_crypto_terminate(iocom);
	if (iocom->sock_fd >= 0) {
		close(iocom->sock_fd);
		iocom->sock_fd = -1;
	}
	if (iocom->alt_fd >= 0 && (iocom->flags & DMSG_IOCOMF_CLOSEALT)) {
		close(iocom->alt_fd);
		iocom->alt_fd = -1;
	}
	dmsg_ioq_done(iocom, &iocom->ioq_rx);
	dmsg_ioq_done(iocom, &iocom->ioq_tx);
	if (iocom->wakeupfds[0] >= 0) {
		close(iocom->wakeupfds[0]);
		iocom->wakeupfds[0] = -1;
	}
	if (iocom->wakeupfds[1] >= 0) {
		close(iocom->wakeupfds[1]);
		iocom->wakeupfds[1] = -1;
	}
	pthread_mutex_destroy(&iocom->mtx);
}

dmsg_msg_t *
dmsg_msg_alloc(dmsg_state_t *state,
	       size_t aux_size, uint32_t cmd,
	       void (*func)(dmsg_msg_t *), void *data)
{
	dmsg_iocom_t *iocom = state->iocom;
	dmsg_msg_t *msg;

	pthread_mutex_lock(&iocom->mtx);
	msg = dmsg_msg_alloc_locked(state, aux_size, cmd, func, data);
	pthread_mutex_unlock(&iocom->mtx);

	return msg;
}

dmsg_msg_t *
dmsg_msg_alloc_locked(dmsg_state_t *state,
	       size_t aux_size, uint32_t cmd,
	       void (*func)(dmsg_msg_t *), void *data)
{
	dmsg_iocom_t *iocom = state->iocom;
	dmsg_state_t *pstate;
	dmsg_msg_t *msg;
	int hbytes;
	size_t aligned_size;

	aligned_size = DMSG_DOALIGN(aux_size);
	if ((cmd & (DMSGF_CREATE | DMSGF_REPLY)) == DMSGF_CREATE) {

		pstate = state;
		state = malloc(sizeof(*state));
		bzero(state, sizeof(*state));
		atomic_add_int(&dmsg_state_count, 1);

		TAILQ_INIT(&state->subq);
		state->parent = pstate;
		state->iocom = iocom;
		state->flags = DMSG_STATE_DYNAMIC;
		state->msgid = (uint64_t)(uintptr_t)state;
		state->txcmd = cmd & ~(DMSGF_CREATE | DMSGF_DELETE);
		state->rxcmd = DMSGF_REPLY;
		state->icmd = state->txcmd & DMSGF_BASECMDMASK;
		state->func = func;
		state->any.any = data;

		state->flags |= DMSG_STATE_SUBINSERTED |
				DMSG_STATE_RBINSERTED;
		state->flags |= pstate->flags & DMSG_STATE_DYING;
		if (TAILQ_EMPTY(&pstate->subq))
			dmsg_state_hold(pstate);
		RB_INSERT(dmsg_state_tree, &iocom->statewr_tree, state);
		TAILQ_INSERT_TAIL(&pstate->subq, state, entry);
		dmsg_state_hold(state);
		dmsg_state_hold(state);
		dmsg_state_hold(state);
	} else {

		pstate = state->parent;
		dmsg_state_hold(state);
	}

	hbytes = (cmd & DMSGF_SIZE) * DMSG_ALIGN;
	assert((size_t)hbytes >= sizeof(struct dmsg_hdr));
	msg = malloc(offsetof(struct dmsg_msg, any.head) + hbytes);
	bzero(msg, offsetof(struct dmsg_msg, any.head));

	if (msg->aux_size != aux_size) {
		if (msg->aux_data) {
			free(msg->aux_data);
			msg->aux_data = NULL;
			msg->aux_size = 0;
		}
		if (aux_size) {
			msg->aux_data = malloc(aligned_size);
			msg->aux_size = aux_size;
			if (aux_size != aligned_size) {
				bzero(msg->aux_data + aux_size,
				      aligned_size - aux_size);
			}
		}
	}

	if (state->flags & DMSG_STATE_OPPOSITE)
		cmd |= DMSGF_REVTRANS;
	if (pstate->flags & DMSG_STATE_OPPOSITE)
		cmd |= DMSGF_REVCIRC;

	bzero(&msg->any.head, hbytes);
	msg->hdr_size = hbytes;
	msg->any.head.magic = DMSG_HDR_MAGIC;
	msg->any.head.cmd = cmd;
	msg->any.head.aux_descr = 0;
	msg->any.head.aux_crc = 0;
	msg->any.head.msgid = state->msgid;
	msg->any.head.circuit = pstate->msgid;
	msg->state = state;

	return (msg);
}

static
void
dmsg_msg_free_locked(dmsg_msg_t *msg)
{
	dmsg_state_t *state;

	if ((state = msg->state) != NULL) {
		dmsg_state_drop(state);
		msg->state = NULL;
	}
	if (msg->aux_data) {
		free(msg->aux_data);
		msg->aux_data = NULL;
	}
	msg->aux_size = 0;
	free (msg);
}

void
dmsg_msg_free(dmsg_msg_t *msg)
{
	dmsg_iocom_t *iocom = msg->state->iocom;

	pthread_mutex_lock(&iocom->mtx);
	dmsg_msg_free_locked(msg);
	pthread_mutex_unlock(&iocom->mtx);
}

void
dmsg_iocom_core(dmsg_iocom_t *iocom)
{
	struct pollfd fds[3];
	char dummybuf[256];
	dmsg_msg_t *msg;
	int timeout;
	int count;
	int wi;
	int si;
	int ai;

	while ((iocom->flags & DMSG_IOCOMF_EOF) == 0) {

		dmio_printf(iocom, 5, "iocom %p %08x\n",
			    iocom, iocom->flags);
		if ((iocom->flags & (DMSG_IOCOMF_RWORK |
				     DMSG_IOCOMF_WWORK |
				     DMSG_IOCOMF_PWORK |
				     DMSG_IOCOMF_SWORK |
				     DMSG_IOCOMF_ARWORK |
				     DMSG_IOCOMF_AWWORK)) == 0) {

			timeout = 5000;

			count = 0;
			wi = -1;
			si = -1;
			ai = -1;

			wi = count++;
			fds[wi].fd = iocom->wakeupfds[0];
			fds[wi].events = POLLIN;
			fds[wi].revents = 0;

			if (iocom->flags & (DMSG_IOCOMF_RREQ |
					    DMSG_IOCOMF_WREQ)) {
				si = count++;
				fds[si].fd = iocom->sock_fd;
				fds[si].events = 0;
				fds[si].revents = 0;

				if (iocom->flags & DMSG_IOCOMF_RREQ)
					fds[si].events |= POLLIN;
				if (iocom->flags & DMSG_IOCOMF_WREQ)
					fds[si].events |= POLLOUT;
			}

			if (iocom->alt_fd >= 0) {
				ai = count++;
				fds[ai].fd = iocom->alt_fd;
				fds[ai].events = POLLIN;
				fds[ai].revents = 0;
			}
			poll(fds, count, timeout);

			if (wi >= 0 && (fds[wi].revents & POLLIN))
				atomic_set_int(&iocom->flags,
					       DMSG_IOCOMF_PWORK);
			if (si >= 0 && (fds[si].revents & POLLIN))
				atomic_set_int(&iocom->flags,
					       DMSG_IOCOMF_RWORK);
			if (si >= 0 && (fds[si].revents & POLLOUT))
				atomic_set_int(&iocom->flags,
					       DMSG_IOCOMF_WWORK);
			if (wi >= 0 && (fds[wi].revents & POLLOUT))
				atomic_set_int(&iocom->flags,
					       DMSG_IOCOMF_WWORK);
			if (ai >= 0 && (fds[ai].revents & POLLIN))
				atomic_set_int(&iocom->flags,
					       DMSG_IOCOMF_ARWORK);
		} else {

			atomic_set_int(&iocom->flags, DMSG_IOCOMF_PWORK);
		}

		if (iocom->flags & DMSG_IOCOMF_SWORK) {
			atomic_clear_int(&iocom->flags, DMSG_IOCOMF_SWORK);
			iocom->signal_callback(iocom);
		}

		if (iocom->flags & DMSG_IOCOMF_PWORK) {
			atomic_clear_int(&iocom->flags, DMSG_IOCOMF_PWORK);
			read(iocom->wakeupfds[0], dummybuf, sizeof(dummybuf));
			atomic_set_int(&iocom->flags, DMSG_IOCOMF_RWORK);
			atomic_set_int(&iocom->flags, DMSG_IOCOMF_WWORK);
		}

		if (iocom->flags & DMSG_IOCOMF_WWORK)
			dmsg_iocom_flush1(iocom);

		if (iocom->flags & DMSG_IOCOMF_RWORK) {
			while ((iocom->flags & DMSG_IOCOMF_EOF) == 0 &&
			       (msg = dmsg_ioq_read(iocom)) != NULL) {
				dmio_printf(iocom, 4, "receive %s\n",
					    dmsg_msg_str(msg));
				iocom->rcvmsg_callback(msg);
				pthread_mutex_lock(&iocom->mtx);
				dmsg_state_cleanuprx(iocom, msg);
				pthread_mutex_unlock(&iocom->mtx);
			}
		}

		if (iocom->flags & DMSG_IOCOMF_ARWORK) {
			atomic_clear_int(&iocom->flags, DMSG_IOCOMF_ARWORK);
			iocom->altmsg_callback(iocom);
		}
	}
}

static
size_t
dmsg_ioq_makeroom(dmsg_ioq_t *ioq, size_t needed)
{
	size_t bytes;
	size_t nmax;

	bytes = ioq->fifo_cdx - ioq->fifo_beg;
	nmax = sizeof(ioq->buf) - ioq->fifo_end;
	if (bytes + nmax / 2 < needed) {
		if (bytes) {
			bcopy(ioq->buf + ioq->fifo_beg,
			      ioq->buf,
			      bytes);
		}
		ioq->fifo_cdx -= ioq->fifo_beg;
		ioq->fifo_beg = 0;
		if (ioq->fifo_cdn < ioq->fifo_end) {
			bcopy(ioq->buf + ioq->fifo_cdn,
			      ioq->buf + ioq->fifo_cdx,
			      ioq->fifo_end - ioq->fifo_cdn);
		}
		ioq->fifo_end -= ioq->fifo_cdn - ioq->fifo_cdx;
		ioq->fifo_cdn = ioq->fifo_cdx;
		nmax = sizeof(ioq->buf) - ioq->fifo_end;
	}
	return(nmax);
}

dmsg_msg_t *
dmsg_ioq_read(dmsg_iocom_t *iocom)
{
	dmsg_ioq_t *ioq = &iocom->ioq_rx;
	dmsg_msg_t *msg;
	dmsg_hdr_t *head;
	ssize_t n;
	size_t bytes;
	size_t nmax;
	uint32_t aux_size;
	uint32_t xcrc32;
	int error;

again:

	if ((msg = TAILQ_FIRST(&ioq->msgq)) != NULL) {
		TAILQ_REMOVE(&ioq->msgq, msg, qentry);

		if (msg->state == &iocom->state0) {
			atomic_set_int(&iocom->flags, DMSG_IOCOMF_EOF);
			dmio_printf(iocom, 1,
				    "EOF ON SOCKET %d\n",
				    iocom->sock_fd);
		}
		return (msg);
	}
	atomic_clear_int(&iocom->flags, DMSG_IOCOMF_RREQ | DMSG_IOCOMF_RWORK);

	if (ioq->error)
		goto skip;

	nmax = sizeof(ioq->buf) - ioq->fifo_end;
	bytes = ioq->fifo_cdx - ioq->fifo_beg;
	msg = ioq->msg;

	switch(ioq->state) {
	case DMSG_MSGQ_STATE_HEADER1:

		nmax = dmsg_ioq_makeroom(ioq, sizeof(msg->any.head));
		if (bytes < sizeof(msg->any.head)) {
			n = read(iocom->sock_fd,
				 ioq->buf + ioq->fifo_end,
				 nmax);
			if (n <= 0) {
				if (n == 0) {
					ioq->error = DMSG_IOQ_ERROR_EOF;
					break;
				}
				if (errno != EINTR &&
				    errno != EINPROGRESS &&
				    errno != EAGAIN) {
					ioq->error = DMSG_IOQ_ERROR_SOCK;
					break;
				}
				n = 0;

			}
			ioq->fifo_end += (size_t)n;
			nmax -= (size_t)n;
		}

		if (iocom->flags & DMSG_IOCOMF_CRYPTED) {
			dmsg_crypto_decrypt(iocom, ioq);
		} else {
			ioq->fifo_cdx = ioq->fifo_end;
			ioq->fifo_cdn = ioq->fifo_end;
		}
		bytes = ioq->fifo_cdx - ioq->fifo_beg;

		assert(msg == NULL);
		if (bytes < sizeof(msg->any.head))
			break;

		head = (void *)(ioq->buf + ioq->fifo_beg);
		if (head->magic != DMSG_HDR_MAGIC &&
		    head->magic != DMSG_HDR_MAGIC_REV) {
			dmio_printf(iocom, 1,
				    "%s: head->magic is bad %02x\n",
				    iocom->label, head->magic);
			if (iocom->flags & DMSG_IOCOMF_CRYPTED)
				dmio_printf(iocom, 1, "%s\n",
					    "(on encrypted link)");
			ioq->error = DMSG_IOQ_ERROR_SYNC;
			break;
		}

		if (head->magic == DMSG_HDR_MAGIC_REV) {
			ioq->hbytes = (bswap32(head->cmd) & DMSGF_SIZE) *
				      DMSG_ALIGN;
			aux_size = bswap32(head->aux_bytes);
		} else {
			ioq->hbytes = (head->cmd & DMSGF_SIZE) *
				      DMSG_ALIGN;
			aux_size = head->aux_bytes;
		}
		ioq->abytes = DMSG_DOALIGN(aux_size);
		ioq->unaligned_aux_size = aux_size;
		if (ioq->hbytes < sizeof(msg->any.head) ||
		    ioq->hbytes > sizeof(msg->any) ||
		    ioq->abytes > DMSG_AUX_MAX) {
			ioq->error = DMSG_IOQ_ERROR_FIELD;
			break;
		}

		msg = dmsg_msg_alloc(&iocom->state0, aux_size,
				     ioq->hbytes / DMSG_ALIGN,
				     NULL, NULL);
		ioq->msg = msg;

		nmax = dmsg_ioq_makeroom(ioq, ioq->hbytes);
		ioq->state = DMSG_MSGQ_STATE_HEADER2;

	case DMSG_MSGQ_STATE_HEADER2:

		assert(msg != NULL);
		if (bytes < ioq->hbytes) {
			assert(nmax > 0);
			n = read(iocom->sock_fd,
				 ioq->buf + ioq->fifo_end,
				 nmax);
			if (n <= 0) {
				if (n == 0) {
					ioq->error = DMSG_IOQ_ERROR_EOF;
					break;
				}
				if (errno != EINTR &&
				    errno != EINPROGRESS &&
				    errno != EAGAIN) {
					ioq->error = DMSG_IOQ_ERROR_SOCK;
					break;
				}
				n = 0;

			}
			ioq->fifo_end += (size_t)n;
			nmax -= (size_t)n;
		}

		if (iocom->flags & DMSG_IOCOMF_CRYPTED) {
			dmsg_crypto_decrypt(iocom, ioq);
		} else {
			ioq->fifo_cdx = ioq->fifo_end;
			ioq->fifo_cdn = ioq->fifo_end;
		}
		bytes = ioq->fifo_cdx - ioq->fifo_beg;

		if (bytes < ioq->hbytes) {
			msg = NULL;
			break;
		}

		head = (void *)(ioq->buf + ioq->fifo_beg);

		if (head->magic == DMSG_HDR_MAGIC_REV)
			xcrc32 = bswap32(head->hdr_crc);
		else
			xcrc32 = head->hdr_crc;
		head->hdr_crc = 0;
		if (dmsg_icrc32(head, ioq->hbytes) != xcrc32) {
			ioq->error = DMSG_IOQ_ERROR_XCRC;
			dmio_printf(iocom, 1, "BAD-XCRC(%08x,%08x) %s\n",
				    xcrc32, dmsg_icrc32(head, ioq->hbytes),
				    dmsg_msg_str(msg));
			assert(0);
			break;
		}
		head->hdr_crc = xcrc32;

		if (head->magic == DMSG_HDR_MAGIC_REV) {
			dmsg_bswap_head(head);
		}

		bcopy(head, &msg->any, ioq->hbytes);

		if (ioq->abytes == 0) {
			ioq->fifo_beg += ioq->hbytes;
			break;
		}

		ioq->fifo_beg += ioq->hbytes;
		bytes -= ioq->hbytes;
		ioq->state = DMSG_MSGQ_STATE_AUXDATA1;

	case DMSG_MSGQ_STATE_AUXDATA1:

		if (bytes >= ioq->abytes) {
			bcopy(ioq->buf + ioq->fifo_beg, msg->aux_data,
			      ioq->abytes);
			msg->aux_size = ioq->abytes;
			ioq->fifo_beg += ioq->abytes;
			assert(ioq->fifo_beg <= ioq->fifo_cdx);
			assert(ioq->fifo_cdx <= ioq->fifo_cdn);
			bytes -= ioq->abytes;
		} else if (bytes) {
			bcopy(ioq->buf + ioq->fifo_beg, msg->aux_data,
			      bytes);
			msg->aux_size = bytes;
			ioq->fifo_beg += bytes;
			if (ioq->fifo_cdx < ioq->fifo_beg)
				ioq->fifo_cdx = ioq->fifo_beg;
			assert(ioq->fifo_beg <= ioq->fifo_cdx);
			assert(ioq->fifo_cdx <= ioq->fifo_cdn);
			bytes = 0;
		} else {
			msg->aux_size = 0;
		}
		ioq->state = DMSG_MSGQ_STATE_AUXDATA2;

	case DMSG_MSGQ_STATE_AUXDATA2:

		assert(msg);
		nmax = dmsg_ioq_makeroom(ioq, ioq->abytes - msg->aux_size);

		if (msg->aux_size < ioq->abytes) {
			assert(nmax > 0);
			assert(bytes == 0);
			n = read(iocom->sock_fd,
				 ioq->buf + ioq->fifo_end,
				 nmax);
			if (n <= 0) {
				if (n == 0) {
					ioq->error = DMSG_IOQ_ERROR_EOF;
					break;
				}
				if (errno != EINTR &&
				    errno != EINPROGRESS &&
				    errno != EAGAIN) {
					ioq->error = DMSG_IOQ_ERROR_SOCK;
					break;
				}
				n = 0;

			}
			ioq->fifo_end += (size_t)n;
			nmax -= (size_t)n;
		}

		if (iocom->flags & DMSG_IOCOMF_CRYPTED) {
			dmsg_crypto_decrypt(iocom, ioq);
		} else {
			ioq->fifo_cdx = ioq->fifo_end;
			ioq->fifo_cdn = ioq->fifo_end;
		}
		bytes = ioq->fifo_cdx - ioq->fifo_beg;

		if (bytes > ioq->abytes - msg->aux_size)
			bytes = ioq->abytes - msg->aux_size;

		if (bytes) {
			bcopy(ioq->buf + ioq->fifo_beg,
			      msg->aux_data + msg->aux_size,
			      bytes);
			msg->aux_size += bytes;
			ioq->fifo_beg += bytes;
		}

		if (msg->aux_size < ioq->abytes) {
			msg = NULL;
			break;
		}
		assert(msg->aux_size == ioq->abytes);
		msg->aux_size = ioq->unaligned_aux_size;

		xcrc32 = dmsg_icrc32(msg->aux_data, ioq->abytes);
		if (xcrc32 != msg->any.head.aux_crc) {
			ioq->error = DMSG_IOQ_ERROR_ACRC;
			dmio_printf(iocom, 1,
				    "iocom: ACRC error %08x vs %08x "
				    "msgid %016jx msgcmd %08x auxsize %d\n",
				    xcrc32,
				    msg->any.head.aux_crc,
				    (intmax_t)msg->any.head.msgid,
				    msg->any.head.cmd,
				    msg->any.head.aux_bytes);
			break;
		}
		break;
	case DMSG_MSGQ_STATE_ERROR:

		assert(msg == NULL);
		break;
	default:

		assert(0);
		break;
	}

	if (msg && ioq->error == 0) {
		if ((msg->any.head.salt & 255) != (ioq->seq & 255)) {
			ioq->error = DMSG_IOQ_ERROR_MSGSEQ;
		} else {
			++ioq->seq;
		}
	}

	if (ioq->error) {
skip:

		dmio_printf(iocom, 1, "IOQ ERROR %d\n", ioq->error);
		assert(ioq->msg == msg);
		if (msg) {
			dmsg_msg_free(msg);
			ioq->msg = NULL;
			msg = NULL;
		}

		ioq->state = DMSG_MSGQ_STATE_ERROR;

		pthread_mutex_lock(&iocom->mtx);
		dmsg_iocom_drain(iocom);
		dmsg_simulate_failure(&iocom->state0, 0, ioq->error);
		pthread_mutex_unlock(&iocom->mtx);
		if (TAILQ_FIRST(&ioq->msgq))
			goto again;

#if 0

		if (msg)
			atomic_set_int(&iocom->flags, DMSG_IOCOMF_RWORK);
#endif
	} else if (msg == NULL) {

		atomic_set_int(&iocom->flags, DMSG_IOCOMF_RREQ);
	} else {

		if (ioq->fifo_beg == ioq->fifo_cdx &&
		    ioq->fifo_cdn == ioq->fifo_end) {
			atomic_set_int(&iocom->flags, DMSG_IOCOMF_RREQ);
			ioq->fifo_cdx = 0;
			ioq->fifo_cdn = 0;
			ioq->fifo_beg = 0;
			ioq->fifo_end = 0;
		} else {
			atomic_set_int(&iocom->flags, DMSG_IOCOMF_RWORK);
		}
		ioq->state = DMSG_MSGQ_STATE_HEADER1;
		ioq->msg = NULL;

		dmio_printf(iocom, 5,
			    "rxmsg cmd=%08x circ=%016jx\n",
			    msg->any.head.cmd,
			    (intmax_t)msg->any.head.circuit);

		error = dmsg_state_msgrx(msg, 0);

		if (error) {

			if (error == DMSG_IOQ_ERROR_EALREADY) {
				dmsg_msg_free(msg);
				goto again;
			}

			ioq->error = error;
			goto skip;
		}

	}
	return (msg);
}

void
dmsg_iocom_flush1(dmsg_iocom_t *iocom)
{
	dmsg_ioq_t *ioq = &iocom->ioq_tx;
	dmsg_msg_t *msg;
	uint32_t xcrc32;
	size_t hbytes;
	size_t abytes;
	dmsg_msg_queue_t tmpq;

	atomic_clear_int(&iocom->flags, DMSG_IOCOMF_WREQ | DMSG_IOCOMF_WWORK);
	TAILQ_INIT(&tmpq);
	pthread_mutex_lock(&iocom->mtx);
	while ((msg = TAILQ_FIRST(&iocom->txmsgq)) != NULL) {
		TAILQ_REMOVE(&iocom->txmsgq, msg, qentry);
		TAILQ_INSERT_TAIL(&tmpq, msg, qentry);
	}
	pthread_mutex_unlock(&iocom->mtx);

	while ((msg = TAILQ_FIRST(&tmpq)) != NULL) {

		TAILQ_REMOVE(&tmpq, msg, qentry);
		if (ioq->error) {
			TAILQ_INSERT_TAIL(&ioq->msgq, msg, qentry);
			++ioq->msgcount;
			continue;
		}

		msg->any.head.magic = DMSG_HDR_MAGIC;
		msg->any.head.salt = (random() << 8) | (ioq->seq & 255);
		++ioq->seq;
		if ((ioq->seq & 32767) == 0) {
			pthread_mutex_lock(&iocom->mtx);
			srandom(time(0));
			pthread_mutex_unlock(&iocom->mtx);
		}

		if (msg->aux_size && msg->any.head.aux_crc == 0) {
			abytes = DMSG_DOALIGN(msg->aux_size);
			xcrc32 = dmsg_icrc32(msg->aux_data, abytes);
			msg->any.head.aux_crc = xcrc32;
		}
		msg->any.head.aux_bytes = msg->aux_size;

		hbytes = (msg->any.head.cmd & DMSGF_SIZE) *
			 DMSG_ALIGN;
		msg->any.head.hdr_crc = 0;
		msg->any.head.hdr_crc = dmsg_icrc32(&msg->any.head, hbytes);

		TAILQ_INSERT_TAIL(&ioq->msgq, msg, qentry);
		++ioq->msgcount;
	}
	dmsg_iocom_flush2(iocom);
}

void
dmsg_iocom_flush2(dmsg_iocom_t *iocom)
{
	dmsg_ioq_t *ioq = &iocom->ioq_tx;
	dmsg_msg_t *msg;
	ssize_t n;
	struct iovec iov[DMSG_IOQ_MAXIOVEC];
	size_t nact;
	size_t hbytes;
	size_t abytes;
	size_t hoff;
	size_t aoff;
	int iovcnt;
	int save_errno;

	if (ioq->error) {
		dmsg_iocom_drain(iocom);
		return;
	}

	iovcnt = 0;
	nact = 0;
	hoff = ioq->hbytes;
	aoff = ioq->abytes;

	TAILQ_FOREACH(msg, &ioq->msgq, qentry) {
		hbytes = (msg->any.head.cmd & DMSGF_SIZE) *
			 DMSG_ALIGN;
		abytes = DMSG_DOALIGN(msg->aux_size);
		assert(hoff <= hbytes && aoff <= abytes);

		if (hoff < hbytes) {
			size_t maxlen = hbytes - hoff;
			if (maxlen > sizeof(ioq->buf) / 2)
				maxlen = sizeof(ioq->buf) / 2;
			iov[iovcnt].iov_base = (char *)&msg->any.head + hoff;
			iov[iovcnt].iov_len = maxlen;
			nact += maxlen;
			++iovcnt;
			if (iovcnt == DMSG_IOQ_MAXIOVEC ||
			    maxlen != hbytes - hoff) {
				break;
			}
		}
		if (aoff < abytes) {
			size_t maxlen = abytes - aoff;
			if (maxlen > sizeof(ioq->buf) / 2)
				maxlen = sizeof(ioq->buf) / 2;

			assert(msg->aux_data != NULL);
			iov[iovcnt].iov_base = (char *)msg->aux_data + aoff;
			iov[iovcnt].iov_len = maxlen;
			nact += maxlen;
			++iovcnt;
			if (iovcnt == DMSG_IOQ_MAXIOVEC ||
			    maxlen != abytes - aoff) {
				break;
			}
		}
		hoff = 0;
		aoff = 0;
	}

	if (iovcnt == 0 && ioq->fifo_beg == ioq->fifo_cdx)
		return;

	if (iocom->flags & DMSG_IOCOMF_CRYPTED) {

		if (ioq->fifo_beg > sizeof(ioq->buf) / 2 &&
		    sizeof(ioq->buf) - ioq->fifo_end < DMSG_ALIGN * 2) {
			bcopy(ioq->buf + ioq->fifo_beg, ioq->buf,
			      ioq->fifo_end - ioq->fifo_beg);
			ioq->fifo_cdx -= ioq->fifo_beg;
			ioq->fifo_cdn -= ioq->fifo_beg;
			ioq->fifo_end -= ioq->fifo_beg;
			ioq->fifo_beg = 0;
		}

		iovcnt = dmsg_crypto_encrypt(iocom, ioq, iov, iovcnt, &nact);
		n = writev(iocom->sock_fd, iov, iovcnt);
		save_errno = errno;
		if (n > 0) {
			ioq->fifo_beg += n;
			if (ioq->fifo_beg == ioq->fifo_end) {
				ioq->fifo_beg = 0;
				ioq->fifo_cdn = 0;
				ioq->fifo_cdx = 0;
				ioq->fifo_end = 0;
			}
		}

	} else {

		n = writev(iocom->sock_fd, iov, iovcnt);
		save_errno = errno;
		if (n > 0)
			nact = n;
		else
			nact = 0;
	}

	while ((msg = TAILQ_FIRST(&ioq->msgq)) != NULL) {
		hbytes = (msg->any.head.cmd & DMSGF_SIZE) *
			 DMSG_ALIGN;
		abytes = DMSG_DOALIGN(msg->aux_size);

		if ((size_t)nact < hbytes - ioq->hbytes) {
			ioq->hbytes += nact;
			nact = 0;
			break;
		}
		nact -= hbytes - ioq->hbytes;
		ioq->hbytes = hbytes;
		if ((size_t)nact < abytes - ioq->abytes) {
			ioq->abytes += nact;
			nact = 0;
			break;
		}
		nact -= abytes - ioq->abytes;

		dmio_printf(iocom, 5,
			    "txmsg cmd=%08x circ=%016jx\n",
			    msg->any.head.cmd,
			    (intmax_t)msg->any.head.circuit);

#ifdef DMSG_BLOCK_DEBUG
		uint32_t tcmd;

		if (msg->any.head.cmd & (DMSGF_CREATE | DMSGF_DELETE)) {
			if ((msg->state->flags & DMSG_STATE_ROOT) == 0) {
				tcmd = (msg->state->icmd & DMSGF_BASECMDMASK) |
					    (msg->any.head.cmd & (DMSGF_CREATE |
								  DMSGF_DELETE |
								  DMSGF_REPLY));
			} else {
				tcmd = 0;
			}
		} else {
			tcmd = msg->any.head.cmd & DMSGF_CMDSWMASK;
		}

		switch (tcmd) {
		case DMSG_BLK_READ | DMSGF_CREATE | DMSGF_DELETE:
		case DMSG_BLK_WRITE | DMSGF_CREATE | DMSGF_DELETE:
			dmio_printf(iocom, 4,
				    "write BIO %-3d %016jx %d@%016jx\n",
				    biocount, msg->any.head.msgid,
				    msg->any.blk_read.bytes,
				    msg->any.blk_read.offset);
			break;
		case DMSG_BLK_READ | DMSGF_CREATE | DMSGF_DELETE | DMSGF_REPLY:
		case DMSG_BLK_WRITE | DMSGF_CREATE | DMSGF_DELETE | DMSGF_REPLY:
			dmio_printf(iocom, 4,
				    "wretr BIO %-3d %016jx %d@%016jx\n",
				    biocount, msg->any.head.msgid,
				    msg->any.blk_read.bytes,
				    msg->any.blk_read.offset);
			break;
		default:
			break;
		}
#endif

		TAILQ_REMOVE(&ioq->msgq, msg, qentry);
		--ioq->msgcount;
		ioq->hbytes = 0;
		ioq->abytes = 0;
		dmsg_msg_free(msg);
	}
	assert(nact == 0);

	if (n < 0) {
		if (save_errno != EINTR &&
		    save_errno != EINPROGRESS &&
		    save_errno != EAGAIN) {

			ioq->error = DMSG_IOQ_ERROR_SOCK;
			dmsg_iocom_drain(iocom);
		} else {

			atomic_set_int(&iocom->flags, DMSG_IOCOMF_WREQ);
		}
	} else if (TAILQ_FIRST(&ioq->msgq) ||
		   TAILQ_FIRST(&iocom->txmsgq) ||
		   ioq->fifo_beg != ioq->fifo_cdx) {

		atomic_set_int(&iocom->flags, DMSG_IOCOMF_WWORK);
	}

	if (ioq->error) {
		dmsg_iocom_drain(iocom);
	}
}

void
dmsg_iocom_drain(dmsg_iocom_t *iocom)
{
	dmsg_ioq_t *ioq = &iocom->ioq_tx;
	dmsg_msg_t *msg;

	atomic_clear_int(&iocom->flags, DMSG_IOCOMF_WREQ | DMSG_IOCOMF_WWORK);
	ioq->hbytes = 0;
	ioq->abytes = 0;

	while ((msg = TAILQ_FIRST(&ioq->msgq)) != NULL) {
		TAILQ_REMOVE(&ioq->msgq, msg, qentry);
		--ioq->msgcount;
		dmsg_msg_free(msg);
	}
}

void
dmsg_msg_write(dmsg_msg_t *msg)
{
	dmsg_iocom_t *iocom = msg->state->iocom;
	dmsg_state_t *state;
	char dummy;

	pthread_mutex_lock(&iocom->mtx);
	state = msg->state;

	dmio_printf(iocom, 5,
		    "msgtx: cmd=%08x msgid=%016jx "
		    "state %p(%08x) error=%d\n",
		    msg->any.head.cmd, msg->any.head.msgid,
		    state, (state ? state->icmd : 0),
		    msg->any.head.error);

#if 0

	if ((state->parent->txcmd & DMSGF_DELETE) ||
	    (state->parent->rxcmd & DMSGF_DELETE)) {
		dmio_printf(iocom, 4, "dmsg_msg_write: EARLY TERMINATION\n");
		dmsg_simulate_failure(state, DMSG_ERR_LOSTLINK);
		dmsg_state_cleanuptx(iocom, msg);
		dmsg_msg_free(msg);
		pthread_mutex_unlock(&iocom->mtx);
		return;
	}
#endif

	if ((state->flags & DMSG_STATE_ROOT) == 0) {

		if ((msg->any.head.cmd & (DMSGF_CREATE | DMSGF_REPLY)) ==
		    DMSGF_CREATE) {
			state->txcmd = msg->any.head.cmd & ~DMSGF_DELETE;
			state->icmd = state->txcmd & DMSGF_BASECMDMASK;
			state->flags &= ~DMSG_STATE_NEW;
		}
		msg->any.head.msgid = state->msgid;

		if (msg->any.head.cmd & DMSGF_CREATE) {
			state->txcmd = msg->any.head.cmd & ~DMSGF_DELETE;
		}
	}

	if (state && (state->txcmd & DMSGF_DELETE)) {
		dmio_printf(iocom, 4,
			    "dmsg_msg_write: drop msg %08x to dead "
			    "circuit state=%p\n",
			    msg->any.head.cmd, state);
		dmsg_msg_free(msg);
		return;
	}

	if (state->flags & DMSG_STATE_DYING) {
#if 0
	if ((state->parent->txcmd & DMSGF_DELETE) ||
	    (state->parent->flags & DMSG_STATE_DYING) ||
	    (state->flags & DMSG_STATE_DYING)) {
#endif

		dmio_printf(iocom, 4,
			    "dmsg_msg_write: Write to dying circuit "
			    "ptxcmd=%08x prxcmd=%08x flags=%08x\n",
			    state->parent->rxcmd,
			    state->parent->txcmd,
			    state->parent->flags);
		dmsg_state_hold(state);
		dmsg_state_cleanuptx(iocom, msg);
		if ((state->flags & DMSG_STATE_ABORTING) == 0) {
			dmsg_simulate_failure(state, 1, DMSG_ERR_LOSTLINK);
		}
		dmsg_state_drop(state);
		dmsg_msg_free(msg);
	} else {

		dmio_printf(iocom, 5,
			    "dmsg_msg_write: commit msg state=%p to txkmsgq\n",
			    state);
		dmsg_state_cleanuptx(iocom, msg);
		TAILQ_INSERT_TAIL(&iocom->txmsgq, msg, qentry);
		dummy = 0;
		write(iocom->wakeupfds[1], &dummy, 1);
	}
	pthread_mutex_unlock(&iocom->mtx);
}

static
void
dmsg_subq_delete(dmsg_state_t *state)
{
	dmsg_state_t *pstate;

	if (state->flags & DMSG_STATE_SUBINSERTED) {
		pstate = state->parent;
		assert(pstate);
		if (pstate->scan == state)
			pstate->scan = NULL;
		TAILQ_REMOVE(&pstate->subq, state, entry);
		state->flags &= ~DMSG_STATE_SUBINSERTED;
		state->parent = NULL;
		if (TAILQ_EMPTY(&pstate->subq))
			dmsg_state_drop(pstate);
		pstate = NULL;
		dmsg_state_drop(state);
	} else {
		assert(state->parent == NULL);
	}
}

static
void
dmsg_simulate_failure(dmsg_state_t *state, int meto, int error)
{
	dmsg_state_t *substate;

	dmsg_state_hold(state);
	if (meto)
		dmsg_state_abort(state);

again:
	TAILQ_FOREACH(substate, &state->subq, entry) {
		if (substate->flags & DMSG_STATE_ABORTING)
			continue;
		state->scan = substate;
		dmsg_simulate_failure(substate, 1, error);
		if (state->scan != substate)
			goto again;
	}

	dmsg_state_drop(state);
}

static
void
dmsg_state_abort(dmsg_state_t *state)
{
	dmsg_iocom_t *iocom;
	dmsg_msg_t *msg;

	if (state->flags & DMSG_STATE_ABORTING)
		return;
	state->flags |= DMSG_STATE_ABORTING;
	dmsg_state_dying(state);
	if (state->flags & DMSG_STATE_NEW) {
		dmio_printf(iocom, 4,
			    "dmsg_state_abort(0): state %p rxcmd %08x "
			    "txcmd %08x flags %08x - in NEW state\n",
			    state, state->rxcmd,
			    state->txcmd, state->flags);
		return;
	}

	if ((state->rxcmd & DMSGF_DELETE) == 0) {
		dmio_printf(iocom, 5,
			    "dmsg_state_abort() on state %p\n",
			    state);
		msg = dmsg_msg_alloc_locked(state, 0, DMSG_LNK_ERROR,
					    NULL, NULL);
		if ((state->rxcmd & DMSGF_CREATE) == 0)
			msg->any.head.cmd |= DMSGF_CREATE;
		msg->any.head.cmd |= DMSGF_DELETE |
				     (state->rxcmd & DMSGF_REPLY);
		msg->any.head.cmd ^= (DMSGF_REVTRANS | DMSGF_REVCIRC);
		msg->any.head.error = DMSG_ERR_LOSTLINK;
		msg->any.head.cmd |= DMSGF_ABORT;

		iocom = state->iocom;
		dmsg_state_msgrx(msg, 1);
		pthread_mutex_unlock(&iocom->mtx);
		iocom->rcvmsg_callback(msg);
		pthread_mutex_lock(&iocom->mtx);
		dmsg_state_cleanuprx(iocom, msg);
#if 0
		TAILQ_INSERT_TAIL(&iocom->ioq_rx.msgq, msg, qentry);
		atomic_set_int(&iocom->flags, DMSG_IOCOMF_RWORK);
#endif
	}
}

static
void
dmsg_state_dying(dmsg_state_t *state)
{
	dmsg_state_t *scan;

	if ((state->flags & DMSG_STATE_DYING) == 0) {
		state->flags |= DMSG_STATE_DYING;
		TAILQ_FOREACH(scan, &state->subq, entry)
			dmsg_state_dying(scan);
	}
}

void
dmsg_msg_reply(dmsg_msg_t *msg, uint32_t error)
{
	dmsg_state_t *state = msg->state;
	dmsg_msg_t *nmsg;
	uint32_t cmd;

	cmd = DMSG_LNK_ERROR;

	if ((state->flags & DMSG_STATE_ROOT) == 0) {
		if (state->txcmd & DMSGF_DELETE)
			return;
		if (state->txcmd & DMSGF_REPLY)
			cmd |= DMSGF_REPLY;
		cmd |= DMSGF_DELETE;
	} else {
		if ((msg->any.head.cmd & DMSGF_REPLY) == 0)
			cmd |= DMSGF_REPLY;
	}

	nmsg = dmsg_msg_alloc(state, 0, cmd, NULL, NULL);
	if ((state->flags & DMSG_STATE_ROOT) == 0) {
		if ((state->txcmd & DMSGF_CREATE) == 0)
			nmsg->any.head.cmd |= DMSGF_CREATE;
	}
	nmsg->any.head.error = error;

	dmsg_msg_write(nmsg);
}

void
dmsg_msg_result(dmsg_msg_t *msg, uint32_t error)
{
	dmsg_state_t *state = msg->state;
	dmsg_msg_t *nmsg;
	uint32_t cmd;

	cmd = DMSG_LNK_ERROR;

	if ((state->flags & DMSG_STATE_ROOT) == 0) {
		if (state->txcmd & DMSGF_DELETE)
			return;
		if (state->txcmd & DMSGF_REPLY)
			cmd |= DMSGF_REPLY;

	} else {
		if ((msg->any.head.cmd & DMSGF_REPLY) == 0)
			cmd |= DMSGF_REPLY;
	}
	nmsg = dmsg_msg_alloc(state, 0, cmd, NULL, NULL);
	if ((state->flags & DMSG_STATE_ROOT) == 0) {
		if ((state->txcmd & DMSGF_CREATE) == 0)
			nmsg->any.head.cmd |= DMSGF_CREATE;
	}
	nmsg->any.head.error = error;

	dmsg_msg_write(nmsg);
}

void
dmsg_state_reply(dmsg_state_t *state, uint32_t error)
{
	dmsg_msg_t *nmsg;
	uint32_t cmd = DMSG_LNK_ERROR | DMSGF_DELETE;

	if (state->txcmd & DMSGF_DELETE)
		return;

	if (state->txcmd & DMSGF_REPLY)
		cmd |= DMSGF_REPLY;

	nmsg = dmsg_msg_alloc(state, 0, cmd, NULL, NULL);
	if ((state->flags & DMSG_STATE_ROOT) == 0) {
		if ((state->txcmd & DMSGF_CREATE) == 0)
			nmsg->any.head.cmd |= DMSGF_CREATE;
	}
	nmsg->any.head.error = error;
	dmsg_msg_write(nmsg);
}

void
dmsg_state_result(dmsg_state_t *state, uint32_t error)
{
	dmsg_msg_t *nmsg;
	uint32_t cmd = DMSG_LNK_ERROR;

	if (state->txcmd & DMSGF_DELETE)
		return;

	if (state->txcmd & DMSGF_REPLY)
		cmd |= DMSGF_REPLY;

	nmsg = dmsg_msg_alloc(state, 0, cmd, NULL, NULL);
	if ((state->flags & DMSG_STATE_ROOT) == 0) {
		if ((state->txcmd & DMSGF_CREATE) == 0)
			nmsg->any.head.cmd |= DMSGF_CREATE;
	}
	nmsg->any.head.error = error;
	dmsg_msg_write(nmsg);
}

static int
dmsg_state_msgrx(dmsg_msg_t *msg, int mstate)
{
	dmsg_iocom_t *iocom = msg->state->iocom;
	dmsg_state_t *state;
	dmsg_state_t *pstate;
	dmsg_state_t sdummy;
	int error;

	pthread_mutex_lock(&iocom->mtx);

	if (DMsgDebugOpt) {
		dmio_printf(iocom, 5,
			    "msgrx: cmd=%08x msgid=%016jx "
			    "circuit=%016jx error=%d\n",
			    msg->any.head.cmd,
			    msg->any.head.msgid,
			    msg->any.head.circuit,
			    msg->any.head.error);
	}

	if (mstate) {
		pstate = msg->state->parent;
	} else if (msg->any.head.circuit) {
		sdummy.msgid = msg->any.head.circuit;

		if (msg->any.head.cmd & DMSGF_REVCIRC) {
			pstate = RB_FIND(dmsg_state_tree,
					 &iocom->statewr_tree,
					 &sdummy);
		} else {
			pstate = RB_FIND(dmsg_state_tree,
					 &iocom->staterd_tree,
					 &sdummy);
		}

		if (pstate == NULL) {
			dmio_printf(iocom, 4,
				    "missing parent in stacked trans %s\n",
				    dmsg_msg_str(msg));
			pthread_mutex_unlock(&iocom->mtx);
			error = DMSG_IOQ_ERROR_EALREADY;

			return error;
		}
	} else {
		pstate = &iocom->state0;
	}

	if (mstate) {
		state = msg->state;
	} else {
		sdummy.msgid = msg->any.head.msgid;
		if (msg->any.head.cmd & DMSGF_REVTRANS) {
			state = RB_FIND(dmsg_state_tree,
					&iocom->statewr_tree, &sdummy);
		} else {
			state = RB_FIND(dmsg_state_tree,
					&iocom->staterd_tree, &sdummy);
		}
	}

	if (DMsgDebugOpt) {
		dmio_printf(iocom, 5, "msgrx:\tstate %p(%08x)",
			    state, (state ? state->icmd : 0));
		if (pstate != &iocom->state0) {
			dmio_printf(iocom, 5,
				    " pstate %p(%08x)",
				    pstate, pstate->icmd);
		}
		dmio_printf(iocom, 5, "%s\n", "");
	}

	if (mstate) {

	} else if (state) {

		dmsg_state_drop(msg->state);
		dmsg_state_hold(state);
		msg->state = state;
		assert(pstate == state->parent);
	} else {

		state = pstate;
	}

	switch(msg->any.head.cmd & (DMSGF_CREATE | DMSGF_DELETE |
				    DMSGF_REPLY)) {
	case DMSGF_CREATE:
	case DMSGF_CREATE | DMSGF_DELETE:

		if (state != pstate) {
			dmio_printf(iocom, 2,
				    "duplicate transaction %s\n",
				    dmsg_msg_str(msg));
			error = DMSG_IOQ_ERROR_TRANS;
			assert(0);
			break;
		}

		state = malloc(sizeof(*state));
		bzero(state, sizeof(*state));
		atomic_add_int(&dmsg_state_count, 1);

		TAILQ_INIT(&state->subq);
		dmsg_state_hold(pstate);
		state->parent = pstate;
		state->iocom = iocom;
		state->flags = DMSG_STATE_DYNAMIC |
			       DMSG_STATE_OPPOSITE;
		state->msgid = msg->any.head.msgid;
		state->txcmd = DMSGF_REPLY;
		state->rxcmd = msg->any.head.cmd & ~DMSGF_DELETE;
		state->icmd = state->rxcmd & DMSGF_BASECMDMASK;
		state->flags &= ~DMSG_STATE_NEW;
		msg->state = state;

		RB_INSERT(dmsg_state_tree, &iocom->staterd_tree, state);
		if (TAILQ_EMPTY(&pstate->subq))
			dmsg_state_hold(pstate);
		TAILQ_INSERT_TAIL(&pstate->subq, state, entry);
		state->flags |= DMSG_STATE_SUBINSERTED |
				DMSG_STATE_RBINSERTED;
		dmsg_state_hold(state);
		dmsg_state_hold(state);
		dmsg_state_hold(state);

		if (pstate->relay)
			state->func = dmsg_state_relay;
		error = 0;
		break;
	case DMSGF_DELETE:

		if (state == pstate) {
			if (msg->any.head.cmd & DMSGF_ABORT) {
				error = DMSG_IOQ_ERROR_EALREADY;
			} else {
				dmio_printf(iocom, 2,
					    "missing-state %s\n",
					    dmsg_msg_str(msg));
				error = DMSG_IOQ_ERROR_TRANS;
				assert(0);
			}
			break;
		}

		if ((state->rxcmd & DMSGF_CREATE) == 0) {
			if (msg->any.head.cmd & DMSGF_ABORT) {
				error = DMSG_IOQ_ERROR_EALREADY;
			} else {
				dmio_printf(iocom, 2,
					    "reused-state %s\n",
					    dmsg_msg_str(msg));
				error = DMSG_IOQ_ERROR_TRANS;
				assert(0);
			}
			break;
		}
		error = 0;
		break;
	default:

		if (msg->any.head.cmd & DMSGF_ABORT) {
			if ((state == pstate) ||
			    (state->rxcmd & DMSGF_CREATE) == 0) {
				error = DMSG_IOQ_ERROR_EALREADY;
				break;
			}
		}
		error = 0;
		break;
	case DMSGF_REPLY | DMSGF_CREATE:
	case DMSGF_REPLY | DMSGF_CREATE | DMSGF_DELETE:

		if (state == pstate) {
			dmio_printf(iocom, 2, "no-state(r) %s\n",
				    dmsg_msg_str(msg));
			error = DMSG_IOQ_ERROR_TRANS;
			assert(0);
			break;
		}
		assert(((state->rxcmd ^ msg->any.head.cmd) & DMSGF_REPLY) == 0);
		state->rxcmd = msg->any.head.cmd & ~DMSGF_DELETE;
		error = 0;
		break;
	case DMSGF_REPLY | DMSGF_DELETE:

		if (state == pstate) {
			if (msg->any.head.cmd & DMSGF_ABORT) {
				error = DMSG_IOQ_ERROR_EALREADY;
			} else {
				dmio_printf(iocom, 2,
					    "no-state(r,d) %s\n",
					    dmsg_msg_str(msg));
				error = DMSG_IOQ_ERROR_TRANS;
				assert(0);
			}
			break;
		}

		if ((state->rxcmd & DMSGF_CREATE) == 0) {
			if (msg->any.head.cmd & DMSGF_ABORT) {
				error = DMSG_IOQ_ERROR_EALREADY;
			} else {
				dmio_printf(iocom, 2,
					    "reused-state(r,d) %s\n",
					    dmsg_msg_str(msg));
				error = DMSG_IOQ_ERROR_TRANS;
				assert(0);
			}
			break;
		}
		error = 0;
		break;
	case DMSGF_REPLY:

		if (msg->any.head.cmd & DMSGF_ABORT) {
			if (state == pstate ||
			    (state->rxcmd & DMSGF_CREATE) == 0) {
				error = DMSG_IOQ_ERROR_EALREADY;
				break;
			}
		}
		error = 0;
		break;
	}

	if (msg->any.head.cmd & (DMSGF_CREATE | DMSGF_DELETE)) {
		if ((msg->state->flags & DMSG_STATE_ROOT) == 0) {
			msg->tcmd = (state->icmd & DMSGF_BASECMDMASK) |
				    (msg->any.head.cmd & (DMSGF_CREATE |
							  DMSGF_DELETE |
							  DMSGF_REPLY));
		} else {
			msg->tcmd = 0;
		}
	} else {
		msg->tcmd = msg->any.head.cmd & DMSGF_CMDSWMASK;
	}

#ifdef DMSG_BLOCK_DEBUG
	switch (msg->tcmd) {
	case DMSG_BLK_READ | DMSGF_CREATE | DMSGF_DELETE:
	case DMSG_BLK_WRITE | DMSGF_CREATE | DMSGF_DELETE:
		dmio_printf(iocom, 4,
			    "read  BIO %-3d %016jx %d@%016jx\n",
			    biocount, msg->any.head.msgid,
			    msg->any.blk_read.bytes,
			    msg->any.blk_read.offset);
		break;
	case DMSG_BLK_READ | DMSGF_CREATE | DMSGF_DELETE | DMSGF_REPLY:
	case DMSG_BLK_WRITE | DMSGF_CREATE | DMSGF_DELETE | DMSGF_REPLY:
		dmio_printf(iocom, 4,
			    "rread BIO %-3d %016jx %d@%016jx\n",
			    biocount, msg->any.head.msgid,
			    msg->any.blk_read.bytes,
			    msg->any.blk_read.offset);
		break;
	default:
		break;
	}
#endif

	assert(msg->state->iocom == iocom);
	assert(msg->state == state);

	if (state->flags & DMSG_STATE_ROOT) {

	} else if (msg->any.head.cmd & DMSGF_DELETE) {

		assert((state->rxcmd & DMSGF_DELETE) == 0);
		state->rxcmd |= DMSGF_DELETE;
		if (state->txcmd & DMSGF_DELETE) {
			assert(state->flags & DMSG_STATE_RBINSERTED);
			if (state->rxcmd & DMSGF_REPLY) {
				assert(msg->any.head.cmd & DMSGF_REPLY);
				RB_REMOVE(dmsg_state_tree,
					  &iocom->statewr_tree, state);
			} else {
				assert((msg->any.head.cmd & DMSGF_REPLY) == 0);
				RB_REMOVE(dmsg_state_tree,
					  &iocom->staterd_tree, state);
			}
			state->flags &= ~DMSG_STATE_RBINSERTED;
			dmsg_state_drop(state);
		}
	}

	pthread_mutex_unlock(&iocom->mtx);

	if (DMsgDebugOpt && error)
		dmio_printf(iocom, 1, "msgrx: error %d\n", error);

	return (error);
}

void
dmsg_state_relay(dmsg_msg_t *lmsg)
{
	dmsg_state_t *lpstate;
	dmsg_state_t *rpstate;
	dmsg_state_t *lstate;
	dmsg_state_t *rstate;
	dmsg_msg_t *rmsg;

#ifdef DMSG_BLOCK_DEBUG
	switch (lmsg->tcmd) {
	case DMSG_BLK_OPEN | DMSGF_CREATE:
		dmio_printf(iocom, 4, "%s\n",
			    "relay BIO_OPEN (CREATE)");
		break;
	case DMSG_BLK_OPEN | DMSGF_DELETE:
		dmio_printf(iocom, 4, "%s\n",
			    "relay BIO_OPEN (DELETE)");
		break;
	case DMSG_BLK_READ | DMSGF_CREATE | DMSGF_DELETE:
	case DMSG_BLK_WRITE | DMSGF_CREATE | DMSGF_DELETE:
		atomic_add_int(&biocount, 1);
		dmio_printf(iocom, 4,
			    "relay BIO %-3d %016jx %d@%016jx\n",
			    biocount, lmsg->any.head.msgid,
			    lmsg->any.blk_read.bytes,
			    lmsg->any.blk_read.offset);
		break;
	case DMSG_BLK_READ | DMSGF_CREATE | DMSGF_DELETE | DMSGF_REPLY:
	case DMSG_BLK_WRITE | DMSGF_CREATE | DMSGF_DELETE | DMSGF_REPLY:
		dmio_printf(iocom, 4,
			    "retrn BIO %-3d %016jx %d@%016jx\n",
			    biocount, lmsg->any.head.msgid,
			    lmsg->any.blk_read.bytes,
			    lmsg->any.blk_read.offset);
		atomic_add_int(&biocount, -1);
		break;
	default:
		break;
	}
#endif

	if ((lmsg->any.head.cmd & (DMSGF_CREATE | DMSGF_REPLY)) ==
	    DMSGF_CREATE) {

		lstate = lmsg->state;
		lpstate = lstate->parent;
		rpstate = lpstate->relay;
		assert(lstate->relay == NULL);
		assert(rpstate != NULL);

		rmsg = dmsg_msg_alloc(rpstate, 0,
				      lmsg->any.head.cmd,
				      dmsg_state_relay, NULL);
		rstate = rmsg->state;
		rstate->relay = lstate;
		lstate->relay = rstate;
		dmsg_state_hold(lstate);
		dmsg_state_hold(rstate);
	} else {

		lstate = lmsg->state;
		rstate = lstate->relay;
		assert(rstate != NULL);

		assert((rstate->txcmd & DMSGF_DELETE) == 0);

#if 0
		if (lstate->flags & DMSG_STATE_ABORTING) {
			dmio_printf(iocom, 4,
				    "relay: relay lost link l=%p r=%p\n",
				    lstate, rstate);
			dmsg_simulate_failure(rstate, 0, DMSG_ERR_LOSTLINK);
		}
#endif

		rmsg = dmsg_msg_alloc(rstate, 0,
				      lmsg->any.head.cmd,
				      dmsg_state_relay, NULL);
	}
	if (lmsg->hdr_size > sizeof(lmsg->any.head)) {
		bcopy(&lmsg->any.head + 1, &rmsg->any.head + 1,
		      lmsg->hdr_size - sizeof(lmsg->any.head));
	}
	rmsg->any.head.error = lmsg->any.head.error;
	rmsg->any.head.reserved02 = lmsg->any.head.reserved02;
	rmsg->any.head.link_verifier = lmsg->any.head.link_verifier;
	rmsg->aux_size = lmsg->aux_size;
	rmsg->aux_data = lmsg->aux_data;
	lmsg->aux_data = NULL;

	dmsg_msg_write(rmsg);
}

void
dmsg_state_cleanuprx(dmsg_iocom_t *iocom, dmsg_msg_t *msg)
{
	dmsg_state_t *state;

	assert(msg->state->iocom == iocom);
	state = msg->state;
	if (state->flags & DMSG_STATE_ROOT) {

		dmsg_msg_free(msg);
	} else if ((state->flags & DMSG_STATE_SUBINSERTED) &&
		   (state->rxcmd & DMSGF_DELETE) &&
		   (state->txcmd & DMSGF_DELETE)) {

		dmsg_subq_delete(state);
		if (state->relay) {
			dmsg_state_drop(state->relay);
			state->relay = NULL;
		}
		dmsg_msg_free(msg);
	} else {

		dmsg_msg_free(msg);
	}
}

static void
dmsg_state_cleanuptx(dmsg_iocom_t *iocom, dmsg_msg_t *msg)
{
	dmsg_state_t *state;

	assert(iocom == msg->state->iocom);
	state = msg->state;

	dmsg_state_hold(state);

	if (state->flags & DMSG_STATE_ROOT) {
		;
	} else if (msg->any.head.cmd & DMSGF_DELETE) {

		if ((state->txcmd & DMSGF_DELETE) == 0 &&
		    (state->rxcmd & DMSGF_DELETE)) {
			state->txcmd |= DMSGF_DELETE;
			assert(state->flags & DMSG_STATE_RBINSERTED);
			if (state->txcmd & DMSGF_REPLY) {
				assert(msg->any.head.cmd & DMSGF_REPLY);
				RB_REMOVE(dmsg_state_tree,
					  &iocom->staterd_tree, state);
			} else {
				assert((msg->any.head.cmd & DMSGF_REPLY) == 0);
				RB_REMOVE(dmsg_state_tree,
					  &iocom->statewr_tree, state);
			}
			state->flags &= ~DMSG_STATE_RBINSERTED;
			dmsg_subq_delete(state);

			if (state->relay) {
				dmsg_state_drop(state->relay);
				state->relay = NULL;
			}
			dmsg_state_drop(state);
		} else if ((state->txcmd & DMSGF_DELETE) == 0) {
			state->txcmd |= DMSGF_DELETE;
		}
	}

	if ((state->flags & (DMSG_STATE_ABORTING | DMSG_STATE_DYING)) &&
	    (state->rxcmd & DMSGF_DELETE) == 0) {
		dmio_printf(iocom, 4,
			    "cleanuptx: state=%p "
			    "executing deferred abort\n",
			    state);
		state->flags &= ~DMSG_STATE_ABORTING;
		dmsg_simulate_failure(state, 1, DMSG_ERR_LOSTLINK);
	}

	dmsg_state_drop(state);
}

void
dmsg_state_hold(dmsg_state_t *state)
{
	atomic_add_int(&state->refs, 1);
}

void
dmsg_state_drop(dmsg_state_t *state)
{
	assert(state->refs > 0);
	if (atomic_fetchadd_int(&state->refs, -1) == 1)
		dmsg_state_free(state);
}

static void
dmsg_state_free(dmsg_state_t *state)
{
	atomic_add_int(&dmsg_state_count, -1);
	dmio_printf(state->iocom, 5, "terminate state %p\n", state);
	assert((state->flags & (DMSG_STATE_ROOT |
				DMSG_STATE_SUBINSERTED |
				DMSG_STATE_RBINSERTED)) == 0);
	assert(TAILQ_EMPTY(&state->subq));
	assert(state->refs == 0);
	if (state->any.any != NULL)
		;
	assert(state->any.any == NULL);
	free(state);
}

void
dmsg_bswap_head(dmsg_hdr_t *head)
{
	head->magic	= bswap16(head->magic);
	head->reserved02 = bswap16(head->reserved02);
	head->salt	= bswap32(head->salt);

	head->msgid	= bswap64(head->msgid);
	head->circuit	= bswap64(head->circuit);
	head->link_verifier= bswap64(head->link_verifier);

	head->cmd	= bswap32(head->cmd);
	head->aux_crc	= bswap32(head->aux_crc);
	head->aux_bytes	= bswap32(head->aux_bytes);
	head->error	= bswap32(head->error);
	head->aux_descr = bswap64(head->aux_descr);
	head->reserved38= bswap32(head->reserved38);
	head->hdr_crc	= bswap32(head->hdr_crc);
}
