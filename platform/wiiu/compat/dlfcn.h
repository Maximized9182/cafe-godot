#pragma once
// Cafe OS has no dlopen-style dynamic loading; these stubs always fail.

#define RTLD_LAZY 0x1
#define RTLD_NOW 0x2
#define RTLD_GLOBAL 0x100
#define RTLD_LOCAL 0
#define RTLD_NEXT ((void *)-1)
#define RTLD_DEFAULT ((void *)0)

static inline void *dlopen(const char *, int) { return nullptr; }
static inline void *dlsym(void *, const char *) { return nullptr; }
static inline int dlclose(void *) { return -1; }
static inline char *dlerror() { return (char *)"dynamic libraries are not supported on Wii U"; }
