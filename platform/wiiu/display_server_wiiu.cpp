#include "display_server_wiiu.h"

#include "core/math/math_funcs.h"
#include "core/os/os.h"
#include "servers/rendering/dummy/rasterizer_dummy.h"

// wiiu_gx2.c
extern "C" bool wiiu_gx2_init(void);
extern "C" void wiiu_gx2_shutdown(void);
extern "C" void wiiu_gx2_present_test(float r, float g, float b);

// wiiu_console.c
extern "C" void wiiu_console_suspend(void);
extern "C" void wiiu_console_resume(void);

Vector<String> DisplayServerWiiU::get_rendering_drivers_func() {
	Vector<String> drivers;
	drivers.push_back("gx2");
	return drivers;
}

DisplayServer *DisplayServerWiiU::create_func(const String &p_rendering_driver, DisplayServerEnums::WindowMode p_mode, DisplayServerEnums::VSyncMode p_vsync_mode, uint32_t p_flags, const Vector2i *p_position, const Vector2i &p_resolution, int p_screen, DisplayServerEnums::Context p_context, int64_t p_parent_window, Error &r_error) {
	r_error = OK;
	// Stage 1: no real renderer yet.
	RasterizerDummy::make_current();
	return memnew(DisplayServerWiiU());
}

void DisplayServerWiiU::register_wiiu_driver() {
	register_create_function("GX2", create_func, get_rendering_drivers_func);
}

void DisplayServerWiiU::present() {
	// Stage 1 test pattern: a slow pulse between coffee brown and crema, so a
	// frozen frame is easy to spot.
	const double t = OS::get_singleton()->get_ticks_usec() / 1000000.0;
	const float k = 0.5f + 0.5f * (float)Math::sin(t * Math::TAU / 4.0);
	const float r = Math::lerp(0.29f, 0.78f, k);
	const float g = Math::lerp(0.18f, 0.58f, k);
	const float b = Math::lerp(0.12f, 0.38f, k);
	wiiu_gx2_present_test(r, g, b);
}

DisplayServerWiiU::DisplayServerWiiU() {
	// GX2 takes over the display and MEM1 from the log console.
	wiiu_console_suspend();
	if (!wiiu_gx2_init()) {
		// Give the display back to the log so the error stays readable.
		wiiu_gx2_shutdown();
		wiiu_console_resume();
		ERR_PRINT("GX2 initialisation failed; showing the log instead.");
	}
}

DisplayServerWiiU::~DisplayServerWiiU() {
	wiiu_gx2_shutdown();
}
