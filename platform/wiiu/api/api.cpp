#include "api.h"

#include "core/config/engine.h"
#include "scene/main/viewport.h"
#include "servers/rendering/rendering_server.h"

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
	return false;
#endif
}

void Cafe::set_gamepad_viewport(Viewport *p_viewport) {
	gamepad_viewport = p_viewport ? p_viewport->get_instance_id() : ObjectID();
	if (SubViewport *sv = Object::cast_to<SubViewport>(p_viewport)) {
		// It is drawn by the display, not sampled by a node: always update it.
		sv->set_update_mode(SubViewport::UPDATE_ALWAYS);
	}
}

Viewport *Cafe::get_gamepad_viewport() const {
	return gamepad_viewport.is_valid() ? Object::cast_to<Viewport>(ObjectDB::get_instance(gamepad_viewport)) : nullptr;
}

RID Cafe::get_gamepad_texture() const {
	Viewport *vp = get_gamepad_viewport();
	return vp ? RS::get_singleton()->viewport_get_texture(vp->get_viewport_rid()) : RID();
}

void Cafe::_bind_methods() {
	ClassDB::bind_method(D_METHOD("is_available"), &Cafe::is_available);
	ClassDB::bind_method(D_METHOD("set_gamepad_viewport", "viewport"), &Cafe::set_gamepad_viewport);
	ClassDB::bind_method(D_METHOD("get_gamepad_viewport"), &Cafe::get_gamepad_viewport);
	ClassDB::bind_method(D_METHOD("get_gamepad_size"), &Cafe::get_gamepad_size);
}

static Cafe *cafe_display = nullptr;

void register_wiiu_api() {
	GDREGISTER_CLASS(Cafe);
	cafe_display = memnew(Cafe);
	Engine::get_singleton()->add_singleton(Engine::Singleton("Cafe", Cafe::get_singleton()));
}

void unregister_wiiu_api() {
	Engine::get_singleton()->remove_singleton("Cafe");
	memdelete(cafe_display);
	cafe_display = nullptr;
}
