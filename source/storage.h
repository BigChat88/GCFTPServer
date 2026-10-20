#ifndef GCFTP_STORAGE_H
#define GCFTP_STORAGE_H

#include <stdbool.h>
#include <stddef.h>

/* Probes and mounts every supported FAT device. Returns how many mounted. */
int storage_init(void);
/* Flushes and unmounts everything */
void storage_shutdown(void);

/* Iterate mounted devices: index 0..storage_count()-1 */
int storage_count(void);
const char *storage_name(int index);
const char *storage_desc(int index);

/* name need not be NUL terminated; len is its length */
bool storage_is_mounted(const char *name, size_t len);

#endif
