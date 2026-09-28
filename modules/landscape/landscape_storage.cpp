/**************************************************************************/
/*  landscape_storage.cpp                                                 */
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

#include "landscape_storage.h"

#include "landscape_data.h"

#include "core/io/compression.h"
#include "core/io/dir_access.h"
#include "core/io/marshalls.h"
#include "core/math/math_funcs_binary.h"
#include "core/os/os.h"

static constexpr uint32_t FILE_MAGIC = 0x534c4447; // "GDLS"
static constexpr uint32_t FILE_VERSION = 1;
static constexpr uint32_t FILE_HEADER_SIZE = 72;
static constexpr uint32_t INDEX_ENTRY_SIZE = 24;
static constexpr uint32_t BLOB_HEADER_SIZE = 16;
static constexpr uint32_t INDEX_FLAG_DATA = 1;

/* LandscapeStorageFile */

Error LandscapeStorageFile::open(const String &p_path, FileAccess::ModeFlags p_mode) {
	MutexLock lock(mutex);
	Error err = OK;
	file = FileAccess::open(p_path, p_mode, &err);
	path = file.is_valid() ? p_path : String();
	return file.is_valid() ? OK : (err != OK ? err : ERR_CANT_OPEN);
}

void LandscapeStorageFile::close() {
	MutexLock lock(mutex);
	file.unref();
}

Vector<uint8_t> LandscapeStorageFile::read(uint64_t p_offset, uint32_t p_size) {
	MutexLock lock(mutex);
	Vector<uint8_t> data;
	ERR_FAIL_COND_V(file.is_null(), data);
	file->seek(p_offset);
	data.resize(p_size);
	const uint64_t read = file->get_buffer(data.ptrw(), p_size);
	if (read != p_size) {
		data.clear();
	}
	return data;
}

uint64_t LandscapeStorageFile::append(const uint8_t *p_data, uint32_t p_size) {
	MutexLock lock(mutex);
	ERR_FAIL_COND_V(file.is_null(), 0);
	file->seek_end();
	const uint64_t offset = file->get_position();
	file->store_buffer(p_data, p_size);
	return offset;
}

LandscapeStorageFile::~LandscapeStorageFile() {
	file.unref();
}

/* Layout */

int LandscapeStorage::compute_domain(const Vector2i &p_size) {
	const uint32_t quads = uint32_t(MAX(MAX(p_size.x, p_size.y) - 1, 1));
	return MAX(int(Math::next_power_of_2(quads)), MIN_DOMAIN);
}

int LandscapeStorage::compute_mip_count(int p_domain) {
	// The coarsest mip is MIN_DOMAIN texels wide, which is enough for the smallest LOD patches.
	return int(Math::get_shift_from_power_of_2(uint32_t(p_domain / MIN_DOMAIN))) + 1;
}

void LandscapeStorage::_setup_layout(const Vector2i &p_size) {
	_clear_tiles();
	size = p_size;
	domain = compute_domain(size);
	const int count = compute_mip_count(domain);
	mips.clear();
	mips.resize(count);
	for (int m = 0; m < count; m++) {
		Mip &mip = mips[m];
		// Mip m texel i maps to the mip 0 texel min(i << m, size - 1).
		mip.last = Vector2i(((size.x - 1) + (1 << m) - 1) >> m, ((size.y - 1) + (1 << m) - 1) >> m);
		mip.tiles = Vector2i((mip.last.x >> TILE_SHIFT) + 1, (mip.last.y >> TILE_SHIFT) + 1);
		mip.infos.resize(mip.tiles.x * mip.tiles.y);
		for (TileInfo &info : mip.infos) {
			info = TileInfo();
			info.min_height = default_height;
			info.max_height = default_height;
		}
	}
	for (Rect2i &rect : pending_mips) {
		rect = Rect2i();
	}
}

void LandscapeStorage::_clear_tiles() {
	for (Mip &mip : mips) {
		for (TileInfo &info : mip.infos) {
			if (info.job.is_valid()) {
				if (WorldStreaming::get_singleton()) {
					WorldStreaming::get_singleton()->cancel(info.job);
				}
				info.job.unref();
			}
			if (info.tile) {
				memdelete(info.tile);
				info.tile = nullptr;
				loaded_tiles--;
			}
			info.loaded = 0;
		}
	}
	if (WorldStreaming::get_singleton() && memory_usage != 0) {
		WorldStreaming::get_singleton()->add_pool_usage(SNAME("Landscape CPU"), -memory_usage);
	}
	memory_usage = 0;
}

void LandscapeStorage::init(const Vector2i &p_size, int p_weightmap_count, float p_default_height) {
	if (overlay.is_valid()) {
		const String overlay_path = overlay->get_path();
		overlay->close();
		overlay.unref();
		DirAccess::remove_absolute(overlay_path);
	}
	if (file.is_valid()) {
		file->close();
		file.unref();
	}
	generation++;
	weightmap_count = CLAMP(p_weightmap_count, 1, MAX_WEIGHTMAPS);
	has_holes = false;
	default_height = p_default_height;
	_setup_layout(p_size);
}

bool LandscapeStorage::has_tile(int p_mip, int p_tx, int p_tz) const {
	if (p_mip < 0 || p_mip >= int(mips.size())) {
		return false;
	}
	const Vector2i tiles = mips[p_mip].tiles;
	return p_tx >= 0 && p_tz >= 0 && p_tx < tiles.x && p_tz < tiles.y;
}

void LandscapeStorage::set_weightmap_count(int p_count) {
	p_count = CLAMP(p_count, 1, MAX_WEIGHTMAPS);
	if (p_count == weightmap_count) {
		return;
	}
	weightmap_count = p_count;
	// Loaded tiles get empty (zero) weights for the new weightmaps. Stored tiles are extended when decoded.
	for (Mip &mip : mips) {
		for (TileInfo &info : mip.infos) {
			if (!info.tile || !(info.loaded & MASK_WEIGHTS)) {
				continue;
			}
			const int64_t before = _tile_bytes(info.tile);
			for (int i = 0; i < MAX_WEIGHTMAPS; i++) {
				Vector<uint8_t> &weights = info.tile->weights[i];
				if (i < weightmap_count && weights.is_empty()) {
					weights.resize(TILE_TEXELS * 4);
					memset(weights.ptrw(), 0, weights.size());
				} else if (i >= weightmap_count) {
					weights.clear();
				}
			}
			_update_memory(info, before);
		}
	}
}

void LandscapeStorage::set_has_holes(bool p_holes) {
	has_holes = p_holes;
	if (!has_holes) {
		for (Mip &mip : mips) {
			for (TileInfo &info : mip.infos) {
				if (info.tile && !info.tile->holes.is_empty()) {
					const int64_t before = _tile_bytes(info.tile);
					info.tile->holes.clear();
					info.dirty = true;
					info.version++;
					_update_memory(info, before);
				}
			}
		}
	}
}

/* Tiles */

uint8_t LandscapeStorage::_layer_group(int p_layer) {
	switch (p_layer) {
		case LAYER_HEIGHTS:
			return MASK_HEIGHTS;
		case LAYER_HOLES:
			return MASK_HOLES;
		default:
			return MASK_WEIGHTS;
	}
}

int64_t LandscapeStorage::_tile_bytes(const Tile *p_tile) const {
	if (!p_tile) {
		return 0;
	}
	int64_t bytes = int64_t(sizeof(Tile)) + p_tile->heights.size() * int64_t(sizeof(float)) + p_tile->holes.size();
	for (const Vector<uint8_t> &weights : p_tile->weights) {
		bytes += weights.size();
	}
	return bytes;
}

void LandscapeStorage::_update_memory(TileInfo &r_info, int64_t p_before) {
	const int64_t delta = _tile_bytes(r_info.tile) - p_before;
	memory_usage += delta;
	if (delta != 0 && WorldStreaming::get_singleton()) {
		WorldStreaming::get_singleton()->add_pool_usage(SNAME("Landscape CPU"), delta);
	}
}

void LandscapeStorage::_fill_default(Tile *p_tile, uint8_t p_mask) const {
	if (p_mask & MASK_HEIGHTS) {
		p_tile->heights.resize(TILE_TEXELS);
		float *h = p_tile->heights.ptrw();
		for (int i = 0; i < TILE_TEXELS; i++) {
			h[i] = default_height;
		}
	}
	if (p_mask & MASK_WEIGHTS) {
		for (int i = 0; i < MAX_WEIGHTMAPS; i++) {
			if (i >= weightmap_count) {
				p_tile->weights[i].clear();
				continue;
			}
			p_tile->weights[i].resize(TILE_TEXELS * 4);
			uint8_t *w = p_tile->weights[i].ptrw();
			memset(w, 0, TILE_TEXELS * 4);
			if (i == 0) {
				// Everything is painted with the first layer by default.
				for (int t = 0; t < TILE_TEXELS; t++) {
					w[t * 4] = 255;
				}
			}
		}
	}
	if (p_mask & MASK_HOLES) {
		p_tile->holes.clear();
	}
}

Vector<uint8_t> LandscapeStorage::_read_blob(const Ref<LandscapeStorageFile> &p_file, uint64_t p_offset, uint32_t p_size, uint8_t p_mask) {
	ERR_FAIL_COND_V(p_file.is_null() || p_size < BLOB_HEADER_SIZE, Vector<uint8_t>());
	if ((p_mask & MASK_ALL) == MASK_ALL) {
		return p_file->read(p_offset, p_size);
	}
	// Partial load: only read the chunks of the requested layers.
	const Vector<uint8_t> header = p_file->read(p_offset, BLOB_HEADER_SIZE);
	ERR_FAIL_COND_V(header.size() != int(BLOB_HEADER_SIZE), Vector<uint8_t>());
	uint32_t sizes[3];
	for (int c = 0; c < 3; c++) {
		sizes[c] = decode_uint32(header.ptr() + 4 + c * 4);
	}
	Vector<uint8_t> blob = header;
	uint64_t chunk_offset = p_offset + BLOB_HEADER_SIZE;
	for (int c = 0; c < 3; c++) {
		if ((p_mask & (1 << c)) && sizes[c] > 0) {
			blob.append_array(p_file->read(chunk_offset, sizes[c]));
		} else {
			encode_uint32(0, blob.ptrw() + 4 + c * 4);
		}
		chunk_offset += sizes[c];
	}
	return blob;
}

Vector<uint8_t> LandscapeStorage::_encode_tile(const Tile *p_tile) const {
	Vector<uint8_t> chunks[3];
	auto compress = [](const uint8_t *p_src, int64_t p_size) {
		Vector<uint8_t> out;
		out.resize(Compression::get_max_compressed_buffer_size(p_size, Compression::MODE_ZSTD));
		const int64_t written = Compression::compress(out.ptrw(), p_src, p_size, Compression::MODE_ZSTD);
		out.resize(MAX(written, int64_t(0)));
		return out;
	};

	if (p_tile->heights.size() == TILE_TEXELS) {
		// Byte planes compress much better than interleaved floats.
		Vector<uint8_t> shuffled;
		shuffled.resize(TILE_TEXELS * 4);
		const uint8_t *src = reinterpret_cast<const uint8_t *>(p_tile->heights.ptr());
		uint8_t *dst = shuffled.ptrw();
		for (int i = 0; i < TILE_TEXELS; i++) {
			for (int b = 0; b < 4; b++) {
				dst[b * TILE_TEXELS + i] = src[i * 4 + b];
			}
		}
		chunks[0] = compress(shuffled.ptr(), shuffled.size());
	}
	{
		Vector<uint8_t> weights;
		weights.resize(int64_t(weightmap_count) * TILE_TEXELS * 4);
		for (int i = 0; i < weightmap_count; i++) {
			if (p_tile->weights[i].size() == TILE_TEXELS * 4) {
				memcpy(weights.ptrw() + int64_t(i) * TILE_TEXELS * 4, p_tile->weights[i].ptr(), TILE_TEXELS * 4);
			} else {
				memset(weights.ptrw() + int64_t(i) * TILE_TEXELS * 4, 0, TILE_TEXELS * 4);
			}
		}
		chunks[1] = compress(weights.ptr(), weights.size());
	}
	if (has_holes && p_tile->holes.size() == TILE_TEXELS) {
		chunks[2] = compress(p_tile->holes.ptr(), TILE_TEXELS);
	}

	Vector<uint8_t> blob;
	blob.resize(BLOB_HEADER_SIZE + chunks[0].size() + chunks[1].size() + chunks[2].size());
	uint8_t *w = blob.ptrw();
	memset(w, 0, BLOB_HEADER_SIZE);
	w[0] = uint8_t((chunks[0].is_empty() ? 0 : MASK_HEIGHTS) | MASK_WEIGHTS | (chunks[2].is_empty() ? 0 : MASK_HOLES));
	w[1] = uint8_t(weightmap_count);
	uint32_t offset = BLOB_HEADER_SIZE;
	for (int c = 0; c < 3; c++) {
		encode_uint32(chunks[c].size(), w + 4 + c * 4);
		if (!chunks[c].is_empty()) {
			memcpy(w + offset, chunks[c].ptr(), chunks[c].size());
			offset += chunks[c].size();
		}
	}
	return blob;
}

bool LandscapeStorage::_decode_tile(const Vector<uint8_t> &p_blob, uint8_t p_mask, int p_weightmap_count, Tile &r_tile, uint8_t &r_mask) {
	ERR_FAIL_COND_V(p_blob.size() < int(BLOB_HEADER_SIZE), false);
	const uint8_t *src = p_blob.ptr();
	const int blob_weightmaps = src[1];
	uint32_t sizes[3];
	for (int c = 0; c < 3; c++) {
		sizes[c] = decode_uint32(src + 4 + c * 4);
	}
	r_mask = 0;
	uint32_t offset = BLOB_HEADER_SIZE;
	for (int c = 0; c < 3; c++) {
		const uint8_t group = uint8_t(1 << c);
		if (!(p_mask & group)) {
			offset += sizes[c];
			continue;
		}
		ERR_FAIL_COND_V(offset + sizes[c] > uint32_t(p_blob.size()), false);
		const uint8_t *chunk = src + offset;
		offset += sizes[c];
		if (c == 0) {
			ERR_FAIL_COND_V_MSG(sizes[c] == 0, false, "Landscape tile without heights.");
			Vector<uint8_t> shuffled;
			shuffled.resize(TILE_TEXELS * 4);
			ERR_FAIL_COND_V(Compression::decompress(shuffled.ptrw(), shuffled.size(), chunk, sizes[c], Compression::MODE_ZSTD) != TILE_TEXELS * 4, false);
			r_tile.heights.resize(TILE_TEXELS);
			uint8_t *dst = reinterpret_cast<uint8_t *>(r_tile.heights.ptrw());
			const uint8_t *s = shuffled.ptr();
			for (int i = 0; i < TILE_TEXELS; i++) {
				for (int b = 0; b < 4; b++) {
					dst[i * 4 + b] = s[b * TILE_TEXELS + i];
				}
			}
		} else if (c == 1) {
			Vector<uint8_t> weights;
			const int64_t raw_size = int64_t(blob_weightmaps) * TILE_TEXELS * 4;
			weights.resize(raw_size);
			if (raw_size > 0) {
				ERR_FAIL_COND_V(Compression::decompress(weights.ptrw(), raw_size, chunk, sizes[c], Compression::MODE_ZSTD) != raw_size, false);
			}
			for (int i = 0; i < MAX_WEIGHTMAPS; i++) {
				if (i >= p_weightmap_count) {
					r_tile.weights[i].clear();
					continue;
				}
				r_tile.weights[i].resize(TILE_TEXELS * 4);
				if (i < blob_weightmaps) {
					memcpy(r_tile.weights[i].ptrw(), weights.ptr() + int64_t(i) * TILE_TEXELS * 4, TILE_TEXELS * 4);
				} else {
					memset(r_tile.weights[i].ptrw(), 0, TILE_TEXELS * 4);
				}
			}
		} else {
			if (sizes[c] == 0) {
				r_tile.holes.clear();
			} else {
				r_tile.holes.resize(TILE_TEXELS);
				ERR_FAIL_COND_V(Compression::decompress(r_tile.holes.ptrw(), TILE_TEXELS, chunk, sizes[c], Compression::MODE_ZSTD) != TILE_TEXELS, false);
			}
		}
		r_mask |= group;
	}
	return true;
}

void LandscapeStorage::_ensure_layers(TileInfo &r_info, uint8_t p_mask) {
	const uint8_t needed = p_mask & ~r_info.loaded & MASK_ALL;
	if (!needed) {
		return;
	}
	if (!r_info.tile) {
		r_info.tile = memnew(Tile);
		loaded_tiles++;
	}
	const int64_t before = _tile_bytes(r_info.tile);
	bool loaded = false;
	if (r_info.source != TileInfo::SOURCE_DEFAULT) {
		const Ref<LandscapeStorageFile> &source = r_info.source == TileInfo::SOURCE_FILE ? file : overlay;
		const Vector<uint8_t> blob = _read_blob(source, r_info.offset, r_info.size, needed);
		Tile decoded;
		uint8_t decoded_mask = 0;
		if (!blob.is_empty() && _decode_tile(blob, needed, weightmap_count, decoded, decoded_mask) && decoded_mask == needed) {
			if (needed & MASK_HEIGHTS) {
				r_info.tile->heights = decoded.heights;
			}
			if (needed & MASK_WEIGHTS) {
				for (int i = 0; i < MAX_WEIGHTMAPS; i++) {
					r_info.tile->weights[i] = decoded.weights[i];
				}
			}
			if (needed & MASK_HOLES) {
				r_info.tile->holes = decoded.holes;
			}
			loaded = true;
		} else {
			ERR_PRINT(vformat("Failed to read a landscape tile from '%s', using default values.", source.is_valid() ? source->get_path() : String()));
		}
	}
	if (!loaded) {
		_fill_default(r_info.tile, needed);
	}
	r_info.loaded |= needed;
	_update_memory(r_info, before);
}

LandscapeStorage::Tile *LandscapeStorage::_load(int p_mip, int p_tx, int p_tz, uint8_t p_mask) {
	TileInfo &info = _info(p_mip, p_tx, p_tz);
	_ensure_layers(info, p_mask);
	info.last_used = ++use_counter;
	return info.tile;
}

const LandscapeStorage::Tile *LandscapeStorage::get_tile(int p_mip, int p_tx, int p_tz, uint8_t p_mask) {
	ERR_FAIL_COND_V(!has_tile(p_mip, p_tx, p_tz), nullptr);
	if (p_mip > 0) {
		_flush_mips();
	}
	return _load(p_mip, p_tx, p_tz, p_mask);
}

const LandscapeStorage::Tile *LandscapeStorage::request_tile(int p_mip, int p_tx, int p_tz, uint8_t p_mask, float p_priority) {
	ERR_FAIL_COND_V(!has_tile(p_mip, p_tx, p_tz), nullptr);
	if (p_mip > 0) {
		_flush_mips();
	}
	TileInfo &info = _info(p_mip, p_tx, p_tz);
	const uint8_t needed = p_mask & ~info.loaded & MASK_ALL;
	if (!needed) {
		info.last_used = ++use_counter;
		return info.tile;
	}
	if (info.source == TileInfo::SOURCE_DEFAULT || !WorldStreaming::get_singleton()) {
		return _load(p_mip, p_tx, p_tz, p_mask); // Constant content, no I/O.
	}
	if (info.job.is_valid()) {
		if ((info.job->mask & needed) == needed) {
			WorldStreaming::get_singleton()->set_job_priority(info.job, p_priority);
			return nullptr;
		}
		if (info.job->is_pending()) {
			return nullptr; // Request the other layers once this load is done.
		}
		info.job.unref();
	}

	Ref<LoadJob> job;
	job.instantiate();
	job->owner = owner;
	job->file = info.source == TileInfo::SOURCE_FILE ? file : overlay;
	job->offset = info.offset;
	job->size = info.size;
	job->mip = p_mip;
	job->tile = Vector2i(p_tx, p_tz);
	job->mask = needed;
	job->generation = generation;
	job->version = info.version;
	job->weightmap_count = weightmap_count;
	info.job = job;
	WorldStreaming::get_singleton()->submit(job, p_priority);
	return nullptr;
}

void LandscapeStorage::LoadJob::run() {
	const Vector<uint8_t> blob = _read_blob(file, offset, size, mask);
	ok = !blob.is_empty() && _decode_tile(blob, mask, weightmap_count, result, result_mask) && result_mask == mask;
}

void LandscapeStorage::LoadJob::finish() {
	LandscapeData *data = ObjectDB::get_instance<LandscapeData>(owner);
	if (data) {
		data->get_storage()._finish_load(this);
	}
}

void LandscapeStorage::_finish_load(LoadJob *p_job) {
	if (p_job->generation != generation || !has_tile(p_job->mip, p_job->tile.x, p_job->tile.y)) {
		return;
	}
	TileInfo &info = _info(p_job->mip, p_job->tile.x, p_job->tile.y);
	if (info.job.ptr() != p_job) {
		return;
	}
	info.job.unref();
	if (!p_job->ok) {
		ERR_PRINT("Failed to stream a landscape tile.");
		return;
	}
	if (info.version != p_job->version) {
		return; // Modified while loading, the loaded data may be stale.
	}
	const uint8_t groups = p_job->result_mask & ~info.loaded;
	if (!groups) {
		return;
	}
	if (!info.tile) {
		info.tile = memnew(Tile);
		loaded_tiles++;
	}
	const int64_t before = _tile_bytes(info.tile);
	if (groups & MASK_HEIGHTS) {
		info.tile->heights = p_job->result.heights;
	}
	if (groups & MASK_WEIGHTS) {
		for (int i = 0; i < MAX_WEIGHTMAPS; i++) {
			// The weightmap count may have changed while loading.
			Vector<uint8_t> weights = i < weightmap_count ? p_job->result.weights[i] : Vector<uint8_t>();
			if (i < weightmap_count && weights.size() != TILE_TEXELS * 4) {
				weights.resize(TILE_TEXELS * 4);
				memset(weights.ptrw(), 0, weights.size());
			}
			info.tile->weights[i] = weights;
		}
	}
	if (groups & MASK_HOLES) {
		info.tile->holes = p_job->result.holes;
	}
	info.loaded |= groups;
	info.last_used = ++use_counter;
	_update_memory(info, before);
}

uint32_t LandscapeStorage::get_tile_version(int p_mip, int p_tx, int p_tz) const {
	ERR_FAIL_COND_V(!has_tile(p_mip, p_tx, p_tz), 0);
	return _info(p_mip, p_tx, p_tz).version;
}

void LandscapeStorage::pin_tile(int p_mip, int p_tx, int p_tz) {
	ERR_FAIL_COND(!has_tile(p_mip, p_tx, p_tz));
	_info(p_mip, p_tx, p_tz).pins++;
}

void LandscapeStorage::unpin_tile(int p_mip, int p_tx, int p_tz) {
	ERR_FAIL_COND(!has_tile(p_mip, p_tx, p_tz));
	TileInfo &info = _info(p_mip, p_tx, p_tz);
	ERR_FAIL_COND(info.pins == 0);
	info.pins--;
}

void LandscapeStorage::wait_for_loads() {
	WorldStreaming *ws = WorldStreaming::get_singleton();
	if (!ws) {
		return;
	}
	for (Mip &mip : mips) {
		for (TileInfo &info : mip.infos) {
			if (info.job.is_valid()) {
				Ref<LoadJob> job = info.job;
				ws->wait(job);
				if (info.job == job) {
					info.job.unref();
				}
			}
		}
	}
}

/* Eviction */

void LandscapeStorage::_compute_range(TileInfo &r_info) {
	if (r_info.range_valid) {
		return;
	}
	ERR_FAIL_COND(!r_info.tile || !(r_info.loaded & MASK_HEIGHTS));
	// Only the valid texels count. The tile is a mip 0 tile, find its position from its address.
	const Mip &mip = mips[0];
	const int64_t index = &r_info - mip.infos.ptr();
	const int tx = int(index % mip.tiles.x);
	const int tz = int(index / mip.tiles.x);
	const int w = MIN(TILE_SIZE, mip.last.x - (tx << TILE_SHIFT) + 1);
	const int h = MIN(TILE_SIZE, mip.last.y - (tz << TILE_SHIFT) + 1);
	float mn = Math::INF;
	float mx = -Math::INF;
	const float *heights = r_info.tile->heights.ptr();
	for (int z = 0; z < h; z++) {
		for (int x = 0; x < w; x++) {
			const float v = heights[z * TILE_SIZE + x];
			mn = MIN(mn, v);
			mx = MAX(mx, v);
		}
	}
	r_info.min_height = mn;
	r_info.max_height = mx;
	r_info.range_valid = true;
}

void LandscapeStorage::_spill(TileInfo &r_info) {
	_ensure_layers(r_info, MASK_ALL);
	if (overlay.is_null()) {
		String dir = OS::get_singleton()->get_cache_path();
		if (dir.is_empty() || !DirAccess::dir_exists_absolute(dir)) {
			dir = OS::get_singleton()->get_user_data_dir();
		}
		const String path = dir.path_join(vformat("godot_landscape_%d_%d.tmp", OS::get_singleton()->get_process_id(), uint64_t(owner)));
		overlay.instantiate();
		const Error err = overlay->open(path, FileAccess::WRITE_READ);
		if (err != OK) {
			overlay.unref();
			ERR_FAIL_MSG(vformat("Can't create the landscape overlay file '%s', modified tiles are kept in memory.", path));
		}
	}
	const Vector<uint8_t> blob = _encode_tile(r_info.tile);
	r_info.offset = overlay->append(blob.ptr(), blob.size());
	r_info.size = blob.size();
	r_info.source = TileInfo::SOURCE_OVERLAY;
	r_info.dirty = false;
}

void LandscapeStorage::_evict(TileInfo &r_info) {
	if (!r_info.tile) {
		return;
	}
	if (&r_info >= mips[0].infos.ptr() && &r_info < mips[0].infos.ptr() + mips[0].infos.size() && (r_info.loaded & MASK_HEIGHTS)) {
		_compute_range(r_info);
	}
	if (r_info.dirty) {
		_spill(r_info);
		if (r_info.dirty) {
			return; // The overlay file isn't available, keep the tile.
		}
	}
	const int64_t before = _tile_bytes(r_info.tile);
	memdelete(r_info.tile);
	r_info.tile = nullptr;
	r_info.loaded = 0;
	loaded_tiles--;
	memory_usage -= before;
	if (WorldStreaming::get_singleton()) {
		WorldStreaming::get_singleton()->add_pool_usage(SNAME("Landscape CPU"), -before);
	}
}

void LandscapeStorage::trim() {
	if (memory_usage <= memory_budget) {
		return;
	}
	struct Candidate {
		uint64_t last_used;
		TileInfo *info;
		bool operator<(const Candidate &p_other) const { return last_used < p_other.last_used; }
	};
	LocalVector<Candidate> candidates;
	for (Mip &mip : mips) {
		for (TileInfo &info : mip.infos) {
			if (info.tile && info.pins == 0) {
				candidates.push_back({ info.last_used, &info });
			}
		}
	}
	candidates.sort();
	const int64_t target = memory_budget - memory_budget / 8;
	for (const Candidate &candidate : candidates) {
		if (memory_usage <= target) {
			break;
		}
		_evict(*candidate.info);
	}
}

/* Regions */

void LandscapeStorage::read_region(int p_layer, int p_mip, const Rect2i &p_rect, void *r_data) {
	ERR_FAIL_INDEX(p_layer, LAYER_MAX);
	ERR_FAIL_INDEX(p_mip, int(mips.size()));
	if (!p_rect.has_area()) {
		return;
	}
	if (p_mip > 0) {
		_flush_mips();
	}
	const int texel_size = _layer_texel_size(p_layer);
	const uint8_t group = _layer_group(p_layer);
	const Mip &mip = mips[p_mip];
	uint8_t *dst = static_cast<uint8_t *>(r_data);

	if (p_layer >= LAYER_WEIGHTS_0 && p_layer <= LAYER_WEIGHTS_3 && p_layer - LAYER_WEIGHTS_0 >= weightmap_count) {
		memset(dst, 0, int64_t(p_rect.size.x) * p_rect.size.y * texel_size);
		return;
	}

	// Clamped core region, read tile by tile, then replicated to the requested rect.
	const int cx0 = CLAMP(p_rect.position.x, 0, mip.last.x);
	const int cz0 = CLAMP(p_rect.position.y, 0, mip.last.y);
	const int cx1 = CLAMP(p_rect.get_end().x - 1, 0, mip.last.x);
	const int cz1 = CLAMP(p_rect.get_end().y - 1, 0, mip.last.y);
	const int cw = cx1 - cx0 + 1;
	const int ch = cz1 - cz0 + 1;
	const bool direct = cx0 == p_rect.position.x && cz0 == p_rect.position.y && cw == p_rect.size.x && ch == p_rect.size.y;
	Vector<uint8_t> core_buffer;
	uint8_t *core = dst;
	if (!direct) {
		core_buffer.resize(int64_t(cw) * ch * texel_size);
		core = core_buffer.ptrw();
	}

	for (int tz = cz0 >> TILE_SHIFT; tz <= cz1 >> TILE_SHIFT; tz++) {
		for (int tx = cx0 >> TILE_SHIFT; tx <= cx1 >> TILE_SHIFT; tx++) {
			const Tile *tile = _load(p_mip, tx, tz, group);
			const uint8_t *src = nullptr;
			switch (p_layer) {
				case LAYER_HEIGHTS:
					src = reinterpret_cast<const uint8_t *>(tile->heights.ptr());
					break;
				case LAYER_HOLES:
					src = tile->holes.is_empty() ? nullptr : tile->holes.ptr();
					break;
				default:
					src = tile->weights[p_layer - LAYER_WEIGHTS_0].ptr();
					break;
			}
			const int x0 = MAX(cx0, tx << TILE_SHIFT);
			const int x1 = MIN(cx1, (tx << TILE_SHIFT) + TILE_MASK);
			const int z0 = MAX(cz0, tz << TILE_SHIFT);
			const int z1 = MIN(cz1, (tz << TILE_SHIFT) + TILE_MASK);
			const int row_bytes = (x1 - x0 + 1) * texel_size;
			for (int z = z0; z <= z1; z++) {
				uint8_t *row = core + (int64_t(z - cz0) * cw + (x0 - cx0)) * texel_size;
				if (src) {
					memcpy(row, src + ((z & TILE_MASK) * TILE_SIZE + (x0 & TILE_MASK)) * texel_size, row_bytes);
				} else {
					memset(row, 0, row_bytes);
				}
			}
		}
	}

	if (direct) {
		return;
	}
	for (int z = 0; z < p_rect.size.y; z++) {
		const int sz = CLAMP(p_rect.position.y + z, cz0, cz1) - cz0;
		const uint8_t *src_row = core + int64_t(sz) * cw * texel_size;
		uint8_t *dst_row = dst + int64_t(z) * p_rect.size.x * texel_size;
		for (int x = 0; x < p_rect.size.x;) {
			const int gx = p_rect.position.x + x;
			if (gx < cx0 || gx > cx1) {
				memcpy(dst_row + x * texel_size, src_row + (gx < cx0 ? 0 : cw - 1) * texel_size, texel_size);
				x++;
			} else {
				const int count = MIN(cx1 - gx + 1, p_rect.size.x - x);
				memcpy(dst_row + x * texel_size, src_row + (gx - cx0) * texel_size, count * texel_size);
				x += count;
			}
		}
	}
}

void LandscapeStorage::_update_mip_region(int p_mip, const Rect2i &p_rect, uint8_t p_groups) {
	// p_rect is in texels of p_mip, inside the valid range.
	static constexpr int BAND = 64;
	for (int bz = p_rect.position.y; bz < p_rect.get_end().y; bz += BAND) {
		const Rect2i target(p_rect.position.x, bz, p_rect.size.x, MIN(BAND, p_rect.get_end().y - bz));
		const Rect2i source(target.position.x * 2 - 1, target.position.y * 2 - 1, target.size.x * 2 + 1, target.size.y * 2 + 1);
		const int sw = source.size.x;
		const int64_t target_count = int64_t(target.size.x) * target.size.y;
		const int64_t source_count = int64_t(source.size.x) * source.size.y;

		if (p_groups & MASK_HEIGHTS) {
			// Point sampling: the even source texels (index 1 of the source rect is 2 * x).
			Vector<float> src;
			src.resize(source_count);
			read_region(LAYER_HEIGHTS, p_mip - 1, source, src.ptrw());
			Vector<float> dst;
			dst.resize(target_count);
			float *d = dst.ptrw();
			const float *s = src.ptr();
			for (int z = 0; z < target.size.y; z++) {
				for (int x = 0; x < target.size.x; x++) {
					d[z * target.size.x + x] = s[(z * 2 + 1) * sw + x * 2 + 1];
				}
			}
			_write_mip_region(LAYER_HEIGHTS, p_mip, target, dst.ptr());
		}

		auto tent = [&](int p_layer, int p_channels) {
			Vector<uint8_t> src;
			src.resize(source_count * p_channels);
			read_region(p_layer, p_mip - 1, source, src.ptrw());
			// Separable [1 2 1] filter: horizontal pass on every source row, then vertical.
			const int tw = target.size.x;
			const int row_values = tw * p_channels;
			LocalVector<uint16_t> horizontal;
			horizontal.resize(source.size.y * row_values);
			const uint8_t *s = src.ptr();
			for (int z = 0; z < source.size.y; z++) {
				const uint8_t *row = s + int64_t(z) * sw * p_channels;
				uint16_t *out = horizontal.ptr() + z * row_values;
				for (int x = 0; x < tw; x++) {
					const uint8_t *t = row + x * 2 * p_channels;
					for (int c = 0; c < p_channels; c++) {
						out[x * p_channels + c] = uint16_t(t[c] + 2 * t[p_channels + c] + t[2 * p_channels + c]);
					}
				}
			}
			Vector<uint8_t> dst;
			dst.resize(target_count * p_channels);
			uint8_t *d = dst.ptrw();
			bool any = false;
			for (int z = 0; z < target.size.y; z++) {
				const uint16_t *r0 = horizontal.ptr() + (z * 2) * row_values;
				const uint16_t *r1 = r0 + row_values;
				const uint16_t *r2 = r1 + row_values;
				uint8_t *out = d + int64_t(z) * row_values;
				for (int i = 0; i < row_values; i++) {
					const uint8_t v = uint8_t((r0[i] + 2 * r1[i] + r2[i] + 8) >> 4);
					out[i] = v;
					any = any || v != 0;
				}
			}
			if (p_layer == LAYER_HOLES && !any) {
				// Keep tiles without holes empty.
				_write_mip_region(p_layer, p_mip, target, nullptr);
			} else {
				_write_mip_region(p_layer, p_mip, target, dst.ptr());
			}
		};
		if (p_groups & MASK_WEIGHTS) {
			for (int i = 0; i < weightmap_count; i++) {
				tent(LAYER_WEIGHTS_0 + i, 4);
			}
		}
		if ((p_groups & MASK_HOLES) && has_holes) {
			tent(LAYER_HOLES, 1);
		}
		trim();
	}
}

void LandscapeStorage::_flush_mips() {
	Rect2i pending[3];
	bool any = false;
	for (int g = 0; g < 3; g++) {
		pending[g] = pending_mips[g];
		pending_mips[g] = Rect2i();
		any = any || pending[g].has_area();
	}
	if (!any) {
		return;
	}
	for (int g = 0; g < 3; g++) {
		Rect2i rect = pending[g];
		if (!rect.has_area()) {
			continue;
		}
		for (int m = 1; m < int(mips.size()); m++) {
			// Texels of mip m whose tent footprint (or point sample) touches the changed texels.
			const Vector2i last = mips[m].last;
			const int x0 = CLAMP((rect.position.x - 1) >> 1, 0, last.x);
			const int z0 = CLAMP((rect.position.y - 1) >> 1, 0, last.y);
			const int x1 = CLAMP((rect.get_end().x - 1 + 2) >> 1, 0, last.x);
			const int z1 = CLAMP((rect.get_end().y - 1 + 2) >> 1, 0, last.y);
			rect = Rect2i(x0, z0, x1 - x0 + 1, z1 - z0 + 1);
			_update_mip_region(m, rect, uint8_t(1 << g));
		}
	}
}

void LandscapeStorage::_write_mip_region(int p_layer, int p_mip, const Rect2i &p_rect, const void *p_data) {
	const Mip &mip = mips[p_mip];
	ERR_FAIL_COND(p_rect.position.x < 0 || p_rect.position.y < 0 || p_rect.get_end().x - 1 > mip.last.x || p_rect.get_end().y - 1 > mip.last.y);
	const int texel_size = _layer_texel_size(p_layer);
	const uint8_t group = _layer_group(p_layer);
	const uint8_t *src = static_cast<const uint8_t *>(p_data);
	if (p_layer >= LAYER_WEIGHTS_0 && p_layer <= LAYER_WEIGHTS_3) {
		ERR_FAIL_COND(p_layer - LAYER_WEIGHTS_0 >= weightmap_count);
	}

	const int x_end = p_rect.get_end().x - 1;
	const int z_end = p_rect.get_end().y - 1;
	for (int tz = p_rect.position.y >> TILE_SHIFT; tz <= z_end >> TILE_SHIFT; tz++) {
		for (int tx = p_rect.position.x >> TILE_SHIFT; tx <= x_end >> TILE_SHIFT; tx++) {
			TileInfo &info = _info(p_mip, tx, tz);
			const int x0 = MAX(p_rect.position.x, tx << TILE_SHIFT);
			const int x1 = MIN(x_end, (tx << TILE_SHIFT) + TILE_MASK);
			const int z0 = MAX(p_rect.position.y, tz << TILE_SHIFT);
			const int z1 = MIN(z_end, (tz << TILE_SHIFT) + TILE_MASK);
			const int row_bytes = (x1 - x0 + 1) * texel_size;

			Tile *tile = _load(p_mip, tx, tz, group);
			const int64_t before = _tile_bytes(tile);
			uint8_t *dst = nullptr;
			switch (p_layer) {
				case LAYER_HEIGHTS:
					dst = reinterpret_cast<uint8_t *>(tile->heights.ptrw());
					break;
				case LAYER_HOLES: {
					if (!src && tile->holes.is_empty()) {
						continue; // Already solid.
					}
					if (tile->holes.is_empty()) {
						tile->holes.resize(TILE_TEXELS);
						memset(tile->holes.ptrw(), 0, TILE_TEXELS);
					}
					dst = tile->holes.ptrw();
				} break;
				default:
					dst = tile->weights[p_layer - LAYER_WEIGHTS_0].ptrw();
					break;
			}
			for (int z = z0; z <= z1; z++) {
				uint8_t *row = dst + ((z & TILE_MASK) * TILE_SIZE + (x0 & TILE_MASK)) * texel_size;
				if (src) {
					memcpy(row, src + (int64_t(z - p_rect.position.y) * p_rect.size.x + (x0 - p_rect.position.x)) * texel_size, row_bytes);
				} else {
					memset(row, 0, row_bytes);
				}
			}
			info.dirty = true;
			info.version++;
			if (p_mip == 0 && p_layer == LAYER_HEIGHTS) {
				info.range_valid = false;
			}
			_update_memory(info, before);
		}
	}
	unsaved = true;
}

void LandscapeStorage::write_region(int p_layer, const Rect2i &p_rect, const void *p_data) {
	ERR_FAIL_INDEX(p_layer, LAYER_MAX);
	if (!p_rect.has_area()) {
		return;
	}
	if (p_layer == LAYER_HOLES && !has_holes) {
		set_has_holes(true);
	}
	_write_mip_region(p_layer, 0, p_rect, p_data);
	_mark_pending(_layer_group(p_layer), p_rect);
}

void LandscapeStorage::_mark_pending(uint8_t p_group, const Rect2i &p_rect) {
	const int g = p_group == MASK_HEIGHTS ? 0 : (p_group == MASK_WEIGHTS ? 1 : 2);
	pending_mips[g] = pending_mips[g].has_area() ? pending_mips[g].merge(p_rect) : p_rect;
	unsaved = true;
}

void LandscapeStorage::mark_all_dirty(uint8_t p_groups) {
	for (int g = 0; g < 3; g++) {
		if (p_groups & (1 << g)) {
			_mark_pending(uint8_t(1 << g), Rect2i(Point2i(), size));
		}
	}
}

/* Texels */

LandscapeStorage::TileInfo &LandscapeStorage::_texel_info(int p_x, int p_z, uint8_t p_mask) {
	TileInfo &info = _info(0, p_x >> TILE_SHIFT, p_z >> TILE_SHIFT);
	if ((info.loaded & p_mask) != p_mask) {
		_ensure_layers(info, p_mask);
	}
	info.last_used = ++use_counter;
	return info;
}

float LandscapeStorage::get_height(int p_x, int p_z) {
	ERR_FAIL_COND_V(mips.is_empty(), 0.0);
	p_x = CLAMP(p_x, 0, size.x - 1);
	p_z = CLAMP(p_z, 0, size.y - 1);
	const TileInfo &info = _texel_info(p_x, p_z, MASK_HEIGHTS);
	return info.tile->heights[(p_z & TILE_MASK) * TILE_SIZE + (p_x & TILE_MASK)];
}

void LandscapeStorage::set_height(int p_x, int p_z, float p_height) {
	ERR_FAIL_INDEX(p_x, size.x);
	ERR_FAIL_INDEX(p_z, size.y);
	TileInfo &info = _texel_info(p_x, p_z, MASK_HEIGHTS);
	info.tile->heights.write[(p_z & TILE_MASK) * TILE_SIZE + (p_x & TILE_MASK)] = p_height;
	info.dirty = true;
	info.version++;
	info.range_valid = false;
	_mark_pending(MASK_HEIGHTS, Rect2i(p_x, p_z, 1, 1));
}

void LandscapeStorage::get_weights(int p_x, int p_z, uint8_t *r_weights) {
	p_x = CLAMP(p_x, 0, size.x - 1);
	p_z = CLAMP(p_z, 0, size.y - 1);
	const TileInfo &info = _texel_info(p_x, p_z, MASK_WEIGHTS);
	const int offset = ((p_z & TILE_MASK) * TILE_SIZE + (p_x & TILE_MASK)) * 4;
	for (int i = 0; i < weightmap_count; i++) {
		memcpy(r_weights + i * 4, info.tile->weights[i].ptr() + offset, 4);
	}
}

void LandscapeStorage::set_weights(int p_x, int p_z, const uint8_t *p_weights) {
	ERR_FAIL_INDEX(p_x, size.x);
	ERR_FAIL_INDEX(p_z, size.y);
	TileInfo &info = _texel_info(p_x, p_z, MASK_WEIGHTS);
	const int offset = ((p_z & TILE_MASK) * TILE_SIZE + (p_x & TILE_MASK)) * 4;
	for (int i = 0; i < weightmap_count; i++) {
		memcpy(info.tile->weights[i].ptrw() + offset, p_weights + i * 4, 4);
	}
	info.dirty = true;
	info.version++;
	_mark_pending(MASK_WEIGHTS, Rect2i(p_x, p_z, 1, 1));
}

bool LandscapeStorage::is_hole(int p_x, int p_z) {
	if (!has_holes || mips.is_empty()) {
		return false;
	}
	p_x = CLAMP(p_x, 0, size.x - 1);
	p_z = CLAMP(p_z, 0, size.y - 1);
	const TileInfo &info = _texel_info(p_x, p_z, MASK_HOLES);
	return !info.tile->holes.is_empty() && info.tile->holes[(p_z & TILE_MASK) * TILE_SIZE + (p_x & TILE_MASK)] >= 128;
}

void LandscapeStorage::set_hole(int p_x, int p_z, bool p_hole) {
	ERR_FAIL_INDEX(p_x, size.x);
	ERR_FAIL_INDEX(p_z, size.y);
	if (!p_hole && !has_holes) {
		return;
	}
	if (!has_holes) {
		set_has_holes(true);
	}
	TileInfo &info = _texel_info(p_x, p_z, MASK_HOLES);
	if (info.tile->holes.is_empty()) {
		if (!p_hole) {
			return;
		}
		const int64_t before = _tile_bytes(info.tile);
		info.tile->holes.resize(TILE_TEXELS);
		memset(info.tile->holes.ptrw(), 0, TILE_TEXELS);
		_update_memory(info, before);
	}
	info.tile->holes.write[(p_z & TILE_MASK) * TILE_SIZE + (p_x & TILE_MASK)] = p_hole ? 255 : 0;
	info.dirty = true;
	info.version++;
	_mark_pending(MASK_HOLES, Rect2i(p_x, p_z, 1, 1));
}

float LandscapeStorage::get_mip_height(int p_mip, int p_x, int p_z) {
	ERR_FAIL_INDEX_V(p_mip, int(mips.size()), 0.0);
	if (p_mip > 0) {
		_flush_mips();
	}
	const Vector2i last = mips[p_mip].last;
	p_x = CLAMP(p_x, 0, last.x);
	p_z = CLAMP(p_z, 0, last.y);
	const Tile *tile = _load(p_mip, p_x >> TILE_SHIFT, p_z >> TILE_SHIFT, MASK_HEIGHTS);
	return tile->heights[(p_z & TILE_MASK) * TILE_SIZE + (p_x & TILE_MASK)];
}

Vector2 LandscapeStorage::get_height_range() {
	if (mips.is_empty()) {
		return Vector2();
	}
	float mn = Math::INF;
	float mx = -Math::INF;
	for (TileInfo &info : mips[0].infos) {
		if (!info.range_valid) {
			_ensure_layers(info, MASK_HEIGHTS);
			_compute_range(info);
		}
		mn = MIN(mn, info.min_height);
		mx = MAX(mx, info.max_height);
	}
	return Vector2(mn, mx);
}

bool LandscapeStorage::has_unsaved_changes() const {
	return unsaved;
}

/* Files */

Error LandscapeStorage::open_file(const String &p_path, Dictionary &r_header) {
	Ref<LandscapeStorageFile> new_file;
	new_file.instantiate();
	Error err = new_file->open(p_path, FileAccess::READ);
	ERR_FAIL_COND_V_MSG(err != OK, err, vformat("Can't open landscape data file '%s'.", p_path));

	const Vector<uint8_t> header = new_file->read(0, FILE_HEADER_SIZE);
	ERR_FAIL_COND_V_MSG(header.size() != int(FILE_HEADER_SIZE), ERR_FILE_CORRUPT, vformat("Invalid landscape data file '%s'.", p_path));
	const uint8_t *h = header.ptr();
	ERR_FAIL_COND_V_MSG(decode_uint32(h) != FILE_MAGIC, ERR_FILE_UNRECOGNIZED, vformat("Invalid landscape data file '%s'.", p_path));
	ERR_FAIL_COND_V_MSG(decode_uint32(h + 4) > FILE_VERSION, ERR_FILE_UNRECOGNIZED, vformat("Landscape data file '%s' was created by a newer version.", p_path));
	const Vector2i file_size(decode_uint32(h + 8), decode_uint32(h + 12));
	const int file_weightmaps = decode_uint32(h + 16);
	const uint32_t flags = decode_uint32(h + 20);
	const float file_default_height = decode_float(h + 24);
	const uint32_t tile_shift = decode_uint32(h + 28);
	const uint32_t mip_count = decode_uint32(h + 32);
	const uint64_t meta_offset = decode_uint64(h + 40);
	const uint32_t meta_size = decode_uint32(h + 48);
	const uint32_t meta_raw_size = decode_uint32(h + 52);
	const uint64_t index_offset = decode_uint64(h + 56);
	const uint32_t index_count = decode_uint32(h + 64);
	ERR_FAIL_COND_V(file_size.x < 2 || file_size.y < 2 || file_size.x > 16384 || file_size.y > 16384, ERR_FILE_CORRUPT);
	ERR_FAIL_COND_V(tile_shift != TILE_SHIFT, ERR_FILE_UNRECOGNIZED);
	ERR_FAIL_COND_V(int(mip_count) != compute_mip_count(compute_domain(file_size)), ERR_FILE_CORRUPT);

	// Reset the storage with the new layout.
	init(file_size, file_weightmaps, file_default_height);
	has_holes = (flags & 1) != 0;

	uint32_t expected = 0;
	for (const Mip &mip : mips) {
		expected += mip.infos.size();
	}
	ERR_FAIL_COND_V(index_count != expected, ERR_FILE_CORRUPT);
	const Vector<uint8_t> index = new_file->read(index_offset, index_count * INDEX_ENTRY_SIZE);
	ERR_FAIL_COND_V(index.size() != int64_t(index_count) * INDEX_ENTRY_SIZE, ERR_FILE_CORRUPT);
	const uint8_t *e = index.ptr();
	for (Mip &mip : mips) {
		for (TileInfo &info : mip.infos) {
			info.offset = decode_uint64(e);
			info.size = decode_uint32(e + 8);
			info.min_height = decode_float(e + 12);
			info.max_height = decode_float(e + 16);
			info.source = (decode_uint32(e + 20) & INDEX_FLAG_DATA) ? TileInfo::SOURCE_FILE : TileInfo::SOURCE_DEFAULT;
			info.range_valid = true;
			e += INDEX_ENTRY_SIZE;
		}
	}

	r_header.clear();
	if (meta_size > 0) {
		const Vector<uint8_t> packed = new_file->read(meta_offset, meta_size);
		Vector<uint8_t> raw;
		raw.resize(meta_raw_size);
		if (packed.size() == int(meta_size) && Compression::decompress(raw.ptrw(), meta_raw_size, packed.ptr(), meta_size, Compression::MODE_ZSTD) == int64_t(meta_raw_size)) {
			Variant meta;
			if (decode_variant(meta, raw.ptr(), raw.size()) == OK && meta.get_type() == Variant::DICTIONARY) {
				r_header = meta;
			}
		}
	}

	file = new_file;
	unsaved = false;
	return OK;
}

Error LandscapeStorage::save_file(const String &p_path, const Dictionary &p_header) {
	ERR_FAIL_COND_V(mips.is_empty(), ERR_UNCONFIGURED);
	wait_for_loads();
	_flush_mips();

	const String temp_path = p_path + ".tmp";
	Error err = OK;
	{
		Ref<FileAccess> out = FileAccess::open(temp_path, FileAccess::WRITE, &err);
		ERR_FAIL_COND_V_MSG(out.is_null(), err, vformat("Can't write landscape data file '%s'.", temp_path));

		Vector<uint8_t> header;
		header.resize(FILE_HEADER_SIZE);
		memset(header.ptrw(), 0, FILE_HEADER_SIZE);
		out->store_buffer(header);

		struct Entry {
			uint64_t offset = 0;
			uint32_t size = 0;
			float min_height = 0.0;
			float max_height = 0.0;
			uint32_t flags = 0;
		};
		LocalVector<Entry> entries;
		for (int m = 0; m < int(mips.size()); m++) {
			for (TileInfo &info : mips[m].infos) {
				Entry entry;
				if (m == 0 && !info.range_valid) {
					_ensure_layers(info, MASK_HEIGHTS);
					_compute_range(info);
				}
				entry.min_height = info.min_height;
				entry.max_height = info.max_height;

				Vector<uint8_t> blob;
				if (info.tile && info.dirty) {
					_ensure_layers(info, MASK_ALL);
					blob = _encode_tile(info.tile);
				} else if (info.source == TileInfo::SOURCE_FILE) {
					blob = file->read(info.offset, info.size);
				} else if (info.source == TileInfo::SOURCE_OVERLAY) {
					blob = overlay->read(info.offset, info.size);
				}
				if (!blob.is_empty()) {
					entry.offset = out->get_position();
					entry.size = blob.size();
					entry.flags = INDEX_FLAG_DATA;
					out->store_buffer(blob);
				}
				entries.push_back(entry);
			}
			trim();
		}

		Vector<uint8_t> meta_packed;
		uint32_t meta_raw_size = 0;
		{
			int len = 0;
			encode_variant(p_header, nullptr, len);
			Vector<uint8_t> raw;
			raw.resize(len);
			encode_variant(p_header, raw.ptrw(), len);
			meta_raw_size = len;
			meta_packed.resize(Compression::get_max_compressed_buffer_size(len, Compression::MODE_ZSTD));
			meta_packed.resize(Compression::compress(meta_packed.ptrw(), raw.ptr(), len, Compression::MODE_ZSTD));
		}
		const uint64_t meta_offset = out->get_position();
		out->store_buffer(meta_packed);

		const uint64_t index_offset = out->get_position();
		Vector<uint8_t> index;
		index.resize(entries.size() * INDEX_ENTRY_SIZE);
		uint8_t *e = index.ptrw();
		for (const Entry &entry : entries) {
			encode_uint64(entry.offset, e);
			encode_uint32(entry.size, e + 8);
			encode_float(entry.min_height, e + 12);
			encode_float(entry.max_height, e + 16);
			encode_uint32(entry.flags, e + 20);
			e += INDEX_ENTRY_SIZE;
		}
		out->store_buffer(index);

		uint8_t *hw = header.ptrw();
		encode_uint32(FILE_MAGIC, hw);
		encode_uint32(FILE_VERSION, hw + 4);
		encode_uint32(size.x, hw + 8);
		encode_uint32(size.y, hw + 12);
		encode_uint32(weightmap_count, hw + 16);
		encode_uint32(has_holes ? 1 : 0, hw + 20);
		encode_float(default_height, hw + 24);
		encode_uint32(TILE_SHIFT, hw + 28);
		encode_uint32(mips.size(), hw + 32);
		encode_uint32(domain, hw + 36);
		encode_uint64(meta_offset, hw + 40);
		encode_uint32(meta_packed.size(), hw + 48);
		encode_uint32(meta_raw_size, hw + 52);
		encode_uint64(index_offset, hw + 56);
		encode_uint32(entries.size(), hw + 64);
		out->seek(0);
		out->store_buffer(header);
		ERR_FAIL_COND_V_MSG(out->get_error() != OK && out->get_error() != ERR_FILE_EOF, ERR_FILE_CANT_WRITE, vformat("Failed to write landscape data file '%s'.", temp_path));

		// Switch the tiles to their new location.
		uint32_t i = 0;
		for (Mip &mip : mips) {
			for (TileInfo &info : mip.infos) {
				const Entry &entry = entries[i++];
				info.source = (entry.flags & INDEX_FLAG_DATA) ? TileInfo::SOURCE_FILE : TileInfo::SOURCE_DEFAULT;
				info.offset = entry.offset;
				info.size = entry.size;
				info.dirty = false;
			}
		}
	}

	// Replace the destination (which may be the file being read).
	if (file.is_valid()) {
		file->close();
	}
	file.unref();
	if (FileAccess::exists(p_path)) {
		DirAccess::remove_absolute(p_path);
	}
	err = DirAccess::rename_absolute(temp_path, p_path);
	ERR_FAIL_COND_V_MSG(err != OK, err, vformat("Can't replace landscape data file '%s'.", p_path));

	file.instantiate();
	err = file->open(p_path, FileAccess::READ);
	if (err != OK) {
		file.unref();
		ERR_FAIL_V_MSG(err, vformat("Can't reopen landscape data file '%s'.", p_path));
	}
	if (overlay.is_valid()) {
		const String overlay_path = overlay->get_path();
		overlay->close();
		overlay.unref();
		DirAccess::remove_absolute(overlay_path);
	}
	generation++;
	unsaved = false;
	return OK;
}

LandscapeStorage::LandscapeStorage() {
}

LandscapeStorage::~LandscapeStorage() {
	_clear_tiles();
	if (overlay.is_valid()) {
		const String overlay_path = overlay->get_path();
		overlay->close();
		overlay.unref();
		DirAccess::remove_absolute(overlay_path);
	}
}
