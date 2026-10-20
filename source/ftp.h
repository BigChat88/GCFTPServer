#ifndef GCFTP_FTP_H
#define GCFTP_FTP_H

#include <gctypes.h>

#include "config.h"

#define APP_NAME    "GC FTP Server"
#define APP_VERSION "1.0.0"

/* Starts the listener thread. Returns 0 on success, negative on error. */
s32 ftp_start(const config_t *cfg);

#endif
