#ifndef	_XKCP_UTIL_
#define	_XKCP_UTIL_

#include "ikcp.h"

#define HTTP_IP_ADDR_LEN	16
#define	OBUF_SIZE 			4096
#define	BUF_RECV_LEN		4096

#define XKCP_CLOSE_SIGNAL	"\xff\xff\xff\xff"
#define XKCP_CLOSE_SIGNAL_LEN	4

struct event;
struct eventbase;
struct sockaddr_in;
struct bufferevent;
struct fec_conn;

struct xkcp_proxy_param {
	struct event_base 	*base;
	int 				xkcpfd;
	struct sockaddr_in	sockaddr;
	int 				addr_len;
	struct fec_conn		*fec;		/* per-peer FEC codec, NULL = disabled */
};

struct xkcp_task {
	iqueue_head			head;
	ikcpcb				*kcp;
	struct bufferevent 	*bev;
	struct sockaddr_in	*sockaddr;
	IUINT32				last_active;
	int					user_owned;
	IUINT32				conv;		/* cached kcp conv, valid after free */
};

typedef struct xkcp_task xkcp_task_type;

void itimeofday(long *sec, long *usec);

IINT64 iclock64(void);

IUINT32 iclock();

int get_task_list_count();

char *get_iface_ip(const char *ifname);

void add_task_tail(struct xkcp_task *task, iqueue_head *head);

void del_task(struct xkcp_task *task);

int get_task_list_size(iqueue_head *task_list);

void dump_task_list(iqueue_head *task_list, struct bufferevent *bev);

void xkcp_set_config_param(ikcpcb *kcp);

void xkcp_set_tcp_nodelay(int fd);

void xkcp_apply_sockbuf(int fd);

void *xkcp_tcp_event_cb(struct bufferevent *bev, short what, struct xkcp_task *task);

void xkcp_tcp_read_cb(struct bufferevent *bev, ikcpcb *kcp);

void xkcp_forward_all_data(iqueue_head *task_list);

void xkcp_forward_data(struct xkcp_task *task);

void xkcp_update_task_list(iqueue_head *task_list);

void set_timer_interval(struct event *timeout);

void xkcp_timer_event_cb(struct event *timeout, iqueue_head *task_list);

ikcpcb *get_kcp_from_conv(IUINT32 conv, iqueue_head *task_list);

struct xkcp_task *get_task_from_conv(IUINT32 conv, iqueue_head *task_list);

/* O(1) conv -> task lookup backed by a hash. peer scopes the key on the
 * server (per-client isolation); pass NULL on the client. Falls back to
 * NULL when the hash is not yet built. */
struct xkcp_task *xkcp_find_task(IUINT32 conv, const struct sockaddr_in *peer);

void xkcp_task_check_timeout(iqueue_head *task_list);

int xkcp_main(int argc, char **argv);

void xkcp_setup_signals(struct event_base *base);

struct evconnlistener *xkcp_create_listener(struct event_base *base, short port, void *ptr);

#endif
