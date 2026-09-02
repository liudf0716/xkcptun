#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#include "xkcp_config.h"
#include "xkcp_proto.h"
#include "xkcp_auth.h"

static void test_proto_handshake(void)
{
	char buf[XKCP_MAX_HDR_LEN];
	char host_out[128];
	uint16_t port_out = 0;

	/* 1. IPv4 test */
	int len = xkcp_proto_encode_header(buf, sizeof(buf), "127.0.0.1", 22);
	assert(len > 0);
	int parsed = xkcp_proto_decode_header(buf, len, host_out, sizeof(host_out), &port_out);
	assert(parsed == len);
	assert(strcmp(host_out, "127.0.0.1") == 0);
	assert(port_out == 22);

	/* 2. Domain name test */
	len = xkcp_proto_encode_header(buf, sizeof(buf), "my-server.local", 8080);
	assert(len > 0);
	parsed = xkcp_proto_decode_header(buf, len, host_out, sizeof(host_out), &port_out);
	assert(parsed == len);
	assert(strcmp(host_out, "my-server.local") == 0);
	assert(port_out == 8080);

	/* 3. Non-proto raw data test */
	const char *raw_data = "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n";
	parsed = xkcp_proto_decode_header(raw_data, strlen(raw_data), host_out, sizeof(host_out), &port_out);
	assert(parsed == 0); /* returns 0 for non-handshake data */

	printf("test_proto_handshake PASSED!\n");
}

static void test_auth_roundtrip(void)
{
	char buf[XKCP_MAX_HDR_LEN];
	char host_out[128];
	uint16_t port_out = 0;
	uint8_t ver = 0;
	const uint8_t *ts = NULL;
	const uint8_t *token = NULL;
	uint32_t now = (uint32_t)time(NULL);

	/* v2 header round-trip: encode -> decode -> verify */
	int len = xkcp_auth_encode_header(buf, sizeof(buf), "127.0.0.1", 22,
					   "secret-key", 12345, now);
	assert(len > 0);
	int parsed = xkcp_proto_decode_header_ex(buf, len, host_out, sizeof(host_out),
						 &port_out, &ver, &ts, &token);
	assert(parsed == len);
	assert(ver == XKCP_PROTO_VER_2);
	assert(strcmp(host_out, "127.0.0.1") == 0 && port_out == 22);
	assert(ts != NULL && token != NULL);
	assert(xkcp_auth_verify("secret-key", 12345, host_out, port_out, ts, token, now) == 0);

	/* negative cases */
	assert(xkcp_auth_verify("wrong-key", 12345, host_out, port_out, ts, token, now) < 0);
	assert(xkcp_auth_verify("secret-key", 999, host_out, port_out, ts, token, now) < 0);
	assert(xkcp_auth_verify("secret-key", 12345, "10.0.0.1", port_out, ts, token, now) < 0);
	assert(xkcp_auth_verify("secret-key", 12345, host_out, 23, ts, token, now) < 0);
	assert(xkcp_auth_verify("secret-key", 12345, host_out, port_out, ts, token,
				now + XKCP_AUTH_WINDOW_SEC + 10) < 0);

	/* domain target + verify */
	len = xkcp_auth_encode_header(buf, sizeof(buf), "example.test", 443, "k", 7, now);
	assert(len > 0);
	parsed = xkcp_proto_decode_header_ex(buf, len, host_out, sizeof(host_out),
					     &port_out, &ver, &ts, &token);
	assert(parsed == len && ver == XKCP_PROTO_VER_2);
	assert(strcmp(host_out, "example.test") == 0 && port_out == 443);
	assert(xkcp_auth_verify("k", 7, host_out, port_out, ts, token, now) == 0);

	/* legacy v1 header still decodes, without auth fields */
	len = xkcp_proto_encode_header(buf, sizeof(buf), "10.1.2.3", 99);
	assert(len > 0);
	parsed = xkcp_proto_decode_header_ex(buf, len, host_out, sizeof(host_out),
					     &port_out, &ver, &ts, &token);
	assert(parsed == len && ver == XKCP_PROTO_VER_1 && ts == NULL && token == NULL);

	/* buffer too small for domain: decode must fail safely with -1 */
	len = xkcp_proto_encode_header(buf, sizeof(buf), "very-long-domain-name.test", 80);
	assert(len > 0);
	char small_host[8];
	assert(xkcp_proto_decode_header_ex(buf, len, small_host, sizeof(small_host),
					   &port_out, &ver, &ts, &token) < 0);

	printf("test_auth_roundtrip PASSED!\n");
}

int main(void)
{
	test_proto_handshake();
	test_auth_roundtrip();

	const char *test_toml = "/tmp/test_xkcp.toml";
	FILE *fp = fopen(test_toml, "w");
	assert(fp != NULL);

	fprintf(fp,
		"# Global test section with common connection info\n"
		"[global]\n"
		"syslog = true\n"
		"mon_port = 9086\n"
		"debug = 7\n"
		"remote_addr = \"172.96.252.145\"\n"
		"remote_port = 9089\n"
		"mode = \"fast3\"\n"
		"mtu = 1350\n"
		"sndwnd = 2048\n"
		"rcvwnd = 2048\n"
		"fec = 1\n"
		"datashard = 10\n"
		"parityshard = 3\n"
		"pacing = 128\n"
		"\n"
		"# Simplified tunnel 1 (inherits global, only specifies local_port and target_port)\n"
		"[[tunnel]]\n"
		"name = \"ssh_dyn\"\n"
		"local_port = 2222\n"
		"target_port = 22\n"
		"\n"
		"# Simplified tunnel 2 (inherits global, specifies target_addr and target_port)\n"
		"[[tunnel]]\n"
		"name = \"web_dyn\"\n"
		"local_port = 8080\n"
		"target_addr = \"192.168.1.50\"\n"
		"target_port = 80\n"
	);
	fclose(fp);

	config_init();
	int rc = xkcp_parse_param(test_toml);
	assert(rc == 0);

	struct xkcp_config *cfg = xkcp_get_config();
	assert(cfg->syslog == 1);
	assert(cfg->mon_port == 9086);
	assert(cfg->num_tunnels == 2);
	assert(cfg->tunnels != NULL);

	/* Tunnel 1 check (inherited values + dynamic target) */
	struct xkcp_param *t1 = &cfg->tunnels[0];
	assert(strcmp(t1->name, "ssh_dyn") == 0);
	assert(t1->local_port == 2222);
	assert(strcmp(t1->remote_addr, "172.96.252.145") == 0);
	assert(t1->remote_port == 9089);
	assert(t1->dynamic_target == 1);
	assert(strcmp(t1->target_addr, "127.0.0.1") == 0);
	assert(t1->target_port == 22);
	assert(t1->nodelay == 1 && t1->interval == 10 && t1->resend == 2 && t1->nc == 1); // inherited fast3 preset
	assert(t1->mtu == 1350);
	assert(t1->sndwnd == 2048);
	assert(t1->rcvwnd == 2048);
	assert(t1->fec == 1);
	assert(t1->data_shard == 10);
	assert(t1->parity_shard == 3);
	assert(t1->pacing == 128);

	/* Tunnel 2 check */
	struct xkcp_param *t2 = &cfg->tunnels[1];
	assert(strcmp(t2->name, "web_dyn") == 0);
	assert(t2->local_port == 8080);
	assert(strcmp(t2->remote_addr, "172.96.252.145") == 0);
	assert(t2->remote_port == 9089);
	assert(t2->dynamic_target == 1);
	assert(strcmp(t2->target_addr, "192.168.1.50") == 0);
	assert(t2->target_port == 80);
	assert(t2->fec == 1);

	xkcp_free_config();
	remove(test_toml);

	printf("All TOML parser & Dynamic Destination unit tests PASSED successfully!\n");
	return 0;
}
