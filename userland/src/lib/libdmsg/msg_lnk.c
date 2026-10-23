#include "dmsg_local.h"

#define DMSG_SPAN_MAXDIST	16

struct h2span_link;
struct h2span_relay;
TAILQ_HEAD(h2span_conn_queue, h2span_conn);
TAILQ_HEAD(h2span_relay_queue, h2span_relay);

RB_HEAD(h2span_cluster_tree, h2span_cluster);
RB_HEAD(h2span_node_tree, h2span_node);
RB_HEAD(h2span_link_tree, h2span_link);
RB_HEAD(h2span_relay_tree, h2span_relay);
uint32_t DMsgRNSS;

struct h2span_conn {
	TAILQ_ENTRY(h2span_conn) entry;
	struct h2span_relay_tree tree;
	dmsg_state_t *state;
	dmsg_lnk_conn_t lnk_conn;
};

struct h2span_cluster {
	RB_ENTRY(h2span_cluster) rbnode;
	struct h2span_node_tree tree;
	dmsg_uuid_t	peer_id;
	uint8_t	peer_type;
	uint8_t reserved01[7];
	char	peer_label[128];
	int	refs;
};

struct h2span_node {
	RB_ENTRY(h2span_node) rbnode;
	struct h2span_link_tree tree;
	struct h2span_cluster *cls;
	uint8_t	pfs_type;
	uint8_t reserved01[7];
	dmsg_uuid_t	pfs_id;
	char	pfs_label[128];
	void	*opaque;
};

struct h2span_link {
	RB_ENTRY(h2span_link) rbnode;
	dmsg_state_t	*state;
	struct h2span_node *node;
	struct h2span_relay_queue relayq;
	dmsg_lnk_span_t lnk_span;
};

struct h2span_relay {
	TAILQ_ENTRY(h2span_relay) entry;
	RB_ENTRY(h2span_relay) rbnode;
	struct h2span_conn	*conn;
	dmsg_state_t		*source_rt;
	dmsg_state_t		*target_rt;
};

typedef struct h2span_conn h2span_conn_t;
typedef struct h2span_cluster h2span_cluster_t;
typedef struct h2span_node h2span_node_t;
typedef struct h2span_link h2span_link_t;
typedef struct h2span_relay h2span_relay_t;

#define dmsg_termstr(array)	_dmsg_termstr((array), sizeof(array))

static h2span_relay_t *dmsg_generate_relay(h2span_conn_t *conn,
					h2span_link_t *slink);
static uint32_t dmsg_rnss(void);

static __inline
void
_dmsg_termstr(char *base, size_t size)
{
	base[size-1] = 0;
}

static
int
h2span_cluster_cmp(h2span_cluster_t *cls1, h2span_cluster_t *cls2)
{
	int r;

	if (cls1->peer_type < cls2->peer_type)
		return(-1);
	if (cls1->peer_type > cls2->peer_type)
		return(1);
	r = dmsg_uuid_compare(&cls1->peer_id, &cls2->peer_id);
	if (r == 0)
		r = strcmp(cls1->peer_label, cls2->peer_label);

	return r;
}

static
int
h2span_node_cmp(h2span_node_t *node1, h2span_node_t *node2)
{
	int r;

	r = strcmp(node1->pfs_label, node2->pfs_label);
	if (r == 0)
		r = dmsg_uuid_compare(&node1->pfs_id, &node2->pfs_id);
	return (r);
}

static
int
h2span_link_cmp(h2span_link_t *link1, h2span_link_t *link2)
{
	if (link1->lnk_span.dist < link2->lnk_span.dist)
		return(-1);
	if (link1->lnk_span.dist > link2->lnk_span.dist)
		return(1);
	if (link1->lnk_span.rnss < link2->lnk_span.rnss)
		return(-1);
	if (link1->lnk_span.rnss > link2->lnk_span.rnss)
		return(1);
#if 1
	if ((uintptr_t)link1->state < (uintptr_t)link2->state)
		return(-1);
	if ((uintptr_t)link1->state > (uintptr_t)link2->state)
		return(1);
#else
	if (link1->state->msgid < link2->state->msgid)
		return(-1);
	if (link1->state->msgid > link2->state->msgid)
		return(1);
#endif
	return(0);
}

static
int
h2span_relay_cmp(h2span_relay_t *relay1, h2span_relay_t *relay2)
{
	h2span_link_t *link1 = relay1->source_rt->any.link;
	h2span_link_t *link2 = relay2->source_rt->any.link;

	if ((intptr_t)link1->node < (intptr_t)link2->node)
		return(-1);
	if ((intptr_t)link1->node > (intptr_t)link2->node)
		return(1);
	if (link1->lnk_span.dist < link2->lnk_span.dist)
		return(-1);
	if (link1->lnk_span.dist > link2->lnk_span.dist)
		return(1);
	if (link1->lnk_span.rnss < link2->lnk_span.rnss)
		return(-1);
	if (link1->lnk_span.rnss > link2->lnk_span.rnss)
		return(1);
#if 1
	if ((uintptr_t)link1->state < (uintptr_t)link2->state)
		return(-1);
	if ((uintptr_t)link1->state > (uintptr_t)link2->state)
		return(1);
#else
	if (link1->state->msgid < link2->state->msgid)
		return(-1);
	if (link1->state->msgid > link2->state->msgid)
		return(1);
#endif
	return(0);
}

RB_PROTOTYPE_STATIC(h2span_cluster_tree, h2span_cluster,
	     rbnode, h2span_cluster_cmp);
RB_PROTOTYPE_STATIC(h2span_node_tree, h2span_node,
	     rbnode, h2span_node_cmp);
RB_PROTOTYPE_STATIC(h2span_link_tree, h2span_link,
	     rbnode, h2span_link_cmp);
RB_PROTOTYPE_STATIC(h2span_relay_tree, h2span_relay,
	     rbnode, h2span_relay_cmp);

RB_GENERATE_STATIC(h2span_cluster_tree, h2span_cluster,
	     rbnode, h2span_cluster_cmp);
RB_GENERATE_STATIC(h2span_node_tree, h2span_node,
	     rbnode, h2span_node_cmp);
RB_GENERATE_STATIC(h2span_link_tree, h2span_link,
	     rbnode, h2span_link_cmp);
RB_GENERATE_STATIC(h2span_relay_tree, h2span_relay,
	     rbnode, h2span_relay_cmp);

static pthread_mutex_t cluster_mtx;
static __attribute__((constructor)) void
cluster_mtx_init(void)
{
	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&cluster_mtx, &attr);
	pthread_mutexattr_destroy(&attr);
}
static struct h2span_cluster_tree cluster_tree = RB_INITIALIZER(cluster_tree);
static struct h2span_conn_queue connq = TAILQ_HEAD_INITIALIZER(connq);
static struct dmsg_media_queue mediaq = TAILQ_HEAD_INITIALIZER(mediaq);

static void dmsg_lnk_span(dmsg_msg_t *msg);
static void dmsg_lnk_conn(dmsg_msg_t *msg);
static void dmsg_lnk_ping(dmsg_msg_t *msg);
static void dmsg_lnk_relay(dmsg_msg_t *msg);
static void dmsg_relay_scan(h2span_conn_t *conn, h2span_node_t *node);
static void dmsg_relay_delete(h2span_relay_t *relay);

void
dmsg_msg_lnk_signal(dmsg_iocom_t *iocom __unused)
{
	pthread_mutex_lock(&cluster_mtx);
	dmsg_relay_scan(NULL, NULL);
	pthread_mutex_unlock(&cluster_mtx);
}

void
dmsg_msg_lnk(dmsg_msg_t *msg)
{
	dmsg_iocom_t *iocom = msg->state->iocom;

	switch(msg->tcmd & DMSGF_BASECMDMASK) {
	case DMSG_LNK_CONN:
		dmsg_lnk_conn(msg);
		break;
	case DMSG_LNK_SPAN:
		dmsg_lnk_span(msg);
		break;
	case DMSG_LNK_PING:
		dmsg_lnk_ping(msg);
		break;
	default:
		iocom->usrmsg_callback(msg, 1);

		break;
	}
}

void
dmsg_lnk_conn(dmsg_msg_t *msg)
{
	dmsg_state_t *state = msg->state;
	dmsg_iocom_t *iocom = state->iocom;
	dmsg_media_t *media;
	h2span_conn_t *conn;
	h2span_relay_t *relay;
	char *alloc = NULL;

	pthread_mutex_lock(&cluster_mtx);

	dmio_printf(iocom, 3,
		"dmsg_lnk_conn: msg %p cmd %08x state %p "
		"txcmd %08x rxcmd %08x\n",
		msg, msg->any.head.cmd, state,
		state->txcmd, state->rxcmd);

	switch(msg->any.head.cmd & DMSGF_TRANSMASK) {
	case DMSG_LNK_CONN | DMSGF_CREATE:
	case DMSG_LNK_CONN | DMSGF_CREATE | DMSGF_DELETE:

		dmio_printf(iocom, 3, "LNK_CONN(%08x): %s/%s\n",
			(uint32_t)msg->any.head.msgid,
			dmsg_uuid_to_str(&msg->any.lnk_conn.peer_id, &alloc),
			msg->any.lnk_conn.peer_label);
		free(alloc);

		conn = dmsg_alloc(sizeof(*conn));
		assert(state->iocom->conn == NULL);

		RB_INIT(&conn->tree);
		state->iocom->conn = conn;
		state->iocom->conn_msgid = state->msgid;
		dmsg_state_hold(state);
		conn->state = state;
		state->func = dmsg_lnk_conn;
		state->any.conn = conn;
		TAILQ_INSERT_TAIL(&connq, conn, entry);
		conn->lnk_conn = msg->any.lnk_conn;

		TAILQ_FOREACH(media, &mediaq, entry) {
			if (dmsg_uuid_compare(&msg->any.lnk_conn.media_id,
					 &media->media_id) == 0) {
				break;
			}
		}
		if (media == NULL) {
			media = dmsg_alloc(sizeof(*media));
			media->media_id = msg->any.lnk_conn.media_id;
			TAILQ_INSERT_TAIL(&mediaq, media, entry);
		}
		state->media = media;
		++media->refs;

		if ((msg->any.head.cmd & DMSGF_DELETE) == 0) {
			iocom->usrmsg_callback(msg, 0);
			dmsg_msg_result(msg, 0);
			dmsg_iocom_signal(iocom);
			break;
		}

	case DMSG_LNK_CONN | DMSGF_DELETE:
	case DMSG_LNK_ERROR | DMSGF_DELETE:

		dmio_printf(iocom, 3, "%s\n", "LNK_CONN: Terminated");
		conn = state->any.conn;
		assert(conn);

		media = state->media;
		--media->refs;
		if (media->refs == 0) {
			dmio_printf(iocom, 3, "%s\n", "Media shutdown");
			TAILQ_REMOVE(&mediaq, media, entry);
			pthread_mutex_unlock(&cluster_mtx);
			iocom->usrmsg_callback(msg, 0);
			pthread_mutex_lock(&cluster_mtx);
			dmsg_free(media);
		}
		state->media = NULL;

		while ((relay = RB_ROOT(&conn->tree)) != NULL) {
			dmsg_relay_delete(relay);
		}

		conn->state = NULL;
		msg->state->any.conn = NULL;
		msg->state->iocom->conn = NULL;
		TAILQ_REMOVE(&connq, conn, entry);
		dmsg_free(conn);

		dmsg_msg_reply(msg, 0);
		dmsg_state_drop(state);

		break;
	default:
		iocom->usrmsg_callback(msg, 1);
#if 0
		if (msg->any.head.cmd & DMSGF_DELETE)
			goto deleteconn;
		dmsg_msg_reply(msg, DMSG_ERR_NOSUPP);
#endif
		break;
	}
	pthread_mutex_unlock(&cluster_mtx);
}

void
dmsg_lnk_span(dmsg_msg_t *msg)
{
	dmsg_state_t *state = msg->state;
	dmsg_iocom_t *iocom = state->iocom;
	h2span_cluster_t dummy_cls;
	h2span_node_t dummy_node;
	h2span_cluster_t *cls;
	h2span_node_t *node;
	h2span_link_t *slink;
	h2span_relay_t *relay;
	char *alloc = NULL;

	if (msg->any.head.cmd & DMSGF_REPLY) {
		dmio_printf(iocom, 2, "%s\n",
			    "Ignore reply to LNK_SPAN");
		return;
	}

	pthread_mutex_lock(&cluster_mtx);

	if (msg->any.head.cmd & DMSGF_CREATE) {
		assert(state->func == NULL);
		state->func = dmsg_lnk_span;

		dmsg_termstr(msg->any.lnk_span.peer_label);
		dmsg_termstr(msg->any.lnk_span.pfs_label);

		dummy_cls.peer_id = msg->any.lnk_span.peer_id;
		dummy_cls.peer_type = msg->any.lnk_span.peer_type;
		bcopy(msg->any.lnk_span.peer_label, dummy_cls.peer_label,
		      sizeof(dummy_cls.peer_label));
		cls = RB_FIND(h2span_cluster_tree, &cluster_tree, &dummy_cls);
		if (cls == NULL) {
			cls = dmsg_alloc(sizeof(*cls));
			cls->peer_id = msg->any.lnk_span.peer_id;
			cls->peer_type = msg->any.lnk_span.peer_type;
			bcopy(msg->any.lnk_span.peer_label,
			      cls->peer_label, sizeof(cls->peer_label));
			RB_INIT(&cls->tree);
			RB_INSERT(h2span_cluster_tree, &cluster_tree, cls);
		}

		dummy_node.pfs_id = msg->any.lnk_span.pfs_id;
		bcopy(msg->any.lnk_span.pfs_label, dummy_node.pfs_label,
		      sizeof(dummy_node.pfs_label));
		node = RB_FIND(h2span_node_tree, &cls->tree, &dummy_node);
		if (node == NULL) {
			node = dmsg_alloc(sizeof(*node));
			node->pfs_id = msg->any.lnk_span.pfs_id;
			node->pfs_type = msg->any.lnk_span.pfs_type;
			bcopy(msg->any.lnk_span.pfs_label, node->pfs_label,
			      sizeof(node->pfs_label));
			node->cls = cls;
			RB_INIT(&node->tree);
			RB_INSERT(h2span_node_tree, &cls->tree, node);
		}

		assert(state->any.link == NULL);
		dmsg_state_hold(state);
		slink = dmsg_alloc(sizeof(*slink));
		TAILQ_INIT(&slink->relayq);
		slink->node = node;
		slink->state = state;
		state->any.link = slink;
		slink->lnk_span = msg->any.lnk_span;

		RB_INSERT(h2span_link_tree, &node->tree, slink);

		dmio_printf(iocom, 3,
			    "LNK_SPAN(thr %p): %p %s cl=%s fs=%s dist=%d\n",
			    iocom, slink,
			    dmsg_uuid_to_str(&msg->any.lnk_span.peer_id,
					     &alloc),
			    msg->any.lnk_span.peer_label,
			    msg->any.lnk_span.pfs_label,
			    msg->any.lnk_span.dist);
		free(alloc);
#if 0
		dmsg_relay_scan(NULL, node);
#endif

		dmsg_state_result(state, 0);
		dmsg_iocom_signal(iocom);
	}

	if (msg->any.head.cmd & DMSGF_DELETE) {
		slink = state->any.link;
		assert(slink->state == state);
		assert(slink != NULL);
		node = slink->node;
		cls = node->cls;

		dmio_printf(iocom, 3,
			    "LNK_DELE(thr %p): %p %s cl=%s fs=%s\n",
			    iocom, slink,
			    dmsg_uuid_to_str(&cls->peer_id, &alloc),
			    cls->peer_label,
			    node->pfs_label);
		free(alloc);

		while ((relay = TAILQ_FIRST(&slink->relayq)) != NULL) {
			dmsg_relay_delete(relay);
		}

		RB_REMOVE(h2span_link_tree, &node->tree, slink);
		if (RB_EMPTY(&node->tree)) {
			RB_REMOVE(h2span_node_tree, &cls->tree, node);
			if (RB_EMPTY(&cls->tree) && cls->refs == 0) {
				RB_REMOVE(h2span_cluster_tree,
					  &cluster_tree, cls);
				dmsg_free(cls);
			}
			node->cls = NULL;
			dmsg_free(node);
			node = NULL;
		}
		state->any.link = NULL;
		slink->state = NULL;
		slink->node = NULL;
		dmsg_state_drop(state);
		dmsg_free(slink);

		dmsg_state_reply(state, 0);

#if 0
		if (node)
			dmsg_relay_scan(NULL, node);
#endif
		if (node)
			dmsg_iocom_signal(iocom);
	}

	pthread_mutex_unlock(&cluster_mtx);
}

static
void
dmsg_lnk_ping(dmsg_msg_t *msg)
{
	dmsg_msg_t *rep;

	if (msg->any.head.cmd & DMSGF_REPLY) {
		msg->state->iocom->usrmsg_callback(msg, 1);
	} else {
		rep = dmsg_msg_alloc(msg->state, 0,
				     DMSG_LNK_PING | DMSGF_REPLY,
				     NULL, NULL);
		dmsg_msg_write(rep);
	}
}

static void dmsg_relay_scan_specific(h2span_node_t *node,
					h2span_conn_t *conn);

static void
dmsg_relay_scan(h2span_conn_t *conn, h2span_node_t *node)
{
	h2span_cluster_t *cls;

	if (node) {

		TAILQ_FOREACH(conn, &connq, entry)
			dmsg_relay_scan_specific(node, conn);
	} else {

		RB_FOREACH(cls, h2span_cluster_tree, &cluster_tree) {

			RB_FOREACH(node, h2span_node_tree, &cls->tree) {

				if (conn) {
					dmsg_relay_scan_specific(node, conn);
				} else {
					TAILQ_FOREACH(conn, &connq, entry) {
					    dmsg_relay_scan_specific(node,
									conn);
					}
					assert(conn == NULL);
				}
			}
		}
	}
}

struct relay_scan_info {
	h2span_node_t *node;
	h2span_relay_t *relay;
};

static int
dmsg_relay_scan_cmp(h2span_relay_t *relay, void *arg)
{
	struct relay_scan_info *info = arg;

	if ((intptr_t)relay->source_rt->any.link->node < (intptr_t)info->node)
		return(-1);
	if ((intptr_t)relay->source_rt->any.link->node > (intptr_t)info->node)
		return(1);
	return(0);
}

static int
dmsg_relay_scan_callback(h2span_relay_t *relay, void *arg)
{
	struct relay_scan_info *info = arg;

	info->relay = relay;
	return(-1);
}

static void
dmsg_relay_scan_specific(h2span_node_t *node, h2span_conn_t *conn)
{
	struct relay_scan_info info;
	h2span_relay_t *relay;
	h2span_relay_t *next_relay;
	h2span_link_t *slink;
	dmsg_lnk_conn_t *lconn;
	dmsg_lnk_span_t *lspan;
	int count;
	int maxcount = 2;
#ifdef REQUIRE_SYMMETRICAL
	uint32_t lastdist = DMSG_SPAN_MAXDIST;
	uint32_t lastrnss = 0;
#endif

	info.node = node;
	info.relay = NULL;

	RB_SCAN(h2span_relay_tree, &conn->tree,
		dmsg_relay_scan_cmp, dmsg_relay_scan_callback, &info);
	relay = info.relay;
	info.relay = NULL;
	if (relay)
		assert(relay->source_rt->any.link->node == node);

	dm_printf(9, "relay scan for connection %p\n", conn);

	count = 0;
	RB_FOREACH(slink, h2span_link_tree, &node->tree) {

		if (++count >= maxcount) {
#ifdef REQUIRE_SYMMETRICAL
			if (lastdist != slink->lnk_span.dist ||
			    lastrnss != slink->lnk_span.rnss) {
				break;
			}
#else
			break;
#endif

		}

		if (relay && relay->source_rt->any.link == slink) {
			relay = RB_NEXT(h2span_relay_tree, &conn->tree, relay);
			continue;
		}

		if (slink->lnk_span.dist > DMSG_SPAN_MAXDIST)
			break;

		if (slink->state->iocom == conn->state->iocom)
			break;

		lspan = &slink->lnk_span;
		lconn = &conn->lnk_conn;
		if (((1LLU << lspan->peer_type) & lconn->peer_mask) == 0)
			break;

		if (lconn->peer_type == DMSG_PEER_CLIENT &&
		    lspan->peer_type == DMSG_PEER_CLIENT) {
			break;
		}

		if (lconn->peer_type == DMSG_PEER_CLIENT &&
		    !dmsg_uuid_is_nil(&lconn->peer_id) &&
		    dmsg_uuid_compare(&slink->node->cls->peer_id,
				 &lconn->peer_id)) {
			break;
		}

		assert(relay == NULL ||
		       relay->source_rt->any.link->node != slink->node ||
		       relay->source_rt->any.link->lnk_span.dist >=
		        slink->lnk_span.dist);
		relay = dmsg_generate_relay(conn, slink);
#ifdef REQUIRE_SYMMETRICAL
		lastdist = slink->lnk_span.dist;
		lastrnss = slink->lnk_span.rnss;
#endif

		relay = RB_NEXT(h2span_relay_tree, &conn->tree, relay);
	}

	while (relay && relay->source_rt->any.link->node == node) {
		next_relay = RB_NEXT(h2span_relay_tree, &conn->tree, relay);
		dm_printf(9, "%s\n", "RELAY DELETE FROM EXTRAS");
		dmsg_relay_delete(relay);
		relay = next_relay;
	}
}

dmsg_state_t *
dmsg_findspan(const char *label)
{
	dmsg_state_t *state;
        h2span_cluster_t *cls;
	h2span_node_t *node;
	h2span_link_t *slink;
	uint64_t msgid = strtoull(label, NULL, 16);

	pthread_mutex_lock(&cluster_mtx);

	state = NULL;
	RB_FOREACH(cls, h2span_cluster_tree, &cluster_tree) {
		RB_FOREACH(node, h2span_node_tree, &cls->tree) {
			RB_FOREACH(slink, h2span_link_tree, &node->tree) {
				if (slink->state->msgid == msgid) {
					state = slink->state;
					goto done;
				}
			}
		}
	}
done:
	pthread_mutex_unlock(&cluster_mtx);

	dm_printf(8, "findspan: %p\n", state);

	return state;
}

static
h2span_relay_t *
dmsg_generate_relay(h2span_conn_t *conn, h2span_link_t *slink)
{
	h2span_relay_t *relay;
	dmsg_msg_t *msg;

	dmsg_state_hold(slink->state);
	relay = dmsg_alloc(sizeof(*relay));
	relay->conn = conn;
	relay->source_rt = slink->state;

	msg = dmsg_msg_alloc(&conn->state->iocom->state0,
			     0, DMSG_LNK_SPAN | DMSGF_CREATE,
			     dmsg_lnk_relay, relay);
	dmsg_state_hold(msg->state);
	relay->target_rt = msg->state;

	msg->any.lnk_span = slink->lnk_span;
	msg->any.lnk_span.dist = slink->lnk_span.dist + 1;
	msg->any.lnk_span.rnss = slink->lnk_span.rnss + dmsg_rnss();

	RB_INSERT(h2span_relay_tree, &conn->tree, relay);
	TAILQ_INSERT_TAIL(&slink->relayq, relay, entry);

	msg->state->relay = relay->source_rt;
	dmsg_state_hold(msg->state->relay);

	dmsg_msg_write(msg);

	return (relay);
}

static void
dmsg_lnk_relay(dmsg_msg_t *msg)
{
	dmsg_state_t *state = msg->state;
	h2span_relay_t *relay;

	assert(msg->any.head.cmd & DMSGF_REPLY);

	if (msg->any.head.cmd & DMSGF_DELETE) {
		pthread_mutex_lock(&cluster_mtx);
		dm_printf(8, "%s\n", "RELAY DELETE FROM LNK_RELAY MSG");
		if ((relay = state->any.relay) != NULL) {
			dmsg_relay_delete(relay);
		} else {
			dmsg_state_reply(state, 0);
		}
		pthread_mutex_unlock(&cluster_mtx);
	}
}

static
void
dmsg_relay_delete(h2span_relay_t *relay)
{
	dm_printf(8,
		  "RELAY DELETE %p RELAY %p ON CLS=%p NODE=%p "
		  "DIST=%d FD %d STATE %p\n",
		  relay->source_rt->any.link,
		  relay,
		  relay->source_rt->any.link->node->cls,
		  relay->source_rt->any.link->node,
		  relay->source_rt->any.link->lnk_span.dist,
		  relay->conn->state->iocom->sock_fd,
		  relay->target_rt);

	RB_REMOVE(h2span_relay_tree, &relay->conn->tree, relay);
	TAILQ_REMOVE(&relay->source_rt->any.link->relayq, relay, entry);

	if (relay->target_rt) {
		relay->target_rt->any.relay = NULL;
		dmsg_state_reply(relay->target_rt, 0);
		dmsg_state_drop(relay->target_rt);

		relay->target_rt = NULL;
	}

	relay->conn = NULL;
	if (relay->source_rt) {
		dmsg_state_drop(relay->source_rt);
		relay->source_rt = NULL;
	}
	dmsg_free(relay);
}

#if 0

h2span_cluster_t *
dmsg_cluster_get(uuid_t *peer_id)
{
	h2span_cluster_t dummy_cls;
	h2span_cluster_t *cls;

	dummy_cls.peer_id = *peer_id;
	pthread_mutex_lock(&cluster_mtx);
	cls = RB_FIND(h2span_cluster_tree, &cluster_tree, &dummy_cls);
	if (cls)
		++cls->refs;
	pthread_mutex_unlock(&cluster_mtx);
	return (cls);
}

void
dmsg_cluster_put(h2span_cluster_t *cls)
{
	pthread_mutex_lock(&cluster_mtx);
	assert(cls->refs > 0);
	--cls->refs;
	if (RB_EMPTY(&cls->tree) && cls->refs == 0) {
		RB_REMOVE(h2span_cluster_tree,
			  &cluster_tree, cls);
		dmsg_free(cls);
	}
	pthread_mutex_unlock(&cluster_mtx);
}

h2span_node_t *
dmsg_node_get(h2span_cluster_t *cls, uuid_t *pfs_id)
{
}

#endif

void
dmsg_shell_tree(dmsg_iocom_t *iocom, char *cmdbuf __unused)
{
	h2span_cluster_t *cls;
	h2span_node_t *node;
	h2span_link_t *slink;
	h2span_relay_t *relay;
	char *uustr = NULL;

	pthread_mutex_lock(&cluster_mtx);
	RB_FOREACH(cls, h2span_cluster_tree, &cluster_tree) {
		dmsg_printf(iocom, "Cluster %s %s (%s)\n",
				  dmsg_peer_type_to_str(cls->peer_type),
				  dmsg_uuid_to_str(&cls->peer_id, &uustr),
				  cls->peer_label);
		RB_FOREACH(node, h2span_node_tree, &cls->tree) {
			dmsg_printf(iocom, "    Node %02x %s (%s)\n",
				node->pfs_type,
				dmsg_uuid_to_str(&node->pfs_id, &uustr),
				node->pfs_label);
			RB_FOREACH(slink, h2span_link_tree, &node->tree) {
				dmsg_printf(iocom,
					    "\tSLink msgid %016jx "
					    "dist=%d via %d\n",
					    (intmax_t)slink->state->msgid,
					    slink->lnk_span.dist,
					    slink->state->iocom->sock_fd);
				TAILQ_FOREACH(relay, &slink->relayq, entry) {
					dmsg_printf(iocom,
					    "\t    Relay-out msgid %016jx "
					    "via %d\n",
					    (intmax_t)relay->target_rt->msgid,
					    relay->target_rt->iocom->sock_fd);
				}
			}
		}
	}
	pthread_mutex_unlock(&cluster_mtx);
	if (uustr)
		free(uustr);
#if 0
	TAILQ_FOREACH(conn, &connq, entry) {
	}
#endif
}

int
dmsg_debug_findspan(uint64_t msgid, dmsg_state_t **statep)
{
	h2span_cluster_t *cls;
	h2span_node_t *node;
	h2span_link_t *slink;

	pthread_mutex_lock(&cluster_mtx);
	RB_FOREACH(cls, h2span_cluster_tree, &cluster_tree) {
		RB_FOREACH(node, h2span_node_tree, &cls->tree) {
			RB_FOREACH(slink, h2span_link_tree, &node->tree) {
				if (slink->state->msgid == msgid) {
					*statep = slink->state;
					goto found;
				}
			}
		}
	}
	pthread_mutex_unlock(&cluster_mtx);
	*statep = NULL;
	return(ENOENT);
found:
	pthread_mutex_unlock(&cluster_mtx);
	return(0);
}

static
uint32_t
dmsg_rnss(void)
{
	if (DMsgRNSS == 0) {
		pthread_mutex_lock(&cluster_mtx);
		while (DMsgRNSS == 0) {
			srandom(time(0));
			DMsgRNSS = random();
		}
		pthread_mutex_unlock(&cluster_mtx);
	}
	return(DMsgRNSS);
}
