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

	struct xkcp_param param;
}

void config_init(void);

struct xkcp_config * xkcp_get_config(void);

int xkcp_parse_param(const char *filename);

int parse_json_param(struct xkcp_param *config, const char *filename);

struct xkcp_param *xkcp_get_param(void);
#endif