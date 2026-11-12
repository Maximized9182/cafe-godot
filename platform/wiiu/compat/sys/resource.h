#pragma once
// newlib's <sys/resource.h> has no rlimit API; add a stub that reports "unknown".

#include_next <sys/resource.h>

#define RLIMIT_STACK 3

typedef unsigned long rlim_t;
struct rlimit {
	rlim_t rlim_cur;
	rlim_t rlim_max;
};

static inline int getrlimit(int, struct rlimit *r_limit) {
	r_limit->rlim_cur = 0;
	r_limit->rlim_max = 0;
	return -1;
}
