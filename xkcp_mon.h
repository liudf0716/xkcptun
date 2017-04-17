#ifndef _XKCP_MON_
#define _XKCP_MON_

void xkcp_mon_accept_cb(struct evconnlistener *listener, evutil_socket_t fd,
    struct sockaddr *a, int slen, void *param);
    
#endif
