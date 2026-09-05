/*
 * FEC - forward error correction for xkcptun
 *
 * Systematic Reed-Solomon erasure code over GF(2^8) using a Cauchy
 * matrix (any square submatrix is invertible, so decoding is always
 * possible whenever at least datashard shards of a group arrive).
 *
 * Copyright: GPLV3, same as the rest of xkcptun.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <sys/time.h>
#include <unistd.h>

#include "fec.h"

/* every data shard payload carries a 2-byte real-packet-length prefix so
 * shards reconstructed from parity can be trimmed back to original size */
#define FEC_LEN_PREFIX	2

/* ------------------------------------------------------------------ */
/* GF(2^8) arithmetic, primitive polynomial 0x11d                      */
/* ------------------------------------------------------------------ */

static uint8_t gf_exp[512];
static uint8_t gf_log[256];
static int gf_ready = 0;

static void gf_init(void)
{
	int i, x = 1;

	if (gf_ready)
		return;
	for (i = 0; i < 255; i++) {
		gf_exp[i] = (uint8_t)x;
		gf_log[x] = (uint8_t)i;
		x <<= 1;
		if (x & 0x100)
			x ^= 0x11d;
	}
	for (; i < 512; i++)
		gf_exp[i] = gf_exp[i - 255];
	gf_ready = 1;
}

static uint8_t gmul(uint8_t a, uint8_t b)
{
	if (!a || !b)
		return 0;
	return gf_exp[gf_log[a] + gf_log[b]];
}

static uint8_t ginv(uint8_t a)
{
	return gf_exp[255 - gf_log[a]];
}

/* ------------------------------------------------------------------ */
/* header pack/unpack                                                  */
/* ------------------------------------------------------------------ */

static uint32_t now_ms(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

static void fec_pack_hdr(uint8_t *h, int parity, uint8_t idx,
			 uint16_t gid, uint16_t size, uint16_t olen)
{
	h[0] = (FEC_VERSION << 4) | (parity ? 1 : 0);
	h[1] = idx;
	h[2] = (uint8_t)(gid >> 8);
	h[3] = (uint8_t)(gid & 0xff);
	h[4] = (uint8_t)(size >> 8);
	h[5] = (uint8_t)(size & 0xff);
	h[6] = (uint8_t)(olen >> 8);
	h[7] = (uint8_t)(olen & 0xff);
}


static int fec_parse_hdr(const char *pkt, int len, int *parity, uint8_t *idx,
			 uint16_t *gid, uint16_t *size, uint16_t *olen)
{
	const uint8_t *h = (const uint8_t *)pkt;

	if (len < FEC_HDR_SIZE || len > 65535)
		return -1;
	if ((h[0] >> 4) != FEC_VERSION)
		return -1;

	*parity = h[0] & 1;
	*idx    = h[1];
	*gid    = (uint16_t)((h[2] << 8) | h[3]);
	*size   = (uint16_t)((h[4] << 8) | h[5]);
	*olen   = (uint16_t)((h[6] << 8) | h[7]);

	if (*size != len - FEC_HDR_SIZE)
		return -1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* decoder group                                                       */
/* ------------------------------------------------------------------ */

struct fec_grp {
	uint16_t gid;
	uint64_t bitmap;	/* bit i set: shard[i] stored */
	int cnt;
	int done;
	uint32_t tick;
	uint8_t **shard;	/* payload buffers (no header), n entries */
	uint16_t *slen;		/* stored payload length */
	uint16_t *olen;		/* original length before padding */
};

/* ------------------------------------------------------------------ */
/* fec_conn                                                            */
/* ------------------------------------------------------------------ */

struct fec_conn {
	int k, r, n;
	int r_eff;		/* parity actually emitted now, <= r */
	int cap;		/* max shard payload bytes */
	uint16_t next_gid;
	uint32_t tick;		/* last encode/decode time, for idle detection */
	uint32_t grp_ts;	/* first shard time of the current tx group */
	uint32_t adapt_ts;	/* last parity-ratio re-adaptation */
	uint32_t rx_shards;	/* shards received since last adaptation */
	uint32_t rx_recovered;	/* shards reconstructed from parity */

	uint8_t *coef;		/* r*k cauchy coefficients */

	/* tx staging: each buffer holds [hdr][payload] of a data shard */
	int tx_cnt;
	uint8_t *tx_shard[FEC_SHARD_MAX];
	uint16_t tx_len[FEC_SHARD_MAX];

	/* rx pending groups ring */
	struct fec_grp grp[FEC_RX_GROUPS];
};

struct fec_conn *fec_conn_new(int datashard, int parityshard, int shard_cap)
{
	struct fec_conn *c;
	int i, j;

	gf_init();

	if (datashard < 1 || datashard > FEC_SHARD_MAX ||
	    parityshard < 0 || parityshard > FEC_SHARD_MAX ||
	    /* the decoder tracks stored shards in a 64-bit bitmap */
	    datashard + parityshard > 64 ||
	    shard_cap <= 0 || shard_cap > 60000)
		return NULL;

	c = calloc(1, sizeof(*c));
	if (!c)
		return NULL;

	c->k = datashard;
	c->r = parityshard;
	c->n = c->k + c->r;
	c->r_eff = c->r;
	c->cap = shard_cap;

	c->coef = malloc((size_t)c->r * c->k);
	if (!c->coef) {
		free(c);
		return NULL;
	}

	/* Seed the tx group id from a random offset instead of 0.  The rx
	 * dedup is a (gid,idx) bitmap over pending/done groups that are only
	 * recycled when the ring refills; if the peer restarts (its gid
	 * counter resets to 0) while our stale ring still holds those gids,
	 * the whole fresh stream would be dropped as duplicates forever.
	 * A random start makes a collision with the peer's stale ring
	 * entries a ~64/65536 event instead of a certainty. */
	{
		struct timeval tv;
		gettimeofday(&tv, NULL);
		c->next_gid = (uint16_t)(tv.tv_sec ^ (tv.tv_usec << 5) ^
					 (uintptr_t)c ^ (getpid() << 8));
	}
	/* Cauchy matrix: x_i = i (parity rows), y_j = k+j... use r+j so
	 * x and y ranges are disjoint: coeff = 1 / (x_i ^ y_j) */
	for (i = 0; i < c->r; i++)
		for (j = 0; j < c->k; j++) {
			uint8_t d = (uint8_t)(i ^ (c->r + j));
			c->coef[i * c->k + j] = ginv(d);
		}

	for (i = 0; i < c->k; i++) {
		c->tx_shard[i] = malloc(FEC_HDR_SIZE + FEC_LEN_PREFIX + c->cap);
		if (!c->tx_shard[i])
			goto err;
	}

	return c;
err:
	fec_conn_free(c);
	return NULL;
}

void fec_conn_free(struct fec_conn *c)
{
	int i, j;

	if (!c)
		return;
	for (i = 0; i < FEC_SHARD_MAX; i++)
		free(c->tx_shard[i]);
	for (i = 0; i < FEC_RX_GROUPS; i++) {
		struct fec_grp *g = &c->grp[i];
		if (g->shard)
			for (j = 0; j < c->n; j++)
				free(g->shard[j]);
		free(g->shard);
		free(g->slen);
		free(g->olen);
	}
	free(c->coef);
	free(c);
}

/* milliseconds since the codec last encoded or decoded a packet */
uint32_t fec_conn_idle_ms(const struct fec_conn *c)
{
	return now_ms() - c->tick;
}

/* ------------------------------------------------------------------ */
/* encoding                                                            */
/* ------------------------------------------------------------------ */

static void fec_emit_parity(struct fec_conn *c, fec_pkt_cb out, void *user);

void fec_conn_encode(struct fec_conn *c, const char *data, int len,
		     fec_pkt_cb out, void *user)
{
	uint16_t gid;
	int i;

	if (!c || !out)
		return;

	c->tick = now_ms();

	if (len > c->cap || len <= 0) {
		/* cannot happen with correct mtu config; drop instead of
		 * corrupting the group */
		static unsigned long dropped = 0;
		fprintf(stderr, "fec: segment %d exceeds shard capacity %d, "
			"dropped (%lu total)\n", len, c->cap, ++dropped);
		return;
	}

	if (c->tx_cnt == 0) {
		gid = c->next_gid++;
		c->grp_ts = now_ms();
	} else
		gid = (uint16_t)(c->next_gid - 1);

	/* stage the shard and emit it immediately: no added latency.
	 * payload layout: [2-byte real length][packet bytes]; the length
	 * prefix survives erasure-reconstruction so recovered shards can
	 * be trimmed back to their original size. */
	i = c->tx_cnt;
	c->tx_shard[i][FEC_HDR_SIZE]     = (uint8_t)(len >> 8);
	c->tx_shard[i][FEC_HDR_SIZE + 1] = (uint8_t)(len & 0xff);
	memcpy(c->tx_shard[i] + FEC_HDR_SIZE + FEC_LEN_PREFIX, data, len);
	c->tx_len[i] = (uint16_t)(len + FEC_LEN_PREFIX);
	fec_pack_hdr(c->tx_shard[i], 0, (uint8_t)i, gid,
		     c->tx_len[i], c->tx_len[i]);
	out(user, (const char *)c->tx_shard[i], FEC_HDR_SIZE + c->tx_len[i]);
	c->tx_cnt++;

	if (c->tx_cnt >= c->k)
		fec_emit_parity(c, out, user);
}

/* Generate parity for the staged tx group (partial groups included),
 * emit it, and close the group. Uses the first r_eff cauchy rows; the
 * decoder derives each parity row purely from its shard index, so a
 * group carrying fewer parity rows than the original ratio stays
 * decodable. */
static void fec_emit_parity(struct fec_conn *c, fec_pkt_cb out, void *user)
{
	int i, j, w, width = 0;

	for (i = 0; i < c->k; i++)
		if (c->tx_len[i] > width)
			width = c->tx_len[i];

	if (c->r_eff > 0) {
		uint8_t *par = malloc((size_t)c->r_eff * (FEC_HDR_SIZE + width));
		if (par) {
			/* zero each parity payload region ([hdr][payload] blocks) */
			for (i = 0; i < c->r_eff; i++)
				memset(par + (size_t)i * (FEC_HDR_SIZE + width) + FEC_HDR_SIZE,
				       0, width);

			for (j = 0; j < c->k; j++) {
				for (i = 0; i < c->r_eff; i++) {
					uint8_t cf = c->coef[i * c->k + j];
					uint8_t *dst = par + (size_t)i * (FEC_HDR_SIZE + width) + FEC_HDR_SIZE;
					const uint8_t *src = c->tx_shard[j] + FEC_HDR_SIZE;
					if (!cf)
						continue;
					for (w = 0; w < c->tx_len[j]; w++)
						dst[w] ^= gmul(cf, src[w]);
				}
			}

			for (i = 0; i < c->r_eff; i++) {
				uint8_t *p = par + (size_t)i * (FEC_HDR_SIZE + width);
				fec_pack_hdr(p, 1, (uint8_t)(c->k + i), (uint16_t)(c->next_gid - 1),
					     (uint16_t)width, (uint16_t)width);
				out(user, (const char *)p, FEC_HDR_SIZE + width);
			}
			free(par);
		}
	}

	c->tx_cnt = 0;
}

void fec_conn_tick(struct fec_conn *c, fec_pkt_cb out, void *user)
{
	uint32_t now;

	if (!c || !out)
		return;

	now = now_ms();

	/* re-adapt the parity ratio every 5s from the observed share of
	 * shards that had to be reconstructed (i.e. were lost in transit) */
	if (now - c->adapt_ts >= 5000) {
		c->adapt_ts = now;
		if (c->rx_shards >= 200) {
			int rate = c->rx_recovered * 100 / c->rx_shards;
			int want = (rate * c->k * 15 + 999) / 1000;	/* ~1.5x loss */

			if (want > c->r)
				want = c->r;
			/* move one shard at a time for stability */
			if (want > c->r_eff && c->r_eff < c->r)
				c->r_eff++;
			else if (want < c->r_eff && c->r_eff > 0)
				c->r_eff--;
			c->rx_shards = 0;
			c->rx_recovered = 0;
		}
	}

	/* a stale partial group gets its parity now instead of never:
	 * this is what protects small interactive flows */
	if (c->tx_cnt > 0 && now - c->grp_ts >= FEC_PARTIAL_FLUSH_MS)
		fec_emit_parity(c, out, user);
}

/* ------------------------------------------------------------------ */
/* decoding                                                            */
/* ------------------------------------------------------------------ */

static void grp_reset(struct fec_grp *g, int n)
{
	int j;
	if (g->shard)
		for (j = 0; j < n; j++)
			free(g->shard[j]);
	free(g->shard);
	free(g->slen);
	free(g->olen);
	memset(g, 0, sizeof(*g));
}

/* Find a group by id, including already-completed ones, so that late or
 * duplicate packets for a finished group are recognized and dropped
 * instead of spawning a bogus new group. */
static struct fec_grp *grp_find(struct fec_conn *c, uint16_t gid)
{
	int i;
	for (i = 0; i < FEC_RX_GROUPS; i++)
		if (c->grp[i].shard && c->grp[i].gid == gid)
			return &c->grp[i];
	return NULL;
}

static struct fec_grp *grp_create(struct fec_conn *c, uint16_t gid, uint32_t now)
{
	struct fec_grp *g = NULL, *oldest = NULL;
	int i;

	/* prefer a genuinely free slot */
	for (i = 0; i < FEC_RX_GROUPS; i++)
		if (!c->grp[i].shard) {
			g = &c->grp[i];
			break;
		}

	/* ring full: recycle the oldest pending group */
	if (!g) {
		for (i = 0; i < FEC_RX_GROUPS; i++) {
			struct fec_grp *e = &c->grp[i];
			if (!oldest || e->tick < oldest->tick)
				oldest = e;
		}
		if (!oldest)
			return NULL;
		grp_reset(oldest, c->n);
		g = oldest;
	}

	memset(g, 0, sizeof(*g));
	g->gid = gid;
	g->tick = now;
	g->shard = calloc(c->n, sizeof(uint8_t *));
	g->slen = calloc(c->n, sizeof(uint16_t));
	g->olen = calloc(c->n, sizeof(uint16_t));
	if (!g->shard || !g->slen || !g->olen) {
		grp_reset(g, c->n);
		return NULL;
	}
	return g;
}

/* Solve M x = rhs in GF(256), m x m matrix against an m x width rhs.
 * Full Gauss-Jordan: on return M is identity and rhs holds the solution. */
static int gauss_solve(uint8_t *M, uint8_t *rhs, int m, int width)
{
	int col, row, r2, cc, w;

	for (col = 0; col < m; col++) {
		int piv = -1;
		uint8_t inv;

		for (row = col; row < m; row++)
			if (M[row * m + col]) {
				piv = row;
				break;
			}
		if (piv < 0)
			return -1;

		if (piv != col) {
			for (cc = 0; cc < m; cc++) {
				uint8_t t = M[col * m + cc];
				M[col * m + cc] = M[piv * m + cc];
				M[piv * m + cc] = t;
			}
			for (w = 0; w < width; w++) {
				uint8_t t = rhs[col * width + w];
				rhs[col * width + w] = rhs[piv * width + w];
				rhs[piv * width + w] = t;
			}
		}

		inv = ginv(M[col * m + col]);
		for (cc = 0; cc < m; cc++)
			M[col * m + cc] = gmul(M[col * m + cc], inv);
		for (w = 0; w < width; w++)
			rhs[col * width + w] = gmul(rhs[col * width + w], inv);

		for (r2 = 0; r2 < m; r2++) {
			uint8_t f;
			if (r2 == col || !M[r2 * m + col])
				continue;
			f = M[r2 * m + col];
			for (cc = 0; cc < m; cc++)
				M[r2 * m + cc] ^= gmul(f, M[col * m + cc]);
			for (w = 0; w < width; w++)
				rhs[r2 * width + w] ^= gmul(f, rhs[col * width + w]);
		}
	}
	return 0;
}

/* deliver a stored data shard payload, trimming the embedded length prefix */
static void grp_deliver_shard(struct fec_conn *c, struct fec_grp *g, int j,
			      fec_pkt_cb out, void *user)
{
	uint16_t plen;

	(void)c;
	plen = (uint16_t)((g->shard[j][0] << 8) | g->shard[j][1]);
	if (plen > 0 && plen + FEC_LEN_PREFIX <= g->slen[j])
		out(user, (const char *)g->shard[j] + FEC_LEN_PREFIX, plen);
}

/* reconstruct missing data shards and deliver them. On transient
 * failure (not enough parity yet, or allocation failure) the group is
 * left pending so reconstruction is retried when more shards arrive. */
static void grp_finish(struct fec_conn *c, struct fec_grp *g,
		       fec_pkt_cb out, void *user)
{
	int missing[FEC_SHARD_MAX], m = 0;
	int par_rows[FEC_SHARD_MAX], np = 0;
	int j, a, b, w, width = 0;
	uint8_t *M, *rhs;

	for (j = 0; j < c->k; j++) {
		if (g->bitmap & ((uint64_t)1 << j))
			continue;
		missing[m++] = j;
	}
	for (j = c->k; j < c->n; j++)
		if (g->bitmap & ((uint64_t)1 << j)) {
			par_rows[np++] = j - c->k;
			if (g->olen[j] > width)
				width = g->olen[j];	/* parity carries group width */
		}

	if (m == 0)
		return;	/* every data shard arrived on its own */
	if (m > np || width <= 0)
		return;	/* not decodable yet; more parity may still arrive */

	M = malloc((size_t)m * m);
	rhs = malloc((size_t)m * width);
	if (!M || !rhs) {
		free(M);
		free(rhs);
		return;	/* keep the group pending so reconstruction is retried */
	}

	for (a = 0; a < m; a++) {
		int pr = par_rows[a];
		memset(rhs + (size_t)a * width, 0, width);
		for (b = 0; b < m; b++)
			M[a * m + b] = c->coef[pr * c->k + missing[b]];
		/* subtract received data contribution from parity */
		for (j = 0; j < c->k; j++) {
			uint8_t cf;
			if (!(g->bitmap & ((uint64_t)1 << j)))
				continue;
			cf = c->coef[pr * c->k + j];
			if (!cf)
				continue;
			for (w = 0; w < g->slen[j]; w++)
				rhs[(size_t)a * width + w] ^=
					gmul(cf, g->shard[j][w]);
		}
		/* xor in the parity payload itself */
		for (w = 0; w < width; w++)
			rhs[(size_t)a * width + w] ^= g->shard[c->k + pr][w];
	}

	if (gauss_solve(M, rhs, m, width) == 0) {
		c->rx_recovered += m;
		/* reserve all reconstructed buffers first so an allocation
		 * failure leaves the group pending instead of half-delivered */
		int ok = 1;
		for (a = 0; a < m && ok; a++) {
			g->shard[missing[a]] = malloc(width);
			if (!g->shard[missing[a]])
				ok = 0;
		}
		if (ok) {
			for (a = 0; a < m; a++) {
				j = missing[a];
				memcpy(g->shard[j], rhs + (size_t)a * width, width);
				g->slen[j] = (uint16_t)width;
				g->olen[j] = (uint16_t)width;
				g->bitmap |= (uint64_t)1 << j;
				grp_deliver_shard(c, g, j, out, user);
			}
		} else {
			for (a = 0; a < m; a++) {
				j = missing[a];
				if (g->shard[j]) {
					free(g->shard[j]);
					g->shard[j] = NULL;
				}
			}
		}
	}
	free(M);
	free(rhs);
}

void fec_conn_decode(struct fec_conn *c, const char *pkt, int len,
		     fec_pkt_cb out, void *user)
{
	int parity, i;
	uint8_t idx;
	uint16_t gid, size, olen;
	struct fec_grp *g;
	uint32_t now;

	if (!c || !out)
		return;

	if (fec_parse_hdr(pkt, len, &parity, &idx, &gid, &size, &olen) < 0) {
		static int warned = 0;
		if (!warned) {
			fprintf(stderr, "fec: non-FEC packet received, "
				"check that both sides enable fec\n");
			warned = 1;
		}
		return;
	}

	if (idx >= c->n)
		return;

	now = now_ms();
	c->tick = now;
	c->rx_shards++;

	/* drop pending groups that stayed incomplete past the TTL so their
	 * buffers and gid dedup state don't linger until the ring recycles */
	for (i = 0; i < FEC_RX_GROUPS; i++) {
		struct fec_grp *e = &c->grp[i];
		if (e->shard && !e->done && now - e->tick > FEC_GRP_TTL_MS) {
			int j;
			for (j = 0; j < c->n; j++) {
				free(e->shard[j]);
				e->shard[j] = NULL;
			}
			e->bitmap = 0;
			e->cnt = 0;
			e->done = 1;
		}
	}

	g = grp_find(c, gid);
	if (!g) {
		g = grp_create(c, gid, now);
		if (!g)
			return;
	}
	g->tick = now;

	if (g->done || (g->bitmap & ((uint64_t)1 << idx)))
		return;	/* duplicate or already delivered */

	if (!parity)
		olen = size;	/* data: padding equals stored size */

	g->shard[idx] = malloc(size);
	if (!g->shard[idx])
		return;
	memcpy(g->shard[idx], pkt + FEC_HDR_SIZE, size);
	g->slen[idx] = size;
	g->olen[idx] = olen;
	g->bitmap |= (uint64_t)1 << idx;
	g->cnt++;

	/* Data shards are handed upstream immediately on arrival, possibly
	 * reordered: ikcp_input reorders by sn and drops duplicates, so this
	 * avoids head-of-line blocking when a shard is lost mid-group. */
	if (!parity)
		grp_deliver_shard(c, g, idx, out, user);

	if (g->cnt >= c->k) {
		uint64_t mask = (c->k >= 64) ? ~(uint64_t)0
					     : (((uint64_t)1 << c->k) - 1);

		grp_finish(c, g, out, user);
		/* the group is finished once every data shard has been
		 * delivered; otherwise it stays pending for late parity */
		if ((g->bitmap & mask) == mask)
			g->done = 1;
	}
}
