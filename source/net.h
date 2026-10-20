#ifndef GCFTP_NET_H
#define GCFTP_NET_H

#include <gctypes.h>
#include <network.h>

#include "config.h"

/* Brings up the BBA using DHCP or the static IP in cfg. ip_out gets the
 * dotted address. Returns 0 on success, negative on failure. */
s32 net_start(const config_t *cfg, char ip_out[16]);
const char *net_error_string(s32 err);

/* Human-readable name of the adapter libogc2 brought up, e.g.
 * "Broadband Adapter (Serial Port 1)" */
const char *net_adapter_name(void);

/* Negotiated link speed for the BBA, e.g. "100 Mbit/s full duplex";
 * NULL for other adapters */
const char *net_link_string(void);

/* Listening TCP socket bound to INADDR_ANY:port, or negative error */
s32 net_listen_on(u16 port, u32 backlog);

/* >0 readable, 0 timeout, <0 error */
s32 net_wait_readable(s32 sock, u32 timeout_ms);

/* Sends everything or returns a negative error */
s32 net_send_all(s32 sock, const void *data, s32 len);

#endif
