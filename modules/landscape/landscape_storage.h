/**************************************************************************/
/*  landscape_storage.h                                                   */
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

#include "core/io/file_access.h"
#include "core/math/rect2i.h"
#include "core/math/vector2i.h"
#include "core/object/ref_counted.h"
#include "core/os/mutex.h"
#include "core/templates/local_vector.h"

#include "modules/streaming/world_streaming.h"

class LandscapeData;

// A file shared by the main thread and the streaming threads.
class LandscapeStorageFile : public RefCounted {
	GDSOFTCLASS(LandscapeStorageFile, RefCounted);

	Mutex mutex;
	Ref<FileAccess> file;
	String path;

public:
	Error open(const String &p_path, FileAccess::ModeFlags p_mode);
	void close();
	bool is_open() const { return file.is_valid(); }
	String get_path() const { return path; }

	Vector<uint8_t> read(uint64_t p_offset, uint32_t p_size);
	// Appends at the end of the file, returns the offset of the data.
	uint64_t append(const uint8_t *p_data, uint32_t p_size);

	~LandscapeStorageFile();
};

// Tiled, streamable storage of the landscape source data.
//
// Every data layer (heights, weightmaps and holes) is stored as a pyramid of mip levels,
// each mip level being split into tiles of TILE_SIZE x TILE_SIZE texels. Mip 0 is the
// editable source, the other levels are derived:
// - heights are point-sampled (mip m texel i is the mip 0 texel min(i << m, size - 1)),
//   so every mip level holds exact heights of the terrain vertices at that resolution,
// - weights and holes are filtered with a vertex centered [1 2 1] / 4 tent filter.
//
// Tiles are loaded on demand (synchronously or through WorldStreaming jobs) from a .lsdata
// file, and evicted in LRU order when the memory budget is exceeded. Modified tiles that
// must be evicted are spilled to a temporary overlay file until the data is saved.
// Tiles that were never written don't use any memory or disk space (constant defaults).
//
// Mip 0 tiles may also hold a "base" copy of their heights and weights: the terrain as sculpted
// and painted by the user, before the landscape splines (roads, rivers...) were composited into
// the final data. Only tiles under splines have a base, see LandscapeData.
class LandscapeStorage {
public:
	static constexpr int TILE_SHIFT = 7;
	static constexpr int TILE_SIZE = 1 << TILE_SHIFT;
	static constexpr int TILE_MASK = TILE_SIZE - 1;
	static constexpr int TILE_TEXELS = TILE_SIZE * TILE_SIZE;
	static constexpr int MAX_WEIGHTMAPS = 4;
	static constexpr int MIN_DOMAIN = 8;

	// Addressable layers.
	enum Layer {
		LAYER_HEIGHTS,
		LAYER_WEIGHTS_0,
		LAYER_WEIGHTS_1,
		LAYER_WEIGHTS_2,
		LAYER_WEIGHTS_3,
		LAYER_HOLES,
		// Base (edit) layers, mip 0 only. Reads return the base where the tile has one and the
		// final data elsewhere, writes only modify the tiles that have a base.
		LAYER_BASE_HEIGHTS,
		LAYER_BASE_WEIGHTS_0,
		LAYER_BASE_WEIGHTS_1,
		LAYER_BASE_WEIGHTS_2,
		LAYER_BASE_WEIGHTS_3,
		LAYER_MAX,
	};

	// Groups of layers, loaded and stored together.
	enum LayerMask {
		MASK_HEIGHTS = 1,
		MASK_WEIGHTS = 2,
		MASK_HOLES = 4,
		MASK_BASE = 8,
		MASK_ALL = MASK_HEIGHTS | MASK_WEIGHTS | MASK_HOLES | MASK_BASE,
	};

	struct Tile {
		Vector<float> heights;
		Vector<uint8_t> weights[MAX_WEIGHTMAPS]; // RGBA8.
		Vector<uint8_t> holes; // Empty when the tile has no hole (0: solid, 255: hole).
		// Base layer (mip 0 tiles under splines only, empty otherwise).
		Vector<float> base_heights;
		Vector<uint8_t> base_weights[MAX_WEIGHTMAPS];
	};

	class LoadJob;

	struct TileInfo {
		enum Source : uint8_t {
			SOURCE_DEFAULT, // Never written: constant content.
			SOURCE_FILE, // Stored in the data file.
			SOURCE_OVERLAY, // Spilled to the overlay file.
		};
		Source source = SOURCE_DEFAULT;
		uint8_t loaded = 0; // LayerMask of the loaded layers.
		bool dirty = false; // Differs from its persistent copy.
		bool range_valid = true; // Mip 0 only: min/max heights are up to date.
		bool has_base = false; // Mip 0 only: the tile holds a base layer.
		uint16_t pins = 0;
		uint32_t version = 0; // Incremented on every change (also for derived mips).
		uint64_t offset = 0;
		uint32_t size = 0;
		float min_height = 0.0;
		float max_height = 0.0;
		uint64_t last_used = 0;
		Tile *tile = nullptr;
		Ref<LoadJob> job;
	};

	struct Mip {
		Vector2i tiles; // Tile count.
		Vector2i last; // Last valid texel coordinate (inclusive).
		LocalVector<TileInfo> infos;
	};

	class LoadJob : public StreamingJob {
		GDSOFTCLASS(LoadJob, StreamingJob);

	public:
		ObjectID owner;
		Ref<LandscapeStorageFile> file;
		uint64_t offset = 0;
		uint32_t size = 0;
		int mip = 0;
		Vector2i tile;
		uint8_t mask = 0;
		uint32_t generation = 0;
		uint32_t version = 0;
		int weightmap_count = 0;

		Tile result;
		uint8_t result_mask = 0;
		bool ok = false;

	protected:
		void run() override;
		void finish() override;
	};

private:
	Vector2i size;
	int domain = MIN_DOMAIN;
	int weightmap_count = 1;
	bool has_holes = false;
	float default_height = 0.0;
	LocalVector<Mip> mips;

	ObjectID owner;
	Ref<LandscapeStorageFile> file;
	Ref<LandscapeStorageFile> overlay;
	uint32_t generation = 0; // Invalidates pending loads when the file changes.

	uint64_t use_counter = 0;
	int64_t memory_usage = 0;
	int64_t memory_budget = 512 * 1024 * 1024;
	int loaded_tiles = 0;

	Rect2i pending_mips[3]; // Mip 0 rects not propagated to the other mips yet (per layer group).
	bool unsaved = false;

	_FORCE_INLINE_ TileInfo &_info(int p_mip, int p_tx, int p_tz) { return mips[p_mip].infos[p_tz * mips[p_mip].tiles.x + p_tx]; }
	_FORCE_INLINE_ const TileInfo &_info(int p_mip, int p_tx, int p_tz) const { return mips[p_mip].infos[p_tz * mips[p_mip].tiles.x + p_tx]; }

	int base_tiles = 0;

	static int _layer_texel_size(int p_layer) { return p_layer == LAYER_HOLES ? 1 : 4; }
	static uint8_t _layer_group(int p_layer);
	static bool _is_base_layer(int p_layer) { return p_layer >= LAYER_BASE_HEIGHTS && p_layer <= LAYER_BASE_WEIGHTS_3; }
	// Data of a layer in a loaded tile (null for empty hole masks). Base layers fall back to the final data.
	const uint8_t *_tile_layer_data(int p_layer, int p_mip, int p_tx, int p_tz);
	int64_t _tile_bytes(const Tile *p_tile) const;
	void _update_memory(TileInfo &r_info, int64_t p_before);

	void _fill_default(Tile *p_tile, uint8_t p_mask) const;
	void _ensure_layers(TileInfo &r_info, uint8_t p_mask);
	Tile *_load(int p_mip, int p_tx, int p_tz, uint8_t p_mask);
	void _evict(TileInfo &r_info);
	void _spill(TileInfo &r_info);
	void _compute_range(TileInfo &r_info);

	Vector<uint8_t> _encode_tile(const Tile *p_tile) const;
	static bool _decode_tile(const Vector<uint8_t> &p_blob, uint8_t p_mask, int p_weightmap_count, Tile &r_tile, uint8_t &r_mask);
	static Vector<uint8_t> _read_blob(const Ref<LandscapeStorageFile> &p_file, uint64_t p_offset, uint32_t p_size, uint8_t p_mask);

	TileInfo &_texel_info(int p_x, int p_z, uint8_t p_mask);
	void _write_mip_region(int p_layer, int p_mip, const Rect2i &p_rect, const void *p_data);
	void _mark_pending(uint8_t p_group, const Rect2i &p_rect);
	void _flush_mips();
	void _update_mip_region(int p_mip, const Rect2i &p_rect, uint8_t p_groups);

	void _setup_layout(const Vector2i &p_size);
	void _clear_tiles();

	friend class LoadJob;
	void _finish_load(LoadJob *p_job);

public:
	static int compute_domain(const Vector2i &p_size);
	static int compute_mip_count(int p_domain);

	void set_owner(ObjectID p_owner) { owner = p_owner; }

	// Layout.
	void init(const Vector2i &p_size, int p_weightmap_count, float p_default_height);
	bool is_valid() const { return !mips.is_empty(); }
	Vector2i get_size() const { return size; }
	int get_domain() const { return domain; }
	int get_mip_count() const { return mips.size(); }
	Vector2i get_mip_tiles(int p_mip) const { return mips[p_mip].tiles; }
	Vector2i get_mip_last(int p_mip) const { return mips[p_mip].last; }
	bool has_tile(int p_mip, int p_tx, int p_tz) const;

	int get_weightmap_count() const { return weightmap_count; }
	void set_weightmap_count(int p_count);
	bool get_has_holes() const { return has_holes; }
	void set_has_holes(bool p_holes);
	float get_default_height() const { return default_height; }

	// Tiles.
	// Returns the tile with (at least) the given layers loaded, loading it synchronously if needed.
	// The pointer is valid until the next call that may evict tiles (trim()) unless the tile is pinned.
	const Tile *get_tile(int p_mip, int p_tx, int p_tz, uint8_t p_mask);
	// Returns the tile if its layers are loaded, otherwise starts an asynchronous load and returns null.
	const Tile *request_tile(int p_mip, int p_tx, int p_tz, uint8_t p_mask, float p_priority);
	uint32_t get_tile_version(int p_mip, int p_tx, int p_tz) const;
	void pin_tile(int p_mip, int p_tx, int p_tz);
	void unpin_tile(int p_mip, int p_tx, int p_tz);
	void wait_for_loads();

	// Regions, in texels of a mip level. Reads clamp the coordinates to the valid texels.
	// Heights are floats, weightmaps RGBA8 and holes 8-bit.
	void read_region(int p_layer, int p_mip, const Rect2i &p_rect, void *r_data);
	// Writes mip 0 texels (the rect must be inside the landscape). The other mips are updated lazily.
	void write_region(int p_layer, const Rect2i &p_rect, const void *p_data);

	// Texel access (mip 0).
	float get_height(int p_x, int p_z);
	void set_height(int p_x, int p_z, float p_height);
	void get_weights(int p_x, int p_z, uint8_t *r_weights); // weightmap_count * 4 values.
	void set_weights(int p_x, int p_z, const uint8_t *p_weights);
	bool is_hole(int p_x, int p_z);
	void set_hole(int p_x, int p_z, bool p_hole);
	// Height of a texel of any mip level (exact terrain height at that vertex).
	float get_mip_height(int p_mip, int p_x, int p_z);

	// Base layer (mip 0). Enabling the base of a tile copies its current heights and weights,
	// disabling it drops the base (the final data is kept as is).
	bool tile_has_base(int p_tx, int p_tz) const;
	void set_tile_base(int p_tx, int p_tz, bool p_enable);
	bool has_base_in_rect(const Rect2i &p_rect) const; // Mip 0 texels.
	int get_base_tile_count() const { return base_tiles; }
	// Base texel access: reads return the final data of tiles without base, writes are ignored there.
	float get_base_height(int p_x, int p_z);
	void get_base_weights(int p_x, int p_z, uint8_t *r_weights);
	void set_base_weights(int p_x, int p_z, const uint8_t *p_weights);

	// Propagates the pending mip 0 changes to the other mip levels.
	void update_mips() { _flush_mips(); }
	void mark_all_dirty(uint8_t p_groups);
	Vector2 get_height_range();

	// Memory.
	void set_memory_budget(int64_t p_bytes) { memory_budget = MAX(p_bytes, int64_t(16 * 1024 * 1024)); }
	int64_t get_memory_budget() const { return memory_budget; }
	int64_t get_memory_usage() const { return memory_usage; }
	int get_loaded_tile_count() const { return loaded_tiles; }
	void trim();

	// Files.
	Error open_file(const String &p_path, Dictionary &r_header);
	Error save_file(const String &p_path, const Dictionary &p_header);
	String get_file_path() const { return file.is_valid() ? file->get_path() : String(); }
	bool is_file_backed() const { return file.is_valid(); }
	bool has_unsaved_changes() const;

	LandscapeStorage();
	~LandscapeStorage();
};
