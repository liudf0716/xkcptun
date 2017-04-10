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
#include "tcp_client.h"

char *response = "HTTP/1.1 502 Bad Gateway \r\n \
Server: nginx/1.4.6 (Ubuntu)\r\n \
Date: Wed, 05 Apr 2017 10:02:04 GMT\r\n \
Content-Type: text/html\r\n \
Connection: keep-alive\r\n\r\n \
<html>\n \
<head><title>502 Bad Gateway</title></head>\n \
<center><h1>502 Bad Gateway</h1></center>\n \
<hr><center>nginx/1.4.6 (Ubuntu)</center>\n \
</body>\n \
</html>\n";

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
			
			char obuf[OBUF_SIZE];
			while(1) {
				memset(obuf, 0, OBUF_SIZE);
				int nrecv = ikcp_recv(task->kcp, obuf, OBUF_SIZE);
				if (nrecv < 0)
					break;
		
				debug(LOG_DEBUG, "ikcp_recv [%d] [%s]", nrecv, obuf);
				evbuffer_add(bufferevent_get_output(task->b_in), obuf, nrecv);
				//ikcp_send(kcp_client, response, strlen(response));
			}
		}
	}
	
	evtimer_add(timeout, &TIMER_TV);
}

static void xkcp_rcv_cb(const int sock, short int which, void *arg)
{	
	struct event_base *base = arg;
	struct sockaddr_in clientaddr;
	int clientlen = sizeof(clientaddr);
	memset(&clientaddr, 0, clientlen);
	
	/* Recv the data, store the address of the sender in server_sin */
	char buf[BUF_RECV_LEN] = {0};
	int len = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *) &clientaddr, &clientlen);
	if (len > 0) {
		int conv = ikcp_getconv(buf);
		ikcpcb *kcp_client = get_kcp_from_conv(conv, &xkcp_task_list);
		debug(LOG_DEBUG, "conv is %d, kcp_client is %d", conv, kcp_client?1:0);
		if (kcp_client == NULL) {
			struct xkcp_proxy_param *param = malloc(sizeof(struct xkcp_proxy_param));
			memset(param, 0, sizeof(struct xkcp_proxy_param));
			memcpy(&param->serveraddr, &clientaddr, clientlen);
			param->udp_fd = sock;
			param->addr_len = clientlen;
			kcp_client = ikcp_create(conv, param);
			kcp_client->output	= xkcp_output;
			ikcp_wndsize(kcp_client, 128, 128);
			ikcp_nodelay(kcp_client, 0, 10, 0, 1);
			
			struct xkcp_task *task = malloc(sizeof(struct xkcp_task));
			assert(task);
			task->kcp = kcp_client;		
			task->svr_addr = &param->serveraddr;
			add_task_tail(task, &xkcp_task_list);
			
			struct sockaddr_in sin;
			memset(&sin, 0, sizeof(sin));
			sin.sin_family = AF_INET;
			sin.sin_addr.s_addr = htonl(0x7f000001); /* 127.0.0.1 */
			sin.sin_port = htons(xkcp_get_param()->remote_port);
			
			struct bufferevent *bev = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);
			if (!bev) {
				debug(LOG_ERR, "bufferevent_socket_new failed [%s]", strerror(errno));
				exit(EXIT_FAILURE);
			}
			task->b_in = bev;
			bufferevent_setcb(bev, tcp_client_read_cb, NULL, tcp_client_event_cb, task);
    		bufferevent_enable(bev, EV_READ|EV_WRITE);
			bufferevent_socket_connect(bev, (struct sockaddr *)&sin, sizeof(sin));
		}
		
		ikcp_input(kcp_client, buf, len);
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
	struct event timer_event, 
	  			*xkcp_event = NULL;
	struct event_base *base = NULL;
	
	base = event_base_new();
	if (!base) {
		debug(LOG_ERR, "event_base_new()");
		exit(0);
	}		
	
	int xkcp_fd = set_xkcp_listener();
	
	xkcp_event = event_new(base, xkcp_fd, EV_READ|EV_PERSIST, xkcp_rcv_cb, base);
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

