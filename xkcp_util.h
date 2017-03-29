#ifndef	_XKCP_UTIL_
#define	_XKCP_UTIL_

void itimeofday(long *sec, long *usec);

IINT64 iclock64(void);

IUINT32 iclock();

char *get_iface_ip(const char *ifname);

#endif