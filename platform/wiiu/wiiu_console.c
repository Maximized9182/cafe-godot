// On-screen log console for the Wii U port, drawn with OSScreen on the TV and
// the GamePad.
//
// Replaces WHBLogConsole, which keeps only the last 16 lines and can't scroll.
// This one registers itself as a WHB log handler, keeps a history of
// CONSOLE_HISTORY_LINES lines (long lines are wrapped), and lets the user
// scroll it with the GamePad: D-pad up/down moves one line (hold to repeat),
// L/R moves a page. While scrolled to the bottom it follows new output.
//
// Framebuffers live in MEM1 and are only valid while the app is in the
// foreground: they are allocated in the ProcUI "acquire" callback and freed
// in "release" (HOME menu), the same way WHBLogConsole does it.
//
// Locking: lines are added under a spin lock. After
// wiiu_console_enter_crash_mode() the lock is no longer taken, because the
// crashed thread may have been holding it.

#include <coreinit/cache.h>
#include <coreinit/memfrmheap.h>
#include <coreinit/memheap.h>
#include <coreinit/screen.h>
#include <coreinit/spinlock.h>
#include <proc_ui/procui.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <vpad/input.h>
#include <whb/log.h>

#define CONSOLE_HISTORY_LINES 1024
// Lines are wrapped at this width so they fit the GamePad (about 70 columns).
#define CONSOLE_WRAP_COLUMNS 68
#define CONSOLE_TV_ROWS 27
#define CONSOLE_DRC_ROWS 17

// Hold D-pad up/down this many frames before it repeats, then repeat every
// CONSOLE_REPEAT_FRAMES frames.
#define CONSOLE_REPEAT_DELAY_FRAMES 20
#define CONSOLE_REPEAT_FRAMES 3

#define CONSOLE_FRAME_HEAP_TAG 0x434F4E53 // "CONS"

static char console_lines[CONSOLE_HISTORY_LINES][CONSOLE_WRAP_COLUMNS + 1];
// Total number of lines ever added; the newest line is
// console_lines[(console_line_count - 1) % CONSOLE_HISTORY_LINES].
static uint32_t console_line_count = 0;
// How many lines the view is scrolled up from the newest line (0 = follow).
static uint32_t console_scroll = 0;

static uint32_t console_color = 0x00000000u;
static OSSpinLock console_lock; // Zero-filled equals OSInitSpinLock.
static volatile bool console_no_lock = false;

static void *console_buffer_tv = NULL;
static void *console_buffer_drc = NULL;
static uint32_t console_buffer_tv_size = 0;
static uint32_t console_buffer_drc_size = 0;
static bool console_acquired = false;

static uint32_t console_hold_frames = 0;

static void console_lock_acquire(void) {
	if (!console_no_lock) {
		OSUninterruptibleSpinLock_Acquire(&console_lock);
	}
}

static void console_lock_release(void) {
	if (!console_no_lock) {
		OSUninterruptibleSpinLock_Release(&console_lock);
	}
}

static uint32_t console_stored_lines(void) {
	return console_line_count < CONSOLE_HISTORY_LINES ? console_line_count : CONSOLE_HISTORY_LINES;
}

// Appends one already-wrapped line. Caller holds the lock.
static void console_push_line(const char *text, size_t len) {
	char *line = console_lines[console_line_count % CONSOLE_HISTORY_LINES];
	memcpy(line, text, len);
	line[len] = '\0';
	console_line_count++;
	// Keep a scrolled-up view on the same lines while new output arrives.
	if (console_scroll != 0 && console_scroll + 1 < console_stored_lines()) {
		console_scroll++;
	}
}

static void console_log_handler(const char *msg) {
	console_lock_acquire();
	const char *p = msg;
	while (*p != '\0') {
		const char *end = p;
		while (*end != '\0' && *end != '\n') {
			end++;
		}
		size_t len = (size_t)(end - p);
		if (len > 0 && p[len - 1] == '\r') {
			len--;
		}
		// Wrap long lines.
		do {
			size_t chunk = len > CONSOLE_WRAP_COLUMNS ? CONSOLE_WRAP_COLUMNS : len;
			console_push_line(p, chunk);
			p += chunk;
			len -= chunk;
		} while (len > 0);
		p = (*end == '\n') ? end + 1 : end;
	}
	console_lock_release();
}

static uint32_t console_proc_acquired(void *context) {
	(void)context;
	MEMHeapHandle heap = MEMGetBaseHeapHandle(MEM_BASE_HEAP_MEM1);
	MEMRecordStateForFrmHeap(heap, CONSOLE_FRAME_HEAP_TAG);
	console_buffer_tv = MEMAllocFromFrmHeapEx(heap, console_buffer_tv_size, 4);
	console_buffer_drc = MEMAllocFromFrmHeapEx(heap, console_buffer_drc_size, 4);
	if (console_buffer_tv == NULL || console_buffer_drc == NULL) {
		MEMFreeByStateToFrmHeap(heap, CONSOLE_FRAME_HEAP_TAG);
		console_buffer_tv = console_buffer_drc = NULL;
		return 0;
	}
	OSScreenSetBufferEx(SCREEN_TV, console_buffer_tv);
	OSScreenSetBufferEx(SCREEN_DRC, console_buffer_drc);
	console_acquired = true;
	return 0;
}

static uint32_t console_proc_released(void *context) {
	(void)context;
	console_acquired = false;
	if (console_buffer_tv != NULL || console_buffer_drc != NULL) {
		MEMFreeByStateToFrmHeap(MEMGetBaseHeapHandle(MEM_BASE_HEAP_MEM1), CONSOLE_FRAME_HEAP_TAG);
		console_buffer_tv = console_buffer_drc = NULL;
	}
	return 0;
}

// Call after WHBProcInit.
void wiiu_console_init(void) {
	OSScreenInit();
	console_buffer_tv_size = OSScreenGetBufferSizeEx(SCREEN_TV);
	console_buffer_drc_size = OSScreenGetBufferSizeEx(SCREEN_DRC);
	console_proc_acquired(NULL);
	OSScreenEnableEx(SCREEN_TV, TRUE);
	OSScreenEnableEx(SCREEN_DRC, TRUE);
	ProcUIRegisterCallback(PROCUI_CALLBACK_ACQUIRE, console_proc_acquired, NULL, 100);
	ProcUIRegisterCallback(PROCUI_CALLBACK_RELEASE, console_proc_released, NULL, 100);
	VPADInit();
	WHBAddLogHandler(console_log_handler);
}

void wiiu_console_free(void) {
	WHBRemoveLogHandler(console_log_handler);
	// Only touch OSScreen while the app still has the foreground: after ProcUI
	// has released it (leaving through HOME), the display registers are
	// unmapped and OSScreenShutdown faults. WHBLogConsoleFree does the same.
	if (console_acquired) {
		OSScreenShutdown();
		console_proc_released(NULL);
	}
}

// Background colour, 0xRRGGBBAA.
void wiiu_console_set_color(uint32_t color) {
	console_color = color;
}

// Called by the crash screen: stop taking the lock (the crashed thread may
// hold it) and jump back to the newest output.
void wiiu_console_enter_crash_mode(uint32_t color) {
	console_no_lock = true;
	console_color = color;
	console_scroll = 0;
}

// Reads the GamePad and scrolls the history.
void wiiu_console_update_input(void) {
	VPADStatus status;
	VPADReadError error = VPAD_READ_SUCCESS;
	if (VPADRead(VPAD_CHAN_0, &status, 1, &error) < 1 || error != VPAD_READ_SUCCESS) {
		return;
	}

	uint32_t stored = console_stored_lines();
	uint32_t max_scroll = stored > CONSOLE_DRC_ROWS ? stored - CONSOLE_DRC_ROWS : 0;
	int32_t delta = 0;

	if (status.hold & (VPAD_BUTTON_UP | VPAD_BUTTON_DOWN)) {
		bool repeat = console_hold_frames >= CONSOLE_REPEAT_DELAY_FRAMES &&
				(console_hold_frames - CONSOLE_REPEAT_DELAY_FRAMES) % CONSOLE_REPEAT_FRAMES == 0;
		if ((status.trigger & VPAD_BUTTON_UP) || ((status.hold & VPAD_BUTTON_UP) && repeat)) {
			delta = 1;
		} else if ((status.trigger & VPAD_BUTTON_DOWN) || ((status.hold & VPAD_BUTTON_DOWN) && repeat)) {
			delta = -1;
		}
		console_hold_frames++;
	} else {
		console_hold_frames = 0;
	}
	if (status.trigger & VPAD_BUTTON_L) {
		delta = CONSOLE_DRC_ROWS - 1;
	} else if (status.trigger & VPAD_BUTTON_R) {
		delta = -(CONSOLE_DRC_ROWS - 1);
	}

	int32_t scroll = (int32_t)console_scroll + delta;
	if (scroll < 0) {
		scroll = 0;
	} else if ((uint32_t)scroll > max_scroll) {
		scroll = (int32_t)max_scroll;
	}
	console_scroll = (uint32_t)scroll;
}

// Rows to draw, copied under the lock so rendering (clearing the ~3.6 MB TV
// buffer, drawing text) runs with interrupts on. The TV shows up to
// CONSOLE_TV_ROWS lines; the GamePad shows the last CONSOLE_DRC_ROWS of them.
static char console_snapshot[CONSOLE_TV_ROWS][CONSOLE_WRAP_COLUMNS + 1];
static uint32_t console_snapshot_count = 0;
static uint32_t console_snapshot_scroll = 0;

// Caller holds the lock.
static void console_take_snapshot(void) {
	uint32_t stored = console_stored_lines();
	uint32_t scroll = console_scroll <= stored ? console_scroll : stored;
	uint32_t available = stored - scroll;
	uint32_t count = available < CONSOLE_TV_ROWS ? available : CONSOLE_TV_ROWS;
	// The view ends `scroll` lines above the newest line.
	uint32_t first = console_line_count - scroll - count;
	for (uint32_t i = 0; i < count; i++) {
		memcpy(console_snapshot[i], console_lines[(first + i) % CONSOLE_HISTORY_LINES],
				CONSOLE_WRAP_COLUMNS + 1);
	}
	console_snapshot_count = count;
	console_snapshot_scroll = scroll;
}

// Builds "-- scrolled up N lines ... --" without snprintf, to stay
// allocation-free.
static void console_format_indicator(char *out, uint32_t n) {
	const char *prefix = "-- scrolled up ";
	const char *suffix = " lines (D-pad / L / R) --";
	size_t pos = 0;
	for (const char *c = prefix; *c; c++) {
		out[pos++] = *c;
	}
	char digits[12];
	int nd = 0;
	do {
		digits[nd++] = (char)('0' + n % 10);
		n /= 10;
	} while (n > 0);
	while (nd > 0) {
		out[pos++] = digits[--nd];
	}
	for (const char *c = suffix; *c && pos < CONSOLE_WRAP_COLUMNS; c++) {
		out[pos++] = *c;
	}
	out[pos] = '\0';
}

static void console_draw_screen(OSScreenID screen, uint32_t rows) {
	OSScreenClearBufferEx(screen, console_color);
	uint32_t text_rows = console_snapshot_scroll != 0 ? rows - 1 : rows;
	uint32_t shown = console_snapshot_count < text_rows ? console_snapshot_count : text_rows;
	uint32_t first = console_snapshot_count - shown;
	for (uint32_t i = 0; i < shown; i++) {
		OSScreenPutFontEx(screen, 0, i, console_snapshot[first + i]);
	}
	if (console_snapshot_scroll != 0) {
		char indicator[CONSOLE_WRAP_COLUMNS + 1];
		console_format_indicator(indicator, console_snapshot_scroll);
		OSScreenPutFontEx(screen, 0, rows - 1, indicator);
	}
}

void wiiu_console_draw(void) {
	if (!console_acquired) {
		return;
	}
	console_lock_acquire();
	console_take_snapshot();
	console_lock_release();
	console_draw_screen(SCREEN_TV, CONSOLE_TV_ROWS);
	console_draw_screen(SCREEN_DRC, CONSOLE_DRC_ROWS);
	DCFlushRange(console_buffer_tv, console_buffer_tv_size);
	DCFlushRange(console_buffer_drc, console_buffer_drc_size);
	OSScreenFlipBuffersEx(SCREEN_TV);
	OSScreenFlipBuffersEx(SCREEN_DRC);
}
