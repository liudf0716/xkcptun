#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <syslog.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include <event2/event.h>
#include <event2/util.h>

#include "udp_server.h"
#include "xkcp_server.h"
#include "xkcp_udp.h"
#include "debug.h"

struct udp_server_session {
	iqueue_head node;
	uint32_t session_id;
	struct sockaddr_in client_sa;
	struct sockaddr_in target_sa;
	int target_fd;
	struct event *target_ev;
	struct xkcp_tunnel *tunnel;
	struct fec_conn *fec;
	int xkcpfd;
	IUINT32 last_active;
};

static struct udp_server_session *find_server_session(struct xkcp_tunnel *tunnel,
						      const struct sockaddr_in *client_sa,
						      uint32_t session_id)
{
	struct udp_server_session *s;
	iqueue_foreach(s, &tunnel->udp_server_sessions, struct udp_server_session, node) {
		if (s->session_id == session_id &&
		    s->client_sa.sin_addr.s_addr == client_sa->sin_addr.s_addr &&
		    s->client_sa.sin_port == client_sa->sin_port) {
			return s;
		}
	}
	return NULL;
}

static void udp_server_target_cb(evutil_socket_t fd, short what, void *arg)
{
	struct udp_server_session *session = arg;
	if (!session) return;

	char buf[XKCP_RECV_BUF_LEN];
	char enc_buf[XKCP_RECV_BUF_LEN + 32];
	int nrecv;

	(void)what;

	while (1) {
		nrecv = recvfrom(fd, buf, sizeof(buf), 0, NULL, NULL);
		if (nrecv <= 0)
			break;

		int enc_len = xkcp_udp_encode_resp(enc_buf, sizeof(enc_buf),
						   session->session_id, buf, nrecv);
		if (enc_len > 0) {
			xkcp_send_tunnel_packet(session->xkcpfd, &session->client_sa,
						session->fec, enc_buf, enc_len, session->tunnel);
			session->last_active = iclock();
			session->tunnel->bytes_out += nrecv;
		}
	}
}

int init_server_udp(struct xkcp_tunnel *tunnel)
{
	if (!tunnel) return -1;
	iqueue_init(&tunnel->udp_server_sessions);
	return 0;
}

void udp_server_handle_packet(struct xkcp_tunnel *tunnel, const int xkcpfd,
			      struct event_base *base, const char *buf, int nrecv,
			      const struct sockaddr_in *from, int from_len)
{
	if (!tunnel || !buf || nrecv < 8 || !from) return;

	(void)from_len;

	uint32_t session_id = 0;
	char target_host[XKCP_AUTH_ADDR_MAX] = {0};
	uint16_t target_port = 0;
	const char *payload = NULL;
	size_t payload_len = 0;

	if (xkcp_udp_decode_req(buf, nrecv, &session_id, target_host, sizeof(target_host),
				&target_port, tunnel->param.key, &payload, &payload_len) != 0) {
		debug(LOG_WARNING, "[%s] Direct UDP packet decode/auth failed from %s:%d",
		      tunnel->name, inet_ntoa(from->sin_addr), ntohs(from->sin_port));
		return;
	}

	struct udp_server_session *session = find_server_session(tunnel, from, session_id);
	if (!session) {
		int target_fd = socket(AF_INET, SOCK_DGRAM, 0);
		if (target_fd < 0) {
			debug(LOG_ERR, "[%s] create UDP target socket failed: %s",
			      tunnel->name, strerror(errno));
			return;
		}

		evutil_make_socket_nonblocking(target_fd);

		struct sockaddr_in target_sa;
		memset(&target_sa, 0, sizeof(target_sa));
		target_sa.sin_family = AF_INET;
		target_sa.sin_port = htons(target_port);

		if (inet_pton(AF_INET, target_host, &target_sa.sin_addr) != 1) {
			struct hostent *he = gethostbyname(target_host);
			if (!he || !he->h_addr) {
				debug(LOG_ERR, "[%s] resolve UDP target host [%s] failed",
				      tunnel->name, target_host);
				close(target_fd);
				return;
			}
			memcpy(&target_sa.sin_addr, he->h_addr, sizeof(struct in_addr));
		}

		session = calloc(1, sizeof(*session));
		if (!session) {
			close(target_fd);
			return;
		}

		session->session_id = session_id;
		session->client_sa = *from;
		session->target_sa = target_sa;
		session->target_fd = target_fd;
		session->tunnel = tunnel;
		session->xkcpfd = xkcpfd;
		session->last_active = iclock();

		if (tunnel->param.fec) {
			char fkey[32];
			snprintf(fkey, sizeof(fkey), "%u:%u",
				 ntohl(from->sin_addr.s_addr), ntohs(from->sin_port));
			session->fec = get_peer_fec(tunnel, fkey);
		}

		session->target_ev = event_new(base, target_fd, EV_READ | EV_PERSIST,
					       udp_server_target_cb, session);
		if (!session->target_ev) {
			close(target_fd);
			free(session);
			return;
		}
		event_add(session->target_ev, NULL);
		iqueue_add(&session->node, &tunnel->udp_server_sessions);
		tunnel->total_conns++;

		debug(LOG_INFO, "[%s] Direct UDP session [%u] from %s:%d -> Target %s:%d created",
		      tunnel->name, session_id, inet_ntoa(from->sin_addr), ntohs(from->sin_port),
		      target_host, target_port);
	} else {
		session->last_active = iclock();
	}

	if (payload && payload_len > 0) {
		sendto(session->target_fd, payload, payload_len, 0,
		       (struct sockaddr *)&session->target_sa, sizeof(session->target_sa));
		tunnel->bytes_in += payload_len;
	}
}

void udp_server_check_timeout(struct xkcp_tunnel *tunnel)
{
	if (!tunnel) return;

	IUINT32 now = iclock();
	IUINT32 timeout_ms = (IUINT32)XKCP_UDP_SESSION_TTL * 1000;

	iqueue_head *p, *n;
	for (p = tunnel->udp_server_sessions.next, n = p->next;
	     p != &tunnel->udp_server_sessions; p = n, n = p->next) {
		struct udp_server_session *s = iqueue_entry(p, struct udp_server_session, node);
		if (_itimediff(now, s->last_active) > (IINT32)timeout_ms) {
			event_del(s->target_ev);
			event_free(s->target_ev);
			close(s->target_fd);
			iqueue_del(&s->node);
			free(s);
		} else if (s->fec) {
			xkcp_fec_conn_tick(s->fec, s->xkcpfd, &s->client_sa, s->tunnel);
		}
	}
}

void udp_server_cleanup(struct xkcp_tunnel *tunnel)
{
	if (!tunnel) return;

	iqueue_head *p, *n;
	for (p = tunnel->udp_server_sessions.next, n = p->next;
	     p != &tunnel->udp_server_sessions; p = n, n = p->next) {
		struct udp_server_session *s = iqueue_entry(p, struct udp_server_session, node);
		event_del(s->target_ev);
		event_free(s->target_ev);
		close(s->target_fd);
		iqueue_del(&s->node);
		free(s);
	}
}
