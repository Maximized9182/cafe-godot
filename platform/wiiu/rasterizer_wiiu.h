#pragma once

// Cafe Godot's 2D renderer on GX2.
//
// Built on Godot's dummy renderer; 3D started as a dummy and now has a small
// forward renderer (RasterizerSceneWiiU). Real parts, one file each:
// - TextureStorageWiiU (texture_storage_wiiu.cpp): GX2 textures (uploaded as
//   RGBA8, with mipmaps), render targets (GX2 colour buffers that can be
//   sampled) and canvas textures. UtilitiesWiiU is there too.
// - MeshStorageWiiU (mesh_storage_wiiu.cpp): keeps 2D mesh surfaces (decoded
//   to points, colours, UVs) and multimesh instance data, so MeshInstance2D,
//   MultiMeshInstance2D and CPUParticles2D draw.
// - MaterialStorageWiiU (material_storage_wiiu.cpp): canvas_item blend modes,
//   and fragment-only canvas shaders compiled at runtime with CafeGLSL.
// - RasterizerCanvasWiiU (rasterizer_canvas_wiiu.cpp): walks the canvas item
//   commands (rect, nine-patch, polygon, primitive, mesh, transform, clip) and
//   draws them as batched triangles, MSDF text included.
// - RasterizerSceneWiiU and LightStorageWiiU (rasterizer_scene_wiiu.cpp):
//   meshes with depth, StandardMaterial3D basics and simple lights.
// - RasterizerWiiU (rasterizer_wiiu.cpp): fits the root render target onto the
//   TV and the GamePad and presents.
//
// Not supported yet: 2D lights and shadows, LCD text, canvas shaders with
// vertex() or the screen texture, skeletons and blend shapes, canvas groups,
// back-buffer copies.

#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid_owner.h"
#include "servers/rendering/dummy/rasterizer_canvas_dummy.h"
#include "servers/rendering/dummy/rasterizer_dummy.h"
#include "servers/rendering/dummy/rasterizer_scene_dummy.h"
#include "servers/rendering/dummy/storage/light_storage.h"
#include "servers/rendering/dummy/storage/material_storage.h"
#include "servers/rendering/dummy/storage/mesh_storage.h"
#include "servers/rendering/dummy/storage/texture_storage.h"
#include "servers/rendering/dummy/storage/utilities.h"
#include "servers/rendering/shader_compiler.h"
#include "servers/rendering/storage/render_scene_buffers.h"

#include "wiiu_gx2.h"

class TextureStorageWiiU : public RendererDummy::TextureStorage {
public:
	struct RenderTarget;

	struct Texture {
		WiiUTexture *gpu = nullptr;
		Ref<Image> image; // Kept for texture_2d_get, small textures only.
		int width = 0;
		int height = 0;
		Image::Format format = Image::FORMAT_RGBA8;
		RenderTarget *render_target = nullptr; // Set for a render target's texture.
		RID proxy_to; // Set for proxy textures.
		Size2i size_override;
		String path;
	};

	struct RenderTarget {
		WiiURenderTarget *gpu = nullptr;
		Size2i size;
		Point2i position;
		RID texture;
		bool clear_requested = false;
		Color clear_color;
		bool transparent = false;
		bool direct_to_screen = false;
		bool used = false;
	};

	struct CanvasTexture {
		RID diffuse;
		RSE::CanvasItemTextureFilter filter = RSE::CANVAS_ITEM_TEXTURE_FILTER_DEFAULT;
		RSE::CanvasItemTextureRepeat repeat = RSE::CANVAS_ITEM_TEXTURE_REPEAT_DEFAULT;
	};

private:
	static inline TextureStorageWiiU *wiiu_singleton = nullptr;

	// Textures up to this many pixels (512x512) keep a CPU copy for texture_2d_get.
	static constexpr int64_t KEEP_IMAGE_MAX_PIXELS = 512 * 512;

	mutable RID_PtrOwner<Texture> textures;
	mutable RID_PtrOwner<RenderTarget> render_targets;
	mutable RID_PtrOwner<CanvasTexture> canvas_textures;

	void _upload(Texture *p_texture, const Ref<Image> &p_image);
	void _release_gpu(Texture *p_texture);

public:
	static TextureStorageWiiU *get_wiiu_singleton() { return wiiu_singleton; }

	// What the canvas renderer needs to draw with a texture or canvas texture.
	struct Resolved {
		WiiUTexture *texture = nullptr; // nullptr: draw untextured (white).
		Size2 size = Size2(1, 1);
		RSE::CanvasItemTextureFilter filter = RSE::CANVAS_ITEM_TEXTURE_FILTER_DEFAULT;
		RSE::CanvasItemTextureRepeat repeat = RSE::CANVAS_ITEM_TEXTURE_REPEAT_DEFAULT;
	};
	Resolved resolve(RID p_texture) const;

	RenderTarget *get_render_target(RID p_rid) const { return render_targets.get_or_null(p_rid); }
	bool owns_wiiu_texture(RID p_rid) const { return textures.owns(p_rid); }
	bool owns_canvas_texture(RID p_rid) const { return canvas_textures.owns(p_rid); }

	/* Canvas textures */
	RID canvas_texture_allocate() override;
	void canvas_texture_initialize(RID p_rid) override;
	void canvas_texture_free(RID p_rid) override;
	void canvas_texture_set_channel(RID p_canvas_texture, RSE::CanvasTextureChannel p_channel, RID p_texture) override;
	void canvas_texture_set_texture_filter(RID p_item, RSE::CanvasItemTextureFilter p_filter) override;
	void canvas_texture_set_texture_repeat(RID p_item, RSE::CanvasItemTextureRepeat p_repeat) override;

	/* Textures */
	RID texture_allocate() override;
	void texture_free(RID p_rid) override;
	void texture_2d_initialize(RID p_texture, const Ref<Image> &p_image) override;
	void texture_2d_update(RID p_texture, const Ref<Image> &p_image, int p_layer = 0) override;
	void texture_2d_placeholder_initialize(RID p_texture) override;
	void texture_proxy_initialize(RID p_texture, RID p_base) override;
	void texture_proxy_update(RID p_proxy, RID p_base) override;
	Ref<Image> texture_2d_get(RID p_texture) const override;
	void texture_replace(RID p_texture, RID p_by_texture) override;
	void texture_set_size_override(RID p_texture, int p_width, int p_height) override;
	void texture_set_path(RID p_texture, const String &p_path) override;
	String texture_get_path(RID p_texture) const override;
	Image::Format texture_get_format(RID p_texture) const override;
	Size2 texture_size_with_proxy(RID p_proxy) override;

	/* Render targets */
	RID render_target_create() override;
	void render_target_free(RID p_rid) override;
	void render_target_set_position(RID p_render_target, int p_x, int p_y) override;
	Point2i render_target_get_position(RID p_render_target) const override;
	void render_target_set_size(RID p_render_target, int p_width, int p_height, uint32_t p_view_count) override;
	Size2i render_target_get_size(RID p_render_target) const override;
	void render_target_set_transparent(RID p_render_target, bool p_is_transparent) override;
	bool render_target_get_transparent(RID p_render_target) const override;
	void render_target_set_direct_to_screen(RID p_render_target, bool p_direct_to_screen) override;
	bool render_target_get_direct_to_screen(RID p_render_target) const override;
	bool render_target_was_used(RID p_render_target) const override;
	void render_target_set_as_unused(RID p_render_target) override;
	void render_target_request_clear(RID p_render_target, const Color &p_clear_color) override;
	bool render_target_is_clear_requested(RID p_render_target) override;
	Color render_target_get_clear_request_color(RID p_render_target) override;
	void render_target_disable_clear_request(RID p_render_target) override;
	void render_target_do_clear_request(RID p_render_target) override;
	RID render_target_get_texture(RID p_render_target) override;

	TextureStorageWiiU();
	~TextureStorageWiiU();
};

// Mesh and multimesh data the canvas renderer draws. The dummy storage keeps
// the RIDs and raw surface data; this adds decoded 2D surfaces and multimesh
// instances on the side, keyed by the same RIDs.
class MeshStorageWiiU : public RendererDummy::MeshStorage {
public:
	struct Surface {
		RSE::PrimitiveType primitive = RSE::PRIMITIVE_TRIANGLES;
		LocalVector<Vector2> points;
		LocalVector<Color> colors; // One per point, or none (white).
		LocalVector<Vector2> uvs; // One per point, or none.
		LocalVector<int> indices; // Empty: points in order.
		Rect2 rect; // Bounds of the points.
		// Surfaces with 3D vertices (triangles or lines) are also kept in GPU
		// memory for the 3D renderer, with their 3D bounds. Owned here: freed
		// with the surface.
		WiiUMesh *gpu = nullptr;
		AABB aabb;
		bool has_aabb = false;
	};

	struct MultiMesh {
		RID mesh;
		int instances = 0;
		RSE::MultimeshTransformFormat format = RSE::MULTIMESH_TRANSFORM_2D;
		bool use_colors = false;
		bool use_custom_data = false;
		int visible_instances = -1; // -1: all.
		int stride = 0; // Floats per instance.
		int color_offset = 0; // Float offset of the colour in an instance.
		LocalVector<float> buffer;
		AABB custom_aabb;
	};

private:
	static inline MeshStorageWiiU *wiiu_singleton = nullptr;

	HashMap<RID, LocalVector<Surface>> mesh_surfaces;
	HashMap<RID, AABB> mesh_custom_aabbs;
	HashMap<RID, MultiMesh> multimeshes;

	void _decode_surface(RID p_mesh, const RenderingServerTypes::SurfaceData &p_surface);
	static void _free_gpu(LocalVector<Surface> &p_surfaces);

public:
	static MeshStorageWiiU *get_wiiu_singleton() { return wiiu_singleton; }

	// nullptr if the mesh has no surfaces.
	const LocalVector<Surface> *get_surfaces(RID p_mesh) const { return mesh_surfaces.getptr(p_mesh); }
	const MultiMesh *get_multimesh(RID p_multimesh) const { return multimeshes.getptr(p_multimesh); }
	// An instance's 2D transform (the x/y part for 3D formats) and colour.
	static Transform2D instance_transform(const MultiMesh &p_mm, int p_index);
	static Color instance_color(const MultiMesh &p_mm, int p_index);

	/* Meshes */
	void mesh_free(RID p_rid) override;
	void mesh_add_surface(RID p_mesh, const RenderingServerTypes::SurfaceData &p_surface) override;
	void mesh_surface_remove(RID p_mesh, int p_surface) override;
	void mesh_clear(RID p_mesh) override;
	void mesh_set_custom_aabb(RID p_mesh, const AABB &p_aabb) override;
	AABB mesh_get_custom_aabb(RID p_mesh) const override;
	AABB mesh_get_aabb(RID p_mesh, RID p_skeleton = RID()) override;

	/* Multimeshes */
	void _multimesh_initialize(RID p_rid) override;
	void _multimesh_free(RID p_rid) override;
	void _multimesh_allocate_data(RID p_multimesh, int p_instances, RSE::MultimeshTransformFormat p_transform_format, bool p_use_colors = false, bool p_use_custom_data = false, bool p_use_indirect = false) override;
	int _multimesh_get_instance_count(RID p_multimesh) const override;
	void _multimesh_set_mesh(RID p_multimesh, RID p_mesh) override;
	RID _multimesh_get_mesh(RID p_multimesh) const override;
	void _multimesh_instance_set_transform(RID p_multimesh, int p_index, const Transform3D &p_transform) override;
	void _multimesh_instance_set_transform_2d(RID p_multimesh, int p_index, const Transform2D &p_transform) override;
	void _multimesh_instance_set_color(RID p_multimesh, int p_index, const Color &p_color) override;
	void _multimesh_instance_set_custom_data(RID p_multimesh, int p_index, const Color &p_color) override;
	Transform3D _multimesh_instance_get_transform(RID p_multimesh, int p_index) const override;
	Transform2D _multimesh_instance_get_transform_2d(RID p_multimesh, int p_index) const override;
	Color _multimesh_instance_get_color(RID p_multimesh, int p_index) const override;
	Color _multimesh_instance_get_custom_data(RID p_multimesh, int p_index) const override;
	void _multimesh_set_buffer(RID p_multimesh, const Vector<float> &p_buffer) override;
	Vector<float> _multimesh_get_buffer(RID p_multimesh) const override;
	void _multimesh_set_visible_instances(RID p_multimesh, int p_visible) override;
	int _multimesh_get_visible_instances(RID p_multimesh) const override;
	void _multimesh_set_custom_aabb(RID p_multimesh, const AABB &p_aabb) override;
	AABB _multimesh_get_custom_aabb(RID p_multimesh) const override;
	AABB _multimesh_get_aabb(RID p_multimesh) override;

	MeshStorageWiiU();
	~MeshStorageWiiU();
};

// canvas_item shaders: the blend mode (the render_mode line, also what
// CanvasItemMaterial generates) is always applied. A shader with only a
// fragment() (no vertex() code, no screen texture, SDF, global/instance
// uniforms or uniform arrays) is also translated to GLSL with Godot's
// ShaderCompiler and compiled with CafeGLSL (cafeglsl.rpl) at runtime; its
// uniforms go into pixel uniform registers. Anything else draws as before.
class MaterialStorageWiiU : public RendererDummy::MaterialStorage {
	static inline MaterialStorageWiiU *wiiu_singleton = nullptr;
	HashMap<RID, WiiUBlend> shader_blend;
	HashMap<RID, RID> material_shader;
	HashMap<RID, String> shader_paths;

	// A compiled canvas_item shader.
	struct CanvasProgram {
		WiiUProgram *program = nullptr;
		String source; // Key into program_cache.
		uint64_t serial = 0; // Tells MaterialData a shader was recompiled.
		struct Param {
			StringName name;
			ShaderLanguage::DataType type = ShaderLanguage::TYPE_VOID;
			Variant default_value;
			int32_t location = -1; // Register word offset (uniforms) or sampler location (textures).
			int32_t vs_location = -1; // Vertex-stage register word offset (uniforms used in vertex()).
			ShaderLanguage::ShaderNode::Uniform::Hint hint = ShaderLanguage::ShaderNode::Uniform::HINT_NONE;
			ShaderLanguage::TextureFilter filter = ShaderLanguage::FILTER_DEFAULT;
			ShaderLanguage::TextureRepeat repeat = ShaderLanguage::REPEAT_DEFAULT;
		};
		LocalVector<Param> uniforms;
		LocalVector<Param> textures;
		uint32_t reg_words = 0;
		uint32_t vs_reg_words = 0;
		bool uses_screen_texture = false; // hint_screen_texture (back-buffer copy).
		bool local_vertices = false; // Has vertex() code: the GPU transforms the item.
		bool sky = false; // A sky shader (drawn by the 3D renderer, WiiUVertex3D layout).
	};
	HashMap<RID, CanvasProgram *> shader_programs;
	// Shaders with the same GLSL share one GX2 program.
	struct CachedProgram {
		WiiUProgram *program = nullptr;
		int refs = 0;
	};
	HashMap<String, CachedProgram> program_cache;
	uint64_t program_serial = 0;

	struct MaterialData {
		HashMap<StringName, Variant> params;
		LocalVector<uint32_t> regs; // Packed uniforms of the current shader.
		LocalVector<uint32_t> vs_regs; // The same for its vertex stage.
		uint64_t built_serial = 0; // 0: regs need packing.
		LocalVector<WiiUProgramTexture> textures;
	};
	HashMap<RID, MaterialData> material_data;

	ShaderCompiler canvas_compiler;
	bool canvas_compiler_ready = false;
	ShaderCompiler sky_compiler;
	bool sky_compiler_ready = false;

	void _init_canvas_compiler();
	void _free_canvas_program(RID p_shader);
	CanvasProgram *_compile_canvas_program(RID p_shader, const String &p_code);
	// Sky shaders: Godot's sky ShaderCompiler (drivers/gles3's renames) and a
	// full-screen template; see rasterizer_scene_wiiu.cpp for the built-ins.
	void _init_sky_compiler();
	CanvasProgram *_compile_sky_program(RID p_shader, const String &p_code);
	// The compiled program (cached by its GLSL) and its material parameters.
	CanvasProgram *_make_program(const String &p_path, const String &p_vs, const String &p_ps, bool p_3d,
			const HashMap<StringName, ShaderLanguage::ShaderNode::Uniform> &p_uniforms, bool p_local_vertices,
			bool p_uses_screen_texture);

public:
	// What the 3D renderer (rasterizer_scene_wiiu.cpp) follows of a spatial
	// shader: render modes, and the features StandardMaterial3D's generated
	// code shows (transparency, alpha scissor, vertex colour, the albedo
	// texture's filter and repeat). The shader code itself isn't compiled:
	// every spatial material draws with the renderer's built-in shading.
	struct SpatialShader {
		bool unshaded = false;
		bool vertex_color = false; // vertex_color_use_as_albedo.
		bool transparent = false; // Alpha blending (drawn after the opaque ones, back to front).
		bool alpha_scissor = false; // Alpha scissor or hash: discard below the threshold.
		WiiUBlend blend = WIIU_BLEND_MIX;
		WiiUCull cull = WIIU_CULL_BACK;
		bool depth_test = true;
		int depth_draw = 1; // 0 never, 1 opaque only (default), 2 always.
		int filter = WIIU_FILTER_LINEAR | WIIU_FILTER_MIPMAPS; // texture_albedo's.
		bool repeat = true;
	};
	struct SpatialMaterial {
		SpatialShader shader;
		Color albedo = Color(1, 1, 1, 1);
		WiiUTexture *texture = nullptr; // texture_albedo (nullptr: white).
		Vector4 uv1 = Vector4(1, 1, 0, 0); // uv1_scale.xy, uv1_offset.xy.
		float alpha_scissor_threshold = 0.5f;
		float roughness = 1.0f;
		float metallic = 0.0f;
		float specular = 0.5f;
	};

private:
	HashMap<RID, SpatialShader> spatial_shaders;

public:
	// False if the material has no spatial shader.
	bool get_spatial_material(RID p_material, SpatialMaterial &r_material) const;

	static MaterialStorageWiiU *get_wiiu_singleton() { return wiiu_singleton; }
	WiiUBlend get_material_blend(RID p_material) const;
	// Fills r_state with the material's compiled shader for a draw: program,
	// registers (both stages), extra textures (valid until the material
	// changes) and local_vertices. Returns false (r_state untouched): draw normally.
	bool get_material_program(RID p_material, int p_filter, bool p_repeat, WiiUDrawState &r_state);
	// True if the material's shader has vertex() code (draw its items with
	// local vertices and a model matrix).
	bool material_uses_local_vertices(RID p_material) const;
	bool material_has_program(RID p_material) const;
	// True if the material's compiled shader reads the screen texture.
	bool material_uses_screen_texture(RID p_material) const;
	// True if the material has a compiled sky shader (get_material_program
	// then gives its program, registers and textures).
	bool material_is_sky(RID p_material) const;

	void shader_free(RID p_rid) override;
	void shader_set_code(RID p_shader, const String &p_code) override;
	void shader_set_path_hint(RID p_shader, const String &p_path) override;
	void material_free(RID p_rid) override;
	void material_set_shader(RID p_material, RID p_shader) override;
	void material_set_param(RID p_material, const StringName &p_param, const Variant &p_value) override;
	Variant material_get_param(RID p_material, const StringName &p_param) const override;

	MaterialStorageWiiU();
	~MaterialStorageWiiU();
};

class UtilitiesWiiU : public RendererDummy::Utilities {
public:
	bool free(RID p_rid) override;
	// Lights are LightStorageWiiU's; the rest as in the dummy.
	RSE::InstanceType get_base_type(RID p_rid) const override;
	void base_update_dependency(RID p_base, DependencyTracker *p_instance) override;
};

// Lights for the 3D renderer: their type, colour, parameters and instance
// transforms (no shadows, projectors or baking).
class LightStorageWiiU : public RendererDummy::LightStorage {
public:
	struct Light {
		RSE::LightType type = RSE::LIGHT_OMNI;
		Color color = Color(1, 1, 1);
		float param[RSE::LIGHT_PARAM_MAX] = {};
		bool negative = false;
		uint32_t cull_mask = 0xFFFFFFFF;
		RSE::LightDirectionalSkyMode sky_mode = RSE::LIGHT_DIRECTIONAL_SKY_MODE_LIGHT_AND_SKY;
		// Shadows on. light_has_shadow stays false so RendererSceneCull
		// doesn't set up its shadow atlases; the 3D renderer reads this.
		bool shadow = false;
		Dependency dependency;
	};
	struct LightInstance {
		RID light;
		Transform3D transform;
	};

private:
	static inline LightStorageWiiU *wiiu_singleton = nullptr;
	mutable RID_Owner<Light, true> light_owner;
	mutable RID_Owner<LightInstance> light_instance_owner;
	RID _light_allocate(RSE::LightType p_type);

public:
	static LightStorageWiiU *get_wiiu_singleton() { return wiiu_singleton; }
	bool owns_light(RID p_rid) const { return light_owner.owns(p_rid); }
	Light *get_light(RID p_rid) const { return light_owner.get_or_null(p_rid); }
	LightInstance *get_light_instance(RID p_rid) const { return light_instance_owner.get_or_null(p_rid); }

	RID directional_light_allocate() override { return _light_allocate(RSE::LIGHT_DIRECTIONAL); }
	void directional_light_initialize(RID p_rid) override {}
	RID omni_light_allocate() override { return _light_allocate(RSE::LIGHT_OMNI); }
	void omni_light_initialize(RID p_rid) override {}
	RID spot_light_allocate() override { return _light_allocate(RSE::LIGHT_SPOT); }
	void spot_light_initialize(RID p_rid) override {}
	void light_free(RID p_rid) override;

	void light_set_color(RID p_light, const Color &p_color) override;
	void light_set_param(RID p_light, RSE::LightParam p_param, float p_value) override;
	void light_set_shadow(RID p_light, bool p_enabled) override;
	void light_set_negative(RID p_light, bool p_enable) override;
	void light_set_cull_mask(RID p_light, uint32_t p_mask) override;
	void light_directional_set_sky_mode(RID p_light, RSE::LightDirectionalSkyMode p_mode) override;
	RSE::LightDirectionalSkyMode light_directional_get_sky_mode(RID p_light) const override;

	RSE::LightType light_get_type(RID p_light) const override;
	AABB light_get_aabb(RID p_light) const override;
	float light_get_param(RID p_light, RSE::LightParam p_param) override;
	Color light_get_color(RID p_light) override;
	uint32_t light_get_cull_mask(RID p_light) const override;

	RID light_instance_create(RID p_light) override;
	void light_instance_free(RID p_light_instance) override;
	void light_instance_set_transform(RID p_light_instance, const Transform3D &p_transform) override;

	LightStorageWiiU();
	~LightStorageWiiU();
};

// What render_scene draws into: the viewport's render target.
class RenderSceneBuffersWiiU : public RenderSceneBuffers {
	GDCLASS(RenderSceneBuffersWiiU, RenderSceneBuffers);

public:
	RID render_target;
	Size2i size;

	void configure(const RenderSceneBuffersConfiguration *p_config) override {
		render_target = p_config->get_render_target();
		size = p_config->get_internal_size();
	}
	void set_fsr_sharpness(float p_fsr_sharpness) override {}
	void set_texture_mipmap_bias(float p_texture_mipmap_bias) override {}
	void set_anisotropic_filtering_level(RSE::ViewportAnisotropicFiltering p_anisotropic_filtering_level) override {}
	void set_use_debanding(bool p_use_debanding) override {}
};

// Cafe Godot's 3D renderer (rasterizer_scene_wiiu.cpp): one forward pass of
// meshes and 3D multimeshes into the viewport's render target, with a depth
// buffer, before its 2D canvas. Every material draws with one built-in
// shader: StandardMaterial3D's albedo colour and texture, UV1 scale/offset,
// vertex colour, unshaded, transparency, alpha scissor, cull and depth
// modes; Lambert diffuse and GGX specular (roughness, metallic, specular)
// from the environment's ambient light, one directional light and the four
// omni/spot lights nearest to each object; the first directional light's
// shadow (one 2048x2048 map, 2x2 PCF). No positional shadows, fog, tone
// mapping, skeletons, blend shapes or spatial shader code yet. Sky shaders
// (ProceduralSkyMaterial, PanoramaSkyMaterial, PhysicalSkyMaterial, custom
// ones) are compiled and drawn behind the scene; the ambient light from a sky
// is a top/bottom blend read from ProceduralSkyMaterial's colours.
class RasterizerSceneWiiU : public RasterizerSceneDummy {
public:
	class GeometryInstanceWiiU : public GeometryInstanceDummy {
	public:
		RID base;
		RSE::InstanceType base_type = RSE::INSTANCE_NONE;
		Transform3D transform;
		AABB aabb;
		AABB transformed_aabb;
		RID material_override;
		Vector<RID> surface_materials;
		uint32_t layer_mask = 1;
		float transparency = 0.0f;

		void set_material_override(RID p_override) override { material_override = p_override; }
		void set_surface_materials(const Vector<RID> &p_materials) override { surface_materials = p_materials; }
		void set_transform(const Transform3D &p_transform, const AABB &p_aabb, const AABB &p_transformed_aabb) override {
			transform = p_transform;
			aabb = p_aabb;
			transformed_aabb = p_transformed_aabb;
		}
		void set_layer_mask(uint32_t p_layer_mask) override { layer_mask = p_layer_mask; }
		void set_transparency(float p_transparency) override { transparency = p_transparency; }
		Transform3D get_transform() override { return transform; }
		AABB get_aabb() override { return aabb; }
	};

private:
	PagedAllocator<GeometryInstanceWiiU> wiiu_geometry_alloc;

	// Skies: only the material is used (drawn every frame, no radiance cubemap).
	struct Sky {
		RID material;
	};
	mutable RID_Owner<Sky, true> sky_owner;
	WiiUMesh *sky_triangle = nullptr; // Full-screen triangle at the far plane.
	// Draws the environment's sky material behind everything drawn so far.
	void _draw_sky(RID p_environment, const CameraData *p_camera, const PagedArray<RID> &p_lights);

	// The built-in shader and where its uniforms are (-1: optimized out).
	// Two variants: [1] has the alpha scissor discard, [0] doesn't (a discard
	// anywhere in a pixel shader turns off the GPU's early depth test, so
	// opaque surfaces would all be shaded before the depth test).
	struct SceneLoc {
		int32_t mvp, model, normal_matrix, uv1; // Vertex stage.
		int32_t albedo, ambient, ambient_ground, params, flags, dir_direction, dir_color, pbr, camera_position; // Pixel stage.
		int32_t light_position[4], light_color[4], light_direction[4], light_extra[4];
		int32_t albedo_texture; // Sampler location.
		int32_t shadow_map, shadow_matrix, shadow_params, shadow_texel;
	};
	WiiUProgram *programs[2] = {};
	SceneLoc locs[2] = {};
	bool program_failed = false;
	bool _ensure_program();

	// Directional shadow map (packed depth in an RGBA8 render target).
	static constexpr int SHADOW_SIZE = 2048;
	WiiUProgram *shadow_program = nullptr;
	bool shadow_program_failed = false;
	int32_t shadow_mvp = -1;
	WiiURenderTarget *shadow_target = nullptr;
	bool _ensure_shadow_program();

public:
	RenderGeometryInstance *geometry_instance_create(RID p_base) override;
	void geometry_instance_free(RenderGeometryInstance *p_geometry_instance) override;
	Ref<RenderSceneBuffers> render_buffers_create() override;

	RID sky_allocate() override;
	void sky_initialize(RID p_rid) override {}
	void sky_set_material(RID p_sky, RID p_material) override;
	bool free(RID p_rid) override;

	void render_scene(const Ref<RenderSceneBuffers> &p_render_buffers, const CameraData *p_camera_data, const CameraData *p_prev_camera_data, const PagedArray<RenderGeometryInstance *> &p_instances, const PagedArray<RID> &p_lights, const PagedArray<RID> &p_reflection_probes, const PagedArray<RID> &p_voxel_gi_instances, const PagedArray<RID> &p_decals, const PagedArray<RID> &p_lightmaps, const PagedArray<RID> &p_fog_volumes, RID p_environment, RID p_camera_attributes, RID p_compositor, RID p_shadow_atlas, RID p_occluder_debug_tex, RID p_reflection_atlas, RID p_reflection_probe, int p_reflection_probe_pass, float p_screen_mesh_lod_threshold, const RenderShadowData *p_render_shadows, int p_render_shadow_count, const RenderSDFGIData *p_render_sdfgi_regions, int p_render_sdfgi_region_count, float p_window_output_max_value, const RenderSDFGIUpdateData *p_sdfgi_update_data = nullptr, RenderingServerTypes::RenderInfo *r_render_info = nullptr) override;

	~RasterizerSceneWiiU();
};

class RasterizerCanvasWiiU : public RasterizerCanvasDummy {
	struct PolygonData {
		LocalVector<int> indices;
		LocalVector<Vector2> points;
		LocalVector<Color> colors; // One per point, one for all, or none (white).
		LocalVector<Vector2> uvs; // One per point, or none.
	};

	HashMap<PolygonID, PolygonData> polygons;
	PolygonID polygon_id_counter = 1;

	// The batch being collected: one draw per texture/filter/primitive change.
	WiiUPrimitive batch_primitive = WIIU_PRIM_TRIANGLES;
	WiiUTexture *batch_texture = nullptr;
	int batch_linear = 0; // WIIU_FILTER_* bits.
	bool batch_repeat = false;
	WiiUBlend batch_blend = WIIU_BLEND_MIX;
	// MSDF text (CANVAS_RECT_MSDF): px range (0: off) and outline of the batch,
	// and of the next _begin.
	float batch_msdf_range = 0.0f, batch_msdf_outline = 0.0f, batch_msdf_px_size = 1.0f;
	float msdf_range = 0.0f, msdf_outline = 0.0f, msdf_px_size = 1.0f;
	WiiUBlend blend = WIIU_BLEND_MIX; // For the next _begin: the current item's material.
	// The material whose compiled shader draws the batch (invalid: none), and
	// the one for the next _begin.
	RID batch_material;
	RID shader_material;
	// Items whose shader has vertex() code are batched alone, with local
	// vertices (the GPU applies the item transform). local_item: the current
	// item's number among those (0: not one), batch_local_item: the batch's.
	uint64_t local_item = 0;
	uint64_t local_item_counter = 0;
	uint64_t batch_local_item = 0;
	Transform2D local_xform; // The current item's transform (local to pixels).
	Transform2D local_inverse; // Render-target pixels back to the item's local space.
	Transform2D batch_local_xform;
	// The render target's back buffer once copied in this canvas_render_items
	// call (screen texture of canvas shaders); nullptr before.
	WiiUTexture *screen_texture = nullptr;
	LocalVector<WiiUVertex> batch;

	void _flush();
	void _begin(WiiUPrimitive p_primitive, WiiUTexture *p_texture, int p_filter, bool p_repeat);
	void _vertex(const Vector2 &p_pos, const Vector2 &p_uv, const Color &p_color);
	void _quad(const Transform2D &p_xform, const Vector2 p_pos[4], const Vector2 p_uv[4], const Color &p_color);

public:
	PolygonID request_polygon(const Vector<int> &p_indices, const Vector<Point2> &p_points, const Vector<Color> &p_colors, const Vector<Point2> &p_uvs = Vector<Point2>(), const Vector<int> &p_bones = Vector<int>(), const Vector<float> &p_weights = Vector<float>(), int p_count = -1) override;
	void free_polygon(PolygonID p_polygon) override;

	void canvas_render_items(RID p_to_render_target, Item *p_item_list, const Color &p_modulate, Light *p_light_list, Light *p_directional_list, const Transform2D &p_canvas_transform, RSE::CanvasItemTextureFilter p_default_filter, RSE::CanvasItemTextureRepeat p_default_repeat, bool p_snap_2d_vertices_to_pixel, bool &r_sdf_used, RenderingServerTypes::RenderInfo *r_render_info = nullptr) override;

	// Draws a textured rectangle into the bound target (used for the blit).
	void draw_texture_rect(WiiUTexture *p_texture, const Rect2 &p_dst, const Rect2 &p_src_uv, bool p_linear);
};

// Maps a point on the GamePad screen (854x480 pixels) to Godot's window
// coordinates, undoing the per-screen fit of the last blit. Returns false if
// the point is outside the game's picture (in the black bars).
bool rasterizer_wiiu_drc_to_window(const Vector2 &p_drc, Vector2 &r_window);

class RasterizerWiiU : public RasterizerDummy {
	bool blitted = false;

	void _blit_to_screen(WiiUScreen p_screen, const RenderingServerTypes::BlitToScreen *p_render_targets, int p_amount);

public:
	void begin_frame(double p_frame_step) override;
	void blit_render_targets_to_screen(int p_screen, const RenderingServerTypes::BlitToScreen *p_render_targets, int p_amount) override;
	void end_frame(bool p_present) override;

	static RendererCompositor *_create_current() {
		return memnew(RasterizerWiiU);
	}
	static void make_current() {
		_create_func = _create_current;
		low_end = true;
	}

	RasterizerWiiU();
};
