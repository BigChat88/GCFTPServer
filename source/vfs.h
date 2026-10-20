#ifndef GCFTP_VFS_H
#define GCFTP_VFS_H

/*
 * The FTP client sees one tree: "/" lists the mounted devices as folders,
 * and "/sd/games/x.iso" maps to the libfat path "sd:/games/x.iso".
 */

#include <stdbool.h>
#include <stddef.h>

#define VFS_PATH_MAX 512

/* Joins arg onto cwd and normalizes "." / ".." into an absolute virtual path */
bool vfs_resolve(const char *cwd, const char *arg, char *out, size_t outsz);

/* Virtual path -> "dev:/..." path. False for "/" or an unmounted device. */
bool vfs_to_real(const char *vpath, char *out, size_t outsz);

bool vfs_is_root(const char *vpath);

/* True for "/" and for "/<mounted device>" */
bool vfs_is_dir(const char *vpath);

#endif
