/**************************************************************************/
/*  landscape_streamer.h                                                  */
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

#ifdef RD_ENABLED

#include "landscape_storage.h"

#include "core/math/plane.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"

#include "modules/streaming/world_streaming.h"

class LandscapeData;
class LandscapeGPU;
class LandscapeLodTree;

// Streams the landscape data into the GPU page pool of a Landscape3D.
//
// Every frame the pages that should be resident are selected on the CPU (without frustum
// culling, so turning the camera doesn't need new pages):
// - the pages of the quadtree nodes the GPU traversal may reach (same screen-space error test),
// - the pages whose resolution matches the screen footprint of their texels (virtual texturing),
// sorted by distance relative to their size, and capped to the pool size. Missing pages are
// assembled on the streaming threads from the storage tiles (heights, normals computed from
// the heights, weights and holes, plus a one texel border) and uploaded into free or least
// recently needed slots. The page table maps every tile of every mip level to the finest
// resident page covering it, so the shaders always find valid (possibly coarser) data.
class LandscapeStreamer {
public:
	struct Camera {
		Vector3 position;
		real_t projection_factor = 1.0; // Pixels per local unit at distance 1.
		bool orthogonal = false;
		Plane planes[6];
	};

	struct Settings {
		int patch_quads = 32;
		float lod_pixel_error = 2.0;
		float micro_amplitude = 0.0;
		float texture_lod_bias = 0.5;
		int max_builds = 32; // Page builds in flight.
		int max_sync_rebuilds = 96; // Modified pages rebuilt synchronously per change (editing).
	};

	struct Stats {
		int slots = 0;
		int resident = 0;
		int desired = 0;
		int building = 0;
		int dropped = 0;

		// Page data waiting for the next update() (uploaded together with the page table).
		struct PendingUpload {
			int slot;
			Vector<uint8_t> data;
		};
		LocalVector<PendingUpload> pending_uploads;
		int64_t pool_bytes = 0;
	};

	class BuildJob : public StreamingJob {
		GDSOFTCLASS(BuildJob, StreamingJob);

	public:
		ObjectID owner;
		uint64_t key = 0;
		uint32_t serial = 0;
		int mip = 0;
		Vector2i tile;
		Vector2i last;
		float spacing = 1.0; // Distance between two texels of this mip level.
		int weightmap_count = 1;
		bool holes = false;
		LandscapeStorage::Tile tiles[9]; // 3x3 neighborhood, center at index 4.
		Vector<uint8_t> result;

	protected:
		void run() override;
		void finish() override;
	};

	static Vector<uint8_t> build_page(const LandscapeStorage::Tile *const *p_tiles, int p_mip, const Vector2i &p_tile, const Vector2i &p_last, float p_spacing, int p_weightmap_count, bool p_holes);

private:
	struct Slot {
		uint64_t key = UINT64_MAX;
		uint64_t last_needed = 0;
	};

	struct Desire {
		uint64_t key;
		float score;
		bool operator<(const Desire &p_other) const { return score < p_other.score; }
	};

	ObjectID owner;
	LandscapeGPU *gpu = nullptr;
	LandscapeData *data = nullptr;
	Settings settings;

	int root_mip = 0;
	int weightmap_count = 1;
	bool holes = false;
	float spacing = 1.0;
	LocalVector<Slot> slots;
	LocalVector<int> free_slots;
	HashMap<uint64_t, int> resident;
	HashMap<uint64_t, Ref<BuildJob>> building;
	HashSet<uint64_t> stale_building; // Modified while being built.
	uint32_t serial = 0;
	uint64_t frame = 0;

	LocalVector<Desire> desired;
	HashSet<uint64_t> desired_set;
	bool desired_dirty = true;
	Vector3 desired_camera = Vector3(Math::INF, Math::INF, Math::INF);

	// Page table layout: rows of every mip level stacked vertically.
	Vector2i table_size;
	int table_rows[16] = {};
	Vector2i table_tiles[16];
	bool table_dirty = true;

	int dropped = 0;

	// Page data waiting for the next update() (uploaded together with the page table).
	struct PendingUpload {
		int slot;
		Vector<uint8_t> data;
	};
	LocalVector<PendingUpload> pending_uploads;

	static _FORCE_INLINE_ uint64_t _key(int p_mip, int p_tx, int p_tz) { return (uint64_t(p_mip) << 48) | (uint64_t(p_tz) << 24) | uint64_t(p_tx); }
	static _FORCE_INLINE_ int _key_mip(uint64_t p_key) { return int(p_key >> 48); }
	static _FORCE_INLINE_ Vector2i _key_tile(uint64_t p_key) { return Vector2i(int(p_key & 0xFFFFFF), int((p_key >> 24) & 0xFFFFFF)); }

	void _add_desire(HashMap<uint64_t, float> &r_scores, uint64_t p_key, float p_score);
	void _update_desired(const LandscapeLodTree *p_tree, const Camera &p_camera);
	bool _gather_tiles(int p_mip, const Vector2i &p_tile, float p_priority, bool p_sync, const LandscapeStorage::Tile **r_tiles);
	bool _start_build(uint64_t p_key, float p_priority);
	void _rebuild_sync(uint64_t p_key, int p_slot);
	void _evict(uint64_t p_key);
	int _allocate_slot();
	void _upload(int p_slot, const Vector<uint8_t> &p_data);
	void _update_page_table();

public:
	void setup(ObjectID p_owner, LandscapeGPU *p_gpu, LandscapeData *p_data, int p_slots, int p_root_mip);
	void clear();
	bool is_valid() const { return data != nullptr && !slots.is_empty(); }

	void set_settings(const Settings &p_settings);
	// Selects, requests and uploads pages. Returns true if the resident set changed.
	bool update(const LandscapeLodTree *p_tree, const Camera &p_camera);
	void mark_region_changed(const Rect2i &p_texel_rect);
	void invalidate_desired() { desired_dirty = true; }
	void page_built(BuildJob *p_job);

	bool is_root_resident() const;
	int get_root_mip() const { return root_mip; }
	Vector2i get_table_size() const { return table_size; }
	PackedInt32Array get_table_rows() const;
	PackedInt32Array get_table_columns() const;
	PackedInt32Array get_table_row_counts() const;
	void get_table_layout(uint32_t *r_rows, uint32_t *r_tiles) const; // 16 entries each, for the LOD traversal.
	Stats get_stats() const;

	~LandscapeStreamer();
};

#endif // RD_ENABLED
