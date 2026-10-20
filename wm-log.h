#ifndef WM_LOG_H
#define WM_LOG_H
#include "diagnostics.h"

void log_init(void);
void log_close(void);
void log_flush(void);
int md_verbose(void);
void md_log(const char *level, const char *fmt, ...);

#define LOG_INFO(...)  do { if (md_diagnostic_level() >= 2) md_log("INFO ", __VA_ARGS__); } while (0)
#define LOG_WARN(...)  md_log("WARN ", __VA_ARGS__)
#define LOG_EVENT(...) do { if (md_diagnostic_level() >= 3) md_log("EVENT", __VA_ARGS__); } while (0)
#define LOG_STATE(...) do { if (md_diagnostic_level() >= 3) md_log("STATE", __VA_ARGS__); } while (0)
#define LOG_TRACE(...) do { if (md_diagnostic_level() >= 4) md_log("TRACE", __VA_ARGS__); } while (0)

#endif /* WM_LOG_H */
