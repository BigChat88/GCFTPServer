#include <gccore.h>
#include <ogc/timesupp.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ftp.h"
#include "net.h"
#include "storage.h"
#include "ui.h"
#include "vfs.h"
#include "xfer.h"

/* lwIP is built with 16 sockets; each session can hold 3 (control,
 * passive listener, data) on top of the main listener. */
#define MAX_SESSIONS      3
#define XFER_BUF_SIZE     XFER_CHUNK  /* listings reuse the first pipeline buffer */
#define CMD_BUF_SIZE      1024
#define REAL_PATH_MAX     (VFS_PATH_MAX + 16)
#define THREAD_STACK      (64 * 1024)

#define IDLE_TIMEOUT_MS   (5 * 60 * 1000)
#define DATA_TIMEOUT_MS   (30 * 1000)
#define ACCEPT_POLL_MS    500

#define PASV_PORT_MIN     50000
#define PASV_PORT_MAX     50999

typedef struct {
	int id;
	volatile bool in_use;
	lwp_t thread;

	s32 ctrl;
	s32 pasv_listen;
	struct sockaddr_in port_addr;  /* target for active (PORT) mode */
	bool port_mode;

	bool user_ok;
	bool logged_in;
	char cwd[VFS_PATH_MAX];
	char rename_from[VFS_PATH_MAX];
	u64 rest_offset;

	char rbuf[CMD_BUF_SIZE];
	int rlen;
	u8 *bufs[XFER_NBUF];
	u8 *xfer;  /* == bufs[0] */
} session_t;

static config_t server_cfg;
static s32 listen_sock = -1;
static lwp_t listen_thread = LWP_THREAD_NULL;
static session_t sessions[MAX_SESSIONS];
static u8 xfer_bufs[MAX_SESSIONS][XFER_NBUF][XFER_CHUNK] __attribute__((aligned(32)));

static mutex_t pasv_mutex = LWP_MUTEX_NULL;
static u16 next_pasv_port = PASV_PORT_MIN;

static const char *months[] = {
	"Jan", "Feb", "Mar", "Apr", "May", "Jun",
	"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

/* ------------------------------------------------------------------------ */
/* Control connection                                                       */
/* ------------------------------------------------------------------------ */

static s32 reply(session_t *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static s32 reply(session_t *s, const char *fmt, ...)
{
	char buf[CMD_BUF_SIZE];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
	va_end(ap);
	if (n < 0)
		return -1;
	if (n > (int)sizeof(buf) - 3)
		n = sizeof(buf) - 3;
	buf[n++] = '\r';
	buf[n++] = '\n';
	return net_send_all(s->ctrl, buf, n);
}

/* Reads one command line (without CRLF). Returns length, or <0 on
 * disconnect / idle timeout. */
static int read_line(session_t *s, char *out, int outsz)
{
	for (;;) {
		char *nl = memchr(s->rbuf, '\n', s->rlen);

		if (nl) {
			int len = nl - s->rbuf;
			int linelen = len;

			if (linelen > 0 && s->rbuf[linelen - 1] == '\r')
				linelen--;
			if (linelen >= outsz)
				linelen = outsz - 1;
			memcpy(out, s->rbuf, linelen);
			out[linelen] = '\0';

			s->rlen -= len + 1;
			memmove(s->rbuf, nl + 1, s->rlen);
			return linelen;
		}

		/* Line longer than the buffer: drop it */
		if (s->rlen == (int)sizeof(s->rbuf))
			s->rlen = 0;

		if (net_wait_readable(s->ctrl, IDLE_TIMEOUT_MS) <= 0)
			return -1;

		s32 got = net_recv(s->ctrl, s->rbuf + s->rlen, sizeof(s->rbuf) - s->rlen, 0);
		if (got <= 0)
			return -1;
		s->rlen += got;
	}
}

/* ------------------------------------------------------------------------ */
/* Data connection                                                          */
/* ------------------------------------------------------------------------ */

static void close_pasv(session_t *s)
{
	if (s->pasv_listen >= 0) {
		net_close(s->pasv_listen);
		s->pasv_listen = -1;
	}
}

static s32 open_pasv_listener(session_t *s, u16 *port_out)
{
	close_pasv(s);

	LWP_MutexLock(pasv_mutex);
	for (int tries = 0; tries <= PASV_PORT_MAX - PASV_PORT_MIN; tries++) {
		u16 port = next_pasv_port++;
		s32 sock;

		if (next_pasv_port > PASV_PORT_MAX)
			next_pasv_port = PASV_PORT_MIN;

		sock = net_listen_on(port, 1);
		if (sock >= 0) {
			LWP_MutexUnlock(pasv_mutex);
			s->pasv_listen = sock;
			s->port_mode = false;
			*port_out = port;
			return 0;
		}
	}
	LWP_MutexUnlock(pasv_mutex);
	return -1;
}

/* Establishes the data connection for the pending transfer command */
static s32 data_open(session_t *s)
{
	if (s->pasv_listen >= 0) {
		struct sockaddr_in addr;
		socklen_t addrlen = sizeof(addr);
		s32 data;

		if (net_wait_readable(s->pasv_listen, DATA_TIMEOUT_MS) <= 0) {
			close_pasv(s);
			return -1;
		}
		data = net_accept(s->pasv_listen, (struct sockaddr *)&addr, &addrlen);
		close_pasv(s);
		return data;
	}

	if (s->port_mode) {
		s32 data = net_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);

		s->port_mode = false;
		if (data < 0)
			return data;
		if (net_connect(data, (struct sockaddr *)&s->port_addr, sizeof(s->port_addr)) < 0) {
			net_close(data);
			return -1;
		}
		return data;
	}

	return -1;
}

/* Opens the data connection and announces it; on failure replies 425 */
static s32 data_begin(session_t *s, const char *what)
{
	s32 data = data_open(s);

	if (data < 0) {
		reply(s, "425 Can't open data connection.");
		return -1;
	}
	/* Never hold back the partial segment at the end of each write */
	u32 one = 1;
	net_setsockopt(data, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

	reply(s, "150 Opening BINARY mode data connection for %s.", what);
	return data;
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

/* Resolves arg against cwd into both the virtual and the real path.
 * Replies 550 and returns false when it does not map to a file system path. */
static bool resolve_real(session_t *s, const char *arg, char *vpath, char *real)
{
	if (!vfs_resolve(s->cwd, arg, vpath, VFS_PATH_MAX) ||
	    !vfs_to_real(vpath, real, REAL_PATH_MAX)) {
		reply(s, "550 Invalid path.");
		return false;
	}
	return true;
}

static void format_time(time_t t, char *out, size_t outsz)
{
	struct tm tm;
	time_t now = time(NULL);

	gmtime_r(&t, &tm);
	/* ls style: show the year instead of the time for old/future files */
	if (now - t > 180 * 24 * 3600 || t - now > 24 * 3600)
		snprintf(out, outsz, "%s %2d  %04d", months[tm.tm_mon], tm.tm_mday, tm.tm_year + 1900);
	else
		snprintf(out, outsz, "%s %2d %02d:%02d", months[tm.tm_mon], tm.tm_mday, tm.tm_hour, tm.tm_min);
}

static int format_entry(char *out, size_t outsz, const char *name, const struct stat *st, bool names_only)
{
	char date[32];

	if (names_only)
		return snprintf(out, outsz, "%s\r\n", name);

	format_time(st->st_mtime, date, sizeof(date));
	return snprintf(out, outsz, "%s 1 gc gc %10llu %s %s\r\n",
	                S_ISDIR(st->st_mode) ? "drwxr-xr-x" : "-rw-r--r--",
	                (unsigned long long)st->st_size, date, name);
}

/* Buffered writer for directory listings */
typedef struct {
	session_t *s;
	s32 data;
	int len;
	bool failed;
} listbuf_t;

static void list_flush(listbuf_t *lb)
{
	if (lb->len > 0 && !lb->failed && net_send_all(lb->data, lb->s->xfer, lb->len) < 0)
		lb->failed = true;
	lb->len = 0;
}

static void list_add(listbuf_t *lb, const char *name, const struct stat *st, bool names_only)
{
	int n;

	if (XFER_BUF_SIZE - lb->len < REAL_PATH_MAX + 128)
		list_flush(lb);
	n = format_entry((char *)lb->s->xfer + lb->len, XFER_BUF_SIZE - lb->len, name, st, names_only);
	if (n > 0)
		lb->len += n;
}

/* ------------------------------------------------------------------------ */
/* Commands                                                                 */
/* ------------------------------------------------------------------------ */

static void cmd_list(session_t *s, const char *arg, bool names_only)
{
	char vpath[VFS_PATH_MAX], real[REAL_PATH_MAX], child[REAL_PATH_MAX];
	listbuf_t lb = { s, -1, 0, false };
	struct stat st;
	DIR *dir = NULL;

	/* Skip ls-style flags such as "-la" that many clients send */
	while (arg && *arg == '-') {
		while (*arg && *arg != ' ')
			arg++;
		while (*arg == ' ')
			arg++;
	}

	if (!vfs_resolve(s->cwd, arg, vpath, sizeof(vpath))) {
		reply(s, "550 Invalid path.");
		return;
	}

	if (vfs_is_root(vpath)) {
		lb.data = data_begin(s, "directory listing");
		if (lb.data < 0)
			return;
		memset(&st, 0, sizeof(st));
		st.st_mode = S_IFDIR;
		st.st_mtime = time(NULL);
		for (int i = 0; i < storage_count(); i++)
			list_add(&lb, storage_name(i), &st, names_only);
		goto done;
	}

	if (!vfs_to_real(vpath, real, sizeof(real))) {
		reply(s, "550 No such file or directory.");
		return;
	}

	if (vfs_is_dir(vpath)) {
		dir = opendir(real);
		if (!dir) {
			reply(s, "550 Can't open directory.");
			return;
		}
	} else if (stat(real, &st) != 0) {
		reply(s, "550 No such file or directory.");
		return;
	}

	lb.data = data_begin(s, "directory listing");
	if (lb.data < 0) {
		if (dir)
			closedir(dir);
		return;
	}

	if (!dir) {
		const char *base = strrchr(vpath, '/');
		list_add(&lb, base ? base + 1 : vpath, &st, names_only);
		goto done;
	}

	for (struct dirent *de; !lb.failed && (de = readdir(dir)) != NULL;) {
		size_t rlen = strlen(real);

		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		int n = snprintf(child, sizeof(child), "%s%s%s", real,
		                 (rlen > 0 && real[rlen - 1] == '/') ? "" : "/", de->d_name);
		if (n < 0 || (size_t)n >= sizeof(child) || stat(child, &st) != 0)
			continue;
		list_add(&lb, de->d_name, &st, names_only);
	}
	closedir(dir);

done:
	list_flush(&lb);
	net_close(lb.data);
	if (lb.failed)
		reply(s, "426 Connection closed; transfer aborted.");
	else
		reply(s, "226 Transfer complete.");
}

static void log_transfer(session_t *s, const char *verb, const char *vpath, u64 bytes, u32 total_ms)
{
	const char *name = strrchr(vpath, '/');
	u32 kbps = total_ms ? (u32)(bytes * 1000 / 1024 / total_ms) : 0;

	ui_log("[%d] %s %s %lluKB %uKB/s", s->id, verb, name ? name + 1 : vpath,
	       (unsigned long long)(bytes / 1024), (unsigned)kbps);
}

/* Pipeline callbacks: the disk side runs in the xfer worker thread */
static s32 disk_read(void *ctx, u8 *buf, s32 cap)
{
	ssize_t n = read(*(int *)ctx, buf, cap);
	return n < 0 ? -1 : (s32)n;
}

static bool disk_write(void *ctx, const u8 *buf, s32 len)
{
	return write(*(int *)ctx, buf, len) == len;
}

static bool net_write_out(void *ctx, const u8 *buf, s32 len)
{
	return net_send_all(*(s32 *)ctx, buf, len) >= 0;
}

typedef struct {
	s32 sock;
	bool eof;
} net_in_t;

/* Fills a whole chunk before handing it to the disk thread. Each recv only
 * yields one TCP segment, so keep reading without a select() while data is
 * already queued. */
static s32 net_read_in(void *ctx, u8 *buf, s32 cap)
{
	net_in_t *in = ctx;
	s32 filled = 0;

	while (filled < cap && !in->eof) {
		s32 got = net_recv(in->sock, buf + filled, cap - filled, MSG_DONTWAIT);

		if (got == -EAGAIN || got == -EWOULDBLOCK) {
			if (net_wait_readable(in->sock, DATA_TIMEOUT_MS) <= 0)
				return -1;
			continue;
		}
		if (got < 0)
			return -1;
		if (got == 0)
			in->eof = true;
		filled += got;
	}
	return filled;
}

static void cmd_retr(session_t *s, const char *arg)
{
	char vpath[VFS_PATH_MAX], real[REAL_PATH_MAX];
	u64 offset = s->rest_offset, start;
	s64 total;
	s32 data;
	int fd;

	s->rest_offset = 0;
	if (!resolve_real(s, arg, vpath, real))
		return;
	if (vfs_is_dir(vpath)) {
		reply(s, "550 Not a regular file.");
		return;
	}

	fd = open(real, O_RDONLY);
	if (fd < 0) {
		reply(s, "550 Can't open file: %s.", strerror(errno));
		return;
	}
	if (offset && lseek(fd, offset, SEEK_SET) < 0) {
		close(fd);
		reply(s, "550 Can't seek to restart offset.");
		return;
	}

	data = data_begin(s, vpath);
	if (data < 0) {
		close(fd);
		return;
	}

	start = gettime();
	total = xfer_run(s->bufs, disk_read, &fd, net_write_out, &data, true);
	close(fd);
	net_close(data);

	if (total < 0) {
		reply(s, "426 Transfer aborted.");
		ui_log("[%d] RETR %s aborted", s->id, vpath);
	} else {
		reply(s, "226 Transfer complete.");
		log_transfer(s, "RETR", vpath, total, diff_msec(start, gettime()));
	}
}

static void cmd_stor(session_t *s, const char *arg, bool append)
{
	char vpath[VFS_PATH_MAX], real[REAL_PATH_MAX];
	u64 offset = s->rest_offset, start;
	s64 total;
	int fd, flags = O_WRONLY | O_CREAT;
	net_in_t in;
	s32 data;

	s->rest_offset = 0;
	if (!resolve_real(s, arg, vpath, real))
		return;

	if (append)
		flags |= O_APPEND;
	else if (offset == 0)
		flags |= O_TRUNC;

	fd = open(real, flags, 0666);
	if (fd < 0) {
		reply(s, "553 Can't create file: %s.", strerror(errno));
		return;
	}
	if (offset && !append && lseek(fd, offset, SEEK_SET) < 0) {
		close(fd);
		reply(s, "550 Can't seek to restart offset.");
		return;
	}

	data = data_begin(s, vpath);
	if (data < 0) {
		close(fd);
		return;
	}

	in.sock = data;
	in.eof = false;
	start = gettime();
	total = xfer_run(s->bufs, net_read_in, &in, disk_write, &fd, false);
	close(fd);
	net_close(data);

	if (total < 0) {
		reply(s, "426 Transfer aborted.");
		ui_log("[%d] STOR %s aborted", s->id, vpath);
	} else {
		reply(s, "226 Transfer complete.");
		log_transfer(s, append ? "APPE" : "STOR", vpath, total, diff_msec(start, gettime()));
	}
}

static void cmd_cwd(session_t *s, const char *arg)
{
	char vpath[VFS_PATH_MAX];

	if (!vfs_resolve(s->cwd, arg, vpath, sizeof(vpath)) || !vfs_is_dir(vpath)) {
		reply(s, "550 No such directory.");
		return;
	}
	strcpy(s->cwd, vpath);
	reply(s, "250 Directory changed to \"%s\".", s->cwd);
}

static void cmd_pasv(session_t *s, bool extended)
{
	u32 ip = net_gethostip();
	u16 port;

	if (open_pasv_listener(s, &port) < 0) {
		reply(s, "425 Can't open passive connection.");
		return;
	}
	if (extended)
		reply(s, "229 Entering Extended Passive Mode (|||%u|).", port);
	else
		reply(s, "227 Entering Passive Mode (%u,%u,%u,%u,%u,%u).",
		      (ip >> 24) & 0xff, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff,
		      port >> 8, port & 0xff);
}

static void cmd_port(session_t *s, const char *arg)
{
	unsigned h1, h2, h3, h4, p1, p2;

	if (sscanf(arg, "%u,%u,%u,%u,%u,%u", &h1, &h2, &h3, &h4, &p1, &p2) != 6 ||
	    h1 > 255 || h2 > 255 || h3 > 255 || h4 > 255 || p1 > 255 || p2 > 255) {
		reply(s, "501 Syntax error in PORT parameters.");
		return;
	}

	close_pasv(s);
	memset(&s->port_addr, 0, sizeof(s->port_addr));
	s->port_addr.sin_len = sizeof(s->port_addr);
	s->port_addr.sin_family = AF_INET;
	s->port_addr.sin_port = htons((p1 << 8) | p2);
	s->port_addr.sin_addr.s_addr = htonl((h1 << 24) | (h2 << 16) | (h3 << 8) | h4);
	s->port_mode = true;
	reply(s, "200 PORT command successful.");
}

static void cmd_user(session_t *s, const char *arg)
{
	s->logged_in = false;
	if (server_cfg.user[0] == '\0') {
		s->user_ok = true;
		reply(s, "331 Anonymous login ok, send any password.");
		return;
	}
	s->user_ok = strcmp(arg, server_cfg.user) == 0;
	reply(s, "331 Password required.");
}

static void cmd_pass(session_t *s, const char *arg)
{
	if (s->user_ok && (server_cfg.pass[0] == '\0' || strcmp(arg, server_cfg.pass) == 0)) {
		s->logged_in = true;
		reply(s, "230 Login successful.");
	} else {
		s->user_ok = false;
		reply(s, "530 Login incorrect.");
	}
}

/* Runs a command that operates on one resolved path */
typedef enum { OP_DELE, OP_MKD, OP_RMD, OP_SIZE, OP_MDTM, OP_RNFR, OP_RNTO } path_op_t;

static void cmd_path_op(session_t *s, path_op_t op, const char *arg)
{
	char vpath[VFS_PATH_MAX], real[REAL_PATH_MAX], from[REAL_PATH_MAX];
	struct stat st;
	struct tm tm;

	if (!*arg) {
		reply(s, "501 Missing argument.");
		return;
	}
	if (!resolve_real(s, arg, vpath, real))
		return;

	switch (op) {
	case OP_DELE:
		if (unlink(real) == 0)
			reply(s, "250 File deleted.");
		else
			reply(s, "550 Delete failed: %s.", strerror(errno));
		break;
	case OP_MKD:
		if (mkdir(real, 0777) == 0)
			reply(s, "257 \"%s\" created.", vpath);
		else
			reply(s, "550 Create directory failed: %s.", strerror(errno));
		break;
	case OP_RMD:
		if (rmdir(real) == 0)
			reply(s, "250 Directory removed.");
		else
			reply(s, "550 Remove directory failed: %s.", strerror(errno));
		break;
	case OP_SIZE:
		if (stat(real, &st) == 0 && !S_ISDIR(st.st_mode))
			reply(s, "213 %llu", (unsigned long long)st.st_size);
		else
			reply(s, "550 Could not get file size.");
		break;
	case OP_MDTM:
		if (stat(real, &st) == 0) {
			gmtime_r(&st.st_mtime, &tm);
			reply(s, "213 %04d%02d%02d%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1,
			      tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
		} else {
			reply(s, "550 Could not get modification time.");
		}
		break;
	case OP_RNFR:
		if (stat(real, &st) == 0) {
			strcpy(s->rename_from, vpath);
			reply(s, "350 Ready for RNTO.");
		} else {
			reply(s, "550 No such file or directory.");
		}
		break;
	case OP_RNTO:
		if (!s->rename_from[0]) {
			reply(s, "503 Send RNFR first.");
			break;
		}
		if (!vfs_to_real(s->rename_from, from, sizeof(from)))
			reply(s, "550 Invalid source path.");
		else if (rename(from, real) == 0)
			reply(s, "250 Rename successful.");
		else
			reply(s, "553 Rename failed: %s.", strerror(errno));
		s->rename_from[0] = '\0';
		break;
	}
}

/* Returns false when the session should end */
static bool handle_command(session_t *s, char *line)
{
	char *cmd = line, *arg;

	arg = strchr(line, ' ');
	if (arg) {
		*arg++ = '\0';
		while (*arg == ' ')
			arg++;
	} else {
		arg = line + strlen(line);
	}
	for (char *p = cmd; *p; p++)
		if (*p >= 'a' && *p <= 'z')
			*p -= 'a' - 'A';

	/* Commands allowed before login */
	if (!strcmp(cmd, "USER")) { cmd_user(s, arg); return true; }
	if (!strcmp(cmd, "PASS")) { cmd_pass(s, arg); return true; }
	if (!strcmp(cmd, "QUIT")) { reply(s, "221 Goodbye."); return false; }
	if (!strcmp(cmd, "SYST")) { reply(s, "215 UNIX Type: L8"); return true; }
	if (!strcmp(cmd, "NOOP")) { reply(s, "200 OK."); return true; }
	if (!strcmp(cmd, "AUTH")) { reply(s, "502 TLS not supported."); return true; }
	if (!strcmp(cmd, "OPTS")) { reply(s, "200 OK."); return true; }
	if (!strcmp(cmd, "FEAT")) {
		reply(s, "211-Features:\r\n SIZE\r\n MDTM\r\n REST STREAM\r\n PASV\r\n EPSV\r\n UTF8\r\n211 End");
		return true;
	}

	if (!s->logged_in) {
		reply(s, "530 Please login with USER and PASS.");
		return true;
	}

	if (!strcmp(cmd, "PWD") || !strcmp(cmd, "XPWD"))
		reply(s, "257 \"%s\" is the current directory.", s->cwd);
	else if (!strcmp(cmd, "CWD") || !strcmp(cmd, "XCWD"))
		cmd_cwd(s, arg);
	else if (!strcmp(cmd, "CDUP") || !strcmp(cmd, "XCUP"))
		cmd_cwd(s, "..");
	else if (!strcmp(cmd, "TYPE"))
		reply(s, "200 Type set to %s.", *arg ? arg : "I");
	else if (!strcmp(cmd, "MODE"))
		reply(s, (*arg == 'S' || *arg == 's') ? "200 Mode set to S." : "504 Only stream mode supported.");
	else if (!strcmp(cmd, "STRU"))
		reply(s, (*arg == 'F' || *arg == 'f') ? "200 Structure set to F." : "504 Only file structure supported.");
	else if (!strcmp(cmd, "PASV"))
		cmd_pasv(s, false);
	else if (!strcmp(cmd, "EPSV")) {
		if (!strcasecmp(arg, "ALL"))
			reply(s, "200 EPSV ALL ok.");
		else
			cmd_pasv(s, true);
	} else if (!strcmp(cmd, "PORT"))
		cmd_port(s, arg);
	else if (!strcmp(cmd, "LIST"))
		cmd_list(s, arg, false);
	else if (!strcmp(cmd, "NLST"))
		cmd_list(s, arg, true);
	else if (!strcmp(cmd, "RETR"))
		cmd_retr(s, arg);
	else if (!strcmp(cmd, "STOR"))
		cmd_stor(s, arg, false);
	else if (!strcmp(cmd, "APPE"))
		cmd_stor(s, arg, true);
	else if (!strcmp(cmd, "REST")) {
		s->rest_offset = strtoull(arg, NULL, 10);
		reply(s, "350 Restarting at %llu.", (unsigned long long)s->rest_offset);
	} else if (!strcmp(cmd, "DELE"))
		cmd_path_op(s, OP_DELE, arg);
	else if (!strcmp(cmd, "MKD") || !strcmp(cmd, "XMKD"))
		cmd_path_op(s, OP_MKD, arg);
	else if (!strcmp(cmd, "RMD") || !strcmp(cmd, "XRMD"))
		cmd_path_op(s, OP_RMD, arg);
	else if (!strcmp(cmd, "SIZE"))
		cmd_path_op(s, OP_SIZE, arg);
	else if (!strcmp(cmd, "MDTM"))
		cmd_path_op(s, OP_MDTM, arg);
	else if (!strcmp(cmd, "RNFR"))
		cmd_path_op(s, OP_RNFR, arg);
	else if (!strcmp(cmd, "RNTO"))
		cmd_path_op(s, OP_RNTO, arg);
	else if (!strcmp(cmd, "ALLO"))
		reply(s, "202 No storage allocation necessary.");
	else if (!strcmp(cmd, "ABOR"))
		reply(s, "226 No transfer to abort.");
	else if (!strcmp(cmd, "STAT"))
		reply(s, "211 " APP_NAME " status OK.");
	else if (!strcmp(cmd, "HELP"))
		reply(s, "214 See RFC 959.");
	else
		reply(s, "502 Command not implemented.");

	return true;
}

/* ------------------------------------------------------------------------ */
/* Threads                                                                  */
/* ------------------------------------------------------------------------ */

static void *session_main(void *arg)
{
	session_t *s = arg;
	char line[CMD_BUF_SIZE];

	reply(s, "220 " APP_NAME " " APP_VERSION " ready.");
	while (read_line(s, line, sizeof(line)) >= 0) {
		if (!handle_command(s, line))
			break;
	}

	close_pasv(s);
	net_close(s->ctrl);
	s->ctrl = -1;
	ui_log("[%d] Client disconnected", s->id);
	s->in_use = false;
	return NULL;
}

static session_t *claim_session(void)
{
	for (int i = 0; i < MAX_SESSIONS; i++) {
		session_t *s = &sessions[i];

		if (s->in_use)
			continue;
		/* Reap the previous thread that used this slot */
		if (s->thread != LWP_THREAD_NULL) {
			LWP_JoinThread(s->thread, NULL);
			s->thread = LWP_THREAD_NULL;
		}
		return s;
	}
	return NULL;
}

static void *listen_main(void *arg)
{
	for (;;) {
		struct sockaddr_in addr;
		socklen_t addrlen = sizeof(addr);
		session_t *s;
		s32 client;

		if (net_wait_readable(listen_sock, ACCEPT_POLL_MS) <= 0)
			continue;
		client = net_accept(listen_sock, (struct sockaddr *)&addr, &addrlen);
		if (client < 0)
			continue;

		s = claim_session();
		if (!s) {
			static const char busy[] = "421 Too many connections, try again later.\r\n";
			net_send_all(client, busy, sizeof(busy) - 1);
			net_close(client);
			continue;
		}

		s->ctrl = client;
		s->pasv_listen = -1;
		s->port_mode = false;
		s->user_ok = false;
		s->logged_in = false;
		s->rest_offset = 0;
		s->rlen = 0;
		s->rename_from[0] = '\0';
		strcpy(s->cwd, "/");
		s->in_use = true;

		ui_log("[%d] Client connected from %s", s->id, inet_ntoa(addr.sin_addr));
		if (LWP_CreateThread(&s->thread, session_main, s, NULL, THREAD_STACK, LWP_PRIO_NORMAL) != 0) {
			s->thread = LWP_THREAD_NULL;
			net_close(client);
			s->in_use = false;
		}
	}
	return NULL;
}

s32 ftp_start(const config_t *cfg)
{
	server_cfg = *cfg;

	for (int i = 0; i < MAX_SESSIONS; i++) {
		sessions[i].id = i + 1;
		sessions[i].in_use = false;
		sessions[i].thread = LWP_THREAD_NULL;
		sessions[i].ctrl = -1;
		sessions[i].pasv_listen = -1;
		for (int b = 0; b < XFER_NBUF; b++)
			sessions[i].bufs[b] = xfer_bufs[i][b];
		sessions[i].xfer = sessions[i].bufs[0];
	}
	LWP_MutexInit(&pasv_mutex, false);

	listen_sock = net_listen_on(cfg->port, MAX_SESSIONS);
	if (listen_sock < 0)
		return listen_sock;

	if (LWP_CreateThread(&listen_thread, listen_main, NULL, NULL, THREAD_STACK, LWP_PRIO_NORMAL) != 0) {
		net_close(listen_sock);
		listen_sock = -1;
		return -1;
	}
	return 0;
}
