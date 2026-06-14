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
#include <stddef.h>

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

enum config_type { CFG_STR, CFG_INT };

struct config_entry {
	const char *name;
	enum config_type type;
	size_t offset;
};

static struct config_entry config_table[] = {
	{"localinterface", CFG_STR,  offsetof(struct xkcp_param, local_interface)},
	{"localport",      CFG_INT,  offsetof(struct xkcp_param, local_port)},
	{"remoteaddr",     CFG_STR,  offsetof(struct xkcp_param, remote_addr)},
	{"remoteport",     CFG_INT,  offsetof(struct xkcp_param, remote_port)},
	{"key",            CFG_STR,  offsetof(struct xkcp_param, key)},
	{"crypt",          CFG_STR,  offsetof(struct xkcp_param, crypt)},
	{"mode",           CFG_STR,  offsetof(struct xkcp_param, mode)},
	{"conn",           CFG_INT,  offsetof(struct xkcp_param, conn)},
	{"autoexpire",     CFG_INT,  offsetof(struct xkcp_param, auto_expire)},
	{"scavengettl",    CFG_INT,  offsetof(struct xkcp_param, scavenge_ttl)},
	{"mtu",            CFG_INT,  offsetof(struct xkcp_param, mtu)},
	{"sndwnd",         CFG_INT,  offsetof(struct xkcp_param, sndwnd)},
	{"rcvwnd",         CFG_INT,  offsetof(struct xkcp_param, rcvwnd)},
	{"datashard",      CFG_INT,  offsetof(struct xkcp_param, data_shard)},
	{"parity_shard",   CFG_INT,  offsetof(struct xkcp_param, parity_shard)},
	{"dscp",           CFG_INT,  offsetof(struct xkcp_param, dscp)},
	{"nocomp",         CFG_INT,  offsetof(struct xkcp_param, nocomp)},
	{"acknodelay",     CFG_INT,  offsetof(struct xkcp_param, ack_nodelay)},
	{"nodelay",        CFG_INT,  offsetof(struct xkcp_param, nodelay)},
	{"interval",       CFG_INT,  offsetof(struct xkcp_param, interval)},
	{"resend",         CFG_INT,  offsetof(struct xkcp_param, resend)},
	{"nc",             CFG_INT,  offsetof(struct xkcp_param, nc)},
	{"keepalive",      CFG_INT,  offsetof(struct xkcp_param, keepalive)},
	{NULL, 0, 0}
};

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
		fclose(f);
		return 1;
	}

	int nread = fread(buf, pos, 1, f);
	fclose(f);
	if (!nread) {
		debug(LOG_ERR, "Failed to read the config file.");
		free(buf);
		return 1;
	}

	buf[pos] = '\0'; // end of string

	json_settings settings = { 0UL, 0, NULL, NULL, NULL };
	char error_buf[512];
	obj = json_parse_ex(&settings, buf, pos, error_buf);

	if (obj == NULL) {
		debug(LOG_ERR, "%s", error_buf);
		free(buf);
		return 1;
	}

	if (obj->type == json_object) {
		unsigned int i;
		for (i = 0; i < obj->u.object.length; i++) {
			char *name		= obj->u.object.values[i].name;
			json_value *value = obj->u.object.values[i].value;
			struct config_entry *e;
			for (e = config_table; e->name != NULL; e++) {
				if (strcmp(name, e->name) == 0) {
					if (e->type == CFG_STR) {
						*(char **)((char *)param + e->offset) = parse_json_string(value);
					} else {
						*(int *)((char *)param + e->offset) = parse_json_int(value);
					}
					debug(LOG_DEBUG, "config %s set", e->name);
					break;
				}
			}
		}
	} else {
		debug(LOG_DEBUG, "Invalid config file");
		free(buf);
		json_value_free(obj);
		return 1;
	}

	free(buf);
	json_value_free(obj);

	return 0;
}
