/*
 * Unit tests for the FEC module: encode groups, erase shards in every
 * possible pattern (exhaustive for small sizes, random for large ones),
 * optionally shuffle and duplicate the survivors, then verify that the
 * decoder reconstructs the original packets.
 *
 * Build: see CMakeLists.txt target test_fec
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fec.h"

struct pktbuf {
	char *data;
	int len;
};

struct caplist {
	struct pktbuf *pkts;
	int n, cap;
};

static void cap_add(void *arg, const char *pkt, int len)
{
	struct caplist *c = arg;
	if (c->n == c->cap) {
		c->cap = c->cap ? c->cap * 2 : 64;
		c->pkts = realloc(c->pkts, (size_t)c->cap * sizeof(struct pktbuf));
	}
	c->pkts[c->n].data = malloc(len);
	memcpy(c->pkts[c->n].data, pkt, len);
	c->pkts[c->n].len = len;
	c->n++;
}

static void cap_free(struct caplist *c)
{
	int i;
	for (i = 0; i < c->n; i++)
		free(c->pkts[i].data);
	free(c->pkts);
	memset(c, 0, sizeof(*c));
}

struct outlist {
	char **data;
	int *lens;
	int n;
	int expect;
};

static void out_collect(void *arg, const char *pkt, int len)
{
	struct outlist *o = arg;
	if (o->n < o->expect) {
		o->data[o->n] = malloc(len);
		memcpy(o->data[o->n], pkt, len);
		o->lens[o->n] = len;
	}
	o->n++;
}

static unsigned int rnd_state = 12345;

static unsigned int rnd(void)
{
	rnd_state = rnd_state * 1103515245 + 12345;
	return rnd_state >> 16;
}

/* test one group: k originals -> sent packets -> erase subset -> decode */
static int run_case(int k, int r, uint64_t erase_mask, int shuffle, int dup,
		    int payload_max)
{
	struct fec_conn *enc, *dec;
	struct caplist sent = {0}, surv = {0};
	struct outlist out;
	char **orig;
	int *olen;
	int i, j, fail = 0;

	orig = calloc(k, sizeof(char *));
	olen = calloc(k, sizeof(int));
	for (i = 0; i < k; i++) {
		olen[i] = (int)(rnd() % (unsigned int)payload_max) + 1;
		orig[i] = malloc(olen[i]);
		for (j = 0; j < olen[i]; j++)
			orig[i][j] = (char)(rnd() & 0xff);
	}

	enc = fec_conn_new(k, r, payload_max);
	dec = fec_conn_new(k, r, payload_max);
	if (!enc || !dec) {
		fprintf(stderr, "fec_conn_new failed k=%d r=%d\n", k, r);
		return 1;
	}

	for (i = 0; i < k; i++)
		fec_conn_encode(enc, orig[i], olen[i], cap_add, &sent);

	/* survivors = sent minus erased */
	for (i = 0; i < sent.n; i++)
		if (!((erase_mask >> i) & 1u))
			cap_add(&surv, sent.pkts[i].data, sent.pkts[i].len);

	/* optional duplication of even-indexed survivors */
	if (dup)
		for (i = 0; i < surv.n; i += 2)
			cap_add(&surv, surv.pkts[i].data, surv.pkts[i].len);

	/* optional shuffle */
	if (shuffle)
		for (i = surv.n - 1; i > 0; i--) {
			j = (int)(rnd() % (unsigned int)(i + 1));
			struct pktbuf t = surv.pkts[i];
			surv.pkts[i] = surv.pkts[j];
			surv.pkts[j] = t;
		}

	memset(&out, 0, sizeof(out));
	out.expect = k;
	out.data = calloc(k, sizeof(char *));
	out.lens = calloc(k, sizeof(int));

	for (i = 0; i < surv.n; i++)
		fec_conn_decode(dec, surv.pkts[i].data, surv.pkts[i].len,
				out_collect, &out);

	if (out.n != k) {
		fail = 1;
		fprintf(stderr, "FAIL k=%d r=%d mask=%llx: delivered %d pkts, want %d\n",
			k, r, (unsigned long long)erase_mask, out.n, k);
	} else {
		for (i = 0; i < k && !fail; i++)
			if (out.lens[i] != olen[i] ||
			    memcmp(out.data[i], orig[i], olen[i]) != 0) {
				fail = 1;
				fprintf(stderr, "FAIL k=%d r=%d mask=%llx: shard %d mismatch\n",
					k, r, (unsigned long long)erase_mask, i);
			}
	}

	for (i = 0; i < k; i++) {
		free(orig[i]);
		free(out.data[i]);
	}
	free(orig);
	free(olen);
	free(out.data);
	free(out.lens);
	cap_free(&sent);
	cap_free(&surv);
	fec_conn_free(enc);
	fec_conn_free(dec);
	return fail;
}

int main(void)
{
	int k, r, fails = 0;
	uint64_t mask;
	long cases = 0;

	/* exhaustive erasure patterns for small groups:
	 * every subset of the k+r sent packets is a candidate erasure set;
	 * only subsets with popcount <= r must be recoverable. */
	for (k = 1; k <= 10; k++) {
		for (r = 0; r <= 4; r++) {
			int n = k + r;
			uint32_t limit = 1u << n;
			for (mask = 0; mask < limit; mask++) {
				int erased = __builtin_popcount(mask);
				if (erased > r)
					continue;
		fails += run_case(k, r, (uint64_t)mask,
				  (mask & 0x100) != 0,
				  (mask & 0x200) != 0,
						  200 + (k * 37 + r * 11) % 100);
				cases++;
			}
		}
	}

	/* random larger configurations */
	{
		int trial;
		for (trial = 0; trial < 300; trial++) {
			k = 20 + (int)(rnd() % 40);	/* up to ~59 */
			r = 1 + (int)(rnd() % 5);
			mask = 0;
		{
			int pos[128], i, erased = 0, t = (int)(rnd() % (unsigned int)r) + 1;
			for (i = 0; i < k + r; i++)
				pos[i] = i;
			for (i = 0; i < t; i++) {
				int pick = (int)(rnd() % (unsigned int)(k + r - i)) + i;
				int tmp = pos[i]; pos[i] = pos[pick]; pos[pick] = tmp;
				mask |= (uint64_t)1 << pos[i];
				erased++;
			}
				if (erased > r)
					continue;
			}
			fails += run_case(k, r, mask, 1, 1, 1400);
			cases++;
		}
	}

	printf("fec tests: %ld cases, %d failures\n", cases, fails);
	return fails ? 1 : 0;
}
