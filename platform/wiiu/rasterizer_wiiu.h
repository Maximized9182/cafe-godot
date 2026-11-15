#pragma once

// Cafe Godot's 2D renderer on GX2.
//
// Built on Godot's dummy renderer: 3D stays a dummy. Real parts, one file each:
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
#include "servers/rendering/dummy/storage/material_storage.h"
#include "servers/rendering/dummy/storage/mesh_storage.h"
#include "servers/rendering/dummy/storage/texture_storage.h"
#include "servers/rendering/dummy/storage/utilities.h"
#include "servers/rendering/shader_compiler.h"

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
			ShaderLanguage::ShaderNode::Uniform::Hint hint = ShaderLanguage::ShaderNode::Uniform::HINT_NONE;
			ShaderLanguage::TextureFilter filter = ShaderLanguage::FILTER_DEFAULT;
			ShaderLanguage::TextureRepeat repeat = ShaderLanguage::REPEAT_DEFAULT;
		};
		LocalVector<Param> uniforms;
		LocalVector<Param> textures;
		uint32_t reg_words = 0;
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
		uint64_t built_serial = 0; // 0: regs need packing.
		LocalVector<WiiUProgramTexture> textures;
	};
	HashMap<RID, MaterialData> material_data;

	ShaderCompiler canvas_compiler;
	bool canvas_compiler_ready = false;

	void _init_canvas_compiler();
	void _free_canvas_program(RID p_shader);
	CanvasProgram *_compile_canvas_program(RID p_shader, const String &p_code);

public:
	static MaterialStorageWiiU *get_wiiu_singleton() { return wiiu_singleton; }
	WiiUBlend get_material_blend(RID p_material) const;
	// The material's compiled shader for a draw, with its registers and extra
	// textures (valid until the material changes); nullptr: draw normally.
	WiiUProgram *get_material_program(RID p_material, int p_filter, bool p_repeat, const uint32_t *&r_regs,
			uint32_t &r_reg_words, const WiiUProgramTexture *&r_textures, uint32_t &r_texture_count);
	bool material_has_program(RID p_material) const;

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
