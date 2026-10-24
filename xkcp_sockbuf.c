#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <syslog.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "xkcp_util.h"
#include "debug.h"

/*
 * Linux doubles the value stored in sk_rcvbuf/sk_sndbuf and getsockopt
 * returns that doubled figure, not the payload queue the application asked
 * for. Other platforms return the size that was applied.
 */
#ifdef __linux__
#define XKCP_SOCKBUF_GET_SCALE 2
#else
#define XKCP_SOCKBUF_GET_SCALE 1
#endif

static int xkcp_set_buf(int fd, int opt, int val)
{
	return setsockopt(fd, SOL_SOCKET, opt, &val, sizeof(val));
}

static int xkcp_get_buf(int fd, int opt, int *out)
{
	socklen_t len = sizeof(*out);
	if (getsockopt(fd, SOL_SOCKET, opt, out, &len) < 0)
		return -1;
	return 0;
}

static int xkcp_try_buf(int fd, int force_opt, int plain_opt, int val, int *used_force)
{
	*used_force = 0;
#ifdef SO_RCVBUFFORCE
	if (force_opt && xkcp_set_buf(fd, force_opt, val) == 0) {
		*used_force = 1;
		return 0;
	}
#else
	(void)force_opt;
#endif
	return xkcp_set_buf(fd, plain_opt, val);
}

void xkcp_apply_sockbuf_param(int fd, struct xkcp_param *param)
{
	const char *name;
	int bufsz;
	int rcv_force = 0, snd_force = 0;
	int rcv = 0, snd = 0;

	if (!param)
		return;

	if (param->dscp > 0) {
		int tos = (param->dscp << 2) & 0xFF;
		if (setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos)) < 0)
			debug(LOG_WARNING, "setsockopt IP_TOS [dscp=%d] failed: %s",
			      param->dscp, strerror(errno));
	}

	bufsz = param->sock_buf;
	if (bufsz <= 0)
		return;

	name = (param->name && param->name[0]) ? param->name : "socket";

	if (xkcp_try_buf(fd,
#ifdef SO_RCVBUFFORCE
			 SO_RCVBUFFORCE,
#else
			 0,
#endif
			 SO_RCVBUF, bufsz, &rcv_force) < 0)
		debug(LOG_WARNING, "[%s] setsockopt SO_RCVBUF [%d] failed: %s",
		      name, bufsz, strerror(errno));

	if (xkcp_try_buf(fd,
#ifdef SO_SNDBUFFORCE
			 SO_SNDBUFFORCE,
#else
			 0,
#endif
			 SO_SNDBUF, bufsz, &snd_force) < 0)
		debug(LOG_WARNING, "[%s] setsockopt SO_SNDBUF [%d] failed: %s",
		      name, bufsz, strerror(errno));

	if (xkcp_get_buf(fd, SO_RCVBUF, &rcv) < 0 ||
	    xkcp_get_buf(fd, SO_SNDBUF, &snd) < 0) {
		debug(LOG_WARNING, "[%s] getsockopt sockbuf failed: %s",
		      name, strerror(errno));
		return;
	}

	debug(LOG_INFO,
	      "[%s] sockbuf requested %d; getsockopt SO_RCVBUF=%d SO_SNDBUF=%d%s%s%s",
	      name, bufsz, rcv, snd,
#ifdef __linux__
	      " (Linux reports 2x the queued payload)",
#else
	      "",
#endif
	      rcv_force ? " rcv=force" : "",
	      snd_force ? " snd=force" : "");

	if (rcv / XKCP_SOCKBUF_GET_SCALE < bufsz)
		debug(LOG_WARNING,
		      "[%s] effective SO_RCVBUF payload %d is below requested %d (getsockopt returned %d). Check net.core.rmem_max. A privileged process can use SO_RCVBUFFORCE; this program does not change sysctl.",
		      name, rcv / XKCP_SOCKBUF_GET_SCALE, bufsz, rcv);

	if (snd / XKCP_SOCKBUF_GET_SCALE < bufsz)
		debug(LOG_WARNING,
		      "[%s] effective SO_SNDBUF payload %d is below requested %d (getsockopt returned %d). Check net.core.wmem_max. A privileged process can use SO_SNDBUFFORCE; this program does not change sysctl.",
		      name, snd / XKCP_SOCKBUF_GET_SCALE, bufsz, snd);
}
