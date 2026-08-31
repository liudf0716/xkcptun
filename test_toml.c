#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "xkcp_config.h"

int main(void)
{
	const char *test_toml = "/tmp/test_xkcp.toml";
	FILE *fp = fopen(test_toml, "w");
	assert(fp != NULL);

	fprintf(fp,
		"# Global test section\n"
		"[global]\n"
		"syslog = true\n"
		"mon_port = 9086\n"
		"debug = 7\n"
		"\n"
		"# First tunnel\n"
		"[[tunnel]]\n"
		"name = \"tunnel_1\"\n"
		"local_interface = \"eth0\"\n"
		"local_port = 2222\n"
		"remote_addr = \"192.168.1.100\"\n"
		"remote_port = 9089\n"
		"mode = \"fast3\"\n"
		"mtu = 1350\n"
		"sndwnd = 2048\n"
		"rcvwnd = 2048\n"
		"fec = 1\n"
		"datashard = 10\n"
		"parityshard = 3\n"
		"pacing = 128\n"
		"lossctrl = 0\n"
		"keepalive = 15\n"
		"\n"
		"# Second tunnel\n"
		"[[tunnel]]\n"
		"name = \"tunnel_2\"\n"
		"local_interface = \"br-lan\"\n"
		"local_port = 3333\n"
		"remote_addr = \"10.0.0.1\"\n"
		"remote_port = 9090\n"
		"mode = \"normal\"\n"
		"fec = 0\n"
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

	/* Tunnel 1 check */
	struct xkcp_param *t1 = &cfg->tunnels[0];
	assert(strcmp(t1->name, "tunnel_1") == 0);
	assert(strcmp(t1->local_interface, "eth0") == 0);
	assert(t1->local_port == 2222);
	assert(strcmp(t1->remote_addr, "192.168.1.100") == 0);
	assert(t1->remote_port == 9089);
	assert(t1->nodelay == 1 && t1->interval == 10 && t1->resend == 2 && t1->nc == 1); // fast3 preset
	assert(t1->mtu == 1350);
	assert(t1->sndwnd == 2048);
	assert(t1->rcvwnd == 2048);
	assert(t1->fec == 1);
	assert(t1->data_shard == 10);
	assert(t1->parity_shard == 3);
	assert(t1->pacing == 128);
	assert(t1->loss_ctrl == 0);
	assert(t1->keepalive == 15);

	/* Tunnel 2 check */
	struct xkcp_param *t2 = &cfg->tunnels[1];
	assert(strcmp(t2->name, "tunnel_2") == 0);
	assert(strcmp(t2->local_interface, "br-lan") == 0);
	assert(t2->local_port == 3333);
	assert(strcmp(t2->remote_addr, "10.0.0.1") == 0);
	assert(t2->remote_port == 9090);
	assert(t2->nodelay == 0 && t2->interval == 30 && t2->resend == 2 && t2->nc == 0); // normal preset
	assert(t2->fec == 0);

	xkcp_free_config();
	remove(test_toml);

	printf("All TOML parser unit tests PASSED successfully!\n");
	return 0;
}
