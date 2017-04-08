#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>

#include <syslog.h>

#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <event2/util.h>


#include "xkcp_util.h"
#include "debug.h"
#include "tcp_client.h"

void tcp_client_event_cb(struct bufferevent *bev, short what, void *ctx)
{
}

void tcp_client_read_cb(struct evconnlistener *listener, evutil_socket_t fd,
    struct sockaddr *a, int slen, void *param)
{
}
