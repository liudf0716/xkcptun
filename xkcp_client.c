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

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>


#include <pthread.h>

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
#include "xkcp_util.h"
#include "tcp_proxy.h"
#include "xkcp_config.h"
#include "commandline.h"
#include "xkcp_client.h"
#include "xkcp_mon.h"
#include "debug.h"

#include <signal.h>

extern struct event_base *g_exit_base;

IQUEUE_HEAD(xkcp_task_list);

static short mport = 9086;
static struct fec_conn *g_fec = NULL;
static struct xkcp_proxy_param *g_client_pp = NULL;

/* deliver one raw KCP packet (post-FEC-decode) to the session layer */
static void client_handle_packet(char *buf, int nrecv)
{
	IUINT32 conv = ikcp_getconv(buf);
	struct xkcp_task *task = xkcp_find_task(conv, NULL);
	if (!task || !task->kcp)
		return;

	if (ikcp_input(task->kcp, buf, nrecv) < 0)
		debug(LOG_INFO, "conv [%u] ikcp_input failed", conv);

	ikcp_flush(task->kcp);
	xkcp_forward_data(task);
}

static void fec_deliver_pkt(void *user, const char *pkt, int len)
{
	(void)user;
	client_handle_packet((char *)pkt, len);
}

void
timer_event_cb(evutil_socket_t fd, short event, void *arg)
{
	xkcp_timer_event_cb(arg, &xkcp_task_list);
	xkcp_fec_tick(g_client_pp);
}

void
xkcp_rcv_cb(const int sock, short int which, void *arg)
{
	char buf[XKCP_RECV_BUF_LEN];
	struct sockaddr_in from;
	socklen_t from_len;
	int nrecv;

	(void)arg;
	(void)which;

	while (1) {
		from_len = sizeof(from);
		nrecv = recvfrom(sock, buf, sizeof(buf), 0,
				 (struct sockaddr *)&from, &from_len);
		if (nrecv <= 0)
			break;

		if (g_fec)
			fec_conn_decode(g_fec, buf, nrecv, fec_deliver_pkt, NULL);
		else
			client_handle_packet(buf, nrecv);
	}
}

static struct evconnlistener *set_tcp_proxy_listener(struct event_base *base, void *ptr)
{
	short lport = xkcp_get_param()->local_port;
	struct sockaddr_in sin;
	char *addr = get_iface_ip(xkcp_get_param()->local_interface);
	if (!addr) {
		debug(LOG_ERR, "get_iface_ip [%s] failed", xkcp_get_param()->local_interface);
		exit(EXIT_FAILURE);
	}

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = inet_addr(addr);
	sin.sin_port = htons(lport);

	struct evconnlistener * listener = evconnlistener_new_bind(base, tcp_proxy_accept_cb, ptr,
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

static void client_task_list_free(void)
{
	struct xkcp_task *task;
	iqueue_head *p, *n;

	for (p = xkcp_task_list.next, n = p->next; p != &xkcp_task_list; p = n, n = p->next) {
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
	struct event timer_event, *xkcp_event = NULL;
	struct hostent *server = NULL;
	struct event_base *base = NULL;
	struct evconnlistener *listener = NULL, *mlistener = NULL;

	base = event_base_new();
	if (!base) {
		debug(LOG_ERR, "event_base_new() failed");
		exit(EXIT_FAILURE);
	}

	g_exit_base = base;
	xkcp_set_event_base(base);

	int xkcp_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (xkcp_fd < 0) {
		debug(LOG_ERR, "create udp socket failed");
		exit(EXIT_FAILURE);
	}

	evutil_make_socket_nonblocking(xkcp_fd);
	xkcp_apply_sockbuf(xkcp_fd);

	server = gethostbyname(xkcp_get_param()->remote_addr);
	if (!server) {
		debug(LOG_ERR, "gethostbyname [%s] failed", xkcp_get_param()->remote_addr);
		exit(EXIT_FAILURE);
	}

	if (xkcp_get_param()->fec) {
		int cap = xkcp_get_param()->mtu > 0 ? xkcp_get_param()->mtu : 1350;
		g_fec = fec_conn_new(xkcp_get_param()->data_shard,
				     xkcp_get_param()->parity_shard,
				     cap);
		if (!g_fec)
			debug(LOG_ERR, "fec_conn_new failed, running without FEC");
	}

	struct xkcp_proxy_param  proxy_param;
	memset(&proxy_param, 0, sizeof(proxy_param));
	proxy_param.base 		= base;
	proxy_param.xkcpfd 		= xkcp_fd;
	proxy_param.sockaddr.sin_family 	= AF_INET;
	proxy_param.sockaddr.sin_port		= htons(xkcp_get_param()->remote_port);
	memcpy((char *)&proxy_param.sockaddr.sin_addr.s_addr, (char *)server->h_addr, server->h_length);
	proxy_param.fec = g_fec;
	g_client_pp = &proxy_param;
	listener = set_tcp_proxy_listener(base, &proxy_param);

	mlistener = set_xkcp_mon_listener(base, mport, &xkcp_task_list);

	event_assign(&timer_event, base, -1, EV_PERSIST, timer_event_cb, &timer_event);
	set_timer_interval(&timer_event);

	xkcp_setup_signals(base);

	xkcp_event = event_new(base, xkcp_fd, EV_READ|EV_PERSIST, xkcp_rcv_cb, &proxy_param);
	event_add(xkcp_event, NULL);

	event_base_dispatch(base);

	event_del(&timer_event);
	if (xkcp_event) {
		event_del(xkcp_event);
		event_free(xkcp_event);
		xkcp_event = NULL;
	}
	xkcp_cleanup_signals();
	xkcp_cleanup_udp_queue();
	evconnlistener_free(mlistener);
	evconnlistener_free(listener);
	close(xkcp_fd);
	client_task_list_free();
	event_base_free(base);
	fec_conn_free(g_fec);

	return 0;
}

int main(int argc, char **argv)
{
	struct xkcp_config *config = xkcp_get_config();
	config->main_loop = client_main_loop;

	return xkcp_main(argc, argv);
}
