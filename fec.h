/*
 * FEC - forward error correction layer for xkcptun
 *
 * Wraps every UDP datagram in a small self-describing frame and adds
 * Reed-Solomon parity packets so the receiver can reconstruct lost
 * packets without a retransmission round-trip.
 *
 * Wire format (8-byte header, big-endian fields):
 *   byte 0    : version<<4 | flags   (bit0: 1 = parity shard)
 *   byte 1    : shard index inside the group
 *   bytes 2-3 : group id
 *   bytes 4-5 : payload size (bytes following the header)
 *   bytes 6-7 : original length before zero-padding
 *               (data shards: real packet len; parity shards: group width)
 */
#ifndef _FEC_H_
#define _FEC_H_

#include <stdint.h>

#define FEC_VERSION		1
#define FEC_HDR_SIZE	8

/* max datashard/parityshard per group */
#define FEC_SHARD_MAX	64
/* decoder pending-group ring size */
#define FEC_RX_GROUPS	64
/* pending groups older than this are dropped */
#define FEC_GRP_TTL_MS	3000
/* a partial tx group older than this gets its parity flushed so small
 * interactive flows get loss protection without filling the group */
#define FEC_PARTIAL_FLUSH_MS	100

struct fec_conn;

/* Create a fec connection. datashard in [1..64], parityshard in [0..64].
 * shard_cap is the max KCP segment size to expect (use the configured mtu). */
struct fec_conn *fec_conn_new(int datashard, int parityshard, int shard_cap);

void fec_conn_free(struct fec_conn *c);

/* milliseconds since the codec last saw a packet (idle detection) */
uint32_t fec_conn_idle_ms(const struct fec_conn *c);

/* Called once per outgoing UDP datagram. The buffers are only valid for
 * the duration of the call. */
typedef void (*fec_pkt_cb)(void *user, const char *pkt, int len);

/* Feed one outgoing KCP segment; out is invoked with one or more framed
 * datagrams to send. Never adds latency: data shards are emitted as soon
 * as they arrive, parity follows immediately after the group completes.
 * Groups left incomplete are sent unprotected (like plain UDP). */
void fec_conn_encode(struct fec_conn *c, const char *data, int len,
		     fec_pkt_cb out, void *user);

/* Feed one received UDP datagram; out is invoked once per recovered raw
 * KCP packet. Data shards are delivered immediately on arrival and may be
 * reordered; only lost shards wait for parity-based reconstruction.
 * ikcp_input reorders by sn and drops duplicates, so this is transparent
 * upstream while avoiding head-of-line blocking. */
void fec_conn_decode(struct fec_conn *c, const char *pkt, int len,
		     fec_pkt_cb out, void *user);

/* Call periodically (every timer tick). Two jobs:
 *  - if the current tx group is partially filled and older than
 *    FEC_PARTIAL_FLUSH_MS, emit its parity immediately and close it;
 *  - re-estimate the parity ratio from the observed post-FEC recovery
 *    rate (only when the ratio is below the configured parityshard). */
void fec_conn_tick(struct fec_conn *c, fec_pkt_cb out, void *user);

#endif
