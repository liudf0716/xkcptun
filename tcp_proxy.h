#ifndef	_TCP_PROXY_
#define	_TCP_PROXY_

struct xkcp_proxy_param {
	struct event_base *base;
	int 	udp_fd;
	struct sockaddr_in	serveraddr;
};

struct xkcp_event_param {
	struct event_base 	*base;
	void	*args;
}

struct xkcp_task {
	iqueue_head			*head;
	ikcpcb				*kcp;
	struct bufferevent 	*b_in;
};

#endif