#include <errno.h>
#include <ogc/exi.h>
#include <stdio.h>
#include <string.h>

#include "net.h"

/* netconn_write already splits by available send buffer; large calls just
 * save round trips to the tcpip thread */
#define SEND_CHUNK (64 * 1024)

s32 net_start(const config_t *cfg, char ip_out[16])
{
	char ip[16] = {0}, netmask[16] = {0}, gateway[16] = {0};
	bool use_dhcp = cfg->ip[0] == '\0';
	s32 ret;

	if (!use_dhcp) {
		strcpy(ip, cfg->ip);
		strcpy(netmask, cfg->netmask[0] ? cfg->netmask : "255.255.255.0");
		strcpy(gateway, cfg->gateway);
	}

	/* if_config probes the BBA first, then the ETH2GC adapters (W6100/W6300,
	 * W5500, ENC28J60) on SP1, SP2, Slot A and Slot B. It can only be called
	 * once per boot: libogc2 latches its init state even on failure. */
	ret = if_config(ip, netmask, gateway, use_dhcp);
	if (ret < 0)
		return ret;

	if (use_dhcp) {
		strcpy(ip_out, ip);
	} else {
		struct in_addr addr = { net_gethostip() };
		strcpy(ip_out, inet_ntoa(addr));
	}
	return 0;
}

const char *net_error_string(s32 err)
{
	switch (err) {
	case -1:
		return "No network adapter found (or invalid static IP)";
	case -2:
		return "No link, or no reply from DHCP";
	default:
		return strerror(-err);
	}
}

const char *net_adapter_name(void)
{
	static char desc[64];
	char ifname[8];
	const char *chip, *port;

	/* The adapter is the first interface added, so its index is 1 */
	if (!if_indextoname(1, ifname))
		return "Unknown adapter";

	/* libogc2 interface names: "en" = BBA, otherwise <chip><port> */
	if (ifname[0] == 'e' && ifname[1] == 'n')
		return "Broadband Adapter (Serial Port 1)";

	switch (ifname[0]) {
	case 'w': chip = "ETH2GC (W6100/W6300)"; break;
	case 'W': chip = "ETH2GC (W5500)";       break;
	case 'E': chip = "ETH2GC (ENC28J60)";    break;
	default:  chip = "Network adapter";      break;
	}
	switch (ifname[1]) {
	case '1': port = "Serial Port 1"; break;
	case '2': port = "Serial Port 2"; break;
	case 'A': port = "Slot A";        break;
	case 'B': port = "Slot B";        break;
	default:  port = "unknown port";  break;
	}
	snprintf(desc, sizeof(desc), "%s (%s)", chip, port);
	return desc;
}

/* BBA NWAY status register, see gcif.c in libogc2 */
#define BBA_NWAYS         0x31
#define BBA_NWAYS_LS10    (1 << 0)
#define BBA_NWAYS_LS100   (1 << 1)
#define BBA_NWAYS_100TXF  (1 << 4)
#define BBA_NWAYS_10TXF   (1 << 6)

static u8 bba_read_reg(u32 reg)
{
	u32 req = (reg << 8) | 0x80000000;
	u8 val = 0;

	/* The driver accesses the BBA from interrupts; take the bus first */
	EXI_LockEx(EXI_CHANNEL_0, EXI_DEVICE_2);
	EXI_Select(EXI_CHANNEL_0, EXI_DEVICE_2, EXI_SPEED32MHZ);
	EXI_ImmEx(EXI_CHANNEL_0, &req, sizeof(req), EXI_WRITE);
	EXI_ImmEx(EXI_CHANNEL_0, &val, sizeof(val), EXI_READ);
	EXI_Deselect(EXI_CHANNEL_0);
	EXI_Unlock(EXI_CHANNEL_0);
	return val;
}

const char *net_link_string(void)
{
	char ifname[8];
	u8 nways;

	if (!if_indextoname(1, ifname) || ifname[0] != 'e' || ifname[1] != 'n')
		return NULL;

	nways = bba_read_reg(BBA_NWAYS);
	if (nways & BBA_NWAYS_LS100)
		return (nways & BBA_NWAYS_100TXF) ? "100 Mbit/s full duplex" : "100 Mbit/s";
	if (nways & BBA_NWAYS_LS10)
		return (nways & BBA_NWAYS_10TXF) ? "10 Mbit/s full duplex" : "10 Mbit/s";
	return "down";
}

s32 net_listen_on(u16 port, u32 backlog)
{
	struct sockaddr_in addr;
	s32 sock, ret;

	sock = net_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (sock < 0)
		return sock;

	memset(&addr, 0, sizeof(addr));
	addr.sin_len = sizeof(addr);
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);

	ret = net_bind(sock, (struct sockaddr *)&addr, sizeof(addr));
	if (ret < 0) {
		net_close(sock);
		return ret;
	}
	ret = net_listen(sock, backlog);
	if (ret < 0) {
		net_close(sock);
		return ret;
	}
	return sock;
}

s32 net_wait_readable(s32 sock, u32 timeout_ms)
{
	struct timeval tv;
	fd_set readset;

	FD_ZERO(&readset);
	FD_SET(sock, &readset);
	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;

	return net_select(sock + 1, &readset, NULL, NULL, &tv);
}

s32 net_send_all(s32 sock, const void *data, s32 len)
{
	const u8 *p = data;

	while (len > 0) {
		s32 chunk = len > SEND_CHUNK ? SEND_CHUNK : len;
		s32 sent = net_send(sock, p, chunk, 0);

		if (sent < 0)
			return sent;
		if (sent == 0)
			return -ECONNRESET;
		p += sent;
		len -= sent;
	}
	return 0;
}
