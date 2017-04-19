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
#include "xkcp_util.h"
#include "tcp_proxy.h"
#include "xkcp_config.h"
#include "commandline.h"
#include "xkcp_client.h"
#include "xkcp_mon.h"
#include "debug.h"

IQUEUE_HEAD(xkcp_task_list);

static short mport = 9086;

void
timer_event_cb(evutil_socket_t fd, short event, void *arg)
{
	xkcp_timer_event_cb(arg, &xkcp_task_list);
}

void
xkcp_rcv_cb(const int sock, short int which, void *arg)
{
	struct xkcp_proxy_param  *ptr = arg;
	char buf[XKCP_RECV_BUF_LEN] = {0};
	int nrecv = 0;
	
	int index = 0;
	if ((nrecv = recvfrom(sock, buf, sizeof(buf)-1, 0, (struct sockaddr *) &ptr->serveraddr, &ptr->addr_len)) > 0) {
		int conv = ikcp_getconv(buf);
		ikcpcb *kcp = get_kcp_from_conv(conv, &xkcp_task_list);
		debug(LOG_DEBUG, "[%d] xkcp_rcv_cb [%d] len [%d] conv [%d] kcp is [%d]", 
			  index++, sock, nrecv, conv, kcp?1:0);
		if (kcp) {
			int nret = ikcp_input(kcp, buf, nrecv);
			if (nret < 0) {
				debug(LOG_INFO, "conv [%d] ikcp_input failed [%d]", nret);
			}
		} else {
			debug(LOG_ERR, "xkcp_rcv_cb -- cant get kcp from peer data!!!!!!");
		}
		memset(buf, 0, XKCP_RECV_BUF_LEN);
		
		xkcp_forward_all_data( &xkcp_task_list);
	}
}

static struct evconnlistener *set_tcp_proxy_listener(struct event_base *base, void *ptr)
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

    struct evconnlistener * listener = evconnlistener_new_bind(base, tcp_proxy_accept_cb, ptr,
	    LEV_OPT_CLOSE_ON_FREE|LEV_OPT_CLOSE_ON_EXEC|LEV_OPT_REUSEABLE,
	    -1, (struct sockaddr*)&sin, sizeof(sin));
    if (!listener) {
    	debug(LOG_ERR, "Couldn't create listener: [%s]", strerror(errno));
    	exit(0);
    }

    return listener;
}

int client_main_loop(void)
{
	struct event_base *base = NULL;
	struct evconnlistener *listener = NULL, *mlistener = NULL;
	int xkcp_fd = socket(AF_INET, SOCK_DGRAM, 0);
	struct event timer_event, *xkcp_event;

	if (xkcp_fd < 0) {
		debug(LOG_ERR, "ERROR, open udp socket");
		exit(0);
	}
	
	if (fcntl(xkcp_fd, F_SETFL, O_NONBLOCK) == -1) {
		debug(LOG_ERR, "ERROR, fcntl error: %s", strerror(errno));
		exit(0);
	}
	
	
	struct hostent *server = gethostbyname(xkcp_get_param()->remote_addr);
	if (!server) {
		debug(LOG_ERR, "ERROR, no such host as %s", xkcp_get_param()->remote_addr);
		exit(0);
	}

	base = event_base_new();
	if (!base) {
		debug(LOG_ERR, "event_base_new()");
		exit(0);
	}	

	struct xkcp_proxy_param  proxy_param;
	memset(&proxy_param, 0, sizeof(proxy_param));
	proxy_param.base 		= base;
	proxy_param.udp_fd 		= xkcp_fd;
	proxy_param.serveraddr.sin_family 	= AF_INET;
	proxy_param.serveraddr.sin_port		= htons(xkcp_get_param()->remote_port);
	memcpy((char *)&proxy_param.serveraddr.sin_addr.s_addr, (char *)server->h_addr, server->h_length);
	listener = set_tcp_proxy_listener(base, &proxy_param);

	mlistener = set_xkcp_mon_listener(base, mport, &xkcp_task_list);
	
	event_assign(&timer_event, base, -1, EV_PERSIST, timer_event_cb, &timer_event);
	set_timer_interval(&timer_event);

	xkcp_event = event_new(base, xkcp_fd, EV_READ|EV_PERSIST, xkcp_rcv_cb, &proxy_param);
	event_add(xkcp_event, NULL);

	event_base_dispatch(base);
	evconnlistener_free(mlistener);
	evconnlistener_free(listener);
	close(xkcp_fd);
	event_base_free(base);

	return 0;
}

int main(int argc, char **argv) 
{
	struct xkcp_config *config = xkcp_get_config();
	config->main_loop = client_main_loop;
	
	return xkcp_main(argc, argv);
}
