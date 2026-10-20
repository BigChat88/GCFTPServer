#ifndef GCFTP_UI_H
#define GCFTP_UI_H

void ui_init(void);
/* Thread-safe printf to the on-screen console. */
void ui_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Freezes everything printed so far as a header; later output scrolls below it */
void ui_begin_log(void);
/* Blanks the video output before exiting */
void ui_shutdown(void);

#endif
