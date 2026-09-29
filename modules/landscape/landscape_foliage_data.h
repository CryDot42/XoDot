/**************************************************************************/
/*  landscape_foliage_data.h                                              */
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

#include "core/io/resource.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/math/aabb.h"
#include "core/math/transform_3d.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"

#include "modules/streaming/world_streaming.h"

class LandscapeStorageFile;

// One foliage instance, in the space of the landscape. The transform is stored in the layout of
// MultiMesh buffers (3 x 4, row-major), so that it can be copied as is (CPU and GPU).
struct LandscapeFoliageInstance {
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

	// Compressed arrays of instances (byte planes of the floats, zstd).
	static PackedByteArray encode(const LandscapeFoliageInstance *p_instances, uint32_t p_count);
	static bool decode(const uint8_t *p_data, int64_t p_size, LocalVector<LandscapeFoliageInstance> &r_instances);
	static bool decode(const PackedByteArray &p_data, LocalVector<LandscapeFoliageInstance> &r_instances);
};

static_assert(sizeof(LandscapeFoliageInstance) == 16 * sizeof(float));

// Instances of a LandscapeFoliage3D, streamed from a .lfdata file.
//
// One layer per foliage type of the node (same order). Each layer is a sparse grid of square
// cells (chunk_size meters) in the landscape space. A .lfdata file holds an index (instance
// count and bounds of every cell) and the compressed instances of each cell: only the index is
// read when the file is loaded, the cells are loaded on demand by the node (asynchronously,
// through WorldStreaming jobs) around the camera and the streaming sources.
//
// Cells written by the node (edited) are kept in memory until the data is saved. Data that
// isn't saved to a .lfdata file (e.g. embedded in a scene) keeps every cell in memory.
class LandscapeFoliageData : public Resource {
	GDCLASS(LandscapeFoliageData, Resource);

public:
	typedef LandscapeFoliageInstance Instance;

	struct CellInfo {
		uint32_t count = 0;
		AABB bounds; // Positions of the instances.
		uint64_t offset = 0; // Compressed instances in the file.
		uint32_t size = 0;
		bool in_file = false;
	};

	class LoadJob : public StreamingJob {
		GDSOFTCLASS(LoadJob, StreamingJob);

	public:
		ObjectID owner;
		Ref<LandscapeStorageFile> file;
		uint64_t offset = 0;
		uint32_t size = 0;
		int layer = 0;
		Vector2i key;
		uint32_t generation = 0;

		LocalVector<Instance> result;
		bool ok = false;

	protected:
		void run() override;
		void finish() override;
	};

private:
	struct Layer {
		HashMap<Vector2i, CellInfo> cells;
		// Cells written since the data was last saved (every cell when there is no file).
		HashMap<Vector2i, LocalVector<Instance>> pending;
	};

	float chunk_size = 32.0;
	LocalVector<Layer> layers;
	Ref<LandscapeStorageFile> file;
	uint32_t generation = 0; // Invalidates the loads from a previous file.
	int64_t pending_bytes = 0;
	bool unsaved = false;

	// Asynchronous loads, per cell (x, z, layer).
	HashMap<Vector3i, Ref<LoadJob>> jobs;
	HashMap<Vector3i, LocalVector<Instance>> loaded; // Finished loads, until they are taken.
	int64_t loaded_bytes = 0;

	// Called before saving, so that the owners write their edited cells.
	LocalVector<Callable> flush_callbacks;

	Layer *_get_layer(int p_layer);
	void _update_memory(int64_t p_pending_delta, int64_t p_loaded_delta);
	void _cancel_jobs();
	void _finish_load(LoadJob *p_job);

	// Embedded serialization (the whole layers).
	void _set_layers_data(const Array &p_layers);
	Array _get_layers_data() const;

protected:
	static void _bind_methods();

public:
	// Layers (one per foliage type).
	void set_layer_count(int p_count);
	int get_layer_count() const { return layers.size(); }
	void insert_layer(int p_index);
	void remove_layer(int p_index);
	void move_layer(int p_from, int p_to);
	void clear_layer(int p_layer);

	// Size of the cells. Changing it drops the cells (the owner writes them again).
	void set_chunk_size(float p_size);
	float get_chunk_size() const { return chunk_size; }

	// Index.
	const HashMap<Vector2i, CellInfo> *get_cells(int p_layer) const;
	int64_t get_instance_count(int p_layer = -1) const;
	int get_cell_count(int p_layer = -1) const;

	// Reads the instances of a cell now (edited cells come from memory).
	bool read_cell(int p_layer, const Vector2i &p_key, LocalVector<Instance> &r_instances);
	// Starts or updates the background load of a cell. Returns true, with the instances, when
	// they are available (right away for edited cells, once loaded for the others).
	bool request_cell(int p_layer, const Vector2i &p_key, float p_priority, LocalVector<Instance> &r_instances);
	void cancel_request(int p_layer, const Vector2i &p_key);
	int get_loading_count() const { return jobs.size(); }
	void wait_for_loads();
	// Writes the instances of a cell (no instance removes it). Kept in memory until saved.
	void write_cell(int p_layer, const Vector2i &p_key, const Instance *p_instances, uint32_t p_count);

	// Files.
	Error save_to_file(const String &p_path);
	Error load_from_file(const String &p_path);
	bool is_streamed() const;
	bool has_unsaved_changes() const { return unsaved; }
	int64_t get_memory_usage() const { return pending_bytes + loaded_bytes; }

	void add_flush_callback(const Callable &p_callback);
	void remove_flush_callback(const Callable &p_callback);

	LandscapeFoliageData();
	~LandscapeFoliageData();
};

// Streamed foliage data files (.lfdata).
class ResourceFormatLoaderLandscapeFoliageData : public ResourceFormatLoader {
	GDSOFTCLASS(ResourceFormatLoaderLandscapeFoliageData, ResourceFormatLoader);

public:
	Ref<Resource> load(const String &p_path, const String &p_original_path = "", Error *r_error = nullptr, bool p_use_sub_threads = false, float *r_progress = nullptr, CacheMode p_cache_mode = CACHE_MODE_REUSE) override;
	void get_recognized_extensions(List<String> *p_extensions) const override;
	bool handles_type(const String &p_type) const override;
	String get_resource_type(const String &p_path) const override;
};

class ResourceFormatSaverLandscapeFoliageData : public ResourceFormatSaver {
	GDSOFTCLASS(ResourceFormatSaverLandscapeFoliageData, ResourceFormatSaver);

public:
	Error save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags = 0) override;
	bool recognize(const Ref<Resource> &p_resource) const override;
	void get_recognized_extensions(const Ref<Resource> &p_resource, List<String> *p_extensions) const override;
};
