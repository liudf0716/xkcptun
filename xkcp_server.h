#ifndef	_XKCP_SERVER_
#define	_XKCP_SERVER_

#include "ikcp.h"

int server_main_loop();

struct fec_conn;

struct jwHashTable * get_xkcp_hash();

/* Drop and free the per-peer FEC codec registered under key "ip:port".
 * No-op when FEC is disabled or no codec exists for the key. */
void xkcp_server_drop_peer_fec(const char *key);

/* Clean up empty task lists and associated hash entries in xkcp_hash */
void clean_useless_client(void);

#endif
