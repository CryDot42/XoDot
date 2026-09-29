/**************************************************************************/
/*  landscape_foliage_gpu.cpp                                             */
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

#ifdef RD_ENABLED

#include "landscape_foliage_gpu.h"

#include "shaders/landscape_foliage_cull.glsl.gen.h"

#include "core/object/callable_mp.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server.h"

#include "modules/streaming/world_streaming.h"

LandscapeFoliageGPU::Shared *LandscapeFoliageGPU::shared = nullptr;
int LandscapeFoliageGPU::shared_users = 0;

static constexpr uint32_t FOLIAGE_COUNTER_COUNT = 32;
static constexpr uint32_t FOLIAGE_LOD_UNKNOWN = 254;
static constexpr uint32_t FOLIAGE_GROUP_SIZE = 64;
static constexpr uint32_t FOLIAGE_MAX_GROUPS_X = 32768;

struct LandscapeFoliageCullPushConstant {
	uint32_t lod;
	uint32_t list;
	uint32_t surfaces;
	uint32_t capacity;
};

static _FORCE_INLINE_ int64_t _foliage_gpu_bytes(uint32_t p_capacity) {
	return int64_t(p_capacity) * (LandscapeFoliageGPU::INSTANCE_FLOATS * sizeof(float) + sizeof(uint32_t));
}

void LandscapeFoliageGPU::_ensure_initialized() {
	if (initialized) {
		return;
	}
	initialized = true;
	shared_users++;
	if (shared) {
		return;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	shared = memnew(Shared);
	Vector<String> modes;
	modes.push_back("\n#define MODE_CLASSIFY\n");
	modes.push_back("\n#define MODE_EMIT\n");
	modes.push_back("\n#define MODE_COMMAND\n");
	shared->shader = memnew(LandscapeFoliageCullShaderRD);
	shared->shader->initialize(modes);
	shared->version = shared->shader->version_create();
	for (int i = 0; i < MODE_MAX; i++) {
		shared->shaders[i] = shared->shader->version_get_shader(shared->version, i);
		shared->pipelines[i] = rd->compute_pipeline_create(shared->shaders[i]);
	}
}

void LandscapeFoliageGPU::_free_entry(Entry &r_entry) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	for (RID *buffer : { &r_entry.source, &r_entry.state, &r_entry.counters, &r_entry.params }) {
		if (buffer->is_valid()) {
			rd->free_rid(*buffer);
			*buffer = RID();
		}
	}
	if (r_entry.capacity > 0 && WorldStreaming::get_singleton()) {
		WorldStreaming::get_singleton()->add_pool_usage(SNAME("Foliage GPU"), -_foliage_gpu_bytes(r_entry.capacity));
	}
	r_entry.capacity = 0;
}

void LandscapeFoliageGPU::set_capacity(uint64_t p_id, uint32_t p_capacity) {
	_ensure_initialized();
	RenderingDevice *rd = RenderingDevice::get_singleton();
	Entry &entry = entries[p_id];
	if (entry.params.is_null()) {
		entry.params = rd->uniform_buffer_create(sizeof(LandscapeFoliageCullParams));
		entry.counters = rd->storage_buffer_create(FOLIAGE_COUNTER_COUNT * sizeof(uint32_t));
	}
	for (RID *buffer : { &entry.source, &entry.state }) {
		if (buffer->is_valid()) {
			rd->free_rid(*buffer);
			*buffer = RID();
		}
	}
	if (entry.capacity > 0 && WorldStreaming::get_singleton()) {
		WorldStreaming::get_singleton()->add_pool_usage(SNAME("Foliage GPU"), -_foliage_gpu_bytes(entry.capacity));
	}
	const uint32_t capacity = MAX(p_capacity, 1u);
	entry.source = rd->storage_buffer_create(capacity * INSTANCE_FLOATS * sizeof(float));
	rd->set_resource_name(entry.source, "Foliage Instances");
	rd->buffer_clear(entry.source, 0, capacity * INSTANCE_FLOATS * sizeof(float)); // Holes.
	entry.state = rd->storage_buffer_create(capacity * sizeof(uint32_t));
	rd->set_resource_name(entry.state, "Foliage LOD State");
	rd->buffer_clear(entry.state, 0, capacity * sizeof(uint32_t));
	entry.capacity = capacity;
	if (WorldStreaming::get_singleton()) {
		WorldStreaming::get_singleton()->add_pool_usage(SNAME("Foliage GPU"), _foliage_gpu_bytes(capacity));
	}
}

void LandscapeFoliageGPU::update_instances(uint64_t p_id, uint32_t p_offset, const Vector<float> &p_data) {
	Entry *entry = entries.getptr(p_id);
	ERR_FAIL_NULL(entry);
	const uint32_t count = p_data.size() / INSTANCE_FLOATS;
	ERR_FAIL_COND(uint64_t(p_offset) + count > entry->capacity);
	if (count == 0) {
		return;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	rd->buffer_update(entry->source, p_offset * INSTANCE_FLOATS * sizeof(float), count * INSTANCE_FLOATS * sizeof(float), p_data.ptr());
	// No known level yet (no hysteresis for the first run).
	LocalVector<uint32_t> unknown;
	unknown.resize(count);
	for (uint32_t &state : unknown) {
		state = FOLIAGE_LOD_UNKNOWN;
	}
	rd->buffer_update(entry->state, p_offset * sizeof(uint32_t), count * sizeof(uint32_t), unknown.ptr());
}

void LandscapeFoliageGPU::run(uint64_t p_id, const Vector<uint8_t> &p_params, const Array &p_outputs, const Vector<int32_t> &p_layout, uint64_t p_serial) {
	ERR_FAIL_COND(p_params.size() != sizeof(LandscapeFoliageCullParams));
	ERR_FAIL_COND(p_outputs.size() != OUTPUT_COUNT || p_layout.size() != OUTPUT_COUNT);
	Entry *entry = entries.getptr(p_id);
	if (!entry || entry->source.is_null()) {
		return;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	RenderingServer *rs = RenderingServer::get_singleton();
	LandscapeFoliageCullParams params;
	memcpy(&params, p_params.ptr(), sizeof(params));
	params.counts[0] = MIN(params.counts[0], entry->capacity);
	rd->buffer_update(entry->params, 0, sizeof(params), &params);
	uint32_t counters[FOLIAGE_COUNTER_COUNT] = {};
	rd->buffer_update(entry->counters, 0, sizeof(counters), counters);

	struct Output {
		RID instances;
		RID command;
		uint32_t lod = 0;
		uint32_t list = 0;
		uint32_t capacity = 0;
		uint32_t surfaces = 0;
	};
	LocalVector<Output> outputs;
	for (int i = 0; i < OUTPUT_COUNT; i++) {
		const RID multimesh = p_outputs[i];
		if (multimesh.is_null()) {
			continue;
		}
		Output output;
		output.instances = rs->multimesh_get_buffer_rd_rid(multimesh);
		output.command = rs->multimesh_get_command_buffer_rd_rid(multimesh);
		output.lod = i % MAX_LODS;
		output.list = i / MAX_LODS;
		output.capacity = uint32_t(p_layout[i]) & MAX_OUTPUT_CAPACITY;
		output.surfaces = uint32_t(p_layout[i]) >> 24;
		if (output.instances.is_valid() && output.command.is_valid()) {
			outputs.push_back(output);
		}
	}

	UniformSetCacheRD *cache = UniformSetCacheRD::get_singleton();
	const uint32_t groups = (params.counts[0] + FOLIAGE_GROUP_SIZE - 1) / FOLIAGE_GROUP_SIZE;
	const uint32_t groups_x = MIN(groups, FOLIAGE_MAX_GROUPS_X);
	const uint32_t groups_y = groups_x > 0 ? (groups + groups_x - 1) / groups_x : 0;
	RD::ComputeListID compute_list = rd->compute_list_begin();
	if (groups > 0) {
		RID classify_set = cache->get_cache(shared->shaders[MODE_CLASSIFY], 0,
				RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, entry->params),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, entry->source),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, entry->state));
		LandscapeFoliageCullPushConstant push = {};
		rd->compute_list_bind_compute_pipeline(compute_list, shared->pipelines[MODE_CLASSIFY]);
		rd->compute_list_bind_uniform_set(compute_list, classify_set, 0);
		rd->compute_list_set_push_constant(compute_list, &push, sizeof(push));
		rd->compute_list_dispatch(compute_list, groups_x, groups_y, 1);
		rd->compute_list_add_barrier(compute_list);

		rd->compute_list_bind_compute_pipeline(compute_list, shared->pipelines[MODE_EMIT]);
		for (const Output &output : outputs) {
			RID emit_set = cache->get_cache(shared->shaders[MODE_EMIT], 0,
					RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, entry->params),
					RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, entry->source),
					RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, entry->state),
					RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, entry->counters),
					RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, output.instances));
			push.lod = output.lod;
			push.list = output.list;
			push.surfaces = output.surfaces;
			push.capacity = output.capacity;
			rd->compute_list_bind_uniform_set(compute_list, emit_set, 0);
			rd->compute_list_set_push_constant(compute_list, &push, sizeof(push));
			rd->compute_list_dispatch(compute_list, groups_x, groups_y, 1);
		}
		rd->compute_list_add_barrier(compute_list);
	}
	rd->compute_list_bind_compute_pipeline(compute_list, shared->pipelines[MODE_COMMAND]);
	for (const Output &output : outputs) {
		RID command_set = cache->get_cache(shared->shaders[MODE_COMMAND], 0,
				RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, entry->params),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, entry->counters),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, output.command));
		LandscapeFoliageCullPushConstant push = {};
		push.lod = output.lod;
		push.list = output.list;
		push.surfaces = output.surfaces;
		push.capacity = output.capacity;
		rd->compute_list_bind_uniform_set(compute_list, command_set, 0);
		rd->compute_list_set_push_constant(compute_list, &push, sizeof(push));
		rd->compute_list_dispatch(compute_list, 1, 1, 1);
	}
	rd->compute_list_end();

	// Statistics, read back without stalling. The GPU object may be gone when they arrive.
	rd->buffer_get_data_async(entry->counters, callable_mp_static(&LandscapeFoliageGPU::_stats_received).bind(get_instance_id(), p_id, p_serial), 0, FOLIAGE_COUNTER_COUNT * sizeof(uint32_t));
}

void LandscapeFoliageGPU::_stats_received(const Vector<uint8_t> &p_data, ObjectID p_gpu, uint64_t p_id, uint64_t p_serial) {
	LandscapeFoliageGPU *gpu = ObjectDB::get_instance<LandscapeFoliageGPU>(p_gpu);
	if (!gpu || p_data.size() < int(FOLIAGE_COUNTER_COUNT * sizeof(uint32_t))) {
		return;
	}
	const uint32_t *counters = reinterpret_cast<const uint32_t *>(p_data.ptr());
	Stats result;
	result.serial = p_serial;
	for (int i = 0; i < OUTPUT_COUNT; i++) {
		result.counts[i] = counters[i];
	}
	MutexLock lock(gpu->stats_mutex);
	Stats *current = gpu->stats.getptr(p_id);
	if (current) {
		if (current->serial < p_serial) {
			*current = result;
		}
	} else if (gpu->entries.has(p_id)) {
		gpu->stats.insert(p_id, result);
	}
}

LandscapeFoliageGPU::Stats LandscapeFoliageGPU::get_stats(uint64_t p_id) const {
	MutexLock lock(stats_mutex);
	const Stats *result = stats.getptr(p_id);
	return result ? *result : Stats();
}

void LandscapeFoliageGPU::free_entry(uint64_t p_id) {
	Entry *entry = entries.getptr(p_id);
	if (entry) {
		_free_entry(*entry);
		entries.erase(p_id);
	}
	MutexLock lock(stats_mutex);
	stats.erase(p_id);
}

void LandscapeFoliageGPU::free_resources() {
	for (KeyValue<uint64_t, Entry> &kv : entries) {
		_free_entry(kv.value);
	}
	entries.clear();
	{
		MutexLock lock(stats_mutex);
		stats.clear();
	}
	if (!initialized) {
		return;
	}
	initialized = false;
	shared_users--;
	if (shared_users == 0 && shared) {
		RenderingDevice *rd = RenderingDevice::get_singleton();
		for (int i = 0; i < MODE_MAX; i++) {
			if (shared->pipelines[i].is_valid() && rd) {
				rd->free_rid(shared->pipelines[i]);
			}
		}
		shared->shader->version_free(shared->version);
		memdelete(shared->shader);
		memdelete(shared);
		shared = nullptr;
	}
}

void LandscapeFoliageGPU::destroy(Object *p_gpu) {
	LandscapeFoliageGPU *gpu = Object::cast_to<LandscapeFoliageGPU>(p_gpu);
	ERR_FAIL_NULL(gpu);
	gpu->free_resources();
	memdelete(gpu);
}

LandscapeFoliageGPU::LandscapeFoliageGPU() {
}

LandscapeFoliageGPU::~LandscapeFoliageGPU() {
	ERR_FAIL_COND_MSG(initialized, "LandscapeFoliageGPU must be freed on the rendering thread with LandscapeFoliageGPU::destroy().");
}

#endif // RD_ENABLED
