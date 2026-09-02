#ifndef _XKCP_UDP_H_
#define _XKCP_UDP_H_

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <arpa/inet.h>
#include "xkcp_proto.h"
#include "xkcp_auth.h"

#define XKCP_UDP_MAGIC_0        'X'
#define XKCP_UDP_MAGIC_1        'U'
#define XKCP_UDP_VER            0x01

#define XKCP_UDP_CMD_REQ        0x01    /* Client -> Server: UDP payload with target */
#define XKCP_UDP_CMD_RESP       0x02    /* Server -> Client: UDP payload reply */

#define XKCP_UDP_SESSION_TTL    30      /* Seconds before inactive UDP session is reaped */

#pragma pack(push, 1)

/* Fixed prefix for client request */
struct xkcp_udp_req_hdr {
	uint8_t  magic[2];      /* 'X', 'U' */
	uint8_t  ver;           /* 0x01 */
	uint8_t  cmd;           /* 0x01 (XKCP_UDP_CMD_REQ) */
	uint32_t session_id;    /* Session identifier allocated by client */
	uint8_t  atype;         /* 0x01 IPv4, 0x03 Domain, 0x04 IPv6 */
};

/* Fixed prefix for server response */
struct xkcp_udp_resp_hdr {
	uint8_t  magic[2];      /* 'X', 'U' */
	uint8_t  ver;           /* 0x01 */
	uint8_t  cmd;           /* 0x02 (XKCP_UDP_CMD_RESP) */
	uint32_t session_id;    /* Session identifier matching the request */
};

#pragma pack(pop)

/*
 * Encode a client UDP tunnel request datagram into buf.
 * Returns the total encoded length on success, or -1 on failure.
 */
static inline int xkcp_udp_encode_req(char *buf, size_t buflen,
				      uint32_t session_id,
				      const char *target_host, uint16_t target_port,
				      const char *key,
				      const char *payload, size_t payload_len)
{
	if (!buf || !target_host || target_port == 0)
		return -1;

	size_t host_len = strlen(target_host);
	if (host_len == 0 || host_len >= XKCP_AUTH_ADDR_MAX)
		return -1;

	/* Determine address type */
	uint8_t atype = XKCP_ATYPE_DOMAIN;
	struct in_addr in4;
	struct in6_addr in6;
	size_t addr_len = 0;

	if (inet_pton(AF_INET, target_host, &in4) == 1) {
		atype = XKCP_ATYPE_IPV4;
		addr_len = 4;
	} else if (inet_pton(AF_INET6, target_host, &in6) == 1) {
		atype = XKCP_ATYPE_IPV6;
		addr_len = 16;
	} else {
		atype = XKCP_ATYPE_DOMAIN;
		addr_len = 1 + host_len;
	}

	size_t hdr_len = sizeof(struct xkcp_udp_req_hdr) + addr_len + sizeof(uint16_t);
	if (key && key[0] != '\0')
		hdr_len += sizeof(uint32_t) + XKCP_AUTH_TOKEN_LEN;

	if (buflen < hdr_len + payload_len)
		return -1;

	struct xkcp_udp_req_hdr *hdr = (struct xkcp_udp_req_hdr *)buf;
	hdr->magic[0] = XKCP_UDP_MAGIC_0;
	hdr->magic[1] = XKCP_UDP_MAGIC_1;
	hdr->ver = XKCP_UDP_VER;
	hdr->cmd = XKCP_UDP_CMD_REQ;
	hdr->session_id = session_id;
	hdr->atype = atype;

	size_t offset = sizeof(struct xkcp_udp_req_hdr);

	if (atype == XKCP_ATYPE_IPV4) {
		memcpy(buf + offset, &in4, 4);
		offset += 4;
	} else if (atype == XKCP_ATYPE_IPV6) {
		memcpy(buf + offset, &in6, 16);
		offset += 16;
	} else {
		buf[offset++] = (uint8_t)host_len;
		memcpy(buf + offset, target_host, host_len);
		offset += host_len;
	}

	uint16_t nport = htons(target_port);
	memcpy(buf + offset, &nport, sizeof(nport));
	offset += sizeof(nport);

	if (key && key[0] != '\0') {
		uint32_t now_ts = (uint32_t)time(NULL);
		uint8_t ts_bytes[4];
		ts_bytes[0] = (uint8_t)(now_ts & 0xff);
		ts_bytes[1] = (uint8_t)((now_ts >> 8) & 0xff);
		ts_bytes[2] = (uint8_t)((now_ts >> 16) & 0xff);
		ts_bytes[3] = (uint8_t)((now_ts >> 24) & 0xff);
		memcpy(buf + offset, ts_bytes, 4);
		offset += 4;

		uint8_t digest[32];
		xkcp_auth_digest(key, session_id, now_ts, target_host, target_port, digest);
		memcpy(buf + offset, digest, XKCP_AUTH_TOKEN_LEN);
		offset += XKCP_AUTH_TOKEN_LEN;
	}

	if (payload && payload_len > 0) {
		memcpy(buf + offset, payload, payload_len);
		offset += payload_len;
	}

	return (int)offset;
}

/*
 * Decode a client UDP tunnel request datagram from buf.
 * Returns 0 on success, or -1 on error/auth-failure.
 */
static inline int xkcp_udp_decode_req(const char *buf, size_t buflen,
				      uint32_t *session_id_out,
				      char *target_host_out, size_t host_out_len,
				      uint16_t *target_port_out,
				      const char *key,
				      const char **payload_out, size_t *payload_len_out)
{
	if (!buf || buflen < sizeof(struct xkcp_udp_req_hdr))
		return -1;

	const struct xkcp_udp_req_hdr *hdr = (const struct xkcp_udp_req_hdr *)buf;
	if (hdr->magic[0] != XKCP_UDP_MAGIC_0 || hdr->magic[1] != XKCP_UDP_MAGIC_1 ||
	    hdr->ver != XKCP_UDP_VER || hdr->cmd != XKCP_UDP_CMD_REQ)
		return -1;

	if (session_id_out)
		*session_id_out = hdr->session_id;

	size_t offset = sizeof(struct xkcp_udp_req_hdr);
	char host[XKCP_AUTH_ADDR_MAX] = {0};

	if (hdr->atype == XKCP_ATYPE_IPV4) {
		if (buflen < offset + 4) return -1;
		inet_ntop(AF_INET, buf + offset, host, sizeof(host));
		offset += 4;
	} else if (hdr->atype == XKCP_ATYPE_IPV6) {
		if (buflen < offset + 16) return -1;
		inet_ntop(AF_INET6, buf + offset, host, sizeof(host));
		offset += 16;
	} else if (hdr->atype == XKCP_ATYPE_DOMAIN) {
		if (buflen < offset + 1) return -1;
		uint8_t dlen = (uint8_t)buf[offset++];
		if (buflen < offset + dlen || dlen >= sizeof(host)) return -1;
		memcpy(host, buf + offset, dlen);
		host[dlen] = '\0';
		offset += dlen;
	} else {
		return -1;
	}

	if (buflen < offset + sizeof(uint16_t))
		return -1;

	uint16_t nport;
	memcpy(&nport, buf + offset, sizeof(nport));
	uint16_t port = ntohs(nport);
	offset += sizeof(nport);

	/* Check and verify HMAC authentication if key configured */
	if (key && key[0] != '\0') {
		if (buflen < offset + sizeof(uint32_t) + XKCP_AUTH_TOKEN_LEN)
			return -1;

		const uint8_t *ts_ptr = (const uint8_t *)(buf + offset);
		offset += sizeof(uint32_t);

		const uint8_t *token = (const uint8_t *)(buf + offset);
		offset += XKCP_AUTH_TOKEN_LEN;

		uint32_t now_sec = (uint32_t)time(NULL);
		uint32_t sid = session_id_out ? *session_id_out : hdr->session_id;
		if (xkcp_auth_verify(key, sid, host, port, ts_ptr, token, now_sec) != 0)
			return -1; /* Auth failure */
	}

	if (target_host_out && host_out_len > 0) {
		size_t hlen = strlen(host);
		if (hlen >= host_out_len) hlen = host_out_len - 1;
		memcpy(target_host_out, host, hlen);
		target_host_out[hlen] = '\0';
	}
	if (target_port_out)
		*target_port_out = port;

	if (payload_out)
		*payload_out = buf + offset;
	if (payload_len_out)
		*payload_len_out = buflen - offset;

	return 0;
}

/*
 * Encode a server UDP response datagram into buf.
 */
static inline int xkcp_udp_encode_resp(char *buf, size_t buflen,
				       uint32_t session_id,
				       const char *payload, size_t payload_len)
{
	if (!buf || buflen < sizeof(struct xkcp_udp_resp_hdr) + payload_len)
		return -1;

	struct xkcp_udp_resp_hdr *hdr = (struct xkcp_udp_resp_hdr *)buf;
	hdr->magic[0] = XKCP_UDP_MAGIC_0;
	hdr->magic[1] = XKCP_UDP_MAGIC_1;
	hdr->ver = XKCP_UDP_VER;
	hdr->cmd = XKCP_UDP_CMD_RESP;
	hdr->session_id = session_id;

	if (payload && payload_len > 0)
		memcpy(buf + sizeof(struct xkcp_udp_resp_hdr), payload, payload_len);

	return (int)(sizeof(struct xkcp_udp_resp_hdr) + payload_len);
}

/*
 * Decode a server UDP response datagram from buf.
 */
static inline int xkcp_udp_decode_resp(const char *buf, size_t buflen,
				       uint32_t *session_id_out,
				       const char **payload_out, size_t *payload_len_out)
{
	if (!buf || buflen < sizeof(struct xkcp_udp_resp_hdr))
		return -1;

	const struct xkcp_udp_resp_hdr *hdr = (const struct xkcp_udp_resp_hdr *)buf;
	if (hdr->magic[0] != XKCP_UDP_MAGIC_0 || hdr->magic[1] != XKCP_UDP_MAGIC_1 ||
	    hdr->ver != XKCP_UDP_VER || hdr->cmd != XKCP_UDP_CMD_RESP)
		return -1;

	if (session_id_out)
		*session_id_out = hdr->session_id;

	if (payload_out)
		*payload_out = buf + sizeof(struct xkcp_udp_resp_hdr);
	if (payload_len_out)
		*payload_len_out = buflen - sizeof(struct xkcp_udp_resp_hdr);

	return 0;
}

#endif /* _XKCP_UDP_H_ */
