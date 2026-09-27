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
#include "shaders/landscape_maps.glsl.gen.h"

#include "core/object/callable_mp.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server.h"

LandscapeGPU::Shared *LandscapeGPU::shared = nullptr;
int LandscapeGPU::shared_users = 0;

struct LandscapeMapsPushConstant {
	int32_t rect_position[2];
	int32_t rect_size[2];
	int32_t source_size[2];
	float spacing;
	float pad;
};

struct LandscapeLodPushConstant {
	uint32_t level;
	uint32_t pad[3];
};

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

	Vector<String> maps_modes;
	maps_modes.push_back("\n#define MODE_NORMALS\n#define FORMAT_RG8\n");
	maps_modes.push_back("\n#define MODE_NORMALS\n");
	maps_modes.push_back("\n#define MODE_DOWNSAMPLE\n");
	maps_modes.push_back("\n#define MODE_DOWNSAMPLE\n#define FORMAT_RG8\n");
	shared->maps_shader = memnew(LandscapeMapsShaderRD);
	shared->maps_shader->initialize(maps_modes);
	shared->maps_version = shared->maps_shader->version_create();
	for (int i = 0; i < MAPS_MODE_MAX; i++) {
		shared->maps_shaders[i] = shared->maps_shader->version_get_shader(shared->maps_version, i);
		shared->maps_pipelines[i] = rd->compute_pipeline_create(shared->maps_shaders[i]);
	}
}

void LandscapeGPU::_free_maps(Maps &r_maps) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	auto free_rid = [rd](RID &r_rid) {
		if (r_rid.is_valid() && rd->texture_is_valid(r_rid)) {
			rd->free_rid(r_rid);
		}
		r_rid = RID();
	};
	// Views first, then the textures owning them.
	for (RID &view : r_maps.normal_views) {
		free_rid(view);
	}
	r_maps.normal_views.clear();
	for (int i = 0; i < MAX_WEIGHTMAPS; i++) {
		for (RID &view : r_maps.weight_views[i]) {
			free_rid(view);
		}
		r_maps.weight_views[i].clear();
		free_rid(r_maps.weights[i]);
	}
	free_rid(r_maps.height);
	free_rid(r_maps.normal);
	free_rid(r_maps.holes);
	r_maps.size = Vector2i();
	r_maps.weightmap_count = 0;
	r_maps.has_holes = false;
}

RID LandscapeGPU::_create_map_texture(RenderingDevice::DataFormat p_format, const Vector2i &p_size, int p_mipmaps, bool p_storage, const String &p_name) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	RD::TextureFormat tf;
	tf.texture_type = RD::TEXTURE_TYPE_2D;
	tf.width = p_size.x;
	tf.height = p_size.y;
	tf.mipmaps = p_mipmaps;
	tf.format = p_format;
	tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
	if (p_storage) {
		tf.usage_bits |= RD::TEXTURE_USAGE_STORAGE_BIT;
	}
	if (p_format == RD::DATA_FORMAT_R8G8B8A8_UNORM) {
		// The RenderingServer creates an sRGB view of RGBA8 textures.
		tf.shareable_formats.push_back(RD::DATA_FORMAT_R8G8B8A8_UNORM);
		tf.shareable_formats.push_back(RD::DATA_FORMAT_R8G8B8A8_SRGB);
	}
	RID texture = rd->texture_create(tf, RD::TextureView());
	if (texture.is_valid()) {
		rd->set_resource_name(texture, p_name);
		rd->texture_clear(texture, Color(0, 0, 0, 0), 0, p_mipmaps, 0, 1);
	}
	return texture;
}

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

void LandscapeGPU::setup_maps(const Vector2i &p_size, int p_weightmap_count, bool p_holes, RID p_height_rs, RID p_normal_rs, const Array &p_weights_rs, RID p_holes_rs) {
	_ensure_initialized();
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_COND(p_size.x < 2 || p_size.y < 2);
	ERR_FAIL_COND(p_weights_rs.size() != MAX_WEIGHTMAPS);

	Maps new_maps;
	new_maps.size = p_size;
	new_maps.mipmaps = Image::get_image_required_mipmaps(p_size.x, p_size.y, Image::FORMAT_RGBA8) + 1;
	new_maps.weightmap_count = CLAMP(p_weightmap_count, 1, MAX_WEIGHTMAPS);
	new_maps.has_holes = p_holes;
	new_maps.normal_format = rd->texture_is_format_supported_for_usage(RD::DATA_FORMAT_R8G8_UNORM, RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT) ? RD::DATA_FORMAT_R8G8_UNORM : RD::DATA_FORMAT_R8G8B8A8_UNORM;

	bool ok = true;
	new_maps.height = _create_map_texture(RD::DATA_FORMAT_R32_SFLOAT, p_size, 1, true, "Landscape Heightmap");
	ok = ok && new_maps.height.is_valid();
	if (ok) {
		new_maps.normal = _create_map_texture(new_maps.normal_format, p_size, new_maps.mipmaps, true, "Landscape Normal Map");
		ok = new_maps.normal.is_valid();
	}
	for (int i = 0; ok && i < MAX_WEIGHTMAPS; i++) {
		// Only the weightmaps in use are allocated at full resolution.
		const bool used = i < new_maps.weightmap_count;
		new_maps.weights[i] = _create_map_texture(RD::DATA_FORMAT_R8G8B8A8_UNORM, used ? p_size : Vector2i(1, 1), used ? new_maps.mipmaps : 1, true, vformat("Landscape Weightmap %d", i));
		ok = new_maps.weights[i].is_valid();
	}
	if (ok) {
		new_maps.holes = _create_map_texture(RD::DATA_FORMAT_R8_UNORM, p_holes ? p_size : Vector2i(1, 1), 1, false, "Landscape Holes");
		ok = new_maps.holes.is_valid();
	}
	if (!ok) {
		_free_maps(new_maps);
		_free_maps(maps);
		maps_valid = false;
		ERR_FAIL_MSG(vformat("Not enough GPU memory for a %dx%d landscape with %d weightmaps. Reduce the resolution or the number of layers.", p_size.x, p_size.y, p_weightmap_count));
	}

	for (int m = 0; m < new_maps.mipmaps; m++) {
		new_maps.normal_views.push_back(rd->texture_create_shared_from_slice(RD::TextureView(), new_maps.normal, 0, m, 1, RD::TEXTURE_SLICE_2D));
	}
	for (int i = 0; i < new_maps.weightmap_count; i++) {
		for (int m = 0; m < new_maps.mipmaps; m++) {
			new_maps.weight_views[i].push_back(rd->texture_create_shared_from_slice(RD::TextureView(), new_maps.weights[i], 0, m, 1, RD::TEXTURE_SLICE_2D));
		}
	}
	// Flat normals until the real data is uploaded.
	rd->texture_clear(new_maps.normal, Color(0.5, 0.5, 0.0, 0.5), 0, new_maps.mipmaps, 0, 1);

	// Point the RenderingServer textures used by the material to the new RD textures.
	RenderingServer *rs = RenderingServer::get_singleton();
	if (p_height_rs.is_valid()) {
		rs->texture_replace(p_height_rs, rs->texture_rd_create(new_maps.height));
	}
	if (p_normal_rs.is_valid()) {
		rs->texture_replace(p_normal_rs, rs->texture_rd_create(new_maps.normal));
	}
	for (int i = 0; i < MAX_WEIGHTMAPS; i++) {
		const RID weights_rs = p_weights_rs[i];
		if (weights_rs.is_valid()) {
			rs->texture_replace(weights_rs, rs->texture_rd_create(new_maps.weights[i]));
		}
	}
	if (p_holes_rs.is_valid()) {
		rs->texture_replace(p_holes_rs, rs->texture_rd_create(new_maps.holes));
	}

	_free_maps(maps);
	maps = std::move(new_maps);
	maps_valid = true;
}

void LandscapeGPU::_upload_rect(RID p_texture, RenderingDevice::DataFormat p_format, const Rect2i &p_rect, int p_layer, const Vector<uint8_t> &p_data) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	RD::TextureFormat tf;
	tf.texture_type = RD::TEXTURE_TYPE_2D;
	tf.width = p_rect.size.x;
	tf.height = p_rect.size.y;
	tf.format = p_format;
	tf.usage_bits = RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
	Vector<Vector<uint8_t>> data;
	data.push_back(p_data);
	RID staging = rd->texture_create(tf, RD::TextureView(), data);
	ERR_FAIL_COND(staging.is_null());
	rd->texture_copy(staging, p_texture, Vector3(), Vector3(p_rect.position.x, p_rect.position.y, 0), Vector3(p_rect.size.x, p_rect.size.y, 1), 0, 0, 0, p_layer);
	rd->free_rid(staging);
}

void LandscapeGPU::upload_heights(const Rect2i &p_rect, const Vector<uint8_t> &p_data) {
	if (!maps_valid) {
		return;
	}
	ERR_FAIL_COND(p_rect.intersection(Rect2i(Point2i(), maps.size)) != p_rect || !p_rect.has_area());
	ERR_FAIL_COND(p_data.size() != int64_t(p_rect.size.x) * p_rect.size.y * 4);
	_upload_rect(maps.height, RD::DATA_FORMAT_R32_SFLOAT, p_rect, 0, p_data);
}

void LandscapeGPU::upload_weights(const Rect2i &p_rect, int p_weightmap, const Vector<uint8_t> &p_data) {
	if (!maps_valid) {
		return;
	}
	ERR_FAIL_INDEX(p_weightmap, maps.weightmap_count);
	ERR_FAIL_COND(p_rect.intersection(Rect2i(Point2i(), maps.size)) != p_rect || !p_rect.has_area());
	ERR_FAIL_COND(p_data.size() != int64_t(p_rect.size.x) * p_rect.size.y * 4);
	_upload_rect(maps.weights[p_weightmap], RD::DATA_FORMAT_R8G8B8A8_UNORM, p_rect, 0, p_data);
}

void LandscapeGPU::upload_holes(const Rect2i &p_rect, const Vector<uint8_t> &p_data) {
	if (!maps_valid || !maps.has_holes) {
		return;
	}
	ERR_FAIL_COND(p_rect.intersection(Rect2i(Point2i(), maps.size)) != p_rect || !p_rect.has_area());
	ERR_FAIL_COND(p_data.size() != int64_t(p_rect.size.x) * p_rect.size.y);
	_upload_rect(maps.holes, RD::DATA_FORMAT_R8_UNORM, p_rect, 0, p_data);
}

void LandscapeGPU::_downsample(const LocalVector<RID> &p_views, int p_view_offset, const Rect2i &p_rect, bool p_rg8) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	UniformSetCacheRD *cache = UniformSetCacheRD::get_singleton();
	const MapsShaderMode mode = p_rg8 ? MAPS_MODE_DOWNSAMPLE_RG8 : MAPS_MODE_DOWNSAMPLE_RGBA8;
	RID shader = shared->maps_shaders[mode];

	Point2i begin = p_rect.position;
	Point2i end = p_rect.get_end();
	Vector2i src_size = maps.size;

	RD::ComputeListID compute_list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(compute_list, shared->maps_pipelines[mode]);
	for (int m = 1; m < maps.mipmaps; m++) {
		const Vector2i dst_size = Vector2i(MAX(src_size.x >> 1, 1), MAX(src_size.y >> 1, 1));
		begin = Point2i(begin.x >> 1, begin.y >> 1);
		end = Point2i(MIN((end.x + 1) >> 1, dst_size.x), MIN((end.y + 1) >> 1, dst_size.y));
		const Vector2i rect_size = (end - begin).maxi(1);

		RD::Uniform u_src(RD::UNIFORM_TYPE_IMAGE, 0, p_views[p_view_offset + m - 1]);
		RD::Uniform u_dst(RD::UNIFORM_TYPE_IMAGE, 1, p_views[p_view_offset + m]);
		RID uniform_set = cache->get_cache(shader, 0, u_src, u_dst);

		LandscapeMapsPushConstant push = {};
		push.rect_position[0] = begin.x;
		push.rect_position[1] = begin.y;
		push.rect_size[0] = rect_size.x;
		push.rect_size[1] = rect_size.y;
		push.source_size[0] = src_size.x;
		push.source_size[1] = src_size.y;

		if (m > 1) {
			rd->compute_list_add_barrier(compute_list);
		}
		rd->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
		rd->compute_list_set_push_constant(compute_list, &push, sizeof(LandscapeMapsPushConstant));
		rd->compute_list_dispatch_threads(compute_list, rect_size.x, rect_size.y, 1);

		src_size = dst_size;
	}
	rd->compute_list_end();
}

void LandscapeGPU::update_normals(const Rect2i &p_rect, float p_spacing) {
	if (!maps_valid) {
		return;
	}
	const Rect2i rect = p_rect.grow(1).intersection(Rect2i(Point2i(), maps.size));
	if (!rect.has_area()) {
		return;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	const bool rg8 = maps.normal_format == RD::DATA_FORMAT_R8G8_UNORM;
	const MapsShaderMode mode = rg8 ? MAPS_MODE_NORMALS_RG8 : MAPS_MODE_NORMALS_RGBA8;
	RID shader = shared->maps_shaders[mode];
	RD::Uniform u_height(RD::UNIFORM_TYPE_IMAGE, 0, maps.height);
	RD::Uniform u_normal(RD::UNIFORM_TYPE_IMAGE, 1, maps.normal_views[0]);
	RID uniform_set = UniformSetCacheRD::get_singleton()->get_cache(shader, 0, u_height, u_normal);

	LandscapeMapsPushConstant push = {};
	push.rect_position[0] = rect.position.x;
	push.rect_position[1] = rect.position.y;
	push.rect_size[0] = rect.size.x;
	push.rect_size[1] = rect.size.y;
	push.source_size[0] = maps.size.x;
	push.source_size[1] = maps.size.y;
	push.spacing = p_spacing;

	RD::ComputeListID compute_list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(compute_list, shared->maps_pipelines[mode]);
	rd->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
	rd->compute_list_set_push_constant(compute_list, &push, sizeof(LandscapeMapsPushConstant));
	rd->compute_list_dispatch_threads(compute_list, rect.size.x, rect.size.y, 1);
	rd->compute_list_end();

	_downsample(maps.normal_views, 0, rect, rg8);
}

void LandscapeGPU::update_weight_mips(const Rect2i &p_rect) {
	if (!maps_valid) {
		return;
	}
	const Rect2i rect = p_rect.intersection(Rect2i(Point2i(), maps.size));
	if (!rect.has_area()) {
		return;
	}
	for (int i = 0; i < maps.weightmap_count; i++) {
		_downsample(maps.weight_views[i], 0, rect, false);
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

void LandscapeGPU::run_lod(int p_list, const Vector<uint8_t> &p_params, int p_max_level) {
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
	if (!maps_valid) {
		// Nothing valid to draw (e.g. out of GPU memory): clear the instance count of the indirect draw.
		const uint32_t zero = 0;
		rd->buffer_update(command_buffer, sizeof(uint32_t), sizeof(uint32_t), &zero);
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
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, instance_buffer));
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

	// Occasionally read back the counters (without stalling) for statistics.
	if ((stat_counter[p_list]++ % 16) == 0) {
		rd->buffer_get_data_async(list.counters, callable_mp(this, &LandscapeGPU::_stats_received).bind(p_list), 0, COUNTER_COUNT * sizeof(uint32_t));
	}
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
	_free_maps(maps);
	maps_valid = false;
	_free_lod();

	initialized = false;
	shared_users--;
	if (shared_users == 0 && shared) {
		shared->lod_shader->version_free(shared->lod_version);
		shared->maps_shader->version_free(shared->maps_version);
		memdelete(shared->lod_shader);
		memdelete(shared->maps_shader);
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
