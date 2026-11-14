// Crash screen and diagnostics for the Wii U.
//
// The log console is black while the game runs and turns red when Godot
// crashes. Two kinds of crash end up on the red screen:
//
// - abort(): wrapped at link time (-Wl,--wrap=abort), so every caller,
//   including libstdc++, newlib and libgcc, lands in __wrap_abort.
// - CPU exceptions (DSI = bad data access, ISI = bad instruction fetch,
//   PROGRAM = illegal instruction or trap, e.g. __builtin_trap): caught with
//   OSSetExceptionCallbackEx. The callback saves the context and resumes the
//   crashed thread in crash_exception_entry() on a private stack.
//
// On the main thread (the default thread of core 1, which runs main() and
// owns ProcUI) the red screen stays up until HOME, then the app exits like a
// normal one, so the console returns to the Wii U menu. A crash on any other
// thread draws the red screen, parks that thread and asks the main thread to
// take over (wiiu_crash_check(), called from the main loop); if the main
// thread is stuck, the red screen still shows but HOME may not respond.
//
// Backtraces print each return address with its symbol (OSGetSymbolName).
// To map an address to a source line, subtract the load offset (runtime
// address of main minus its link address from `powerpc-eabi-nm`) and run:
//   powerpc-eabi-addr2line -f -C -e bin/godot.wiiu.template_debug.ppc32.elf <addr>
//
// malloc/calloc/realloc/free and _sbrk_r are also wrapped for heap diagnostics
// (allocation logging, sbrk trace and a heap consistency check).

#include <coreinit/context.h>
#include <coreinit/debug.h>
#include <coreinit/exception.h>
#include <coreinit/memexpheap.h>
#include <coreinit/memheap.h>
#include <coreinit/memory.h>
#include <coreinit/systeminfo.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <errno.h>
#include <malloc.h>
#include <reent.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <proc_ui/procui.h>
#include <sysapp/launch.h>
#include <vpad/input.h>
#include <whb/log.h>
#include <whb/proc.h>

extern int main(int argc, char **argv);
extern void __real_abort(void) __attribute__((noreturn));

// wiiu_console.c
void wiiu_console_set_color(uint32_t color);
void wiiu_console_enter_crash_mode(uint32_t color);
void wiiu_console_update_input(void);
void wiiu_console_draw(void);
void wiiu_console_free(void);
void wiiu_console_resume(void);
void wiiu_console_set_exit_footer(void);
void wiiu_console_handle_buttons(uint32_t hold, uint32_t trigger);

// wiiu_console.c (GX2 log view)
uint32_t wiiu_console_copy_view(uint32_t p_rows, char (*p_out)[69], uint32_t *r_bottom, uint32_t *r_total);
const char *wiiu_console_get_footer(void);

#include "wiiu_gx2.h"

// Console background colours, 0xRRGGBBAA.
#define CONSOLE_COLOR_NORMAL 0x00000000u
#define CONSOLE_COLOR_CRASH 0xA00000FFu

#define CRASH_MAX_FRAMES 32
#define CRASH_STACK_SIZE (64 * 1024)

// ---------------------------------------------------------------------------
// Allocation-free logging
// ---------------------------------------------------------------------------

// WHBLogPrintf and WHBLogPrint both allocate a 2048-byte line buffer from the
// default heap (which wut routes into newlib malloc), so they print nothing
// when memory runs out. WHBLogWrite doesn't allocate, so this formats into a
// static buffer, adds the newline itself and calls WHBLogWrite.
// Not thread-safe; it's only for diagnostics.
static char log_buffer[256];

static void log_static(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void log_static(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	int len = vsnprintf(log_buffer, sizeof(log_buffer) - 1, fmt, args);
	va_end(args);
	if (len < 0) {
		len = 0;
	} else if (len > (int)sizeof(log_buffer) - 2) {
		len = (int)sizeof(log_buffer) - 2;
	}
	log_buffer[len] = '\n';
	log_buffer[len + 1] = '\0';
	WHBLogWrite(log_buffer);
}

// ---------------------------------------------------------------------------
// Heap diagnostics
// ---------------------------------------------------------------------------

extern void *__real_malloc(size_t size);
extern void *__real_calloc(size_t count, size_t size);
extern void *__real_realloc(void *ptr, size_t size);
extern void *__real__sbrk_r(struct _reent *reent, ptrdiff_t incr);

// newlib's mallocr state: the top chunk (the one malloc grows with sbrk) is
// bin_at(0)->fd, i.e. __malloc_av_[2] (av_[0] and av_[1] are padding). Before
// the first sbrk it points at bin_at(0) itself ("initial_top", =&av_[0]).
// A chunk's size word is at offset 4; the low 2 bits are flags.
extern uint8_t *__malloc_av_[];
extern char *__malloc_sbrk_base;
extern unsigned long __malloc_top_pad;
extern unsigned long __malloc_max_sbrked_mem;

#define LARGE_ALLOC_BYTES (1024 * 1024)

static void log_address(const char *label, uint32_t addr);
static void log_backtrace(uintptr_t sp);

// Heap consistency check. In newlib's mallocr the top chunk always ends at
// the program break (top + size == sbrk(0)) unless someone else moved the
// break. When something writes past the end of the block just below top, it
// overwrites top's size word; malloc then thinks the break was moved by a
// "foreign" sbrk, and if the old top is now tiny it forces malloc() to
// return NULL without setting errno. heap_check() runs on entry to and exit
// from every malloc/calloc/realloc/free and reports the first time top no
// longer ends at the break, with the most recent allocations (the culprit is
// usually the one whose block ends right at top).

#define HEAP_RECENT_ALLOCS 16
#define HEAP_CALLER_FRAMES 6

typedef struct {
	void *ptr;
	uint32_t size;
	const char *func;
	uint32_t callers[HEAP_CALLER_FRAMES];
} HeapAllocRecord;

static HeapAllocRecord heap_recent[HEAP_RECENT_ALLOCS];
// Callers of the previous heap_check that passed: the overrun happened
// between that call and the one that detected it.
static uint32_t heap_last_ok_callers[HEAP_CALLER_FRAMES];
static const char *heap_last_ok_where = NULL;
static uint32_t heap_recent_count = 0;
static bool heap_corruption_reported = false;
// Reports wait until the console and log handlers exist (set by
// wiiu_crash_screen_init); before that the ring still records.
static bool heap_reporting_enabled = false;
static uint32_t heap_check_count = 0;

// Saves the first HEAP_CALLER_FRAMES return addresses above the wrapper, so
// the record names the code that called Memory::alloc_static etc.
static void heap_capture_callers(uint32_t *out) {
	uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
	int skip = 1; // heap_capture_callers' own caller is the wrapper.
	int n = 0;
	for (int i = 0; i < HEAP_CALLER_FRAMES + skip + 1 && n < HEAP_CALLER_FRAMES; i++) {
		if (sp == 0 || (sp & 3) != 0) {
			break;
		}
		uintptr_t next = *(uintptr_t *)sp;
		if (next == 0 || next <= sp || (next & 3) != 0) {
			break;
		}
		if (i >= skip) {
			out[n++] = *(uint32_t *)(next + 4);
		}
		sp = next;
	}
	while (n < HEAP_CALLER_FRAMES) {
		out[n++] = 0;
	}
}

static void heap_record(const char *func, void *ptr, size_t size) {
	if (ptr == NULL) {
		return;
	}
	HeapAllocRecord *r = &heap_recent[heap_recent_count % HEAP_RECENT_ALLOCS];
	r->ptr = ptr;
	r->size = (uint32_t)size;
	r->func = func;
	heap_capture_callers(r->callers);
	heap_recent_count++;
}

// Walks the chunk chain from the start of the sbrk heap to top and logs the
// chunk right below top (the usual culprit) or where the chain breaks.
static void heap_walk_to_top(uint8_t *top) {
	uint8_t *chunk = (uint8_t *)__malloc_sbrk_base;
	if (chunk == NULL || chunk == (uint8_t *)-1) {
		log_static("[heapcheck]   no sbrk base, can't walk the heap");
		return;
	}
	// mallocr aligns the first chunk so that chunk + 8 is 8-byte aligned.
	while (((uintptr_t)chunk + 8) & 7) {
		chunk++;
	}
	uint8_t *prev = NULL;
	uint32_t count = 0;
	while (chunk < top) {
		uint32_t size = *(uint32_t *)(chunk + 4) & ~3u;
		if (size < 16 || (size & 7) != 0 || chunk + size > top) {
			log_static("[heapcheck]   chain breaks at chunk #%u %p: size word %08x (prev chunk %p)",
					(unsigned)count, (void *)chunk, (unsigned)*(uint32_t *)(chunk + 4), (void *)prev);
			return;
		}
		prev = chunk;
		chunk += size;
		count++;
	}
	if (chunk != top) {
		log_static("[heapcheck]   walk overshot top: ended at %p after %u chunks", (void *)chunk,
				(unsigned)count);
		return;
	}
	log_static("[heapcheck]   walk reached top after %u chunks; chunk below top %p, size word %08x",
			(unsigned)count, (void *)prev, prev ? (unsigned)*(uint32_t *)(prev + 4) : 0u);
	if (prev == NULL) {
		return;
	}
	uint8_t *user = prev + 8;
	uint32_t n = heap_recent_count < HEAP_RECENT_ALLOCS ? heap_recent_count : HEAP_RECENT_ALLOCS;
	for (uint32_t i = 0; i < n; i++) {
		HeapAllocRecord *r = &heap_recent[(heap_recent_count - 1 - i) % HEAP_RECENT_ALLOCS];
		if ((uint8_t *)r->ptr == user) {
			log_static("[heapcheck]   it's %s(%u) = %p, %u allocations ago:", r->func,
					(unsigned)r->size, r->ptr, (unsigned)i);
			for (int f = 0; f < HEAP_CALLER_FRAMES && r->callers[f] != 0; f++) {
				log_address("[heapcheck]       ", r->callers[f]);
			}
			return;
		}
	}
	log_static("[heapcheck]   it's not among the last %u recorded allocations", (unsigned)n);
}

// Logs 64 bytes below top (and top's header) as hex and ASCII: the culprit's
// tail data usually shows what kind of object overran.
static void heap_dump_below_top(uint8_t *top) {
	for (int row = -64; row < 16; row += 16) {
		uint8_t *b = top + row;
		char ascii[17];
		for (int i = 0; i < 16; i++) {
			ascii[i] = (b[i] >= 32 && b[i] < 127) ? (char)b[i] : '.';
		}
		ascii[16] = '\0';
		log_static("[heapcheck]   top%+03d: %08x %08x %08x %08x  %s", row, (unsigned)((uint32_t *)b)[0],
				(unsigned)((uint32_t *)b)[1], (unsigned)((uint32_t *)b)[2], (unsigned)((uint32_t *)b)[3],
				ascii);
	}
}

// newlib's malloc lock (recursive), so the check sees a consistent heap while
// other threads allocate.
extern void __malloc_lock(struct _reent *reent);
extern void __malloc_unlock(struct _reent *reent);

static void heap_check_locked(const char *where, void *caller);

static void heap_check(const char *where, void *caller) {
	if (heap_corruption_reported || !heap_reporting_enabled) {
		return;
	}
	__malloc_lock(_REENT);
	heap_check_locked(where, caller);
	__malloc_unlock(_REENT);
}

static void heap_check_locked(const char *where, void *caller) {
	if (heap_corruption_reported) {
		return;
	}
	heap_check_count++;
	uint8_t *top = __malloc_av_[2];
	if (top == NULL || top == (uint8_t *)&__malloc_av_[0]) {
		return; // No heap yet.
	}
	uint32_t top_size = *(uint32_t *)(top + 4) & ~3u;
	uint8_t *top_end = top + top_size;
	uint8_t *brk = (uint8_t *)sbrk(0);
	if (top_end == brk) {
		heap_last_ok_where = where;
		heap_capture_callers(heap_last_ok_callers);
		return;
	}
	heap_corruption_reported = true;
	log_static("[heapcheck] top chunk no longer ends at the break (check #%u, %s):",
			(unsigned)heap_check_count, where);
	log_address("[heapcheck]   detected in call from ", (uint32_t)(uintptr_t)caller);
	log_static("[heapcheck]   backtrace of the detecting call:");
	log_backtrace((uintptr_t)__builtin_frame_address(0));
	log_static("[heapcheck]   previous good check (%s) was called from:",
			heap_last_ok_where ? heap_last_ok_where : "none");
	for (int f = 0; f < HEAP_CALLER_FRAMES && heap_last_ok_callers[f] != 0; f++) {
		log_address("[heapcheck]       ", heap_last_ok_callers[f]);
	}
	log_static("[heapcheck]   top %p, size word %08x, ends %p, break %p, diff %d", (void *)top,
			(unsigned)*(uint32_t *)(top + 4), (void *)top_end, (void *)brk, (int)(brk - top_end));
	heap_dump_below_top(top);
	heap_walk_to_top(top);
	uint32_t n = heap_recent_count < HEAP_RECENT_ALLOCS ? heap_recent_count : HEAP_RECENT_ALLOCS;
	log_static("[heapcheck]   last %u allocations, newest first (end = ptr + size):", (unsigned)n);
	for (uint32_t i = 0; i < n; i++) {
		HeapAllocRecord *r = &heap_recent[(heap_recent_count - 1 - i) % HEAP_RECENT_ALLOCS];
		uint8_t *end = (uint8_t *)r->ptr + r->size;
		log_static("[heapcheck]   %s %p + %u = %p (top - end = %d)", r->func, r->ptr,
				(unsigned)r->size, (void *)end, (int)(top - end));
		for (int f = 0; f < HEAP_CALLER_FRAMES && r->callers[f] != 0; f++) {
			log_address("[heapcheck]       ", r->callers[f]);
		}
	}
}

// Logs where the app's MEM1/MEM2 regions are.
void wiiu_log_memory_bounds(void) {
	uint32_t base = 0, size = 0;
	OSGetMemBound(OS_MEM1, &base, &size);
	log_static("[mem] MEM1 bound: base 0x%08x, size 0x%08x (%u KB)", (unsigned)base,
			(unsigned)size, (unsigned)(size / 1024));
	OSGetMemBound(OS_MEM2, &base, &size);
	log_static("[mem] MEM2 bound: base 0x%08x, size 0x%08x (%u KB)", (unsigned)base,
			(unsigned)size, (unsigned)(size / 1024));
}

// Logs the newlib heap (mallinfo, top chunk, sbrk break) and what is left in
// the MEM2 base heap.
void wiiu_log_heap_state(const char *tag) {
	struct mallinfo info = mallinfo();
	log_static("[heap] %s: malloc arena %u, in use %u, free %u", tag,
			(unsigned)info.arena, (unsigned)info.uordblks, (unsigned)info.fordblks);
	uint8_t *top = __malloc_av_[2];
	uint32_t top_size = top ? (*(uint32_t *)(top + 4) & ~3u) : 0;
	log_static("[heap] %s: top %p size %u ends %p; sbrk(0) %p; sbrk_base %p, top_pad %lu, max_sbrked %lu",
			tag, (void *)top, (unsigned)top_size, (void *)(top + top_size), sbrk(0),
			(void *)__malloc_sbrk_base, __malloc_top_pad, __malloc_max_sbrked_mem);
	MEMHeapHandle mem2 = MEMGetBaseHeapHandle(MEM_BASE_HEAP_MEM2);
	log_static("[heap] %s: MEM2 heap %p, largest free block %u, total free %u", tag,
			(void *)mem2, (unsigned)MEMGetAllocatableSizeForExpHeapEx(mem2, 4),
			(unsigned)MEMGetTotalFreeSizeForExpHeap(mem2));
}

static void log_alloc(const char *func, size_t count, size_t size, void *ptr, void *caller) {
	log_static("[heap] %s(%u x %u bytes) = %p, errno %d, called from %p", func,
			(unsigned)count, (unsigned)size, ptr, errno, caller);
	if (ptr == NULL) {
		wiiu_log_heap_state("after failed allocation");
	}
}

extern void __real_free(void *ptr);

void __wrap_free(void *ptr) {
	heap_check("before free", __builtin_return_address(0));
	__real_free(ptr);
	heap_check("after free", __builtin_return_address(0));
}

void *__wrap_malloc(size_t size) {
	heap_check("before malloc", __builtin_return_address(0));
	void *ptr = __real_malloc(size);
	heap_record("malloc", ptr, size);
	heap_check("after malloc", __builtin_return_address(0));
	if (size != 0 && (ptr == NULL /* || size >= LARGE_ALLOC_BYTES */)) {
		log_alloc("malloc", 1, size, ptr, __builtin_return_address(0));
	}
	return ptr;
}

void *__wrap_calloc(size_t count, size_t size) {
	heap_check("before calloc", __builtin_return_address(0));
	void *ptr = __real_calloc(count, size);
	size_t total = count * size;
	heap_record("calloc", ptr, total);
	heap_check("after calloc", __builtin_return_address(0));
	if (total != 0 && (ptr == NULL /* || total >= LARGE_ALLOC_BYTES */)) {
		log_alloc("calloc", count, size, ptr, __builtin_return_address(0));
	}
	return ptr;
}

void *__wrap_realloc(void *old_ptr, size_t size) {
	heap_check("before realloc", __builtin_return_address(0));
	void *ptr = __real_realloc(old_ptr, size);
	heap_record("realloc", ptr, size);
	heap_check("after realloc", __builtin_return_address(0));
	if (size != 0 && (ptr == NULL /* || size >= LARGE_ALLOC_BYTES */)) {
		log_alloc("realloc", 1, size, ptr, __builtin_return_address(0));
	}
	return ptr;
}

// Every heap extension or trim goes through _sbrk_r (malloc and malloc_trim
// are its only callers). Set to true to log the complete history of the
// break; failures are always logged.
static const bool sbrk_trace = false;

void *__wrap__sbrk_r(struct _reent *reent, ptrdiff_t incr) {
	void *before = (incr != 0) ? __real__sbrk_r(reent, 0) : NULL;
	void *result = __real__sbrk_r(reent, incr);
	if (incr != 0 && (sbrk_trace || result == (void *)-1)) {
		void *after = __real__sbrk_r(reent, 0);
		log_static("[sbrk] %+d: break %p -> %p, returned %p, caller %p", (int)incr, before,
				after, result, __builtin_return_address(0));
	}
	return result;
}

// ---------------------------------------------------------------------------
// Crash screen
// ---------------------------------------------------------------------------

// WHBProcIsRunning() calls ProcUIShutdown() itself once it sees the Exit
// message and returns false; calling it again after that makes
// ProcUIProcessMessages OSPanic ("procui.c" line 641). Everything goes through
// this wrapper, which remembers the false and never calls into ProcUI again.
static volatile bool proc_running = true;
static volatile bool crash_in_progress;

// Set by the OS layer (os_wiiu.cpp) while Godot's main loop runs: told when
// the app loses the foreground (HOME menu) and gets it back, for Godot's
// focus notifications. Never called during a crash.
void (*wiiu_proc_focus_callback)(bool p_focused) = NULL;

static bool proc_release_pending = false;
static bool proc_in_background = false;

// WHBProcIsRunning, except that on "release foreground" the app gets one more
// frame before ProcUIDrawDoneRelease (like SDL2's Wii U backend), so the game
// sees the focus loss first (e.g. pauses) and the HOME menu opens on top of
// that frame.
bool wiiu_proc_is_running(void) {
	if (!proc_running) {
		return false;
	}
	if (proc_release_pending) {
		proc_release_pending = false;
		proc_in_background = true;
		ProcUIDrawDoneRelease();
	}
	switch (ProcUIProcessMessages(TRUE)) {
		case PROCUI_STATUS_EXITING:
			proc_running = false;
			ProcUIShutdown();
			break;
		case PROCUI_STATUS_RELEASE_FOREGROUND:
			proc_release_pending = true;
			if (wiiu_proc_focus_callback && !crash_in_progress) {
				wiiu_proc_focus_callback(false);
			}
			break;
		case PROCUI_STATUS_IN_FOREGROUND:
			if (proc_in_background) {
				proc_in_background = false;
				if (wiiu_proc_focus_callback && !crash_in_progress) {
					wiiu_proc_focus_callback(true);
				}
			}
			break;
		default:
			break;
	}
	return proc_running;
}

// HOME pressed while the HOME menu is disabled: ProcUI swallows the press and
// sends "Home Button Denied" instead (the GamePad's HOME bit may never show
// up in VPADRead then), so this callback is what notices it.
static volatile bool home_pressed = false;

static uint32_t home_denied_callback(void *context) {
	(void)context;
	home_pressed = true;
	return 0;
}

// Keeps the OSScreen log up until the user leaves. HOME goes straight back to
// the Wii U Menu (SYSLaunchMenu quits the app; it doesn't open the HOME
// menu). The HOME menu is disabled here, because opening it on top of
// OSScreen often hung the console. Reads the GamePad once per frame for log
// scrolling. Returns once ProcUI has sent Exit.
void wiiu_wait_for_home_exit(void) {
	home_pressed = false;
	OSEnableHomeButtonMenu(FALSE);
	ProcUIRegisterCallback(PROCUI_CALLBACK_HOME_BUTTON_DENIED, home_denied_callback, NULL, 100);
	bool launched = false;
	while (wiiu_proc_is_running()) {
		VPADStatus status;
		VPADReadError error = VPAD_READ_SUCCESS;
		if (VPADRead(VPAD_CHAN_0, &status, 1, &error) > 0 && error == VPAD_READ_SUCCESS) {
			if (status.trigger & VPAD_BUTTON_HOME) {
				home_pressed = true;
			}
			wiiu_console_handle_buttons(status.hold, status.trigger);
		}
		if (home_pressed && !launched) {
			log_static("HOME pressed: returning to the Wii U Menu.");
			SYSLaunchMenu();
			launched = true;
		}
		wiiu_console_draw();
		OSSleepTicks(OSMillisecondsToTicks(16));
	}
}

static void draw_centered_box(WiiUScreen p_screen, const char *const *p_lines, int p_count);
bool wiiu_chord_update(uint32_t hold, uint32_t trigger);
void wiiu_draw_log_gx2(WiiUScreen p_screen, float p_r, float p_g, float p_b, float p_a, const char *p_footer);

// The crash screen while GX2 runs: the game's last frame with a short
// message in the middle (no jumpscare), or the log after the chord. HOME goes
// back to the Wii U Menu, as on the OSScreen log. Returns once ProcUI has sent
// Exit.
static void crash_overlay_loop(void) {
	// Button names as CafeStd's icons: ZL, L, D-pad right, X, Y, TV; HOME.
	static const char *const message[3] = {
		"Godot crashed!",
		"Press \xEE\x82\x85+\xEE\x82\x83+\xEE\x81\xBC+\xEE\x80\x82+\xEE\x80\x83+\xEE\x82\x89 to see crash log",
		"Press \xEE\x81\x84 to exit",
	};
	wiiu_console_set_exit_footer();
	wiiu_gx2_save_frame();
	home_pressed = false;
	OSEnableHomeButtonMenu(FALSE);
	ProcUIRegisterCallback(PROCUI_CALLBACK_HOME_BUTTON_DENIED, home_denied_callback, NULL, 100);
	bool show_log = false;
	bool launched = false;
	while (wiiu_proc_is_running()) {
		VPADStatus status;
		VPADReadError error = VPAD_READ_SUCCESS;
		if (VPADRead(VPAD_CHAN_0, &status, 1, &error) > 0 && error == VPAD_READ_SUCCESS) {
			if (status.trigger & VPAD_BUTTON_HOME) {
				home_pressed = true;
			}
			if (wiiu_chord_update(status.hold, status.trigger)) {
				show_log = !show_log;
			} else if (show_log && (status.trigger & VPAD_BUTTON_B)) {
				show_log = false; // B closes the log.
			} else if (show_log) {
				wiiu_console_handle_buttons(status.hold, status.trigger);
			}
		}
		if (home_pressed && !launched) {
			log_static("HOME pressed: returning to the Wii U Menu.");
			SYSLaunchMenu();
			launched = true;
		}
		if (!wiiu_gx2_can_draw()) {
			OSSleepTicks(OSMillisecondsToTicks(16));
			continue;
		}
		wiiu_gx2_frame_begin();
		for (int s = 0; s < 2; s++) {
			const WiiUScreen screen = s == 0 ? WIIU_SCREEN_TV : WIIU_SCREEN_DRC;
			wiiu_gx2_bind_screen(screen);
			if (show_log) {
				wiiu_draw_log_gx2(screen, 0.63f, 0.0f, 0.0f, 1.0f, "Press B to go back, HOME to exit. Up/Down/L/R scroll.");
			} else {
				wiiu_gx2_draw_saved_frame(screen);
				draw_centered_box(screen, message, 3);
			}
		}
		wiiu_gx2_frame_present();
	}
}

static volatile bool crash_in_progress = false;
// The crash happened inside gx2.rpl: GX2 can't be trusted to draw the overlay.
static volatile bool crash_in_gx2 = false;

// ---------------------------------------------------------------------------
// Log toggle chord and the GX2-drawn log
// ---------------------------------------------------------------------------

// ZL, L, D-pad Right, X, Y, TV, pressed in that order, each one added while
// the previous ones are held.
static const uint32_t chord_buttons[6] = {
	VPAD_BUTTON_ZL, VPAD_BUTTON_L, VPAD_BUTTON_RIGHT, VPAD_BUTTON_X, VPAD_BUTTON_Y, VPAD_BUTTON_TV
};
static int chord_step = 0;

// Feed every GamePad sample; returns true once when the chord completes.
bool wiiu_chord_update(uint32_t hold, uint32_t trigger) {
	for (int i = 0; i < chord_step; i++) {
		if (!(hold & chord_buttons[i])) {
			chord_step = 0;
			break;
		}
	}
	if (chord_step < 6 && (trigger & chord_buttons[chord_step])) {
		chord_step++;
		if (chord_step == 6) {
			chord_step = 0;
			return true;
		}
	}
	return false;
}

// The log drawn over the game while it runs (toggled by the chord).
static volatile bool log_overlay_visible = false;

bool wiiu_log_overlay_visible(void) {
	return log_overlay_visible;
}

void wiiu_log_overlay_toggle(void) {
	log_overlay_visible = !log_overlay_visible;
}

// Draws the log over the bound screen with GX2 and the built-in bitmap font.
// p_footer is the bottom line while the log isn't scrolled up.
void wiiu_draw_log_gx2(WiiUScreen p_screen, float p_r, float p_g, float p_b, float p_a, const char *p_footer) {
	static char lines[48][69];
	uint32_t w, h;
	wiiu_gx2_screen_size(p_screen, &w, &h);
	wiiu_gx2_draw_rect(0, 0, (float)w, (float)h, p_r, p_g, p_b, p_a);
	uint32_t rows = h / WIIU_FONT_CELL_H - 1; // Last row: footer.
	if (rows > 48) {
		rows = 48;
	}
	uint32_t bottom = 0, total = 0;
	const uint32_t n = wiiu_console_copy_view(rows, lines, &bottom, &total);
	const float x0 = (float)((w - 68 * WIIU_FONT_CELL_W) / 2);
	for (uint32_t i = 0; i < n; i++) {
		wiiu_gx2_draw_text(x0, (float)(i * WIIU_FONT_CELL_H), lines[i], 1, 1, 1, 1);
	}
	char footer[80];
	if (bottom < total) {
		snprintf(footer, sizeof(footer), "%u/%u, use Up/Down/L/R", (unsigned)bottom, (unsigned)total);
	} else {
		snprintf(footer, sizeof(footer), "%s", p_footer);
	}
	wiiu_gx2_draw_text(x0, (float)(h - WIIU_FONT_CELL_H), footer, 1, 1, 0.6f, 1);
}

// wiiu_system_font.cpp: CafeStd (the Wii U system font) with button icons.
float wiiu_sysfont_measure(const char *p_text, float p_px);
float wiiu_sysfont_line_height(float p_px);
void wiiu_sysfont_draw(float p_x, float p_y, float p_px, const char *p_text, float p_r, float p_g, float p_b, float p_a);

// A centred, half-transparent red box with white centred lines in the system
// font (the crash message), sized to the text.
static void draw_centered_box(WiiUScreen p_screen, const char *const *p_lines, int p_count) {
	uint32_t w, h;
	wiiu_gx2_screen_size(p_screen, &w, &h);
	const float px = (float)h * 0.05f; // 36 px on the TV, 24 on the GamePad.
	const float line = wiiu_sysfont_line_height(px) * 1.15f;
	float longest = 0.0f;
	for (int i = 0; i < p_count; i++) {
		const float lw = wiiu_sysfont_measure(p_lines[i], px);
		longest = lw > longest ? lw : longest;
	}
	float bw = longest + px * 3.0f;
	const float min_w = (float)w * 0.7f;
	bw = bw < min_w ? min_w : bw;
	const float bh = line * (float)p_count + px * 3.0f;
	const float bx = ((float)w - bw) * 0.5f;
	const float by = ((float)h - bh) * 0.5f;
	wiiu_gx2_draw_rect(bx, by, bw, bh, 0.85f, 0.0f, 0.0f, 0.5f);
	const float ty0 = by + (bh - line * (float)p_count) * 0.5f;
	for (int i = 0; i < p_count; i++) {
		const float tw = wiiu_sysfont_measure(p_lines[i], px);
		wiiu_sysfont_draw(((float)w - tw) * 0.5f, ty0 + (float)i * line, px, p_lines[i], 1, 1, 1, 1);
	}
}
// Set right before WHBProcShutdown: after that there is no foreground, no
// console and no ProcUI to wait on, so a crash just ends the process.
static volatile bool crash_procui_shut_down = false;

void wiiu_crash_note_procui_shutdown(void) {
	crash_procui_shut_down = true;
}
static volatile bool crash_pending_for_main = false;

static bool on_main_thread(void) {
	return OSGetCurrentThread() == OSGetDefaultThread(1);
}

// Logs addr together with the symbol Cafe OS knows for it.
static void log_address(const char *label, uint32_t addr) {
	char symbol[96];
	symbol[0] = '\0';
	OSGetSymbolName(addr, symbol, sizeof(symbol));
	log_static("%s%08x %s", label, (unsigned)addr, symbol);
}

// PowerPC EABI frame: word 0 is the caller's stack pointer (back chain), and
// the caller's frame holds the saved LR at offset 4.
static void log_backtrace(uintptr_t sp) {
	log_static("Backtrace:");
	for (int i = 0; i < CRASH_MAX_FRAMES; i++) {
		if (sp == 0 || (sp & 3) != 0) {
			break;
		}
		uintptr_t next = *(uintptr_t *)sp;
		if (next == 0 || next <= sp || (next & 3) != 0) {
			break;
		}
		char label[16];
		snprintf(label, sizeof(label), "  #%02d ", i);
		log_address(label, *(uint32_t *)(next + 4));
		sp = next;
	}
}

// Shows the red crash screen. On the main thread it waits for HOME and exits
// the app; on any other thread it parks the thread and hands over to the main
// thread through wiiu_crash_check().
static void enter_crash_screen(void) __attribute__((noreturn));
static void enter_crash_screen(void) {
	if (crash_procui_shut_down) {
		OSReport("Godot crashed after ProcUI shutdown; exiting.\n");
		_Exit(EXIT_FAILURE);
	}
	wiiu_console_enter_crash_mode(CONSOLE_COLOR_CRASH);

	if (!on_main_thread()) {
		log_static("The crash was not on the main thread; waiting for it to take over.");
		wiiu_console_draw();
		crash_pending_for_main = true;
		for (;;) {
			OSSuspendThread(OSGetCurrentThread());
		}
	}

	log_static("Waiting for HOME.");
	if (wiiu_gx2_can_draw() && !crash_in_gx2) {
		// Keep the game's last frame up with a short message; the chord shows
		// the log (both drawn with GX2 alone, not Godot's renderer).
		crash_overlay_loop();
	} else {
		// No GX2, or GX2 itself is what broke: hand the display back to the
		// OSScreen log console.
		if (wiiu_gx2_is_active()) {
			wiiu_gx2_shutdown();
		}
		wiiu_console_resume();
		wiiu_console_set_exit_footer();
		wiiu_wait_for_home_exit();
	}
	wiiu_console_free();
	WHBProcShutdown();
	// _Exit skips atexit handlers and static destructors: the engine is in an
	// unknown state, so running its teardown could crash again.
	_Exit(EXIT_FAILURE);
}

// Called by the main loop every frame: if another thread crashed, show the
// crash screen from the main thread, where HOME works.
void wiiu_crash_check(void) {
	if (crash_pending_for_main) {
		enter_crash_screen();
	}
}

void __wrap_abort(void) __attribute__((noreturn));
void __wrap_abort(void) {
	if (crash_in_progress) {
		// abort() from inside the crash screen itself: give up quietly.
		__real_abort();
	}
	crash_in_progress = true;
	wiiu_console_enter_crash_mode(CONSOLE_COLOR_CRASH);

	OSThread *thread = OSGetCurrentThread();
	log_static("*** Godot crashed: abort() called ***");
	log_static("Thread %p (%s)", (void *)thread, (thread && thread->name) ? thread->name : "?");
	log_static("main is at %p", (void *)&main);
	log_address("Caller: ", (uint32_t)(uintptr_t)__builtin_return_address(0));
	log_backtrace((uintptr_t)__builtin_frame_address(0));
	enter_crash_screen();
}

// Exception context saved by the callback, read by crash_exception_entry.
static OSContext crash_context;
static OSExceptionType crash_exception_type;
static OSThread *crash_thread;
static uint8_t crash_stack[CRASH_STACK_SIZE] __attribute__((aligned(16)));

static const char *exception_name(OSExceptionType type) {
	switch (type) {
		case OS_EXCEPTION_TYPE_DSI:
			return "DSI (invalid data access)";
		case OS_EXCEPTION_TYPE_ISI:
			return "ISI (invalid instruction fetch)";
		case OS_EXCEPTION_TYPE_PROGRAM:
			return "PROGRAM (illegal instruction or trap)";
		default:
			return "unknown";
	}
}

// The crashed thread resumes here, on crash_stack, after the exception
// callback returns.
static void crash_exception_entry(void) __attribute__((noreturn, used));
static void crash_exception_entry(void) {
	wiiu_console_enter_crash_mode(CONSOLE_COLOR_CRASH);
	{
		char symbol[96];
		symbol[0] = '\0';
		OSGetSymbolName(crash_context.srr0, symbol, sizeof(symbol));
		crash_in_gx2 = strstr(symbol, "gx2") != NULL;
	}
	log_static("*** Godot crashed: %s ***", exception_name(crash_exception_type));
	log_static("Thread %p (%s)", (void *)crash_thread,
			(crash_thread && crash_thread->name) ? crash_thread->name : "?");
	log_static("main is at %p", (void *)&main);
	log_address("SRR0: ", crash_context.srr0);
	log_address("LR:   ", crash_context.lr);
	log_static("DAR: %08x  DSISR: %08x  SRR1: %08x", (unsigned)crash_context.dar,
			(unsigned)crash_context.dsisr, (unsigned)crash_context.srr1);
	for (int i = 0; i < 32; i += 4) {
		log_static("r%-2d %08x  r%-2d %08x  r%-2d %08x  r%-2d %08x", i,
				(unsigned)crash_context.gpr[i], i + 1, (unsigned)crash_context.gpr[i + 1], i + 2,
				(unsigned)crash_context.gpr[i + 2], i + 3, (unsigned)crash_context.gpr[i + 3]);
	}
	log_backtrace(crash_context.gpr[1]);
	enter_crash_screen();
}

// Runs in exception context: only save state and redirect the thread. Returns
// FALSE (let Cafe OS handle it) for a second crash while the first is shown.
static BOOL crash_exception_callback(OSExceptionType type, OSContext *context) {
	if (crash_in_progress) {
		return FALSE;
	}
	crash_in_progress = true;
	memcpy(&crash_context, context, sizeof(crash_context));
	crash_exception_type = type;
	crash_thread = OSGetCurrentThread();

	// Resume at crash_exception_entry on a private stack (the thread's own
	// stack may be what broke), with an empty back chain.
	uintptr_t sp = ((uintptr_t)crash_stack + sizeof(crash_stack) - 64) & ~(uintptr_t)15;
	*(uint32_t *)sp = 0;
	context->gpr[1] = (uint32_t)sp;
	context->srr0 = (uint32_t)(uintptr_t)&crash_exception_entry;
	return TRUE;
}

static BOOL crash_dsi_callback(OSContext *context) {
	return crash_exception_callback(OS_EXCEPTION_TYPE_DSI, context);
}

static BOOL crash_isi_callback(OSContext *context) {
	return crash_exception_callback(OS_EXCEPTION_TYPE_ISI, context);
}

static BOOL crash_program_callback(OSContext *context) {
	return crash_exception_callback(OS_EXCEPTION_TYPE_PROGRAM, context);
}

// Call once after wiiu_console_init: black console, crash handlers on.
void wiiu_crash_screen_init(void) {
	wiiu_console_set_color(CONSOLE_COLOR_NORMAL);
	OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_DSI,
			crash_dsi_callback);
	OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_ISI,
			crash_isi_callback);
	OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_PROGRAM,
			crash_program_callback);
	// Logging works from here on: catch corruption that happened before main.
	heap_reporting_enabled = true;
	heap_check("start of main", __builtin_return_address(0));
}
