#include "joypad_wiiu.h"

#include "core/input/input_event.h"
#include "rasterizer_wiiu.h"

#include <padscore/kpad.h>
#include <padscore/wpad.h>
#include <vpad/input.h>
#include <whb/log.h>

// Log the GamePad's raw button bits whenever they change. Used to check which
// buttons reach the app at all (the TV button may be taken by the system).
static constexpr bool LOG_GAMEPAD_BITS = true;

namespace {

enum Kind {
	KIND_GAMEPAD,
	KIND_WIIMOTE,
	KIND_NUNCHUK,
	KIND_CLASSIC,
	KIND_PRO,
};

struct ButtonMap {
	uint32_t mask;
	JoyButton button;
};

constexpr uint64_t bit(JoyButton p_button) {
	return 1ull << (int)p_button;
}

uint64_t map_buttons(uint32_t p_hold, const ButtonMap *p_map, int p_count) {
	uint64_t buttons = 0;
	for (int i = 0; i < p_count; i++) {
		if (p_hold & p_map[i].mask) {
			buttons |= bit(p_map[i].button);
		}
	}
	return buttons;
}

// By label: Wii U A = JoyButton::A, B = B, X = X, Y = Y.
const ButtonMap GAMEPAD_BUTTONS[] = {
	{ VPAD_BUTTON_A, JoyButton::A },
	{ VPAD_BUTTON_B, JoyButton::B },
	{ VPAD_BUTTON_X, JoyButton::X },
	{ VPAD_BUTTON_Y, JoyButton::Y },
	{ VPAD_BUTTON_MINUS, JoyButton::BACK },
	{ VPAD_BUTTON_PLUS, JoyButton::START },
	{ VPAD_BUTTON_STICK_L, JoyButton::LEFT_STICK },
	{ VPAD_BUTTON_STICK_R, JoyButton::RIGHT_STICK },
	{ VPAD_BUTTON_L, JoyButton::LEFT_SHOULDER },
	{ VPAD_BUTTON_R, JoyButton::RIGHT_SHOULDER },
	{ VPAD_BUTTON_UP, JoyButton::DPAD_UP },
	{ VPAD_BUTTON_DOWN, JoyButton::DPAD_DOWN },
	{ VPAD_BUTTON_LEFT, JoyButton::DPAD_LEFT },
	{ VPAD_BUTTON_RIGHT, JoyButton::DPAD_RIGHT },
	{ VPAD_BUTTON_TV, JoyButton::MISC1 },
};

const ButtonMap PRO_BUTTONS[] = {
	{ WPAD_PRO_BUTTON_A, JoyButton::A },
	{ WPAD_PRO_BUTTON_B, JoyButton::B },
	{ WPAD_PRO_BUTTON_X, JoyButton::X },
	{ WPAD_PRO_BUTTON_Y, JoyButton::Y },
	{ WPAD_PRO_BUTTON_MINUS, JoyButton::BACK },
	{ WPAD_PRO_BUTTON_PLUS, JoyButton::START },
	{ WPAD_PRO_BUTTON_STICK_L, JoyButton::LEFT_STICK },
	{ WPAD_PRO_BUTTON_STICK_R, JoyButton::RIGHT_STICK },
	{ WPAD_PRO_BUTTON_L, JoyButton::LEFT_SHOULDER },
	{ WPAD_PRO_BUTTON_R, JoyButton::RIGHT_SHOULDER },
	{ WPAD_PRO_BUTTON_UP, JoyButton::DPAD_UP },
	{ WPAD_PRO_BUTTON_DOWN, JoyButton::DPAD_DOWN },
	{ WPAD_PRO_BUTTON_LEFT, JoyButton::DPAD_LEFT },
	{ WPAD_PRO_BUTTON_RIGHT, JoyButton::DPAD_RIGHT },
};

const ButtonMap CLASSIC_BUTTONS[] = {
	{ WPAD_CLASSIC_BUTTON_A, JoyButton::A },
	{ WPAD_CLASSIC_BUTTON_B, JoyButton::B },
	{ WPAD_CLASSIC_BUTTON_X, JoyButton::X },
	{ WPAD_CLASSIC_BUTTON_Y, JoyButton::Y },
	{ WPAD_CLASSIC_BUTTON_MINUS, JoyButton::BACK },
	{ WPAD_CLASSIC_BUTTON_PLUS, JoyButton::START },
	{ WPAD_CLASSIC_BUTTON_L, JoyButton::LEFT_SHOULDER },
	{ WPAD_CLASSIC_BUTTON_R, JoyButton::RIGHT_SHOULDER },
	{ WPAD_CLASSIC_BUTTON_UP, JoyButton::DPAD_UP },
	{ WPAD_CLASSIC_BUTTON_DOWN, JoyButton::DPAD_DOWN },
	{ WPAD_CLASSIC_BUTTON_LEFT, JoyButton::DPAD_LEFT },
	{ WPAD_CLASSIC_BUTTON_RIGHT, JoyButton::DPAD_RIGHT },
};

// A Wii Remote held upright: A, B, 1 and 2 map to A, B, X and Y.
const ButtonMap WIIMOTE_BUTTONS[] = {
	{ WPAD_BUTTON_A, JoyButton::A },
	{ WPAD_BUTTON_B, JoyButton::B },
	{ WPAD_BUTTON_1, JoyButton::X },
	{ WPAD_BUTTON_2, JoyButton::Y },
	{ WPAD_BUTTON_MINUS, JoyButton::BACK },
	{ WPAD_BUTTON_PLUS, JoyButton::START },
	{ WPAD_BUTTON_UP, JoyButton::DPAD_UP },
	{ WPAD_BUTTON_DOWN, JoyButton::DPAD_DOWN },
	{ WPAD_BUTTON_LEFT, JoyButton::DPAD_LEFT },
	{ WPAD_BUTTON_RIGHT, JoyButton::DPAD_RIGHT },
};

const ButtonMap NUNCHUK_BUTTONS[] = {
	{ WPAD_BUTTON_C, JoyButton::LEFT_SHOULDER },
};

#define COUNT(a) (int)(sizeof(a) / sizeof((a)[0]))

const char *kind_name(int p_kind) {
	switch (p_kind) {
		case KIND_GAMEPAD:
			return "Wii U GamePad";
		case KIND_WIIMOTE:
			return "Wii Remote";
		case KIND_NUNCHUK:
			return "Wii Remote + Nunchuk";
		case KIND_CLASSIC:
			return "Wii Remote + Classic Controller";
		case KIND_PRO:
			return "Wii U Pro Controller";
		default:
			return "Wii controller";
	}
}

// Not in Godot's SDL mapping database, so events pass through unmapped.
const char *kind_guid(int p_kind) {
	switch (p_kind) {
		case KIND_GAMEPAD:
			return "cafe0000000000000000000000000001";
		case KIND_WIIMOTE:
			return "cafe0000000000000000000000000002";
		case KIND_NUNCHUK:
			return "cafe0000000000000000000000000003";
		case KIND_CLASSIC:
			return "cafe0000000000000000000000000004";
		case KIND_PRO:
			return "cafe0000000000000000000000000005";
		default:
			return "cafe0000000000000000000000000000";
	}
}

// VPAD/KPAD sticks are -1..1 with +Y up; Godot's are +Y down.
float stick_y(float p_y) {
	return -p_y;
}

} // namespace

JoypadWiiU::JoypadWiiU(Input *p_input) :
		input(p_input) {
	KPADInit();
	// Lets KPAD report Wii U Pro Controllers (and Bloopair's emulated ones).
	WPADEnableURCC(TRUE);
}

JoypadWiiU::~JoypadWiiU() {
	KPADShutdown();
}

void JoypadWiiU::set_connected(int p_device, int p_kind, const String &p_name, const String &p_guid) {
	Device &d = devices[p_device];
	if (d.connected && d.kind == p_kind) {
		return;
	}
	if (d.connected) {
		set_disconnected(p_device);
	}
	d.connected = true;
	d.kind = p_kind;
	input->joy_connection_changed(p_device, true, p_name, p_guid);
	// WHBLogPrintf("Joypad %d connected: %s", p_device, p_name.utf8().get_data());
}

void JoypadWiiU::set_disconnected(int p_device) {
	Device &d = devices[p_device];
	if (!d.connected) {
		return;
	}
	// Release everything that's still held before the device goes away.
	update_buttons(p_device, 0);
	for (int a = 0; a < (int)JoyAxis::SDL_MAX; a++) {
		update_axis(p_device, (JoyAxis)a, 0.0f);
	}
	d.connected = false;
	d.kind = -1;
	input->joy_connection_changed(p_device, false, "");
	// WHBLogPrintf("Joypad %d disconnected", p_device);
}

void JoypadWiiU::update_buttons(int p_device, uint64_t p_buttons) {
	Device &d = devices[p_device];
	uint64_t changed = d.buttons ^ p_buttons;
	if (changed == 0) {
		return;
	}
	for (int b = 0; b < (int)JoyButton::SDL_MAX; b++) {
		if (changed & (1ull << b)) {
			input->joy_button(p_device, (JoyButton)b, (p_buttons >> b) & 1);
		}
	}
	d.buttons = p_buttons;
}

void JoypadWiiU::update_axis(int p_device, JoyAxis p_axis, float p_value) {
	Device &d = devices[p_device];
	if (d.axes[(int)p_axis] == p_value) {
		return;
	}
	d.axes[(int)p_axis] = p_value;
	input->joy_axis(p_device, p_axis, p_value);
}

void JoypadWiiU::process_gamepad() {
	VPADStatus status;
	VPADReadError error = VPAD_READ_SUCCESS;
	int32_t read = VPADRead(VPAD_CHAN_0, &status, 1, &error);
	gamepad_trigger = 0;
	if (read < 1 || error != VPAD_READ_SUCCESS) {
		// VPAD_READ_NO_SAMPLES just means no new sample this frame.
		if (error == VPAD_READ_INVALID_CONTROLLER) {
			set_disconnected(GAMEPAD_DEVICE);
		}
		return;
	}
	set_connected(GAMEPAD_DEVICE, KIND_GAMEPAD, kind_name(KIND_GAMEPAD), kind_guid(KIND_GAMEPAD));
	gamepad_hold = status.hold;
	gamepad_trigger = status.trigger;

	// Debug: log the GamePad's raw button bits whenever they change.
	// if (LOG_GAMEPAD_BITS && status.hold != gamepad_logged_hold) {
	// 	WHBLogPrintf("[input] GamePad hold %08x", (unsigned)status.hold);
	// 	gamepad_logged_hold = status.hold;
	// }

	process_touch(status.tpNormal);

	uint64_t buttons = map_buttons(status.hold, GAMEPAD_BUTTONS, COUNT(GAMEPAD_BUTTONS));
	update_buttons(GAMEPAD_DEVICE, buttons);
	update_axis(GAMEPAD_DEVICE, JoyAxis::LEFT_X, status.leftStick.x);
	update_axis(GAMEPAD_DEVICE, JoyAxis::LEFT_Y, stick_y(status.leftStick.y));
	update_axis(GAMEPAD_DEVICE, JoyAxis::RIGHT_X, status.rightStick.x);
	update_axis(GAMEPAD_DEVICE, JoyAxis::RIGHT_Y, stick_y(status.rightStick.y));
	// ZL/ZR are digital on the GamePad.
	update_axis(GAMEPAD_DEVICE, JoyAxis::TRIGGER_LEFT, (status.hold & VPAD_BUTTON_ZL) ? 1.0f : 0.0f);
	update_axis(GAMEPAD_DEVICE, JoyAxis::TRIGGER_RIGHT, (status.hold & VPAD_BUTTON_ZR) ? 1.0f : 0.0f);
}

void JoypadWiiU::process_touch(const VPADTouchData &p_touch) {
	bool down = p_touch.touched != 0;
	Vector2 position = touch_position;
	if (down) {
		VPADTouchData calibrated;
		// Calibrated to GamePad screen pixels (854x480).
		VPADGetTPCalibratedPointEx(VPAD_CHAN_0, VPAD_TP_854X480, &calibrated, &p_touch);
		if (p_touch.validity != VPAD_VALID) {
			// One axis is unreliable this sample: keep the last position.
			if (!touching) {
				return;
			}
		} else if (!rasterizer_wiiu_drc_to_window(Vector2(calibrated.x, calibrated.y), position)) {
			// In the black bars around the picture: treat as no touch.
			down = false;
		}
	}

	if (down && !touching) {
		Ref<InputEventScreenTouch> ev;
		ev.instantiate();
		ev->set_index(0);
		ev->set_position(position);
		ev->set_pressed(true);
		input->parse_input_event(ev);
	} else if (down && touching && position != touch_position) {
		Ref<InputEventScreenDrag> ev;
		ev.instantiate();
		ev->set_index(0);
		ev->set_position(position);
		ev->set_relative(position - touch_position);
		ev->set_relative_screen_position(position - touch_position);
		input->parse_input_event(ev);
	} else if (!down && touching) {
		Ref<InputEventScreenTouch> ev;
		ev.instantiate();
		ev->set_index(0);
		ev->set_position(touch_position);
		ev->set_pressed(false);
		input->parse_input_event(ev);
	}
	touching = down;
	if (down) {
		touch_position = position;
	}
}

void JoypadWiiU::process_kpad(int p_channel) {
	const int device = 1 + p_channel;
	WPADExtensionType ext_type;
	if (WPADProbe((WPADChan)p_channel, &ext_type) != 0) {
		set_disconnected(device);
		return;
	}

	KPADStatus status;
	KPADError error = KPAD_ERROR_OK;
	uint32_t read = KPADReadEx((KPADChan)p_channel, &status, 1, &error);
	if (read < 1 || error != KPAD_ERROR_OK) {
		// No new sample this frame; keep the last state.
		return;
	}

	int kind;
	switch (status.extensionType) {
		case WPAD_EXT_PRO_CONTROLLER:
			kind = KIND_PRO;
			break;
		case WPAD_EXT_CLASSIC:
		case WPAD_EXT_MPLUS_CLASSIC:
			kind = KIND_CLASSIC;
			break;
		case WPAD_EXT_NUNCHUK:
		case WPAD_EXT_MPLUS_NUNCHUK:
			kind = KIND_NUNCHUK;
			break;
		default:
			kind = KIND_WIIMOTE;
			break;
	}
	set_connected(device, kind, kind_name(kind), kind_guid(kind));

	uint64_t buttons = 0;
	float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
	switch (kind) {
		case KIND_PRO:
			buttons = map_buttons(status.pro.hold, PRO_BUTTONS, COUNT(PRO_BUTTONS));
			lx = status.pro.leftStick.x;
			ly = stick_y(status.pro.leftStick.y);
			rx = status.pro.rightStick.x;
			ry = stick_y(status.pro.rightStick.y);
			lt = (status.pro.hold & WPAD_PRO_TRIGGER_ZL) ? 1.0f : 0.0f;
			rt = (status.pro.hold & WPAD_PRO_TRIGGER_ZR) ? 1.0f : 0.0f;
			break;
		case KIND_CLASSIC:
			buttons = map_buttons(status.classic.hold, CLASSIC_BUTTONS, COUNT(CLASSIC_BUTTONS));
			lx = status.classic.leftStick.x;
			ly = stick_y(status.classic.leftStick.y);
			rx = status.classic.rightStick.x;
			ry = stick_y(status.classic.rightStick.y);
			// Original Classic Controllers have analog L/R; the Pro version is digital.
			lt = (status.classic.hold & WPAD_CLASSIC_BUTTON_ZL) ? 1.0f : status.classic.leftTrigger;
			rt = (status.classic.hold & WPAD_CLASSIC_BUTTON_ZR) ? 1.0f : status.classic.rightTrigger;
			break;
		case KIND_NUNCHUK:
			buttons = map_buttons(status.hold, WIIMOTE_BUTTONS, COUNT(WIIMOTE_BUTTONS)) |
					map_buttons(status.nunchuk.hold, NUNCHUK_BUTTONS, COUNT(NUNCHUK_BUTTONS));
			lx = status.nunchuk.stick.x;
			ly = stick_y(status.nunchuk.stick.y);
			lt = (status.nunchuk.hold & WPAD_BUTTON_Z) ? 1.0f : 0.0f;
			break;
		default:
			buttons = map_buttons(status.hold, WIIMOTE_BUTTONS, COUNT(WIIMOTE_BUTTONS));
			break;
	}
	update_buttons(device, buttons);
	update_axis(device, JoyAxis::LEFT_X, lx);
	update_axis(device, JoyAxis::LEFT_Y, ly);
	update_axis(device, JoyAxis::RIGHT_X, rx);
	update_axis(device, JoyAxis::RIGHT_Y, ry);
	update_axis(device, JoyAxis::TRIGGER_LEFT, lt);
	update_axis(device, JoyAxis::TRIGGER_RIGHT, rt);
}

void JoypadWiiU::process() {
	process_gamepad();
	for (int c = 0; c < KPAD_CHANNELS; c++) {
		process_kpad(c);
	}
}
