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

#include <netdb.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <syslog.h>

#include <event2/event.h>
#include <event2/event_struct.h>
#include <event2/bufferevent_ssl.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <event2/util.h>

#include "ikcp.h"
#include "debug.h"
#include "xkcp_server.h"
#include "xkcp_config.h"
#include "xkcp_util.h"
#include "xkcp_mon.h"
#include "tcp_client.h"

IQUEUE_HEAD(xkcp_task_list);

static short mport = 9087;

static struct xkcp_proxy_param *param = NULL;

iqueue_head * get_xkcp_task_list()
{
	return &xkcp_task_list;
}

static void timer_event_cb(evutil_socket_t fd, short event, void *arg)
{
	xkcp_timer_event_cb(arg, &xkcp_task_list);
}

static void xkcp_rcv_cb(const int sock, short int which, void *arg)
{	
	struct event_base *base = arg;
	struct sockaddr_in clientaddr;
	int clientlen = sizeof(clientaddr);
	memset(&clientaddr, 0, clientlen);
	
	char buf[BUF_RECV_LEN] = {0};
	int len = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *) &clientaddr, &clientlen);
	if (len > 0) {
		if (param == NULL) {
			param = malloc(sizeof(struct xkcp_proxy_param));
			memset(param, 0, sizeof(struct xkcp_proxy_param));
			memcpy(&param->serveraddr, &clientaddr, clientlen);
			param->udp_fd = sock;
			param->addr_len = clientlen;
		}
		int conv = ikcp_getconv(buf);
		ikcpcb *kcp_server = get_kcp_from_conv(conv, &xkcp_task_list);
		debug(LOG_INFO, "xkcp_server: xkcp_rcv_cb -- xkcp sock %d conv is %d, kcp_server is %d, recv data %d", 
			  sock, conv, kcp_server?1:0, len);
		if (kcp_server == NULL) {
			kcp_server = ikcp_create(conv, param);
			xkcp_set_config_param(kcp_server);

			struct xkcp_task *task = malloc(sizeof(struct xkcp_task));
			assert(task);
			task->kcp = kcp_server;		
			task->svr_addr = &param->serveraddr;
			add_task_tail(task, &xkcp_task_list);

			struct bufferevent *bev = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);
			if (!bev) {
				debug(LOG_ERR, "bufferevent_socket_new failed [%s]", strerror(errno));
				exit(EXIT_FAILURE);
			}
			task->bev = bev;
			bufferevent_setcb(bev, tcp_client_read_cb, NULL, tcp_client_event_cb, task);
			bufferevent_enable(bev, EV_READ);
			if (bufferevent_socket_connect_hostname(bev, NULL, AF_INET, 
												   xkcp_get_param()->remote_addr,
												   xkcp_get_param()->remote_port) < 0) {
				bufferevent_free(bev);
				debug(LOG_ERR, "bufferevent_socket_connect failed [%s]", strerror(errno));
				exit(EXIT_FAILURE);
			}
			debug(LOG_INFO, "tcp client [%d] connect to [%s]:[%d] success", bufferevent_getfd(bev),
				  xkcp_get_param()->remote_addr, xkcp_get_param()->remote_port);
		} 

		int nret = ikcp_input(kcp_server, buf, len);
		if (nret < 0) {
			debug(LOG_INFO, "[%d] ikcp_input failed [%d]", kcp_server->conv, nret);
		}
	}
	
	xkcp_forward_all_data(&xkcp_task_list);
}

static int set_xkcp_listener()
{
	short lport = xkcp_get_param()->local_port;
	struct sockaddr_in sin;
	char *addr = get_iface_ip(xkcp_get_param()->local_interface);
	if (!addr) {
		debug(LOG_ERR, "get_iface_ip [%s] failed", xkcp_get_param()->local_interface);
		exit(0);
	}
	
	memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = inet_addr(addr);
    sin.sin_port = htons(lport);
	
	int xkcp_fd = socket(AF_INET, SOCK_DGRAM, 0);
	
	if (bind(xkcp_fd, (struct sockaddr *) &sin, sizeof(sin))) {
		debug(LOG_ERR, "xkcp_fd bind() failed %s ", strerror(errno));
		exit(EXIT_FAILURE);
	}
	
	return xkcp_fd;
}

int server_main_loop()
{
	struct event timer_event, 
	  			*xkcp_event = NULL;
	struct event_base *base = NULL;
	struct evconnlistener *mon_listener = NULL;
	
	base = event_base_new();
	if (!base) {
		debug(LOG_ERR, "event_base_new()");
		exit(0);
	}		
	
	int xkcp_fd = set_xkcp_listener();
	
	mon_listener = set_xkcp_mon_listener(base, mport, &xkcp_task_list);
		
	xkcp_event = event_new(base, xkcp_fd, EV_READ|EV_PERSIST, xkcp_rcv_cb, base);
	event_add(xkcp_event, NULL);
	
	event_assign(&timer_event, base, -1, EV_PERSIST, timer_event_cb, &timer_event);
	set_timer_interval(&timer_event);	

	event_base_dispatch(base);
	
	evconnlistener_free(mon_listener);
	close(xkcp_fd);
	event_base_free(base);

	return 0;
}

int main(int argc, char **argv) 
{
	struct xkcp_config *config = xkcp_get_config();
	config->main_loop = server_main_loop;
	
	return xkcp_main(argc, argv);
}

