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
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <arpa/inet.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <syslog.h>

#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <event2/util.h>
#include <event2/dns.h>

#include "xkcp_server.h"
#include "xkcp_util.h"
#include "tcp_client.h"
#include "debug.h"
#include "ikcp.h"
#include "jwHash.h"

static int parse_proxy_host_port(const char *str, char *host, size_t host_sz, uint16_t *port)
{
	if (!str || !str[0]) return -1;
	const char *p = str;
	if (strncmp(p, "socks5://", 9) == 0) p += 9;
	else if (strncmp(p, "socks5h://", 10) == 0) p += 10;
	else if (strncmp(p, "tcp://", 6) == 0) p += 6;

	const char *colon = strrchr(p, ':');
	if (colon) {
		size_t hlen = colon - p;
		if (hlen == 0) {
			snprintf(host, host_sz, "127.0.0.1");
		} else {
			if (hlen >= host_sz) hlen = host_sz - 1;
			memcpy(host, p, hlen);
			host[hlen] = '\0';
		}
		*port = (uint16_t)atoi(colon + 1);
	} else {
		snprintf(host, host_sz, "%s", p);
		*port = 1080;
	}
	return (*port > 0) ? 0 : -1;
}

int xkcp_server_connect_direct(struct xkcp_task *task, const char *host, uint16_t port)
{
	if (!task || !host || port == 0)
		return -1;

	struct xkcp_tunnel *tunnel = task->tunnel;
	struct event_base *base = tunnel ? tunnel->base : NULL;
	struct evdns_base *dns_base = (tunnel && tunnel->mgr) ? tunnel->mgr->dns_base : NULL;
	if (!base) return -1;

	struct bufferevent *bev = bufferevent_socket_new(base, -1,
		BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);
	if (!bev) {
		debug(LOG_ERR, "bufferevent_socket_new failed [%s]", strerror(errno));
		return -1;
	}

	task->bev = bev;
	task->egress_state = XKCP_EGRESS_DIRECT;
	bufferevent_setcb(bev, tcp_client_read_cb, NULL, tcp_client_event_cb, task);
	bufferevent_enable(bev, EV_READ | EV_WRITE);

	struct sockaddr_in sin;
	struct sockaddr_in6 sin6;
	int ret;

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(port);
	memset(&sin6, 0, sizeof(sin6));
	sin6.sin6_family = AF_INET6;
	sin6.sin6_port = htons(port);

	if (inet_pton(AF_INET, host, &sin.sin_addr) == 1) {
		ret = bufferevent_socket_connect(bev, (struct sockaddr *)&sin, sizeof(sin));
	} else if (inet_pton(AF_INET6, host, &sin6.sin6_addr) == 1) {
		ret = bufferevent_socket_connect(bev, (struct sockaddr *)&sin6, sizeof(sin6));
	} else {
		ret = bufferevent_socket_connect_hostname(bev, dns_base, AF_UNSPEC, host, port);
	}

	if (ret < 0) {
		bufferevent_free(bev);
		task->bev = NULL;
		debug(LOG_ERR, "connect to direct target %s:%u failed", host, port);
		return -1;
	}

	debug(LOG_INFO, "[%s] conv [%u] connecting direct to target [%s]:[%u]",
	      tunnel ? tunnel->name : "server", task->conv, host, port);
	return 0;
}

static int xkcp_server_connect_proxy(struct xkcp_task *task, const char *proxy_str,
				     const char *target_host, uint16_t target_port)
{
	(void)target_host;
	(void)target_port;
	if (!task || !proxy_str)
		return -1;

	struct xkcp_tunnel *tunnel = task->tunnel;
	struct event_base *base = tunnel ? tunnel->base : NULL;
	struct evdns_base *dns_base = (tunnel && tunnel->mgr) ? tunnel->mgr->dns_base : NULL;
	if (!base) return -1;

	char phost[128] = {0};
	uint16_t pport = 0;
	if (parse_proxy_host_port(proxy_str, phost, sizeof(phost), &pport) < 0) {
		debug(LOG_ERR, "[%s] invalid egress_proxy string [%s]",
		      tunnel ? tunnel->name : "server", proxy_str);
		return -1;
	}

	struct bufferevent *bev = bufferevent_socket_new(base, -1,
		BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);
	if (!bev) {
		debug(LOG_ERR, "bufferevent_socket_new for proxy failed [%s]", strerror(errno));
		return -1;
	}

	task->bev = bev;
	task->egress_state = XKCP_EGRESS_S5_CONNECTING;
	bufferevent_setcb(bev, tcp_client_read_cb, NULL, tcp_client_event_cb, task);
	bufferevent_enable(bev, EV_READ | EV_WRITE);

	struct sockaddr_in sin;
	struct sockaddr_in6 sin6;
	int ret;

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(pport);
	memset(&sin6, 0, sizeof(sin6));
	sin6.sin6_family = AF_INET6;
	sin6.sin6_port = htons(pport);

	if (inet_pton(AF_INET, phost, &sin.sin_addr) == 1) {
		ret = bufferevent_socket_connect(bev, (struct sockaddr *)&sin, sizeof(sin));
	} else if (inet_pton(AF_INET6, phost, &sin6.sin6_addr) == 1) {
		ret = bufferevent_socket_connect(bev, (struct sockaddr *)&sin6, sizeof(sin6));
	} else {
		ret = bufferevent_socket_connect_hostname(bev, dns_base, AF_UNSPEC, phost, pport);
	}

	if (ret < 0) {
		bufferevent_free(bev);
		task->bev = NULL;
		debug(LOG_ERR, "connect to egress_proxy %s:%u failed", phost, pport);
		return -1;
	}

	debug(LOG_INFO, "[%s] conv [%u] connecting to egress_proxy [%s:%u] for target [%s]:[%u]",
	      tunnel ? tunnel->name : "server", task->conv, phost, pport,
	      task->target_host, task->target_port);
	return 0;
}

int xkcp_server_connect_target(struct xkcp_task *task, const char *host, uint16_t port)
{
	if (!task || !host || port == 0)
		return -1;

	snprintf(task->target_host, sizeof(task->target_host), "%s", host);
	task->target_port = port;

	struct xkcp_tunnel *tunnel = task->tunnel;
	const char *egress_proxy = (tunnel && tunnel->param.egress_proxy && tunnel->param.egress_proxy[0]) ?
				    tunnel->param.egress_proxy : NULL;

	if (egress_proxy) {
		int ret = xkcp_server_connect_proxy(task, egress_proxy, host, port);
		if (ret == 0)
			return 0;

		if (tunnel->param.egress_fallback_direct) {
			debug(LOG_WARNING, "[%s] conv [%u] egress_proxy [%s] connect failed immediately, falling back to direct target [%s]:[%u]",
			      tunnel->name, task->conv, egress_proxy, host, port);
			return xkcp_server_connect_direct(task, host, port);
		}
		return -1;
	}

	return xkcp_server_connect_direct(task, host, port);
}

void tcp_client_event_cb(struct bufferevent *bev, short what, void *ctx)
{
	struct xkcp_task *task = ctx;
	if (!task) return;
	struct xkcp_tunnel *tunnel = task->tunnel;

	if (what & BEV_EVENT_CONNECTED) {
		xkcp_set_tcp_nodelay(bufferevent_getfd(bev));

		if (task->egress_state == XKCP_EGRESS_S5_CONNECTING) {
			uint8_t greeting[3] = {0x05, 0x01, 0x00};
			bufferevent_write(bev, greeting, sizeof(greeting));
			task->egress_state = XKCP_EGRESS_S5_GREETING_SENT;
			debug(LOG_INFO, "[%s] conv [%u] connected to egress_proxy [%s], sent SOCKS5 greeting for [%s]:[%u]",
			      tunnel ? tunnel->name : "server", task->conv,
			      tunnel && tunnel->param.egress_proxy ? tunnel->param.egress_proxy : "",
			      task->target_host, task->target_port);
			return;
		}

		debug(LOG_INFO, "[%s] conv [%u] connected to target [%s]:[%u]%s",
		      tunnel ? tunnel->name : "server", task->conv,
		      task->target_host, task->target_port,
		      task->egress_state == XKCP_EGRESS_DIRECT ? " (direct)" : "");

		if (task->pending_buf && evbuffer_get_length(task->pending_buf) > 0) {
			evbuffer_add_buffer(bufferevent_get_output(bev), task->pending_buf);
			evbuffer_free(task->pending_buf);
			task->pending_buf = NULL;
		}
		xkcp_forward_data(task);
		return;
	}

	if (what & (BEV_EVENT_EOF | BEV_EVENT_ERROR)) {
		int in_proxy_handshake = (task->egress_state == XKCP_EGRESS_S5_CONNECTING ||
					  task->egress_state == XKCP_EGRESS_S5_GREETING_SENT ||
					  task->egress_state == XKCP_EGRESS_S5_REQUEST_SENT);

		if (in_proxy_handshake && tunnel && tunnel->param.egress_fallback_direct) {
			debug(LOG_WARNING, "[%s] conv [%u] egress_proxy [%s] failed (%s), falling back to direct connection to [%s]:[%u]",
			      tunnel->name, task->conv,
			      tunnel->param.egress_proxy ? tunnel->param.egress_proxy : "",
			      (what & BEV_EVENT_EOF) ? "EOF" : strerror(errno),
			      task->target_host, task->target_port);

			bufferevent_free(bev);
			task->bev = NULL;
			task->egress_state = XKCP_EGRESS_DIRECT;

			if (xkcp_server_connect_direct(task, task->target_host, task->target_port) < 0) {
				debug(LOG_ERR, "[%s] conv [%u] direct fallback to [%s]:[%u] failed",
				      tunnel->name, task->conv, task->target_host, task->target_port);
				void *puser = xkcp_tcp_event_cb(NULL, what, task);
				if (puser) free(puser);
				clean_useless_client(tunnel);
			}
			return;
		}

		void *puser = xkcp_tcp_event_cb(bev, what, ctx);
		if (puser) free(puser);

		if (tunnel)
			clean_useless_client(tunnel);
	}
}

void tcp_client_read_cb(struct bufferevent *bev, void *ctx)
{
	struct xkcp_task *task = ctx;
	if (!task) return;
	struct xkcp_tunnel *tunnel = task->tunnel;
	struct evbuffer *input = bufferevent_get_input(bev);

	if (task->egress_state == XKCP_EGRESS_S5_GREETING_SENT) {
		if (evbuffer_get_length(input) < 2)
			return;
		uint8_t rep[2];
		evbuffer_copyout(input, rep, 2);
		if (rep[0] != 0x05 || rep[1] != 0x00) {
			debug(LOG_WARNING, "[%s] conv [%u] egress_proxy rejected auth (0x%02x, 0x%02x)",
			      tunnel ? tunnel->name : "server", task->conv, rep[0], rep[1]);
			goto proxy_fail;
		}
		evbuffer_drain(input, 2);

		uint8_t req[512];
		req[0] = 0x05;
		req[1] = 0x01;
		req[2] = 0x00;
		size_t req_len = 0;

		struct in_addr sin_addr;
		struct in6_addr sin6_addr;
		if (inet_pton(AF_INET, task->target_host, &sin_addr) == 1) {
			req[3] = 0x01;
			memcpy(req + 4, &sin_addr.s_addr, 4);
			uint16_t p = htons(task->target_port);
			memcpy(req + 8, &p, 2);
			req_len = 10;
		} else if (inet_pton(AF_INET6, task->target_host, &sin6_addr) == 1) {
			req[3] = 0x04;
			memcpy(req + 4, &sin6_addr, 16);
			uint16_t p = htons(task->target_port);
			memcpy(req + 20, &p, 2);
			req_len = 22;
		} else {
			req[3] = 0x03;
			size_t dlen = strlen(task->target_host);
			if (dlen > 255) dlen = 255;
			req[4] = (uint8_t)dlen;
			memcpy(req + 5, task->target_host, dlen);
			uint16_t p = htons(task->target_port);
			memcpy(req + 5 + dlen, &p, 2);
			req_len = 5 + dlen + 2;
		}

		bufferevent_write(bev, req, req_len);
		task->egress_state = XKCP_EGRESS_S5_REQUEST_SENT;
		debug(LOG_INFO, "[%s] conv [%u] egress_proxy sent CONNECT for [%s]:[%u]",
		      tunnel ? tunnel->name : "server", task->conv,
		      task->target_host, task->target_port);

		if (evbuffer_get_length(input) == 0)
			return;
	}

	if (task->egress_state == XKCP_EGRESS_S5_REQUEST_SENT) {
		size_t in_len = evbuffer_get_length(input);
		if (in_len < 4)
			return;
		uint8_t rep[5];
		evbuffer_copyout(input, rep, in_len < 5 ? in_len : 5);
		if (rep[0] != 0x05 || rep[1] != 0x00) {
			debug(LOG_WARNING, "[%s] conv [%u] egress_proxy connect to [%s]:[%u] failed (REP=0x%02x)",
			      tunnel ? tunnel->name : "server", task->conv,
			      task->target_host, task->target_port, rep[1]);
			goto proxy_fail;
		}

		/* Drain variable-length SOCKS5 BND.ADDR + BND.PORT before forwarding remote stream */
		size_t needed = 0;
		if (rep[3] == 0x01) {
			needed = 4 + 4 + 2;
		} else if (rep[3] == 0x04) {
			needed = 4 + 16 + 2;
		} else if (rep[3] == 0x03) {
			if (in_len < 5) return;
			uint8_t dlen = rep[4];
			needed = 4 + 1 + dlen + 2;
		} else {
			debug(LOG_WARNING, "[%s] conv [%u] egress_proxy unknown atyp 0x%02x",
			      tunnel ? tunnel->name : "server", task->conv, rep[3]);
			goto proxy_fail;
		}

		if (in_len < needed)
			return;

		evbuffer_drain(input, needed);
		task->egress_state = XKCP_EGRESS_READY;

		debug(LOG_INFO, "[%s] conv [%u] egress_proxy tunnel ready -> [%s]:[%u]",
		      tunnel ? tunnel->name : "server", task->conv,
		      task->target_host, task->target_port);

		if (task->pending_buf && evbuffer_get_length(task->pending_buf) > 0) {
			evbuffer_add_buffer(bufferevent_get_output(bev), task->pending_buf);
			evbuffer_free(task->pending_buf);
			task->pending_buf = NULL;
		}

		if (evbuffer_get_length(input) == 0)
			return;
	}

	ikcpcb *kcp = task->kcp;
	if (!kcp)
		return;
	xkcp_tcp_read_cb(bev, kcp);
	task->last_keepalive = task->last_active = task->last_recv = iclock();
	xkcp_forward_data(task);
	return;

proxy_fail:
	if (tunnel && tunnel->param.egress_fallback_direct) {
		debug(LOG_WARNING, "[%s] conv [%u] egress_proxy handshake failed, falling back to direct connection to [%s]:[%u]",
		      tunnel->name, task->conv, task->target_host, task->target_port);
		bufferevent_free(bev);
		task->bev = NULL;
		task->egress_state = XKCP_EGRESS_DIRECT;

		if (xkcp_server_connect_direct(task, task->target_host, task->target_port) < 0) {
			debug(LOG_ERR, "[%s] conv [%u] direct fallback to [%s]:[%u] failed",
			      tunnel->name, task->conv, task->target_host, task->target_port);
			void *puser = xkcp_tcp_event_cb(NULL, BEV_EVENT_ERROR, task);
			if (puser) free(puser);
			clean_useless_client(tunnel);
		}
	} else {
		void *puser = xkcp_tcp_event_cb(bev, BEV_EVENT_ERROR, task);
		if (puser) free(puser);
		if (tunnel) clean_useless_client(tunnel);
	}
}
