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

/** @file commandline.c
	@brief Command line argument handling
	@author Copyright (C) 2004 Philippe April <papril777@yahoo.com>
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <syslog.h>
#include <getopt.h>

#include "xkcp_config.h"
#include "commandline.h"
#include "debug.h"
#include "version.h"

static struct option long_options[] = {
	{ "key",		required_argument,	NULL, GETOPT_VAL_KEY},
	{ "crypt",		required_argument,	NULL, GETOPT_VAL_CRYPT},
	{ "mode",		required_argument,	NULL, GETOPT_VAL_MODE},
	{ "mtu",		required_argument,	NULL, GETOPT_VAL_MTU},
	{ "sndwnd",		required_argument,	NULL, GETOPT_VAL_SNDWND},
	{ "rcvwnd",		required_argument,	NULL, GETOPT_VAL_RCVWND},
	{ "dshard",		required_argument,	NULL, GETOPT_VAL_DATASHARD},
	{ "pshard",		required_argument,	NULL, GETOPT_VAL_PARITYSHARD},
	{ "dscp",		required_argument,	NULL, GETOPT_VAL_DSCP},
	{ "nocomp",		no_argument,		NULL, GETOPT_VAL_NOCOMP},
	{ "acknodelay",	no_argument,		NULL, GETOPT_VAL_ACKNODELAY},
	{ "nodelay",	no_argument,		NULL, GETOPT_VAL_NODELAY},
	{ "interval",	required_argument,	NULL, GETOPT_VAL_INTERVAL},
	{ "resend",		required_argument,	NULL, GETOPT_VAL_RESEND},
	{ "nc",			required_argument,	NULL, GETOPT_VAL_NC},
	{ "sockbuf",	required_argument,	NULL, GETOPT_VAL_SOCKBUF},
	{ "keepalive",	required_argument,	NULL, GETOPT_VAL_KEEPALIVE},
	{ "conntimeout",required_argument,	NULL, GETOPT_VAL_CONN_TIMEOUT},
	{ "syslog",		no_argument,		NULL, GETOPT_VAL_SYSLOG},
	{ "help",		no_argument,		NULL, GETOPT_VAL_HELP},
	{ "version",	no_argument,		NULL, GETOPT_VAL_VERSION},
	{ NULL,			0,					NULL, 0 }
};

/** @internal
 * @brief Print usage
 *
 * Prints usage
 */
void
usage(const char *appname)
{
	fprintf(stdout, "Usage: %s [options]\n", appname);
	fprintf(stdout, "\n");
	fprintf(stdout, "options:\n");
	fprintf(stdout, "  -c <filename>     Use this config file (TOML)\n");
	fprintf(stdout, "  -f                Run in foreground\n");
	fprintf(stdout, "  -h --help         Print usage\n");
	fprintf(stdout, "  -v --version      Print version information\n");
	fprintf(stdout, "  -d <level>        Debug level\n");
	fprintf(stdout, "  --syslog          Log to syslog\n\n");

	fprintf(stdout, "  -i <interface>    Interface to use (default: any/0.0.0.0)\n");
	fprintf(stdout, "  -l <port>         Port number of your local server (default: 9089 server / 0 client)\n");
	fprintf(stdout, "  -s <host>         Host name or IP address of your remote server\n");
	fprintf(stdout, "  -p <port>         Port number of your remote server (default: 9089)\n");
	fprintf(stdout, "  -k <string>       Pre-shared key for tunnel authentication\n");
	fprintf(stdout, "                    (must match on client and server; default: \"it's a secret\")\n");
	fprintf(stdout, "  -e <string>       (reserved, not implemented)\n");
	fprintf(stdout, "  -m <string>       Profiles: fast3, fast2, fast, normal (default: \"fast3\")\n");
	fprintf(stdout, "  -M --mtu <num>    MTU of your network interface\n");
	fprintf(stdout, "  -S --sndwnd <num> Send window size(num of packets) (default: 512)\n");
	fprintf(stdout, "  -R --rcvwnd <num> Receive window size(num of packets) (default: 512)\n");
	fprintf(stdout, "  -D --dshard <num> Reed-solomon erasure coding - datashard (default: 10)\n");
	fprintf(stdout, "  -P --pshard <num> Reed-solomon erasure coding - parityshard (default: 3)\n");
	fprintf(stdout, "  -N --nocomp       Disable compression\n");
	fprintf(stdout, "  -A --acknodelay   Ack no delay\n");
	fprintf(stdout, "  -L --nodelay      No delay\n");
	fprintf(stdout, "  -T --interval <num>\n");
	fprintf(stdout, "  -K --keepalive <num>\n");
	fprintf(stdout, "  --conntimeout <num> Connection timeout in seconds (default: 60)\n");

	fprintf(stdout, "\n");
}

/** Uses getopt() to parse the command line and set configuration values
 * also populates restartargv
 */
void
parse_commandline(int argc, char **argv)
{
	int c;
	struct xkcp_config *config = xkcp_get_config();
	struct xkcp_param *param = &config->param;
	const char *optstring = "Ac:D:d:e:fhi:K:k:L:l:M:m:NP:p:R:S:s:T:v";

	/* Pre-pass: load the config file first so command-line options,
	 * parsed below, take precedence over values from the file. */
	opterr = 0;
	optind = 1;
	while (-1 != (c = getopt_long(argc, argv, optstring, long_options, NULL)))
		if (c == 'c' && optarg) {
			free(config->config_file);
			config->config_file = strdup(optarg);
		}

	if (config->config_file &&
	    xkcp_parse_param(config->config_file) != 0) {
		debug(LOG_ERR, "xkcp_parse_param failed \n");
		usage(argv[0]);
		exit(1);
	}

	opterr = 1;
	optind = 1;
	while (-1 != (c = getopt_long(argc, argv, optstring, long_options, NULL)))
		switch (c) {

		case GETOPT_VAL_HELP:
		case 'h':
			usage(argv[0]);
			exit(1);
			break;

		case 'c':
			if (optarg) {
				free(config->config_file);
				config->config_file = strdup(optarg);
			}
			break;

		case 'f':
			config->daemon = 0;
			debugconf.log_stderr = 1;
			break;

		case 'd':
			if (optarg) {
				debugconf.debuglevel = atoi(optarg);
			}
			break;

#define OVERRIDE_PARAM_STR(field, val) do { \
		free(param->field); \
		param->field = strdup(val); \
		if (config->num_tunnels > 0 && config->tunnels) { \
			for (int _t = 0; _t < config->num_tunnels; _t++) { \
				free(config->tunnels[_t].field); \
				config->tunnels[_t].field = strdup(val); \
			} \
		} \
	} while (0)

#define OVERRIDE_PARAM_INT(field, val) do { \
		param->field = (val); \
		if (config->num_tunnels > 0 && config->tunnels) { \
			for (int _t = 0; _t < config->num_tunnels; _t++) { \
				config->tunnels[_t].field = (val); \
			} \
		} \
	} while (0)

		case 'i':
			OVERRIDE_PARAM_STR(local_interface, optarg);
			break;

		case 'l':
			OVERRIDE_PARAM_INT(local_port, atoi(optarg));
			break;

		case 's':
			OVERRIDE_PARAM_STR(remote_addr, optarg);
			break;

		case 'p':
			OVERRIDE_PARAM_INT(remote_port, atoi(optarg));
			break;

		case 'k':
			OVERRIDE_PARAM_STR(key, optarg);
			break;

		case 'e':
			OVERRIDE_PARAM_STR(crypt, optarg);
			break;

		case 'm':
			OVERRIDE_PARAM_STR(mode, optarg);
			xkcp_apply_mode_param(param);
			if (config->num_tunnels > 0 && config->tunnels) {
				for (int _t = 0; _t < config->num_tunnels; _t++) {
					xkcp_apply_mode_param(&config->tunnels[_t]);
				}
			}
			break;

		case GETOPT_VAL_MTU:
		case 'M':
			OVERRIDE_PARAM_INT(mtu, atoi(optarg));
			break;

		case GETOPT_VAL_SNDWND:
		case 'S':
			OVERRIDE_PARAM_INT(sndwnd, atoi(optarg));
			break;

		case GETOPT_VAL_RCVWND:
		case 'R':
			OVERRIDE_PARAM_INT(rcvwnd, atoi(optarg));
			break;

		case GETOPT_VAL_DATASHARD:
		case 'D':
			OVERRIDE_PARAM_INT(data_shard, atoi(optarg));
			break;

		case GETOPT_VAL_PARITYSHARD:
		case 'P':
			OVERRIDE_PARAM_INT(parity_shard, atoi(optarg));
			break;

		case GETOPT_VAL_NOCOMP:
		case 'N':
			OVERRIDE_PARAM_INT(nocomp, 1);
			break;

		case GETOPT_VAL_ACKNODELAY:
		case 'A':
			OVERRIDE_PARAM_INT(ack_nodelay, 1);
			break;

		case GETOPT_VAL_NODELAY:
		case 'L':
			OVERRIDE_PARAM_INT(nodelay, 1);
			break;

		case GETOPT_VAL_INTERVAL:
			OVERRIDE_PARAM_INT(interval, atoi(optarg));
			break;

		case GETOPT_VAL_KEEPALIVE:
			OVERRIDE_PARAM_INT(keepalive, atoi(optarg));
			break;

		case GETOPT_VAL_CONN_TIMEOUT:
			OVERRIDE_PARAM_INT(conn_timeout, atoi(optarg));
			break;

		case GETOPT_VAL_SYSLOG:
			debugconf.log_syslog = 1;
			debugconf.log_stderr = 0;
			break;

		case GETOPT_VAL_VERSION:
		case 'v':
			   fprintf(stdout, "This is %s version " VERSION "\n", argv[0]);
			exit(1);
			break;

		default:
			usage(argv[0]);
			exit(1);
			break;
		}

	/* Client needs a server address; the pure dynamic gateway server does not. */
	if (!config->is_server &&
	    (!param->remote_addr || param->remote_addr[0] == '\0')) {
		usage(argv[0]);
		exit(1);
	}
}
