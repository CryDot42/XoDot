/**************************************************************************/
/*  landscape_data.h                                                      */
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

#pragma once

#include "core/io/image.h"
#include "core/io/resource.h"
#include "core/math/rect2i.h"
#include "core/math/vector2i.h"
#include "core/variant/typed_array.h"

// Terrain source data: a regular grid of heights (in meters) plus up to
// 16 weight-blended material layers stored as four RGBA8 weightmaps.
//
// Texel (x, z) maps to the local position (x * vertex_spacing, height, z * vertex_spacing)
// of the owning Landscape3D node, so the landscape covers
// (size - 1) * vertex_spacing meters along each axis.
class LandscapeData : public Resource {
	GDCLASS(LandscapeData, Resource);

public:
	static constexpr int MIN_RESOLUTION = 2;
	static constexpr int MAX_RESOLUTION = 16384;
	static constexpr int MAX_LAYERS = 16;
	static constexpr int MAX_WEIGHTMAPS = MAX_LAYERS / 4;

	enum ChangeFlags {
		CHANGED_HEIGHTS = 1,
		CHANGED_WEIGHTS = 2,
		CHANGED_ALL = CHANGED_HEIGHTS | CHANGED_WEIGHTS,
	};

private:
	Vector2i size;
	real_t vertex_spacing = 1.0;

	Vector<float> heights;
	Vector<uint8_t> weightmaps[MAX_WEIGHTMAPS];
	int weightmap_count = 0;

	mutable bool height_range_dirty = true;
	mutable Vector2 height_range;

	void _allocate_weightmap(int p_index);
	_FORCE_INLINE_ int _index(int p_x, int p_z) const { return p_z * size.x + p_x; }

	// Serialization helpers.
	void _set_size(const Vector2i &p_size);
	void _set_heights(const PackedFloat32Array &p_heights);
	PackedFloat32Array _get_heights() const;
	void _set_weightmaps(const Array &p_weightmaps);
	Array _get_weightmaps() const;

	static Ref<Image> _load_png16(const String &p_path);
	static Ref<Image> _load_raw16(const String &p_path);
	static Error _save_png16(const String &p_path, const Vector<uint16_t> &p_data, int p_width, int p_height);

protected:
	static void _bind_methods();

public:
	// Creation.
	void create(const Vector2i &p_size, real_t p_vertex_spacing = 1.0, float p_height = 0.0);
	void resize(const Vector2i &p_size);
	bool is_valid() const { return size.x >= MIN_RESOLUTION && size.y >= MIN_RESOLUTION && heights.size() == size.x * size.y; }

	Vector2i get_size() const { return size; }
	void set_vertex_spacing(real_t p_spacing);
	real_t get_vertex_spacing() const { return vertex_spacing; }
	Vector2 get_world_size() const;

	// Heights.
	_FORCE_INLINE_ float get_height_fast(int p_x, int p_z) const { return heights.ptr()[_index(p_x, p_z)]; }
	float get_height(int p_x, int p_z) const;
	void set_height(int p_x, int p_z, float p_height);
	float sample_height(real_t p_local_x, real_t p_local_z) const;
	Vector3 sample_normal(real_t p_local_x, real_t p_local_z) const;
	Vector2 get_height_range() const;

	const float *get_heights_ptr() const { return heights.ptr(); }
	float *get_heights_ptrw() { return heights.ptrw(); }
	const Vector<float> &get_heights_vector() const { return heights; }

	// Weights (layer painting).
	void set_weightmap_count(int p_count);
	int get_weightmap_count() const { return weightmap_count; }
	void ensure_layer_capacity(int p_layer_count);
	int get_layer_capacity() const { return weightmap_count * 4; }

	float get_layer_weight(int p_x, int p_z, int p_layer) const;
	void set_layer_weight(int p_x, int p_z, int p_layer, float p_weight);
	void get_weights(int p_x, int p_z, float *r_weights) const;
	void set_weights(int p_x, int p_z, const float *p_weights);
	int get_dominant_layer(int p_x, int p_z) const;
	void fill_layer(int p_layer);
	void remove_layer(int p_layer);

	const uint8_t *get_weightmap_ptr(int p_index) const;
	uint8_t *get_weightmap_ptrw(int p_index);
	const Vector<uint8_t> &get_weightmap_vector(int p_index) const;

	// Region snapshots (used by undo/redo and runtime scripts).
	Dictionary get_region(const Rect2i &p_rect, bool p_heights = true, bool p_weights = true) const;
	void set_region(const Dictionary &p_region);

	// Images.
	Ref<Image> get_heightmap_image() const;
	void set_heightmap_image(const Ref<Image> &p_image, float p_scale = 1.0, float p_offset = 0.0);
	Ref<Image> get_weightmap_image(int p_index) const;
	void set_weightmap_image(int p_index, const Ref<Image> &p_image);

	Error import_heightmap(const String &p_path, float p_scale = 1.0, float p_offset = 0.0, bool p_resize = true);
	Error export_heightmap(const String &p_path) const;

	// Change notification. Emits `region_changed`.
	void notify_region_changed(const Rect2i &p_rect, int p_flags);
	Rect2i clip_rect(const Rect2i &p_rect) const;

	LandscapeData();
};

VARIANT_ENUM_CAST(LandscapeData::ChangeFlags);
