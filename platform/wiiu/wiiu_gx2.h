#pragma once

// GX2 layer for Cafe Godot's renderer (wiiu_gx2.c). Plain C so it can be used
// from the C++ renderer (rasterizer_wiiu.cpp) and the C platform code.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One vertex of texture.gsh: position (render-target pixels when passed to
// wiiu_gx2_draw; converted to clip space there), texture coordinate, colour.
typedef struct {
	float x, y, z;
	float u, v;
	float r, g, b, a;
} WiiUVertex;

typedef enum {
	WIIU_PRIM_TRIANGLES = 0,
	WIIU_PRIM_LINES = 1,
} WiiUPrimitive;

typedef enum {
	WIIU_SCREEN_TV = 0,
	WIIU_SCREEN_DRC = 1,
} WiiUScreen;

typedef struct WiiUTexture WiiUTexture;
typedef struct WiiURenderTarget WiiURenderTarget;

bool wiiu_gx2_init(void);
void wiiu_gx2_shutdown(void);
bool wiiu_gx2_is_active(void);
// True while the app has the foreground and the screens can be drawn.
bool wiiu_gx2_can_draw(void);

void wiiu_gx2_screen_size(WiiUScreen p_screen, uint32_t *r_width, uint32_t *r_height);

// Textures: RGBA8, rows of p_width * 4 bytes.
WiiUTexture *wiiu_gx2_texture_create(uint32_t p_width, uint32_t p_height, const uint8_t *p_rgba);
void wiiu_gx2_texture_update(WiiUTexture *p_texture, const uint8_t *p_rgba);
void wiiu_gx2_texture_free(WiiUTexture *p_texture);
// A 1x1 opaque white texture, for untextured drawing.
WiiUTexture *wiiu_gx2_white_texture(void);

// Render targets: a colour buffer that can also be sampled as a texture.
WiiURenderTarget *wiiu_gx2_render_target_create(uint32_t p_width, uint32_t p_height);
void wiiu_gx2_render_target_free(WiiURenderTarget *p_rt);
WiiUTexture *wiiu_gx2_render_target_texture(WiiURenderTarget *p_rt);

// Per frame: begin (resets the vertex arena), draw into targets, present.
void wiiu_gx2_frame_begin(void);
void wiiu_gx2_frame_present(void);

// Binding a target sets the viewport and scissor to the whole target.
void wiiu_gx2_bind_render_target(WiiURenderTarget *p_rt);
void wiiu_gx2_bind_screen(WiiUScreen p_screen);
void wiiu_gx2_clear(float p_r, float p_g, float p_b, float p_a);
void wiiu_gx2_set_scissor(int32_t p_x, int32_t p_y, int32_t p_width, int32_t p_height);
// Draws p_count vertices (pixel coordinates of the bound target).
void wiiu_gx2_draw(WiiUPrimitive p_primitive, WiiUTexture *p_texture, bool p_linear, bool p_repeat,
		const WiiUVertex *p_vertices, uint32_t p_count);

// Numbers for the stats overlay (wiiu_stats.cpp). Draws/vertices are from the
// last finished frame; GPU time is the frame's span on the GPU, two frames late.
typedef struct {
	uint32_t draws;
	uint32_t vertices;
	uint32_t textures;
	uint64_t texture_bytes;
	uint32_t render_targets;
	uint64_t render_target_bytes;
	float gpu_ms;
	float flip_wait_ms; // Last frame, in GX2WaitForFlip (vsync).
} WiiUGX2Stats;
void wiiu_gx2_get_stats(WiiUGX2Stats *r_stats);

// Debug (HOME hang hunt): a short description of what the main thread is doing.
extern volatile const char *wiiu_main_state;

// Text and rectangles drawn with GX2 alone (crash overlay, log), using the
// built-in 10x20 bitmap font (crash_font_data.c, ASCII 32-126).
#define WIIU_FONT_CELL_W 10
#define WIIU_FONT_CELL_H 20
void wiiu_gx2_draw_rect(float p_x, float p_y, float p_w, float p_h, float p_r, float p_g, float p_b, float p_a);
// One line at (p_x, p_y), top-left; characters outside ASCII 32-126 draw as '?'.
void wiiu_gx2_draw_text(float p_x, float p_y, const char *p_text, float p_r, float p_g, float p_b, float p_a);

// Keeps a copy of the last frame of both screens (crash overlay).
bool wiiu_gx2_save_frame(void);
// Draws the saved frame over the whole bound screen.
void wiiu_gx2_draw_saved_frame(WiiUScreen p_screen);

#ifdef __cplusplus
}
#endif
