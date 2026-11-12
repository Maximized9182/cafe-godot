#pragma once
// The Wii U filesystems have no extended attributes; every call reports ENOTSUP.

#include <cerrno>
#include <sys/types.h>

static inline ssize_t getxattr(const char *, const char *, void *, size_t) { errno = ENOTSUP; return -1; }
static inline int setxattr(const char *, const char *, const void *, size_t, int) { errno = ENOTSUP; return -1; }
static inline int removexattr(const char *, const char *) { errno = ENOTSUP; return -1; }
static inline ssize_t listxattr(const char *, char *, size_t) { errno = ENOTSUP; return -1; }
