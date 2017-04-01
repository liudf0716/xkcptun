#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>


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
#include "debug.h"

IQUEUE_HEAD(xkcp_task_list);

static void
set_timer_interval(struct event *timeout)
{
	struct timeval tv;
	evutil_timerclear(&tv);
    tv.tv_usec = xkcp_get_param()->interval;
	event_add(timeout, &tv);
}

void
timer_event_cb(evutil_socket_t fd, short event, void *arg)
{
	struct event *timeout = arg;
	struct xkcp_task *task;
	iqueue_head *task_list = &xkcp_task_list;
	iqueue_foreach(task, task_list, xkcp_task_type, head) {
		if (task->kcp) {
			ikcp_update(task->kcp, iclock());
		}
	}

	set_timer_interval(timeout);
}

void
xkcp_rcv_cb(const int sock, short int which, void *arg)
{
#define	CHECK_TIME	5
	struct sockaddr_in server_sin;
	socklen_t server_sz = sizeof(server_sin);
	char buf[XKCP_RECV_BUF_LEN] = {0};
	int nrecv = 0;
	
	debug(LOG_DEBUG, "xkcp_rcv_cb [%d]", sock);
	
	while ((nrecv = recvfrom(sock, buf, sizeof(buf)-1, 0, (struct sockaddr *) &server_sin, &server_sz)) > 0) {
		ikcpcb *kcp = get_kcp_from_conv(ikcp_getconv(buf), &xkcp_task_list);
		if (kcp) {
			ikcp_input(kcp, buf, nrecv);
		}
		memset(buf, 0, XKCP_RECV_BUF_LEN);
	}
	
	int i = 0;
	for(; i < CHECK_TIME; i++) {
		char obuf[XKCP_SEND_BUF_LEN];
		struct xkcp_task *task = NULL;
		iqueue_head 	*task_list = &xkcp_task_list;
		int has_data = 0;
		iqueue_foreach(task, task_list, xkcp_task_type, head) {
			if (task->kcp) {
				has_data = 1;
				memset(obuf, 0, XKCP_SEND_BUF_LEN);
				nrecv = ikcp_recv(task->kcp, obuf, XKCP_SEND_BUF_LEN-1);
				if (nrecv > 0) {
					debug(LOG_DEBUG, "xkcp_rcv_cb: ikcp_recv [%d]", nrecv);
					evbuffer_add(bufferevent_get_output(task->b_in), obuf, nrecv);
				}
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

int main_loop(void)
{
	struct event_base *base;
	struct evconnlistener *listener;
	int xkcp_fd = socket(AF_INET, SOCK_DGRAM, 0);
	struct event timer_event, xkcp_event;

	if (xkcp_fd < 0) {
		debug(LOG_ERR, "ERROR, open udp socket");
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

	event_assign(&timer_event, base, -1, EV_PERSIST, timer_event_cb, &timer_event);
	set_timer_interval(&timer_event);

	event_assign(&xkcp_event, base, xkcp_fd, EV_READ|EV_PERSIST, xkcp_rcv_cb, NULL);
	
	event_base_dispatch(base);

	evconnlistener_free(listener);
	close(xkcp_fd);
	event_base_free(base);

	return 0;
}

int xkcp_main(int argc, char **argv)
{
	struct xkcp_config *config = xkcp_get_config();

	config_init();

	parse_commandline(argc, argv);

	if ( xkcp_parse_param(config->config_file) ) {
		debug(LOG_ERR, "xkcp_parse_param failed \n");
		usage();
		exit(0);
	}

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
