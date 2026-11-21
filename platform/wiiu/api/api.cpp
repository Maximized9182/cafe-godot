#include "api.h"

#include "core/config/engine.h"
#include "core/input/input_event.h"
#include "core/object/callable_mp.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "servers/display/display_server.h"
#include "servers/rendering/rendering_server.h"

#ifndef WIIU_ENABLED
#include "scene/gui/texture_rect.h"
#endif

Cafe::Cafe() {
	singleton = this;
}

Cafe::~Cafe() {
	if (singleton == this) {
		singleton = nullptr;
	}
}

bool Cafe::is_available() const {
#ifdef WIIU_ENABLED
	return true;
#else
	return DisplayServer::get_singleton() && DisplayServer::get_singleton()->get_name() != "headless";
#endif
}

bool Cafe::is_wiiu() const {
#ifdef WIIU_ENABLED
	return true;
#else
	return false;
#endif
}

void Cafe::set_gamepad_viewport(Viewport *p_viewport) {
	gamepad_viewport = p_viewport ? p_viewport->get_instance_id() : ObjectID();
	if (SubViewport *sv = Object::cast_to<SubViewport>(p_viewport)) {
		// It is drawn by the display, not sampled by a node: always update it.
		sv->set_update_mode(SubViewport::UPDATE_ALWAYS);
	}
#ifndef WIIU_ENABLED
	if (p_viewport) {
		_open_pc_window(p_viewport);
	} else {
		_close_pc_window();
	}
#endif
}

Viewport *Cafe::get_gamepad_viewport() const {
	return gamepad_viewport.is_valid() ? Object::cast_to<Viewport>(ObjectDB::get_instance(gamepad_viewport)) : nullptr;
}

RID Cafe::get_gamepad_texture() const {
	Viewport *vp = get_gamepad_viewport();
	return vp ? RS::get_singleton()->viewport_get_texture(vp->get_viewport_rid()) : RID();
}

#ifndef WIIU_ENABLED
// The desktop's GamePad: a native 854x480 window showing the viewport. Mouse
// clicks and drags in it reach the viewport as touches (and left-mouse
// events), in the viewport's pixels, like the real GamePad's touch screen.
void Cafe::_open_pc_window(Viewport *p_viewport) {
	_close_pc_window();
	if (!is_available() || SceneTree::get_singleton() == nullptr) {
		return;
	}
	Window *win = memnew(Window);
	win->set_visible(false); // force_native can't change while shown.
	win->set_title("Cafe GamePad (854x480)");
	win->set_force_native(true);
	const Vector2i size = get_gamepad_size();
	win->set_size(size);
	win->set_content_scale_size(size);
	win->set_content_scale_mode(Window::CONTENT_SCALE_MODE_CANVAS_ITEMS);
	win->set_content_scale_aspect(Window::CONTENT_SCALE_ASPECT_KEEP);
	TextureRect *view = memnew(TextureRect);
	view->set_texture(p_viewport->get_texture());
	view->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
	view->set_stretch_mode(TextureRect::STRETCH_SCALE);
	view->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	view->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	win->add_child(view);
	win->connect("window_input", callable_mp(this, &Cafe::_pc_window_input));
	win->connect("close_requested", callable_mp((Window *)win, &Window::hide));
	// The game usually calls this from _ready, while its parent is still
	// setting up children: add and show the window afterwards.
	Window *root = SceneTree::get_singleton()->get_root();
	callable_mp((Node *)root, &Node::add_child).bind(win, false, Node::INTERNAL_MODE_DISABLED).call_deferred();
	callable_mp(win, &Window::show).call_deferred();
	pc_window = win->get_instance_id();
}

void Cafe::_close_pc_window() {
	if (Window *win = pc_window.is_valid() ? Object::cast_to<Window>(ObjectDB::get_instance(pc_window)) : nullptr) {
		win->queue_free();
	}
	pc_window = ObjectID();
	pc_touching = false;
}

void Cafe::_pc_window_input(const Ref<InputEvent> &p_event) {
	Window *win = pc_window.is_valid() ? Object::cast_to<Window>(ObjectDB::get_instance(pc_window)) : nullptr;
	Viewport *vp = get_gamepad_viewport();
	if (win == nullptr || vp == nullptr) {
		return;
	}
	// Window pixels -> the viewport's pixels (the window letterboxes it).
	const Transform2D to_view = win->get_final_transform().affine_inverse();
	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid() && mb->get_button_index() == MouseButton::LEFT) {
		const Vector2 pos = to_view.xform(mb->get_position());
		Ref<InputEventScreenTouch> touch;
		touch.instantiate();
		touch->set_index(0);
		touch->set_position(pos);
		touch->set_pressed(mb->is_pressed());
		vp->push_input(touch, true);
		Ref<InputEventMouseButton> out;
		out.instantiate();
		out->set_button_index(MouseButton::LEFT);
		out->set_pressed(mb->is_pressed());
		out->set_position(pos);
		out->set_global_position(pos);
		out->set_button_mask(mb->is_pressed() ? BitField<MouseButtonMask>(MouseButtonMask::LEFT) : BitField<MouseButtonMask>());
		vp->push_input(out, true);
		pc_touching = mb->is_pressed();
		return;
	}
	Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_valid() && pc_touching) {
		const Vector2 pos = to_view.xform(mm->get_position());
		const Vector2 rel = to_view.basis_xform(mm->get_relative());
		Ref<InputEventScreenDrag> drag;
		drag.instantiate();
		drag->set_index(0);
		drag->set_position(pos);
		drag->set_relative(rel);
		drag->set_relative_screen_position(rel);
		vp->push_input(drag, true);
		Ref<InputEventMouseMotion> out;
		out.instantiate();
		out->set_position(pos);
		out->set_global_position(pos);
		out->set_relative(rel);
		out->set_button_mask(MouseButtonMask::LEFT);
		vp->push_input(out, true);
	}
}
#endif

void Cafe::_bind_methods() {
	ClassDB::bind_method(D_METHOD("is_available"), &Cafe::is_available);
	ClassDB::bind_method(D_METHOD("is_wiiu"), &Cafe::is_wiiu);
	ClassDB::bind_method(D_METHOD("set_gamepad_viewport", "viewport"), &Cafe::set_gamepad_viewport);
	ClassDB::bind_method(D_METHOD("get_gamepad_viewport"), &Cafe::get_gamepad_viewport);
	ClassDB::bind_method(D_METHOD("get_gamepad_size"), &Cafe::get_gamepad_size);
}

static Cafe *cafe_instance = nullptr;

void register_wiiu_api() {
	GDREGISTER_CLASS(Cafe);
	cafe_instance = memnew(Cafe);
	Engine::get_singleton()->add_singleton(Engine::Singleton("Cafe", Cafe::get_singleton()));
}

void unregister_wiiu_api() {
	Engine::get_singleton()->remove_singleton("Cafe");
	memdelete(cafe_instance);
	cafe_instance = nullptr;
}
