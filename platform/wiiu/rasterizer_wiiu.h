#pragma once

// Cafe Godot's 2D renderer on GX2.
//
// Built on Godot's dummy renderer: 3D, materials, meshes, lights and particles
// stay dummies. Real parts:
// - TextureStorageWiiU: GX2 textures (uploaded as RGBA8), render targets
//   (GX2 colour buffers that can be sampled) and canvas textures.
// - RasterizerCanvasWiiU: walks the canvas item commands (rect, nine-patch,
//   polygon, primitive, transform, clip) and draws them as batched triangles.
// - RasterizerWiiU: fits the root render target onto the TV and the GamePad
//   and presents.
//
// Not supported yet: canvas shaders/materials (drawn with the default "mix"
// blend), 2D lights and shadows, MSDF/LCD text effects, meshes, multimeshes,
// particles, canvas groups, back-buffer copies, mipmaps.

#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid_owner.h"
#include "servers/rendering/dummy/rasterizer_canvas_dummy.h"
#include "servers/rendering/dummy/rasterizer_dummy.h"
#include "servers/rendering/dummy/storage/texture_storage.h"
#include "servers/rendering/dummy/storage/utilities.h"

#include "wiiu_gx2.h"

class TextureStorageWiiU : public RendererDummy::TextureStorage {
public:
	struct RenderTarget;

	struct Texture {
		WiiUTexture *gpu = nullptr;
		Ref<Image> image; // Kept for texture_2d_get.
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
	bool batch_linear = false;
	bool batch_repeat = false;
	LocalVector<WiiUVertex> batch;

	void _flush();
	void _begin(WiiUPrimitive p_primitive, WiiUTexture *p_texture, bool p_linear, bool p_repeat);
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
