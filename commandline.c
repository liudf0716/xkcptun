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
	fprintf(stdout, "  -c <filename>     Use this config file\n");
	fprintf(stdout, "  -f                Run in foreground\n");
	fprintf(stdout, "  -h --help         Print usage\n");
	fprintf(stdout, "  -v --version      Print version information\n");
	fprintf(stdout, "  -d <level>        Debug level\n");
	fprintf(stdout, "  --syslog          Log to syslog\n\n");

	fprintf(stdout, "  -i <interface>    Interface to use (default br-lan)\n");
	fprintf(stdout, "  -l <port>         Port number of your local server (default: 9088)\n");
	fprintf(stdout, "  -s <host>         Host name or IP address of your remote server \n");
	fprintf(stdout, "  -p <port>         Port number of your remote server (default: 9089)\n");
	fprintf(stdout, "  -k <string>       Pre-shared secret between client and server\n");
	fprintf(stdout, "  -e <string>       Encrypt method: none\n");
	fprintf(stdout, "  -m <string>       Profiles: fast3, fast2, fast, normal (default: \"fast\")\n");
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

	while (-1 != (c = getopt_long(argc, argv, "Ac:D:d:e:fhi:K:k:L:l:M:m:NP:p:R:S:s:T:v",
									long_options, NULL)))
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

		case 'i':
			free(param->local_interface);
			param->local_interface = strdup(optarg);
			break;

		case 'l':
			param->local_port = atoi(optarg);
			break;

		case 's':
			free(param->remote_addr);
			param->remote_addr = strdup(optarg);
			break;

		case 'p':
			param->remote_port = atoi(optarg);
			break;

		case 'k':
			free(param->key);
			param->key = strdup(optarg);
			break;

		case 'e':
			free(param->crypt);
			param->crypt = strdup(optarg);
			break;

		case 'm':
			free(param->mode);
			param->mode = strdup(optarg);
			break;

		case GETOPT_VAL_MTU:
		case 'M':
			param->mtu= atoi(optarg);
			break;

		case GETOPT_VAL_SNDWND:
		case 'S':
			param->sndwnd= atoi(optarg);
			break;

		case GETOPT_VAL_RCVWND:
		case 'R':
			param->rcvwnd= atoi(optarg);
			break;

		case GETOPT_VAL_DATASHARD:
		case 'D':
			param->data_shard= atoi(optarg);
			break;

		case GETOPT_VAL_PARITYSHARD:
		case 'P':
			param->parity_shard= atoi(optarg);
			break;

		case GETOPT_VAL_NOCOMP:
		case 'N':
			param->nocomp = 1;
			break;

		case GETOPT_VAL_ACKNODELAY:
		case 'A':
			param->ack_nodelay = 1;
			break;

		case GETOPT_VAL_NODELAY:
		case 'L':
			param->nodelay = 1;
			break;

		case GETOPT_VAL_INTERVAL:
			param->interval = atoi(optarg);
			break;

		case GETOPT_VAL_KEEPALIVE:
			param->keepalive = atoi(optarg);
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

	if (NULL != config->config_file)
		if (xkcp_parse_param(config->config_file)) {
			debug(LOG_ERR, "xkcp_parse_param failed \n");
			usage(argv[0]);
			exit(0);
		}

	if (!param->remote_addr) {
		usage(argv[0]);
		exit(0);
	}
}
