#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <sys/socket.h>
#include <netinet/in.h>

#include <pthread.h>

#include <event2/bufferevent_ssl.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <event2/util.h>

#include "ikcp.h"
#include "xkcp_util.h"
#include "tcp_proxy.h"
#include "xkcp_config.h"
#include "commandline.h"
#include "xkcp_client.h"

IQUEUE_HEAD(xkcp_task_list);

void
timer_event_cb(evutil_socket_t fd, short event, void *arg)
{
	ikcpcb *task;
	iqueue_foreach(task, xkcp_task_list, ikcpcb, kcp)
		ikcp_update(task, iclock());

	event_add(timeout, arg);
}

ikcpcb *
get_kcp_from_conv(int conv)
{
	ikcpcb *task;
	iqueue_foreach(task, xkcp_task_list, ikcpcb, kcp)
		if (task->conv == conv)
			return task;

	return NULL;
}

void
xkcp_rcv_cb(const int sock, short int which, void *arg)
{
	struct ikcp_param *param = arg;
	struct sockaddr_in server_sin;
	socklen_t server_sz = sizeof(server_sin);
	char buf[XKCP_RECV_BUF_LEN] = {0};
	int nrecv = 0;

	while ((nrecv = recvfrom(sock, buf, sizeof(buf)-1, 0, (struct sockaddr *) &server_sin, &server_sz)) > 0) {
		int conv = 0;
		ikcp_decode32u(buf, &conv);
		ikcpcb *kcp = get_kcp_from_conv(conv);
		if (kcp) {
			ikcp_iput(kcp, buf, nrecv);
		}
		memset(buf, 0, XKCP_RECV_BUF_LEN);
	}

	while (1) {
		char obuf[XKCP_SEND_BUF_LEN];
		ikcpcb *task = NULL;
		int has_data = 0;
		iqueue_foreach(task, xkcp_task_list, ikcpcb, kcp) {
			has_data = 1;
			memset(buf, 0, XKCP_SEND_BUF_LEN);
			nrecv = ikcp_recv(task, buf, XKCP_SEND_BUF_LEN-1);
			if (nrecv > 0) {
				struct bufferevent *b_in = task->b_in;
				evbuffer_add(b_in, buf, nrecv);
			}
		}
		
		if (!has_data)
			break;
	}
}

static struct evconnlistener *set_tcp_proxy_listener(struct event_base *base, void *ptr)
{
	short lport = xkcp_get_param()->local_port;
	struct sockaddr_in sin;
	char *addr = get_iface_ip(xkcp_get_param()->local_interface);
	if (!addr) {
		debug(LOG_ERROR, "get_iface_ip [%s] failed", ifname);
		exit(0);
	}

	memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = inet_addr(addr);
    sin.sin_port = htons(port);

    struct evconnlistener * listener = evconnlistener_new_bind(base, tcp_proxy_accept_cb, ptr,
	    LEV_OPT_CLOSE_ON_FREE|LEV_OPT_CLOSE_ON_EXEC|LEV_OPT_REUSEABLE,
	    -1, (struct sockaddr*)&sin, sizeof(sin));
    if (!listener) {
    	debug(LOG_ERROR, "Couldn't create listener");
    	exit(0);
    }

    return listener;
}

int main_loop(void)
{
	struct event_base *base;
	struct evconnlistener *listener;
	int sock = socket(AF_INET, SOCK_DGRAM, 0);
	struct event timer_event, xkcp_event;

	if (sockfd < 0) {
		debug(LOG_ERROR, "ERROR, open udp socket");
		exit(0);
	}

	struct hostent *server = gethostbyname(xkcp_get_param()->remote_addr);
	if (!server) {
		debug(LOG_ERROR, "ERROR, no such host as %s", xkcp_get_param()->remote_addr);
		exit(0);
	}

	base = event_base_new();
	if (!base) {
		debug(LOG_ERROR, "event_base_new()");
		exit(0);
	}	

	struct xkcp_proxy_param  proxy_param;
	memset(&proxy_param, 0, sizeof(proxy_param));
	proxy_param.base 		= base;
	proxy_param.udp_fd 		= sock;
	proxy_param.serveraddr.sin_family 	= AF_INET;
	proxy_param.serveraddr.sin_port		= htons(xkcp_get_param()->remote_port);
	memcpy((char *)&proxy_param.serveraddr.sin_addr.s_addr, (char *)server->h_addr, server->h_length);
	listener = set_tcp_proxy_listener(base, &proxy_param);

	struct xkcp_event_param event_param;
	event_param.base 	= base;
	event_param.args 	= listener;
	event_assign(&xkcp_event, base, socket, EV_READ | EV_PERSIST, xkcp_rcv_cb, &event_param);
	event_add(&xkcp_event, NULL);

	struct timeval tv;
	event_assign(&timer_event, base, -1, EV_PERSIST, timer_event_cb, &xkcp_task_list);
	evutil_timerclear(&tv);
    tv.tv_usec = xkcp_get_param()->interval;
	event_add(&timer_event, &tv);

	event_base_dispatch(base);

	evconnlistener_free(listener);
	close(sock);
	event_base_free(base);

	return 0;
}

int xkcp_main(int argc, char **argv)
{
	xkcp_config *config = xkcp_get_config();

	config_init();

	parse_commandline(argc, argv);

	config_parse_param(config->config_file);

	if (config->daemon) {

        debug(LOG_INFO, "Forking into background");

        switch (fork()) {
        case 0:                /* child */
            setsid();
            main_loop();
            break;

        default:               /* parent */
            exit(0);
            break;
        }
    } else {
        main_loop();
    }

    return (0);                 /* never reached */
}