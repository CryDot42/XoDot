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

#include "core/io/file_access.h"
#include "core/object/class_db.h"

#include <png.h>

void LandscapeData::_allocate_weightmap(int p_index) {
	ERR_FAIL_INDEX(p_index, MAX_WEIGHTMAPS);
	weightmaps[p_index].resize(int64_t(size.x) * size.y * 4);
	memset(weightmaps[p_index].ptrw(), 0, weightmaps[p_index].size());
}

void LandscapeData::create(const Vector2i &p_size, real_t p_vertex_spacing, float p_height) {
	ERR_FAIL_COND_MSG(p_size.x < MIN_RESOLUTION || p_size.y < MIN_RESOLUTION, vformat("Landscape resolution must be at least %d.", MIN_RESOLUTION));
	ERR_FAIL_COND_MSG(p_size.x > MAX_RESOLUTION || p_size.y > MAX_RESOLUTION, vformat("Landscape resolution can't exceed %d.", MAX_RESOLUTION));
	ERR_FAIL_COND(p_vertex_spacing <= 0.0);

	size = p_size;
	vertex_spacing = p_vertex_spacing;

	heights.resize(int64_t(size.x) * size.y);
	float *h = heights.ptrw();
	for (int64_t i = 0; i < heights.size(); i++) {
		h[i] = p_height;
	}

	for (int i = 0; i < MAX_WEIGHTMAPS; i++) {
		weightmaps[i].clear();
	}
	weightmap_count = 1;
	_allocate_weightmap(0);
	fill_layer(0);

	height_range_dirty = true;
	notify_region_changed(Rect2i(Point2i(), size), CHANGED_ALL);
	emit_changed();
}

void LandscapeData::resize(const Vector2i &p_size) {
	ERR_FAIL_COND(!is_valid());
	ERR_FAIL_COND_MSG(p_size.x < MIN_RESOLUTION || p_size.y < MIN_RESOLUTION, vformat("Landscape resolution must be at least %d.", MIN_RESOLUTION));
	ERR_FAIL_COND_MSG(p_size.x > MAX_RESOLUTION || p_size.y > MAX_RESOLUTION, vformat("Landscape resolution can't exceed %d.", MAX_RESOLUTION));
	if (p_size == size) {
		return;
	}

	// Keep the same world size: the vertex spacing follows the new resolution.
	const Vector2 world_size = get_world_size();

	Ref<Image> height_image = get_heightmap_image();
	height_image->resize(p_size.x, p_size.y, Image::INTERPOLATE_BILINEAR);

	Vector<Ref<Image>> weight_images;
	for (int i = 0; i < weightmap_count; i++) {
		Ref<Image> img = get_weightmap_image(i);
		img->resize(p_size.x, p_size.y, Image::INTERPOLATE_BILINEAR);
		weight_images.push_back(img);
	}

	size = p_size;
	vertex_spacing = MAX(world_size.x / real_t(size.x - 1), CMP_EPSILON);

	const Vector<uint8_t> height_bytes = height_image->get_data();
	heights.resize(int64_t(size.x) * size.y);
	memcpy(heights.ptrw(), height_bytes.ptr(), heights.size() * sizeof(float));
	for (int i = 0; i < weight_images.size(); i++) {
		weightmaps[i] = weight_images[i]->get_data();
	}

	height_range_dirty = true;
	notify_region_changed(Rect2i(Point2i(), size), CHANGED_ALL);
	emit_changed();
}

void LandscapeData::_set_size(const Vector2i &p_size) {
	// Used by deserialization only, the data arrays are assigned afterwards.
	size = p_size.clamp(Vector2i(), Vector2i(MAX_RESOLUTION, MAX_RESOLUTION));
	height_range_dirty = true;
}

void LandscapeData::set_vertex_spacing(real_t p_spacing) {
	ERR_FAIL_COND(p_spacing <= 0.0);
	if (vertex_spacing == p_spacing) {
		return;
	}
	vertex_spacing = p_spacing;
	notify_region_changed(Rect2i(Point2i(), size), CHANGED_ALL);
	emit_changed();
}

Vector2 LandscapeData::get_world_size() const {
	return Vector2(MAX(size.x - 1, 0), MAX(size.y - 1, 0)) * vertex_spacing;
}

float LandscapeData::get_height(int p_x, int p_z) const {
	if (heights.is_empty()) {
		return 0.0;
	}
	p_x = CLAMP(p_x, 0, size.x - 1);
	p_z = CLAMP(p_z, 0, size.y - 1);
	return heights[_index(p_x, p_z)];
}

void LandscapeData::set_height(int p_x, int p_z, float p_height) {
	ERR_FAIL_INDEX(p_x, size.x);
	ERR_FAIL_INDEX(p_z, size.y);
	heights.write[_index(p_x, p_z)] = p_height;
	height_range_dirty = true;
}

float LandscapeData::sample_height(real_t p_local_x, real_t p_local_z) const {
	if (!is_valid()) {
		return 0.0;
	}
	const real_t fx = CLAMP(p_local_x / vertex_spacing, 0.0, real_t(size.x - 1));
	const real_t fz = CLAMP(p_local_z / vertex_spacing, 0.0, real_t(size.y - 1));
	const int x0 = MIN(int(fx), size.x - 2);
	const int z0 = MIN(int(fz), size.y - 2);
	const real_t tx = fx - x0;
	const real_t tz = fz - z0;
	const float *h = heights.ptr();
	const float h00 = h[_index(x0, z0)];
	const float h10 = h[_index(x0 + 1, z0)];
	const float h01 = h[_index(x0, z0 + 1)];
	const float h11 = h[_index(x0 + 1, z0 + 1)];
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
	if (!height_range_dirty) {
		return height_range;
	}
	height_range_dirty = false;
	if (heights.is_empty()) {
		height_range = Vector2();
		return height_range;
	}
	const float *h = heights.ptr();
	float mn = h[0];
	float mx = h[0];
	const int64_t count = heights.size();
	for (int64_t i = 1; i < count; i++) {
		mn = MIN(mn, h[i]);
		mx = MAX(mx, h[i]);
	}
	height_range = Vector2(mn, mx);
	return height_range;
}

void LandscapeData::set_weightmap_count(int p_count) {
	p_count = CLAMP(p_count, 0, MAX_WEIGHTMAPS);
	if (p_count == weightmap_count) {
		return;
	}
	for (int i = weightmap_count; i < p_count; i++) {
		_allocate_weightmap(i);
	}
	for (int i = p_count; i < MAX_WEIGHTMAPS; i++) {
		weightmaps[i].clear();
	}
	weightmap_count = p_count;
	notify_region_changed(Rect2i(Point2i(), size), CHANGED_WEIGHTS);
	emit_changed();
}

void LandscapeData::ensure_layer_capacity(int p_layer_count) {
	const int needed = CLAMP((p_layer_count + 3) / 4, 1, MAX_WEIGHTMAPS);
	if (needed > weightmap_count) {
		set_weightmap_count(needed);
	}
}

float LandscapeData::get_layer_weight(int p_x, int p_z, int p_layer) const {
	ERR_FAIL_INDEX_V(p_layer, MAX_LAYERS, 0.0);
	const int map = p_layer >> 2;
	if (map >= weightmap_count) {
		return 0.0;
	}
	p_x = CLAMP(p_x, 0, size.x - 1);
	p_z = CLAMP(p_z, 0, size.y - 1);
	return weightmaps[map][int64_t(_index(p_x, p_z)) * 4 + (p_layer & 3)] / 255.0;
}

void LandscapeData::set_layer_weight(int p_x, int p_z, int p_layer, float p_weight) {
	ERR_FAIL_INDEX(p_layer, MAX_LAYERS);
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
	const int64_t base = int64_t(_index(p_x, p_z)) * 4;
	for (int m = 0; m < MAX_WEIGHTMAPS; m++) {
		if (m < weightmap_count) {
			const uint8_t *w = weightmaps[m].ptr() + base;
			for (int c = 0; c < 4; c++) {
				r_weights[m * 4 + c] = w[c] / 255.0;
			}
		} else {
			for (int c = 0; c < 4; c++) {
				r_weights[m * 4 + c] = 0.0;
			}
		}
	}
}

void LandscapeData::set_weights(int p_x, int p_z, const float *p_weights) {
	float total = 0.0;
	int layers = weightmap_count * 4;
	for (int i = 0; i < layers; i++) {
		total += MAX(p_weights[i], 0.0f);
	}
	int quantized[MAX_LAYERS] = {};
	int sum = 0;
	int largest = 0;
	if (total <= 0.0) {
		quantized[0] = 255;
		sum = 255;
	} else {
		for (int i = 0; i < layers; i++) {
			quantized[i] = int(Math::round(MAX(p_weights[i], 0.0f) / total * 255.0f));
			sum += quantized[i];
			if (quantized[i] > quantized[largest]) {
				largest = i;
			}
		}
	}
	// Keep the sum exact so that weights stay normalized after quantization.
	quantized[largest] = CLAMP(quantized[largest] + (255 - sum), 0, 255);

	const int64_t base = int64_t(_index(p_x, p_z)) * 4;
	for (int m = 0; m < weightmap_count; m++) {
		uint8_t *w = weightmaps[m].ptrw() + base;
		for (int c = 0; c < 4; c++) {
			w[c] = uint8_t(quantized[m * 4 + c]);
		}
	}
}

int LandscapeData::get_dominant_layer(int p_x, int p_z) const {
	float w[MAX_LAYERS];
	p_x = CLAMP(p_x, 0, size.x - 1);
	p_z = CLAMP(p_z, 0, size.y - 1);
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
	ensure_layer_capacity(p_layer + 1);
	const int map = p_layer >> 2;
	const int channel = p_layer & 3;
	const int64_t count = int64_t(size.x) * size.y;
	for (int m = 0; m < weightmap_count; m++) {
		uint8_t *w = weightmaps[m].ptrw();
		for (int64_t i = 0; i < count; i++) {
			for (int c = 0; c < 4; c++) {
				w[i * 4 + c] = (m == map && c == channel) ? 255 : 0;
			}
		}
	}
	notify_region_changed(Rect2i(Point2i(), size), CHANGED_WEIGHTS);
}

void LandscapeData::remove_layer(int p_layer) {
	ERR_FAIL_INDEX(p_layer, MAX_LAYERS);
	if (!is_valid() || weightmap_count == 0) {
		return;
	}
	// Remove the channel of the layer and shift the following layers down, keeping weights normalized.
	const int64_t count = int64_t(size.x) * size.y;
	float w[MAX_LAYERS];
	for (int64_t i = 0; i < count; i++) {
		const int x = int(i % size.x);
		const int z = int(i / size.x);
		get_weights(x, z, w);
		for (int l = p_layer; l < MAX_LAYERS - 1; l++) {
			w[l] = w[l + 1];
		}
		w[MAX_LAYERS - 1] = 0.0;
		set_weights(x, z, w);
	}
	notify_region_changed(Rect2i(Point2i(), size), CHANGED_WEIGHTS);
}

const uint8_t *LandscapeData::get_weightmap_ptr(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, weightmap_count, nullptr);
	return weightmaps[p_index].ptr();
}

uint8_t *LandscapeData::get_weightmap_ptrw(int p_index) {
	ERR_FAIL_INDEX_V(p_index, weightmap_count, nullptr);
	return weightmaps[p_index].ptrw();
}

const Vector<uint8_t> &LandscapeData::get_weightmap_vector(int p_index) const {
	static const Vector<uint8_t> empty;
	ERR_FAIL_INDEX_V(p_index, weightmap_count, empty);
	return weightmaps[p_index];
}

Rect2i LandscapeData::clip_rect(const Rect2i &p_rect) const {
	return p_rect.intersection(Rect2i(Point2i(), size));
}

Dictionary LandscapeData::get_region(const Rect2i &p_rect, bool p_heights, bool p_weights) const {
	Dictionary region;
	const Rect2i rect = clip_rect(p_rect);
	region["rect"] = rect;
	if (!rect.has_area()) {
		return region;
	}

	if (p_heights) {
		PackedFloat32Array data;
		data.resize(rect.size.x * rect.size.y);
		float *w = data.ptrw();
		const float *h = heights.ptr();
		for (int z = 0; z < rect.size.y; z++) {
			memcpy(w + z * rect.size.x, h + _index(rect.position.x, rect.position.y + z), sizeof(float) * rect.size.x);
		}
		region["heights"] = data;
	}

	if (p_weights) {
		Array maps;
		for (int m = 0; m < weightmap_count; m++) {
			PackedByteArray data;
			data.resize(rect.size.x * rect.size.y * 4);
			uint8_t *w = data.ptrw();
			const uint8_t *src = weightmaps[m].ptr();
			for (int z = 0; z < rect.size.y; z++) {
				memcpy(w + z * rect.size.x * 4, src + int64_t(_index(rect.position.x, rect.position.y + z)) * 4, rect.size.x * 4);
			}
			maps.push_back(data);
		}
		region["weightmaps"] = maps;
	}
	return region;
}

void LandscapeData::set_region(const Dictionary &p_region) {
	ERR_FAIL_COND(!p_region.has("rect"));
	const Rect2i rect = p_region["rect"];
	ERR_FAIL_COND_MSG(clip_rect(rect) != rect, "Region is outside of the landscape bounds.");
	if (!rect.has_area()) {
		return;
	}
	int flags = 0;

	if (p_region.has("heights")) {
		const PackedFloat32Array data = p_region["heights"];
		ERR_FAIL_COND(data.size() != rect.size.x * rect.size.y);
		float *h = heights.ptrw();
		const float *r = data.ptr();
		for (int z = 0; z < rect.size.y; z++) {
			memcpy(h + _index(rect.position.x, rect.position.y + z), r + z * rect.size.x, sizeof(float) * rect.size.x);
		}
		flags |= CHANGED_HEIGHTS;
		height_range_dirty = true;
	}

	if (p_region.has("weightmaps")) {
		const Array maps = p_region["weightmaps"];
		ensure_layer_capacity(maps.size() * 4);
		for (int m = 0; m < MIN(maps.size(), weightmap_count); m++) {
			const PackedByteArray data = maps[m];
			ERR_CONTINUE(data.size() != rect.size.x * rect.size.y * 4);
			uint8_t *w = weightmaps[m].ptrw();
			const uint8_t *r = data.ptr();
			for (int z = 0; z < rect.size.y; z++) {
				memcpy(w + int64_t(_index(rect.position.x, rect.position.y + z)) * 4, r + z * rect.size.x * 4, rect.size.x * 4);
			}
		}
		flags |= CHANGED_WEIGHTS;
	}

	if (flags) {
		notify_region_changed(rect, flags);
	}
}

Ref<Image> LandscapeData::get_heightmap_image() const {
	ERR_FAIL_COND_V(!is_valid(), Ref<Image>());
	Vector<uint8_t> bytes;
	bytes.resize(heights.size() * sizeof(float));
	memcpy(bytes.ptrw(), heights.ptr(), bytes.size());
	return Image::create_from_data(size.x, size.y, false, Image::FORMAT_RF, bytes);
}

void LandscapeData::set_heightmap_image(const Ref<Image> &p_image, float p_scale, float p_offset) {
	ERR_FAIL_COND(p_image.is_null() || p_image->is_empty());
	ERR_FAIL_COND(!is_valid());

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

	const Vector<uint8_t> data = img->get_data();
	const float *src = reinterpret_cast<const float *>(data.ptr());
	float *h = heights.ptrw();
	for (int64_t i = 0; i < heights.size(); i++) {
		h[i] = src[i] * p_scale + p_offset;
	}
	height_range_dirty = true;
	notify_region_changed(Rect2i(Point2i(), size), CHANGED_HEIGHTS);
}

Ref<Image> LandscapeData::get_weightmap_image(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, weightmap_count, Ref<Image>());
	return Image::create_from_data(size.x, size.y, false, Image::FORMAT_RGBA8, weightmaps[p_index]);
}

void LandscapeData::set_weightmap_image(int p_index, const Ref<Image> &p_image) {
	ERR_FAIL_INDEX(p_index, MAX_WEIGHTMAPS);
	ERR_FAIL_COND(p_image.is_null() || p_image->is_empty());
	ERR_FAIL_COND(!is_valid());
	if (p_index >= weightmap_count) {
		set_weightmap_count(p_index + 1);
	}
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
	weightmaps[p_index] = img->get_data();
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

	// 16-bit linear grayscale keeps the full precision of the source heightmap.
	png_img.format = PNG_FORMAT_LINEAR_Y;
	const int width = png_img.width;
	const int height = png_img.height;
	Vector<uint16_t> pixels;
	pixels.resize(int64_t(width) * height);
	if (!png_image_finish_read(&png_img, nullptr, pixels.ptrw(), 0, nullptr)) {
		png_image_free(&png_img);
		ERR_FAIL_V_MSG(Ref<Image>(), vformat("Failed to decode PNG file '%s': %s.", p_path, png_img.message));
	}

	Vector<uint8_t> data;
	data.resize(pixels.size() * sizeof(float));
	float *dst = reinterpret_cast<float *>(data.ptrw());
	const uint16_t *src = pixels.ptr();
	for (int64_t i = 0; i < pixels.size(); i++) {
		dst[i] = src[i] / 65535.0f;
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
		} else if (new_size != size) {
			// Recreate while keeping the vertex spacing and the painted layers (resampled).
			Vector<Ref<Image>> weight_images;
			for (int i = 0; i < weightmap_count; i++) {
				Ref<Image> wimg = get_weightmap_image(i);
				wimg->resize(new_size.x, new_size.y, Image::INTERPOLATE_BILINEAR);
				weight_images.push_back(wimg);
			}
			size = new_size;
			heights.resize(int64_t(size.x) * size.y);
			for (int i = 0; i < weight_images.size(); i++) {
				weightmaps[i] = weight_images[i]->get_data();
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

	if (ext == "exr") {
		return get_heightmap_image()->save_exr(p_path, true);
	}

	// Integer formats are normalized to the current height range.
	const Vector2 range = get_height_range();
	const float scale = range.y > range.x ? 65535.0f / (range.y - range.x) : 0.0f;
	Vector<uint16_t> data;
	data.resize(heights.size());
	uint16_t *w = data.ptrw();
	const float *h = heights.ptr();
	for (int64_t i = 0; i < heights.size(); i++) {
		w[i] = uint16_t(CLAMP(Math::round((h[i] - range.x) * scale), 0.0f, 65535.0f));
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

void LandscapeData::notify_region_changed(const Rect2i &p_rect, int p_flags) {
	const Rect2i rect = clip_rect(p_rect);
	if (!rect.has_area()) {
		return;
	}
	if (p_flags & CHANGED_HEIGHTS) {
		height_range_dirty = true;
	}
	emit_signal(SNAME("region_changed"), rect, p_flags);
}

void LandscapeData::_set_heights(const PackedFloat32Array &p_heights) {
	heights = p_heights;
	height_range_dirty = true;
}

PackedFloat32Array LandscapeData::_get_heights() const {
	return heights;
}

void LandscapeData::_set_weightmaps(const Array &p_weightmaps) {
	weightmap_count = MIN(p_weightmaps.size(), MAX_WEIGHTMAPS);
	for (int i = 0; i < MAX_WEIGHTMAPS; i++) {
		if (i < weightmap_count) {
			weightmaps[i] = PackedByteArray(p_weightmaps[i]);
		} else {
			weightmaps[i].clear();
		}
	}
}

Array LandscapeData::_get_weightmaps() const {
	Array maps;
	for (int i = 0; i < weightmap_count; i++) {
		maps.push_back(PackedByteArray(weightmaps[i]));
	}
	return maps;
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

	ClassDB::bind_method(D_METHOD("notify_region_changed", "rect", "flags"), &LandscapeData::notify_region_changed);

	ClassDB::bind_method(D_METHOD("_set_heights", "heights"), &LandscapeData::_set_heights);
	ClassDB::bind_method(D_METHOD("_get_heights"), &LandscapeData::_get_heights);
	ClassDB::bind_method(D_METHOD("_set_weightmaps", "weightmaps"), &LandscapeData::_set_weightmaps);
	ClassDB::bind_method(D_METHOD("_get_weightmaps"), &LandscapeData::_get_weightmaps);

	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2I, "size", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY), "_set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "vertex_spacing", PROPERTY_HINT_RANGE, "0.01,100,0.001,or_greater,suffix:m"), "set_vertex_spacing", "get_vertex_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "heights", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "_set_heights", "_get_heights");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "weightmaps", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "_set_weightmaps", "_get_weightmaps");

	ADD_SIGNAL(MethodInfo("region_changed", PropertyInfo(Variant::RECT2I, "rect"), PropertyInfo(Variant::INT, "flags")));

	BIND_ENUM_CONSTANT(CHANGED_HEIGHTS);
	BIND_ENUM_CONSTANT(CHANGED_WEIGHTS);
	BIND_ENUM_CONSTANT(CHANGED_ALL);

	BIND_CONSTANT(MAX_RESOLUTION);
	BIND_CONSTANT(MAX_LAYERS);
}

LandscapeData::LandscapeData() {
}
