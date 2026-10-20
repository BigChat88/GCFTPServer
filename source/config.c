#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "storage.h"

#define CONFIG_FILE "gcftp.ini"

static char *trim(char *s)
{
	char *end;

	while (isspace((unsigned char)*s))
		s++;
	end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1]))
		end--;
	*end = '\0';
	return s;
}

static void copy_value(char *dst, size_t size, const char *value)
{
	snprintf(dst, size, "%s", value);
}

static void parse_line(config_t *cfg, char *line)
{
	char *eq, *key, *value;

	line = trim(line);
	if (*line == '\0' || *line == '#' || *line == ';' || *line == '[')
		return;

	eq = strchr(line, '=');
	if (!eq)
		return;
	*eq = '\0';
	key = trim(line);
	value = trim(eq + 1);

	if (strcasecmp(key, "user") == 0)
		copy_value(cfg->user, sizeof(cfg->user), value);
	else if (strcasecmp(key, "pass") == 0 || strcasecmp(key, "password") == 0)
		copy_value(cfg->pass, sizeof(cfg->pass), value);
	else if (strcasecmp(key, "port") == 0) {
		long port = strtol(value, NULL, 10);
		if (port > 0 && port < 65536)
			cfg->port = (u16)port;
	} else if (strcasecmp(key, "ip") == 0)
		copy_value(cfg->ip, sizeof(cfg->ip), value);
	else if (strcasecmp(key, "netmask") == 0)
		copy_value(cfg->netmask, sizeof(cfg->netmask), value);
	else if (strcasecmp(key, "gateway") == 0)
		copy_value(cfg->gateway, sizeof(cfg->gateway), value);
}

void config_load(config_t *cfg)
{
	char path[64];
	char line[256];

	memset(cfg, 0, sizeof(*cfg));
	copy_value(cfg->user, sizeof(cfg->user), CONFIG_DEFAULT_USER);
	cfg->port = 21;

	for (int i = 0; i < storage_count(); i++) {
		FILE *f;

		snprintf(path, sizeof(path), "%s:/%s", storage_name(i), CONFIG_FILE);
		f = fopen(path, "r");
		if (!f)
			continue;

		while (fgets(line, sizeof(line), f))
			parse_line(cfg, line);
		fclose(f);
		copy_value(cfg->source, sizeof(cfg->source), path);
		return;
	}
}
