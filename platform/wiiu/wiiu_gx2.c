// GX2 display setup for Cafe Godot: owns the TV and GamePad (DRC) outputs.
//
// Modelled on Love Potion's driver/display (MIT, (c) 2018-present Serena S.
// Postelnek & Logan Hickok-Dickson) and wut's WHBGfx.
//
// Memory: the scan buffers and colour buffers live in the MEM1 frame heap and
// are only valid while the app has the foreground. They're allocated in the
// ProcUI "acquire" callback and freed in "release" (HOME menu), so they don't
// leak into the HOME menu's memory. OSScreen (the log console in
// wiiu_console.c) also uses the MEM1 frame heap and the display controller, so
// only one of the two may own the display at a time: the console is suspended
// before wiiu_gx2_init and resumed after wiiu_gx2_shutdown (e.g. by the crash
// screen).

#include <coreinit/cache.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/memfrmheap.h>
#include <coreinit/memheap.h>
#include <gx2/clear.h>
#include <gx2/draw.h>
#include <gx2/sampler.h>
#include <gx2/shaders.h>
#include <gx2/texture.h>
#include <gx2/utils.h>
#include <gx2r/buffer.h>
#include <whb/gfx.h>
#include <gx2/context.h>
#include <gx2/display.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/state.h>
#include <gx2/surface.h>
#include <gx2/swap.h>
#include <proc_ui/procui.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <whb/log.h>

#define GX2_FRAME_HEAP_TAG 0x47583242 // "GX2B"

#define TV_WIDTH 1280
#define TV_HEIGHT 720
#define DRC_WIDTH 854
#define DRC_HEIGHT 480

#define SCAN_BUFFER_ALIGNMENT 0x1000

typedef struct {
	GX2ColorBuffer color_buffer;
	void *scan_buffer;
	uint32_t scan_buffer_size;
} Screen;

static bool gx2_active = false;
static bool gx2_acquired = false;
static bool gx2_first_frame = true;
static void *gx2_command_buffer = NULL;
static GX2ContextState *gx2_context = NULL;
static Screen gx2_tv;
static Screen gx2_drc;

static void init_color_buffer(GX2ColorBuffer *cb, uint32_t width, uint32_t height) {
	memset(cb, 0, sizeof(*cb));
	cb->surface.use = GX2_SURFACE_USE_TEXTURE_COLOR_BUFFER_TV;
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
	gx2_first_frame = true;
	return 0;
}

static uint32_t gx2_proc_released(void *context) {
	(void)context;
	if (!gx2_acquired) {
		return 0;
	}
	// Let the GPU finish with the buffers before they go back to the system.
	GX2DrawDone();
	gx2_acquired = false;
	MEMFreeByStateToFrmHeap(MEMGetBaseHeapHandle(MEM_BASE_HEAP_MEM1), GX2_FRAME_HEAP_TAG);
	return 0;
}

// ---------------------------------------------------------------------------
// Stage 2 test: one textured quad.
//
// texture.gsh (from Love Potion, see shaders/README.md): vertex inputs
// VertexPosition vec3, VertexTexCoord vec2, VertexColor vec4; uniform block
// Transformation { mat4 mdlvMtx; mat4 projMtx; }; pixel = colour * texture0.
// ---------------------------------------------------------------------------

extern const uint8_t wiiu_shader_texture[];

typedef struct {
	float x, y, z;
	float u, v;
	float r, g, b, a;
} Vertex;

#define VERTEX_STRIDE ((uint32_t)sizeof(Vertex))

static bool quad_ready = false;
static WHBGfxShaderGroup quad_shader;
static uint32_t quad_uniform_location = 0;
static uint32_t quad_sampler_location = 0;
static Vertex *quad_vertices = NULL;
static uint32_t *quad_uniforms = NULL; // 32 words, byte-swapped
static GX2Texture quad_texture;
static GX2Sampler quad_sampler;

static uint32_t swap32(uint32_t v) {
	return __builtin_bswap32(v);
}

static bool quad_init(void) {
	if (!WHBGfxLoadGFDShaderGroup(&quad_shader, 0, wiiu_shader_texture)) {
		WHBLogPrint("[gx2] couldn't load texture.gsh");
		return false;
	}
	WHBGfxInitShaderAttribute(&quad_shader, "VertexPosition", 0, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32_32);
	WHBGfxInitShaderAttribute(&quad_shader, "VertexTexCoord", 0, 12, GX2_ATTRIB_FORMAT_FLOAT_32_32);
	WHBGfxInitShaderAttribute(&quad_shader, "VertexColor", 0, 20, GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32);
	WHBGfxInitFetchShader(&quad_shader);

	GX2UniformBlock *block = GX2GetVertexUniformBlock(quad_shader.vertexShader, "Transformation");
	if (block == NULL) {
		WHBLogPrint("[gx2] texture.gsh has no Transformation block");
		return false;
	}
	quad_uniform_location = block->offset;
	quad_sampler_location = 0;
	for (uint32_t i = 0; i < quad_shader.pixelShader->samplerVarCount; i++) {
		if (strcmp(quad_shader.pixelShader->samplerVars[i].name, "texture0") == 0) {
			quad_sampler_location = quad_shader.pixelShader->samplerVars[i].location;
		}
	}

	// Two identity matrices. GX2 reads uniform blocks little-endian, so every
	// 32-bit word is byte-swapped (as Love Potion does).
	quad_uniforms = (uint32_t *)MEMAllocFromDefaultHeapEx(GX2_UNIFORM_BLOCK_ALIGNMENT, GX2_UNIFORM_BLOCK_ALIGNMENT);
	for (int m = 0; m < 2; m++) {
		for (int i = 0; i < 16; i++) {
			float v = (i % 5 == 0) ? 1.0f : 0.0f;
			uint32_t bits;
			memcpy(&bits, &v, 4);
			quad_uniforms[m * 16 + i] = swap32(bits);
		}
	}
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU | GX2_INVALIDATE_MODE_UNIFORM_BLOCK, quad_uniforms, GX2_UNIFORM_BLOCK_ALIGNMENT);

	// A quad in the middle of the screen (clip space, +Y up), vertex colour white.
	// Texture: red top-left, green top-right, blue bottom-left, white bottom-right.
	static const Vertex corners[4] = {
		{ -0.5f, 0.5f, 0.0f, 0.0f, 0.0f, 1, 1, 1, 1 },
		{ 0.5f, 0.5f, 0.0f, 1.0f, 0.0f, 1, 1, 1, 1 },
		{ 0.5f, -0.5f, 0.0f, 1.0f, 1.0f, 1, 1, 1, 1 },
		{ -0.5f, -0.5f, 0.0f, 0.0f, 1.0f, 1, 1, 1, 1 },
	};
	quad_vertices = (Vertex *)MEMAllocFromDefaultHeapEx(sizeof(corners), GX2_VERTEX_BUFFER_ALIGNMENT);
	memcpy(quad_vertices, corners, sizeof(corners));
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU | GX2_INVALIDATE_MODE_ATTRIBUTE_BUFFER, quad_vertices, sizeof(corners));

	memset(&quad_texture, 0, sizeof(quad_texture));
	quad_texture.surface.width = 2;
	quad_texture.surface.height = 2;
	quad_texture.surface.depth = 1;
	quad_texture.surface.mipLevels = 1;
	quad_texture.surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
	quad_texture.surface.aa = GX2_AA_MODE1X;
	quad_texture.surface.use = GX2_SURFACE_USE_TEXTURE;
	quad_texture.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
	quad_texture.surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
	quad_texture.viewNumMips = 1;
	quad_texture.viewNumSlices = 1;
	quad_texture.compMap = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A);
	GX2CalcSurfaceSizeAndAlignment(&quad_texture.surface);
	GX2InitTextureRegs(&quad_texture);
	quad_texture.surface.image = MEMAllocFromDefaultHeapEx(quad_texture.surface.imageSize, quad_texture.surface.alignment);
	memset(quad_texture.surface.image, 0, quad_texture.surface.imageSize);
	static const uint8_t pixels[2][2][4] = {
		{ { 255, 0, 0, 255 }, { 0, 255, 0, 255 } },
		{ { 0, 0, 255, 255 }, { 255, 255, 255, 255 } },
	};
	uint8_t *image = (uint8_t *)quad_texture.surface.image;
	for (int y = 0; y < 2; y++) {
		memcpy(image + y * quad_texture.surface.pitch * 4, pixels[y], sizeof(pixels[y]));
	}
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU | GX2_INVALIDATE_MODE_TEXTURE, quad_texture.surface.image, quad_texture.surface.imageSize);

	GX2InitSampler(&quad_sampler, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_POINT);

	WHBLogPrintf("[gx2] test quad ready: uniform block at %u, sampler %u, texture pitch %u",
			(unsigned)quad_uniform_location, (unsigned)quad_sampler_location, (unsigned)quad_texture.surface.pitch);
	quad_ready = true;
	return true;
}

static void quad_free(void) {
	if (!quad_ready) {
		return;
	}
	quad_ready = false;
	WHBGfxFreeShaderGroup(&quad_shader);
	MEMFreeToDefaultHeap(quad_texture.surface.image);
	MEMFreeToDefaultHeap(quad_vertices);
	MEMFreeToDefaultHeap(quad_uniforms);
}

static void quad_draw(void) {
	GX2SetShaderMode(GX2_SHADER_MODE_UNIFORM_BLOCK);
	GX2SetFetchShader(&quad_shader.fetchShader);
	GX2SetVertexShader(quad_shader.vertexShader);
	GX2SetPixelShader(quad_shader.pixelShader);
	GX2SetVertexUniformBlock(quad_uniform_location, 32 * 4, quad_uniforms);
	GX2SetAttribBuffer(0, 4 * VERTEX_STRIDE, VERTEX_STRIDE, quad_vertices);
	GX2SetPixelTexture(&quad_texture, quad_sampler_location);
	GX2SetPixelSampler(&quad_sampler, quad_sampler_location);
	GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS);
	GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, FALSE);
	GX2SetColorControl(GX2_LOGIC_OP_COPY, 0xFF, FALSE, TRUE);
	GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_INV_SRC_ALPHA,
			GX2_BLEND_COMBINE_MODE_ADD, TRUE, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_INV_SRC_ALPHA,
			GX2_BLEND_COMBINE_MODE_ADD);
	GX2DrawEx(GX2_PRIMITIVE_MODE_QUADS, 4, 0, 1);
}

// Clears one screen's colour buffer, draws the test quad into it and copies it
// to the scan buffer.
static void render_screen(Screen *s, uint32_t width, uint32_t height, GX2ScanTarget target,
		float r, float g, float b) {
	GX2ClearColor(&s->color_buffer, r, g, b, 1.0f);
	// GX2ClearColor changes GPU state; restore ours, then bind this screen.
	GX2SetContextState(gx2_context);
	GX2SetColorBuffer(&s->color_buffer, GX2_RENDER_TARGET_0);
	GX2SetViewport(0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f);
	GX2SetScissor(0, 0, width, height);
	if (quad_ready) {
		quad_draw();
	}
	GX2CopyColorBufferToScanBuffer(&s->color_buffer, target);
}

// Stage 2: pulsing background plus the test quad on both screens, vsync.
void wiiu_gx2_present_test(float r, float g, float b) {
	if (!gx2_active || !gx2_acquired) {
		return;
	}
	render_screen(&gx2_tv, TV_WIDTH, TV_HEIGHT, GX2_SCAN_TARGET_TV, r, g, b);
	render_screen(&gx2_drc, DRC_WIDTH, DRC_HEIGHT, GX2_SCAN_TARGET_DRC, r, g, b);
	GX2SwapScanBuffers();
	GX2Flush();
	// Re-enable the outputs every frame, as WHBGfx does (see present_clear).
	GX2SetTVEnable(TRUE);
	GX2SetDRCEnable(TRUE);
	GX2WaitForFlip();
}

bool wiiu_gx2_is_active(void) {
	return gx2_active;
}

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

	init_color_buffer(&gx2_tv.color_buffer, TV_WIDTH, TV_HEIGHT);
	init_color_buffer(&gx2_drc.color_buffer, DRC_WIDTH, DRC_HEIGHT);
	uint32_t unused;
	GX2CalcTVSize(GX2_TV_RENDER_MODE_WIDE_720P, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8,
			GX2_BUFFERING_MODE_DOUBLE, &gx2_tv.scan_buffer_size, &unused);
	GX2CalcDRCSize(GX2_DRC_RENDER_MODE_SINGLE, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8,
			GX2_BUFFERING_MODE_DOUBLE, &gx2_drc.scan_buffer_size, &unused);

	GX2SetupContextStateEx(gx2_context, TRUE);
	GX2SetContextState(gx2_context);

	gx2_active = true;
	quad_init();
	gx2_proc_acquired(NULL);
	ProcUIRegisterCallback(PROCUI_CALLBACK_ACQUIRE, gx2_proc_acquired, NULL, 100);
	ProcUIRegisterCallback(PROCUI_CALLBACK_RELEASE, gx2_proc_released, NULL, 100);
	WHBLogPrintf("[gx2] initialised: TV %dx%d, GamePad %dx%d", TV_WIDTH, TV_HEIGHT, DRC_WIDTH, DRC_HEIGHT);
	return gx2_acquired;
}

// Stage 1: clear both screens to one colour and present with vsync.
void wiiu_gx2_present_clear(float r, float g, float b) {
	if (!gx2_active || !gx2_acquired) {
		return;
	}
	GX2ClearColor(&gx2_tv.color_buffer, r, g, b, 1.0f);
	GX2ClearColor(&gx2_drc.color_buffer, r, g, b, 1.0f);
	// GX2ClearColor changes GPU state; restore ours.
	GX2SetContextState(gx2_context);
	GX2CopyColorBufferToScanBuffer(&gx2_tv.color_buffer, GX2_SCAN_TARGET_TV);
	GX2CopyColorBufferToScanBuffer(&gx2_drc.color_buffer, GX2_SCAN_TARGET_DRC);
	GX2SwapScanBuffers();
	GX2Flush();
	// Re-enable the outputs every frame, as WHBGfx does: enabling them only on
	// the first frame left both screens black, because the system turns them
	// off again while it finishes the launch (they came back only after an
	// overlay such as Aroma's config menu restored the display state).
	GX2SetTVEnable(TRUE);
	GX2SetDRCEnable(TRUE);
	gx2_first_frame = false;
	// Paces the loop to the display (60 Hz).
	GX2WaitForFlip();
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
	quad_free();
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
