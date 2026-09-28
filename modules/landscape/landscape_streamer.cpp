/**************************************************************************/
/*  landscape_streamer.cpp                                                */
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

#include "landscape_streamer.h"

#ifdef RD_ENABLED

#include "landscape_3d.h"
#include "landscape_data.h"
#include "landscape_gpu.h"
#include "landscape_lod_tree.h"

#include "core/object/callable_mp.h"
#include "servers/rendering/rendering_server.h"

static constexpr int PAGE_TEXELS = LandscapeGPU::PAGE_TEXELS;

static _FORCE_INLINE_ int _floor_div(int p_a, int p_b) {
	return p_a >= 0 ? p_a / p_b : -((-p_a + p_b - 1) / p_b);
}

/* Page assembly */

Vector<uint8_t> LandscapeStreamer::build_page(const LandscapeStorage::Tile *const *p_tiles, int p_mip, const Vector2i &p_tile, const Vector2i &p_last, float p_spacing, int p_weightmap_count, bool p_holes) {
	constexpr int texels = PAGE_TEXELS * PAGE_TEXELS;
	constexpr int SHIFT = LandscapeStorage::TILE_SHIFT;
	constexpr int MASK = LandscapeStorage::TILE_MASK;
	const Vector2i base = p_tile * LandscapeStorage::TILE_SIZE;

	Vector<uint8_t> result;
	result.resize(int64_t(texels) * (4 + 2 + 4 * p_weightmap_count + (p_holes ? 1 : 0)));
	uint8_t *out = result.ptrw();

	// Returns the tile holding a texel of the mip level (clamped to the valid texels) and its index in that tile.
	auto locate = [&](int p_x, int p_z, int &r_index) -> const LandscapeStorage::Tile * {
		p_x = CLAMP(p_x, 0, p_last.x);
		p_z = CLAMP(p_z, 0, p_last.y);
		const int nx = (p_x >> SHIFT) - p_tile.x + 1;
		const int nz = (p_z >> SHIFT) - p_tile.y + 1;
		r_index = (p_z & MASK) * LandscapeStorage::TILE_SIZE + (p_x & MASK);
		if (nx < 0 || nx > 2 || nz < 0 || nz > 2) {
			return nullptr;
		}
		return p_tiles[nz * 3 + nx];
	};

	// Heights with a one texel margin (for the normals).
	constexpr int H = PAGE_TEXELS + 2;
	LocalVector<float> heights;
	heights.resize(H * H);
	for (int z = 0; z < H; z++) {
		for (int x = 0; x < H; x++) {
			int index;
			const LandscapeStorage::Tile *tile = locate(base.x + x - 1, base.y + z - 1, index);
			heights[z * H + x] = (tile && !tile->heights.is_empty()) ? tile->heights[index] : 0.0f;
		}
	}

	float *out_heights = reinterpret_cast<float *>(out);
	uint8_t *out_normals = out + texels * 4;
	for (int z = 0; z < PAGE_TEXELS; z++) {
		for (int x = 0; x < PAGE_TEXELS; x++) {
			const float *row = &heights[(z + 1) * H + x + 1];
			out_heights[z * PAGE_TEXELS + x] = row[0];
			const Vector3 n = Vector3(row[-1] - row[1], 2.0f * p_spacing, row[-H] - row[H]).normalized();
			out_normals[(z * PAGE_TEXELS + x) * 2 + 0] = uint8_t(CLAMP(Math::round((n.x * 0.5f + 0.5f) * 255.0f), 0.0f, 255.0f));
			out_normals[(z * PAGE_TEXELS + x) * 2 + 1] = uint8_t(CLAMP(Math::round((n.z * 0.5f + 0.5f) * 255.0f), 0.0f, 255.0f));
		}
	}

	uint8_t *w = out_normals + texels * 2;
	for (int i = 0; i < p_weightmap_count; i++) {
		for (int z = 0; z < PAGE_TEXELS; z++) {
			for (int x = 0; x < PAGE_TEXELS; x++) {
				int index;
				const LandscapeStorage::Tile *tile = locate(base.x + x, base.y + z, index);
				uint8_t *dst = w + (z * PAGE_TEXELS + x) * 4;
				if (tile && tile->weights[i].size() == LandscapeStorage::TILE_TEXELS * 4) {
					memcpy(dst, tile->weights[i].ptr() + index * 4, 4);
				} else {
					dst[0] = i == 0 ? 255 : 0;
					dst[1] = dst[2] = dst[3] = 0;
				}
			}
		}
		w += texels * 4;
	}

	if (p_holes) {
		for (int z = 0; z < PAGE_TEXELS; z++) {
			for (int x = 0; x < PAGE_TEXELS; x++) {
				int index;
				const LandscapeStorage::Tile *tile = locate(base.x + x, base.y + z, index);
				w[z * PAGE_TEXELS + x] = (tile && !tile->holes.is_empty()) ? tile->holes[index] : 0;
			}
		}
	}
	return result;
}

void LandscapeStreamer::BuildJob::run() {
	const LandscapeStorage::Tile *ptrs[9];
	for (int i = 0; i < 9; i++) {
		ptrs[i] = tiles[i].heights.is_empty() ? nullptr : &tiles[i];
	}
	result = build_page(ptrs, mip, tile, last, spacing, weightmap_count, holes);
	// Release the tile snapshots early.
	for (LandscapeStorage::Tile &t : tiles) {
		t = LandscapeStorage::Tile();
	}
}

void LandscapeStreamer::BuildJob::finish() {
	Landscape3D *landscape = ObjectDB::get_instance<Landscape3D>(owner);
	if (landscape) {
		landscape->_page_built(this);
	}
}

/* Setup */

void LandscapeStreamer::setup(ObjectID p_owner, LandscapeGPU *p_gpu, LandscapeData *p_data, int p_slots, int p_root_mip) {
	clear();
	ERR_FAIL_NULL(p_data);
	ERR_FAIL_COND(!p_data->is_valid());
	LandscapeStorage &storage = p_data->get_storage();
	ERR_FAIL_INDEX(p_root_mip, MIN(storage.get_mip_count(), 16));

	owner = p_owner;
	gpu = p_gpu;
	data = p_data;
	root_mip = p_root_mip;
	weightmap_count = storage.get_weightmap_count();
	holes = storage.get_has_holes();
	spacing = p_data->get_vertex_spacing();

	int rows = 0;
	for (int m = 0; m < 16; m++) {
		table_rows[m] = rows;
		table_tiles[m] = m <= root_mip ? storage.get_mip_tiles(m) : Vector2i();
		rows += table_tiles[m].y;
	}
	table_size = Vector2i(MAX(table_tiles[0].x, 1), MAX(rows, 1));

	slots.resize(p_slots);
	free_slots.clear();
	for (int i = p_slots - 1; i >= 0; i--) {
		slots[i] = Slot();
		free_slots.push_back(i);
	}
	desired_dirty = true;
	table_dirty = true;
}

void LandscapeStreamer::clear() {
	if (WorldStreaming::get_singleton()) {
		for (KeyValue<uint64_t, Ref<BuildJob>> &kv : building) {
			WorldStreaming::get_singleton()->cancel(kv.value);
		}
	}
	building.clear();
	stale_building.clear();
	resident.clear();
	slots.clear();
	free_slots.clear();
	desired.clear();
	desired_set.clear();
	pending_uploads.clear();
	data = nullptr;
	gpu = nullptr;
	dropped = 0;
	desired_camera = Vector3(Math::INF, Math::INF, Math::INF);
}

void LandscapeStreamer::set_settings(const Settings &p_settings) {
	if (p_settings.lod_pixel_error != settings.lod_pixel_error || p_settings.texture_lod_bias != settings.texture_lod_bias || p_settings.micro_amplitude != settings.micro_amplitude || p_settings.patch_quads != settings.patch_quads) {
		desired_dirty = true;
	}
	settings = p_settings;
}

/* Selection */

void LandscapeStreamer::_add_desire(HashMap<uint64_t, float> &r_scores, uint64_t p_key, float p_score) {
	float *existing = r_scores.getptr(p_key);
	if (existing) {
		*existing = MIN(*existing, p_score);
	} else {
		r_scores.insert(p_key, p_score);
	}
}

void LandscapeStreamer::_update_desired(const LandscapeLodTree *p_tree, const Camera &p_camera) {
	HashMap<uint64_t, float> scores;
	LandscapeStorage &storage = data->get_storage();
	const Vector2i size = storage.get_size();
	const int max_level = p_tree->get_max_level();
	const int patch = p_tree->get_patch_quads();
	const float pf = MAX(float(p_camera.projection_factor), 1e-6f);
	const float amplitude = settings.micro_amplitude;

	auto aabb_distance = [&](const Vector3 &p_min, const Vector3 &p_max) {
		return p_camera.position.distance_to(p_camera.position.clamp(p_min, p_max));
	};
	auto frustum_factor = [&](const Vector3 &p_min, const Vector3 &p_max) {
		for (int i = 0; i < 6; i++) {
			const Plane &plane = p_camera.planes[i];
			const Vector3 nearest(plane.normal.x > 0 ? p_min.x : p_max.x, plane.normal.y > 0 ? p_min.y : p_max.y, plane.normal.z > 0 ? p_min.z : p_max.z);
			if (plane.distance_to(nearest) > 0.0) {
				return 4.0f; // Outside of the view: streamed after the visible pages.
			}
		}
		return 1.0f;
	};

	// The root page covers the whole landscape and is the fallback of every other page.
	_add_desire(scores, _key(root_mip, 0, 0), -1.0);

	// Geometry: pages of the quadtree nodes the traversal can reach (without frustum culling).
	struct Item {
		int level;
		int x;
		int z;
	};
	LocalVector<Item> stack;
	stack.push_back({ 0, 0, 0 });
	while (!stack.is_empty()) {
		const Item item = stack[stack.size() - 1];
		stack.remove_at(stack.size() - 1);
		float mn, mx, err;
		if (!p_tree->get_node(item.level, item.x, item.z, mn, mx, err)) {
			continue;
		}
		const int mip = max_level - item.level;
		const int node_texels = patch << mip;
		const Vector3 aabb_min(item.x * node_texels * spacing, mn - amplitude, item.z * node_texels * spacing);
		const Vector3 aabb_max(MIN((item.x + 1) * node_texels, size.x - 1) * spacing, mx + amplitude, MIN((item.z + 1) * node_texels, size.y - 1) * spacing);
		const float dist = aabb_distance(aabb_min, aabb_max);
		const float tile_world = float(LandscapeStorage::TILE_SIZE << mip) * spacing;
		if (mip <= root_mip) {
			_add_desire(scores, _key(mip, (item.x * patch) >> LandscapeStorage::TILE_SHIFT, (item.z * patch) >> LandscapeStorage::TILE_SHIFT), dist / tile_world * frustum_factor(aabb_min, aabb_max));
		}
		if (item.level >= max_level) {
			continue;
		}
		const float error = MAX(err, amplitude);
		const float sse = p_camera.orthogonal ? error * pf : error * pf / MAX(dist, 1e-4f);
		if (sse > settings.lod_pixel_error) {
			for (int c = 0; c < 4; c++) {
				stack.push_back({ item.level + 1, item.x * 2 + (c & 1), item.z * 2 + (c >> 1) });
			}
		}
	}

	// Textures: pages whose texels match the screen footprint (see ls_page_uv() in the shader).
	const int tree_offset = int(Math::get_shift_from_power_of_2(uint32_t(LandscapeStorage::TILE_SIZE))) - int(Math::get_shift_from_power_of_2(uint32_t(patch)));
	for (int m = 0; m <= root_mip; m++) {
		const Vector2i tiles = table_tiles[m];
		const float tile_world = float(LandscapeStorage::TILE_SIZE << m) * spacing;
		float radius;
		if (p_camera.orthogonal) {
			// Constant footprint: only the matching mip level (and the coarser fallbacks) are needed.
			const int ortho_mip = int(Math::floor(Math::log2(1.0f / (pf * spacing)) + settings.texture_lod_bias));
			radius = m >= ortho_mip ? 1e30f : -1.0f;
		} else {
			radius = pf * spacing * Math::pow(2.0f, float(m) + 1.0f - settings.texture_lod_bias);
		}
		if (radius < 0.0) {
			continue;
		}
		const float reach = MIN(radius, 1e7f);
		const int tx0 = CLAMP(int(Math::floor((p_camera.position.x - reach) / tile_world)), 0, tiles.x - 1);
		const int tx1 = CLAMP(int(Math::floor((p_camera.position.x + reach) / tile_world)), 0, tiles.x - 1);
		const int tz0 = CLAMP(int(Math::floor((p_camera.position.z - reach) / tile_world)), 0, tiles.y - 1);
		const int tz1 = CLAMP(int(Math::floor((p_camera.position.z + reach) / tile_world)), 0, tiles.y - 1);
		// Height bounds of the tile, from the quadtree node covering it.
		const int level = CLAMP(max_level - m - tree_offset, 0, max_level);
		const int level_shift = max_level - m - level - tree_offset; // > 0 when a node covers several tiles.
		for (int tz = tz0; tz <= tz1; tz++) {
			for (int tx = tx0; tx <= tx1; tx++) {
				float mn = 0.0, mx = 0.0, err = 0.0;
				const int nx = level_shift >= 0 ? tx >> level_shift : tx << -level_shift;
				const int nz = level_shift >= 0 ? tz >> level_shift : tz << -level_shift;
				if (!p_tree->get_node(level, MIN(nx, (1 << level) - 1), MIN(nz, (1 << level) - 1), mn, mx, err)) {
					const Vector2 range = p_tree->get_height_range();
					mn = range.x;
					mx = range.y;
				}
				const Vector3 aabb_min(tx * tile_world, mn - amplitude, tz * tile_world);
				const Vector3 aabb_max(MIN((tx + 1) * tile_world, (size.x - 1) * spacing), mx + amplitude, MIN((tz + 1) * tile_world, (size.y - 1) * spacing));
				const float dist = aabb_distance(aabb_min, aabb_max);
				if (dist <= radius) {
					_add_desire(scores, _key(m, tx, tz), dist / tile_world * frustum_factor(aabb_min, aabb_max));
				}
			}
		}
	}

	desired.clear();
	for (const KeyValue<uint64_t, float> &kv : scores) {
		desired.push_back({ kv.key, kv.value });
	}
	desired.sort();
	if (desired.size() > slots.size()) {
		desired.resize(slots.size()); // Least important pages don't fit in the pool.
	}
	desired_set.clear();
	for (const Desire &desire : desired) {
		desired_set.insert(desire.key);
	}
}

/* Building */

bool LandscapeStreamer::_gather_tiles(int p_mip, const Vector2i &p_tile, float p_priority, bool p_sync, const LandscapeStorage::Tile **r_tiles) {
	using LS = LandscapeStorage;
	// Layers needed from the neighbors: the page border (right and bottom) needs everything,
	// the normals need the heights around the page (no top left corner).
	static const uint8_t masks[9] = {
		0, LS::MASK_HEIGHTS, LS::MASK_HEIGHTS,
		LS::MASK_HEIGHTS, LS::MASK_ALL, LS::MASK_ALL,
		LS::MASK_HEIGHTS, LS::MASK_ALL, LS::MASK_ALL
	};
	LandscapeStorage &storage = data->get_storage();
	bool ok = true;
	for (int i = 0; i < 9; i++) {
		r_tiles[i] = nullptr;
		const int x = p_tile.x + (i % 3) - 1;
		const int z = p_tile.y + (i / 3) - 1;
		if (masks[i] == 0 || !storage.has_tile(p_mip, x, z)) {
			continue;
		}
		r_tiles[i] = p_sync ? storage.get_tile(p_mip, x, z, masks[i]) : storage.request_tile(p_mip, x, z, masks[i], p_priority);
		ok = ok && r_tiles[i] != nullptr;
	}
	return ok;
}

bool LandscapeStreamer::_start_build(uint64_t p_key, float p_priority) {
	const int mip = _key_mip(p_key);
	const Vector2i tile = _key_tile(p_key);
	const LandscapeStorage::Tile *tiles[9];
	if (!_gather_tiles(mip, tile, p_priority, false, tiles)) {
		return false; // Waiting for the storage tiles.
	}
	Ref<BuildJob> job;
	job.instantiate();
	job->owner = owner;
	job->key = p_key;
	job->serial = ++serial;
	job->mip = mip;
	job->tile = tile;
	job->last = data->get_storage().get_mip_last(mip);
	job->spacing = spacing * float(1 << mip);
	job->weightmap_count = weightmap_count;
	job->holes = holes;
	for (int i = 0; i < 9; i++) {
		if (tiles[i]) {
			job->tiles[i] = *tiles[i]; // Copy-on-write snapshot, safe to read on the streaming threads.
		}
	}
	building.insert(p_key, job);
	stale_building.erase(p_key);
	WorldStreaming::get_singleton()->submit(job, p_priority);
	return true;
}

void LandscapeStreamer::_rebuild_sync(uint64_t p_key, int p_slot) {
	const int mip = _key_mip(p_key);
	const Vector2i tile = _key_tile(p_key);
	const LandscapeStorage::Tile *tiles[9];
	_gather_tiles(mip, tile, 0.0, true, tiles);
	pending_uploads.push_back({ p_slot, build_page(tiles, mip, tile, data->get_storage().get_mip_last(mip), spacing * float(1 << mip), weightmap_count, holes) });
}

void LandscapeStreamer::_evict(uint64_t p_key) {
	const int *slot = resident.getptr(p_key);
	if (!slot) {
		return;
	}
	const int index = *slot;
	resident.erase(p_key);
	slots[index] = Slot();
	free_slots.push_back(index);
	table_dirty = true;
}

void LandscapeStreamer::page_built(BuildJob *p_job) {
	Ref<BuildJob> *entry = building.getptr(p_job->key);
	if (!entry || entry->ptr() != p_job || !data) {
		return;
	}
	const uint64_t key = p_job->key;
	building.erase(key);
	const bool was_stale = stale_building.has(key);
	stale_building.erase(key);

	if (was_stale) {
		return; // Modified while building: the data is outdated, the page is requested again.
	}
	int slot = -1;
	if (const int *existing = resident.getptr(key)) {
		slot = *existing;
	} else {
		if (!desired_set.has(key)) {
			return; // Not needed anymore.
		}
		slot = _allocate_slot();
		if (slot < 0) {
			dropped++;
			return;
		}
		slots[slot].key = key;
		resident.insert(key, slot);
		table_dirty = true;
	}
	slots[slot].last_needed = MAX(slots[slot].last_needed, frame);
	pending_uploads.push_back({ slot, p_job->result });
	p_job->result = Vector<uint8_t>();
}

int LandscapeStreamer::_allocate_slot() {
	if (!free_slots.is_empty()) {
		const int slot = free_slots[free_slots.size() - 1];
		free_slots.remove_at(free_slots.size() - 1);
		return slot;
	}
	// Evict the least recently needed page that isn't needed now (never the root page).
	const uint64_t root_key = _key(root_mip, 0, 0);
	int best = -1;
	for (uint32_t i = 0; i < slots.size(); i++) {
		const Slot &slot = slots[i];
		if (slot.key == root_key || slot.last_needed >= frame || desired_set.has(slot.key)) {
			continue;
		}
		if (best < 0 || slot.last_needed < slots[best].last_needed) {
			best = i;
		}
	}
	if (best >= 0) {
		resident.erase(slots[best].key);
		slots[best] = Slot();
		table_dirty = true;
	}
	return best;
}

/* Update */

bool LandscapeStreamer::update(const LandscapeLodTree *p_tree, const Camera &p_camera) {
	if (!is_valid() || !p_tree || !p_tree->is_valid()) {
		return false;
	}
	frame++;
	if (desired_dirty || p_camera.position.distance_to(desired_camera) > spacing * 0.5) {
		_update_desired(p_tree, p_camera);
		desired_camera = p_camera.position;
		desired_dirty = false;
	}

	WorldStreaming *ws = WorldStreaming::get_singleton();
	for (const Desire &desire : desired) {
		const float priority = -desire.score;
		if (const int *slot = resident.getptr(desire.key)) {
			slots[*slot].last_needed = frame;
			continue;
		}
		if (Ref<BuildJob> *job = building.getptr(desire.key)) {
			ws->set_job_priority(*job, priority);
			continue;
		}
		if (int(building.size()) < settings.max_builds) {
			_start_build(desire.key, priority);
		}
	}

	bool changed = false;
	if (!pending_uploads.is_empty()) {
		RenderingServer *rs = RenderingServer::get_singleton();
		for (const PendingUpload &upload : pending_uploads) {
			rs->call_on_render_thread(callable_mp(gpu, &LandscapeGPU::upload_page).bind(upload.slot, upload.data));
		}
		pending_uploads.clear();
		changed = true;
	}
	// The table is uploaded after the pages it references.
	if (table_dirty) {
		_update_page_table();
		changed = true;
	}
	return changed;
}

void LandscapeStreamer::mark_region_changed(const Rect2i &p_texel_rect) {
	if (!is_valid()) {
		return;
	}
	// Every resident page must hold up to date data: shared vertices are read from different mip
	// levels, which must agree. Pages are rebuilt right away (coarse ones first, the root page is
	// always affected), the ones over the budget are evicted and streamed in again.
	int rebuilt = 0;
	for (int m = root_mip; m >= 0; m--) {
		// Texels of the mip level influenced by the change (point sampling, tent filter and normals).
		const int x0 = ((p_texel_rect.position.x - 2) >> m) - 2;
		const int z0 = ((p_texel_rect.position.y - 2) >> m) - 2;
		const int x1 = ((p_texel_rect.get_end().x + 1) >> m) + 2;
		const int z1 = ((p_texel_rect.get_end().y + 1) >> m) + 2;
		// A page holds the texels [128 t - 1, 128 t + 129] of its mip level (with the normals).
		const Vector2i tiles = table_tiles[m];
		const int tx0 = MAX(_floor_div(x0 - 129, 128), 0);
		const int tz0 = MAX(_floor_div(z0 - 129, 128), 0);
		const int tx1 = MIN(_floor_div(x1 + 1, 128), tiles.x - 1);
		const int tz1 = MIN(_floor_div(z1 + 1, 128), tiles.y - 1);
		for (int tz = tz0; tz <= tz1; tz++) {
			for (int tx = tx0; tx <= tx1; tx++) {
				const uint64_t key = _key(m, tx, tz);
				if (building.has(key)) {
					stale_building.insert(key);
				}
				const int *slot = resident.getptr(key);
				if (!slot) {
					continue;
				}
				if (rebuilt < settings.max_sync_rebuilds || m == root_mip) {
					// Immediate feedback while editing, the modified tiles are in memory.
					_rebuild_sync(key, *slot);
					rebuilt++;
				} else {
					_evict(key);
				}
			}
		}
	}
}

void LandscapeStreamer::_update_page_table() {
	table_dirty = false;
	Vector<uint8_t> bytes;
	bytes.resize(int64_t(table_size.x) * table_size.y * sizeof(float));
	float *table = reinterpret_cast<float *>(bytes.ptrw());
	for (int64_t i = 0; i < int64_t(table_size.x) * table_size.y; i++) {
		table[i] = -1.0;
	}
	// Coarse to fine: tiles without a resident page inherit the entry of their parent.
	for (int m = root_mip; m >= 0; m--) {
		const Vector2i tiles = table_tiles[m];
		for (int tz = 0; tz < tiles.y; tz++) {
			for (int tx = 0; tx < tiles.x; tx++) {
				float entry = -1.0;
				if (const int *slot = resident.getptr(_key(m, tx, tz))) {
					entry = float(*slot | (m << LandscapeGPU::PAGE_SLOT_BITS));
				} else if (m < root_mip) {
					const Vector2i parent = Vector2i(tx >> 1, tz >> 1).min(table_tiles[m + 1] - Vector2i(1, 1));
					entry = table[(table_rows[m + 1] + parent.y) * table_size.x + parent.x];
				}
				table[(table_rows[m] + tz) * table_size.x + tx] = entry;
			}
		}
	}
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp(gpu, &LandscapeGPU::upload_page_table).bind(bytes));
}

/* Queries */

bool LandscapeStreamer::is_root_resident() const {
	return resident.has(_key(root_mip, 0, 0));
}

PackedInt32Array LandscapeStreamer::get_table_rows() const {
	PackedInt32Array result;
	for (int m = 0; m < 16; m++) {
		result.push_back(table_rows[m]);
	}
	return result;
}

PackedInt32Array LandscapeStreamer::get_table_columns() const {
	PackedInt32Array result;
	for (int m = 0; m < 16; m++) {
		result.push_back(table_tiles[m].x);
	}
	return result;
}

PackedInt32Array LandscapeStreamer::get_table_row_counts() const {
	PackedInt32Array result;
	for (int m = 0; m < 16; m++) {
		result.push_back(table_tiles[m].y);
	}
	return result;
}

void LandscapeStreamer::get_table_layout(uint32_t *r_rows, uint32_t *r_tiles) const {
	for (int m = 0; m < 16; m++) {
		r_rows[m] = table_rows[m];
		r_tiles[m] = uint32_t(table_tiles[m].x) | (uint32_t(table_tiles[m].y) << 16);
	}
}

LandscapeStreamer::Stats LandscapeStreamer::get_stats() const {
	Stats stats;
	stats.slots = slots.size();
	stats.resident = resident.size();
	stats.desired = desired.size();
	stats.building = building.size();
	stats.dropped = dropped;
	stats.pool_bytes = LandscapeGPU::get_slot_size(weightmap_count, holes) * slots.size();
	return stats;
}

LandscapeStreamer::~LandscapeStreamer() {
	clear();
}

#endif // RD_ENABLED
