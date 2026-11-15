#include "rasterizer_wiiu.h"

#include "core/io/image.h"
#include "core/math/math_funcs.h"
#include "core/os/os.h"
#include "scene/resources/texture.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_default.h"

// ===========================================================================
// MeshStorageWiiU
// ===========================================================================

MeshStorageWiiU::MeshStorageWiiU() {
	wiiu_singleton = this;
}

MeshStorageWiiU::~MeshStorageWiiU() {
	for (KeyValue<RID, LocalVector<Surface>> &E : mesh_surfaces) {
		_free_gpu(E.value);
	}
	wiiu_singleton = nullptr;
}

// The 3D copy of a surface (WiiUVertex3D in GPU memory): positions, normals
// (or none: facing +Z), UVs and colours (white without them).
static void _upload_3d(MeshStorageWiiU::Surface &r_surface, const Array &p_arrays, const PackedVector3Array &p_vertices) {
	const int count = p_vertices.size();
	LocalVector<WiiUVertex3D> out;
	out.resize(count);
	PackedVector3Array normals;
	if (p_arrays[RSE::ARRAY_NORMAL].get_type() == Variant::PACKED_VECTOR3_ARRAY) {
		normals = p_arrays[RSE::ARRAY_NORMAL];
	}
	PackedVector2Array uvs;
	if (p_arrays[RSE::ARRAY_TEX_UV].get_type() == Variant::PACKED_VECTOR2_ARRAY) {
		uvs = p_arrays[RSE::ARRAY_TEX_UV];
	}
	PackedColorArray colors;
	if (p_arrays[RSE::ARRAY_COLOR].get_type() == Variant::PACKED_COLOR_ARRAY) {
		colors = p_arrays[RSE::ARRAY_COLOR];
	}
	const bool has_normals = normals.size() == count;
	const bool has_uvs = uvs.size() == count;
	const bool has_colors = colors.size() == count;
	for (int i = 0; i < count; i++) {
		WiiUVertex3D &o = out[i];
		o.x = p_vertices[i].x;
		o.y = p_vertices[i].y;
		o.z = p_vertices[i].z;
		const Vector3 n = has_normals ? normals[i] : Vector3(0, 0, 1);
		o.nx = n.x;
		o.ny = n.y;
		o.nz = n.z;
		o.u = has_uvs ? uvs[i].x : 0.0f;
		o.v = has_uvs ? uvs[i].y : 0.0f;
		const Color c = has_colors ? colors[i] : Color(1, 1, 1, 1);
		o.r = c.r;
		o.g = c.g;
		o.b = c.b;
		o.a = c.a;
	}
	LocalVector<uint32_t> indices;
	if (p_arrays[RSE::ARRAY_INDEX].get_type() == Variant::PACKED_INT32_ARRAY) {
		const PackedInt32Array idx = p_arrays[RSE::ARRAY_INDEX];
		indices.resize(idx.size());
		for (int i = 0; i < idx.size(); i++) {
			indices[i] = (uint32_t)idx[i];
		}
	}
	const WiiUPrimitive prim = r_surface.primitive == RSE::PRIMITIVE_LINES ? WIIU_PRIM_LINES : WIIU_PRIM_TRIANGLES;
	r_surface.gpu = wiiu_gx2_mesh_create(prim, out.ptr(), (uint32_t)count, indices.size() ? indices.ptr() : nullptr,
			indices.size());
}

void MeshStorageWiiU::_free_gpu(LocalVector<Surface> &p_surfaces) {
	for (Surface &s : p_surfaces) {
		wiiu_gx2_mesh_free(s.gpu);
		s.gpu = nullptr;
	}
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
			if (v.size() > 0) {
				s.aabb = AABB(v[0], Vector3());
				for (int i = 1; i < v.size(); i++) {
					s.aabb.expand_to(v[i]);
				}
				s.has_aabb = true;
			}
			if (p_surface.primitive == RSE::PRIMITIVE_TRIANGLES || p_surface.primitive == RSE::PRIMITIVE_LINES) {
				_upload_3d(s, arrays, v);
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
	if (LocalVector<Surface> *surfaces = mesh_surfaces.getptr(p_rid)) {
		_free_gpu(*surfaces);
	}
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
		wiiu_gx2_mesh_free((*surfaces)[p_surface].gpu);
		surfaces->remove_at(p_surface);
	}
}

void MeshStorageWiiU::mesh_clear(RID p_mesh) {
	RendererDummy::MeshStorage::mesh_clear(p_mesh);
	if (LocalVector<Surface> *surfaces = mesh_surfaces.getptr(p_mesh)) {
		_free_gpu(*surfaces);
	}
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
	// 3D surfaces: their 3D bounds (the scene culls by them).
	bool has_3d = false;
	AABB aabb;
	for (const Surface &surface : *surfaces) {
		if (surface.has_aabb) {
			aabb = has_3d ? aabb.merge(surface.aabb) : surface.aabb;
			has_3d = true;
		}
	}
	if (has_3d) {
		return aabb;
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
