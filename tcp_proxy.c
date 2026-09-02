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

#include <sys/socket.h>
#include <netinet/in.h>
#include <syslog.h>

#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <event2/util.h>

#include "xkcp_util.h"
#include "xkcp_proto.h"
#include "xkcp_auth.h"
#include "tcp_proxy.h"
#include "xkcp_client.h"
#include "debug.h"

#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static uint32_t next_conv_id = 0;

static uint32_t gen_conv_id(void *tunnel)
{
	uint32_t conv;

	do {
		if (next_conv_id == 0)
			next_conv_id = (uint32_t)time(NULL) ^ (uint32_t)getpid();
		next_conv_id = next_conv_id * 1103515245 + 12345;
		conv = next_conv_id;
	} while (conv == 0 || xkcp_find_task((IUINT32)conv, NULL, tunnel) != NULL);

	return conv;
}

#include <arpa/inet.h>
#ifndef SO_ORIGINAL_DST
#define SO_ORIGINAL_DST 80
#endif

static void
tcp_proxy_read_cb(struct bufferevent *bev, void *ctx)
{
	struct xkcp_task *task = ctx;
	if (!task || !task->kcp)
		return;
	xkcp_tcp_read_cb(bev, task->kcp);
	task->last_keepalive = task->last_active = iclock();
	xkcp_forward_data(task);
}

static void
tcp_proxy_event_cb(struct bufferevent *bev, short what, void *ctx)
{
	xkcp_tcp_event_cb(bev, what, ctx);
}

static void
tcp_proxy_socks5_read_cb(struct bufferevent *bev, void *ctx)
{
	struct xkcp_task *task = ctx;
	struct xkcp_tunnel *tunnel = task ? task->tunnel : NULL;
	struct evbuffer *input = bufferevent_get_input(bev);
	size_t len = evbuffer_get_length(input);
	unsigned char buf[512];

	if (!task || !tunnel || !task->kcp)
		return;

	if (task->socks5_state == XKCP_S5_GREETING) {
		if (len < 2)
			return;
		evbuffer_copyout(input, buf, 2);
		uint8_t ver = buf[0];
		uint8_t nmethods = buf[1];
		if (ver != 5) {
			debug(LOG_WARNING, "[%s] invalid SOCKS5 version [%u], closing", tunnel->name, ver);
			bufferevent_free(bev);
			return;
		}
		if (len < (size_t)(2 + nmethods))
			return;
		evbuffer_drain(input, 2 + nmethods);

		/* Reply: VER 5, METHOD 0 (No authentication required) */
		uint8_t reply[2] = {0x05, 0x00};
		bufferevent_write(bev, reply, 2);
		task->socks5_state = XKCP_S5_REQUEST;

		len = evbuffer_get_length(input);
		if (len == 0)
			return;
	}

	if (task->socks5_state == XKCP_S5_REQUEST) {
		if (len < 4)
			return;
		evbuffer_copyout(input, buf, 4);
		uint8_t ver = buf[0];
		uint8_t cmd = buf[1];
		uint8_t atyp = buf[3];

		if (ver != 5 || cmd != 1 /* CONNECT */) {
			debug(LOG_WARNING, "[%s] unsupported SOCKS5 cmd [%u]", tunnel->name, cmd);
			uint8_t err_rep[10] = {0x05, 0x07, 0x00, 0x01, 0,0,0,0, 0,0};
			bufferevent_write(bev, err_rep, sizeof(err_rep));
			bufferevent_free(bev);
			return;
		}

		char thost[256] = {0};
		uint16_t tport = 0;
		size_t req_len = 0;

		if (atyp == 1) { /* IPv4 */
			req_len = 4 + 4 + 2;
			if (len < req_len)
				return;
			evbuffer_copyout(input, buf, req_len);
			struct in_addr in;
			memcpy(&in.s_addr, buf + 4, 4);
			inet_ntop(AF_INET, &in, thost, sizeof(thost));
			tport = ntohs(*(uint16_t *)(buf + 8));
		} else if (atyp == 3) { /* Domain name */
			if (len < 5)
				return;
			evbuffer_copyout(input, buf, 5);
			uint8_t dlen = buf[4];
			req_len = 4 + 1 + dlen + 2;
			if (len < req_len)
				return;
			evbuffer_copyout(input, buf, req_len);
			if (dlen >= sizeof(thost))
				dlen = sizeof(thost) - 1;
			memcpy(thost, buf + 5, dlen);
			thost[dlen] = '\0';
			tport = ntohs(*(uint16_t *)(buf + 5 + dlen));
		} else if (atyp == 4) { /* IPv6 */
			req_len = 4 + 16 + 2;
			if (len < req_len)
				return;
			evbuffer_copyout(input, buf, req_len);
			struct in6_addr in6;
			memcpy(&in6, buf + 4, 16);
			inet_ntop(AF_INET6, &in6, thost, sizeof(thost));
			tport = ntohs(*(uint16_t *)(buf + 20));
		} else {
			debug(LOG_WARNING, "[%s] unsupported SOCKS5 atyp [%u]", tunnel->name, atyp);
			uint8_t err_rep[10] = {0x05, 0x08, 0x00, 0x01, 0,0,0,0, 0,0};
			bufferevent_write(bev, err_rep, sizeof(err_rep));
			bufferevent_free(bev);
			return;
		}

		evbuffer_drain(input, req_len);

		/* Reply: Success (0x00), BND.ADDR 0.0.0.0:0 */
		uint8_t succ_rep[10] = {0x05, 0x00, 0x00, 0x01, 0,0,0,0, 0,0};
		bufferevent_write(bev, succ_rep, sizeof(succ_rep));

		/* Send authenticated dynamic target header to KCP */
		char hdr_buf[XKCP_MAX_HDR_LEN];
		int hlen = xkcp_auth_encode_header(hdr_buf, sizeof(hdr_buf), thost, tport,
						   tunnel->param.key, task->conv, (uint32_t)time(NULL));
		if (hlen > 0) {
			ikcp_send(task->kcp, hdr_buf, hlen);
			debug(LOG_INFO, "[%s] conv [%u] SOCKS5 connected to target [%s]:[%u]",
			      tunnel->name, task->conv, thost, tport);
		} else {
			debug(LOG_ERR, "[%s] conv [%u] encode dynamic target [%s]:[%u] failed",
			      tunnel->name, task->conv, thost, tport);
		}
		snprintf(task->target_host, sizeof(task->target_host), "%s", thost);
		task->target_port = tport;

		/* Switch to standard stream relay */
		task->socks5_state = XKCP_S5_CONNECTED;
		bufferevent_setcb(bev, tcp_proxy_read_cb, NULL, tcp_proxy_event_cb, task);

		/* Forward any early data */
		if (evbuffer_get_length(input) > 0) {
			tcp_proxy_read_cb(bev, task);
		}
	}
}

#ifdef __linux__
#include <linux/bpf.h>
#include <sys/syscall.h>

struct xkcp_bpf_tcp_session_key {
	uint32_t client_ip;
	uint16_t client_port;
	uint16_t pad;
};

struct xkcp_bpf_tcp_session_val {
	uint32_t orig_dst_ip;
	uint16_t orig_dst_port;
	uint16_t pad;
	uint64_t timestamp;
};

static int xkcp_lookup_xdns_tcp_session(struct in_addr client_ip, uint16_t client_port, struct sockaddr_in *orig_dst)
{
	static int bpf_map_fd = -1;
	if (bpf_map_fd < 0) {
		union bpf_attr attr;
		memset(&attr, 0, sizeof(attr));
		attr.pathname = (uint64_t)(uintptr_t)"/sys/fs/bpf/xdns_tcp_sessions";
		bpf_map_fd = syscall(__NR_bpf, BPF_OBJ_GET, &attr, sizeof(attr));
		if (bpf_map_fd < 0) {
			attr.pathname = (uint64_t)(uintptr_t)"/sys/fs/bpf/tc/globals/xdns_tcp_sessions";
			bpf_map_fd = syscall(__NR_bpf, BPF_OBJ_GET, &attr, sizeof(attr));
		}
	}
	if (bpf_map_fd < 0)
		return -1;

	struct xkcp_bpf_tcp_session_key key;
	memset(&key, 0, sizeof(key));
	key.client_ip = client_ip.s_addr;
	key.client_port = client_port;

	struct xkcp_bpf_tcp_session_val val;
	union bpf_attr lookup_attr;
	memset(&lookup_attr, 0, sizeof(lookup_attr));
	lookup_attr.map_fd = bpf_map_fd;
	lookup_attr.key = (uint64_t)(uintptr_t)&key;
	lookup_attr.value = (uint64_t)(uintptr_t)&val;

	if (syscall(__NR_bpf, BPF_MAP_LOOKUP_ELEM, &lookup_attr, sizeof(lookup_attr)) == 0) {
		orig_dst->sin_family = AF_INET;
		orig_dst->sin_addr.s_addr = val.orig_dst_ip;
		orig_dst->sin_port = val.orig_dst_port;
		return 0;
	}
	return -1;
}
#endif

void
tcp_proxy_accept_cb(struct evconnlistener *listener, evutil_socket_t fd,
    struct sockaddr *a, int slen, void *param)
{
	struct xkcp_proxy_param *p = param;
	struct xkcp_tunnel *tunnel = p ? p->tunnel : NULL;
	struct bufferevent *b_in = NULL;
	struct event_base *base = evconnlistener_get_base(listener);

	if (!tunnel) {
		/* no tunnel context: cannot route this connection anywhere */
		debug(LOG_ERR, "accept without tunnel context, closing fd [%d]", fd);
		evutil_closesocket(fd);
		return;
	}

	xkcp_set_tcp_nodelay(fd);
	b_in = bufferevent_socket_new(base, fd,
	    BEV_OPT_CLOSE_ON_FREE|BEV_OPT_DEFER_CALLBACKS);
	if (!b_in) {
		debug(LOG_ERR, "[%s] bufferevent_socket_new failed, closing fd [%d]",
		      tunnel->name, fd);
		evutil_closesocket(fd);
		return;
	}

	IUINT32 conv = gen_conv_id(tunnel);
	ikcpcb *kcp_client = ikcp_create(conv, p);
	if (!kcp_client) {
		debug(LOG_ERR, "[%s] ikcp_create failed, closing fd [%d]", tunnel->name, fd);
		bufferevent_free(b_in);
		return;
	}
	xkcp_set_tunnel_config_param(kcp_client, &tunnel->param);

	debug(LOG_INFO, "[%s] accept new client [%d] in, conv [%u]",
	      tunnel->name, fd, conv);

	struct xkcp_task *task = malloc(sizeof(struct xkcp_task));
	if (!task) {
		debug(LOG_ERR, "[%s] alloc task failed, closing fd [%d]", tunnel->name, fd);
		ikcp_release(kcp_client);
		bufferevent_free(b_in);
		return;
	}
	task->kcp = kcp_client;
	task->bev = b_in;
	task->sockaddr = &p->sockaddr;
	task->last_active = iclock();
	task->last_keepalive = task->last_active;
	task->user_owned = 0;
	task->conv = conv;
	task->tunnel = tunnel;
	task->handshake_done = 1;
	task->target_host[0] = '\0';
	task->target_port = 0;
	task->socks5_state = XKCP_S5_NONE;

	int is_socks5 = (tunnel->param.proxy_type &&
			 !strcasecmp(tunnel->param.proxy_type, "socks5"));
	int is_redir = (tunnel->param.proxy_type &&
			(!strcasecmp(tunnel->param.proxy_type, "redir") ||
			 !strcasecmp(tunnel->param.proxy_type, "transparent")));

	if (is_socks5) {
		task->socks5_state = XKCP_S5_GREETING;
		add_task_tail(task, &tunnel->client_task_list);
		bufferevent_setcb(b_in, tcp_proxy_socks5_read_cb, NULL, tcp_proxy_event_cb, task);
		bufferevent_enable(b_in, EV_READ | EV_WRITE);
		return;
	}

	char thost[256] = "127.0.0.1";
	uint16_t tport = (uint16_t)tunnel->param.target_port;
	int need_header = (tunnel->param.dynamic_target || tunnel->param.target_port > 0 || is_redir);

	if (is_redir) {
		struct sockaddr_in orig_dst;
		socklen_t dlen = sizeof(orig_dst);
		int found = 0;
		if (getsockopt(fd, SOL_IP, SO_ORIGINAL_DST, &orig_dst, &dlen) == 0) {
			inet_ntop(AF_INET, &orig_dst.sin_addr, thost, sizeof(thost));
			tport = ntohs(orig_dst.sin_port);
			found = 1;
			debug(LOG_INFO, "[%s] conv [%u] SO_ORIGINAL_DST target [%s]:[%u]",
			      tunnel->name, conv, thost, tport);
		}
#ifdef __linux__
		if (!found) {
			struct sockaddr_in client_peer;
			socklen_t plen = sizeof(client_peer);
			if (getpeername(fd, (struct sockaddr *)&client_peer, &plen) == 0) {
				if (xkcp_lookup_xdns_tcp_session(client_peer.sin_addr, client_peer.sin_port, &orig_dst) == 0) {
					inet_ntop(AF_INET, &orig_dst.sin_addr, thost, sizeof(thost));
					tport = ntohs(orig_dst.sin_port);
					found = 1;
					debug(LOG_INFO, "[%s] conv [%u] eBPF session target [%s]:[%u]",
					      tunnel->name, conv, thost, tport);
				}
			}
		}
#endif
		if (!found) {
			debug(LOG_WARNING, "[%s] conv [%u] failed to resolve transparent original destination",
			      tunnel->name, conv);
			if (tunnel->param.target_addr)
				snprintf(thost, sizeof(thost), "%s", tunnel->param.target_addr);
		}
	} else if (tunnel->param.target_addr) {
		snprintf(thost, sizeof(thost), "%s", tunnel->param.target_addr);
	}

	if (need_header) {
		char hdr_buf[XKCP_MAX_HDR_LEN];
		int hlen = xkcp_auth_encode_header(hdr_buf, sizeof(hdr_buf), thost, tport,
						   tunnel->param.key, conv, (uint32_t)time(NULL));
		if (hlen > 0) {
			ikcp_send(kcp_client, hdr_buf, hlen);
			debug(LOG_INFO, "[%s] conv [%u] sent authenticated dynamic target header [%s]:[%u]",
			      tunnel->name, conv, thost, tport);
		} else {
			debug(LOG_ERR, "[%s] conv [%u] encode dynamic target header [%s]:[%u] failed",
			      tunnel->name, conv, thost, tport);
		}
	}

	snprintf(task->target_host, sizeof(task->target_host), "%s", thost);
	task->target_port = tport;

	add_task_tail(task, &tunnel->client_task_list);

	bufferevent_setcb(b_in, tcp_proxy_read_cb, NULL, tcp_proxy_event_cb, task);
	bufferevent_enable(b_in,  EV_READ | EV_WRITE );
}
