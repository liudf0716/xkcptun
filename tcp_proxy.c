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
#include <errno.h>

#include <syslog.h>

#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <event2/util.h>


#include "xkcp_util.h"
#include "tcp_proxy.h"
#include "xkcp_client.h"
#include "debug.h"

extern iqueue_head xkcp_task_list;

static void
tcp_proxy_read_cb(struct bufferevent *bev, void *ctx) 
{
	struct xkcp_task *task = ctx;
	ikcpcb *kcp = task->kcp;
	xkcp_tcp_read_cb(bev, kcp);
	xkcp_forward_all_data(&xkcp_task_list);
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
	struct bufferevent *b_in = NULL;
	struct event_base *base = evconnlistener_get_base(listener);

	b_in = bufferevent_socket_new(base, fd,
	    BEV_OPT_CLOSE_ON_FREE|BEV_OPT_DEFER_CALLBACKS);
	assert(b_in);
	
	static int conv = 1;
	ikcpcb *kcp_client 	= ikcp_create(conv, param);
	xkcp_set_config_param(kcp_client);
	conv++;
	
	debug(LOG_INFO, "accept new client [%d] in, conv [%d]", fd, conv-1);

	struct xkcp_task *task = malloc(sizeof(struct xkcp_task));
	assert(task);
	task->kcp = kcp_client;
	task->bev = b_in;
	task->sockaddr = &p->sockaddr;
	add_task_tail(task, &xkcp_task_list);

	bufferevent_setcb(b_in, tcp_proxy_read_cb, NULL, tcp_proxy_event_cb, task);
	bufferevent_enable(b_in,  EV_READ | EV_WRITE );
}
