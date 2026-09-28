/**************************************************************************/
/*  landscape_spline_system.cpp                                           */
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

#include "landscape_spline_system.h"

#include "landscape_spline_3d.h"

#include "core/config/engine.h"
#include "core/os/os.h"

LandscapeSplineSystem::~LandscapeSplineSystem() {
	if (data.is_valid() && data->get_compositor() == this) {
		data->set_compositor(nullptr);
	}
}

void LandscapeSplineSystem::set_data(const Ref<LandscapeData> &p_data) {
	if (data == p_data) {
		return;
	}
	if (data.is_valid() && data->get_compositor() == this) {
		data->set_compositor(nullptr);
	}
	data = p_data;
	// In the editor, sculpting and painting under splines edit the base layer and the splines stay
	// on top. At runtime, edits (e.g. craters) are written through, on roads too.
	if (data.is_valid() && Engine::get_singleton()->is_editor_hint()) {
		data->set_compositor(this);
	}
	invalidate_all();
}

void LandscapeSplineSystem::register_spline(LandscapeSpline3D *p_spline) {
	// Duplicated nodes (copy and paste, scenes instanced several times) share the id of their source.
	for (LandscapeSpline3D *other : splines) {
		if (other != p_spline && other->get_spline_id() == p_spline->get_spline_id()) {
			p_spline->regenerate_spline_id();
			break;
		}
	}
	if (!splines.has(p_spline)) {
		splines.push_back(p_spline);
	}
	spline_changed(p_spline, false);
}

void LandscapeSplineSystem::unregister_spline(LandscapeSpline3D *p_spline) {
	splines.erase(p_spline);
	forced.erase(p_spline);
	spline_changed(nullptr, false);
}

void LandscapeSplineSystem::spline_changed(LandscapeSpline3D *p_spline, bool p_force) {
	if (p_spline && p_force) {
		forced.insert(p_spline);
	}
	const uint64_t now = OS::get_singleton()->get_ticks_usec();
	if (!dirty) {
		first_change_usec = now;
	}
	dirty = true;
	last_change_usec = now;
}

void LandscapeSplineSystem::invalidate_all() {
	for (LandscapeSpline3D *spline : splines) {
		spline->_terrain_invalidated();
	}
	spline_changed(nullptr, false);
}

void LandscapeSplineSystem::force_rebuild() {
	force_all = true;
	invalidate_all();
}

PackedInt64Array LandscapeSplineSystem::_encode_record(const LandscapeSplineTerrainShape &p_shape) {
	// [flags, then hash, x, y, width, height of every block].
	PackedInt64Array record;
	record.resize(1 + p_shape.blocks.size() * 5);
	int64_t *w = record.ptrw();
	w[0] = p_shape.merge_dirty_blocks ? 1 : 0;
	for (uint32_t b = 0; b < p_shape.blocks.size(); b++) {
		const LandscapeSplineTerrainShape::Block &block = p_shape.blocks[b];
		int64_t *e = w + 1 + b * 5;
		e[0] = int64_t(block.hash);
		e[1] = block.rect.position.x;
		e[2] = block.rect.position.y;
		e[3] = block.rect.size.x;
		e[4] = block.rect.size.y;
	}
	return record;
}

static Rect2i _spline_record_rect(const PackedInt64Array &p_record, int p_block) {
	const int base = 1 + p_block * 5;
	return Rect2i(int(p_record[base + 1]), int(p_record[base + 2]), int(p_record[base + 3]), int(p_record[base + 4]));
}

void LandscapeSplineSystem::_add_record_rects(const PackedInt64Array &p_record, LocalVector<Rect2i> &r_rects) {
	if (p_record.size() < 1) {
		return;
	}
	const int blocks = (p_record.size() - 1) / 5;
	if (p_record[0] & 1) {
		// Lakes: everything between the blocks may be affected (inside of the shore).
		Rect2i merged;
		for (int b = 0; b < blocks; b++) {
			const Rect2i rect = _spline_record_rect(p_record, b);
			if (rect.has_area()) {
				merged = merged.has_area() ? merged.merge(rect) : rect;
			}
		}
		if (merged.has_area()) {
			r_rects.push_back(merged);
		}
		return;
	}
	for (int b = 0; b < blocks; b++) {
		const Rect2i rect = _spline_record_rect(p_record, b);
		if (rect.has_area()) {
			r_rects.push_back(rect);
		}
	}
}

void LandscapeSplineSystem::_get_sorted_shapes(LocalVector<SortedSpline> &r_shapes) const {
	r_shapes.clear();
	for (LandscapeSpline3D *spline : splines) {
		const LandscapeSplineTerrainShape &shape = spline->get_terrain_shape();
		if (shape.is_empty()) {
			continue;
		}
		SortedSpline entry;
		entry.priority = spline->get_terrain_priority();
		entry.id = spline->get_spline_id();
		entry.shape = &shape;
		r_shapes.push_back(entry);
	}
	// Splines with a higher priority are applied last (on top).
	r_shapes.sort();
}

void LandscapeSplineSystem::_composite_rect(const Rect2i &p_rect, int p_flags, const LocalVector<SortedSpline> &p_shapes) {
	LandscapeStorage &storage = data->get_storage();
	const int64_t count = int64_t(p_rect.size.x) * p_rect.size.y;
	const int weightmaps = data->get_weightmap_count();
	const int layers = weightmaps * 4;
	const bool heights_changed = p_flags & LandscapeData::CHANGED_HEIGHTS;
	const bool weights_changed = p_flags & LandscapeData::CHANGED_WEIGHTS;

	// Start from the base layer, then apply the splines by priority.
	LocalVector<float> heights;
	heights.resize(count);
	storage.read_region(LandscapeStorage::LAYER_BASE_HEIGHTS, 0, p_rect, heights.ptr());
	LocalVector<float> weights;
	LocalVector<uint8_t> bytes;
	if (weights_changed) {
		weights.resize(count * layers);
		bytes.resize(count * 4);
		for (int m = 0; m < weightmaps; m++) {
			storage.read_region(LandscapeStorage::LAYER_BASE_WEIGHTS_0 + m, 0, p_rect, bytes.ptr());
			for (int64_t i = 0; i < count; i++) {
				for (int c = 0; c < 4; c++) {
					weights[i * layers + m * 4 + c] = bytes[i * 4 + c] / 255.0f;
				}
			}
		}
	}
	const real_t spacing = data->get_vertex_spacing();
	for (const SortedSpline &entry : p_shapes) {
		LandscapeSplineTerrain::apply(*entry.shape, spacing, p_rect, heights.ptr(), weights_changed ? weights.ptr() : nullptr, layers);
	}

	if (heights_changed) {
		storage.write_region(LandscapeStorage::LAYER_HEIGHTS, p_rect, heights.ptr());
	}
	if (weights_changed) {
		LocalVector<uint8_t> quantized;
		quantized.resize(count * layers);
		for (int64_t i = 0; i < count; i++) {
			LandscapeData::quantize_weights(weights.ptr() + i * layers, layers, quantized.ptr() + i * layers);
		}
		for (int m = 0; m < weightmaps; m++) {
			for (int64_t i = 0; i < count; i++) {
				for (int c = 0; c < 4; c++) {
					bytes[i * 4 + c] = quantized[i * layers + m * 4 + c];
				}
			}
			storage.write_region(LandscapeStorage::LAYER_WEIGHTS_0 + m, p_rect, bytes.ptr());
		}
	}
}

void LandscapeSplineSystem::composite_region(LandscapeData *p_data, const Rect2i &p_rect, int p_flags) {
	ERR_FAIL_COND(p_data != data.ptr());
	const Rect2i rect = data->clip_rect(p_rect);
	if (!rect.has_area() || !data->has_base_in_rect(rect)) {
		return;
	}
	LocalVector<SortedSpline> shapes;
	_get_sorted_shapes(shapes);
	// Elsewhere than in tiles with a base, the final data already is the edit layer.
	const int tile_size = LandscapeStorage::TILE_SIZE;
	for (int tz = rect.position.y / tile_size; tz <= (rect.get_end().y - 1) / tile_size; tz++) {
		for (int tx = rect.position.x / tile_size; tx <= (rect.get_end().x - 1) / tile_size; tx++) {
			if (!data->tile_has_base(Vector2i(tx, tz))) {
				continue;
			}
			const Rect2i tile_rect = rect.intersection(Rect2i(tx * tile_size, tz * tile_size, tile_size, tile_size));
			if (tile_rect.has_area()) {
				_composite_rect(tile_rect, p_flags, shapes);
			}
		}
	}
}

bool LandscapeSplineSystem::process(bool p_immediate) {
	if (!dirty || data.is_null() || !data->is_valid()) {
		return false;
	}
	const uint64_t now = OS::get_singleton()->get_ticks_usec();
	if (!p_immediate && now - last_change_usec < EDIT_IDLE_USEC && now - first_change_usec < EDIT_MAX_DELAY_USEC) {
		return false; // Wait until the spline stops moving (or at most a few frames) before sculpting.
	}
	dirty = false;
	composited_tiles = 0;

	// What changed since the data was last composited: compare the blocks of every spline
	// with the records saved in the data.
	const Dictionary records = data->get_spline_records();
	Dictionary new_records;
	LocalVector<Rect2i> dirty_rects;
	HashSet<int64_t> seen;
	for (LandscapeSpline3D *spline : splines) {
		const LandscapeSplineTerrainShape &shape = spline->get_terrain_shape();
		const int64_t id = spline->get_spline_id();
		seen.insert(id);
		const PackedInt64Array record = shape.is_empty() ? PackedInt64Array() : _encode_record(shape);
		if (!record.is_empty()) {
			new_records[id] = record;
		}
		const PackedInt64Array old = records.get(id, PackedInt64Array());
		if (force_all || forced.has(spline)) {
			_add_record_rects(old, dirty_rects);
			_add_record_rects(record, dirty_rects);
			continue;
		}
		if (old == record) {
			continue;
		}
		LocalVector<Rect2i> changed;
		const int old_blocks = old.size() > 0 ? (old.size() - 1) / 5 : 0;
		const int new_blocks = record.size() > 0 ? (record.size() - 1) / 5 : 0;
		for (int b = 0; b < MAX(old_blocks, new_blocks); b++) {
			bool same = b < old_blocks && b < new_blocks;
			for (int k = 0; k < 5 && same; k++) {
				same = old[1 + b * 5 + k] == record[1 + b * 5 + k];
			}
			if (same) {
				continue;
			}
			if (b < old_blocks) {
				changed.push_back(_spline_record_rect(old, b));
			}
			if (b < new_blocks) {
				changed.push_back(_spline_record_rect(record, b));
			}
		}
		const bool merge = (old.size() > 0 && (old[0] & 1)) || (record.size() > 0 && (record[0] & 1));
		Rect2i merged;
		for (const Rect2i &rect : changed) {
			if (!rect.has_area()) {
				continue;
			}
			if (merge) {
				merged = merged.has_area() ? merged.merge(rect) : rect;
			} else {
				dirty_rects.push_back(rect);
			}
		}
		if (merged.has_area()) {
			dirty_rects.push_back(merged);
		}
	}
	// Splines that were removed, disabled or whose terrain was never cleaned up.
	for (const KeyValue<Variant, Variant> &kv : records) {
		if (!seen.has(int64_t(kv.key))) {
			_add_record_rects(kv.value, dirty_rects);
		}
	}
	forced.clear();

	HashSet<Vector2i> tiles;
	const Vector2i tile_count = data->get_tile_count();
	const int tile_size = LandscapeStorage::TILE_SIZE;
	for (const Rect2i &dirty_rect : dirty_rects) {
		const Rect2i rect = data->clip_rect(dirty_rect);
		if (!rect.has_area()) {
			continue;
		}
		for (int tz = rect.position.y / tile_size; tz <= (rect.get_end().y - 1) / tile_size; tz++) {
			for (int tx = rect.position.x / tile_size; tx <= (rect.get_end().x - 1) / tile_size; tx++) {
				tiles.insert(Vector2i(tx, tz));
			}
		}
	}
	if (force_all) {
		// Also clean up bases without records (e.g. data edited while the splines were missing).
		for (int tz = 0; tz < tile_count.y; tz++) {
			for (int tx = 0; tx < tile_count.x; tx++) {
				if (data->tile_has_base(Vector2i(tx, tz))) {
					tiles.insert(Vector2i(tx, tz));
				}
			}
		}
		force_all = false;
	}
	data->set_spline_records(new_records);
	if (tiles.is_empty()) {
		return records != new_records;
	}

	LocalVector<SortedSpline> shapes;
	_get_sorted_shapes(shapes);
	LocalVector<Vector2i> sorted_tiles;
	for (const Vector2i &tile : tiles) {
		sorted_tiles.push_back(tile);
	}
	sorted_tiles.sort();
	for (const Vector2i &tile : sorted_tiles) {
		const Rect2i rect = data->clip_rect(Rect2i(tile * tile_size, Size2i(tile_size, tile_size)));
		if (!rect.has_area()) {
			continue;
		}
		bool covered = false;
		for (uint32_t s = 0; s < shapes.size() && !covered; s++) {
			for (const LandscapeSplineTerrainShape::Block &block : shapes[s].shape->blocks) {
				if (block.rect.intersects(rect)) {
					covered = true;
					break;
				}
			}
		}
		if (covered) {
			if (!data->tile_has_base(tile)) {
				data->capture_base(tile);
			}
			_composite_rect(rect, LandscapeData::CHANGED_HEIGHTS | LandscapeData::CHANGED_WEIGHTS, shapes);
		} else if (data->tile_has_base(tile)) {
			// No spline here anymore: the terrain is the base again.
			data->release_base(tile);
		} else {
			continue;
		}
		composited_tiles++;
		data->notify_region_changed(rect, LandscapeData::CHANGED_HEIGHTS | LandscapeData::CHANGED_WEIGHTS);
	}
	return true;
}
