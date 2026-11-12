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
	if (gx2_first_frame) {
		// Turn the outputs on only once there's a frame to show.
		GX2SetTVEnable(TRUE);
		GX2SetDRCEnable(TRUE);
		gx2_first_frame = false;
	}
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
