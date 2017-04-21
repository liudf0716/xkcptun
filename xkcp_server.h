#ifndef	_XKCP_SERVER_
#define	_XKCP_SERVER_

#include "ikcp.h"

int server_main_loop();

struct jwHashTable;

struct jwHashTable * get_xkcp_hash();

#endif
