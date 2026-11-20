#include "api.h"

#include "core/config/engine.h"
#include "scene/main/viewport.h"
#include "servers/rendering/rendering_server.h"

CafeDisplay::CafeDisplay() {
	singleton = this;
}

CafeDisplay::~CafeDisplay() {
	if (singleton == this) {
		singleton = nullptr;
	}
}

bool CafeDisplay::is_available() const {
#ifdef WIIU_ENABLED
	return true;
#else
	return false;
#endif
}

void CafeDisplay::set_gamepad_viewport(Viewport *p_viewport) {
	gamepad_viewport = p_viewport ? p_viewport->get_instance_id() : ObjectID();
	if (SubViewport *sv = Object::cast_to<SubViewport>(p_viewport)) {
		// It is drawn by the display, not sampled by a node: always update it.
		sv->set_update_mode(SubViewport::UPDATE_ALWAYS);
	}
}

Viewport *CafeDisplay::get_gamepad_viewport() const {
	return gamepad_viewport.is_valid() ? Object::cast_to<Viewport>(ObjectDB::get_instance(gamepad_viewport)) : nullptr;
}

RID CafeDisplay::get_gamepad_texture() const {
	Viewport *vp = get_gamepad_viewport();
	return vp ? RS::get_singleton()->viewport_get_texture(vp->get_viewport_rid()) : RID();
}

void CafeDisplay::_bind_methods() {
	ClassDB::bind_method(D_METHOD("is_available"), &CafeDisplay::is_available);
	ClassDB::bind_method(D_METHOD("set_gamepad_viewport", "viewport"), &CafeDisplay::set_gamepad_viewport);
	ClassDB::bind_method(D_METHOD("get_gamepad_viewport"), &CafeDisplay::get_gamepad_viewport);
	ClassDB::bind_method(D_METHOD("get_gamepad_size"), &CafeDisplay::get_gamepad_size);
}

static CafeDisplay *cafe_display = nullptr;

void register_wiiu_api() {
	GDREGISTER_CLASS(CafeDisplay);
	cafe_display = memnew(CafeDisplay);
	Engine::get_singleton()->add_singleton(Engine::Singleton("CafeDisplay", CafeDisplay::get_singleton()));
}

void unregister_wiiu_api() {
	Engine::get_singleton()->remove_singleton("CafeDisplay");
	memdelete(cafe_display);
	cafe_display = nullptr;
}
