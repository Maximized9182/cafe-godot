#pragma once

#include "servers/display/display_server_headless.h"

// Cafe Godot's display server: owns the TV and GamePad through GX2.
//
// Stage 1 of the renderer: rendering still goes through Godot's dummy
// renderer (as headless does); each frame just clears both screens and
// presents with vsync. Window/screen APIs are inherited from
// DisplayServerHeadless as stubs for now.
class DisplayServerWiiU : public DisplayServerHeadless {
	GDSOFTCLASS(DisplayServerWiiU, DisplayServerHeadless);

	static Vector<String> get_rendering_drivers_func();
	static DisplayServer *create_func(const String &p_rendering_driver, DisplayServerEnums::WindowMode p_mode, DisplayServerEnums::VSyncMode p_vsync_mode, uint32_t p_flags, const Vector2i *p_position, const Vector2i &p_resolution, int p_screen, DisplayServerEnums::Context p_context, int64_t p_parent_window, Error &r_error);

public:
	String get_name() const override { return "GX2"; }

	static void register_wiiu_driver();

	// Shows the frame on the TV and GamePad (called once per frame by OS_WiiU).
	void present();

	DisplayServerWiiU();
	~DisplayServerWiiU();
};
