#pragma once

#include "servers/display/display_server_headless.h"

// Cafe Godot's display server: owns the TV and GamePad through GX2 and
// installs the GX2 2D renderer (rasterizer_wiiu.cpp).
//
// Godot sees one fullscreen 1280x720 window on one screen (the TV's size).
// The renderer fits the root viewport onto the TV and the GamePad separately.
// Other window/screen APIs are inherited from DisplayServerHeadless as stubs.
class DisplayServerWiiU : public DisplayServerHeadless {
	GDSOFTCLASS(DisplayServerWiiU, DisplayServerHeadless);

	static Vector<String> get_rendering_drivers_func();
	static DisplayServer *create_func(const String &p_rendering_driver, DisplayServerEnums::WindowMode p_mode, DisplayServerEnums::VSyncMode p_vsync_mode, uint32_t p_flags, const Vector2i *p_position, const Vector2i &p_resolution, int p_screen, DisplayServerEnums::Context p_context, int64_t p_parent_window, Error &r_error);

public:
	String get_name() const override { return "GX2"; }

	int get_screen_count() const override { return 1; }
	Size2i screen_get_size(int p_screen = DisplayServerEnums::SCREEN_OF_MAIN_WINDOW) const override;
	Rect2i screen_get_usable_rect(int p_screen = DisplayServerEnums::SCREEN_OF_MAIN_WINDOW) const override;
	float screen_get_refresh_rate(int p_screen = DisplayServerEnums::SCREEN_OF_MAIN_WINDOW) const override { return 60.0f; }
	Vector<DisplayServerEnums::WindowID> get_window_list() const override;
	Size2i window_get_size(DisplayServerEnums::WindowID p_window = DisplayServerEnums::MAIN_WINDOW_ID) const override;
	Size2i window_get_size_with_decorations(DisplayServerEnums::WindowID p_window = DisplayServerEnums::MAIN_WINDOW_ID) const override;
	DisplayServerEnums::WindowMode window_get_mode(DisplayServerEnums::WindowID p_window = DisplayServerEnums::MAIN_WINDOW_ID) const override { return DisplayServerEnums::WINDOW_MODE_FULLSCREEN; }

	static void register_wiiu_driver();

	DisplayServerWiiU();
	~DisplayServerWiiU();
};
