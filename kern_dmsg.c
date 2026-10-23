#include "hammer2.h"

#include <linux/kthread.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/uio.h>
#include <linux/err.h>
#include <linux/jiffies.h>
#include <linux/wait.h>
#include <linux/net.h>
#include <net/sock.h>

#ifndef UIO_SYSSPACE
#define UIO_SYSSPACE	1
#endif
#ifndef SHUT_RDWR
#define SHUT_RDWR	2
#endif
#ifndef intmax_t
typedef long long intmax_t;
#endif

#define ticks		((int)jiffies)

#define TAILQ_EMPTY(hp)		list_empty(&(hp)->head)

#define DMSG_MSGQ_FIRST(hp)	\
	list_first_entry_or_null(&(hp)->head, kdmsg_msg_t, qentry)
#define DMSG_SUBQ_FIRST(hp)	\
	list_first_entry_or_null(&(hp)->head, kdmsg_state_t, entry)

static inline void
fp_shutdown(struct file *fp, int how)
{
	struct socket *sock;
	int err;

	(void)how;
	if (fp == NULL)
		return;
	sock = sock_from_file(fp);
	if (sock)
		kernel_sock_shutdown(sock, SHUT_RDWR);
	(void)err;
}

static inline int
lksleep(const volatile void *ident, struct lock *lk, int flags,
	const char *wmesg, int timo)
{
	(void)ident;
	(void)flags;
	(void)wmesg;
	lockmgr(lk, LK_RELEASE);
	schedule_timeout_uninterruptible(timo ? timo : 1);
	lockmgr(lk, LK_EXCLUSIVE);
	return 0;
}

static inline int
fp_read(struct file *fp, void *buf, size_t nbytes, ssize_t *res,
	int all, int seg)
{
	loff_t pos = 0;
	size_t done = 0;
	ssize_t n;

	(void)all;
	(void)seg;
	while (done < nbytes) {
		n = kernel_read(fp, (char *)buf + done, nbytes - done, &pos);
		if (n == 0)
			break;
		if (n < 0)
			return ((int)-n);
		done += (size_t)n;
	}
	if (res)
		*res = (ssize_t)done;
	return (done == nbytes) ? 0 : EPIPE;
}

static inline int
fp_write(struct file *fp, void *buf, size_t nbytes, ssize_t *res, int seg)
{
	loff_t pos = 0;
	size_t done = 0;
	ssize_t n;

	(void)seg;
	while (done < nbytes) {
		n = kernel_write(fp, (char *)buf + done, nbytes - done, &pos);
		if (n < 0)
			return ((int)-n);
		if (n == 0)
			break;
		done += (size_t)n;
	}
	if (res)
		*res = (ssize_t)done;
	return (done == nbytes) ? 0 : EPIPE;
}

#define fhold(fp)	get_file(fp)
#define fdrop(fp)	fput(fp)

struct kdmsg_kthread_arg {
	void	(*func)(void *);
	void	*arg;
};

static int
kdmsg_kthread_trampoline(void *data)
{
	struct kdmsg_kthread_arg ka = *(struct kdmsg_kthread_arg *)data;

	hfree(data, M_HAMMER2, sizeof(struct kdmsg_kthread_arg));
	ka.func(ka.arg);
	return (0);
}

#define lwkt_create(_fn, _ar, _tdp, _p4, _p5, _p6, ...)		\
	do {								\
		struct kdmsg_kthread_arg *_ka =				\
		    hmalloc(sizeof(*_ka), M_HAMMER2, M_WAITOK);		\
		struct task_struct *_t;					\
		_ka->func = (_fn);					\
		_ka->arg = (_ar);					\
		_t = kthread_run(kdmsg_kthread_trampoline, _ka,		\
				 __VA_ARGS__);				\
		*(_tdp) = IS_ERR(_t) ? NULL : (thread_t)_t;		\
	} while (0)

#define lwkt_exit()	return

RB_GENERATE(kdmsg_state_tree, kdmsg_state, rbnode, kdmsg_state_cmp);

static int kdmsg_debug = 1;

#define kd_printf(level, ctl, ...)      	\
        if (kdmsg_debug >= (level)) kprintf("kdmsg: " ctl, __VA_ARGS__)

#define kdio_printf(iocom, level, ctl, ...)      \
        if (kdmsg_debug >= (level)) kprintf("kdmsg: " ctl, __VA_ARGS__)

static int kdmsg_msg_receive_handling(kdmsg_msg_t *msg);
static int kdmsg_state_msgrx(kdmsg_msg_t *msg);
static int kdmsg_state_msgtx(kdmsg_msg_t *msg);
static void kdmsg_msg_write_locked(kdmsg_iocom_t *iocom, kdmsg_msg_t *msg);
static void kdmsg_state_cleanuprx(kdmsg_msg_t *msg);
static void kdmsg_state_cleanuptx(kdmsg_msg_t *msg);
static void kdmsg_subq_delete(kdmsg_state_t *state);
static void kdmsg_simulate_failure(kdmsg_state_t *state, int meto, int error);
static void kdmsg_state_abort(kdmsg_state_t *state);
static void kdmsg_state_dying(kdmsg_state_t *state);
static void kdmsg_state_free(kdmsg_state_t *state);
static void kdmsg_drain_msg(kdmsg_msg_t *msg);

#ifdef KDMSG_DEBUG
#define KDMSG_DEBUG_ARGS	, const char *file, int line
#define kdmsg_state_hold(state)	_kdmsg_state_hold(state, __FILE__, __LINE__)
#define kdmsg_state_drop(state)	_kdmsg_state_drop(state, __FILE__, __LINE__)
#else
#define KDMSG_DEBUG 0
#define KDMSG_DEBUG_ARGS
#define kdmsg_state_hold(state)	_kdmsg_state_hold(state)
#define kdmsg_state_drop(state)	_kdmsg_state_drop(state)
#endif
static void _kdmsg_state_hold(kdmsg_state_t *state KDMSG_DEBUG_ARGS);
static void _kdmsg_state_drop(kdmsg_state_t *state KDMSG_DEBUG_ARGS);

static void kdmsg_iocom_thread_rd(void *arg);
static void kdmsg_iocom_thread_wr(void *arg);
static int kdmsg_autorxmsg(kdmsg_msg_t *msg);

void
kdmsg_iocom_init(kdmsg_iocom_t *iocom, void *handle, uint32_t flags,
		 struct malloc_type *mmsg,
		 int (*rcvmsg)(kdmsg_msg_t *msg))
{
	bzero(iocom, sizeof(*iocom));
	iocom->handle = handle;
	iocom->mmsg = mmsg;
	iocom->rcvmsg = rcvmsg;
	iocom->flags = flags;
	lockinit(&iocom->msglk, "h2msg", 0, 0);
	init_waitqueue_head(&iocom->msg_waitq);
	TAILQ_INIT(&iocom->msgq);
	RB_INIT(&iocom->staterd_tree);
	RB_INIT(&iocom->statewr_tree);

	iocom->state0.iocom = iocom;
	iocom->state0.parent = &iocom->state0;
	TAILQ_INIT(&iocom->state0.subq);
}

void
kdmsg_iocom_reconnect(kdmsg_iocom_t *iocom, struct file *fp,
		      const char *subsysname)
{

	lockmgr(&iocom->msglk, LK_EXCLUSIVE);
	atomic_set_int(&iocom->msg_ctl, KDMSG_CLUSTERCTL_KILLRX);
	while (iocom->msgrd_td || iocom->msgwr_td) {
		wake_up(&iocom->msg_waitq);
		lksleep(iocom, &iocom->msglk, 0, "clstrkl", hz);
	}

	if (iocom->msg_fp) {
		fdrop(iocom->msg_fp);
		iocom->msg_fp = NULL;
	}

	iocom->msg_ctl = 0;
	iocom->msg_fp = fp;
	iocom->msg_seq = 0;
	iocom->flags &= ~KDMSG_IOCOMF_EXITNOACC;

	lwkt_create(kdmsg_iocom_thread_rd, iocom, &iocom->msgrd_td,
		    NULL, 0, -1, "%s-msgrd", subsysname);
	lwkt_create(kdmsg_iocom_thread_wr, iocom, &iocom->msgwr_td,
		    NULL, 0, -1, "%s-msgwr", subsysname);
	lockmgr(&iocom->msglk, LK_RELEASE);
}

static int kdmsg_lnk_conn_reply(kdmsg_state_t *state, kdmsg_msg_t *msg);
static int kdmsg_lnk_span_reply(kdmsg_state_t *state, kdmsg_msg_t *msg);

void
kdmsg_iocom_autoinitiate(kdmsg_iocom_t *iocom,
			 void (*auto_callback)(kdmsg_msg_t *msg))
{
	kdmsg_msg_t *msg;

	iocom->auto_callback = auto_callback;

	msg = kdmsg_msg_alloc(&iocom->state0,
			      DMSG_LNK_CONN | DMSGF_CREATE,
			      kdmsg_lnk_conn_reply, NULL);
	iocom->auto_lnk_conn.head = msg->any.head;
	msg->any.lnk_conn = iocom->auto_lnk_conn;
	iocom->conn_state = msg->state;
	kdmsg_state_hold(msg->state);
	kdmsg_msg_write(msg);
	hprintf("DMSGTRACE autoinit queued LNK_CONN cmd=%08x\n",
		msg->any.head.cmd);
}

static
int
kdmsg_lnk_conn_reply(kdmsg_state_t *state, kdmsg_msg_t *msg)
{
	kdmsg_iocom_t *iocom = state->iocom;
	kdmsg_msg_t *rmsg;

	if ((msg->any.head.cmd & DMSGF_CREATE) &&
	    (iocom->flags & KDMSG_IOCOMF_AUTOTXSPAN)) {
		rmsg = kdmsg_msg_alloc(&iocom->state0,
				       DMSG_LNK_SPAN | DMSGF_CREATE,
				       kdmsg_lnk_span_reply, NULL);
		iocom->auto_lnk_span.head = rmsg->any.head;
		rmsg->any.lnk_span = iocom->auto_lnk_span;
		kdmsg_msg_write(rmsg);
	}

	if (iocom->auto_callback)
		iocom->auto_callback(msg);

	if ((state->txcmd & DMSGF_DELETE) == 0 &&
	    (msg->any.head.cmd & DMSGF_DELETE)) {

		if (iocom->conn_state)
			kdmsg_state_drop(iocom->conn_state);
		iocom->conn_state = NULL;
		kdmsg_msg_reply(msg, 0);
	}

	return (0);
}

static
int
kdmsg_lnk_span_reply(kdmsg_state_t *state, kdmsg_msg_t *msg)
{

	if (state->iocom->auto_callback)
		state->iocom->auto_callback(msg);

	if ((state->txcmd & DMSGF_DELETE) == 0 &&
	    (msg->any.head.cmd & DMSGF_DELETE)) {
		kdmsg_msg_reply(msg, 0);
	}
	return (0);
}

void
kdmsg_iocom_uninit(kdmsg_iocom_t *iocom)
{
	kdmsg_state_t *state;
	kdmsg_msg_t *msg;
	int retries;

	lockmgr(&iocom->msglk, LK_EXCLUSIVE);

	atomic_set_int(&iocom->msg_ctl, KDMSG_CLUSTERCTL_KILLRX);
	msg = kdmsg_msg_alloc(&iocom->state0, DMSG_LNK_PING, NULL, NULL);
	kdmsg_msg_write_locked(iocom, msg);

	retries = 10;
	while (iocom->msgrd_td || iocom->msgwr_td) {
		wake_up(&iocom->msg_waitq);
		lksleep(iocom, &iocom->msglk, 0, "clstrkl", hz);
		if (--retries == 0 && iocom->msg_fp) {
			kdio_printf(iocom, 0, "%s\n",
				    "iocom_uninit: "
				    "shitcanning unresponsive pipe");
			fp_shutdown(iocom->msg_fp, SHUT_RDWR);

		}
	}

	if ((state = iocom->freerd_state) != NULL) {
		iocom->freerd_state = NULL;
		kdmsg_state_drop(state);
	}

	if ((state = iocom->freewr_state) != NULL) {
		iocom->freewr_state = NULL;
		kdmsg_state_drop(state);
	}

	if (iocom->msg_fp) {
		fdrop(iocom->msg_fp);
		iocom->msg_fp = NULL;
	}
	lockmgr(&iocom->msglk, LK_RELEASE);
}

static
void
kdmsg_iocom_thread_rd(void *arg)
{
	kdmsg_iocom_t *iocom = arg;
	dmsg_hdr_t hdr;
	kdmsg_msg_t *msg = NULL;
	size_t hbytes;
	size_t abytes;
	int error = 0;

	while ((iocom->msg_ctl & KDMSG_CLUSTERCTL_KILLRX) == 0) {

		error = fp_read(iocom->msg_fp, &hdr, sizeof(hdr),
				NULL, 1, UIO_SYSSPACE);
		hprintf("DMSGTRACE RD hdr err=%d magic=%04x cmd=%08x\n",
			error, error ? 0 : hdr.magic, error ? 0 : hdr.cmd);
		if (error)
			break;
		if (hdr.magic != DMSG_HDR_MAGIC) {
			kdio_printf(iocom, 1, "bad magic: %04x\n", hdr.magic);
			error = EINVAL;
			break;
		}
		hbytes = (hdr.cmd & DMSGF_SIZE) * DMSG_ALIGN;
		if (hbytes < sizeof(hdr) || hbytes > DMSG_HDR_MAX) {
			kdio_printf(iocom, 1, "bad header size %zd\n", hbytes);
			error = EINVAL;
			break;
		}

		msg = kdmsg_msg_alloc(&iocom->state0,
				      hdr.cmd & DMSGF_BASECMDMASK,
				      NULL, NULL);
		msg->any.head = hdr;
		msg->hdr_size = hbytes;
		if (hbytes > sizeof(hdr)) {
			error = fp_read(iocom->msg_fp, &msg->any.head + 1,
					hbytes - sizeof(hdr),
					NULL, 1, UIO_SYSSPACE);
			if (error) {
				kdio_printf(iocom, 1, "%s\n",
					    "short msg received");
				error = EINVAL;
				break;
			}
		}
		msg->aux_size = hdr.aux_bytes;
		if (msg->aux_size > DMSG_AUX_MAX) {
			kdio_printf(iocom, 1,
				    "illegal msg payload size %zd\n",
				    msg->aux_size);
			error = EINVAL;
			break;
		}
		if (msg->aux_size) {
			abytes = DMSG_DOALIGN(msg->aux_size);
			msg->aux_data = hmalloc(abytes, iocom->mmsg, M_WAITOK);
			msg->flags |= KDMSG_FLAG_AUXALLOC;
			error = fp_read(iocom->msg_fp, msg->aux_data,
					abytes, NULL, 1, UIO_SYSSPACE);
			if (error) {
				kdio_printf(iocom, 1, "%s\n",
					    "short msg payload received");
				break;
			}
		}

		error = kdmsg_msg_receive_handling(msg);
		msg = NULL;
	}

#if 0
	kdio_printf(iocom, 1, "read thread terminating error=%d\n", error);
#endif

	lockmgr(&iocom->msglk, LK_EXCLUSIVE);
	if (msg)
		kdmsg_msg_free(msg);

	fp_shutdown(iocom->msg_fp, SHUT_RDWR);
	atomic_set_int(&iocom->msg_ctl, KDMSG_CLUSTERCTL_KILLRX |
					KDMSG_CLUSTERCTL_KILLTX);
	iocom->msgrd_td = NULL;
	lockmgr(&iocom->msglk, LK_RELEASE);
	wake_up(&iocom->msg_waitq);

	wakeup(iocom);
	lwkt_exit();
}

static
void
kdmsg_iocom_thread_wr(void *arg)
{
	kdmsg_iocom_t *iocom = arg;
	kdmsg_msg_t *msg;
	ssize_t res;
	size_t abytes;
	int error = 0;
	int save_ticks;
	int didwarn;

	msg = NULL;
	lockmgr(&iocom->msglk, LK_EXCLUSIVE);

	while ((iocom->msg_ctl & KDMSG_CLUSTERCTL_KILLTX) == 0 && error == 0) {

		if (TAILQ_EMPTY(&iocom->msgq)) {

			atomic_set_int(&iocom->msg_ctl,
				       KDMSG_CLUSTERCTL_SLEEPING);
			lockmgr(&iocom->msglk, LK_RELEASE);
			wait_event_interruptible_timeout(iocom->msg_waitq,
			    !TAILQ_EMPTY(&iocom->msgq) ||
			    (iocom->msg_ctl & KDMSG_CLUSTERCTL_KILLTX), hz);
			lockmgr(&iocom->msglk, LK_EXCLUSIVE);
			atomic_clear_int(&iocom->msg_ctl,
					 KDMSG_CLUSTERCTL_SLEEPING);
		}

		while ((msg = DMSG_MSGQ_FIRST(&iocom->msgq)) != NULL) {

			TAILQ_REMOVE(&iocom->msgq, msg, qentry);

			error = kdmsg_state_msgtx(msg);
			if (error == EALREADY) {
				error = 0;
				kdmsg_msg_free(msg);
				continue;
			}
			if (error) {
				kdmsg_msg_free(msg);
				break;
			}

			lockmgr(&iocom->msglk, LK_RELEASE);
			error = fp_write(iocom->msg_fp, &msg->any,
					 msg->hdr_size, &res, UIO_SYSSPACE);
			hprintf("DMSGTRACE WR cmd=%08x hdr=%zu err=%d res=%zd\n",
				msg->any.head.cmd, msg->hdr_size, error,
				(ssize_t)res);
			if (error || res != msg->hdr_size) {
				if (error == 0)
					error = EINVAL;
				lockmgr(&iocom->msglk, LK_EXCLUSIVE);
				kdmsg_state_cleanuptx(msg);
				break;
			}
			if (msg->aux_size) {
				abytes = DMSG_DOALIGN(msg->aux_size);
				error = fp_write(iocom->msg_fp,
						 msg->aux_data, abytes,
						 &res, UIO_SYSSPACE);
				if (error || res != abytes) {
					if (error == 0)
						error = EINVAL;
					lockmgr(&iocom->msglk, LK_EXCLUSIVE);
					kdmsg_state_cleanuptx(msg);
					break;
				}
			}
			lockmgr(&iocom->msglk, LK_EXCLUSIVE);
			kdmsg_state_cleanuptx(msg);
		}
	}

#if 0
	kdio_printf(iocom, 1, "write thread terminating error=%d\n", error);
#endif

	fp_shutdown(iocom->msg_fp, SHUT_RDWR);
	atomic_set_int(&iocom->msg_ctl, KDMSG_CLUSTERCTL_KILLRX |
					KDMSG_CLUSTERCTL_KILLTX);
	wake_up(&iocom->msg_waitq);

	while (iocom->msgrd_td) {
		wake_up(&iocom->msg_waitq);
		lksleep(iocom, &iocom->msglk, 0, "clstrkt", hz);
	}

	save_ticks = ticks;
	didwarn = 0;
	iocom->flags |= KDMSG_IOCOMF_EXITNOACC;

	while (DMSG_MSGQ_FIRST(&iocom->msgq) ||
	       RB_MIN(kdmsg_state_tree, &iocom->staterd_tree) ||
	       RB_MIN(kdmsg_state_tree, &iocom->statewr_tree) ||
	       iocom->conn_state) {

		kdmsg_drain_msgq(iocom);
		kdmsg_simulate_failure(&iocom->state0, 0, DMSG_ERR_LOSTLINK);

		lksleep(iocom, &iocom->msglk, 0, "clstrtk", hz / 2);

		if ((int)(ticks - save_ticks) > hz*2 && didwarn == 0) {
			didwarn = 1;
			kdio_printf(iocom, 0,
				    "Warning, write thread on %p "
				    "still terminating\n",
				    iocom);
		}
		if ((int)(ticks - save_ticks) > hz*15 && didwarn == 1) {
			didwarn = 2;
			kdio_printf(iocom, 0,
				    "Warning, write thread on %p "
				    "still terminating\n",
				    iocom);
		}
		if ((int)(ticks - save_ticks) > hz*60) {
			kdio_printf(iocom, 0,
				    "Can't terminate: msgq %p "
				    "rd_tree %p wr_tree %p\n",
				    DMSG_MSGQ_FIRST(&iocom->msgq),
				    RB_MIN(kdmsg_state_tree, &iocom->staterd_tree),
				    RB_MIN(kdmsg_state_tree, &iocom->statewr_tree));
			lksleep(iocom, &iocom->msglk, 0, "clstrtk", hz * 10);
		}
	}

	lockmgr(&iocom->msglk, LK_RELEASE);

	KKASSERT(RB_EMPTY(&iocom->staterd_tree));
	KKASSERT(RB_EMPTY(&iocom->statewr_tree));
	KKASSERT(iocom->conn_state == NULL);

	if (iocom->exit_func) {

		iocom->msgwr_td = NULL;
		iocom->exit_func(iocom);
	} else {

		iocom->msgwr_td = NULL;
		wakeup(iocom);
	}
	lwkt_exit();
}

void
kdmsg_drain_msgq(kdmsg_iocom_t *iocom)
{
	kdmsg_msg_t *msg;

	while ((msg = DMSG_MSGQ_FIRST(&iocom->msgq)) != NULL) {
		TAILQ_REMOVE(&iocom->msgq, msg, qentry);
		kdmsg_drain_msg(msg);
	}
}

static void
kdmsg_drain_msg(kdmsg_msg_t *msg)
{
	if (kdmsg_state_msgtx(msg)) {
		kdmsg_msg_free(msg);
	} else {
		if (msg->state) {
			kdmsg_simulate_failure(msg->state,
					       0, DMSG_ERR_LOSTLINK);
		}
		kdmsg_state_cleanuptx(msg);
	}
}

static
int
kdmsg_msg_receive_handling(kdmsg_msg_t *msg)
{
	kdmsg_iocom_t *iocom = msg->state->iocom;
	int error;

	error = kdmsg_state_msgrx(msg);
	if (msg->state->flags & KDMSG_STATE_ABORTING) {
		kdio_printf(iocom, 5,
			    "kdmsg_state_abort(b): state %p rxcmd=%08x "
			    "txcmd=%08x msgrx error %d\n",
			    msg->state, msg->state->rxcmd,
			    msg->state->txcmd, error);
	}
	if (error) {

		if (msg->state->flags & KDMSG_STATE_ABORTING)
			kdio_printf(iocom, 5,
				    "X1 state %p error %d\n",
				    msg->state, error);
		kdmsg_msg_free(msg);
		if (error == EALREADY)
			error = 0;
	} else if (msg->state && msg->state->func) {

		if (msg->state->flags & KDMSG_STATE_ABORTING)
			kdio_printf(iocom, 5,
				    "X2 state %p func %p\n",
				    msg->state, msg->state->func);
		error = msg->state->func(msg->state, msg);
		kdmsg_state_cleanuprx(msg);
	} else if (iocom->flags & KDMSG_IOCOMF_AUTOANY) {
		if (msg->state->flags & KDMSG_STATE_ABORTING)
			kdio_printf(iocom, 5,
				    "X3 state %p\n", msg->state);
		error = kdmsg_autorxmsg(msg);
		kdmsg_state_cleanuprx(msg);
	} else {
		if (msg->state->flags & KDMSG_STATE_ABORTING)
			kdio_printf(iocom, 5,
				    "X4 state %p\n", msg->state);
		error = iocom->rcvmsg(msg);
		kdmsg_state_cleanuprx(msg);
	}
	return error;
}

static
int
kdmsg_state_msgrx(kdmsg_msg_t *msg)
{
	kdmsg_iocom_t *iocom = msg->state->iocom;
	kdmsg_state_t *state;
	kdmsg_state_t *pstate;
	kdmsg_state_t sdummy;
	int error;

	bzero(&sdummy, sizeof(sdummy));

	if ((state = iocom->freerd_state) == NULL) {
		state = hmalloc(sizeof(*state), iocom->mmsg, M_WAITOK | M_ZERO);
		state->flags = KDMSG_STATE_DYNAMIC;
		state->iocom = iocom;
		state->refs = 1;
		TAILQ_INIT(&state->subq);
		iocom->freerd_state = state;
	}
	state = NULL;

	lockmgr(&iocom->msglk, LK_EXCLUSIVE);

again:
	if (msg->state == &iocom->state0) {
		sdummy.msgid = msg->any.head.msgid;
		sdummy.iocom = iocom;
		if (msg->any.head.cmd & DMSGF_REVTRANS) {
			state = RB_FIND(kdmsg_state_tree, &iocom->statewr_tree,
					&sdummy);
		} else {
			state = RB_FIND(kdmsg_state_tree, &iocom->staterd_tree,
					&sdummy);
		}

		if (state == NULL)
			state = &iocom->state0;
		if (state->flags & KDMSG_STATE_INTERLOCK) {
			state->flags |= KDMSG_STATE_SIGNAL;
			lksleep(state, &iocom->msglk, 0, "dmrace", hz);
			goto again;
		}
		kdmsg_state_hold(state);
		kdmsg_state_drop(msg->state);
		msg->state = state;
	} else {
		state = msg->state;
	}

	if ((msg->any.head.cmd & (DMSGF_CREATE | DMSGF_DELETE |
				  DMSGF_ABORT)) == 0) {
		error = 0;
		goto done;
	}

	switch(msg->any.head.cmd & (DMSGF_CREATE|DMSGF_DELETE|DMSGF_REPLY)) {
	case DMSGF_CREATE:
	case DMSGF_CREATE | DMSGF_DELETE:

		if (state != &iocom->state0) {
			kdio_printf(iocom, 1, "%s\n",
				    "duplicate transaction");
			error = EINVAL;
			break;
		}

		if (msg->any.head.circuit) {
			sdummy.msgid = msg->any.head.circuit;

			if (msg->any.head.cmd & DMSGF_REVCIRC) {
				pstate = RB_FIND(kdmsg_state_tree,
						 &iocom->statewr_tree,
						 &sdummy);
			} else {
				pstate = RB_FIND(kdmsg_state_tree,
						 &iocom->staterd_tree,
						 &sdummy);
			}
			if (pstate == NULL) {
				kdio_printf(iocom, 1, "%s\n",
					    "missing parent in "
					    "stacked trans");
				error = EINVAL;
				break;
			}
		} else {
			pstate = &iocom->state0;
		}

		kdmsg_state_drop(state);
		state = iocom->freerd_state;
		iocom->freerd_state = NULL;

		msg->state = state;
		state->parent = pstate;
		KKASSERT(state->iocom == iocom);
		state->flags |= KDMSG_STATE_RBINSERTED |
				KDMSG_STATE_SUBINSERTED |
			        KDMSG_STATE_OPPOSITE;
		if (TAILQ_EMPTY(&pstate->subq))
			kdmsg_state_hold(pstate);
		kdmsg_state_hold(state);
		kdmsg_state_hold(state);
		state->icmd = msg->any.head.cmd & DMSGF_BASECMDMASK;
		state->rxcmd = msg->any.head.cmd & ~DMSGF_DELETE;
		state->txcmd = DMSGF_REPLY;
		state->msgid = msg->any.head.msgid;
		state->flags &= ~KDMSG_STATE_NEW;
		RB_INSERT(kdmsg_state_tree, &iocom->staterd_tree, state);
		TAILQ_INSERT_TAIL(&pstate->subq, state, entry);
		error = 0;
		break;
	case DMSGF_DELETE:

		if (state == &iocom->state0) {
			if (msg->any.head.cmd & DMSGF_ABORT) {
				kdio_printf(iocom, 1, "%s\n",
					    "msgrx: "
					    "state already A");
				error = EALREADY;
			} else {
				kdio_printf(iocom, 1, "%s\n",
					    "msgrx: no state for DELETE");
				error = EINVAL;
			}
			break;
		}

		if ((state->rxcmd & DMSGF_CREATE) == 0) {
			if (msg->any.head.cmd & DMSGF_ABORT) {
				kdio_printf(iocom, 1, "%s\n",
					    "msgrx: state already B");
				error = EALREADY;
			} else {
				kdio_printf(iocom, 1, "%s\n",
					    "msgrx: state reused for DELETE");
				error = EINVAL;
			}
			break;
		}
		error = 0;
		break;
	default:

		if (msg->any.head.cmd & DMSGF_ABORT) {
			if (state == &iocom->state0 ||
			    (state->rxcmd & DMSGF_CREATE) == 0) {
				error = EALREADY;
				break;
			}
		}
		error = 0;
		break;
	case DMSGF_REPLY | DMSGF_CREATE:
	case DMSGF_REPLY | DMSGF_CREATE | DMSGF_DELETE:

		if (state == &iocom->state0) {
			kdio_printf(iocom, 1,
				    "msgrx: no state match for "
				    "REPLY cmd=%08x msgid=%016jx\n",
				    msg->any.head.cmd,
				    (intmax_t)msg->any.head.msgid);
			error = EINVAL;
			break;
		}
		state->rxcmd = msg->any.head.cmd & ~DMSGF_DELETE;
		error = 0;
		break;
	case DMSGF_REPLY | DMSGF_DELETE:

		if (state == &iocom->state0) {
			if (msg->any.head.cmd & DMSGF_ABORT) {
				error = EALREADY;
			} else {
				kdio_printf(iocom, 1, "%s\n",
					    "msgrx: no state match "
					    "for REPLY|DELETE");
				error = EINVAL;
			}
			break;
		}

		if ((state->rxcmd & DMSGF_CREATE) == 0) {
			if (msg->any.head.cmd & DMSGF_ABORT) {
				error = EALREADY;
			} else {
				kdio_printf(iocom, 1, "%s\n",
					    "msgrx: state reused "
					    "for REPLY|DELETE");
				error = EINVAL;
			}
			break;
		}
		error = 0;
		break;
	case DMSGF_REPLY:

		if (msg->any.head.cmd & DMSGF_ABORT) {
			if (state == &iocom->state0 ||
			    (state->rxcmd & DMSGF_CREATE) == 0) {
				error = EALREADY;
				break;
			}
		}
		error = 0;
		break;
	}

done:
	if (msg->any.head.cmd & (DMSGF_CREATE | DMSGF_DELETE)) {
		if (state != &iocom->state0) {
			msg->tcmd = (msg->state->icmd & DMSGF_BASECMDMASK) |
				    (msg->any.head.cmd & (DMSGF_CREATE |
							  DMSGF_DELETE |
							  DMSGF_REPLY));
		} else {
			msg->tcmd = 0;
		}
	} else {
		msg->tcmd = msg->any.head.cmd & DMSGF_CMDSWMASK;
	}

	if ((state = msg->state) == NULL || error != 0) {
		kdio_printf(iocom, 1,
			    "msgrx: state=%p error %d\n",
			    state, error);
	} else if (msg->any.head.cmd & DMSGF_DELETE) {
		KKASSERT((state->rxcmd & DMSGF_DELETE) == 0);
		state->rxcmd |= DMSGF_DELETE;
		if (state->txcmd & DMSGF_DELETE) {
			KKASSERT(state->flags & KDMSG_STATE_RBINSERTED);
			if (state->rxcmd & DMSGF_REPLY) {
				KKASSERT(msg->any.head.cmd &
					 DMSGF_REPLY);
				RB_REMOVE(kdmsg_state_tree,
					  &iocom->statewr_tree, state);
			} else {
				KKASSERT((msg->any.head.cmd &
					  DMSGF_REPLY) == 0);
				RB_REMOVE(kdmsg_state_tree,
					  &iocom->staterd_tree, state);
			}
			state->flags &= ~KDMSG_STATE_RBINSERTED;
			kdmsg_state_drop(state);
		}
	}
	lockmgr(&iocom->msglk, LK_RELEASE);

	return (error);
}

static int
kdmsg_autorxmsg(kdmsg_msg_t *msg)
{
	kdmsg_iocom_t *iocom = msg->state->iocom;
	kdmsg_msg_t *rep;
	int error = 0;
	uint32_t cmd;

	if (msg->state) {
		cmd = (msg->state->icmd & DMSGF_BASECMDMASK) |
		      (msg->any.head.cmd & (DMSGF_CREATE |
					    DMSGF_DELETE |
					    DMSGF_REPLY));
	} else {
		cmd = 0;
	}

	switch(cmd) {
	case DMSG_LNK_PING:

		rep = kdmsg_msg_alloc(msg->state, DMSG_LNK_PING | DMSGF_REPLY,
				      NULL, NULL);
		kdmsg_msg_write(rep);
		break;
	case DMSG_LNK_PING | DMSGF_REPLY:

		break;
	case DMSG_LNK_CONN | DMSGF_CREATE:
	case DMSG_LNK_CONN | DMSGF_CREATE | DMSGF_DELETE:

		if ((msg->any.head.cmd & DMSGF_DELETE) == 0) {
			if (iocom->flags & KDMSG_IOCOMF_AUTOCONN) {
				kdmsg_msg_result(msg, 0);
				if (iocom->auto_callback)
					iocom->auto_callback(msg);
			} else {
				error = iocom->rcvmsg(msg);
			}
			break;
		}

	case DMSG_LNK_CONN | DMSGF_DELETE:

		if (iocom->flags & KDMSG_IOCOMF_AUTOCONN) {
			if (iocom->auto_callback)
				iocom->auto_callback(msg);
			kdmsg_msg_reply(msg, 0);
		} else {
			error = iocom->rcvmsg(msg);
		}
		break;
	case DMSG_LNK_SPAN | DMSGF_CREATE:
	case DMSG_LNK_SPAN | DMSGF_CREATE | DMSGF_DELETE:

		if (iocom->flags & KDMSG_IOCOMF_AUTORXSPAN) {
			if ((msg->any.head.cmd & DMSGF_DELETE) == 0) {
				if (iocom->auto_callback)
					iocom->auto_callback(msg);
				break;
			}

		} else {
			error = iocom->rcvmsg(msg);
			break;
		}

	case DMSG_LNK_SPAN | DMSGF_DELETE:

		if (iocom->flags & KDMSG_IOCOMF_AUTORXSPAN) {
			if (iocom->auto_callback)
				iocom->auto_callback(msg);
			kdmsg_msg_reply(msg, 0);
		} else {
			error = iocom->rcvmsg(msg);
		}
		break;
	default:

		error = iocom->rcvmsg(msg);
		break;
	}
	return (error);
}

static
void
kdmsg_state_cleanuprx(kdmsg_msg_t *msg)
{
	kdmsg_state_t *state = msg->state;
	kdmsg_iocom_t *iocom = state->iocom;

	lockmgr(&iocom->msglk, LK_EXCLUSIVE);
	if (state != &iocom->state0) {

		if ((msg->any.head.cmd & DMSGF_DELETE) &&
		    DMSG_SUBQ_FIRST(&msg->state->subq)) {
			kdio_printf(iocom, 2,
				    "simulate failure for substates of "
				    "state %p cmd %08x/%08x\n",
				    msg->state,
				    msg->state->rxcmd,
				    msg->state->txcmd);
			kdmsg_simulate_failure(msg->state,
					       0, DMSG_ERR_LOSTLINK);
		}

		if ((state->flags & KDMSG_STATE_SUBINSERTED) &&
		    (state->rxcmd & DMSGF_DELETE) &&
		    (state->txcmd & DMSGF_DELETE)) {

			kdmsg_subq_delete(state);
		}
	}
	kdmsg_msg_free(msg);

	lockmgr(&iocom->msglk, LK_RELEASE);
}

static
void
kdmsg_subq_delete(kdmsg_state_t *state)
{
	kdmsg_state_t *pstate;

	if (state->flags & KDMSG_STATE_SUBINSERTED) {
		pstate = state->parent;
		KKASSERT(pstate);
		if (pstate->scan == state)
			pstate->scan = NULL;
		TAILQ_REMOVE(&pstate->subq, state, entry);
		state->flags &= ~KDMSG_STATE_SUBINSERTED;
		state->parent = NULL;
		if (TAILQ_EMPTY(&pstate->subq)) {
			kdmsg_state_drop(pstate);
		}
		pstate = NULL;
		kdmsg_state_drop(state);
	} else {
		KKASSERT(state->parent == NULL);
	}
}

static
void
kdmsg_simulate_failure(kdmsg_state_t *state, int meto, int error)
{
	kdmsg_state_t *substate;

	kdmsg_state_hold(state);

	if (meto)
		kdmsg_state_abort(state);

again:
	TAILQ_FOREACH(substate, &state->subq, entry) {
		if (substate->flags & KDMSG_STATE_ABORTING)
			continue;
		state->scan = substate;
		kdmsg_simulate_failure(substate, 1, error);
		if (state->scan != substate)
			goto again;
	}
	kdmsg_state_drop(state);
}

static
void
kdmsg_state_abort(kdmsg_state_t *state)
{
	kdmsg_msg_t *msg;

	KKASSERT((state->flags & KDMSG_STATE_ABORTING) == 0);
	if (state->flags & KDMSG_STATE_ABORTING)
		return;
	state->flags |= KDMSG_STATE_ABORTING;
	kdmsg_state_dying(state);
	if (state->flags & KDMSG_STATE_NEW) {
		kdio_printf(iocom, 5,
			    "kdmsg_state_abort(0): state %p rxcmd %08x "
			    "txcmd %08x flags %08x - in NEW state\n",
			    state, state->rxcmd,
			    state->txcmd, state->flags);
		return;
	}

	kdio_printf(iocom, 5,
		    "kdmsg_state_abort(1): state %p rxcmd %08x txcmd %08x\n",
		    state, state->rxcmd, state->txcmd);
	if ((state->rxcmd & DMSGF_DELETE) == 0) {
		msg = kdmsg_msg_alloc(state, DMSG_LNK_ERROR, NULL, NULL);
		if ((state->rxcmd & DMSGF_CREATE) == 0)
			msg->any.head.cmd |= DMSGF_CREATE;
		msg->any.head.cmd |= DMSGF_DELETE |
				     (state->rxcmd & DMSGF_REPLY);
		msg->any.head.cmd ^= (DMSGF_REVTRANS | DMSGF_REVCIRC);
		msg->any.head.error = DMSG_ERR_LOSTLINK;
		kdio_printf(iocom, 5,
			    "kdmsg_state_abort(a): state %p msgcmd %08x\n",
			    state, msg->any.head.cmd);

		lockmgr(&state->iocom->msglk, LK_RELEASE);
		kdmsg_msg_receive_handling(msg);
		lockmgr(&state->iocom->msglk, LK_EXCLUSIVE);
		msg = NULL;
	}
	kdio_printf(iocom, 5,
		    "kdmsg_state_abort(2): state %p rxcmd %08x txcmd %08x\n",
		    state, state->rxcmd, state->txcmd);
}

static
void
kdmsg_state_dying(kdmsg_state_t *state)
{
	kdmsg_state_t *scan;

	if ((state->flags & KDMSG_STATE_DYING) == 0) {
		state->flags |= KDMSG_STATE_DYING;
		TAILQ_FOREACH(scan, &state->subq, entry)
			kdmsg_state_dying(scan);
	}
}

static
int
kdmsg_state_msgtx(kdmsg_msg_t *msg)
{
	kdmsg_iocom_t *iocom = msg->state->iocom;
	kdmsg_state_t *state;
	int error;

	if ((state = iocom->freewr_state) == NULL) {
		state = hmalloc(sizeof(*state), iocom->mmsg, M_WAITOK | M_ZERO);
		state->flags = KDMSG_STATE_DYNAMIC;
		state->iocom = iocom;
		state->refs = 1;
		TAILQ_INIT(&state->subq);
		iocom->freewr_state = state;
	}

	state = msg->state;

	if ((msg->any.head.cmd & (DMSGF_CREATE | DMSGF_DELETE |
				  DMSGF_ABORT)) == 0) {
		return(0);
	}

	switch(msg->any.head.cmd & (DMSGF_CREATE | DMSGF_DELETE |
				    DMSGF_REPLY)) {
	case DMSGF_CREATE:
	case DMSGF_CREATE | DMSGF_DELETE:

		KKASSERT(state != NULL);
		state->icmd = msg->any.head.cmd & DMSGF_BASECMDMASK;
		state->txcmd = msg->any.head.cmd & ~DMSGF_DELETE;
		state->rxcmd = DMSGF_REPLY;
		state->flags &= ~KDMSG_STATE_NEW;
		error = 0;
		break;
	case DMSGF_DELETE:

		if (state == &iocom->state0) {
			if (msg->any.head.cmd & DMSGF_ABORT) {
				error = EALREADY;
			} else {
				kdio_printf(iocom, 1,
					"msgtx: no state match "
					"for DELETE cmd=%08x msgid=%016jx\n",
					msg->any.head.cmd,
					(intmax_t)msg->any.head.msgid);
				error = EINVAL;
			}
			break;
		}

		if ((state->txcmd & DMSGF_CREATE) == 0) {
			if (msg->any.head.cmd & DMSGF_ABORT) {
				error = EALREADY;
			} else {
				kdio_printf(iocom, 1, "%s\n",
					    "msgtx: state reused "
					    "for DELETE");
				error = EINVAL;
			}
			break;
		}
		error = 0;
		break;
	default:

		if (msg->any.head.cmd & DMSGF_ABORT) {
			if (state == &state->iocom->state0 ||
			    (state->txcmd & DMSGF_CREATE) == 0) {
				error = EALREADY;
				break;
			}
		}
		error = 0;
		break;
	case DMSGF_REPLY | DMSGF_CREATE:
	case DMSGF_REPLY | DMSGF_CREATE | DMSGF_DELETE:

		if (state == &state->iocom->state0) {
			kdio_printf(iocom, 1, "%s\n",
				    "msgtx: no state match "
				    "for REPLY | CREATE");
			error = EINVAL;
			break;
		}
		state->txcmd = msg->any.head.cmd & ~DMSGF_DELETE;
		error = 0;
		break;
	case DMSGF_REPLY | DMSGF_DELETE:

		if (state == &state->iocom->state0) {
			if (msg->any.head.cmd & DMSGF_ABORT) {
				error = EALREADY;
			} else {
				kdio_printf(iocom, 1, "%s\n",
					    "msgtx: no state match "
					    "for REPLY | DELETE");
				error = EINVAL;
			}
			break;
		}

		if ((state->txcmd & DMSGF_CREATE) == 0) {
			if (msg->any.head.cmd & DMSGF_ABORT) {
				error = EALREADY;
			} else {
				kdio_printf(iocom, 1, "%s\n",
					    "msgtx: state reused "
					    "for REPLY | DELETE");
				error = EINVAL;
			}
			break;
		}
		error = 0;
		break;
	case DMSGF_REPLY:

		if (msg->any.head.cmd & DMSGF_ABORT) {
			if (state == &state->iocom->state0 ||
			    (state->txcmd & DMSGF_CREATE) == 0) {
				error = EALREADY;
				break;
			}
		}
		error = 0;
		break;
	}

	if (state && error == 0)
		state->flags |= KDMSG_STATE_INTERLOCK;

	return (error);
}

static
void
kdmsg_state_cleanuptx(kdmsg_msg_t *msg)
{
	kdmsg_iocom_t *iocom = msg->state->iocom;
	kdmsg_state_t *state;

	if ((state = msg->state) == NULL) {
		kdmsg_msg_free(msg);
		return;
	}

	if (state->flags & KDMSG_STATE_SIGNAL) {
		kdio_printf(iocom, 1, "state %p interlock!\n", state);
		wakeup(state);
	}
	state->flags &= ~(KDMSG_STATE_INTERLOCK | KDMSG_STATE_SIGNAL);
	kdmsg_state_hold(state);

	if (msg->any.head.cmd & DMSGF_DELETE) {
		KKASSERT((state->txcmd & DMSGF_DELETE) == 0);
		state->txcmd |= DMSGF_DELETE;
		if (state->rxcmd & DMSGF_DELETE) {
			KKASSERT(state->flags & KDMSG_STATE_RBINSERTED);
			if (state->txcmd & DMSGF_REPLY) {
				KKASSERT(msg->any.head.cmd &
					 DMSGF_REPLY);
				RB_REMOVE(kdmsg_state_tree,
					  &iocom->staterd_tree, state);
			} else {
				KKASSERT((msg->any.head.cmd &
					  DMSGF_REPLY) == 0);
				RB_REMOVE(kdmsg_state_tree,
					  &iocom->statewr_tree, state);
			}
			state->flags &= ~KDMSG_STATE_RBINSERTED;

			if (TAILQ_EMPTY(&state->subq))
				kdmsg_subq_delete(state);
			kdmsg_msg_free(msg);
			kdmsg_state_drop(state);
		} else {
			kdmsg_msg_free(msg);
		}
	} else {
		kdmsg_msg_free(msg);
	}

	if ((state->flags & (KDMSG_STATE_ABORTING | KDMSG_STATE_DYING)) &&
	    (state->rxcmd & DMSGF_DELETE) == 0) {
		kdio_printf(iocom, 5,
			    "kdmsg_state_cleanuptx: state=%p "
			    "executing deferred abort\n",
			    state);
		state->flags &= ~KDMSG_STATE_ABORTING;
		kdmsg_state_abort(state);
	}
	kdmsg_state_drop(state);
}

static
void
_kdmsg_state_hold(kdmsg_state_t *state KDMSG_DEBUG_ARGS)
{
	atomic_add_int(&state->refs, 1);
#if KDMSG_DEBUG
	kd_printf(4, "state %p +%d\t%s:%d\n", state, state->refs, file, line);
#endif
}

static
void
_kdmsg_state_drop(kdmsg_state_t *state KDMSG_DEBUG_ARGS)
{
	KKASSERT(state->refs > 0);
#if KDMSG_DEBUG
	kd_printf(4, "state %p -%d\t%s:%d\n", state, state->refs, file, line);
#endif
	if (atomic_fetchadd_int(&state->refs, -1) == 1)
		kdmsg_state_free(state);
}

static
void
kdmsg_state_free(kdmsg_state_t *state)
{
	kdmsg_iocom_t *iocom = state->iocom;

	KKASSERT((state->flags & KDMSG_STATE_RBINSERTED) == 0);
	KKASSERT((state->flags & KDMSG_STATE_SUBINSERTED) == 0);
	KKASSERT(TAILQ_EMPTY(&state->subq));

	if (state != &state->iocom->state0)
		hfree(state, iocom->mmsg);
}

kdmsg_msg_t *
kdmsg_msg_alloc(kdmsg_state_t *state, uint32_t cmd,
		int (*func)(kdmsg_state_t *, kdmsg_msg_t *), void *data)
{
	kdmsg_iocom_t *iocom = state->iocom;
	kdmsg_state_t *pstate;
	kdmsg_msg_t *msg;
	size_t hbytes;

	KKASSERT(iocom != NULL);
	hbytes = (cmd & DMSGF_SIZE) * DMSG_ALIGN;
	msg = hmalloc(offsetof(struct kdmsg_msg, any) + hbytes,
		      iocom->mmsg, M_WAITOK | M_ZERO);
	msg->hdr_size = hbytes;

	if ((cmd & (DMSGF_CREATE | DMSGF_REPLY)) == DMSGF_CREATE) {

		pstate = state;
		state = hmalloc(sizeof(*state), iocom->mmsg, M_WAITOK | M_ZERO);
		TAILQ_INIT(&state->subq);
		state->iocom = iocom;
		state->parent = pstate;
		state->flags = KDMSG_STATE_DYNAMIC |
			       KDMSG_STATE_NEW;
		state->func = func;
		state->any.any = data;
		state->msgid = (uint64_t)(uintptr_t)state;

		lockmgr(&iocom->msglk, LK_EXCLUSIVE);
		if (RB_INSERT(kdmsg_state_tree, &iocom->statewr_tree, state))
			panic("duplicate msgid allocated");
		if (TAILQ_EMPTY(&pstate->subq))
			kdmsg_state_hold(pstate);
		TAILQ_INSERT_TAIL(&pstate->subq, state, entry);
		state->flags |= KDMSG_STATE_RBINSERTED |
				KDMSG_STATE_SUBINSERTED;
		state->flags |= pstate->flags & KDMSG_STATE_DYING;
		kdmsg_state_hold(state);
		kdmsg_state_hold(state);
		kdmsg_state_hold(state);
		lockmgr(&iocom->msglk, LK_RELEASE);
	} else {
		pstate = state->parent;
		KKASSERT(pstate != NULL);
		kdmsg_state_hold(state);
	}

	if (state->flags & KDMSG_STATE_OPPOSITE)
		cmd |= DMSGF_REVTRANS;
	if (pstate->flags & KDMSG_STATE_OPPOSITE)
		cmd |= DMSGF_REVCIRC;

	msg->any.head.magic = DMSG_HDR_MAGIC;
	msg->any.head.cmd = cmd;
	msg->any.head.msgid = state->msgid;
	msg->any.head.circuit = pstate->msgid;
	msg->state = state;

	return (msg);
}

void
kdmsg_msg_free(kdmsg_msg_t *msg)
{
	kdmsg_iocom_t *iocom = msg->state->iocom;
	kdmsg_state_t *state;

	if ((msg->flags & KDMSG_FLAG_AUXALLOC) &&
	    msg->aux_data && msg->aux_size) {
		hfree(msg->aux_data, iocom->mmsg);
		msg->aux_data = NULL;
		msg->flags &= ~KDMSG_FLAG_AUXALLOC;
	}
	if ((state = msg->state) != NULL) {
		msg->state = NULL;
		kdmsg_state_drop(state);
	}
	msg->aux_data = NULL;
	msg->aux_size = 0;

	hfree(msg, iocom->mmsg);
}

void
kdmsg_detach_aux_data(kdmsg_msg_t *msg, kdmsg_data_t *data)
{
	if (msg->flags & KDMSG_FLAG_AUXALLOC) {
		data->aux_data = msg->aux_data;
		data->aux_size = msg->aux_size;
		data->iocom = msg->state->iocom;
		msg->flags &= ~KDMSG_FLAG_AUXALLOC;
	} else {
		data->aux_data = NULL;
		data->aux_size = 0;
		data->iocom = msg->state->iocom;
	}
}

void
kdmsg_free_aux_data(kdmsg_data_t *data)
{
	if (data->aux_data) {
		hfree(data->aux_data, data->iocom->mmsg);
		data->aux_data = NULL;
	}
}

int
kdmsg_state_cmp(kdmsg_state_t *state1, kdmsg_state_t *state2)
{
	if (state1->iocom < state2->iocom)
		return(-1);
	if (state1->iocom > state2->iocom)
		return(1);
	if (state1->msgid < state2->msgid)
		return(-1);
	if (state1->msgid > state2->msgid)
		return(1);
	return(0);
}

void
kdmsg_msg_write(kdmsg_msg_t *msg)
{
	kdmsg_iocom_t *iocom = msg->state->iocom;

	lockmgr(&iocom->msglk, LK_EXCLUSIVE);
	kdmsg_msg_write_locked(iocom, msg);
	lockmgr(&iocom->msglk, LK_RELEASE);
}

static void
kdmsg_msg_write_locked(kdmsg_iocom_t *iocom, kdmsg_msg_t *msg)
{
	kdmsg_state_t *state;

	if (msg->state) {

		state = msg->state;
		msg->any.head.msgid = state->msgid;
	} else {

		state = NULL;
		msg->any.head.msgid = 0;
	}

	if (state) {
		KKASSERT((state->txcmd & DMSGF_DELETE) == 0);
		if (state->flags & KDMSG_STATE_DYING) {
#if 0
		if ((state->flags & KDMSG_STATE_DYING) ||
		    (state->parent->txcmd & DMSGF_DELETE) ||
		    (state->parent->flags & KDMSG_STATE_DYING)) {
#endif
			kdio_printf(iocom, 4,
				    "kdmsg_msg_write: Write to dying circuit "
				    "state=%p "
				    "ptxcmd=%08x prxcmd=%08x flags=%08x\n",
				    state,
				    state->parent->rxcmd,
				    state->parent->txcmd,
				    state->parent->flags);
			kdmsg_state_hold(state);
			kdmsg_state_msgtx(msg);
			kdmsg_state_cleanuptx(msg);
			kdmsg_state_drop(state);
			return;
		}
	}

	msg->any.head.salt =   (iocom->msg_seq & 255);
	++iocom->msg_seq;

	if (msg->aux_data && msg->aux_size) {
		uint32_t abytes = DMSG_DOALIGN(msg->aux_size);

		msg->any.head.aux_bytes = msg->aux_size;
		msg->any.head.aux_crc = iscsi_crc32(msg->aux_data, abytes);
	}
	msg->any.head.hdr_crc = 0;
	msg->any.head.hdr_crc = iscsi_crc32(msg->any.buf, msg->hdr_size);

	if (iocom->flags & KDMSG_IOCOMF_EXITNOACC)
		kdmsg_drain_msg(msg);
	else
		TAILQ_INSERT_TAIL(&iocom->msgq, msg, qentry);

	if (iocom->msg_ctl & KDMSG_CLUSTERCTL_SLEEPING) {
		atomic_clear_int(&iocom->msg_ctl,
				 KDMSG_CLUSTERCTL_SLEEPING);
		wake_up(&iocom->msg_waitq);
	}
}

void
kdmsg_msg_reply(kdmsg_msg_t *msg, uint32_t error)
{
	kdmsg_state_t *state = msg->state;
	kdmsg_msg_t *nmsg;
	uint32_t cmd;

	cmd = DMSG_LNK_ERROR;

	if (state != &state->iocom->state0) {
		if (state->txcmd & DMSGF_DELETE)
			return;
		if ((state->txcmd & DMSGF_CREATE) == 0)
			cmd |= DMSGF_CREATE;
		if (state->txcmd & DMSGF_REPLY)
			cmd |= DMSGF_REPLY;
		cmd |= DMSGF_DELETE;
	} else {
		if ((msg->any.head.cmd & DMSGF_REPLY) == 0)
			cmd |= DMSGF_REPLY;
	}

	nmsg = kdmsg_msg_alloc(state, cmd, NULL, NULL);
	nmsg->any.head.error = error;
	kdmsg_msg_write(nmsg);
}

void
kdmsg_msg_result(kdmsg_msg_t *msg, uint32_t error)
{
	kdmsg_state_t *state = msg->state;
	kdmsg_msg_t *nmsg;
	uint32_t cmd;

	cmd = DMSG_LNK_ERROR;

	if (state != &state->iocom->state0) {
		if (state->txcmd & DMSGF_DELETE)
			return;
		if ((state->txcmd & DMSGF_CREATE) == 0)
			cmd |= DMSGF_CREATE;
		if (state->txcmd & DMSGF_REPLY)
			cmd |= DMSGF_REPLY;

	} else {
		if ((msg->any.head.cmd & DMSGF_REPLY) == 0)
			cmd |= DMSGF_REPLY;
	}

	nmsg = kdmsg_msg_alloc(state, cmd, NULL, NULL);
	nmsg->any.head.error = error;
	kdmsg_msg_write(nmsg);
}

void
kdmsg_state_reply(kdmsg_state_t *state, uint32_t error)
{
	kdmsg_msg_t *nmsg;
	uint32_t cmd;

	cmd = DMSG_LNK_ERROR;

	KKASSERT(state);
	if (state->txcmd & DMSGF_DELETE)
		return;
	if ((state->txcmd & DMSGF_CREATE) == 0)
		cmd |= DMSGF_CREATE;
	if (state->txcmd & DMSGF_REPLY)
		cmd |= DMSGF_REPLY;
	cmd |= DMSGF_DELETE;

	nmsg = kdmsg_msg_alloc(state, cmd, NULL, NULL);
	nmsg->any.head.error = error;
	kdmsg_msg_write(nmsg);
}

void
kdmsg_state_result(kdmsg_state_t *state, uint32_t error)
{
	kdmsg_msg_t *nmsg;
	uint32_t cmd;

	cmd = DMSG_LNK_ERROR;

	KKASSERT(state);
	if (state->txcmd & DMSGF_DELETE)
		return;
	if ((state->txcmd & DMSGF_CREATE) == 0)
		cmd |= DMSGF_CREATE;
	if (state->txcmd & DMSGF_REPLY)
		cmd |= DMSGF_REPLY;

	nmsg = kdmsg_msg_alloc(state, cmd, NULL, NULL);
	nmsg->any.head.error = error;
	kdmsg_msg_write(nmsg);
}
