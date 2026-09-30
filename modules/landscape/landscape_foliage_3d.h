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

#include "landscape_foliage_data.h"
#include "landscape_foliage_type.h"

#include "core/math/random_pcg.h"
#include "core/templates/hash_map.h"
#include "core/variant/typed_array.h"
#include "scene/3d/node_3d.h"

class Camera3D;
class LandscapeFoliageGPU;
class Shader;
class ShaderMaterial;
class StandardMaterial3D;

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
// Instances follow the terrain when it is sculpted (`follow_terrain`). The debug views color the
// instances by level of detail, cell or cell state and draw the bounds of the drawn cells.
//
// The renderer culls the cells by the view frustum and occlusion (HZB) of every camera. While the
// LOD of the landscape is frozen, the cells out of the view of its camera are hidden too (only
// their shadows are drawn), like the patches of the landscape.
//
// Optionally (`gpu_indirect`, Forward+ and Mobile), the instances of the loaded cells are culled
// (view frustum and occlusion buffer of every camera, per instance) and sorted per level of detail
// on the GPU, straight into indirect MultiMeshes (see LandscapeFoliageGPU). Instances saved in a LandscapeFoliageData file (`data`, .lfdata) are
// streamed: only the cells within the cull distance of the camera and of the streaming sources are
// loaded (in the background), the others are released within a memory budget.
class LandscapeFoliage3D : public Node3D {
	GDCLASS(LandscapeFoliage3D, Node3D);

public:
	enum DebugView {
		DEBUG_VIEW_DISABLED,
		DEBUG_VIEW_LOD_LEVELS, // Color of the level of detail of the instances.
		DEBUG_VIEW_CELLS, // Color per cell (chunk).
		DEBUG_VIEW_CELL_STATE, // Cells drawn as a whole or sorted per instance.
	};

	// One instance, in the space of the landscape (see LandscapeFoliageInstance).
	typedef LandscapeFoliageInstance Instance;
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
		Vector2i key;
		LocalVector<Instance> instances;
		AABB bounds; // Landscape space, including the meshes.
		bool bounds_dirty = true;
		// Streaming (`data`): cells that aren't loaded only know their instance count and bounds.
		bool loaded = true;
		bool data_dirty = false; // Modified since written to the data.
		uint32_t stored_count = 0;
		AABB stored_bounds; // Positions of the instances.
		bool requested = false;
		float request_priority = 0.0;
		bool gpu_dirty = true; // Modified since uploaded to the GPU (gpu_indirect).
		// Rendering.
		Batch batches[LandscapeFoliageType::MAX_LODS];
		int state = STATE_NONE;
		bool render_dirty = true;
		Vector3 sort_camera; // Camera position of the last per-instance sort.
		LocalVector<uint8_t> instance_lods; // Level of each instance (per-instance sort).
		bool out_of_view = false; // Out of the frozen view of the LOD camera: only shadows are drawn.
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
		bool data_dirty = false; // A cell is modified since written to the data.
		// GPU indirect rendering: the instances of the cells around the camera, one slot per cell
		// in the instance buffer of the GPU (the rest of a slot is holes).
		struct GPUOutput {
			RID multimesh;
			RID instance;
			uint32_t capacity = 0;
			int surfaces = 0;
			AABB bounds;
			// Most instances received since window_start (shrinks the capacity when mostly unused).
			uint32_t window_peak = 0;
			uint64_t window_start = 0;
		};
		struct GPUSlot {
			uint32_t offset = 0;
			uint32_t capacity = 0;
		};
		uint64_t gpu_id = 0;
		GPUOutput gpu_outputs[2][LandscapeFoliageType::MAX_LODS]; // Main (visible), shadow.
		HashMap<Vector2i, GPUSlot> gpu_slots;
		LocalVector<GPUSlot> gpu_free; // Released slots.
		uint32_t gpu_capacity = 0; // Instance buffer.
		uint32_t gpu_used = 0; // End of the last slot.
		uint32_t gpu_count = 0; // Instances in the slots.
		AABB gpu_bounds; // Of the cells with a slot (shadow lists).
		AABB gpu_view_bounds; // Of every cell, only growing (main lists, stable for the HZB of the renderer).
		bool gpu_dirty = true; // A cell changed or the camera moved: slots to update.
		bool gpu_outputs_dirty = true;
		bool gpu_run = true; // Culling to run.
		Vector3 gpu_slots_camera;
		uint64_t gpu_outputs_serial = 0; // Last change of the outputs (the statistics of older ones are ignored).
		uint32_t gpu_dropped = 0; // Instances beyond the capacity of the outputs (last statistics).
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

	// Streaming: the cells of the data within the cull distance of the camera and of the
	// streaming sources are loaded (nearest first), the others are released (hysteresis, budget).
	struct CellRequest {
		Entry *entry = nullptr;
		Vector2i key;
	};
	Ref<LandscapeFoliageData> data;
	LocalVector<CellRequest> requests; // Cells being loaded.
	bool streaming_dirty = true;
	LocalVector<Vector3> stream_centers; // Landscape space, range scale in the order of the sources.
	LocalVector<real_t> stream_scales;
	int64_t loaded_bytes = 0; // Loaded instances, reported to the "Foliage CPU" pool.
	int loaded_cells = 0;
	bool data_dirty = false; // Cells to write to the data.
	bool _is_streamed() const;
	void _drop_cells(Entry *p_entry);
	void _populate_entry(Entry *p_entry, int p_index);
	void _reload_from_data();
	void _adopt_loaded_cells();
	void _detach_data();
	int _get_entry_index(const Entry *p_entry) const;
	void _load_cell(Entry *p_entry, Cell &r_cell);
	void _install_cell(Entry *p_entry, Cell &r_cell, LocalVector<Instance> &r_instances);
	void _evict_cell(Entry *p_entry, Cell &r_cell);
	void _cancel_requests(const Entry *p_entry = nullptr);
	void _ensure_all_loaded(Entry *p_entry);
	void _flush_data();
	void _mark_data_edited();
	void _process_streaming();
	void _add_loaded_bytes(int64_t p_bytes);

	// The view of the camera of the LOD of the landscape (landscape space), frozen with its LOD.
	struct View {
		bool valid = false;
		Vector3 position;
		bool frustum = false; // The planes are valid.
		Plane planes[6]; // Normals point outside.
	};
	View view;
	void _update_view();
	bool _is_in_view(const AABB &p_bounds) const;

	// GPU indirect rendering (culling and LOD selection on the GPU, see LandscapeFoliageGPU).
	bool gpu_indirect = false;
	bool gpu_frustum_culling = true;
	bool gpu_occlusion_culling = true;
	int gpu_max_instances = 262144;
	LandscapeFoliageGPU *gpu = nullptr;
	uint64_t next_gpu_id = 1;
	uint64_t next_gpu_serial = 1;
	bool gpu_lod_valid = false; // LOD camera of the last levels chosen on the GPU.
	Vector3 gpu_lod_camera;
	struct GPUViewSettings {
		RID scenario;
		RID lod_viewport;
		bool frozen = false;
		Transform3D space;
		uint32_t render_layers = 0;
		uint32_t flags = 0;
		bool operator==(const GPUViewSettings &p_other) const { return scenario == p_other.scenario && lod_viewport == p_other.lod_viewport && frozen == p_other.frozen && space == p_other.space && render_layers == p_other.render_layers && flags == p_other.flags; }
	};
	GPUViewSettings gpu_view_settings; // Last sent to the GPU.
	bool gpu_view_settings_sent = false;
	bool _is_gpu_active() const;
	void _send_gpu_outputs(Entry *p_entry);
	void _update_gpu_view_settings(bool p_visible);
	void _free_gpu_outputs(Entry *p_entry);
	void _free_gpu(Entry *p_entry);
	void _gpu_call(const Callable &p_callable);
	void _update_gpu_output_settings(Entry *p_entry, int p_list, int p_lod);
	void _update_all_gpu_output_settings();
	void _release_gpu_slot(Entry *p_entry, const Vector2i &p_key);
	bool _allocate_gpu_slot(Entry *p_entry, uint32_t p_capacity, uint32_t &r_offset);
	void _write_gpu_slot(const Cell &p_cell, const Entry::GPUSlot &p_slot, float *r_buffer);
	void _update_gpu_slots(Entry *p_entry, const Vector3 &p_camera, bool p_has_camera);
	void _update_gpu_outputs(Entry *p_entry);
	void _update_gpu(const Vector3 &p_camera, bool p_has_camera);
	void _frame_pre_draw();
	Camera3D *_get_view_camera() const;

	// Rendering state.
	bool render_pending = true;
	bool view_culling = false; // The cells out of the frozen view are hidden (out_of_view).
	void _update_view_culling();
	bool has_last_camera = false;
	Vector3 last_camera;
	uint64_t last_update_usec = 0;
	int last_updated_cells = 0;

	// Debug views.
	DebugView debug_view = DEBUG_VIEW_DISABLED;
	RID debug_bounds_mesh;
	RID debug_bounds_instance;
	bool debug_bounds_dirty = false;
	int debug_bounds_count = 0;
	static Ref<Shader> debug_shader;
	static Ref<ShaderMaterial> debug_material;
	static Ref<StandardMaterial3D> debug_bounds_material;
	static RID _get_debug_material();
	Color _get_debug_cell_color(const Cell &p_cell, int p_lod) const;
	void _update_debug_colors(const Cell &p_cell);
	void _update_debug_bounds();
	void _free_debug_bounds();

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
	void _update_batch_settings(Entry *p_entry, const Cell &p_cell, int p_lod, Batch &r_batch);
	void _update_all_batch_settings();
	int _get_uniform_state(const Entry *p_entry, real_t p_min_distance, real_t p_max_distance) const;
	uint8_t _get_instance_lod(const Entry *p_entry, const Instance &p_instance, real_t p_distance, uint8_t p_current) const;
	bool _update_cell(Entry *p_entry, Cell &r_cell, int p_state, const Vector3 &p_camera);
	void _update_rendering();

	// Serialization.
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
	void set_gpu_indirect(bool p_enable);
	bool is_gpu_indirect() const { return gpu_indirect; }
	void set_gpu_frustum_culling(bool p_enable);
	bool is_gpu_frustum_culling() const { return gpu_frustum_culling; }
	void set_gpu_occlusion_culling(bool p_enable);
	bool is_gpu_occlusion_culling() const { return gpu_occlusion_culling; }
	void set_gpu_max_instances(int p_count);
	int get_gpu_max_instances() const { return gpu_max_instances; }
	bool is_gpu_indirect_active() const { return _is_gpu_active(); }
	static bool is_gpu_indirect_supported();

	// Streaming.
	void set_data(const Ref<LandscapeFoliageData> &p_data);
	Ref<LandscapeFoliageData> get_data() const { return data; }
	// Saves every instance to a new .lfdata file and streams them from it.
	Error save_to_data_file(const String &p_path);
	// Loads every cell of the streamed data (e.g. before processing all instances).
	void load_all_cells();

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

	// Debug.
	void set_debug_view(DebugView p_view);
	DebugView get_debug_view() const { return debug_view; }
	// Colors of the debug views (the same as the debug views of Landscape3D for the same index).
	static Color get_debug_color(int p_index);
	static Color get_debug_state_color(bool p_sorted_per_instance);
	static Color get_debug_gpu_color();

	Dictionary get_statistics() const;
	void force_update();
	PackedStringArray get_configuration_warnings() const override;

	static void cleanup_shared_resources();

	LandscapeFoliage3D();
	~LandscapeFoliage3D();
};

VARIANT_ENUM_CAST(LandscapeFoliage3D::DebugView);
