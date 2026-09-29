/**************************************************************************/
/*  landscape_foliage_3d.cpp                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "landscape_foliage_3d.h"

#include "landscape_3d.h"

#include "core/config/engine.h"
#include "core/io/compression.h"
#include "core/io/marshalls.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/templates/hashfuncs.h"
#include "scene/3d/camera_3d.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/world_3d.h"
#include "scene/resources/material.h"
#include "scene/resources/shader.h"
#include "servers/rendering/rendering_server.h"

// Serialized instances: header, then the zstd-compressed byte planes of the instance floats.
static constexpr uint32_t FOLIAGE_DATA_MAGIC = 0x494F464C; // "LFOI"
static constexpr uint32_t FOLIAGE_DATA_VERSION = 1;
static constexpr int FOLIAGE_DATA_HEADER = 16;
// Level of an instance whose level is not known yet (per-instance sort).
static constexpr uint8_t FOLIAGE_LOD_UNKNOWN = 254;
// Candidates evaluated by a paint dab (the density is estimated from at least the minimum).
static constexpr int MIN_PAINT_CANDIDATES = 16;
static constexpr int MAX_PAINT_CANDIDATES = 65536;
// Camera move that triggers a new evaluation of the cells.
static constexpr real_t MIN_CAMERA_STEP = 0.25;

Ref<Shader> LandscapeFoliage3D::debug_shader;
Ref<ShaderMaterial> LandscapeFoliage3D::debug_material;
Ref<StandardMaterial3D> LandscapeFoliage3D::debug_bounds_material;

/* Instance */

Transform3D LandscapeFoliage3D::Instance::get_transform() const {
	return Transform3D(xform[0], xform[1], xform[2], xform[4], xform[5], xform[6], xform[8], xform[9], xform[10], xform[3], xform[7], xform[11]);
}

void LandscapeFoliage3D::Instance::set_transform(const Transform3D &p_transform) {
	const Basis &b = p_transform.basis;
	xform[0] = b.rows[0].x;
	xform[1] = b.rows[0].y;
	xform[2] = b.rows[0].z;
	xform[3] = p_transform.origin.x;
	xform[4] = b.rows[1].x;
	xform[5] = b.rows[1].y;
	xform[6] = b.rows[1].z;
	xform[7] = p_transform.origin.y;
	xform[8] = b.rows[2].x;
	xform[9] = b.rows[2].y;
	xform[10] = b.rows[2].z;
	xform[11] = p_transform.origin.z;
}

Vector3 LandscapeFoliage3D::Instance::get_align_normal() const {
	const float y2 = 1.0f - normal_x * normal_x - normal_z * normal_z;
	return Vector3(normal_x, Math::sqrt(MAX(y2, 0.0f)), normal_z);
}

void LandscapeFoliage3D::Instance::set_align_normal(const Vector3 &p_normal) {
	Vector3 n = p_normal.normalized();
	if (n.y < 0.0) {
		n = -n;
	}
	normal_x = n.x;
	normal_z = n.z;
}

/* Helpers */

namespace {

// Minimum distance between the instances of a type (UE "Radius").
struct FoliageSpacingGrid {
	real_t radius = 0.0;
	HashMap<Vector2i, LocalVector<Vector2>> cells;

	Vector2i key(const Vector2 &p_position) const {
		return Vector2i(int(Math::floor(p_position.x / radius)), int(Math::floor(p_position.y / radius)));
	}
	bool is_free(const Vector2 &p_position) const {
		if (radius <= 0.0) {
			return true;
		}
		const Vector2i k = key(p_position);
		const real_t radius_sq = radius * radius;
		for (int z = -1; z <= 1; z++) {
			for (int x = -1; x <= 1; x++) {
				const LocalVector<Vector2> *points = cells.getptr(k + Vector2i(x, z));
				if (!points) {
					continue;
				}
				for (const Vector2 &p : *points) {
					if (p.distance_squared_to(p_position) < radius_sq) {
						return false;
					}
				}
			}
		}
		return true;
	}
	void add(const Vector2 &p_position) {
		if (radius > 0.0) {
			cells[key(p_position)].push_back(p_position);
		}
	}
};

bool _rect_has_point(const Rect2 &p_rect, const Vector2 &p_point) {
	// Half-open, so that adjacent rects don't share points.
	return p_point.x >= p_rect.position.x && p_point.y >= p_rect.position.y && p_point.x < p_rect.position.x + p_rect.size.x && p_point.y < p_rect.position.y + p_rect.size.y;
}

} // namespace

/* Types */

LandscapeFoliage3D::Entry *LandscapeFoliage3D::_get_entry(int p_type_index) const {
	ERR_FAIL_INDEX_V(p_type_index, int(entries.size()), nullptr);
	return entries[p_type_index];
}

void LandscapeFoliage3D::_connect_type(Entry *p_entry, bool p_connect) {
	if (p_entry->type.is_null()) {
		return;
	}
	// A type may be used by several entries: reference counted connections.
	const Callable callback = callable_mp(this, &LandscapeFoliage3D::_types_changed);
	if (p_connect) {
		p_entry->type->connect(CoreStringName(changed), callback, CONNECT_REFERENCE_COUNTED);
	} else if (p_entry->type->is_connected(CoreStringName(changed), callback)) {
		p_entry->type->disconnect(CoreStringName(changed), callback);
	}
}

void LandscapeFoliage3D::_types_changed() {
	// Only what the rendering depends on (meshes, LODs, material) rebuilds the cells.
	for (Entry *entry : entries) {
		const uint64_t hash = entry->type.is_valid() ? entry->type->get_render_hash() : 0;
		if (hash == entry->render_hash) {
			continue;
		}
		entry->render_hash = hash;
		_update_entry_lods(entry);
		for (KeyValue<Vector2i, Cell> &kv : entry->cells) {
			_free_cell_rendering(kv.value);
			kv.value.bounds_dirty = true;
		}
		render_pending = true;
	}
	update_configuration_warnings();
}

void LandscapeFoliage3D::_update_entry_lods(Entry *p_entry) {
	p_entry->lod_count = 0;
	p_entry->cull_end = 0.0;
	p_entry->cull_begin = 0.0;
	p_entry->transition = 0.0;
	p_entry->mesh_bounds = AABB();
	if (p_entry->type.is_null()) {
		return;
	}
	const LandscapeFoliageType *type = p_entry->type.ptr();
	p_entry->lod_count = MIN(type->get_lod_count(), LandscapeFoliageType::MAX_LODS);
	float previous = 0.0;
	for (int i = 0; i < p_entry->lod_count; i++) {
		// Levels are used in order, whatever the distances.
		const float start = i == 0 ? 0.0f : MAX(type->get_lod_start_distance(i) * lod_distance_scale, previous);
		p_entry->lod_starts[i] = start;
		previous = start;
		const Ref<Mesh> mesh = type->get_lod_effective_mesh(i);
		p_entry->lod_meshes[i] = mesh.is_valid() ? mesh->get_rid() : RID();
		p_entry->lod_shadows[i] = type->is_lod_casting_shadows(i);
	}
	p_entry->cull_end = type->get_cull_distance() * lod_distance_scale;
	p_entry->cull_begin = p_entry->cull_end * (1.0f - type->get_cull_random());
	p_entry->transition = type->get_lod_transition();
	p_entry->mesh_bounds = type->get_mesh_bounds();
}

int LandscapeFoliage3D::add_foliage_type(const Ref<LandscapeFoliageType> &p_type) {
	Entry *entry = memnew(Entry);
	entry->type = p_type;
	entry->render_hash = p_type.is_valid() ? p_type->get_render_hash() : 0;
	_connect_type(entry, true);
	_update_entry_lods(entry);
	entries.push_back(entry);
	render_pending = true;
	notify_property_list_changed();
	update_configuration_warnings();
	return entries.size() - 1;
}

void LandscapeFoliage3D::remove_foliage_type(int p_index) {
	Entry *entry = _get_entry(p_index);
	ERR_FAIL_NULL(entry);
	_connect_type(entry, false);
	for (KeyValue<Vector2i, Cell> &kv : entry->cells) {
		_free_cell_rendering(kv.value);
	}
	memdelete(entry);
	entries.remove_at(p_index);
	notify_property_list_changed();
	update_configuration_warnings();
}

void LandscapeFoliage3D::set_foliage_type(int p_index, const Ref<LandscapeFoliageType> &p_type) {
	Entry *entry = _get_entry(p_index);
	ERR_FAIL_NULL(entry);
	if (entry->type == p_type) {
		return;
	}
	_connect_type(entry, false);
	entry->type = p_type;
	entry->render_hash = p_type.is_valid() ? p_type->get_render_hash() : 0;
	_connect_type(entry, true);
	_update_entry_lods(entry);
	for (KeyValue<Vector2i, Cell> &kv : entry->cells) {
		_free_cell_rendering(kv.value);
		kv.value.bounds_dirty = true;
	}
	render_pending = true;
	notify_property_list_changed();
	update_configuration_warnings();
}

Ref<LandscapeFoliageType> LandscapeFoliage3D::get_foliage_type(int p_index) const {
	const Entry *entry = _get_entry(p_index);
	ERR_FAIL_NULL_V(entry, Ref<LandscapeFoliageType>());
	return entry->type;
}

int LandscapeFoliage3D::find_foliage_type(const Ref<LandscapeFoliageType> &p_type) const {
	for (uint32_t i = 0; i < entries.size(); i++) {
		if (entries[i]->type == p_type) {
			return i;
		}
	}
	return -1;
}

void LandscapeFoliage3D::move_foliage_type(int p_from, int p_to) {
	ERR_FAIL_INDEX(p_from, int(entries.size()));
	ERR_FAIL_INDEX(p_to, int(entries.size()));
	if (p_from == p_to) {
		return;
	}
	Entry *entry = entries[p_from];
	entries.remove_at(p_from);
	entries.insert(p_to, entry);
	notify_property_list_changed();
}

/* Cells */

Vector2i LandscapeFoliage3D::_get_cell_key(const Vector3 &p_position) const {
	return Vector2i(int(Math::floor(p_position.x / chunk_size)), int(Math::floor(p_position.z / chunk_size)));
}

Rect2 LandscapeFoliage3D::_get_cell_rect(const Vector2i &p_key) const {
	return Rect2(Vector2(p_key) * chunk_size, Vector2(chunk_size, chunk_size));
}

LandscapeFoliage3D::Cell &LandscapeFoliage3D::_get_or_create_cell(Entry *p_entry, const Vector2i &p_key) {
	Cell *cell = p_entry->cells.getptr(p_key);
	if (!cell) {
		cell = &p_entry->cells.insert(p_key, Cell())->value;
		cell->key = p_key;
	}
	return *cell;
}

void LandscapeFoliage3D::_cell_changed(Entry *p_entry, Cell &r_cell) {
	r_cell.render_dirty = true;
	r_cell.bounds_dirty = true;
	r_cell.state = STATE_NONE;
	r_cell.instance_lods.clear();
	p_entry->serialized_dirty = true;
	render_pending = true;
}

void LandscapeFoliage3D::_add_instance(Entry *p_entry, const Instance &p_instance) {
	Cell &cell = _get_or_create_cell(p_entry, _get_cell_key(p_instance.get_position()));
	cell.instances.push_back(p_instance);
	p_entry->count++;
	_cell_changed(p_entry, cell);
}

void LandscapeFoliage3D::_update_cell_bounds(Entry *p_entry, Cell &r_cell) {
	if (!r_cell.bounds_dirty) {
		return;
	}
	r_cell.bounds_dirty = false;
	if (r_cell.instances.is_empty()) {
		r_cell.bounds = AABB();
		return;
	}
	// Positions, grown by the farthest point of the meshes at the largest scale.
	AABB positions(r_cell.instances[0].get_position(), Vector3());
	float max_scale_sq = 0.0;
	for (const Instance &instance : r_cell.instances) {
		positions.expand_to(instance.get_position());
		const float *x = instance.xform;
		for (int c = 0; c < 3; c++) {
			max_scale_sq = MAX(max_scale_sq, x[c] * x[c] + x[4 + c] * x[4 + c] + x[8 + c] * x[8 + c]);
		}
	}
	real_t reach = 1.0;
	if (p_entry->mesh_bounds.has_volume() || p_entry->mesh_bounds.has_surface()) {
		const AABB &mesh = p_entry->mesh_bounds;
		const Vector3 far = mesh.position.abs().max((mesh.position + mesh.size).abs());
		reach = far.length();
	}
	r_cell.bounds = positions.grow(reach * Math::sqrt(max_scale_sq) + 0.01);
}

template <typename F>
void LandscapeFoliage3D::_for_each_cell_in_rect(Entry *p_entry, const Rect2 &p_rect, F p_function) {
	const Vector2i begin(int(Math::floor(p_rect.position.x / chunk_size)), int(Math::floor(p_rect.position.y / chunk_size)));
	const Vector2i end(int(Math::floor((p_rect.position.x + p_rect.size.x) / chunk_size)), int(Math::floor((p_rect.position.y + p_rect.size.y) / chunk_size)));
	const int64_t keys = int64_t(end.x - begin.x + 1) * int64_t(end.y - begin.y + 1);
	if (keys > int64_t(p_entry->cells.size())) {
		for (KeyValue<Vector2i, Cell> &kv : p_entry->cells) {
			if (kv.key.x >= begin.x && kv.key.x <= end.x && kv.key.y >= begin.y && kv.key.y <= end.y) {
				p_function(kv.key, kv.value);
			}
		}
		return;
	}
	for (int z = begin.y; z <= end.y; z++) {
		for (int x = begin.x; x <= end.x; x++) {
			const Vector2i key(x, z);
			Cell *cell = p_entry->cells.getptr(key);
			if (cell) {
				p_function(key, *cell);
			}
		}
	}
}

void LandscapeFoliage3D::_remove_empty_cells(Entry *p_entry) {
	LocalVector<Vector2i> empty;
	for (KeyValue<Vector2i, Cell> &kv : p_entry->cells) {
		if (kv.value.instances.is_empty()) {
			_free_cell_rendering(kv.value);
			empty.push_back(kv.key);
		}
	}
	for (const Vector2i &key : empty) {
		p_entry->cells.erase(key);
	}
}

void LandscapeFoliage3D::_rebin_all() {
	for (Entry *entry : entries) {
		LocalVector<Instance> all;
		all.reserve(entry->count);
		for (KeyValue<Vector2i, Cell> &kv : entry->cells) {
			_free_cell_rendering(kv.value);
			for (const Instance &instance : kv.value.instances) {
				all.push_back(instance);
			}
		}
		entry->cells.clear();
		entry->count = 0;
		for (const Instance &instance : all) {
			_add_instance(entry, instance);
		}
		entry->serialized_dirty = true;
	}
	render_pending = true;
}

/* Terrain */

bool LandscapeFoliage3D::_has_terrain() const {
	return landscape && landscape->get_data().is_valid() && landscape->get_data()->is_valid();
}

bool LandscapeFoliage3D::_sample_ground(const Vector2 &p_position, real_t &r_height, Vector3 &r_normal, float *r_weights, bool &r_hole) const {
	if (!_has_terrain()) {
		return false;
	}
	const Ref<LandscapeData> terrain = landscape->get_data();
	const Vector2 world = terrain->get_world_size();
	if (p_position.x < 0.0 || p_position.y < 0.0 || p_position.x > world.x || p_position.y > world.y) {
		return false;
	}
	r_height = terrain->sample_height(p_position.x, p_position.y);
	r_normal = terrain->sample_normal(p_position.x, p_position.y);
	const real_t spacing = terrain->get_vertex_spacing();
	const Vector2 texel = p_position / spacing;
	r_hole = terrain->has_holes() && terrain->is_hole(int(Math::round(texel.x)), int(Math::round(texel.y)));
	if (r_weights) {
		// Bilinear weights of the four closest texels.
		const Vector2i size = terrain->get_size();
		const int x0 = CLAMP(int(Math::floor(texel.x)), 0, size.x - 1);
		const int z0 = CLAMP(int(Math::floor(texel.y)), 0, size.y - 1);
		const int x1 = MIN(x0 + 1, size.x - 1);
		const int z1 = MIN(z0 + 1, size.y - 1);
		const float tx = CLAMP(float(texel.x - x0), 0.0f, 1.0f);
		const float tz = CLAMP(float(texel.y - z0), 0.0f, 1.0f);
		float w[4][LandscapeData::MAX_LAYERS];
		terrain->get_weights(x0, z0, w[0]);
		terrain->get_weights(x1, z0, w[1]);
		terrain->get_weights(x0, z1, w[2]);
		terrain->get_weights(x1, z1, w[3]);
		for (int i = 0; i < LandscapeData::MAX_LAYERS; i++) {
			r_weights[i] = Math::lerp(Math::lerp(w[0][i], w[1][i], tx), Math::lerp(w[2][i], w[3][i], tx), tz);
		}
	}
	return true;
}

bool LandscapeFoliage3D::_make_instance(const LandscapeFoliageType *p_type, const Vector2 &p_position, bool p_filter, Instance &r_instance) {
	real_t height;
	Vector3 normal;
	bool hole;
	float weights[LandscapeData::MAX_LAYERS];
	const bool use_weights = p_filter && p_type->uses_layers();
	if (!_sample_ground(p_position, height, normal, use_weights ? weights : nullptr, hole) || hole) {
		return false;
	}
	if (p_filter) {
		const real_t global_height = landscape->local_to_global(Vector3(p_position.x, height, p_position.y)).y;
		if (!p_type->accepts(normal, global_height, use_weights ? weights : nullptr)) {
			return false;
		}
	}
	const LandscapeFoliageType::Placement placement = p_type->generate(Vector3(p_position.x, height, p_position.y), normal, rng);
	r_instance.set_transform(placement.transform);
	r_instance.offset = placement.offset;
	r_instance.random = placement.random;
	r_instance.set_align_normal(placement.align_normal);
	return true;
}

void LandscapeFoliage3D::_snap_instance(const LandscapeFoliageType *p_type, Instance &r_instance) const {
	const Vector3 position = r_instance.get_position();
	real_t height;
	Vector3 normal;
	bool hole;
	if (!_sample_ground(Vector2(position.x, position.z), height, normal, nullptr, hole)) {
		return;
	}
	Transform3D xform = r_instance.get_transform();
	xform.origin.y = height + r_instance.offset;
	if (p_type && p_type->is_aligned_to_normal()) {
		// Rotated by the change of the ground normal (keeps the random rotation and scale).
		const Vector3 old_normal = r_instance.get_align_normal();
		const Vector3 new_normal = p_type->get_align_normal(normal);
		if (!old_normal.is_equal_approx(new_normal)) {
			xform.basis = Basis(Quaternion(old_normal, new_normal)) * xform.basis;
			r_instance.set_align_normal(new_normal);
		}
	}
	r_instance.set_transform(xform);
}

void LandscapeFoliage3D::_snap_rect(const Rect2 &p_rect) {
	const bool all = !p_rect.has_area();
	for (Entry *entry : entries) {
		const LandscapeFoliageType *type = entry->type.ptr();
		auto snap_cell = [&](const Vector2i &p_key, Cell &r_cell) {
			bool changed = false;
			for (Instance &instance : r_cell.instances) {
				const Vector3 position = instance.get_position();
				if (!all && !p_rect.has_point(Vector2(position.x, position.z))) {
					continue;
				}
				const Instance before = instance;
				_snap_instance(type, instance);
				changed = changed || memcmp(&before, &instance, sizeof(Instance)) != 0;
			}
			if (changed) {
				_cell_changed(entry, r_cell);
			}
		};
		if (all) {
			for (KeyValue<Vector2i, Cell> &kv : entry->cells) {
				snap_cell(kv.key, kv.value);
			}
		} else {
			_for_each_cell_in_rect(entry, p_rect, snap_cell);
		}
	}
}

void LandscapeFoliage3D::snap_to_terrain(const Rect2 &p_rect) {
	_snap_rect(p_rect);
}

void LandscapeFoliage3D::_terrain_region_changed(const Rect2i &p_texel_rect, int p_flags) {
	if (!follow_terrain || !(p_flags & LandscapeData::CHANGED_HEIGHTS) || !_has_terrain()) {
		return;
	}
	const real_t spacing = landscape->get_data()->get_vertex_spacing();
	// Instances between the changed texels and their neighbors move too.
	const Rect2 rect = Rect2(Vector2(p_texel_rect.position) * spacing, Vector2(p_texel_rect.size) * spacing).grow(spacing);
	for (Rect2 &pending : snap_rects) {
		if (pending.intersects(rect)) {
			pending = pending.merge(rect);
			return;
		}
	}
	snap_rects.push_back(rect);
}

void LandscapeFoliage3D::_process_snapping() {
	if (snap_rects.is_empty()) {
		return;
	}
	// Changes of a frame (e.g. brush dabs) are applied together.
	LocalVector<Rect2> rects = std::move(snap_rects);
	snap_rects.clear();
	for (const Rect2 &rect : rects) {
		_snap_rect(rect);
	}
}

/* Painting */

int LandscapeFoliage3D::_random_round(real_t p_value) {
	const real_t whole = Math::floor(p_value);
	return int(whole) + (rng.randf() < p_value - whole ? 1 : 0);
}

int LandscapeFoliage3D::paint(int p_type_index, const Vector3 &p_global_center, real_t p_radius, real_t p_density_scale) {
	Entry *entry = _get_entry(p_type_index);
	ERR_FAIL_NULL_V(entry, 0);
	if (entry->type.is_null() || !_has_terrain() || p_radius <= 0.0 || p_density_scale <= 0.0) {
		return 0;
	}
	const LandscapeFoliageType *type = entry->type.ptr();
	const Vector3 local = global_to_landscape(p_global_center);
	const Vector2 center(local.x, local.z);
	const real_t radius_sq = p_radius * p_radius;
	const real_t target = type->get_density() / 100.0 * Math::PI * radius_sq * p_density_scale;
	if (target <= 0.0) {
		return 0;
	}

	// Candidates spread over the circle. The fraction that passes the filters tells how much of
	// the brush the type grows in, so that repeated dabs converge to the density there.
	const int wanted = _random_round(target);
	const int evaluated = CLAMP(wanted, MIN_PAINT_CANDIDATES, MAX_PAINT_CANDIDATES);
	LocalVector<Instance> valid;
	for (int i = 0; i < evaluated; i++) {
		const real_t r = p_radius * Math::sqrt(rng.randf());
		const real_t angle = rng.randf() * Math::TAU;
		Instance instance;
		if (_make_instance(type, center + Vector2(Math::cos(angle), Math::sin(angle)) * r, true, instance)) {
			valid.push_back(instance);
		}
	}
	if (valid.is_empty()) {
		return 0;
	}

	FoliageSpacingGrid grid;
	grid.radius = type->get_radius();
	int existing = 0;
	const real_t reach = p_radius + grid.radius;
	const Rect2 rect = Rect2(center, Vector2()).grow(reach);
	_for_each_cell_in_rect(entry, rect, [&](const Vector2i &p_key, Cell &r_cell) {
		for (const Instance &instance : r_cell.instances) {
			const Vector3 p = instance.get_position();
			const Vector2 p2(p.x, p.z);
			const real_t distance_sq = p2.distance_squared_to(center);
			if (distance_sq <= radius_sq) {
				existing++;
			}
			if (distance_sq <= reach * reach) {
				grid.add(p2);
			}
		}
	});
	const int needed = _random_round(target * real_t(valid.size()) / real_t(evaluated)) - existing;
	int added = 0;
	for (const Instance &instance : valid) {
		if (added >= needed) {
			break;
		}
		const Vector3 p = instance.get_position();
		const Vector2 p2(p.x, p.z);
		if (!grid.is_free(p2)) {
			continue;
		}
		grid.add(p2);
		_add_instance(entry, instance);
		added++;
	}
	return added;
}

int LandscapeFoliage3D::erase(int p_type_index, const Vector3 &p_global_center, real_t p_radius, real_t p_density_scale) {
	if (p_type_index < 0) {
		int removed = 0;
		for (uint32_t i = 0; i < entries.size(); i++) {
			removed += erase(i, p_global_center, p_radius, p_density_scale);
		}
		return removed;
	}
	Entry *entry = _get_entry(p_type_index);
	ERR_FAIL_NULL_V(entry, 0);
	if (p_radius <= 0.0) {
		return 0;
	}
	const Vector3 local = global_to_landscape(p_global_center);
	const Vector2 center(local.x, local.z);
	const real_t radius_sq = p_radius * p_radius;

	struct Hit {
		Vector2i key;
		uint32_t index;
		bool operator<(const Hit &p_other) const {
			if (key != p_other.key) {
				return key < p_other.key;
			}
			return index > p_other.index; // Removed from the end of the cell.
		}
	};
	LocalVector<Hit> hits;
	_for_each_cell_in_rect(entry, Rect2(center, Vector2()).grow(p_radius), [&](const Vector2i &p_key, Cell &r_cell) {
		for (uint32_t i = 0; i < r_cell.instances.size(); i++) {
			const Vector3 p = r_cell.instances[i].get_position();
			if (Vector2(p.x, p.z).distance_squared_to(center) <= radius_sq) {
				hits.push_back({ p_key, i });
			}
		}
	});
	const real_t density = entry->type.is_valid() ? entry->type->get_density() : 0.0f;
	const int keep = p_density_scale > 0.0 ? _random_round(density / 100.0 * Math::PI * radius_sq * p_density_scale) : 0;
	const int remove = int(hits.size()) - keep;
	if (remove <= 0) {
		return 0;
	}
	if (keep > 0) {
		// Random instances are removed (Fisher-Yates on the first ones).
		for (int i = 0; i < remove; i++) {
			const int j = i + int(rng.rand() % uint32_t(hits.size() - i));
			SWAP(hits[i], hits[j]);
		}
		hits.resize(remove);
	}
	hits.sort();
	Cell *cell = nullptr;
	Vector2i cell_key;
	for (const Hit &hit : hits) {
		if (!cell || hit.key != cell_key) {
			cell_key = hit.key;
			cell = entry->cells.getptr(cell_key);
			_cell_changed(entry, *cell);
		}
		cell->instances.remove_at_unordered(hit.index);
	}
	entry->count -= hits.size();
	_remove_empty_cells(entry);
	return hits.size();
}

bool LandscapeFoliage3D::place_instance(int p_type_index, const Vector3 &p_global_position) {
	Entry *entry = _get_entry(p_type_index);
	ERR_FAIL_NULL_V(entry, false);
	if (entry->type.is_null()) {
		return false;
	}
	const Vector3 local = global_to_landscape(p_global_position);
	Instance instance;
	if (!_make_instance(entry->type.ptr(), Vector2(local.x, local.z), false, instance)) {
		return false;
	}
	_add_instance(entry, instance);
	return true;
}

int LandscapeFoliage3D::fill(int p_type_index, const Rect2 &p_rect) {
	Entry *entry = _get_entry(p_type_index);
	ERR_FAIL_NULL_V(entry, 0);
	if (entry->type.is_null() || !_has_terrain()) {
		return 0;
	}
	const LandscapeFoliageType *type = entry->type.ptr();
	const Rect2 bounds(Vector2(), landscape->get_data()->get_world_size());
	const Rect2 rect = p_rect.has_area() ? p_rect.intersection(bounds) : bounds;
	if (!rect.has_area() || type->get_density() <= 0.0) {
		return 0;
	}
	// Cell by cell: the density is completed where the type grows, like paint().
	const Vector2i begin(int(Math::floor(rect.position.x / chunk_size)), int(Math::floor(rect.position.y / chunk_size)));
	const Vector2i end(int(Math::floor(rect.get_end().x / chunk_size)), int(Math::floor(rect.get_end().y / chunk_size)));
	const real_t spacing = type->get_radius();
	int added = 0;
	for (int cz = begin.y; cz <= end.y; cz++) {
		for (int cx = begin.x; cx <= end.x; cx++) {
			const Rect2 tile = _get_cell_rect(Vector2i(cx, cz)).intersection(rect);
			if (!tile.has_area()) {
				continue;
			}
			const int wanted = _random_round(type->get_density() / 100.0 * tile.get_area());
			if (wanted <= 0) {
				continue;
			}
			LocalVector<Instance> valid;
			for (int i = 0; i < wanted; i++) {
				Instance instance;
				const Vector2 p = tile.position + Vector2(rng.randf(), rng.randf()) * tile.size;
				if (_make_instance(type, p, true, instance)) {
					valid.push_back(instance);
				}
			}
			if (valid.is_empty()) {
				continue;
			}
			FoliageSpacingGrid grid;
			grid.radius = spacing;
			int existing = 0;
			_for_each_cell_in_rect(entry, tile.grow(spacing), [&](const Vector2i &p_key, Cell &r_cell) {
				for (const Instance &instance : r_cell.instances) {
					const Vector3 p = instance.get_position();
					const Vector2 p2(p.x, p.z);
					existing += _rect_has_point(tile, p2) ? 1 : 0;
					grid.add(p2);
				}
			});
			const int needed = int(valid.size()) - existing;
			int placed = 0;
			for (const Instance &instance : valid) {
				if (placed >= needed) {
					break;
				}
				const Vector3 p = instance.get_position();
				if (!grid.is_free(Vector2(p.x, p.z))) {
					continue;
				}
				grid.add(Vector2(p.x, p.z));
				_add_instance(entry, instance);
				placed++;
			}
			added += placed;
		}
	}
	return added;
}

int64_t LandscapeFoliage3D::estimate_fill_count(int p_type_index) const {
	const Entry *entry = _get_entry(p_type_index);
	ERR_FAIL_NULL_V(entry, 0);
	if (entry->type.is_null() || !_has_terrain()) {
		return 0;
	}
	const Vector2 world = landscape->get_data()->get_world_size();
	return int64_t(entry->type->get_density() / 100.0 * world.x * world.y);
}

int LandscapeFoliage3D::reapply(int p_type_index, const Vector3 &p_global_center, real_t p_radius) {
	Entry *entry = _get_entry(p_type_index);
	ERR_FAIL_NULL_V(entry, 0);
	if (entry->type.is_null() || !_has_terrain() || p_radius <= 0.0) {
		return 0;
	}
	const LandscapeFoliageType *type = entry->type.ptr();
	const Vector3 local = global_to_landscape(p_global_center);
	const Vector2 center(local.x, local.z);
	const real_t radius_sq = p_radius * p_radius;
	int changed = 0;
	_for_each_cell_in_rect(entry, Rect2(center, Vector2()).grow(p_radius), [&](const Vector2i &p_key, Cell &r_cell) {
		bool cell_changed = false;
		for (int64_t i = int64_t(r_cell.instances.size()) - 1; i >= 0; i--) {
			Instance &instance = r_cell.instances[i];
			const Vector3 p = instance.get_position();
			if (Vector2(p.x, p.z).distance_squared_to(center) > radius_sq) {
				continue;
			}
			Instance reapplied;
			if (_make_instance(type, Vector2(p.x, p.z), true, reapplied)) {
				reapplied.random = instance.random; // Keeps the variation of the shaders.
				instance = reapplied;
			} else {
				r_cell.instances.remove_at_unordered(i);
				entry->count--;
			}
			cell_changed = true;
			changed++;
		}
		if (cell_changed) {
			_cell_changed(entry, r_cell);
		}
	});
	_remove_empty_cells(entry);
	return changed;
}

void LandscapeFoliage3D::clear(int p_type_index) {
	for (uint32_t i = 0; i < entries.size(); i++) {
		if (p_type_index >= 0 && int(i) != p_type_index) {
			continue;
		}
		Entry *entry = entries[i];
		for (KeyValue<Vector2i, Cell> &kv : entry->cells) {
			_free_cell_rendering(kv.value);
		}
		entry->cells.clear();
		entry->count = 0;
		entry->serialized_dirty = true;
	}
	render_pending = true;
}

/* Instances */

int64_t LandscapeFoliage3D::get_instance_count(int p_type_index) const {
	if (p_type_index < 0) {
		int64_t count = 0;
		for (const Entry *entry : entries) {
			count += entry->count;
		}
		return count;
	}
	const Entry *entry = _get_entry(p_type_index);
	ERR_FAIL_NULL_V(entry, 0);
	return entry->count;
}

bool LandscapeFoliage3D::add_instance(int p_type_index, const Transform3D &p_global_transform) {
	Entry *entry = _get_entry(p_type_index);
	ERR_FAIL_NULL_V(entry, false);
	const Transform3D xform = _get_space_transform().affine_inverse() * p_global_transform;
	Instance instance;
	instance.set_transform(xform);
	instance.random = rng.randf();
	instance.offset = 0.0;
	instance.set_align_normal(Vector3(0, 1, 0));
	real_t height;
	Vector3 normal;
	bool hole;
	if (_sample_ground(Vector2(xform.origin.x, xform.origin.z), height, normal, nullptr, hole)) {
		// Follows the terrain from where it is.
		instance.offset = xform.origin.y - height;
		if (entry->type.is_valid()) {
			instance.set_align_normal(entry->type->get_align_normal(normal));
		}
	}
	_add_instance(entry, instance);
	return true;
}

void LandscapeFoliage3D::add_instances(int p_type_index, const TypedArray<Transform3D> &p_global_transforms) {
	ERR_FAIL_INDEX(p_type_index, int(entries.size()));
	for (int i = 0; i < p_global_transforms.size(); i++) {
		add_instance(p_type_index, p_global_transforms[i]);
	}
}

TypedArray<Transform3D> LandscapeFoliage3D::get_instance_transforms(int p_type_index, const AABB &p_global_aabb) const {
	TypedArray<Transform3D> result;
	const Entry *entry = _get_entry(p_type_index);
	ERR_FAIL_NULL_V(entry, result);
	const Transform3D space = _get_space_transform();
	const bool filter = p_global_aabb.size != Vector3();
	for (const KeyValue<Vector2i, Cell> &kv : entry->cells) {
		for (const Instance &instance : kv.value.instances) {
			const Transform3D xform = space * instance.get_transform();
			if (!filter || p_global_aabb.has_point(xform.origin)) {
				result.push_back(xform);
			}
		}
	}
	return result;
}

TypedArray<Transform3D> LandscapeFoliage3D::get_instances_in_radius(int p_type_index, const Vector3 &p_global_center, real_t p_radius) const {
	TypedArray<Transform3D> result;
	const Entry *entry = _get_entry(p_type_index);
	ERR_FAIL_NULL_V(entry, result);
	const Transform3D space = _get_space_transform();
	const Vector3 local = space.affine_inverse().xform(p_global_center);
	const Vector2 center(local.x, local.z);
	const real_t radius_sq = p_radius * p_radius;
	const_cast<LandscapeFoliage3D *>(this)->_for_each_cell_in_rect(const_cast<Entry *>(entry), Rect2(center, Vector2()).grow(p_radius), [&](const Vector2i &p_key, Cell &r_cell) {
		for (const Instance &instance : r_cell.instances) {
			const Vector3 p = instance.get_position();
			if (Vector2(p.x, p.z).distance_squared_to(center) <= radius_sq) {
				result.push_back(space * instance.get_transform());
			}
		}
	});
	return result;
}

int LandscapeFoliage3D::remove_instances_in_radius(int p_type_index, const Vector3 &p_global_center, real_t p_radius) {
	return erase(p_type_index, p_global_center, p_radius, 0.0);
}

PackedFloat32Array LandscapeFoliage3D::get_instances_in_rect(int p_type_index, const Rect2 &p_rect) const {
	PackedFloat32Array result;
	const Entry *entry = _get_entry(p_type_index);
	ERR_FAIL_NULL_V(entry, result);
	const_cast<LandscapeFoliage3D *>(this)->_for_each_cell_in_rect(const_cast<Entry *>(entry), p_rect, [&](const Vector2i &p_key, Cell &r_cell) {
		for (const Instance &instance : r_cell.instances) {
			const Vector3 p = instance.get_position();
			if (_rect_has_point(p_rect, Vector2(p.x, p.z))) {
				const int64_t offset = result.size();
				result.resize(offset + INSTANCE_FLOATS);
				memcpy(result.ptrw() + offset, &instance, sizeof(Instance));
			}
		}
	});
	return result;
}

void LandscapeFoliage3D::set_instances_in_rect(int p_type_index, const Rect2 &p_rect, const PackedFloat32Array &p_instances) {
	Entry *entry = _get_entry(p_type_index);
	ERR_FAIL_NULL(entry);
	ERR_FAIL_COND(p_instances.size() % INSTANCE_FLOATS != 0);
	_for_each_cell_in_rect(entry, p_rect, [&](const Vector2i &p_key, Cell &r_cell) {
		bool changed = false;
		for (int64_t i = int64_t(r_cell.instances.size()) - 1; i >= 0; i--) {
			const Vector3 p = r_cell.instances[i].get_position();
			if (_rect_has_point(p_rect, Vector2(p.x, p.z))) {
				r_cell.instances.remove_at_unordered(i);
				entry->count--;
				changed = true;
			}
		}
		if (changed) {
			_cell_changed(entry, r_cell);
		}
	});
	const Instance *instances = reinterpret_cast<const Instance *>(p_instances.ptr());
	for (int64_t i = 0; i < p_instances.size() / INSTANCE_FLOATS; i++) {
		_add_instance(entry, instances[i]);
	}
	_remove_empty_cells(entry);
}

Vector<Rect2> LandscapeFoliage3D::get_cell_rects(const Rect2 &p_rect) const {
	Vector<Rect2> rects;
	const Vector2i begin(int(Math::floor(p_rect.position.x / chunk_size)), int(Math::floor(p_rect.position.y / chunk_size)));
	const Vector2i end(int(Math::floor(p_rect.get_end().x / chunk_size)), int(Math::floor(p_rect.get_end().y / chunk_size)));
	for (int z = begin.y; z <= end.y; z++) {
		for (int x = begin.x; x <= end.x; x++) {
			rects.push_back(_get_cell_rect(Vector2i(x, z)));
		}
	}
	return rects;
}

/* Serialization */

PackedByteArray LandscapeFoliage3D::_encode_instances(const LocalVector<Instance> &p_instances) {
	PackedByteArray result;
	if (p_instances.is_empty()) {
		return result;
	}
	// Byte planes of the floats compress much better than interleaved floats.
	const int64_t floats = int64_t(p_instances.size()) * INSTANCE_FLOATS;
	Vector<uint8_t> shuffled;
	shuffled.resize(floats * 4);
	const uint8_t *src = reinterpret_cast<const uint8_t *>(p_instances.ptr());
	uint8_t *dst = shuffled.ptrw();
	for (int64_t i = 0; i < floats; i++) {
		for (int b = 0; b < 4; b++) {
			dst[b * floats + i] = src[i * 4 + b];
		}
	}
	result.resize(FOLIAGE_DATA_HEADER + Compression::get_max_compressed_buffer_size(shuffled.size(), Compression::MODE_ZSTD));
	uint8_t *w = result.ptrw();
	encode_uint32(FOLIAGE_DATA_MAGIC, w);
	encode_uint32(FOLIAGE_DATA_VERSION, w + 4);
	encode_uint32(p_instances.size(), w + 8);
	encode_uint32(INSTANCE_FLOATS, w + 12);
	const int64_t written = Compression::compress(w + FOLIAGE_DATA_HEADER, shuffled.ptr(), shuffled.size(), Compression::MODE_ZSTD);
	ERR_FAIL_COND_V(written < 0, PackedByteArray());
	result.resize(FOLIAGE_DATA_HEADER + written);
	return result;
}

bool LandscapeFoliage3D::_decode_instances(const PackedByteArray &p_data, LocalVector<Instance> &r_instances) {
	r_instances.clear();
	if (p_data.is_empty()) {
		return true;
	}
	ERR_FAIL_COND_V_MSG(p_data.size() < FOLIAGE_DATA_HEADER, false, "Invalid foliage instance data.");
	const uint8_t *r = p_data.ptr();
	ERR_FAIL_COND_V_MSG(decode_uint32(r) != FOLIAGE_DATA_MAGIC, false, "Invalid foliage instance data.");
	ERR_FAIL_COND_V_MSG(decode_uint32(r + 4) > FOLIAGE_DATA_VERSION, false, "The foliage instance data was saved by a newer version.");
	const uint32_t count = decode_uint32(r + 8);
	ERR_FAIL_COND_V_MSG(decode_uint32(r + 12) != uint32_t(INSTANCE_FLOATS), false, "Invalid foliage instance data.");
	ERR_FAIL_COND_V_MSG(count > (1u << 27), false, "Corrupted foliage instance data.");
	const int64_t floats = int64_t(count) * INSTANCE_FLOATS;
	Vector<uint8_t> shuffled;
	shuffled.resize(floats * 4);
	const int64_t size = Compression::decompress(shuffled.ptrw(), shuffled.size(), r + FOLIAGE_DATA_HEADER, p_data.size() - FOLIAGE_DATA_HEADER, Compression::MODE_ZSTD);
	ERR_FAIL_COND_V_MSG(size != shuffled.size(), false, "Corrupted foliage instance data.");
	r_instances.resize(count);
	uint8_t *dst = reinterpret_cast<uint8_t *>(r_instances.ptr());
	const uint8_t *s = shuffled.ptr();
	for (int64_t i = 0; i < floats; i++) {
		for (int b = 0; b < 4; b++) {
			dst[i * 4 + b] = s[b * floats + i];
		}
	}
	return true;
}

PackedByteArray LandscapeFoliage3D::get_type_data(int p_type_index) const {
	Entry *entry = _get_entry(p_type_index);
	ERR_FAIL_NULL_V(entry, PackedByteArray());
	if (entry->serialized_dirty) {
		// Sorted by cell, so that the same instances are always saved the same way.
		LocalVector<Vector2i> keys;
		for (const KeyValue<Vector2i, Cell> &kv : entry->cells) {
			keys.push_back(kv.key);
		}
		keys.sort();
		LocalVector<Instance> all;
		all.reserve(entry->count);
		for (const Vector2i &key : keys) {
			for (const Instance &instance : entry->cells[key].instances) {
				all.push_back(instance);
			}
		}
		entry->serialized = _encode_instances(all);
		entry->serialized_dirty = false;
	}
	return entry->serialized;
}

void LandscapeFoliage3D::set_type_data(int p_type_index, const PackedByteArray &p_data) {
	Entry *entry = _get_entry(p_type_index);
	ERR_FAIL_NULL(entry);
	LocalVector<Instance> instances;
	if (!_decode_instances(p_data, instances)) {
		return;
	}
	for (KeyValue<Vector2i, Cell> &kv : entry->cells) {
		_free_cell_rendering(kv.value);
	}
	entry->cells.clear();
	entry->count = 0;
	for (const Instance &instance : instances) {
		_add_instance(entry, instance);
	}
	entry->serialized_dirty = true;
	render_pending = true;
}

void LandscapeFoliage3D::_set_foliage_types_bind(const TypedArray<LandscapeFoliageType> &p_types) {
	// Instances of existing types are kept (by index), extra types are removed.
	while (int(entries.size()) > p_types.size()) {
		remove_foliage_type(entries.size() - 1);
	}
	for (int i = 0; i < p_types.size(); i++) {
		if (i < int(entries.size())) {
			set_foliage_type(i, p_types[i]);
		} else {
			add_foliage_type(p_types[i]);
		}
	}
}

TypedArray<LandscapeFoliageType> LandscapeFoliage3D::_get_foliage_types_bind() const {
	TypedArray<LandscapeFoliageType> types;
	for (const Entry *entry : entries) {
		types.push_back(entry->type);
	}
	return types;
}

void LandscapeFoliage3D::_set_instance_data(const Array &p_data) {
	for (int i = 0; i < p_data.size(); i++) {
		if (i >= int(entries.size())) {
			add_foliage_type(Ref<LandscapeFoliageType>()); // The types may be set afterwards.
		}
		set_type_data(i, p_data[i]);
	}
}

Array LandscapeFoliage3D::_get_instance_data() const {
	Array instance_data;
	for (uint32_t i = 0; i < entries.size(); i++) {
		instance_data.push_back(get_type_data(i));
	}
	return instance_data;
}

bool LandscapeFoliage3D::_set(const StringName &p_name, const Variant &p_value) {
	// Editor access to the types ("type_<index>"), stored in `foliage_types`.
	const String property = p_name;
	if (property.begins_with("type_") && property.substr(5).is_valid_int()) {
		const int index = property.substr(5).to_int();
		if (index >= 0 && index < int(entries.size())) {
			set_foliage_type(index, p_value);
			return true;
		}
	}
	return false;
}

bool LandscapeFoliage3D::_get(const StringName &p_name, Variant &r_ret) const {
	const String property = p_name;
	if (property.begins_with("type_") && property.substr(5).is_valid_int()) {
		const int index = property.substr(5).to_int();
		if (index >= 0 && index < int(entries.size())) {
			r_ret = entries[index]->type;
			return true;
		}
	}
	return false;
}

void LandscapeFoliage3D::_get_property_list(List<PropertyInfo> *p_list) const {
	// Types can be edited (or replaced, keeping the instances) in the inspector. They are added
	// and removed in the Foliage tab of the Landscape dock, which also handles their instances.
	p_list->push_back(PropertyInfo(Variant::NIL, "Foliage Types", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_GROUP));
	for (uint32_t i = 0; i < entries.size(); i++) {
		p_list->push_back(PropertyInfo(Variant::OBJECT, vformat("type_%d", i), PROPERTY_HINT_RESOURCE_TYPE, LandscapeFoliageType::get_class_static(), PROPERTY_USAGE_EDITOR));
	}
}

/* Rendering */

Transform3D LandscapeFoliage3D::_get_space_transform() const {
	if (landscape && landscape->is_inside_tree()) {
		return landscape->get_global_transform();
	}
	return is_inside_tree() ? get_global_transform() : Transform3D();
}

Vector3 LandscapeFoliage3D::global_to_landscape(const Vector3 &p_global) const {
	return _get_space_transform().affine_inverse().xform(p_global);
}

bool LandscapeFoliage3D::_get_camera(Vector3 &r_position) const {
	Vector3 global;
	if (landscape) {
		if (!landscape->get_view_position(global)) {
			return false;
		}
	} else {
		Camera3D *camera = nullptr;
#ifdef TOOLS_ENABLED
		if (Engine::get_singleton()->is_editor_hint() && Landscape3D::editor_camera_callback && is_part_of_edited_scene()) {
			camera = Landscape3D::editor_camera_callback();
		}
#endif
		if (!camera && get_viewport()) {
			camera = get_viewport()->get_camera_3d();
		}
		if (!camera || !camera->is_inside_tree()) {
			return false;
		}
		global = camera->get_camera_transform().origin;
	}
	r_position = global_to_landscape(global);
	return true;
}

void LandscapeFoliage3D::_free_batch(Batch &r_batch) {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (r_batch.instance.is_valid()) {
		rs->free_rid(r_batch.instance);
		r_batch.instance = RID();
	}
	if (r_batch.multimesh.is_valid()) {
		rs->free_rid(r_batch.multimesh);
		r_batch.multimesh = RID();
	}
	r_batch.count = 0;
}

void LandscapeFoliage3D::_free_cell_rendering(Cell &r_cell) {
	for (Batch &batch : r_cell.batches) {
		_free_batch(batch);
	}
	r_cell.state = STATE_NONE;
	r_cell.render_dirty = true;
	r_cell.instance_lods.clear();
	debug_bounds_dirty = true;
}

void LandscapeFoliage3D::_free_all_rendering() {
	for (Entry *entry : entries) {
		for (KeyValue<Vector2i, Cell> &kv : entry->cells) {
			_free_cell_rendering(kv.value);
		}
	}
	_free_debug_bounds();
	render_pending = true;
}

void LandscapeFoliage3D::_update_batch_settings(Entry *p_entry, const Cell &p_cell, int p_lod, Batch &r_batch) {
	if (r_batch.instance.is_null()) {
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	rs->instance_set_transform(r_batch.instance, _get_space_transform());
	rs->instance_set_layer_mask(r_batch.instance, render_layers);
	rs->instance_set_visible(r_batch.instance, is_visible_in_tree());
	const bool shadows = cast_shadows && p_entry->lod_shadows[p_lod];
	rs->instance_geometry_set_cast_shadows_setting(r_batch.instance, shadows ? RSE::SHADOW_CASTING_SETTING_ON : RSE::SHADOW_CASTING_SETTING_OFF);
	if (debug_view != DEBUG_VIEW_DISABLED) {
		// One shared material, the color is an instance uniform of the batch.
		rs->instance_geometry_set_material_override(r_batch.instance, _get_debug_material());
		rs->instance_geometry_set_shader_parameter(r_batch.instance, SNAME("foliage_debug_color"), _get_debug_cell_color(p_cell, p_lod));
		return;
	}
	const Ref<Material> material = p_entry->type.is_valid() ? p_entry->type->get_material_override() : Ref<Material>();
	rs->instance_geometry_set_material_override(r_batch.instance, material.is_valid() ? material->get_rid() : RID());
}

void LandscapeFoliage3D::_update_all_batch_settings() {
	for (Entry *entry : entries) {
		for (KeyValue<Vector2i, Cell> &kv : entry->cells) {
			for (int lod = 0; lod < LandscapeFoliageType::MAX_LODS; lod++) {
				_update_batch_settings(entry, kv.value, lod, kv.value.batches[lod]);
			}
		}
	}
	if (debug_bounds_instance.is_valid()) {
		RenderingServer *rs = RenderingServer::get_singleton();
		rs->instance_set_transform(debug_bounds_instance, _get_space_transform());
		rs->instance_set_layer_mask(debug_bounds_instance, render_layers);
		rs->instance_set_visible(debug_bounds_instance, is_visible_in_tree());
	}
}

/* Debug views */

Color LandscapeFoliage3D::get_debug_color(int p_index) {
	// Same colors as ls_debug_color() of the landscape shader.
	const float h = Math::fposmod(float(p_index) * 0.61803398875f, 1.0f);
	const float offsets[3] = { 0.0f, 2.0f / 3.0f, 1.0f / 3.0f };
	float c[3];
	for (int i = 0; i < 3; i++) {
		const float f = Math::fposmod(h + offsets[i], 1.0f);
		c[i] = CLAMP(Math::abs(f * 6.0f - 3.0f) - 1.0f, 0.0f, 1.0f);
	}
	return Color(Math::lerp(1.0f, c[0], 0.75f), Math::lerp(1.0f, c[1], 0.75f), Math::lerp(1.0f, c[2], 0.75f));
}

Color LandscapeFoliage3D::get_debug_state_color(bool p_sorted_per_instance) {
	// Orange: sorted per instance (crosses a LOD or cull distance), blue: drawn as a whole.
	return p_sorted_per_instance ? Color(1.0, 0.55, 0.2) : Color(0.35, 0.65, 1.0);
}

RID LandscapeFoliage3D::_get_debug_material() {
	if (debug_material.is_null()) {
		debug_shader.instantiate();
		debug_shader->set_code(R"(
shader_type spatial;
render_mode cull_disabled;

instance uniform vec3 foliage_debug_color = vec3(1.0);

void fragment() {
	ALBEDO = foliage_debug_color;
	ROUGHNESS = 0.9;
}
)");
		debug_material.instantiate();
		debug_material->set_shader(debug_shader);
	}
	return debug_material->get_rid();
}

Color LandscapeFoliage3D::_get_debug_cell_color(const Cell &p_cell, int p_lod) const {
	switch (debug_view) {
		case DEBUG_VIEW_LOD_LEVELS:
			return get_debug_color(p_lod);
		case DEBUG_VIEW_CELLS:
			return get_debug_color(p_cell.key.x * 7919 + p_cell.key.y * 104729);
		case DEBUG_VIEW_CELL_STATE:
			return get_debug_state_color(p_cell.state == STATE_MIXED);
		default:
			return Color(1, 1, 1);
	}
}

void LandscapeFoliage3D::_update_debug_colors(const Cell &p_cell) {
	RenderingServer *rs = RenderingServer::get_singleton();
	for (int lod = 0; lod < LandscapeFoliageType::MAX_LODS; lod++) {
		if (p_cell.batches[lod].instance.is_valid()) {
			rs->instance_geometry_set_shader_parameter(p_cell.batches[lod].instance, SNAME("foliage_debug_color"), _get_debug_cell_color(p_cell, lod));
		}
	}
}

void LandscapeFoliage3D::_free_debug_bounds() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (debug_bounds_instance.is_valid()) {
		rs->free_rid(debug_bounds_instance);
		debug_bounds_instance = RID();
	}
	if (debug_bounds_mesh.is_valid()) {
		rs->free_rid(debug_bounds_mesh);
		debug_bounds_mesh = RID();
	}
	debug_bounds_count = 0;
	debug_bounds_dirty = true;
}

void LandscapeFoliage3D::_update_debug_bounds() {
	debug_bounds_dirty = false;
	if (debug_view == DEBUG_VIEW_DISABLED || !is_inside_tree() || get_world_3d().is_null()) {
		_free_debug_bounds();
		debug_bounds_dirty = false;
		return;
	}
	// Bounds of the drawn cells: the color of the cell (Cells, Cell State), of the level of detail
	// of the cells drawn as a whole (LOD Levels, white when sorted per instance).
	PackedVector3Array lines;
	PackedColorArray colors;
	int count = 0;
	for (const Entry *entry : entries) {
		for (const KeyValue<Vector2i, Cell> &kv : entry->cells) {
			const Cell &cell = kv.value;
			bool drawn = false;
			for (const Batch &batch : cell.batches) {
				drawn = drawn || batch.instance.is_valid();
			}
			if (!drawn) {
				continue;
			}
			Color color = _get_debug_cell_color(cell, MAX(cell.state, 0));
			if (debug_view == DEBUG_VIEW_LOD_LEVELS && cell.state == STATE_MIXED) {
				color = Color(1, 1, 1);
			}
			for (int edge = 0; edge < 12; edge++) {
				Vector3 from;
				Vector3 to;
				cell.bounds.get_edge(edge, from, to);
				lines.push_back(from);
				lines.push_back(to);
				colors.push_back(color);
				colors.push_back(color);
			}
			count++;
		}
	}
	debug_bounds_count = count;
	RenderingServer *rs = RenderingServer::get_singleton();
	if (lines.is_empty()) {
		_free_debug_bounds();
		debug_bounds_dirty = false;
		return;
	}
	if (debug_bounds_material.is_null()) {
		debug_bounds_material.instantiate();
		debug_bounds_material->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
		debug_bounds_material->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
	}
	if (debug_bounds_mesh.is_null()) {
		debug_bounds_mesh = rs->mesh_create();
	} else {
		rs->mesh_clear(debug_bounds_mesh);
	}
	Array arrays;
	arrays.resize(RSE::ARRAY_MAX);
	arrays[RSE::ARRAY_VERTEX] = lines;
	arrays[RSE::ARRAY_COLOR] = colors;
	rs->mesh_add_surface_from_arrays(debug_bounds_mesh, RSE::PRIMITIVE_LINES, arrays);
	rs->mesh_surface_set_material(debug_bounds_mesh, 0, debug_bounds_material->get_rid());
	if (debug_bounds_instance.is_null()) {
		debug_bounds_instance = rs->instance_create2(debug_bounds_mesh, get_world_3d()->get_scenario());
		rs->instance_attach_object_instance_id(debug_bounds_instance, get_instance_id());
		rs->instance_geometry_set_cast_shadows_setting(debug_bounds_instance, RSE::SHADOW_CASTING_SETTING_OFF);
		rs->instance_set_transform(debug_bounds_instance, _get_space_transform());
		rs->instance_set_layer_mask(debug_bounds_instance, render_layers);
		rs->instance_set_visible(debug_bounds_instance, is_visible_in_tree());
	}
}

void LandscapeFoliage3D::set_debug_view(DebugView p_view) {
	if (debug_view == p_view) {
		return;
	}
	debug_view = p_view;
	_update_all_batch_settings();
	if (debug_view == DEBUG_VIEW_DISABLED) {
		_free_debug_bounds();
	}
	debug_bounds_dirty = true;
	render_pending = true;
}

void LandscapeFoliage3D::cleanup_shared_resources() {
	debug_material.unref();
	debug_shader.unref();
	debug_bounds_material.unref();
}

void LandscapeFoliage3D::_set_batch(Entry *p_entry, Cell &r_cell, int p_lod, const LocalVector<uint32_t> *p_indices) {
	Batch &batch = r_cell.batches[p_lod];
	const int count = p_indices ? int(p_indices->size()) : int(r_cell.instances.size());
	if (count == 0 || p_lod >= p_entry->lod_count || p_entry->lod_meshes[p_lod].is_null() || !is_inside_tree() || get_world_3d().is_null()) {
		_free_batch(batch);
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	if (batch.multimesh.is_null()) {
		batch.multimesh = rs->multimesh_create();
		rs->multimesh_set_mesh(batch.multimesh, p_entry->lod_meshes[p_lod]);
	}
	// Transform and INSTANCE_CUSTOM (per-instance random value in x).
	rs->multimesh_allocate_data(batch.multimesh, count, RSE::MULTIMESH_TRANSFORM_3D, false, true);
	Vector<float> buffer;
	buffer.resize(count * INSTANCE_FLOATS);
	float *w = buffer.ptrw();
	for (int i = 0; i < count; i++) {
		const Instance &instance = r_cell.instances[p_indices ? (*p_indices)[i] : uint32_t(i)];
		memcpy(w, instance.xform, sizeof(instance.xform));
		w[12] = instance.random;
		w[13] = 0.0;
		w[14] = 0.0;
		w[15] = 0.0;
		w += INSTANCE_FLOATS;
	}
	rs->multimesh_set_buffer(batch.multimesh, buffer);
	// The bounds of the whole cell: no bounds computation when instances change their level.
	rs->multimesh_set_custom_aabb(batch.multimesh, r_cell.bounds);
	if (batch.instance.is_null()) {
		batch.instance = rs->instance_create2(batch.multimesh, get_world_3d()->get_scenario());
		rs->instance_attach_object_instance_id(batch.instance, get_instance_id());
		_update_batch_settings(p_entry, r_cell, p_lod, batch);
	}
	batch.count = count;
}

int LandscapeFoliage3D::_get_uniform_state(const Entry *p_entry, real_t p_min_distance, real_t p_max_distance) const {
	const float half = p_entry->transition * 0.5f;
	if (p_entry->cull_end > 0.0f && p_min_distance > p_entry->cull_end + half) {
		return STATE_CULLED;
	}
	for (int k = p_entry->lod_count - 1; k >= 0; k--) {
		if (k > 0 && p_min_distance < p_entry->lod_starts[k]) {
			continue;
		}
		// The band of the closest point must contain the whole cell, beyond the hysteresis
		// margins, so that every instance is at this level whatever its previous level.
		if (k > 0 && p_min_distance < p_entry->lod_starts[k] + half) {
			return STATE_MIXED;
		}
		float end = k + 1 < p_entry->lod_count ? p_entry->lod_starts[k + 1] - half : float(Math::INF);
		if (p_entry->cull_end > 0.0f) {
			end = MIN(end, p_entry->cull_begin - half);
		}
		return p_max_distance < end ? k : int(STATE_MIXED);
	}
	return STATE_MIXED;
}

uint8_t LandscapeFoliage3D::_get_instance_lod(const Entry *p_entry, const Instance &p_instance, real_t p_distance, uint8_t p_current) const {
	const float half = p_entry->transition * 0.5f;
	const bool known = p_current < LandscapeFoliageType::MAX_LODS;
	if (p_entry->cull_end > 0.0f) {
		// Each instance has its own cull distance between cull_begin and cull_end.
		const float cull = p_entry->cull_end - (p_entry->cull_end - p_entry->cull_begin) * p_instance.random;
		const float margin = p_current == LOD_CULLED ? -half : (known ? half : 0.0f);
		if (p_distance > cull + margin) {
			return LOD_CULLED;
		}
	}
	for (int k = p_entry->lod_count - 1; k > 0; k--) {
		float start = p_entry->lod_starts[k];
		if (known) {
			// Hysteresis: the boundary moves away from the current level.
			start += p_current >= k ? -half : half;
		}
		if (p_distance >= start) {
			return k;
		}
	}
	return 0;
}

bool LandscapeFoliage3D::_update_cell(Entry *p_entry, Cell &r_cell, int p_state, const Vector3 &p_camera) {
	_update_cell_bounds(p_entry, r_cell);
	const uint32_t count = r_cell.instances.size();
	debug_bounds_dirty = true;
	if (p_state == STATE_CULLED) {
		_free_cell_rendering(r_cell);
		r_cell.state = STATE_CULLED;
		r_cell.render_dirty = false;
		return true;
	}
	if (p_state >= 0) {
		r_cell.state = p_state; // Colors of the debug views.
		for (int lod = 0; lod < LandscapeFoliageType::MAX_LODS; lod++) {
			if (lod == p_state) {
				_set_batch(p_entry, r_cell, lod, nullptr);
			} else {
				_free_batch(r_cell.batches[lod]);
			}
		}
		r_cell.state = p_state;
		r_cell.render_dirty = false;
		// Known levels, for the hysteresis when the cell becomes mixed.
		r_cell.instance_lods.resize(count);
		memset(r_cell.instance_lods.ptr(), p_state, count);
		if (debug_view == DEBUG_VIEW_CELL_STATE) {
			_update_debug_colors(r_cell); // The batch of the level may have been sorted per instance.
		}
		return true;
	}

	// Mixed: level per instance. Only the levels that gained or lost instances are uploaded.
	bool lod_changed[LandscapeFoliageType::MAX_LODS] = {};
	const bool rebuild = r_cell.state != STATE_MIXED || r_cell.render_dirty;
	if (r_cell.instance_lods.size() != count) {
		r_cell.instance_lods.resize(count);
		memset(r_cell.instance_lods.ptr(), FOLIAGE_LOD_UNKNOWN, count);
	}
	LocalVector<uint32_t> indices[LandscapeFoliageType::MAX_LODS];
	for (uint32_t i = 0; i < count; i++) {
		const Instance &instance = r_cell.instances[i];
		const uint8_t previous = r_cell.instance_lods[i];
		const uint8_t lod = _get_instance_lod(p_entry, instance, p_camera.distance_to(instance.get_position()), previous);
		if (lod != previous) {
			r_cell.instance_lods[i] = lod;
			if (previous < LandscapeFoliageType::MAX_LODS) {
				lod_changed[previous] = true;
			}
			if (lod < LandscapeFoliageType::MAX_LODS) {
				lod_changed[lod] = true;
			}
		}
		if (lod < LandscapeFoliageType::MAX_LODS) {
			indices[lod].push_back(i);
		}
	}
	r_cell.sort_camera = p_camera;
	r_cell.state = STATE_MIXED;
	r_cell.render_dirty = false;
	for (int lod = 0; lod < LandscapeFoliageType::MAX_LODS; lod++) {
		if (rebuild || lod_changed[lod]) {
			_set_batch(p_entry, r_cell, lod, &indices[lod]);
		}
	}
	if (debug_view == DEBUG_VIEW_CELL_STATE && rebuild) {
		_update_debug_colors(r_cell);
	}
	return true;
}

void LandscapeFoliage3D::_update_rendering() {
	if (!is_inside_tree() || get_world_3d().is_null() || !is_visible_in_tree()) {
		return;
	}
	Vector3 camera;
	bool has_camera = _get_camera(camera);
	if (landscape && landscape->is_lod_frozen() && has_last_camera) {
		// The LOD of the landscape is frozen: the foliage keeps its levels too, to inspect them.
		camera = last_camera;
		has_camera = true;
	}
	if (!has_camera) {
		camera = Vector3(); // Without camera, everything is drawn at the first level.
	}
	if (!render_pending && has_camera == has_last_camera && camera.distance_squared_to(last_camera) < MIN_CAMERA_STEP * MIN_CAMERA_STEP) {
		return;
	}

	struct Work {
		real_t distance;
		Entry *entry;
		Cell *cell;
		int state;
		bool operator<(const Work &p_other) const { return distance < p_other.distance; }
	};
	LocalVector<Work> work;
	for (Entry *entry : entries) {
		if (entry->type.is_null()) {
			continue;
		}
		const real_t step = MAX(entry->transition * 0.5f, float(MIN_CAMERA_STEP));
		for (KeyValue<Vector2i, Cell> &kv : entry->cells) {
			Cell &cell = kv.value;
			_update_cell_bounds(entry, cell);
			real_t min_distance = 0.0;
			real_t max_distance = 0.0;
			if (has_camera) {
				const Vector3 begin = cell.bounds.position;
				const Vector3 end = cell.bounds.position + cell.bounds.size;
				min_distance = camera.distance_to(camera.clamp(begin, end));
				Vector3 farthest;
				for (int axis = 0; axis < 3; axis++) {
					farthest[axis] = Math::abs(camera[axis] - begin[axis]) > Math::abs(camera[axis] - end[axis]) ? begin[axis] : end[axis];
				}
				max_distance = camera.distance_to(farthest);
			}
			const int state = _get_uniform_state(entry, min_distance, max_distance);
			const bool needed = cell.render_dirty || state != cell.state || (state == STATE_MIXED && cell.sort_camera.distance_squared_to(camera) >= step * step);
			if (needed) {
				work.push_back({ min_distance, entry, &cell, state });
			}
		}
	}

	// Nearest cells first, within the time budget of the frame.
	work.sort();
	const uint64_t begin = OS::get_singleton()->get_ticks_usec();
	uint32_t done = 0;
	for (; done < work.size(); done++) {
		if (done >= uint32_t(MIN_CELL_UPDATES_PER_FRAME) && OS::get_singleton()->get_ticks_usec() - begin > CELL_UPDATE_BUDGET_USEC) {
			break;
		}
		const Work &w = work[done];
		_update_cell(w.entry, *w.cell, w.state, camera);
	}
	render_pending = done < work.size();
	if (debug_bounds_dirty && debug_view != DEBUG_VIEW_DISABLED) {
		_update_debug_bounds();
	}
	last_camera = camera;
	has_last_camera = has_camera;
	last_update_usec = OS::get_singleton()->get_ticks_usec() - begin;
	last_updated_cells = done;
}

/* Settings */

void LandscapeFoliage3D::set_chunk_size(float p_size) {
	p_size = CLAMP(p_size, 4.0f, 4096.0f);
	if (Math::is_equal_approx(p_size, chunk_size)) {
		return;
	}
	chunk_size = p_size;
	_rebin_all();
}

void LandscapeFoliage3D::set_lod_distance_scale(float p_scale) {
	lod_distance_scale = MAX(p_scale, 0.01f);
	for (Entry *entry : entries) {
		_update_entry_lods(entry);
		for (KeyValue<Vector2i, Cell> &kv : entry->cells) {
			kv.value.render_dirty = true;
		}
	}
	render_pending = true;
}

void LandscapeFoliage3D::set_follow_terrain(bool p_enable) {
	follow_terrain = p_enable;
	if (!follow_terrain) {
		snap_rects.clear();
	}
}

void LandscapeFoliage3D::set_render_layers(uint32_t p_layers) {
	render_layers = p_layers;
	_update_all_batch_settings();
}

void LandscapeFoliage3D::set_cast_shadows(bool p_enable) {
	cast_shadows = p_enable;
	_update_all_batch_settings();
}

void LandscapeFoliage3D::force_update() {
	for (Entry *entry : entries) {
		_update_entry_lods(entry);
		for (KeyValue<Vector2i, Cell> &kv : entry->cells) {
			_free_cell_rendering(kv.value);
			kv.value.bounds_dirty = true;
		}
	}
	render_pending = true;
}

Dictionary LandscapeFoliage3D::get_statistics() const {
	int64_t cells = 0;
	int64_t cells_rendered = 0;
	int64_t cells_mixed = 0;
	int64_t batches = 0;
	int64_t drawn = 0;
	for (const Entry *entry : entries) {
		cells += entry->cells.size();
		for (const KeyValue<Vector2i, Cell> &kv : entry->cells) {
			bool rendered = false;
			for (const Batch &batch : kv.value.batches) {
				if (batch.instance.is_valid()) {
					batches++;
					drawn += batch.count;
					rendered = true;
				}
			}
			cells_rendered += rendered ? 1 : 0;
			cells_mixed += kv.value.state == STATE_MIXED ? 1 : 0;
		}
	}
	Dictionary stats;
	stats["types"] = int(entries.size());
	stats["instances"] = get_instance_count();
	stats["cells"] = cells;
	stats["cells_rendered"] = cells_rendered;
	stats["cells_mixed"] = cells_mixed;
	stats["batches"] = batches;
	stats["instances_drawn"] = drawn;
	stats["update_usec"] = int64_t(last_update_usec);
	stats["updated_cells"] = last_updated_cells;
	stats["debug_bounds"] = debug_bounds_count;
	return stats;
}

/* Scene */

Landscape3D *LandscapeFoliage3D::_find_landscape() const {
	Node *parent = get_parent();
	while (parent) {
		Landscape3D *found = Object::cast_to<Landscape3D>(parent);
		if (found) {
			return found;
		}
		parent = parent->get_parent();
	}
	return nullptr;
}

PackedStringArray LandscapeFoliage3D::get_configuration_warnings() const {
	PackedStringArray warnings = Node3D::get_configuration_warnings();
	if (!_find_landscape()) {
		warnings.push_back(RTR("LandscapeFoliage3D paints foliage on a Landscape3D ancestor. Without one, instances can only be added by scripts and don't follow any terrain."));
	}
	for (uint32_t i = 0; i < entries.size(); i++) {
		const Ref<LandscapeFoliageType> &type = entries[i]->type;
		if (type.is_null()) {
			warnings.push_back(vformat(RTR("Foliage type %d is empty: its instances aren't drawn."), i));
		} else if (type->get_mesh().is_null()) {
			warnings.push_back(vformat(RTR("Foliage type %d has no mesh."), i));
		}
	}
	return warnings;
}

void LandscapeFoliage3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			landscape = _find_landscape();
			if (landscape) {
				landscape->_register_foliage(this);
			}
			render_pending = true;
			set_process_internal(true);
		} break;

		case NOTIFICATION_EXIT_TREE: {
			_free_all_rendering();
			if (landscape) {
				landscape->_unregister_foliage(this);
				landscape = nullptr;
			}
			snap_rects.clear();
			set_process_internal(false);
		} break;

		case NOTIFICATION_ENTER_WORLD: {
			render_pending = true;
		} break;

		case NOTIFICATION_EXIT_WORLD: {
			_free_all_rendering();
		} break;

		case NOTIFICATION_TRANSFORM_CHANGED: {
			// Instances are in the space of the landscape: only the landscape moves them.
			const Transform3D space = _get_space_transform();
			RenderingServer *rs = RenderingServer::get_singleton();
			for (Entry *entry : entries) {
				for (KeyValue<Vector2i, Cell> &kv : entry->cells) {
					for (const Batch &batch : kv.value.batches) {
						if (batch.instance.is_valid()) {
							rs->instance_set_transform(batch.instance, space);
						}
					}
				}
			}
			if (debug_bounds_instance.is_valid()) {
				rs->instance_set_transform(debug_bounds_instance, space);
			}
			render_pending = true;
		} break;

		case NOTIFICATION_VISIBILITY_CHANGED: {
			_update_all_batch_settings();
			render_pending = true;
		} break;

		case NOTIFICATION_INTERNAL_PROCESS: {
			_process_snapping();
			_update_rendering();
		} break;
	}
}

void LandscapeFoliage3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("add_foliage_type", "type"), &LandscapeFoliage3D::add_foliage_type);
	ClassDB::bind_method(D_METHOD("remove_foliage_type", "index"), &LandscapeFoliage3D::remove_foliage_type);
	ClassDB::bind_method(D_METHOD("set_foliage_type", "index", "type"), &LandscapeFoliage3D::set_foliage_type);
	ClassDB::bind_method(D_METHOD("get_foliage_type", "index"), &LandscapeFoliage3D::get_foliage_type);
	ClassDB::bind_method(D_METHOD("get_foliage_type_count"), &LandscapeFoliage3D::get_foliage_type_count);
	ClassDB::bind_method(D_METHOD("find_foliage_type", "type"), &LandscapeFoliage3D::find_foliage_type);
	ClassDB::bind_method(D_METHOD("move_foliage_type", "from", "to"), &LandscapeFoliage3D::move_foliage_type);
	ClassDB::bind_method(D_METHOD("set_foliage_types", "types"), &LandscapeFoliage3D::_set_foliage_types_bind);
	ClassDB::bind_method(D_METHOD("get_foliage_types"), &LandscapeFoliage3D::_get_foliage_types_bind);
	ClassDB::bind_method(D_METHOD("set_instance_data", "data"), &LandscapeFoliage3D::_set_instance_data);
	ClassDB::bind_method(D_METHOD("get_instance_data"), &LandscapeFoliage3D::_get_instance_data);

	ClassDB::bind_method(D_METHOD("set_chunk_size", "size"), &LandscapeFoliage3D::set_chunk_size);
	ClassDB::bind_method(D_METHOD("get_chunk_size"), &LandscapeFoliage3D::get_chunk_size);
	ClassDB::bind_method(D_METHOD("set_lod_distance_scale", "scale"), &LandscapeFoliage3D::set_lod_distance_scale);
	ClassDB::bind_method(D_METHOD("get_lod_distance_scale"), &LandscapeFoliage3D::get_lod_distance_scale);
	ClassDB::bind_method(D_METHOD("set_follow_terrain", "enable"), &LandscapeFoliage3D::set_follow_terrain);
	ClassDB::bind_method(D_METHOD("is_following_terrain"), &LandscapeFoliage3D::is_following_terrain);
	ClassDB::bind_method(D_METHOD("set_render_layers", "layers"), &LandscapeFoliage3D::set_render_layers);
	ClassDB::bind_method(D_METHOD("get_render_layers"), &LandscapeFoliage3D::get_render_layers);
	ClassDB::bind_method(D_METHOD("set_cast_shadows", "enable"), &LandscapeFoliage3D::set_cast_shadows);
	ClassDB::bind_method(D_METHOD("is_casting_shadows"), &LandscapeFoliage3D::is_casting_shadows);

	ClassDB::bind_method(D_METHOD("paint", "type_index", "global_center", "radius", "density_scale"), &LandscapeFoliage3D::paint, DEFVAL(1.0));
	ClassDB::bind_method(D_METHOD("erase", "type_index", "global_center", "radius", "density_scale"), &LandscapeFoliage3D::erase, DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("place_instance", "type_index", "global_position"), &LandscapeFoliage3D::place_instance);
	ClassDB::bind_method(D_METHOD("fill", "type_index", "rect"), &LandscapeFoliage3D::fill, DEFVAL(Rect2()));
	ClassDB::bind_method(D_METHOD("reapply", "type_index", "global_center", "radius"), &LandscapeFoliage3D::reapply);
	ClassDB::bind_method(D_METHOD("clear", "type_index"), &LandscapeFoliage3D::clear, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("snap_to_terrain", "rect"), &LandscapeFoliage3D::snap_to_terrain, DEFVAL(Rect2()));
	ClassDB::bind_method(D_METHOD("estimate_fill_count", "type_index"), &LandscapeFoliage3D::estimate_fill_count);

	ClassDB::bind_method(D_METHOD("get_instance_count", "type_index"), &LandscapeFoliage3D::get_instance_count, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("add_instance", "type_index", "global_transform"), &LandscapeFoliage3D::add_instance);
	ClassDB::bind_method(D_METHOD("add_instances", "type_index", "global_transforms"), &LandscapeFoliage3D::add_instances);
	ClassDB::bind_method(D_METHOD("get_instance_transforms", "type_index", "global_aabb"), &LandscapeFoliage3D::get_instance_transforms, DEFVAL(AABB()));
	ClassDB::bind_method(D_METHOD("get_instances_in_radius", "type_index", "global_center", "radius"), &LandscapeFoliage3D::get_instances_in_radius);
	ClassDB::bind_method(D_METHOD("remove_instances_in_radius", "type_index", "global_center", "radius"), &LandscapeFoliage3D::remove_instances_in_radius);
	ClassDB::bind_method(D_METHOD("get_instances_in_rect", "type_index", "rect"), &LandscapeFoliage3D::get_instances_in_rect);
	ClassDB::bind_method(D_METHOD("set_instances_in_rect", "type_index", "rect", "instances"), &LandscapeFoliage3D::set_instances_in_rect);
	ClassDB::bind_method(D_METHOD("get_type_data", "type_index"), &LandscapeFoliage3D::get_type_data);
	ClassDB::bind_method(D_METHOD("set_type_data", "type_index", "data"), &LandscapeFoliage3D::set_type_data);
	ClassDB::bind_method(D_METHOD("set_debug_view", "view"), &LandscapeFoliage3D::set_debug_view);
	ClassDB::bind_method(D_METHOD("get_debug_view"), &LandscapeFoliage3D::get_debug_view);
	ClassDB::bind_method(D_METHOD("get_statistics"), &LandscapeFoliage3D::get_statistics);
	ClassDB::bind_method(D_METHOD("force_update"), &LandscapeFoliage3D::force_update);

	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "foliage_types", PROPERTY_HINT_ARRAY_TYPE, MAKE_RESOURCE_TYPE_HINT(LandscapeFoliageType::get_class_static()), PROPERTY_USAGE_NO_EDITOR), "set_foliage_types", "get_foliage_types");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "instance_data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), "set_instance_data", "get_instance_data");

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "chunk_size", PROPERTY_HINT_RANGE, "4,1024,0.1,or_greater,suffix:m"), "set_chunk_size", "get_chunk_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lod_distance_scale", PROPERTY_HINT_RANGE, "0.01,8,0.01,or_greater"), "set_lod_distance_scale", "get_lod_distance_scale");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "follow_terrain"), "set_follow_terrain", "is_following_terrain");

	ADD_GROUP("Rendering", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "render_layers", PROPERTY_HINT_LAYERS_3D_RENDER), "set_render_layers", "get_render_layers");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "cast_shadows"), "set_cast_shadows", "is_casting_shadows");

	ADD_GROUP("Debug", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "debug_view", PROPERTY_HINT_ENUM, "Disabled,LOD Levels,Cells,Cell State"), "set_debug_view", "get_debug_view");

	BIND_ENUM_CONSTANT(DEBUG_VIEW_DISABLED);
	BIND_ENUM_CONSTANT(DEBUG_VIEW_LOD_LEVELS);
	BIND_ENUM_CONSTANT(DEBUG_VIEW_CELLS);
	BIND_ENUM_CONSTANT(DEBUG_VIEW_CELL_STATE);

	BIND_CONSTANT(INSTANCE_FLOATS);
}

LandscapeFoliage3D::LandscapeFoliage3D() {
	set_notify_transform(true);
	rng.seed(uint64_t(OS::get_singleton()->get_ticks_usec()) ^ hash_murmur3_one_64(uint64_t(get_instance_id())));
}

LandscapeFoliage3D::~LandscapeFoliage3D() {
	for (Entry *entry : entries) {
		_connect_type(entry, false);
		for (KeyValue<Vector2i, Cell> &kv : entry->cells) {
			_free_cell_rendering(kv.value);
		}
		memdelete(entry);
	}
	entries.clear();
	_free_debug_bounds();
}
