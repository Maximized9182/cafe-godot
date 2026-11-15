#include "rasterizer_wiiu.h"

#include "core/math/math_funcs.h"
#include "core/os/os.h"
#include "core/templates/sort_array.h"
#include "servers/rendering/rendering_server_globals.h"

#include <whb/log.h>

// ===========================================================================
// LightStorageWiiU
// ===========================================================================

LightStorageWiiU::LightStorageWiiU() {
	wiiu_singleton = this;
}

LightStorageWiiU::~LightStorageWiiU() {
	wiiu_singleton = nullptr;
}

RID LightStorageWiiU::_light_allocate(RSE::LightType p_type) {
	Light light;
	light.type = p_type;
	// Light3D sets all of these when it is made; the same defaults until then.
	light.param[RSE::LIGHT_PARAM_ENERGY] = 1.0f;
	light.param[RSE::LIGHT_PARAM_INDIRECT_ENERGY] = 1.0f;
	light.param[RSE::LIGHT_PARAM_RANGE] = 5.0f;
	light.param[RSE::LIGHT_PARAM_ATTENUATION] = 1.0f;
	light.param[RSE::LIGHT_PARAM_SPOT_ANGLE] = 45.0f;
	light.param[RSE::LIGHT_PARAM_SPOT_ATTENUATION] = 1.0f;
	return light_owner.make_rid(light);
}

void LightStorageWiiU::light_free(RID p_rid) {
	Light *light = light_owner.get_or_null(p_rid);
	if (light) {
		light->dependency.deleted_notify(p_rid);
		light_owner.free(p_rid);
	}
}

void LightStorageWiiU::light_set_color(RID p_light, const Color &p_color) {
	if (Light *light = light_owner.get_or_null(p_light)) {
		light->color = p_color;
	}
}

void LightStorageWiiU::light_set_param(RID p_light, RSE::LightParam p_param, float p_value) {
	Light *light = light_owner.get_or_null(p_light);
	if (light == nullptr || p_param < 0 || p_param >= RSE::LIGHT_PARAM_MAX) {
		return;
	}
	light->param[p_param] = p_value;
	if (p_param == RSE::LIGHT_PARAM_RANGE || p_param == RSE::LIGHT_PARAM_SPOT_ANGLE) {
		light->dependency.changed_notify(Dependency::DEPENDENCY_CHANGED_AABB); // The culling bounds.
	}
	light->dependency.changed_notify(Dependency::DEPENDENCY_CHANGED_LIGHT);
}

void LightStorageWiiU::light_set_negative(RID p_light, bool p_enable) {
	if (Light *light = light_owner.get_or_null(p_light)) {
		light->negative = p_enable;
	}
}

void LightStorageWiiU::light_set_cull_mask(RID p_light, uint32_t p_mask) {
	if (Light *light = light_owner.get_or_null(p_light)) {
		light->cull_mask = p_mask;
		light->dependency.changed_notify(Dependency::DEPENDENCY_CHANGED_CULL_MASK);
	}
}

void LightStorageWiiU::light_directional_set_sky_mode(RID p_light, RSE::LightDirectionalSkyMode p_mode) {
	if (Light *light = light_owner.get_or_null(p_light)) {
		light->sky_mode = p_mode;
	}
}

RSE::LightDirectionalSkyMode LightStorageWiiU::light_directional_get_sky_mode(RID p_light) const {
	const Light *light = light_owner.get_or_null(p_light);
	return light ? light->sky_mode : RSE::LIGHT_DIRECTIONAL_SKY_MODE_LIGHT_AND_SKY;
}

RSE::LightType LightStorageWiiU::light_get_type(RID p_light) const {
	const Light *light = light_owner.get_or_null(p_light);
	return light ? light->type : RSE::LIGHT_DIRECTIONAL;
}

AABB LightStorageWiiU::light_get_aabb(RID p_light) const {
	const Light *light = light_owner.get_or_null(p_light);
	if (light == nullptr || light->type == RSE::LIGHT_DIRECTIONAL) {
		return AABB();
	}
	const float r = light->param[RSE::LIGHT_PARAM_RANGE];
	return AABB(-Vector3(r, r, r), Vector3(r, r, r) * 2.0f);
}

float LightStorageWiiU::light_get_param(RID p_light, RSE::LightParam p_param) {
	const Light *light = light_owner.get_or_null(p_light);
	return light && p_param >= 0 && p_param < RSE::LIGHT_PARAM_MAX ? light->param[p_param] : 0.0f;
}

Color LightStorageWiiU::light_get_color(RID p_light) {
	const Light *light = light_owner.get_or_null(p_light);
	return light ? light->color : Color();
}

uint32_t LightStorageWiiU::light_get_cull_mask(RID p_light) const {
	const Light *light = light_owner.get_or_null(p_light);
	return light ? light->cull_mask : 0xFFFFFFFF;
}

RID LightStorageWiiU::light_instance_create(RID p_light) {
	LightInstance li;
	li.light = p_light;
	return light_instance_owner.make_rid(li);
}

void LightStorageWiiU::light_instance_free(RID p_light_instance) {
	if (light_instance_owner.owns(p_light_instance)) {
		light_instance_owner.free(p_light_instance);
	}
}

void LightStorageWiiU::light_instance_set_transform(RID p_light_instance, const Transform3D &p_transform) {
	if (LightInstance *li = light_instance_owner.get_or_null(p_light_instance)) {
		li->transform = p_transform;
	}
}

// ===========================================================================
// RasterizerSceneWiiU
// ===========================================================================

// The built-in shader. Lighting is in world space and linear colour: the
// albedo texture and vertex colours are sRGB (StandardMaterial3D's
// source_color), and the result goes back to sRGB (no tone mapping).
// Light colours arrive already multiplied by their energy, as Godot's
// renderers do it (the 1/PI of Lambert cancels Godot's PI in the energy).
static const char *scene_vs_glsl =
		"#version 330\n"
		"#extension GL_ARB_separate_shader_objects : enable\n"
		"layout(location = 0) in vec3 VertexPosition;\n"
		"layout(location = 1) in vec3 VertexNormal;\n"
		"layout(location = 2) in vec2 VertexTexCoord;\n"
		"layout(location = 3) in vec4 VertexColor;\n"
		"layout(location = 0) out vec3 VaryingWorldPos;\n"
		"layout(location = 1) out vec3 VaryingNormal;\n"
		"layout(location = 2) out vec2 VaryingTexCoord;\n"
		"layout(location = 3) out vec4 VaryingColor;\n"
		"uniform mat4 mvp;\n"
		"uniform mat4 model;\n"
		"uniform mat4 normal_matrix;\n"
		"uniform vec4 uv1;\n"
		"void main()\n"
		"{\n"
		"    gl_Position = mvp * vec4(VertexPosition, 1.0);\n"
		"    VaryingWorldPos = (model * vec4(VertexPosition, 1.0)).xyz;\n"
		"    VaryingNormal = (normal_matrix * vec4(VertexNormal, 0.0)).xyz;\n"
		"    VaryingTexCoord = VertexTexCoord * uv1.xy + uv1.zw;\n"
		"    VaryingColor = VertexColor;\n"
		"}\n";

static const char *scene_ps_glsl =
		"#version 330\n"
		"#extension GL_ARB_separate_shader_objects : enable\n"
		"layout(location = 0) in vec3 VaryingWorldPos;\n"
		"layout(location = 1) in vec3 VaryingNormal;\n"
		"layout(location = 2) in vec2 VaryingTexCoord;\n"
		"layout(location = 3) in vec4 VaryingColor;\n"
		"layout(location = 0) out vec4 frag_color;\n"
		"uniform sampler2D albedo_texture;\n"
		"uniform vec4 albedo;\n" // Linear RGB, alpha.
		"uniform vec4 ambient;\n" // Linear RGB, from above.
		"uniform vec4 ambient_ground;\n" // Linear RGB, from below (sky ambient; else the same).
		"uniform vec4 params;\n" // x: unshaded, y: alpha scissor threshold (< 0: off), z: omni lights, w: vertex colour.
		"uniform vec4 flags;\n" // x: directional light on.
		"uniform vec4 dir_direction;\n" // Towards the light.
		"uniform vec4 dir_color;\n"
		"uniform vec4 omni_position0;\n" // xyz, w: 1 / range.
		"uniform vec4 omni_position1;\n"
		"uniform vec4 omni_position2;\n"
		"uniform vec4 omni_position3;\n"
		"uniform vec4 omni_color0;\n" // rgb, w: attenuation (decay).
		"uniform vec4 omni_color1;\n"
		"uniform vec4 omni_color2;\n"
		"uniform vec4 omni_color3;\n"
		"vec3 omni_light(vec4 p_pos, vec4 p_color, vec3 p_vertex, vec3 p_normal)\n"
		"{\n"
		"    vec3 d = p_pos.xyz - p_vertex;\n"
		"    float dist = length(d);\n"
		"    vec3 l = d / max(dist, 0.0001);\n"
		"    float nd = dist * p_pos.w;\n"
		"    nd *= nd;\n"
		"    nd *= nd;\n"
		"    nd = max(1.0 - nd, 0.0);\n"
		"    nd *= nd;\n"
		"    float att = nd * pow(max(dist, 0.0001), -p_color.w);\n"
		"    return p_color.rgb * (max(dot(p_normal, l), 0.0) * att);\n"
		"}\n"
		"void main()\n"
		"{\n"
		"    vec4 tex = texture(albedo_texture, VaryingTexCoord);\n"
		"    tex.rgb = pow(tex.rgb, vec3(2.2));\n"
		"    vec4 base = albedo * tex;\n"
		"    if (params.w > 0.5) {\n"
		"        base *= vec4(pow(VaryingColor.rgb, vec3(2.2)), VaryingColor.a);\n"
		"    }\n"
		"    if (params.y >= 0.0 && base.a < params.y) {\n"
		"        discard;\n"
		"    }\n"
		"    vec3 color = base.rgb;\n"
		"    if (params.x < 0.5) {\n"
		"        vec3 n = normalize(VaryingNormal);\n"
		"        vec3 light = mix(ambient_ground.rgb, ambient.rgb, n.y * 0.5 + 0.5);\n"
		"        if (flags.x > 0.5) {\n"
		"            light += dir_color.rgb * max(dot(n, dir_direction.xyz), 0.0);\n"
		"        }\n"
		"        if (params.z > 0.5) {\n"
		"            light += omni_light(omni_position0, omni_color0, VaryingWorldPos, n);\n"
		"        }\n"
		"        if (params.z > 1.5) {\n"
		"            light += omni_light(omni_position1, omni_color1, VaryingWorldPos, n);\n"
		"        }\n"
		"        if (params.z > 2.5) {\n"
		"            light += omni_light(omni_position2, omni_color2, VaryingWorldPos, n);\n"
		"        }\n"
		"        if (params.z > 3.5) {\n"
		"            light += omni_light(omni_position3, omni_color3, VaryingWorldPos, n);\n"
		"        }\n"
		"        color *= light;\n"
		"    }\n"
		"    frag_color = vec4(pow(max(color, vec3(0.0)), vec3(1.0 / 2.2)), base.a);\n"
		"}\n";

bool RasterizerSceneWiiU::_ensure_program() {
	if (program) {
		return true;
	}
	if (program_failed) {
		return false;
	}
	char log[1024];
	program = wiiu_gx2_program_compile_3d(scene_vs_glsl, scene_ps_glsl, log, sizeof(log));
	if (program == nullptr) {
		program_failed = true;
		WHBLogPrintf("[3d] couldn't compile the 3D shader (needs cafeglsl.rpl): %s", log);
		return false;
	}
	loc.mvp = wiiu_gx2_program_vertex_uniform_offset(program, "mvp");
	loc.model = wiiu_gx2_program_vertex_uniform_offset(program, "model");
	loc.normal_matrix = wiiu_gx2_program_vertex_uniform_offset(program, "normal_matrix");
	loc.uv1 = wiiu_gx2_program_vertex_uniform_offset(program, "uv1");
	loc.albedo = wiiu_gx2_program_uniform_offset(program, "albedo");
	loc.ambient = wiiu_gx2_program_uniform_offset(program, "ambient");
	loc.ambient_ground = wiiu_gx2_program_uniform_offset(program, "ambient_ground");
	loc.params = wiiu_gx2_program_uniform_offset(program, "params");
	loc.flags = wiiu_gx2_program_uniform_offset(program, "flags");
	loc.dir_direction = wiiu_gx2_program_uniform_offset(program, "dir_direction");
	loc.dir_color = wiiu_gx2_program_uniform_offset(program, "dir_color");
	for (int i = 0; i < 4; i++) {
		loc.omni_position[i] = wiiu_gx2_program_uniform_offset(program, (String("omni_position") + itos(i)).utf8().get_data());
		loc.omni_color[i] = wiiu_gx2_program_uniform_offset(program, (String("omni_color") + itos(i)).utf8().get_data());
	}
	loc.albedo_texture = wiiu_gx2_program_sampler_location(program, "albedo_texture");
	WHBLogPrint("[3d] 3D shader compiled");
	return true;
}

RasterizerSceneWiiU::~RasterizerSceneWiiU() {
	wiiu_gx2_program_free(program);
	wiiu_gx2_mesh_free(sky_triangle);
}

RenderGeometryInstance *RasterizerSceneWiiU::geometry_instance_create(RID p_base) {
	const RSE::InstanceType type = RSG::utilities->get_base_type(p_base);
	ERR_FAIL_COND_V(!((1 << type) & RSE::INSTANCE_GEOMETRY_MASK), nullptr);
	GeometryInstanceWiiU *gi = wiiu_geometry_alloc.alloc();
	gi->base = p_base;
	gi->base_type = type;
	return gi;
}

void RasterizerSceneWiiU::geometry_instance_free(RenderGeometryInstance *p_geometry_instance) {
	GeometryInstanceWiiU *gi = static_cast<GeometryInstanceWiiU *>(p_geometry_instance);
	ERR_FAIL_NULL(gi);
	wiiu_geometry_alloc.free(gi);
}

Ref<RenderSceneBuffers> RasterizerSceneWiiU::render_buffers_create() {
	Ref<RenderSceneBuffersWiiU> rb;
	rb.instantiate();
	return rb;
}

RID RasterizerSceneWiiU::sky_allocate() {
	return sky_owner.make_rid(Sky());
}

void RasterizerSceneWiiU::sky_set_material(RID p_sky, RID p_material) {
	if (Sky *sky = sky_owner.get_or_null(p_sky)) {
		sky->material = p_material;
	}
}

bool RasterizerSceneWiiU::free(RID p_rid) {
	if (sky_owner.owns(p_rid)) {
		sky_owner.free(p_rid);
		return true;
	}
	return RasterizerSceneDummy::free(p_rid);
}

// Shader registers: vec4s at word offsets (-1: not in the shader).
namespace {
struct Regs {
	uint32_t words[256 * 4];
	uint32_t count = 0;

	void put(int32_t p_offset, float p_x, float p_y, float p_z, float p_w) {
		if (p_offset < 0 || (uint32_t)p_offset + 4 > 256 * 4) {
			return;
		}
		while (count < (uint32_t)p_offset + 4) {
			words[count++] = 0;
		}
		const float v[4] = { p_x, p_y, p_z, p_w };
		memcpy(&words[p_offset], v, sizeof(v));
	}
	void put(int32_t p_offset, const Color &p_c) { put(p_offset, p_c.r, p_c.g, p_c.b, p_c.a); }
	// A mat4: one column per register.
	void put(int32_t p_offset, const Projection &p_m) {
		if (p_offset < 0) {
			return;
		}
		for (int c = 0; c < 4; c++) {
			put(p_offset + 4 * c, p_m.columns[c].x, p_m.columns[c].y, p_m.columns[c].z, p_m.columns[c].w);
		}
	}
	void put(int32_t p_offset, const Transform3D &p_t) { put(p_offset, Projection(p_t)); }
};

// One surface to draw this frame.
struct DrawItem {
	const WiiUMesh *mesh;
	Transform3D transform;
	MaterialStorageWiiU::SpatialMaterial material;
	float alpha = 1.0f; // GeometryInstance3D.transparency.
	float depth = 0.0f; // Distance along the camera's view (transparent sort).
};

struct SortBackToFront {
	_FORCE_INLINE_ bool operator()(const DrawItem *p_a, const DrawItem *p_b) const { return p_a->depth > p_b->depth; }
};
} // namespace

// ProceduralSkyMaterial's default horizon: the background under a sky until
// the sky shader draws over it (and the whole sky if it can't be compiled).
static const Color SKY_FALLBACK_BG = Color(0.6463f, 0.6558f, 0.6708f);

static Color material_color(RID p_material, const char *p_param, const Color &p_default) {
	const Variant v = RSG::material_storage->material_get_param(p_material, StringName(p_param));
	return v.get_type() == Variant::COLOR ? Color(v) : p_default;
}

// The ambient light a sky gives, from above and from below (sRGB). The sky's
// radiance isn't integrated: ProceduralSkyMaterial and PhysicalSkyMaterial
// colours give a top/bottom pair, other skies a grey-blue guess.
static void sky_ambient(RID p_material, Color &r_top, Color &r_bottom) {
	r_top = Color(0.5f, 0.55f, 0.6f);
	r_bottom = Color(0.35f, 0.33f, 0.3f);
	if (p_material.is_null()) {
		return;
	}
	const Color none(-1, -1, -1);
	const Color top = material_color(p_material, "sky_top_color", none);
	if (top.r >= 0.0f) {
		const Color horizon = material_color(p_material, "sky_horizon_color", top);
		const Color ground = material_color(p_material, "ground_bottom_color", horizon);
		const Color ground_horizon = material_color(p_material, "ground_horizon_color", ground);
		r_top = top.lerp(horizon, 0.5f);
		r_bottom = ground.lerp(ground_horizon, 0.5f);
		return;
	}
	const Color rayleigh = material_color(p_material, "rayleigh_color", none);
	if (rayleigh.r >= 0.0f) {
		r_top = rayleigh;
		r_bottom = material_color(p_material, "ground_color", r_bottom);
	}
}

void RasterizerSceneWiiU::render_scene(const Ref<RenderSceneBuffers> &p_render_buffers, const CameraData *p_camera_data, const CameraData *p_prev_camera_data, const PagedArray<RenderGeometryInstance *> &p_instances, const PagedArray<RID> &p_lights, const PagedArray<RID> &p_reflection_probes, const PagedArray<RID> &p_voxel_gi_instances, const PagedArray<RID> &p_decals, const PagedArray<RID> &p_lightmaps, const PagedArray<RID> &p_fog_volumes, RID p_environment, RID p_camera_attributes, RID p_compositor, RID p_shadow_atlas, RID p_occluder_debug_tex, RID p_reflection_atlas, RID p_reflection_probe, int p_reflection_probe_pass, float p_screen_mesh_lod_threshold, const RenderShadowData *p_render_shadows, int p_render_shadow_count, const RenderSDFGIData *p_render_sdfgi_regions, int p_render_sdfgi_region_count, float p_window_output_max_value, const RenderSDFGIUpdateData *p_sdfgi_update_data, RenderingServerTypes::RenderInfo *r_render_info) {
	RenderSceneBuffersWiiU *rb = Object::cast_to<RenderSceneBuffersWiiU>(p_render_buffers.ptr());
	TextureStorageWiiU *ts = TextureStorageWiiU::get_wiiu_singleton();
	MeshStorageWiiU *ms = MeshStorageWiiU::get_wiiu_singleton();
	MaterialStorageWiiU *mat_storage = MaterialStorageWiiU::get_wiiu_singleton();
	LightStorageWiiU *ls = LightStorageWiiU::get_wiiu_singleton();
	if (rb == nullptr || p_camera_data == nullptr || ts == nullptr || ms == nullptr || mat_storage == nullptr || ls == nullptr) {
		return;
	}
	TextureStorageWiiU::RenderTarget *rt = ts->get_render_target(rb->render_target);
	if (rt == nullptr || rt->gpu == nullptr || !wiiu_gx2_can_draw()) {
		return;
	}
	wiiu_gx2_bind_render_target(rt->gpu);
	wiiu_gx2_set_scissor(0, 0, rt->size.x, rt->size.y);

	// Background and ambient light from the environment.
	bool clear = rt->clear_requested;
	Color clear_color = rt->clear_color;
	Color ambient(0, 0, 0);
	Color ambient_ground(0, 0, 0);
	bool draw_sky = false;
	if (is_environment(p_environment)) {
		const float bg_energy = environment_get_bg_energy_multiplier(p_environment);
		Color bg = clear_color;
		switch (environment_get_background(p_environment)) {
			case RSE::ENV_BG_CLEAR_COLOR:
				clear = true;
				break;
			case RSE::ENV_BG_COLOR:
				bg = environment_get_bg_color(p_environment);
				clear_color = Color(bg.r * bg_energy, bg.g * bg_energy, bg.b * bg_energy, bg.a);
				clear = true;
				break;
			case RSE::ENV_BG_SKY:
				bg = SKY_FALLBACK_BG;
				clear_color = bg;
				clear = true;
				draw_sky = true;
				break;
			default: // Canvas, keep: what is already there.
				clear = false;
				break;
		}
		const float ambient_energy = environment_get_ambient_light_energy(p_environment);
		switch (environment_get_ambient_source(p_environment)) {
			case RSE::ENV_AMBIENT_SOURCE_DISABLED:
				break;
			case RSE::ENV_AMBIENT_SOURCE_COLOR:
				ambient = environment_get_ambient_light(p_environment).srgb_to_linear() * ambient_energy;
				break;
			default: // Background or sky.
				if (environment_get_background(p_environment) == RSE::ENV_BG_SKY || environment_get_ambient_source(p_environment) == RSE::ENV_AMBIENT_SOURCE_SKY) {
					// Sky light mixed with the ambient colour by sky_contribution, as in drivers/gles3.
					const Sky *sky = sky_owner.get_or_null(environment_get_sky(p_environment));
					Color top, bottom;
					sky_ambient(sky ? sky->material : RID(), top, bottom);
					const float mix = environment_get_ambient_sky_contribution(p_environment);
					const Color color = environment_get_ambient_light(p_environment).srgb_to_linear() * ambient_energy;
					ambient = color.lerp(top.srgb_to_linear() * (bg_energy * ambient_energy), mix);
					ambient_ground = color.lerp(bottom.srgb_to_linear() * (bg_energy * ambient_energy), mix);
				} else {
					ambient = bg.srgb_to_linear() * (bg_energy * ambient_energy);
				}
				break;
		}
		if (environment_get_ambient_source(p_environment) != RSE::ENV_AMBIENT_SOURCE_SKY &&
				!(environment_get_ambient_source(p_environment) == RSE::ENV_AMBIENT_SOURCE_BG && environment_get_background(p_environment) == RSE::ENV_BG_SKY)) {
			ambient_ground = ambient;
		}
	}
	if (clear) {
		wiiu_gx2_clear(clear_color.r, clear_color.g, clear_color.b, rt->transparent ? clear_color.a : 1.0f);
	}
	rt->clear_requested = false;
	rt->used = true;
	if (!_ensure_program()) {
		return;
	}
	wiiu_gx2_clear_depth();

	// Lights: one directional, up to four omni.
	bool has_dir = false;
	Vector3 dir_direction;
	Color dir_color;
	int omni_count = 0;
	Vector3 omni_position[4];
	float omni_inv_range[4] = {};
	Color omni_color[4];
	float omni_decay[4] = {};
	for (uint32_t i = 0; i < p_lights.size(); i++) {
		const LightStorageWiiU::LightInstance *li = ls->get_light_instance(p_lights[i]);
		const LightStorageWiiU::Light *light = li ? ls->get_light(li->light) : nullptr;
		if (light == nullptr) {
			continue;
		}
		const float energy = light->param[RSE::LIGHT_PARAM_ENERGY] * (light->negative ? -1.0f : 1.0f);
		const Color c = light->color.srgb_to_linear() * energy;
		if (light->type == RSE::LIGHT_DIRECTIONAL) {
			if (!has_dir) {
				has_dir = true;
				dir_direction = li->transform.basis.get_column(2).normalized();
				dir_color = c;
			}
		} else if (light->type == RSE::LIGHT_OMNI && omni_count < 4) {
			omni_position[omni_count] = li->transform.origin;
			omni_inv_range[omni_count] = 1.0f / MAX(light->param[RSE::LIGHT_PARAM_RANGE], 0.0001f);
			omni_color[omni_count] = c;
			omni_decay[omni_count] = light->param[RSE::LIGHT_PARAM_ATTENUATION];
			omni_count++;
		}
	}

	// What to draw: every surface of the visible meshes and 3D multimeshes.
	const Transform3D view = p_camera_data->main_transform.affine_inverse();
	const Projection view_projection = p_camera_data->main_projection * Projection(view);
	const Vector3 cam_pos = p_camera_data->main_transform.origin;
	const Vector3 cam_forward = -p_camera_data->main_transform.basis.get_column(2).normalized();
	LocalVector<DrawItem> items;
	auto add_mesh = [&](GeometryInstanceWiiU *p_gi, RID p_mesh, const Transform3D &p_transform, bool p_instance_materials) {
		const LocalVector<MeshStorageWiiU::Surface> *surfaces = ms->get_surfaces(p_mesh);
		if (surfaces == nullptr) {
			return;
		}
		for (uint32_t s = 0; s < surfaces->size(); s++) {
			const MeshStorageWiiU::Surface &surface = (*surfaces)[s];
			if (surface.gpu == nullptr) {
				continue;
			}
			RID material = p_gi->material_override;
			if (material.is_null() && p_instance_materials && (int)s < p_gi->surface_materials.size()) {
				material = p_gi->surface_materials[s];
			}
			if (material.is_null()) {
				material = ms->mesh_surface_get_material(p_mesh, s);
			}
			DrawItem item;
			item.mesh = surface.gpu;
			item.transform = p_transform;
			if (material.is_null() || !mat_storage->get_spatial_material(material, item.material)) {
				item.material = MaterialStorageWiiU::SpatialMaterial();
			}
			item.alpha = 1.0f - p_gi->transparency;
			const Vector3 center = p_transform.xform(surface.has_aabb ? surface.aabb.get_center() : Vector3());
			item.depth = (center - cam_pos).dot(cam_forward);
			items.push_back(item);
		}
	};
	for (uint32_t i = 0; i < p_instances.size(); i++) {
		GeometryInstanceWiiU *gi = static_cast<GeometryInstanceWiiU *>(p_instances[i]);
		if (gi->base_type == RSE::INSTANCE_MESH) {
			add_mesh(gi, gi->base, gi->transform, true);
		} else if (gi->base_type == RSE::INSTANCE_MULTIMESH) {
			const MeshStorageWiiU::MultiMesh *mm = ms->get_multimesh(gi->base);
			if (mm == nullptr || mm->format != RSE::MULTIMESH_TRANSFORM_3D || mm->mesh.is_null()) {
				continue;
			}
			const int count = mm->visible_instances < 0 ? mm->instances : MIN(mm->visible_instances, mm->instances);
			for (int n = 0; n < count; n++) {
				const float *d = mm->buffer.ptr() + n * mm->stride;
				// 3x4 row-major: basis rows with the origin in the last column.
				const Transform3D xf(d[0], d[1], d[2], d[4], d[5], d[6], d[8], d[9], d[10], d[3], d[7], d[11]);
				add_mesh(gi, mm->mesh, gi->transform * xf, false);
			}
		}
	}

	LocalVector<DrawItem *> opaque;
	LocalVector<DrawItem *> transparent;
	for (DrawItem &item : items) {
		const bool blended = item.material.shader.transparent || item.alpha < 1.0f;
		(blended ? transparent : opaque).push_back(&item);
	}
	if (transparent.size() > 1) {
		SortArray<DrawItem *, SortBackToFront> sorter;
		sorter.sort(transparent.ptr(), transparent.size());
	}

	static Regs vs, ps;
	auto draw = [&](const DrawItem &p_item, bool p_blended) {
		const MaterialStorageWiiU::SpatialMaterial &m = p_item.material;
		vs.count = 0;
		vs.put(loc.mvp, view_projection * Projection(p_item.transform));
		vs.put(loc.model, p_item.transform);
		vs.put(loc.normal_matrix, Transform3D(p_item.transform.basis.inverse().transposed(), Vector3()));
		vs.put(loc.uv1, m.uv1.x, m.uv1.y, m.uv1.z, m.uv1.w);
		ps.count = 0;
		Color albedo = m.albedo.srgb_to_linear();
		albedo.a = m.albedo.a * p_item.alpha;
		ps.put(loc.albedo, albedo);
		ps.put(loc.ambient, ambient);
		ps.put(loc.ambient_ground, ambient_ground);
		ps.put(loc.params, m.shader.unshaded ? 1.0f : 0.0f, m.shader.alpha_scissor ? m.alpha_scissor_threshold : -1.0f,
				(float)omni_count, m.shader.vertex_color ? 1.0f : 0.0f);
		ps.put(loc.flags, has_dir ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
		if (has_dir) {
			ps.put(loc.dir_direction, dir_direction.x, dir_direction.y, dir_direction.z, 0.0f);
			ps.put(loc.dir_color, dir_color);
		}
		for (int i = 0; i < omni_count; i++) {
			ps.put(loc.omni_position[i], omni_position[i].x, omni_position[i].y, omni_position[i].z, omni_inv_range[i]);
			ps.put(loc.omni_color[i], omni_color[i].r, omni_color[i].g, omni_color[i].b, omni_decay[i]);
		}
		WiiUProgramTexture tex;
		tex.texture = m.texture;
		tex.location = loc.albedo_texture;
		tex.filter = m.shader.filter;
		tex.repeat = m.shader.repeat;

		WiiUMeshDraw d = {};
		d.program = program;
		d.mesh = p_item.mesh;
		d.vs_regs = vs.words;
		d.vs_reg_words = vs.count;
		d.ps_regs = ps.words;
		d.ps_reg_words = ps.count;
		d.textures = &tex;
		d.texture_count = 1;
		d.blend = p_blended ? m.shader.blend : WIIU_BLEND_DISABLED;
		d.cull = m.shader.cull;
		d.depth_test = m.shader.depth_test;
		d.depth_write = m.shader.depth_draw == 2 || (m.shader.depth_draw == 1 && !p_blended);
		wiiu_gx2_draw_mesh(&d);
	};
	for (DrawItem *item : opaque) {
		draw(*item, false);
	}
	if (draw_sky) {
		_draw_sky(p_environment, p_camera_data, p_lights);
	}
	for (DrawItem *item : transparent) {
		draw(*item, true);
	}
	if (r_render_info) {
		r_render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_OBJECTS_IN_FRAME] += p_instances.size();
		r_render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_DRAW_CALLS_IN_FRAME] += items.size();
	}
}

void RasterizerSceneWiiU::_draw_sky(RID p_environment, const CameraData *p_camera, const PagedArray<RID> &p_lights) {
	const Sky *sky = sky_owner.get_or_null(environment_get_sky(p_environment));
	MaterialStorageWiiU *ms = MaterialStorageWiiU::get_wiiu_singleton();
	LightStorageWiiU *ls = LightStorageWiiU::get_wiiu_singleton();
	if (sky == nullptr || sky->material.is_null() || ms == nullptr || !ms->material_is_sky(sky->material)) {
		return; // The flat fallback colour stays.
	}
	WiiUDrawState st = WIIU_DRAW_STATE_DEFAULT;
	if (!ms->get_material_program(sky->material, WIIU_FILTER_LINEAR | WIIU_FILTER_MIPMAPS, true, st)) {
		return;
	}
	if (sky_triangle == nullptr) {
		// One triangle covering the screen (clip space; the shader puts it at z = 1).
		WiiUVertex3D v[3] = {};
		v[0].x = -1.0f;
		v[0].y = -1.0f;
		v[1].x = 3.0f;
		v[1].y = -1.0f;
		v[2].x = -1.0f;
		v[2].y = 3.0f;
		sky_triangle = wiiu_gx2_mesh_create(WIIU_PRIM_TRIANGLES, v, 3, nullptr, 0);
		if (sky_triangle == nullptr) {
			return;
		}
	}
	WiiUProgram *sky_program = st.program;

	static Regs vs, ps;
	// Clip position -> world-space view direction: the inverse of the
	// projection times the camera rotation (no translation).
	const Transform3D rotation(p_camera->main_transform.basis.orthonormalized().inverse(), Vector3());
	const Projection inv_view_projection = (p_camera->main_projection * Projection(rotation)).inverse();
	vs.count = 0;
	vs.put(wiiu_gx2_program_vertex_uniform_offset(sky_program, "sky_inv_view_projection"), inv_view_projection);

	ps.count = MIN(st.reg_words, (uint32_t)(256 * 4));
	if (ps.count > 0) {
		memcpy(ps.words, st.regs, ps.count * 4);
	}
	ps.put(wiiu_gx2_program_uniform_offset(sky_program, "sky_params"), (float)(OS::get_singleton()->get_ticks_usec() / 1000000.0),
			environment_get_bg_energy_multiplier(p_environment), 0.0f, 0.0f);
	const Vector3 pos = p_camera->main_transform.origin;
	ps.put(wiiu_gx2_program_uniform_offset(sky_program, "sky_position"), pos.x, pos.y, pos.z, 1.0f);
	// Directional lights that show in the sky (drivers/gles3: sky_globals).
	int count = 0;
	for (uint32_t i = 0; i < p_lights.size() && count < 4; i++) {
		const LightStorageWiiU::LightInstance *li = ls ? ls->get_light_instance(p_lights[i]) : nullptr;
		const LightStorageWiiU::Light *light = li ? ls->get_light(li->light) : nullptr;
		if (light == nullptr || light->type != RSE::LIGHT_DIRECTIONAL || light->sky_mode == RSE::LIGHT_DIRECTIONAL_SKY_MODE_LIGHT_ONLY) {
			continue;
		}
		const Vector3 dir = li->transform.basis.xform(Vector3(0, 0, 1)).normalized();
		const float energy = light->param[RSE::LIGHT_PARAM_ENERGY] * (light->negative ? -1.0f : 1.0f);
		const String n = itos(count);
		ps.put(wiiu_gx2_program_uniform_offset(sky_program, ("sky_light" + n + "_direction").utf8().get_data()), dir.x, dir.y, dir.z, energy);
		ps.put(wiiu_gx2_program_uniform_offset(sky_program, ("sky_light" + n + "_color").utf8().get_data()), light->color.r,
				light->color.g, light->color.b, Math::deg_to_rad(light->param[RSE::LIGHT_PARAM_SIZE]));
		count++;
	}
	ps.put(wiiu_gx2_program_uniform_offset(sky_program, "sky_light_count"), (float)count, 0.0f, 0.0f, 0.0f);

	WiiUMeshDraw d = {};
	d.program = sky_program;
	d.mesh = sky_triangle;
	d.vs_regs = vs.words;
	d.vs_reg_words = vs.count;
	d.ps_regs = ps.words;
	d.ps_reg_words = ps.count;
	d.textures = st.textures;
	d.texture_count = st.texture_count;
	d.blend = WIIU_BLEND_DISABLED;
	d.cull = WIIU_CULL_DISABLED;
	d.depth_test = true; // Only where nothing was drawn (depth still 1.0).
	d.depth_write = false;
	wiiu_gx2_draw_mesh(&d);
}
