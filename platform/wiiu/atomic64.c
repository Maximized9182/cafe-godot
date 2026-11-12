// 64-bit atomic helpers for 32-bit PowerPC.
//
// The Espresso CPU has no 64-bit load-reserve/store-conditional, so GCC lowers
// std::atomic<uint64_t> to libatomic calls (__atomic_*_8). devkitPPC ships no
// libatomic, so they're implemented here with a small table of spinlocks keyed
// by address. Cafe OS's uninterruptible spinlocks are used so a thread holding
// a lock can't be preempted by a higher-priority thread spinning on the same
// core. A zero-filled OSSpinLock is the same as one set up by OSInitSpinLock.

#include <coreinit/spinlock.h>

#include <stdbool.h>
#include <stdint.h>

#define LOCK_COUNT 64

static OSSpinLock locks[LOCK_COUNT];

static inline OSSpinLock *lock_for(const volatile void *p_ptr) {
	uintptr_t addr = (uintptr_t)p_ptr;
	return &locks[(addr >> 3) % LOCK_COUNT];
}

static inline void lock(OSSpinLock *p_lock) {
	OSUninterruptibleSpinLock_Acquire(p_lock);
}

static inline void unlock(OSSpinLock *p_lock) {
	OSUninterruptibleSpinLock_Release(p_lock);
}

// Defined under asm names so GCC doesn't treat them as redeclared builtins.
uint64_t wiiu_atomic_load_8(const volatile void *p_ptr, int p_order) __asm__("__atomic_load_8");
void wiiu_atomic_store_8(volatile void *p_ptr, uint64_t p_val, int p_order) __asm__("__atomic_store_8");
uint64_t wiiu_atomic_exchange_8(volatile void *p_ptr, uint64_t p_val, int p_order) __asm__("__atomic_exchange_8");
bool wiiu_atomic_compare_exchange_8(volatile void *p_ptr, void *p_expected, uint64_t p_desired, bool p_weak, int p_success, int p_failure) __asm__("__atomic_compare_exchange_8");

uint64_t wiiu_atomic_load_8(const volatile void *p_ptr, int p_order) {
	(void)p_order;
	OSSpinLock *l = lock_for(p_ptr);
	lock(l);
	uint64_t v = *(const volatile uint64_t *)p_ptr;
	unlock(l);
	return v;
}

void wiiu_atomic_store_8(volatile void *p_ptr, uint64_t p_val, int p_order) {
	(void)p_order;
	OSSpinLock *l = lock_for(p_ptr);
	lock(l);
	*(volatile uint64_t *)p_ptr = p_val;
	unlock(l);
}

uint64_t wiiu_atomic_exchange_8(volatile void *p_ptr, uint64_t p_val, int p_order) {
	(void)p_order;
	OSSpinLock *l = lock_for(p_ptr);
	lock(l);
	uint64_t old = *(volatile uint64_t *)p_ptr;
	*(volatile uint64_t *)p_ptr = p_val;
	unlock(l);
	return old;
}

bool wiiu_atomic_compare_exchange_8(volatile void *p_ptr, void *p_expected, uint64_t p_desired, bool p_weak, int p_success, int p_failure) {
	(void)p_weak;
	(void)p_success;
	(void)p_failure;
	OSSpinLock *l = lock_for(p_ptr);
	lock(l);
	uint64_t cur = *(volatile uint64_t *)p_ptr;
	bool ok = cur == *(uint64_t *)p_expected;
	if (ok) {
		*(volatile uint64_t *)p_ptr = p_desired;
	} else {
		*(uint64_t *)p_expected = cur;
	}
	unlock(l);
	return ok;
}

#define WIIU_FETCH_OP(m_name, m_op)                                                                      \
	uint64_t wiiu_atomic_fetch_##m_name##_8(volatile void *p_ptr, uint64_t p_val, int p_order)          \
			__asm__("__atomic_fetch_" #m_name "_8");                                                     \
	uint64_t wiiu_atomic_##m_name##_fetch_8(volatile void *p_ptr, uint64_t p_val, int p_order)          \
			__asm__("__atomic_" #m_name "_fetch_8");                                                     \
	uint64_t wiiu_atomic_fetch_##m_name##_8(volatile void *p_ptr, uint64_t p_val, int p_order) {        \
		(void)p_order;                                                                                  \
		OSSpinLock *l = lock_for(p_ptr);                                                         \
		lock(l);                                                                                        \
		uint64_t old = *(volatile uint64_t *)p_ptr;                                                     \
		*(volatile uint64_t *)p_ptr = m_op;                                                             \
		unlock(l);                                                                                      \
		return old;                                                                                     \
	}                                                                                                   \
	uint64_t wiiu_atomic_##m_name##_fetch_8(volatile void *p_ptr, uint64_t p_val, int p_order) {        \
		(void)p_order;                                                                                  \
		OSSpinLock *l = lock_for(p_ptr);                                                         \
		lock(l);                                                                                        \
		uint64_t old = *(volatile uint64_t *)p_ptr;                                                     \
		uint64_t res = m_op;                                                                            \
		*(volatile uint64_t *)p_ptr = res;                                                              \
		unlock(l);                                                                                      \
		return res;                                                                                     \
	}

WIIU_FETCH_OP(add, old + p_val)
WIIU_FETCH_OP(sub, old - p_val)
WIIU_FETCH_OP(and, old & p_val)
WIIU_FETCH_OP(or, old | p_val)
WIIU_FETCH_OP(xor, old ^ p_val)
WIIU_FETCH_OP(nand, ~(old & p_val))
