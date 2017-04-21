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


#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>

#include <syslog.h>

#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <event2/util.h>

#include "xkcp_server.h"
#include "xkcp_util.h"
#include "tcp_client.h"
#include "debug.h"
#include "ikcp.h"
#include "jwHash.h"

static void clean_useless_client()
{
	jwHashTable *table = get_xkcp_hash();
	for(int i = 0; i < table->buckets; i++) {
		jwHashEntry *entry = table->bucket[i];
		while(entry) {
			jwHashEntry *next = entry->next
			iqueue_head *list = entry->value.ptrValue;
			if (list && iqueue_is_empty(list)) {
				free(entry->value.strValue);
				free(entry->key.strValue);
				free(entry);
			}
			// move to next entry
			entry = next;;
		}
	}
}

void tcp_client_event_cb(struct bufferevent *bev, short what, void *ctx)
{
	void *puser = xkcp_tcp_event_cb(bev, what, ctx);
	if (puser)
		free(puser);
	
	if (what & (BEV_EVENT_EOF|BEV_EVENT_ERROR)) {
		clean_useless_client();
	}
}

void tcp_client_read_cb(struct bufferevent *bev, void *ctx)
{
	struct xkcp_task *task = ctx;
	ikcpcb *kcp = task->kcp;
	xkcp_tcp_read_cb(bev, kcp);
}
