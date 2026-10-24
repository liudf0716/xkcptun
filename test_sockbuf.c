#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "xkcp_config.h"

void xkcp_apply_sockbuf_param(int fd, struct xkcp_param *param);

#ifdef __linux__
#define PAYLOAD_SCALE 2
#else
#define PAYLOAD_SCALE 1
#endif

static int payload(int fd, int opt)
{
	int val = 0;
	socklen_t len = sizeof(val);
	if (getsockopt(fd, SOL_SOCKET, opt, &val, &len) < 0)
		return -1;
	return val / PAYLOAD_SCALE;
}

static int read_int_file(const char *path, int *out)
{
	FILE *fp = fopen(path, "r");
	if (!fp)
		return -1;
	if (fscanf(fp, "%d", out) != 1) {
		fclose(fp);
		return -1;
	}
	fclose(fp);
	return 0;
}

static void expect(int cond, const char *msg)
{
	if (!cond) {
		fprintf(stderr, "FAIL: %s\n", msg);
		exit(1);
	}
}

int main(void)
{
	struct xkcp_param param;
	int fd;
	int rcv, rmem = 0;
	const int small = 4096;
	const int huge = 64 * 1024 * 1024;

	memset(&param, 0, sizeof(param));
	param.name = "sockbuf-test";
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	expect(fd >= 0, "udp socket");

	param.sock_buf = 0;
	xkcp_apply_sockbuf_param(fd, &param);

	param.sock_buf = small;
	xkcp_apply_sockbuf_param(fd, &param);
	rcv = payload(fd, SO_RCVBUF);
	expect(rcv >= small, "small SO_RCVBUF request is applied");
	printf("small request %d -> payload %d\n", small, rcv);

	param.sock_buf = huge;
	xkcp_apply_sockbuf_param(fd, &param);
	rcv = payload(fd, SO_RCVBUF);
	expect(rcv > 0, "huge request leaves a readable buffer");

	if (read_int_file("/proc/sys/net/core/rmem_max", &rmem) == 0 && rcv >= huge) {
		printf("force path: payload %d >= requested %d (rmem_max %d)\n",
		       rcv, huge, rmem);
	} else if (rmem > 0) {
		expect(rcv <= rmem, "without force, payload stays within rmem_max");
		printf("fallback path: payload %d <= rmem_max %d (no CAP_NET_ADMIN or force unsupported)\n",
		       rcv, rmem);
	} else {
		printf("skip rmem_max check (sysctl unreadable); payload %d\n", rcv);
	}

	printf("test_sockbuf PASSED\n");
	return 0;
}
