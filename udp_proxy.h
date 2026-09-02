#ifndef _UDP_PROXY_H_
#define _UDP_PROXY_H_

#include "xkcp_util.h"

int init_udp_proxy(struct xkcp_tunnel *tunnel);
void udp_proxy_handle_server_packet(struct xkcp_tunnel *tunnel, const char *buf, int nrecv);
void udp_proxy_check_timeout(struct xkcp_tunnel *tunnel);
void udp_proxy_cleanup(struct xkcp_tunnel *tunnel);

#endif /* _UDP_PROXY_H_ */
