#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "storage.h"
#include "vfs.h"

#define MAX_DEPTH 64

bool vfs_resolve(const char *cwd, const char *arg, char *out, size_t outsz)
{
	char tmp[VFS_PATH_MAX * 2];
	size_t starts[MAX_DEPTH];
	size_t olen = 0;
	int depth = 0;
	char *save, *tok;
	int n;

	if (!arg || !*arg)
		arg = ".";
	if (arg[0] == '/')
		n = snprintf(tmp, sizeof(tmp), "%s", arg);
	else
		n = snprintf(tmp, sizeof(tmp), "%s/%s", cwd, arg);
	if (n < 0 || (size_t)n >= sizeof(tmp) || outsz < 2)
		return false;

	out[0] = '\0';
	for (tok = strtok_r(tmp, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
		size_t tl;

		if (strcmp(tok, ".") == 0)
			continue;
		if (strcmp(tok, "..") == 0) {
			if (depth > 0) {
				olen = starts[--depth];
				out[olen] = '\0';
			}
			continue;
		}

		tl = strlen(tok);
		if (depth >= MAX_DEPTH || olen + 1 + tl + 1 > outsz)
			return false;
		starts[depth++] = olen;
		out[olen++] = '/';
		memcpy(out + olen, tok, tl);
		olen += tl;
		out[olen] = '\0';
	}

	if (olen == 0) {
		out[0] = '/';
		out[1] = '\0';
	}
	return true;
}

bool vfs_is_root(const char *vpath)
{
	return vpath[0] == '/' && vpath[1] == '\0';
}

bool vfs_to_real(const char *vpath, char *out, size_t outsz)
{
	const char *dev, *rest;
	size_t devlen;
	int n;

	if (vpath[0] != '/' || vfs_is_root(vpath))
		return false;

	dev = vpath + 1;
	rest = strchr(dev, '/');
	devlen = rest ? (size_t)(rest - dev) : strlen(dev);
	if (!storage_is_mounted(dev, devlen))
		return false;

	n = snprintf(out, outsz, "%.*s:%s", (int)devlen, dev, rest ? rest : "/");
	return n > 0 && (size_t)n < outsz;
}

bool vfs_is_dir(const char *vpath)
{
	char real[VFS_PATH_MAX + 16];
	struct stat st;

	if (vfs_is_root(vpath))
		return true;
	if (!vfs_to_real(vpath, real, sizeof(real)))
		return false;
	/* A device root has no '/' after the device name */
	if (strchr(vpath + 1, '/') == NULL)
		return true;
	return stat(real, &st) == 0 && S_ISDIR(st.st_mode);
}
