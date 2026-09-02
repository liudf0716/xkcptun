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
\********************************************************************/

#ifndef _XKCP_AUTH_
#define _XKCP_AUTH_

#include <stdint.h>
#include <stddef.h>

#define XKCP_AUTH_TOKEN_LEN	16
#define XKCP_AUTH_TS_LEN	4
/* max replay window for the timestamp carried in the auth header */
#define XKCP_AUTH_WINDOW_SEC	600

/*
 * Dynamic-target authentication (protocol v2 header).
 *
 * The client appends a timestamp and a truncated HMAC-SHA256 token after the
 * v1 CONNECT header:
 *   token = HMAC-SHA256(key, "xkcp-auth-v2" || conv_le4 || ts_le4 ||
 *                            atype || addr_bytes || port_le2)[0..15]
 * where addr_bytes is 4 bytes (IPv4), 16 bytes (IPv6) or 1+len+name (domain),
 * exactly as serialized in the header itself. The server recomputes the HMAC
 * over the decoded fields and rejects the session on mismatch or when
 * |now - ts| exceeds XKCP_AUTH_WINDOW_SEC.
 */

/* Build a full v2 CONNECT header (address + port + ts + token) into buf.
 * Returns the encoded length, or < 0 on error. */
int xkcp_auth_encode_header(char *buf, size_t buflen, const char *host,
			    uint16_t port, const char *key, uint32_t conv,
			    uint32_t now_sec);

/* Verify the ts+token pair at ts_ptr/token against host/port/conv/key.
 * Returns 0 on success, < 0 on failure. */
int xkcp_auth_verify(const char *key, uint32_t conv, const char *host,
		     uint16_t port, const uint8_t *ts, const uint8_t *token,
		     uint32_t now_sec);

/* Compute full 32-byte HMAC-SHA256 digest */
int xkcp_auth_digest(const char *key, uint32_t conv, uint32_t ts,
		     const char *host, uint16_t port, uint8_t out[32]);

#endif
