#ifndef	_XKCP_SERVER_
#define	_XKCP_SERVER_

#include "ikcp.h"

int server_main_loop();

typedef struct jwHashTable jwHashTable;

jwHashTable * get_xkcp_hash();

#endif
