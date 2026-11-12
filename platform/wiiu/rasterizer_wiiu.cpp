#include "rasterizer_wiiu.h"

#include "core/io/image.h"
#include "core/math/math_funcs.h"

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

// Converts to RGBA8 (level 0 only) and uploads, recreating the GX2 texture if
// the size changed.
void TextureStorageWiiU::_upload(Texture *p_texture, const Ref<Image> &p_image) {
	ERR_FAIL_COND(p_image.is_null() || p_image->is_empty());
	Ref<Image> img = p_image->duplicate();
	if (img->is_compressed()) {
		img->decompress();
	}
	if (img->has_mipmaps()) {
		img->clear_mipmaps();
	}
	if (img->get_format() != Image::FORMAT_RGBA8) {
		img->convert(Image::FORMAT_RGBA8);
	}
	const int w = img->get_width();
	const int h = img->get_height();
	Vector<uint8_t> data = img->get_data();

	if (p_texture->gpu && (p_texture->width != w || p_texture->height != h)) {
		_release_gpu(p_texture);
	}
	if (p_texture->gpu) {
		wiiu_gx2_texture_update(p_texture->gpu, data.ptr());
	} else {
		p_texture->gpu = wiiu_gx2_texture_create(w, h, data.ptr());
	}
	p_texture->width = w;
	p_texture->height = h;
	p_texture->format = p_image->get_format();
	p_texture->image = p_image;
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
	if (batch.size() > 0) {
		wiiu_gx2_draw(batch_primitive, batch_texture, batch_linear, batch_repeat, batch.ptr(), batch.size());
		batch.clear();
	}
}

void RasterizerCanvasWiiU::_begin(WiiUPrimitive p_primitive, WiiUTexture *p_texture, bool p_linear, bool p_repeat) {
	if (p_primitive != batch_primitive || p_texture != batch_texture || p_linear != batch_linear || p_repeat != batch_repeat) {
		_flush();
		batch_primitive = p_primitive;
		batch_texture = p_texture;
		batch_linear = p_linear;
		batch_repeat = p_repeat;
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

static bool filter_is_linear(RSE::CanvasItemTextureFilter p_filter) {
	return p_filter == RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR ||
			p_filter == RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS ||
			p_filter == RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC;
}

void RasterizerCanvasWiiU::canvas_render_items(RID p_to_render_target, Item *p_item_list, const Color &p_modulate, Light *p_light_list, Light *p_directional_list, const Transform2D &p_canvas_transform, RSE::CanvasItemTextureFilter p_default_filter, RSE::CanvasItemTextureRepeat p_default_repeat, bool p_snap_2d_vertices_to_pixel, bool &r_sdf_used, RenderingServerTypes::RenderInfo *r_render_info) {
	TextureStorageWiiU *ts = TextureStorageWiiU::get_wiiu_singleton();
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
		// final_transform already maps the item into render-target pixels.
		const Transform2D base_xform = ci->final_transform;
		Transform2D xform = base_xform;
		const Color base_color = ci->final_modulate * p_modulate;

		// Picks texture and sampler state for a draw command.
		auto use_texture = [&](RID p_texture, WiiUPrimitive p_primitive, Size2 &r_size) {
			TextureStorageWiiU::Resolved res = ts->resolve(p_texture);
			const RSE::CanvasItemTextureFilter f = res.filter == RSE::CANVAS_ITEM_TEXTURE_FILTER_DEFAULT ? item_filter : res.filter;
			const RSE::CanvasItemTextureRepeat rep = res.repeat == RSE::CANVAS_ITEM_TEXTURE_REPEAT_DEFAULT ? item_repeat : res.repeat;
			_begin(p_primitive, res.texture, filter_is_linear(f), rep != RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED && rep != RSE::CANVAS_ITEM_TEXTURE_REPEAT_DEFAULT);
			r_size = res.size;
			return res.texture != nullptr;
		};

		for (const Item::Command *c = ci->commands; c; c = c->next) {
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
					// Tile modes are drawn stretched for now.
					for (int y = 0; y < 3; y++) {
						for (int x = 0; x < 3; x++) {
							if (x == 1 && y == 1 && !np->draw_center) {
								continue;
							}
							const Vector2 pos[4] = {
								Vector2(xs[x], ys[y]), Vector2(xs[x + 1], ys[y]),
								Vector2(xs[x + 1], ys[y + 1]), Vector2(xs[x], ys[y + 1])
							};
							const Vector2 uv[4] = {
								Vector2(us[x], vs[y]), Vector2(us[x + 1], vs[y]),
								Vector2(us[x + 1], vs[y + 1]), Vector2(us[x], vs[y + 1])
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

				default:
					// Meshes, multimeshes, particles, animation slices: not supported yet.
					break;
			}
		}
	}
	_flush();
}

// ===========================================================================
// RasterizerWiiU
// ===========================================================================

RasterizerWiiU::RasterizerWiiU() {
	// Swap the dummy pieces that need to be real. Deleting the old ones first
	// lets the new ones register as their singletons.
	memdelete(canvas);
	canvas = memnew(RasterizerCanvasWiiU);
	memdelete(texture_storage);
	texture_storage = memnew(TextureStorageWiiU);
	memdelete(utilities);
	utilities = memnew(UtilitiesWiiU);
}

void RasterizerWiiU::begin_frame(double p_frame_step) {
	RasterizerDummy::begin_frame(p_frame_step);
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

void RasterizerWiiU::end_frame(bool p_present) {
	if (!wiiu_gx2_can_draw()) {
		return;
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
