#include "rasterizer_wiiu.h"

#include "core/io/image.h"
#include "core/math/math_funcs.h"
#include "core/os/os.h"
#include "scene/resources/texture.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_default.h"

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
		WiiUDrawState state = WIIU_DRAW_STATE_DEFAULT;
		state.blend = batch_blend;
		state.msdf_px_range = batch_msdf_range;
		state.msdf_outline = batch_msdf_outline;
		state.msdf_px_size = batch_msdf_px_size;
		MaterialStorageWiiU *mats = MaterialStorageWiiU::get_wiiu_singleton();
		if (batch_material.is_valid() && mats && mats->get_material_program(batch_material, batch_linear, batch_repeat, state)) {
			state.time = RSG::rasterizer ? (float)RSG::rasterizer->get_total_time() : 0.0f;
			state.screen_texture = screen_texture;
			if (state.local_vertices) {
				const Transform2D &m = batch_local_xform;
				state.model[0] = m.columns[0].x;
				state.model[1] = m.columns[0].y;
				state.model[2] = m.columns[1].x;
				state.model[3] = m.columns[1].y;
				state.model[4] = m.columns[2].x;
				state.model[5] = m.columns[2].y;
			}
		}
		wiiu_gx2_draw(&state, batch_primitive, batch_texture, batch_linear, batch_repeat, batch.ptr() + start, count);
	}
	batch.clear();
}

void RasterizerCanvasWiiU::_begin(WiiUPrimitive p_primitive, WiiUTexture *p_texture, int p_linear, bool p_repeat) {
	if (p_primitive != batch_primitive || p_texture != batch_texture || p_linear != batch_linear || p_repeat != batch_repeat || blend != batch_blend ||
			msdf_range != batch_msdf_range || msdf_outline != batch_msdf_outline || msdf_px_size != batch_msdf_px_size ||
			shader_material != batch_material || local_item != batch_local_item) {
		_flush();
		batch_material = shader_material;
		batch_local_item = local_item;
		batch_local_xform = local_xform;
		batch_primitive = p_primitive;
		batch_texture = p_texture;
		batch_linear = p_linear;
		batch_repeat = p_repeat;
		batch_blend = blend;
		batch_msdf_range = msdf_range;
		batch_msdf_outline = msdf_outline;
		batch_msdf_px_size = msdf_px_size;
	}
}

void RasterizerCanvasWiiU::_vertex(const Vector2 &p_pos, const Vector2 &p_uv, const Color &p_color) {
	WiiUVertex v;
	// Callers pass render-target pixels; a vertex() shader wants the item's
	// local space (VERTEX), so undo the item transform for those.
	const Vector2 pos = local_item ? local_inverse.xform(p_pos) : p_pos;
	v.x = pos.x;
	v.y = pos.y;
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
	shader_material = RID();
	local_item = 0;
	msdf_range = 0.0f;
	msdf_outline = 0.0f;
	msdf_px_size = 1.0f;
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
	const Size2i rt_size = rt->size;
	Item *current_clip = nullptr;
	screen_texture = nullptr;
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
			shader_material = (mats && material.is_valid() && mats->material_has_program(material)) ? material : RID();
			// Screen texture: copy what's drawn so far, as drivers/gles3 does:
			// once per canvas for the first shader that reads it, and again at
			// every BackBufferCopy node (always the whole target here).
			// vertex() shaders: local vertices, the item transform on the GPU.
			local_item = 0;
			if (shader_material.is_valid() && mats->material_uses_local_vertices(shader_material)) {
				local_xform = ci->final_transform;
				if (Math::is_zero_approx(local_xform.determinant())) {
					shader_material = RID(); // Zero-size item: no local space; draws nothing anyway.
				} else {
					local_inverse = local_xform.affine_inverse();
					local_item = ++local_item_counter;
				}
			}
			const bool wants_screen = shader_material.is_valid() && screen_texture == nullptr && mats->material_uses_screen_texture(shader_material);
			if (ci->copy_back_buffer || wants_screen) {
				_flush();
				screen_texture = wiiu_gx2_render_target_copy_back_buffer(rt->gpu);
			}
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
				msdf_range = 0.0f; // Only TYPE_RECT with CANVAS_RECT_MSDF sets it.
				msdf_outline = 0.0f;
				msdf_px_size = 1.0f;
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
						// MSDF glyphs: the shader turns the distance field into coverage.
						msdf_range = (rect->flags & CANVAS_RECT_MSDF) ? MAX(rect->px_range, 0.0001f) : 0.0f;
						msdf_outline = (rect->flags & CANVAS_RECT_MSDF) ? rect->outline : 0.0f;
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
						if (msdf_range > 0.0f && textured) {
							// Screen pixels per texel along each axis (GLES3 gets
							// this from fwidth(uv) in the shader).
							const float tex_w = MAX(Math::abs(u1 - u0) * tsize.x, 0.0001f);
							const float tex_h = MAX(Math::abs(v1 - v0) * tsize.y, 0.0001f);
							const float sx = xform.columns[0].length() * dst.size.x / tex_w;
							const float sy = xform.columns[1].length() * dst.size.y / tex_h;
							// Quantized so neighbouring glyphs share a batch.
							msdf_px_size = Math::snapped(MAX(0.5f * msdf_range * (sx + sy), 1.0f), 0.25f);
							_begin(WIIU_PRIM_TRIANGLES, batch_texture, batch_linear, batch_repeat);
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
