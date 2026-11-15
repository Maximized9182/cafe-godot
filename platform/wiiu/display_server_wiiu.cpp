#include "display_server_wiiu.h"

#include "keyboard_mouse_wiiu.h"

#include "rasterizer_wiiu.h"
#include "wiiu_gx2.h"

// wiiu_console.c
extern "C" void wiiu_console_suspend(void);
extern "C" void wiiu_console_resume(void);

// The size Godot sees for the window and screen: the TV's 720p.
static const Size2i WIIU_WINDOW_SIZE(1280, 720);

Vector<String> DisplayServerWiiU::get_rendering_drivers_func() {
	Vector<String> drivers;
	drivers.push_back("gx2");
	return drivers;
}

DisplayServer *DisplayServerWiiU::create_func(const String &p_rendering_driver, DisplayServerEnums::WindowMode p_mode, DisplayServerEnums::VSyncMode p_vsync_mode, uint32_t p_flags, const Vector2i *p_position, const Vector2i &p_resolution, int p_screen, DisplayServerEnums::Context p_context, int64_t p_parent_window, Error &r_error) {
	r_error = OK;
	DisplayServerWiiU *ds = memnew(DisplayServerWiiU());
	// The GX2 2D renderer if GX2 came up, else the dummy one (log on screen).
	if (wiiu_gx2_can_draw()) {
		RasterizerWiiU::make_current();
	} else {
		RasterizerDummy::make_current();
	}
	return ds;
}

void DisplayServerWiiU::register_wiiu_driver() {
	register_create_function("Cafe OS", create_func, get_rendering_drivers_func);
}

Size2i DisplayServerWiiU::screen_get_size(int p_screen) const {
	return WIIU_WINDOW_SIZE;
}

Rect2i DisplayServerWiiU::screen_get_usable_rect(int p_screen) const {
	return Rect2i(Point2i(), WIIU_WINDOW_SIZE);
}

Vector<DisplayServerEnums::WindowID> DisplayServerWiiU::get_window_list() const {
	Vector<DisplayServerEnums::WindowID> list;
	list.push_back(DisplayServerEnums::MAIN_WINDOW_ID);
	return list;
}

Size2i DisplayServerWiiU::window_get_size(DisplayServerEnums::WindowID p_window) const {
	return WIIU_WINDOW_SIZE;
}

Size2i DisplayServerWiiU::window_get_size_with_decorations(DisplayServerEnums::WindowID p_window) const {
	return WIIU_WINDOW_SIZE;
}

void DisplayServerWiiU::mouse_set_mode(DisplayServerEnums::MouseMode p_mode) {
	if (KeyboardMouseWiiU *kbm = KeyboardMouseWiiU::get_singleton()) {
		kbm->set_mouse_mode(p_mode);
	}
}

DisplayServerEnums::MouseMode DisplayServerWiiU::mouse_get_mode() const {
	const KeyboardMouseWiiU *kbm = KeyboardMouseWiiU::get_singleton();
	return kbm ? kbm->get_mouse_mode() : DisplayServerEnums::MOUSE_MODE_VISIBLE;
}

Point2i DisplayServerWiiU::mouse_get_position() const {
	const KeyboardMouseWiiU *kbm = KeyboardMouseWiiU::get_singleton();
	return kbm ? Point2i(kbm->get_mouse_position()) : Point2i();
}

BitField<MouseButtonMask> DisplayServerWiiU::mouse_get_button_state() const {
	const KeyboardMouseWiiU *kbm = KeyboardMouseWiiU::get_singleton();
	return kbm ? kbm->get_mouse_buttons() : BitField<MouseButtonMask>(MouseButtonMask::NONE);
}

// True while GX2 owns the screens and the app is in the foreground.
bool DisplayServerWiiU::window_can_draw(DisplayServerEnums::WindowID p_window) const {
	return wiiu_gx2_can_draw();
}

bool DisplayServerWiiU::can_any_window_draw() const {
	return wiiu_gx2_can_draw();
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

void DisplayServerWiiU::send_window_event(DisplayServerEnums::WindowEvent p_event) {
	if (window_event_callback.is_valid()) {
		window_event_callback.call((int)p_event);
	}
}
