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


#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>

#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/types.h>          /* See NOTES */
#include <sys/socket.h>
#include <arpa/inet.h>
#include <net/if.h>

#include <event2/event.h>
#include <event2/event_struct.h>
#include <event2/bufferevent_ssl.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <event2/util.h>

static short port = 9087;

static void timeoutcb(evutil_socket_t fd, short what, void *arg)
{
	struct event_base *base = arg;
	printf("timeout\n");

	event_base_loopexit(base, NULL);
}

static void readcb(struct bufferevent *bev, void *ctx)
{
	struct event_base *base = ctx;
	struct evbuffer *input = bufferevent_get_input(bev);
	int  len = evbuffer_get_length(input);
    
	if (len > 0) {
		char *buf = malloc(len+1);
		memset(buf, 0, len+1);
		evbuffer_remove(input, buf, len);
		printf("%s\n", buf);
	}
	
	event_base_loopexit(base, NULL);
}

static void usage(const char *prog)
{
	printf("%s address cmd [param]\n", prog);
}

int main(int argc, char **argv)
{
	struct event *evtimeout;
  	struct timeval timeout;
	struct event_base *base;
  	struct bufferevent *bev;
	char  *cmd, *addr;
	
	if (argc < 3) {
		usage(argv[0]);
		return 1;
	}
	
	addr 	= argv[1];
	cmd		= argv[2];
	printf("addr %s \n", addr);
	
	base = event_base_new();
	if (!base) {
		puts("Couldn't open event base");
		return 1;
	}
	
	timeout.tv_sec = 2;
  	timeout.tv_usec = 0;
	
	evtimeout = evtimer_new(base, timeoutcb, base);
  	evtimer_add(evtimeout, &timeout);
	
	bev = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);
	bufferevent_setcb(bev, readcb, NULL, NULL, base);
	bufferevent_enable(bev, EV_READ|EV_WRITE);
	
	if (bufferevent_socket_connect_hostname(bev, NULL, AF_INET, addr, port) < 0) {
		bufferevent_free(bev);
		printf("bufferevent_socket_connect failed [%s]", strerror(errno));
		exit(EXIT_FAILURE);
	}
	
	printf("send cmd %s\n", cmd);
	bufferevent_write(bev, cmd, strlen(cmd));
	
	event_base_dispatch(base);
	
	bufferevent_free(bev);
	event_free(evtimeout);
	event_base_free(base);
}
