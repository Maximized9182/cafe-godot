// Cafe Godot's stats overlay: frame timing, CPU and GPU load, memory and
// engine counters, drawn with GX2 over the game on both screens.
//
// Toggled by the chord ZR, R, D-pad Left, Y, B, L (each added while the
// previous ones are held, like the log chord). The panel sits in a corner of
// the screen (top right at first); dragging it on the GamePad moves it, and on
// release it snaps to the nearest corner. Touches that start on the panel are
// not passed to the game.

#include "wiiu_gx2.h"

#include "core/config/engine.h"
#include "core/os/memory.h"
#include "core/os/os.h"
#include "main/performance.h"

#include <coreinit/memexpheap.h>
#include <coreinit/memfrmheap.h>
#include <coreinit/memheap.h>
#include <coreinit/memory.h>
#include <coreinit/systeminfo.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <vpad/input.h>

#include <stdio.h>
#include <string.h>

// wiiu_system_font.cpp: CafeStd (the Wii U system font).
extern "C" float wiiu_sysfont_measure(const char *p_text, float p_px);
extern "C" float wiiu_sysfont_line_height(float p_px);
extern "C" void wiiu_sysfont_draw(float p_x, float p_y, float p_px, const char *p_text, float p_r, float p_g, float p_b, float p_a);

namespace {

// ---------------------------------------------------------------------------
// Toggle chord
// ---------------------------------------------------------------------------

const uint32_t chord_buttons[6] = {
	VPAD_BUTTON_ZR, VPAD_BUTTON_R, VPAD_BUTTON_LEFT, VPAD_BUTTON_Y, VPAD_BUTTON_B, VPAD_BUTTON_L
};
int chord_step = 0;
bool visible = false;

// ---------------------------------------------------------------------------
// Placement
// ---------------------------------------------------------------------------

enum Corner {
	TOP_LEFT,
	TOP_RIGHT,
	BOTTOM_LEFT,
	BOTTOM_RIGHT,
};
Corner corner = TOP_RIGHT;
// While dragging: the panel's top-left as a fraction of the screen size.
bool dragging = false;
float drag_x = 0.0f, drag_y = 0.0f;
float grab_dx = 0.0f, grab_dy = 0.0f; // Touch point minus panel top-left (fractions).

constexpr int MAX_LINES = 16;
constexpr float PANEL_W = 470.0f; // In TV pixels; scaled per screen.
constexpr float PAD = 10.0f;
constexpr float FONT_PX = 18.0f;
constexpr float GRAPH_H = 44.0f;
constexpr float EDGE = 12.0f;

// Panel scale per screen: the GamePad's picture is 854x480, but a 2/3 scale
// is too small to read there, so it gets 0.8.
float screen_scale(WiiUScreen p_screen) {
	return p_screen == WIIU_SCREEN_TV ? 1.0f : 0.8f;
}

char lines[MAX_LINES][96];
int line_count = 0;

float panel_height(float p_scale) {
	const float line = wiiu_sysfont_line_height(FONT_PX * p_scale) * 1.08f;
	return PAD * p_scale * 2.0f + line * (float)line_count + (GRAPH_H + 6.0f) * p_scale;
}

void panel_rect(WiiUScreen p_screen, float &r_x, float &r_y, float &r_w, float &r_h) {
	uint32_t sw, sh;
	wiiu_gx2_screen_size(p_screen, &sw, &sh);
	const float s = screen_scale(p_screen);
	r_w = PANEL_W * s;
	r_h = panel_height(s);
	if (dragging) {
		r_x = drag_x * (float)sw;
		r_y = drag_y * (float)sh;
	} else {
		const float e = EDGE * s;
		r_x = (corner == TOP_LEFT || corner == BOTTOM_LEFT) ? e : (float)sw - r_w - e;
		r_y = (corner == TOP_LEFT || corner == TOP_RIGHT) ? e : (float)sh - r_h - e;
	}
}

// ---------------------------------------------------------------------------
// Measurements
// ---------------------------------------------------------------------------

constexpr int FRAME_HISTORY = 90;
float frame_ms[FRAME_HISTORY] = {};
int frame_head = 0; // Next slot to write.
int frame_filled = 0;
OSTime last_frame_time = 0;

uint8_t battery = 0xFF;

// Per-thread CPU time (OSThread::coreTimeConsumedNs) between samples.
struct ThreadSample {
	OSThread *thread;
	uint64_t ns;
};
constexpr int MAX_THREADS = 64;
ThreadSample thread_prev[MAX_THREADS];
int thread_prev_count = 0;
OSTime cpu_last_sample = 0;
float cpu_total_pct = -1.0f; // Of all three cores; -1: not measured yet.
float cpu_main_pct = -1.0f; // Of one core.
char cpu_busiest[48] = "";

uint64_t prev_ns_for(OSThread *p_thread) {
	for (int i = 0; i < thread_prev_count; i++) {
		if (thread_prev[i].thread == p_thread) {
			return thread_prev[i].ns;
		}
	}
	return UINT64_MAX;
}

void sample_cpu() {
	const OSTime now = OSGetSystemTime();
	const uint64_t elapsed_ns = cpu_last_sample ? (uint64_t)OSTicksToNanoseconds(now - cpu_last_sample) : 0;

	// Walk this process's active threads (activeLink is a list through all of them).
	OSThread *first = OSGetCurrentThread();
	for (int guard = 0; guard < MAX_THREADS && first->activeLink.prev; guard++) {
		first = first->activeLink.prev;
	}
	ThreadSample cur[MAX_THREADS];
	int count = 0;
	uint64_t total_delta = 0, main_delta = 0, best_delta = 0;
	const char *best_name = nullptr;
	OSThread *main_thread = OSGetDefaultThread(1);
	for (OSThread *t = first; t && count < MAX_THREADS; t = t->activeLink.next) {
		cur[count].thread = t;
		cur[count].ns = t->coreTimeConsumedNs;
		const uint64_t prev = prev_ns_for(t);
		if (elapsed_ns && prev != UINT64_MAX && cur[count].ns >= prev) {
			const uint64_t d = cur[count].ns - prev;
			total_delta += d;
			if (t == main_thread) {
				main_delta = d;
			} else if (d > best_delta) {
				best_delta = d;
				best_name = t->name;
			}
		}
		count++;
	}
	memcpy(thread_prev, cur, sizeof(ThreadSample) * count);
	thread_prev_count = count;
	cpu_last_sample = now;
	if (elapsed_ns) {
		cpu_total_pct = (float)((double)total_delta * 100.0 / ((double)elapsed_ns * 3.0));
		cpu_main_pct = (float)((double)main_delta * 100.0 / (double)elapsed_ns);
		if (best_name && best_delta > 0) {
			snprintf(cpu_busiest, sizeof(cpu_busiest), "%.20s %.0f%%", best_name, (double)best_delta * 100.0 / (double)elapsed_ns);
		} else {
			cpu_busiest[0] = '\0';
		}
	}
}

double mb(uint64_t p_bytes) {
	return (double)p_bytes / (1024.0 * 1024.0);
}

OSTime text_last_update = 0;

// Rebuilds the text lines (4 times a second, so they're readable).
void update_lines() {
	line_count = 0;
	auto add = [](const char *p_fmt, auto... p_args) {
		if (line_count < MAX_LINES) {
			snprintf(lines[line_count++], sizeof(lines[0]), p_fmt, p_args...);
		}
	};

	// Frame timing over the last second.
	const int n = MIN(frame_filled, 60);
	float sum = 0.0f, mn = 1e9f, mx = 0.0f;
	for (int i = 0; i < n; i++) {
		const float v = frame_ms[(frame_head - 1 - i + FRAME_HISTORY) % FRAME_HISTORY];
		sum += v;
		mn = MIN(mn, v);
		mx = MAX(mx, v);
	}
	const float avg = n ? sum / (float)n : 0.0f;
	if (n == 0) {
		mn = 0.0f;
	}
	Performance *perf = Performance::get_singleton();
	add("%s", "Cafe Godot stats");
	add("FPS %.0f   frame %.2f ms (%.2f-%.2f)", (double)Engine::get_singleton()->get_frames_per_second(), (double)avg, (double)mn, (double)mx);
	if (perf) {
		add("Process %.2f ms   physics %.2f ms",
				perf->get_monitor(Performance::TIME_PROCESS) * 1000.0,
				perf->get_monitor(Performance::TIME_PHYSICS_PROCESS) * 1000.0);
	}

	WiiUGX2Stats gx2;
	wiiu_gx2_get_stats(&gx2);
	if (cpu_total_pct >= 0.0f) {
		add("CPU main %.0f%%   all 3 cores %.0f%%", (double)cpu_main_pct, (double)cpu_total_pct);
		if (cpu_busiest[0]) {
			add("    busiest other thread: %s", cpu_busiest);
		}
	}
	add("GPU %.2f ms (%.0f%% of frame)", (double)gx2.gpu_ms, avg > 0.0f ? (double)(gx2.gpu_ms * 100.0f / avg) : 0.0);
	add("Draws %u   vertices %u", (unsigned)gx2.draws, (unsigned)gx2.vertices);

	// Memory.
	uint32_t mem2_start = 0, mem2_size = 0;
	OSGetMemBound(OS_MEM2, &mem2_start, &mem2_size);
	MEMHeapHandle mem2 = MEMGetBaseHeapHandle(MEM_BASE_HEAP_MEM2);
	const uint32_t mem2_free = MEMGetTotalFreeSizeForExpHeap(mem2);
	const uint32_t mem2_largest = MEMGetAllocatableSizeForExpHeapEx(mem2, 4);
	add("MEM2 %.1f / %.1f MB used, largest free %.1f MB",
			mb(mem2_size > mem2_free ? mem2_size - mem2_free : 0), mb(mem2_size), mb(mem2_largest));
	MEMHeapHandle mem1 = MEMGetBaseHeapHandle(MEM_BASE_HEAP_MEM1);
	add("MEM1 free %.2f MB", mb(MEMGetAllocatableSizeForFrmHeapEx(mem1, 4)));
	add("Godot heap %.1f MB (peak %.1f MB)", mb(Memory::get_mem_usage()), mb(Memory::get_mem_max_usage()));
	add("Textures %u (%.1f MB)   targets %u (%.1f MB)", (unsigned)gx2.textures, mb(gx2.texture_bytes),
			(unsigned)gx2.render_targets, mb(gx2.render_target_bytes));
	if (perf) {
		add("Objects %.0f   nodes %.0f   orphans %.0f",
				perf->get_monitor(Performance::OBJECT_COUNT),
				perf->get_monitor(Performance::OBJECT_NODE_COUNT),
				perf->get_monitor(Performance::OBJECT_ORPHAN_NODE_COUNT));
	}

	const uint64_t up = OS::get_singleton()->get_ticks_msec() / 1000;
	if (battery <= 6) {
		add("GamePad battery %u/6   uptime %02u:%02u:%02u", (unsigned)battery,
				(unsigned)(up / 3600), (unsigned)(up / 60 % 60), (unsigned)(up % 60));
	} else {
		add("Uptime %02u:%02u:%02u", (unsigned)(up / 3600), (unsigned)(up / 60 % 60), (unsigned)(up % 60));
	}
}

} // namespace

// ---------------------------------------------------------------------------
// API (called from os_wiiu.cpp, joypad_wiiu.cpp and rasterizer_wiiu.cpp)
// ---------------------------------------------------------------------------

// Feed every GamePad sample; returns true once when the chord completes.
extern "C" bool wiiu_stats_chord_update(uint32_t p_hold, uint32_t p_trigger) {
	for (int i = 0; i < chord_step; i++) {
		if (!(p_hold & chord_buttons[i])) {
			chord_step = 0;
			break;
		}
	}
	if (chord_step < 6 && (p_trigger & chord_buttons[chord_step])) {
		chord_step++;
		if (chord_step == 6) {
			chord_step = 0;
			return true;
		}
	}
	return false;
}

extern "C" void wiiu_stats_toggle(void) {
	visible = !visible;
	dragging = false;
	text_last_update = 0;
	cpu_last_sample = 0;
	thread_prev_count = 0;
}

extern "C" bool wiiu_stats_visible(void) {
	return visible;
}

extern "C" void wiiu_stats_set_battery(uint8_t p_level) {
	battery = p_level;
}

// A GamePad touch sample in screen pixels (854x480). p_have_point is false
// when the position isn't valid this sample. p_game_touching: the game is
// already tracking a touch (it started off the panel). Returns true if the
// panel took the touch, which then mustn't reach the game.
extern "C" bool wiiu_stats_touch(bool p_down, bool p_have_point, float p_x, float p_y, bool p_game_touching) {
	if (dragging) {
		if (p_down) {
			if (p_have_point) {
				drag_x = p_x / 854.0f - grab_dx;
				drag_y = p_y / 480.0f - grab_dy;
			}
			return true;
		}
		// Released: snap to the corner nearest the panel's centre.
		float x, y, w, h;
		panel_rect(WIIU_SCREEN_DRC, x, y, w, h);
		const bool left = (x + w * 0.5f) < 427.0f;
		const bool top = (y + h * 0.5f) < 240.0f;
		corner = top ? (left ? TOP_LEFT : TOP_RIGHT) : (left ? BOTTOM_LEFT : BOTTOM_RIGHT);
		dragging = false;
		return true;
	}
	if (!visible || !p_down || !p_have_point || p_game_touching) {
		return false;
	}
	float x, y, w, h;
	panel_rect(WIIU_SCREEN_DRC, x, y, w, h);
	if (p_x < x || p_x >= x + w || p_y < y || p_y >= y + h) {
		return false;
	}
	dragging = true;
	drag_x = x / 854.0f;
	drag_y = y / 480.0f;
	grab_dx = p_x / 854.0f - drag_x;
	grab_dy = p_y / 480.0f - drag_y;
	return true;
}

// Once per presented frame, before drawing.
extern "C" void wiiu_stats_frame(void) {
	const OSTime now = OSGetSystemTime();
	if (last_frame_time) {
		frame_ms[frame_head] = (float)((double)OSTicksToMicroseconds(now - last_frame_time) / 1000.0);
		frame_head = (frame_head + 1) % FRAME_HISTORY;
		frame_filled = MIN(frame_filled + 1, FRAME_HISTORY);
	}
	last_frame_time = now;
	if (!visible) {
		return;
	}
	if (!cpu_last_sample || OSTicksToMilliseconds(now - cpu_last_sample) >= 500) {
		sample_cpu();
	}
	if (!text_last_update || OSTicksToMilliseconds(now - text_last_update) >= 250) {
		text_last_update = now;
		update_lines();
	}
}

// Draws the panel over the bound screen.
extern "C" void wiiu_stats_draw(WiiUScreen p_screen) {
	if (!visible || line_count == 0) {
		return;
	}
	const float s = screen_scale(p_screen);
	float x, y, w, h;
	panel_rect(p_screen, x, y, w, h);
	wiiu_gx2_draw_rect(x, y, w, h, 0.0f, 0.0f, 0.0f, dragging ? 0.85f : 0.7f);

	const float px = FONT_PX * s;
	const float line = wiiu_sysfont_line_height(px) * 1.08f;
	float ty = y + PAD * s;
	for (int i = 0; i < line_count; i++) {
		if (i == 0) {
			wiiu_sysfont_draw(x + PAD * s, ty, px, lines[i], 1.0f, 0.85f, 0.3f, 1.0f);
			const char *hint = dragging ? "release to snap" : "drag to move";
			const float hw = wiiu_sysfont_measure(hint, px * 0.8f);
			wiiu_sysfont_draw(x + w - PAD * s - hw, ty + px * 0.15f, px * 0.8f, hint, 0.7f, 0.7f, 0.7f, 1.0f);
		} else {
			wiiu_sysfont_draw(x + PAD * s, ty, px, lines[i], 1.0f, 1.0f, 1.0f, 1.0f);
		}
		ty += line;
	}

	// Frame time graph: one bar per frame, newest on the right, up to 33.3 ms
	// (two frames at 60 Hz); the line marks 16.7 ms.
	const float gx = x + PAD * s;
	const float gw = w - PAD * s * 2.0f;
	const float gh = GRAPH_H * s;
	const float gy = ty + 4.0f * s;
	wiiu_gx2_draw_rect(gx, gy, gw, gh, 1.0f, 1.0f, 1.0f, 0.08f);
	const float bar_w = gw / (float)FRAME_HISTORY;
	for (int i = 0; i < frame_filled; i++) {
		const float v = frame_ms[(frame_head - frame_filled + i + FRAME_HISTORY) % FRAME_HISTORY];
		const float bh = MIN(v / 33.3f, 1.0f) * gh;
		const int slot = FRAME_HISTORY - frame_filled + i;
		float r = 0.3f, g = 0.9f, b = 0.4f;
		if (v > 34.0f) {
			r = 1.0f, g = 0.3f, b = 0.3f;
		} else if (v > 17.5f) {
			r = 1.0f, g = 0.8f, b = 0.2f;
		}
		wiiu_gx2_draw_rect(gx + (float)slot * bar_w, gy + gh - bh, MAX(bar_w - 1.0f, 1.0f), bh, r, g, b, 0.9f);
	}
	wiiu_gx2_draw_rect(gx, gy + gh * 0.5f, gw, MAX(1.0f, s), 1.0f, 1.0f, 1.0f, 0.5f);
}
