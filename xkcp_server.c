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
#include "jwHash.h"
#include "xkcp_server.h"
#include "xkcp_config.h"
#include "xkcp_util.h"
#include "xkcp_mon.h"
#include "tcp_client.h"

#ifndef NI_MAXHOST
#define NI_MAXHOST      1025
#endif
#ifndef NI_MAXSERV
#define NI_MAXSERV      32
#endif

static short mport = 9087;
static jwHashTable *xkcp_hash = NULL;

jwHashTable * get_xkcp_hash()
{
	return xkcp_hash;
}

static void timer_event_cb(evutil_socket_t fd, short event, void *arg)
{
	hash_iterator(xkcp_hash, (void*)xkcp_update_task_list, HASHPTR);
	
	set_timer_interval(arg);
}

static struct xkcp_task *create_new_tcp_connection(const int xkcpfd, struct event_base *base,
			struct sockaddr_in *from, int from_len, int conv, iqueue_head *task_list)
{
	struct xkcp_proxy_param *param = malloc(sizeof(struct xkcp_proxy_param));
	assert(param);
	memset(param, 0, sizeof(struct xkcp_proxy_param));
	memcpy(&param->sockaddr, from, from_len);
	param->xkcpfd = xkcpfd;
	param->addr_len = from_len;

	ikcpcb *kcp_server = ikcp_create(conv, param);
	xkcp_set_config_param(kcp_server);

	struct xkcp_task *task = malloc(sizeof(struct xkcp_task));
	assert(task);
	task->kcp = kcp_server;		
	task->sockaddr = &param->sockaddr;
	
	struct bufferevent *bev = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);
	if (!bev) {
		debug(LOG_ERR, "bufferevent_socket_new failed [%s]", strerror(errno));
		goto err;
	}
	
	task->bev = bev;
	bufferevent_setcb(bev, tcp_client_read_cb, NULL, tcp_client_event_cb, task);
	bufferevent_enable(bev, EV_READ);
	if (bufferevent_socket_connect_hostname(bev, NULL, AF_INET, 
										   xkcp_get_param()->remote_addr,
										   xkcp_get_param()->remote_port) < 0) {
		bufferevent_free(bev);
		debug(LOG_ERR, "bufferevent_socket_connect failed [%s]", strerror(errno));
		goto err;
	}
	add_task_tail(task, task_list);
	debug(LOG_INFO, "tcp client [%d] connect to [%s]:[%d] success", bufferevent_getfd(bev),
		  xkcp_get_param()->remote_addr, xkcp_get_param()->remote_port);
	return task;
err:
	// Asserts ensure param and task are non-NULL if execution reaches here via goto.
	// kcp_server is also non-NULL and assigned to task->kcp.
	if (task->kcp) { // task is not NULL (assert), task->kcp is kcp_server
		ikcp_release(task->kcp);
		// task->kcp->user (which is param) is NOT freed by ikcp_release.
	}
	free(param); // Free the user data for kcp
	free(task);  // Free the task structure
	return NULL;
}

static void accept_client_data(const int xkcpfd, struct event_base *base,
			struct sockaddr_in *from, int from_len, char *data, int len)
{
	char host[NI_MAXHOST] = {0};
    char serv[NI_MAXSERV] = {0};
	char key[NI_MAXHOST+NI_MAXSERV+1] = {0};
	
	int nret = getnameinfo((struct sockaddr *) from, from_len,
                    host, sizeof(host), serv, sizeof(serv),
                    NI_NUMERICHOST | NI_DGRAM);
	if (nret) {
		debug(LOG_INFO, "accept_new_client: getnameinfo error %s", strerror(errno));
		return ;
	}
	
	iqueue_head *task_list = NULL;
	snprintf(key, NI_MAXHOST+NI_MAXSERV+1, "%s:%s", host, serv);
	struct xkcp_task *task = NULL;
	int conv = ikcp_getconv(data);
	debug(LOG_DEBUG, "accept_new_client: [%s:%s] conv [%d] len [%d]", host, serv, conv, len);
	if (get_ptr_by_str(xkcp_hash, key, (void*)&task_list) == HASHOK) {
		//old client	
		task = get_task_from_conv(conv, task_list);
		debug(LOG_DEBUG, "old client, task is %d", task!=NULL?1:0);
		if (!task) {
			// new tcp connection
			task = create_new_tcp_connection(xkcpfd, base, from, from_len, conv, task_list);
		}
	} else {
		// new client
		if (!task_list) {
			debug(LOG_DEBUG, "new client");
			task_list = malloc(sizeof(iqueue_head));
			iqueue_init(task_list);
		}
		add_ptr_by_str(xkcp_hash, key, task_list);
		task = create_new_tcp_connection(xkcpfd, base, from, from_len, conv, task_list);
	}
	
	if (task && task->kcp) {
		int nret = ikcp_input(task->kcp, data, len);
		if (nret < 0) {
			debug(LOG_INFO, "[%d] ikcp_input failed [%d]", task->kcp->conv, nret);
		}
	}
	
	if (task_list) {
		debug(LOG_DEBUG, "accept_client_data: xkcp_forward_all_data ...");
		xkcp_forward_all_data(task_list);
	}
}

static void xkcp_rcv_cb(const int sock, short int which, void *arg)
{	
	struct event_base *base = arg;
	struct sockaddr_in clientaddr;
	int clientlen = sizeof(clientaddr);
	memset(&clientaddr, 0, clientlen);
	
	char buf[BUF_RECV_LEN] = {0};
	int len = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *) &clientaddr, (socklen_t*)&clientlen);
	if (len > 0) {
		accept_client_data(sock, base, &clientaddr, clientlen, buf, len);
	}	
}

static int set_xkcp_listener()
{
	short lport = xkcp_get_param()->local_port;
	struct sockaddr_in sin;
	char *addr = get_iface_ip(xkcp_get_param()->local_interface);
	if (!addr) {
		debug(LOG_ERR, "get_iface_ip [%s] failed", xkcp_get_param()->local_interface);
		free(addr); // Free addr even if it's NULL (safe)
		exit(0);
	}
	
	memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = inet_addr(addr);
    sin.sin_port = htons(lport);
	
	int xkcp_fd = socket(AF_INET, SOCK_DGRAM, 0);
	
	if (bind(xkcp_fd, (struct sockaddr *) &sin, sizeof(sin))) {
		debug(LOG_ERR, "xkcp_fd bind() failed %s ", strerror(errno));
		free(addr);
		exit(EXIT_FAILURE);
	}
	
	free(addr);
	return xkcp_fd;
}

static void task_list_free(iqueue_head *task_list)
{
	if (task_list == NULL) {
		return;
	}

	iqueue_head *p, *n;
	for (p = (task_list)->next; p != (task_list); p = n) {
		n = p->next;
		struct xkcp_task *task = iqueue_entry(p, struct xkcp_task, head);

		if (task->kcp) {
			struct xkcp_proxy_param *param = (struct xkcp_proxy_param *)task->kcp->user;
			ikcp_release(task->kcp);
			if (param) {
				free(param);
			}
			task->kcp = NULL;
		}

		if (task->bev) {
			bufferevent_free(task->bev);
			task->bev = NULL;
		}

		iqueue_del(&task->head); // or iqueue_del(p); since task->head is p
		free(task);
	}

	free(task_list);
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
	
	xkcp_hash = create_hash(100);
	
	int xkcp_fd = set_xkcp_listener();
	
	mon_listener = set_xkcp_mon_listener(base, mport, xkcp_hash);
	set_xkcp_server_flag(1); // set it's xkcp server
	
	xkcp_event = event_new(base, xkcp_fd, EV_READ|EV_PERSIST, xkcp_rcv_cb, base);
	event_add(xkcp_event, NULL);
	
	event_assign(&timer_event, base, -1, EV_PERSIST, timer_event_cb, &timer_event);
	set_timer_interval(&timer_event);	

	event_base_dispatch(base);
	
	evconnlistener_free(mon_listener);
	close(xkcp_fd);
	event_base_free(base);
	delete_hash(xkcp_hash, (void*)task_list_free, HASHPTR/*value*/, HASHSTRING/*key*/);
	
	return 0;
}

int main(int argc, char **argv) 
{
	struct xkcp_config *config = xkcp_get_config();
	config->main_loop = server_main_loop;
	
	return xkcp_main(argc, argv);
}

