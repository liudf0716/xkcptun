#ifndef	_XKCP_UTIL_
#define	_XKCP_UTIL_

#include "ikcp.h"

#define HTTP_IP_ADDR_LEN	16
#define	OBUF_SIZE 			4096
#define	BUF_RECV_LEN		1500

struct event;
struct eventbase;
struct sockaddr_in;
struct bufferevent;

struct xkcp_proxy_param {
	struct event_base 	*base;
	int 				udp_fd;
	struct sockaddr_in	serveraddr;
	int 				addr_len;
};

struct xkcp_task {
	iqueue_head			head;
	ikcpcb				*kcp;
	struct bufferevent 	*b_in;
	struct sockaddr_in	*svr_addr;
};

typedef struct xkcp_task xkcp_task_type;

void itimeofday(long *sec, long *usec);

IINT64 iclock64(void);

IUINT32 iclock();

int get_task_list_count();

char *get_iface_ip(const char *ifname);

void add_task_tail(struct xkcp_task *task, iqueue_head *head);

void del_task(struct xkcp_task *task);

void xkcp_tcp_event_cb(struct bufferevent *bev, short what, struct xkcp_task *task);

void xkcp_tcp_read_cb(struct bufferevent *bev, ikcpcb *kcp);

void xkcp_check_task_status(iqueue_head *task_list);

void xkcp_forward_all_data(iqueue_head *task_list);

void xkcp_forward_data(struct xkcp_task *task);

void set_timer_interval(struct event *timeout);

void xkcp_timer_event_cb(struct event *timeout, iqueue_head *task_list);

ikcpcb *get_kcp_from_conv(int conv, iqueue_head *task_list);

int xkcp_main(int argc, char **argv);

#endif
