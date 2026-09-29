/**************************************************************************/
/*  landscape_foliage_3d.h                                                */
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

#include "landscape_foliage_type.h"

#include "core/math/random_pcg.h"
#include "core/templates/hash_map.h"
#include "core/variant/typed_array.h"
#include "scene/3d/node_3d.h"

class Landscape3D;

// Foliage painted on a Landscape3D (UE-like foliage): trees, bushes, grass, rocks...
//
// Instances of each LandscapeFoliageType are stored in the space of the parent landscape and
// sorted into a grid of square cells (chunks, `chunk_size`):
// - each cell is drawn with one MultiMesh per level of detail, with its own bounds, so the
//   cells are frustum and occlusion culled independently,
// - the level of detail of each instance is chosen on the CPU from its distance to the camera:
//   cells entirely within one LOD band are drawn with that level as a whole and only updated
//   when they leave the band, cells crossing a LOD or cull distance are sorted per instance,
//   with hysteresis (`lod_transition`), and only uploaded when an instance changes its level,
// - cells beyond the cull distance have no rendering resources, the updates are limited to a
//   time budget per frame (nearest cells first).
// Instances follow the terrain when it is sculpted (`follow_terrain`).
class LandscapeFoliage3D : public Node3D {
	GDCLASS(LandscapeFoliage3D, Node3D);

public:
	// One instance, in the space of the landscape. The transform is stored in the layout of
	// MultiMesh buffers (3 x 4, row-major), so that it can be copied as is.
	struct Instance {
		float xform[12];
		float offset; // Vertical offset from the ground.
		float random; // Per-instance random value in [0, 1), INSTANCE_CUSTOM.x in shaders.
		float normal_x; // Up vector of the alignment (its Y is positive).
		float normal_z;

		Vector3 get_position() const { return Vector3(xform[3], xform[7], xform[11]); }
		Transform3D get_transform() const;
		void set_transform(const Transform3D &p_transform);
		Vector3 get_align_normal() const;
		void set_align_normal(const Vector3 &p_normal);
	};
	static constexpr int INSTANCE_FLOATS = sizeof(Instance) / sizeof(float);
	static_assert(INSTANCE_FLOATS == 16);

	// Cells updated per frame and time budget of the rendering updates.
	static constexpr int MIN_CELL_UPDATES_PER_FRAME = 8;
	static constexpr uint64_t CELL_UPDATE_BUDGET_USEC = 3000;

private:
	enum CellState {
		STATE_NONE = -1, // Not evaluated (or instances changed).
		STATE_CULLED = -2, // Beyond the cull distance: no rendering resources.
		STATE_MIXED = -3, // Crosses a LOD or cull distance: LOD per instance.
		// >= 0: all instances use this level of detail.
	};
	static constexpr uint8_t LOD_CULLED = 255;

	struct Batch {
		RID multimesh;
		RID instance;
		int count = 0;
	};

	struct Cell {
		LocalVector<Instance> instances;
		AABB bounds; // Landscape space, including the meshes.
		bool bounds_dirty = true;
		// Rendering.
		Batch batches[LandscapeFoliageType::MAX_LODS];
		int state = STATE_NONE;
		bool render_dirty = true;
		Vector3 sort_camera; // Camera position of the last per-instance sort.
		LocalVector<uint8_t> instance_lods; // Level of each instance (per-instance sort).
	};

	struct Entry {
		Ref<LandscapeFoliageType> type;
		HashMap<Vector2i, Cell> cells;
		int64_t count = 0;
		uint64_t render_hash = 0;
		AABB mesh_bounds;
		// Levels of detail in use (distances scaled by lod_distance_scale).
		int lod_count = 0;
		float lod_starts[LandscapeFoliageType::MAX_LODS] = {};
		RID lod_meshes[LandscapeFoliageType::MAX_LODS];
		bool lod_shadows[LandscapeFoliageType::MAX_LODS] = {};
		float cull_end = 0.0; // 0: never culled.
		float cull_begin = 0.0; // Closest cull distance of an instance (cull_random).
		float transition = 0.0;
		// Serialized instances (cache).
		PackedByteArray serialized;
		bool serialized_dirty = true;
	};
	LocalVector<Entry *> entries;

	// Settings.
	float chunk_size = 32.0;
	float lod_distance_scale = 1.0;
	bool follow_terrain = true;
	uint32_t render_layers = 1;
	bool cast_shadows = true;

	Landscape3D *landscape = nullptr;
	RandomPCG rng;

	// Rendering state.
	bool render_pending = true;
	bool has_last_camera = false;
	Vector3 last_camera;
	uint64_t last_update_usec = 0;
	int last_updated_cells = 0;

	// Terrain following.
	LocalVector<Rect2> snap_rects; // Landscape space.

	Entry *_get_entry(int p_type_index) const;
	void _connect_type(Entry *p_entry, bool p_connect);
	void _types_changed();
	void _update_entry_lods(Entry *p_entry);
	Vector2i _get_cell_key(const Vector3 &p_position) const;
	Rect2 _get_cell_rect(const Vector2i &p_key) const;
	Cell &_get_or_create_cell(Entry *p_entry, const Vector2i &p_key);
	void _cell_changed(Entry *p_entry, Cell &r_cell);
	void _add_instance(Entry *p_entry, const Instance &p_instance);
	void _update_cell_bounds(Entry *p_entry, Cell &r_cell);
	template <typename F>
	void _for_each_cell_in_rect(Entry *p_entry, const Rect2 &p_rect, F p_function);
	void _remove_empty_cells(Entry *p_entry);
	void _rebin_all();

	// Terrain.
	bool _has_terrain() const;
	bool _sample_ground(const Vector2 &p_position, real_t &r_height, Vector3 &r_normal, float *r_weights, bool &r_hole) const;
	bool _make_instance(const LandscapeFoliageType *p_type, const Vector2 &p_position, bool p_filter, Instance &r_instance);
	void _snap_instance(const LandscapeFoliageType *p_type, Instance &r_instance) const;
	void _snap_rect(const Rect2 &p_rect);
	void _process_snapping();
	int _random_round(real_t p_value);

	// Rendering.
	Transform3D _get_space_transform() const;
	bool _get_camera(Vector3 &r_position) const;
	void _free_batch(Batch &r_batch);
	void _free_cell_rendering(Cell &r_cell);
	void _free_all_rendering();
	void _set_batch(Entry *p_entry, Cell &r_cell, int p_lod, const LocalVector<uint32_t> *p_indices);
	void _update_batch_settings(Entry *p_entry, int p_lod, Batch &r_batch);
	void _update_all_batch_settings();
	int _get_uniform_state(const Entry *p_entry, real_t p_min_distance, real_t p_max_distance) const;
	uint8_t _get_instance_lod(const Entry *p_entry, const Instance &p_instance, real_t p_distance, uint8_t p_current) const;
	bool _update_cell(Entry *p_entry, Cell &r_cell, int p_state, const Vector3 &p_camera);
	void _update_rendering();

	// Serialization.
	static PackedByteArray _encode_instances(const LocalVector<Instance> &p_instances);
	static bool _decode_instances(const PackedByteArray &p_data, LocalVector<Instance> &r_instances);
	void _set_foliage_types_bind(const TypedArray<LandscapeFoliageType> &p_types);
	TypedArray<LandscapeFoliageType> _get_foliage_types_bind() const;
	void _set_instance_data(const Array &p_data);
	Array _get_instance_data() const;

	Landscape3D *_find_landscape() const;

protected:
	void _notification(int p_what);
	bool _set(const StringName &p_name, const Variant &p_value);
	bool _get(const StringName &p_name, Variant &r_ret) const;
	void _get_property_list(List<PropertyInfo> *p_list) const;
	static void _bind_methods();

public:
	// Foliage types.
	int add_foliage_type(const Ref<LandscapeFoliageType> &p_type);
	void remove_foliage_type(int p_index);
	void set_foliage_type(int p_index, const Ref<LandscapeFoliageType> &p_type); // Keeps the instances.
	Ref<LandscapeFoliageType> get_foliage_type(int p_index) const;
	int get_foliage_type_count() const { return entries.size(); }
	int find_foliage_type(const Ref<LandscapeFoliageType> &p_type) const;
	void move_foliage_type(int p_from, int p_to);

	// Settings.
	void set_chunk_size(float p_size);
	float get_chunk_size() const { return chunk_size; }
	void set_lod_distance_scale(float p_scale);
	float get_lod_distance_scale() const { return lod_distance_scale; }
	void set_follow_terrain(bool p_enable);
	bool is_following_terrain() const { return follow_terrain; }
	void set_render_layers(uint32_t p_layers);
	uint32_t get_render_layers() const { return render_layers; }
	void set_cast_shadows(bool p_enable);
	bool is_casting_shadows() const { return cast_shadows; }

	// Painting (UE-like tools). Centers are global positions, radii are in meters.
	// Adds instances until the density of the type (scaled) is reached in the circle.
	int paint(int p_type_index, const Vector3 &p_global_center, real_t p_radius, real_t p_density_scale = 1.0);
	// Removes instances until the density of the type (scaled) is left in the circle
	// (all of them with 0). A negative type index erases every type.
	int erase(int p_type_index, const Vector3 &p_global_center, real_t p_radius, real_t p_density_scale = 0.0);
	// Places one instance on the ground below the position (placement settings, no filters).
	bool place_instance(int p_type_index, const Vector3 &p_global_position);
	// Places the type everywhere it grows, at its density, in a rect of the landscape space
	// (the whole landscape when empty). Returns the number of new instances.
	int fill(int p_type_index, const Rect2 &p_rect = Rect2());
	// Applies the current placement settings and filters of the type to its instances in the circle.
	int reapply(int p_type_index, const Vector3 &p_global_center, real_t p_radius);
	void clear(int p_type_index = -1);
	// Puts the instances in a rect of the landscape space (all when empty) back on the terrain.
	void snap_to_terrain(const Rect2 &p_rect = Rect2());
	// Number of instances the type would have after fill() on the whole landscape.
	int64_t estimate_fill_count(int p_type_index) const;

	// Instances.
	int64_t get_instance_count(int p_type_index = -1) const;
	bool add_instance(int p_type_index, const Transform3D &p_global_transform);
	void add_instances(int p_type_index, const TypedArray<Transform3D> &p_global_transforms);
	TypedArray<Transform3D> get_instance_transforms(int p_type_index, const AABB &p_global_aabb = AABB()) const;
	TypedArray<Transform3D> get_instances_in_radius(int p_type_index, const Vector3 &p_global_center, real_t p_radius) const;
	int remove_instances_in_radius(int p_type_index, const Vector3 &p_global_center, real_t p_radius);

	// Raw instances (landscape space, INSTANCE_FLOATS per instance) in a rect: undo/redo and tools.
	PackedFloat32Array get_instances_in_rect(int p_type_index, const Rect2 &p_rect) const;
	void set_instances_in_rect(int p_type_index, const Rect2 &p_rect, const PackedFloat32Array &p_instances);
	PackedByteArray get_type_data(int p_type_index) const;
	void set_type_data(int p_type_index, const PackedByteArray &p_data);
	// Rects of the cells (chunks) touching a rect of the landscape space.
	Vector<Rect2> get_cell_rects(const Rect2 &p_rect) const;

	// Landscape.
	Landscape3D *get_landscape() const { return landscape; }
	Vector3 global_to_landscape(const Vector3 &p_global) const;
	void _terrain_region_changed(const Rect2i &p_texel_rect, int p_flags);

	Dictionary get_statistics() const;
	void force_update();
	PackedStringArray get_configuration_warnings() const override;

	LandscapeFoliage3D();
	~LandscapeFoliage3D();
};
