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
#include <arpa/inet.h>
#include <syslog.h>

#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <event2/util.h>

#include "xkcp_mon.h"
#include "xkcp_util.h"
#include "xkcp_config.h"
#include "udp_proxy.h"
#include "udp_server.h"
#include "version.h"
#include "debug.h"
#include "jwHash.h"

typedef void (*spy_cmd_process)(struct bufferevent *bev, void *ctx, const char *arg);

struct user_spy_cmd {
	char *command;
	spy_cmd_process cmd_process;
};

static void get_client_list(struct bufferevent *bev, void *ctx, const char *arg);
static void get_client_status(struct bufferevent *bev, void *ctx, const char *arg);
static void get_client_json(struct bufferevent *bev, void *ctx, const char *arg);

static void get_server_list(struct bufferevent *bev, void *ctx, const char *arg);
static void get_server_status(struct bufferevent *bev, void *ctx, const char *arg);
static void get_server_json(struct bufferevent *bev, void *ctx, const char *arg);

static void xkcp_mon_read_cb(struct bufferevent *bev, void *ctx);
static void xkcp_mon_write_cb(struct bufferevent *bev, void *ctx);
static void xkcp_mon_event_cb(struct bufferevent *bev, short what, void *ctx);

struct user_spy_cmd client_cmd[] = {
	{"list", get_client_list},
	{"status", get_client_status},
	{"json", get_client_json},
	{"stats", get_client_json},
	{NULL, NULL}
};

struct user_spy_cmd server_cmd[] = {
	{"list", get_server_list},
	{"status", get_server_status},
	{"client", get_server_list},
	{"json", get_server_json},
	{"stats", get_server_json},
	{NULL, NULL}
};

static void evbuffer_add_json_str(struct evbuffer *out, const char *s)
{
	evbuffer_add(out, "\"", 1);
	if (s) {
		for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
			switch (*p) {
			case '"':  evbuffer_add(out, "\\\"", 2); break;
			case '\\': evbuffer_add(out, "\\\\", 2); break;
			case '\b': evbuffer_add(out, "\\b", 2); break;
			case '\f': evbuffer_add(out, "\\f", 2); break;
			case '\n': evbuffer_add(out, "\\n", 2); break;
			case '\r': evbuffer_add(out, "\\r", 2); break;
			case '\t': evbuffer_add(out, "\\t", 2); break;
			default:
				if (*p < 0x20)
					evbuffer_add_printf(out, "\\u%04x", *p);
				else
					evbuffer_add(out, p, 1);
				break;
			}
		}
	}
	evbuffer_add(out, "\"", 1);
}

static void get_client_list(struct bufferevent *bev, void *ctx, const char *arg)
{
	struct xkcp_manager *mgr = ctx;
	struct evbuffer *output = bufferevent_get_output(bev);
	(void)arg;

	if (!mgr) return;

	evbuffer_add_printf(output, "xkcptun client tunnels (%d total):\n", mgr->num_tunnels);
	struct xkcp_tunnel *t;
	iqueue_foreach(t, &mgr->tunnel_list, struct xkcp_tunnel, node) {
		xkcp_calc_tunnel_rates(t);
		int is_udp = (t->param.proto && strcmp(t->param.proto, "udp") == 0);
		int conns = is_udp ? get_queue_size(&t->udp_client_sessions)
				   : get_task_list_size(&t->client_task_list);
		const char *proto_tag = is_udp ? " (UDP)" : "";
		char in_str[32] = {0}, out_str[32] = {0};
		xkcp_human_bytes(t->bytes_in, in_str, sizeof(in_str));
		xkcp_human_bytes(t->bytes_out, out_str, sizeof(out_str));

		if (t->param.dynamic_target && t->param.target_port > 0) {
			evbuffer_add_printf(output, "  - [%s]%s :%d -> %s:%d -> Target %s:%d (mode: %s, fec: %d, conns: %d, total: %u, in: %s, out: %s)\n",
					    t->name, proto_tag, t->param.local_port, t->param.remote_addr,
					    t->param.remote_port, t->param.target_addr ? t->param.target_addr : "127.0.0.1",
					    t->param.target_port, t->param.mode ? t->param.mode : "fast3",
					    t->param.fec, conns, t->total_conns, in_str, out_str);
		} else {
			evbuffer_add_printf(output, "  - [%s]%s :%d -> %s:%d (mode: %s, fec: %d, conns: %d, total: %u, in: %s, out: %s)\n",
					    t->name, proto_tag, t->param.local_port, t->param.remote_addr,
					    t->param.remote_port, t->param.mode ? t->param.mode : "fast3",
					    t->param.fec, conns, t->total_conns, in_str, out_str);
		}
	}
}

static void get_client_status(struct bufferevent *bev, void *ctx, const char *arg)
{
	struct xkcp_manager *mgr = ctx;
	struct evbuffer *output = bufferevent_get_output(bev);

	if (!mgr) return;

	struct xkcp_tunnel *t;
	iqueue_foreach(t, &mgr->tunnel_list, struct xkcp_tunnel, node) {
		if (arg && strlen(arg) > 0 && strcmp(arg, t->name) != 0)
			continue;
		xkcp_calc_tunnel_rates(t);
		int is_udp = (t->param.proto && strcmp(t->param.proto, "udp") == 0);
		char in_str[32] = {0}, out_str[32] = {0};
		xkcp_human_bytes(t->bytes_in, in_str, sizeof(in_str));
		xkcp_human_bytes(t->bytes_out, out_str, sizeof(out_str));

		evbuffer_add_printf(output, "tunnel [%s]%s (: %d -> %s:%d) [total: %u, in: %s, out: %s]:\n",
				    t->name, is_udp ? " (UDP)" : "",
				    t->param.local_port, t->param.remote_addr, t->param.remote_port,
				    t->total_conns, in_str, out_str);
		if (is_udp) {
			struct udp_client_session *s;
			int ucount = 0;
			uint64_t now_ts = (uint64_t)time(NULL);
			iqueue_foreach(s, &t->udp_client_sessions, struct udp_client_session, node) {
				ucount++;
				uint64_t uptime = (now_ts >= s->create_time && s->create_time > 0) ? (now_ts - s->create_time) : 0;
				char sin_str[32] = {0}, sout_str[32] = {0};
				char c_ip[32] = {0};
				inet_ntop(AF_INET, &s->client_sa.sin_addr, c_ip, sizeof(c_ip));
				xkcp_human_bytes(s->bytes_in, sin_str, sizeof(sin_str));
				xkcp_human_bytes(s->bytes_out, sout_str, sizeof(sout_str));
				evbuffer_add_printf(output, "\t[%d] session [%u] %s:%u -> [%s]:[%u] (in: %s, out: %s, age: %llus)\n",
						    ucount, s->session_id, c_ip, ntohs(s->client_sa.sin_port),
						    s->target_host[0] ? s->target_host : (t->param.target_addr ? t->param.target_addr : "127.0.0.1"),
						    s->target_port > 0 ? s->target_port : (uint16_t)t->param.target_port,
						    sin_str, sout_str, (unsigned long long)uptime);
			}
			if (ucount == 0)
				evbuffer_add_printf(output, "\tno active connections\n");
		} else {
			dump_task_list(&t->client_task_list, bev);
		}
	}
}

static void get_client_json(struct bufferevent *bev, void *ctx, const char *arg)
{
	(void)arg;
	struct xkcp_manager *mgr = ctx;
	struct evbuffer *output = bufferevent_get_output(bev);
	if (!mgr) return;

	uint64_t total_in = 0, total_out = 0;
	uint32_t total_conns_all = 0, active_conns_all = 0;
	uint32_t total_rx_rate = 0, total_tx_rate = 0;

	struct xkcp_tunnel *t;
	iqueue_foreach(t, &mgr->tunnel_list, struct xkcp_tunnel, node) {
		xkcp_calc_tunnel_rates(t);
		total_in += t->bytes_in;
		total_out += t->bytes_out;
		total_conns_all += t->total_conns;
		total_rx_rate += t->rx_rate_bps;
		total_tx_rate += t->tx_rate_bps;
		int is_udp = (t->param.proto && strcmp(t->param.proto, "udp") == 0);
		active_conns_all += is_udp ? get_queue_size(&t->udp_client_sessions)
					   : get_task_list_size(&t->client_task_list);
	}

	evbuffer_add_printf(output, "{\n");
	evbuffer_add_printf(output, "  \"role\": \"client\",\n");
	evbuffer_add_printf(output, "  \"version\": \"%s\",\n", VERSION);
	evbuffer_add_printf(output, "  \"num_tunnels\": %d,\n", mgr->num_tunnels);
	evbuffer_add_printf(output, "  \"total_conns\": %u,\n", total_conns_all);
	evbuffer_add_printf(output, "  \"active_conns\": %u,\n", active_conns_all);
	evbuffer_add_printf(output, "  \"bytes_in\": %llu,\n", (unsigned long long)total_in);
	evbuffer_add_printf(output, "  \"bytes_out\": %llu,\n", (unsigned long long)total_out);
	evbuffer_add_printf(output, "  \"rx_rate_bps\": %u,\n", total_rx_rate);
	evbuffer_add_printf(output, "  \"tx_rate_bps\": %u,\n", total_tx_rate);
	evbuffer_add_printf(output, "  \"tunnels\": [\n");

	int t_idx = 0;
	iqueue_foreach(t, &mgr->tunnel_list, struct xkcp_tunnel, node) {
		if (t_idx > 0) evbuffer_add_printf(output, ",\n");
		t_idx++;

		int is_udp = (t->param.proto && strcmp(t->param.proto, "udp") == 0);
		int conns = is_udp ? get_queue_size(&t->udp_client_sessions)
				   : get_task_list_size(&t->client_task_list);
		const char *ptype = is_udp ? "forward" : (t->param.proxy_type ? t->param.proxy_type : (t->param.target_addr ? "forward" : "redir"));

		evbuffer_add_printf(output, "    {\n");
		evbuffer_add_printf(output, "      \"name\": "); evbuffer_add_json_str(output, t->name); evbuffer_add(output, ",\n", 2);
		evbuffer_add_printf(output, "      \"proto\": \"%s\",\n", is_udp ? "udp" : "tcp");
		evbuffer_add_printf(output, "      \"proxy_type\": "); evbuffer_add_json_str(output, ptype); evbuffer_add(output, ",\n", 2);
		evbuffer_add_printf(output, "      \"local_port\": %d,\n", t->param.local_port);
		evbuffer_add_printf(output, "      \"remote_addr\": "); evbuffer_add_json_str(output, t->param.remote_addr ? t->param.remote_addr : ""); evbuffer_add(output, ",\n", 2);
		evbuffer_add_printf(output, "      \"remote_port\": %d,\n", t->param.remote_port);
		evbuffer_add_printf(output, "      \"target_addr\": "); evbuffer_add_json_str(output, t->param.target_addr ? t->param.target_addr : ""); evbuffer_add(output, ",\n", 2);
		evbuffer_add_printf(output, "      \"target_port\": %d,\n", t->param.target_port);
		evbuffer_add_printf(output, "      \"mode\": "); evbuffer_add_json_str(output, t->param.mode ? t->param.mode : "fast3"); evbuffer_add(output, ",\n", 2);
		evbuffer_add_printf(output, "      \"fec\": %d,\n", t->param.fec);
		evbuffer_add_printf(output, "      \"total_conns\": %u,\n", t->total_conns);
		evbuffer_add_printf(output, "      \"active_conns\": %d,\n", conns);
		evbuffer_add_printf(output, "      \"bytes_in\": %llu,\n", (unsigned long long)t->bytes_in);
		evbuffer_add_printf(output, "      \"bytes_out\": %llu,\n", (unsigned long long)t->bytes_out);
		evbuffer_add_printf(output, "      \"rx_rate_bps\": %u,\n", t->rx_rate_bps);
		evbuffer_add_printf(output, "      \"tx_rate_bps\": %u,\n", t->tx_rate_bps);
		evbuffer_add_printf(output, "      \"sessions\": [\n");

		uint64_t now_ts = (uint64_t)time(NULL);
		int s_idx = 0;
		if (is_udp) {
			struct udp_client_session *s;
			iqueue_foreach(s, &t->udp_client_sessions, struct udp_client_session, node) {
				if (s_idx > 0) evbuffer_add_printf(output, ",\n");
				s_idx++;
				uint64_t uptime = (now_ts >= s->create_time && s->create_time > 0) ? (now_ts - s->create_time) : 0;
				char c_ip[32] = {0};
				inet_ntop(AF_INET, &s->client_sa.sin_addr, c_ip, sizeof(c_ip));
				const char *th = s->target_host[0] ? s->target_host : (t->param.target_addr ? t->param.target_addr : "127.0.0.1");
				uint16_t tp = s->target_port > 0 ? s->target_port : (uint16_t)t->param.target_port;
				evbuffer_add_printf(output, "        {\n");
				evbuffer_add_printf(output, "          \"id\": %u,\n", s->session_id);
				evbuffer_add_printf(output, "          \"proto\": \"udp\",\n");
				evbuffer_add_printf(output, "          \"client_host\": "); evbuffer_add_json_str(output, c_ip); evbuffer_add(output, ",\n", 2);
				evbuffer_add_printf(output, "          \"client_port\": %u,\n", ntohs(s->client_sa.sin_port));
				evbuffer_add_printf(output, "          \"target_host\": "); evbuffer_add_json_str(output, th); evbuffer_add(output, ",\n", 2);
				evbuffer_add_printf(output, "          \"target_port\": %u,\n", tp);
				evbuffer_add_printf(output, "          \"uptime\": %llu,\n", (unsigned long long)uptime);
				evbuffer_add_printf(output, "          \"bytes_in\": %llu,\n", (unsigned long long)s->bytes_in);
				evbuffer_add_printf(output, "          \"bytes_out\": %llu,\n", (unsigned long long)s->bytes_out);
				evbuffer_add_printf(output, "          \"rx_rate_bps\": 0,\n");
				evbuffer_add_printf(output, "          \"tx_rate_bps\": 0,\n");
				evbuffer_add_printf(output, "          \"rtt_ms\": 0\n");
				evbuffer_add_printf(output, "        }");
			}
		} else {
			struct xkcp_task *task;
			iqueue_foreach(task, &t->client_task_list, xkcp_task_type, head) {
				if (s_idx > 0) evbuffer_add_printf(output, ",\n");
				s_idx++;
				xkcp_calc_task_rates(task);
				uint64_t uptime = (now_ts >= task->create_time && task->create_time > 0) ? (now_ts - task->create_time) : 0;
				uint32_t rtt = task->kcp ? (uint32_t)task->kcp->rx_srtt : 0;
				uint32_t rto = task->kcp ? (uint32_t)task->kcp->rx_rto : 0;
				const char *chost = task->client_host[0] ? task->client_host : "127.0.0.1";
				evbuffer_add_printf(output, "        {\n");
				evbuffer_add_printf(output, "          \"conv\": %u,\n", task->conv);
				evbuffer_add_printf(output, "          \"proto\": \"tcp\",\n");
				evbuffer_add_printf(output, "          \"client_host\": "); evbuffer_add_json_str(output, chost); evbuffer_add(output, ",\n", 2);
				evbuffer_add_printf(output, "          \"client_port\": %u,\n", task->client_port);
				evbuffer_add_printf(output, "          \"target_host\": "); evbuffer_add_json_str(output, task->target_host); evbuffer_add(output, ",\n", 2);
				evbuffer_add_printf(output, "          \"target_port\": %u,\n", task->target_port);
				evbuffer_add_printf(output, "          \"uptime\": %llu,\n", (unsigned long long)uptime);
				evbuffer_add_printf(output, "          \"bytes_in\": %llu,\n", (unsigned long long)task->bytes_in);
				evbuffer_add_printf(output, "          \"bytes_out\": %llu,\n", (unsigned long long)task->bytes_out);
				evbuffer_add_printf(output, "          \"rx_rate_bps\": %u,\n", task->rx_rate_bps);
				evbuffer_add_printf(output, "          \"tx_rate_bps\": %u,\n", task->tx_rate_bps);
				evbuffer_add_printf(output, "          \"rtt_ms\": %u,\n", rtt);
				evbuffer_add_printf(output, "          \"rto_ms\": %u\n", rto);
				evbuffer_add_printf(output, "        }");
			}
		}
		evbuffer_add_printf(output, "\n      ]\n");
		evbuffer_add_printf(output, "    }");
	}
	evbuffer_add_printf(output, "\n  ]\n");
	evbuffer_add_printf(output, "}\n");
}

static void get_server_list(struct bufferevent *bev, void *ctx, const char *arg)
{
	struct xkcp_manager *mgr = ctx;
	struct evbuffer *output = bufferevent_get_output(bev);
	(void)arg;

	if (!mgr) return;

	evbuffer_add_printf(output, "xkcptun server tunnels (%d total):\n", mgr->num_tunnels);
	struct xkcp_tunnel *t;
	iqueue_foreach(t, &mgr->tunnel_list, struct xkcp_tunnel, node) {
		xkcp_calc_tunnel_rates(t);
		int total_clients = 0;
		if (t->server_xkcp_hash) {
			for (int i = 0; i < t->server_xkcp_hash->buckets; i++) {
				jwHashEntry *entry = t->server_xkcp_hash->bucket[i];
				while (entry) {
					total_clients++;
					entry = entry->next;
				}
			}
		}
		char in_str[32] = {0}, out_str[32] = {0};
		xkcp_human_bytes(t->bytes_in, in_str, sizeof(in_str));
		xkcp_human_bytes(t->bytes_out, out_str, sizeof(out_str));

		evbuffer_add_printf(output, "  - [%s] UDP :%d -> TCP %s:%d (mode: %s, fec: %d, peers: %d, total: %u, in: %s, out: %s)\n",
				    t->name, t->param.local_port, t->param.remote_addr,
				    t->param.remote_port, t->param.mode ? t->param.mode : "fast3",
				    t->param.fec, total_clients, t->total_conns, in_str, out_str);
	}
}

static void get_server_status(struct bufferevent *bev, void *ctx, const char *arg)
{
	struct xkcp_manager *mgr = ctx;
	struct evbuffer *output = bufferevent_get_output(bev);

	if (!mgr) return;

	struct xkcp_tunnel *t;
	iqueue_foreach(t, &mgr->tunnel_list, struct xkcp_tunnel, node) {
		if (arg && strlen(arg) > 0 && strcmp(arg, t->name) != 0)
			continue;
		xkcp_calc_tunnel_rates(t);
		char in_str[32] = {0}, out_str[32] = {0};
		xkcp_human_bytes(t->bytes_in, in_str, sizeof(in_str));
		xkcp_human_bytes(t->bytes_out, out_str, sizeof(out_str));

		evbuffer_add_printf(output, "tunnel [%s] (UDP :%d -> TCP %s:%d) [total: %u, in: %s, out: %s]:\n",
				    t->name, t->param.local_port, t->param.remote_addr, t->param.remote_port,
				    t->total_conns, in_str, out_str);
		if (t->server_xkcp_hash) {
			for (int i = 0; i < t->server_xkcp_hash->buckets; i++) {
				jwHashEntry *entry = t->server_xkcp_hash->bucket[i];
				while (entry) {
					evbuffer_add_printf(output, "  peer [%s]:\n", entry->key.strValue);
					dump_task_list((iqueue_head *)entry->value.ptrValue, bev);
					entry = entry->next;
				}
			}
		}
	}
}

static void get_server_json(struct bufferevent *bev, void *ctx, const char *arg)
{
	(void)arg;
	struct xkcp_manager *mgr = ctx;
	struct evbuffer *output = bufferevent_get_output(bev);
	if (!mgr) return;

	uint64_t total_in = 0, total_out = 0;
	uint32_t total_conns_all = 0, active_conns_all = 0;
	uint32_t total_rx_rate = 0, total_tx_rate = 0;

	struct xkcp_tunnel *t;
	iqueue_foreach(t, &mgr->tunnel_list, struct xkcp_tunnel, node) {
		xkcp_calc_tunnel_rates(t);
		total_in += t->bytes_in;
		total_out += t->bytes_out;
		total_conns_all += t->total_conns;
		total_rx_rate += t->rx_rate_bps;
		total_tx_rate += t->tx_rate_bps;
		if (t->server_xkcp_hash) {
			for (int i = 0; i < t->server_xkcp_hash->buckets; i++) {
				jwHashEntry *entry = t->server_xkcp_hash->bucket[i];
				while (entry) {
					active_conns_all += get_task_list_size((iqueue_head *)entry->value.ptrValue);
					entry = entry->next;
				}
			}
		}
		active_conns_all += get_queue_size(&t->udp_server_sessions);
	}

	evbuffer_add_printf(output, "{\n");
	evbuffer_add_printf(output, "  \"role\": \"server\",\n");
	evbuffer_add_printf(output, "  \"version\": \"%s\",\n", VERSION);
	evbuffer_add_printf(output, "  \"num_tunnels\": %d,\n", mgr->num_tunnels);
	evbuffer_add_printf(output, "  \"total_conns\": %u,\n", total_conns_all);
	evbuffer_add_printf(output, "  \"active_conns\": %u,\n", active_conns_all);
	evbuffer_add_printf(output, "  \"bytes_in\": %llu,\n", (unsigned long long)total_in);
	evbuffer_add_printf(output, "  \"bytes_out\": %llu,\n", (unsigned long long)total_out);
	evbuffer_add_printf(output, "  \"rx_rate_bps\": %u,\n", total_rx_rate);
	evbuffer_add_printf(output, "  \"tx_rate_bps\": %u,\n", total_tx_rate);
	evbuffer_add_printf(output, "  \"tunnels\": [\n");

	int t_idx = 0;
	iqueue_foreach(t, &mgr->tunnel_list, struct xkcp_tunnel, node) {
		if (t_idx > 0) evbuffer_add_printf(output, ",\n");
		t_idx++;

		int active_conns = 0;
		if (t->server_xkcp_hash) {
			for (int i = 0; i < t->server_xkcp_hash->buckets; i++) {
				jwHashEntry *entry = t->server_xkcp_hash->bucket[i];
				while (entry) {
					active_conns += get_task_list_size((iqueue_head *)entry->value.ptrValue);
					entry = entry->next;
				}
			}
		}
		active_conns += get_queue_size(&t->udp_server_sessions);

		evbuffer_add_printf(output, "    {\n");
		evbuffer_add_printf(output, "      \"name\": "); evbuffer_add_json_str(output, t->name); evbuffer_add(output, ",\n", 2);
		evbuffer_add_printf(output, "      \"local_port\": %d,\n", t->param.local_port);
		evbuffer_add_printf(output, "      \"remote_addr\": "); evbuffer_add_json_str(output, t->param.remote_addr ? t->param.remote_addr : ""); evbuffer_add(output, ",\n", 2);
		evbuffer_add_printf(output, "      \"remote_port\": %d,\n", t->param.remote_port);
		evbuffer_add_printf(output, "      \"mode\": "); evbuffer_add_json_str(output, t->param.mode ? t->param.mode : "fast3"); evbuffer_add(output, ",\n", 2);
		evbuffer_add_printf(output, "      \"fec\": %d,\n", t->param.fec);
		evbuffer_add_printf(output, "      \"total_conns\": %u,\n", t->total_conns);
		evbuffer_add_printf(output, "      \"active_conns\": %d,\n", active_conns);
		evbuffer_add_printf(output, "      \"bytes_in\": %llu,\n", (unsigned long long)t->bytes_in);
		evbuffer_add_printf(output, "      \"bytes_out\": %llu,\n", (unsigned long long)t->bytes_out);
		evbuffer_add_printf(output, "      \"rx_rate_bps\": %u,\n", t->rx_rate_bps);
		evbuffer_add_printf(output, "      \"tx_rate_bps\": %u,\n", t->tx_rate_bps);
		evbuffer_add_printf(output, "      \"sessions\": [\n");

		uint64_t now_ts = (uint64_t)time(NULL);
		int s_idx = 0;
		if (t->server_xkcp_hash) {
			for (int i = 0; i < t->server_xkcp_hash->buckets; i++) {
				jwHashEntry *entry = t->server_xkcp_hash->bucket[i];
				while (entry) {
					iqueue_head *task_list = (iqueue_head *)entry->value.ptrValue;
					struct xkcp_task *task;
					iqueue_foreach(task, task_list, xkcp_task_type, head) {
						if (s_idx > 0) evbuffer_add_printf(output, ",\n");
						s_idx++;
						xkcp_calc_task_rates(task);
						uint64_t uptime = (now_ts >= task->create_time && task->create_time > 0) ? (now_ts - task->create_time) : 0;
						uint32_t rtt = task->kcp ? (uint32_t)task->kcp->rx_srtt : 0;
						uint32_t rto = task->kcp ? (uint32_t)task->kcp->rx_rto : 0;
						const char *chost = task->client_host[0] ? task->client_host : entry->key.strValue;
						evbuffer_add_printf(output, "        {\n");
						evbuffer_add_printf(output, "          \"conv\": %u,\n", task->conv);
						evbuffer_add_printf(output, "          \"proto\": \"tcp\",\n");
						evbuffer_add_printf(output, "          \"client_host\": "); evbuffer_add_json_str(output, chost); evbuffer_add(output, ",\n", 2);
						evbuffer_add_printf(output, "          \"client_port\": %u,\n", task->client_port);
						evbuffer_add_printf(output, "          \"target_host\": "); evbuffer_add_json_str(output, task->target_host); evbuffer_add(output, ",\n", 2);
						evbuffer_add_printf(output, "          \"target_port\": %u,\n", task->target_port);
						evbuffer_add_printf(output, "          \"uptime\": %llu,\n", (unsigned long long)uptime);
						evbuffer_add_printf(output, "          \"bytes_in\": %llu,\n", (unsigned long long)task->bytes_in);
						evbuffer_add_printf(output, "          \"bytes_out\": %llu,\n", (unsigned long long)task->bytes_out);
						evbuffer_add_printf(output, "          \"rx_rate_bps\": %u,\n", task->rx_rate_bps);
						evbuffer_add_printf(output, "          \"tx_rate_bps\": %u,\n", task->tx_rate_bps);
						evbuffer_add_printf(output, "          \"rtt_ms\": %u,\n", rtt);
						evbuffer_add_printf(output, "          \"rto_ms\": %u\n", rto);
						evbuffer_add_printf(output, "        }");
					}
					entry = entry->next;
				}
			}
		}
		{
			struct udp_server_session *us;
			iqueue_foreach(us, &t->udp_server_sessions, struct udp_server_session, node) {
				if (s_idx > 0) evbuffer_add_printf(output, ",\n");
				s_idx++;
				uint64_t uptime = (now_ts >= us->create_time && us->create_time > 0) ? (now_ts - us->create_time) : 0;
				char c_ip[32] = {0};
				inet_ntop(AF_INET, &us->client_sa.sin_addr, c_ip, sizeof(c_ip));
				evbuffer_add_printf(output, "        {\n");
				evbuffer_add_printf(output, "          \"id\": %u,\n", us->session_id);
				evbuffer_add_printf(output, "          \"proto\": \"udp\",\n");
				evbuffer_add_printf(output, "          \"client_host\": "); evbuffer_add_json_str(output, c_ip); evbuffer_add(output, ",\n", 2);
				evbuffer_add_printf(output, "          \"client_port\": %u,\n", ntohs(us->client_sa.sin_port));
				evbuffer_add_printf(output, "          \"target_host\": "); evbuffer_add_json_str(output, us->target_host); evbuffer_add(output, ",\n", 2);
				evbuffer_add_printf(output, "          \"target_port\": %u,\n", us->target_port);
				evbuffer_add_printf(output, "          \"uptime\": %llu,\n", (unsigned long long)uptime);
				evbuffer_add_printf(output, "          \"bytes_in\": %llu,\n", (unsigned long long)us->bytes_in);
				evbuffer_add_printf(output, "          \"bytes_out\": %llu,\n", (unsigned long long)us->bytes_out);
				evbuffer_add_printf(output, "          \"rx_rate_bps\": 0,\n");
				evbuffer_add_printf(output, "          \"tx_rate_bps\": 0,\n");
				evbuffer_add_printf(output, "          \"rtt_ms\": 0\n");
				evbuffer_add_printf(output, "        }");
			}
		}
		evbuffer_add_printf(output, "\n      ]\n");
		evbuffer_add_printf(output, "    }");
	}
	evbuffer_add_printf(output, "\n  ]\n");
	evbuffer_add_printf(output, "}\n");
}

static void process_user_cmd(struct bufferevent *bev, const char *cmd_line, void *ctx)
{
	struct xkcp_manager *mgr = ctx;
	char cmd[64] = {0};
	char arg[128] = {0};
	int is_http = 0;

	if (strncasecmp(cmd_line, "GET ", 4) == 0) {
		is_http = 1;
		char path[128] = {0};
		sscanf(cmd_line + 4, "%127s", path);
		if (strstr(path, "json") || strstr(path, "stats")) {
			snprintf(cmd, sizeof(cmd), "%s", "json");
		} else if (strstr(path, "status")) {
			snprintf(cmd, sizeof(cmd), "%s", "status");
		} else {
			snprintf(cmd, sizeof(cmd), "%s", "list");
		}
	} else {
		sscanf(cmd_line, "%63s %127s", cmd, arg);
	}

	struct user_spy_cmd *table = (mgr && mgr->is_server) ? server_cmd : client_cmd;

	for (int i = 0; table[i].command != NULL; i++) {
		if (strcmp(cmd, table[i].command) == 0) {
			if (is_http) {
				int is_json = (strcmp(cmd, "json") == 0 || strcmp(cmd, "stats") == 0);
				evbuffer_add_printf(bufferevent_get_output(bev),
					"HTTP/1.1 200 OK\r\nContent-Type: %s; charset=utf-8\r\nConnection: close\r\n\r\n",
					is_json ? "application/json" : "text/plain");
			}
			table[i].cmd_process(bev, ctx, arg);
			bufferevent_disable(bev, EV_READ);
			bufferevent_setcb(bev, NULL, xkcp_mon_write_cb, xkcp_mon_event_cb, ctx);
			bufferevent_enable(bev, EV_WRITE);
			return;
		}
	}

	/* Default fallback for bare "status" or empty command */
	if (table[0].cmd_process) {
		if (is_http) {
			evbuffer_add_printf(bufferevent_get_output(bev),
				"HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nConnection: close\r\n\r\n");
		}
		table[0].cmd_process(bev, ctx, arg);
		bufferevent_disable(bev, EV_READ);
		bufferevent_setcb(bev, NULL, xkcp_mon_write_cb, xkcp_mon_event_cb, ctx);
		bufferevent_enable(bev, EV_WRITE);
	}
}

static void xkcp_mon_event_cb(struct bufferevent *bev, short what, void *ctx)
{
	(void)ctx;
	if (what & (BEV_EVENT_EOF|BEV_EVENT_ERROR)) {
		bufferevent_free(bev);
	}
}

static void xkcp_mon_write_cb(struct bufferevent *bev, void *ctx)
{
	(void)ctx;
	struct evbuffer *output = bufferevent_get_output(bev);
	if (evbuffer_get_length(output) == 0)
		bufferevent_free(bev);
}

static void xkcp_mon_read_cb(struct bufferevent *bev, void *ctx)
{
	struct evbuffer *input = bufferevent_get_input(bev);
	int len = evbuffer_get_length(input);

	if (len > 0) { 
		char *buf = malloc(len + 1);
		if (buf) {
			memset(buf, 0, len + 1);
			if (evbuffer_remove(input, buf, len) > 0) {
				/* Strip trailing newlines / spaces */
				while (len > 0 && (buf[len - 1] == '\r' || buf[len - 1] == '\n' || buf[len - 1] == ' ')) {
					buf[--len] = '\0';
				}
				process_user_cmd(bev, buf, ctx);
			}
			free(buf);
		}
	}
}

void xkcp_mon_accept_cb(struct evconnlistener *listener, evutil_socket_t fd,
    struct sockaddr *a, int slen, void *ptr)
{
	(void)a; (void)slen;
	struct bufferevent *b_in = NULL;
	struct event_base *base = evconnlistener_get_base(listener);
	
	b_in = bufferevent_socket_new(base, fd,
	    BEV_OPT_CLOSE_ON_FREE|BEV_OPT_DEFER_CALLBACKS);
	assert(b_in);
	
	bufferevent_setcb(b_in, xkcp_mon_read_cb, NULL, xkcp_mon_event_cb, ptr);
	bufferevent_enable(b_in,  EV_READ);
}

struct evconnlistener *set_xkcp_mon_listener(struct event_base *base, short port, void *ptr)
{
	struct sockaddr_in sin;
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	/* management interface: loopback only, it exposes tunnel details */
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sin.sin_port = htons(port);

	struct evconnlistener *listener = evconnlistener_new_bind(
		base, xkcp_mon_accept_cb, ptr,
		LEV_OPT_CLOSE_ON_FREE|LEV_OPT_CLOSE_ON_EXEC|LEV_OPT_REUSEABLE,
		-1, (struct sockaddr*)&sin, sizeof(sin));
	if (!listener) {
		debug(LOG_WARNING, "Couldn't create mon listener on port %d: [%s]", port, strerror(errno));
		return NULL;
	}

	debug(LOG_INFO, "Monitor listening on 127.0.0.1:%d", port);
	return listener;
}
