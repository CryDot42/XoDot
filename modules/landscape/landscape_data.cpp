/**************************************************************************/
/*  landscape_data.cpp                                                    */
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

#include "landscape_data.h"

#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "core/object/class_db.h"
#include "core/os/os.h"

#include <png.h>

// Rows processed at once by whole-landscape operations.
static constexpr int BAND_ROWS = 128;

/* Creation */

void LandscapeData::create(const Vector2i &p_size, real_t p_vertex_spacing, float p_height) {
	ERR_FAIL_COND_MSG(p_size.x < MIN_RESOLUTION || p_size.y < MIN_RESOLUTION, vformat("Landscape resolution must be at least %d.", MIN_RESOLUTION));
	ERR_FAIL_COND_MSG(p_size.x > MAX_RESOLUTION || p_size.y > MAX_RESOLUTION, vformat("Landscape resolution can't exceed %d.", MAX_RESOLUTION));
	ERR_FAIL_COND(p_vertex_spacing <= 0.0);

	vertex_spacing = p_vertex_spacing;
	// All tiles start with constant defaults (flat, first layer), without using memory.
	storage.init(p_size, 1, p_height);
	_clear_lod_trees();
	saved_lod_trees.clear();
	spline_records.clear();
	pending_composite = Rect2i();
	notify_region_changed(Rect2i(Point2i(), p_size), CHANGED_ALL);
	emit_changed();
}

void LandscapeData::resize(const Vector2i &p_size) {
	ERR_FAIL_COND(!is_valid());
	ERR_FAIL_COND_MSG(p_size.x < MIN_RESOLUTION || p_size.y < MIN_RESOLUTION, vformat("Landscape resolution must be at least %d.", MIN_RESOLUTION));
	ERR_FAIL_COND_MSG(p_size.x > MAX_RESOLUTION || p_size.y > MAX_RESOLUTION, vformat("Landscape resolution can't exceed %d.", MAX_RESOLUTION));
	if (p_size == get_size()) {
		return;
	}

	// Keep the same world size: the vertex spacing follows the new resolution.
	const Vector2 world_size = get_world_size();
	const int weightmaps = get_weightmap_count();
	const bool holes = has_holes();
	const Vector2i old_size = get_size();

	// The edit layer is resampled: the splines are applied again on the new resolution.
	Ref<Image> height_image = Image::create_from_data(old_size.x, old_size.y, false, Image::FORMAT_RF, _read_layer_rows(LandscapeStorage::LAYER_BASE_HEIGHTS, 4));
	height_image->resize(p_size.x, p_size.y, Image::INTERPOLATE_BILINEAR);
	Vector<Ref<Image>> weight_images;
	for (int i = 0; i < weightmaps; i++) {
		Ref<Image> img = Image::create_from_data(old_size.x, old_size.y, false, Image::FORMAT_RGBA8, _read_layer_rows(LandscapeStorage::LAYER_BASE_WEIGHTS_0 + i, 4));
		img->resize(p_size.x, p_size.y, Image::INTERPOLATE_BILINEAR);
		weight_images.push_back(img);
	}
	Ref<Image> holes_image;
	if (holes) {
		Vector<uint8_t> hole_data;
		hole_data.resize(int64_t(old_size.x) * old_size.y);
		storage.read_region(LandscapeStorage::LAYER_HOLES, 0, Rect2i(Point2i(), old_size), hole_data.ptrw());
		holes_image = Image::create_from_data(old_size.x, old_size.y, false, Image::FORMAT_R8, hole_data);
		holes_image->resize(p_size.x, p_size.y, Image::INTERPOLATE_NEAREST);
	}

	storage.init(p_size, weightmaps, 0.0);
	vertex_spacing = MAX(world_size.x / real_t(p_size.x - 1), CMP_EPSILON);
	_clear_lod_trees();
	saved_lod_trees.clear();
	spline_records.clear();
	pending_composite = Rect2i();

	_write_image_rows(LandscapeStorage::LAYER_HEIGHTS, height_image->get_data(), 4);
	for (int i = 0; i < weight_images.size(); i++) {
		_write_image_rows(LandscapeStorage::LAYER_WEIGHTS_0 + i, weight_images[i]->get_data(), 4);
	}
	if (holes_image.is_valid()) {
		storage.set_has_holes(true);
		_write_image_rows(LandscapeStorage::LAYER_HOLES, holes_image->get_data(), 1);
	}

	notify_region_changed(Rect2i(Point2i(), p_size), CHANGED_ALL);
	emit_changed();
}

void LandscapeData::_write_image_rows(int p_layer, const Vector<uint8_t> &p_data, int p_texel_size, bool p_edit) {
	// Writes a full resolution image in bands to keep the memory usage bounded.
	const Vector2i size = get_size();
	ERR_FAIL_COND(p_data.size() != int64_t(size.x) * size.y * p_texel_size);
	// Heights and weights have a base layer (LAYER_BASE_HEIGHTS + index of the final layer).
	const bool edit_base = p_edit && has_base() && p_layer != LandscapeStorage::LAYER_HOLES;
	for (int z = 0; z < size.y; z += BAND_ROWS) {
		const Rect2i band(0, z, size.x, MIN(BAND_ROWS, size.y - z));
		const uint8_t *band_data = p_data.ptr() + int64_t(z) * size.x * p_texel_size;
		storage.write_region(p_layer, band, band_data);
		if (edit_base) {
			storage.write_region(LandscapeStorage::LAYER_BASE_HEIGHTS + p_layer, band, band_data);
		}
		storage.trim();
	}
	if (edit_base) {
		_request_composite(Rect2i(Point2i(), size), p_layer == LandscapeStorage::LAYER_HEIGHTS ? CHANGED_HEIGHTS : CHANGED_WEIGHTS);
	}
}

Vector<uint8_t> LandscapeData::_read_layer_rows(int p_layer, int p_texel_size) const {
	const Vector2i size = get_size();
	Vector<uint8_t> data;
	data.resize(int64_t(size.x) * size.y * p_texel_size);
	for (int z = 0; z < size.y; z += BAND_ROWS) {
		const Rect2i band(0, z, size.x, MIN(BAND_ROWS, size.y - z));
		storage.read_region(p_layer, 0, band, data.ptrw() + int64_t(z) * size.x * p_texel_size);
		storage.trim();
	}
	return data;
}

void LandscapeData::_set_size(const Vector2i &p_size) {
	// Used by deserialization only, the data arrays are assigned afterwards.
	const Vector2i size = p_size.clamp(Vector2i(), Vector2i(MAX_RESOLUTION, MAX_RESOLUTION));
	if (size.x < MIN_RESOLUTION || size.y < MIN_RESOLUTION) {
		return;
	}
	if (!is_valid() || size != get_size()) {
		storage.init(size, 1, 0.0);
		_clear_lod_trees();
		saved_lod_trees.clear();
		emit_changed();
	}
}

void LandscapeData::set_vertex_spacing(real_t p_spacing) {
	ERR_FAIL_COND(p_spacing <= 0.0);
	if (vertex_spacing == p_spacing) {
		return;
	}
	vertex_spacing = p_spacing;
	if (is_valid()) {
		notify_region_changed(Rect2i(Point2i(), get_size()), CHANGED_ALL);
	}
	emit_changed();
}

Vector2 LandscapeData::get_world_size() const {
	const Vector2i size = get_size();
	return Vector2(MAX(size.x - 1, 0), MAX(size.y - 1, 0)) * vertex_spacing;
}

Rect2i LandscapeData::clip_rect(const Rect2i &p_rect) const {
	return p_rect.intersection(Rect2i(Point2i(), get_size()));
}

/* Heights */

float LandscapeData::get_height(int p_x, int p_z) const {
	if (!is_valid()) {
		return 0.0;
	}
	return storage.get_height(p_x, p_z);
}

void LandscapeData::set_height(int p_x, int p_z, float p_height) {
	ERR_FAIL_COND(!is_valid());
	storage.set_height(p_x, p_z, p_height);
}

float LandscapeData::sample_height(real_t p_local_x, real_t p_local_z) const {
	if (!is_valid()) {
		return 0.0;
	}
	const Vector2i size = get_size();
	const real_t fx = CLAMP(p_local_x / vertex_spacing, 0.0, real_t(size.x - 1));
	const real_t fz = CLAMP(p_local_z / vertex_spacing, 0.0, real_t(size.y - 1));
	const int x0 = MIN(int(fx), size.x - 2);
	const int z0 = MIN(int(fz), size.y - 2);
	const real_t tx = fx - x0;
	const real_t tz = fz - z0;
	const float h00 = storage.get_height(x0, z0);
	const float h10 = storage.get_height(x0 + 1, z0);
	const float h01 = storage.get_height(x0, z0 + 1);
	const float h11 = storage.get_height(x0 + 1, z0 + 1);
	return Math::lerp(Math::lerp(h00, h10, float(tx)), Math::lerp(h01, h11, float(tx)), float(tz));
}

Vector3 LandscapeData::sample_normal(real_t p_local_x, real_t p_local_z) const {
	const real_t s = vertex_spacing;
	const float hl = sample_height(p_local_x - s, p_local_z);
	const float hr = sample_height(p_local_x + s, p_local_z);
	const float hd = sample_height(p_local_x, p_local_z - s);
	const float hu = sample_height(p_local_x, p_local_z + s);
	return Vector3(hl - hr, 2.0 * s, hd - hu).normalized();
}

Vector2 LandscapeData::get_height_range() const {
	if (!is_valid()) {
		return Vector2();
	}
	return storage.get_height_range();
}

void LandscapeData::read_heights(const Rect2i &p_rect, float *r_heights) const {
	ERR_FAIL_COND(!is_valid());
	storage.read_region(LandscapeStorage::LAYER_HEIGHTS, 0, p_rect, r_heights);
}

void LandscapeData::write_heights(const Rect2i &p_rect, const float *p_heights) {
	ERR_FAIL_COND(!is_valid());
	ERR_FAIL_COND_MSG(clip_rect(p_rect) != p_rect, "The rect must be inside the landscape.");
	storage.write_region(LandscapeStorage::LAYER_HEIGHTS, p_rect, p_heights);
}

/* Edit layer */

float LandscapeData::get_edit_height(int p_x, int p_z) const {
	if (!is_valid()) {
		return 0.0;
	}
	return storage.get_base_height(p_x, p_z);
}

float LandscapeData::sample_edit_height(real_t p_local_x, real_t p_local_z) const {
	if (!has_base()) {
		return sample_height(p_local_x, p_local_z);
	}
	const Vector2i size = get_size();
	const real_t fx = CLAMP(p_local_x / vertex_spacing, 0.0, real_t(size.x - 1));
	const real_t fz = CLAMP(p_local_z / vertex_spacing, 0.0, real_t(size.y - 1));
	const int x0 = MIN(int(fx), size.x - 2);
	const int z0 = MIN(int(fz), size.y - 2);
	const float tx = float(fx - x0);
	const float tz = float(fz - z0);
	const float h00 = storage.get_base_height(x0, z0);
	const float h10 = storage.get_base_height(x0 + 1, z0);
	const float h01 = storage.get_base_height(x0, z0 + 1);
	const float h11 = storage.get_base_height(x0 + 1, z0 + 1);
	return Math::lerp(Math::lerp(h00, h10, tx), Math::lerp(h01, h11, tx), tz);
}

void LandscapeData::read_edit_heights(const Rect2i &p_rect, float *r_heights) const {
	ERR_FAIL_COND(!is_valid());
	storage.read_region(LandscapeStorage::LAYER_BASE_HEIGHTS, 0, p_rect, r_heights);
}

void LandscapeData::write_edit_heights(const Rect2i &p_rect, const float *p_heights) {
	ERR_FAIL_COND(!is_valid());
	ERR_FAIL_COND_MSG(clip_rect(p_rect) != p_rect, "The rect must be inside the landscape.");
	// The final data is written too: it is the edit layer where there is no base, and it is
	// composited again from the base (and the splines) where there is one.
	storage.write_region(LandscapeStorage::LAYER_HEIGHTS, p_rect, p_heights);
	if (storage.has_base_in_rect(p_rect)) {
		storage.write_region(LandscapeStorage::LAYER_BASE_HEIGHTS, p_rect, p_heights);
		_request_composite(p_rect, CHANGED_HEIGHTS);
	}
}

/* Weights */

void LandscapeData::set_weightmap_count(int p_count) {
	ERR_FAIL_COND(!is_valid());
	p_count = CLAMP(p_count, 1, MAX_WEIGHTMAPS);
	if (p_count == get_weightmap_count()) {
		return;
	}
	storage.set_weightmap_count(p_count);
	emit_changed();
}

void LandscapeData::ensure_layer_capacity(int p_layer_count) {
	if (!is_valid()) {
		return;
	}
	const int needed = CLAMP((p_layer_count + 3) / 4, 1, MAX_WEIGHTMAPS);
	if (needed > get_weightmap_count()) {
		set_weightmap_count(needed);
	}
}

float LandscapeData::get_layer_weight(int p_x, int p_z, int p_layer) const {
	ERR_FAIL_INDEX_V(p_layer, MAX_LAYERS, 0.0);
	if (!is_valid() || (p_layer >> 2) >= get_weightmap_count()) {
		return 0.0;
	}
	uint8_t w[MAX_LAYERS];
	storage.get_weights(p_x, p_z, w);
	return w[p_layer] / 255.0;
}

void LandscapeData::set_layer_weight(int p_x, int p_z, int p_layer, float p_weight) {
	ERR_FAIL_INDEX(p_layer, MAX_LAYERS);
	ERR_FAIL_COND(!is_valid());
	const Vector2i size = get_size();
	ERR_FAIL_INDEX(p_x, size.x);
	ERR_FAIL_INDEX(p_z, size.y);
	ensure_layer_capacity(p_layer + 1);
	float w[MAX_LAYERS];
	get_weights(p_x, p_z, w);
	p_weight = CLAMP(p_weight, 0.0f, 1.0f);
	float others = 0.0;
	for (int i = 0; i < MAX_LAYERS; i++) {
		if (i != p_layer) {
			others += w[i];
		}
	}
	// Weight-blended layers: redistribute the remaining weight among the other layers.
	for (int i = 0; i < MAX_LAYERS; i++) {
		if (i == p_layer) {
			w[i] = p_weight;
		} else if (others > 0.0) {
			w[i] = w[i] / others * (1.0 - p_weight);
		}
	}
	if (others <= 0.0 && p_layer != 0) {
		w[0] = 1.0 - p_weight;
	}
	set_weights(p_x, p_z, w);
}

void LandscapeData::get_weights(int p_x, int p_z, float *r_weights) const {
	uint8_t w[MAX_LAYERS] = {};
	if (is_valid()) {
		storage.get_weights(p_x, p_z, w);
	}
	const int layers = get_weightmap_count() * 4;
	for (int i = 0; i < MAX_LAYERS; i++) {
		r_weights[i] = i < layers ? w[i] / 255.0f : 0.0f;
	}
}

void LandscapeData::quantize_weights(const float *p_weights, int p_layers, uint8_t *r_quantized) {
	float total = 0.0;
	for (int i = 0; i < p_layers; i++) {
		total += MAX(p_weights[i], 0.0f);
	}
	int quantized[LandscapeData::MAX_LAYERS] = {};
	int sum = 0;
	int largest = 0;
	if (total <= 0.0) {
		quantized[0] = 255;
		sum = 255;
	} else {
		const float inv_total = 255.0f / total;
		for (int i = 0; i < p_layers; i++) {
			quantized[i] = int(MAX(p_weights[i], 0.0f) * inv_total + 0.5f);
			sum += quantized[i];
			if (quantized[i] > quantized[largest]) {
				largest = i;
			}
		}
	}
	// Keep the sum exact so that weights stay normalized after quantization.
	quantized[largest] = CLAMP(quantized[largest] + (255 - sum), 0, 255);
	for (int i = 0; i < p_layers; i++) {
		r_quantized[i] = uint8_t(quantized[i]);
	}
}

void LandscapeData::set_weights(int p_x, int p_z, const float *p_weights) {
	ERR_FAIL_COND(!is_valid());
	uint8_t w[MAX_LAYERS];
	quantize_weights(p_weights, get_weightmap_count() * 4, w);
	storage.set_weights(p_x, p_z, w);
}

void LandscapeData::get_edit_weights(int p_x, int p_z, float *r_weights) const {
	uint8_t w[MAX_LAYERS] = {};
	if (is_valid()) {
		storage.get_base_weights(p_x, p_z, w);
	}
	const int layers = get_weightmap_count() * 4;
	for (int i = 0; i < MAX_LAYERS; i++) {
		r_weights[i] = i < layers ? w[i] / 255.0f : 0.0f;
	}
}

void LandscapeData::set_edit_weights(int p_x, int p_z, const float *p_weights) {
	ERR_FAIL_COND(!is_valid());
	uint8_t w[MAX_LAYERS];
	quantize_weights(p_weights, get_weightmap_count() * 4, w);
	storage.set_weights(p_x, p_z, w);
	if (storage.tile_has_base(p_x >> LandscapeStorage::TILE_SHIFT, p_z >> LandscapeStorage::TILE_SHIFT)) {
		storage.set_base_weights(p_x, p_z, w);
		_request_composite(Rect2i(p_x, p_z, 1, 1), CHANGED_WEIGHTS);
	}
}

int LandscapeData::get_dominant_layer(int p_x, int p_z) const {
	float w[MAX_LAYERS];
	get_weights(p_x, p_z, w);
	int best = 0;
	for (int i = 1; i < MAX_LAYERS; i++) {
		if (w[i] > w[best]) {
			best = i;
		}
	}
	return best;
}

void LandscapeData::fill_layer(int p_layer) {
	ERR_FAIL_INDEX(p_layer, MAX_LAYERS);
	ERR_FAIL_COND(!is_valid());
	ensure_layer_capacity(p_layer + 1);
	const Vector2i size = get_size();
	const int map = p_layer >> 2;
	const int channel = p_layer & 3;
	const bool base = has_base();
	Vector<uint8_t> band;
	for (int m = 0; m < get_weightmap_count(); m++) {
		for (int z = 0; z < size.y; z += BAND_ROWS) {
			const Rect2i rect(0, z, size.x, MIN(BAND_ROWS, size.y - z));
			band.resize(int64_t(rect.size.x) * rect.size.y * 4);
			uint8_t *w = band.ptrw();
			memset(w, 0, band.size());
			if (m == map) {
				for (int64_t i = 0; i < int64_t(rect.size.x) * rect.size.y; i++) {
					w[i * 4 + channel] = 255;
				}
			}
			storage.write_region(LandscapeStorage::LAYER_WEIGHTS_0 + m, rect, w);
			if (base) {
				storage.write_region(LandscapeStorage::LAYER_BASE_WEIGHTS_0 + m, rect, w);
			}
			storage.trim();
		}
	}
	if (base) {
		_request_composite(Rect2i(Point2i(), size), CHANGED_WEIGHTS);
	}
	notify_region_changed(Rect2i(Point2i(), size), CHANGED_WEIGHTS);
}

void LandscapeData::remove_layer(int p_layer) {
	ERR_FAIL_INDEX(p_layer, MAX_LAYERS);
	if (!is_valid()) {
		return;
	}
	// Remove the channel of the layer and shift the following layers down, keeping weights normalized.
	const Vector2i size = get_size();
	const int weightmaps = get_weightmap_count();
	const int layers = weightmaps * 4;
	Vector<uint8_t> bands[MAX_WEIGHTMAPS];
	// Both the final data and the base layer are shifted (the splines are composited again afterwards).
	const int passes = has_base() ? 2 : 1;
	for (int pass = 0; pass < passes; pass++) {
		const int first_layer = pass == 0 ? LandscapeStorage::LAYER_WEIGHTS_0 : LandscapeStorage::LAYER_BASE_WEIGHTS_0;
		for (int z = 0; z < size.y; z += BAND_ROWS) {
			const Rect2i rect(0, z, size.x, MIN(BAND_ROWS, size.y - z));
			if (pass == 1 && !storage.has_base_in_rect(rect)) {
				continue;
			}
			const int64_t count = int64_t(rect.size.x) * rect.size.y;
			for (int m = 0; m < weightmaps; m++) {
				bands[m].resize(count * 4);
				storage.read_region(first_layer + m, 0, rect, bands[m].ptrw());
			}
			float w[MAX_LAYERS];
			uint8_t q[MAX_LAYERS];
			for (int64_t i = 0; i < count; i++) {
				for (int l = 0; l < layers; l++) {
					w[l] = bands[l >> 2][i * 4 + (l & 3)] / 255.0f;
				}
				for (int l = p_layer; l < layers - 1; l++) {
					w[l] = w[l + 1];
				}
				if (p_layer < layers) {
					w[layers - 1] = 0.0;
				}
				quantize_weights(w, layers, q);
				for (int l = 0; l < layers; l++) {
					bands[l >> 2].write[i * 4 + (l & 3)] = q[l];
				}
			}
			for (int m = 0; m < weightmaps; m++) {
				storage.write_region(first_layer + m, rect, bands[m].ptr());
			}
			storage.trim();
		}
	}
	if (passes == 2) {
		_request_composite(Rect2i(Point2i(), size), CHANGED_WEIGHTS);
	}
	notify_region_changed(Rect2i(Point2i(), size), CHANGED_WEIGHTS);
}

/* Holes */

bool LandscapeData::is_hole(int p_x, int p_z) const {
	return is_valid() && storage.is_hole(p_x, p_z);
}

void LandscapeData::set_hole(int p_x, int p_z, bool p_hole) {
	ERR_FAIL_COND(!is_valid());
	const bool had_holes = has_holes();
	storage.set_hole(p_x, p_z, p_hole);
	if (!had_holes && has_holes()) {
		// Landscapes using this data need their hole resources.
		emit_changed();
	}
}

void LandscapeData::get_hole_cells(const Rect2i &p_texel_rect, int p_cell_quads, HashSet<Vector2i> &r_cells) const {
	ERR_FAIL_COND(p_cell_quads <= 0);
	const Rect2i rect = clip_rect(p_texel_rect);
	if (!has_holes() || !rect.has_area()) {
		return;
	}
	constexpr int TILE_SIZE = LandscapeStorage::TILE_SIZE;
	const Vector2i tile_begin = rect.position / TILE_SIZE;
	const Vector2i tile_end = (rect.get_end() - Vector2i(1, 1)) / TILE_SIZE;
	for (int tz = tile_begin.y; tz <= tile_end.y; tz++) {
		for (int tx = tile_begin.x; tx <= tile_end.x; tx++) {
			const LandscapeStorage::Tile *tile = storage.get_tile(0, tx, tz, LandscapeStorage::MASK_HOLES);
			if (!tile || tile->holes.is_empty()) {
				continue; // No hole in the tile.
			}
			const Rect2i texels = rect.intersection(Rect2i(tx * TILE_SIZE, tz * TILE_SIZE, TILE_SIZE, TILE_SIZE));
			const uint8_t *holes = tile->holes.ptr();
			for (int z = texels.position.y; z < texels.get_end().y; z++) {
				for (int x = texels.position.x; x < texels.get_end().x; x++) {
					if (holes[(z - tz * TILE_SIZE) * TILE_SIZE + (x - tx * TILE_SIZE)] == 0) {
						continue;
					}
					// The texels on the border of a cell belong to its neighbors too.
					const Vector2i cell(x / p_cell_quads, z / p_cell_quads);
					const bool left = x % p_cell_quads == 0 && cell.x > 0;
					const bool top = z % p_cell_quads == 0 && cell.y > 0;
					r_cells.insert(cell);
					if (left) {
						r_cells.insert(cell - Vector2i(1, 0));
					}
					if (top) {
						r_cells.insert(cell - Vector2i(0, 1));
					}
					if (left && top) {
						r_cells.insert(cell - Vector2i(1, 1));
					}
				}
			}
		}
		storage.trim();
	}
}

void LandscapeData::clear_holes() {
	if (!has_holes()) {
		return;
	}
	storage.set_has_holes(false);
	notify_region_changed(Rect2i(Point2i(), get_size()), CHANGED_HOLES);
	emit_changed();
}

/* Regions */

Dictionary LandscapeData::get_region(const Rect2i &p_rect, bool p_heights, bool p_weights) const {
	Dictionary region;
	const Rect2i rect = clip_rect(p_rect);
	region["rect"] = rect;
	if (!rect.has_area()) {
		return region;
	}
	const int64_t count = int64_t(rect.size.x) * rect.size.y;

	if (p_heights) {
		PackedFloat32Array data;
		data.resize(count);
		storage.read_region(LandscapeStorage::LAYER_HEIGHTS, 0, rect, data.ptrw());
		region["heights"] = data;

		if (has_holes()) {
			PackedByteArray hole_data;
			hole_data.resize(count);
			storage.read_region(LandscapeStorage::LAYER_HOLES, 0, rect, hole_data.ptrw());
			region["holes"] = hole_data;
		}
	}

	if (p_weights) {
		Array maps;
		for (int m = 0; m < get_weightmap_count(); m++) {
			PackedByteArray data;
			data.resize(count * 4);
			storage.read_region(LandscapeStorage::LAYER_WEIGHTS_0 + m, 0, rect, data.ptrw());
			maps.push_back(data);
		}
		region["weightmaps"] = maps;
	}

	if (storage.has_base_in_rect(rect)) {
		// Edit layer under splines (the final data elsewhere in the rect).
		if (p_heights) {
			PackedFloat32Array data;
			data.resize(count);
			storage.read_region(LandscapeStorage::LAYER_BASE_HEIGHTS, 0, rect, data.ptrw());
			region["base_heights"] = data;
		}
		if (p_weights) {
			Array maps;
			for (int m = 0; m < get_weightmap_count(); m++) {
				PackedByteArray data;
				data.resize(count * 4);
				storage.read_region(LandscapeStorage::LAYER_BASE_WEIGHTS_0 + m, 0, rect, data.ptrw());
				maps.push_back(data);
			}
			region["base_weightmaps"] = maps;
		}
	}
	return region;
}

void LandscapeData::set_region(const Dictionary &p_region) {
	ERR_FAIL_COND(!is_valid());
	ERR_FAIL_COND(!p_region.has("rect"));
	const Rect2i rect = p_region["rect"];
	ERR_FAIL_COND_MSG(clip_rect(rect) != rect, "Region is outside of the landscape bounds.");
	if (!rect.has_area()) {
		return;
	}
	const int64_t count = int64_t(rect.size.x) * rect.size.y;
	int flags = 0;

	if (p_region.has("heights")) {
		const PackedFloat32Array data = p_region["heights"];
		ERR_FAIL_COND(data.size() != count);
		storage.write_region(LandscapeStorage::LAYER_HEIGHTS, rect, data.ptr());
		flags |= CHANGED_HEIGHTS;
	}

	if (p_region.has("holes") || (p_region.has("heights") && has_holes())) {
		// Snapshots taken before the first hole was painted don't contain holes: restore them as solid.
		const PackedByteArray data = p_region.get("holes", PackedByteArray());
		ERR_FAIL_COND(!data.is_empty() && data.size() != count);
		if (!data.is_empty() || has_holes()) {
			const bool had_holes = has_holes();
			storage.write_region(LandscapeStorage::LAYER_HOLES, rect, data.is_empty() ? nullptr : data.ptr());
			flags |= CHANGED_HOLES;
			if (!had_holes) {
				emit_changed();
			}
		}
	}

	if (p_region.has("weightmaps")) {
		const Array maps = p_region["weightmaps"];
		ensure_layer_capacity(maps.size() * 4);
		for (int m = 0; m < MIN(maps.size(), get_weightmap_count()); m++) {
			const PackedByteArray data = maps[m];
			ERR_CONTINUE(data.size() != count * 4);
			storage.write_region(LandscapeStorage::LAYER_WEIGHTS_0 + m, rect, data.ptr());
		}
		flags |= CHANGED_WEIGHTS;
	}

	if (storage.has_base_in_rect(rect)) {
		// Restore the edit layer under splines. Snapshots taken where there was no base contain it
		// as final data. The splines are composited again on top of it.
		int base_flags = 0;
		if (p_region.has("heights")) {
			const PackedFloat32Array data = p_region.get("base_heights", p_region["heights"]);
			ERR_FAIL_COND(data.size() != count);
			storage.write_region(LandscapeStorage::LAYER_BASE_HEIGHTS, rect, data.ptr());
			base_flags |= CHANGED_HEIGHTS;
		}
		if (p_region.has("weightmaps")) {
			const Array maps = p_region.get("base_weightmaps", p_region["weightmaps"]);
			for (int m = 0; m < MIN(maps.size(), get_weightmap_count()); m++) {
				const PackedByteArray data = maps[m];
				ERR_CONTINUE(data.size() != count * 4);
				storage.write_region(LandscapeStorage::LAYER_BASE_WEIGHTS_0 + m, rect, data.ptr());
			}
			base_flags |= CHANGED_WEIGHTS;
		}
		_request_composite(rect, base_flags);
	}

	if (flags) {
		notify_region_changed(rect, flags);
	}
}

/* Images */

Ref<Image> LandscapeData::get_heightmap_image() const {
	ERR_FAIL_COND_V(!is_valid(), Ref<Image>());
	const Vector2i size = get_size();
	Vector<uint8_t> bytes;
	bytes.resize(int64_t(size.x) * size.y * sizeof(float));
	storage.read_region(LandscapeStorage::LAYER_HEIGHTS, 0, Rect2i(Point2i(), size), bytes.ptrw());
	return Image::create_from_data(size.x, size.y, false, Image::FORMAT_RF, bytes);
}

void LandscapeData::set_heightmap_image(const Ref<Image> &p_image, float p_scale, float p_offset) {
	ERR_FAIL_COND(p_image.is_null() || p_image->is_empty());
	ERR_FAIL_COND(!is_valid());
	const Vector2i size = get_size();

	Ref<Image> img = p_image->duplicate();
	if (img->is_compressed()) {
		img->decompress();
	}
	img->clear_mipmaps();
	if (img->get_format() != Image::FORMAT_RF) {
		img->convert(Image::FORMAT_RF);
	}
	if (img->get_width() != size.x || img->get_height() != size.y) {
		img->resize(size.x, size.y, Image::INTERPOLATE_BILINEAR);
	}

	Vector<uint8_t> data = img->get_data();
	img.unref();
	float *h = reinterpret_cast<float *>(data.ptrw());
	const int64_t count = int64_t(size.x) * size.y;
	for (int64_t i = 0; i < count; i++) {
		h[i] = h[i] * p_scale + p_offset;
	}
	// The imported heightmap is the new edit layer, splines are composited on top of it.
	_write_image_rows(LandscapeStorage::LAYER_HEIGHTS, data, 4, true);
	notify_region_changed(Rect2i(Point2i(), size), CHANGED_HEIGHTS);
}

Ref<Image> LandscapeData::get_weightmap_image(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, get_weightmap_count(), Ref<Image>());
	const Vector2i size = get_size();
	Vector<uint8_t> bytes;
	bytes.resize(int64_t(size.x) * size.y * 4);
	storage.read_region(LandscapeStorage::LAYER_WEIGHTS_0 + p_index, 0, Rect2i(Point2i(), size), bytes.ptrw());
	return Image::create_from_data(size.x, size.y, false, Image::FORMAT_RGBA8, bytes);
}

void LandscapeData::set_weightmap_image(int p_index, const Ref<Image> &p_image) {
	ERR_FAIL_INDEX(p_index, MAX_WEIGHTMAPS);
	ERR_FAIL_COND(p_image.is_null() || p_image->is_empty());
	ERR_FAIL_COND(!is_valid());
	if (p_index >= get_weightmap_count()) {
		set_weightmap_count(p_index + 1);
	}
	const Vector2i size = get_size();
	Ref<Image> img = p_image->duplicate();
	if (img->is_compressed()) {
		img->decompress();
	}
	img->clear_mipmaps();
	if (img->get_format() != Image::FORMAT_RGBA8) {
		img->convert(Image::FORMAT_RGBA8);
	}
	if (img->get_width() != size.x || img->get_height() != size.y) {
		img->resize(size.x, size.y, Image::INTERPOLATE_BILINEAR);
	}
	_write_image_rows(LandscapeStorage::LAYER_WEIGHTS_0 + p_index, img->get_data(), 4, true);
	notify_region_changed(Rect2i(Point2i(), size), CHANGED_WEIGHTS);
}

Ref<Image> LandscapeData::_load_png16(const String &p_path) {
	const Vector<uint8_t> file = FileAccess::get_file_as_bytes(p_path);
	ERR_FAIL_COND_V_MSG(file.is_empty(), Ref<Image>(), vformat("Can't open PNG file: '%s'.", p_path));

	png_image png_img;
	memset(&png_img, 0, sizeof(png_img));
	png_img.version = PNG_IMAGE_VERSION;
	if (!png_image_begin_read_from_memory(&png_img, file.ptr(), file.size())) {
		ERR_FAIL_V_MSG(Ref<Image>(), vformat("Invalid PNG file '%s': %s.", p_path, png_img.message));
	}

	const int width = png_img.width;
	const int height = png_img.height;
	const bool is_16_bit = (png_img.format & PNG_FORMAT_FLAG_LINEAR) != 0;

	Vector<uint8_t> data;
	data.resize(int64_t(width) * height * sizeof(float));
	float *dst = reinterpret_cast<float *>(data.ptrw());
	if (is_16_bit) {
		// 16-bit linear grayscale keeps the full precision of the source heightmap.
		png_img.format = PNG_FORMAT_LINEAR_Y;
		Vector<uint16_t> pixels;
		pixels.resize(int64_t(width) * height);
		if (!png_image_finish_read(&png_img, nullptr, pixels.ptrw(), 0, nullptr)) {
			png_image_free(&png_img);
			ERR_FAIL_V_MSG(Ref<Image>(), vformat("Failed to decode PNG file '%s': %s.", p_path, png_img.message));
		}
		const uint16_t *src = pixels.ptr();
		for (int64_t i = 0; i < pixels.size(); i++) {
			dst[i] = src[i] / 65535.0f;
		}
	} else {
		// 8-bit files are read as stored (no gamma conversion), heightmaps are linear data.
		png_img.format = PNG_FORMAT_GRAY;
		Vector<uint8_t> pixels;
		pixels.resize(int64_t(width) * height);
		if (!png_image_finish_read(&png_img, nullptr, pixels.ptrw(), 0, nullptr)) {
			png_image_free(&png_img);
			ERR_FAIL_V_MSG(Ref<Image>(), vformat("Failed to decode PNG file '%s': %s.", p_path, png_img.message));
		}
		const uint8_t *src = pixels.ptr();
		for (int64_t i = 0; i < pixels.size(); i++) {
			dst[i] = src[i] / 255.0f;
		}
	}
	return Image::create_from_data(width, height, false, Image::FORMAT_RF, data);
}

Ref<Image> LandscapeData::_load_raw16(const String &p_path) {
	Error err;
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ, &err);
	ERR_FAIL_COND_V_MSG(f.is_null(), Ref<Image>(), vformat("Can't open RAW heightmap: '%s'.", p_path));
	const uint64_t length = f->get_length();
	const uint64_t count = length / 2;
	const int side = int(Math::round(Math::sqrt(double(count))));
	ERR_FAIL_COND_V_MSG(uint64_t(side) * side != count, Ref<Image>(), "RAW heightmaps must be square and contain 16-bit little-endian samples.");

	Vector<uint8_t> data;
	data.resize(count * sizeof(float));
	float *dst = reinterpret_cast<float *>(data.ptrw());
	f->set_big_endian(false);
	for (uint64_t i = 0; i < count; i++) {
		dst[i] = f->get_16() / 65535.0f;
	}
	return Image::create_from_data(side, side, false, Image::FORMAT_RF, data);
}

Error LandscapeData::_save_png16(const String &p_path, const Vector<uint16_t> &p_data, int p_width, int p_height) {
	png_image png_img;
	memset(&png_img, 0, sizeof(png_img));
	png_img.version = PNG_IMAGE_VERSION;
	png_img.width = p_width;
	png_img.height = p_height;
	png_img.format = PNG_FORMAT_LINEAR_Y;

	png_alloc_size_t buffer_size = 0;
	if (!png_image_write_get_memory_size(png_img, buffer_size, 0, p_data.ptr(), 0, nullptr)) {
		ERR_FAIL_V_MSG(ERR_CANT_CREATE, vformat("Failed to encode PNG: %s.", png_img.message));
	}
	Vector<uint8_t> buffer;
	buffer.resize(buffer_size);
	if (!png_image_write_to_memory(&png_img, buffer.ptrw(), &buffer_size, 0, p_data.ptr(), 0, nullptr)) {
		ERR_FAIL_V_MSG(ERR_CANT_CREATE, vformat("Failed to encode PNG: %s.", png_img.message));
	}

	Error err;
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::WRITE, &err);
	ERR_FAIL_COND_V_MSG(f.is_null(), err, vformat("Can't write file: '%s'.", p_path));
	f->store_buffer(buffer.ptr(), buffer_size);
	return OK;
}

Error LandscapeData::import_heightmap(const String &p_path, float p_scale, float p_offset, bool p_resize) {
	const String ext = p_path.get_extension().to_lower();
	Ref<Image> img;
	if (ext == "png") {
		img = _load_png16(p_path);
	} else if (ext == "r16" || ext == "raw") {
		img = _load_raw16(p_path);
	} else {
		img = Image::load_from_file(p_path);
	}
	ERR_FAIL_COND_V_MSG(img.is_null() || img->is_empty(), ERR_FILE_CORRUPT, vformat("Can't load heightmap: '%s'.", p_path));

	if (img->is_compressed()) {
		img->decompress();
	}
	if (img->get_format() != Image::FORMAT_RF) {
		img->convert(Image::FORMAT_RF);
	}

	if (p_resize || !is_valid()) {
		const Vector2i new_size(CLAMP(img->get_width(), MIN_RESOLUTION, MAX_RESOLUTION), CLAMP(img->get_height(), MIN_RESOLUTION, MAX_RESOLUTION));
		if (!is_valid()) {
			create(new_size, vertex_spacing);
		} else if (new_size != get_size()) {
			// Recreate while keeping the vertex spacing and the painted layers (resampled edit layer,
			// the splines are applied again on the new resolution).
			const int weightmaps = get_weightmap_count();
			const Vector2i old_size = get_size();
			Vector<Ref<Image>> weight_images;
			for (int i = 0; i < weightmaps; i++) {
				Ref<Image> wimg = Image::create_from_data(old_size.x, old_size.y, false, Image::FORMAT_RGBA8, _read_layer_rows(LandscapeStorage::LAYER_BASE_WEIGHTS_0 + i, 4));
				wimg->resize(new_size.x, new_size.y, Image::INTERPOLATE_BILINEAR);
				weight_images.push_back(wimg);
			}
			storage.init(new_size, weightmaps, 0.0);
			_clear_lod_trees();
			saved_lod_trees.clear();
			spline_records.clear();
			pending_composite = Rect2i();
			for (int i = 0; i < weight_images.size(); i++) {
				_write_image_rows(LandscapeStorage::LAYER_WEIGHTS_0 + i, weight_images[i]->get_data(), 4);
			}
		}
	}

	set_heightmap_image(img, p_scale, p_offset);
	emit_changed();
	return OK;
}

Error LandscapeData::export_heightmap(const String &p_path) const {
	ERR_FAIL_COND_V(!is_valid(), ERR_UNCONFIGURED);
	const String ext = p_path.get_extension().to_lower();
	const Vector2i size = get_size();

	if (ext == "exr") {
		return get_heightmap_image()->save_exr(p_path, true);
	}

	// Integer formats are normalized to the current height range.
	const Vector2 range = get_height_range();
	const float scale = range.y > range.x ? 65535.0f / (range.y - range.x) : 0.0f;
	Vector<uint16_t> data;
	data.resize(int64_t(size.x) * size.y);
	uint16_t *w = data.ptrw();
	LocalVector<float> band;
	for (int z = 0; z < size.y; z += BAND_ROWS) {
		const Rect2i rect(0, z, size.x, MIN(BAND_ROWS, size.y - z));
		band.resize(int64_t(rect.size.x) * rect.size.y);
		storage.read_region(LandscapeStorage::LAYER_HEIGHTS, 0, rect, band.ptr());
		for (uint32_t i = 0; i < band.size(); i++) {
			w[int64_t(z) * size.x + i] = uint16_t(CLAMP(Math::round((band[i] - range.x) * scale), 0.0f, 65535.0f));
		}
		storage.trim();
	}

	if (ext == "png") {
		return _save_png16(p_path, data, size.x, size.y);
	}
	if (ext == "r16" || ext == "raw") {
		Error err;
		Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::WRITE, &err);
		ERR_FAIL_COND_V_MSG(f.is_null(), err, vformat("Can't write file: '%s'.", p_path));
		f->set_big_endian(false);
		for (int64_t i = 0; i < data.size(); i++) {
			f->store_16(data[i]);
		}
		return OK;
	}
	ERR_FAIL_V_MSG(ERR_FILE_UNRECOGNIZED, vformat("Unsupported heightmap export format: '%s'. Use .exr, .png, .r16 or .raw.", ext));
}

void LandscapeData::set_layer_weights_image(int p_layer, const Ref<Image> &p_image) {
	ERR_FAIL_INDEX(p_layer, MAX_LAYERS);
	ERR_FAIL_COND(p_image.is_null() || p_image->is_empty());
	ERR_FAIL_COND(!is_valid());
	ensure_layer_capacity(p_layer + 1);
	const Vector2i size = get_size();

	Ref<Image> img = p_image->duplicate();
	if (img->is_compressed()) {
		img->decompress();
	}
	img->clear_mipmaps();
	img->convert(Image::FORMAT_RF);
	if (img->get_width() != size.x || img->get_height() != size.y) {
		img->resize(size.x, size.y, Image::INTERPOLATE_BILINEAR);
	}
	const Vector<uint8_t> bytes = img->get_data();
	img.unref();
	const float *mask = reinterpret_cast<const float *>(bytes.ptr());

	// The imported mask becomes the weight of the layer, the other layers share the rest.
	// It is applied to the edit layer (the base under splines), the splines are composited again afterwards.
	const int weightmaps = get_weightmap_count();
	const int layers = weightmaps * 4;
	const bool base = has_base();
	Vector<uint8_t> bands[MAX_WEIGHTMAPS];
	float w[MAX_LAYERS];
	uint8_t q[MAX_LAYERS];
	for (int z0 = 0; z0 < size.y; z0 += BAND_ROWS) {
		const Rect2i rect(0, z0, size.x, MIN(BAND_ROWS, size.y - z0));
		const int64_t count = int64_t(rect.size.x) * rect.size.y;
		for (int m = 0; m < weightmaps; m++) {
			bands[m].resize(count * 4);
			storage.read_region(LandscapeStorage::LAYER_BASE_WEIGHTS_0 + m, 0, rect, bands[m].ptrw());
		}
		uint8_t *band_ptrs[MAX_WEIGHTMAPS] = {};
		for (int m = 0; m < weightmaps; m++) {
			band_ptrs[m] = bands[m].ptrw();
		}
		const float *band_mask = mask + int64_t(z0) * size.x;
		for (int64_t i = 0; i < count; i++) {
			const float target = CLAMP(band_mask[i], 0.0f, 1.0f);
			int others = 0;
			for (int l = 0; l < layers; l++) {
				const int v = band_ptrs[l >> 2][i * 4 + (l & 3)];
				w[l] = v;
				if (l != p_layer) {
					others += v;
				}
			}
			// The other layers share the remaining weight, in proportion of their current weights.
			const float scale = others > 0 ? (1.0f - target) / float(others) : 0.0f;
			for (int l = 0; l < layers; l++) {
				w[l] = l == p_layer ? target : w[l] * scale;
			}
			if (others <= 0 && target < 1.0f) {
				w[p_layer == 0 ? 1 : 0] = 1.0f - target;
			}
			quantize_weights(w, layers, q);
			for (int l = 0; l < layers; l++) {
				band_ptrs[l >> 2][i * 4 + (l & 3)] = q[l];
			}
		}
		for (int m = 0; m < weightmaps; m++) {
			storage.write_region(LandscapeStorage::LAYER_WEIGHTS_0 + m, rect, bands[m].ptr());
			if (base) {
				storage.write_region(LandscapeStorage::LAYER_BASE_WEIGHTS_0 + m, rect, bands[m].ptr());
			}
		}
		storage.trim();
	}
	if (base) {
		_request_composite(Rect2i(Point2i(), size), CHANGED_WEIGHTS);
	}
	notify_region_changed(Rect2i(Point2i(), size), CHANGED_WEIGHTS);
}

Error LandscapeData::import_layer_weights(int p_layer, const String &p_path) {
	ERR_FAIL_COND_V(!is_valid(), ERR_UNCONFIGURED);
	const String ext = p_path.get_extension().to_lower();
	Ref<Image> img;
	if (ext == "png") {
		img = _load_png16(p_path);
	} else if (ext == "r16" || ext == "raw") {
		img = _load_raw16(p_path);
	} else {
		img = Image::load_from_file(p_path);
	}
	ERR_FAIL_COND_V_MSG(img.is_null() || img->is_empty(), ERR_FILE_CORRUPT, vformat("Can't load weightmap: '%s'.", p_path));
	set_layer_weights_image(p_layer, img);
	return OK;
}

/* Change notification and LOD trees */

void LandscapeData::notify_region_changed(const Rect2i &p_rect, int p_flags) {
	Rect2i rect = clip_rect(p_rect);
	// Edits of the base layer are composited with the splines first.
	_flush_composite(rect, p_flags);
	if (!rect.has_area()) {
		return;
	}
	if (p_flags & CHANGED_HEIGHTS) {
		saved_lod_trees.clear();
		for (KeyValue<int, LandscapeLodTree *> &kv : lod_trees) {
			kv.value->update(storage, rect);
		}
	}
	storage.trim();
	emit_signal(SNAME("region_changed"), rect, p_flags);
}

void LandscapeData::_clear_lod_trees() {
	for (KeyValue<int, LandscapeLodTree *> &kv : lod_trees) {
		memdelete(kv.value);
	}
	lod_trees.clear();
}

/* Splines */

void LandscapeData::_request_composite(const Rect2i &p_rect, int p_flags) {
	if (!compositor || compositing || !p_flags) {
		// Without compositor, edits are written through the base and the final data alike.
		return;
	}
	const Rect2i rect = clip_rect(p_rect);
	if (!rect.has_area()) {
		return;
	}
	pending_composite = pending_composite.has_area() ? pending_composite.merge(rect) : rect;
	pending_composite_flags |= p_flags;
}

void LandscapeData::_flush_composite(Rect2i &r_rect, int &r_flags) {
	if (!pending_composite.has_area()) {
		return;
	}
	const Rect2i rect = pending_composite;
	const int flags = pending_composite_flags;
	pending_composite = Rect2i();
	pending_composite_flags = 0;
	if (compositor) {
		compositing = true;
		compositor->composite_region(this, rect, flags);
		compositing = false;
	}
	r_rect = r_rect.has_area() ? r_rect.merge(rect) : rect;
	r_flags |= flags;
}

void LandscapeData::set_compositor(LandscapeDataCompositor *p_compositor) {
	compositor = p_compositor;
	if (!compositor) {
		pending_composite = Rect2i();
		pending_composite_flags = 0;
	}
}

Vector2i LandscapeData::get_tile_count() const {
	return is_valid() ? storage.get_mip_tiles(0) : Vector2i();
}

void LandscapeData::capture_base(const Vector2i &p_tile) {
	ERR_FAIL_COND(!is_valid());
	storage.set_tile_base(p_tile.x, p_tile.y, true);
}

void LandscapeData::release_base(const Vector2i &p_tile) {
	ERR_FAIL_COND(!is_valid());
	if (!storage.tile_has_base(p_tile.x, p_tile.y)) {
		return;
	}
	// Without splines, the final data is the base.
	const Rect2i rect = clip_rect(Rect2i(p_tile * LandscapeStorage::TILE_SIZE, Size2i(LandscapeStorage::TILE_SIZE, LandscapeStorage::TILE_SIZE)));
	const int64_t count = int64_t(rect.size.x) * rect.size.y;
	LocalVector<float> heights;
	heights.resize(count);
	storage.read_region(LandscapeStorage::LAYER_BASE_HEIGHTS, 0, rect, heights.ptr());
	storage.write_region(LandscapeStorage::LAYER_HEIGHTS, rect, heights.ptr());
	LocalVector<uint8_t> weights;
	weights.resize(count * 4);
	for (int m = 0; m < get_weightmap_count(); m++) {
		storage.read_region(LandscapeStorage::LAYER_BASE_WEIGHTS_0 + m, 0, rect, weights.ptr());
		storage.write_region(LandscapeStorage::LAYER_WEIGHTS_0 + m, rect, weights.ptr());
	}
	storage.set_tile_base(p_tile.x, p_tile.y, false);
}

void LandscapeData::clear_base() {
	if (!is_valid() || !has_base()) {
		return;
	}
	const Vector2i tiles = get_tile_count();
	for (int tz = 0; tz < tiles.y; tz++) {
		for (int tx = 0; tx < tiles.x; tx++) {
			if (storage.tile_has_base(tx, tz)) {
				storage.set_tile_base(tx, tz, false);
			}
		}
	}
	spline_records.clear();
	storage.trim();
}

void LandscapeData::set_spline_records(const Dictionary &p_records) {
	spline_records = p_records;
}

const LandscapeLodTree *LandscapeData::get_lod_tree(int p_patch_quads) const {
	ERR_FAIL_COND_V(!is_valid(), nullptr);
	LandscapeLodTree **existing = lod_trees.getptr(p_patch_quads);
	if (existing) {
		return *existing;
	}
	LandscapeLodTree *tree = memnew(LandscapeLodTree);
	bool restored = false;
	if (saved_lod_trees.has(p_patch_quads)) {
		const PackedFloat32Array saved = saved_lod_trees[p_patch_quads];
		LocalVector<float> saved_nodes;
		saved_nodes.resize(saved.size());
		memcpy(saved_nodes.ptr(), saved.ptr(), saved.size() * sizeof(float));
		restored = tree->set_nodes(get_size(), p_patch_quads, saved_nodes);
	}
	if (!restored) {
		const uint64_t begin = OS::get_singleton()->get_ticks_msec();
		tree->build(storage, p_patch_quads);
		const uint64_t elapsed = OS::get_singleton()->get_ticks_msec() - begin;
		if (elapsed > 500) {
			print_verbose(vformat("Landscape: built the LOD tree of a %dx%d landscape in %d ms. Save the data to cache it.", get_size().x, get_size().y, elapsed));
		}
		storage.trim();
	}
	lod_trees.insert(p_patch_quads, tree);
	return tree;
}

/* Files */

Error LandscapeData::save_to_file(const String &p_path) {
	ERR_FAIL_COND_V(!is_valid(), ERR_UNCONFIGURED);
	if (lod_trees.is_empty() && saved_lod_trees.is_empty()) {
		// Save the tree of the default patch size, so that loading doesn't need to read every tile.
		get_lod_tree(32);
	}
	Dictionary header;
	header["vertex_spacing"] = vertex_spacing;
	Dictionary trees;
	for (const KeyValue<int, LandscapeLodTree *> &kv : lod_trees) {
		const LocalVector<float> &nodes = kv.value->get_nodes();
		PackedFloat32Array packed;
		packed.resize(nodes.size());
		memcpy(packed.ptrw(), nodes.ptr(), nodes.size() * sizeof(float));
		trees[kv.key] = packed;
	}
	// Keep trees that were loaded but not used in this session.
	for (const KeyValue<Variant, Variant> &kv : saved_lod_trees) {
		if (!trees.has(kv.key)) {
			trees[kv.key] = kv.value;
		}
	}
	header["lod_trees"] = trees;
	if (!spline_records.is_empty()) {
		header["spline_records"] = spline_records;
	}
	return storage.save_file(p_path, header);
}

Error LandscapeData::load_from_file(const String &p_path) {
	Dictionary header;
	const Error err = storage.open_file(p_path, header);
	if (err != OK) {
		return err;
	}
	vertex_spacing = MAX(real_t(header.get("vertex_spacing", 1.0)), real_t(CMP_EPSILON));
	_clear_lod_trees();
	saved_lod_trees = header.get("lod_trees", Dictionary());
	spline_records = header.get("spline_records", Dictionary());
	pending_composite = Rect2i();
	emit_changed();
	return OK;
}

/* Serialization of embedded data */

void LandscapeData::_set_heights(const PackedFloat32Array &p_heights) {
	ERR_FAIL_COND(!is_valid());
	const Vector2i size = get_size();
	ERR_FAIL_COND_MSG(p_heights.size() != int64_t(size.x) * size.y, "The heights don't match the landscape resolution.");
	Vector<uint8_t> bytes;
	bytes.resize(p_heights.size() * sizeof(float));
	memcpy(bytes.ptrw(), p_heights.ptr(), bytes.size());
	_write_image_rows(LandscapeStorage::LAYER_HEIGHTS, bytes, 4);
	_clear_lod_trees();
	saved_lod_trees.clear();
	emit_changed();
}

PackedFloat32Array LandscapeData::_get_heights() const {
	PackedFloat32Array heights;
	if (!is_valid()) {
		return heights;
	}
	const Vector2i size = get_size();
	heights.resize(int64_t(size.x) * size.y);
	storage.read_region(LandscapeStorage::LAYER_HEIGHTS, 0, Rect2i(Point2i(), size), heights.ptrw());
	return heights;
}

void LandscapeData::_set_weightmaps(const Array &p_weightmaps) {
	ERR_FAIL_COND(!is_valid());
	const Vector2i size = get_size();
	const int count = CLAMP(int(p_weightmaps.size()), 1, MAX_WEIGHTMAPS);
	storage.set_weightmap_count(count);
	for (int i = 0; i < MIN(int(p_weightmaps.size()), MAX_WEIGHTMAPS); i++) {
		const PackedByteArray data = p_weightmaps[i];
		ERR_CONTINUE_MSG(data.size() != int64_t(size.x) * size.y * 4, "A weightmap doesn't match the landscape resolution.");
		_write_image_rows(LandscapeStorage::LAYER_WEIGHTS_0 + i, data, 4);
	}
	emit_changed();
}

Array LandscapeData::_get_weightmaps() const {
	Array maps;
	for (int i = 0; i < get_weightmap_count() && is_valid(); i++) {
		maps.push_back(get_weightmap_image(i)->get_data());
	}
	return maps;
}

void LandscapeData::_set_holes(const PackedByteArray &p_holes) {
	if (p_holes.is_empty() || !is_valid()) {
		return;
	}
	const Vector2i size = get_size();
	ERR_FAIL_COND_MSG(p_holes.size() != int64_t(size.x) * size.y, "The hole mask doesn't match the landscape resolution.");
	// Stored as 0/1 in resources, 0/255 in the storage (filtered in the mip levels).
	Vector<uint8_t> holes = p_holes;
	uint8_t *w = holes.ptrw();
	for (int64_t i = 0; i < holes.size(); i++) {
		w[i] = w[i] ? 255 : 0;
	}
	storage.set_has_holes(true);
	_write_image_rows(LandscapeStorage::LAYER_HOLES, holes, 1);
	emit_changed();
}

PackedByteArray LandscapeData::_get_holes() const {
	PackedByteArray holes;
	if (!has_holes() || !is_valid()) {
		return holes;
	}
	const Vector2i size = get_size();
	holes.resize(int64_t(size.x) * size.y);
	storage.read_region(LandscapeStorage::LAYER_HOLES, 0, Rect2i(Point2i(), size), holes.ptrw());
	uint8_t *w = holes.ptrw();
	for (int64_t i = 0; i < holes.size(); i++) {
		w[i] = w[i] >= 128 ? 1 : 0;
	}
	return holes;
}

void LandscapeData::_set_base_tiles(const Dictionary &p_tiles) {
	if (p_tiles.is_empty() || !is_valid()) {
		return;
	}
	// Tile -> [base heights, [base weightmaps]] of the valid texels of the tile.
	for (const KeyValue<Variant, Variant> &kv : p_tiles) {
		const Vector2i tile = kv.key;
		ERR_CONTINUE(!storage.has_tile(0, tile.x, tile.y));
		const Array entry = kv.value;
		ERR_CONTINUE(entry.size() != 2);
		const Rect2i rect = clip_rect(Rect2i(tile * LandscapeStorage::TILE_SIZE, Size2i(LandscapeStorage::TILE_SIZE, LandscapeStorage::TILE_SIZE)));
		const int64_t count = int64_t(rect.size.x) * rect.size.y;
		const PackedFloat32Array heights = entry[0];
		const Array maps = entry[1];
		ERR_CONTINUE(heights.size() != count);
		storage.set_tile_base(tile.x, tile.y, true);
		storage.write_region(LandscapeStorage::LAYER_BASE_HEIGHTS, rect, heights.ptr());
		for (int m = 0; m < MIN(maps.size(), get_weightmap_count()); m++) {
			const PackedByteArray weights = maps[m];
			ERR_CONTINUE(weights.size() != count * 4);
			storage.write_region(LandscapeStorage::LAYER_BASE_WEIGHTS_0 + m, rect, weights.ptr());
		}
	}
}

Dictionary LandscapeData::_get_base_tiles() const {
	Dictionary tiles;
	if (!is_valid() || !has_base()) {
		return tiles;
	}
	const Vector2i count = get_tile_count();
	for (int tz = 0; tz < count.y; tz++) {
		for (int tx = 0; tx < count.x; tx++) {
			if (!storage.tile_has_base(tx, tz)) {
				continue;
			}
			const Rect2i rect = clip_rect(Rect2i(Vector2i(tx, tz) * LandscapeStorage::TILE_SIZE, Size2i(LandscapeStorage::TILE_SIZE, LandscapeStorage::TILE_SIZE)));
			const int64_t texels = int64_t(rect.size.x) * rect.size.y;
			PackedFloat32Array heights;
			heights.resize(texels);
			storage.read_region(LandscapeStorage::LAYER_BASE_HEIGHTS, 0, rect, heights.ptrw());
			Array maps;
			for (int m = 0; m < get_weightmap_count(); m++) {
				PackedByteArray weights;
				weights.resize(texels * 4);
				storage.read_region(LandscapeStorage::LAYER_BASE_WEIGHTS_0 + m, 0, rect, weights.ptrw());
				maps.push_back(weights);
			}
			Array entry;
			entry.push_back(heights);
			entry.push_back(maps);
			tiles[Vector2i(tx, tz)] = entry;
		}
	}
	return tiles;
}

void LandscapeData::_bind_methods() {
	ClassDB::bind_method(D_METHOD("create", "size", "vertex_spacing", "height"), &LandscapeData::create, DEFVAL(1.0), DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("resize", "size"), &LandscapeData::resize);
	ClassDB::bind_method(D_METHOD("is_valid"), &LandscapeData::is_valid);

	ClassDB::bind_method(D_METHOD("_set_size", "size"), &LandscapeData::_set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &LandscapeData::get_size);
	ClassDB::bind_method(D_METHOD("set_vertex_spacing", "spacing"), &LandscapeData::set_vertex_spacing);
	ClassDB::bind_method(D_METHOD("get_vertex_spacing"), &LandscapeData::get_vertex_spacing);
	ClassDB::bind_method(D_METHOD("get_world_size"), &LandscapeData::get_world_size);

	ClassDB::bind_method(D_METHOD("get_height", "x", "z"), &LandscapeData::get_height);
	ClassDB::bind_method(D_METHOD("set_height", "x", "z", "height"), &LandscapeData::set_height);
	ClassDB::bind_method(D_METHOD("sample_height", "local_x", "local_z"), &LandscapeData::sample_height);
	ClassDB::bind_method(D_METHOD("sample_normal", "local_x", "local_z"), &LandscapeData::sample_normal);
	ClassDB::bind_method(D_METHOD("get_height_range"), &LandscapeData::get_height_range);

	ClassDB::bind_method(D_METHOD("set_weightmap_count", "count"), &LandscapeData::set_weightmap_count);
	ClassDB::bind_method(D_METHOD("get_weightmap_count"), &LandscapeData::get_weightmap_count);
	ClassDB::bind_method(D_METHOD("ensure_layer_capacity", "layer_count"), &LandscapeData::ensure_layer_capacity);
	ClassDB::bind_method(D_METHOD("get_layer_weight", "x", "z", "layer"), &LandscapeData::get_layer_weight);
	ClassDB::bind_method(D_METHOD("set_layer_weight", "x", "z", "layer", "weight"), &LandscapeData::set_layer_weight);
	ClassDB::bind_method(D_METHOD("get_dominant_layer", "x", "z"), &LandscapeData::get_dominant_layer);
	ClassDB::bind_method(D_METHOD("fill_layer", "layer"), &LandscapeData::fill_layer);
	ClassDB::bind_method(D_METHOD("remove_layer", "layer"), &LandscapeData::remove_layer);

	ClassDB::bind_method(D_METHOD("get_region", "rect", "heights", "weights"), &LandscapeData::get_region, DEFVAL(true), DEFVAL(true));
	ClassDB::bind_method(D_METHOD("set_region", "region"), &LandscapeData::set_region);

	ClassDB::bind_method(D_METHOD("get_heightmap_image"), &LandscapeData::get_heightmap_image);
	ClassDB::bind_method(D_METHOD("set_heightmap_image", "image", "scale", "offset"), &LandscapeData::set_heightmap_image, DEFVAL(1.0), DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("get_weightmap_image", "index"), &LandscapeData::get_weightmap_image);
	ClassDB::bind_method(D_METHOD("set_weightmap_image", "index", "image"), &LandscapeData::set_weightmap_image);
	ClassDB::bind_method(D_METHOD("import_heightmap", "path", "scale", "offset", "resize"), &LandscapeData::import_heightmap, DEFVAL(1.0), DEFVAL(0.0), DEFVAL(true));
	ClassDB::bind_method(D_METHOD("export_heightmap", "path"), &LandscapeData::export_heightmap);
	ClassDB::bind_method(D_METHOD("import_layer_weights", "layer", "path"), &LandscapeData::import_layer_weights);
	ClassDB::bind_method(D_METHOD("set_layer_weights_image", "layer", "image"), &LandscapeData::set_layer_weights_image);

	ClassDB::bind_method(D_METHOD("has_holes"), &LandscapeData::has_holes);
	ClassDB::bind_method(D_METHOD("is_hole", "x", "z"), &LandscapeData::is_hole);
	ClassDB::bind_method(D_METHOD("set_hole", "x", "z", "hole"), &LandscapeData::set_hole);
	ClassDB::bind_method(D_METHOD("clear_holes"), &LandscapeData::clear_holes);

	ClassDB::bind_method(D_METHOD("get_edit_height", "x", "z"), &LandscapeData::get_edit_height);
	ClassDB::bind_method(D_METHOD("has_base_layer"), &LandscapeData::has_base);
	ClassDB::bind_method(D_METHOD("clear_base_layer"), &LandscapeData::clear_base);
	ClassDB::bind_method(D_METHOD("_set_base_tiles", "tiles"), &LandscapeData::_set_base_tiles);
	ClassDB::bind_method(D_METHOD("_get_base_tiles"), &LandscapeData::_get_base_tiles);
	ClassDB::bind_method(D_METHOD("set_spline_records", "records"), &LandscapeData::set_spline_records);
	ClassDB::bind_method(D_METHOD("get_spline_records"), &LandscapeData::get_spline_records);

	ClassDB::bind_method(D_METHOD("notify_region_changed", "rect", "flags"), &LandscapeData::notify_region_changed);

	ClassDB::bind_method(D_METHOD("save_to_file", "path"), &LandscapeData::save_to_file);
	ClassDB::bind_method(D_METHOD("load_from_file", "path"), &LandscapeData::load_from_file);
	ClassDB::bind_method(D_METHOD("is_streamed"), &LandscapeData::is_streamed);
	ClassDB::bind_method(D_METHOD("has_unsaved_changes"), &LandscapeData::has_unsaved_changes);
	ClassDB::bind_method(D_METHOD("get_memory_usage"), &LandscapeData::get_memory_usage);
	ClassDB::bind_method(D_METHOD("get_loaded_tile_count"), &LandscapeData::get_loaded_tile_count);
	ClassDB::bind_method(D_METHOD("trim_memory"), &LandscapeData::trim_memory);

	ClassDB::bind_method(D_METHOD("_set_heights", "heights"), &LandscapeData::_set_heights);
	ClassDB::bind_method(D_METHOD("_get_heights"), &LandscapeData::_get_heights);
	ClassDB::bind_method(D_METHOD("_set_weightmaps", "weightmaps"), &LandscapeData::_set_weightmaps);
	ClassDB::bind_method(D_METHOD("_get_weightmaps"), &LandscapeData::_get_weightmaps);
	ClassDB::bind_method(D_METHOD("_set_holes", "holes"), &LandscapeData::_set_holes);
	ClassDB::bind_method(D_METHOD("_get_holes"), &LandscapeData::_get_holes);

	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2I, "size", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY), "_set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "vertex_spacing", PROPERTY_HINT_RANGE, "0.01,100,0.001,or_greater,suffix:m"), "set_vertex_spacing", "get_vertex_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "heights", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "_set_heights", "_get_heights");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "weightmaps", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "_set_weightmaps", "_get_weightmaps");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "holes", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "_set_holes", "_get_holes");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "base_tiles", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "_set_base_tiles", "_get_base_tiles");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "spline_records", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_spline_records", "get_spline_records");

	ADD_SIGNAL(MethodInfo("region_changed", PropertyInfo(Variant::RECT2I, "rect"), PropertyInfo(Variant::INT, "flags")));

	BIND_ENUM_CONSTANT(CHANGED_HEIGHTS);
	BIND_ENUM_CONSTANT(CHANGED_WEIGHTS);
	BIND_ENUM_CONSTANT(CHANGED_HOLES);
	BIND_ENUM_CONSTANT(CHANGED_ALL);

	BIND_CONSTANT(MAX_RESOLUTION);
	BIND_CONSTANT(MAX_LAYERS);
}

LandscapeData::LandscapeData() {
	storage.set_owner(get_instance_id());
	storage.set_memory_budget(int64_t(GLOBAL_GET("rendering/landscape/streaming/cpu_cache_size_mb")) * 1024 * 1024);
}

LandscapeData::~LandscapeData() {
	_clear_lod_trees();
}

/* ResourceFormatLoaderLandscapeData */

Ref<Resource> ResourceFormatLoaderLandscapeData::load(const String &p_path, const String &p_original_path, Error *r_error, bool p_use_sub_threads, float *r_progress, CacheMode p_cache_mode) {
	Ref<LandscapeData> data;
	data.instantiate();
	const Error err = data->load_from_file(p_path);
	if (r_error) {
		*r_error = err;
	}
	if (err != OK) {
		return Ref<Resource>();
	}
	return data;
}

void ResourceFormatLoaderLandscapeData::get_recognized_extensions(List<String> *p_extensions) const {
	p_extensions->push_back("lsdata");
}

bool ResourceFormatLoaderLandscapeData::handles_type(const String &p_type) const {
	return p_type == "LandscapeData";
}

String ResourceFormatLoaderLandscapeData::get_resource_type(const String &p_path) const {
	return p_path.get_extension().to_lower() == "lsdata" ? "LandscapeData" : "";
}

/* ResourceFormatSaverLandscapeData */

Error ResourceFormatSaverLandscapeData::save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags) {
	Ref<LandscapeData> data = p_resource;
	ERR_FAIL_COND_V(data.is_null(), ERR_INVALID_PARAMETER);
	return data->save_to_file(p_path);
}

bool ResourceFormatSaverLandscapeData::recognize(const Ref<Resource> &p_resource) const {
	return Object::cast_to<LandscapeData>(p_resource.ptr()) != nullptr;
}

void ResourceFormatSaverLandscapeData::get_recognized_extensions(const Ref<Resource> &p_resource, List<String> *p_extensions) const {
	if (Object::cast_to<LandscapeData>(p_resource.ptr())) {
		p_extensions->push_back("lsdata");
	}
}
