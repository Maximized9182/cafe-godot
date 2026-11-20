#pragma once

#include "core/math/vector2i.h"
#include "core/object/class_db.h"
#include "core/object/object.h"
#include "core/templates/rid.h"

class Viewport;

// Cafe Godot: the Wii U's two screens. Games not made for it get the main
// window on the TV, mirrored onto the GamePad. A game that asks for it can
// instead put its own Viewport on the GamePad:
//
//   if Engine.has_singleton("CafeDisplay"):
//       Engine.get_singleton("CafeDisplay").set_gamepad_viewport($GamePadView)
//
// The viewport is a SubViewport, usually 854x480 (the GamePad's size); it is
// shown fitted on the GamePad, and GamePad touches are delivered to it (touch
// and left-mouse events, in the viewport's own pixels) instead of to the
// main window. Passing null goes back to mirroring.
class CafeDisplay : public Object {
	GDCLASS(CafeDisplay, Object);

	static inline CafeDisplay *singleton = nullptr;
	ObjectID gamepad_viewport;

protected:
	static void _bind_methods();

public:
	static CafeDisplay *get_singleton() { return singleton; }

	// True when running on a Wii U (the class also exists in other builds of
	// this engine, where it does nothing).
	bool is_available() const;
	void set_gamepad_viewport(Viewport *p_viewport);
	Viewport *get_gamepad_viewport() const;
	Vector2i get_gamepad_size() const { return Vector2i(854, 480); }
	// The render target texture of the GamePad viewport (renderer use).
	RID get_gamepad_texture() const;

	CafeDisplay();
	~CafeDisplay();
};

void register_wiiu_api();
void unregister_wiiu_api();
