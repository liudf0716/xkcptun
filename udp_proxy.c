#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <syslog.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include <event2/event.h>
#include <event2/util.h>

#include "udp_proxy.h"
#include "xkcp_udp.h"
#include "debug.h"

struct udp_client_session {
	iqueue_head node;
	uint32_t session_id;
	struct sockaddr_in client_sa;
	IUINT32 last_active;
};

static uint32_t g_client_session_seq = 1;

static struct udp_client_session *find_client_session_by_sa(struct xkcp_tunnel *tunnel,
							    const struct sockaddr_in *sa)
{
	struct udp_client_session *s;
	iqueue_foreach(s, &tunnel->udp_client_sessions, struct udp_client_session, node) {
		if (s->client_sa.sin_addr.s_addr == sa->sin_addr.s_addr &&
		    s->client_sa.sin_port == sa->sin_port) {
			return s;
		}
	}
	return NULL;
}

static struct udp_client_session *find_client_session_by_id(struct xkcp_tunnel *tunnel,
							    uint32_t session_id)
{
	struct udp_client_session *s;
	iqueue_foreach(s, &tunnel->udp_client_sessions, struct udp_client_session, node) {
		if (s->session_id == session_id)
			return s;
	}
	return NULL;
}

static void udp_proxy_read_cb(evutil_socket_t fd, short what, void *arg)
{
	struct xkcp_tunnel *tunnel = arg;
	if (!tunnel) return;

	char buf[XKCP_RECV_BUF_LEN];
	char enc_buf[XKCP_RECV_BUF_LEN + 128];
	struct sockaddr_in client_sa;
	socklen_t sa_len;
	int nrecv;

	(void)what;

	while (1) {
		sa_len = sizeof(client_sa);
		nrecv = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&client_sa, &sa_len);
		if (nrecv <= 0)
			break;

		struct udp_client_session *session = find_client_session_by_sa(tunnel, &client_sa);
		if (!session) {
			session = calloc(1, sizeof(*session));
			if (!session) continue;
			session->session_id = g_client_session_seq++;
			if (g_client_session_seq == 0) g_client_session_seq = 1;
			session->client_sa = client_sa;
			session->last_active = iclock();
			iqueue_add(&session->node, &tunnel->udp_client_sessions);
			tunnel->total_conns++;
		} else {
			session->last_active = iclock();
		}

		const char *target_host = tunnel->param.target_addr ? tunnel->param.target_addr : "127.0.0.1";
		uint16_t target_port = tunnel->param.target_port > 0 ? (uint16_t)tunnel->param.target_port : 53;

		int enc_len = xkcp_udp_encode_req(enc_buf, sizeof(enc_buf),
						  session->session_id,
						  target_host, target_port,
						  tunnel->param.key,
						  buf, nrecv);
		if (enc_len > 0) {
			xkcp_send_tunnel_packet(tunnel->xkcp_fd, &tunnel->client_proxy_param.sockaddr,
						tunnel->client_fec, enc_buf, enc_len, tunnel);
			tunnel->bytes_in += nrecv;
		}
	}
}

int init_udp_proxy(struct xkcp_tunnel *tunnel)
{
	if (!tunnel) return -1;

	iqueue_init(&tunnel->udp_client_sessions);

	char *addr = get_iface_ip(tunnel->param.local_interface);
	if (!addr) {
		debug(LOG_ERR, "[%s] get_iface_ip [%s] failed",
		      tunnel->name, tunnel->param.local_interface);
		return -1;
	}

	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		debug(LOG_ERR, "[%s] create UDP proxy socket failed: %s",
		      tunnel->name, strerror(errno));
		free(addr);
		return -1;
	}

	evutil_make_socket_nonblocking(fd);
	evutil_make_listen_socket_reuseable(fd);

	if (tunnel->param.sock_buf > 0) {
		int opt = tunnel->param.sock_buf;
		setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &opt, sizeof(opt));
		setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &opt, sizeof(opt));
	}

	struct sockaddr_in sin;
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = inet_addr(addr);
	sin.sin_port = htons(tunnel->param.local_port);

	if (bind(fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
		debug(LOG_ERR, "[%s] bind UDP proxy socket to %s:%d failed: %s",
		      tunnel->name, addr, tunnel->param.local_port, strerror(errno));
		close(fd);
		free(addr);
		return -1;
	}

	tunnel->udp_local_fd = fd;
	tunnel->udp_local_ev = event_new(tunnel->base, fd, EV_READ | EV_PERSIST,
					 udp_proxy_read_cb, tunnel);
	if (!tunnel->udp_local_ev) {
		debug(LOG_ERR, "[%s] event_new failed for UDP proxy", tunnel->name);
		close(fd);
		free(addr);
		return -1;
	}
	event_add(tunnel->udp_local_ev, NULL);

	debug(LOG_INFO, "[%s] UDP Proxy listening on %s:%d -> Remote %s:%d -> Target %s:%d",
	      tunnel->name, addr, tunnel->param.local_port,
	      tunnel->param.remote_addr, tunnel->param.remote_port,
	      tunnel->param.target_addr ? tunnel->param.target_addr : "127.0.0.1",
	      tunnel->param.target_port);

	free(addr);
	return 0;
}

void udp_proxy_handle_server_packet(struct xkcp_tunnel *tunnel, const char *buf, int nrecv)
{
	if (!tunnel || !buf || nrecv < 8) return;

	uint32_t session_id = 0;
	const char *payload = NULL;
	size_t payload_len = 0;

	if (xkcp_udp_decode_resp(buf, nrecv, &session_id, &payload, &payload_len) != 0)
		return;

	struct udp_client_session *session = find_client_session_by_id(tunnel, session_id);
	if (session && payload && payload_len > 0) {
		sendto(tunnel->udp_local_fd, payload, payload_len, 0,
		       (struct sockaddr *)&session->client_sa, sizeof(session->client_sa));
		session->last_active = iclock();
		tunnel->bytes_out += payload_len;
	}
}

void udp_proxy_check_timeout(struct xkcp_tunnel *tunnel)
{
	if (!tunnel) return;

	IUINT32 now = iclock();
	IUINT32 timeout_ms = (IUINT32)XKCP_UDP_SESSION_TTL * 1000;

	iqueue_head *p, *n;
	for (p = tunnel->udp_client_sessions.next, n = p->next;
	     p != &tunnel->udp_client_sessions; p = n, n = p->next) {
		struct udp_client_session *s = iqueue_entry(p, struct udp_client_session, node);
		if (_itimediff(now, s->last_active) > (IINT32)timeout_ms) {
			iqueue_del(&s->node);
			free(s);
		}
	}
}

void udp_proxy_cleanup(struct xkcp_tunnel *tunnel)
{
	if (!tunnel) return;

	if (tunnel->udp_local_ev) {
		event_del(tunnel->udp_local_ev);
		event_free(tunnel->udp_local_ev);
		tunnel->udp_local_ev = NULL;
	}
	if (tunnel->udp_local_fd > 0) {
		close(tunnel->udp_local_fd);
		tunnel->udp_local_fd = -1;
	}

	iqueue_head *p, *n;
	for (p = tunnel->udp_client_sessions.next, n = p->next;
	     p != &tunnel->udp_client_sessions; p = n, n = p->next) {
		struct udp_client_session *s = iqueue_entry(p, struct udp_client_session, node);
		iqueue_del(&s->node);
		free(s);
	}
}
