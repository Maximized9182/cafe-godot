#pragma once

#include "core/math/vector2i.h"
#include "core/object/class_db.h"
#include "core/object/object.h"
#include "core/templates/rid.h"

class Viewport;
class InputEvent;
template <typename T>
class Ref;

// Cafe Godot: the Wii U's two screens, also on the desktop. Games not made for
// it get the main window on the TV, mirrored onto the GamePad. A game that
// asks for it can instead put its own Viewport on the GamePad:
//
//   if Engine.has_singleton("Cafe"):
//       Engine.get_singleton("Cafe").set_gamepad_viewport($GamePadView)
//
// The viewport is a SubViewport, usually 854x480 (the GamePad's size); it is
// shown fitted on the GamePad, and GamePad touches are delivered to it (touch
// and left-mouse events, in the viewport's own pixels) instead of to the
// main window. Passing null goes back to mirroring.
//
// On the desktop (editor, playtests) the GamePad is a second window
// ("Cafe GamePad") showing the viewport; clicks in it are the touches.
class Cafe : public Object {
	GDCLASS(Cafe, Object);

	static inline Cafe *singleton = nullptr;
	ObjectID gamepad_viewport;
#ifndef WIIU_ENABLED
	ObjectID pc_window;
	bool pc_touching = false;
	void _open_pc_window(Viewport *p_viewport);
	void _close_pc_window();
	void _pc_window_input(const Ref<InputEvent> &p_event);
#endif

protected:
	static void _bind_methods();

public:
	static Cafe *get_singleton() { return singleton; }

	// True when a second GamePad screen exists: the Wii U itself, or the
	// desktop's GamePad window. False headless.
	bool is_available() const;
	// True only on a real Wii U.
	bool is_wiiu() const;
	void set_gamepad_viewport(Viewport *p_viewport);
	Viewport *get_gamepad_viewport() const;
	Vector2i get_gamepad_size() const { return Vector2i(854, 480); }
	// The render target texture of the GamePad viewport (renderer use).
	RID get_gamepad_texture() const;

	Cafe();
	~Cafe();
};

void register_wiiu_api();
void unregister_wiiu_api();
