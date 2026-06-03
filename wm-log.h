#ifndef WM_LOG_H
#define WM_LOG_H

void log_init(void);
int md_verbose(void);
void md_log(const char *level, const char *fmt, ...);

#define LOG_INFO(...)  md_log("INFO ", __VA_ARGS__)
#define LOG_WARN(...)  md_log("WARN ", __VA_ARGS__)
#define LOG_EVENT(...) md_log("EVENT", __VA_ARGS__)
#define LOG_STATE(...) md_log("STATE", __VA_ARGS__)

#endif /* WM_LOG_H */
