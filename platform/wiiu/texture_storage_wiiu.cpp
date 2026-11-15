#include "rasterizer_wiiu.h"

#include "core/io/image.h"
#include "core/math/math_funcs.h"
#include "core/os/os.h"
#include "scene/resources/texture.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_default.h"

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
