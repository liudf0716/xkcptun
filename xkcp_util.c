/********************************************************************\
 * This program is free software; you can redistribute it and/or    *
 * modify it under the terms of the GNU General Public License as   *
 * published by the Free Software Foundation; either version 2 of   *
 * the License, or (at your option) any later version.              *
 *                                                                  *
 * This program is distributed in the hope that it will be useful,  *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of   *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the    *
 * GNU General Public License for more details.                     *
 *                                                                  *
 * You should have received a copy of the GNU General Public License*
 * along with this program; if not, contact:                        *
 *                                                                  *
 * Free Software Foundation           Voice:  +1-617-542-5942       *
 * 59 Temple Place - Suite 330        Fax:    +1-617-542-2652       *
 * Boston, MA  02111-1307,  USA       gnu@gnu.org                   *
 *                                                                  *
\********************************************************************/

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <net/if.h>

#include <event2/event.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <event2/util.h>

#include <syslog.h>
#include <signal.h>

#include "ikcp.h"
#include "fec.h"
#include "xkcp_proto.h"
#include "xkcp_auth.h"
#include "xkcp_util.h"
#include "xkcp_config.h"
#include "commandline.h"
#include "debug.h"
#include "jwHash.h"

struct event_base *g_exit_base = NULL;
static struct event_base *g_evbase = NULL;

void itimeofday(long *sec, long *usec)
{
	struct timeval time;
	gettimeofday(&time, NULL);
	if (sec) *sec = time.tv_sec;
	if (usec) *usec = time.tv_usec;
}

IINT64 iclock64(void)
{
	long s, u;
	IINT64 value;
	itimeofday(&s, &u);
	value = ((IINT64)s) * 1000 + (u / 1000);
	return value;
}

void xkcp_set_sk_mark(int fd)
{
#ifdef SO_MARK
	uint32_t mark = XKCP_SK_MARK;
	setsockopt(fd, SOL_SOCKET, SO_MARK, &mark, sizeof(mark));
#endif
}

IUINT32 iclock(void)
{
	return (IUINT32)(iclock64() & 0xfffffffful);
}

char *get_iface_ip(const char *ifname)
{
	struct ifreq if_data;
	struct in_addr in;
	char *ip_str;
	int sockd;
	u_int32_t ip;

	if (!ifname || strlen(ifname) == 0) {
		return strdup("0.0.0.0");
	}

	/* Create a socket */
	if ((sockd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
		debug(LOG_ERR, "socket(): %s", strerror(errno));
		return strdup("0.0.0.0");
	}

	/* Get IP of internal interface */
	memset(&if_data, 0, sizeof(if_data));
	strncpy(if_data.ifr_name, ifname, sizeof(if_data.ifr_name) - 1);

	/* Get the IP address */
	if (ioctl(sockd, SIOCGIFADDR, &if_data) < 0) {
		debug(LOG_DEBUG, "ioctl(): SIOCGIFADDR for [%s] failed (%s), fallback to 0.0.0.0",
		      ifname, strerror(errno));
		close(sockd);
		return strdup("0.0.0.0");
	}
	memcpy((void *)&ip, (void *)&if_data.ifr_addr.sa_data + 2, 4);
	in.s_addr = ip;

	close(sockd);
	ip_str = malloc(HTTP_IP_ADDR_LEN);
	if (ip_str && inet_ntop(AF_INET, &in, ip_str, HTTP_IP_ADDR_LEN))
		return ip_str;

	if (ip_str) free(ip_str);
	return strdup("0.0.0.0");
}

static void
__list_add(iqueue_head *entry,
	iqueue_head *prev,
	iqueue_head *next)
{
	next->prev = entry;
	entry->next = next;
	entry->prev = prev;
	prev->next = entry;
}

static void
__list_del(iqueue_head *prev, iqueue_head *next)
{
	next->prev = prev;
	prev->next = next;
}

/* conv -> task index backed by scoped hash.
 * Scoped by tunnel pointer to guarantee multi-tunnel isolation. */
static jwHashTable *conv_hash = NULL;

static void conv_build_key(char *key, size_t klen, IUINT32 conv,
			   const struct sockaddr_in *peer, void *tunnel)
{
	if (peer)
		snprintf(key, klen, "s:%p:%u:%u:%u", tunnel,
			 ntohl(peer->sin_addr.s_addr), ntohs(peer->sin_port),
			 conv);
	else
		snprintf(key, klen, "c:%p:%u", tunnel, conv);
}

void
add_task_tail(struct xkcp_task *task, iqueue_head *head) {
	iqueue_head *entry = &task->head;

	__list_add(entry, head->prev, head);

	if (!task->kcp)
		return;
	task->conv = task->kcp->conv;
	if (!conv_hash)
		conv_hash = create_hash(1024);
	char key[64];
	conv_build_key(key, sizeof(key), task->conv,
		       task->user_owned ? task->sockaddr : NULL,
		       task->tunnel);
	add_ptr_by_str(conv_hash, key, task);
}

void
del_task(struct xkcp_task *task) {
	iqueue_head *entry = &task->head;

	__list_del(entry->prev, entry->next);

	if (conv_hash && task->conv) {
		char key[64];
		struct xkcp_task *cur = NULL;

		conv_build_key(key, sizeof(key), task->conv,
			       task->user_owned ? task->sockaddr : NULL,
			       task->tunnel);
		if (get_ptr_by_str(conv_hash, key, (void **)&cur) == HASHOK &&
		    cur == task)
			del_by_str(conv_hash, key);
	}
}

struct xkcp_task *xkcp_find_task(IUINT32 conv, const struct sockaddr_in *peer, void *tunnel)
{
	if (!conv_hash || conv == 0)
		return NULL;

	char key[64];
	struct xkcp_task *task = NULL;
	conv_build_key(key, sizeof(key), conv, peer, tunnel);
	if (get_ptr_by_str(conv_hash, key, (void **)&task) == HASHOK)
		return task;
	return NULL;
}

struct fec_send_ctx {
	int fd;
	const struct sockaddr_in *addr;
	struct xkcp_tunnel *tunnel;
};

static void fec_send_pkt(void *user, const char *pkt, int len)
{
	struct fec_send_ctx *ctx = user;
	int nret = sendto(ctx->fd, pkt, len, 0, (struct sockaddr *)ctx->addr,
			  sizeof(*ctx->addr));

	if (nret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
		xkcp_enqueue_udp_at(ctx->fd, ctx->addr, pkt, len, ctx->tunnel);
	else if (nret < 0)
		debug(LOG_ERR, "fec sendto: %s", strerror(errno));
}

void xkcp_send_tunnel_packet(int fd, const struct sockaddr_in *addr, struct fec_conn *fec,
			     const char *buf, int len, struct xkcp_tunnel *tunnel)
{
	if (!buf || len <= 0 || !addr) return;

	if (fec) {
		struct fec_send_ctx ctx = { fd, addr, tunnel };
		fec_conn_encode(fec, buf, len, fec_send_pkt, &ctx);
		return;
	}

	int nret = sendto(fd, buf, len, 0, (struct sockaddr *)addr, sizeof(*addr));
	if (nret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
		xkcp_enqueue_udp_at(fd, addr, buf, len, tunnel);
	} else if (nret < 0) {
		debug(LOG_ERR, "sendto: %s", strerror(errno));
	}
}

/* ---- UDP egress queue ------------------------------------------------ */
static struct evbuffer *g_udp_pend = NULL;
static struct event *g_udp_wev = NULL;
static int g_udp_wev_active = 0;

void xkcp_set_event_base(struct event_base *base)
{
	g_evbase = base;
}

#define UDP_PEND_HDR	(sizeof(struct sockaddr_in) + 2)
#define UDP_PEND_MAX	(4 * 1024 * 1024)

static void udp_pend_drain_cb(evutil_socket_t fd, short what, void *arg)
{
	struct xkcp_tunnel *tunnel = arg;
	struct evbuffer **p_pend = tunnel ? &tunnel->udp_pend : &g_udp_pend;
	struct event **p_wev = tunnel ? &tunnel->udp_wev : &g_udp_wev;
	int *p_active = tunnel ? &tunnel->udp_wev_active : &g_udp_wev_active;
	char buf[2048];

	(void)what;

	if (!*p_pend) return;

	while (evbuffer_get_length(*p_pend) >= UDP_PEND_HDR) {
		struct sockaddr_in sa;
		uint16_t len;
		int nret;

		evbuffer_remove(*p_pend, &sa, sizeof(sa));
		evbuffer_remove(*p_pend, &len, sizeof(len));
		if (len > sizeof(buf)) {
			evbuffer_drain(*p_pend, len);
			continue;
		}
		evbuffer_remove(*p_pend, buf, len);

		nret = sendto(fd, buf, len, 0, (struct sockaddr *)&sa, sizeof(sa));
		if (nret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			evbuffer_prepend(*p_pend, buf, len);
			evbuffer_prepend(*p_pend, &len, sizeof(len));
			evbuffer_prepend(*p_pend, &sa, sizeof(sa));
			return;
		}
	}

	if (*p_active && *p_wev) {
		event_del(*p_wev);
		*p_active = 0;
	}
}

void xkcp_enqueue_udp_at(evutil_socket_t fd, const struct sockaddr_in *sa,
			 const char *buf, int len, struct xkcp_tunnel *tunnel)
{
	uint16_t l = (uint16_t)len;
	struct evbuffer **p_pend = tunnel ? &tunnel->udp_pend : &g_udp_pend;
	struct event **p_wev = tunnel ? &tunnel->udp_wev : &g_udp_wev;
	int *p_active = tunnel ? &tunnel->udp_wev_active : &g_udp_wev_active;
	struct event_base *base = tunnel ? tunnel->base : g_evbase;

	if (!*p_pend) {
		*p_pend = evbuffer_new();
		if (!*p_pend) return;
		*p_wev = event_new(base, fd, EV_WRITE|EV_PERSIST, udp_pend_drain_cb, tunnel);
	}

	if (evbuffer_get_length(*p_pend) > UDP_PEND_MAX)
		return;

	evbuffer_add(*p_pend, sa, sizeof(*sa));
	evbuffer_add(*p_pend, &l, sizeof(l));
	evbuffer_add(*p_pend, buf, len);

	if (!*p_active && *p_wev) {
		event_add(*p_wev, NULL);
		*p_active = 1;
	}
}

void xkcp_fec_tick(struct xkcp_proxy_param *ptr)
{
	if (!ptr || !ptr->fec)
		return;

	struct fec_send_ctx ctx = { ptr->xkcpfd, &ptr->sockaddr, ptr->tunnel };
	fec_conn_tick(ptr->fec, fec_send_pkt, &ctx);
}

void xkcp_fec_conn_tick(struct fec_conn *fec, int fd, const struct sockaddr_in *addr, struct xkcp_tunnel *tunnel)
{
	if (!fec || !addr)
		return;

	struct fec_send_ctx ctx = { fd, addr, tunnel };
	fec_conn_tick(fec, fec_send_pkt, &ctx);
}

static int xkcp_output(const char *buf, int len, ikcpcb *kcp, void *user)
{
	struct xkcp_proxy_param *ptr = user;
	int nret;

	if (ptr->fec) {
		struct fec_send_ctx ctx = { ptr->xkcpfd, &ptr->sockaddr, ptr->tunnel };
		fec_conn_encode(ptr->fec, buf, len, fec_send_pkt, &ctx);
		return len;
	}

	nret = sendto(ptr->xkcpfd, buf, len, 0, (struct sockaddr *)&ptr->sockaddr, sizeof(ptr->sockaddr));
	if (nret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
		xkcp_enqueue_udp_at(ptr->xkcpfd, &ptr->sockaddr, buf, len, ptr->tunnel);
		return len;
	}
	if (nret < 0)
		debug(LOG_ERR, "xkcp_output conv [%u] fd [%d] sendto: %s",
			  kcp->conv, ptr->xkcpfd, strerror(errno));

	return nret;
}

void xkcp_set_tunnel_config_param(ikcpcb *kcp, struct xkcp_param *param)
{
	kcp->output = xkcp_output;
	ikcp_wndsize(kcp, param->sndwnd, param->rcvwnd);
	kcp->loss_wnd = (param->loss_ctrl != 0) ? kcp->snd_wnd : 0;
	kcp->pacing = param->pacing;
	ikcp_nodelay(kcp, param->nodelay, param->interval, param->resend, param->nc);
	if (param->mtu > 0) {
		int mtu = param->mtu;
		if (param->fec && mtu > FEC_HDR_SIZE)
			mtu -= FEC_HDR_SIZE;
		ikcp_setmtu(kcp, mtu);
	}
}

void xkcp_set_tcp_nodelay(int fd)
{
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

void xkcp_apply_sockbuf_param(int fd, struct xkcp_param *param)
{
	if (!param) return;

	if (param->dscp > 0) {
		int tos = (param->dscp << 2) & 0xFF;
		if (setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos)) < 0)
			debug(LOG_WARNING, "setsockopt IP_TOS [dscp=%d] failed: %s", param->dscp, strerror(errno));
	}

	int bufsz = param->sock_buf;
	if (bufsz <= 0)
		return;

	if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz)) < 0)
		debug(LOG_WARNING, "setsockopt SO_RCVBUF [%d] failed: %s", bufsz, strerror(errno));
	if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz)) < 0)
		debug(LOG_WARNING, "setsockopt SO_SNDBUF [%d] failed: %s", bufsz, strerror(errno));
}

void *xkcp_tcp_event_cb(struct bufferevent *bev, short what, struct xkcp_task *task)
{
	void *puser = NULL;

	if (!task)
		return NULL;

	if (what & (BEV_EVENT_EOF|BEV_EVENT_ERROR)) {
		debug(LOG_INFO, "tcp closed conv [%u] what [%d] fd [%d]",
			  task->conv, what, bufferevent_getfd(bev));
		if (task->kcp) {
			ikcp_send(task->kcp, XKCP_CLOSE_SIGNAL, XKCP_CLOSE_SIGNAL_LEN);
			/* ikcp_flush alone is a no-op before the first ikcp_update */
			ikcp_update(task->kcp, iclock());
			puser = task->user_owned ? task->kcp->user : NULL;
			ikcp_release(task->kcp);
			task->kcp = NULL;
		}
		del_task(task);
		bufferevent_free(bev);
		free(task);
	}
	return puser;
}

#define XKCP_SND_QUE_HIGH	256	/* queued KCP segments */
#define XKCP_SND_QUE_LOW	64

/* teardown a session when data is in flight but the peer has gone completely
 * silent (no data, no ACKs) for this long */
#define XKCP_DEAD_LINK_TIMEOUT_MS	30000

void xkcp_tcp_read_cb(struct bufferevent *bev, ikcpcb *kcp)
{
	char buf[2048];
	int len, nret;
	struct evbuffer *input = bufferevent_get_input(bev);
	int snd_que_high = (kcp->snd_wnd > 256) ? (int)kcp->snd_wnd : 256;

	if (kcp->nsnd_que > snd_que_high) {
		bufferevent_disable(bev, EV_READ);
		return;
	}

	while ((len = evbuffer_remove(input, buf, sizeof(buf))) > 0) {
		nret = ikcp_send(kcp, buf, len);
		if (nret < 0)
			debug(LOG_INFO, "ikcp_send conv [%u] failed [%d] len [%d]",
				  kcp->conv, nret, len);
		if (kcp->nsnd_que > snd_que_high) {
			bufferevent_disable(bev, EV_READ);
			break;
		}
	}
	ikcp_flush(kcp);
}

void xkcp_update_task_list(iqueue_head *task_list, const struct xkcp_param *param)
{
	iqueue_head *p, *n;

	/* safe iteration: xkcp_forward_data may free the current task */
	for (p = task_list->next, n = p->next; p != task_list; p = n, n = p->next) {
		struct xkcp_task *task = iqueue_entry(p, struct xkcp_task, head);
		IUINT32 now = iclock();
		int alive;

		if (!task->kcp)
			continue;

		/* dead-link teardown: we still have unacked/queued data but the peer
		 * has sent nothing (not even ACKs) for XKCP_DEAD_LINK_TIMEOUT_MS.
		 * Without this, a session whose peer died pumps retransmissions
		 * forever, flooding the link (observed: >100k UDP packets per
		 * zombie conv). kcp->state == (IUINT32)-1 is KCP's own
		 * dead_link verdict (a segment retransmitted IKCP_DEADLINK times). */
		if ((task->kcp->state == (IUINT32)-1) ||
		    ((task->kcp->nsnd_buf > 0 || task->kcp->nsnd_que > 0) &&
		     _itimediff(now, task->last_recv) > (IINT32)XKCP_DEAD_LINK_TIMEOUT_MS)) {
			debug(LOG_WARNING, "dead link: conv [%u] no peer input for %d ms "
				  "with %d/%d segments in flight, tearing down "
				  "(now=%u last_recv=%u state=%u)",
				  task->conv, XKCP_DEAD_LINK_TIMEOUT_MS,
				  (int)task->kcp->nsnd_buf, (int)task->kcp->nsnd_que,
				  now, task->last_recv, task->kcp->state);
			xkcp_teardown_dead_task(task);
			continue;
		}

		/* in-band keepalive when the session has gone quiet */
		if (param && param->keepalive > 0) {
			IUINT32 idle_ms = (IUINT32)param->keepalive * 1000;
			if (_itimediff(now, task->last_active) > (IINT32)idle_ms &&
			    _itimediff(now, task->last_keepalive) > (IINT32)idle_ms) {
				ikcp_send(task->kcp, XKCP_NOP_SIGNAL, XKCP_NOP_SIGNAL_LEN);
				task->last_keepalive = now;
			}
		}

		ikcp_update(task->kcp, now);
		alive = xkcp_forward_data(task);

		if (alive && task->kcp)
			xkcp_fec_tick((struct xkcp_proxy_param *)task->kcp->user);

		if (alive && task->bev && task->kcp) {
			int snd_que_low = task->kcp->snd_wnd > 256 ? (int)(task->kcp->snd_wnd / 4) : XKCP_SND_QUE_LOW;
			if (task->kcp->nsnd_que < snd_que_low &&
			    !(bufferevent_get_enabled(task->bev) & EV_READ)) {
				bufferevent_enable(task->bev, EV_READ);
			}
		}
	}
}

/* tear down a session whose peer has gone silent while data is in flight.
 * the local TCP side is closed immediately (client sees a reset), a close
 * signal is sent best-effort (usually lost, since the link is dead), and the
 * kcp session is released so retransmissions stop. */
void xkcp_teardown_dead_task(struct xkcp_task *task)
{
	if (task->bev) {
		bufferevent_free(task->bev);
		task->bev = NULL;
	}
	del_task(task);
	if (task->kcp) {
		ikcp_send(task->kcp, XKCP_CLOSE_SIGNAL, XKCP_CLOSE_SIGNAL_LEN);
		ikcp_update(task->kcp, iclock());
		void *puser = task->kcp->user;
		ikcp_release(task->kcp);
		task->kcp = NULL;
		if (task->user_owned && puser)
			free(puser);
	}
	free(task);
}

static void xkcp_bev_drain_cb(struct bufferevent *bev, void *ctx)
{
	(void)ctx;
	struct evbuffer *output = bufferevent_get_output(bev);
	if (evbuffer_get_length(output) == 0) {
		bufferevent_free(bev);
	}
}

static void xkcp_bev_drain_event_cb(struct bufferevent *bev, short what, void *ctx)
{
	(void)what;
	(void)ctx;
	bufferevent_free(bev);
}

#define XKCP_TCP_OUTBUF_LIMIT	(4 * 1024 * 1024)

/* tear a session down: notify peer, unlink and release everything.
 * used for handshake/auth rejects where no TCP payload is pending. */
static int forward_close_session(struct xkcp_task *task)
{
	del_task(task);
	if (task->bev) {
		bufferevent_free(task->bev);
		task->bev = NULL;
	}
	if (task->kcp) {
		ikcp_send(task->kcp, XKCP_CLOSE_SIGNAL, XKCP_CLOSE_SIGNAL_LEN);
		/* ikcp_flush alone is a no-op before the first ikcp_update
		 * (kcp->updated == 0), e.g. for sessions rejected on the very
		 * first packet; ikcp_update both marks updated and flushes */
		ikcp_update(task->kcp, iclock());
		void *puser = task->kcp->user;
		ikcp_release(task->kcp);
		task->kcp = NULL;
		if (task->user_owned && puser)
			free(puser);
	}
	free(task);
	return 0;
}

int xkcp_forward_data(struct xkcp_task *task)
{
	char sbuf[OBUF_SIZE];

	if (task->bev &&
	    evbuffer_get_length(bufferevent_get_output(task->bev)) >
	    XKCP_TCP_OUTBUF_LIMIT)
		return 1;

	while (1) {
		int peeksize = ikcp_peeksize(task->kcp);
		if (peeksize < 0)
			break;

		char *obuf = sbuf;
		char *heapbuf = NULL;
		if (peeksize > (int)sizeof(sbuf)) {
			heapbuf = malloc(peeksize);
			if (!heapbuf) {
				debug(LOG_ERR, "conv [%u] alloc %d bytes for recv failed",
					  task->kcp->conv, peeksize);
				break;
			}
			obuf = heapbuf;
		}

		int nrecv = ikcp_recv(task->kcp, obuf, peeksize);
		if (nrecv < 0) {
			free(heapbuf);
			break;
		}

		/* any in-band message (data or ACK) counts as peer activity */
		task->last_active = task->last_recv = iclock();

		if (nrecv == XKCP_CLOSE_SIGNAL_LEN &&
			memcmp(obuf, XKCP_CLOSE_SIGNAL, XKCP_CLOSE_SIGNAL_LEN) == 0) {
			debug(LOG_INFO, "conv [%u] received close signal, closing tcp connection", task->conv);
			free(heapbuf);
			if (task->bev) {
				struct evbuffer *out = bufferevent_get_output(task->bev);
				if (evbuffer_get_length(out) > 0) {
					bufferevent_disable(task->bev, EV_READ);
					bufferevent_setcb(task->bev, NULL, xkcp_bev_drain_cb, xkcp_bev_drain_event_cb, NULL);
				} else {
					bufferevent_free(task->bev);
				}
				task->bev = NULL;
			}
			del_task(task);
			if (task->user_owned && task->kcp) {
				void *puser = task->kcp->user;
				ikcp_release(task->kcp);
				if (puser) free(puser);
			} else if (task->kcp) {
				ikcp_release(task->kcp);
			}
			free(task);
			return 0;
		}

		/* Server side: perform dynamic destination handshake if not done yet */
		if (task->user_owned && !task->handshake_done) {
			char target_host[256] = {0};
			uint16_t target_port = 0;
			uint8_t ver = 0;
			const uint8_t *ts = NULL;
			const uint8_t *token = NULL;
			int hdr_len = xkcp_proto_decode_header_ex(obuf, nrecv, target_host,
					sizeof(target_host), &target_port, &ver, &ts, &token);
			int (*conn_fn)(struct xkcp_task *, const char *, uint16_t) =
				task->tunnel ? task->tunnel->connect_target : NULL;

			if (hdr_len > 0 && ver >= XKCP_PROTO_VER_2) {
				const char *key = (task->tunnel && task->tunnel->param.key) ?
					task->tunnel->param.key : xkcp_get_param()->key;
				uint32_t now_sec = (uint32_t)(iclock64() / 1000);

				task->handshake_done = 1;
				if (xkcp_auth_verify(key, task->conv, target_host, target_port,
						     ts, token, now_sec) < 0) {
					debug(LOG_WARNING, "[%s] conv [%u] authentication failed for target [%s]:[%u]",
					      task->tunnel ? task->tunnel->name : "server",
					      task->conv, target_host, target_port);
					free(heapbuf);
					return forward_close_session(task);
				}
				snprintf(task->target_host, sizeof(task->target_host), "%s", target_host);
				task->target_port = target_port;
				if (!conn_fn || conn_fn(task, target_host, target_port) < 0) {
					debug(LOG_ERR, "[%s] conv [%u] connect to dynamic target [%s]:[%u] failed",
					      task->tunnel ? task->tunnel->name : "server", task->conv, target_host, target_port);
					free(heapbuf);
					return forward_close_session(task);
				}
				if (nrecv > hdr_len && task->bev) {
					evbuffer_add(bufferevent_get_output(task->bev), obuf + hdr_len, nrecv - hdr_len);
				}
			} else if (hdr_len > 0) {
				debug(LOG_WARNING, "[%s] conv [%u] legacy unauthenticated dynamic header rejected",
				      task->tunnel ? task->tunnel->name : "server", task->conv);
				free(heapbuf);
				return forward_close_session(task);
			} else {
				/* Fallback to the tunnel's static backend, if any */
				task->handshake_done = 1;
				const char *fb_host = task->tunnel && task->tunnel->param.remote_addr &&
				                      task->tunnel->param.remote_addr[0] ?
						      task->tunnel->param.remote_addr : NULL;
				uint16_t fb_port = task->tunnel ?
				                   (uint16_t)task->tunnel->param.remote_port : 0;

				if (!fb_host || fb_port == 0) {
					debug(LOG_WARNING, "[%s] conv [%u] no dynamic header and no static backend, dropping",
					      task->tunnel ? task->tunnel->name : "server", task->conv);
					free(heapbuf);
					return forward_close_session(task);
				}
				snprintf(task->target_host, sizeof(task->target_host), "%s", fb_host);
				task->target_port = fb_port;
				if (!conn_fn || conn_fn(task, fb_host, fb_port) < 0) {
					debug(LOG_ERR, "[%s] conv [%u] connect to fallback target [%s]:[%u] failed",
					      task->tunnel ? task->tunnel->name : "server", task->conv, fb_host, fb_port);
					free(heapbuf);
					return forward_close_session(task);
				}
				if (task->bev) {
					evbuffer_add(bufferevent_get_output(task->bev), obuf, nrecv);
				}
			}
			free(heapbuf);
			task->last_keepalive = task->last_active = iclock();
			continue;
		}

		/* in-band keepalive: activity only, never forwarded to TCP */
		if (nrecv == XKCP_NOP_SIGNAL_LEN &&
			memcmp(obuf, XKCP_NOP_SIGNAL, XKCP_NOP_SIGNAL_LEN) == 0) {
			free(heapbuf);
			continue;
		}

		if (task->bev)
			evbuffer_add(bufferevent_get_output(task->bev), obuf, nrecv);

		free(heapbuf);
		task->last_keepalive = task->last_active = iclock();
	}

	return 1;
}

void set_timer_interval_ms(struct event *timeout, int interval_ms)
{
	struct timeval tv;
	if (interval_ms <= 0) interval_ms = 20;
	tv.tv_sec = interval_ms / 1000;
	tv.tv_usec = (interval_ms % 1000) * 1000;
	evtimer_add(timeout, &tv);
}

int get_queue_size(iqueue_head *head)
{
	if (!head || !head->next) return 0;
	int count = 0;
	iqueue_head *p;
	for (p = head->next; p && p != head; p = p->next)
		count++;
	return count;
}

int get_task_list_size(iqueue_head *task_list)
{
	return get_queue_size(task_list);
}

void dump_task_list(iqueue_head *task_list, struct bufferevent *bev)
{
	struct xkcp_task *task;
	struct evbuffer *output = bufferevent_get_output(bev);
	int count = 0;
	iqueue_foreach(task, task_list, xkcp_task_type, head) {
		count++;
		if (task->target_port > 0) {
			evbuffer_add_printf(output, "\t[%d] conv [%u] -> [%s]:[%u]\n",
					    count, task->conv, task->target_host, task->target_port);
		} else {
			evbuffer_add_printf(output, "\t[%d] conv [%u]\n", count, task->conv);
		}
	}
	if (count == 0)
		evbuffer_add_printf(output, "\tno active connections\n");
}

void xkcp_task_check_timeout_val(iqueue_head *task_list, int timeout_sec)
{
	if (timeout_sec <= 0)
		return;

	IUINT32 now = iclock();
	IUINT32 timeout_ms = (IUINT32)timeout_sec * 1000;
	iqueue_head *p, *n;

	for (p = task_list->next, n = p->next; p != task_list; p = n, n = p->next) {
		struct xkcp_task *task = iqueue_entry(p, struct xkcp_task, head);
		if (_itimediff(now, task->last_active) > (IINT32)timeout_ms) {
			debug(LOG_INFO, "task conv [%u] timed out after %d seconds, closing",
			      task->conv, timeout_sec);
			if (task->bev)
				bufferevent_free(task->bev);
			del_task(task);
			if (task->kcp) {
				void *puser = task->user_owned ? task->kcp->user : NULL;
				ikcp_release(task->kcp);
				if (puser) free(puser);
			}
			free(task);
		}
	}
}

static struct event *g_sigterm_ev = NULL;
static struct event *g_sigint_ev = NULL;

static void sigterm_cb(evutil_socket_t sig, short events, void *user_data)
{
	struct event_base *base = user_data;
	debug(LOG_INFO, "Caught signal %d, shutting down", sig);
	event_base_loopbreak(base);
}

void xkcp_setup_signals(struct event_base *base)
{
	g_sigterm_ev = evsignal_new(base, SIGTERM, sigterm_cb, base);
	g_sigint_ev = evsignal_new(base, SIGINT, sigterm_cb, base);
	if (g_sigterm_ev)
		event_add(g_sigterm_ev, NULL);
	if (g_sigint_ev)
		event_add(g_sigint_ev, NULL);
}

void xkcp_cleanup_signals(void)
{
	if (g_sigterm_ev) {
		event_del(g_sigterm_ev);
		event_free(g_sigterm_ev);
		g_sigterm_ev = NULL;
	}
	if (g_sigint_ev) {
		event_del(g_sigint_ev);
		event_free(g_sigint_ev);
		g_sigint_ev = NULL;
	}
}

void xkcp_cleanup_udp_queue(void)
{
	if (g_udp_wev) {
		event_del(g_udp_wev);
		event_free(g_udp_wev);
		g_udp_wev = NULL;
		g_udp_wev_active = 0;
	}
	if (g_udp_pend) {
		evbuffer_free(g_udp_pend);
		g_udp_pend = NULL;
	}
	if (conv_hash) {
		delete_hash(conv_hash, NULL, HASHSTRING, HASHPTR);
		conv_hash = NULL;
	}
}

int xkcp_main(int argc, char **argv)
{
	config_init();
	parse_commandline(argc, argv);

	xkcp_apply_mode();

	if (xkcp_get_config()->syslog) {
		debugconf.log_syslog = 1;
	}

	if (xkcp_get_config()->daemon) {
		if (daemon(1, 1)) {
			debug(LOG_ERR, "daemon failed: %s", strerror(errno));
			exit(EXIT_FAILURE);
		}
	}

	if (!xkcp_get_config()->main_loop) {
		debug(LOG_ERR, "main_loop is NULL!");
		return 1;
	}

	int ret = xkcp_get_config()->main_loop();
	xkcp_free_config();
	return ret;
}
