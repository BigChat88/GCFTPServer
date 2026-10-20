#include <gccore.h>
#include <stdlib.h>

#include "config.h"
#include "ftp.h"
#include "net.h"
#include "storage.h"
#include "ui.h"

static void wait_for_start(void)
{
	while (SYS_MainLoop()) {
		PAD_ScanPads();
		if (PAD_ButtonsDown(0) & PAD_BUTTON_START)
			break;
		VIDEO_WaitVSync();
	}
}

static void app_exit(void)
{
	ui_shutdown();
	storage_shutdown();
	exit(0);
}

static void fail(void)
{
	ui_log("\nPress START to exit.");
	wait_for_start();
	app_exit();
}

int main(int argc, char **argv)
{
	config_t cfg;
	char ip[16] = {0};
	s32 ret;

	ui_init();
	ui_log("\n " APP_NAME " " APP_VERSION "\n");

	ui_log("Mounting storage...");
	if (storage_init() == 0)
		ui_log("  No devices mounted (the FTP root will be empty)");
	for (int i = 0; i < storage_count(); i++)
		ui_log("  /%-9s %s", storage_name(i), storage_desc(i));

	config_load(&cfg);
	if (cfg.source[0])
		ui_log("Config: %s", cfg.source);
	else
		ui_log("Config: defaults (no gcftp.ini found)");

	ui_log("Starting network (%s)...", cfg.ip[0] ? "static IP" : "DHCP");
	ret = net_start(&cfg, ip);
	if (ret < 0) {
		ui_log("Network error (%d): %s", (int)ret, net_error_string(ret));
		if (ret == -2)
			ui_log("Adapter: %s", net_adapter_name());
		ui_log("Check the network adapter and the cable.");
		fail();
	}
	ui_log("  Adapter: %s", net_adapter_name());
	if (net_link_string())
		ui_log("  Link:    %s", net_link_string());

	ret = ftp_start(&cfg);
	if (ret < 0) {
		ui_log("Could not open port %u (%d)", cfg.port, (int)ret);
		fail();
	}

	ui_log("\nServer ready:  ftp://%s:%u", ip, cfg.port);
	if (cfg.user[0])
		ui_log("User: %s   Password: %s", cfg.user, cfg.pass[0] ? "(set in gcftp.ini)" : "(any)");
	else
		ui_log("Anonymous access (any user/password)");
	ui_log("Passive mode, ports 50000-50999. Press START to exit.");
	ui_begin_log();

	wait_for_start();
	app_exit();
	return 0;
}
