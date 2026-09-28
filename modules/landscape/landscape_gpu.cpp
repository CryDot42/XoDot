/**************************************************************************/
/*  landscape_gpu.cpp                                                     */
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

#include "landscape_gpu.h"

#include "shaders/landscape_lod.glsl.gen.h"

#include "core/object/callable_mp.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server.h"

#include "modules/streaming/world_streaming.h"

LandscapeGPU::Shared *LandscapeGPU::shared = nullptr;
int LandscapeGPU::shared_users = 0;

struct LandscapeLodPushConstant {
	uint32_t level;
	uint32_t pad[3];
};

int64_t LandscapeGPU::get_slot_size(int p_weightmap_count, bool p_holes) {
	return int64_t(PAGE_TEXELS) * PAGE_TEXELS * (4 + 2 + 4 * p_weightmap_count + (p_holes ? 1 : 0));
}

void LandscapeGPU::_ensure_initialized() {
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

	Vector<String> lod_modes;
	lod_modes.push_back("\n#define MODE_TRAVERSE\n");
	lod_modes.push_back("\n#define MODE_STITCH\n");
	shared->lod_shader = memnew(LandscapeLodShaderRD);
	shared->lod_shader->initialize(lod_modes);
	shared->lod_version = shared->lod_shader->version_create();
	for (int i = 0; i < LOD_MODE_MAX; i++) {
		shared->lod_shaders[i] = shared->lod_shader->version_get_shader(shared->lod_version, i);
		shared->lod_pipelines[i] = rd->compute_pipeline_create(shared->lod_shaders[i]);
	}
}

/* Pages */

void LandscapeGPU::_free_pages(Pages &r_pages) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	auto free_rid = [rd](RID &r_rid) {
		if (r_rid.is_valid() && rd->texture_is_valid(r_rid)) {
			rd->free_rid(r_rid);
		}
		r_rid = RID();
	};
	free_rid(r_pages.heights);
	free_rid(r_pages.normals);
	for (RID &weights : r_pages.weights) {
		free_rid(weights);
	}
	free_rid(r_pages.hole_pages);
	free_rid(r_pages.table);
	if (r_pages.bytes != 0 && WorldStreaming::get_singleton()) {
		WorldStreaming::get_singleton()->add_pool_usage(SNAME("Landscape GPU"), -r_pages.bytes);
	}
	r_pages = Pages();
}

RID LandscapeGPU::_create_array(RenderingDevice::DataFormat p_format, int p_size, int p_layers, const String &p_name) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	RD::TextureFormat tf;
	tf.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
	tf.width = p_size;
	tf.height = p_size;
	tf.array_layers = p_layers;
	tf.format = p_format;
	tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	if (p_format == RD::DATA_FORMAT_R8G8B8A8_UNORM) {
		// The RenderingServer creates an sRGB view of RGBA8 textures.
		tf.shareable_formats.push_back(RD::DATA_FORMAT_R8G8B8A8_UNORM);
		tf.shareable_formats.push_back(RD::DATA_FORMAT_R8G8B8A8_SRGB);
	}
	RID texture = rd->texture_create(tf, RD::TextureView());
	if (texture.is_valid()) {
		rd->set_resource_name(texture, p_name);
		rd->texture_clear(texture, Color(0, 0, 0, 0), 0, 1, 0, p_layers);
	}
	return texture;
}

void LandscapeGPU::setup_pages(int p_slots, int p_weightmap_count, bool p_holes, const Vector2i &p_table_size, RID p_heights_rs, RID p_normals_rs, const Array &p_weights_rs, RID p_holes_rs, RID p_table_rs) {
	_ensure_initialized();
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_COND(p_slots < 2 || p_slots > MAX_SLOTS);
	ERR_FAIL_COND(p_table_size.x < 1 || p_table_size.y < 1);
	ERR_FAIL_COND(p_weights_rs.size() != MAX_WEIGHTMAPS);

	_free_pages(pages);
	pages_valid = false;

	Pages new_pages;
	new_pages.slots = p_slots;
	new_pages.weightmap_count = CLAMP(p_weightmap_count, 1, MAX_WEIGHTMAPS);
	new_pages.holes = p_holes;
	new_pages.table_size = p_table_size;
	new_pages.normal_format = rd->texture_is_format_supported_for_usage(RD::DATA_FORMAT_R8G8_UNORM, RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT) ? RD::DATA_FORMAT_R8G8_UNORM : RD::DATA_FORMAT_R8G8B8A8_UNORM;

	bool ok = true;
	new_pages.heights = _create_array(RD::DATA_FORMAT_R32_SFLOAT, PAGE_TEXELS, p_slots, "Landscape Height Pages");
	ok = new_pages.heights.is_valid();
	if (ok) {
		new_pages.normals = _create_array(new_pages.normal_format, PAGE_TEXELS, p_slots, "Landscape Normal Pages");
		ok = new_pages.normals.is_valid();
	}
	for (int i = 0; ok && i < MAX_WEIGHTMAPS; i++) {
		const bool used = i < new_pages.weightmap_count;
		// Layered RenderingServer textures need at least two layers.
		new_pages.weights[i] = _create_array(RD::DATA_FORMAT_R8G8B8A8_UNORM, used ? PAGE_TEXELS : 1, used ? p_slots : 2, vformat("Landscape Weight Pages %d", i));
		ok = new_pages.weights[i].is_valid();
	}
	if (ok) {
		new_pages.hole_pages = _create_array(RD::DATA_FORMAT_R8_UNORM, p_holes ? PAGE_TEXELS : 1, p_holes ? p_slots : 2, "Landscape Hole Pages");
		ok = new_pages.hole_pages.is_valid();
	}
	if (ok) {
		RD::TextureFormat tf;
		tf.texture_type = RD::TEXTURE_TYPE_2D;
		tf.width = p_table_size.x;
		tf.height = p_table_size.y;
		tf.format = RD::DATA_FORMAT_R32_SFLOAT;
		tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		new_pages.table = rd->texture_create(tf, RD::TextureView());
		ok = new_pages.table.is_valid();
		if (ok) {
			rd->set_resource_name(new_pages.table, "Landscape Page Table");
			rd->texture_clear(new_pages.table, Color(-1, -1, -1, -1), 0, 1, 0, 1);
		}
	}
	if (!ok) {
		_free_pages(new_pages);
		ERR_FAIL_MSG(vformat("Not enough GPU memory for %d landscape pages. Reduce the streaming pool size of the landscape.", p_slots));
	}

	new_pages.bytes = get_slot_size(new_pages.weightmap_count, p_holes) * p_slots + int64_t(p_table_size.x) * p_table_size.y * 4;
	if (WorldStreaming::get_singleton()) {
		WorldStreaming::get_singleton()->add_pool_usage(SNAME("Landscape GPU"), new_pages.bytes);
	}

	// Point the RenderingServer textures used by the material to the new RD textures.
	RenderingServer *rs = RenderingServer::get_singleton();
	if (p_heights_rs.is_valid()) {
		rs->texture_replace(p_heights_rs, rs->texture_rd_create(new_pages.heights, RSE::TEXTURE_LAYERED_2D_ARRAY));
	}
	if (p_normals_rs.is_valid()) {
		rs->texture_replace(p_normals_rs, rs->texture_rd_create(new_pages.normals, RSE::TEXTURE_LAYERED_2D_ARRAY));
	}
	for (int i = 0; i < MAX_WEIGHTMAPS; i++) {
		const RID weights_rs = p_weights_rs[i];
		if (weights_rs.is_valid()) {
			rs->texture_replace(weights_rs, rs->texture_rd_create(new_pages.weights[i], RSE::TEXTURE_LAYERED_2D_ARRAY));
		}
	}
	if (p_holes_rs.is_valid()) {
		rs->texture_replace(p_holes_rs, rs->texture_rd_create(new_pages.hole_pages, RSE::TEXTURE_LAYERED_2D_ARRAY));
	}
	if (p_table_rs.is_valid()) {
		rs->texture_replace(p_table_rs, rs->texture_rd_create(new_pages.table));
	}

	pages = new_pages;
	pages_valid = true;
}

void LandscapeGPU::upload_page(int p_slot, const Vector<uint8_t> &p_data) {
	if (!pages_valid) {
		return;
	}
	ERR_FAIL_INDEX(p_slot, pages.slots);
	constexpr int64_t texels = int64_t(PAGE_TEXELS) * PAGE_TEXELS;
	const int64_t expected = texels * (4 + 2 + 4 * pages.weightmap_count + (pages.holes ? 1 : 0));
	ERR_FAIL_COND(p_data.size() != expected);
	RenderingDevice *rd = RenderingDevice::get_singleton();
	const uint8_t *src = p_data.ptr();

	auto layer = [&](int64_t p_bytes) {
		Vector<uint8_t> data;
		data.resize(p_bytes);
		memcpy(data.ptrw(), src, p_bytes);
		src += p_bytes;
		return data;
	};

	rd->texture_update(pages.heights, p_slot, layer(texels * 4));
	Vector<uint8_t> normals = layer(texels * 2);
	if (pages.normal_format != RD::DATA_FORMAT_R8G8_UNORM) {
		// RGBA8 fallback: X, Z, 0, Z (the shader decodes red and alpha, see ls_decode_normal()).
		Vector<uint8_t> expanded;
		expanded.resize(texels * 4);
		uint8_t *w = expanded.ptrw();
		const uint8_t *r = normals.ptr();
		for (int64_t i = 0; i < texels; i++) {
			w[i * 4 + 0] = r[i * 2 + 0];
			w[i * 4 + 1] = r[i * 2 + 1];
			w[i * 4 + 2] = 0;
			w[i * 4 + 3] = r[i * 2 + 1];
		}
		normals = expanded;
	}
	rd->texture_update(pages.normals, p_slot, normals);
	for (int i = 0; i < pages.weightmap_count; i++) {
		rd->texture_update(pages.weights[i], p_slot, layer(texels * 4));
	}
	if (pages.holes) {
		rd->texture_update(pages.hole_pages, p_slot, layer(texels));
	}
}

void LandscapeGPU::upload_page_table(const Vector<uint8_t> &p_table) {
	if (!pages_valid) {
		return;
	}
	ERR_FAIL_COND(p_table.size() != int64_t(pages.table_size.x) * pages.table_size.y * 4);
	RenderingDevice::get_singleton()->texture_update(pages.table, 0, p_table);
}

void LandscapeGPU::_clear_draw(int p_list) {
	const RID command_buffer = RenderingServer::get_singleton()->multimesh_get_command_buffer_rd_rid(lists[p_list].multimesh);
	if (command_buffer.is_valid()) {
		const uint32_t zero = 0;
		RenderingDevice::get_singleton()->buffer_update(command_buffer, sizeof(uint32_t), sizeof(uint32_t), &zero);
	}
}

/* LOD */

void LandscapeGPU::_free_lod() {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (bounds_buffer.is_valid()) {
		rd->free_rid(bounds_buffer);
		bounds_buffer = RID();
	}
	bounds_size = 0;
	for (LodList &list : lists) {
		RID *buffers[] = { &list.params_buffer, &list.node_lists, &list.counters, &list.args[0], &list.args[1], &list.split_bits };
		for (RID *buffer : buffers) {
			if (buffer->is_valid()) {
				rd->free_rid(*buffer);
				*buffer = RID();
			}
		}
		list = LodList();
	}
}

void LandscapeGPU::setup_lod(int p_node_count, int p_list_capacity, int p_instance_capacity, int p_max_level, RID p_multimesh_main, RID p_multimesh_shadow) {
	_ensure_initialized();
	_free_lod();
	ERR_FAIL_COND(p_node_count <= 0 || p_list_capacity <= 0 || p_instance_capacity <= 0);
	ERR_FAIL_INDEX(p_max_level, MAX_LEVELS);

	RenderingDevice *rd = RenderingDevice::get_singleton();
	bounds_size = uint32_t(p_node_count) * 4 * sizeof(float);
	bounds_buffer = rd->storage_buffer_create(bounds_size);
	rd->set_resource_name(bounds_buffer, "Landscape LOD Bounds");

	// Bits for levels [0, max_level), one bit per node.
	uint32_t split_words = 0;
	for (int l = 0; l < p_max_level; l++) {
		split_words += MAX((1u << (2 * l)) / 32u, 1u);
	}
	split_words = MAX(split_words, 4u);

	const RID multimeshes[LIST_MAX] = { p_multimesh_main, p_multimesh_shadow };
	for (int i = 0; i < LIST_MAX; i++) {
		LodList &list = lists[i];
		list.multimesh = multimeshes[i];
		list.list_capacity = p_list_capacity;
		list.instance_capacity = p_instance_capacity;
		list.split_bits_size = split_words * sizeof(uint32_t);

		list.params_buffer = rd->uniform_buffer_create(sizeof(LandscapeLodParams));
		list.node_lists = rd->storage_buffer_create(uint32_t(p_max_level + 2) * p_list_capacity * sizeof(uint32_t));
		list.counters = rd->storage_buffer_create(COUNTER_COUNT * sizeof(uint32_t));
		for (int j = 0; j < 2; j++) {
			list.args[j] = rd->storage_buffer_create(MAX_LEVELS * 4 * sizeof(uint32_t), {}, RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
		}
		list.split_bits = rd->storage_buffer_create(list.split_bits_size);
	}
}

void LandscapeGPU::upload_bounds(int p_offset, const Vector<float> &p_data) {
	ERR_FAIL_COND(bounds_buffer.is_null());
	const uint32_t offset = uint32_t(p_offset) * sizeof(float);
	const uint32_t bytes = uint32_t(p_data.size()) * sizeof(float);
	ERR_FAIL_COND(offset + bytes > bounds_size);
	if (bytes > 0) {
		RenderingDevice::get_singleton()->buffer_update(bounds_buffer, offset, bytes, p_data.ptr());
	}
}

void LandscapeGPU::run_lod(int p_list, const Vector<uint8_t> &p_params, int p_max_level, bool p_enabled) {
	ERR_FAIL_INDEX(p_list, LIST_MAX);
	ERR_FAIL_COND(p_params.size() != sizeof(LandscapeLodParams));
	ERR_FAIL_INDEX(p_max_level, MAX_LEVELS);
	LodList &list = lists[p_list];
	if (list.params_buffer.is_null() || bounds_buffer.is_null()) {
		return;
	}

	RenderingServer *rs = RenderingServer::get_singleton();
	const RID instance_buffer = rs->multimesh_get_buffer_rd_rid(list.multimesh);
	const RID command_buffer = rs->multimesh_get_command_buffer_rd_rid(list.multimesh);
	if (instance_buffer.is_null() || command_buffer.is_null()) {
		return;
	}

	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (!pages_valid || !p_enabled) {
		// Nothing valid to draw (e.g. out of GPU memory or still streaming the root page).
		_clear_draw(p_list);
		return;
	}
	rd->buffer_update(list.params_buffer, 0, sizeof(LandscapeLodParams), p_params.ptr());

	// Seed the traversal with the root node.
	uint32_t counters[COUNTER_COUNT] = {};
	counters[0] = 1;
	rd->buffer_update(list.counters, 0, sizeof(counters), counters);
	const uint32_t root = 0;
	rd->buffer_update(list.node_lists, 0, sizeof(uint32_t), &root);

	uint32_t args[MAX_LEVELS * 4];
	for (int l = 0; l < MAX_LEVELS; l++) {
		args[l * 4 + 0] = 0;
		args[l * 4 + 1] = 1;
		args[l * 4 + 2] = 1;
		args[l * 4 + 3] = 0;
	}
	rd->buffer_update(list.args[1], 0, sizeof(args), args);
	args[0] = 1; // Level 0 always processes the root.
	rd->buffer_update(list.args[0], 0, sizeof(args), args);
	rd->buffer_clear(list.split_bits, 0, list.split_bits_size);

	UniformSetCacheRD *cache = UniformSetCacheRD::get_singleton();
	RID traverse_shader = shared->lod_shaders[LOD_MODE_TRAVERSE];
	RID traverse_sets[2];
	for (int i = 0; i < 2; i++) {
		// Even levels read their dispatch arguments from args[0] and write the next level into args[1], and vice versa.
		traverse_sets[i] = cache->get_cache(traverse_shader, 0,
				RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, list.params_buffer),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, bounds_buffer),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, list.node_lists),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, list.counters),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, list.args[(i + 1) % 2]),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, list.split_bits),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, instance_buffer),
				RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 8, pages.table));
	}
	RID stitch_set = cache->get_cache(shared->lod_shaders[LOD_MODE_STITCH], 0,
			RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, list.params_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, list.counters),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, list.split_bits),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, instance_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, command_buffer));

	RD::ComputeListID compute_list = rd->compute_list_begin();
	for (int l = 0; l <= p_max_level; l++) {
		LandscapeLodPushConstant push = {};
		push.level = l;
		rd->compute_list_bind_compute_pipeline(compute_list, shared->lod_pipelines[LOD_MODE_TRAVERSE]);
		rd->compute_list_bind_uniform_set(compute_list, traverse_sets[l % 2], 0);
		rd->compute_list_set_push_constant(compute_list, &push, sizeof(LandscapeLodPushConstant));
		rd->compute_list_dispatch_indirect(compute_list, list.args[l % 2], l * 4 * sizeof(uint32_t));
		rd->compute_list_add_barrier(compute_list);
	}
	rd->compute_list_bind_compute_pipeline(compute_list, shared->lod_pipelines[LOD_MODE_STITCH]);
	rd->compute_list_bind_uniform_set(compute_list, stitch_set, 0);
	rd->compute_list_dispatch(compute_list, (list.instance_capacity + 63) / 64, 1, 1);
	rd->compute_list_end();

	// Read back the counters (without stalling) for statistics. The traversal only runs when the view changes.
	rd->buffer_get_data_async(list.counters, callable_mp(this, &LandscapeGPU::_stats_received).bind(p_list), 0, COUNTER_COUNT * sizeof(uint32_t));
}

void LandscapeGPU::_stats_received(const Vector<uint8_t> &p_data, int p_list) {
	ERR_FAIL_INDEX(p_list, LIST_MAX);
	if (p_data.size() < int(COUNTER_COUNT * sizeof(uint32_t))) {
		return;
	}
	const uint32_t *counters = reinterpret_cast<const uint32_t *>(p_data.ptr());
	stat_patches[p_list].set(MIN(counters[20], lists[p_list].instance_capacity));
	stat_overflow[p_list].set(counters[21]);
}

void LandscapeGPU::free_resources() {
	if (!initialized) {
		return;
	}
	_free_pages(pages);
	pages_valid = false;
	_free_lod();

	initialized = false;
	shared_users--;
	if (shared_users == 0 && shared) {
		shared->lod_shader->version_free(shared->lod_version);
		memdelete(shared->lod_shader);
		memdelete(shared);
		shared = nullptr;
	}
}

void LandscapeGPU::destroy(Object *p_gpu) {
	LandscapeGPU *gpu = Object::cast_to<LandscapeGPU>(p_gpu);
	ERR_FAIL_NULL(gpu);
	gpu->free_resources();
	memdelete(gpu);
}

LandscapeGPU::LandscapeGPU() {
}

LandscapeGPU::~LandscapeGPU() {
	ERR_FAIL_COND_MSG(initialized, "LandscapeGPU must be freed on the rendering thread with LandscapeGPU::destroy().");
}

#endif // RD_ENABLED
