// GX2 layer for Cafe Godot: owns the TV and GamePad (DRC) outputs and provides
// the few drawing primitives the 2D renderer (rasterizer_wiiu.cpp) needs:
// textures, render targets, clears, scissor and batched textured triangles.
//
// Modelled on Love Potion's driver/display (MIT, (c) 2018-present Serena S.
// Postelnek & Logan Hickok-Dickson) and wut's WHBGfx. All drawing goes through
// texture.gsh (shaders/README.md): vertices are converted to clip space on the
// CPU, so the shader's Transformation block holds two identity matrices.
//
// Memory: the scan buffers and screen colour buffers live in the MEM1 frame
// heap and are only valid while the app has the foreground. They're allocated
// in the ProcUI "acquire" callback and freed in "release" (HOME menu), so they
// don't leak into the HOME menu's memory. Textures, render targets and the
// vertex arenas live in MEM2 and survive the HOME menu. OSScreen (the log
// console in wiiu_console.c) also uses the MEM1 frame heap and the display
// controller, so only one of the two may own the display at a time: the
// console stays suspended while GX2 runs and resumes after wiiu_gx2_shutdown
// (e.g. on the crash screen).
//
// Must only be called from the main thread (core 1), which owns GX2.

#include "wiiu_gx2.h"

#include <coreinit/cache.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/memfrmheap.h>
#include <coreinit/memheap.h>
#include <coreinit/time.h>
#include <gx2/clear.h>
#include <gx2/context.h>
#include <gx2/display.h>
#include <gx2/draw.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/sampler.h>
#include <gx2/shaders.h>
#include <gx2/state.h>
#include <gx2/surface.h>
#include <gx2/swap.h>
#include <gx2/texture.h>
#include <gx2/utils.h>
#include <proc_ui/procui.h>
#include <stdlib.h>
#include <string.h>
#include <whb/gfx.h>
#include <whb/log.h>

// GPU timestamps (gx2.rpl exports these; wut has no header for them).
extern void GX2SampleTopGPUCycle(uint64_t *result);
extern void GX2SampleBottomGPUCycle(uint64_t *result);
extern uint64_t GX2GPUTimeToCPUTime(uint64_t time);

// Counters for the stats overlay (wiiu_stats.cpp).
static uint32_t stat_draws, stat_vertices; // This frame so far.
static uint32_t stat_last_draws, stat_last_vertices; // Last finished frame.
static uint32_t stat_textures, stat_render_targets;
static uint64_t stat_texture_bytes, stat_render_target_bytes;
// GPU time per frame: top-of-pipe at frame start, bottom-of-pipe after the
// last draw; read back two frames later (the GPU writes them).
#define GPU_TS_SLOTS 4
static uint64_t *gpu_ts = NULL; // [slot][top, bottom]
static uint32_t gpu_ts_frame = 0;
static float stat_gpu_ms = 0.0f;
static float stat_flip_wait_ms = 0.0f;
static WiiUBlend draw_blend = WIIU_BLEND_MIX;

void wiiu_gx2_set_blend(WiiUBlend p_blend) {
	draw_blend = p_blend;
}
// Draws since the last GX2Flush. GX2 keeps commands in its buffer until a
// flush, so without these the GPU only started on a frame at present time and
// CPU + GPU time added up (7.5 + 11.4 ms: every vblank missed, 30 fps).
#define DRAWS_PER_FLUSH 16
static uint32_t draws_since_flush = 0;

#define GX2_FRAME_HEAP_TAG 0x47583242 // "GX2B"

#define TV_WIDTH 1280
#define TV_HEIGHT 720
#define DRC_WIDTH 854
#define DRC_HEIGHT 480

#define SCAN_BUFFER_ALIGNMENT 0x1000

// Two vertex arenas, used on alternate frames: the GPU reads frame N's
// vertices while the CPU fills frame N+1's.
#define ARENA_BYTES (2 * 1024 * 1024)

extern const uint8_t wiiu_shader_texture[];

// wiiu_system_font.cpp
bool wiiu_sysfont_init(void);
void wiiu_sysfont_free(void);

// crash_font_data.c
extern const uint8_t wiiu_crash_font[][WIIU_FONT_CELL_W * WIIU_FONT_CELL_H];
extern const uint32_t wiiu_crash_font_first;
extern const uint32_t wiiu_crash_font_count;
#define FONT_COLUMNS 16

typedef struct {
	GX2ColorBuffer color_buffer;
	void *scan_buffer;
	uint32_t scan_buffer_size;
} Screen;

struct WiiUTexture {
	GX2Texture texture;
	uint32_t width;
	uint32_t height;
	bool owns_image;
};

struct WiiURenderTarget {
	GX2ColorBuffer color_buffer;
	WiiUTexture texture; // Shares the colour buffer's image.
};

static bool gx2_active = false;
static bool gx2_acquired = false;
static void *gx2_command_buffer = NULL;
static GX2ContextState *gx2_context = NULL;
static Screen gx2_tv;
static Screen gx2_drc;

static bool draw_ready = false;
static WHBGfxShaderGroup draw_shader;
static uint32_t draw_uniform_location = 0;
static uint32_t draw_sampler_location = 0;
static uint32_t *draw_uniforms = NULL; // 32 words, byte-swapped
static GX2Sampler draw_samplers[2][2]; // [linear][repeat]
static WiiUTexture *draw_white = NULL;
static WiiUTexture *draw_font = NULL; // FONT_COLUMNS glyphs per row
static WiiUTexture *saved_frame[2] = { NULL, NULL }; // [WiiUScreen]

static uint8_t *arena[2] = { NULL, NULL };
static uint32_t arena_index = 0;
static uint32_t arena_used = 0;

// The bound target.
static GX2ColorBuffer *target_cb = NULL;
static WiiURenderTarget *target_rt = NULL;
static uint32_t target_width = 0;
static uint32_t target_height = 0;
static int32_t scissor_x = 0, scissor_y = 0, scissor_w = 0, scissor_h = 0;

// ---------------------------------------------------------------------------
// Screens and ProcUI
// ---------------------------------------------------------------------------

static void init_color_buffer(GX2ColorBuffer *cb, uint32_t width, uint32_t height, GX2SurfaceUse use) {
	memset(cb, 0, sizeof(*cb));
	cb->surface.use = use;
	cb->surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
	cb->surface.width = width;
	cb->surface.height = height;
	cb->surface.depth = 1;
	cb->surface.mipLevels = 1;
	cb->surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
	cb->surface.aa = GX2_AA_MODE1X;
	cb->surface.tileMode = GX2_TILE_MODE_DEFAULT;
	cb->viewNumSlices = 1;
	GX2CalcSurfaceSizeAndAlignment(&cb->surface);
	GX2InitColorBufferRegs(cb);
}

static bool alloc_screen_memory(MEMHeapHandle heap, Screen *s, bool tv) {
	s->scan_buffer = MEMAllocFromFrmHeapEx(heap, s->scan_buffer_size, SCAN_BUFFER_ALIGNMENT);
	s->color_buffer.surface.image =
			MEMAllocFromFrmHeapEx(heap, s->color_buffer.surface.imageSize, s->color_buffer.surface.alignment);
	if (s->scan_buffer == NULL || s->color_buffer.surface.image == NULL) {
		return false;
	}
	// The display reads the scan buffer directly: flush it out of the CPU cache.
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU, s->scan_buffer, s->scan_buffer_size);
	// The colour buffer is GPU-only too: no dirty CPU lines left on top.
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU, s->color_buffer.surface.image, s->color_buffer.surface.imageSize);
	if (tv) {
		GX2SetTVBuffer(s->scan_buffer, s->scan_buffer_size, GX2_TV_RENDER_MODE_WIDE_720P,
				GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, GX2_BUFFERING_MODE_DOUBLE);
		GX2SetTVScale(TV_WIDTH, TV_HEIGHT);
	} else {
		GX2SetDRCBuffer(s->scan_buffer, s->scan_buffer_size, GX2_DRC_RENDER_MODE_SINGLE,
				GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, GX2_BUFFERING_MODE_DOUBLE);
		GX2SetDRCScale(DRC_WIDTH, DRC_HEIGHT);
	}
	return true;
}

static uint32_t gx2_proc_acquired(void *context) {
	(void)context;
	if (!gx2_active) {
		return 0;
	}
	MEMHeapHandle heap = MEMGetBaseHeapHandle(MEM_BASE_HEAP_MEM1);
	MEMRecordStateForFrmHeap(heap, GX2_FRAME_HEAP_TAG);
	if (!alloc_screen_memory(heap, &gx2_tv, true) || !alloc_screen_memory(heap, &gx2_drc, false)) {
		WHBLogPrint("[gx2] out of MEM1 for the scan/colour buffers");
		MEMFreeByStateToFrmHeap(heap, GX2_FRAME_HEAP_TAG);
		return 0;
	}
	gx2_acquired = true;
	return 0;
}

static uint32_t gx2_proc_released(void *context) {
	(void)context;
	if (!gx2_acquired) {
		return 0;
	}
	// Let the GPU finish with the buffers before they go back to the system.
	// GX2DrawDone gives up (FALSE) after the GPU timeout: then the GPU hung.
	WHBLogPrint("[home] gx2: GX2DrawDone"); // Debug (HOME freeze), temporary.
	if (!GX2DrawDone()) {
		WHBLogPrint("[gx2] GPU timed out while releasing the foreground (GPU hung)");
	}
	// With one frame in flight a swap can still be queued: let it flip before
	// the scan buffers go back to the system (at most ~3 frames).
	for (int i = 0; i < 6; i++) {
		uint32_t swap_count = 0, flip_count = 0;
		OSTime last_flip = 0, last_vsync = 0;
		GX2GetSwapStatus(&swap_count, &flip_count, &last_flip, &last_vsync);
		if (flip_count >= swap_count) {
			break;
		}
		GX2WaitForFlip();
	}
	WHBLogPrint("[home] gx2: freeing screen buffers");
	gx2_acquired = false;
	if (target_cb == &gx2_tv.color_buffer || target_cb == &gx2_drc.color_buffer) {
		target_cb = NULL;
	}
	MEMFreeByStateToFrmHeap(MEMGetBaseHeapHandle(MEM_BASE_HEAP_MEM1), GX2_FRAME_HEAP_TAG);
	// As SDL2's Wii U backend does after freeing its buffers ("necessary to
	// avoid a black frame on leaving foreground").
	GX2SetTVEnable(TRUE);
	GX2SetDRCEnable(TRUE);
	WHBLogPrint("[home] gx2: released");
	return 0;
}

bool wiiu_gx2_is_active(void) {
	return gx2_active;
}

bool wiiu_gx2_can_draw(void) {
	return gx2_active && gx2_acquired && draw_ready;
}

void wiiu_gx2_screen_size(WiiUScreen p_screen, uint32_t *r_width, uint32_t *r_height) {
	*r_width = p_screen == WIIU_SCREEN_TV ? TV_WIDTH : DRC_WIDTH;
	*r_height = p_screen == WIIU_SCREEN_TV ? TV_HEIGHT : DRC_HEIGHT;
}

// ---------------------------------------------------------------------------
// Textures and render targets
// ---------------------------------------------------------------------------

static void init_texture_view(WiiUTexture *t, const GX2Surface *surface) {
	memset(&t->texture, 0, sizeof(t->texture));
	t->texture.surface = *surface;
	t->texture.viewNumMips = 1;
	t->texture.viewNumSlices = 1;
	t->texture.compMap = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A);
	GX2InitTextureRegs(&t->texture);
	t->width = surface->width;
	t->height = surface->height;
}

static void apply_target(void);

// Fills a tiled texture from RGBA rows: the CPU writes a linear staging
// surface and the GPU converts it with GX2CopySurface. Needs the foreground
// (GPU commands); waits for the copy so the staging memory can go.
// Fills a tiled texture from RGBA rows: the CPU writes a linear staging
// surface and the GPU converts it with GX2CopySurface. Needs the foreground
// (GPU commands); waits for the copy so the staging memory can go.
static bool upload_tiled(WiiUTexture *p_texture, const uint8_t *p_rgba) {
	if (!wiiu_gx2_can_draw()) {
		return false;
	}
	GX2Surface staging = p_texture->texture.surface;
	staging.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
	staging.swizzle = 0; // The tiled surface's swizzle scrambles a linear one.
	staging.image = NULL;
	staging.mipmaps = NULL;
	GX2CalcSurfaceSizeAndAlignment(&staging);
	staging.image = MEMAllocFromDefaultHeapEx(staging.imageSize, staging.alignment);
	if (staging.image == NULL) {
		return false;
	}
	uint8_t *image = (uint8_t *)staging.image;
	const uint32_t row = p_texture->width * 4;
	if (p_rgba == NULL) {
		memset(image, 0, staging.imageSize);
	} else {
		for (uint32_t y = 0; y < p_texture->height; y++) {
			memcpy(image + y * staging.pitch * 4, p_rgba + y * row, row);
		}
	}
	GX2Surface *dst = &p_texture->texture.surface;
	// Staging: out of the CPU cache, and drop GPU texture cache lines of an
	// earlier buffer at the same address. Destination: no stale GPU lines.
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, staging.image, staging.imageSize);
	GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE | GX2_INVALIDATE_MODE_COLOR_BUFFER, dst->image, dst->imageSize);
	GX2CopySurface(&staging, 0, 0, dst, 0, 0);
	GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER | GX2_INVALIDATE_MODE_TEXTURE, dst->image, dst->imageSize);
	// GX2CopySurface changes GPU state: put ours back (target, viewport, scissor).
	GX2SetContextState(gx2_context);
	if (target_cb != NULL) {
		apply_target();
	}
	GX2DrawDone();
	MEMFreeToDefaultHeap(staging.image);
	return true;
}

// Linear textures (p_tiled false) are a plain row copy, also without the
// foreground; tiled ones sample much faster but need the GPU to upload.
static WiiUTexture *texture_create(uint32_t p_width, uint32_t p_height, const uint8_t *p_rgba, bool p_tiled);

WiiUTexture *wiiu_gx2_texture_create(uint32_t p_width, uint32_t p_height, const uint8_t *p_rgba) {
	return texture_create(p_width, p_height, p_rgba, false);
}

WiiUTexture *wiiu_gx2_texture_create_tiled(uint32_t p_width, uint32_t p_height, const uint8_t *p_rgba) {
	// Tiling needs the GPU; in the background (HOME menu) fall back to linear.
	return texture_create(p_width, p_height, p_rgba, wiiu_gx2_can_draw());
}

static WiiUTexture *texture_create(uint32_t p_width, uint32_t p_height, const uint8_t *p_rgba, bool p_tiled) {
	WiiUTexture *t = (WiiUTexture *)calloc(1, sizeof(WiiUTexture));
	if (t == NULL) {
		return NULL;
	}
	GX2Surface surface;
	memset(&surface, 0, sizeof(surface));
	surface.width = p_width;
	surface.height = p_height;
	surface.depth = 1;
	surface.mipLevels = 1;
	surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
	surface.aa = GX2_AA_MODE1X;
	surface.use = GX2_SURFACE_USE_TEXTURE;
	surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
	surface.tileMode = p_tiled ? GX2_TILE_MODE_DEFAULT : GX2_TILE_MODE_LINEAR_ALIGNED;
	GX2CalcSurfaceSizeAndAlignment(&surface);
	surface.image = MEMAllocFromDefaultHeapEx(surface.imageSize, surface.alignment);
	if (surface.image == NULL) {
		WHBLogPrintf("[gx2] out of memory for a %ux%u texture", (unsigned)p_width, (unsigned)p_height);
		free(t);
		return NULL;
	}
	// Memory only the GPU writes (tiled: GX2CopySurface) must not keep dirty
	// CPU cache lines from its previous owner: written back later, they land
	// on top of the texture (random garbage in sprites, 2026-11-13).
	if (p_tiled) {
		GX2Invalidate(GX2_INVALIDATE_MODE_CPU, surface.image, surface.imageSize);
	}
	init_texture_view(t, &surface);
	t->owns_image = true;
	stat_textures++;
	stat_texture_bytes += surface.imageSize;
	wiiu_gx2_texture_update(t, p_rgba);
	return t;
}

void wiiu_gx2_texture_update(WiiUTexture *p_texture, const uint8_t *p_rgba) {
	if (p_texture == NULL || !p_texture->owns_image) {
		return;
	}
	GX2Surface *s = &p_texture->texture.surface;
	if (s->tileMode != GX2_TILE_MODE_LINEAR_ALIGNED) {
		if (!upload_tiled(p_texture, p_rgba)) {
			WHBLogPrintf("[gx2] couldn't update a %ux%u tiled texture (background or out of memory)",
					(unsigned)p_texture->width, (unsigned)p_texture->height);
		}
		return;
	}
	uint8_t *image = (uint8_t *)s->image;
	const uint32_t row = p_texture->width * 4;
	if (p_rgba == NULL) {
		memset(image, 0, s->imageSize);
	} else {
		for (uint32_t y = 0; y < p_texture->height; y++) {
			memcpy(image + y * s->pitch * 4, p_rgba + y * row, row);
		}
	}
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU | GX2_INVALIDATE_MODE_TEXTURE, s->image, s->imageSize);
}

void wiiu_gx2_texture_free(WiiUTexture *p_texture) {
	if (p_texture == NULL) {
		return;
	}
	// The GPU may still read it from the last frame. Only while in the
	// foreground: after ProcUI releases it (HOME, or exit, where Godot frees
	// its resources afterwards) GX2 calls fault, and the release callback has
	// already waited for the GPU.
	if (gx2_acquired) {
		GX2DrawDone();
	}
	if (p_texture->owns_image) {
		stat_textures--;
		stat_texture_bytes -= p_texture->texture.surface.imageSize;
		MEMFreeToDefaultHeap(p_texture->texture.surface.image);
	}
	free(p_texture);
}

WiiUTexture *wiiu_gx2_white_texture(void) {
	return draw_white;
}

WiiURenderTarget *wiiu_gx2_render_target_create(uint32_t p_width, uint32_t p_height) {
	WiiURenderTarget *rt = (WiiURenderTarget *)calloc(1, sizeof(WiiURenderTarget));
	if (rt == NULL) {
		return NULL;
	}
	init_color_buffer(&rt->color_buffer, p_width, p_height,
			(GX2SurfaceUse)(GX2_SURFACE_USE_TEXTURE | GX2_SURFACE_USE_COLOR_BUFFER));
	rt->color_buffer.surface.image =
			MEMAllocFromDefaultHeapEx(rt->color_buffer.surface.imageSize, rt->color_buffer.surface.alignment);
	if (rt->color_buffer.surface.image == NULL) {
		WHBLogPrintf("[gx2] out of memory for a %ux%u render target", (unsigned)p_width, (unsigned)p_height);
		free(rt);
		return NULL;
	}
	// Only the GPU writes it: no dirty CPU cache lines on top (see texture_create).
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU, rt->color_buffer.surface.image, rt->color_buffer.surface.imageSize);
	init_texture_view(&rt->texture, &rt->color_buffer.surface);
	rt->texture.owns_image = false;
	stat_render_targets++;
	stat_render_target_bytes += rt->color_buffer.surface.imageSize;
	return rt;
}

void wiiu_gx2_render_target_free(WiiURenderTarget *p_rt) {
	if (p_rt == NULL) {
		return;
	}
	// See wiiu_gx2_texture_free.
	if (gx2_acquired) {
		GX2DrawDone();
	}
	if (target_rt == p_rt) {
		target_rt = NULL;
		target_cb = NULL;
	}
	stat_render_targets--;
	stat_render_target_bytes -= p_rt->color_buffer.surface.imageSize;
	MEMFreeToDefaultHeap(p_rt->color_buffer.surface.image);
	free(p_rt);
}

WiiUTexture *wiiu_gx2_render_target_texture(WiiURenderTarget *p_rt) {
	return p_rt ? &p_rt->texture : NULL;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

static bool draw_init(void) {
	if (!WHBGfxLoadGFDShaderGroup(&draw_shader, 0, wiiu_shader_texture)) {
		WHBLogPrint("[gx2] couldn't load texture.gsh");
		return false;
	}
	WHBGfxInitShaderAttribute(&draw_shader, "VertexPosition", 0, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32_32);
	WHBGfxInitShaderAttribute(&draw_shader, "VertexTexCoord", 0, 12, GX2_ATTRIB_FORMAT_FLOAT_32_32);
	WHBGfxInitShaderAttribute(&draw_shader, "VertexColor", 0, 20, GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32);
	WHBGfxInitFetchShader(&draw_shader);

	GX2UniformBlock *block = GX2GetVertexUniformBlock(draw_shader.vertexShader, "Transformation");
	if (block == NULL) {
		WHBLogPrint("[gx2] texture.gsh has no Transformation block");
		return false;
	}
	draw_uniform_location = block->offset;
	for (uint32_t i = 0; i < draw_shader.pixelShader->samplerVarCount; i++) {
		if (strcmp(draw_shader.pixelShader->samplerVars[i].name, "texture0") == 0) {
			draw_sampler_location = draw_shader.pixelShader->samplerVars[i].location;
		}
	}

	// Two identity matrices (vertices arrive in clip space). GX2 reads uniform
	// blocks little-endian, so every 32-bit word is byte-swapped.
	draw_uniforms = (uint32_t *)MEMAllocFromDefaultHeapEx(GX2_UNIFORM_BLOCK_ALIGNMENT, GX2_UNIFORM_BLOCK_ALIGNMENT);
	for (int m = 0; m < 2; m++) {
		for (int i = 0; i < 16; i++) {
			float v = (i % 5 == 0) ? 1.0f : 0.0f;
			uint32_t bits;
			memcpy(&bits, &v, 4);
			draw_uniforms[m * 16 + i] = __builtin_bswap32(bits);
		}
	}
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU | GX2_INVALIDATE_MODE_UNIFORM_BLOCK, draw_uniforms, GX2_UNIFORM_BLOCK_ALIGNMENT);

	for (int linear = 0; linear < 2; linear++) {
		for (int repeat = 0; repeat < 2; repeat++) {
			GX2InitSampler(&draw_samplers[linear][repeat],
					repeat ? GX2_TEX_CLAMP_MODE_WRAP : GX2_TEX_CLAMP_MODE_CLAMP,
					linear ? GX2_TEX_XY_FILTER_MODE_LINEAR : GX2_TEX_XY_FILTER_MODE_POINT);
		}
	}

	for (int i = 0; i < 2; i++) {
		arena[i] = (uint8_t *)MEMAllocFromDefaultHeapEx(ARENA_BYTES, GX2_VERTEX_BUFFER_ALIGNMENT);
		if (arena[i] == NULL) {
			WHBLogPrint("[gx2] out of memory for the vertex arenas");
			return false;
		}
	}

	static const uint8_t white[4] = { 255, 255, 255, 255 };
	draw_white = wiiu_gx2_texture_create(1, 1, white);

	// The bitmap font as a white RGBA atlas with the glyph coverage in alpha.
	const uint32_t rows = (wiiu_crash_font_count + FONT_COLUMNS - 1) / FONT_COLUMNS;
	const uint32_t aw = FONT_COLUMNS * WIIU_FONT_CELL_W;
	const uint32_t ah = rows * WIIU_FONT_CELL_H;
	uint8_t *atlas = (uint8_t *)calloc(aw * ah, 4);
	if (atlas != NULL) {
		for (uint32_t g = 0; g < wiiu_crash_font_count; g++) {
			const uint32_t gx = (g % FONT_COLUMNS) * WIIU_FONT_CELL_W;
			const uint32_t gy = (g / FONT_COLUMNS) * WIIU_FONT_CELL_H;
			for (uint32_t y = 0; y < WIIU_FONT_CELL_H; y++) {
				for (uint32_t x = 0; x < WIIU_FONT_CELL_W; x++) {
					uint8_t *px = atlas + ((gy + y) * aw + gx + x) * 4;
					px[0] = px[1] = px[2] = 255;
					px[3] = wiiu_crash_font[g][y * WIIU_FONT_CELL_W + x];
				}
			}
		}
		draw_font = wiiu_gx2_texture_create(aw, ah, atlas);
		free(atlas);
	}
	draw_ready = draw_white != NULL && draw_font != NULL;
	if (draw_ready) {
		// CafeStd for the crash overlay, baked now so a crash doesn't need FreeType.
		wiiu_sysfont_init();
	}
	return draw_ready;
}

static void draw_free(void) {
	wiiu_sysfont_free();
	for (int i = 0; i < 2; i++) {
		if (saved_frame[i]) {
			wiiu_gx2_texture_free(saved_frame[i]);
			saved_frame[i] = NULL;
		}
	}
	if (draw_font) {
		wiiu_gx2_texture_free(draw_font);
		draw_font = NULL;
	}
	if (draw_white) {
		wiiu_gx2_texture_free(draw_white);
		draw_white = NULL;
	}
	for (int i = 0; i < 2; i++) {
		if (arena[i]) {
			MEMFreeToDefaultHeap(arena[i]);
			arena[i] = NULL;
		}
	}
	if (draw_uniforms) {
		MEMFreeToDefaultHeap(draw_uniforms);
		draw_uniforms = NULL;
	}
	if (draw_ready) {
		WHBGfxFreeShaderGroup(&draw_shader);
	}
	draw_ready = false;
}

// Colour writes go through the colour-buffer cache; flush them before a
// render target is sampled as a texture.
static void finish_target(void) {
	if (target_rt != NULL) {
		GX2Surface *s = &target_rt->color_buffer.surface;
		GX2Invalidate((GX2InvalidateMode)(GX2_INVALIDATE_MODE_COLOR_BUFFER | GX2_INVALIDATE_MODE_TEXTURE), s->image, s->imageSize);
	}
}

static void apply_target(void) {
	GX2SetContextState(gx2_context);
	GX2SetColorBuffer(target_cb, GX2_RENDER_TARGET_0);
	GX2SetViewport(0.0f, 0.0f, (float)target_width, (float)target_height, 0.0f, 1.0f);
	GX2SetScissor((uint32_t)scissor_x, (uint32_t)scissor_y, (uint32_t)scissor_w, (uint32_t)scissor_h);
}

static void bind(GX2ColorBuffer *cb, WiiURenderTarget *rt) {
	if (cb == target_cb) {
		return;
	}
	finish_target();
	target_cb = cb;
	target_rt = rt;
	target_width = cb->surface.width;
	target_height = cb->surface.height;
	scissor_x = scissor_y = 0;
	scissor_w = (int32_t)target_width;
	scissor_h = (int32_t)target_height;
	apply_target();
}

void wiiu_gx2_bind_render_target(WiiURenderTarget *p_rt) {
	if (p_rt != NULL && wiiu_gx2_can_draw()) {
		bind(&p_rt->color_buffer, p_rt);
	}
}

void wiiu_gx2_bind_screen(WiiUScreen p_screen) {
	if (wiiu_gx2_can_draw()) {
		bind(p_screen == WIIU_SCREEN_TV ? &gx2_tv.color_buffer : &gx2_drc.color_buffer, NULL);
	}
}

void wiiu_gx2_clear(float p_r, float p_g, float p_b, float p_a) {
	if (target_cb == NULL || !wiiu_gx2_can_draw()) {
		return;
	}
	GX2ClearColor(target_cb, p_r, p_g, p_b, p_a);
	// GX2ClearColor changes GPU state; restore ours.
	apply_target();
}

void wiiu_gx2_set_scissor(int32_t p_x, int32_t p_y, int32_t p_width, int32_t p_height) {
	if (target_cb == NULL) {
		return;
	}
	// Clamp to the target: GX2 rejects scissors outside it.
	int32_t x0 = p_x < 0 ? 0 : p_x;
	int32_t y0 = p_y < 0 ? 0 : p_y;
	int32_t x1 = p_x + p_width;
	int32_t y1 = p_y + p_height;
	if (x1 > (int32_t)target_width) {
		x1 = (int32_t)target_width;
	}
	if (y1 > (int32_t)target_height) {
		y1 = (int32_t)target_height;
	}
	if (x1 < x0) {
		x1 = x0;
	}
	if (y1 < y0) {
		y1 = y0;
	}
	scissor_x = x0;
	scissor_y = y0;
	scissor_w = x1 - x0;
	scissor_h = y1 - y0;
	GX2SetScissor((uint32_t)scissor_x, (uint32_t)scissor_y, (uint32_t)scissor_w, (uint32_t)scissor_h);
}

void wiiu_gx2_draw(WiiUPrimitive p_primitive, WiiUTexture *p_texture, bool p_linear, bool p_repeat,
		const WiiUVertex *p_vertices, uint32_t p_count) {
	if (p_count == 0 || target_cb == NULL || !wiiu_gx2_can_draw() || scissor_w == 0 || scissor_h == 0) {
		return;
	}
	const uint32_t bytes = p_count * (uint32_t)sizeof(WiiUVertex);
	if (bytes > ARENA_BYTES) {
		WHBLogPrintf("[gx2] draw of %u vertices is too big for the arena", (unsigned)p_count);
		return;
	}
	if (arena_used + bytes > ARENA_BYTES) {
		// Out of room this frame: wait for the GPU, then reuse the arena.
		GX2DrawDone();
		arena_used = 0;
	}
	WiiUVertex *out = (WiiUVertex *)(arena[arena_index] + arena_used);
	// Render-target pixels (origin top-left) to clip space (+Y up).
	const float sx = 2.0f / (float)target_width;
	const float sy = 2.0f / (float)target_height;
	for (uint32_t i = 0; i < p_count; i++) {
		out[i] = p_vertices[i];
		out[i].x = p_vertices[i].x * sx - 1.0f;
		out[i].y = 1.0f - p_vertices[i].y * sy;
		out[i].z = 0.0f;
	}
	GX2Invalidate((GX2InvalidateMode)(GX2_INVALIDATE_MODE_CPU | GX2_INVALIDATE_MODE_ATTRIBUTE_BUFFER), out, bytes);
	arena_used += (bytes + GX2_VERTEX_BUFFER_ALIGNMENT - 1) & ~(uint32_t)(GX2_VERTEX_BUFFER_ALIGNMENT - 1);

	WiiUTexture *texture = p_texture ? p_texture : draw_white;
	GX2SetShaderMode(GX2_SHADER_MODE_UNIFORM_BLOCK);
	GX2SetFetchShader(&draw_shader.fetchShader);
	GX2SetVertexShader(draw_shader.vertexShader);
	GX2SetPixelShader(draw_shader.pixelShader);
	GX2SetVertexUniformBlock(draw_uniform_location, 32 * 4, draw_uniforms);
	GX2SetAttribBuffer(0, bytes, (uint32_t)sizeof(WiiUVertex), out);
	GX2SetPixelTexture(&texture->texture, draw_sampler_location);
	GX2SetPixelSampler(&draw_samplers[p_linear ? 1 : 0][p_repeat ? 1 : 0], draw_sampler_location);
	GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS);
	GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, FALSE);
	GX2SetColorControl(GX2_LOGIC_OP_COPY, draw_blend == WIIU_BLEND_DISABLED ? 0x00 : 0xFF, FALSE, TRUE);
	// The blend factors Godot's GLES3 renderer uses for each canvas blend mode
	// (straight alpha; colour first, then alpha).
	switch (draw_blend) {
		case WIIU_BLEND_ADD:
			GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_ONE,
					GX2_BLEND_COMBINE_MODE_ADD, TRUE, GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_ONE,
					GX2_BLEND_COMBINE_MODE_ADD);
			break;
		case WIIU_BLEND_SUB: // dst - src * alpha.
			GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_ONE,
					GX2_BLEND_COMBINE_MODE_REV_SUB, TRUE, GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_ONE,
					GX2_BLEND_COMBINE_MODE_REV_SUB);
			break;
		case WIIU_BLEND_MUL:
			GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_DST_COLOR, GX2_BLEND_MODE_ZERO,
					GX2_BLEND_COMBINE_MODE_ADD, TRUE, GX2_BLEND_MODE_DST_ALPHA, GX2_BLEND_MODE_ZERO,
					GX2_BLEND_COMBINE_MODE_ADD);
			break;
		case WIIU_BLEND_PREMUL_ALPHA:
			GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_INV_SRC_ALPHA,
					GX2_BLEND_COMBINE_MODE_ADD, TRUE, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_INV_SRC_ALPHA,
					GX2_BLEND_COMBINE_MODE_ADD);
			break;
		default: // Mix (and disabled, where the mask above turns blending off).
			GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_INV_SRC_ALPHA,
					GX2_BLEND_COMBINE_MODE_ADD, TRUE, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_INV_SRC_ALPHA,
					GX2_BLEND_COMBINE_MODE_ADD);
			break;
	}
	GX2DrawEx(p_primitive == WIIU_PRIM_LINES ? GX2_PRIMITIVE_MODE_LINES : GX2_PRIMITIVE_MODE_TRIANGLES,
			p_count, 0, 1);
	stat_draws++;
	stat_vertices += p_count;
	if (++draws_since_flush >= DRAWS_PER_FLUSH) {
		draws_since_flush = 0;
		GX2Flush(); // Let the GPU start while the CPU builds the rest.
	}
}

// ---------------------------------------------------------------------------
// Overlay drawing (crash screen, log)
// ---------------------------------------------------------------------------

static void push_quad(WiiUVertex *v, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
		float r, float g, float b, float a) {
	const float xs[6] = { x0, x1, x1, x0, x1, x0 };
	const float ys[6] = { y0, y0, y1, y0, y1, y1 };
	const float us[6] = { u0, u1, u1, u0, u1, u0 };
	const float vs[6] = { v0, v0, v1, v0, v1, v1 };
	for (int i = 0; i < 6; i++) {
		v[i].x = xs[i];
		v[i].y = ys[i];
		v[i].z = 0.0f;
		v[i].u = us[i];
		v[i].v = vs[i];
		v[i].r = r;
		v[i].g = g;
		v[i].b = b;
		v[i].a = a;
	}
}

void wiiu_gx2_draw_rect(float p_x, float p_y, float p_w, float p_h, float p_r, float p_g, float p_b, float p_a) {
	WiiUVertex v[6];
	push_quad(v, p_x, p_y, p_x + p_w, p_y + p_h, 0, 0, 1, 1, p_r, p_g, p_b, p_a);
	wiiu_gx2_draw(WIIU_PRIM_TRIANGLES, draw_white, false, false, v, 6);
}

void wiiu_gx2_draw_text(float p_x, float p_y, const char *p_text, float p_r, float p_g, float p_b, float p_a) {
	if (draw_font == NULL || p_text == NULL) {
		return;
	}
	WiiUVertex v[6 * 128];
	uint32_t n = 0;
	const float aw = (float)draw_font->width;
	const float ah = (float)draw_font->height;
	float x = p_x;
	for (const char *c = p_text; *c && n + 6 <= 6 * 128; c++, x += WIIU_FONT_CELL_W) {
		uint32_t code = (uint8_t)*c;
		if (code == ' ') {
			continue;
		}
		if (code < wiiu_crash_font_first || code >= wiiu_crash_font_first + wiiu_crash_font_count) {
			code = '?';
		}
		const uint32_t g = code - wiiu_crash_font_first;
		const float gx = (float)((g % FONT_COLUMNS) * WIIU_FONT_CELL_W);
		const float gy = (float)((g / FONT_COLUMNS) * WIIU_FONT_CELL_H);
		push_quad(&v[n], x, p_y, x + WIIU_FONT_CELL_W, p_y + WIIU_FONT_CELL_H,
				gx / aw, gy / ah, (gx + WIIU_FONT_CELL_W) / aw, (gy + WIIU_FONT_CELL_H) / ah,
				p_r, p_g, p_b, p_a);
		n += 6;
	}
	wiiu_gx2_draw(WIIU_PRIM_TRIANGLES, draw_font, false, false, v, n);
}

bool wiiu_gx2_save_frame(void) {
	if (!wiiu_gx2_can_draw()) {
		return false;
	}
	Screen *screens[2] = { &gx2_tv, &gx2_drc };
	for (int i = 0; i < 2; i++) {
		if (saved_frame[i] == NULL) {
			GX2Surface *src = &screens[i]->color_buffer.surface;
			WiiUTexture *t = (WiiUTexture *)calloc(1, sizeof(WiiUTexture));
			if (t == NULL) {
				return false;
			}
			GX2Surface surface = *src;
			surface.use = GX2_SURFACE_USE_TEXTURE;
			surface.image = NULL;
			GX2CalcSurfaceSizeAndAlignment(&surface);
			surface.image = MEMAllocFromDefaultHeapEx(surface.imageSize, surface.alignment);
			if (surface.image == NULL) {
				free(t);
				return false;
			}
			init_texture_view(t, &surface);
			t->owns_image = true;
			saved_frame[i] = t;
		}
		GX2CopySurface(&screens[i]->color_buffer.surface, 0, 0, &saved_frame[i]->texture.surface, 0, 0);
		GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, saved_frame[i]->texture.surface.image, saved_frame[i]->texture.surface.imageSize);
	}
	// The copies must be done before the screens are drawn over.
	GX2DrawDone();
	return true;
}

void wiiu_gx2_draw_saved_frame(WiiUScreen p_screen) {
	WiiUTexture *t = saved_frame[p_screen == WIIU_SCREEN_TV ? 0 : 1];
	if (t == NULL || target_cb == NULL) {
		return;
	}
	WiiUVertex v[6];
	push_quad(v, 0, 0, (float)target_width, (float)target_height, 0, 0, 1, 1, 1, 1, 1, 1);
	wiiu_gx2_draw(WIIU_PRIM_TRIANGLES, t, false, false, v, 6);
}

void wiiu_gx2_frame_begin(void) {
	arena_index ^= 1;
	arena_used = 0;
	stat_last_draws = stat_draws;
	stat_last_vertices = stat_vertices;
	stat_draws = 0;
	stat_vertices = 0;
	if (!wiiu_gx2_can_draw()) {
		return;
	}
	if (gpu_ts == NULL) {
		gpu_ts = (uint64_t *)MEMAllocFromDefaultHeapEx(GPU_TS_SLOTS * 2 * sizeof(uint64_t), 64);
		if (gpu_ts == NULL) {
			return;
		}
		memset(gpu_ts, 0, GPU_TS_SLOTS * 2 * sizeof(uint64_t));
		DCFlushRange(gpu_ts, GPU_TS_SLOTS * 2 * sizeof(uint64_t));
	}
	gpu_ts_frame++;
	// Two frames back: the GPU has finished it by now.
	uint64_t *old = gpu_ts + ((gpu_ts_frame + GPU_TS_SLOTS - 2) % GPU_TS_SLOTS) * 2;
	DCInvalidateRange(old, 2 * sizeof(uint64_t));
	if (old[0] != 0 && old[1] > old[0]) {
		const uint64_t ticks = GX2GPUTimeToCPUTime(old[1]) - GX2GPUTimeToCPUTime(old[0]);
		stat_gpu_ms = (float)((double)ticks * 1000.0 / (double)OSTimerClockSpeed);
	}
	uint64_t *cur = gpu_ts + (gpu_ts_frame % GPU_TS_SLOTS) * 2;
	cur[0] = cur[1] = 0;
	DCFlushRange(cur, 2 * sizeof(uint64_t));
	GX2SampleTopGPUCycle(&cur[0]);
}

void wiiu_gx2_get_stats(WiiUGX2Stats *r_stats) {
	r_stats->draws = stat_last_draws;
	r_stats->vertices = stat_last_vertices;
	r_stats->textures = stat_textures;
	r_stats->texture_bytes = stat_texture_bytes;
	r_stats->render_targets = stat_render_targets;
	r_stats->render_target_bytes = stat_render_target_bytes;
	r_stats->gpu_ms = stat_gpu_ms;
	r_stats->flip_wait_ms = stat_flip_wait_ms;
}

void wiiu_gx2_frame_present(void) {
	if (!wiiu_gx2_can_draw()) {
		return;
	}
	finish_target();
	if (gpu_ts != NULL) {
		GX2SampleBottomGPUCycle(&gpu_ts[(gpu_ts_frame % GPU_TS_SLOTS) * 2 + 1]);
	}
	GX2CopyColorBufferToScanBuffer(&gx2_tv.color_buffer, GX2_SCAN_TARGET_TV);
	GX2CopyColorBufferToScanBuffer(&gx2_drc.color_buffer, GX2_SCAN_TARGET_DRC);
	GX2SwapScanBuffers();
	GX2Flush();
	// Re-enable the outputs every frame, as WHBGfx does: enabling them only on
	// the first frame left both screens black, because the system turns them
	// off again while it finishes the launch.
	GX2SetTVEnable(TRUE);
	GX2SetDRCEnable(TRUE);
	// Paces the loop to the display (60 Hz), with one frame in flight: wait
	// only while an earlier swap is still pending, not for this one. Waiting
	// for this frame's own flip made the CPU (game logic) and the GPU take
	// turns, so ~7 ms of CPU + ~11 ms of GPU missed the 16.7 ms frame (FNF ran
	// at ~35 fps with both under budget). The vertex arenas are double
	// buffered for this (arena_index flips each frame).
	const OSTime wait_start = OSGetSystemTime();
	for (;;) {
		uint32_t swap_count = 0, flip_count = 0;
		OSTime last_flip = 0, last_vsync = 0;
		GX2GetSwapStatus(&swap_count, &flip_count, &last_flip, &last_vsync);
		if (swap_count <= flip_count + 1) {
			break;
		}
		GX2WaitForFlip();
	}
	stat_flip_wait_ms = (float)((double)OSTicksToMicroseconds(OSGetSystemTime() - wait_start) / 1000.0);
	// The next frame starts with no target bound.
	target_cb = NULL;
	target_rt = NULL;
}

// ---------------------------------------------------------------------------
// Init / shutdown
// ---------------------------------------------------------------------------

// Call with the log console suspended (it shares MEM1 and the display).
bool wiiu_gx2_init(void) {
	if (gx2_active) {
		return true;
	}
	gx2_command_buffer = MEMAllocFromDefaultHeapEx(GX2_COMMAND_BUFFER_SIZE, GX2_COMMAND_BUFFER_ALIGNMENT);
	gx2_context = (GX2ContextState *)MEMAllocFromDefaultHeapEx(sizeof(GX2ContextState), GX2_CONTEXT_STATE_ALIGNMENT);
	if (gx2_command_buffer == NULL || gx2_context == NULL) {
		WHBLogPrint("[gx2] couldn't allocate the command buffer / context state");
		return false;
	}
	uint32_t attributes[] = {
		GX2_INIT_CMD_BUF_BASE, (uint32_t)(uintptr_t)gx2_command_buffer,
		GX2_INIT_CMD_BUF_POOL_SIZE, GX2_COMMAND_BUFFER_SIZE,
		GX2_INIT_END
	};
	GX2Init(attributes);

	init_color_buffer(&gx2_tv.color_buffer, TV_WIDTH, TV_HEIGHT, GX2_SURFACE_USE_TEXTURE_COLOR_BUFFER_TV);
	init_color_buffer(&gx2_drc.color_buffer, DRC_WIDTH, DRC_HEIGHT, GX2_SURFACE_USE_TEXTURE_COLOR_BUFFER_TV);
	uint32_t unused;
	GX2CalcTVSize(GX2_TV_RENDER_MODE_WIDE_720P, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8,
			GX2_BUFFERING_MODE_DOUBLE, &gx2_tv.scan_buffer_size, &unused);
	GX2CalcDRCSize(GX2_DRC_RENDER_MODE_SINGLE, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8,
			GX2_BUFFERING_MODE_DOUBLE, &gx2_drc.scan_buffer_size, &unused);

	GX2SetupContextStateEx(gx2_context, TRUE);
	GX2SetContextState(gx2_context);

	gx2_active = true;
	if (!draw_init()) {
		return false;
	}
	gx2_proc_acquired(NULL);
	ProcUIRegisterCallback(PROCUI_CALLBACK_ACQUIRE, gx2_proc_acquired, NULL, 100);
	ProcUIRegisterCallback(PROCUI_CALLBACK_RELEASE, gx2_proc_released, NULL, 100);
	// WHBLogPrintf("[gx2] initialised: TV %dx%d, GamePad %dx%d", TV_WIDTH, TV_HEIGHT, DRC_WIDTH, DRC_HEIGHT);
	return gx2_acquired;
}

// Stops GX2 and gives MEM1 and the display back (to the log console).
void wiiu_gx2_shutdown(void) {
	if (!gx2_active) {
		return;
	}
	if (gx2_acquired) {
		GX2SetTVEnable(FALSE);
		GX2SetDRCEnable(FALSE);
		gx2_proc_released(NULL);
	}
	target_cb = NULL;
	target_rt = NULL;
	draw_free();
	GX2Shutdown();
	gx2_active = false;
	if (gx2_context) {
		MEMFreeToDefaultHeap(gx2_context);
		gx2_context = NULL;
	}
	if (gx2_command_buffer) {
		MEMFreeToDefaultHeap(gx2_command_buffer);
		gx2_command_buffer = NULL;
	}
}
