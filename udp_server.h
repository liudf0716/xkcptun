#ifndef _UDP_SERVER_H_
#define _UDP_SERVER_H_

#include "xkcp_util.h"

int init_server_udp(struct xkcp_tunnel *tunnel);
void udp_server_handle_packet(struct xkcp_tunnel *tunnel, const int xkcpfd,
			      struct event_base *base, const char *buf, int nrecv,
			      const struct sockaddr_in *from, int from_len);
void udp_server_check_timeout(struct xkcp_tunnel *tunnel);
void udp_server_cleanup(struct xkcp_tunnel *tunnel);

#endif /* _UDP_SERVER_H_ */
