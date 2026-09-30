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
#include "landscape_foliage_gpu.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/io/compression.h"
#include "core/io/marshalls.h"
#include "core/io/resource_saver.h"
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

#include "modules/streaming/world_streaming.h"

// Level of an instance whose level is not known yet (per-instance sort).
static constexpr uint8_t FOLIAGE_LOD_UNKNOWN = 254;
// Candidates evaluated by a paint dab (the density is estimated from at least the minimum).
static constexpr int MIN_PAINT_CANDIDATES = 16;
static constexpr int MAX_PAINT_CANDIDATES = 65536;
// Camera move that triggers a new evaluation of the cells.
static constexpr real_t MIN_CAMERA_STEP = 0.25;
// Capacity of an indirect MultiMesh of the GPU culling (24 bits).
static constexpr int FOLIAGE_GPU_MAX_OUTPUT = (1 << 24) - 1;

Ref<Shader> LandscapeFoliage3D::debug_shader;
Ref<ShaderMaterial> LandscapeFoliage3D::debug_material;
Ref<StandardMaterial3D> LandscapeFoliage3D::debug_bounds_material;

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
		_free_gpu_outputs(entry); // Other meshes or levels.
		entry->gpu_dirty = true;
		render_pending = true;
		streaming_dirty = true; // Other cull distance.
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
	entry->gpu_id = next_gpu_id++;
	_connect_type(entry, true);
	_update_entry_lods(entry);
	entries.push_back(entry);
	if (data.is_valid()) {
		// Layers are aligned with the types: the type takes the layer of its index, if any.
		const int index = entries.size() - 1;
		if (index < data->get_layer_count()) {
			_populate_entry(entry, index);
		} else {
			data->set_layer_count(entries.size());
			_mark_data_edited();
		}
	}
	render_pending = true;
	notify_property_list_changed();
	update_configuration_warnings();
	return entries.size() - 1;
}

void LandscapeFoliage3D::remove_foliage_type(int p_index) {
	Entry *entry = _get_entry(p_index);
	ERR_FAIL_NULL(entry);
	_connect_type(entry, false);
	if (data.is_valid()) {
		_flush_data();
		_cancel_requests(); // The loads are addressed by layer index.
	}
	_drop_cells(entry);
	_free_gpu(entry);
	memdelete(entry);
	entries.remove_at(p_index);
	if (data.is_valid() && p_index < data->get_layer_count()) {
		data->remove_layer(p_index);
		_mark_data_edited();
		streaming_dirty = true;
	}
	render_pending = true;
	debug_bounds_dirty = true;
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
	_free_gpu_outputs(entry);
	entry->gpu_dirty = true;
	render_pending = true;
	streaming_dirty = true;
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
	if (data.is_valid()) {
		_flush_data();
		_cancel_requests(); // The loads are addressed by layer index.
		data->set_layer_count(MAX(data->get_layer_count(), int(entries.size())));
		data->move_layer(p_from, p_to);
		_mark_data_edited();
		streaming_dirty = true;
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
	} else if (!cell->loaded) {
		_load_cell(p_entry, *cell); // Edited: its instances are needed.
	}
	return *cell;
}

void LandscapeFoliage3D::_cell_changed(Entry *p_entry, Cell &r_cell) {
	r_cell.render_dirty = true;
	r_cell.bounds_dirty = true;
	r_cell.state = STATE_NONE;
	r_cell.instance_lods.clear();
	r_cell.data_dirty = true;
	r_cell.gpu_dirty = true;
	p_entry->serialized_dirty = true;
	p_entry->data_dirty = true;
	p_entry->gpu_dirty = true;
	data_dirty = true;
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
	if (!r_cell.loaded) {
		// Only the positions are known: grown by the meshes at the largest scale of the type.
		real_t reach = 1.0;
		if (p_entry->mesh_bounds.has_volume() || p_entry->mesh_bounds.has_surface()) {
			const AABB &mesh = p_entry->mesh_bounds;
			reach = mesh.position.abs().max((mesh.position + mesh.size).abs()).length();
		}
		real_t scale = 1.0;
		if (p_entry->type.is_valid()) {
			scale = MAX(MAX(p_entry->type->get_scale_max(), p_entry->type->get_vertical_scale_max()), 1.0f);
		}
		r_cell.bounds = r_cell.stored_bounds.grow(reach * scale + 0.01);
		return;
	}
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
				if (!kv.value.loaded) {
					_load_cell(p_entry, kv.value);
				}
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
				if (!cell->loaded) {
					_load_cell(p_entry, *cell);
				}
				p_function(key, *cell);
			}
		}
	}
}

void LandscapeFoliage3D::_remove_empty_cells(Entry *p_entry) {
	LocalVector<Vector2i> empty;
	for (KeyValue<Vector2i, Cell> &kv : p_entry->cells) {
		if (kv.value.loaded && kv.value.instances.is_empty()) {
			_free_cell_rendering(kv.value);
			empty.push_back(kv.key);
		}
	}
	const int index = data.is_valid() ? _get_entry_index(p_entry) : -1;
	for (const Vector2i &key : empty) {
		p_entry->cells.erase(key);
		if (index >= 0) {
			data->write_cell(index, key, nullptr, 0);
		}
	}
	if (index >= 0 && !empty.is_empty()) {
		_mark_data_edited();
	}
}

void LandscapeFoliage3D::_rebin_all() {
	if (data.is_valid()) {
		// The data sorts its instances into the new cells.
		_flush_data();
		data->set_chunk_size(chunk_size);
		_mark_data_edited();
		_reload_from_data();
		return;
	}
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
		if (all) {
			_ensure_all_loaded(entry);
		}
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
		_drop_cells(entry);
		if (data.is_valid() && int(i) < data->get_layer_count()) {
			data->clear_layer(i);
			_mark_data_edited();
		}
	}
	render_pending = true;
	debug_bounds_dirty = true;
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
	const_cast<LandscapeFoliage3D *>(this)->_ensure_all_loaded(const_cast<Entry *>(entry));
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

PackedByteArray LandscapeFoliage3D::get_type_data(int p_type_index) const {
	Entry *entry = _get_entry(p_type_index);
	ERR_FAIL_NULL_V(entry, PackedByteArray());
	if (entry->serialized_dirty) {
		const_cast<LandscapeFoliage3D *>(this)->_ensure_all_loaded(entry);
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
		entry->serialized = LandscapeFoliageInstance::encode(all.ptr(), all.size());
		entry->serialized_dirty = false;
	}
	return entry->serialized;
}

void LandscapeFoliage3D::set_type_data(int p_type_index, const PackedByteArray &p_data) {
	Entry *entry = _get_entry(p_type_index);
	ERR_FAIL_NULL(entry);
	LocalVector<Instance> instances;
	if (!LandscapeFoliageInstance::decode(p_data, instances)) {
		return;
	}
	_drop_cells(entry);
	if (data.is_valid() && p_type_index < data->get_layer_count()) {
		data->clear_layer(p_type_index); // The new cells are written by the next flush.
		_mark_data_edited();
	}
	for (const Instance &instance : instances) {
		_add_instance(entry, instance);
	}
	entry->serialized_dirty = true;
	entry->gpu_dirty = true;
	render_pending = true;
	debug_bounds_dirty = true;
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
	if (data.is_valid()) {
		return instance_data; // Saved in the data.
	}
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

Camera3D *LandscapeFoliage3D::_get_view_camera() const {
	// The camera of the LOD of the landscape.
	Camera3D *camera = nullptr;
	if (landscape) {
		camera = landscape->get_lod_camera();
	} else {
#ifdef TOOLS_ENABLED
		if (Engine::get_singleton()->is_editor_hint() && Landscape3D::editor_camera_callback && is_part_of_edited_scene()) {
			camera = Landscape3D::editor_camera_callback();
		}
#endif
		if (!camera && get_viewport()) {
			camera = get_viewport()->get_camera_3d();
		}
	}
	return (camera && camera->is_inside_tree()) ? camera : nullptr;
}

bool LandscapeFoliage3D::_get_camera(Vector3 &r_position) const {
	const Camera3D *camera = _get_view_camera();
	if (!camera) {
		return false;
	}
	r_position = global_to_landscape(camera->get_camera_transform().origin);
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
		_free_gpu(entry);
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
	_update_all_gpu_output_settings();
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

Color LandscapeFoliage3D::get_debug_gpu_color() {
	// Green: culled and sorted on the GPU (gpu_indirect).
	return Color(0.45, 0.9, 0.4);
}

static _FORCE_INLINE_ int _foliage_debug_cell_index(const Vector2i &p_key) {
	// Also computed by the GPU culling (custom data of the instances), in 16 bits.
	return (p_key.x * 7919 + p_key.y * 104729) & 0xFFFF;
}

RID LandscapeFoliage3D::_get_debug_material() {
	if (debug_material.is_null()) {
		debug_shader.instantiate();
		// The color is an instance uniform, or the color of the cell of the instance (GPU culling).
		debug_shader->set_code(R"(
shader_type spatial;
render_mode cull_disabled;

instance uniform vec3 foliage_debug_color = vec3(1.0);
instance uniform int foliage_debug_source = 0;

varying flat float cell;

vec3 debug_color(int p_index) {
	float h = fract(float(p_index) * 0.61803398875);
	vec3 c = clamp(abs(fract(h + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
	return mix(vec3(1.0), c, 0.75);
}

void vertex() {
	cell = INSTANCE_CUSTOM.y;
}

void fragment() {
	ALBEDO = foliage_debug_source == 1 ? debug_color(int(cell + 0.5)) : foliage_debug_color;
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
			return get_debug_color(_foliage_debug_cell_index(p_cell.key));
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
	const bool gpu_active = _is_gpu_active();
	for (const Entry *entry : entries) {
		for (const KeyValue<Vector2i, Cell> &kv : entry->cells) {
			const Cell &cell = kv.value;
			bool drawn = false;
			if (gpu_active) {
				// The cells uploaded to the GPU.
				drawn = cell.loaded && entry->gpu_slots.has(kv.key);
			} else {
				for (const Batch &batch : cell.batches) {
					drawn = drawn || batch.instance.is_valid();
				}
			}
			if (!drawn) {
				continue;
			}
			Color color = _get_debug_cell_color(cell, MAX(cell.state, 0));
			if (gpu_active) {
				color = debug_view == DEBUG_VIEW_CELLS ? color : (debug_view == DEBUG_VIEW_CELL_STATE ? get_debug_gpu_color() : Color(1, 1, 1));
			} else if (debug_view == DEBUG_VIEW_LOD_LEVELS && cell.state == STATE_MIXED) {
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
	if (_is_gpu_active()) {
		_update_gpu(camera, has_camera);
		last_camera = camera;
		has_last_camera = has_camera;
		return;
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
			if (!cell.loaded) {
				continue; // Streamed out (no rendering resources).
			}
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

/* Streaming */

bool LandscapeFoliage3D::_is_streamed() const {
	return data.is_valid() && data->is_streamed();
}

int LandscapeFoliage3D::_get_entry_index(const Entry *p_entry) const {
	for (uint32_t i = 0; i < entries.size(); i++) {
		if (entries[i] == p_entry) {
			return i;
		}
	}
	return -1;
}

void LandscapeFoliage3D::_add_loaded_bytes(int64_t p_bytes) {
	// Only the instances of a data are reported (with the memory of the data itself).
	if (data.is_null() || p_bytes == 0) {
		return;
	}
	loaded_bytes += p_bytes;
	if (WorldStreaming::get_singleton()) {
		WorldStreaming::get_singleton()->add_pool_usage(SNAME("Foliage CPU"), p_bytes);
	}
}

void LandscapeFoliage3D::_cancel_requests(const Entry *p_entry) {
	for (int64_t i = int64_t(requests.size()) - 1; i >= 0; i--) {
		const CellRequest request = requests[i];
		if (p_entry && request.entry != p_entry) {
			continue;
		}
		Cell *cell = request.entry->cells.getptr(request.key);
		if (cell) {
			cell->requested = false;
		}
		const int index = _get_entry_index(request.entry);
		if (data.is_valid() && index >= 0) {
			data->cancel_request(index, request.key);
		}
		requests.remove_at_unordered(i);
	}
}

void LandscapeFoliage3D::_drop_cells(Entry *p_entry) {
	_cancel_requests(p_entry);
	int64_t bytes = 0;
	for (KeyValue<Vector2i, Cell> &kv : p_entry->cells) {
		_free_cell_rendering(kv.value);
		if (kv.value.loaded) {
			bytes += int64_t(kv.value.instances.size()) * sizeof(Instance);
			loaded_cells -= data.is_valid() ? 1 : 0;
		}
	}
	_add_loaded_bytes(-bytes);
	p_entry->cells.clear();
	p_entry->count = 0;
	p_entry->serialized_dirty = true;
	p_entry->data_dirty = false;
	p_entry->gpu_dirty = true;
	render_pending = true;
	debug_bounds_dirty = true;
}

void LandscapeFoliage3D::_populate_entry(Entry *p_entry, int p_index) {
	// The cells of the layer, not loaded yet (only their instance count and bounds are known).
	_drop_cells(p_entry);
	const HashMap<Vector2i, LandscapeFoliageData::CellInfo> *infos = data.is_valid() ? data->get_cells(p_index) : nullptr;
	if (infos) {
		for (const KeyValue<Vector2i, LandscapeFoliageData::CellInfo> &kv : *infos) {
			Cell &cell = p_entry->cells.insert(kv.key, Cell())->value;
			cell.key = kv.key;
			cell.loaded = false;
			cell.stored_count = kv.value.count;
			cell.stored_bounds = kv.value.bounds;
			p_entry->count += kv.value.count;
		}
	}
	streaming_dirty = true;
}

void LandscapeFoliage3D::_reload_from_data() {
	_cancel_requests();
	chunk_size = data->get_chunk_size();
	for (uint32_t i = 0; i < entries.size(); i++) {
		_populate_entry(entries[i], i);
	}
	data_dirty = false;
	streaming_dirty = true;
	render_pending = true;
	debug_bounds_dirty = true;
}

void LandscapeFoliage3D::_adopt_loaded_cells() {
	// The data holds the same instances as the loaded cells of the node.
	int64_t bytes = 0;
	loaded_cells = 0;
	for (Entry *entry : entries) {
		entry->data_dirty = false;
		for (KeyValue<Vector2i, Cell> &kv : entry->cells) {
			kv.value.data_dirty = false;
			if (kv.value.loaded) {
				bytes += int64_t(kv.value.instances.size()) * sizeof(Instance);
				loaded_cells++;
			}
		}
	}
	data_dirty = false;
	_add_loaded_bytes(bytes - loaded_bytes);
	streaming_dirty = true;
}

void LandscapeFoliage3D::_detach_data() {
	// Every instance goes back to the node (saved in the scene).
	_flush_data();
	_cancel_requests();
	for (Entry *entry : entries) {
		_ensure_all_loaded(entry);
		entry->serialized_dirty = true;
		entry->data_dirty = false;
		for (KeyValue<Vector2i, Cell> &kv : entry->cells) {
			kv.value.data_dirty = false;
		}
	}
	data->remove_flush_callback(callable_mp(this, &LandscapeFoliage3D::_flush_data));
	_add_loaded_bytes(-loaded_bytes);
	loaded_bytes = 0;
	loaded_cells = 0;
	data_dirty = false;
	data.unref();
}

void LandscapeFoliage3D::_install_cell(Entry *p_entry, Cell &r_cell, LocalVector<Instance> &r_instances) {
	p_entry->count += int64_t(r_instances.size()) - int64_t(r_cell.stored_count);
	r_cell.instances = std::move(r_instances);
	r_cell.loaded = true;
	r_cell.requested = false; // Its request is dropped by the next poll.
	r_cell.data_dirty = false;
	r_cell.stored_count = 0;
	r_cell.bounds_dirty = true;
	r_cell.render_dirty = true;
	r_cell.state = STATE_NONE;
	r_cell.instance_lods.clear();
	r_cell.gpu_dirty = true;
	p_entry->gpu_dirty = true;
	render_pending = true;
	debug_bounds_dirty = true;
	loaded_cells++;
	_add_loaded_bytes(int64_t(r_cell.instances.size()) * sizeof(Instance));
}

void LandscapeFoliage3D::_load_cell(Entry *p_entry, Cell &r_cell) {
	// Now (edits and queries): waits for its load if it is in progress.
	LocalVector<Instance> instances;
	const int index = _get_entry_index(p_entry);
	if (data.is_valid() && index >= 0) {
		data->read_cell(index, r_cell.key, instances);
	}
	_install_cell(p_entry, r_cell, instances);
}

void LandscapeFoliage3D::_evict_cell(Entry *p_entry, Cell &r_cell) {
	const int index = _get_entry_index(p_entry);
	if (!r_cell.loaded || data.is_null() || index < 0) {
		return;
	}
	if (r_cell.data_dirty) {
		data->write_cell(index, r_cell.key, r_cell.instances.ptr(), r_cell.instances.size());
		r_cell.data_dirty = false;
		_mark_data_edited();
	}
	_free_cell_rendering(r_cell);
	AABB positions;
	for (uint32_t i = 0; i < r_cell.instances.size(); i++) {
		if (i == 0) {
			positions = AABB(r_cell.instances[i].get_position(), Vector3());
		} else {
			positions.expand_to(r_cell.instances[i].get_position());
		}
	}
	_add_loaded_bytes(-int64_t(r_cell.instances.size()) * int64_t(sizeof(Instance)));
	r_cell.stored_count = r_cell.instances.size();
	r_cell.stored_bounds = positions;
	r_cell.instances.reset();
	r_cell.instance_lods.reset();
	r_cell.loaded = false;
	r_cell.bounds_dirty = true;
	p_entry->gpu_dirty = true;
	loaded_cells--;
}

void LandscapeFoliage3D::_ensure_all_loaded(Entry *p_entry) {
	for (KeyValue<Vector2i, Cell> &kv : p_entry->cells) {
		if (!kv.value.loaded) {
			_load_cell(p_entry, kv.value);
		}
	}
}

void LandscapeFoliage3D::load_all_cells() {
	for (Entry *entry : entries) {
		_ensure_all_loaded(entry);
	}
}

void LandscapeFoliage3D::_flush_data() {
	// The edited cells are written to the data (in memory until it is saved).
	if (data.is_null() || !data_dirty) {
		return;
	}
	for (uint32_t i = 0; i < entries.size(); i++) {
		Entry *entry = entries[i];
		if (!entry->data_dirty) {
			continue;
		}
		for (KeyValue<Vector2i, Cell> &kv : entry->cells) {
			Cell &cell = kv.value;
			if (cell.loaded && cell.data_dirty) {
				data->write_cell(i, kv.key, cell.instances.ptr(), cell.instances.size());
				cell.data_dirty = false;
			}
		}
		entry->data_dirty = false;
	}
	data_dirty = false;
}

void LandscapeFoliage3D::_mark_data_edited() {
#ifdef TOOLS_ENABLED
	// Saved with the scene.
	if (data.is_valid() && Engine::get_singleton()->is_editor_hint()) {
		data->set_edited(true);
	}
#endif
}

void LandscapeFoliage3D::set_data(const Ref<LandscapeFoliageData> &p_data) {
	if (data == p_data) {
		return;
	}
	if (data.is_valid()) {
		_detach_data();
	}
	data = p_data;
	if (data.is_valid()) {
		data->add_flush_callback(callable_mp(this, &LandscapeFoliage3D::_flush_data));
		if (data->get_cell_count() == 0) {
			// An empty data takes the instances of the node.
			data->set_chunk_size(chunk_size);
			data->set_layer_count(MAX(data->get_layer_count(), int(entries.size())));
			for (uint32_t i = 0; i < entries.size(); i++) {
				for (const KeyValue<Vector2i, Cell> &kv : entries[i]->cells) {
					if (!kv.value.instances.is_empty()) {
						data->write_cell(i, kv.key, kv.value.instances.ptr(), kv.value.instances.size());
					}
				}
			}
			_adopt_loaded_cells();
			if (get_instance_count() > 0) {
				_mark_data_edited();
			}
		} else {
			if (data->get_layer_count() < int(entries.size())) {
				data->set_layer_count(entries.size());
				_mark_data_edited();
			}
			_reload_from_data();
		}
	}
	streaming_dirty = true;
	render_pending = true;
	update_configuration_warnings();
}

Error LandscapeFoliage3D::save_to_data_file(const String &p_path) {
	ERR_FAIL_COND_V(p_path.is_empty(), ERR_INVALID_PARAMETER);
	Ref<LandscapeFoliageData> target = data;
	if (target.is_null()) {
		target.instantiate();
		target->set_chunk_size(chunk_size);
		target->set_layer_count(entries.size());
		for (uint32_t i = 0; i < entries.size(); i++) {
			for (const KeyValue<Vector2i, Cell> &kv : entries[i]->cells) {
				if (!kv.value.instances.is_empty()) {
					target->write_cell(i, kv.key, kv.value.instances.ptr(), kv.value.instances.size());
				}
			}
		}
	}
	const String path = ProjectSettings::get_singleton()->localize_path(p_path);
	const Error err = ResourceSaver::save(target, path);
	ERR_FAIL_COND_V_MSG(err != OK, err, vformat("Can't save the foliage data to '%s'.", path));
	if (target->get_path() != path) {
		target->set_path(path, true); // Loaded from this path from now on.
	}
	if (target != data) {
		// The loaded cells are kept: the far ones are released by the streaming.
		data = target;
		data->add_flush_callback(callable_mp(this, &LandscapeFoliage3D::_flush_data));
		_adopt_loaded_cells();
		update_configuration_warnings();
	}
	return OK;
}

void LandscapeFoliage3D::_process_streaming() {
	if (data.is_null()) {
		return;
	}
	if (WorldStreaming::get_singleton()) {
		WorldStreaming::get_singleton()->process(); // Finishes the loads.
	}
	if (data_dirty) {
		_flush_data();
		_mark_data_edited();
	}

	// Finished loads.
	for (int64_t i = int64_t(requests.size()) - 1; i >= 0; i--) {
		const CellRequest request = requests[i];
		Cell *cell = request.entry->cells.getptr(request.key);
		if (!cell || cell->loaded || !cell->requested) {
			requests.remove_at_unordered(i);
			continue;
		}
		LocalVector<Instance> instances;
		if (data->request_cell(_get_entry_index(request.entry), request.key, cell->request_priority, instances)) {
			_install_cell(request.entry, *cell, instances);
			requests.remove_at_unordered(i);
		}
	}

	// What is needed: the cells within the cull distance of the camera and of the streaming
	// sources (scaled by their range), every cell without camera or cull distance.
	LocalVector<Vector3> centers;
	LocalVector<real_t> scales;
	Vector3 camera;
	if (_get_camera(camera)) {
		centers.push_back(camera);
		scales.push_back(1.0);
	}
	if (WorldStreaming::get_singleton() && is_inside_tree() && get_world_3d().is_valid()) {
		for (const WorldStreaming::Source &source : WorldStreaming::get_singleton()->get_sources(get_world_3d()->get_scenario())) {
			centers.push_back(global_to_landscape(source.position));
			scales.push_back(MAX(source.range_scale, real_t(0.0)));
		}
	}
	bool moved = streaming_dirty || centers.size() != stream_centers.size();
	const real_t step = chunk_size * 0.25f;
	for (uint32_t i = 0; i < centers.size() && !moved; i++) {
		moved = centers[i].distance_squared_to(stream_centers[i]) > step * step || !Math::is_equal_approx(scales[i], stream_scales[i]);
	}
	if (!moved) {
		return;
	}
	streaming_dirty = false;
	stream_centers = centers;
	stream_scales = scales;

	struct Candidate {
		real_t distance;
		Entry *entry;
		Cell *cell;
		bool operator<(const Candidate &p_other) const { return distance > p_other.distance; } // Farthest first.
	};
	LocalVector<Candidate> evictable;
	int64_t bytes = 0;
	int cells = 0;
	for (uint32_t index = 0; index < entries.size(); index++) {
		Entry *entry = entries[index];
		const bool all = centers.is_empty() || entry->cull_end <= 0.0f;
		const real_t radius = entry->cull_end + entry->transition * 0.5f + chunk_size * 0.5f;
		const real_t keep = radius * 1.25f + chunk_size; // Hysteresis.
		for (KeyValue<Vector2i, Cell> &kv : entry->cells) {
			Cell &cell = kv.value;
			real_t distance = 0.0;
			if (!all) {
				_update_cell_bounds(entry, cell);
				const Vector3 begin = cell.bounds.position;
				const Vector3 end = cell.bounds.position + cell.bounds.size;
				distance = Math::INF;
				for (uint32_t i = 0; i < centers.size(); i++) {
					if (scales[i] > 0.0) {
						distance = MIN(distance, centers[i].distance_to(centers[i].clamp(begin, end)) / scales[i]);
					}
				}
			}
			const bool needed = entry->type.is_valid() && distance <= radius;
			if (needed) {
				if (!cell.loaded) {
					cell.request_priority = -float(MIN(distance, real_t(1e9)));
					if (!cell.requested) {
						LocalVector<Instance> instances;
						if (data->request_cell(index, kv.key, cell.request_priority, instances)) {
							_install_cell(entry, cell, instances);
						} else {
							cell.requested = true;
							requests.push_back({ entry, kv.key });
						}
					}
				}
			} else if (cell.requested) {
				if (entry->type.is_null() || distance > keep) {
					data->cancel_request(index, kv.key);
					cell.requested = false; // Its request is dropped by the next poll.
				}
			} else if (cell.loaded && !cell.data_dirty) {
				if (entry->type.is_null() || distance > keep) {
					_evict_cell(entry, cell);
				} else {
					evictable.push_back({ distance, entry, &cell });
				}
			}
			if (cell.loaded) {
				bytes += int64_t(cell.instances.size()) * sizeof(Instance);
				cells++;
			}
		}
	}
	// Fixes the drift of the edits.
	_add_loaded_bytes(bytes - loaded_bytes);
	loaded_cells = cells;

	// Over the budget: the farthest cells that aren't needed are released first.
	const int64_t budget = int64_t(GLOBAL_GET("rendering/landscape/streaming/foliage_cache_size_mb")) * 1024 * 1024;
	if (loaded_bytes > budget && !evictable.is_empty()) {
		evictable.sort();
		for (const Candidate &candidate : evictable) {
			if (loaded_bytes <= budget) {
				break;
			}
			_evict_cell(candidate.entry, *candidate.cell);
		}
	}
}

/* GPU indirect rendering */

bool LandscapeFoliage3D::is_gpu_indirect_supported() {
#ifdef RD_ENABLED
	return RenderingServer::get_singleton() && RenderingServer::get_singleton()->get_rendering_device() != nullptr;
#else
	return false;
#endif
}

bool LandscapeFoliage3D::_is_gpu_active() const {
	return gpu_indirect && gpu != nullptr && is_inside_tree();
}

void LandscapeFoliage3D::_gpu_call(const Callable &p_callable) {
	RenderingServer::get_singleton()->call_on_render_thread(p_callable);
}

void LandscapeFoliage3D::_free_gpu_outputs(Entry *p_entry) {
	RenderingServer *rs = RenderingServer::get_singleton();
	for (int list = 0; list < 2; list++) {
		for (int lod = 0; lod < LandscapeFoliageType::MAX_LODS; lod++) {
			Entry::GPUOutput &output = p_entry->gpu_outputs[list][lod];
			if (output.instance.is_valid()) {
				rs->free_rid(output.instance);
			}
			if (output.multimesh.is_valid()) {
				rs->free_rid(output.multimesh);
			}
			output = Entry::GPUOutput();
		}
	}
	p_entry->gpu_outputs_dirty = true;
	p_entry->gpu_run = true;
	p_entry->gpu_dropped = 0;
}

void LandscapeFoliage3D::_free_gpu(Entry *p_entry) {
	_free_gpu_outputs(p_entry);
#ifdef RD_ENABLED
	if (gpu && p_entry->gpu_capacity > 0) {
		_gpu_call(callable_mp(gpu, &LandscapeFoliageGPU::free_entry).bind(p_entry->gpu_id));
	}
#endif
	p_entry->gpu_slots.clear();
	p_entry->gpu_free.clear();
	p_entry->gpu_capacity = 0;
	p_entry->gpu_used = 0;
	p_entry->gpu_count = 0;
	p_entry->gpu_bounds = AABB();
	p_entry->gpu_dirty = true;
	for (KeyValue<Vector2i, Cell> &kv : p_entry->cells) {
		kv.value.gpu_dirty = true;
	}
	debug_bounds_dirty = true;
}

void LandscapeFoliage3D::_update_gpu_output_settings(Entry *p_entry, int p_list, int p_lod) {
	const Entry::GPUOutput &output = p_entry->gpu_outputs[p_list][p_lod];
	if (output.instance.is_null()) {
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	rs->instance_set_transform(output.instance, _get_space_transform());
	rs->instance_set_layer_mask(output.instance, render_layers);
	rs->instance_set_visible(output.instance, is_visible_in_tree());
	// The shadows of a level are drawn by its shadow list: every instance within the cull
	// distance, also out of the view.
	rs->instance_geometry_set_cast_shadows_setting(output.instance, p_list == 1 ? RSE::SHADOW_CASTING_SETTING_SHADOWS_ONLY : RSE::SHADOW_CASTING_SETTING_OFF);
	if (debug_view != DEBUG_VIEW_DISABLED) {
		// Cells: the color of the cell of each instance (custom data).
		rs->instance_geometry_set_material_override(output.instance, _get_debug_material());
		const Color color = debug_view == DEBUG_VIEW_LOD_LEVELS ? get_debug_color(p_lod) : get_debug_gpu_color();
		rs->instance_geometry_set_shader_parameter(output.instance, SNAME("foliage_debug_color"), color);
		rs->instance_geometry_set_shader_parameter(output.instance, SNAME("foliage_debug_source"), debug_view == DEBUG_VIEW_CELLS ? 1 : 0);
		return;
	}
	const Ref<Material> material = p_entry->type.is_valid() ? p_entry->type->get_material_override() : Ref<Material>();
	rs->instance_geometry_set_material_override(output.instance, material.is_valid() ? material->get_rid() : RID());
}

void LandscapeFoliage3D::_update_all_gpu_output_settings() {
	for (Entry *entry : entries) {
		for (int list = 0; list < 2; list++) {
			for (int lod = 0; lod < LandscapeFoliageType::MAX_LODS; lod++) {
				_update_gpu_output_settings(entry, list, lod);
			}
		}
	}
}

bool LandscapeFoliage3D::_allocate_gpu_slot(Entry *p_entry, uint32_t p_capacity, uint32_t &r_offset) {
	// First fit among the released slots, then after the last slot.
	for (uint32_t i = 0; i < p_entry->gpu_free.size(); i++) {
		Entry::GPUSlot &range = p_entry->gpu_free[i];
		if (range.capacity >= p_capacity) {
			r_offset = range.offset;
			range.offset += p_capacity;
			range.capacity -= p_capacity;
			if (range.capacity == 0) {
				p_entry->gpu_free.remove_at_unordered(i);
			}
			return true;
		}
	}
	if (uint64_t(p_entry->gpu_used) + p_capacity <= p_entry->gpu_capacity) {
		r_offset = p_entry->gpu_used;
		p_entry->gpu_used += p_capacity;
		return true;
	}
	return false;
}

void LandscapeFoliage3D::_release_gpu_slot(Entry *p_entry, const Vector2i &p_key) {
	const Entry::GPUSlot *slot = p_entry->gpu_slots.getptr(p_key);
	if (!slot) {
		return;
	}
#ifdef RD_ENABLED
	// Its instances become holes.
	Vector<float> holes;
	holes.resize(int64_t(slot->capacity) * INSTANCE_FLOATS);
	memset(holes.ptrw(), 0, holes.size() * sizeof(float));
	_gpu_call(callable_mp(gpu, &LandscapeFoliageGPU::update_instances).bind(p_entry->gpu_id, slot->offset, holes));
#endif
	if (slot->offset + slot->capacity == p_entry->gpu_used) {
		p_entry->gpu_used = slot->offset;
	} else {
		p_entry->gpu_free.push_back(*slot);
	}
	p_entry->gpu_slots.erase(p_key);
}

void LandscapeFoliage3D::_write_gpu_slot(const Cell &p_cell, const Entry::GPUSlot &p_slot, float *r_buffer) {
	// Transform, random value, cell (debug view) and 1 per instance, then holes (zeros).
	const float cell_index = float(_foliage_debug_cell_index(p_cell.key));
	float *w = r_buffer;
	for (const Instance &instance : p_cell.instances) {
		memcpy(w, instance.xform, sizeof(instance.xform));
		w[12] = instance.random;
		w[13] = cell_index;
		w[14] = 0.0f;
		w[15] = 1.0f;
		w += INSTANCE_FLOATS;
	}
	memset(w, 0, int64_t(p_slot.capacity - p_cell.instances.size()) * INSTANCE_FLOATS * sizeof(float));
}

void LandscapeFoliage3D::_update_gpu_slots(Entry *p_entry, const Vector3 &p_camera, bool p_has_camera) {
#ifdef RD_ENABLED
	// The loaded cells within the cull distance of the camera (all without camera or cull
	// distance) have a slot. Slots are released beyond a margin, so that small camera moves
	// don't upload anything.
	const bool all = !p_has_camera || p_entry->cull_end <= 0.0f;
	const real_t range = p_entry->cull_end + p_entry->transition * 0.5f + chunk_size * 0.5f;
	const real_t keep = range + chunk_size;
	auto distance_to = [&](Cell &r_cell) -> real_t {
		_update_cell_bounds(p_entry, r_cell);
		return p_camera.distance_to(p_camera.clamp(r_cell.bounds.position, r_cell.bounds.position + r_cell.bounds.size));
	};

	bool changed = false;
	LocalVector<Vector2i> released;
	for (const KeyValue<Vector2i, Entry::GPUSlot> &kv : p_entry->gpu_slots) {
		Cell *cell = p_entry->cells.getptr(kv.key);
		if (!cell || !cell->loaded || cell->instances.is_empty() || p_entry->type.is_null() || (!all && distance_to(*cell) > keep)) {
			released.push_back(kv.key);
		}
	}
	for (const Vector2i &key : released) {
		_release_gpu_slot(p_entry, key);
		changed = true;
	}

	LocalVector<Cell *> uploads;
	if (p_entry->type.is_valid()) {
		for (KeyValue<Vector2i, Cell> &kv : p_entry->cells) {
			Cell &cell = kv.value;
			if (!cell.loaded || cell.instances.is_empty()) {
				continue;
			}
			const bool has_slot = p_entry->gpu_slots.has(kv.key);
			if (has_slot ? !cell.gpu_dirty : (!all && distance_to(cell) > range)) {
				continue;
			}
			uploads.push_back(&cell);
		}
	}
	changed = changed || !uploads.is_empty();

	// Modified cells are written in place when they fit, with room to grow (painting).
	bool repack = false;
	for (Cell *cell : uploads) {
		const uint32_t count = cell->instances.size();
		Entry::GPUSlot *slot = p_entry->gpu_slots.getptr(cell->key);
		if (slot && count > slot->capacity) {
			_release_gpu_slot(p_entry, cell->key);
			slot = nullptr;
		}
		if (!slot) {
			Entry::GPUSlot new_slot;
			new_slot.capacity = count + count / 4 + 16;
			if (!_allocate_gpu_slot(p_entry, new_slot.capacity, new_slot.offset)) {
				repack = true;
				break;
			}
			slot = &p_entry->gpu_slots.insert(cell->key, new_slot)->value;
		}
		Vector<float> buffer;
		buffer.resize(int64_t(slot->capacity) * INSTANCE_FLOATS);
		_write_gpu_slot(*cell, *slot, buffer.ptrw());
		_gpu_call(callable_mp(gpu, &LandscapeFoliageGPU::update_instances).bind(p_entry->gpu_id, slot->offset, buffer));
		cell->gpu_dirty = false;
	}

	uint64_t slotted = 0;
	for (const KeyValue<Vector2i, Entry::GPUSlot> &kv : p_entry->gpu_slots) {
		slotted += kv.value.capacity;
	}
	// Also packed again when fragmented or much larger than needed.
	repack = repack || p_entry->gpu_used > slotted * 2 + 65536 || p_entry->gpu_capacity > slotted * 4 + 65536;
	if (repack) {
		LocalVector<Cell *> cells;
		for (const KeyValue<Vector2i, Entry::GPUSlot> &kv : p_entry->gpu_slots) {
			cells.push_back(p_entry->cells.getptr(kv.key));
		}
		for (Cell *cell : uploads) {
			if (!p_entry->gpu_slots.has(cell->key)) {
				cells.push_back(cell);
			}
		}
		p_entry->gpu_slots.clear();
		p_entry->gpu_free.clear();
		uint64_t total = 0;
		for (Cell *cell : cells) {
			total += cell->instances.size() + cell->instances.size() / 4 + 16;
		}
		ERR_FAIL_COND_MSG(total > uint64_t(UINT32_MAX / (INSTANCE_FLOATS * sizeof(float))), "Too many foliage instances for GPU indirect rendering.");
		const uint32_t capacity = uint32_t(MIN(MAX(total + total / 2, uint64_t(4096)), uint64_t(UINT32_MAX / (INSTANCE_FLOATS * sizeof(float)))));
		Vector<float> buffer;
		buffer.resize(int64_t(total) * INSTANCE_FLOATS);
		uint32_t offset = 0;
		for (Cell *cell : cells) {
			Entry::GPUSlot slot;
			slot.offset = offset;
			slot.capacity = cell->instances.size() + cell->instances.size() / 4 + 16;
			_write_gpu_slot(*cell, slot, buffer.ptrw() + int64_t(offset) * INSTANCE_FLOATS);
			p_entry->gpu_slots.insert(cell->key, slot);
			cell->gpu_dirty = false;
			offset += slot.capacity;
		}
		p_entry->gpu_capacity = capacity;
		p_entry->gpu_used = offset;
		_gpu_call(callable_mp(gpu, &LandscapeFoliageGPU::set_capacity).bind(p_entry->gpu_id, capacity));
		_gpu_call(callable_mp(gpu, &LandscapeFoliageGPU::update_instances).bind(p_entry->gpu_id, 0, buffer));
	}

	// Also when only the bounds changed (other meshes).
	uint32_t count = 0;
	AABB bounds;
	bool first = true;
	for (const KeyValue<Vector2i, Entry::GPUSlot> &kv : p_entry->gpu_slots) {
		Cell *cell = p_entry->cells.getptr(kv.key);
		_update_cell_bounds(p_entry, *cell);
		count += cell->instances.size();
		bounds = first ? cell->bounds : bounds.merge(cell->bounds);
		first = false;
	}
	changed = changed || count != p_entry->gpu_count || bounds != p_entry->gpu_bounds;
	if (!changed) {
		return;
	}
	p_entry->gpu_count = count;
	p_entry->gpu_bounds = bounds;
	p_entry->gpu_outputs_dirty = true;
	p_entry->gpu_run = true;
	debug_bounds_dirty = true;
#endif
}

void LandscapeFoliage3D::_update_gpu_outputs(Entry *p_entry) {
#ifdef RD_ENABLED
	// The capacity of each output follows the instances it received in the last runs: it starts
	// large enough for every instance (up to gpu_max_instances), shrinks when mostly unused and
	// grows before it overflows.
	const LandscapeFoliageGPU::Stats stats = gpu->get_stats(p_entry->gpu_id);
	const bool new_stats = stats.serial > p_entry->gpu_outputs_serial;
	if (!p_entry->gpu_outputs_dirty && !new_stats) {
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	const uint32_t limit = CLAMP(MIN(p_entry->gpu_count, uint32_t(gpu_max_instances)), 1u, uint32_t(FOLIAGE_GPU_MAX_OUTPUT));
	constexpr uint32_t min_capacity = 4096;
	uint32_t dropped = 0;
	bool changed = false;
	for (int list = 0; list < 2; list++) {
		for (int lod = 0; lod < LandscapeFoliageType::MAX_LODS; lod++) {
			Entry::GPUOutput &output = p_entry->gpu_outputs[list][lod];
			const bool wanted = p_entry->gpu_count > 0 && lod < p_entry->lod_count && p_entry->lod_meshes[lod].is_valid() && (list == 0 || (cast_shadows && p_entry->lod_shadows[lod]));
			if (!wanted) {
				if (output.multimesh.is_valid()) {
					rs->free_rid(output.instance);
					rs->free_rid(output.multimesh);
					output = Entry::GPUOutput();
					changed = true;
				}
				continue;
			}
			const uint32_t emitted = new_stats ? stats.counts[list * LandscapeFoliageType::MAX_LODS + lod] : 0;
			uint32_t capacity = output.capacity;
			if (new_stats && output.multimesh.is_valid()) {
				dropped += emitted > output.capacity ? emitted - output.capacity : 0;
				if (uint64_t(emitted) * 10 >= uint64_t(capacity) * 9) {
					capacity = MAX(capacity * 2, emitted * 2);
				} else if (emitted * 8 < capacity && capacity > min_capacity) {
					capacity = MAX(emitted * 2, min_capacity);
				}
			}
			if (capacity == 0) {
				capacity = limit;
			}
			capacity = CLAMP(capacity, 1u, limit);
			if (output.multimesh.is_null()) {
				output.multimesh = rs->multimesh_create();
				rs->multimesh_allocate_data(output.multimesh, capacity, RSE::MULTIMESH_TRANSFORM_3D, false, true, true);
				rs->multimesh_set_mesh(output.multimesh, p_entry->lod_meshes[lod]);
				output.surfaces = MIN(rs->mesh_get_surface_count(p_entry->lod_meshes[lod]), 255);
				output.capacity = capacity;
				output.instance = rs->instance_create2(output.multimesh, get_world_3d()->get_scenario());
				rs->instance_attach_object_instance_id(output.instance, get_instance_id());
				_update_gpu_output_settings(p_entry, list, lod);
				rs->multimesh_set_custom_aabb(output.multimesh, p_entry->gpu_bounds);
				changed = true;
			} else {
				if (capacity != output.capacity) {
					rs->multimesh_allocate_data(output.multimesh, capacity, RSE::MULTIMESH_TRANSFORM_3D, false, true, true);
					output.capacity = capacity;
					changed = true;
				}
				if (p_entry->gpu_outputs_dirty) {
					rs->multimesh_set_custom_aabb(output.multimesh, p_entry->gpu_bounds);
				}
			}
		}
	}
	if (new_stats) {
		p_entry->gpu_dropped = dropped;
	}
	// Statistics of the runs before a change describe the previous capacities.
	p_entry->gpu_outputs_serial = changed ? p_entry->gpu_serial : MAX(p_entry->gpu_outputs_serial, stats.serial);
	p_entry->gpu_run = p_entry->gpu_run || changed || p_entry->gpu_outputs_dirty;
	p_entry->gpu_outputs_dirty = false;
#endif
}

void LandscapeFoliage3D::_update_gpu(const Vector3 &p_camera, bool p_has_camera) {
	const real_t step = chunk_size * 0.25f;
	for (Entry *entry : entries) {
		const bool moved = p_has_camera != has_last_camera || (p_has_camera && entry->gpu_slots_camera.distance_squared_to(p_camera) > step * step);
		if (entry->gpu_dirty || moved) {
			entry->gpu_dirty = false;
			entry->gpu_slots_camera = p_camera;
			_update_gpu_slots(entry, p_camera, p_has_camera);
		}
		_update_gpu_outputs(entry);
	}
	if (debug_bounds_dirty && debug_view != DEBUG_VIEW_DISABLED) {
		_update_debug_bounds();
	}
	render_pending = false;
}

void LandscapeFoliage3D::_frame_pre_draw() {
#ifdef RD_ENABLED
	if (!_is_gpu_active() || !is_visible_in_tree()) {
		return;
	}
	// The view of this frame (after the scripts moved the camera), frozen with the LOD of the
	// landscape to inspect the culling.
	bool valid = false;
	bool frustum = false;
	Vector3 camera;
	Plane planes[6];
	if (landscape && landscape->is_lod_frozen() && gpu_view_valid) {
		valid = true;
		frustum = gpu_view_frustum;
		camera = gpu_view_camera;
		for (int i = 0; i < 6; i++) {
			planes[i] = gpu_view_planes[i];
		}
	} else {
		const Camera3D *view = _get_view_camera();
		if (view) {
			valid = true;
			const Transform3D to_landscape = _get_space_transform().affine_inverse();
			camera = to_landscape.xform(view->get_camera_transform().origin);
			const Vector<Plane> view_planes = view->get_frustum();
			frustum = gpu_frustum_culling && view_planes.size() == 6;
			for (int i = 0; i < 6 && frustum; i++) {
				planes[i] = to_landscape.xform(view_planes[i]);
			}
		}
	}
	bool view_changed = valid != gpu_view_valid || frustum != gpu_view_frustum || camera != gpu_view_camera;
	for (int i = 0; i < 6 && frustum && !view_changed; i++) {
		view_changed = planes[i] != gpu_view_planes[i];
	}
	gpu_view_valid = valid;
	gpu_view_frustum = frustum;
	gpu_view_camera = camera;
	for (int i = 0; i < 6; i++) {
		gpu_view_planes[i] = planes[i];
	}

	for (Entry *entry : entries) {
		if (!view_changed && !entry->gpu_run) {
			continue;
		}
		entry->gpu_run = false;
		Array outputs;
		outputs.resize(LandscapeFoliageGPU::OUTPUT_COUNT);
		Vector<int32_t> layout;
		layout.resize(LandscapeFoliageGPU::OUTPUT_COUNT);
		bool any = false;
		for (int list = 0; list < 2; list++) {
			for (int lod = 0; lod < LandscapeFoliageType::MAX_LODS; lod++) {
				const Entry::GPUOutput &output = entry->gpu_outputs[list][lod];
				const int index = list * LandscapeFoliageType::MAX_LODS + lod;
				layout.write[index] = 0;
				if (output.multimesh.is_valid()) {
					outputs[index] = output.multimesh;
					layout.write[index] = int32_t(output.capacity | (uint32_t(output.surfaces) << 24));
					any = true;
				}
			}
		}
		if (!any) {
			continue;
		}
		LandscapeFoliageCullParams params;
		for (int i = 0; i < 6; i++) {
			params.planes[i][0] = planes[i].normal.x;
			params.planes[i][1] = planes[i].normal.y;
			params.planes[i][2] = planes[i].normal.z;
			params.planes[i][3] = planes[i].d;
		}
		params.camera[0] = camera.x;
		params.camera[1] = camera.y;
		params.camera[2] = camera.z;
		for (int lod = 0; lod < entry->lod_count; lod++) {
			params.lod_starts[lod] = entry->lod_starts[lod];
		}
		if (valid) {
			params.cull[0] = entry->cull_end;
			params.cull[1] = entry->cull_begin;
			params.cull[2] = entry->transition * 0.5f;
			params.counts[1] = MAX(entry->lod_count, 1);
		} else {
			params.counts[1] = 1; // Without camera, everything is drawn at the first level.
		}
		const AABB &mesh = entry->mesh_bounds;
		const Vector3 center = mesh.get_center();
		params.sphere[0] = center.x;
		params.sphere[1] = center.y;
		params.sphere[2] = center.z;
		params.sphere[3] = mesh.size.length() * 0.5f;
		params.counts[0] = entry->gpu_used;
		params.counts[3] = frustum ? 1 : 0;
		Vector<uint8_t> bytes;
		bytes.resize(sizeof(params));
		memcpy(bytes.ptrw(), &params, sizeof(params));
		entry->gpu_serial++;
		_gpu_call(callable_mp(gpu, &LandscapeFoliageGPU::run).bind(entry->gpu_id, bytes, outputs, layout, entry->gpu_serial));
	}
#endif
}

void LandscapeFoliage3D::set_gpu_indirect(bool p_enable) {
	if (gpu_indirect == p_enable) {
		return;
	}
	const bool was_active = _is_gpu_active();
	gpu_indirect = p_enable;
#ifdef RD_ENABLED
	if (gpu_indirect && !gpu && is_gpu_indirect_supported()) {
		gpu = memnew(LandscapeFoliageGPU);
		if (is_inside_tree()) {
			RenderingServer::get_singleton()->connect(SNAME("frame_pre_draw"), callable_mp(this, &LandscapeFoliage3D::_frame_pre_draw));
		}
	}
#endif
	if (was_active != _is_gpu_active()) {
		// Switches between the CPU and the GPU paths.
		for (Entry *entry : entries) {
			for (KeyValue<Vector2i, Cell> &kv : entry->cells) {
				_free_cell_rendering(kv.value);
			}
			_free_gpu(entry);
		}
		_free_debug_bounds();
		render_pending = true;
	}
	update_configuration_warnings();
}

void LandscapeFoliage3D::set_gpu_frustum_culling(bool p_enable) {
	gpu_frustum_culling = p_enable;
}

void LandscapeFoliage3D::set_gpu_max_instances(int p_count) {
	gpu_max_instances = CLAMP(p_count, 1024, FOLIAGE_GPU_MAX_OUTPUT);
	for (Entry *entry : entries) {
		entry->gpu_outputs_dirty = true;
	}
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
		entry->gpu_dirty = true; // Other cull distance.
		entry->gpu_run = true;
	}
	render_pending = true;
	streaming_dirty = true;
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
	for (Entry *entry : entries) {
		entry->gpu_outputs_dirty = true; // Shadow lists.
	}
	_update_all_batch_settings();
}

void LandscapeFoliage3D::force_update() {
	for (Entry *entry : entries) {
		_update_entry_lods(entry);
		for (KeyValue<Vector2i, Cell> &kv : entry->cells) {
			_free_cell_rendering(kv.value);
			kv.value.bounds_dirty = true;
		}
		_free_gpu(entry);
	}
	render_pending = true;
	streaming_dirty = true;
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
	// GPU indirect rendering: instances in the GPU buffers, drawn (read back from the last run).
	const bool gpu_active = _is_gpu_active();
	int64_t gpu_instances = 0;
	int64_t gpu_cells = 0;
	int64_t gpu_shadow_drawn = 0;
	int64_t gpu_dropped = 0;
	int64_t gpu_memory = 0;
#ifdef RD_ENABLED
	if (gpu_active) {
		drawn = 0;
		for (const Entry *entry : entries) {
			gpu_instances += entry->gpu_count;
			gpu_cells += entry->gpu_slots.size();
			gpu_dropped += entry->gpu_dropped;
			gpu_memory += int64_t(entry->gpu_capacity) * (INSTANCE_FLOATS * sizeof(float) + sizeof(uint32_t));
			const LandscapeFoliageGPU::Stats gpu_stats = gpu->get_stats(entry->gpu_id);
			for (int list = 0; list < 2; list++) {
				for (int lod = 0; lod < LandscapeFoliageType::MAX_LODS; lod++) {
					const Entry::GPUOutput &output = entry->gpu_outputs[list][lod];
					if (output.multimesh.is_null()) {
						continue;
					}
					batches++;
					gpu_memory += int64_t(output.capacity) * INSTANCE_FLOATS * sizeof(float);
					const int64_t count = MIN(gpu_stats.counts[list * LandscapeFoliageType::MAX_LODS + lod], output.capacity);
					if (list == 0) {
						drawn += count;
					} else {
						gpu_shadow_drawn += count;
					}
				}
			}
		}
	}
#endif
	Dictionary stats;
	stats["types"] = int(entries.size());
	stats["instances"] = get_instance_count();
	stats["cells"] = cells;
	stats["cells_rendered"] = gpu_active ? gpu_cells : cells_rendered;
	stats["cells_mixed"] = cells_mixed;
	stats["batches"] = batches;
	stats["instances_drawn"] = drawn;
	stats["update_usec"] = int64_t(last_update_usec);
	stats["updated_cells"] = last_updated_cells;
	stats["debug_bounds"] = debug_bounds_count;
	stats["gpu_indirect"] = gpu_active;
	stats["gpu_instances"] = gpu_instances;
	stats["gpu_shadow_instances_drawn"] = gpu_shadow_drawn;
	stats["gpu_dropped"] = gpu_dropped;
	stats["gpu_memory"] = gpu_memory;
	stats["streamed"] = _is_streamed();
	stats["cells_loaded"] = data.is_valid() ? int64_t(loaded_cells) : cells;
	stats["cells_loading"] = int64_t(requests.size());
	stats["loaded_memory"] = loaded_bytes;
	stats["data_memory"] = data.is_valid() ? data->get_memory_usage() : int64_t(0);
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
	if (gpu_indirect && !is_gpu_indirect_supported()) {
		warnings.push_back(RTR("GPU indirect rendering requires the Forward+ or Mobile renderer: the foliage is culled on the CPU."));
	}
	if ((data.is_null() || !data->get_path().is_resource_file()) && get_instance_count() > 100000) {
		warnings.push_back(RTR("Many instances are saved in the scene: save them to a .lfdata file (Foliage tab of the Landscape dock) to stream them."));
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
			if (gpu) {
				RenderingServer::get_singleton()->connect(SNAME("frame_pre_draw"), callable_mp(this, &LandscapeFoliage3D::_frame_pre_draw));
			}
			render_pending = true;
			streaming_dirty = true;
			gpu_view_valid = false;
			set_process_internal(true);
		} break;

		case NOTIFICATION_EXIT_TREE: {
			_free_all_rendering();
			if (gpu && RenderingServer::get_singleton()->is_connected(SNAME("frame_pre_draw"), callable_mp(this, &LandscapeFoliage3D::_frame_pre_draw))) {
				RenderingServer::get_singleton()->disconnect(SNAME("frame_pre_draw"), callable_mp(this, &LandscapeFoliage3D::_frame_pre_draw));
			}
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
				for (int list = 0; list < 2; list++) {
					for (int lod = 0; lod < LandscapeFoliageType::MAX_LODS; lod++) {
						if (entry->gpu_outputs[list][lod].instance.is_valid()) {
							rs->instance_set_transform(entry->gpu_outputs[list][lod].instance, space);
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
			_process_streaming();
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
	ClassDB::bind_method(D_METHOD("set_gpu_indirect", "enable"), &LandscapeFoliage3D::set_gpu_indirect);
	ClassDB::bind_method(D_METHOD("is_gpu_indirect"), &LandscapeFoliage3D::is_gpu_indirect);
	ClassDB::bind_method(D_METHOD("set_gpu_frustum_culling", "enable"), &LandscapeFoliage3D::set_gpu_frustum_culling);
	ClassDB::bind_method(D_METHOD("is_gpu_frustum_culling"), &LandscapeFoliage3D::is_gpu_frustum_culling);
	ClassDB::bind_method(D_METHOD("set_gpu_max_instances", "count"), &LandscapeFoliage3D::set_gpu_max_instances);
	ClassDB::bind_method(D_METHOD("get_gpu_max_instances"), &LandscapeFoliage3D::get_gpu_max_instances);
	ClassDB::bind_method(D_METHOD("is_gpu_indirect_active"), &LandscapeFoliage3D::is_gpu_indirect_active);
	ClassDB::bind_static_method("LandscapeFoliage3D", D_METHOD("is_gpu_indirect_supported"), &LandscapeFoliage3D::is_gpu_indirect_supported);

	ClassDB::bind_method(D_METHOD("set_data", "data"), &LandscapeFoliage3D::set_data);
	ClassDB::bind_method(D_METHOD("get_data"), &LandscapeFoliage3D::get_data);
	ClassDB::bind_method(D_METHOD("save_to_data_file", "path"), &LandscapeFoliage3D::save_to_data_file);
	ClassDB::bind_method(D_METHOD("load_all_cells"), &LandscapeFoliage3D::load_all_cells);

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

	ADD_GROUP("GPU", "gpu_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "gpu_indirect"), "set_gpu_indirect", "is_gpu_indirect");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "gpu_frustum_culling"), "set_gpu_frustum_culling", "is_gpu_frustum_culling");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "gpu_max_instances", PROPERTY_HINT_RANGE, "1024,16777215,1"), "set_gpu_max_instances", "get_gpu_max_instances");

	// After the types (the layers of the data are aligned with them) and the chunk size.
	ADD_GROUP("Streaming", "");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "data", PROPERTY_HINT_RESOURCE_TYPE, LandscapeFoliageData::get_class_static()), "set_data", "get_data");

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
	if (data.is_valid()) {
		// The data may outlive the node.
		_flush_data();
		_cancel_requests();
		data->remove_flush_callback(callable_mp(this, &LandscapeFoliage3D::_flush_data));
		_add_loaded_bytes(-loaded_bytes);
	}
	for (Entry *entry : entries) {
		_connect_type(entry, false);
		for (KeyValue<Vector2i, Cell> &kv : entry->cells) {
			_free_cell_rendering(kv.value);
		}
		_free_gpu_outputs(entry);
		memdelete(entry);
	}
	entries.clear();
	_free_debug_bounds();
#ifdef RD_ENABLED
	if (gpu) {
		// After the pending calls of the node, on the rendering thread.
		RenderingServer::get_singleton()->call_on_render_thread(callable_mp_static(&LandscapeFoliageGPU::destroy).bind(gpu));
		gpu = nullptr;
	}
#endif
}
