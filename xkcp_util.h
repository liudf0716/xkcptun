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

#ifndef	_XKCP_UTIL_
#define	_XKCP_UTIL_

#include "ikcp.h"
#include "xkcp_config.h"
#include "jwHash.h"

#include <event2/util.h>
#include <event2/event.h>
#include <event2/event_struct.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/listener.h>

#include <sys/socket.h>
#include <netinet/in.h>

#define HTTP_IP_ADDR_LEN	16
#define	OBUF_SIZE 			65536
#define	BUF_RECV_LEN		65536
#define XKCP_RECV_BUF_LEN	65536
#define XKCP_SEND_BUF_LEN	(10 * 15000)

#define XKCP_CLOSE_SIGNAL	"\xff\xff\xff\xff"
#define XKCP_CLOSE_SIGNAL_LEN	4

/* in-band keepalive: skipped by both endpoints, never forwarded to TCP.
 * a 4-byte all-zero message from a real peer stream is dropped by design. */
#define XKCP_NOP_SIGNAL		"\x00\x00\x00\x00"
#define XKCP_NOP_SIGNAL_LEN	4

struct fec_conn;
struct xkcp_tunnel;
struct xkcp_manager;
struct evdns_base;

struct xkcp_proxy_param {
	struct event_base 	*base;
	int 				xkcpfd;
	struct sockaddr_in	sockaddr;
	int 				addr_len;
	struct fec_conn		*fec;		/* per-peer FEC codec, NULL = disabled */
	struct xkcp_tunnel	*tunnel;	/* back-pointer to owning tunnel */
};

struct xkcp_task {
	iqueue_head			head;
	ikcpcb				*kcp;
	struct bufferevent 	*bev;
	struct sockaddr_in	*sockaddr;
	IUINT32				last_active;
	IUINT32				last_keepalive;
	int					user_owned;
	IUINT32				conv;		/* cached kcp conv, valid after free */
	struct xkcp_tunnel	*tunnel;	/* back-pointer to owning tunnel */
	int					handshake_done;	/* server: 1 = destination connected */
	char				target_host[256];
	uint16_t			target_port;
	int					socks5_state;	/* 0=none, 1=greeting, 2=request, 3=connected */
};

enum {
	XKCP_S5_NONE = 0,
	XKCP_S5_GREETING,
	XKCP_S5_REQUEST,
	XKCP_S5_CONNECTED
};

typedef struct xkcp_task xkcp_task_type;

/* Unified Tunnel Context */
struct xkcp_tunnel {
	iqueue_head			node;			/* node in xkcp_manager.tunnel_list */
	char				name[64];		/* tunnel identifier name */
	struct xkcp_param	param;			/* per-tunnel parameters */
	struct event_base	*base;			/* event base */
	int					xkcp_fd;		/* UDP socket */
	struct evconnlistener *listener;	/* TCP listener (client) */
	struct event		*xkcp_event;	/* UDP read event */
	struct event		timer_event;	/* periodic tick timer event */

	/* Client specific */
	struct fec_conn		*client_fec;
	struct xkcp_proxy_param client_proxy_param;
	iqueue_head			client_task_list;
	int					udp_local_fd;		/* local UDP listening socket */
	struct event		*udp_local_ev;		/* read event on local UDP socket */
	iqueue_head			udp_client_sessions;	/* active client UDP sessions */

	/* Server specific */
	jwHashTable			*server_xkcp_hash;
	jwHashTable			*server_fec_hash;
	iqueue_head			udp_server_sessions;	/* active server UDP sessions */
	IUINT32				last_cleanup;	/* last periodic cleanup tick */
	int					(*connect_target)(struct xkcp_task *task, const char *host, uint16_t port);

	/* Egress UDP backpressure queue */
	struct evbuffer		*udp_pend;
	struct event		*udp_wev;
	int					udp_wev_active;

	/* Metrics & Statistics */
	uint64_t			bytes_in;
	uint64_t			bytes_out;
	uint32_t			total_conns;

	struct xkcp_manager	*mgr;			/* parent manager */
};

/* Unified Master Manager */
struct xkcp_manager {
	struct event_base	*base;
	iqueue_head		tunnel_list;
	int			num_tunnels;
	struct evconnlistener *mon_listener;
	struct evdns_base	*dns_base;	/* async resolver (server) */
	int			is_server;
};

void itimeofday(long *sec, long *usec);

IINT64 iclock64(void);

IUINT32 iclock(void);

static inline IINT32 _itimediff(IUINT32 later, IUINT32 earlier)
{
	return ((IINT32)(later - earlier));
}

char *get_iface_ip(const char *ifname);

void add_task_tail(struct xkcp_task *task, iqueue_head *head);

void del_task(struct xkcp_task *task);

int get_queue_size(iqueue_head *head);
int get_task_list_size(iqueue_head *task_list);

void dump_task_list(iqueue_head *task_list, struct bufferevent *bev);

void xkcp_set_tunnel_config_param(ikcpcb *kcp, struct xkcp_param *param);

void xkcp_set_tcp_nodelay(int fd);

void xkcp_apply_sockbuf_param(int fd, struct xkcp_param *param);

/* event callback on TCP EOF/error; returns the kcp user pointer when the
 * caller owns it (server sessions) so it can be freed */
void *xkcp_tcp_event_cb(struct bufferevent *bev, short what, struct xkcp_task *task);

void xkcp_tcp_read_cb(struct bufferevent *bev, ikcpcb *kcp);

/* pump KCP -> TCP for one task; returns 0 if the task was freed */
int xkcp_forward_data(struct xkcp_task *task);

/* tick all sessions in a task list: kcp_update + forward + idle keepalive */
void xkcp_update_task_list(iqueue_head *task_list, const struct xkcp_param *param);

void set_timer_interval_ms(struct event *timeout, int interval_ms);

void xkcp_task_check_timeout_val(iqueue_head *task_list, int timeout_sec);

/* O(1) conv -> task lookup backed by a scoped hash */
struct xkcp_task *xkcp_find_task(IUINT32 conv, const struct sockaddr_in *peer, void *tunnel);

void xkcp_set_event_base(struct event_base *base);

/* queue a UDP datagram that hit EAGAIN; drained on socket writability */
void xkcp_enqueue_udp_at(evutil_socket_t fd, const struct sockaddr_in *sa,
			 const char *buf, int len, struct xkcp_tunnel *tunnel);

void xkcp_send_tunnel_packet(int fd, const struct sockaddr_in *addr, struct fec_conn *fec,
			     const char *buf, int len, struct xkcp_tunnel *tunnel);

/* periodic FEC tick for one session's peer codec (parity flush + adaptation) */
void xkcp_fec_tick(struct xkcp_proxy_param *ptr);
void xkcp_fec_conn_tick(struct fec_conn *fec, int fd, const struct sockaddr_in *addr, struct xkcp_tunnel *tunnel);

int xkcp_main(int argc, char **argv);

void xkcp_setup_signals(struct event_base *base);

void xkcp_cleanup_signals(void);

void xkcp_cleanup_udp_queue(void);

#endif
