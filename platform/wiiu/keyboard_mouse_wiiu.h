#pragma once

#include "core/input/input.h"
#include "servers/display/display_server_enums.h"

// USB keyboard and mouse for Cafe Godot.
//
// - Keyboard: the system keyboard library (nsyskbd), which reports HID usage
//   codes and the typed character. Sent as InputEventKey with a US layout for
//   keycode/physical_keycode and the system's character as unicode; held keys
//   repeat (echo) like on a PC.
// - Mouse: raw HID (nsyshid). Mice are switched to the boot protocol (buttons,
//   X/Y movement, wheel) and read continuously. The cursor lives in window
//   coordinates; RasterizerWiiU draws it while the mouse mode shows it.
//
// The system calls back on its own threads; events are queued and sent to
// Godot from process() on the main thread.
class KeyboardMouseWiiU {
public:
	KeyboardMouseWiiU(Input *p_input);
	~KeyboardMouseWiiU();

	// Sends the queued keyboard and mouse events; once per frame.
	void process();

	static KeyboardMouseWiiU *get_singleton() { return singleton; }

	// Where to draw the cursor (window coordinates); false: don't draw one
	// (no mouse yet, or a hidden/captured mouse mode).
	bool get_cursor(Vector2 &r_position) const;
	Vector2 get_mouse_position() const { return mouse_position; }
	BitField<MouseButtonMask> get_mouse_buttons() const { return mouse_mask; }

	void set_mouse_mode(DisplayServerEnums::MouseMode p_mode) { mouse_mode = p_mode; }
	DisplayServerEnums::MouseMode get_mouse_mode() const { return mouse_mode; }

private:
	static inline KeyboardMouseWiiU *singleton = nullptr;
	Input *input = nullptr;

	// Keyboard.
	bool kbd_ready = false;
	uint32_t modifiers = 0; // HID modifier bits (0xE0-0xE7 -> bits 0-7).
	int repeat_code = 0; // HID code of the key that repeats (0: none).
	char32_t repeat_unicode = 0;
	uint64_t repeat_next_ms = 0;
	void send_key(int p_hid, bool p_pressed, char32_t p_unicode, bool p_echo);

	// Mouse.
	bool hid_ready = false;
	bool mouse_seen = false;
	Vector2 mouse_position;
	BitField<MouseButtonMask> mouse_mask = MouseButtonMask::NONE;
	DisplayServerEnums::MouseMode mouse_mode = DisplayServerEnums::MOUSE_MODE_VISIBLE;
};
