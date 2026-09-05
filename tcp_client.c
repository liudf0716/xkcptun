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
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <arpa/inet.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <syslog.h>

#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <event2/util.h>

#include "xkcp_server.h"
#include "xkcp_util.h"
#include "tcp_client.h"
#include "debug.h"
#include "ikcp.h"
#include "jwHash.h"

#include <event2/dns.h>

void tcp_client_event_cb(struct bufferevent *bev, short what, void *ctx)
{
	struct xkcp_task *task = ctx;
	struct xkcp_tunnel *tunnel = task ? task->tunnel : NULL;

	if (what & BEV_EVENT_CONNECTED) {
		xkcp_set_tcp_nodelay(bufferevent_getfd(bev));
		debug(LOG_INFO, "[%s] conv [%u] connected to target [%s]:[%u]",
		      tunnel ? tunnel->name : "server", task->conv,
		      task->target_host, task->target_port);
		xkcp_forward_data(task);
		return;
	}

	void *puser = xkcp_tcp_event_cb(bev, what, ctx);
	if (puser)
		free(puser);

	if (what & (BEV_EVENT_EOF|BEV_EVENT_ERROR)) {
		if (tunnel)
			clean_useless_client(tunnel);
	}
}

void tcp_client_read_cb(struct bufferevent *bev, void *ctx)
{
	struct xkcp_task *task = ctx;
	ikcpcb *kcp = task->kcp;
	if (!kcp)
		return;
	xkcp_tcp_read_cb(bev, kcp);
	task->last_keepalive = task->last_active = task->last_recv = iclock();
	xkcp_forward_data(task);
}

int xkcp_server_connect_target(struct xkcp_task *task, const char *host, uint16_t port)
{
	if (!task || !host || port == 0)
		return -1;

	struct xkcp_tunnel *tunnel = task->tunnel;
	struct event_base *base = tunnel ? tunnel->base : NULL;
	struct evdns_base *dns_base = (tunnel && tunnel->mgr) ? tunnel->mgr->dns_base : NULL;
	if (!base) return -1;

	struct bufferevent *bev = bufferevent_socket_new(base, -1,
		BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);
	if (!bev) {
		debug(LOG_ERR, "bufferevent_socket_new failed [%s]", strerror(errno));
		return -1;
	}

	task->bev = bev;
	bufferevent_setcb(bev, tcp_client_read_cb, NULL, tcp_client_event_cb, task);
	bufferevent_enable(bev, EV_READ | EV_WRITE);

	struct sockaddr_in sin;
	struct sockaddr_in6 sin6;
	int ret;

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(port);
	memset(&sin6, 0, sizeof(sin6));
	sin6.sin6_family = AF_INET6;
	sin6.sin6_port = htons(port);

	if (inet_pton(AF_INET, host, &sin.sin_addr) == 1) {
		ret = bufferevent_socket_connect(bev, (struct sockaddr *)&sin, sizeof(sin));
	} else if (inet_pton(AF_INET6, host, &sin6.sin6_addr) == 1) {
		ret = bufferevent_socket_connect(bev, (struct sockaddr *)&sin6, sizeof(sin6));
	} else {
		ret = bufferevent_socket_connect_hostname(bev, dns_base, AF_UNSPEC, host, port);
	}

	if (ret < 0) {
		bufferevent_free(bev);
		task->bev = NULL;
		debug(LOG_ERR, "connect to target %s:%u failed", host, port);
		return -1;
	}

	debug(LOG_INFO, "[%s] conv [%u] connecting to target [%s]:[%u]",
	      tunnel ? tunnel->name : "server", task->conv, host, port);
	return 0;
}
