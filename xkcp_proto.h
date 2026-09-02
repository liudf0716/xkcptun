/********************************************************************\
 * This program is free software; you can redistribute it and/or    *
 * modify it under the terms of the GNU General Public License as   *
 * published by the Free Software Foundation; either version 2 of   *
 * the License, or (at your option) any later version.              *
 *                                                                  *
 * This program is distributed in the hope that it will be useful,  *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of   *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the    *
 * GNU General Public License for more details.                     *
 *                                                                  *
 * You should have received a copy of the GNU General Public License*
 * along with this program; if not, contact:                        *
 *                                                                  *
 * Free Software Foundation           Voice:  +1-617-542-5942       *
 * 59 Temple Place - Suite 330        Fax:    +1-617-542-2652       *
 * Boston, MA  02111-1307,  USA       gnu@gnu.org                   *
 *                                                                  *
\********************************************************************/

#ifndef _XKCP_PROTO_H_
#define _XKCP_PROTO_H_

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <arpa/inet.h>

#define XKCP_PROTO_MAGIC_0      0x58 /* 'X' */
#define XKCP_PROTO_MAGIC_1      0x4B /* 'K' */
#define XKCP_PROTO_VER_1        0x01 /* CONNECT, no authentication */
#define XKCP_PROTO_VER_2        0x02 /* CONNECT + ts(4) + auth token(16) */

#define XKCP_CMD_CONNECT        0x01

#define XKCP_ATYPE_IPV4         0x01
#define XKCP_ATYPE_DOMAIN       0x03
#define XKCP_ATYPE_IPV6         0x04

#define XKCP_AUTH_TS_LEN        4
#define XKCP_AUTH_TOKEN_LEN     16
/* 4 (IPv4) / 16 (IPv6) / 1 + 255 (domain) */
#define XKCP_AUTH_ADDR_MAX      256

/* Maximum header length: 5(prefix) + 263(addr+port) + 4(ts) + 16(token) */
#define XKCP_MAX_HDR_LEN        300

#pragma pack(push, 1)
struct xkcp_hdr_prefix {
	uint8_t magic[2];  /* 0x58, 0x4B */
	uint8_t ver;       /* 0x01 */
	uint8_t cmd;       /* 0x01 = CONNECT */
	uint8_t atype;     /* 0x01 = IPv4, 0x03 = Domain, 0x04 = IPv6 */
};
#pragma pack(pop)

/*
 * Serialize host into wire address bytes (same layout as the header body).
 * Returns the byte count, or < 0 if host is not a valid address/domain.
 */
static inline int xkcp_proto_serialize_addr(const char *host, uint8_t *out,
					    size_t outlen, uint8_t *atype_out)
{
	struct in_addr in4;
	struct in6_addr in6;
	size_t hlen;

	if (!host || !out || !atype_out)
		return -1;

	if (inet_pton(AF_INET, host, &in4) == 1) {
		if (outlen < 4) return -1;
		*atype_out = XKCP_ATYPE_IPV4;
		memcpy(out, &in4.s_addr, 4);
		return 4;
	}
	if (inet_pton(AF_INET6, host, &in6) == 1) {
		if (outlen < 16) return -1;
		*atype_out = XKCP_ATYPE_IPV6;
		memcpy(out, &in6.s6_addr, 16);
		return 16;
	}

	hlen = strlen(host);
	if (hlen == 0 || hlen > 255 || outlen < 1 + hlen)
		return -1;
	*atype_out = XKCP_ATYPE_DOMAIN;
	out[0] = (uint8_t)hlen;
	memcpy(out + 1, host, hlen);
	return (int)(1 + hlen);
}

/*
 * Encode dynamic destination header.
 * Returns encoded header length in bytes, or < 0 on error.
 */
static inline int xkcp_proto_encode_header(char *buf, size_t buflen, const char *host, uint16_t port)
{
	if (!buf || !host || buflen < 8)
		return -1;

	struct xkcp_hdr_prefix *hdr = (struct xkcp_hdr_prefix *)buf;
	hdr->magic[0] = XKCP_PROTO_MAGIC_0;
	hdr->magic[1] = XKCP_PROTO_MAGIC_1;
	hdr->ver = XKCP_PROTO_VER_1;
	hdr->cmd = XKCP_CMD_CONNECT;

	size_t offset = sizeof(struct xkcp_hdr_prefix);
	struct in_addr in4;
	struct in6_addr in6;

	if (inet_pton(AF_INET, host, &in4) == 1) {
		hdr->atype = XKCP_ATYPE_IPV4;
		if (offset + 4 + 2 > buflen) return -1;
		memcpy(buf + offset, &in4.s_addr, 4);
		offset += 4;
	} else if (inet_pton(AF_INET6, host, &in6) == 1) {
		hdr->atype = XKCP_ATYPE_IPV6;
		if (offset + 16 + 2 > buflen) return -1;
		memcpy(buf + offset, &in6.s6_addr, 16);
		offset += 16;
	} else {
		/* Domain name */
		hdr->atype = XKCP_ATYPE_DOMAIN;
		size_t hlen = strlen(host);
		if (hlen > 255 || offset + 1 + hlen + 2 > buflen)
			return -1;
		buf[offset++] = (uint8_t)hlen;
		memcpy(buf + offset, host, hlen);
		offset += hlen;
	}

	uint16_t nport = htons(port);
	memcpy(buf + offset, &nport, 2);
	offset += 2;

	return (int)offset;
}

/*
 * Decode dynamic destination header.
 * On success, fills host_out and port_out, sets ver_out to the header
 * version and, for v2, ts_out and token_out to the timestamp and HMAC token
 * carried after the port. Returns the number of consumed header bytes
 * (including ts+token for v2).
 * If data is not a valid dynamic header (e.g. raw payload from legacy
 * client), returns 0. If the header is incomplete, returns -1.
 */
static inline int xkcp_proto_decode_header_ex(const char *buf, size_t buflen,
					      char *host_out, size_t host_out_len,
					      uint16_t *port_out, uint8_t *ver_out,
					      const uint8_t **ts_out,
					      const uint8_t **token_out)
{
	if (!buf || buflen < sizeof(struct xkcp_hdr_prefix))
		return 0;

	const struct xkcp_hdr_prefix *hdr = (const struct xkcp_hdr_prefix *)buf;
	if (hdr->magic[0] != XKCP_PROTO_MAGIC_0 ||
	    hdr->magic[1] != XKCP_PROTO_MAGIC_1 ||
	    (hdr->ver != XKCP_PROTO_VER_1 && hdr->ver != XKCP_PROTO_VER_2) ||
	    hdr->cmd != XKCP_CMD_CONNECT) {
		return 0; /* Not a dynamic destination header */
	}

	if (ver_out) *ver_out = hdr->ver;
	if (ts_out) *ts_out = NULL;
	if (token_out) *token_out = NULL;

	size_t offset = sizeof(struct xkcp_hdr_prefix);

	if (hdr->atype == XKCP_ATYPE_IPV4) {
		if (buflen < offset + 4 + 2)
			return -1; /* Incomplete */

		struct in_addr in4;
		memcpy(&in4.s_addr, buf + offset, 4);
		offset += 4;
		if (host_out && host_out_len > 0)
			inet_ntop(AF_INET, &in4, host_out, host_out_len);
	} else if (hdr->atype == XKCP_ATYPE_IPV6) {
		if (buflen < offset + 16 + 2)
			return -1; /* Incomplete */

		struct in6_addr in6;
		memcpy(&in6.s6_addr, buf + offset, 16);
		offset += 16;
		if (host_out && host_out_len > 0)
			inet_ntop(AF_INET6, &in6, host_out, host_out_len);
	} else if (hdr->atype == XKCP_ATYPE_DOMAIN) {
		if (buflen < offset + 1)
			return -1;
		uint8_t dlen = (uint8_t)buf[offset++];
		if (buflen < offset + dlen + 2)
			return -1; /* Incomplete */

		if (host_out && host_out_len > 0) {
			size_t copy_len = dlen < (host_out_len - 1) ? dlen : (host_out_len - 1);
			memcpy(host_out, buf + offset, copy_len);
			host_out[copy_len] = '\0';
		}
		offset += dlen;
	} else {
		return 0; /* Unknown address type */
	}

	uint16_t nport;
	memcpy(&nport, buf + offset, 2);
	offset += 2;

	if (port_out)
		*port_out = ntohs(nport);

	if (hdr->ver == XKCP_PROTO_VER_2) {
		if (buflen < offset + XKCP_AUTH_TS_LEN + XKCP_AUTH_TOKEN_LEN)
			return -1; /* Incomplete */
		if (ts_out)
			*ts_out = (const uint8_t *)buf + offset;
		if (token_out)
			*token_out = (const uint8_t *)buf + offset + XKCP_AUTH_TS_LEN;
		offset += XKCP_AUTH_TS_LEN + XKCP_AUTH_TOKEN_LEN;
	}

	return (int)offset;
}

/* Backward-compatible wrapper: version-insensitive decode. */
static inline int xkcp_proto_decode_header(const char *buf, size_t buflen,
					   char *host_out, size_t host_out_len,
					   uint16_t *port_out)
{
	uint8_t ver;
	return xkcp_proto_decode_header_ex(buf, buflen, host_out, host_out_len,
					   port_out, &ver, NULL, NULL);
}

#endif /* _XKCP_PROTO_H_ */
