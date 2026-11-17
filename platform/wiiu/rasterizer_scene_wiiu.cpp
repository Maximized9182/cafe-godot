#include "rasterizer_wiiu.h"

#include "core/config/project_settings.h"
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

void LightStorageWiiU::light_set_shadow(RID p_light, bool p_enabled) {
	if (Light *light = light_owner.get_or_null(p_light)) {
		light->shadow = p_enabled;
	}
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

void LightStorageWiiU::light_directional_set_shadow_mode(RID p_light, RSE::LightDirectionalShadowMode p_mode) {
	if (Light *light = light_owner.get_or_null(p_light)) {
		light->shadow_mode = p_mode;
	}
}

RSE::LightDirectionalShadowMode LightStorageWiiU::light_directional_get_shadow_mode(RID p_light) {
	const Light *light = light_owner.get_or_null(p_light);
	return light ? light->shadow_mode : RSE::LIGHT_DIRECTIONAL_SHADOW_ORTHOGONAL;
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
		"uniform vec4 pbr;\n" // x: roughness, y: metallic, z: specular.
		"uniform vec4 ambient;\n" // Linear RGB, from above.
		"uniform vec4 ambient_ground;\n" // Linear RGB, from below (sky ambient; else the same).
		"uniform vec4 params;\n" // x: unshaded, y: alpha scissor threshold (< 0: off), z: local lights, w: vertex colour.
		"uniform vec4 flags;\n" // x: directional light on.
		"uniform vec4 camera_position;\n"
		"uniform vec4 dir_direction;\n" // Towards the light; w: specular amount.
		"uniform vec4 dir_color;\n"
		// Directional shadow: up to four splits (cascades) in one shadow map,
		// a quarter each (the whole map with one split). Per split: world ->
		// split clip space (mat4, one column per register) and
		// shadow_split (x: depth bias, y: normal offset in world units, z: one
		// texel of depth, w: depth range in world units). params x: on, y:
		// the size of a split in the map (1 or 0.5), w: opacity. splits: how
		// far each split reaches along the camera's view (shadow_forward).
		// Depth is a float in the red channel.
		"uniform sampler2D shadow_map;\n"
		"uniform mat4 shadow_matrix0;\n"
		"uniform mat4 shadow_matrix1;\n"
		"uniform mat4 shadow_matrix2;\n"
		"uniform mat4 shadow_matrix3;\n"
		"uniform vec4 shadow_split0;\n"
		"uniform vec4 shadow_split1;\n"
		"uniform vec4 shadow_split2;\n"
		"uniform vec4 shadow_split3;\n"
		"uniform vec4 shadow_splits;\n"
		"uniform vec4 shadow_forward;\n"
		"uniform vec4 shadow_params;\n"
		"uniform vec4 shadow_texel;\n" // xy: 1 / size, zw: size.
		"uniform vec4 shadow_slope;\n" // z: 1 for the depth colours debug view.
		// Which split P is in: its matrix, its biases and where it is in the
		// map. False past the last split.
		"bool shadow_split_of(vec3 P, out mat4 m, out vec4 b, out vec2 offset)\n"
		"{\n"
		"    float d = dot(P - camera_position.xyz, shadow_forward.xyz);\n"
		"    if (d < shadow_splits.x) {\n"
		"        m = shadow_matrix0; b = shadow_split0; offset = vec2(0.0, 0.0);\n"
		"    } else if (d < shadow_splits.y) {\n"
		"        m = shadow_matrix1; b = shadow_split1; offset = vec2(0.5, 0.0);\n"
		"    } else if (d < shadow_splits.z) {\n"
		"        m = shadow_matrix2; b = shadow_split2; offset = vec2(0.0, 0.5);\n"
		"    } else if (d < shadow_splits.w) {\n"
		"        m = shadow_matrix3; b = shadow_split3; offset = vec2(0.5, 0.5);\n"
		"    } else {\n"
		"        return false;\n"
		"    }\n"
		"    return true;\n"
		"}\n"
		// Debug (rendering/wiiu/shadow_debug = 1). Colours how far the surface
		// is behind what the shadow map holds at its texel (no biases, no
		// filtering): red = behind it (shadowed), green = in front of it
		// (lit), full colour at 0.5 units or more; blue = nothing in the map
		// there.
		"vec3 shadow_debug(vec3 P)\n"
		"{\n"
		"    mat4 m; vec4 b; vec2 offset;\n"
		"    if (!shadow_split_of(P, m, b, offset)) {\n"
		"        return vec3(0.0, 0.0, 0.3);\n"
		"    }\n"
		"    vec4 clip = m * vec4(P, 1.0);\n"
		"    vec3 c = clip.xyz / clip.w;\n"
		"    vec2 uv = vec2(c.x * 0.5 + 0.5, 0.5 - c.y * 0.5) * shadow_params.y + offset;\n"
		"    vec2 texel = (floor(uv * shadow_texel.zw) + 0.5) * shadow_texel.xy;\n"
		"    float stored = texture(shadow_map, texel).r;\n"
		"    if (stored >= 1.0) {\n"
		"        return vec3(0.0, 0.0, 1.0);\n"
		"    }\n"
		// Mode 8 (shadow_slope.w = 1): red = the stored depth, green = this
		// surface's, both as a sawtooth repeating every quarter unit. Smooth
		// ramps = full precision; flat steps in red = the map lost precision.
		"    if (shadow_slope.w > 0.5) {\n"
		"        return vec3(fract(stored * b.w * 4.0), fract((c.z * 0.5 + 0.5) * b.w * 4.0), 0.0);\n"
		"    }\n"
		"    float behind = (c.z * 0.5 + 0.5 - stored) * b.w;\n"
		"    return behind > 0.0 ? vec3(clamp(behind * 2.0, 0.1, 1.0), 0.0, 0.0) : vec3(0.0, clamp(-behind * 2.0, 0.1, 1.0), 0.0);\n"
		"}\n"
		"float shadow_depth(vec2 p_uv)\n"
		"{\n"
		"    return texture(shadow_map, p_uv).r;\n"
		"}\n"
		// 2x2 percentage-closer filtering, bilinearly weighted.
		"float directional_shadow(vec3 P, vec3 N, vec3 L)\n"
		"{\n"
		"    mat4 m; vec4 b; vec2 offset;\n"
		"    if (!shadow_split_of(P, m, b, offset)) {\n"
		"        return 1.0;\n"
		"    }\n"
		"    vec3 normal_offset = N * ((1.0 - max(0.0, dot(L, N))) * b.y);\n"
		"    vec4 clip = m * vec4(P + normal_offset, 1.0);\n"
		"    vec3 c = clip.xyz / clip.w;\n"
		"    if (any(greaterThan(abs(c), vec3(1.0)))) {\n"
		"        return 1.0;\n"
		"    }\n"
		"    vec2 uv = vec2(c.x * 0.5 + 0.5, 0.5 - c.y * 0.5) * shadow_params.y + offset;\n"
		// Slope-scaled bias: surfaces turned edge-on to the light change depth
		// fast across a texel (tan of the light angle, capped at 8 texels).
		"    float ndotl = clamp(dot(L, N), 0.01, 1.0);\n"
		"    float slope = min(sqrt(1.0 - ndotl * ndotl) / ndotl, 8.0);\n"
		"    float depth = c.z * 0.5 + 0.5 - b.x - slope * b.z;\n"
		"    vec2 t = uv * shadow_texel.zw - 0.5;\n"
		"    vec2 f = fract(t);\n"
		"    vec2 base = (floor(t) + 0.5) * shadow_texel.xy;\n"
		"    float s00 = step(depth, shadow_depth(base));\n"
		"    float s10 = step(depth, shadow_depth(base + vec2(shadow_texel.x, 0.0)));\n"
		"    float s01 = step(depth, shadow_depth(base + vec2(0.0, shadow_texel.y)));\n"
		"    float s11 = step(depth, shadow_depth(base + shadow_texel.xy));\n"
		"    float s = mix(mix(s00, s10, f.x), mix(s01, s11, f.x), f.y);\n"
		"    return mix(1.0, s, shadow_params.w);\n"
		"}\n"
		// Local lights (omni and spot), the four nearest to the object:
		// position (w: 1 / range), colour (w: attenuation), direction the
		// spot points (w: cos of the cone angle; -2 for omni lights), extra
		// (x: spot attenuation, y: specular amount).
		"uniform vec4 light_position0;\n"
		"uniform vec4 light_position1;\n"
		"uniform vec4 light_position2;\n"
		"uniform vec4 light_position3;\n"
		"uniform vec4 light_color0;\n"
		"uniform vec4 light_color1;\n"
		"uniform vec4 light_color2;\n"
		"uniform vec4 light_color3;\n"
		"uniform vec4 light_direction0;\n"
		"uniform vec4 light_direction1;\n"
		"uniform vec4 light_direction2;\n"
		"uniform vec4 light_direction3;\n"
		"uniform vec4 light_extra0;\n"
		"uniform vec4 light_extra1;\n"
		"uniform vec4 light_extra2;\n"
		"uniform vec4 light_extra3;\n"
		// drivers/gles3/shaders/scene.glsl's light_compute (Lambert diffuse,
		// Schlick-GGX specular). Light colours lack Godot's factor of PI and
		// diffuse its 1/PI; D_GGX drops its 1/PI to match.
		"void light_compute(vec3 N, vec3 L, vec3 V, vec3 light_color, float specular_amount, vec3 f0, float roughness,\n"
		"        inout vec3 diffuse_light, inout vec3 specular_light)\n"
		"{\n"
		"    float NdotL = min(dot(N, L), 1.0);\n"
		"    float cNdotL = max(NdotL, 0.0);\n"
		"    diffuse_light += light_color * cNdotL;\n"
		"    if (cNdotL <= 0.0 || specular_amount <= 0.0) {\n"
		"        return;\n"
		"    }\n"
		"    vec3 H = normalize(V + L);\n"
		"    float cNdotH = clamp(dot(N, H), 0.0, 1.0);\n"
		"    float cLdotH = clamp(dot(L, H), 0.0, 1.0);\n"
		"    float cNdotV = max(dot(N, V), 1e-4);\n"
		"    float alpha_ggx = roughness * roughness;\n"
		"    float a = cNdotH * alpha_ggx;\n"
		"    float k = alpha_ggx / (1.0 - cNdotH * cNdotH + a * a);\n"
		"    float D = k * k;\n"
		"    float G = 0.5 / mix(2.0 * cNdotL * cNdotV, cNdotL + cNdotV, alpha_ggx);\n"
		"    float m = 1.0 - cLdotH;\n"
		"    float m2 = m * m;\n"
		"    float f90 = clamp(50.0 * f0.g, 0.0, 1.0);\n"
		"    vec3 F = f0 + (f90 - f0) * (m2 * m2 * m);\n"
		"    specular_light += cNdotL * D * F * G * light_color * specular_amount;\n"
		"}\n"
		"void local_light(vec4 p_pos, vec4 p_color, vec4 p_dir, vec4 p_extra, vec3 P, vec3 N, vec3 V, vec3 f0, float roughness,\n"
		"        inout vec3 diffuse_light, inout vec3 specular_light)\n"
		"{\n"
		"    vec3 d = p_pos.xyz - P;\n"
		"    float dist = length(d);\n"
		"    vec3 L = d / max(dist, 0.0001);\n"
		"    float nd = dist * p_pos.w;\n"
		"    nd *= nd;\n"
		"    nd *= nd;\n"
		"    nd = max(1.0 - nd, 0.0);\n"
		"    nd *= nd;\n"
		"    float att = nd * pow(max(dist, 0.0001), -p_color.w);\n"
		"    if (p_dir.w > -1.5) {\n"
		"        float scos = max(dot(-L, p_dir.xyz), p_dir.w);\n"
		"        float spot_rim = max(0.0001, (1.0 - scos) / (1.0 - p_dir.w));\n"
		"        att *= 1.0 - pow(spot_rim, p_extra.x);\n"
		"    }\n"
		"    light_compute(N, L, V, p_color.rgb * att, p_extra.y, f0, roughness, diffuse_light, specular_light);\n"
		"}\n"
		"void main()\n"
		"{\n"
		"    vec4 tex = texture(albedo_texture, VaryingTexCoord);\n"
		"    tex.rgb = pow(tex.rgb, vec3(2.2));\n"
		"    vec4 base = albedo * tex;\n"
		"    if (params.w > 0.5) {\n"
		"        base *= vec4(pow(VaryingColor.rgb, vec3(2.2)), VaryingColor.a);\n"
		"    }\n"
		"#ifdef USE_ALPHA_SCISSOR\n"
		"    if (base.a < params.y) {\n"
		"        discard;\n"
		"    }\n"
		"#endif\n"
		"    vec3 color = base.rgb;\n"
		"    if (params.x < 0.5) {\n"
		"        vec3 N = normalize(VaryingNormal);\n"
		"        vec3 P = VaryingWorldPos;\n"
		"        vec3 V = normalize(camera_position.xyz - P);\n"
		"        float roughness = clamp(pbr.x, 0.04, 1.0);\n"
		"        float metallic = pbr.y;\n"
		"        float dielectric = 0.16 * pbr.z * pbr.z;\n"
		"        vec3 f0 = mix(vec3(dielectric), base.rgb, vec3(metallic));\n"
		"        vec3 diffuse_light = mix(ambient_ground.rgb, ambient.rgb, N.y * 0.5 + 0.5);\n"
		"        vec3 specular_light = vec3(0.0);\n"
		"        if (flags.x > 0.5) {\n"
		"            float shadow = shadow_params.x > 0.5 ? directional_shadow(P, N, dir_direction.xyz) : 1.0;\n"
		"            light_compute(N, dir_direction.xyz, V, dir_color.rgb * shadow, dir_direction.w, f0, roughness, diffuse_light, specular_light);\n"
		"        }\n"
		"        if (params.z > 0.5) {\n"
		"            local_light(light_position0, light_color0, light_direction0, light_extra0, P, N, V, f0, roughness, diffuse_light, specular_light);\n"
		"        }\n"
		"        if (params.z > 1.5) {\n"
		"            local_light(light_position1, light_color1, light_direction1, light_extra1, P, N, V, f0, roughness, diffuse_light, specular_light);\n"
		"        }\n"
		"        if (params.z > 2.5) {\n"
		"            local_light(light_position2, light_color2, light_direction2, light_extra2, P, N, V, f0, roughness, diffuse_light, specular_light);\n"
		"        }\n"
		"        if (params.z > 3.5) {\n"
		"            local_light(light_position3, light_color3, light_direction3, light_extra3, P, N, V, f0, roughness, diffuse_light, specular_light);\n"
		"        }\n"
		// Ambient reflections: the ambient colour seen along the reflection,
		// scaled by the split-sum approximation drivers/gles3 uses (no
		// radiance cubemap).
		"        vec3 R = reflect(-V, N);\n"
		"        vec3 reflected = mix(ambient_ground.rgb, ambient.rgb, R.y * 0.5 + 0.5);\n"
		"        float ndotv = clamp(dot(N, V), 0.0, 1.0);\n"
		"        vec4 r = roughness * vec4(-1.0, -0.0275, -0.572, 0.022) + vec4(1.0, 0.0425, 1.04, -0.04);\n"
		"        float a004 = min(r.x * r.x, exp2(-9.28 * ndotv)) * r.x + r.y;\n"
		"        vec2 env = vec2(-1.04, 1.04) * a004 + r.zw;\n"
		"        specular_light += reflected * (f0 * env.x + env.y);\n"
		"        color = base.rgb * (1.0 - metallic) * diffuse_light + specular_light;\n"
		"        if (shadow_params.x > 0.5 && shadow_slope.z > 0.5) {\n"
		"            color = shadow_debug(P);\n"
		"        }\n"
		"    }\n"
		"    frag_color = vec4(pow(max(color, vec3(0.0)), vec3(1.0 / 2.2)), base.a);\n"
		"}\n";

// The shadow pass: depth from the light into a 32-bit float render target,
// read back by directional_shadow() above. (It was packed into RGBA8 before;
// some depths came back off by a step, which showed as bands of shadow acne
// across lit faces.) That guess was wrong: the bands stayed with float depth
// and went away with a smaller shadow_max_distance, so the cause was one map
// spread over 100 units (now split, see render_scene). Float depth stays:
// it's simpler.
static const char *shadow_vs_glsl =
		"#version 330\n"
		"#extension GL_ARB_separate_shader_objects : enable\n"
		"layout(location = 0) in vec3 VertexPosition;\n"
		"layout(location = 0) out float VaryingDepth;\n"
		"uniform mat4 mvp;\n"
		"void main()\n"
		"{\n"
		"    gl_Position = mvp * vec4(VertexPosition, 1.0);\n"
		// The same 0..1 depth directional_shadow() computes (orthographic:
		// w is 1, so it interpolates linearly). gl_FragCoord.z wasn't it.
		"    VaryingDepth = gl_Position.z / gl_Position.w * 0.5 + 0.5;\n"
		"}\n";

static const char *shadow_ps_glsl =
		"#version 330\n"
		"#extension GL_ARB_separate_shader_objects : enable\n"
		"layout(location = 0) in float VaryingDepth;\n"
		"layout(location = 0) out vec4 frag_color;\n"
		"void main()\n"
		"{\n"
		// 24 bits in RGB, each channel an exact multiple of 1/255: the GPU
		// rounds when it writes UNORM8, so the usual fract() packing came
		// out up to half a step off in red (shadow acne).
		// Now the target is FLOAT_R32, so the depth goes in as it is.
		"    frag_color = vec4(clamp(VaryingDepth, 0.0, 1.0), 0.0, 0.0, 1.0);\n"
		"}\n";

// Shadow debug mode 7: the hardware's depth instead of VaryingDepth.
static const char *shadow_ps_fragcoord_glsl =
		"#version 330\n"
		"#extension GL_ARB_separate_shader_objects : enable\n"
		"layout(location = 0) in float VaryingDepth;\n"
		"layout(location = 0) out vec4 frag_color;\n"
		"void main()\n"
		"{\n"
		"    frag_color = vec4(gl_FragCoord.z + VaryingDepth * 0.0, 0.0, 0.0, 1.0);\n"
		"}\n";

bool RasterizerSceneWiiU::_ensure_shadow_program() {
	if (shadow_program) {
		return true;
	}
	if (shadow_program_failed) {
		return false;
	}
	char log[1024];
	shadow_program = wiiu_gx2_program_compile_3d(shadow_vs_glsl, shadow_ps_glsl, log, sizeof(log));
	if (shadow_program == nullptr) {
		shadow_program_failed = true;
		WHBLogPrintf("[3d] couldn't compile the shadow shader: %s", log);
		return false;
	}
	shadow_mvp = wiiu_gx2_program_vertex_uniform_offset(shadow_program, "mvp");
	shadow_program_fragcoord = wiiu_gx2_program_compile_3d(shadow_vs_glsl, shadow_ps_fragcoord_glsl, log, sizeof(log));
	if (shadow_program_fragcoord) {
		shadow_mvp_fragcoord = wiiu_gx2_program_vertex_uniform_offset(shadow_program_fragcoord, "mvp");
	} else {
		WHBLogPrintf("[3d] couldn't compile the gl_FragCoord.z shadow shader: %s", log);
	}
	return true;
}

bool RasterizerSceneWiiU::_ensure_program() {
	if (programs[0] && programs[1]) {
		return true;
	}
	if (program_failed) {
		return false;
	}
	for (int v = 0; v < 2; v++) {
		// The variant's #define goes after the #version line.
		String ps = scene_ps_glsl;
		if (v == 1) {
			const int eol = ps.find("\n");
			ps = ps.substr(0, eol + 1) + "#define USE_ALPHA_SCISSOR\n" + ps.substr(eol + 1);
		}
		const CharString ps_utf8 = ps.utf8();
		char log[1024];
		WiiUProgram *program = wiiu_gx2_program_compile_3d(scene_vs_glsl, ps_utf8.get_data(), log, sizeof(log));
		if (program == nullptr) {
			program_failed = true;
			WHBLogPrintf("[3d] couldn't compile the 3D shader (needs cafeglsl.rpl): %s", log);
			return false;
		}
		programs[v] = program;
		SceneLoc &loc = locs[v];
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
		loc.pbr = wiiu_gx2_program_uniform_offset(program, "pbr");
		loc.camera_position = wiiu_gx2_program_uniform_offset(program, "camera_position");
		for (int i = 0; i < 4; i++) {
			const String n = itos(i);
			loc.light_position[i] = wiiu_gx2_program_uniform_offset(program, ("light_position" + n).utf8().get_data());
			loc.light_color[i] = wiiu_gx2_program_uniform_offset(program, ("light_color" + n).utf8().get_data());
			loc.light_direction[i] = wiiu_gx2_program_uniform_offset(program, ("light_direction" + n).utf8().get_data());
			loc.light_extra[i] = wiiu_gx2_program_uniform_offset(program, ("light_extra" + n).utf8().get_data());
		}
		loc.albedo_texture = wiiu_gx2_program_sampler_location(program, "albedo_texture");
		loc.shadow_map = wiiu_gx2_program_sampler_location(program, "shadow_map");
		for (int i = 0; i < 4; i++) {
			const String n = itos(i);
			loc.shadow_matrix[i] = wiiu_gx2_program_uniform_offset(program, ("shadow_matrix" + n).utf8().get_data());
			loc.shadow_split[i] = wiiu_gx2_program_uniform_offset(program, ("shadow_split" + n).utf8().get_data());
		}
		loc.shadow_splits = wiiu_gx2_program_uniform_offset(program, "shadow_splits");
		loc.shadow_forward = wiiu_gx2_program_uniform_offset(program, "shadow_forward");
		loc.shadow_params = wiiu_gx2_program_uniform_offset(program, "shadow_params");
		loc.shadow_texel = wiiu_gx2_program_uniform_offset(program, "shadow_texel");
		loc.shadow_slope = wiiu_gx2_program_uniform_offset(program, "shadow_slope");
	}
	WHBLogPrint("[3d] 3D shader compiled");
	return true;
}

RasterizerSceneWiiU::~RasterizerSceneWiiU() {
	wiiu_gx2_program_free(programs[0]);
	wiiu_gx2_program_free(programs[1]);
	wiiu_gx2_program_free(shadow_program);
	wiiu_gx2_program_free(shadow_program_fragcoord);
	wiiu_gx2_render_target_free(shadow_target);
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
	Vector3 center; // World-space centre of the surface (light selection).
	float radius = 0.0f;
};

// An omni or spot light for the scene shader.
struct LocalLight {
	Vector3 position;
	float range = 1.0f;
	Color color;
	float decay = 1.0f;
	float specular = 1.0f;
	Vector3 direction;
	float cos_angle = -2.0f; // -2: omni.
	float spot_attenuation = 1.0f;
};

struct SortBackToFront {
	_FORCE_INLINE_ bool operator()(const DrawItem *p_a, const DrawItem *p_b) const { return p_a->depth > p_b->depth; }
};

// Opaque surfaces near to far, so the depth test skips shading hidden pixels.
struct SortFrontToBack {
	_FORCE_INLINE_ bool operator()(const DrawItem *p_a, const DrawItem *p_b) const { return p_a->depth < p_b->depth; }
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

	// Lights: one directional; omni and spot lights go to the objects they
	// reach (the four nearest, per draw).
	bool has_dir = false;
	Vector3 dir_direction;
	Color dir_color;
	float dir_specular = 0.0f;
	const LightStorageWiiU::Light *dir_light = nullptr;
	Transform3D dir_transform;
	LocalVector<LocalLight> local_lights;
	for (uint32_t i = 0; i < p_lights.size(); i++) {
		const LightStorageWiiU::LightInstance *li = ls->get_light_instance(p_lights[i]);
		const LightStorageWiiU::Light *light = li ? ls->get_light(li->light) : nullptr;
		if (light == nullptr || light->sky_mode == RSE::LIGHT_DIRECTIONAL_SKY_MODE_SKY_ONLY) {
			continue;
		}
		const float energy = light->param[RSE::LIGHT_PARAM_ENERGY] * (light->negative ? -1.0f : 1.0f);
		const Color c = light->color.srgb_to_linear() * energy;
		if (light->type == RSE::LIGHT_DIRECTIONAL) {
			if (!has_dir) {
				has_dir = true;
				dir_direction = li->transform.basis.get_column(2).normalized();
				dir_color = c;
				dir_specular = light->param[RSE::LIGHT_PARAM_SPECULAR];
				dir_light = light;
				dir_transform = li->transform;
			}
		} else if (light->type == RSE::LIGHT_OMNI || light->type == RSE::LIGHT_SPOT) {
			LocalLight l;
			l.position = li->transform.origin;
			l.range = MAX(light->param[RSE::LIGHT_PARAM_RANGE], 0.0001f);
			l.color = c;
			l.decay = light->param[RSE::LIGHT_PARAM_ATTENUATION];
			l.specular = light->param[RSE::LIGHT_PARAM_SPECULAR] * 2.0f; // As drivers/gles3.
			if (light->type == RSE::LIGHT_SPOT) {
				l.direction = -li->transform.basis.get_column(2).normalized();
				l.cos_angle = Math::cos(Math::deg_to_rad(light->param[RSE::LIGHT_PARAM_SPOT_ANGLE]));
				l.spot_attenuation = light->param[RSE::LIGHT_PARAM_SPOT_ATTENUATION];
			}
			local_lights.push_back(l);
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
			item.center = center;
			item.radius = surface.has_aabb ? p_transform.basis.xform(surface.aabb.size * 0.5f).length() : 0.0f;
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
	if (opaque.size() > 1) {
		SortArray<DrawItem *, SortFrontToBack> sorter;
		sorter.sort(opaque.ptr(), opaque.size());
	}

	// Directional shadow map: orthographic views from the sun, one per split
	// (see below; it used to be one covering a sphere in front of the camera,
	// radius from shadow_max_distance), drawn with the opaque surfaces the
	// camera sees.
	bool use_shadow = false;
	Projection shadow_split_matrix[4];
	Vector4 shadow_split_bias[4]; // Depth bias, normal offset, texel depth, depth range.
	float shadow_split_far[4] = {};
	int shadow_split_count = 1;
	float shadow_split_scale = 1.0f;
	float shadow_opacity = 1.0f;
	// Shadow debugging, set from GDScript (ProjectSettings.set_setting):
	// 0 off, 1 colour by depth against the map (see shadow_debug()), 2 shadow
	// pass without culling, 3 shadow pass with the material's own cull (not
	// reversed), 4 no depth or normal bias, 5 shadow pass without the depth
	// test, 6 the shadow map drawn in the top right corner, 7 shadow pass
	// writing gl_FragCoord.z, 8 stored and surface depth as sawtooth stripes.
	const int shadow_debug_mode = int(ProjectSettings::get_singleton()->get_setting("rendering/wiiu/shadow_debug", 0));
	if (dir_light && dir_light->shadow && !opaque.is_empty() && _ensure_shadow_program()) {
		if (shadow_target == nullptr) {
			shadow_target = wiiu_gx2_render_target_create_float(SHADOW_SIZE, SHADOW_SIZE);
		}
		if (shadow_target) {
			// Splits (cascades) as Godot's: from the camera's near plane to
			// shadow_max_distance, cut at the split offsets (fractions of it).
			// Each covers its slice of the view frustum with its own
			// orthographic view from the sun, in its own quarter of the map
			// (one split: the whole map). With one map over all of
			// shadow_max_distance (100 by default) a texel was 5 cm, and the
			// box near the camera got bands of wrong shadow on lit faces.
			const Projection &cam_projection = p_camera_data->main_projection;
			const float max_distance = MIN(CLAMP(dir_light->param[RSE::LIGHT_PARAM_SHADOW_MAX_DISTANCE], 1.0f, 1000.0f), cam_projection.get_z_far());
			const float z_near = cam_projection.get_z_near();
			switch (dir_light->shadow_mode) {
				case RSE::LIGHT_DIRECTIONAL_SHADOW_ORTHOGONAL:
					shadow_split_count = 1;
					shadow_split_far[0] = max_distance;
					break;
				case RSE::LIGHT_DIRECTIONAL_SHADOW_PARALLEL_2_SPLITS:
					shadow_split_count = 2;
					shadow_split_far[0] = max_distance * dir_light->param[RSE::LIGHT_PARAM_SHADOW_SPLIT_1_OFFSET];
					shadow_split_far[1] = max_distance;
					break;
				default:
					shadow_split_count = 4;
					shadow_split_far[0] = max_distance * dir_light->param[RSE::LIGHT_PARAM_SHADOW_SPLIT_1_OFFSET];
					shadow_split_far[1] = max_distance * dir_light->param[RSE::LIGHT_PARAM_SHADOW_SPLIT_2_OFFSET];
					shadow_split_far[2] = max_distance * dir_light->param[RSE::LIGHT_PARAM_SHADOW_SPLIT_3_OFFSET];
					shadow_split_far[3] = max_distance;
					break;
			}
			for (int i = shadow_split_count; i < 4; i++) {
				shadow_split_far[i] = shadow_split_far[shadow_split_count - 1];
			}
			shadow_split_scale = shadow_split_count == 1 ? 1.0f : 0.5f;
			const float split_size = SHADOW_SIZE * shadow_split_scale;
			// Half the view's width and height at distance d.
			const Vector2 near_half = cam_projection.get_viewport_half_extents();
			const bool cam_ortho = cam_projection.is_orthogonal();
			auto half_extents = [&](float d) { return cam_ortho ? near_half : near_half * (d / z_near); };
			const Basis basis = dir_transform.basis.orthonormalized();
			const Basis cam_basis = p_camera_data->main_transform.basis.orthonormalized();
			shadow_opacity = dir_light->param[RSE::LIGHT_PARAM_SHADOW_OPACITY];

			wiiu_gx2_bind_render_target(shadow_target);
			wiiu_gx2_clear(1.0f, 1.0f, 1.0f, 1.0f);
			wiiu_gx2_clear_depth();
			static Regs svs;
			for (int split = 0; split < shadow_split_count; split++) {
				// A sphere around the split's slice of the frustum (it doesn't
				// change size as the camera turns, so the texel snapping below
				// keeps the shadows still).
				const float d0 = split == 0 ? z_near : shadow_split_far[split - 1];
				const float d1 = shadow_split_far[split];
				const float mid = (d0 + d1) * 0.5f;
				float radius = 0.0f;
				for (int k = 0; k < 2; k++) {
					const float d = k == 0 ? d0 : d1;
					const Vector2 he = half_extents(d);
					radius = MAX(radius, Vector3(he.x, he.y, d - mid).length());
				}
				radius = Math::ceil(radius * 16.0f) / 16.0f;
				const Vector3 center = cam_pos + cam_forward * mid;
				// Light space: looks along the light (its -Z); texel-snapped so
				// the shadows don't shimmer as the camera moves.
				const float texel = (2.0f * radius) / split_size;
				Vector3 ls_center = basis.xform_inv(center);
				ls_center.x = Math::floor(ls_center.x / texel) * texel;
				ls_center.y = Math::floor(ls_center.y / texel) * texel;
				const float back = radius + 100.0f; // Casters behind the sphere, towards the light.
				const Transform3D light_xf(basis, basis.xform(ls_center) + basis.get_column(2) * back);
				Projection ortho;
				ortho.set_orthogonal(-radius, radius, -radius, radius, 0.0f, back + radius);
				shadow_split_matrix[split] = ortho * Projection(light_xf.affine_inverse());
				const float depth_range = back + radius;
				// As drivers/gles3: depth bias = shadow_bias / 100 of the depth
				// range (0..1 depth here); normal offset = normal_bias texels,
				// scaled in the shader by how far the surface turns from the light.
				Vector4 &b = shadow_split_bias[split];
				b.x = dir_light->param[RSE::LIGHT_PARAM_SHADOW_BIAS] / 100.0f;
				b.y = dir_light->param[RSE::LIGHT_PARAM_SHADOW_NORMAL_BIAS] * texel;
				b.z = texel / depth_range;
				b.w = depth_range;
				if (shadow_debug_mode == 4) {
					b.x = b.y = b.z = 0.0f;
				}
				// The split's quarter of the map: clip space squeezed into it,
				// and a scissor so nothing spills into the others.
				const float ox = (split & 1) ? 0.5f : 0.0f;
				const float oy = (split & 2) ? 0.5f : 0.0f;
				const float s = shadow_split_scale;
				const Projection atlas(Vector4(s, 0, 0, 0), Vector4(0, s, 0, 0), Vector4(0, 0, 1, 0),
						Vector4(s + 2.0f * ox - 1.0f, 1.0f - 2.0f * oy - s, 0, 1));
				const Projection atlas_view_projection = atlas * shadow_split_matrix[split];
				wiiu_gx2_set_scissor((int)(ox * SHADOW_SIZE), (int)(oy * SHADOW_SIZE), (int)split_size, (int)split_size);
				const Vector3 ls_split = basis.xform_inv(center);
				for (DrawItem *item : opaque) {
					// Skip what's nowhere near the split (seen from the sun).
					const Vector3 ls_item = basis.xform_inv(item->center);
					if (item->radius > 0.0f && Vector2(ls_item.x - ls_split.x, ls_item.y - ls_split.y).length() > radius + item->radius) {
						continue;
					}
					svs.count = 0;
					const bool fragcoord = shadow_debug_mode == 7 && shadow_program_fragcoord != nullptr;
					svs.put(fragcoord ? shadow_mvp_fragcoord : shadow_mvp, atlas_view_projection * Projection(item->transform));
					WiiUMeshDraw d = {};
					d.program = fragcoord ? shadow_program_fragcoord : shadow_program;
					d.mesh = item->mesh;
					d.vs_regs = svs.words;
					d.vs_reg_words = svs.count;
					d.blend = WIIU_BLEND_DISABLED;
					// Reversed cull, as drivers/gles3 does for directional shadows:
					// only faces turned away from the light go into the map, so lit
					// faces don't shadow themselves (two-sided materials stay so).
					const WiiUCull cull = item->material.shader.cull;
					d.cull = cull == WIIU_CULL_BACK ? WIIU_CULL_FRONT : (cull == WIIU_CULL_FRONT ? WIIU_CULL_BACK : WIIU_CULL_DISABLED);
					if (shadow_debug_mode == 2) {
						d.cull = WIIU_CULL_DISABLED;
					} else if (shadow_debug_mode == 3) {
						d.cull = cull;
					}
					d.depth_test = shadow_debug_mode != 5;
					d.depth_write = true;
					wiiu_gx2_draw_mesh(&d);
				}
			}
			for (int i = shadow_split_count; i < 4; i++) {
				shadow_split_matrix[i] = shadow_split_matrix[shadow_split_count - 1];
				shadow_split_bias[i] = shadow_split_bias[shadow_split_count - 1];
			}
			if (shadow_debug_mode == 9) {
				static int stats_frame = 0;
				if (++stats_frame % 120 == 1) {
					wiiu_gx2_debug_float_stats(shadow_target);
				}
			}
			static int logged_mode = -1;
			if (logged_mode != shadow_debug_mode) {
				logged_mode = shadow_debug_mode;
				WHBLogPrintf("[3d] shadow debug %d: %d splits, far %.2f %.2f %.2f %.2f, split 0 bias %.6f normal offset %.5f texel depth %.8f range %.2f",
						shadow_debug_mode, shadow_split_count, shadow_split_far[0], shadow_split_far[1], shadow_split_far[2], shadow_split_far[3],
						shadow_split_bias[0].x, shadow_split_bias[0].y, shadow_split_bias[0].z, shadow_split_bias[0].w);
			}
			// Back to the viewport's target (its colour and depth are kept).
			wiiu_gx2_bind_render_target(rt->gpu);
			wiiu_gx2_set_scissor(0, 0, rt->size.x, rt->size.y);
			use_shadow = true;
		}
	}

	static Regs vs, ps;
	auto draw = [&](const DrawItem &p_item, bool p_blended) {
		const MaterialStorageWiiU::SpatialMaterial &m = p_item.material;
		const int variant = m.shader.alpha_scissor ? 1 : 0;
		const SceneLoc &loc = locs[variant];
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
		// The four local lights nearest to the surface that can reach it.
		int light_count = 0;
		const LocalLight *chosen[4];
		float chosen_dist[4];
		for (const LocalLight &l : local_lights) {
			const float dist = MAX(0.0f, l.position.distance_to(p_item.center) - p_item.radius);
			if (dist > l.range) {
				continue;
			}
			int at = light_count < 4 ? light_count++ : 4;
			while (at > 0 && chosen_dist[at - 1] > dist) {
				if (at < 4) {
					chosen[at] = chosen[at - 1];
					chosen_dist[at] = chosen_dist[at - 1];
				}
				at--;
			}
			if (at < 4) {
				chosen[at] = &l;
				chosen_dist[at] = dist;
			}
		}
		ps.put(loc.params, m.shader.unshaded ? 1.0f : 0.0f, m.shader.alpha_scissor ? m.alpha_scissor_threshold : -1.0f,
				(float)light_count, m.shader.vertex_color ? 1.0f : 0.0f);
		ps.put(loc.flags, has_dir ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
		ps.put(loc.pbr, m.roughness, m.metallic, m.specular, 0.0f);
		ps.put(loc.camera_position, cam_pos.x, cam_pos.y, cam_pos.z, 1.0f);
		if (has_dir) {
			ps.put(loc.dir_direction, dir_direction.x, dir_direction.y, dir_direction.z, dir_specular);
			ps.put(loc.dir_color, dir_color);
		}
		for (int i = 0; i < light_count; i++) {
			const LocalLight &l = *chosen[i];
			ps.put(loc.light_position[i], l.position.x, l.position.y, l.position.z, 1.0f / l.range);
			ps.put(loc.light_color[i], l.color.r, l.color.g, l.color.b, l.decay);
			ps.put(loc.light_direction[i], l.direction.x, l.direction.y, l.direction.z, l.cos_angle);
			ps.put(loc.light_extra[i], l.spot_attenuation, l.specular, 0.0f, 0.0f);
		}
		ps.put(loc.shadow_params, use_shadow ? 1.0f : 0.0f, shadow_split_scale, 0.0f, shadow_opacity);
		WiiUProgramTexture tex[2];
		tex[0].texture = m.texture;
		tex[0].location = loc.albedo_texture;
		tex[0].filter = m.shader.filter;
		tex[0].repeat = m.shader.repeat;
		uint32_t tex_count = 1;
		if (use_shadow) {
			for (int i = 0; i < 4; i++) {
				ps.put(loc.shadow_matrix[i], shadow_split_matrix[i]);
				const Vector4 &b = shadow_split_bias[i];
				ps.put(loc.shadow_split[i], b.x, b.y, b.z, b.w);
			}
			ps.put(loc.shadow_splits, shadow_split_far[0], shadow_split_far[1], shadow_split_far[2], shadow_split_far[3]);
			ps.put(loc.shadow_forward, cam_forward.x, cam_forward.y, cam_forward.z, 0.0f);
			ps.put(loc.shadow_texel, 1.0f / SHADOW_SIZE, 1.0f / SHADOW_SIZE, (float)SHADOW_SIZE, (float)SHADOW_SIZE);
			ps.put(loc.shadow_slope, 0.0f, 0.0f, (shadow_debug_mode == 1 || shadow_debug_mode == 8) ? 1.0f : 0.0f, shadow_debug_mode == 8 ? 1.0f : 0.0f);
			tex[1].texture = wiiu_gx2_render_target_texture(shadow_target);
			tex[1].location = loc.shadow_map;
			tex[1].filter = 0; // Nearest: depth can't be filtered.
			tex[1].repeat = false;
			tex_count = 2;
		}

		WiiUMeshDraw d = {};
		d.program = programs[variant];
		d.mesh = p_item.mesh;
		d.vs_regs = vs.words;
		d.vs_reg_words = vs.count;
		d.ps_regs = ps.words;
		d.ps_reg_words = ps.count;
		d.textures = tex;
		d.texture_count = tex_count;
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
	if (shadow_debug_mode == 6 && use_shadow) {
		// The shadow map's red channel (0..1 depth, 1 = empty), 360 pixels.
		const float x0 = rt->size.x - 370.0f, y0 = 10.0f, x1 = x0 + 360.0f, y1 = y0 + 360.0f;
		const WiiUVertex v[6] = {
			{ x0, y0, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f },
			{ x1, y0, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f },
			{ x1, y1, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
			{ x0, y0, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f },
			{ x1, y1, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
			{ x0, y1, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
		};
		wiiu_gx2_draw(nullptr, WIIU_PRIM_TRIANGLES, wiiu_gx2_render_target_texture(shadow_target), 0, false, v, 6);
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
