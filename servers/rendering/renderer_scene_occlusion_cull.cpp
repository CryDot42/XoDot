/**************************************************************************/
/*  renderer_scene_occlusion_cull.cpp                                     */
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

#include "renderer_scene_occlusion_cull.h"

#include "core/config/engine.h"
#include "core/object/worker_thread_pool.h"
#include "servers/rendering/rendering_server.h"

RendererSceneOcclusionCull *RendererSceneOcclusionCull::singleton = nullptr;

bool RendererSceneOcclusionCull::HZBuffer::occlusion_jitter_enabled = false;

void RendererSceneOcclusionCull::HZBuffer::clear() {
	if (sizes.is_empty()) {
		return; // Already cleared
	}

	data.clear();
	sizes.clear();
	mips.clear();

	debug_data.clear();
	if (debug_image.is_valid()) {
		debug_image.unref();
	}

	ERR_FAIL_NULL(RenderingServer::get_singleton());
	RS::get_singleton()->free_rid(debug_texture);
}

void RendererSceneOcclusionCull::HZBuffer::resize(const Size2i &p_size) {
	occlusion_buffer_size = p_size;

	if (p_size == Size2i()) {
		clear();
		return;
	}

	if (!sizes.is_empty() && p_size == sizes[0]) {
		return; // Size didn't change
	}

	int mip_count = 0;
	int data_size = 0;
	int w = p_size.x;
	int h = p_size.y;

	while (true) {
		data_size += h * w;

		w = MAX(1, w >> 1);
		h = MAX(1, h >> 1);

		mip_count++;

		if (w == 1U && h == 1U) {
			data_size += 1U;
			mip_count++;
			break;
		}
	}

	data.resize(data_size);
	mips.resize(mip_count);
	sizes.resize(mip_count);

	w = p_size.x;
	h = p_size.y;
	float *ptr = data.ptr();

	for (int i = 0; i < mip_count; i++) {
		sizes[i] = Size2i(w, h);
		mips[i] = ptr;

		ptr = &ptr[w * h];
		w = MAX(1, w >> 1);
		h = MAX(1, h >> 1);
	}

	for (int i = 0; i < data_size; i++) {
		data[i] = FLT_MAX;
	}

	debug_data.resize(sizes[0].x * sizes[0].y);
	if (debug_texture.is_valid()) {
		RS::get_singleton()->free_rid(debug_texture);
		debug_texture = RID();
	}
}

void RendererSceneOcclusionCull::HZBuffer::clear_data() {
	if (sizes.is_empty()) {
		return;
	}

	float *ptr = mips[0];
	const int texel_count = sizes[0].x * sizes[0].y;
	for (int i = 0; i < texel_count; i++) {
		ptr[i] = FLT_MAX;
	}
}

void RendererSceneOcclusionCull::HZBuffer::_reproject_texel_from_row(const ReprojectionData &p_data, const Vector4 &p_view_row, const Vector4 &p_clip_row, real_t p_ndc_x, float p_depth, ReprojectedTexel &r_texel) {
	// Texels where nothing was rendered are reprojected from the far plane, and don't occlude anything.
	// They must still be written, so that they're not mistaken for holes in the reprojection.
	const bool background = !(p_depth > 0.0f);
	const real_t depth = background ? 0.0f : p_depth;

	// Homogeneous coordinates are linear in the normalized device coordinates of the readback.
	const Vector4 view_h = p_view_row + p_data.src_ndc_to_view.columns[0] * p_ndc_x + p_data.src_ndc_to_view.columns[2] * depth;
	const Vector3 view = Vector3(view_h.x, view_h.y, view_h.z) / view_h.w;
	r_texel.valid = -view.z >= p_data.z_near; // Also false for NaN.
	if (!r_texel.valid) {
		return; // Behind the camera.
	}

	const Vector4 clip = p_clip_row + p_data.src_ndc_to_clip.columns[0] * p_ndc_x + p_data.src_ndc_to_clip.columns[2] * depth;
	r_texel.position = (Vector2(clip.x, clip.y) / clip.w + Vector2(1.0f, 1.0f)) * p_data.target_scale;
	// Same distance metric as the one used when testing occlusion.
	r_texel.distance = background ? FLT_MAX : (p_data.cam_orthogonal ? float(-view.z) : float(view.length()));
	r_texel.occluder = true;
}

void RendererSceneOcclusionCull::HZBuffer::_reproject_texel(const ReprojectionData &p_data, int p_x, int p_y, float p_depth, ReprojectedTexel &r_texel) const {
	const real_t ndc_x = (p_x + 0.5f) / p_data.src_size.x * 2.0f - 1.0f;
	const real_t ndc_y = (p_y + 0.5f) / p_data.src_size.y * 2.0f - 1.0f;
	const Vector4 view_row = p_data.src_ndc_to_view.columns[1] * ndc_y + p_data.src_ndc_to_view.columns[3];
	const Vector4 clip_row = p_data.src_ndc_to_clip.columns[1] * ndc_y + p_data.src_ndc_to_clip.columns[3];
	_reproject_texel_from_row(p_data, view_row, clip_row, ndc_x, p_depth, r_texel);
}

void RendererSceneOcclusionCull::HZBuffer::_reproject_row(uint32_t p_row, const ReprojectionData *p_data) {
	const int width = p_data->src_size.x;
	const float *depth = p_data->depth + p_row * width;
	ReprojectedTexel *texels = reprojected_texels.ptr() + p_row * width;

	const real_t ndc_y = (p_row + 0.5f) / p_data->src_size.y * 2.0f - 1.0f;
	const Vector4 view_row = p_data->src_ndc_to_view.columns[1] * ndc_y + p_data->src_ndc_to_view.columns[3];
	const Vector4 clip_row = p_data->src_ndc_to_clip.columns[1] * ndc_y + p_data->src_ndc_to_clip.columns[3];
	const real_t ndc_x_step = 2.0f / width;

	for (int x = 0; x < width; x++) {
		_reproject_texel_from_row(*p_data, view_row, clip_row, (x + 0.5f) * ndc_x_step - 1.0f, depth[x], texels[x]);
	}
}

void RendererSceneOcclusionCull::HZBuffer::_find_disocclusions_row(uint32_t p_row, const ReprojectionData *p_data) {
	// When the camera moves, parts of the scene that weren't visible in the readback can appear next to the edges
	// of objects (disocclusion). Gaps that are large enough are left as holes in the reprojection, but smaller ones
	// would be covered by the texels around them. Find the edges where such gaps open, and stop them from occluding.

	// Neighboring texels whose distances differ by more than this ratio are considered to belong to different surfaces.
	constexpr float DISCONTINUITY_RATIO = 1.1f;
	// Minimum gap (in texels) opening between a surface and the one behind it for the edge of the former to stop occluding.
	constexpr float DISOCCLUSION_THRESHOLD = 0.05f;

	const Size2i &src_size = p_data->src_size;
	const int y = p_row;
	// Only the texels of this row are written, so rows can be processed in parallel.
	ReprojectedTexel *row = reprojected_texels.ptr() + y * src_size.x;
	const ReprojectedTexel *neighbor_rows[2] = {
		y > 0 ? row - src_size.x : nullptr,
		y + 1 < src_size.y ? row + src_size.x : nullptr,
	};

	for (int x = 0; x < src_size.x; x++) {
		ReprojectedTexel &texel = row[x];
		if (!texel.valid || texel.distance == FLT_MAX) {
			continue;
		}

		const float discontinuity_distance = texel.distance * DISCONTINUITY_RATIO;
		const ReprojectedTexel *neighbors[4] = {
			x > 0 ? &row[x - 1] : nullptr,
			x + 1 < src_size.x ? &row[x + 1] : nullptr,
			neighbor_rows[0] ? &neighbor_rows[0][x] : nullptr,
			neighbor_rows[1] ? &neighbor_rows[1][x] : nullptr,
		};

		for (int n = 0; n < 4; n++) {
			const ReprojectedTexel *neighbor = neighbors[n];
			if (!neighbor || !neighbor->valid || neighbor->distance <= discontinuity_distance) {
				continue; // Nearer, or likely to be the same surface.
			}

			// Where the neighbor would be if it was at the same depth as this texel (i.e. on the same surface).
			const int neighbor_index = (y + (n == 2 ? -1 : (n == 3 ? 1 : 0))) * src_size.x + x + (n == 0 ? -1 : (n == 1 ? 1 : 0));
			ReprojectedTexel same_depth;
			_reproject_texel(*p_data, neighbor_index % src_size.x, neighbor_index / src_size.x, p_data->depth[y * src_size.x + x], same_depth);
			if (!same_depth.valid) {
				texel.occluder = false;
				break;
			}

			// Parallax moves the neighbor away from this texel when a gap opens between them.
			const Vector2 direction = same_depth.position - texel.position;
			const real_t length = direction.length();
			if (length > CMP_EPSILON && (neighbor->position - same_depth.position).dot(direction) / length > DISOCCLUSION_THRESHOLD) {
				texel.occluder = false;
				break;
			}
		}
	}
}

void RendererSceneOcclusionCull::HZBuffer::_unocclude_bounds(float *p_data, const AABB &p_aabb, const Transform3D &p_cam_inv_transform, const Projection &p_cam_projection) const {
	const real_t z_near = p_cam_projection.get_z_near();

	Vector3 corners[8];
	bool in_front[8];
	bool any_in_front = false;
	for (int i = 0; i < 8; i++) {
		corners[i] = p_cam_inv_transform.xform(p_aabb.get_endpoint(i));
		in_front[i] = -corners[i].z >= z_near;
		any_in_front = any_in_front || in_front[i];
	}
	if (!any_in_front) {
		return; // Fully behind the camera.
	}

	// Screen space bounds of the part in front of the near plane: corners in front of it,
	// and intersections of the edges crossing it.
	Vector2 rect_min = Vector2(FLT_MAX, FLT_MAX);
	Vector2 rect_max = Vector2(-FLT_MAX, -FLT_MAX);
	auto add_point = [&](const Vector3 &p_view) {
		const Vector3 projected = p_cam_projection.xform(p_view);
		const Vector2 point = Vector2(projected.x, projected.y);
		rect_min = rect_min.min(point);
		rect_max = rect_max.max(point);
	};

	for (int i = 0; i < 8; i++) {
		if (in_front[i]) {
			add_point(corners[i]);
		}
		for (int axis = 0; axis < 3; axis++) {
			const int j = i | (1 << axis); // Endpoints differing by one axis are connected by an edge.
			if (j != i && in_front[i] != in_front[j]) {
				const real_t t = (-z_near - corners[i].z) / (corners[j].z - corners[i].z);
				add_point(corners[i].lerp(corners[j], t));
			}
		}
	}

	// Include the neighboring texels, as the edges of the reprojection are only accurate to a texel.
	const Size2i &size = sizes[0];
	const int from_x = CLAMP(int(Math::floor((rect_min.x * 0.5f + 0.5f) * size.x)) - 1, 0, size.x);
	const int from_y = CLAMP(int(Math::floor((rect_min.y * 0.5f + 0.5f) * size.y)) - 1, 0, size.y);
	const int to_x = CLAMP(int(Math::ceil((rect_max.x * 0.5f + 0.5f) * size.x)) + 1, 0, size.x);
	const int to_y = CLAMP(int(Math::ceil((rect_max.y * 0.5f + 0.5f) * size.y)) + 1, 0, size.y);

	for (int y = from_y; y < to_y; y++) {
		for (int x = from_x; x < to_x; x++) {
			p_data[y * size.x + x] = FLT_MAX;
		}
	}
}

bool RendererSceneOcclusionCull::HZBuffer::reproject_depth(const DepthReadback &p_readback, const Transform3D &p_cam_transform, const Projection &p_cam_projection, bool p_cam_orthogonal, bool p_merge, Span<AABB> p_changed_bounds) {
	if (is_empty() || !p_readback.is_valid()) {
		return false;
	}

	// Below this, the overhead of threading is higher than what it saves.
	constexpr int THREADED_REPROJECTION_MIN_TEXELS = 16384;

	const Size2i &size = sizes[0];
	const int texel_count = size.x * size.y;

	// Forward-splat every texel of the readback into the current view. Negative values mean that nothing landed in a texel.
	reprojection.resize(texel_count);
	float *splat = reprojection.ptr();
	for (int i = 0; i < texel_count; i++) {
		splat[i] = -1.0f;
	}

	const Size2i &src_size = p_readback.size;
	const int src_texel_count = src_size.x * src_size.y;
	const Transform3D src_view_to_view = p_cam_transform.affine_inverse() * p_readback.cam_transform;
	reprojected_texels.resize(src_texel_count);
	bool any_written = false;

	ReprojectionData reprojection_data;
	reprojection_data.src_size = src_size;
	reprojection_data.z_near = p_cam_projection.get_z_near();
	reprojection_data.target_scale = Vector2(size) * 0.5f;
	reprojection_data.cam_orthogonal = p_cam_orthogonal;

	// Without parallax (i.e. when a perspective camera only rotates), nothing can be disoccluded.
	const bool find_disocclusions = p_cam_orthogonal || p_readback.cam_orthogonal || !src_view_to_view.origin.is_zero_approx();

	for (uint32_t v = 0; v < p_readback.view_count; v++) {
		reprojection_data.depth = &p_readback.depth[v * src_texel_count];
		reprojection_data.src_ndc_to_view = Projection(src_view_to_view) * p_readback.inv_projection[v];
		reprojection_data.src_ndc_to_clip = p_cam_projection * reprojection_data.src_ndc_to_view;

		if (src_texel_count >= THREADED_REPROJECTION_MIN_TEXELS) {
			WorkerThreadPool::GroupID group_task = WorkerThreadPool::get_singleton()->add_template_group_task(this, &HZBuffer::_reproject_row, &reprojection_data, src_size.y, -1, true, SNAME("HZBOcclusionReproject"));
			WorkerThreadPool::get_singleton()->wait_for_group_task_completion(group_task);

			if (find_disocclusions) {
				group_task = WorkerThreadPool::get_singleton()->add_template_group_task(this, &HZBuffer::_find_disocclusions_row, &reprojection_data, src_size.y, -1, true, SNAME("HZBOcclusionFindDisocclusions"));
				WorkerThreadPool::get_singleton()->wait_for_group_task_completion(group_task);
			}
		} else {
			for (int y = 0; y < src_size.y; y++) {
				_reproject_row(y, &reprojection_data);
			}
			if (find_disocclusions) {
				for (int y = 0; y < src_size.y; y++) {
					_find_disocclusions_row(y, &reprojection_data);
				}
			}
		}

		const ReprojectedTexel *texels = reprojected_texels.ptr();
		for (int i = 0; i < src_texel_count; i++) {
			const ReprojectedTexel &texel = texels[i];
			if (!texel.valid) {
				continue;
			}

			// Written this way to also discard NaNs, and to avoid converting out of range values to integers.
			if (!(texel.position.x >= 0.0f && texel.position.y >= 0.0f && texel.position.x < size.x && texel.position.y < size.y)) {
				continue;
			}
			const int tx = MIN(int(texel.position.x), size.x - 1);
			const int ty = MIN(int(texel.position.y), size.y - 1);

			// Several texels can land in the same place. Keep the farthest one to remain conservative.
			float &dst = splat[ty * size.x + tx];
			dst = MAX(dst, texel.occluder ? texel.distance : FLT_MAX);
			any_written = true;
		}
	}

	if (!any_written) {
		return false;
	}

	// Forward-splatting leaves small cracks when surfaces get closer to the camera. Fill texels that have
	// valid neighbors on two opposite sides with the farthest of them, leave all other holes unoccluded.
	static const int crack_offsets[4][2] = { { 1, 0 }, { 0, 1 }, { 1, 1 }, { 1, -1 } };

	reprojection_result.resize(texel_count);
	float *result = reprojection_result.ptr();
	for (int y = 0; y < size.y; y++) {
		for (int x = 0; x < size.x; x++) {
			float value = splat[y * size.x + x];

			if (value < 0.0f) {
				float fill = -1.0f;
				for (int i = 0; i < 4; i++) {
					const int x0 = x - crack_offsets[i][0];
					const int y0 = y - crack_offsets[i][1];
					const int x1 = x + crack_offsets[i][0];
					const int y1 = y + crack_offsets[i][1];
					if (x0 < 0 || x1 >= size.x || MIN(y0, y1) < 0 || MAX(y0, y1) >= size.y) {
						continue;
					}

					const float a = splat[y0 * size.x + x0];
					const float b = splat[y1 * size.x + x1];
					if (a >= 0.0f && b >= 0.0f) {
						fill = MAX(fill, MAX(a, b));
					}
				}

				value = fill >= 0.0f ? fill : FLT_MAX;
			}

			result[y * size.x + x] = value;
		}
	}

	// Objects that moved (or disappeared) since the depth buffer was rendered are still where they were in the
	// reprojection. Don't let them occlude what they may have revealed.
	if (!p_changed_bounds.is_empty()) {
		const Transform3D cam_inv_transform = p_cam_transform.affine_inverse();
		for (const AABB &aabb : p_changed_bounds) {
			_unocclude_bounds(result, aabb, cam_inv_transform, p_cam_projection);
		}
	}

	float *dst = mips[0];
	for (int i = 0; i < texel_count; i++) {
		dst[i] = p_merge ? MIN(dst[i], result[i]) : result[i];
	}

	if (!p_merge) {
		debug_tex_range = p_cam_projection.get_z_far();
	}

	return true;
}

void RendererSceneOcclusionCull::HZBuffer::update_mips() {
	// Keep this up to date as a local to be used for occlusion timers.
	occlusion_frame = Engine::get_singleton()->get_frames_drawn();

	if (sizes.is_empty()) {
		return;
	}

	for (uint32_t mip = 1; mip < mips.size(); mip++) {
		for (int y = 0; y < sizes[mip].y; y++) {
			for (int x = 0; x < sizes[mip].x; x++) {
				int prev_x = x * 2;
				int prev_y = y * 2;

				int prev_w = sizes[mip - 1].width;
				int prev_h = sizes[mip - 1].height;

				bool odd_w = (prev_w % 2) != 0;
				bool odd_h = (prev_h % 2) != 0;

#define CHECK_OFFSET(xx, yy) max_depth = MAX(max_depth, mips[mip - 1][MIN(prev_h - 1, prev_y + (yy)) * prev_w + MIN(prev_w - 1, prev_x + (xx))])

				float max_depth = mips[mip - 1][prev_y * sizes[mip - 1].x + prev_x];
				CHECK_OFFSET(0, 1);
				CHECK_OFFSET(1, 0);
				CHECK_OFFSET(1, 1);

				if (odd_w) {
					CHECK_OFFSET(2, 0);
					CHECK_OFFSET(2, 1);
				}

				if (odd_h) {
					CHECK_OFFSET(0, 2);
					CHECK_OFFSET(1, 2);
				}

				if (odd_w && odd_h) {
					CHECK_OFFSET(2, 2);
				}

				mips[mip][y * sizes[mip].x + x] = max_depth;
#undef CHECK_OFFSET
			}
		}
	}
}

RID RendererSceneOcclusionCull::HZBuffer::get_debug_texture() {
	if (sizes.is_empty() || sizes[0] == Size2i()) {
		return RID();
	}

	if (debug_image.is_null()) {
		debug_image.instantiate();
	}

	unsigned char *ptrw = debug_data.ptrw();
	for (int i = 0; i < debug_data.size(); i++) {
		ptrw[i] = MIN(Math::log(1.0 + mips[0][i]) / Math::log(1.0 + debug_tex_range), 1.0) * 255;
	}

	debug_image->set_data(sizes[0].x, sizes[0].y, false, Image::FORMAT_L8, debug_data);

	if (debug_texture.is_null()) {
		debug_texture = RS::get_singleton()->texture_2d_create(debug_image);
	} else {
		RenderingServer::get_singleton()->texture_2d_update(debug_texture, debug_image);
	}

	return debug_texture;
}

////////////////////////////////////////////////////////

void RendererSceneOcclusionCull::add_buffer(RID p_buffer) {
	ERR_FAIL_COND(buffers.has(p_buffer));
	buffers[p_buffer] = HZBuffer();
}

void RendererSceneOcclusionCull::remove_buffer(RID p_buffer) {
	ERR_FAIL_COND(!buffers.has(p_buffer));
	buffers[p_buffer].clear();
	buffers.erase(p_buffer);
}

RendererSceneOcclusionCull::HZBuffer *RendererSceneOcclusionCull::buffer_get_ptr(RID p_buffer) {
	return buffers.getptr(p_buffer);
}

void RendererSceneOcclusionCull::buffer_set_size(RID p_buffer, const Vector2i &p_size) {
	HZBuffer *buffer = buffers.getptr(p_buffer);
	ERR_FAIL_NULL(buffer);
	buffer->resize(p_size);
}

void RendererSceneOcclusionCull::buffer_update(RID p_buffer, const Transform3D &p_cam_transform, const Projection &p_cam_projection, bool p_cam_orthogonal, const DepthReadback *p_depth_readback, Span<AABB> p_changed_bounds) {
	HZBuffer *buffer = buffers.getptr(p_buffer);
	if (!buffer || buffer->is_empty()) {
		return;
	}

	if (buffer->is_using_occluders()) {
		_print_warning();
	}

	if (buffer->is_using_depth_readback() && p_depth_readback && buffer->reproject_depth(*p_depth_readback, p_cam_transform, p_cam_projection, p_cam_orthogonal, false, p_changed_bounds)) {
		buffer->update_mips();
	} else {
		buffer->clear_data();
		buffer->update_mips();
	}
}

RID RendererSceneOcclusionCull::buffer_get_debug_texture(RID p_buffer) {
	HZBuffer *buffer = buffers.getptr(p_buffer);
	ERR_FAIL_NULL_V(buffer, RID());
	return buffer->get_debug_texture();
}

void RendererSceneOcclusionCull::buffer_set_use_occluders(RID p_buffer, bool p_enable) {
	HZBuffer *buffer = buffer_get_ptr(p_buffer);
	ERR_FAIL_NULL(buffer);
	buffer->set_use_occluders(p_enable);
}

void RendererSceneOcclusionCull::buffer_set_use_depth_readback(RID p_buffer, bool p_enable) {
	HZBuffer *buffer = buffer_get_ptr(p_buffer);
	ERR_FAIL_NULL(buffer);
	buffer->set_use_depth_readback(p_enable);
}
