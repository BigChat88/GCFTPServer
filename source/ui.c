#include <gccore.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ui.h"

/* Overscan margins; X must keep the framebuffer pointer 4-byte aligned */
#define MARGIN_X 24
#define MARGIN_Y 24
#define FONT_HEIGHT 16
#define MIN_LOG_ROWS 4

static void *xfb = NULL;
static GXRModeObj *rmode = NULL;
static mutex_t log_mutex = LWP_MUTEX_NULL;

/* CON_Init ignores its x/y origin when drawing, so place the console by
 * offsetting the framebuffer pointer instead. Rows above the console area
 * are never touched by it, scrolling included. */
static void console_at(int top_row, int rows)
{
	u32 stride = rmode->fbWidth * VI_DISPLAY_PIX_SZ;
	u8 *origin = (u8 *)xfb + (MARGIN_Y + top_row * FONT_HEIGHT) * stride + MARGIN_X * VI_DISPLAY_PIX_SZ;

	CON_Init(origin, 0, 0, rmode->fbWidth - 2 * MARGIN_X, rows * FONT_HEIGHT, stride);
}

static int total_rows(void)
{
	return (rmode->xfbHeight - 2 * MARGIN_Y) / FONT_HEIGHT;
}

void ui_init(void)
{
	VIDEO_Init();
	PAD_Init();

	rmode = VIDEO_GetPreferredMode(NULL);
	xfb = SYS_AllocateFramebuffer(rmode);
	/* A fresh framebuffer is all zeros, which is green in YUV */
	VIDEO_ClearFrameBuffer(rmode, xfb, COLOR_BLACK);
	console_at(0, total_rows());

	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(xfb);
	VIDEO_SetBlack(false);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if (rmode->viTVMode & VI_NON_INTERLACE)
		VIDEO_WaitVSync();

	LWP_MutexInit(&log_mutex, false);
}

void ui_begin_log(void)
{
	char rule[128];
	int col, row, cols, rows;

	LWP_MutexLock(log_mutex);
	CON_GetPosition(&col, &row);
	CON_GetMetrics(&cols, &rows);
	if (col > 0)
		row++;

	if (total_rows() - (row + 1) >= MIN_LOG_ROWS) {
		if (cols > (int)sizeof(rule) - 1)
			cols = sizeof(rule) - 1;
		memset(rule, '-', cols);
		rule[cols] = '\0';
		printf("\x1b[%d;0H%s", row, rule);
		console_at(row + 1, total_rows() - (row + 1));
	}
	LWP_MutexUnlock(log_mutex);
}

void ui_log(const char *fmt, ...)
{
	va_list ap;

	LWP_MutexLock(log_mutex);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
	LWP_MutexUnlock(log_mutex);
}

void ui_shutdown(void)
{
	/* Keep the TV from showing memory being overwritten during the reset */
	VIDEO_SetBlack(true);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	VIDEO_WaitVSync();
}
