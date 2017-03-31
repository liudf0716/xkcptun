#ifndef	_XKCP_UTIL_
#define	_XKCP_UTIL_

#include "ikcp.h"

#define HTTP_IP_ADDR_LEN	16

void itimeofday(long *sec, long *usec);

IINT64 iclock64(void);

IUINT32 iclock();

char *get_iface_ip(const char *ifname);

void add_task_tail(struct xkcp_task *task, iqueue_head *head);

void del_task(struct xkcp_task *task);

ikcpcb *get_kcp_from_conv(int conv, iqueue_head *task_list)

#endif
