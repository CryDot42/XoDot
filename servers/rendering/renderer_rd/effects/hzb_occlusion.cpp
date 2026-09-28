/**************************************************************************/
/*  hzb_occlusion.cpp                                                     */
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

#include "hzb_occlusion.h"

#include "core/object/callable_mp.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server_globals.h"

using namespace RendererRD;

void HZBOcclusion::HZBOcclusionBuffers::_free_readback_buffer() {
	if (readback_buffer.is_valid()) {
		ERR_FAIL_NULL(RD::get_singleton());
		// The buffer is only freed once the frames using it are done, so pending readbacks remain valid.
		RD::get_singleton()->free_rid(readback_buffer);
		readback_buffer = RID();
	}
	readback_buffer_size = 0;
}

void HZBOcclusion::HZBOcclusionBuffers::_readback_completed(const Vector<uint8_t> &p_data, uint64_t p_id) {
	const PendingReadback &pending = pending_readbacks[p_id % MAX_PENDING_READBACKS];
	if (pending.id != p_id) {
		return; // Too many readbacks in flight, the metadata of this one was overwritten.
	}

	if (latest_readback.view_count > 0 && pending.frame < latest_readback.frame) {
		return; // Older than what we already have.
	}

	const uint32_t texel_count = pending.size.x * pending.size.y * pending.view_count;
	ERR_FAIL_COND(uint64_t(p_data.size()) < uint64_t(texel_count) * sizeof(float));

	latest_readback.depth.resize(texel_count);
	memcpy(latest_readback.depth.ptr(), p_data.ptr(), texel_count * sizeof(float));
	latest_readback.size = pending.size;
	latest_readback.view_count = pending.view_count;
	latest_readback.cam_transform = pending.cam_transform;
	latest_readback.cam_orthogonal = pending.cam_orthogonal;
	for (uint32_t v = 0; v < pending.view_count; v++) {
		latest_readback.inv_projection[v] = pending.inv_projection[v];
	}
	latest_readback.frame = pending.frame;
}

void HZBOcclusion::HZBOcclusionBuffers::free_data() {
	// Called when the render buffers are reconfigured (e.g. resized). The last readback is kept,
	// since it can be reprojected regardless of the size of the render buffers.
	_free_readback_buffer();
}

HZBOcclusion::HZBOcclusionBuffers::~HZBOcclusionBuffers() {
	_free_readback_buffer();
}

////////////////////////////////////////////////////////

HZBOcclusion::HZBOcclusion() {
	Vector<String> modes;
	modes.push_back("\n");
	modes.push_back("\n#define MODE_MSAA\n");

	downsample_shader.initialize(modes);
	downsample_shader_version = downsample_shader.version_create();

	for (int i = 0; i < DOWNSAMPLE_MODE_MAX; i++) {
		downsample_pipelines[i] = RD::get_singleton()->compute_pipeline_create(downsample_shader.version_get_shader(downsample_shader_version, i));
	}
}

HZBOcclusion::~HZBOcclusion() {
	downsample_shader.version_free(downsample_shader_version);
}

const RendererSceneOcclusionCull::DepthReadback *HZBOcclusion::request_depth(Ref<RenderSceneBuffersRD> p_render_buffers, const Size2i &p_size) {
	ERR_FAIL_COND_V(p_render_buffers.is_null(), nullptr);

	Ref<HZBOcclusionBuffers> buffers;
	if (p_render_buffers->has_custom_data(RB_HZB_OCCLUSION_BUFFERS)) {
		buffers = p_render_buffers->get_custom_data(RB_HZB_OCCLUSION_BUFFERS);
	} else {
		buffers.instantiate();
		p_render_buffers->set_custom_data(RB_HZB_OCCLUSION_BUFFERS, buffers);
	}

	buffers->requested_size = p_size.maxi(0);

	return buffers->latest_readback.is_valid() ? &buffers->latest_readback : nullptr;
}

bool HZBOcclusion::is_depth_requested(Ref<RenderSceneBuffersRD> p_render_buffers) const {
	if (p_render_buffers.is_null() || !p_render_buffers->has_custom_data(RB_HZB_OCCLUSION_BUFFERS)) {
		return false;
	}

	Ref<HZBOcclusionBuffers> buffers = p_render_buffers->get_custom_data(RB_HZB_OCCLUSION_BUFFERS);
	return buffers->requested_size.x > 0 && buffers->requested_size.y > 0;
}

void HZBOcclusion::capture_depth(Ref<RenderSceneBuffersRD> p_render_buffers, const RenderSceneDataRD *p_scene_data, bool p_msaa) {
	if (!is_depth_requested(p_render_buffers)) {
		return;
	}
	ERR_FAIL_NULL(p_scene_data);

	Ref<HZBOcclusionBuffers> buffers = p_render_buffers->get_custom_data(RB_HZB_OCCLUSION_BUFFERS);
	const Size2i size = buffers->requested_size;
	buffers->requested_size = Size2i();

	const Size2i source_size = p_render_buffers->get_internal_size();
	if (source_size.x <= 0 || source_size.y <= 0) {
		return;
	}

	const uint32_t view_count = MIN(p_render_buffers->get_view_count(), HZBOcclusionBuffers::MAX_VIEWS);
	const uint32_t texel_count = size.x * size.y;
	const uint32_t buffer_size = texel_count * view_count * sizeof(float);

	if (buffers->readback_buffer.is_null() || buffers->readback_buffer_size != buffer_size) {
		buffers->_free_readback_buffer();
		buffers->readback_buffer = RD::get_singleton()->storage_buffer_create(buffer_size);
		ERR_FAIL_COND(buffers->readback_buffer.is_null());
		buffers->readback_buffer_size = buffer_size;
	}

	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	const DownsampleMode mode = p_msaa ? DOWNSAMPLE_MODE_MSAA : DOWNSAMPLE_MODE_DEFAULT;
	RID shader = downsample_shader.version_get_shader(downsample_shader_version, mode);
	ERR_FAIL_COND(shader.is_null());

	RID default_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RD::Uniform u_dest_depth(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, buffers->readback_buffer);

	DownsamplePushConstant push_constant = {};
	push_constant.source_size[0] = source_size.x;
	push_constant.source_size[1] = source_size.y;
	push_constant.dest_size[0] = size.x;
	push_constant.dest_size[1] = size.y;
	push_constant.sample_count = p_msaa ? (1 << int(p_render_buffers->get_texture_samples())) : 1;

	RD::get_singleton()->draw_command_begin_label("HZB Occlusion Depth Downsample");

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, downsample_pipelines[mode]);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 1, u_dest_depth), 1);

	for (uint32_t v = 0; v < view_count; v++) {
		RID source_depth = p_msaa ? p_render_buffers->get_depth_msaa(v) : p_render_buffers->get_depth_texture(v);
		RD::Uniform u_source_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ default_sampler, source_depth }));
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader, 0, u_source_depth), 0);

		push_constant.dest_offset = v * texel_count;
		RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(DownsamplePushConstant));
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, size.x, size.y, 1);
	}

	RD::get_singleton()->compute_list_end();

	RD::get_singleton()->draw_command_end_label();

	// Store what's needed to reproject the depth buffer once it's read back.
	const uint64_t id = buffers->next_readback_id++;
	HZBOcclusionBuffers::PendingReadback &pending = buffers->pending_readbacks[id % HZBOcclusionBuffers::MAX_PENDING_READBACKS];
	pending.id = id;
	pending.size = size;
	pending.view_count = view_count;
	pending.cam_transform = p_scene_data->cam_transform;
	pending.cam_orthogonal = p_scene_data->cam_orthogonal;
	for (uint32_t v = 0; v < view_count; v++) {
		// This is the projection used for rendering, which includes the depth correction (reversed Z, flipped Y) and TAA jitter.
		const Projection projection = view_count == 1 ? p_scene_data->get_cam_projection() : p_scene_data->get_view_projection(v);
		pending.inv_projection[v] = projection.inverse();
	}
	pending.frame = RSG::rasterizer->get_frame_number();

	Error err = RD::get_singleton()->buffer_get_data_async(buffers->readback_buffer, callable_mp(buffers.ptr(), &HZBOcclusionBuffers::_readback_completed).bind(id), 0, buffer_size);
	if (err != OK) {
		pending.id = 0;
	}
}
