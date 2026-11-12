#pragma once
// No syslog on Cafe OS; engine output goes through WiiULogger instead.

#include <cstdarg>

#define LOG_ERR 3
#define LOG_WARNING 4
#define LOG_INFO 6

static inline void openlog(const char *, int, int) {}
static inline void closelog() {}
static inline void syslog(int, const char *, ...) {}
static inline void vsyslog(int, const char *, va_list) {}
