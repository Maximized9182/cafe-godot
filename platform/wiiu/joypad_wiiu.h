#pragma once

#include "core/input/input.h"

#include <vpad/input.h>

#include <cstdint>

// Controller input for Cafe Godot.
//
// One place reads every controller once per frame (VPAD's `trigger` bits are
// per read call, so a second reader would steal button presses):
// - device 0: the Wii U GamePad (VPAD);
// - devices 1-4: Wii Remotes on KPAD channels 0-3, with or without a
//   Nunchuk, Classic Controller or Wii U Pro Controller (Bloopair-connected
//   third-party pads show up as Pro Controllers).
//
// Buttons map by their printed label: the Wii U "A" button is JoyButton::A,
// "B" is JoyButton::B and so on (the user's choice; note that the bottom face
// button is B on Nintendo pads, unlike Xbox-style JoyButton::A). The GUIDs are
// not in Godot's mapping database, so Input passes the raw JoyButton/JoyAxis
// values through unchanged.
class JoypadWiiU {
public:
	static constexpr int KPAD_CHANNELS = 4;
	static constexpr int GAMEPAD_DEVICE = 0;

	JoypadWiiU(Input *p_input);
	~JoypadWiiU();

	// Reads all controllers and sends Godot input events for what changed.
	void process();

	// The GamePad sample of the last process() call (for the log console).
	uint32_t get_gamepad_hold() const { return gamepad_hold; }
	uint32_t get_gamepad_trigger() const { return gamepad_trigger; }

private:
	struct Device {
		bool connected = false;
		int kind = -1; // Controller type; a change reconnects the device.
		uint64_t buttons = 0; // Bitmask of JoyButton values currently down.
		float axes[(int)JoyAxis::SDL_MAX] = {};
	};

	Input *input = nullptr;
	Device devices[1 + KPAD_CHANNELS];

	// GamePad touchscreen, sent as touch index 0 (Godot emulates the mouse).
	bool touching = false;
	Vector2 touch_position; // Window coordinates.

	uint32_t gamepad_hold = 0;
	uint32_t gamepad_trigger = 0;
	uint32_t gamepad_logged_hold = 0;

	void set_connected(int p_device, int p_kind, const String &p_name, const String &p_guid);
	void set_disconnected(int p_device);
	void update_buttons(int p_device, uint64_t p_buttons);
	void update_axis(int p_device, JoyAxis p_axis, float p_value);

	void process_gamepad();
	void process_touch(const VPADTouchData &p_touch);
	void process_kpad(int p_channel);
};
