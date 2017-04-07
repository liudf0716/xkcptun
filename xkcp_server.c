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

IQUEUE_HEAD(xkcp_task_list);

static int
xkcp_output(const char *buf, int len, ikcpcb *kcp, void *user)
{
	struct xkcp_proxy_param *ptr = user;
	int nret = sendto(ptr->udp_fd, buf, len, 0, &ptr->serveraddr, sizeof(ptr->serveraddr));
	debug(LOG_DEBUG, "xkcp output [%d] [%d], send datagram from %d (%s)", 
		  ptr->udp_fd, len, nret, strerror(errno));
	return nret;
}

static void timer_event_cb(int nothing, short int which, void *ev)
{
	struct event *timeout = ev;
	struct xkcp_task *task;
	iqueue_head *task_list = &xkcp_task_list;
	iqueue_foreach(task, task_list, xkcp_task_type, head) {
		if (task->kcp) {
			ikcp_update(task->kcp, iclock());		
		}
	}
	
	evtimer_add(timeout, &TIMER_TV);
}

static void xkcp_rcv_cb(const int sock, short int which, void *arg)
{	
	char buf[BUF_RECV_LEN] = {0};

	/* Recv the data, store the address of the sender in server_sin */
	int len = recvfrom(sock, &buf, sizeof(buf) - 1, 0, (struct sockaddr *) &param->serveraddr, &param->addr_len);
	if (len > 0) {
		int conv = ikcp_getconv(buf);
		ikcpcb *kcp_client = get_kcp_from_conv(conv, &xkcp_task_list);
		debug(LOG_DEBUG, "conv is %d, kcp_client is %d", conv, kcp_client?1:0);
		if (kcp_client == NULL) {
			struct xkcp_proxy_param *param = malloc(sizeof(struct xkcp_proxy_param));
			memset(param, 0, sizeof(struct xkcp_proxy_param));
			param->udp_fd = sock;
			param->addr_len = sizeof(struct sockaddr_in);
			kcp_client = ikcp_create(conv, param);
			kcp_client->output	= xkcp_output;
			ikcp_wndsize(kcp_client, 128, 128);
			ikcp_nodelay(kcp_client, 0, 10, 0, 1);
		}
		
		ikcp_input(kcp_client, buf, len);

		struct xkcp_task *task = malloc(sizeof(struct xkcp_task));
		assert(task);
		task->kcp = kcp_client;
		task->svr_addr = &param->serveraddr;
		add_task_tail(task, &xkcp_task_list);
		
		while(1) {
			char data[1024] = {0};
			if (ikcp_recv(kcp_client, data, 1023) > 0) {
				debug(LOG_DEBUG, "recv data is %s \n response is %s", data, response);
				ikcp_send(kcp_client, response, strlen(response));
			} else
				break;
		}
	} 
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
	struct event timer_event, *xkcp_event;
	struct event_base *base;
	
	base = event_base_new();
	if (!base) {
		debug(LOG_ERR, "event_base_new()");
		exit(0);
	}	
	
	struct hostent *server = gethostbyname(xkcp_get_param()->remote_addr);
	if (!server) {
		debug(LOG_ERR, "ERROR, no such host as %s", xkcp_get_param()->remote_addr);
		exit(0);
	}
	
	int xkcp_fd = set_xkcp_listener();
	
	xkcp_event = event_new(base, xkcp_fd, EV_READ|EV_PERSIST, xkcp_rcv_cb, NULL);
	event_add(xkcp_event, NULL);
	
	event_assign(&timer_event, base, -1, EV_PERSIST, timer_event_cb, &timer_event);
	set_timer_interval(&timer_event);	

	event_base_dispatch(base);
	
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

