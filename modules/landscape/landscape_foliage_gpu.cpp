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
#include "servers/rendering/renderer_scene_occlusion_cull.h"
#include "servers/rendering/rendering_method.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_globals.h"

#include "modules/streaming/world_streaming.h"

LandscapeFoliageGPU::Shared *LandscapeFoliageGPU::shared = nullptr;

// Per list: count per level, overflow, culled by the frustum, by occlusion (see the shader).
static constexpr uint32_t FOLIAGE_LIST_COUNTERS = 16;
static constexpr uint32_t FOLIAGE_COUNTER_FRUSTUM_CULLED = 9;
static constexpr uint32_t FOLIAGE_COUNTER_OCCLUSION_CULLED = 10;
static constexpr uint32_t FOLIAGE_COUNTER_COUNT = FOLIAGE_LIST_COUNTERS * LandscapeFoliageGPU::LIST_MAX;
static constexpr uint32_t FOLIAGE_LOD_UNKNOWN = 254;
static constexpr uint32_t FOLIAGE_GROUP_SIZE = 64;
static constexpr uint32_t FOLIAGE_MAX_GROUPS_X = 32768;
static constexpr int FOLIAGE_MAX_HZB_MIPS = 16;
static constexpr uint32_t FOLIAGE_VIEW_ORTHOGONAL = 4;

struct LandscapeFoliageCullPushConstant {
	uint32_t lod;
	uint32_t list;
	uint32_t surfaces;
	uint32_t capacity;
};

// Layout of the uniform buffer of the view (main list) consumed by the foliage culling shader (std140).
struct LandscapeFoliageViewParams {
	float planes[6][4] = {};
	float space[3][4] = {};
	float view[3][4] = {};
	float projection[16] = {};
	float camera[4] = {};
	float hzb[4] = {};
	uint32_t flags[4] = {};
	uint32_t hzb_mips[FOLIAGE_MAX_HZB_MIPS][4] = {};
};

static_assert(sizeof(LandscapeFoliageViewParams) == 560, "LandscapeFoliageViewParams must match the std140 layout of the foliage culling shader.");

static _FORCE_INLINE_ int64_t _foliage_gpu_bytes(uint32_t p_capacity) {
	return int64_t(p_capacity) * (LandscapeFoliageGPU::INSTANCE_FLOATS * sizeof(float) + sizeof(uint32_t));
}

static _FORCE_INLINE_ void _foliage_store_rows(const Transform3D &p_transform, float r_rows[3][4]) {
	for (int i = 0; i < 3; i++) {
		r_rows[i][0] = p_transform.basis.rows[i].x;
		r_rows[i][1] = p_transform.basis.rows[i].y;
		r_rows[i][2] = p_transform.basis.rows[i].z;
		r_rows[i][3] = p_transform.origin[i];
	}
}

void LandscapeFoliageGPU::_ensure_initialized() {
	if (initialized) {
		return;
	}
	initialized = true;
	if (!shared) {
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
		// The main lists are culled for every camera drawn, with its occlusion buffer.
		RSG::scene->camera_callback_add(callable_mp_static(&LandscapeFoliageGPU::_camera_drawn));
	}
	shared->users.push_back(this);
}

void LandscapeFoliageGPU::_ensure_view_resources() {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (view_params.is_null()) {
		view_params = rd->uniform_buffer_create(sizeof(LandscapeFoliageViewParams));
	}
	for (int i = 0; i < 2; i++) {
		if (hzb_buffers[i].is_null()) {
			// Bound even without occlusion culling.
			hzb_capacities[i] = 4;
			hzb_buffers[i] = rd->storage_buffer_create(hzb_capacities[i] * sizeof(float));
			rd->set_resource_name(hzb_buffers[i], "Foliage Occlusion Buffer");
		}
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
	r_entry.instance_count = 0;
	r_entry.classified = false;
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
	entry.instance_count = 0;
	entry.classified = false; // Nothing to emit before the levels are chosen.
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
	// No known level yet (no hysteresis for the first run), not emitted before.
	LocalVector<uint32_t> unknown;
	unknown.resize(count);
	for (uint32_t &state : unknown) {
		state = FOLIAGE_LOD_UNKNOWN;
	}
	rd->buffer_update(entry->state, p_offset * sizeof(uint32_t), count * sizeof(uint32_t), unknown.ptr());
}

void LandscapeFoliageGPU::set_outputs(uint64_t p_id, const Array &p_outputs, const Vector<int32_t> &p_layout, uint64_t p_serial) {
	ERR_FAIL_COND(p_outputs.size() != OUTPUT_COUNT || p_layout.size() != OUTPUT_COUNT);
	Entry *entry = entries.getptr(p_id);
	if (!entry) {
		bool any = false;
		for (int i = 0; i < OUTPUT_COUNT; i++) {
			any = any || RID(p_outputs[i]).is_valid();
		}
		if (!any) {
			return;
		}
		entry = &entries.insert(p_id, Entry())->value;
	}
	for (int i = 0; i < OUTPUT_COUNT; i++) {
		Output &output = entry->outputs[i];
		output.multimesh = p_outputs[i];
		output.capacity = uint32_t(p_layout[i]) & MAX_OUTPUT_CAPACITY;
		output.surfaces = uint32_t(p_layout[i]) >> 24;
	}
	entry->serial = p_serial;
	entry->main_view = 0;
}

void LandscapeFoliageGPU::_emit(Entry &r_entry, int p_list, RD::ComputeListID p_compute_list, RID p_hzb) {
	// Appends the instances of each level to the output of the level, then writes the draw commands.
	RenderingDevice *rd = RenderingDevice::get_singleton();
	RenderingServer *rs = RenderingServer::get_singleton();
	struct Target {
		RID instances;
		RID command;
		LandscapeFoliageCullPushConstant push = {};
	};
	LocalVector<Target> targets;
	for (int lod = 0; lod < MAX_LODS; lod++) {
		const Output &output = r_entry.outputs[p_list * MAX_LODS + lod];
		if (output.multimesh.is_null()) {
			continue;
		}
		Target target;
		target.instances = rs->multimesh_get_buffer_rd_rid(output.multimesh);
		target.command = rs->multimesh_get_command_buffer_rd_rid(output.multimesh);
		target.push.lod = lod;
		target.push.list = p_list;
		target.push.surfaces = output.surfaces;
		target.push.capacity = output.capacity;
		if (target.instances.is_valid() && target.command.is_valid()) {
			targets.push_back(target);
		}
	}

	UniformSetCacheRD *cache = UniformSetCacheRD::get_singleton();
	const uint32_t groups = (r_entry.instance_count + FOLIAGE_GROUP_SIZE - 1) / FOLIAGE_GROUP_SIZE;
	const uint32_t groups_x = MIN(groups, FOLIAGE_MAX_GROUPS_X);
	const uint32_t groups_y = groups_x > 0 ? (groups + groups_x - 1) / groups_x : 0;
	if (groups > 0 && !targets.is_empty()) {
		rd->compute_list_bind_compute_pipeline(p_compute_list, shared->pipelines[MODE_EMIT]);
		for (const Target &target : targets) {
			RID emit_set = cache->get_cache(shared->shaders[MODE_EMIT], 0,
					RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, r_entry.params),
					RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, r_entry.source),
					RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, r_entry.state),
					RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, r_entry.counters),
					RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, target.instances),
					RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 6, view_params),
					RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, p_hzb));
			rd->compute_list_bind_uniform_set(p_compute_list, emit_set, 0);
			rd->compute_list_set_push_constant(p_compute_list, &target.push, sizeof(target.push));
			rd->compute_list_dispatch(p_compute_list, groups_x, groups_y, 1);
		}
		rd->compute_list_add_barrier(p_compute_list);
	}
	if (!targets.is_empty()) {
		rd->compute_list_bind_compute_pipeline(p_compute_list, shared->pipelines[MODE_COMMAND]);
		for (const Target &target : targets) {
			RID command_set = cache->get_cache(shared->shaders[MODE_COMMAND], 0,
					RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, r_entry.params),
					RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, r_entry.counters),
					RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, target.command));
			rd->compute_list_bind_uniform_set(p_compute_list, command_set, 0);
			rd->compute_list_set_push_constant(p_compute_list, &target.push, sizeof(target.push));
			rd->compute_list_dispatch(p_compute_list, 1, 1, 1);
		}
	}
}

void LandscapeFoliageGPU::run_lod(uint64_t p_id, const Vector<uint8_t> &p_params) {
	ERR_FAIL_COND(p_params.size() != sizeof(LandscapeFoliageCullParams));
	Entry *entry = entries.getptr(p_id);
	if (!entry || entry->source.is_null()) {
		return;
	}
	_ensure_view_resources(); // Bound by the emission, unused by the shadow list.
	RenderingDevice *rd = RenderingDevice::get_singleton();
	LandscapeFoliageCullParams params;
	memcpy(&params, p_params.ptr(), sizeof(params));
	params.counts[0] = MIN(params.counts[0], entry->capacity);
	entry->instance_count = params.counts[0];
	rd->buffer_update(entry->params, 0, sizeof(params), &params);
	const uint32_t zeros[FOLIAGE_LIST_COUNTERS] = {};
	rd->buffer_update(entry->counters, LIST_SHADOW * FOLIAGE_LIST_COUNTERS * sizeof(uint32_t), sizeof(zeros), zeros);

	const uint32_t groups = (entry->instance_count + FOLIAGE_GROUP_SIZE - 1) / FOLIAGE_GROUP_SIZE;
	const uint32_t groups_x = MIN(groups, FOLIAGE_MAX_GROUPS_X);
	const uint32_t groups_y = groups_x > 0 ? (groups + groups_x - 1) / groups_x : 0;
	RD::ComputeListID compute_list = rd->compute_list_begin();
	if (groups > 0) {
		RID classify_set = UniformSetCacheRD::get_singleton()->get_cache(shared->shaders[MODE_CLASSIFY], 0,
				RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, entry->params),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, entry->source),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, entry->state));
		LandscapeFoliageCullPushConstant push = {};
		rd->compute_list_bind_compute_pipeline(compute_list, shared->pipelines[MODE_CLASSIFY]);
		rd->compute_list_bind_uniform_set(compute_list, classify_set, 0);
		rd->compute_list_set_push_constant(compute_list, &push, sizeof(push));
		rd->compute_list_dispatch(compute_list, groups_x, groups_y, 1);
		rd->compute_list_add_barrier(compute_list);
	}
	_emit(*entry, LIST_SHADOW, compute_list, hzb_buffers[1]);
	rd->compute_list_end();
	entry->classified = true;
	entry->main_view = 0; // Other levels: emitted again by the next camera drawn.

	// Statistics, read back without stalling. The GPU object may be gone when they arrive.
	rd->buffer_get_data_async(entry->counters, callable_mp_static(&LandscapeFoliageGPU::_stats_received).bind(get_instance_id(), p_id, entry->serial, LIST_SHADOW), LIST_SHADOW * FOLIAGE_LIST_COUNTERS * sizeof(uint32_t), FOLIAGE_LIST_COUNTERS * sizeof(uint32_t));
}

void LandscapeFoliageGPU::set_view_settings(RID p_scenario, RID p_lod_viewport, bool p_frozen, const Transform3D &p_space, uint32_t p_render_layers, uint32_t p_flags) {
	if (p_lod_viewport != lod_viewport && !p_frozen) {
		lod_view = View(); // Another camera.
	}
	scenario = p_scenario;
	lod_viewport = p_lod_viewport;
	frozen = p_frozen;
	space = p_space;
	render_layers = p_render_layers;
	view_flags = p_flags;
	// Emitted again by the next camera drawn.
	for (KeyValue<uint64_t, Entry> &kv : entries) {
		kv.value.main_view = 0;
	}
}

bool LandscapeFoliageGPU::_upload_occlusion(RID p_viewport, int p_slot, View &r_view) {
	// The occlusion buffer of the viewport, just updated for its camera by the renderer.
	r_view.occlusion = false;
	RendererSceneOcclusionCull *occlusion_cull = RendererSceneOcclusionCull::get_singleton();
	const RendererSceneOcclusionCull::HZBuffer *buffer = occlusion_cull ? occlusion_cull->buffer_get_ptr(p_viewport) : nullptr;
	if (!buffer || buffer->is_empty() || (!buffer->is_using_depth_readback() && !buffer->is_using_occluders())) {
		return false;
	}
	// The coarsest mips (if more than supported) are only needed by very large bounds.
	const int mip_count = MIN(buffer->get_mip_count(), FOLIAGE_MAX_HZB_MIPS);
	uint32_t size = 0;
	for (int i = 0; i < mip_count; i++) {
		const Size2i &mip = buffer->get_mip_size(i);
		r_view.hzb_mips[i][0] = size;
		r_view.hzb_mips[i][1] = mip.x;
		r_view.hzb_mips[i][2] = mip.y;
		r_view.hzb_mips[i][3] = 0;
		size += mip.x * mip.y;
	}
	const LocalVector<float> &data = buffer->get_data();
	ERR_FAIL_COND_V(size == 0 || data.size() < size, false);

	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (hzb_capacities[p_slot] < size) {
		rd->free_rid(hzb_buffers[p_slot]);
		hzb_capacities[p_slot] = size;
		hzb_buffers[p_slot] = rd->storage_buffer_create(size * sizeof(float));
		rd->set_resource_name(hzb_buffers[p_slot], "Foliage Occlusion Buffer");
	}
	rd->buffer_update(hzb_buffers[p_slot], 0, size * sizeof(float), data.ptr());
	r_view.hzb_size = buffer->get_mip_size(0);
	r_view.hzb_mip_count = mip_count;
	r_view.occlusion = true;
	return true;
}

void LandscapeFoliageGPU::_draw_view(RID p_viewport, const Transform3D &p_transform, const Projection &p_projection, bool p_orthogonal, uint32_t p_layers) {
	if ((p_layers & render_layers) == 0) {
		return; // Not seen by this camera.
	}
	bool any = false;
	for (const KeyValue<uint64_t, Entry> &kv : entries) {
		any = any || (kv.value.classified && kv.value.source.is_valid());
	}
	if (!any) {
		return;
	}
	_ensure_view_resources();

	// The view that culls the main lists: this camera, or the last one of the LOD camera while frozen.
	View current;
	int slot = 1;
	uint64_t view_id = 0;
	if (frozen && lod_view.valid) {
		current = lod_view;
		slot = 0;
		view_id = lod_view_id;
	} else {
		current.valid = true;
		current.transform = p_transform;
		current.projection = p_projection;
		current.orthogonal = p_orthogonal;
		slot = p_viewport == lod_viewport ? 0 : 1;
		if (view_flags & VIEW_OCCLUSION_CULLING) {
			_upload_occlusion(p_viewport, slot, current);
		}
		// Without occlusion buffer, the lists emitted for the same camera are still valid.
		const bool same = !current.occlusion && last_view_id != 0 && last_view_viewport == p_viewport && last_view.transform == p_transform && last_view.projection == p_projection && last_view.orthogonal == p_orthogonal;
		view_id = same ? last_view_id : ++view_serial;
		last_view = current;
		last_view_viewport = p_viewport;
		last_view_id = view_id;
		if (slot == 0) {
			lod_view = current; // Kept while frozen.
			lod_view_id = view_id;
		}
	}

	LocalVector<KeyValue<uint64_t, Entry> *> pending;
	for (KeyValue<uint64_t, Entry> &kv : entries) {
		if (kv.value.classified && kv.value.source.is_valid() && kv.value.main_view != view_id) {
			pending.push_back(&kv);
		}
	}
	if (pending.is_empty()) {
		return;
	}

	LandscapeFoliageViewParams params;
	if (view_flags & VIEW_FRUSTUM_CULLING) {
		const Vector<Plane> planes = current.projection.get_projection_planes(current.transform);
		for (int i = 0; i < 6; i++) {
			// Never culls without a plane.
			const Plane plane = i < planes.size() ? planes[i] : Plane(Vector3(), 1e30);
			params.planes[i][0] = plane.normal.x;
			params.planes[i][1] = plane.normal.y;
			params.planes[i][2] = plane.normal.z;
			params.planes[i][3] = plane.d;
		}
		params.flags[0] |= VIEW_FRUSTUM_CULLING;
	}
	_foliage_store_rows(space, params.space);
	_foliage_store_rows(current.transform.affine_inverse(), params.view);
	for (int i = 0; i < 4; i++) {
		for (int j = 0; j < 4; j++) {
			params.projection[i * 4 + j] = current.projection.columns[i][j];
		}
	}
	params.camera[0] = current.transform.origin.x;
	params.camera[1] = current.transform.origin.y;
	params.camera[2] = current.transform.origin.z;
	params.camera[3] = current.projection.get_z_near();
	params.hzb[0] = current.hzb_size.x;
	params.hzb[1] = current.hzb_size.y;
	params.hzb[2] = current.hzb_mip_count;
	params.hzb[3] = MAX(MAX(space.basis.get_column(0).length(), space.basis.get_column(1).length()), space.basis.get_column(2).length());
	if (current.occlusion) {
		params.flags[0] |= VIEW_OCCLUSION_CULLING;
		memcpy(params.hzb_mips, current.hzb_mips, sizeof(params.hzb_mips));
	}
	if (current.orthogonal) {
		params.flags[0] |= FOLIAGE_VIEW_ORTHOGONAL;
	}

	RenderingDevice *rd = RenderingDevice::get_singleton();
	rd->buffer_update(view_params, 0, sizeof(params), &params);
	const uint32_t zeros[FOLIAGE_LIST_COUNTERS] = {};
	for (const KeyValue<uint64_t, Entry> *kv : pending) {
		rd->buffer_update(kv->value.counters, LIST_MAIN * FOLIAGE_LIST_COUNTERS * sizeof(uint32_t), sizeof(zeros), zeros);
	}
	RD::ComputeListID compute_list = rd->compute_list_begin();
	for (KeyValue<uint64_t, Entry> *kv : pending) {
		_emit(kv->value, LIST_MAIN, compute_list, hzb_buffers[slot]);
	}
	rd->compute_list_end();

	for (KeyValue<uint64_t, Entry> *kv : pending) {
		Entry &entry = kv->value;
		entry.main_view = view_id;
		rd->buffer_get_data_async(entry.counters, callable_mp_static(&LandscapeFoliageGPU::_stats_received).bind(get_instance_id(), kv->key, entry.serial, LIST_MAIN), LIST_MAIN * FOLIAGE_LIST_COUNTERS * sizeof(uint32_t), FOLIAGE_LIST_COUNTERS * sizeof(uint32_t));
	}
}

void LandscapeFoliageGPU::_camera_drawn(RID p_scenario, RID p_viewport, const Transform3D &p_transform, const Projection &p_projection, bool p_orthogonal, uint32_t p_layers) {
	if (!shared) {
		return;
	}
	for (LandscapeFoliageGPU *gpu : shared->users) {
		if (gpu->scenario == p_scenario) {
			gpu->_draw_view(p_viewport, p_transform, p_projection, p_orthogonal, p_layers);
		}
	}
}

void LandscapeFoliageGPU::_stats_received(const Vector<uint8_t> &p_data, ObjectID p_gpu, uint64_t p_id, uint64_t p_serial, int p_list) {
	LandscapeFoliageGPU *gpu = ObjectDB::get_instance<LandscapeFoliageGPU>(p_gpu);
	if (!gpu || p_data.size() < int(FOLIAGE_LIST_COUNTERS * sizeof(uint32_t))) {
		return;
	}
	const uint32_t *counters = reinterpret_cast<const uint32_t *>(p_data.ptr());
	MutexLock lock(gpu->stats_mutex);
	StatsEntry *entry = gpu->stats.getptr(p_id);
	if (!entry) {
		if (!gpu->entries.has(p_id)) {
			return;
		}
		entry = &gpu->stats.insert(p_id, StatsEntry())->value;
	}
	Stats &last = entry->last;
	last.serial = MAX(last.serial, p_serial);
	last.lists |= 1 << p_list;
	for (int lod = 0; lod < MAX_LODS; lod++) {
		last.counts[p_list * MAX_LODS + lod] = counters[lod];
	}
	if (p_list == LIST_MAIN) {
		last.frustum_culled = counters[FOLIAGE_COUNTER_FRUSTUM_CULLED];
		last.occlusion_culled = counters[FOLIAGE_COUNTER_OCCLUSION_CULLED];
	}
	// Most instances of every camera, for the outputs they were emitted with.
	Stats &peak = entry->peak;
	if (p_serial < peak.serial) {
		return;
	}
	if (p_serial > peak.serial) {
		peak = Stats();
		peak.serial = p_serial;
	}
	peak.lists |= 1 << p_list;
	for (int lod = 0; lod < MAX_LODS; lod++) {
		uint32_t &count = peak.counts[p_list * MAX_LODS + lod];
		count = MAX(count, counters[lod]);
	}
}

LandscapeFoliageGPU::Stats LandscapeFoliageGPU::get_stats(uint64_t p_id) const {
	MutexLock lock(stats_mutex);
	const StatsEntry *result = stats.getptr(p_id);
	return result ? result->last : Stats();
}

LandscapeFoliageGPU::Stats LandscapeFoliageGPU::take_peak_stats(uint64_t p_id) {
	MutexLock lock(stats_mutex);
	StatsEntry *result = stats.getptr(p_id);
	if (!result) {
		return Stats();
	}
	const Stats peak = result->peak;
	result->peak = Stats();
	result->peak.serial = peak.serial; // Older counts are ignored.
	return peak;
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
	RenderingDevice *rd = RenderingDevice::get_singleton();
	for (RID *buffer : { &view_params, &hzb_buffers[0], &hzb_buffers[1] }) {
		if (buffer->is_valid()) {
			rd->free_rid(*buffer);
			*buffer = RID();
		}
	}
	hzb_capacities[0] = 0;
	hzb_capacities[1] = 0;
	lod_view = View();
	last_view_id = 0;
	if (!initialized) {
		return;
	}
	initialized = false;
	shared->users.erase(this);
	if (shared->users.is_empty()) {
		RSG::scene->camera_callback_remove(callable_mp_static(&LandscapeFoliageGPU::_camera_drawn));
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
