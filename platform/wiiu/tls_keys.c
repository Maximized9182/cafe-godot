// pthread thread-specific data keys for Cafe OS.
//
// devkitPPC's newlib implements pthread_key_create/setspecific/getspecific/
// key_delete by calling weak __syscall_tls_* hooks, and wut (1.9.1) doesn't
// provide them, so pthread_key_create returns ENOSYS. libgcc's emulated TLS
// (Clang's -femulated-tls, used for every thread_local in Godot) creates one
// such key on first use and calls abort() if that fails.
//
// Each key maps directly onto one Cafe OS thread-specific slot
// (OSSetThreadSpecific/OSGetThreadSpecific). Slots 0-13 belong to the
// application; 14 and 15 are reserved by wut. A new thread's slots start at 0,
// which matches the NULL initial value pthread requires.
//
// Limitation: destructors are recorded but not run at thread exit, so each
// exiting thread leaks its emutls block. Godot's threads are mostly
// long-lived (main thread, worker pool), so this is acceptable for now.

#include <coreinit/spinlock.h>
#include <coreinit/thread.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#define TLS_KEY_COUNT (OS_THREAD_SPECIFIC_13 + 1)

static OSSpinLock tls_lock; // Zero-filled equals OSInitSpinLock.
static bool tls_used[TLS_KEY_COUNT];
static void (*tls_destructors[TLS_KEY_COUNT])(void *);

int __syscall_tls_create(uint32_t *key, void (*destructor)(void *)) {
	int result = EAGAIN;
	OSUninterruptibleSpinLock_Acquire(&tls_lock);
	for (uint32_t i = 0; i < TLS_KEY_COUNT; i++) {
		if (!tls_used[i]) {
			tls_used[i] = true;
			tls_destructors[i] = destructor;
			// The key may have been used and deleted before; this thread's
			// stale value must not leak into the new key.
			OSSetThreadSpecific((OSThreadSpecificID)i, NULL);
			*key = i;
			result = 0;
			break;
		}
	}
	OSUninterruptibleSpinLock_Release(&tls_lock);
	return result;
}

int __syscall_tls_set(uint32_t key, const void *value) {
	if (key >= TLS_KEY_COUNT || !tls_used[key]) {
		return EINVAL;
	}
	OSSetThreadSpecific((OSThreadSpecificID)key, (void *)value);
	return 0;
}

void *__syscall_tls_get(uint32_t key) {
	if (key >= TLS_KEY_COUNT) {
		return NULL;
	}
	return OSGetThreadSpecific((OSThreadSpecificID)key);
}

int __syscall_tls_delete(uint32_t key) {
	if (key >= TLS_KEY_COUNT) {
		return EINVAL;
	}
	int result = EINVAL;
	OSUninterruptibleSpinLock_Acquire(&tls_lock);
	if (tls_used[key]) {
		tls_used[key] = false;
		tls_destructors[key] = NULL;
		result = 0;
	}
	OSUninterruptibleSpinLock_Release(&tls_lock);
	return result;
}
