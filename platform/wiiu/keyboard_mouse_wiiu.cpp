#include "keyboard_mouse_wiiu.h"

#include "core/input/input_event.h"
#include "core/os/mutex.h"
#include "core/os/os.h"
#include "core/templates/local_vector.h"
#include "servers/display/display_server.h"

#include <nsyshid/hid.h>
#include <nsyskbd/nsyskbd.h>
#include <whb/log.h>

namespace {

// What the system threads hand to the main thread.
struct KeyEvent {
	uint8_t hid;
	bool pressed;
	char32_t unicode;
};

Mutex queue_mutex;
LocalVector<KeyEvent> key_queue;
int32_t mouse_dx = 0;
int32_t mouse_dy = 0;
int32_t mouse_wheel = 0;
uint8_t mouse_buttons = 0;
bool mouse_reported = false;

// ---------------------------------------------------------------------------
// Keyboard (nsyskbd callbacks, on a system thread)
// ---------------------------------------------------------------------------

void kbd_attach(KBDAttachEvent *p_event) {
	(void)p_event;
}

void kbd_detach(KBDAttachEvent *p_event) {
	(void)p_event;
}

void kbd_key(KBDKeyEvent *p_event) {
	KeyEvent e;
	e.hid = p_event->hidCode;
	e.pressed = p_event->isPressedDown != 0;
	e.unicode = p_event->asUTF16Character;
	MutexLock lock(queue_mutex);
	if (key_queue.size() < 256) {
		key_queue.push_back(e);
	}
}

// HID usage page 7 (keyboard) code to Godot's key, by US position.
Key hid_to_key(int p_hid, KeyLocation &r_location) {
	r_location = KeyLocation::UNSPECIFIED;
	if (p_hid >= 0x04 && p_hid <= 0x1D) {
		return (Key)((int)Key::A + (p_hid - 0x04));
	}
	if (p_hid >= 0x1E && p_hid <= 0x26) {
		return (Key)((int)Key::KEY_1 + (p_hid - 0x1E));
	}
	if (p_hid >= 0x3A && p_hid <= 0x45) {
		return (Key)((int)Key::F1 + (p_hid - 0x3A));
	}
	if (p_hid >= 0x59 && p_hid <= 0x61) {
		return (Key)((int)Key::KP_1 + (p_hid - 0x59));
	}
	switch (p_hid) {
		case 0x27:
			return Key::KEY_0;
		case 0x28:
			return Key::ENTER;
		case 0x29:
			return Key::ESCAPE;
		case 0x2A:
			return Key::BACKSPACE;
		case 0x2B:
			return Key::TAB;
		case 0x2C:
			return Key::SPACE;
		case 0x2D:
			return Key::MINUS;
		case 0x2E:
			return Key::EQUAL;
		case 0x2F:
			return Key::BRACKETLEFT;
		case 0x30:
			return Key::BRACKETRIGHT;
		case 0x31:
		case 0x32:
			return Key::BACKSLASH;
		case 0x33:
			return Key::SEMICOLON;
		case 0x34:
			return Key::APOSTROPHE;
		case 0x35:
			return Key::QUOTELEFT;
		case 0x36:
			return Key::COMMA;
		case 0x37:
			return Key::PERIOD;
		case 0x38:
			return Key::SLASH;
		case 0x39:
			return Key::CAPSLOCK;
		case 0x46:
			return Key::PRINT;
		case 0x47:
			return Key::SCROLLLOCK;
		case 0x48:
			return Key::PAUSE;
		case 0x49:
			return Key::INSERT;
		case 0x4A:
			return Key::HOME;
		case 0x4B:
			return Key::PAGEUP;
		case 0x4C:
			return Key::KEY_DELETE;
		case 0x4D:
			return Key::END;
		case 0x4E:
			return Key::PAGEDOWN;
		case 0x4F:
			return Key::RIGHT;
		case 0x50:
			return Key::LEFT;
		case 0x51:
			return Key::DOWN;
		case 0x52:
			return Key::UP;
		case 0x53:
			return Key::NUMLOCK;
		case 0x54:
			return Key::KP_DIVIDE;
		case 0x55:
			return Key::KP_MULTIPLY;
		case 0x56:
			return Key::KP_SUBTRACT;
		case 0x57:
			return Key::KP_ADD;
		case 0x58:
			return Key::KP_ENTER;
		case 0x62:
			return Key::KP_0;
		case 0x63:
			return Key::KP_PERIOD;
		case 0x64:
			return Key::SECTION;
		case 0x65:
			return Key::MENU;
		case 0xE0:
		case 0xE4:
			r_location = p_hid == 0xE0 ? KeyLocation::LEFT : KeyLocation::RIGHT;
			return Key::CTRL;
		case 0xE1:
		case 0xE5:
			r_location = p_hid == 0xE1 ? KeyLocation::LEFT : KeyLocation::RIGHT;
			return Key::SHIFT;
		case 0xE2:
		case 0xE6:
			r_location = p_hid == 0xE2 ? KeyLocation::LEFT : KeyLocation::RIGHT;
			return Key::ALT;
		case 0xE3:
		case 0xE7:
			r_location = p_hid == 0xE3 ? KeyLocation::LEFT : KeyLocation::RIGHT;
			return Key::META;
		default:
			return Key::NONE;
	}
}

// ---------------------------------------------------------------------------
// Mouse (nsyshid callbacks, on a system thread)
// ---------------------------------------------------------------------------

// HID mice in boot protocol: report = buttons, X, Y (signed), wheel (some).
constexpr int MAX_MICE = 4;
struct Mouse {
	bool active = false;
	uint32_t handle = 0;
	uint8_t interface_index = 0;
	uint32_t size = 0;
};
Mouse mice[MAX_MICE];
// IOS transfers need 64-byte aligned buffers.
alignas(64) uint8_t mouse_buffers[MAX_MICE][64];
HIDClient hid_client;

void mouse_read(Mouse *p_mouse);

void mouse_read_done(uint32_t p_handle, int32_t p_error, uint8_t *p_buffer, uint32_t p_bytes, void *p_context) {
	(void)p_handle;
	Mouse *m = (Mouse *)p_context;
	if (!m->active) {
		return;
	}
	if (p_error < 0) {
		// Unplugged (the detach callback follows) or a transfer error: stop.
		return;
	}
	if (p_bytes >= 3) {
		MutexLock lock(queue_mutex);
		mouse_buttons = p_buffer[0];
		mouse_dx += (int8_t)p_buffer[1];
		mouse_dy += (int8_t)p_buffer[2];
		if (p_bytes >= 4) {
			mouse_wheel += (int8_t)p_buffer[3];
		}
		mouse_reported = true;
	}
	mouse_read(m);
}

void mouse_read(Mouse *p_mouse) {
	const int index = (int)(p_mouse - mice);
	HIDRead(p_mouse->handle, mouse_buffers[index], p_mouse->size, mouse_read_done, p_mouse);
}

void mouse_protocol_set(uint32_t p_handle, int32_t p_error, uint8_t *p_buffer, uint32_t p_bytes, void *p_context) {
	(void)p_handle;
	(void)p_buffer;
	(void)p_bytes;
	Mouse *m = (Mouse *)p_context;
	if (p_error < 0) {
		WHBLogPrintf("[input] mouse: SetProtocol(boot) failed (%d), reading anyway", (int)p_error);
	}
	if (m->active) {
		mouse_read(m);
	}
}

int32_t hid_attach(HIDClient *p_client, HIDDevice *p_device, HIDAttachEvent p_attach) {
	(void)p_client;
	// Boot-interface mice only (subclass 1, protocol 2); keyboards are nsyskbd's.
	if (p_device->subClass != 1 || p_device->protocol != 2) {
		return 0;
	}
	if (p_attach == HID_DEVICE_ATTACH) {
		for (int i = 0; i < MAX_MICE; i++) {
			if (!mice[i].active) {
				Mouse &m = mice[i];
				m.handle = p_device->handle;
				m.interface_index = p_device->interfaceIndex;
				m.size = p_device->maxPacketSizeRx > 0 && p_device->maxPacketSizeRx <= 64 ? p_device->maxPacketSizeRx : 8;
				m.active = true;
				WHBLogPrintf("[input] mouse connected (%04x:%04x)", (unsigned)p_device->vid, (unsigned)p_device->pid);
				HIDSetProtocol(m.handle, m.interface_index, 0, mouse_protocol_set, &m);
				return 1;
			}
		}
		return 0;
	}
	for (int i = 0; i < MAX_MICE; i++) {
		if (mice[i].active && mice[i].handle == p_device->handle) {
			mice[i].active = false;
			WHBLogPrint("[input] mouse disconnected");
		}
	}
	return 0;
}

} // namespace

KeyboardMouseWiiU::KeyboardMouseWiiU(Input *p_input) :
		input(p_input) {
	singleton = this;
	kbd_ready = KBDSetup(kbd_attach, kbd_detach, kbd_key) == KDB_ERROR_NONE;
	if (!kbd_ready) {
		WHBLogPrint("[input] USB keyboards unavailable (KBDSetup failed)");
	}
	hid_ready = HIDSetup() >= 0;
	if (hid_ready) {
		HIDAddClient(&hid_client, hid_attach);
	} else {
		WHBLogPrint("[input] USB mice unavailable (HIDSetup failed)");
	}
	const Size2i size = DisplayServer::get_singleton() ? DisplayServer::get_singleton()->window_get_size() : Size2i(1280, 720);
	mouse_position = Vector2(size) * 0.5f;
}

KeyboardMouseWiiU::~KeyboardMouseWiiU() {
	if (hid_ready) {
		for (int i = 0; i < MAX_MICE; i++) {
			mice[i].active = false;
		}
		HIDDelClient(&hid_client);
		HIDTeardown();
	}
	if (kbd_ready) {
		KBDTeardown();
	}
	singleton = nullptr;
}

bool KeyboardMouseWiiU::get_cursor(Vector2 &r_position) const {
	if (!mouse_seen || (mouse_mode != DisplayServerEnums::MOUSE_MODE_VISIBLE && mouse_mode != DisplayServerEnums::MOUSE_MODE_CONFINED)) {
		return false;
	}
	r_position = mouse_position;
	return true;
}

void KeyboardMouseWiiU::send_key(int p_hid, bool p_pressed, char32_t p_unicode, bool p_echo) {
	KeyLocation location;
	const Key key = hid_to_key(p_hid, location);
	if (key == Key::NONE && p_unicode == 0) {
		return;
	}
	Ref<InputEventKey> ev;
	ev.instantiate();
	ev->set_pressed(p_pressed);
	ev->set_echo(p_echo);
	ev->set_keycode(key);
	ev->set_physical_keycode(key);
	ev->set_key_label(key);
	ev->set_location(location);
	// Typed characters only (no control codes), and only on press.
	if (p_pressed && p_unicode >= 32 && p_unicode != 127) {
		ev->set_unicode(p_unicode);
	}
	ev->set_ctrl_pressed(modifiers & 0x11);
	ev->set_shift_pressed(modifiers & 0x22);
	ev->set_alt_pressed(modifiers & 0x44);
	ev->set_meta_pressed(modifiers & 0x88);
	input->parse_input_event(ev);
}

void KeyboardMouseWiiU::process() {
	LocalVector<KeyEvent> keys;
	int32_t dx = 0, dy = 0, wheel = 0;
	uint8_t buttons = 0;
	bool reported = false;
	{
		MutexLock lock(queue_mutex);
		keys = key_queue;
		key_queue.clear();
		dx = mouse_dx;
		dy = mouse_dy;
		wheel = mouse_wheel;
		buttons = mouse_buttons;
		reported = mouse_reported;
		mouse_dx = mouse_dy = mouse_wheel = 0;
		mouse_reported = false;
	}

	// Keyboard.
	const uint64_t now = OS::get_singleton()->get_ticks_msec();
	for (const KeyEvent &e : keys) {
		if (e.hid >= 0xE0 && e.hid <= 0xE7) {
			const uint32_t bitv = 1u << (e.hid - 0xE0);
			modifiers = e.pressed ? (modifiers | bitv) : (modifiers & ~bitv);
		}
		send_key(e.hid, e.pressed, e.unicode, false);
		if (e.pressed && !(e.hid >= 0xE0 && e.hid <= 0xE7)) {
			repeat_code = e.hid;
			repeat_unicode = e.unicode;
			repeat_next_ms = now + 500;
		} else if (!e.pressed && e.hid == repeat_code) {
			repeat_code = 0;
		}
	}
	// Held keys repeat (echo), like a PC keyboard: after 500 ms, 30 per second.
	if (repeat_code != 0) {
		for (int n = 0; now >= repeat_next_ms && n < 4; n++) {
			send_key(repeat_code, true, repeat_unicode, true);
			repeat_next_ms += 33;
		}
		if (now >= repeat_next_ms) {
			repeat_next_ms = now + 33;
		}
	}

	// Mouse.
	if (!reported) {
		return;
	}
	mouse_seen = true;
	const Vector2 delta((float)dx, (float)dy);
	const bool captured = mouse_mode == DisplayServerEnums::MOUSE_MODE_CAPTURED;
	if (delta != Vector2()) {
		if (!captured) {
			const Size2i size = DisplayServer::get_singleton() ? DisplayServer::get_singleton()->window_get_size() : Size2i(1280, 720);
			mouse_position = (mouse_position + delta).clamp(Vector2(), Vector2(size.x - 1, size.y - 1));
		}
		Ref<InputEventMouseMotion> ev;
		ev.instantiate();
		ev->set_position(mouse_position);
		ev->set_global_position(mouse_position);
		ev->set_relative(delta);
		ev->set_relative_screen_position(delta);
		ev->set_button_mask(mouse_mask);
		input->parse_input_event(ev);
	}
	static const MouseButton BUTTONS[5] = { MouseButton::LEFT, MouseButton::RIGHT, MouseButton::MIDDLE, MouseButton::MB_XBUTTON1, MouseButton::MB_XBUTTON2 };
	static const MouseButtonMask MASKS[5] = { MouseButtonMask::LEFT, MouseButtonMask::RIGHT, MouseButtonMask::MIDDLE, MouseButtonMask::MB_XBUTTON1, MouseButtonMask::MB_XBUTTON2 };
	for (int b = 0; b < 5; b++) {
		const bool down = buttons & (1 << b);
		if (down == mouse_mask.has_flag(MASKS[b])) {
			continue;
		}
		if (down) {
			mouse_mask.set_flag(MASKS[b]);
		} else {
			mouse_mask.clear_flag(MASKS[b]);
		}
		Ref<InputEventMouseButton> ev;
		ev.instantiate();
		ev->set_button_index(BUTTONS[b]);
		ev->set_pressed(down);
		ev->set_position(mouse_position);
		ev->set_global_position(mouse_position);
		ev->set_button_mask(mouse_mask);
		input->parse_input_event(ev);
	}
	// Wheel: a press and release per notch (positive = away from the user).
	for (int n = 0; n < (wheel < 0 ? -wheel : wheel) && n < 16; n++) {
		const MouseButton index = wheel > 0 ? MouseButton::WHEEL_UP : MouseButton::WHEEL_DOWN;
		for (int pressed = 1; pressed >= 0; pressed--) {
			Ref<InputEventMouseButton> ev;
			ev.instantiate();
			ev->set_button_index(index);
			ev->set_pressed(pressed);
			ev->set_factor(1.0f);
			ev->set_position(mouse_position);
			ev->set_global_position(mouse_position);
			ev->set_button_mask(mouse_mask);
			input->parse_input_event(ev);
		}
	}
}
