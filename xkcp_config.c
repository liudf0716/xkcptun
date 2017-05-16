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
#include <stdio.h>
#include <stdlib.h>

#include <syslog.h>
#include <json-c/json.h>

#include "debug.h"
#include "xkcp_config.h"

static struct xkcp_config config;

struct xkcp_config * xkcp_get_config(void)
{
	return &config;
}

struct xkcp_param *xkcp_get_param(void)
{
	return &config.param;
}

static void xkcp_param_init(struct xkcp_param *param)
{
	memset(&config.param, 0, sizeof(struct xkcp_param));
	param->local_interface = NULL;
	param->remote_addr = NULL;
	param->key = NULL;
	param->crypt = NULL;
	param->mode = NULL;

	param->mtu = 1350;
	param->sndwnd = 512;
	param->rcvwnd = 512;
	param->data_shard = 10;
	param->parity_shard = 3;
	param->dscp = 0;
	param->nocomp = 1;
	param->ack_nodelay = 0;
	param->nodelay = 1;
	param->interval = 20;
	param->resend = 2;
	param->nc = 0;
	param->sock_buf = 4194304;
	param->keepalive = 10;
}

void config_init()
{
	config.daemon 		= 1;
	config.is_server 	= 0;

	xkcp_param_init(&config.param);
}

static void xkcp_param_free(struct xkcp_param *param)
{
	if (param->local_interface)
		free(param->local_interface);

	if (param->remote_addr)
		free(param->remote_addr);

	if (param->key)
		free(param->key);

	if (param->crypt)
		free(param->crypt);

	if (param->mode)
		free(param->mode);
}

int xkcp_parse_param(const char *filename)
{
	return xkcp_parse_json_param(&config.param, filename);
}

// 1: error; 0, success
int xkcp_parse_json_param(struct xkcp_param *param, const char *filename)
{
	if (!param)
		return 1;

	int nret = 0;
	struct json_object *json_config = json_object_from_file(filename);
	if (!json_config || !json_object_is_type(json_config, json_type_object)) {
		debug(LOG_ERR, "json_object_from_file [%s] failed", filename);
		return 1;
	}

	struct json_object *j_obj = NULL;
	if (json_object_object_get_ex(json_config, "localinterface", &j_obj)) {
		param->local_interface = strdup(json_object_get_string(j_obj));
	} else
		param->local_interface = strdup("br-lan");
	debug(LOG_DEBUG, "local_interface is %s", param->local_interface);

	if (json_object_object_get_ex(json_config, "localport", &j_obj)) {
		param->local_port = json_object_get_int(j_obj);
	} else
		param->local_port = 9088;
	debug(LOG_DEBUG, "local_port is %d", param->local_port);

	if (json_object_object_get_ex(json_config, "remoteaddr", &j_obj)) {
		param->remote_addr = strdup(json_object_get_string(j_obj));
		debug(LOG_DEBUG, "remote_addr is %s", param->remote_addr);
	}

	if (json_object_object_get_ex(json_config, "remoteport", &j_obj)) {
		param->remote_port = json_object_get_int(j_obj);
	} else
		param->remote_port = 9089;

	debug(LOG_DEBUG, "remote_port is %d", param->remote_port);

	if (json_object_object_get_ex(json_config, "key", &j_obj)) {
		param->key = strdup(json_object_get_string(j_obj));
	}

	if (json_object_object_get_ex(json_config, "crypt", &j_obj)) {
		param->crypt = strdup(json_object_get_string(j_obj));
	}

	if (json_object_object_get_ex(json_config, "mode", &j_obj)) {
		param->mode = strdup(json_object_get_string(j_obj));
	}

	if (json_object_object_get_ex(json_config, "conn", &j_obj)) {
		param->conn = json_object_get_int(j_obj);
	}

	if (json_object_object_get_ex(json_config, "autoexpire", &j_obj)) {
		param->auto_expire = json_object_get_int(j_obj);
	}

	if (json_object_object_get_ex(json_config, "scavengettl", &j_obj)) {
		param->scavenge_ttl = json_object_get_int(j_obj);
	}

	if (json_object_object_get_ex(json_config, "mtu", &j_obj)) {
		param->mtu = json_object_get_int(j_obj);
		debug(LOG_DEBUG, "mtu is %d", param->mtu);
	}


	if (json_object_object_get_ex(json_config, "sndwnd", &j_obj)) {
		param->sndwnd = json_object_get_int(j_obj);
		debug(LOG_DEBUG, "sndwnd is %d", param->sndwnd);
	}


	if (json_object_object_get_ex(json_config, "rcvwnd", &j_obj)) {
		param->rcvwnd = json_object_get_int(j_obj);
		debug(LOG_DEBUG, "rcvwnd is %d", param->rcvwnd);
	}

	if (json_object_object_get_ex(json_config, "datashard", &j_obj)) {
		param->data_shard = json_object_get_int(j_obj);
	}

	if (json_object_object_get_ex(json_config, "parity_shard", &j_obj)) {
		param->parity_shard = json_object_get_int(j_obj);
	}

	if (json_object_object_get_ex(json_config, "dscp", &j_obj)) {
		param->dscp = json_object_get_int(j_obj);
	}

	if (json_object_object_get_ex(json_config, "nocomp", &j_obj)) {
		param->nocomp = json_object_get_int(j_obj);
	}

	if (json_object_object_get_ex(json_config, "acknodelay", &j_obj)) {
		param->ack_nodelay = json_object_get_int(j_obj);
	}

	if (json_object_object_get_ex(json_config, "nodelay", &j_obj)) {
		param->nodelay = json_object_get_int(j_obj);
	}

	if (json_object_object_get_ex(json_config, "interval", &j_obj)) {
		param->interval = json_object_get_int(j_obj);
		debug(LOG_DEBUG, "interval is %d", param->interval);
	}

	if (json_object_object_get_ex(json_config, "resend", &j_obj)) {
		param->resend = json_object_get_int(j_obj);
		debug(LOG_DEBUG, "resend is %d", param->resend);
	}

	if (json_object_object_get_ex(json_config, "nc", &j_obj)) {
		param->nc = json_object_get_int(j_obj);
		debug(LOG_DEBUG, "nc is %d", param->nc);
	}

	if (json_object_object_get_ex(json_config, "sockbuf", &j_obj)) {
		param->sock_buf = json_object_get_int(j_obj);
	}

	if (json_object_object_get_ex(json_config, "keepalive", &j_obj)) {
		param->keepalive = json_object_get_int(j_obj);
	}

	debug(LOG_DEBUG, "keepalive is %d", param->keepalive);

err:
	json_object_put(json_config);
	if (nret) xkcp_param_free(param);
	return nret;
}
