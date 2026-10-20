#ifndef GCFTP_CONFIG_H
#define GCFTP_CONFIG_H

#include <stdbool.h>
#include <gctypes.h>

#define CONFIG_DEFAULT_USER "cube"

typedef struct {
	char user[32];     /* empty = anonymous, any user/password accepted */
	char pass[32];     /* empty = any password accepted */
	u16 port;
	/* Static IP; all three empty = DHCP */
	char ip[16];
	char netmask[16];
	char gateway[16];
	char source[64];   /* where the config was loaded from, empty = defaults */
} config_t;

/* Fills cfg with defaults and overrides them from the first gcftp.ini found */
void config_load(config_t *cfg);

#endif
