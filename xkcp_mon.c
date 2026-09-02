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
#include <arpa/inet.h>
#include <syslog.h>

#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <event2/util.h>

#include "xkcp_mon.h"
#include "xkcp_util.h"
#include "xkcp_config.h"
#include "debug.h"
#include "jwHash.h"

typedef void (*spy_cmd_process)(struct bufferevent *bev, void *ctx, const char *arg);

struct user_spy_cmd {
	char *command;
	spy_cmd_process cmd_process;
};

static void get_client_list(struct bufferevent *bev, void *ctx, const char *arg);
static void get_client_status(struct bufferevent *bev, void *ctx, const char *arg);
static void get_server_list(struct bufferevent *bev, void *ctx, const char *arg);
static void get_server_status(struct bufferevent *bev, void *ctx, const char *arg);
static void xkcp_mon_read_cb(struct bufferevent *bev, void *ctx);
static void xkcp_mon_write_cb(struct bufferevent *bev, void *ctx);
static void xkcp_mon_event_cb(struct bufferevent *bev, short what, void *ctx);

struct user_spy_cmd client_cmd[] = {
	{"list", get_client_list},
	{"status", get_client_status},
	{NULL, NULL}
};

struct user_spy_cmd server_cmd[] = {
	{"list", get_server_list},
	{"status", get_server_status},
	{"client", get_server_list},
	{NULL, NULL}
};

static void get_client_list(struct bufferevent *bev, void *ctx, const char *arg)
{
	struct xkcp_manager *mgr = ctx;
	struct evbuffer *output = bufferevent_get_output(bev);
	(void)arg;

	if (!mgr) return;

	evbuffer_add_printf(output, "xkcptun client tunnels (%d total):\n", mgr->num_tunnels);
	struct xkcp_tunnel *t;
	iqueue_foreach(t, &mgr->tunnel_list, struct xkcp_tunnel, node) {
		int conns = get_task_list_size(&t->client_task_list);
		if (t->param.dynamic_target && t->param.target_port > 0) {
			evbuffer_add_printf(output, "  - [%s] :%d -> %s:%d -> Target %s:%d (mode: %s, fec: %d, conns: %d)\n",
					    t->name, t->param.local_port, t->param.remote_addr,
					    t->param.remote_port, t->param.target_addr ? t->param.target_addr : "127.0.0.1",
					    t->param.target_port, t->param.mode ? t->param.mode : "fast3",
					    t->param.fec, conns);
		} else {
			evbuffer_add_printf(output, "  - [%s] :%d -> %s:%d (mode: %s, fec: %d, conns: %d)\n",
					    t->name, t->param.local_port, t->param.remote_addr,
					    t->param.remote_port, t->param.mode ? t->param.mode : "fast3",
					    t->param.fec, conns);
		}
	}
}

static void get_client_status(struct bufferevent *bev, void *ctx, const char *arg)
{
	struct xkcp_manager *mgr = ctx;
	struct evbuffer *output = bufferevent_get_output(bev);

	if (!mgr) return;

	struct xkcp_tunnel *t;
	iqueue_foreach(t, &mgr->tunnel_list, struct xkcp_tunnel, node) {
		if (arg && strlen(arg) > 0 && strcmp(arg, t->name) != 0)
			continue;
		evbuffer_add_printf(output, "tunnel [%s] (: %d -> %s:%d):\n",
				    t->name, t->param.local_port, t->param.remote_addr, t->param.remote_port);
		dump_task_list(&t->client_task_list, bev);
	}
}

static void get_server_list(struct bufferevent *bev, void *ctx, const char *arg)
{
	struct xkcp_manager *mgr = ctx;
	struct evbuffer *output = bufferevent_get_output(bev);
	(void)arg;

	if (!mgr) return;

	evbuffer_add_printf(output, "xkcptun server tunnels (%d total):\n", mgr->num_tunnels);
	struct xkcp_tunnel *t;
	iqueue_foreach(t, &mgr->tunnel_list, struct xkcp_tunnel, node) {
		int total_clients = 0;
		if (t->server_xkcp_hash) {
			for (int i = 0; i < t->server_xkcp_hash->buckets; i++) {
				jwHashEntry *entry = t->server_xkcp_hash->bucket[i];
				while (entry) {
					total_clients++;
					entry = entry->next;
				}
			}
		}
		evbuffer_add_printf(output, "  - [%s] UDP :%d -> TCP %s:%d (mode: %s, fec: %d, peers: %d)\n",
				    t->name, t->param.local_port, t->param.remote_addr,
				    t->param.remote_port, t->param.mode ? t->param.mode : "fast3",
				    t->param.fec, total_clients);
	}
}

static void get_server_status(struct bufferevent *bev, void *ctx, const char *arg)
{
	struct xkcp_manager *mgr = ctx;
	struct evbuffer *output = bufferevent_get_output(bev);

	if (!mgr) return;

	struct xkcp_tunnel *t;
	iqueue_foreach(t, &mgr->tunnel_list, struct xkcp_tunnel, node) {
		if (arg && strlen(arg) > 0 && strcmp(arg, t->name) != 0)
			continue;

		evbuffer_add_printf(output, "tunnel [%s] (UDP :%d -> TCP %s:%d):\n",
				    t->name, t->param.local_port, t->param.remote_addr, t->param.remote_port);
		if (t->server_xkcp_hash) {
			for (int i = 0; i < t->server_xkcp_hash->buckets; i++) {
				jwHashEntry *entry = t->server_xkcp_hash->bucket[i];
				while (entry) {
					evbuffer_add_printf(output, "  peer [%s]:\n", entry->key.strValue);
					dump_task_list((iqueue_head *)entry->value.ptrValue, bev);
					entry = entry->next;
				}
			}
		}
	}
}

static void process_user_cmd(struct bufferevent *bev, const char *cmd_line, void *ctx)
{
	struct xkcp_manager *mgr = ctx;
	char cmd[64] = {0};
	char arg[128] = {0};

	sscanf(cmd_line, "%63s %127s", cmd, arg);

	struct user_spy_cmd *table = (mgr && mgr->is_server) ? server_cmd : client_cmd;

	for (int i = 0; table[i].command != NULL; i++) {
		if (strcmp(cmd, table[i].command) == 0) {
			table[i].cmd_process(bev, ctx, arg);
			bufferevent_setcb(bev, xkcp_mon_read_cb, xkcp_mon_write_cb, xkcp_mon_event_cb, ctx);
			bufferevent_enable(bev, EV_WRITE);
			return;
		}
	}

	/* Default fallback for bare "status" or empty command */
	if (table[0].cmd_process) {
		table[0].cmd_process(bev, ctx, arg);
		bufferevent_setcb(bev, xkcp_mon_read_cb, xkcp_mon_write_cb, xkcp_mon_event_cb, ctx);
		bufferevent_enable(bev, EV_WRITE);
	}
}

static void xkcp_mon_event_cb(struct bufferevent *bev, short what, void *ctx)
{
	(void)ctx;
	if (what & (BEV_EVENT_EOF|BEV_EVENT_ERROR)) {
		bufferevent_free(bev);
	}
}

static void xkcp_mon_write_cb(struct bufferevent *bev, void *ctx)
{
	(void)ctx;
	struct evbuffer *output = bufferevent_get_output(bev);
	if (evbuffer_get_length(output) == 0)
		bufferevent_free(bev);
}

static void xkcp_mon_read_cb(struct bufferevent *bev, void *ctx)
{
	struct evbuffer *input = bufferevent_get_input(bev);
	int len = evbuffer_get_length(input);

	if (len > 0) { 
		char *buf = malloc(len + 1);
		if (buf) {
			memset(buf, 0, len + 1);
			if (evbuffer_remove(input, buf, len) > 0) {
				/* Strip trailing newlines / spaces */
				while (len > 0 && (buf[len - 1] == '\r' || buf[len - 1] == '\n' || buf[len - 1] == ' ')) {
					buf[--len] = '\0';
				}
				process_user_cmd(bev, buf, ctx);
			}
			free(buf);
		}
	}
}

void xkcp_mon_accept_cb(struct evconnlistener *listener, evutil_socket_t fd,
    struct sockaddr *a, int slen, void *ptr)
{
	(void)a; (void)slen;
	struct bufferevent *b_in = NULL;
	struct event_base *base = evconnlistener_get_base(listener);
	
	b_in = bufferevent_socket_new(base, fd,
	    BEV_OPT_CLOSE_ON_FREE|BEV_OPT_DEFER_CALLBACKS);
	assert(b_in);
	
	bufferevent_setcb(b_in, xkcp_mon_read_cb, NULL, xkcp_mon_event_cb, ptr);
	bufferevent_enable(b_in,  EV_READ);
}

struct evconnlistener *set_xkcp_mon_listener(struct event_base *base, short port, void *ptr)
{
	struct sockaddr_in sin;
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	/* management interface: loopback only, it exposes tunnel details */
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sin.sin_port = htons(port);

	struct evconnlistener *listener = evconnlistener_new_bind(
		base, xkcp_mon_accept_cb, ptr,
		LEV_OPT_CLOSE_ON_FREE|LEV_OPT_CLOSE_ON_EXEC|LEV_OPT_REUSEABLE,
		-1, (struct sockaddr*)&sin, sizeof(sin));
	if (!listener) {
		debug(LOG_WARNING, "Couldn't create mon listener on port %d: [%s]", port, strerror(errno));
		return NULL;
	}

	debug(LOG_INFO, "Monitor listening on 127.0.0.1:%d", port);
	return listener;
}
