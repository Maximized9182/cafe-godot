#include "rasterizer_wiiu.h"

#include "core/io/image.h"
#include "core/math/math_funcs.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_default.h"

// wiiu_stats.cpp: frame timing marks for the stats overlay.
extern "C" void wiiu_stats_mark(int p_mark);

// ===========================================================================
// TextureStorageWiiU
// ===========================================================================

TextureStorageWiiU::TextureStorageWiiU() {
	wiiu_singleton = this;
}

TextureStorageWiiU::~TextureStorageWiiU() {
	wiiu_singleton = nullptr;
}

void TextureStorageWiiU::_release_gpu(Texture *p_texture) {
	if (p_texture->gpu) {
		wiiu_gx2_texture_free(p_texture->gpu);
		p_texture->gpu = nullptr;
	}
}

// Converts to RGBA8 (keeping the image's mipmaps, if it has any) and
// uploads, recreating the GX2 texture if the size or mip count changed.
void TextureStorageWiiU::_upload(Texture *p_texture, const Ref<Image> &p_image) {
	ERR_FAIL_COND(p_image.is_null() || p_image->is_empty());
	// Only copy when the image needs converting: big atlases are tens of MB.
	Ref<Image> img = p_image;
	if (img->is_compressed() || img->get_format() != Image::FORMAT_RGBA8) {
		img = p_image->duplicate();
		if (img->is_compressed()) {
			img->decompress();
		}
		if (img->get_format() != Image::FORMAT_RGBA8) {
			img->convert(Image::FORMAT_RGBA8);
		}
	}
	const int w = img->get_width();
	const int h = img->get_height();
	Vector<uint8_t> data = img->get_data();
	// Mip levels as pointers into data (Godot's chain halves down to 1x1,
	// like GX2's). Used when a canvas item asks for a *_WITH_MIPMAPS filter.
	constexpr int MAX_LEVELS = 14;
	const uint8_t *levels[MAX_LEVELS];
	const int level_count = MIN(img->has_mipmaps() ? img->get_mipmap_count() + 1 : 1, MAX_LEVELS);
	for (int l = 0; l < level_count; l++) {
		int64_t ofs = 0, size = 0;
		img->get_mipmap_offset_and_size(l, ofs, size);
		levels[l] = data.ptr() + ofs;
	}

	if (p_texture->gpu && (p_texture->width != w || p_texture->height != h ||
								  (int)wiiu_gx2_texture_levels(p_texture->gpu) != level_count)) {
		_release_gpu(p_texture);
	}
	if (p_texture->gpu) {
		wiiu_gx2_texture_update_levels(p_texture->gpu, levels, level_count);
	} else {
		p_texture->gpu = wiiu_gx2_texture_create_tiled_levels(w, h, levels, level_count);
	}
	p_texture->width = w;
	p_texture->height = h;
	p_texture->format = p_image->get_format();
	// The CPU copy is only for texture_2d_get (Texture2D.get_image()). Keeping
	// it for every texture doubles texture memory, so only small images keep
	// one; the others are on the GPU only.
	p_texture->image = (int64_t)w * h <= KEEP_IMAGE_MAX_PIXELS ? p_image : Ref<Image>();
}

TextureStorageWiiU::Resolved TextureStorageWiiU::resolve(RID p_texture) const {
	Resolved r;
	RID rid = p_texture;
	if (CanvasTexture *ct = canvas_textures.get_or_null(rid)) {
		r.filter = ct->filter;
		r.repeat = ct->repeat;
		rid = ct->diffuse;
	}
	Texture *t = textures.get_or_null(rid);
	// Follow proxies (a few levels at most).
	for (int i = 0; t && t->proxy_to.is_valid() && i < 4; i++) {
		t = textures.get_or_null(t->proxy_to);
	}
	if (t == nullptr) {
		return r;
	}
	if (t->render_target) {
		r.texture = wiiu_gx2_render_target_texture(t->render_target->gpu);
		r.size = Size2(MAX(t->render_target->size.x, 1), MAX(t->render_target->size.y, 1));
	} else {
		r.texture = t->gpu;
		r.size = Size2(MAX(t->width, 1), MAX(t->height, 1));
	}
	return r;
}

/* Canvas textures */

RID TextureStorageWiiU::canvas_texture_allocate() {
	return canvas_textures.make_rid(memnew(CanvasTexture));
}

void TextureStorageWiiU::canvas_texture_initialize(RID p_rid) {}

void TextureStorageWiiU::canvas_texture_free(RID p_rid) {
	CanvasTexture *ct = canvas_textures.get_or_null(p_rid);
	ERR_FAIL_NULL(ct);
	canvas_textures.free(p_rid);
	memdelete(ct);
}

void TextureStorageWiiU::canvas_texture_set_channel(RID p_canvas_texture, RSE::CanvasTextureChannel p_channel, RID p_texture) {
	CanvasTexture *ct = canvas_textures.get_or_null(p_canvas_texture);
	ERR_FAIL_NULL(ct);
	// Normal and specular maps are for 2D lighting, which isn't supported.
	if (p_channel == RSE::CANVAS_TEXTURE_CHANNEL_DIFFUSE) {
		ct->diffuse = p_texture;
	}
}

void TextureStorageWiiU::canvas_texture_set_texture_filter(RID p_item, RSE::CanvasItemTextureFilter p_filter) {
	if (CanvasTexture *ct = canvas_textures.get_or_null(p_item)) {
		ct->filter = p_filter;
	}
}

void TextureStorageWiiU::canvas_texture_set_texture_repeat(RID p_item, RSE::CanvasItemTextureRepeat p_repeat) {
	if (CanvasTexture *ct = canvas_textures.get_or_null(p_item)) {
		ct->repeat = p_repeat;
	}
}

/* Textures */

RID TextureStorageWiiU::texture_allocate() {
	return textures.make_rid(memnew(Texture));
}

void TextureStorageWiiU::texture_free(RID p_rid) {
	Texture *t = textures.get_or_null(p_rid);
	ERR_FAIL_NULL(t);
	ERR_FAIL_COND_MSG(t->render_target != nullptr, "Can't free a render target's texture; free the render target.");
	_release_gpu(t);
	textures.free(p_rid);
	memdelete(t);
}

void TextureStorageWiiU::texture_2d_initialize(RID p_texture, const Ref<Image> &p_image) {
	Texture *t = textures.get_or_null(p_texture);
	ERR_FAIL_NULL(t);
	_upload(t, p_image);
}

void TextureStorageWiiU::texture_2d_update(RID p_texture, const Ref<Image> &p_image, int p_layer) {
	Texture *t = textures.get_or_null(p_texture);
	ERR_FAIL_NULL(t);
	_upload(t, p_image);
}

void TextureStorageWiiU::texture_2d_placeholder_initialize(RID p_texture) {
	Texture *t = textures.get_or_null(p_texture);
	ERR_FAIL_NULL(t);
	Ref<Image> image = Image::create_empty(4, 4, false, Image::FORMAT_RGBA8);
	image->fill(Color(1, 0, 1, 1));
	_upload(t, image);
}

void TextureStorageWiiU::texture_proxy_initialize(RID p_texture, RID p_base) {
	Texture *t = textures.get_or_null(p_texture);
	ERR_FAIL_NULL(t);
	t->proxy_to = p_base;
}

void TextureStorageWiiU::texture_proxy_update(RID p_proxy, RID p_base) {
	texture_proxy_initialize(p_proxy, p_base);
}

Ref<Image> TextureStorageWiiU::texture_2d_get(RID p_texture) const {
	Texture *t = textures.get_or_null(p_texture);
	ERR_FAIL_NULL_V(t, Ref<Image>());
	if (t->image.is_null() && t->gpu) {
		WARN_PRINT_ONCE("Cafe Godot: get_image() on a texture larger than 512x512 returns null (its pixels are only on the GPU).");
	}
	return t->image;
}

void TextureStorageWiiU::texture_replace(RID p_texture, RID p_by_texture) {
	Texture *t = textures.get_or_null(p_texture);
	Texture *by = textures.get_or_null(p_by_texture);
	ERR_FAIL_COND(t == nullptr || by == nullptr);
	_release_gpu(t);
	t->gpu = by->gpu;
	t->image = by->image;
	t->width = by->width;
	t->height = by->height;
	t->format = by->format;
	t->proxy_to = by->proxy_to;
	by->gpu = nullptr;
	texture_free(p_by_texture);
}

void TextureStorageWiiU::texture_set_size_override(RID p_texture, int p_width, int p_height) {
	if (Texture *t = textures.get_or_null(p_texture)) {
		t->size_override = Size2i(p_width, p_height);
	}
}

void TextureStorageWiiU::texture_set_path(RID p_texture, const String &p_path) {
	if (Texture *t = textures.get_or_null(p_texture)) {
		t->path = p_path;
	}
}

String TextureStorageWiiU::texture_get_path(RID p_texture) const {
	Texture *t = textures.get_or_null(p_texture);
	return t ? t->path : String();
}

Image::Format TextureStorageWiiU::texture_get_format(RID p_texture) const {
	Texture *t = textures.get_or_null(p_texture);
	return t ? t->format : Image::FORMAT_MAX;
}

Size2 TextureStorageWiiU::texture_size_with_proxy(RID p_proxy) {
	return resolve(p_proxy).size;
}

/* Render targets */

RID TextureStorageWiiU::render_target_create() {
	RenderTarget *rt = memnew(RenderTarget);
	Texture *t = memnew(Texture);
	t->render_target = rt;
	rt->texture = textures.make_rid(t);
	return render_targets.make_rid(rt);
}

void TextureStorageWiiU::render_target_free(RID p_rid) {
	RenderTarget *rt = render_targets.get_or_null(p_rid);
	ERR_FAIL_NULL(rt);
	if (rt->gpu) {
		wiiu_gx2_render_target_free(rt->gpu);
	}
	if (Texture *t = textures.get_or_null(rt->texture)) {
		textures.free(rt->texture);
		memdelete(t);
	}
	render_targets.free(p_rid);
	memdelete(rt);
}

void TextureStorageWiiU::render_target_set_position(RID p_render_target, int p_x, int p_y) {
	if (RenderTarget *rt = render_targets.get_or_null(p_render_target)) {
		rt->position = Point2i(p_x, p_y);
	}
}

Point2i TextureStorageWiiU::render_target_get_position(RID p_render_target) const {
	RenderTarget *rt = render_targets.get_or_null(p_render_target);
	return rt ? rt->position : Point2i();
}

void TextureStorageWiiU::render_target_set_size(RID p_render_target, int p_width, int p_height, uint32_t p_view_count) {
	RenderTarget *rt = render_targets.get_or_null(p_render_target);
	ERR_FAIL_NULL(rt);
	const Size2i size(p_width, p_height);
	if (rt->size == size && (rt->gpu != nullptr || size.x <= 0 || size.y <= 0)) {
		return;
	}
	if (rt->gpu) {
		wiiu_gx2_render_target_free(rt->gpu);
		rt->gpu = nullptr;
	}
	rt->size = size;
	if (size.x > 0 && size.y > 0) {
		rt->gpu = wiiu_gx2_render_target_create(size.x, size.y);
	}
}

Size2i TextureStorageWiiU::render_target_get_size(RID p_render_target) const {
	RenderTarget *rt = render_targets.get_or_null(p_render_target);
	return rt ? rt->size : Size2i();
}

void TextureStorageWiiU::render_target_set_transparent(RID p_render_target, bool p_is_transparent) {
	if (RenderTarget *rt = render_targets.get_or_null(p_render_target)) {
		rt->transparent = p_is_transparent;
	}
}

bool TextureStorageWiiU::render_target_get_transparent(RID p_render_target) const {
	RenderTarget *rt = render_targets.get_or_null(p_render_target);
	return rt && rt->transparent;
}

void TextureStorageWiiU::render_target_set_direct_to_screen(RID p_render_target, bool p_direct_to_screen) {
	if (RenderTarget *rt = render_targets.get_or_null(p_render_target)) {
		rt->direct_to_screen = p_direct_to_screen;
	}
}

bool TextureStorageWiiU::render_target_get_direct_to_screen(RID p_render_target) const {
	RenderTarget *rt = render_targets.get_or_null(p_render_target);
	return rt && rt->direct_to_screen;
}

bool TextureStorageWiiU::render_target_was_used(RID p_render_target) const {
	RenderTarget *rt = render_targets.get_or_null(p_render_target);
	return rt && rt->used;
}

void TextureStorageWiiU::render_target_set_as_unused(RID p_render_target) {
	if (RenderTarget *rt = render_targets.get_or_null(p_render_target)) {
		rt->used = false;
	}
}

void TextureStorageWiiU::render_target_request_clear(RID p_render_target, const Color &p_clear_color) {
	if (RenderTarget *rt = render_targets.get_or_null(p_render_target)) {
		rt->clear_requested = true;
		rt->clear_color = p_clear_color;
	}
}

bool TextureStorageWiiU::render_target_is_clear_requested(RID p_render_target) {
	RenderTarget *rt = render_targets.get_or_null(p_render_target);
	return rt && rt->clear_requested;
}

Color TextureStorageWiiU::render_target_get_clear_request_color(RID p_render_target) {
	RenderTarget *rt = render_targets.get_or_null(p_render_target);
	return rt ? rt->clear_color : Color();
}

void TextureStorageWiiU::render_target_disable_clear_request(RID p_render_target) {
	if (RenderTarget *rt = render_targets.get_or_null(p_render_target)) {
		rt->clear_requested = false;
	}
}

void TextureStorageWiiU::render_target_do_clear_request(RID p_render_target) {
	RenderTarget *rt = render_targets.get_or_null(p_render_target);
	if (rt == nullptr || !rt->clear_requested || rt->gpu == nullptr) {
		return;
	}
	wiiu_gx2_bind_render_target(rt->gpu);
	const Color c = rt->clear_color;
	wiiu_gx2_clear(c.r, c.g, c.b, rt->transparent ? c.a : 1.0f);
	rt->clear_requested = false;
}

RID TextureStorageWiiU::render_target_get_texture(RID p_render_target) {
	RenderTarget *rt = render_targets.get_or_null(p_render_target);
	return rt ? rt->texture : RID();
}

// ===========================================================================
// UtilitiesWiiU
// ===========================================================================

bool UtilitiesWiiU::free(RID p_rid) {
	TextureStorageWiiU *ts = TextureStorageWiiU::get_wiiu_singleton();
	if (ts && ts->owns_wiiu_texture(p_rid)) {
		ts->texture_free(p_rid);
		return true;
	}
	if (ts && ts->owns_canvas_texture(p_rid)) {
		ts->canvas_texture_free(p_rid);
		return true;
	}
	return RendererDummy::Utilities::free(p_rid);
}

// ===========================================================================
// MeshStorageWiiU
// ===========================================================================

MeshStorageWiiU::MeshStorageWiiU() {
	wiiu_singleton = this;
}

MeshStorageWiiU::~MeshStorageWiiU() {
	wiiu_singleton = nullptr;
}

// Decodes a surface into points/colours/UVs/indices once, when it is added.
// Note: surfaces from ArrayMesh resources store their vertex bytes
// little-endian and are not swapped; meshes built at run time (QuadMesh and
// other primitive meshes, CPUParticles2D, SurfaceTool, ArrayMesh from arrays)
// are native and draw correctly.
void MeshStorageWiiU::_decode_surface(RID p_mesh, const RenderingServerTypes::SurfaceData &p_surface) {
	const Array arrays = RenderingServer::get_singleton()->mesh_create_arrays_from_surface_data(p_surface);
	Surface s;
	s.primitive = p_surface.primitive;
	if (arrays.size() == RSE::ARRAY_MAX) {
		const Variant &verts = arrays[RSE::ARRAY_VERTEX];
		if (verts.get_type() == Variant::PACKED_VECTOR2_ARRAY) {
			const PackedVector2Array v = verts;
			s.points.resize(v.size());
			for (int i = 0; i < v.size(); i++) {
				s.points[i] = v[i];
			}
		} else if (verts.get_type() == Variant::PACKED_VECTOR3_ARRAY) {
			const PackedVector3Array v = verts;
			s.points.resize(v.size());
			for (int i = 0; i < v.size(); i++) {
				s.points[i] = Vector2(v[i].x, v[i].y);
			}
		}
		if (arrays[RSE::ARRAY_COLOR].get_type() == Variant::PACKED_COLOR_ARRAY) {
			const PackedColorArray c = arrays[RSE::ARRAY_COLOR];
			if (c.size() == (int)s.points.size()) {
				s.colors.resize(c.size());
				for (int i = 0; i < c.size(); i++) {
					s.colors[i] = c[i];
				}
			}
		}
		if (arrays[RSE::ARRAY_TEX_UV].get_type() == Variant::PACKED_VECTOR2_ARRAY) {
			const PackedVector2Array uv = arrays[RSE::ARRAY_TEX_UV];
			if (uv.size() == (int)s.points.size()) {
				s.uvs.resize(uv.size());
				for (int i = 0; i < uv.size(); i++) {
					s.uvs[i] = uv[i];
				}
			}
		}
		if (arrays[RSE::ARRAY_INDEX].get_type() == Variant::PACKED_INT32_ARRAY) {
			const PackedInt32Array idx = arrays[RSE::ARRAY_INDEX];
			s.indices.resize(idx.size());
			for (int i = 0; i < idx.size(); i++) {
				s.indices[i] = idx[i];
			}
		}
	}
	if (s.points.size() > 0) {
		s.rect = Rect2(s.points[0], Size2());
		for (uint32_t i = 1; i < s.points.size(); i++) {
			s.rect.expand_to(s.points[i]);
		}
	}
	mesh_surfaces[p_mesh].push_back(s);
}

void MeshStorageWiiU::mesh_free(RID p_rid) {
	mesh_surfaces.erase(p_rid);
	mesh_custom_aabbs.erase(p_rid);
	RendererDummy::MeshStorage::mesh_free(p_rid);
}

void MeshStorageWiiU::mesh_add_surface(RID p_mesh, const RenderingServerTypes::SurfaceData &p_surface) {
	RendererDummy::MeshStorage::mesh_add_surface(p_mesh, p_surface);
	if (owns_mesh(p_mesh)) {
		_decode_surface(p_mesh, p_surface);
	}
}

void MeshStorageWiiU::mesh_surface_remove(RID p_mesh, int p_surface) {
	RendererDummy::MeshStorage::mesh_surface_remove(p_mesh, p_surface);
	LocalVector<Surface> *surfaces = mesh_surfaces.getptr(p_mesh);
	if (surfaces && p_surface >= 0 && p_surface < (int)surfaces->size()) {
		surfaces->remove_at(p_surface);
	}
}

void MeshStorageWiiU::mesh_clear(RID p_mesh) {
	RendererDummy::MeshStorage::mesh_clear(p_mesh);
	mesh_surfaces.erase(p_mesh);
}

void MeshStorageWiiU::mesh_set_custom_aabb(RID p_mesh, const AABB &p_aabb) {
	mesh_custom_aabbs[p_mesh] = p_aabb;
}

AABB MeshStorageWiiU::mesh_get_custom_aabb(RID p_mesh) const {
	const AABB *aabb = mesh_custom_aabbs.getptr(p_mesh);
	return aabb ? *aabb : AABB();
}

// The canvas culls mesh items by this, so it must cover the points.
AABB MeshStorageWiiU::mesh_get_aabb(RID p_mesh, RID p_skeleton) {
	const AABB *custom = mesh_custom_aabbs.getptr(p_mesh);
	if (custom && custom->has_volume()) {
		return *custom;
	}
	const LocalVector<Surface> *surfaces = mesh_surfaces.getptr(p_mesh);
	if (surfaces == nullptr || surfaces->size() == 0) {
		return AABB();
	}
	Rect2 r = (*surfaces)[0].rect;
	for (uint32_t i = 1; i < surfaces->size(); i++) {
		r = r.merge((*surfaces)[i].rect);
	}
	return AABB(Vector3(r.position.x, r.position.y, 0), Vector3(r.size.x, r.size.y, 0));
}

void MeshStorageWiiU::_multimesh_initialize(RID p_rid) {
	RendererDummy::MeshStorage::_multimesh_initialize(p_rid);
	multimeshes.insert(p_rid, MultiMesh());
}

void MeshStorageWiiU::_multimesh_free(RID p_rid) {
	multimeshes.erase(p_rid);
	RendererDummy::MeshStorage::_multimesh_free(p_rid);
}

// Instance layout, as in the other renderers: transform (8 floats for 2D,
// 12 for 3D, row-major), then colour (4) if used, then custom data (4).
void MeshStorageWiiU::_multimesh_allocate_data(RID p_multimesh, int p_instances, RSE::MultimeshTransformFormat p_transform_format, bool p_use_colors, bool p_use_custom_data, bool p_use_indirect) {
	MultiMesh *mm = multimeshes.getptr(p_multimesh);
	ERR_FAIL_NULL(mm);
	mm->instances = MAX(p_instances, 0);
	mm->format = p_transform_format;
	mm->use_colors = p_use_colors;
	mm->use_custom_data = p_use_custom_data;
	mm->color_offset = p_transform_format == RSE::MULTIMESH_TRANSFORM_2D ? 8 : 12;
	mm->stride = mm->color_offset + (p_use_colors ? 4 : 0) + (p_use_custom_data ? 4 : 0);
	mm->visible_instances = -1;
	mm->buffer.resize(mm->instances * mm->stride);
	for (int i = 0; i < mm->instances; i++) {
		// Identity transforms, white colours.
		float *d = mm->buffer.ptr() + i * mm->stride;
		for (int k = 0; k < mm->stride; k++) {
			d[k] = 0.0f;
		}
		if (mm->format == RSE::MULTIMESH_TRANSFORM_2D) {
			d[0] = d[5] = 1.0f;
		} else {
			d[0] = d[5] = d[10] = 1.0f;
		}
		if (mm->use_colors) {
			d[mm->color_offset] = d[mm->color_offset + 1] = d[mm->color_offset + 2] = d[mm->color_offset + 3] = 1.0f;
		}
	}
}

int MeshStorageWiiU::_multimesh_get_instance_count(RID p_multimesh) const {
	const MultiMesh *mm = multimeshes.getptr(p_multimesh);
	return mm ? mm->instances : 0;
}

void MeshStorageWiiU::_multimesh_set_mesh(RID p_multimesh, RID p_mesh) {
	MultiMesh *mm = multimeshes.getptr(p_multimesh);
	ERR_FAIL_NULL(mm);
	mm->mesh = p_mesh;
}

RID MeshStorageWiiU::_multimesh_get_mesh(RID p_multimesh) const {
	const MultiMesh *mm = multimeshes.getptr(p_multimesh);
	return mm ? mm->mesh : RID();
}

void MeshStorageWiiU::_multimesh_instance_set_transform(RID p_multimesh, int p_index, const Transform3D &p_transform) {
	MultiMesh *mm = multimeshes.getptr(p_multimesh);
	ERR_FAIL_NULL(mm);
	ERR_FAIL_INDEX(p_index, mm->instances);
	ERR_FAIL_COND(mm->format != RSE::MULTIMESH_TRANSFORM_3D);
	float *d = mm->buffer.ptr() + p_index * mm->stride;
	for (int row = 0; row < 3; row++) {
		d[row * 4 + 0] = p_transform.basis.rows[row][0];
		d[row * 4 + 1] = p_transform.basis.rows[row][1];
		d[row * 4 + 2] = p_transform.basis.rows[row][2];
		d[row * 4 + 3] = p_transform.origin[row];
	}
}

void MeshStorageWiiU::_multimesh_instance_set_transform_2d(RID p_multimesh, int p_index, const Transform2D &p_transform) {
	MultiMesh *mm = multimeshes.getptr(p_multimesh);
	ERR_FAIL_NULL(mm);
	ERR_FAIL_INDEX(p_index, mm->instances);
	ERR_FAIL_COND(mm->format != RSE::MULTIMESH_TRANSFORM_2D);
	float *d = mm->buffer.ptr() + p_index * mm->stride;
	d[0] = p_transform.columns[0][0];
	d[1] = p_transform.columns[1][0];
	d[2] = 0.0f;
	d[3] = p_transform.columns[2][0];
	d[4] = p_transform.columns[0][1];
	d[5] = p_transform.columns[1][1];
	d[6] = 0.0f;
	d[7] = p_transform.columns[2][1];
}

void MeshStorageWiiU::_multimesh_instance_set_color(RID p_multimesh, int p_index, const Color &p_color) {
	MultiMesh *mm = multimeshes.getptr(p_multimesh);
	ERR_FAIL_NULL(mm);
	ERR_FAIL_INDEX(p_index, mm->instances);
	ERR_FAIL_COND(!mm->use_colors);
	float *d = mm->buffer.ptr() + p_index * mm->stride + mm->color_offset;
	d[0] = p_color.r;
	d[1] = p_color.g;
	d[2] = p_color.b;
	d[3] = p_color.a;
}

void MeshStorageWiiU::_multimesh_instance_set_custom_data(RID p_multimesh, int p_index, const Color &p_color) {
	MultiMesh *mm = multimeshes.getptr(p_multimesh);
	ERR_FAIL_NULL(mm);
	ERR_FAIL_INDEX(p_index, mm->instances);
	ERR_FAIL_COND(!mm->use_custom_data);
	float *d = mm->buffer.ptr() + p_index * mm->stride + mm->color_offset + (mm->use_colors ? 4 : 0);
	d[0] = p_color.r;
	d[1] = p_color.g;
	d[2] = p_color.b;
	d[3] = p_color.a;
}

Transform3D MeshStorageWiiU::_multimesh_instance_get_transform(RID p_multimesh, int p_index) const {
	const MultiMesh *mm = multimeshes.getptr(p_multimesh);
	ERR_FAIL_NULL_V(mm, Transform3D());
	ERR_FAIL_INDEX_V(p_index, mm->instances, Transform3D());
	ERR_FAIL_COND_V(mm->format != RSE::MULTIMESH_TRANSFORM_3D, Transform3D());
	const float *d = mm->buffer.ptr() + p_index * mm->stride;
	Transform3D t;
	for (int row = 0; row < 3; row++) {
		t.basis.rows[row] = Vector3(d[row * 4 + 0], d[row * 4 + 1], d[row * 4 + 2]);
		t.origin[row] = d[row * 4 + 3];
	}
	return t;
}

Transform2D MeshStorageWiiU::instance_transform(const MultiMesh &p_mm, int p_index) {
	const float *d = p_mm.buffer.ptr() + p_index * p_mm.stride;
	// Rows x (floats 0-3) and y (4-7) in both formats; 3D keeps the x/y part.
	return Transform2D(d[0], d[4], d[1], d[5], d[3], d[7]);
}

Color MeshStorageWiiU::instance_color(const MultiMesh &p_mm, int p_index) {
	if (!p_mm.use_colors) {
		return Color(1, 1, 1, 1);
	}
	const float *d = p_mm.buffer.ptr() + p_index * p_mm.stride + p_mm.color_offset;
	return Color(d[0], d[1], d[2], d[3]);
}

Transform2D MeshStorageWiiU::_multimesh_instance_get_transform_2d(RID p_multimesh, int p_index) const {
	const MultiMesh *mm = multimeshes.getptr(p_multimesh);
	ERR_FAIL_NULL_V(mm, Transform2D());
	ERR_FAIL_INDEX_V(p_index, mm->instances, Transform2D());
	ERR_FAIL_COND_V(mm->format != RSE::MULTIMESH_TRANSFORM_2D, Transform2D());
	return instance_transform(*mm, p_index);
}

Color MeshStorageWiiU::_multimesh_instance_get_color(RID p_multimesh, int p_index) const {
	const MultiMesh *mm = multimeshes.getptr(p_multimesh);
	ERR_FAIL_NULL_V(mm, Color());
	ERR_FAIL_INDEX_V(p_index, mm->instances, Color());
	return instance_color(*mm, p_index);
}

Color MeshStorageWiiU::_multimesh_instance_get_custom_data(RID p_multimesh, int p_index) const {
	const MultiMesh *mm = multimeshes.getptr(p_multimesh);
	ERR_FAIL_NULL_V(mm, Color());
	ERR_FAIL_INDEX_V(p_index, mm->instances, Color());
	if (!mm->use_custom_data) {
		return Color();
	}
	const float *d = mm->buffer.ptr() + p_index * mm->stride + mm->color_offset + (mm->use_colors ? 4 : 0);
	return Color(d[0], d[1], d[2], d[3]);
}

void MeshStorageWiiU::_multimesh_set_buffer(RID p_multimesh, const Vector<float> &p_buffer) {
	MultiMesh *mm = multimeshes.getptr(p_multimesh);
	ERR_FAIL_NULL(mm);
	ERR_FAIL_COND(p_buffer.size() != mm->instances * mm->stride);
	memcpy(mm->buffer.ptr(), p_buffer.ptr(), p_buffer.size() * sizeof(float));
}

Vector<float> MeshStorageWiiU::_multimesh_get_buffer(RID p_multimesh) const {
	const MultiMesh *mm = multimeshes.getptr(p_multimesh);
	ERR_FAIL_NULL_V(mm, Vector<float>());
	Vector<float> out;
	out.resize(mm->buffer.size());
	if (mm->buffer.size() > 0) {
		memcpy(out.ptrw(), mm->buffer.ptr(), mm->buffer.size() * sizeof(float));
	}
	return out;
}

void MeshStorageWiiU::_multimesh_set_visible_instances(RID p_multimesh, int p_visible) {
	MultiMesh *mm = multimeshes.getptr(p_multimesh);
	ERR_FAIL_NULL(mm);
	mm->visible_instances = p_visible;
}

int MeshStorageWiiU::_multimesh_get_visible_instances(RID p_multimesh) const {
	const MultiMesh *mm = multimeshes.getptr(p_multimesh);
	return mm ? mm->visible_instances : 0;
}

void MeshStorageWiiU::_multimesh_set_custom_aabb(RID p_multimesh, const AABB &p_aabb) {
	MultiMesh *mm = multimeshes.getptr(p_multimesh);
	ERR_FAIL_NULL(mm);
	mm->custom_aabb = p_aabb;
}

AABB MeshStorageWiiU::_multimesh_get_custom_aabb(RID p_multimesh) const {
	const MultiMesh *mm = multimeshes.getptr(p_multimesh);
	return mm ? mm->custom_aabb : AABB();
}

// Union of the mesh's bounds under every visible instance (the canvas culls by it).
AABB MeshStorageWiiU::_multimesh_get_aabb(RID p_multimesh) {
	MultiMesh *mm = multimeshes.getptr(p_multimesh);
	ERR_FAIL_NULL_V(mm, AABB());
	if (mm->custom_aabb.has_volume()) {
		return mm->custom_aabb;
	}
	const AABB mesh_aabb = mesh_get_aabb(mm->mesh);
	const Rect2 mesh_rect(mesh_aabb.position.x, mesh_aabb.position.y, mesh_aabb.size.x, mesh_aabb.size.y);
	const int count = mm->visible_instances < 0 ? mm->instances : MIN(mm->visible_instances, mm->instances);
	Rect2 r;
	for (int i = 0; i < count; i++) {
		const Rect2 ir = instance_transform(*mm, i).xform(mesh_rect);
		r = i == 0 ? ir : r.merge(ir);
	}
	return AABB(Vector3(r.position.x, r.position.y, 0), Vector3(r.size.x, r.size.y, 0));
}

// ===========================================================================
// MaterialStorageWiiU
// ===========================================================================

MaterialStorageWiiU::MaterialStorageWiiU() {
	wiiu_singleton = this;
}

MaterialStorageWiiU::~MaterialStorageWiiU() {
	wiiu_singleton = nullptr;
}

// The blend_* word of a canvas_item shader's render_mode line.
static WiiUBlend blend_from_shader_code(const String &p_code) {
	if (!p_code.contains("canvas_item")) {
		return WIIU_BLEND_MIX;
	}
	int at = p_code.find("render_mode");
	while (at != -1) {
		const int end = p_code.find(";", at);
		if (end == -1) {
			break;
		}
		const String modes = p_code.substr(at + 11, end - at - 11);
		for (const String &m : modes.split(",")) {
			const String mode = m.strip_edges();
			if (mode == "blend_add") {
				return WIIU_BLEND_ADD;
			} else if (mode == "blend_sub") {
				return WIIU_BLEND_SUB;
			} else if (mode == "blend_mul") {
				return WIIU_BLEND_MUL;
			} else if (mode == "blend_premul_alpha") {
				return WIIU_BLEND_PREMUL_ALPHA;
			} else if (mode == "blend_disabled") {
				return WIIU_BLEND_DISABLED;
			}
		}
		at = p_code.find("render_mode", end);
	}
	return WIIU_BLEND_MIX;
}

void MaterialStorageWiiU::shader_free(RID p_rid) {
	shader_blend.erase(p_rid);
	RendererDummy::MaterialStorage::shader_free(p_rid);
}

void MaterialStorageWiiU::shader_set_code(RID p_shader, const String &p_code) {
	RendererDummy::MaterialStorage::shader_set_code(p_shader, p_code);
	const WiiUBlend b = blend_from_shader_code(p_code);
	if (b == WIIU_BLEND_MIX) {
		shader_blend.erase(p_shader);
	} else {
		shader_blend[p_shader] = b;
	}
}

void MaterialStorageWiiU::material_free(RID p_rid) {
	material_shader.erase(p_rid);
	RendererDummy::MaterialStorage::material_free(p_rid);
}

void MaterialStorageWiiU::material_set_shader(RID p_material, RID p_shader) {
	RendererDummy::MaterialStorage::material_set_shader(p_material, p_shader);
	material_shader[p_material] = p_shader;
}

WiiUBlend MaterialStorageWiiU::get_material_blend(RID p_material) const {
	const RID *shader = material_shader.getptr(p_material);
	if (shader == nullptr) {
		return WIIU_BLEND_MIX;
	}
	const WiiUBlend *b = shader_blend.getptr(*shader);
	return b ? *b : WIIU_BLEND_MIX;
}

// ===========================================================================
// RasterizerCanvasWiiU
// ===========================================================================

RendererCanvasRender::PolygonID RasterizerCanvasWiiU::request_polygon(const Vector<int> &p_indices, const Vector<Point2> &p_points, const Vector<Color> &p_colors, const Vector<Point2> &p_uvs, const Vector<int> &p_bones, const Vector<float> &p_weights, int p_count) {
	PolygonData data;
	const int index_count = p_count < 0 ? p_indices.size() : MIN(p_count, p_indices.size());
	data.indices.resize(index_count);
	for (int i = 0; i < index_count; i++) {
		data.indices[i] = p_indices[i];
	}
	data.points.resize(p_points.size());
	for (int i = 0; i < p_points.size(); i++) {
		data.points[i] = p_points[i];
	}
	if (p_colors.size() == p_points.size() || p_colors.size() == 1) {
		data.colors.resize(p_colors.size());
		for (int i = 0; i < p_colors.size(); i++) {
			data.colors[i] = p_colors[i];
		}
	}
	if (p_uvs.size() == p_points.size()) {
		data.uvs.resize(p_uvs.size());
		for (int i = 0; i < p_uvs.size(); i++) {
			data.uvs[i] = p_uvs[i];
		}
	}
	const PolygonID id = polygon_id_counter++;
	polygons.insert(id, data);
	return id;
}

void RasterizerCanvasWiiU::free_polygon(PolygonID p_polygon) {
	polygons.erase(p_polygon);
}

void RasterizerCanvasWiiU::_flush() {
	// In chunks that fit the GX2 vertex arena (2 MB). A multiple of 6 splits
	// cleanly for both triangles (3) and line pairs (2).
	static constexpr uint32_t MAX_DRAW_VERTICES = 6 * 8192;
	for (uint32_t start = 0; start < batch.size(); start += MAX_DRAW_VERTICES) {
		const uint32_t count = MIN(MAX_DRAW_VERTICES, batch.size() - start);
		wiiu_gx2_set_blend(batch_blend);
		wiiu_gx2_draw(batch_primitive, batch_texture, batch_linear, batch_repeat, batch.ptr() + start, count);
		wiiu_gx2_set_blend(WIIU_BLEND_MIX);
	}
	batch.clear();
}

void RasterizerCanvasWiiU::_begin(WiiUPrimitive p_primitive, WiiUTexture *p_texture, int p_linear, bool p_repeat) {
	if (p_primitive != batch_primitive || p_texture != batch_texture || p_linear != batch_linear || p_repeat != batch_repeat || blend != batch_blend) {
		_flush();
		batch_primitive = p_primitive;
		batch_texture = p_texture;
		batch_linear = p_linear;
		batch_repeat = p_repeat;
		batch_blend = blend;
	}
}

void RasterizerCanvasWiiU::_vertex(const Vector2 &p_pos, const Vector2 &p_uv, const Color &p_color) {
	WiiUVertex v;
	v.x = p_pos.x;
	v.y = p_pos.y;
	v.z = 0.0f;
	v.u = p_uv.x;
	v.v = p_uv.y;
	v.r = p_color.r;
	v.g = p_color.g;
	v.b = p_color.b;
	v.a = p_color.a;
	batch.push_back(v);
}

// Two triangles: corners 0-1-2 and 0-2-3.
void RasterizerCanvasWiiU::_quad(const Transform2D &p_xform, const Vector2 p_pos[4], const Vector2 p_uv[4], const Color &p_color) {
	static const int order[6] = { 0, 1, 2, 0, 2, 3 };
	for (int i = 0; i < 6; i++) {
		const int k = order[i];
		_vertex(p_xform.xform(p_pos[k]), p_uv[k], p_color);
	}
}

void RasterizerCanvasWiiU::draw_texture_rect(WiiUTexture *p_texture, const Rect2 &p_dst, const Rect2 &p_src_uv, bool p_linear) {
	// The blit to the screen is a plain copy: blending it with the render
	// target's alpha darkened whatever blend_sub had left alpha below 1.
	blend = WIIU_BLEND_DISABLED;
	_begin(WIIU_PRIM_TRIANGLES, p_texture, p_linear, false);
	const Vector2 pos[4] = {
		p_dst.position,
		Vector2(p_dst.position.x + p_dst.size.x, p_dst.position.y),
		p_dst.position + p_dst.size,
		Vector2(p_dst.position.x, p_dst.position.y + p_dst.size.y),
	};
	const Vector2 uv[4] = {
		p_src_uv.position,
		Vector2(p_src_uv.position.x + p_src_uv.size.x, p_src_uv.position.y),
		p_src_uv.position + p_src_uv.size,
		Vector2(p_src_uv.position.x, p_src_uv.position.y + p_src_uv.size.y),
	};
	_quad(Transform2D(), pos, uv, Color(1, 1, 1, 1));
	_flush();
}

// Godot's canvas filter as WIIU_FILTER_* bits (anisotropic is plain trilinear).
static int filter_mode(RSE::CanvasItemTextureFilter p_filter) {
	switch (p_filter) {
		case RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR:
			return WIIU_FILTER_LINEAR;
		case RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS:
		case RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS_ANISOTROPIC:
			return WIIU_FILTER_MIPMAPS;
		case RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS:
		case RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC:
			return WIIU_FILTER_LINEAR | WIIU_FILTER_MIPMAPS;
		default:
			return 0;
	}
}

void RasterizerCanvasWiiU::canvas_render_items(RID p_to_render_target, Item *p_item_list, const Color &p_modulate, Light *p_light_list, Light *p_directional_list, const Transform2D &p_canvas_transform, RSE::CanvasItemTextureFilter p_default_filter, RSE::CanvasItemTextureRepeat p_default_repeat, bool p_snap_2d_vertices_to_pixel, bool &r_sdf_used, RenderingServerTypes::RenderInfo *r_render_info) {
	TextureStorageWiiU *ts = TextureStorageWiiU::get_wiiu_singleton();
	MeshStorageWiiU *ms = MeshStorageWiiU::get_wiiu_singleton();
	TextureStorageWiiU::RenderTarget *rt = ts->get_render_target(p_to_render_target);
	if (rt == nullptr || rt->gpu == nullptr || !wiiu_gx2_can_draw()) {
		return;
	}
	wiiu_gx2_bind_render_target(rt->gpu);
	if (rt->clear_requested) {
		const Color c = rt->clear_color;
		wiiu_gx2_clear(c.r, c.g, c.b, rt->transparent ? c.a : 1.0f);
		rt->clear_requested = false;
	}
	rt->used = true;

	const Size2i rt_size = rt->size;
	Item *current_clip = nullptr;
	bool clip_ignored = false;
	batch.clear();

	auto apply_clip = [&](Item *p_clip) {
		_flush();
		if (p_clip && !clip_ignored) {
			const Rect2 r = p_clip->final_clip_rect;
			const int x0 = (int)Math::floor(r.position.x);
			const int y0 = (int)Math::floor(r.position.y);
			const int x1 = (int)Math::ceil(r.position.x + r.size.x);
			const int y1 = (int)Math::ceil(r.position.y + r.size.y);
			wiiu_gx2_set_scissor(x0, y0, x1 - x0, y1 - y0);
		} else {
			wiiu_gx2_set_scissor(0, 0, rt_size.x, rt_size.y);
		}
	};

	for (Item *ci = p_item_list; ci; ci = ci->next) {
		if (ci->final_clip_owner != current_clip) {
			current_clip = ci->final_clip_owner;
			clip_ignored = false;
			apply_clip(current_clip);
		}

		const RSE::CanvasItemTextureFilter item_filter = ci->texture_filter == RSE::CANVAS_ITEM_TEXTURE_FILTER_DEFAULT ? p_default_filter : ci->texture_filter;
		const RSE::CanvasItemTextureRepeat item_repeat = ci->texture_repeat == RSE::CANVAS_ITEM_TEXTURE_REPEAT_DEFAULT ? p_default_repeat : ci->texture_repeat;
		// Blend mode of the item's material (render_mode blend_* / CanvasItemMaterial).
		{
			const RID material = ci->material_owner == nullptr ? ci->material : ci->material_owner->material;
			MaterialStorageWiiU *mats = MaterialStorageWiiU::get_wiiu_singleton();
			blend = (mats && material.is_valid()) ? mats->get_material_blend(material) : WIIU_BLEND_MIX;
		}
		// final_transform already maps the item into render-target pixels.
		Transform2D base_xform = ci->final_transform;
		Transform2D xform = base_xform;
		const Color base_color = ci->final_modulate * p_modulate;

		// Picks texture and sampler state for a draw command.
		auto use_texture = [&](RID p_texture, WiiUPrimitive p_primitive, Size2 &r_size) {
			TextureStorageWiiU::Resolved res = ts->resolve(p_texture);
			const RSE::CanvasItemTextureFilter f = res.filter == RSE::CANVAS_ITEM_TEXTURE_FILTER_DEFAULT ? item_filter : res.filter;
			const RSE::CanvasItemTextureRepeat rep = res.repeat == RSE::CANVAS_ITEM_TEXTURE_REPEAT_DEFAULT ? item_repeat : res.repeat;
			_begin(p_primitive, res.texture, filter_mode(f), rep != RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED && rep != RSE::CANVAS_ITEM_TEXTURE_REPEAT_DEFAULT);
			r_size = res.size;
			return res.texture != nullptr;
		};

		// Draws a mesh's surfaces (MeshInstance2D, multimesh instances).
		auto draw_mesh = [&](RID p_mesh, RID p_texture, const Transform2D &p_xform, const Color &p_color) {
			const LocalVector<MeshStorageWiiU::Surface> *surfaces = ms ? ms->get_surfaces(p_mesh) : nullptr;
			if (surfaces == nullptr) {
				return;
			}
			for (const MeshStorageWiiU::Surface &surf : *surfaces) {
				const bool lines = surf.primitive == RSE::PRIMITIVE_LINES || surf.primitive == RSE::PRIMITIVE_LINE_STRIP;
				if (surf.primitive == RSE::PRIMITIVE_POINTS || surf.points.size() == 0) {
					continue;
				}
				Size2 tsize;
				use_texture(p_texture, lines ? WIIU_PRIM_LINES : WIIU_PRIM_TRIANGLES, tsize);
				const bool indexed = surf.indices.size() > 0;
				const uint32_t count = indexed ? surf.indices.size() : surf.points.size();
				auto vertex_at = [&](uint32_t p_i) {
					int idx = indexed ? surf.indices[p_i] : (int)p_i;
					if (idx < 0 || idx >= (int)surf.points.size()) {
						idx = 0; // Keep the primitive count right.
					}
					const Color col = surf.colors.size() > 0 ? surf.colors[idx] * p_color : p_color;
					const Vector2 uv = surf.uvs.size() > 0 ? surf.uvs[idx] : Vector2();
					_vertex(p_xform.xform(surf.points[idx]), uv, col);
				};
				switch (surf.primitive) {
					case RSE::PRIMITIVE_TRIANGLES:
						for (uint32_t i = 0; i + 2 < count; i += 3) {
							vertex_at(i);
							vertex_at(i + 1);
							vertex_at(i + 2);
						}
						break;
					case RSE::PRIMITIVE_LINES:
						for (uint32_t i = 0; i + 1 < count; i += 2) {
							vertex_at(i);
							vertex_at(i + 1);
						}
						break;
					case RSE::PRIMITIVE_TRIANGLE_STRIP:
						for (uint32_t i = 2; i < count; i++) {
							vertex_at(i - 2);
							vertex_at(i - 1);
							vertex_at(i);
						}
						break;
					case RSE::PRIMITIVE_LINE_STRIP:
						for (uint32_t i = 1; i < count; i++) {
							vertex_at(i - 1);
							vertex_at(i);
						}
						break;
					default:
						break;
				}
			}
		};

		// Draws the item's commands once, shifted by p_offset (Parallax2D repeats).
		auto record_commands = [&](const Point2 &p_offset) {
			base_xform = ci->final_transform;
			if (ci->repeat_source_item && (p_offset.x || p_offset.y)) {
				// The offset is in the repeat source's space, like GLES3.
				base_xform.columns[2] += ci->repeat_source_item->final_transform.basis_xform(p_offset);
			}
			xform = base_xform;
			// Inside an animation slice that is not playing now (draw_animation_slice).
			bool skipping = false;
			for (const Item::Command *c = ci->commands; c; c = c->next) {
				if (skipping && c->type != Item::Command::TYPE_ANIMATION_SLICE) {
					continue;
				}
				switch (c->type) {
					case Item::Command::TYPE_TRANSFORM: {
						const Item::CommandTransform *t = static_cast<const Item::CommandTransform *>(c);
						xform = base_xform * t->xform;
					} break;

					case Item::Command::TYPE_CLIP_IGNORE: {
						const Item::CommandClipIgnore *ig = static_cast<const Item::CommandClipIgnore *>(c);
						if (clip_ignored != ig->ignore) {
							clip_ignored = ig->ignore;
							apply_clip(current_clip);
						}
					} break;

					case Item::Command::TYPE_RECT: {
						const Item::CommandRect *rect = static_cast<const Item::CommandRect *>(c);
						Size2 tsize;
						const bool textured = use_texture(rect->texture, WIIU_PRIM_TRIANGLES, tsize);
						if (rect->flags & CANVAS_RECT_TILE) {
							// Tiling needs a repeating sampler.
							_begin(WIIU_PRIM_TRIANGLES, batch_texture, batch_linear, true);
						}

						Rect2 dst = rect->rect;
						if (dst.size.x < 0) {
							dst.position.x += dst.size.x;
							dst.size.x = -dst.size.x;
						}
						if (dst.size.y < 0) {
							dst.position.y += dst.size.y;
							dst.size.y = -dst.size.y;
						}
						Rect2 src(0, 0, 1, 1);
						if (textured && (rect->flags & CANVAS_RECT_REGION)) {
							src = Rect2(rect->source.position / tsize, rect->source.size / tsize);
						}
						float u0 = src.position.x, u1 = src.position.x + src.size.x;
						float v0 = src.position.y, v1 = src.position.y + src.size.y;
						if (rect->flags & CANVAS_RECT_FLIP_H) {
							SWAP(u0, u1);
						}
						if (rect->flags & CANVAS_RECT_FLIP_V) {
							SWAP(v0, v1);
						}
						const Vector2 pos[4] = {
							dst.position,
							Vector2(dst.position.x + dst.size.x, dst.position.y),
							dst.position + dst.size,
							Vector2(dst.position.x, dst.position.y + dst.size.y),
						};
						static const int corner[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
						Vector2 uv[4];
						for (int k = 0; k < 4; k++) {
							int bx = corner[k][0], by = corner[k][1];
							if (rect->flags & CANVAS_RECT_TRANSPOSE) {
								SWAP(bx, by);
							}
							uv[k] = Vector2(bx ? u1 : u0, by ? v1 : v0);
						}
						_quad(xform, pos, uv, rect->modulate * base_color);
					} break;

					case Item::Command::TYPE_NINEPATCH: {
						const Item::CommandNinePatch *np = static_cast<const Item::CommandNinePatch *>(c);
						Size2 tsize;
						const bool textured = use_texture(np->texture, WIIU_PRIM_TRIANGLES, tsize);
						const Color color = np->color * base_color;
						Rect2 src = np->source;
						if (!textured) {
							src = Rect2(0, 0, 1, 1);
							tsize = Size2(1, 1);
						} else if (src.size == Vector2()) {
							src = Rect2(Vector2(), tsize);
						}
						const Rect2 dst = np->rect;
						// margin: left, top, right, bottom (Side order).
						const float ml = np->margin[SIDE_LEFT], mt = np->margin[SIDE_TOP];
						const float mr = np->margin[SIDE_RIGHT], mb = np->margin[SIDE_BOTTOM];
						const float xs[4] = { dst.position.x, dst.position.x + ml, dst.position.x + dst.size.x - mr, dst.position.x + dst.size.x };
						const float ys[4] = { dst.position.y, dst.position.y + mt, dst.position.y + dst.size.y - mb, dst.position.y + dst.size.y };
						float us[4], vs[4];
						if (textured) {
							us[0] = src.position.x / tsize.x;
							us[1] = (src.position.x + ml) / tsize.x;
							us[2] = (src.position.x + src.size.x - mr) / tsize.x;
							us[3] = (src.position.x + src.size.x) / tsize.x;
							vs[0] = src.position.y / tsize.y;
							vs[1] = (src.position.y + mt) / tsize.y;
							vs[2] = (src.position.y + src.size.y - mb) / tsize.y;
							vs[3] = (src.position.y + src.size.y) / tsize.y;
						} else {
							for (int i = 0; i < 4; i++) {
								us[i] = vs[i] = 0.5f;
							}
						}
						// Each axis as segments: the margins, and the centre stretched,
						// tiled (partial last tile) or tiled to fit (whole tiles), like
						// the other renderers' map_ninepatch_axis.
						struct Segment {
							float p0, p1, t0, t1;
							int cell;
						};
						// p_half_texel: half a texel in UV units; tiles are inset by it so
						// linear filtering doesn't blend in the border at each tile seam.
						auto axis_segments = [](const float p_pos[4], const float p_uv[4], float p_src_center, RSE::NinePatchAxisMode p_mode, float p_half_texel, LocalVector<Segment> &r_out) {
							r_out.clear();
							r_out.push_back({ p_pos[0], p_pos[1], p_uv[0], p_uv[1], 0 });
							const float center = p_pos[2] - p_pos[1];
							if (p_mode == RSE::NINE_PATCH_STRETCH || p_src_center <= 0.0f || center <= 0.0f) {
								r_out.push_back({ p_pos[1], p_pos[2], p_uv[1], p_uv[2], 1 });
							} else {
								float tile = p_src_center;
								if (p_mode == RSE::NINE_PATCH_TILE_FIT) {
									tile = center / MAX(1.0f, Math::floor(center / p_src_center + 0.5f));
								}
								tile = MAX(tile, center / 256.0f); // At most 256 tiles.
								const float u0 = p_uv[1] + p_half_texel;
								const float u1 = p_uv[2] - p_half_texel;
								for (float at = p_pos[1]; at < p_pos[2] - 0.001f; at += tile) {
									const float len = MIN(tile, p_pos[2] - at);
									const float frac = len / tile;
									r_out.push_back({ at, at + len, u0, u0 + (u1 - u0) * frac, 1 });
								}
							}
							r_out.push_back({ p_pos[2], p_pos[3], p_uv[2], p_uv[3], 2 });
						};
						LocalVector<Segment> seg_x, seg_y;
						const bool linear_np = (batch_linear & WIIU_FILTER_LINEAR) != 0;
						axis_segments(xs, us, src.size.x - ml - mr, np->axis_x, textured && linear_np ? 0.5f / tsize.x : 0.0f, seg_x);
						axis_segments(ys, vs, src.size.y - mt - mb, np->axis_y, textured && linear_np ? 0.5f / tsize.y : 0.0f, seg_y);
						for (const Segment &sy : seg_y) {
							for (const Segment &sx : seg_x) {
								if (sx.cell == 1 && sy.cell == 1 && !np->draw_center) {
									continue;
								}
								const Vector2 pos[4] = {
									Vector2(sx.p0, sy.p0), Vector2(sx.p1, sy.p0),
									Vector2(sx.p1, sy.p1), Vector2(sx.p0, sy.p1)
								};
								const Vector2 uv[4] = {
									Vector2(sx.t0, sy.t0), Vector2(sx.t1, sy.t0),
									Vector2(sx.t1, sy.t1), Vector2(sx.t0, sy.t1)
								};
								_quad(xform, pos, uv, color);
							}
						}
					} break;

					case Item::Command::TYPE_POLYGON: {
						const Item::CommandPolygon *poly = static_cast<const Item::CommandPolygon *>(c);
						PolygonData *data = polygons.getptr(poly->polygon.polygon_id);
						if (data == nullptr) {
							break;
						}
						const bool lines = poly->primitive == RSE::PRIMITIVE_LINES || poly->primitive == RSE::PRIMITIVE_LINE_STRIP;
						if (poly->primitive == RSE::PRIMITIVE_POINTS) {
							break;
						}
						Size2 tsize;
						use_texture(poly->texture, lines ? WIIU_PRIM_LINES : WIIU_PRIM_TRIANGLES, tsize);
						const uint32_t count = data->indices.size() > 0 ? data->indices.size() : data->points.size();
						auto vertex_at = [&](uint32_t p_i) {
							const int idx = data->indices.size() > 0 ? data->indices[p_i] : (int)p_i;
							if (idx < 0 || idx >= (int)data->points.size()) {
								return;
							}
							Color col = base_color;
							if (data->colors.size() == data->points.size()) {
								col = data->colors[idx] * base_color;
							} else if (data->colors.size() == 1) {
								col = data->colors[0] * base_color;
							}
							const Vector2 uv = data->uvs.size() > 0 ? data->uvs[idx] : Vector2();
							_vertex(xform.xform(data->points[idx]), uv, col);
						};
						switch (poly->primitive) {
							case RSE::PRIMITIVE_TRIANGLES:
							case RSE::PRIMITIVE_LINES:
								for (uint32_t i = 0; i < count; i++) {
									vertex_at(i);
								}
								break;
							case RSE::PRIMITIVE_TRIANGLE_STRIP:
								for (uint32_t i = 2; i < count; i++) {
									vertex_at(i - 2);
									vertex_at(i - 1);
									vertex_at(i);
								}
								break;
							case RSE::PRIMITIVE_LINE_STRIP:
								for (uint32_t i = 1; i < count; i++) {
									vertex_at(i - 1);
									vertex_at(i);
								}
								break;
							default:
								break;
						}
					} break;

					case Item::Command::TYPE_PRIMITIVE: {
						const Item::CommandPrimitive *prim = static_cast<const Item::CommandPrimitive *>(c);
						Size2 tsize;
						if (prim->point_count == 2) {
							use_texture(prim->texture, WIIU_PRIM_LINES, tsize);
							for (int i = 0; i < 2; i++) {
								_vertex(xform.xform(prim->points[i]), prim->uvs[i], prim->colors[i] * base_color);
							}
						} else if (prim->point_count == 3 || prim->point_count == 4) {
							use_texture(prim->texture, WIIU_PRIM_TRIANGLES, tsize);
							static const int tri[6] = { 0, 1, 2, 0, 2, 3 };
							const int n = prim->point_count == 3 ? 3 : 6;
							for (int i = 0; i < n; i++) {
								const int k = tri[i];
								_vertex(xform.xform(prim->points[k]), prim->uvs[k], prim->colors[k] * base_color);
							}
						} else if (prim->point_count == 1) {
							// A point: a one-pixel quad.
							use_texture(prim->texture, WIIU_PRIM_TRIANGLES, tsize);
							const Vector2 p = prim->points[0];
							const Vector2 pos[4] = { p, p + Vector2(1, 0), p + Vector2(1, 1), p + Vector2(0, 1) };
							const Vector2 uv[4] = { prim->uvs[0], prim->uvs[0], prim->uvs[0], prim->uvs[0] };
							_quad(xform, pos, uv, prim->colors[0] * base_color);
						}
					} break;

					case Item::Command::TYPE_MESH: {
						const Item::CommandMesh *m = static_cast<const Item::CommandMesh *>(c);
						draw_mesh(m->mesh, m->texture, xform * m->transform, base_color * m->modulate);
					} break;

					case Item::Command::TYPE_MULTIMESH: {
						const Item::CommandMultiMesh *mmc = static_cast<const Item::CommandMultiMesh *>(c);
						const MeshStorageWiiU::MultiMesh *mm = ms ? ms->get_multimesh(mmc->multimesh) : nullptr;
						if (mm == nullptr || mm->mesh.is_null()) {
							break;
						}
						const int count = mm->visible_instances < 0 ? mm->instances : MIN(mm->visible_instances, mm->instances);
						for (int i = 0; i < count; i++) {
							const Color col = MeshStorageWiiU::instance_color(*mm, i);
							const Transform2D inst = MeshStorageWiiU::instance_transform(*mm, i);
							// Nothing to draw: invisible, or a zero basis (CPUParticles2D
							// zeroes the transform of inactive particles).
							if (col.a <= 0.0f || (inst.columns[0] == Vector2() && inst.columns[1] == Vector2())) {
								continue;
							}
							draw_mesh(mm->mesh, mmc->texture, xform * inst, base_color * col);
						}
					} break;

					case Item::Command::TYPE_ANIMATION_SLICE: {
						const Item::CommandAnimationSlice *as = static_cast<const Item::CommandAnimationSlice *>(c);
						const double local_time = Math::fposmod(RSG::rasterizer->get_total_time() - as->offset, as->animation_length);
						skipping = !(local_time >= as->slice_begin && local_time < as->slice_end);
						RenderingServerDefault::redraw_request(); // A visible animation needs redraws.
					} break;

					default:
						// GPUParticles2D (TYPE_PARTICLES): not supported yet.
						break;
				}
			}
		};

		// Parallax2D's repeat_size / repeat_times: draw the commands
		// repeat_times + 1 times per repeating axis, centred on the item.
		if (!ci->repeat_size.x && !ci->repeat_size.y) {
			record_commands(Point2());
		} else {
			const Point2 start = ci->repeat_size * -(ci->repeat_times / 2);
			const int times_x = ci->repeat_size.x ? ci->repeat_times : 0;
			const int times_y = ci->repeat_size.y ? ci->repeat_times : 0;
			for (int ry = 0; ry <= times_y; ry++) {
				for (int rx = 0; rx <= times_x; rx++) {
					record_commands(start + Point2(rx * ci->repeat_size.x, ry * ci->repeat_size.y));
				}
			}
		}
	}
	_flush();
}

// ===========================================================================
// RasterizerWiiU
// ===========================================================================

// Geometry of the last blit to the GamePad, for mapping touches back.
static struct {
	bool valid = false;
	Size2 rt_size; // Render-target pixels.
	Rect2 drc_rect; // Where the render target was drawn on the GamePad.
	Rect2 window_rect; // Where Godot thinks it is in the window (BlitToScreen::dst_rect).
} last_drc_blit;

bool rasterizer_wiiu_drc_to_window(const Vector2 &p_drc, Vector2 &r_window) {
	if (!last_drc_blit.valid || last_drc_blit.drc_rect.size.x <= 0 || last_drc_blit.drc_rect.size.y <= 0) {
		// No blit yet: scale the GamePad screen to the 1280x720 window.
		r_window = p_drc * (1280.0f / 854.0f);
		return true;
	}
	const Rect2 &drc = last_drc_blit.drc_rect;
	if (!drc.has_point(p_drc)) {
		return false;
	}
	// GamePad pixel -> render-target pixel -> window pixel.
	const Vector2 rt = (p_drc - drc.position) * last_drc_blit.rt_size / drc.size;
	const Rect2 &win = last_drc_blit.window_rect;
	if (win.size.x > 0 && win.size.y > 0) {
		r_window = win.position + rt * win.size / last_drc_blit.rt_size;
	} else {
		r_window = rt;
	}
	return true;
}

RasterizerWiiU::RasterizerWiiU() {
	// Swap the dummy pieces that need to be real. Deleting the old ones first
	// lets the new ones register as their singletons.
	memdelete(canvas);
	canvas = memnew(RasterizerCanvasWiiU);
	memdelete(texture_storage);
	texture_storage = memnew(TextureStorageWiiU);
	memdelete(utilities);
	utilities = memnew(UtilitiesWiiU);
	memdelete(mesh_storage);
	mesh_storage = memnew(MeshStorageWiiU);
	memdelete(material_storage);
	material_storage = memnew(MaterialStorageWiiU);
}

void RasterizerWiiU::begin_frame(double p_frame_step) {
	RasterizerDummy::begin_frame(p_frame_step);
	wiiu_stats_mark(1); // Rendering starts.
	wiiu_gx2_frame_begin();
	blitted = false;
}

// Fits the render targets onto one screen: the largest whole-number scale
// that fits (crisp pixels, nearest filtering), or a smooth downscale if the
// target is bigger than the screen. Ignores Godot's window rect on purpose,
// so the TV and the GamePad each get their own fit.
void RasterizerWiiU::_blit_to_screen(WiiUScreen p_screen, const RenderingServerTypes::BlitToScreen *p_render_targets, int p_amount) {
	TextureStorageWiiU *ts = TextureStorageWiiU::get_wiiu_singleton();
	RasterizerCanvasWiiU *cv = static_cast<RasterizerCanvasWiiU *>(canvas);
	uint32_t sw, sh;
	wiiu_gx2_screen_size(p_screen, &sw, &sh);
	wiiu_gx2_bind_screen(p_screen);
	wiiu_gx2_clear(0, 0, 0, 1);
	for (int i = 0; i < p_amount; i++) {
		TextureStorageWiiU::RenderTarget *rt = ts->get_render_target(p_render_targets[i].render_target);
		if (rt == nullptr || rt->gpu == nullptr) {
			continue;
		}
		const float rw = (float)rt->size.x;
		const float rh = (float)rt->size.y;
		float scale = MIN((float)sw / rw, (float)sh / rh);
		bool linear = true;
		if (scale >= 1.0f) {
			scale = Math::floor(scale);
			linear = false;
		}
		const Size2 size(Math::round(rw * scale), Math::round(rh * scale));
		const Point2 pos(Math::floor(((float)sw - size.x) * 0.5f), Math::floor(((float)sh - size.y) * 0.5f));
		cv->draw_texture_rect(wiiu_gx2_render_target_texture(rt->gpu), Rect2(pos, size), p_render_targets[i].src_rect, linear);
		if (p_screen == WIIU_SCREEN_DRC && i == 0) {
			last_drc_blit.valid = true;
			last_drc_blit.rt_size = Size2(rw, rh);
			last_drc_blit.drc_rect = Rect2(pos, size);
			last_drc_blit.window_rect = Rect2(p_render_targets[i].dst_rect);
		}
	}
}

void RasterizerWiiU::blit_render_targets_to_screen(int p_screen, const RenderingServerTypes::BlitToScreen *p_render_targets, int p_amount) {
	if (!wiiu_gx2_can_draw()) {
		return;
	}
	_blit_to_screen(WIIU_SCREEN_TV, p_render_targets, p_amount);
	_blit_to_screen(WIIU_SCREEN_DRC, p_render_targets, p_amount);
	blitted = true;
}

// crash_log.c: the log overlay toggled by ZL+L+RIGHT+X+Y+TV.
extern "C" bool wiiu_log_overlay_visible(void);
extern "C" void wiiu_draw_log_gx2(WiiUScreen p_screen, float p_r, float p_g, float p_b, float p_a, const char *p_footer);
// wiiu_stats.cpp: the stats overlay toggled by ZR+R+LEFT+Y+B+L.
extern "C" void wiiu_stats_frame(void);
extern "C" bool wiiu_stats_visible(void);
extern "C" void wiiu_stats_draw(WiiUScreen p_screen);

void RasterizerWiiU::end_frame(bool p_present) {
	if (!wiiu_gx2_can_draw()) {
		return;
	}
	wiiu_stats_mark(2); // Rendering done, presenting next.
	wiiu_stats_frame();
	if (wiiu_stats_visible() && blitted) {
		for (int s = 0; s < 2; s++) {
			const WiiUScreen screen = s == 0 ? WIIU_SCREEN_TV : WIIU_SCREEN_DRC;
			wiiu_gx2_bind_screen(screen);
			wiiu_stats_draw(screen);
		}
	}
	if (wiiu_log_overlay_visible()) {
		for (int s = 0; s < 2; s++) {
			const WiiUScreen screen = s == 0 ? WIIU_SCREEN_TV : WIIU_SCREEN_DRC;
			wiiu_gx2_bind_screen(screen);
			wiiu_draw_log_gx2(screen, 0, 0, 0, 0.8f, "Press B to close the log. Up/Down/L/R scroll.");
		}
	}
	if (!blitted) {
		// Nothing was attached to the screen this frame: show black, not garbage.
		wiiu_gx2_bind_screen(WIIU_SCREEN_TV);
		wiiu_gx2_clear(0, 0, 0, 1);
		wiiu_gx2_bind_screen(WIIU_SCREEN_DRC);
		wiiu_gx2_clear(0, 0, 0, 1);
	}
	if (p_present) {
		wiiu_gx2_frame_present();
	}
}
