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
#include "core/math/math_funcs.h"
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
#include <whb/log.h>

#include <malloc.h>
#include <stdio.h>
#include <string.h>

// wiiu_system_font.cpp: CafeStd (the Wii U system font).
// Defined in audio_driver_wiiu.cpp and crash_log.c.
extern "C" volatile uint32_t wiiu_audio_underruns;
extern "C" volatile uint32_t wiiu_sbrk_total;
// stats_font_data.c: JetBrains Mono (SIL OFL 1.1) baked at 15 px (TV) and 12 px (GamePad).
extern "C" const uint8_t wiiu_stats_font_0[95][9 * 21];
extern "C" const uint8_t wiiu_stats_font_1[95][8 * 17];
extern "C" const uint32_t wiiu_stats_font_cell_w[2];
extern "C" const uint32_t wiiu_stats_font_cell_h[2];
extern "C" const float wiiu_stats_font_advance[2];

namespace {

// ---------------------------------------------------------------------------
// Toggle chord
// ---------------------------------------------------------------------------

const uint32_t chord_buttons[6] = {
	VPAD_BUTTON_ZR, VPAD_BUTTON_R, VPAD_BUTTON_LEFT, VPAD_BUTTON_Y, VPAD_BUTTON_B, VPAD_BUTTON_L
};
int chord_step = 0;
bool visible = false;
float update_cost_ms = 0.0f; // Time the last text refresh took (shown in the header).


// ---------------------------------------------------------------------------
// Placement
// ---------------------------------------------------------------------------

enum Corner {
	TOP_LEFT,
	TOP_RIGHT,
	BOTTOM_LEFT,
	BOTTOM_RIGHT,
};
Corner corner = TOP_LEFT;
// While dragging: the panel's top-left as a fraction of the screen size.
bool dragging = false;
float drag_x = 0.0f, drag_y = 0.0f;
float grab_dx = 0.0f, grab_dy = 0.0f; // Touch point minus panel top-left (fractions).

constexpr int MAX_LINES = 16;
constexpr float PANEL_W = 440.0f; // In TV pixels; scaled per screen.
constexpr float PAD = 10.0f;
constexpr float GRAPH_H = 34.0f;
constexpr float EDGE = 12.0f;
constexpr float ARROW_HIT = 36.0f; // Touch target of the expand arrow (top right of the panel).
constexpr int COL_VALUE = 10; // Column starts, in characters (monospace font).
constexpr int COL_NOTE = 18;

// Compact: FPS, CPU, GPU and the frame graph. Expanded adds the rest.
bool expanded = false;
bool arrow_held = false;

// Panel scale per screen: the GamePad's picture is 854x480, but a 2/3 scale
// is too small to read there, so it gets 0.8.
float screen_scale(WiiUScreen p_screen) {
	return p_screen == WIIU_SCREEN_TV ? 1.0f : 0.8f;
}

struct Row {
	char key[24];
	char value[32];
	char note[64];
	bool bad;
};
Row lines[MAX_LINES];
int line_count = 0;

// Font index per screen: 0 = TV (15 px), 1 = GamePad (12 px, the 0.8 scale).
int font_index(WiiUScreen p_screen) {
	return p_screen == WIIU_SCREEN_TV ? 0 : 1;
}

float row_height(int p_font) {
	return (float)wiiu_stats_font_cell_h[p_font];
}

float panel_height(float p_scale) {
	const int f = p_scale < 1.0f ? 1 : 0;
	return PAD * p_scale * 2.0f + row_height(f) * (float)line_count + (GRAPH_H + 6.0f) * p_scale;
}

constexpr int FONT_COLS = 16;
constexpr int FONT_ROWS = 6; // 95 glyphs.
WiiUTexture *font_atlas[2] = {};

// Builds the glyph atlas of one font size (white, alpha from the baked cells).
WiiUTexture *get_font_atlas(int p_font) {
	if (font_atlas[p_font] == nullptr) {
		const uint32_t cw = wiiu_stats_font_cell_w[p_font], ch = wiiu_stats_font_cell_h[p_font];
		const uint32_t aw = cw * FONT_COLS, ah = ch * FONT_ROWS;
		uint8_t *rgba = (uint8_t *)calloc((size_t)aw * ah, 4);
		if (rgba == nullptr) {
			return nullptr;
		}
		for (int g = 0; g < 95; g++) {
			const uint8_t *cell = p_font == 0 ? wiiu_stats_font_0[g] : wiiu_stats_font_1[g];
			const uint32_t ox = (uint32_t)(g % FONT_COLS) * cw, oy = (uint32_t)(g / FONT_COLS) * ch;
			for (uint32_t y = 0; y < ch; y++) {
				for (uint32_t x = 0; x < cw; x++) {
					uint8_t *px = rgba + (((size_t)(oy + y) * aw) + ox + x) * 4;
					px[0] = px[1] = px[2] = 255;
					px[3] = cell[y * cw + x];
				}
			}
		}
		font_atlas[p_font] = wiiu_gx2_texture_create(aw, ah, rgba);
		free(rgba);
	}
	return font_atlas[p_font];
}

// Text width in pixels for p_chars characters.
float text_width(int p_font, int p_chars) {
	return (float)p_chars * wiiu_stats_font_advance[p_font];
}

// One line, top-left at (p_x, p_y), glyphs on whole pixels (1:1 with the atlas).
void draw_text(int p_font, float p_x, float p_y, const char *p_text, float p_r, float p_g, float p_b) {
	WiiUTexture *atlas = get_font_atlas(p_font);
	if (atlas == nullptr) {
		return;
	}
	const float cw = (float)wiiu_stats_font_cell_w[p_font], ch = (float)wiiu_stats_font_cell_h[p_font];
	const float aw = cw * FONT_COLS, ah = ch * FONT_ROWS;
	const float adv = wiiu_stats_font_advance[p_font];
	const float y0 = Math::floor(p_y), y1 = y0 + ch;
	WiiUVertex v[6 * 80];
	uint32_t n = 0;
	int i = 0;
	for (const char *c = p_text; *c && n + 6 <= 6 * 80; c++, i++) {
		int code = (unsigned char)*c;
		if (code <= 32 || code > 126) {
			continue; // Space (nothing to draw) and anything outside ASCII.
		}
		const int g = code - 32;
		const float u0 = (float)(g % FONT_COLS) * cw / aw, v0 = (float)(g / FONT_COLS) * ch / ah;
		const float u1 = u0 + cw / aw, v1 = v0 + ch / ah;
		const float x0 = Math::floor(p_x + (float)i * adv + 0.5f), x1 = x0 + cw;
		const float xs[6] = { x0, x1, x1, x0, x1, x0 };
		const float ys[6] = { y0, y0, y1, y0, y1, y1 };
		const float us[6] = { u0, u1, u1, u0, u1, u0 };
		const float vs[6] = { v0, v0, v1, v0, v1, v1 };
		for (int k = 0; k < 6; k++) {
			WiiUVertex &vert = v[n + k];
			vert.x = xs[k];
			vert.y = ys[k];
			vert.z = 0.0f;
			vert.u = us[k];
			vert.v = vs[k];
			vert.r = p_r;
			vert.g = p_g;
			vert.b = p_b;
			vert.a = 1.0f;
		}
		n += 6;
	}
	if (n > 0) {
		wiiu_gx2_draw(nullptr, WIIU_PRIM_TRIANGLES, atlas, WIIU_FILTER_LINEAR, false, v, n);
	}
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

// Frame breakdown from the marks: 0 = loop start, 1 = rendering starts,
// 2 = rendering done. Summed over the text update interval, then averaged.
OSTime mark_time[3] = {};
double sum_game_ms = 0.0, sum_render_ms = 0.0, sum_wait_ms = 0.0;
int sum_frames = 0;
float avg_game_ms = 0.0f, avg_render_ms = 0.0f, avg_wait_ms = 0.0f;

double mb(uint64_t p_bytes) {
	return (double)p_bytes / (1024.0 * 1024.0);
}

OSTime text_last_update = 0;

// Rebuilds the table rows (twice a second, so they're readable).
void update_lines() {
	line_count = 0;
	auto add = [](bool p_bad, const char *p_key, const char *p_value, const char *p_note_fmt, auto... p_args) {
		if (line_count < MAX_LINES) {
			Row &r = lines[line_count++];
			snprintf(r.key, sizeof(r.key), "%s", p_key);
			snprintf(r.value, sizeof(r.value), "%s", p_value);
			snprintf(r.note, sizeof(r.note), p_note_fmt, p_args...);
			r.bad = p_bad;
		}
	};
	char v[32];

	// Frame timing over the last second.
	const int n = MIN(frame_filled, 60);
	float sum = 0.0f, mn = 1e9f, mx = 0.0f;
	for (int i = 0; i < n; i++) {
		const float f = frame_ms[(frame_head - 1 - i + FRAME_HISTORY) % FRAME_HISTORY];
		sum += f;
		mn = MIN(mn, f);
		mx = MAX(mx, f);
	}
	const float avg = n ? sum / (float)n : 0.0f;
	if (n == 0) {
		mn = 0.0f;
	}
	Performance *perf = Performance::get_singleton();

	WiiUGX2Stats gx2;
	wiiu_gx2_get_stats(&gx2);
	if (sum_frames > 0) {
		avg_game_ms = (float)(sum_game_ms / sum_frames);
		avg_render_ms = (float)(sum_render_ms / sum_frames);
		avg_wait_ms = (float)(sum_wait_ms / sum_frames);
		sum_game_ms = sum_render_ms = sum_wait_ms = 0.0;
		sum_frames = 0;
	}
	// Main thread (core 1): game = input, scripts, physics, animation, UI;
	// render = walking the canvas and building GX2 draws; vsync = waiting.
	const float busy = avg_game_ms + avg_render_ms;
	const double fps = Engine::get_singleton()->get_frames_per_second();
	const double cpu_pct = avg > 0.0f ? (double)(busy * 100.0f / avg) : 0.0;
	const double gpu_pct = avg > 0.0f ? (double)(gx2.gpu_ms * 100.0f / avg) : 0.0;

	snprintf(v, sizeof(v), "%.0f", fps);
	add(fps < 55.0, "FPS", v, "%.1f ms (%.1f-%.1f)", (double)avg, (double)mn, (double)mx);
	snprintf(v, sizeof(v), "%.0f%%", cpu_pct);
	add(busy > 16.7f, "CPU", v, "%.1f ms: game %.1f, render %.1f", (double)busy, (double)avg_game_ms, (double)avg_render_ms);
	snprintf(v, sizeof(v), "%.0f%%", gpu_pct);
	add(gx2.gpu_ms > 16.7f, "GPU", v, "%.1f ms", (double)gx2.gpu_ms);
	if (!expanded) {
		return;
	}

	snprintf(v, sizeof(v), "%.1f ms", (double)avg_wait_ms);
	add(false, "Vsync wait", v, "%s", "");
	snprintf(v, sizeof(v), "%u", (unsigned)gx2.draw_done_count);
	add(gx2.draw_done_ms > 2.0f, "GPU waits", v, "%.1f ms in DrawDone", (double)gx2.draw_done_ms);
	snprintf(v, sizeof(v), "+%u/~%u", (unsigned)gx2.tex_new, (unsigned)gx2.tex_updates);
	add(false, "Tex/frame", v, "%u freed", (unsigned)gx2.tex_freed);
	snprintf(v, sizeof(v), "%u", (unsigned)gx2.draws);
	add(false, "Draws", v, "%u vertices", (unsigned)gx2.vertices);
	snprintf(v, sizeof(v), "%u", (unsigned)gx2.textures);
	add(false, "Textures", v, "%.1f MB", mb(gx2.texture_bytes));
	snprintf(v, sizeof(v), "%u", (unsigned)gx2.render_targets);
	add(false, "Targets", v, "%.1f MB", mb(gx2.render_target_bytes));

	// Memory. No mallinfo: it walks the heap under the allocator lock and took
	// ~20 ms with FNF's 280 MB arena, which stalled AX (a short buzz on every
	// refresh, with no underrun of our own). No MEMGetTotalFreeSizeForExpHeap /
	// MEMGetAllocatableSizeForExpHeapEx on MEM2 either (interrupts off while
	// they walk; and wut's malloc reserves MEM2 at start-up, so they read 0).
	snprintf(v, sizeof(v), "%.1f MB", mb(Memory::get_mem_usage()));
	add(false, "Heap", v, "peak %.1f MB", mb(Memory::get_mem_max_usage()));
	snprintf(v, sizeof(v), "%.1f MB", mb(wiiu_sbrk_total));
	add(false, "Arena", v, "%s", "malloc");
	MEMHeapHandle mem1 = MEMGetBaseHeapHandle(MEM_BASE_HEAP_MEM1);
	snprintf(v, sizeof(v), "%.2f MB", mb(MEMGetAllocatableSizeForFrmHeapEx(mem1, 4)));
	add(false, "MEM1 free", v, "%s", "");
	if (perf) {
		snprintf(v, sizeof(v), "%.0f", perf->get_monitor(Performance::OBJECT_COUNT));
		add(false, "Objects", v, "%.0f nodes, %.0f orphans", perf->get_monitor(Performance::OBJECT_NODE_COUNT),
				perf->get_monitor(Performance::OBJECT_ORPHAN_NODE_COUNT));
		// TIME_PROCESS includes rendering and the vsync wait here, so only physics.
		snprintf(v, sizeof(v), "%.2f ms", perf->get_monitor(Performance::TIME_PHYSICS_PROCESS) * 1000.0);
		add(false, "Physics", v, "%s", "per step");
	}
	snprintf(v, sizeof(v), "%u", (unsigned)wiiu_audio_underruns);
	add(wiiu_audio_underruns > 0, "Audio", v, "%s", "underruns");

	const uint64_t up = OS::get_singleton()->get_ticks_msec() / 1000;
	snprintf(v, sizeof(v), "%02u:%02u:%02u", (unsigned)(up / 3600), (unsigned)(up / 60 % 60), (unsigned)(up % 60));
	if (battery <= 6) {
		add(false, "Uptime", v, "GamePad battery %u/6", (unsigned)battery);
	} else {
		add(false, "Uptime", v, "%s", "");
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
	sum_frames = 0;
	sum_game_ms = sum_render_ms = sum_wait_ms = 0.0;
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
	if (arrow_held) {
		// The tap that toggled the panel is still down; it isn't the game's.
		if (!p_down) {
			arrow_held = false;
			return false;
		}
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
	const float hit = ARROW_HIT * screen_scale(WIIU_SCREEN_DRC);
	if (p_x >= x + w - hit && p_y < y + hit) {
		expanded = !expanded;
		arrow_held = true;
		line_count = 0;
		text_last_update = 0;
		return true;
	}
	dragging = true;
	drag_x = x / 854.0f;
	drag_y = y / 480.0f;
	grab_dx = p_x / 854.0f - drag_x;
	grab_dy = p_y / 480.0f - drag_y;
	return true;
}

extern "C" void wiiu_stats_mark(int p_mark) {
	const OSTime now = OSGetSystemTime();
	if (p_mark == 0 && mark_time[0] && mark_time[1] && mark_time[2]) {
		// One whole frame is marked: add it up.
		WiiUGX2Stats gx2;
		wiiu_gx2_get_stats(&gx2);
		sum_game_ms += (double)OSTicksToMicroseconds(mark_time[1] - mark_time[0]) / 1000.0;
		sum_render_ms += (double)OSTicksToMicroseconds(mark_time[2] - mark_time[1]) / 1000.0;
		sum_wait_ms += gx2.flip_wait_ms;
		sum_frames++;
		mark_time[1] = mark_time[2] = 0;
	}
	if (p_mark >= 0 && p_mark < 3) {
		mark_time[p_mark] = now;
	}
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
	// Twice a second: collecting everything each refresh plus drawing the panel
	// made red spikes in the graph at 4 refreshes a second.
	if (!text_last_update || OSTicksToMilliseconds(now - text_last_update) >= 500) {
		text_last_update = now;
		update_lines();
		update_cost_ms = (float)((double)OSTicksToMicroseconds(OSGetSystemTime() - now) / 1000.0);
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

	// Every rectangle (background, graph, 16.7 ms line, arrow) goes into one
	// untextured draw; the text is one draw per cell.
	static WiiUVertex quads[6 * (FRAME_HISTORY + 4) + 3];
	uint32_t n = 0;
	auto vert = [&](float p_x, float p_y, float p_r, float p_g, float p_b, float p_a) {
		WiiUVertex &v = quads[n++];
		v.x = p_x;
		v.y = p_y;
		v.z = 0.0f;
		v.u = v.v = 0.5f;
		v.r = p_r;
		v.g = p_g;
		v.b = p_b;
		v.a = p_a;
	};
	auto quad = [&](float p_x, float p_y, float p_w, float p_h, float p_r, float p_g, float p_b, float p_a) {
		const float xs[6] = { p_x, p_x + p_w, p_x + p_w, p_x, p_x + p_w, p_x };
		const float ys[6] = { p_y, p_y, p_y + p_h, p_y, p_y + p_h, p_y + p_h };
		for (int k = 0; k < 6; k++) {
			vert(xs[k], ys[k], p_r, p_g, p_b, p_a);
		}
	};
	quad(x, y, w, h, 0.0f, 0.0f, 0.0f, dragging ? 0.85f : 0.75f);

	const int fi = font_index(p_screen);
	const float line = row_height(fi);
	const float gy = y + PAD * s + line * (float)line_count + 4.0f * s;

	// Frame time graph: one bar per frame, newest on the right, up to 33.3 ms
	// (two frames at 60 Hz); the line marks 16.7 ms. Grey bars, red over budget.
	const float gx = x + PAD * s;
	const float gw = w - PAD * s * 2.0f;
	const float gh = GRAPH_H * s;
	const float bar_w = gw / (float)FRAME_HISTORY;
	for (int i = 0; i < frame_filled; i++) {
		const float f = frame_ms[(frame_head - frame_filled + i + FRAME_HISTORY) % FRAME_HISTORY];
		const float bh = MIN(f / 33.3f, 1.0f) * gh;
		const int slot = FRAME_HISTORY - frame_filled + i;
		if (f > 17.5f) {
			quad(gx + (float)slot * bar_w, gy + gh - bh, MAX(bar_w - 1.0f, 1.0f), bh, 1.0f, 0.38f, 0.38f, 1.0f);
		} else {
			quad(gx + (float)slot * bar_w, gy + gh - bh, MAX(bar_w - 1.0f, 1.0f), bh, 0.78f, 0.78f, 0.78f, 1.0f);
		}
	}
	quad(gx, gy + gh * 0.5f, gw, MAX(1.0f, s), 0.5f, 0.5f, 0.5f, 1.0f);

	// Expand arrow, top right: points down when collapsed, up when expanded.
	const float ax = x + w - 18.0f * s, ay = y + 14.0f * s, ar = 5.0f * s;
	if (expanded) {
		vert(ax - ar, ay + ar * 0.6f, 0.7f, 0.7f, 0.7f, 1.0f);
		vert(ax + ar, ay + ar * 0.6f, 0.7f, 0.7f, 0.7f, 1.0f);
		vert(ax, ay - ar * 0.8f, 0.7f, 0.7f, 0.7f, 1.0f);
	} else {
		vert(ax - ar, ay - ar * 0.6f, 0.7f, 0.7f, 0.7f, 1.0f);
		vert(ax + ar, ay - ar * 0.6f, 0.7f, 0.7f, 0.7f, 1.0f);
		vert(ax, ay + ar * 0.8f, 0.7f, 0.7f, 0.7f, 1.0f);
	}
	wiiu_gx2_draw(nullptr, WIIU_PRIM_TRIANGLES, nullptr, 0, false, quads, n);

	const float adv = wiiu_stats_font_advance[fi];
	float ty = y + PAD * s;
	for (int i = 0; i < line_count; i++) {
		const Row &r = lines[i];
		draw_text(fi, x + PAD * s, ty, r.key, 0.6f, 0.6f, 0.6f);
		if (r.bad) {
			draw_text(fi, x + PAD * s + COL_VALUE * adv, ty, r.value, 1.0f, 0.38f, 0.38f);
		} else {
			draw_text(fi, x + PAD * s + COL_VALUE * adv, ty, r.value, 0.92f, 0.92f, 0.92f);
		}
		draw_text(fi, x + PAD * s + COL_NOTE * adv, ty, r.note, 0.6f, 0.6f, 0.6f);
		ty += line;
	}
	if (dragging) {
		draw_text(fi, x + w - PAD * s - text_width(fi, 15), y + h - row_height(fi) - 2.0f, "release to snap", 0.7f, 0.7f, 0.7f);
	}
}
