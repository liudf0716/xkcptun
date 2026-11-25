#ifndef _UDP_SERVER_H_
#define _UDP_SERVER_H_

#include <netinet/in.h>
#include "xkcp_util.h"

struct evdns_request;
struct evbuffer;

struct udp_server_session {
	iqueue_head node;
	uint32_t session_id;
	struct sockaddr_in client_sa;
	struct sockaddr_in target_sa;
	char target_host[256];
	uint16_t target_port;
	int target_fd;
	struct event *target_ev;
	struct xkcp_tunnel *tunnel;
	struct fec_conn *fec;
	int xkcpfd;
	IUINT32 last_active;
	uint64_t create_time;
	uint64_t bytes_in;
	uint64_t bytes_out;
	int dns_resolved;
	int dns_failed;
	struct evdns_request *dns_req;
	struct evbuffer *pending_pkts;
};

int init_server_udp(struct xkcp_tunnel *tunnel);
void udp_server_handle_packet(struct xkcp_tunnel *tunnel, const int xkcpfd,
			      struct event_base *base, const char *buf, int nrecv,
			      const struct sockaddr_in *from, int from_len);
void udp_server_check_timeout(struct xkcp_tunnel *tunnel);
void udp_server_cleanup(struct xkcp_tunnel *tunnel);

#endif /* _UDP_SERVER_H_ */
