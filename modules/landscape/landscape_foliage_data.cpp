/**************************************************************************/
/*  landscape_foliage_data.cpp                                            */
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

#include "landscape_foliage_data.h"

#include "landscape_storage.h"

#include "core/io/compression.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/marshalls.h"
#include "core/object/class_db.h"

// Compressed instance arrays: header, then the zstd-compressed byte planes of the floats.
static constexpr uint32_t INSTANCES_MAGIC = 0x494F464C; // "LFOI"
static constexpr uint32_t INSTANCES_VERSION = 1;
static constexpr int INSTANCES_HEADER = 16;
static constexpr int INSTANCE_FLOATS = int(sizeof(LandscapeFoliageInstance) / sizeof(float));
static constexpr uint32_t MAX_INSTANCES = 1u << 27;

// .lfdata files: header, compressed instances of the cells, compressed index.
static constexpr uint32_t FILE_MAGIC = 0x4644464C; // "LFDF"
static constexpr uint32_t FILE_VERSION = 1;
static constexpr int FILE_HEADER_SIZE = 64;
static constexpr int INDEX_ENTRY_SIZE = 52;

/* Instance */

Transform3D LandscapeFoliageInstance::get_transform() const {
	return Transform3D(xform[0], xform[1], xform[2], xform[4], xform[5], xform[6], xform[8], xform[9], xform[10], xform[3], xform[7], xform[11]);
}

void LandscapeFoliageInstance::set_transform(const Transform3D &p_transform) {
	const Basis &b = p_transform.basis;
	xform[0] = b.rows[0].x;
	xform[1] = b.rows[0].y;
	xform[2] = b.rows[0].z;
	xform[3] = p_transform.origin.x;
	xform[4] = b.rows[1].x;
	xform[5] = b.rows[1].y;
	xform[6] = b.rows[1].z;
	xform[7] = p_transform.origin.y;
	xform[8] = b.rows[2].x;
	xform[9] = b.rows[2].y;
	xform[10] = b.rows[2].z;
	xform[11] = p_transform.origin.z;
}

Vector3 LandscapeFoliageInstance::get_align_normal() const {
	const float y2 = 1.0f - normal_x * normal_x - normal_z * normal_z;
	return Vector3(normal_x, Math::sqrt(MAX(y2, 0.0f)), normal_z);
}

void LandscapeFoliageInstance::set_align_normal(const Vector3 &p_normal) {
	Vector3 n = p_normal.normalized();
	if (n.y < 0.0) {
		n = -n;
	}
	normal_x = n.x;
	normal_z = n.z;
}

PackedByteArray LandscapeFoliageInstance::encode(const LandscapeFoliageInstance *p_instances, uint32_t p_count) {
	PackedByteArray result;
	if (p_count == 0) {
		return result;
	}
	// Byte planes of the floats compress much better than interleaved floats.
	const int64_t floats = int64_t(p_count) * INSTANCE_FLOATS;
	Vector<uint8_t> shuffled;
	shuffled.resize(floats * 4);
	const uint8_t *src = reinterpret_cast<const uint8_t *>(p_instances);
	uint8_t *dst = shuffled.ptrw();
	for (int64_t i = 0; i < floats; i++) {
		for (int b = 0; b < 4; b++) {
			dst[b * floats + i] = src[i * 4 + b];
		}
	}
	result.resize(INSTANCES_HEADER + Compression::get_max_compressed_buffer_size(shuffled.size(), Compression::MODE_ZSTD));
	uint8_t *w = result.ptrw();
	encode_uint32(INSTANCES_MAGIC, w);
	encode_uint32(INSTANCES_VERSION, w + 4);
	encode_uint32(p_count, w + 8);
	encode_uint32(INSTANCE_FLOATS, w + 12);
	const int64_t written = Compression::compress(w + INSTANCES_HEADER, shuffled.ptr(), shuffled.size(), Compression::MODE_ZSTD);
	ERR_FAIL_COND_V(written < 0, PackedByteArray());
	result.resize(INSTANCES_HEADER + written);
	return result;
}

bool LandscapeFoliageInstance::decode(const uint8_t *p_data, int64_t p_size, LocalVector<LandscapeFoliageInstance> &r_instances) {
	r_instances.clear();
	if (p_size == 0) {
		return true;
	}
	ERR_FAIL_COND_V_MSG(p_size < INSTANCES_HEADER, false, "Invalid foliage instance data.");
	ERR_FAIL_COND_V_MSG(decode_uint32(p_data) != INSTANCES_MAGIC, false, "Invalid foliage instance data.");
	ERR_FAIL_COND_V_MSG(decode_uint32(p_data + 4) > INSTANCES_VERSION, false, "The foliage instance data was saved by a newer version.");
	const uint32_t count = decode_uint32(p_data + 8);
	ERR_FAIL_COND_V_MSG(decode_uint32(p_data + 12) != uint32_t(INSTANCE_FLOATS), false, "Invalid foliage instance data.");
	ERR_FAIL_COND_V_MSG(count > MAX_INSTANCES, false, "Corrupted foliage instance data.");
	const int64_t floats = int64_t(count) * INSTANCE_FLOATS;
	Vector<uint8_t> shuffled;
	shuffled.resize(floats * 4);
	const int64_t size = Compression::decompress(shuffled.ptrw(), shuffled.size(), p_data + INSTANCES_HEADER, p_size - INSTANCES_HEADER, Compression::MODE_ZSTD);
	ERR_FAIL_COND_V_MSG(size != shuffled.size(), false, "Corrupted foliage instance data.");
	r_instances.resize(count);
	uint8_t *dst = reinterpret_cast<uint8_t *>(r_instances.ptr());
	const uint8_t *s = shuffled.ptr();
	for (int64_t i = 0; i < floats; i++) {
		for (int b = 0; b < 4; b++) {
			dst[i * 4 + b] = s[b * floats + i];
		}
	}
	return true;
}

bool LandscapeFoliageInstance::decode(const PackedByteArray &p_data, LocalVector<LandscapeFoliageInstance> &r_instances) {
	return decode(p_data.ptr(), p_data.size(), r_instances);
}

static AABB _foliage_positions_bounds(const LandscapeFoliageInstance *p_instances, uint32_t p_count) {
	if (p_count == 0) {
		return AABB();
	}
	AABB bounds(p_instances[0].get_position(), Vector3());
	for (uint32_t i = 1; i < p_count; i++) {
		bounds.expand_to(p_instances[i].get_position());
	}
	return bounds;
}

static _FORCE_INLINE_ Vector3i _foliage_cell_id(int p_layer, const Vector2i &p_key) {
	return Vector3i(p_key.x, p_key.y, p_layer);
}

/* Loads */

void LandscapeFoliageData::LoadJob::run() {
	const Vector<uint8_t> blob = file.is_valid() ? file->read(offset, size) : Vector<uint8_t>();
	ok = !blob.is_empty() && LandscapeFoliageInstance::decode(blob.ptr(), blob.size(), result);
}

void LandscapeFoliageData::LoadJob::finish() {
	LandscapeFoliageData *data = ObjectDB::get_instance<LandscapeFoliageData>(owner);
	if (data) {
		data->_finish_load(this);
	}
}

void LandscapeFoliageData::_finish_load(LoadJob *p_job) {
	if (p_job->generation != generation) {
		return;
	}
	const Vector3i id = _foliage_cell_id(p_job->layer, p_job->key);
	Ref<LoadJob> *current = jobs.getptr(id);
	if (!current || current->ptr() != p_job) {
		return;
	}
	jobs.erase(id);
	if (!p_job->ok) {
		// Loaded empty, so that it isn't requested again and again.
		ERR_PRINT(vformat("Failed to stream the foliage cell (%d, %d) of layer %d.", p_job->key.x, p_job->key.y, p_job->layer));
		p_job->result.clear();
	}
	_update_memory(0, int64_t(p_job->result.size()) * sizeof(Instance));
	loaded[id] = std::move(p_job->result);
}

void LandscapeFoliageData::_update_memory(int64_t p_pending_delta, int64_t p_loaded_delta) {
	pending_bytes += p_pending_delta;
	loaded_bytes += p_loaded_delta;
	if (WorldStreaming::get_singleton() && (p_pending_delta != 0 || p_loaded_delta != 0)) {
		WorldStreaming::get_singleton()->add_pool_usage(SNAME("Foliage CPU"), p_pending_delta + p_loaded_delta);
	}
}

void LandscapeFoliageData::_cancel_jobs() {
	for (KeyValue<Vector3i, Ref<LoadJob>> &kv : jobs) {
		if (WorldStreaming::get_singleton()) {
			WorldStreaming::get_singleton()->cancel(kv.value);
		}
	}
	jobs.clear();
	int64_t bytes = 0;
	for (const KeyValue<Vector3i, LocalVector<Instance>> &kv : loaded) {
		bytes += int64_t(kv.value.size()) * sizeof(Instance);
	}
	loaded.clear();
	_update_memory(0, -bytes);
	generation++;
}

void LandscapeFoliageData::wait_for_loads() {
	if (!WorldStreaming::get_singleton()) {
		return;
	}
	LocalVector<Ref<LoadJob>> pending_jobs;
	for (const KeyValue<Vector3i, Ref<LoadJob>> &kv : jobs) {
		pending_jobs.push_back(kv.value);
	}
	for (const Ref<LoadJob> &job : pending_jobs) {
		WorldStreaming::get_singleton()->wait(job);
	}
}

/* Layers */

LandscapeFoliageData::Layer *LandscapeFoliageData::_get_layer(int p_layer) {
	ERR_FAIL_COND_V(p_layer < 0 || p_layer > 4096, nullptr);
	if (p_layer >= int(layers.size())) {
		layers.resize(p_layer + 1);
	}
	return &layers[p_layer];
}

void LandscapeFoliageData::set_layer_count(int p_count) {
	ERR_FAIL_COND(p_count < 0);
	if (p_count == int(layers.size())) {
		return;
	}
	while (int(layers.size()) > p_count) {
		remove_layer(layers.size() - 1);
	}
	layers.resize(p_count);
	unsaved = true;
}

void LandscapeFoliageData::insert_layer(int p_index) {
	ERR_FAIL_INDEX(p_index, int(layers.size()) + 1);
	_cancel_jobs(); // Loads are addressed by layer index.
	layers.insert(p_index, Layer());
	unsaved = true;
}

void LandscapeFoliageData::remove_layer(int p_index) {
	ERR_FAIL_INDEX(p_index, int(layers.size()));
	_cancel_jobs();
	clear_layer(p_index);
	layers.remove_at(p_index);
	unsaved = true;
}

void LandscapeFoliageData::move_layer(int p_from, int p_to) {
	ERR_FAIL_INDEX(p_from, int(layers.size()));
	ERR_FAIL_INDEX(p_to, int(layers.size()));
	if (p_from == p_to) {
		return;
	}
	_cancel_jobs();
	Layer layer = std::move(layers[p_from]);
	layers.remove_at(p_from);
	layers.insert(p_to, std::move(layer));
	unsaved = true;
}

void LandscapeFoliageData::clear_layer(int p_layer) {
	ERR_FAIL_INDEX(p_layer, int(layers.size()));
	Layer &layer = layers[p_layer];
	int64_t bytes = 0;
	for (const KeyValue<Vector2i, LocalVector<Instance>> &kv : layer.pending) {
		bytes += int64_t(kv.value.size()) * sizeof(Instance);
		cancel_request(p_layer, kv.key);
	}
	for (const KeyValue<Vector2i, CellInfo> &kv : layer.cells) {
		cancel_request(p_layer, kv.key);
	}
	layer.pending.clear();
	layer.cells.clear();
	_update_memory(-bytes, 0);
	unsaved = true;
}

void LandscapeFoliageData::set_chunk_size(float p_size) {
	p_size = CLAMP(p_size, 4.0f, 4096.0f);
	if (Math::is_equal_approx(p_size, chunk_size)) {
		return;
	}
	// Every instance is sorted into the new cells (in memory until saved).
	LocalVector<LocalVector<Instance>> all;
	all.resize(layers.size());
	for (uint32_t l = 0; l < layers.size(); l++) {
		LocalVector<Vector2i> keys;
		for (const KeyValue<Vector2i, CellInfo> &kv : layers[l].cells) {
			keys.push_back(kv.key);
		}
		for (const Vector2i &key : keys) {
			LocalVector<Instance> instances;
			read_cell(l, key, instances);
			for (const Instance &instance : instances) {
				all[l].push_back(instance);
			}
		}
	}
	for (uint32_t l = 0; l < layers.size(); l++) {
		clear_layer(l);
	}
	chunk_size = p_size;
	for (uint32_t l = 0; l < layers.size(); l++) {
		HashMap<Vector2i, LocalVector<Instance>> cells;
		for (const Instance &instance : all[l]) {
			const Vector3 p = instance.get_position();
			cells[Vector2i(int(Math::floor(p.x / chunk_size)), int(Math::floor(p.z / chunk_size)))].push_back(instance);
		}
		for (const KeyValue<Vector2i, LocalVector<Instance>> &kv : cells) {
			write_cell(l, kv.key, kv.value.ptr(), kv.value.size());
		}
	}
	unsaved = true;
}

/* Cells */

const HashMap<Vector2i, LandscapeFoliageData::CellInfo> *LandscapeFoliageData::get_cells(int p_layer) const {
	if (p_layer < 0 || p_layer >= int(layers.size())) {
		return nullptr;
	}
	return &layers[p_layer].cells;
}

int64_t LandscapeFoliageData::get_instance_count(int p_layer) const {
	int64_t count = 0;
	for (uint32_t l = 0; l < layers.size(); l++) {
		if (p_layer >= 0 && int(l) != p_layer) {
			continue;
		}
		for (const KeyValue<Vector2i, CellInfo> &kv : layers[l].cells) {
			count += kv.value.count;
		}
	}
	return count;
}

int LandscapeFoliageData::get_cell_count(int p_layer) const {
	int count = 0;
	for (uint32_t l = 0; l < layers.size(); l++) {
		if (p_layer < 0 || int(l) == p_layer) {
			count += layers[l].cells.size();
		}
	}
	return count;
}

bool LandscapeFoliageData::read_cell(int p_layer, const Vector2i &p_key, LocalVector<Instance> &r_instances) {
	r_instances.clear();
	if (p_layer < 0 || p_layer >= int(layers.size())) {
		return false;
	}
	Layer &layer = layers[p_layer];
	const LocalVector<Instance> *pending = layer.pending.getptr(p_key);
	if (pending) {
		r_instances = *pending;
		return true;
	}
	const CellInfo *info = layer.cells.getptr(p_key);
	if (!info) {
		return false;
	}
	const Vector3i id = _foliage_cell_id(p_layer, p_key);
	Ref<LoadJob> *job = jobs.getptr(id);
	if (job && WorldStreaming::get_singleton()) {
		WorldStreaming::get_singleton()->wait(*job); // Calls finish(), which stores the result.
	}
	LocalVector<Instance> *result = loaded.getptr(id);
	if (result) {
		_update_memory(0, -int64_t(result->size()) * sizeof(Instance));
		r_instances = std::move(*result);
		loaded.erase(id);
		return true;
	}
	if (!info->in_file || file.is_null()) {
		return info->count == 0;
	}
	const Vector<uint8_t> blob = file->read(info->offset, info->size);
	ERR_FAIL_COND_V_MSG(blob.is_empty(), false, "Failed to read a foliage cell.");
	return LandscapeFoliageInstance::decode(blob.ptr(), blob.size(), r_instances);
}

bool LandscapeFoliageData::request_cell(int p_layer, const Vector2i &p_key, float p_priority, LocalVector<Instance> &r_instances) {
	if (p_layer < 0 || p_layer >= int(layers.size())) {
		r_instances.clear();
		return true;
	}
	Layer &layer = layers[p_layer];
	const CellInfo *info = layer.cells.getptr(p_key);
	const Vector3i id = _foliage_cell_id(p_layer, p_key);
	if (layer.pending.has(p_key) || !info || !info->in_file || file.is_null() || !WorldStreaming::get_singleton() || loaded.has(id)) {
		read_cell(p_layer, p_key, r_instances);
		return true;
	}
	Ref<LoadJob> *current = jobs.getptr(id);
	if (current) {
		WorldStreaming::get_singleton()->set_job_priority(*current, p_priority);
		return false;
	}
	Ref<LoadJob> job;
	job.instantiate();
	job->owner = get_instance_id();
	job->file = file;
	job->offset = info->offset;
	job->size = info->size;
	job->layer = p_layer;
	job->key = p_key;
	job->generation = generation;
	jobs[id] = job;
	WorldStreaming::get_singleton()->submit(job, p_priority);
	return false;
}

void LandscapeFoliageData::cancel_request(int p_layer, const Vector2i &p_key) {
	const Vector3i id = _foliage_cell_id(p_layer, p_key);
	Ref<LoadJob> *job = jobs.getptr(id);
	if (job) {
		if (WorldStreaming::get_singleton()) {
			WorldStreaming::get_singleton()->cancel(*job);
		}
		jobs.erase(id);
	}
	LocalVector<Instance> *result = loaded.getptr(id);
	if (result) {
		_update_memory(0, -int64_t(result->size()) * sizeof(Instance));
		loaded.erase(id);
	}
}

void LandscapeFoliageData::write_cell(int p_layer, const Vector2i &p_key, const Instance *p_instances, uint32_t p_count) {
	Layer *layer = _get_layer(p_layer);
	ERR_FAIL_NULL(layer);
	cancel_request(p_layer, p_key); // A load in progress would be stale.
	int64_t delta = 0;
	LocalVector<Instance> *pending = layer->pending.getptr(p_key);
	if (pending) {
		delta -= int64_t(pending->size()) * sizeof(Instance);
	}
	if (p_count == 0) {
		layer->pending.erase(p_key);
		layer->cells.erase(p_key);
	} else {
		if (!pending) {
			pending = &layer->pending.insert(p_key, LocalVector<Instance>())->value;
		}
		pending->resize(p_count);
		memcpy(pending->ptr(), p_instances, p_count * sizeof(Instance));
		delta += int64_t(p_count) * sizeof(Instance);
		CellInfo &info = layer->cells[p_key];
		info.count = p_count;
		info.bounds = _foliage_positions_bounds(p_instances, p_count);
	}
	_update_memory(delta, 0);
	unsaved = true;
}

/* Files */

bool LandscapeFoliageData::is_streamed() const {
	return file.is_valid() && file->is_open();
}

void LandscapeFoliageData::add_flush_callback(const Callable &p_callback) {
	if (!flush_callbacks.has(p_callback)) {
		flush_callbacks.push_back(p_callback);
	}
}

void LandscapeFoliageData::remove_flush_callback(const Callable &p_callback) {
	flush_callbacks.erase(p_callback);
}

Error LandscapeFoliageData::save_to_file(const String &p_path) {
	// The owners write their edited cells first.
	for (const Callable &callback : LocalVector<Callable>(flush_callbacks)) {
		if (callback.is_valid()) {
			callback.call();
		}
	}
	wait_for_loads();

	struct Entry {
		int layer = 0;
		Vector2i key;
		CellInfo info;
	};
	LocalVector<Entry> entries;
	const String temp_path = p_path + ".tmp";
	Error err = OK;
	{
		Ref<FileAccess> out = FileAccess::open(temp_path, FileAccess::WRITE, &err);
		ERR_FAIL_COND_V_MSG(out.is_null(), err, vformat("Can't write foliage data file '%s'.", temp_path));
		Vector<uint8_t> header;
		header.resize(FILE_HEADER_SIZE);
		memset(header.ptrw(), 0, FILE_HEADER_SIZE);
		out->store_buffer(header);

		for (uint32_t l = 0; l < layers.size(); l++) {
			Layer &layer = layers[l];
			// Sorted, so that the same instances are always saved the same way.
			LocalVector<Vector2i> keys;
			for (const KeyValue<Vector2i, CellInfo> &kv : layer.cells) {
				keys.push_back(kv.key);
			}
			keys.sort();
			for (const Vector2i &key : keys) {
				const CellInfo &info = layer.cells[key];
				Vector<uint8_t> blob;
				const LocalVector<Instance> *pending = layer.pending.getptr(key);
				if (pending) {
					blob = LandscapeFoliageInstance::encode(pending->ptr(), pending->size());
				} else if (info.in_file && file.is_valid()) {
					blob = file->read(info.offset, info.size);
				}
				if (blob.is_empty()) {
					ERR_PRINT(vformat("Failed to save a foliage cell of layer %d.", l));
					continue;
				}
				Entry entry;
				entry.layer = l;
				entry.key = key;
				entry.info = info;
				entry.info.offset = out->get_position();
				entry.info.size = blob.size();
				entry.info.in_file = true;
				out->store_buffer(blob);
				entries.push_back(entry);
			}
		}

		Vector<uint8_t> index;
		index.resize(entries.size() * INDEX_ENTRY_SIZE);
		uint8_t *e = index.ptrw();
		for (const Entry &entry : entries) {
			encode_uint32(entry.layer, e);
			encode_uint32(uint32_t(entry.key.x), e + 4);
			encode_uint32(uint32_t(entry.key.y), e + 8);
			encode_uint32(entry.info.count, e + 12);
			encode_uint64(entry.info.offset, e + 16);
			encode_uint32(entry.info.size, e + 24);
			const Vector3 end = entry.info.bounds.get_end();
			const float bounds[6] = { float(entry.info.bounds.position.x), float(entry.info.bounds.position.y), float(entry.info.bounds.position.z), float(end.x), float(end.y), float(end.z) };
			for (int i = 0; i < 6; i++) {
				encode_float(bounds[i], e + 28 + i * 4);
			}
			e += INDEX_ENTRY_SIZE;
		}
		Vector<uint8_t> packed;
		packed.resize(Compression::get_max_compressed_buffer_size(MAX(index.size(), 1), Compression::MODE_ZSTD));
		packed.resize(MAX(Compression::compress(packed.ptrw(), index.ptr(), index.size(), Compression::MODE_ZSTD), int64_t(0)));
		const uint64_t index_offset = out->get_position();
		out->store_buffer(packed);

		uint8_t *hw = header.ptrw();
		encode_uint32(FILE_MAGIC, hw);
		encode_uint32(FILE_VERSION, hw + 4);
		encode_uint32(layers.size(), hw + 8);
		encode_float(chunk_size, hw + 12);
		encode_uint64(index_offset, hw + 16);
		encode_uint32(packed.size(), hw + 24);
		encode_uint32(index.size(), hw + 28);
		encode_uint32(entries.size(), hw + 32);
		out->seek(0);
		out->store_buffer(header);
		ERR_FAIL_COND_V_MSG(out->get_error() != OK && out->get_error() != ERR_FILE_EOF, ERR_FILE_CANT_WRITE, vformat("Failed to write foliage data file '%s'.", temp_path));
	}

	// Replace the destination (which may be the file being read).
	if (file.is_valid()) {
		file->close();
		file.unref();
	}
	if (FileAccess::exists(p_path)) {
		DirAccess::remove_absolute(p_path);
	}
	err = DirAccess::rename_absolute(temp_path, p_path);
	ERR_FAIL_COND_V_MSG(err != OK, err, vformat("Can't replace foliage data file '%s'.", p_path));
	file.instantiate();
	err = file->open(p_path, FileAccess::READ);
	if (err != OK) {
		file.unref();
		ERR_FAIL_V_MSG(err, vformat("Can't reopen foliage data file '%s'.", p_path));
	}

	// Every cell is in the new file now.
	int64_t pending_freed = 0;
	for (Layer &layer : layers) {
		for (const KeyValue<Vector2i, LocalVector<Instance>> &kv : layer.pending) {
			pending_freed += int64_t(kv.value.size()) * sizeof(Instance);
		}
		layer.pending.clear();
		layer.cells.clear();
	}
	for (const Entry &entry : entries) {
		layers[entry.layer].cells[entry.key] = entry.info;
	}
	_update_memory(-pending_freed, 0);
	generation++; // The finished loads stay valid: they read the same content.
	unsaved = false;
	return OK;
}

Error LandscapeFoliageData::load_from_file(const String &p_path) {
	Ref<LandscapeStorageFile> new_file;
	new_file.instantiate();
	Error err = new_file->open(p_path, FileAccess::READ);
	ERR_FAIL_COND_V_MSG(err != OK, err, vformat("Can't open foliage data file '%s'.", p_path));
	const Vector<uint8_t> header = new_file->read(0, FILE_HEADER_SIZE);
	ERR_FAIL_COND_V_MSG(header.size() != FILE_HEADER_SIZE, ERR_FILE_CORRUPT, vformat("Invalid foliage data file '%s'.", p_path));
	const uint8_t *h = header.ptr();
	ERR_FAIL_COND_V_MSG(decode_uint32(h) != FILE_MAGIC, ERR_FILE_UNRECOGNIZED, vformat("Invalid foliage data file '%s'.", p_path));
	ERR_FAIL_COND_V_MSG(decode_uint32(h + 4) > FILE_VERSION, ERR_FILE_UNRECOGNIZED, vformat("Foliage data file '%s' was created by a newer version.", p_path));
	const uint32_t layer_count = decode_uint32(h + 8);
	const float file_chunk_size = decode_float(h + 12);
	const uint64_t index_offset = decode_uint64(h + 16);
	const uint32_t index_size = decode_uint32(h + 24);
	const uint32_t index_raw_size = decode_uint32(h + 28);
	const uint32_t entry_count = decode_uint32(h + 32);
	ERR_FAIL_COND_V(layer_count > 4096 || file_chunk_size < 1.0 || index_raw_size != entry_count * INDEX_ENTRY_SIZE, ERR_FILE_CORRUPT);

	Vector<uint8_t> index;
	index.resize(index_raw_size);
	if (index_raw_size > 0) {
		const Vector<uint8_t> packed = new_file->read(index_offset, index_size);
		ERR_FAIL_COND_V_MSG(packed.size() != int64_t(index_size), ERR_FILE_CORRUPT, vformat("Invalid foliage data file '%s'.", p_path));
		ERR_FAIL_COND_V_MSG(Compression::decompress(index.ptrw(), index.size(), packed.ptr(), packed.size(), Compression::MODE_ZSTD) != int64_t(index_raw_size), ERR_FILE_CORRUPT, vformat("Invalid foliage data file '%s'.", p_path));
	}

	_cancel_jobs();
	int64_t pending_freed = 0;
	for (const Layer &layer : layers) {
		for (const KeyValue<Vector2i, LocalVector<Instance>> &kv : layer.pending) {
			pending_freed += int64_t(kv.value.size()) * sizeof(Instance);
		}
	}
	_update_memory(-pending_freed, 0);
	layers.clear();
	layers.resize(layer_count);
	chunk_size = file_chunk_size;
	const uint8_t *e = index.ptr();
	for (uint32_t i = 0; i < entry_count; i++, e += INDEX_ENTRY_SIZE) {
		const uint32_t layer = decode_uint32(e);
		ERR_CONTINUE(layer >= layer_count);
		const Vector2i key(int32_t(decode_uint32(e + 4)), int32_t(decode_uint32(e + 8)));
		CellInfo info;
		info.count = decode_uint32(e + 12);
		info.offset = decode_uint64(e + 16);
		info.size = decode_uint32(e + 24);
		const Vector3 begin(decode_float(e + 28), decode_float(e + 32), decode_float(e + 36));
		const Vector3 end(decode_float(e + 40), decode_float(e + 44), decode_float(e + 48));
		info.bounds = AABB(begin, end - begin);
		info.in_file = true;
		layers[layer].cells[key] = info;
	}
	file = new_file;
	generation++;
	unsaved = false;
	emit_changed();
	return OK;
}

/* Embedded serialization */

void LandscapeFoliageData::_set_layers_data(const Array &p_layers) {
	_cancel_jobs();
	for (uint32_t l = 0; l < layers.size(); l++) {
		clear_layer(l);
	}
	if (file.is_valid()) {
		file->close();
		file.unref();
	}
	layers.clear();
	layers.resize(p_layers.size());
	for (int l = 0; l < p_layers.size(); l++) {
		LocalVector<Instance> instances;
		ERR_CONTINUE(!LandscapeFoliageInstance::decode(PackedByteArray(p_layers[l]), instances));
		HashMap<Vector2i, LocalVector<Instance>> cells;
		for (const Instance &instance : instances) {
			const Vector3 p = instance.get_position();
			cells[Vector2i(int(Math::floor(p.x / chunk_size)), int(Math::floor(p.z / chunk_size)))].push_back(instance);
		}
		for (const KeyValue<Vector2i, LocalVector<Instance>> &kv : cells) {
			write_cell(l, kv.key, kv.value.ptr(), kv.value.size());
		}
	}
	unsaved = false;
}

Array LandscapeFoliageData::_get_layers_data() const {
	// Every cell of every layer (loads the cells that aren't in memory).
	LandscapeFoliageData *self = const_cast<LandscapeFoliageData *>(this);
	for (const Callable &callback : LocalVector<Callable>(flush_callbacks)) {
		if (callback.is_valid()) {
			callback.call();
		}
	}
	Array result;
	for (uint32_t l = 0; l < layers.size(); l++) {
		LocalVector<Vector2i> keys;
		for (const KeyValue<Vector2i, CellInfo> &kv : layers[l].cells) {
			keys.push_back(kv.key);
		}
		keys.sort();
		LocalVector<Instance> all;
		for (const Vector2i &key : keys) {
			LocalVector<Instance> instances;
			self->read_cell(l, key, instances);
			for (const Instance &instance : instances) {
				all.push_back(instance);
			}
		}
		result.push_back(LandscapeFoliageInstance::encode(all.ptr(), all.size()));
	}
	return result;
}

void LandscapeFoliageData::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_chunk_size", "size"), &LandscapeFoliageData::set_chunk_size);
	ClassDB::bind_method(D_METHOD("get_chunk_size"), &LandscapeFoliageData::get_chunk_size);
	ClassDB::bind_method(D_METHOD("get_layer_count"), &LandscapeFoliageData::get_layer_count);
	ClassDB::bind_method(D_METHOD("get_instance_count", "layer"), &LandscapeFoliageData::get_instance_count, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("get_cell_count", "layer"), &LandscapeFoliageData::get_cell_count, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("is_streamed"), &LandscapeFoliageData::is_streamed);
	ClassDB::bind_method(D_METHOD("has_unsaved_changes"), &LandscapeFoliageData::has_unsaved_changes);
	ClassDB::bind_method(D_METHOD("get_memory_usage"), &LandscapeFoliageData::get_memory_usage);
	ClassDB::bind_method(D_METHOD("_set_layers_data", "layers"), &LandscapeFoliageData::_set_layers_data);
	ClassDB::bind_method(D_METHOD("_get_layers_data"), &LandscapeFoliageData::_get_layers_data);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "chunk_size", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), "set_chunk_size", "get_chunk_size");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "_layers_data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR | PROPERTY_USAGE_INTERNAL), "_set_layers_data", "_get_layers_data");
}

LandscapeFoliageData::LandscapeFoliageData() {
}

LandscapeFoliageData::~LandscapeFoliageData() {
	_cancel_jobs();
	int64_t bytes = 0;
	for (const Layer &layer : layers) {
		for (const KeyValue<Vector2i, LocalVector<Instance>> &kv : layer.pending) {
			bytes += int64_t(kv.value.size()) * sizeof(Instance);
		}
	}
	_update_memory(-bytes, 0);
	if (file.is_valid()) {
		file->close();
	}
}

/* Loader and saver */

Ref<Resource> ResourceFormatLoaderLandscapeFoliageData::load(const String &p_path, const String &p_original_path, Error *r_error, bool p_use_sub_threads, float *r_progress, CacheMode p_cache_mode) {
	Ref<LandscapeFoliageData> data;
	data.instantiate();
	const Error err = data->load_from_file(p_path);
	if (r_error) {
		*r_error = err;
	}
	if (err != OK) {
		return Ref<Resource>();
	}
	return data;
}

void ResourceFormatLoaderLandscapeFoliageData::get_recognized_extensions(List<String> *p_extensions) const {
	p_extensions->push_back("lfdata");
}

bool ResourceFormatLoaderLandscapeFoliageData::handles_type(const String &p_type) const {
	return p_type == "LandscapeFoliageData";
}

String ResourceFormatLoaderLandscapeFoliageData::get_resource_type(const String &p_path) const {
	return p_path.get_extension().to_lower() == "lfdata" ? "LandscapeFoliageData" : "";
}

Error ResourceFormatSaverLandscapeFoliageData::save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags) {
	Ref<LandscapeFoliageData> data = p_resource;
	ERR_FAIL_COND_V(data.is_null(), ERR_INVALID_PARAMETER);
	return data->save_to_file(p_path);
}

bool ResourceFormatSaverLandscapeFoliageData::recognize(const Ref<Resource> &p_resource) const {
	return Object::cast_to<LandscapeFoliageData>(p_resource.ptr()) != nullptr;
}

void ResourceFormatSaverLandscapeFoliageData::get_recognized_extensions(const Ref<Resource> &p_resource, List<String> *p_extensions) const {
	if (Object::cast_to<LandscapeFoliageData>(p_resource.ptr())) {
		p_extensions->push_back("lfdata");
	}
}
