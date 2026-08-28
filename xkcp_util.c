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


#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>

#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/types.h>		  /* See NOTES */
#include <sys/socket.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/tcp.h>

#include <event2/event.h>
#include <event2/event_struct.h>
#include <event2/bufferevent_ssl.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <event2/util.h>

#include <syslog.h>

#include "ikcp.h"
#include "fec.h"
#include "jwHash.h"
#include "xkcp_util.h"
#include "xkcp_config.h"
#include "xkcp_mon.h"
#include "commandline.h"
#include "debug.h"

#include <signal.h>

struct event_base *g_exit_base = NULL;

static void sigterm_cb(evutil_socket_t sig, short events, void *arg)
{
	debug(LOG_INFO, "Caught signal %d, shutting down", sig);
	struct event_base *base = arg;
	event_base_loopexit(base, NULL);
}

static int task_list_count = 0;

int get_task_list_count()
{
	return task_list_count;
}

void itimeofday(long *sec, long *usec)
{
	struct timeval time;
	gettimeofday(&time, NULL);
	if (sec) *sec = time.tv_sec;
	if (usec) *usec = time.tv_usec;
}

/* get clock in millisecond 64 */
IINT64 iclock64(void)
{
	long s, u;
	IINT64 value;
	itimeofday(&s, &u);
	value = ((IINT64)s) * 1000 + (u / 1000);
	return value;
}

IUINT32 iclock()
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

	/* Create a socket */
	if ((sockd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
		debug(LOG_ERR, "socket(): %s", strerror(errno));
		return NULL;
	}

	/* Get IP of internal interface */
	strncpy(if_data.ifr_name, ifname, 15);
	if_data.ifr_name[15] = '\0';

	/* Get the IP address */
	if (ioctl(sockd, SIOCGIFADDR, &if_data) < 0) {
		debug(LOG_ERR, "ioctl(): SIOCGIFADDR %s", strerror(errno));
		close(sockd);
		return NULL;
	}
	memcpy((void *)&ip, (void *)&if_data.ifr_addr.sa_data + 2, 4);
	in.s_addr = ip;

	close(sockd);
	ip_str = malloc(HTTP_IP_ADDR_LEN);
	memset(ip_str, 0, HTTP_IP_ADDR_LEN);
	if(ip_str&&inet_ntop(AF_INET, &in, ip_str, HTTP_IP_ADDR_LEN))
		return ip_str;

	if (ip_str) free(ip_str);
	return NULL;
}

static void
__list_add(iqueue_head *entry, iqueue_head *prev, iqueue_head *next)
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

/* conv -> task index: avoids the O(n) list scan per UDP packet.
 * Keys are scoped ("c:<conv>" client-side, "s:<ip>:<port>:<conv>" on the
 * server) so identical convs from different peers can never collide. */
static jwHashTable *conv_hash = NULL;

static void conv_build_key(char *key, size_t klen, IUINT32 conv,
			   const struct sockaddr_in *peer)
{
	if (peer)
		snprintf(key, klen, "s:%u:%u:%u",
			 ntohl(peer->sin_addr.s_addr), ntohs(peer->sin_port),
			 conv);
	else
		snprintf(key, klen, "c:%u", conv);
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
	char key[48];
	conv_build_key(key, sizeof(key), task->conv,
		       task->user_owned ? task->sockaddr : NULL);
	add_ptr_by_str(conv_hash, key, task);
}

void
del_task(struct xkcp_task *task) {
	iqueue_head *entry = &task->head;

	__list_del(entry->prev, entry->next);

	if (conv_hash && task->kcp) {
		char key[48];
		struct xkcp_task *cur = NULL;

		conv_build_key(key, sizeof(key), task->conv,
			       task->user_owned ? task->sockaddr : NULL);
		/* only drop the index when it still points at us: a new
		 * task with the same key may have replaced our entry */
		if (get_ptr_by_str(conv_hash, key, (void **)&cur) == HASHOK &&
		    cur == task)
			del_by_str(conv_hash, key);
	}
}

struct fec_send_ctx {
	int fd;
	struct sockaddr_in *addr;
};

static void fec_send_pkt(void *user, const char *pkt, int len)
{
	struct fec_send_ctx *ctx = user;

	if (sendto(ctx->fd, pkt, len, 0, (struct sockaddr *)ctx->addr,
		   sizeof(*ctx->addr)) < 0)
		debug(LOG_ERR, "fec sendto: %s", strerror(errno));
}

/* periodic FEC tick: flush parity for stale partial groups and adapt the
 * parity ratio to the observed loss rate */
void xkcp_fec_tick(struct xkcp_proxy_param *ptr)
{
	if (!ptr || !ptr->fec)
		return;

	struct fec_send_ctx ctx = { ptr->xkcpfd, &ptr->sockaddr };
	fec_conn_tick(ptr->fec, fec_send_pkt, &ctx);
}

static int xkcp_output(const char *buf, int len, ikcpcb *kcp, void *user)
{
	struct xkcp_proxy_param *ptr = user;
	int nret;

	if (ptr->fec) {
		/* frame + (at group end) parity-protect the segment */
		struct fec_send_ctx ctx = { ptr->xkcpfd, &ptr->sockaddr };
		fec_conn_encode(ptr->fec, buf, len, fec_send_pkt, &ctx);
		return len;
	}

	nret = sendto(ptr->xkcpfd, buf, len, 0, (struct sockaddr *)&ptr->sockaddr, sizeof(ptr->sockaddr));
	if (nret < 0)
		debug(LOG_ERR, "xkcp_output conv [%u] fd [%d] sendto: %s",
			  kcp->conv, ptr->xkcpfd, strerror(errno));

	return nret;
}

void xkcp_set_config_param(ikcpcb *kcp)
{
	struct xkcp_param *param = xkcp_get_param();
	kcp->output	= xkcp_output;
	ikcp_wndsize(kcp, param->sndwnd, param->rcvwnd);
	/* loss-driven AIMD starts unrestricted and adapts down on loss */
	kcp->loss_wnd = (param->loss_ctrl != 0) ? kcp->snd_wnd : 0;
	ikcp_nodelay(kcp, param->nodelay, param->interval, param->resend, param->nc);
	/* FEC frames add an 8-byte header to every datagram: shrink the KCP
	 * mtu accordingly so framed packets stay within the path MTU. */
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

void xkcp_apply_sockbuf(int fd)
{
	struct xkcp_param *param = xkcp_get_param();
	int bufsz = param->sock_buf;

	if (bufsz <= 0)
		return;

	/* The kernel doubles the requested value and caps it at net.core.*mem_max;
	 * failures are non-fatal, we just keep the system defaults. */
	if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz)) < 0)
		debug(LOG_WARNING, "setsockopt SO_RCVBUF [%d] failed: %s", bufsz, strerror(errno));
	if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz)) < 0)
		debug(LOG_WARNING, "setsockopt SO_SNDBUF [%d] failed: %s", bufsz, strerror(errno));
}


void *xkcp_tcp_event_cb(struct bufferevent *bev, short what, struct xkcp_task *task)
{
	void *puser = NULL;
	if (what & (BEV_EVENT_EOF|BEV_EVENT_ERROR)) {
		/* Unbind callbacks first: bufferevents created with
		 * BEV_OPT_DEFER_CALLBACKS may still have queued callbacks that
		 * would otherwise run against a freed task (ctx). */
		bufferevent_setcb(bev, NULL, NULL, NULL, NULL);
		if (task) {
			puser = task->kcp->user;
			debug(LOG_INFO, "tcp closed conv [%u] what [%d] fd [%d]",
				  task->kcp->conv, what, bufferevent_getfd(bev));
			if (task->bev != bev) {
				bufferevent_free(task->bev);
				debug(LOG_ERR, "impossible here\n");
			}
			ikcp_send(task->kcp, XKCP_CLOSE_SIGNAL, XKCP_CLOSE_SIGNAL_LEN);
			ikcp_flush(task->kcp);
			ikcp_release(task->kcp);
			del_task(task);
			free(task);
		}
		bufferevent_free(bev);
	} else if (what & BEV_EVENT_CONNECTED) {
		xkcp_set_tcp_nodelay(bufferevent_getfd(bev));
	}

	return puser;
}

void xkcp_tcp_read_cb(struct bufferevent *bev, ikcpcb *kcp)
{
	char buf[2048];
	int  len, nret;
	struct evbuffer *input = bufferevent_get_input(bev);
	while ((len = evbuffer_remove(input, buf, sizeof(buf))) > 0) {
		nret = ikcp_send(kcp, buf, len);
		if (nret < 0)
			debug(LOG_INFO, "ikcp_send conv [%u] failed [%d] len [%d]",
				  kcp->conv, nret, len);
	}
	ikcp_flush(kcp);
}

static void dump_task(struct xkcp_task *task, struct bufferevent *bev, int index) {
	struct evbuffer *output = bufferevent_get_output(bev);
	ikcpcb *kcp = task->kcp;
	IUINT32 inflight = kcp->snd_nxt - kcp->snd_una;
	int retrans_pct = kcp->snd_pkts > 0
			  ? (int)(kcp->loss_pkts * 100 / kcp->snd_pkts) : 0;
	evbuffer_add_printf(output,
			"[%d]\t connection [%d]\t conv [%u]:\n --->state [%d] nrcv_buf [%d] "
			"nsnd_buf [%d] nrcv_que [%d] nsnd_que [%d] rcv_nxt [%d] probe [%d] "
			"peek  [%d] stream [%d]\n"
			"      srtt [%d ms] rto [%d ms] inflight [%u] cwnd [%u] loss_wnd [%u] "
			"snd_pkts [%u] loss_pkts [%u] retrans [%d%%]\n",
			index, bufferevent_getfd(task->bev), kcp->conv, kcp->state,
			kcp->nrcv_buf, kcp->nsnd_buf, kcp->nrcv_que,
			kcp->nsnd_que, kcp->rcv_nxt, kcp->probe,
			ikcp_peeksize(kcp), kcp->stream,
			kcp->rx_srtt, kcp->rx_rto, inflight, kcp->cwnd, kcp->loss_wnd,
			kcp->snd_pkts, kcp->loss_pkts, retrans_pct);
}

int get_task_list_size(iqueue_head *task_list)
{
	struct xkcp_task *task;
	int num = 0;
	iqueue_foreach(task, task_list, xkcp_task_type, head) {
		if (task->kcp) {
			num++;
		}
	}

	return num;
}

void dump_task_list(iqueue_head *task_list, struct bufferevent *bev) {
	struct xkcp_task *task;
	task_list_count = 0;
	iqueue_foreach(task, task_list, xkcp_task_type, head) {
		/* skip tasks already torn down: kcp released or bev closed */
		if (task->kcp && task->bev) {
			dump_task(task, bev, ++task_list_count);
		}
	}
}

void xkcp_forward_all_data(iqueue_head *task_list)
{
	struct xkcp_task *task;
	iqueue_foreach(task, task_list, xkcp_task_type, head) {
		if (task->kcp) {
			xkcp_forward_data(task);
		}
	}
}

/* stop draining KCP into TCP when downstream is slower than the tunnel;
 * data stays in the KCP receive queue, its window closes and backpressure
 * propagates to the sender. The periodic timer resumes draining. */
#define XKCP_TCP_OUTBUF_LIMIT	(256 * 1024)

void xkcp_forward_data(struct xkcp_task *task)
{
	char sbuf[OBUF_SIZE];
	IUINT32 now = iclock();

	/* a backed-up connection is still active: update liveness here too,
	 * or the timeout sweep would kill it while it waits for the slow
	 * downstream to drain */
	task->last_active = now;

	if (task->bev &&
	    evbuffer_get_length(bufferevent_get_output(task->bev)) >
	    XKCP_TCP_OUTBUF_LIMIT)
		return;

	while (1) {
		/* Ask KCP for the exact size of the next complete packet so a
		 * payload larger than OBUF_SIZE can never get stuck in the
		 * receive queue (ikcp_recv would return -3 forever). */
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

		if (nrecv == XKCP_CLOSE_SIGNAL_LEN && memcmp(obuf, XKCP_CLOSE_SIGNAL, XKCP_CLOSE_SIGNAL_LEN) == 0) {
			debug(LOG_INFO, "conv [%u] received close signal", task->kcp->conv);
			if (task->bev) {
				bufferevent_free(task->bev);
				task->bev = NULL;
			}
			free(heapbuf);
			break;
		}

		if (task->bev)
			evbuffer_add(bufferevent_get_output(task->bev), obuf, nrecv);

		free(heapbuf);
	}
}

struct xkcp_task *
get_task_from_conv(IUINT32 conv, iqueue_head *task_list)
{
	struct xkcp_task *task;
	iqueue_foreach(task, task_list, xkcp_task_type, head)
		if (task->kcp && task->kcp->conv == conv)
			return task;

	return NULL;
}

struct xkcp_task *
xkcp_find_task(IUINT32 conv, const struct sockaddr_in *peer)
{
	char key[48];
	struct xkcp_task *task = NULL;

	if (!conv_hash)
		return NULL;
	conv_build_key(key, sizeof(key), conv, peer);
	if (get_ptr_by_str(conv_hash, key, (void **)&task) == HASHOK)
		return (task && task->kcp) ? task : NULL;
	return NULL;
}

ikcpcb *
get_kcp_from_conv(IUINT32 conv, iqueue_head *task_list)
{
	struct xkcp_task *task;
	iqueue_foreach(task, task_list, xkcp_task_type, head)
		if (task->kcp && task->kcp->conv == conv) {
			return task->kcp;
		}

	return NULL;
}

int xkcp_main(int argc, char **argv)
{
	struct xkcp_config *config = xkcp_get_config();

	config_init();

	parse_commandline(argc, argv);

	xkcp_apply_mode();

	if (config->main_loop == NULL) {
		debug(LOG_ERR, "should set main_loop firstly");
		exit(EXIT_FAILURE);
	}

	if (config->daemon) {

		debug(LOG_INFO, "Forking into background");

		switch (fork()) {
		case 0:				/* child */
			setsid();
			config->main_loop();
			break;

		default:			   /* parent */
			exit(0);
			break;
		}
	} else {
		config->main_loop();
	}

	return (0);				 /* never reached */
}

void
set_timer_interval(struct event *timeout)
{
	int interval_ms = xkcp_get_param()->interval;
	struct timeval tv;

	if (interval_ms < 1)
		interval_ms = 10;
	evutil_timerclear(&tv);
	tv.tv_sec = interval_ms / 1000;
	tv.tv_usec = (interval_ms % 1000) * 1000;
	event_add(timeout, &tv);
}

void xkcp_update_task_list(iqueue_head *task_list)
{
	struct xkcp_task *task;
	IUINT32 now = iclock();

	iqueue_foreach(task, task_list, xkcp_task_type, head) {
		if (task->kcp)
			ikcp_update(task->kcp, now);
	}
}

void xkcp_timer_event_cb(struct event *timeout, iqueue_head *task_list)
{
	xkcp_update_task_list(task_list);
	xkcp_task_check_timeout(task_list);
	set_timer_interval(timeout);
}

void xkcp_task_check_timeout(iqueue_head *task_list)
{
	int conn_timeout = xkcp_get_param()->conn_timeout;
	if (conn_timeout <= 0)
		return;

	IUINT32 now = iclock();
	IUINT32 timeout_ms = (IUINT32)conn_timeout * 1000;
	struct xkcp_task *task;
	iqueue_head *p, *n;

	for (p = task_list->next; p != task_list; p = n) {
		n = p->next;
		task = iqueue_entry(p, struct xkcp_task, head);
		if (!task->kcp)
			continue;

		IUINT32 idle = now - task->last_active;
		if (idle > timeout_ms) {
			debug(LOG_INFO, "task conv [%u] timed out after %d seconds, closing",
				  task->kcp->conv, conn_timeout);

			if (task->user_owned) {
				struct xkcp_proxy_param *param = (struct xkcp_proxy_param *)task->kcp->user;
				ikcp_send(task->kcp, XKCP_CLOSE_SIGNAL, XKCP_CLOSE_SIGNAL_LEN);
				ikcp_flush(task->kcp);
				ikcp_release(task->kcp);
				if (param)
					free(param);
			} else {
				ikcp_send(task->kcp, XKCP_CLOSE_SIGNAL, XKCP_CLOSE_SIGNAL_LEN);
				ikcp_flush(task->kcp);
				ikcp_release(task->kcp);
			}

			if (task->bev) {
				bufferevent_setcb(task->bev, NULL, NULL, NULL, NULL);
				bufferevent_free(task->bev);
				task->bev = NULL;
			}

			del_task(task);
			free(task);
		}
	}
}

void xkcp_setup_signals(struct event_base *base)
{
	struct event *sigterm_ev = evsignal_new(base, SIGTERM, sigterm_cb, base);
	struct event *sigint_ev = evsignal_new(base, SIGINT, sigterm_cb, base);
	event_add(sigterm_ev, NULL);
	event_add(sigint_ev, NULL);
}

struct evconnlistener *xkcp_create_listener(struct event_base *base, short port, void *ptr)
{
	struct sockaddr_in sin;
	char *addr = get_iface_ip(xkcp_get_param()->local_interface);
	if (!addr) {
		debug(LOG_ERR, "get_iface_ip [%s] failed", xkcp_get_param()->local_interface);
		exit(EXIT_FAILURE);
	}

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = inet_addr(addr);
	sin.sin_port = htons(port);

	struct evconnlistener *listener = evconnlistener_new_bind(base, xkcp_mon_accept_cb, ptr,
		LEV_OPT_CLOSE_ON_FREE|LEV_OPT_CLOSE_ON_EXEC|LEV_OPT_REUSEABLE,
		-1, (struct sockaddr*)&sin, sizeof(sin));
	if (!listener) {
		debug(LOG_ERR, "Couldn't create listener: [%s]", strerror(errno));
		free(addr);
		exit(EXIT_FAILURE);
	}

	free(addr);
	return listener;
}
