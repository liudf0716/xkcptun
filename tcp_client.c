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
	struct xkcp_task *task = ctx;

	if (what & (BEV_EVENT_EOF|BEV_EVENT_ERROR)) {
		if (task) {
			debug(LOG_DEBUG, "tcp_client_event_cb what is [%d] socket [%d]", 
				  what, bufferevent_getfd(bev));
			del_task(task);
			ikcp_release(task->kcp);
			if (task->b_in != bev) {
				bufferevent_free(task->b_in);
				printf("impossible here\n");
			}
			free(task);
		}
		bufferevent_free(bev);
	}
}

void tcp_client_read_cb(struct bufferevent *bev, void *ctx)
{
	struct xkcp_task *task = ctx;
	ikcpcb *kcp = task->kcp;
	struct evbuffer *src;
	size_t	len;
	
	src = bufferevent_get_input(bev);
	len = evbuffer_get_length(src);
	
	if (len > 0) {
		char *data = malloc(len);
		memset(data, 0, len);
		evbuffer_copyout(src, data, len);
		debug(LOG_DEBUG, "read data from server [%d] --> ikcp_send", len);
		ikcp_send(kcp, data, len);
		free(data);
	}
}
