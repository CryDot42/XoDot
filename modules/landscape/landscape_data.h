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

#include "landscape_lod_tree.h"
#include "landscape_storage.h"

#include "core/io/image.h"
#include "core/io/resource.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/math/rect2i.h"
#include "core/math/vector2i.h"
#include "core/templates/hash_map.h"
#include "core/variant/typed_array.h"

// Terrain source data: a regular grid of heights (in meters) plus up to
// 16 weight-blended material layers stored as four RGBA8 weightmaps, and an optional hole mask.
//
// Texel (x, z) maps to the local position (x * vertex_spacing, height, z * vertex_spacing)
// of the owning Landscape3D node, so the landscape covers
// (size - 1) * vertex_spacing meters along each axis.
//
// The data lives in a tiled LandscapeStorage. Saved as a .lsdata file, it is streamed:
// tiles are only loaded when they are needed (rendering, collision, queries, editing).
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
		CHANGED_HOLES = 4,
		CHANGED_ALL = CHANGED_HEIGHTS | CHANGED_WEIGHTS | CHANGED_HOLES,
	};

private:
	real_t vertex_spacing = 1.0;
	mutable LandscapeStorage storage;

	mutable HashMap<int, LandscapeLodTree *> lod_trees;
	Dictionary saved_lod_trees; // Patch size -> nodes, as loaded from the data file.
	void _clear_lod_trees();

	// Serialization helpers (embedded resources, .lsdata files are handled by ResourceFormatLandscapeData).
	void _set_size(const Vector2i &p_size);
	void _set_heights(const PackedFloat32Array &p_heights);
	PackedFloat32Array _get_heights() const;
	void _set_weightmaps(const Array &p_weightmaps);
	Array _get_weightmaps() const;
	void _set_holes(const PackedByteArray &p_holes);
	PackedByteArray _get_holes() const;

	void _write_image_rows(int p_layer, const Vector<uint8_t> &p_data, int p_texel_size);

	static Ref<Image> _load_png16(const String &p_path);
	static Ref<Image> _load_raw16(const String &p_path);
	static Error _save_png16(const String &p_path, const Vector<uint16_t> &p_data, int p_width, int p_height);

protected:
	static void _bind_methods();

public:
	// Creation.
	void create(const Vector2i &p_size, real_t p_vertex_spacing = 1.0, float p_height = 0.0);
	void resize(const Vector2i &p_size);
	bool is_valid() const { return storage.is_valid(); }

	Vector2i get_size() const { return storage.get_size(); }
	void set_vertex_spacing(real_t p_spacing);
	real_t get_vertex_spacing() const { return vertex_spacing; }
	Vector2 get_world_size() const;

	// Heights.
	float get_height(int p_x, int p_z) const;
	void set_height(int p_x, int p_z, float p_height);
	float sample_height(real_t p_local_x, real_t p_local_z) const;
	Vector3 sample_normal(real_t p_local_x, real_t p_local_z) const;
	Vector2 get_height_range() const;
	// Bulk access to mip 0 heights (the rect must be inside the landscape for writes).
	void read_heights(const Rect2i &p_rect, float *r_heights) const;
	void write_heights(const Rect2i &p_rect, const float *p_heights);

	// Weights (layer painting).
	void set_weightmap_count(int p_count);
	int get_weightmap_count() const { return storage.get_weightmap_count(); }
	void ensure_layer_capacity(int p_layer_count);
	int get_layer_capacity() const { return get_weightmap_count() * 4; }

	float get_layer_weight(int p_x, int p_z, int p_layer) const;
	void set_layer_weight(int p_x, int p_z, int p_layer, float p_weight);
	void get_weights(int p_x, int p_z, float *r_weights) const;
	void set_weights(int p_x, int p_z, const float *p_weights);
	int get_dominant_layer(int p_x, int p_z) const;
	void fill_layer(int p_layer);
	void remove_layer(int p_layer);

	// Holes (visibility), e.g. for caves.
	bool has_holes() const { return storage.get_has_holes(); }
	bool is_hole(int p_x, int p_z) const;
	void set_hole(int p_x, int p_z, bool p_hole);
	void clear_holes();

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
	Error import_layer_weights(int p_layer, const String &p_path);
	void set_layer_weights_image(int p_layer, const Ref<Image> &p_image);

	// Change notification. Updates the derived data (mip levels, LOD trees) and emits `region_changed`.
	void notify_region_changed(const Rect2i &p_rect, int p_flags);
	Rect2i clip_rect(const Rect2i &p_rect) const;

	// LOD tree for a patch size (built on first use, kept up to date and saved with the data).
	const LandscapeLodTree *get_lod_tree(int p_patch_quads) const;

	// Streaming.
	LandscapeStorage &get_storage() const { return storage; }
	Error save_to_file(const String &p_path);
	Error load_from_file(const String &p_path);
	bool is_streamed() const { return storage.is_file_backed(); }
	bool has_unsaved_changes() const { return storage.has_unsaved_changes(); }
	int64_t get_memory_usage() const { return storage.get_memory_usage(); }
	int get_loaded_tile_count() const { return storage.get_loaded_tile_count(); }
	void trim_memory() { storage.trim(); }

	LandscapeData();
	~LandscapeData();
};

VARIANT_ENUM_CAST(LandscapeData::ChangeFlags);

// Streamed landscape data files (.lsdata).
class ResourceFormatLoaderLandscapeData : public ResourceFormatLoader {
	GDSOFTCLASS(ResourceFormatLoaderLandscapeData, ResourceFormatLoader);

public:
	Ref<Resource> load(const String &p_path, const String &p_original_path = "", Error *r_error = nullptr, bool p_use_sub_threads = false, float *r_progress = nullptr, CacheMode p_cache_mode = CACHE_MODE_REUSE) override;
	void get_recognized_extensions(List<String> *p_extensions) const override;
	bool handles_type(const String &p_type) const override;
	String get_resource_type(const String &p_path) const override;
};

class ResourceFormatSaverLandscapeData : public ResourceFormatSaver {
	GDSOFTCLASS(ResourceFormatSaverLandscapeData, ResourceFormatSaver);

public:
	Error save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags = 0) override;
	bool recognize(const Ref<Resource> &p_resource) const override;
	void get_recognized_extensions(const Ref<Resource> &p_resource, List<String> *p_extensions) const override;
};
