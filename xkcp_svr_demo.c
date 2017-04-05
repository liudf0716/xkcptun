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

#include <event.h>

#include "ikcp.h"
#include "debug.h"
#include "xkcp_util.h"

#define	BUF_RECV_LEN	1500

static struct timeval TIMER_TV = {0, 10};

IQUEUE_HEAD(xkcp_task_list);

char *response = "HTTP/1.1 502 Bad Gateway \r\n \
	Server: nginx/1.4.6 (Ubuntu)\r\n \
	Date: Wed, 05 Apr 2017 10:02:04 GMT\r\n \
	Content-Type: text/html\r\n \
	Connection: keep-alive\r\n\r\n \
	<html> \
	<head><title>502 Bad Gateway</title></head> \
	<center><h1>502 Bad Gateway</h1></center> \
	<hr><center>nginx/1.4.6 (Ubuntu)</center> \
	</body> \
	</html>";

static int
xkcp_output(const char *buf, int len, ikcpcb *kcp, void *user)
{
	struct xkcp_proxy_param *ptr = user;
	struct hostent *hostp = gethostbyaddr((const char *)&ptr->serveraddr.sin_addr.s_addr,
              sizeof(ptr->serveraddr.sin_addr.s_addr), AF_INET);
	char *hostaddrp = inet_ntoa(ptr->serveraddr.sin_addr);
	
	int nret = sendto(ptr->udp_fd, buf, len, 0, &ptr->serveraddr, sizeof(ptr->serveraddr));
	debug(LOG_DEBUG, "xkcp output [%d] [%d], send datagram from %d (%s)", 
		  ptr->udp_fd, len, nret, strerror(errno));
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


static void udp_cb(const int sock, short int which, void *arg)
{
	struct xkcp_proxy_param *param = malloc(sizeof(struct xkcp_proxy_param));
	memset(param, 0, sizeof(struct xkcp_proxy_param));
	param->addr_len = sizeof(struct sockaddr_in);
	char buf[BUF_RECV_LEN] = {0};

	/* Recv the data, store the address of the sender in server_sin */
	int len = recvfrom(sock, &buf, sizeof(buf) - 1, 0, (struct sockaddr *) &param->serveraddr, &param->addr_len);
	debug(LOG_DEBUG, "udp_cb receive [%d] [%d]", len, param->addr_len);
	if ( len == -1) {
		perror("recvfrom()");
		free(param);
		event_loopbreak();
	} else if (len > 0) {
		int conv = ikcp_getconv(buf);
		ikcpcb *kcp_client = get_kcp_from_conv(conv, &xkcp_task_list);
		debug(LOG_DEBUG, "conv is %d, kcp_client is %d", conv, kcp_client?1:0);
		if (kcp_client == NULL) {
			param->udp_fd = sock;
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

int main(int argc, char **argv)
{
	int sock;
	struct event timer_event, udp_event;
	struct sockaddr_in sin;
	
	if (argc != 3) {
		printf("%s ip port\n", argv[0]);
		return 0;
	}
	
	short port = atoi(argv[2]);
	char  *host = argv[1];
	sock = socket(AF_INET, SOCK_DGRAM, 0);
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(INADDR_ANY);
	sin.sin_port = htons(port);
	
	debug(LOG_DEBUG, "UDP SERVER PORT is %d", port);
	
	if (bind(sock, (struct sockaddr *) &sin, sizeof(sin))) {
		perror("bind()");
		exit(EXIT_FAILURE);
	}

	/* Initialize libevent */
	event_init();

	/* Add the clock event */
	evtimer_set(&timer_event, &timer_event_cb, &timer_event);
	evtimer_add(&timer_event, &TIMER_TV);
	
	/* Add the UDP event */
	event_set(&udp_event, sock, EV_READ|EV_PERSIST, udp_cb, NULL);
	event_add(&udp_event, 0);

	/* Enter the event loop; does not return. */
	event_dispatch();
	close(sock);
	return 0;
}
