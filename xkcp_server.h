#ifndef	_XKCP_SERVER_
#define	_XKCP_SERVER_

#include "ikcp.h"

int server_main_loop();

iqueue_head * get_xkcp_task_list();

#endif
