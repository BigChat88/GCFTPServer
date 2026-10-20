#include <gccore.h>
#include <fat.h>
#include <ogc/dvd.h>
#include <sdcard/gcsd.h>
#include <stdio.h>
#include <string.h>

#include "storage.h"

typedef struct {
	const char *name; /* devoptab name, also the top-level FTP directory */
	const char *desc;
	DISC_INTERFACE *io;
	bool mounted;
} storage_dev_t;

/* The BBA lives on Serial Port 1, so none of these share its EXI device */
static storage_dev_t devices[] = {
	{ "sd",       "SD2SP2 (Serial Port 2)", &__io_gcsd2, false },
	{ "carda",    "SD Gecko (Slot A)",      &__io_gcsda, false },
	{ "cardb",    "SD Gecko (Slot B)",      &__io_gcsdb, false },
	{ "gcloader", "GC Loader",              &__io_gcode, false },
};

#define NUM_DEVICES (sizeof(devices) / sizeof(devices[0]))

static storage_dev_t *mounted[NUM_DEVICES];
static int num_mounted = 0;

int storage_init(void)
{
	num_mounted = 0;
	for (size_t i = 0; i < NUM_DEVICES; i++) {
		devices[i].mounted = fatMountSimple(devices[i].name, devices[i].io);
		if (devices[i].mounted)
			mounted[num_mounted++] = &devices[i];
	}
	return num_mounted;
}

void storage_shutdown(void)
{
	char name[16];

	for (int i = 0; i < num_mounted; i++) {
		snprintf(name, sizeof(name), "%s:", mounted[i]->name);
		fatUnmount(name);
		mounted[i]->mounted = false;
	}
	num_mounted = 0;
}

int storage_count(void)
{
	return num_mounted;
}

const char *storage_name(int index)
{
	return (index >= 0 && index < num_mounted) ? mounted[index]->name : NULL;
}

const char *storage_desc(int index)
{
	return (index >= 0 && index < num_mounted) ? mounted[index]->desc : NULL;
}

bool storage_is_mounted(const char *name, size_t len)
{
	for (int i = 0; i < num_mounted; i++) {
		if (strlen(mounted[i]->name) == len && strncmp(mounted[i]->name, name, len) == 0)
			return true;
	}
	return false;
}
