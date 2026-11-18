#ifndef _UDP_PROXY_H_
#define _UDP_PROXY_H_

#include <netinet/in.h>
#include "xkcp_util.h"

struct udp_client_session {
	iqueue_head node;
	uint32_t session_id;
	struct sockaddr_in client_sa;
	IUINT32 last_active;
	uint64_t create_time;	/* epoch seconds */
	uint64_t bytes_in;
	uint64_t bytes_out;
	char target_host[64];
	uint16_t target_port;
};

int init_udp_proxy(struct xkcp_tunnel *tunnel);
void udp_proxy_handle_server_packet(struct xkcp_tunnel *tunnel, const char *buf, int nrecv);
void udp_proxy_check_timeout(struct xkcp_tunnel *tunnel);
void udp_proxy_cleanup(struct xkcp_tunnel *tunnel);

#endif /* _UDP_PROXY_H_ */
