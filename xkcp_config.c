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

#include "json.h"
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

#if 0
void xkcp_param_free(struct xkcp_param *param)
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
#endif

static int parse_json_int(const json_value *value)
{
	if (value->type == json_integer) {
		return value->u.integer;
	} else if (value->type == json_boolean) {
		return value->u.boolean;
	} else {
		debug(LOG_ERR, "Need type :%d or %d, now get wrong type: %d", json_integer, json_boolean, value->type);
		debug(LOG_ERR, "Invalid config format.");
	}
	return 0;
}

static char * parse_json_string(const json_value *value)
{
	if (value->type == json_string) {
		return strdup(value->u.string.ptr);
	} else if (value->type == json_null) {
		return NULL;
	} else {
		debug(LOG_ERR, "Need type :%d or %d, now get wrong type: %d", json_string, json_null, value->type);
		debug(LOG_ERR, "Invalid config format.");
	}
	return 0;
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

	char * buf;
	json_value *obj;

	FILE *f = fopen(filename, "rb");
	if (f == NULL) {
		debug(LOG_ERR, "Invalid config path.");
		return 1;
	}

	fseek(f, 0, SEEK_END);
	long pos = ftell(f);
	fseek(f, 0, SEEK_SET);

	buf = malloc(pos + 1);
	if (buf == NULL) {
		debug(LOG_ERR, "No enough memory.");
		return 1;
	}

	int nread = fread(buf, pos, 1, f);
	if (!nread) {
		debug(LOG_ERR, "Failed to read the config file.");
		return 1;
	}
	fclose(f);

	buf[pos] = '\0'; // end of string

	json_settings settings = { 0UL, 0, NULL, NULL, NULL };
	char error_buf[512];
	obj = json_parse_ex(&settings, buf, pos, error_buf);

	if (obj == NULL) {
		debug(LOG_ERR, "%s", error_buf);
		return 1;
	}

	if (obj->type == json_object) {
		unsigned int i;
		for (i = 0; i < obj->u.object.length; i++) {
			char *name		= obj->u.object.values[i].name;
			json_value *value = obj->u.object.values[i].value;
			if (strcmp(name, "localinterface") == 0) {
				param->local_interface = parse_json_string(value);
				debug(LOG_DEBUG, "local_interface is %s", param->local_interface);
			} else if (strcmp(name, "localport") == 0) {
				param->local_port = parse_json_int(value);;
				debug(LOG_DEBUG, "local_port is %d", param->local_port);
			} else if (strcmp(name, "remoteaddr") == 0) {
				param->remote_addr = parse_json_string(value);
				debug(LOG_DEBUG, "remote_addr is %s", param->remote_addr);
			} else if (strcmp(name, "remoteport") == 0) {
				param->remote_port = parse_json_int(value);;
				debug(LOG_DEBUG, "remote_port is %d", param->remote_port);
			} else if (strcmp(name, "key") == 0) {
				param->key = parse_json_string(value);
			} else if (strcmp(name, "crypt") == 0) {
				param->crypt = parse_json_string(value);
			} else if (strcmp(name, "mode") == 0) {
				param->mode = parse_json_string(value);
			} else if (strcmp(name, "conn") == 0) {
				param->conn = parse_json_int(value);;
			} else if (strcmp(name, "autoexpire") == 0) {
				param->auto_expire = parse_json_int(value);;
			} else if (strcmp(name, "scavengettl") == 0) {
				param->scavenge_ttl = parse_json_int(value);;
			} else if (strcmp(name, "mtu") == 0) {
				param->mtu = parse_json_int(value);;
			} else if (strcmp(name, "sndwnd") == 0) {
				param->sndwnd = parse_json_int(value);;
			} else if (strcmp(name, "rcvwnd") == 0) {
				param->rcvwnd = parse_json_int(value);;
			} else if (strcmp(name, "datashard") == 0) {
				param->data_shard = parse_json_int(value);;
			} else if (strcmp(name, "parity_shard") == 0) {
				param->parity_shard = parse_json_int(value);;
			} else if (strcmp(name, "dscp") == 0) {
				param->dscp = parse_json_int(value);;
			} else if (strcmp(name, "nocomp") == 0) {
				param->nocomp = parse_json_int(value);;
			} else if (strcmp(name, "acknodelay") == 0) {
				param->ack_nodelay = parse_json_int(value);;
			} else if (strcmp(name, "nodelay") == 0) {
				param->nodelay = parse_json_int(value);;
			} else if (strcmp(name, "interval") == 0) {
				param->interval = parse_json_int(value);;
			} else if (strcmp(name, "resend") == 0) {
				param->resend = parse_json_int(value);;
			} else if (strcmp(name, "nc") == 0) {
				param->nc = parse_json_int(value);;
			} else if (strcmp(name, "keepalive") == 0) {
				param->keepalive = parse_json_int(value);;
			}
		}
	} else {
		debug(LOG_DEBUG, "Invalid config file");
		return 1;
	}

	free(buf);
	json_value_free(obj);

	return 0;
}
