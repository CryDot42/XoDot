/**************************************************************************/
/*  hzb_occlusion.h                                                       */
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

#include "servers/rendering/renderer_rd/shaders/effects/hzb_occlusion_downsample.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_data_rd.h"
#include "servers/rendering/renderer_scene_occlusion_cull.h"

#define RB_HZB_OCCLUSION_BUFFERS SNAME("hzb_occlusion_buffers")

namespace RendererRD {

// Captures a downsampled copy of the depth buffer and reads it back asynchronously,
// so that it can be used on the CPU for HZB (Hierarchical Z-Buffer) occlusion culling.
// The readback arrives a few frames later, it's reprojected into the current camera by
// `RendererSceneOcclusionCull::HZBuffer::reproject_depth()`.
class HZBOcclusion {
private:
	enum DownsampleMode {
		DOWNSAMPLE_MODE_DEFAULT,
		DOWNSAMPLE_MODE_MSAA,
		DOWNSAMPLE_MODE_MAX,
	};

	struct DownsamplePushConstant {
		int32_t source_size[2];
		int32_t dest_size[2];
		uint32_t dest_offset;
		int32_t sample_count;
		uint32_t pad[2];
	};

	HzbOcclusionDownsampleShaderRD downsample_shader;
	RID downsample_shader_version;
	RID downsample_pipelines[DOWNSAMPLE_MODE_MAX];

public:
	class HZBOcclusionBuffers : public RenderBufferCustomDataRD {
		GDCLASS(HZBOcclusionBuffers, RenderBufferCustomDataRD);

		friend class HZBOcclusion;

		static constexpr uint32_t MAX_VIEWS = RendererSceneOcclusionCull::DepthReadback::MAX_VIEWS;
		// Readbacks usually take as many frames as there are frames in flight, but some may be delayed further.
		static constexpr uint32_t MAX_PENDING_READBACKS = 8;

		struct PendingReadback {
			uint64_t id = 0; // Zero when unused.
			Size2i size;
			uint32_t view_count = 0;
			Transform3D cam_transform;
			bool cam_orthogonal = false;
			Projection inv_projection[MAX_VIEWS];
			uint64_t frame = 0;
		};

		// Size of the depth capture requested for the next frame, or zero if nothing was requested.
		Size2i requested_size;

		RID readback_buffer;
		uint32_t readback_buffer_size = 0;

		PendingReadback pending_readbacks[MAX_PENDING_READBACKS];
		uint64_t next_readback_id = 1;

		RendererSceneOcclusionCull::DepthReadback latest_readback;

		void _free_readback_buffer();
		void _readback_completed(const Vector<uint8_t> &p_data, uint64_t p_id);

	public:
		virtual void configure(RenderSceneBuffersRD *p_render_buffers) override {}
		virtual void free_data() override;

		~HZBOcclusionBuffers();
	};

	// Requests the depth buffer of the next frame to be captured, and returns the latest readback (if any).
	const RendererSceneOcclusionCull::DepthReadback *request_depth(Ref<RenderSceneBuffersRD> p_render_buffers, const Size2i &p_size);
	bool is_depth_requested(Ref<RenderSceneBuffersRD> p_render_buffers) const;
	// Downsamples the depth buffer of the render buffers and starts reading it back, if it was requested.
	// If `p_msaa` is `true`, the multisampled depth buffer is used instead of the resolved one.
	void capture_depth(Ref<RenderSceneBuffersRD> p_render_buffers, const RenderSceneDataRD *p_scene_data, bool p_msaa);

	HZBOcclusion();
	~HZBOcclusion();
};

} // namespace RendererRD
