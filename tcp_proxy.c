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

#include <sys/socket.h>
#include <netinet/in.h>
#include <syslog.h>

#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <event2/util.h>

#include "xkcp_util.h"
#include "xkcp_proto.h"
#include "xkcp_auth.h"
#include "tcp_proxy.h"
#include "xkcp_client.h"
#include "debug.h"

#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static uint32_t next_conv_id = 0;

static uint32_t gen_conv_id(void *tunnel)
{
	uint32_t conv;

	do {
		if (next_conv_id == 0)
			next_conv_id = (uint32_t)time(NULL) ^ (uint32_t)getpid();
		next_conv_id = next_conv_id * 1103515245 + 12345;
		conv = next_conv_id;
	} while (conv == 0 || xkcp_find_task((IUINT32)conv, NULL, tunnel) != NULL);

	return conv;
}

static void
tcp_proxy_read_cb(struct bufferevent *bev, void *ctx)
{
	struct xkcp_task *task = ctx;
	if (!task || !task->kcp)
		return;
	xkcp_tcp_read_cb(bev, task->kcp);
	task->last_active = iclock();
	xkcp_forward_data(task);
}

static void
tcp_proxy_event_cb(struct bufferevent *bev, short what, void *ctx)
{
	xkcp_tcp_event_cb(bev, what, ctx);
}

void
tcp_proxy_accept_cb(struct evconnlistener *listener, evutil_socket_t fd,
    struct sockaddr *a, int slen, void *param)
{
	struct xkcp_proxy_param *p = param;
	struct xkcp_tunnel *tunnel = p ? p->tunnel : NULL;
	struct bufferevent *b_in = NULL;
	struct event_base *base = evconnlistener_get_base(listener);

	if (!tunnel) {
		/* no tunnel context: cannot route this connection anywhere */
		debug(LOG_ERR, "accept without tunnel context, closing fd [%d]", fd);
		evutil_closesocket(fd);
		return;
	}

	xkcp_set_tcp_nodelay(fd);
	b_in = bufferevent_socket_new(base, fd,
	    BEV_OPT_CLOSE_ON_FREE|BEV_OPT_DEFER_CALLBACKS);
	if (!b_in) {
		debug(LOG_ERR, "[%s] bufferevent_socket_new failed, closing fd [%d]",
		      tunnel->name, fd);
		evutil_closesocket(fd);
		return;
	}

	IUINT32 conv = gen_conv_id(tunnel);
	ikcpcb *kcp_client = ikcp_create(conv, p);
	if (!kcp_client) {
		debug(LOG_ERR, "[%s] ikcp_create failed, closing fd [%d]", tunnel->name, fd);
		bufferevent_free(b_in);
		return;
	}
	xkcp_set_tunnel_config_param(kcp_client, &tunnel->param);

	debug(LOG_INFO, "[%s] accept new client [%d] in, conv [%u]",
	      tunnel->name, fd, conv);

	struct xkcp_task *task = malloc(sizeof(struct xkcp_task));
	if (!task) {
		debug(LOG_ERR, "[%s] alloc task failed, closing fd [%d]", tunnel->name, fd);
		ikcp_release(kcp_client);
		bufferevent_free(b_in);
		return;
	}
	task->kcp = kcp_client;
	task->bev = b_in;
	task->sockaddr = &p->sockaddr;
	task->last_active = iclock();
	task->user_owned = 0;
	task->conv = conv;
	task->tunnel = tunnel;
	task->handshake_done = 1;
	task->target_host[0] = '\0';
	task->target_port = 0;

	if (tunnel->param.dynamic_target || tunnel->param.target_port > 0) {
		char hdr_buf[XKCP_MAX_HDR_LEN];
		const char *thost = tunnel->param.target_addr ? tunnel->param.target_addr : "127.0.0.1";
		uint16_t tport = (uint16_t)tunnel->param.target_port;
		int hlen = xkcp_auth_encode_header(hdr_buf, sizeof(hdr_buf), thost, tport,
						   tunnel->param.key, conv, (uint32_t)time(NULL));
		if (hlen > 0) {
			ikcp_send(kcp_client, hdr_buf, hlen);
			debug(LOG_INFO, "[%s] conv [%u] sent authenticated dynamic target header [%s]:[%u]",
			      tunnel->name, conv, thost, tport);
		} else {
			debug(LOG_ERR, "[%s] conv [%u] encode dynamic target header [%s]:[%u] failed",
			      tunnel->name, conv, thost, tport);
		}
	}

	add_task_tail(task, &tunnel->client_task_list);

	bufferevent_setcb(b_in, tcp_proxy_read_cb, NULL, tcp_proxy_event_cb, task);
	bufferevent_enable(b_in,  EV_READ | EV_WRITE );
}
