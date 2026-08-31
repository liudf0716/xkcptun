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
#include <strings.h>
#include <syslog.h>

#include <errno.h>

#include "toml.h"
#include "debug.h"
#include "xkcp_config.h"

static struct xkcp_config config;

struct xkcp_config *xkcp_get_config(void)
{
	return &config;
}

struct xkcp_param *xkcp_get_param(void)
{
	return &config.param;
}

void xkcp_param_init(struct xkcp_param *param)
{
	memset(param, 0, sizeof(struct xkcp_param));
	param->name = NULL;
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
	param->loss_ctrl = 1;
	param->pacing = 0;
	param->fec = 0;
	param->sock_buf = 4194304;
	param->keepalive = 10;
	param->conn_timeout = 60;
}

void config_init(void)
{
	memset(&config, 0, sizeof(struct xkcp_config));
	config.daemon = 1;
	config.is_server = 0;
	config.syslog = 0;
	config.mon_port = 0;
	config.num_tunnels = 0;
	config.tunnels = NULL;

	xkcp_param_init(&config.param);
}

void xkcp_apply_mode_param(struct xkcp_param *param)
{
	if (!param || !param->mode)
		return;

	if (!strcasecmp(param->mode, "fast3")) {
		param->nodelay = 1; param->interval = 10; param->resend = 2; param->nc = 1;
	} else if (!strcasecmp(param->mode, "fast2")) {
		param->nodelay = 1; param->interval = 10; param->resend = 2; param->nc = 0;
	} else if (!strcasecmp(param->mode, "fast")) {
		param->nodelay = 0; param->interval = 20; param->resend = 2; param->nc = 0;
	} else if (!strcasecmp(param->mode, "normal")) {
		param->nodelay = 0; param->interval = 30; param->resend = 2; param->nc = 0;
	} else if (!strcasecmp(param->mode, "manual")) {
		/* user-specified custom knobs, do not override */
	} else {
		debug(LOG_ERR, "unknown mode [%s], ignored (valid: fast3/fast2/fast/normal/manual)",
			  param->mode);
	}
}

void xkcp_apply_mode(void)
{
	if (config.num_tunnels > 0 && config.tunnels) {
		for (int i = 0; i < config.num_tunnels; i++) {
			xkcp_apply_mode_param(&config.tunnels[i]);
		}
	} else {
		xkcp_apply_mode_param(&config.param);
	}
}

static void parse_string_opt(toml_table_t *tab, const char *key1, const char *key2, char **dst)
{
	toml_datum_t d = toml_string_in(tab, key1);
	if (!d.ok && key2) d = toml_string_in(tab, key2);
	if (d.ok) {
		if (*dst) free(*dst);
		*dst = d.u.s;
	}
}

static void parse_int_opt(toml_table_t *tab, const char *key1, const char *key2, int *dst)
{
	toml_datum_t d = toml_int_in(tab, key1);
	if (!d.ok && key2) d = toml_int_in(tab, key2);
	if (d.ok) {
		*dst = (int)d.u.i;
		return;
	}
	/* Also check bool in case user used true/false for 1/0 */
	d = toml_bool_in(tab, key1);
	if (!d.ok && key2) d = toml_bool_in(tab, key2);
	if (d.ok) {
		*dst = d.u.b ? 1 : 0;
	}
}

static void parse_tunnel_table(toml_table_t *tab, struct xkcp_param *param)
{
	if (!tab || !param) return;

	parse_string_opt(tab, "name", NULL, &param->name);
	parse_string_opt(tab, "local_interface", "localinterface", &param->local_interface);
	parse_int_opt(tab, "local_port", "localport", &param->local_port);
	parse_string_opt(tab, "remote_addr", "remoteaddr", &param->remote_addr);
	parse_int_opt(tab, "remote_port", "remoteport", &param->remote_port);
	parse_string_opt(tab, "key", NULL, &param->key);
	parse_string_opt(tab, "crypt", NULL, &param->crypt);
	parse_string_opt(tab, "mode", NULL, &param->mode);

	parse_int_opt(tab, "conn", NULL, &param->conn);
	parse_int_opt(tab, "auto_expire", "autoexpire", &param->auto_expire);
	parse_int_opt(tab, "scavenge_ttl", "scavengettl", &param->scavenge_ttl);
	parse_int_opt(tab, "mtu", NULL, &param->mtu);
	parse_int_opt(tab, "sndwnd", NULL, &param->sndwnd);
	parse_int_opt(tab, "rcvwnd", NULL, &param->rcvwnd);
	parse_int_opt(tab, "data_shard", "datashard", &param->data_shard);
	parse_int_opt(tab, "parity_shard", "parityshard", &param->parity_shard);
	parse_int_opt(tab, "dscp", NULL, &param->dscp);
	parse_int_opt(tab, "nocomp", NULL, &param->nocomp);
	parse_int_opt(tab, "ack_nodelay", "acknodelay", &param->ack_nodelay);
	parse_int_opt(tab, "nodelay", NULL, &param->nodelay);
	parse_int_opt(tab, "interval", NULL, &param->interval);
	parse_int_opt(tab, "resend", NULL, &param->resend);
	parse_int_opt(tab, "nc", NULL, &param->nc);
	parse_int_opt(tab, "loss_ctrl", "lossctrl", &param->loss_ctrl);
	parse_int_opt(tab, "pacing", NULL, &param->pacing);
	parse_int_opt(tab, "fec", NULL, &param->fec);
	parse_int_opt(tab, "sock_buf", "sockbuf", &param->sock_buf);
	parse_int_opt(tab, "keepalive", NULL, &param->keepalive);
	parse_int_opt(tab, "conn_timeout", "conntimeout", &param->conn_timeout);

	xkcp_apply_mode_param(param);
}

int xkcp_parse_param(const char *filename)
{
	if (!filename) return 1;

	FILE *fp = fopen(filename, "r");
	if (!fp) {
		debug(LOG_ERR, "Open config file [%s] failed: %s", filename, strerror(errno));
		return 1;
	}

	char errbuf[256] = {0};
	toml_table_t *root = toml_parse_file(fp, errbuf, sizeof(errbuf));
	fclose(fp);

	if (!root) {
		debug(LOG_ERR, "TOML parse [%s] error: %s", filename, errbuf);
		return 1;
	}

	/* 1. Parse [global] section if present */
	toml_table_t *global_tab = toml_table_in(root, "global");
	if (global_tab) {
		parse_int_opt(global_tab, "syslog", NULL, &config.syslog);
		parse_int_opt(global_tab, "mon_port", "monport", &config.mon_port);
		int dbglvl = -1;
		parse_int_opt(global_tab, "debug", NULL, &dbglvl);
		if (dbglvl >= 0) debugconf.debuglevel = dbglvl;
	}

	/* 2. Check for [[tunnel]] or [[client]] or [[server]] array of tables */
	toml_array_t *tunnels_arr = toml_array_in(root, "tunnel");
	if (!tunnels_arr) tunnels_arr = toml_array_in(root, "tunnels");
	if (!tunnels_arr) tunnels_arr = toml_array_in(root, config.is_server ? "server" : "client");

	if (tunnels_arr && toml_array_nelem(tunnels_arr) > 0) {
		int n = toml_array_nelem(tunnels_arr);
		config.num_tunnels = n;
		config.tunnels = calloc(n, sizeof(struct xkcp_param));
		if (!config.tunnels) {
			debug(LOG_ERR, "Memory allocation failed for %d tunnels", n);
			toml_free(root);
			return 1;
		}
		for (int i = 0; i < n; i++) {
			xkcp_param_init(&config.tunnels[i]);
			toml_table_t *tt = toml_table_at(tunnels_arr, i);
			parse_tunnel_table(tt, &config.tunnels[i]);
			if (!config.tunnels[i].name) {
				char auto_name[32];
				snprintf(auto_name, sizeof(auto_name), "tunnel_%d", i + 1);
				config.tunnels[i].name = strdup(auto_name);
			}
		}
		/* For backwards compatibility, point single param to first tunnel */
		config.param = config.tunnels[0];
	} else {
		/* 3. Fallback: Parse root table directly as single-tunnel configuration */
		parse_tunnel_table(root, &config.param);
		config.num_tunnels = 1;
		config.tunnels = calloc(1, sizeof(struct xkcp_param));
		if (config.tunnels) {
			config.tunnels[0] = config.param;
			if (!config.tunnels[0].name)
				config.tunnels[0].name = strdup("default");
		}
	}

	toml_free(root);
	return 0;
}

void xkcp_free_config(void)
{
	if (config.config_file) {
		free(config.config_file);
		config.config_file = NULL;
	}
	if (config.tunnels && config.tunnels != &config.param) {
		for (int i = 0; i < config.num_tunnels; i++) {
			if (config.tunnels[i].name) free(config.tunnels[i].name);
			if (config.tunnels[i].local_interface) free(config.tunnels[i].local_interface);
			if (config.tunnels[i].remote_addr) free(config.tunnels[i].remote_addr);
			if (config.tunnels[i].key) free(config.tunnels[i].key);
			if (config.tunnels[i].crypt) free(config.tunnels[i].crypt);
			if (config.tunnels[i].mode) free(config.tunnels[i].mode);
		}
		free(config.tunnels);
		config.tunnels = NULL;
	}
}
