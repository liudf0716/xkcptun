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

#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <event2/util.h>

#include <syslog.h>

#include "xkcp_mon.h"
#include "xkcp_util.h"
#include "xkcp_server.h"
#include "debug.h"

static void process_user_cmd(struct bufferevent *bev, const char *cmd)
{
	debug(LOG_DEBUG, "cmd is %s", cmd);
	if (strcmp(cmd, "status") == 0) {
		dump_task_list(get_xkcp_task_list(), bev);
	}
}

static void xkcp_mon_event_cb(struct bufferevent *bev, short what, void *ctx)
{
	if (what & (BEV_EVENT_EOF|BEV_EVENT_ERROR)) {
		bufferevent_free(bev);
	}
}

static void xkcp_mon_read_cb(struct bufferevent *bev, void *ctx)
{
	struct evbuffer *input = bufferevent_get_input(bev);
	int len = evbuffer_get_length(input);
	
	debug(LOG_DEBUG, "xkcp_mon_read_cb [%D]", len);
	
	if ( len > 0) { 
		char *buf = malloc(len+1);
		memset(buf, 0, len+1);
		if (evbuffer_remove(input, buf, len) > 0)
			process_user_cmd(bev, buf);
	}
}

void xkcp_mon_accept_cb(struct evconnlistener *listener, evutil_socket_t fd,
    struct sockaddr *a, int slen, void *param)
{
	struct bufferevent *b_in = NULL;
	struct event_base *base = param;
	
	b_in = bufferevent_socket_new(base, fd,
	    BEV_OPT_CLOSE_ON_FREE|BEV_OPT_DEFER_CALLBACKS);
	assert(b_in);
	
	debug(LOG_INFO, "accept new mon client in");
	
	bufferevent_setcb(b_in, xkcp_mon_read_cb, NULL, xkcp_mon_event_cb, NULL);
	bufferevent_enable(b_in,  EV_READ | EV_WRITE | EV_PERSIST);
}
