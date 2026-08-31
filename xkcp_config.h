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

#ifndef	_XKCP_CONFIG_
#define	_XKCP_CONFIG_

#include <stdint.h>

struct xkcp_param {
	char 	*name;			    // tunnel identifier name / alias
	char	*local_interface;   // local interface to bind
	int		local_port; 	    // local listen port
	char	*remote_addr;	    // remote server host (client) or backend host (server)
	int		remote_port;	    // remote server port (client) or backend port (server)

	/* Dynamic Destination */
	char	*target_addr;	    // dynamic destination host on remote side
	int		target_port;	    // dynamic destination port on remote side
	int		dynamic_target;	    // 1 = enable dynamic destination first-packet header

	char	*key;			    // key
	char	*crypt; 		    // crypt
	char	*mode;			    // mode
	int 	conn;			    // conn (compat)
	int 	auto_expire;	    // autoexpire (compat)
	int 	scavenge_ttl;	    // scavengettl (compat)
	int		mtu;			    // mtu
	int		sndwnd;			    // sndwnd
	int		rcvwnd;			    // rcvwnd
	int		data_shard;		    // datashard (FEC)
	int		parity_shard;  	    // parityshard (FEC)
	int		dscp;			    // dscp TOS
	int 	nocomp; 		    // nocomp (compat)
	int		ack_nodelay;	    // acknodelay (compat)
	int 	nodelay;		    // nodelay
	int		interval;		    // interval
	int 	resend;			    // resend
	int 	nc; 			    // no congestion
	int		loss_ctrl;		    // loss-driven AIMD send window (1=on)
	int		pacing;			    // pacing: max KCP segments per flush tick (0=off)
	int		fec;			    // fec enable (1=frame all datagrams)
	int 	sock_buf;		    // sockbuf
	int 	keepalive;		    // keepalive ping interval (seconds)
	int 	conn_timeout;	    // conn_timeout (seconds, 0=disabled)
};

struct xkcp_config {
	char 	*config_file;
	int 	daemon;
	int 	is_server;
	int 	syslog;
	int 	mon_port;
	int 	(*main_loop)(void);

	/* Multi-tunnel definitions */
	int 	num_tunnels;
	struct xkcp_param *tunnels;

	/* Default param template (from [global] or command-line) */
	struct xkcp_param param;
};

void config_init(void);

struct xkcp_config *xkcp_get_config(void);

struct xkcp_param *xkcp_get_param(void);

void xkcp_param_init(struct xkcp_param *param);

void xkcp_param_clone(struct xkcp_param *dst, const struct xkcp_param *src);

void xkcp_param_free(struct xkcp_param *param);

int xkcp_parse_param(const char *filename);

void xkcp_apply_mode_param(struct xkcp_param *param);

void xkcp_apply_mode(void);

void xkcp_free_config(void);

#endif
