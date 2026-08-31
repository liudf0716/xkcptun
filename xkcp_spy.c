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
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <net/if.h>

#include <event2/event.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/util.h>

static void timeoutcb(evutil_socket_t fd, short what, void *arg)
{
	(void)fd;
	(void)what;
	struct event_base *base = arg;
	fprintf(stderr, "Connection timed out\n");
	event_base_loopexit(base, NULL);
}

static void eventcb(struct bufferevent *bev, short what, void *ctx)
{
	(void)bev;
	struct event_base *base = ctx;
	if (what & (BEV_EVENT_EOF|BEV_EVENT_ERROR)) {
		event_base_loopexit(base, NULL);
	}
}

static void readcb(struct bufferevent *bev, void *ctx)
{
	(void)ctx;
	struct evbuffer *input = bufferevent_get_input(bev);
	int len = evbuffer_get_length(input);

	if (len > 0) {
		char *buf = malloc(len + 1);
		if (buf) {
			memset(buf, 0, len + 1);
			evbuffer_remove(input, buf, len);
			printf("%s", buf);
			free(buf);
		}
	}
}

static void usage(const char *prog)
{
	printf("Usage: %s [-h <host>] [-s|-c|-p <port>] [-t <cmd>] [-m <param>]\n", prog);
	printf("       %s [-h <host>] [-s|-c] <cmd> [param]\n", prog);
	printf("\nOptions:\n");
	printf("  -h <host>    Target host address (default: 127.0.0.1)\n");
	printf("  -c           Query xkcp_client (default port: 9086)\n");
	printf("  -s           Query xkcp_server (default port: 9087)\n");
	printf("  -p <port>    Custom monitor port\n");
	printf("  -t <cmd>     Command to execute (e.g. list, status)\n");
	printf("  -m <param>   Parameter for command (e.g. tunnel name)\n");
	printf("\nCommands:\n");
	printf("  list         List all managed tunnels\n");
	printf("  status [name] Show active connections for all tunnels or a specific tunnel\n");
}

int main(int argc, char **argv)
{
	struct event *evtimeout = NULL;
	struct timeval timeout;
	struct event_base *base = NULL;
	struct bufferevent *bev = NULL;
	char *cmd = NULL, *addr = NULL, *param = NULL;
	int port = 0, opt;

	while ((opt = getopt(argc, argv, "h:scp:t:m:")) != -1) {
		switch (opt) {
		case 'h':
			addr = strdup(optarg);
			break;
		case 's':
			if (port == 0) port = 9087;
			break;
		case 'c':
			if (port == 0) port = 9086;
			break;
		case 'p':
			port = atoi(optarg);
			break;
		case 't':
			cmd = strdup(optarg);
			break;
		case 'm':
			param = strdup(optarg);
			break;
		default:
			usage(argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	/* Support positional arguments after flags: xkcp_spy -c list */
	if (!cmd && optind < argc) {
		cmd = strdup(argv[optind++]);
		if (!param && optind < argc) {
			param = strdup(argv[optind++]);
		}
	}

	if (!cmd)
		cmd = strdup("list");

	if (!addr)
		addr = strdup("127.0.0.1");

	if (port == 0)
		port = 9086;

	base = event_base_new();
	if (!base) {
		fprintf(stderr, "Couldn't open event base\n");
		free(cmd);
		free(addr);
		if (param) free(param);
		exit(EXIT_FAILURE);
	}

	timeout.tv_sec = 5;
	timeout.tv_usec = 0;

	evtimeout = evtimer_new(base, timeoutcb, base);
	if (evtimeout)
		evtimer_add(evtimeout, &timeout);

	bev = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);
	if (!bev) {
		fprintf(stderr, "bufferevent_socket_new failed\n");
		free(cmd);
		free(addr);
		if (param) free(param);
		if (evtimeout) event_free(evtimeout);
		event_base_free(base);
		exit(EXIT_FAILURE);
	}

	bufferevent_setcb(bev, readcb, NULL, eventcb, base);
	bufferevent_enable(bev, EV_READ|EV_WRITE);

	if (bufferevent_socket_connect_hostname(bev, NULL, AF_INET, addr, port) < 0) {
		fprintf(stderr, "bufferevent_socket_connect to %s:%d failed: [%s]\n",
			addr, port, strerror(errno));
		bufferevent_free(bev);
		if (evtimeout) event_free(evtimeout);
		event_base_free(base);
		free(addr);
		free(cmd);
		if (param) free(param);
		exit(EXIT_FAILURE);
	}
	free(addr);

	if (!param) {
		bufferevent_write(bev, cmd, strlen(cmd));
	} else {
		int size = strlen(cmd) + strlen(param) + 2;
		char *input = malloc(size);
		if (input) {
			snprintf(input, size, "%s %s", cmd, param);
			bufferevent_write(bev, input, strlen(input));
			free(input);
		}
		free(param);
	}
	free(cmd);

	event_base_dispatch(base);

	bufferevent_free(bev);
	if (evtimeout)
		event_free(evtimeout);
	event_base_free(base);
	return 0;
}
