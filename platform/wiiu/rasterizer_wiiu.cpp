#include "rasterizer_wiiu.h"

#include "core/io/image.h"
#include "core/math/math_funcs.h"
#include "core/os/os.h"
#include "scene/resources/texture.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_default.h"

// wiiu_stats.cpp: frame timing marks for the stats overlay.
extern "C" void wiiu_stats_mark(int p_mark);

// ===========================================================================
// RasterizerWiiU
// ===========================================================================

// Geometry of the last blit to the GamePad, for mapping touches back.
static struct {
	bool valid = false;
	Size2 rt_size; // Render-target pixels.
	Rect2 drc_rect; // Where the render target was drawn on the GamePad.
	Rect2 window_rect; // Where Godot thinks it is in the window (BlitToScreen::dst_rect).
} last_drc_blit;

bool rasterizer_wiiu_drc_to_window(const Vector2 &p_drc, Vector2 &r_window) {
	if (!last_drc_blit.valid || last_drc_blit.drc_rect.size.x <= 0 || last_drc_blit.drc_rect.size.y <= 0) {
		// No blit yet: scale the GamePad screen to the 1280x720 window.
		r_window = p_drc * (1280.0f / 854.0f);
		return true;
	}
	const Rect2 &drc = last_drc_blit.drc_rect;
	if (!drc.has_point(p_drc)) {
		return false;
	}
	// GamePad pixel -> render-target pixel -> window pixel.
	const Vector2 rt = (p_drc - drc.position) * last_drc_blit.rt_size / drc.size;
	const Rect2 &win = last_drc_blit.window_rect;
	if (win.size.x > 0 && win.size.y > 0) {
		r_window = win.position + rt * win.size / last_drc_blit.rt_size;
	} else {
		r_window = rt;
	}
	return true;
}

RasterizerWiiU::RasterizerWiiU() {
	// Swap the dummy pieces that need to be real. Deleting the old ones first
	// lets the new ones register as their singletons.
	memdelete(canvas);
	canvas = memnew(RasterizerCanvasWiiU);
	memdelete(texture_storage);
	texture_storage = memnew(TextureStorageWiiU);
	memdelete(utilities);
	utilities = memnew(UtilitiesWiiU);
	memdelete(mesh_storage);
	mesh_storage = memnew(MeshStorageWiiU);
	memdelete(material_storage);
	material_storage = memnew(MaterialStorageWiiU);
}

void RasterizerWiiU::begin_frame(double p_frame_step) {
	RasterizerDummy::begin_frame(p_frame_step);
	wiiu_stats_mark(1); // Rendering starts.
	wiiu_gx2_frame_begin();
	blitted = false;
}

// Fits the render targets onto one screen: the largest whole-number scale
// that fits (crisp pixels, nearest filtering), or a smooth downscale if the
// target is bigger than the screen. Ignores Godot's window rect on purpose,
// so the TV and the GamePad each get their own fit.
void RasterizerWiiU::_blit_to_screen(WiiUScreen p_screen, const RenderingServerTypes::BlitToScreen *p_render_targets, int p_amount) {
	TextureStorageWiiU *ts = TextureStorageWiiU::get_wiiu_singleton();
	RasterizerCanvasWiiU *cv = static_cast<RasterizerCanvasWiiU *>(canvas);
	uint32_t sw, sh;
	wiiu_gx2_screen_size(p_screen, &sw, &sh);
	wiiu_gx2_bind_screen(p_screen);
	wiiu_gx2_clear(0, 0, 0, 1);
	for (int i = 0; i < p_amount; i++) {
		TextureStorageWiiU::RenderTarget *rt = ts->get_render_target(p_render_targets[i].render_target);
		if (rt == nullptr || rt->gpu == nullptr) {
			continue;
		}
		const float rw = (float)rt->size.x;
		const float rh = (float)rt->size.y;
		float scale = MIN((float)sw / rw, (float)sh / rh);
		bool linear = true;
		if (scale >= 1.0f) {
			scale = Math::floor(scale);
			linear = false;
		}
		const Size2 size(Math::round(rw * scale), Math::round(rh * scale));
		const Point2 pos(Math::floor(((float)sw - size.x) * 0.5f), Math::floor(((float)sh - size.y) * 0.5f));
		cv->draw_texture_rect(wiiu_gx2_render_target_texture(rt->gpu), Rect2(pos, size), p_render_targets[i].src_rect, linear);
		if (p_screen == WIIU_SCREEN_DRC && i == 0) {
			last_drc_blit.valid = true;
			last_drc_blit.rt_size = Size2(rw, rh);
			last_drc_blit.drc_rect = Rect2(pos, size);
			last_drc_blit.window_rect = Rect2(p_render_targets[i].dst_rect);
		}
	}
}

void RasterizerWiiU::blit_render_targets_to_screen(int p_screen, const RenderingServerTypes::BlitToScreen *p_render_targets, int p_amount) {
	if (!wiiu_gx2_can_draw()) {
		return;
	}
	_blit_to_screen(WIIU_SCREEN_TV, p_render_targets, p_amount);
	_blit_to_screen(WIIU_SCREEN_DRC, p_render_targets, p_amount);
	blitted = true;
}

// crash_log.c: the log overlay toggled by ZL+L+RIGHT+X+Y+TV.
extern "C" bool wiiu_log_overlay_visible(void);
extern "C" void wiiu_draw_log_gx2(WiiUScreen p_screen, float p_r, float p_g, float p_b, float p_a, const char *p_footer);
// wiiu_stats.cpp: the stats overlay toggled by ZR+R+LEFT+Y+B+L.
extern "C" void wiiu_stats_frame(void);
extern "C" bool wiiu_stats_visible(void);
extern "C" void wiiu_stats_draw(WiiUScreen p_screen);

void RasterizerWiiU::end_frame(bool p_present) {
	if (!wiiu_gx2_can_draw()) {
		return;
	}
	wiiu_stats_mark(2); // Rendering done, presenting next.
	wiiu_stats_frame();
	if (wiiu_stats_visible() && blitted) {
		for (int s = 0; s < 2; s++) {
			const WiiUScreen screen = s == 0 ? WIIU_SCREEN_TV : WIIU_SCREEN_DRC;
			wiiu_gx2_bind_screen(screen);
			wiiu_stats_draw(screen);
		}
	}
	if (wiiu_log_overlay_visible()) {
		for (int s = 0; s < 2; s++) {
			const WiiUScreen screen = s == 0 ? WIIU_SCREEN_TV : WIIU_SCREEN_DRC;
			wiiu_gx2_bind_screen(screen);
			wiiu_draw_log_gx2(screen, 0, 0, 0, 0.8f, "Press B to close the log. Up/Down/L/R scroll.");
		}
	}
	if (!blitted) {
		// Nothing was attached to the screen this frame: show black, not garbage.
		wiiu_gx2_bind_screen(WIIU_SCREEN_TV);
		wiiu_gx2_clear(0, 0, 0, 1);
		wiiu_gx2_bind_screen(WIIU_SCREEN_DRC);
		wiiu_gx2_clear(0, 0, 0, 1);
	}
	if (p_present) {
		wiiu_gx2_frame_present();
	}
}
