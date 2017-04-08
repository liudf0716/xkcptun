#ifndef	_TCP_CLIENT_
#define _TCP_CLIENT_

struct evconnlistener;
struct sockaddr;

void tcp_client_read_cb(struct evconnlistener *listener, evutil_socket_t fd,
    					 struct sockaddr *a, int slen, void *param);

#endif
