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

// Canvas blend modes (Godot's render_mode blend_* / CanvasItemMaterial).
typedef enum {
	WIIU_BLEND_MIX = 0,
	WIIU_BLEND_ADD,
	WIIU_BLEND_SUB,
	WIIU_BLEND_MUL,
	WIIU_BLEND_PREMUL_ALPHA,
	WIIU_BLEND_DISABLED,
} WiiUBlend;

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
// Tiled (GPU-native layout, much faster to sample); falls back to linear in
// the background. Uploading waits for the GPU, so use it for game textures,
// not for things made during a crash.
WiiUTexture *wiiu_gx2_texture_create_tiled(uint32_t p_width, uint32_t p_height, const uint8_t *p_rgba);
void wiiu_gx2_texture_update(WiiUTexture *p_texture, const uint8_t *p_rgba);
// With mipmaps: p_levels[0..p_count-1] are RGBA8 levels, each level half the
// previous one (at least 1x1). Mipmaps need the tiled path; in the background
// only level 0 is kept.
WiiUTexture *wiiu_gx2_texture_create_tiled_levels(uint32_t p_width, uint32_t p_height, const uint8_t *const *p_levels, uint32_t p_count);
void wiiu_gx2_texture_update_levels(WiiUTexture *p_texture, const uint8_t *const *p_levels, uint32_t p_count);
// Number of mip levels of the GPU texture (1 without mipmaps).
uint32_t wiiu_gx2_texture_levels(const WiiUTexture *p_texture);
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
// Shader programs compiled at runtime from GLSL 330 with CafeGLSL, in
// uniform register mode. Vertex inputs: VertexPosition (vec3, clip space),
// VertexTexCoord (vec2), VertexColor (vec4). NULL without CafeGLSL or on a
// compile error (the error goes to r_log).
typedef struct WiiUProgram WiiUProgram;
bool wiiu_gx2_glsl_available(void);
WiiUProgram *wiiu_gx2_program_compile(const char *p_vs, const char *p_ps, char *r_log, int p_log_size);
void wiiu_gx2_program_free(WiiUProgram *p_program);
// Pixel shader lookups: a uniform's register offset (32-bit words, as
// GX2SetPixelUniformReg takes it) or a sampler's location. -1 if the shader
// doesn't have it (or the compiler optimized it out).
int32_t wiiu_gx2_program_uniform_offset(const WiiUProgram *p_program, const char *p_name);
int32_t wiiu_gx2_program_sampler_location(const WiiUProgram *p_program, const char *p_name);

// A canvas_item shader program for the following wiiu_gx2_draw calls (NULL:
// the built-in shaders). p_regs: the pixel uniform registers from word 0
// (material parameters); the draw adds the built-ins it knows (time,
// color_texture_pixel_size, screen_pixel_size). The draw's texture goes to
// the color_texture sampler, p_textures to theirs. The pointers must stay
// valid until the draws are done; whoever sets a program sets NULL afterwards.
typedef struct {
	WiiUTexture *texture;
	int32_t location;
	int filter; // WIIU_FILTER_* bits.
	bool repeat;
} WiiUProgramTexture;
void wiiu_gx2_set_program(WiiUProgram *p_program, const uint32_t *p_regs, uint32_t p_reg_words,
		const WiiUProgramTexture *p_textures, uint32_t p_texture_count, float p_time);
// 1x1 textures for shader texture defaults (hint_default_black/transparent).
WiiUTexture *wiiu_gx2_black_texture(void);
WiiUTexture *wiiu_gx2_transparent_texture(void);

// MSDF text for the following wiiu_gx2_draw calls: the texture is a
// multichannel signed distance field (Godot's FontFile MSDF mode). p_px_range
// 0 turns it off; p_outline is the outline size in pixels (0: fill).
// p_px_size: screen pixels per distance-field unit (sharpness).
void wiiu_gx2_set_msdf(float p_px_range, float p_outline, float p_px_size);
// Blend for the following wiiu_gx2_draw calls (WIIU_BLEND_MIX by default).
// Whoever sets another mode sets it back to mix afterwards.
void wiiu_gx2_set_blend(WiiUBlend p_blend);
// Texture filter for wiiu_gx2_draw: bit 0 linear (else nearest), bit 1 use the
// texture's mipmaps (trilinear when linear).
#define WIIU_FILTER_LINEAR 1
#define WIIU_FILTER_MIPMAPS 2
// Draws p_count vertices (pixel coordinates of the bound target).
void wiiu_gx2_draw(WiiUPrimitive p_primitive, WiiUTexture *p_texture, int p_filter, bool p_repeat,
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
