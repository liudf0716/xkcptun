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

#include <netdb.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <syslog.h>
#include <signal.h>

#include <event2/event.h>
#include <event2/event_struct.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <event2/util.h>

#include "ikcp.h"
#include "fec.h"
#include "debug.h"
#include "jwHash.h"
#include "xkcp_server.h"
#include "xkcp_config.h"
#include "xkcp_util.h"
#include "xkcp_mon.h"
#include "tcp_client.h"

extern struct event_base *g_exit_base;

static struct fec_conn *get_peer_fec(struct xkcp_tunnel *tunnel, const char *key)
{
	struct fec_conn *f = NULL;
	int cap;

	if (!tunnel || !tunnel->server_fec_hash)
		return NULL;
	if (get_ptr_by_str(tunnel->server_fec_hash, (char *)key, (void **)&f) == HASHOK)
		return f;

	cap = tunnel->param.mtu > 0 ? tunnel->param.mtu : 1350;
	f = fec_conn_new(tunnel->param.data_shard, tunnel->param.parity_shard, cap);
	if (!f) {
		debug(LOG_ERR, "[%s] fec_conn_new failed for [%s], FEC off for this peer",
		      tunnel->name, key);
		return NULL;
	}
	if (add_ptr_by_str(tunnel->server_fec_hash, (char *)key, f) != HASHOK) {
		fec_conn_free(f);
		return NULL;
	}
	return f;
}

void xkcp_server_drop_peer_fec(struct xkcp_tunnel *tunnel, const char *key)
{
	struct fec_conn *f = NULL;

	if (!tunnel || !tunnel->server_fec_hash || !key)
		return;
	if (get_ptr_by_str(tunnel->server_fec_hash, (char *)key, (void **)&f) != HASHOK || !f)
		return;
	del_by_str(tunnel->server_fec_hash, (char *)key);
	fec_conn_free(f);
}

void clean_useless_client(struct xkcp_tunnel *tunnel)
{
	if (!tunnel || !tunnel->server_xkcp_hash)
		return;
	jwHashTable *table = tunnel->server_xkcp_hash;
	for (int i = 0; i < table->buckets; i++) {
		jwHashEntry *entry = table->bucket[i];
		while (entry) {
			jwHashEntry *next = entry->next;
			iqueue_head *list = entry->value.ptrValue;
			if (list && iqueue_is_empty(list)) {
				free(list);
				xkcp_server_drop_peer_fec(tunnel, entry->key.strValue);
				del_by_str(table, entry->key.strValue);
			}
			entry = next;
		}
	}
}

#define FEC_PEER_IDLE_MS	(120 * 1000)

static void sweep_idle_peer_fec(struct xkcp_tunnel *tunnel)
{
	if (!tunnel || !tunnel->server_fec_hash)
		return;
	jwHashTable *fec_hash = tunnel->server_fec_hash;
	for (size_t b = 0; b < fec_hash->buckets; b++) {
		jwHashEntry **pp = &fec_hash->bucket[b];
		while (*pp) {
			jwHashEntry *e = *pp;
			struct fec_conn *f = e->value.ptrValue;

			if (f && fec_conn_idle_ms(f) > FEC_PEER_IDLE_MS) {
				*pp = e->next;
				debug(LOG_INFO, "[%s] reclaimed idle FEC codec for [%s]",
				      tunnel->name, e->key.strValue);
				fec_conn_free(f);
				free(e->key.strValue);
				free(e);
				continue;
			}
			pp = &e->next;
		}
	}
}

static void server_tick_task_list(iqueue_head *task_list)
{
	struct xkcp_task *task;
	iqueue_foreach(task, task_list, xkcp_task_type, head) {
		if (task->kcp)
			xkcp_fec_tick((struct xkcp_proxy_param *)task->kcp->user);
	}
}

static void timer_event_cb(evutil_socket_t fd, short event, void *arg)
{
	struct xkcp_tunnel *tunnel = arg;
	if (!tunnel) return;

	(void)fd;
	(void)event;

	if (tunnel->server_xkcp_hash) {
		hash_iterator(tunnel->server_xkcp_hash, (void*)xkcp_update_task_list, HASHPTR);
		hash_iterator(tunnel->server_xkcp_hash, (void*)xkcp_task_check_timeout, HASHPTR);
		clean_useless_client(tunnel);
		hash_iterator(tunnel->server_xkcp_hash, (void*)server_tick_task_list, HASHPTR);
	}
	sweep_idle_peer_fec(tunnel);

	set_timer_interval_ms(&tunnel->timer_event, tunnel->param.interval);
}

static struct xkcp_task *create_new_tcp_connection(struct xkcp_tunnel *tunnel, const int xkcpfd,
			struct event_base *base, struct sockaddr_in *from, int from_len,
			IUINT32 conv, iqueue_head *task_list)
{
	struct xkcp_proxy_param *param = malloc(sizeof(struct xkcp_proxy_param));
	assert(param);
	memset(param, 0, sizeof(struct xkcp_proxy_param));
	memcpy(&param->sockaddr, from, from_len);
	param->xkcpfd = xkcpfd;
	param->addr_len = from_len;
	param->tunnel = tunnel;

	ikcpcb *kcp_server = ikcp_create(conv, param);
	xkcp_set_tunnel_config_param(kcp_server, &tunnel->param);

	if (tunnel->param.fec) {
		char fkey[32];
		snprintf(fkey, sizeof(fkey), "%u:%u",
			 ntohl(from->sin_addr.s_addr), ntohs(from->sin_port));
		param->fec = get_peer_fec(tunnel, fkey);
	}

	struct xkcp_task *task = malloc(sizeof(struct xkcp_task));
	assert(task);
	task->kcp = kcp_server;		
	task->sockaddr = &param->sockaddr;
	task->last_active = iclock();
	task->user_owned = 1;
	task->tunnel = tunnel;
	
	struct bufferevent *bev = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);
	if (!bev) {
		debug(LOG_ERR, "bufferevent_socket_new failed [%s]", strerror(errno));
		goto err;
	}
	
	task->bev = bev;
	bufferevent_setcb(bev, tcp_client_read_cb, NULL, tcp_client_event_cb, task);
	bufferevent_enable(bev, EV_READ);
	{
		struct sockaddr_in sin;
		memset(&sin, 0, sizeof(sin));
		sin.sin_family = AF_INET;
		sin.sin_port = htons(tunnel->param.remote_port);
		if (inet_aton(tunnel->param.remote_addr, &sin.sin_addr)) {
			if (bufferevent_socket_connect(bev, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
				bufferevent_free(bev);
				debug(LOG_ERR, "bufferevent_socket_connect failed [%s]", strerror(errno));
				goto err;
			}
		} else if (bufferevent_socket_connect_hostname(bev, NULL, AF_INET,
							       tunnel->param.remote_addr,
							       tunnel->param.remote_port) < 0) {
			bufferevent_free(bev);
			debug(LOG_ERR, "bufferevent_socket_connect failed [%s]", strerror(errno));
			goto err;
		}
	}
	add_task_tail(task, task_list);
	debug(LOG_INFO, "[%s] new session conv [%u] -> [%s]:[%d]",
	      tunnel->name, conv, tunnel->param.remote_addr, tunnel->param.remote_port);
	return task;
err:
	if (task->kcp) {
		ikcp_release(task->kcp);
	}
	free(param);
	free(task);
	return NULL;
}

static void server_handle_packet(struct xkcp_tunnel *tunnel, const int xkcpfd,
				 struct event_base *base, char *buf, int nrecv,
				 struct sockaddr_in *from, int from_len)
{
	IUINT32 conv = ikcp_getconv(buf);
	if (conv == 0) return;

	struct xkcp_task *task = xkcp_find_task(conv, from, tunnel);
	if (!task) {
		char ipstr[32];
		snprintf(ipstr, sizeof(ipstr), "%u:%u",
			 ntohl(from->sin_addr.s_addr), ntohs(from->sin_port));

		iqueue_head *task_list = NULL;
		if (get_ptr_by_str(tunnel->server_xkcp_hash, ipstr, (void **)&task_list) != HASHOK || !task_list) {
			task_list = malloc(sizeof(iqueue_head));
			assert(task_list);
			iqueue_init(task_list);
			add_ptr_by_str(tunnel->server_xkcp_hash, ipstr, task_list);
		}
		task = create_new_tcp_connection(tunnel, xkcpfd, base, from, from_len, conv, task_list);
		if (!task) return;
	}

	if (task->kcp) {
		if (ikcp_input(task->kcp, buf, nrecv) < 0)
			debug(LOG_INFO, "[%s] conv [%u] ikcp_input failed", tunnel->name, conv);
		ikcp_flush(task->kcp);
		xkcp_forward_data(task);
	}
}

struct fec_rx_ctx {
	struct xkcp_tunnel *tunnel;
	int xkcpfd;
	struct event_base *base;
	struct sockaddr_in *from;
	int from_len;
};

static void fec_server_deliver_pkt(void *user, const char *pkt, int len)
{
	struct fec_rx_ctx *ctx = user;
	server_handle_packet(ctx->tunnel, ctx->xkcpfd, ctx->base, (char *)pkt, len,
			     ctx->from, ctx->from_len);
}

static void xkcp_server_rcv_cb(const int sock, short int which, void *arg)
{
	struct xkcp_tunnel *tunnel = arg;
	char buf[XKCP_RECV_BUF_LEN];
	struct sockaddr_in from;
	socklen_t from_len;
	int nrecv;

	(void)which;

	while (1) {
		from_len = sizeof(from);
		nrecv = recvfrom(sock, buf, sizeof(buf), 0,
				 (struct sockaddr *)&from, &from_len);
		if (nrecv <= 0)
			break;

		if (tunnel->param.fec) {
			char fkey[32];
			snprintf(fkey, sizeof(fkey), "%u:%u",
				 ntohl(from.sin_addr.s_addr), ntohs(from.sin_port));
			struct fec_conn *f = get_peer_fec(tunnel, fkey);
			if (f) {
				struct fec_rx_ctx ctx = {
					.tunnel = tunnel,
					.xkcpfd = sock,
					.base = tunnel->base,
					.from = &from,
					.from_len = (int)from_len
				};
				fec_conn_decode(f, buf, nrecv, fec_server_deliver_pkt, &ctx);
				continue;
			}
		}

		server_handle_packet(tunnel, sock, tunnel->base, buf, nrecv, &from, (int)from_len);
	}
}

static int set_xkcp_listener(struct xkcp_tunnel *tunnel)
{
	struct sockaddr_in sin;
	char *addr = get_iface_ip(tunnel->param.local_interface);
	if (!addr) {
		debug(LOG_ERR, "[%s] get_iface_ip [%s] failed",
		      tunnel->name, tunnel->param.local_interface);
		return -1;
	}

	int xkcp_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (xkcp_fd < 0) {
		debug(LOG_ERR, "[%s] socket(): %s", tunnel->name, strerror(errno));
		free(addr);
		return -1;
	}

	evutil_make_socket_nonblocking(xkcp_fd);
	xkcp_apply_sockbuf_param(xkcp_fd, &tunnel->param);

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = inet_addr(addr);
	sin.sin_port = htons(tunnel->param.local_port);

	if (bind(xkcp_fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
		debug(LOG_ERR, "[%s] xkcp_fd bind(%s:%d) failed: %s",
		      tunnel->name, addr, tunnel->param.local_port, strerror(errno));
		close(xkcp_fd);
		free(addr);
		return -1;
	}

	debug(LOG_INFO, "[%s] UDP KCP server listening on %s:%d -> Target %s:%d",
	      tunnel->name, addr, tunnel->param.local_port,
	      tunnel->param.remote_addr, tunnel->param.remote_port);
	free(addr);
	return xkcp_fd;
}

static void task_list_free(iqueue_head *task_list)
{
	if (!task_list) return;
	struct xkcp_task *task;
	iqueue_head *p, *n;
	for (p = task_list->next, n = p->next; p != task_list; p = n, n = p->next) {
		task = iqueue_entry(p, struct xkcp_task, head);
		if (task->bev) bufferevent_free(task->bev);
		del_task(task);
		if (task->kcp) {
			void *puser = task->kcp->user;
			ikcp_release(task->kcp);
			if (puser) free(puser);
		}
		free(task);
	}
	free(task_list);
}

int server_main_loop(void)
{
	struct event_base *base = NULL;
	struct xkcp_manager mgr;
	struct xkcp_config *cfg = xkcp_get_config();

	base = event_base_new();
	if (!base) {
		debug(LOG_ERR, "event_base_new() failed");
		exit(EXIT_FAILURE);
	}

	g_exit_base = base;
	xkcp_set_event_base(base);

	memset(&mgr, 0, sizeof(mgr));
	mgr.base = base;
	mgr.is_server = 1;
	iqueue_init(&mgr.tunnel_list);

	int num_tunnels = cfg->num_tunnels > 0 ? cfg->num_tunnels : 1;
	struct xkcp_param *params = cfg->num_tunnels > 0 ? cfg->tunnels : &cfg->param;

	for (int i = 0; i < num_tunnels; i++) {
		struct xkcp_param *p = &params[i];
		struct xkcp_tunnel *tunnel = calloc(1, sizeof(struct xkcp_tunnel));
		if (!tunnel) continue;

		snprintf(tunnel->name, sizeof(tunnel->name), "%s", p->name ? p->name : "default");
		tunnel->param = *p;
		tunnel->base = base;
		tunnel->mgr = &mgr;
		tunnel->server_xkcp_hash = create_hash(1024);
		tunnel->server_fec_hash = create_hash(1024);

		tunnel->xkcp_fd = set_xkcp_listener(tunnel);
		if (tunnel->xkcp_fd < 0) {
			delete_hash(tunnel->server_fec_hash, (void *)fec_conn_free, HASHPTR, HASHSTRING);
			delete_hash(tunnel->server_xkcp_hash, (void *)task_list_free, HASHPTR, HASHSTRING);
			free(tunnel);
			continue;
		}

		event_assign(&tunnel->timer_event, base, -1, EV_PERSIST, timer_event_cb, tunnel);
		set_timer_interval_ms(&tunnel->timer_event, p->interval);

		tunnel->xkcp_event = event_new(base, tunnel->xkcp_fd, EV_READ|EV_PERSIST,
					       xkcp_server_rcv_cb, tunnel);
		if (tunnel->xkcp_event)
			event_add(tunnel->xkcp_event, NULL);

		iqueue_add_tail(&tunnel->node, &mgr.tunnel_list);
		mgr.num_tunnels++;
	}

	if (mgr.num_tunnels == 0) {
		debug(LOG_ERR, "No active server tunnels could be initialized. Exiting.");
		event_base_free(base);
		return 1;
	}

	short mport = cfg->mon_port > 0 ? (short)cfg->mon_port : 9087;
	mgr.mon_listener = set_xkcp_mon_listener(base, mport, &mgr);

	xkcp_setup_signals(base);

	debug(LOG_INFO, "xkcptun server started with %d active tunnel(s), mon_port %d",
	      mgr.num_tunnels, mport);

	event_base_dispatch(base);

	/* Cleanup */
	xkcp_cleanup_signals();
	xkcp_cleanup_udp_queue();
	if (mgr.mon_listener)
		evconnlistener_free(mgr.mon_listener);

	iqueue_head *p, *n;
	for (p = mgr.tunnel_list.next, n = p->next; p != &mgr.tunnel_list; p = n, n = p->next) {
		struct xkcp_tunnel *t = iqueue_entry(p, struct xkcp_tunnel, node);
		event_del(&t->timer_event);
		if (t->xkcp_event) {
			event_del(t->xkcp_event);
			event_free(t->xkcp_event);
		}
		if (t->udp_wev) {
			event_del(t->udp_wev);
			event_free(t->udp_wev);
		}
		if (t->udp_pend)
			evbuffer_free(t->udp_pend);
		if (t->xkcp_fd >= 0)
			close(t->xkcp_fd);
		delete_hash(t->server_fec_hash, (void *)fec_conn_free, HASHPTR, HASHSTRING);
		delete_hash(t->server_xkcp_hash, (void *)task_list_free, HASHPTR, HASHSTRING);
		iqueue_del(&t->node);
		free(t);
	}

	event_base_free(base);
	debug(LOG_INFO, "xkcptun server stopped cleanly");
	return 0;
}

int main(int argc, char **argv)
{
	struct xkcp_config *config = xkcp_get_config();
	config->is_server = 1;
	config->main_loop = server_main_loop;

	return xkcp_main(argc, argv);
}
