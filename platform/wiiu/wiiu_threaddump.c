// Debug tool, not built by default (add it to SCsub and call it).
// Written for the HOME hang hunt (2026-11-13): logs every thread of this process with
// where it is (PC, LR, a short backtrace with Cafe OS symbols) and what it waits
// on (mutex and its owner, thread queue). Called from the audio thread's
// background heartbeat, which keeps running while the main thread is stuck in
// ProcUI's release, so a hang leaves the stuck thread's location in the log.
// Addresses in Godot resolve with
//   powerpc-eabi-addr2line -f -C -e bin/godot.wiiu.template_debug.ppc32.elf <addr>
// (subtract the load offset the crash log prints, if any).

#include <coreinit/debug.h>
#include <coreinit/memorymap.h>
#include <coreinit/mutex.h>
#include <coreinit/thread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <whb/log.h>

#define DUMP_MAX_THREADS 64
#define DUMP_MAX_FRAMES 10

static bool readable(const void *p_ptr, uint32_t p_size) {
	const uint32_t a = (uint32_t)p_ptr;
	return a != 0 && OSIsAddressValid(a) && OSIsAddressValid(a + p_size - 1);
}

static const char *safe_name(const char *p_name) {
	if (!readable(p_name, 1)) {
		return "?";
	}
	return p_name;
}

static void symbol_of(uint32_t p_addr, char *r_buf, int p_size) {
	r_buf[0] = '\0';
	if (p_addr != 0 && OSIsAddressValid(p_addr)) {
		OSGetSymbolName(p_addr, r_buf, p_size);
	}
}

static void dump_thread(int p_index, OSThread *p_thread, OSThread *p_self) {
	char sym_pc[80];
	char sym_lr[80];
	symbol_of(p_thread->context.srr0, sym_pc, sizeof(sym_pc));
	symbol_of(p_thread->context.lr, sym_lr, sizeof(sym_lr));
	WHBLogPrintf("[threads] #%d %p \"%s\" state %u prio %d/%d attr %02x suspend %d%s",
			p_index, (void *)p_thread, safe_name(p_thread->name), (unsigned)p_thread->state,
			(int)p_thread->priority, (int)p_thread->basePriority, (unsigned)p_thread->attr,
			(int)p_thread->suspendCounter, p_thread == p_self ? " (this, dumping)" : "");
	if (p_thread == p_self) {
		return;
	}
	WHBLogPrintf("[threads]    pc %08x %s | lr %08x %s", (unsigned)p_thread->context.srr0, sym_pc,
			(unsigned)p_thread->context.lr, sym_lr);
	OSMutex *m = p_thread->mutex;
	if (m != NULL && readable(m, sizeof(OSMutex))) {
		OSThread *owner = m->owner;
		WHBLogPrintf("[threads]    waits on mutex %p \"%s\" owned by %p \"%s\"", (void *)m, safe_name(m->name),
				(void *)owner, readable(owner, sizeof(OSThread)) ? safe_name(owner->name) : "?");
	}
	if (p_thread->queue != NULL) {
		WHBLogPrintf("[threads]    waits on queue %p", (void *)p_thread->queue);
	}
	// Back chain within the thread's own stack.
	uint32_t lo = (uint32_t)p_thread->stackEnd;
	uint32_t hi = (uint32_t)p_thread->stackStart;
	if (lo > hi) {
		const uint32_t t = lo;
		lo = hi;
		hi = t;
	}
	uint32_t sp = p_thread->context.gpr[1];
	for (int i = 0; i < DUMP_MAX_FRAMES; i++) {
		if (sp < lo || sp + 8 > hi || (sp & 3) != 0 || !readable((void *)sp, 4)) {
			break;
		}
		const uint32_t next = *(uint32_t *)sp;
		if (next <= sp || next + 8 > hi || (next & 3) != 0 || !readable((void *)next, 8)) {
			break;
		}
		const uint32_t ret = *(uint32_t *)(next + 4);
		char sym[80];
		symbol_of(ret, sym, sizeof(sym));
		WHBLogPrintf("[threads]      #%d %08x %s", i, (unsigned)ret, sym);
		sp = next;
	}
}

void wiiu_thread_dump(const char *p_reason) {
	OSThread *self = OSGetCurrentThread();
	WHBLogPrintf("[threads] dump (%s); main thread %p", p_reason, (void *)OSGetDefaultThread(1));
	// Rewind to the head of the active thread list, then walk it.
	OSThread *t = self;
	for (int i = 0; i < DUMP_MAX_THREADS && readable(t->activeLink.prev, sizeof(OSThread)); i++) {
		t = t->activeLink.prev;
		if (t == self) {
			break;
		}
	}
	OSThread *start = t;
	for (int i = 0; i < DUMP_MAX_THREADS && t != NULL; i++) {
		if (!readable(t, sizeof(OSThread)) || t->tag != OS_THREAD_TAG) {
			WHBLogPrintf("[threads] #%d %p: not a thread, stopping", i, (void *)t);
			break;
		}
		dump_thread(i, t, self);
		t = t->activeLink.next;
		if (t == start) {
			break;
		}
	}
	WHBLogPrint("[threads] end");
}
