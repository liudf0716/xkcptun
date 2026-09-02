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
#include "xkcp_util.h"
#include "tcp_proxy.h"
#include "xkcp_config.h"
#include "xkcp_client.h"
#include "xkcp_mon.h"
#include "udp_proxy.h"
#include "xkcp_udp.h"
#include "debug.h"

extern struct event_base *g_exit_base;

/* deliver one raw KCP packet (post-FEC-decode) to the session layer */
static void client_handle_packet(struct xkcp_tunnel *tunnel, char *buf, int nrecv)
{
	if (nrecv >= 8 && buf[0] == XKCP_UDP_MAGIC_0 && buf[1] == XKCP_UDP_MAGIC_1) {
		udp_proxy_handle_server_packet(tunnel, buf, nrecv);
		return;
	}

	if (nrecv < 24)
		return;

	IUINT32 conv = ikcp_getconv(buf);
	struct xkcp_task *task = xkcp_find_task(conv, NULL, tunnel);
	if (!task || !task->kcp)
		return;

	if (ikcp_input(task->kcp, buf, nrecv) < 0)
		debug(LOG_INFO, "[%s] conv [%u] ikcp_input failed",
		      tunnel ? tunnel->name : "default", conv);

	xkcp_forward_data(task);
}

static void fec_deliver_pkt(void *user, const char *pkt, int len)
{
	struct xkcp_tunnel *tunnel = user;
	client_handle_packet(tunnel, (char *)pkt, len);
}

static void timer_event_cb(evutil_socket_t fd, short event, void *arg)
{
	struct xkcp_tunnel *tunnel = arg;
	if (!tunnel) return;

	(void)fd;
	(void)event;

	if (tunnel->param.proto && strcmp(tunnel->param.proto, "udp") == 0) {
		udp_proxy_check_timeout(tunnel);
		if (tunnel->client_fec) {
			xkcp_fec_conn_tick(tunnel->client_fec, tunnel->xkcp_fd,
					   &tunnel->client_proxy_param.sockaddr, tunnel);
		}
	} else {
		xkcp_update_task_list(&tunnel->client_task_list, &tunnel->param);
		xkcp_task_check_timeout_val(&tunnel->client_task_list, tunnel->param.conn_timeout);
	}
	set_timer_interval_ms(&tunnel->timer_event, tunnel->param.interval);
}

static void xkcp_rcv_cb(const int sock, short int which, void *arg)
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

		if (tunnel && tunnel->client_fec)
			fec_conn_decode(tunnel->client_fec, buf, nrecv, fec_deliver_pkt, tunnel);
		else
			client_handle_packet(tunnel, buf, nrecv);
	}
}

static struct evconnlistener *set_tcp_proxy_listener(struct event_base *base, struct xkcp_tunnel *tunnel)
{
	short lport = tunnel->param.local_port;
	struct sockaddr_in sin;
	char *addr = get_iface_ip(tunnel->param.local_interface);
	if (!addr) {
		debug(LOG_ERR, "[%s] get_iface_ip [%s] failed",
		      tunnel->name, tunnel->param.local_interface);
		return NULL;
	}

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = inet_addr(addr);
	sin.sin_port = htons(lport);

	struct evconnlistener *listener = evconnlistener_new_bind(
		base, tcp_proxy_accept_cb, &tunnel->client_proxy_param,
		LEV_OPT_CLOSE_ON_FREE|LEV_OPT_CLOSE_ON_EXEC|LEV_OPT_REUSEABLE,
		-1, (struct sockaddr*)&sin, sizeof(sin));
	if (!listener) {
		debug(LOG_ERR, "[%s] Couldn't create TCP listener on %s:%d: [%s]",
		      tunnel->name, addr, lport, strerror(errno));
		free(addr);
		return NULL;
	}

	debug(LOG_INFO, "[%s] TCP Proxy listening on %s:%d -> Remote %s:%d",
	      tunnel->name, addr, lport, tunnel->param.remote_addr, tunnel->param.remote_port);
	free(addr);
	return listener;
}

static void client_task_list_free(struct xkcp_tunnel *tunnel)
{
	struct xkcp_task *task;
	iqueue_head *p, *n;

	for (p = tunnel->client_task_list.next, n = p->next;
	     p != &tunnel->client_task_list; p = n, n = p->next) {
		task = iqueue_entry(p, struct xkcp_task, head);
		if (task->kcp) {
			ikcp_flush(task->kcp);
			ikcp_release(task->kcp);
			task->kcp = NULL;
		}
		if (task->bev) {
			bufferevent_free(task->bev);
			task->bev = NULL;
		}
		iqueue_del(&task->head);
		free(task);
	}
}

int client_main_loop(void)
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
	mgr.is_server = 0;
	iqueue_init(&mgr.tunnel_list);

	int num_tunnels = (cfg->num_tunnels > 0 && cfg->tunnels) ? cfg->num_tunnels : 1;
	struct xkcp_param *params = (cfg->num_tunnels > 0 && cfg->tunnels) ? cfg->tunnels : &cfg->param;

	for (int i = 0; i < num_tunnels; i++) {
		struct xkcp_param *p = &params[i];
		struct xkcp_tunnel *tunnel = calloc(1, sizeof(struct xkcp_tunnel));
		if (!tunnel) {
			debug(LOG_ERR, "Failed to allocate memory for tunnel");
			continue;
		}

		snprintf(tunnel->name, sizeof(tunnel->name), "%s", p->name ? p->name : "default");
		tunnel->param = *p;
		tunnel->base = base;
		tunnel->mgr = &mgr;
		iqueue_init(&tunnel->client_task_list);

		/* Resolve remote server address */
		struct hostent *server = gethostbyname(p->remote_addr);
		if (!server) {
			debug(LOG_ERR, "[%s] gethostbyname [%s] failed", tunnel->name, p->remote_addr);
			free(tunnel);
			continue;
		}

		int xkcp_fd = socket(AF_INET, SOCK_DGRAM, 0);
		if (xkcp_fd < 0) {
			debug(LOG_ERR, "[%s] create udp socket failed: %s", tunnel->name, strerror(errno));
			free(tunnel);
			continue;
		}

		evutil_make_socket_nonblocking(xkcp_fd);
		xkcp_apply_sockbuf_param(xkcp_fd, p);
		tunnel->xkcp_fd = xkcp_fd;

		if (p->fec) {
			int cap = p->mtu > 0 ? p->mtu : 1350;
			tunnel->client_fec = fec_conn_new(p->data_shard, p->parity_shard, cap);
			if (!tunnel->client_fec)
				debug(LOG_ERR, "[%s] fec_conn_new failed, running without FEC", tunnel->name);
		}

		tunnel->client_proxy_param.base = base;
		tunnel->client_proxy_param.xkcpfd = xkcp_fd;
		tunnel->client_proxy_param.sockaddr.sin_family = AF_INET;
		tunnel->client_proxy_param.sockaddr.sin_port = htons(p->remote_port);
		memcpy((char *)&tunnel->client_proxy_param.sockaddr.sin_addr.s_addr,
		       (char *)server->h_addr, server->h_length);
		tunnel->client_proxy_param.addr_len = sizeof(tunnel->client_proxy_param.sockaddr);
		tunnel->client_proxy_param.fec = tunnel->client_fec;
		tunnel->client_proxy_param.tunnel = tunnel;

		int is_udp = (p->proto && strcmp(p->proto, "udp") == 0);
		if (is_udp) {
			if (init_udp_proxy(tunnel) != 0) {
				close(xkcp_fd);
				if (tunnel->client_fec) fec_conn_free(tunnel->client_fec);
				free(tunnel);
				continue;
			}
		} else {
			tunnel->listener = set_tcp_proxy_listener(base, tunnel);
			if (!tunnel->listener) {
				close(xkcp_fd);
				if (tunnel->client_fec) fec_conn_free(tunnel->client_fec);
				free(tunnel);
				continue;
			}
		}

		event_assign(&tunnel->timer_event, base, -1, EV_PERSIST, timer_event_cb, tunnel);
		set_timer_interval_ms(&tunnel->timer_event, p->interval);

		tunnel->xkcp_event = event_new(base, xkcp_fd, EV_READ|EV_PERSIST, xkcp_rcv_cb, tunnel);
		if (tunnel->xkcp_event)
			event_add(tunnel->xkcp_event, NULL);

		iqueue_add_tail(&tunnel->node, &mgr.tunnel_list);
		mgr.num_tunnels++;
	}

	if (mgr.num_tunnels == 0) {
		debug(LOG_ERR, "No active tunnels could be initialized. Exiting.");
		event_base_free(base);
		return 1;
	}

	short mport = cfg->mon_port > 0 ? (short)cfg->mon_port : 9086;
	mgr.mon_listener = set_xkcp_mon_listener(base, mport, &mgr);

	xkcp_setup_signals(base);

	debug(LOG_INFO, "xkcptun client started with %d active tunnel(s), mon_port %d",
	      mgr.num_tunnels, mport);

	event_base_dispatch(base);

	/* Cleanup on exit */
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
		if (t->param.proto && strcmp(t->param.proto, "udp") == 0) {
			udp_proxy_cleanup(t);
		} else if (t->listener) {
			evconnlistener_free(t->listener);
		}
		if (t->xkcp_fd >= 0)
			close(t->xkcp_fd);
		client_task_list_free(t);
		if (t->client_fec)
			fec_conn_free(t->client_fec);
		iqueue_del(&t->node);
		free(t);
	}

	event_base_free(base);
	debug(LOG_INFO, "xkcptun client stopped cleanly");
	return 0;
}

int main(int argc, char **argv)
{
	struct xkcp_config *config = xkcp_get_config();
	config->is_server = 0;
	config->main_loop = client_main_loop;

	return xkcp_main(argc, argv);
}
