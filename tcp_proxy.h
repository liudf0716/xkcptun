#ifndef	_TCP_PROXY_
#define	_TCP_PROXY_

#include "ikcp.h"

struct eventbase;
struct sockaddr_in;
struct bufferevent;

struct xkcp_proxy_param {
	struct event_base *base;
	int 	udp_fd;
	struct sockaddr_in	serveraddr;
};

struct xkcp_event_param {
	struct event_base 	*base;
	void	*args;
};

typedef	struct xkcp_event_param	xkcp_event_param_type;

struct xkcp_task {
	iqueue_head			head;
	ikcpcb				*kcp;
	struct bufferevent 	*b_in;
	struct sockaddr_in	*svr_addr;
};

typedef struct xkcp_task xkcp_task_type;

void tcp_proxy_accept_cb(struct evconnlistener *listener, evutil_socket_t fd,
    					 struct sockaddr *a, int slen, void *param);

#endif
