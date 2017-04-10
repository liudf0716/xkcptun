#ifndef	_TCP_CLIENT_
#define _TCP_CLIENT_

struct evconnlistener;
struct bufferevent;
struct sockaddr;

void tcp_client_event_cb(struct bufferevent *bev, short what, void *ctx);

void tcp_client_read_cb(struct bufferevent *bev, void *ctx);

#endif
