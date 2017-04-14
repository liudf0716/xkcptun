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

struct xkcp_param {
	char	*local_interface; 	// localaddr
	char	*remote_addr; 	// remoteaddr
	char	*key;			// key
	char	*crypt;			// crypt
	char	*mode;			// mode
	int		local_port;		// local tcp listen port
	int		remote_port;	// remote udp connect port
	int 	conn;			// conn
	int 	auto_expire;	// autoexpire
	int 	scavenge_ttl;	// scavengettl
	int		mtu;			// mtu
	int		sndwnd;			// sndwnd
	int		rcvwnd;			// rcvwnd
	int		data_shard;		// datashard	 
	int		parity_shard;  	// parityshard
	int		dscp;			// dscp
	int 	nocomp; 		// nocomp
	int		ack_nodelay;	// acknodelay
	int 	nodelay;		// nodelay
	int		interval;		// interval
	int 	resend;			// resend
	int 	nc; 			// no congestion
	int 	sock_buf;		// sockbuf
	int 	keepalive;		// keepalive
};

struct xkcp_config {
	char 	*config_file;
	int 	daemon;  
	int		is_server;
	int		(*main_loop)();
	
	struct xkcp_param param;
};

void config_init(void);

struct xkcp_config * xkcp_get_config(void);

int xkcp_parse_param(const char *filename);

int xkcp_parse_json_param(struct xkcp_param *config, const char *filename);

struct xkcp_param *xkcp_get_param(void);

#endif
