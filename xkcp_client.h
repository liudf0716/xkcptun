#ifndef	_XKCP_CLIENT_
#define	_XKCP_CLIENT_

#include <event2/util.h>
#include "ikcp.h"

struct xkcp_task;


#define	XKCP_RECV_BUF_LEN	1500
#define	XKCP_SEND_BUF_LEN	10*15000

void xkcp_rcv_cb(const int sock, short int which, void *arg);

ikcpcb *get_kcp_from_conv(int conv);

void timer_event_cb(evutil_socket_t fd, short event, void *arg);

int xkcp_main(int argc, char **argv);

int main_loop(void);

#endif
