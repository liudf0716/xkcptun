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

/** @file xkcp_auth.c
 *  @brief HMAC-SHA256 tunnel authentication for the dynamic gateway
 */

#include <string.h>

#include "xkcp_auth.h"
#include "xkcp_proto.h"

/* "xkcp-auth-v2" || conv(4) || ts(4) || atype(1) || addr(<=261) || port(2) */
#define AUTH_MSG_MAX	300

/* ---- SHA-256 (FIPS 180-4) -------------------------------------------- */

struct sha256_ctx {
	uint32_t h[8];
	uint64_t nbytes;
	uint8_t buf[64];
	size_t buflen;
};

static const uint32_t sha256_k[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
	0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
	0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
	0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
	0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
	0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
	0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
	0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static uint32_t ror32(uint32_t x, int n)
{
	return (x >> n) | (x << (32 - n));
}

static void sha256_init(struct sha256_ctx *c)
{
	c->h[0] = 0x6a09e667; c->h[1] = 0xbb67ae85;
	c->h[2] = 0x3c6ef372; c->h[3] = 0xa54ff53a;
	c->h[4] = 0x510e527f; c->h[5] = 0x9b05688c;
	c->h[6] = 0x1f83d9ab; c->h[7] = 0x5be0cd19;
	c->nbytes = 0;
	c->buflen = 0;
}

static void sha256_block(struct sha256_ctx *c, const uint8_t *p)
{
	uint32_t w[64];
	uint32_t a, b, cc, d, e, f, g, h;
	int i;

	for (i = 0; i < 16; i++)
		w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
		       ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
	for (i = 16; i < 64; i++) {
		uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}

	a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
	e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];

	for (i = 0; i < 64; i++) {
		uint32_t S1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
		uint32_t ch = (e & f) ^ (~e & g);
		uint32_t t1 = h + S1 + ch + sha256_k[i] + w[i];
		uint32_t S0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
		uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
		uint32_t t2 = S0 + maj;

		h = g; g = f; f = e; e = d + t1;
		d = cc; cc = b; b = a; a = t1 + t2;
	}

	c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
	c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha256_update(struct sha256_ctx *c, const void *data, size_t len)
{
	const uint8_t *p = data;

	c->nbytes += len;
	if (c->buflen) {
		size_t take = 64 - c->buflen;
		if (take > len) take = len;
		memcpy(c->buf + c->buflen, p, take);
		c->buflen += take;
		p += take;
		len -= take;
		if (c->buflen == 64) {
			sha256_block(c, c->buf);
			c->buflen = 0;
		}
	}
	while (len >= 64) {
		sha256_block(c, p);
		p += 64;
		len -= 64;
	}
	if (len) {
		memcpy(c->buf, p, len);
		c->buflen = len;
	}
}

static void sha256_final(struct sha256_ctx *c, uint8_t out[32])
{
	uint64_t bits = c->nbytes * 8;
	uint8_t pad = 0x80;
	size_t i;

	sha256_update(c, &pad, 1);
	pad = 0;
	while (c->buflen != 56)
		sha256_update(c, &pad, 1);

	for (i = 0; i < 8; i++)
		c->buf[56 + i] = (uint8_t)(bits >> (56 - i * 8));
	sha256_block(c, c->buf);
	c->buflen = 0;

	for (i = 0; i < 8; i++) {
		out[i * 4] = (uint8_t)(c->h[i] >> 24);
		out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
		out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
		out[i * 4 + 3] = (uint8_t)c->h[i];
	}
}

static void hmac_sha256(const uint8_t *key, size_t key_len,
			const uint8_t *data, size_t data_len, uint8_t out[32])
{
	uint8_t k[64], ipad[64], opad[64], inner[32];
	struct sha256_ctx c;
	size_t i;

	memset(k, 0, sizeof(k));
	if (key_len > 64) {
		sha256_init(&c);
		sha256_update(&c, key, key_len);
		sha256_final(&c, k);
	} else if (key_len > 0) {
		memcpy(k, key, key_len);
	}

	for (i = 0; i < 64; i++) {
		ipad[i] = k[i] ^ 0x36;
		opad[i] = k[i] ^ 0x5c;
	}

	sha256_init(&c);
	sha256_update(&c, ipad, 64);
	sha256_update(&c, data, data_len);
	sha256_final(&c, inner);

	sha256_init(&c);
	sha256_update(&c, opad, 64);
	sha256_update(&c, inner, 32);
	sha256_final(&c, out);
}

/* ---- token construction / verification -------------------------------- */

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int xkcp_auth_digest(const char *key, uint32_t conv, uint32_t ts,
		     const char *host, uint16_t port, uint8_t out[32])
{
	uint8_t msg[AUTH_MSG_MAX];
	uint8_t addr[XKCP_AUTH_ADDR_MAX];
	uint8_t atype = 0;
	int addr_len;
	size_t klen = strlen(key);
	size_t off;

	memcpy(msg, "xkcp-auth-v2", 12);
	off = 12;
	put_le32(msg + off, conv); off += 4;
	put_le32(msg + off, ts); off += 4;

	addr_len = xkcp_proto_serialize_addr(host, addr, sizeof(addr), &atype);
	if (addr_len <= 0)
		return -1;
	msg[off++] = atype;
	memcpy(msg + off, addr, (size_t)addr_len);
	off += (size_t)addr_len;
	msg[off++] = (uint8_t)(port & 0xff);
	msg[off++] = (uint8_t)(port >> 8);

	hmac_sha256((const uint8_t *)key, klen, msg, off, out);
	return 0;
}

static int ct_memcmp16(const uint8_t *a, const uint8_t *b)
{
	uint8_t d = 0;
	int i;

	for (i = 0; i < XKCP_AUTH_TOKEN_LEN; i++)
		d |= a[i] ^ b[i];
	return d != 0;
}

int xkcp_auth_encode_header(char *buf, size_t buflen, const char *host,
			    uint16_t port, const char *key, uint32_t conv,
			    uint32_t now_sec)
{
	uint8_t digest[32];
	int n;
	size_t off;

	if (!buf || !key || !host)
		return -1;

	n = xkcp_proto_encode_header(buf, buflen, host, port);
	if (n < 0)
		return -1;
	if ((size_t)n + XKCP_AUTH_TS_LEN + XKCP_AUTH_TOKEN_LEN > buflen)
		return -1;

	buf[2] = XKCP_PROTO_VER_2; /* CONNECT + auth */

	if (xkcp_auth_digest(key, conv, now_sec, host, port, digest) < 0)
		return -1;

	off = (size_t)n;
	put_le32((uint8_t *)buf + off, now_sec);
	off += XKCP_AUTH_TS_LEN;
	memcpy(buf + off, digest, XKCP_AUTH_TOKEN_LEN);
	off += XKCP_AUTH_TOKEN_LEN;

	return (int)off;
}

int xkcp_auth_verify(const char *key, uint32_t conv, const char *host,
		     uint16_t port, const uint8_t *ts, const uint8_t *token,
		     uint32_t now_sec)
{
	uint8_t digest[32];
	uint32_t t;

	if (!key || !host || !ts || !token)
		return -1;

	t = get_le32(ts);
	{
		int32_t diff = (int32_t)(now_sec - t);
		if (diff < 0) diff = -diff;
		if (diff > (int32_t)XKCP_AUTH_WINDOW_SEC)
			return -1;
	}

	if (xkcp_auth_digest(key, conv, t, host, port, digest) < 0)
		return -1;

	return ct_memcmp16(digest, token) ? -1 : 0;
}
