#ifndef	_TCP_CLIENT_
#define _TCP_CLIENT_

struct evconnlistener;
struct bufferevent;
struct sockaddr;

void tcp_client_event_cb(struct bufferevent *bev, short what, void *ctx);

void tcp_client_read_cb(struct evconnlistener *listener, evutil_socket_t fd,
    					 struct sockaddr *a, int slen, void *param);

#endif
