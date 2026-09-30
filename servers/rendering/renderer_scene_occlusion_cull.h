/**************************************************************************/
/*  renderer_scene_occlusion_cull.h                                       */
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

#include "core/io/image.h"
#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "core/templates/span.h"
#include "servers/rendering/rendering_server_enums.h"

#include <cfloat> // FLT_MIN, FLT_MAX

class RendererSceneOcclusionCull {
protected:
	static RendererSceneOcclusionCull *singleton;

public:
	// A downsampled copy of the depth buffer of a previously rendered frame, read back from the GPU.
	// It is reprojected into the current camera to fill the HZB (Hierarchical Z-Buffer), which allows
	// the depth buffer itself to be used for occlusion culling, without having to place occluders.
	struct DepthReadback {
		static constexpr uint32_t MAX_VIEWS = 2;

		// Farthest reversed-Z depth (1.0 is the near plane, 0.0 is the far plane) found in each texel.
		// Texel (x, y) of view v is stored at `depth[(v * size.y + y) * size.x + x]`, and covers normalized
		// device coordinates centered at `((x + 0.5) / size.x * 2 - 1, (y + 0.5) / size.y * 2 - 1)`.
		LocalVector<float> depth;
		Size2i size;
		uint32_t view_count = 0;
		// Camera transform (view space to world space) used when rendering the depth buffer.
		Transform3D cam_transform;
		bool cam_orthogonal = false;
		// For each view, transforms normalized device coordinates (including depth) into view space.
		Projection inv_projection[MAX_VIEWS];
		// Value of `RendererCompositor::get_frame_number()` when the depth buffer was rendered.
		uint64_t frame = 0;

		_FORCE_INLINE_ bool is_valid() const {
			return view_count > 0 && view_count <= MAX_VIEWS && size.x > 0 && size.y > 0 && depth.size() == view_count * size.x * size.y;
		}
	};

	class HZBuffer {
	protected:
		LocalVector<float> data;
		LocalVector<Size2i> sizes;
		LocalVector<float *> mips;

		// Scratch buffers used when reprojecting a depth readback.
		struct ReprojectedTexel {
			Vector2 position; // In texels of the first mip.
			float distance = 0.0f; // FLT_MAX for the background.
			bool valid = false; // False when behind the camera.
			bool occluder = false; // False when it may be next to parts of the scene that weren't visible in the readback.
		};
		LocalVector<ReprojectedTexel> reprojected_texels;
		LocalVector<float> reprojection;
		LocalVector<float> reprojection_result;

		struct ReprojectionData {
			const float *depth = nullptr;
			Size2i src_size;
			// From normalized device coordinates of the readback to the view space and the clip space of the camera.
			Projection src_ndc_to_view;
			Projection src_ndc_to_clip;
			real_t z_near = 0.0;
			Vector2 target_scale;
			bool cam_orthogonal = false;
		};

		void _reproject_texel(const ReprojectionData &p_data, int p_x, int p_y, float p_depth, ReprojectedTexel &r_texel) const;
		_FORCE_INLINE_ static void _reproject_texel_from_row(const ReprojectionData &p_data, const Vector4 &p_view_row, const Vector4 &p_clip_row, real_t p_ndc_x, float p_depth, ReprojectedTexel &r_texel);
		void _reproject_row(uint32_t p_row, const ReprojectionData *p_data);
		void _find_disocclusions_row(uint32_t p_row, const ReprojectionData *p_data);
		void _unocclude_bounds(float *p_data, const AABB &p_aabb, const Vector3 &p_cam_position, const Transform3D &p_cam_inv_transform, const Projection &p_cam_projection, bool p_cam_orthogonal) const;

		bool use_occluders = true;
		bool use_depth_readback = false;

		RID debug_texture;
		Ref<Image> debug_image;
		PackedByteArray debug_data;
		float debug_tex_range = 0.0f;

		uint64_t occlusion_frame = 0;
		Size2i occlusion_buffer_size;

		_FORCE_INLINE_ bool _is_occluded(const real_t p_bounds[6], const Vector3 &p_cam_position, const Transform3D &p_cam_inv_transform, const Projection &p_cam_projection, real_t p_near, bool p_is_orthogonal) const {
			if (is_empty()) {
				return false;
			}

			Vector3 closest_point = p_cam_position.clamp(Vector3(p_bounds[0], p_bounds[1], p_bounds[2]), Vector3(p_bounds[3], p_bounds[4], p_bounds[5]));

			if (closest_point == p_cam_position) {
				return false;
			}

			Vector3 closest_point_view = p_cam_inv_transform.xform(closest_point);
			if (closest_point_view.z > -p_near) {
				return false;
			}

			// Force distance calculation to use double precision to avoid floating-point overflow for distant objects.
			closest_point = closest_point - p_cam_position;
			float min_depth = Math::sqrt((double)closest_point.x * (double)closest_point.x + (double)closest_point.y * (double)closest_point.y + (double)closest_point.z * (double)closest_point.z);

			Vector2 rect_min = Vector2(FLT_MAX, FLT_MAX);
			Vector2 rect_max = Vector2(FLT_MIN, FLT_MIN);

			for (int j = 0; j < 8; j++) {
				// Bitmask to cycle through the corners of the AABB.
				Vector3 corner = Vector3(
						j & 4 ? p_bounds[0] : p_bounds[3],
						j & 2 ? p_bounds[1] : p_bounds[4],
						j & 1 ? p_bounds[2] : p_bounds[5]);
				Vector3 view = p_cam_inv_transform.xform(corner);

				// When using an orthogonal camera, the closest point of an AABB to the camera is guaranteed to be a corner.
				if (p_is_orthogonal) {
					min_depth = MIN(min_depth, -view.z);
				}

				Vector3 projected = p_cam_projection.xform(view);

				if (-view.z < 0.0) {
					rect_min = Vector2(0.0f, 0.0f);
					rect_max = Vector2(1.0f, 1.0f);
					break;
				}

				Vector2 normalized = Vector2(projected.x * 0.5f + 0.5f, projected.y * 0.5f + 0.5f);
				rect_min = rect_min.min(normalized);
				rect_max = rect_max.max(normalized);
			}

			rect_max = rect_max.minf(1);
			rect_min = rect_min.maxf(0);

			int mip_count = mips.size();

			Vector2 screen_diagonal = (rect_max - rect_min) * sizes[0];
			float size = MAX(screen_diagonal.x, screen_diagonal.y);
			float l = Math::ceil(Math::log2(size));
			int lod = CLAMP(l, 0, mip_count - 1);

			const int max_samples = 512;
			int sample_count = 0;
			bool visible = true;

			for (; lod >= 0; lod--) {
				int w = sizes[lod].x;
				int h = sizes[lod].y;

				int minx = CLAMP(rect_min.x * w - 1, 0, w - 1);
				int maxx = CLAMP(rect_max.x * w + 1, 0, w - 1);

				int miny = CLAMP(rect_min.y * h - 1, 0, h - 1);
				int maxy = CLAMP(rect_max.y * h + 1, 0, h - 1);

				sample_count += (maxx - minx + 1) * (maxy - miny + 1);

				if (sample_count > max_samples) {
					return false;
				}

				visible = false;
				for (int y = miny; y <= maxy; y++) {
					for (int x = minx; x <= maxx; x++) {
						float depth = mips[lod][y * w + x];
						if (depth > min_depth) {
							visible = true;
							break;
						}
					}
					if (visible) {
						break;
					}
				}

				if (!visible) {
					return true;
				}
			}

			return !visible;
		}

	public:
		static bool occlusion_jitter_enabled;

		_FORCE_INLINE_ bool is_empty() const {
			return sizes.is_empty();
		}

		virtual void clear();
		virtual void resize(const Size2i &p_size);

		// Marks every texel of the first mip as unoccluded.
		void clear_data();
		void update_mips();

		// Reprojects a depth buffer rendered from another point of view (usually a few frames ago) into
		// the given camera, and writes the result into the first mip. If `p_merge` is `true`, the result is
		// merged with the current contents (e.g. occluders). Returns `false` if nothing was written.
		// `p_changed_bounds` are the world space bounds of geometry that moved or disappeared since the
		// depth buffer was rendered: the depth buffer doesn't occlude anything there.
		// `update_mips()` must be called afterwards.
		bool reproject_depth(const DepthReadback &p_readback, const Transform3D &p_cam_transform, const Projection &p_cam_projection, bool p_cam_orthogonal, bool p_merge, Span<AABB> p_changed_bounds = Span<AABB>());

		// Whether occluders (OccluderInstance3D) are rendered into this buffer.
		_FORCE_INLINE_ bool is_using_occluders() const { return use_occluders; }
		_FORCE_INLINE_ void set_use_occluders(bool p_enable) { use_occluders = p_enable; }

		// Whether the depth buffer rendered by the GPU is reprojected into this buffer (HZB occlusion culling).
		_FORCE_INLINE_ bool is_using_depth_readback() const { return use_depth_readback; }
		_FORCE_INLINE_ void set_use_depth_readback(bool p_enable) { use_depth_readback = p_enable; }

		// Thin wrapper around _is_occluded(),
		// allowing occlusion timers to delay the disappearance
		// of objects to prevent flickering when using jittering.
		_FORCE_INLINE_ bool is_occluded(const real_t p_bounds[6], const Vector3 &p_cam_position, const Transform3D &p_cam_inv_transform, const Projection &p_cam_projection, real_t p_near, bool p_is_orthogonal, uint64_t &r_occlusion_timeout) const {
			bool occluded = _is_occluded(p_bounds, p_cam_position, p_cam_inv_transform, p_cam_projection, p_near, p_is_orthogonal);

			// Special case, temporal jitter disabled,
			// so we don't use occlusion timers.
			if (!occlusion_jitter_enabled) {
				return occluded;
			}

			if (!occluded) {
//#define DEBUG_RASTER_OCCLUSION_JITTER
#ifdef DEBUG_RASTER_OCCLUSION_JITTER
				r_occlusion_timeout = occlusion_frame + 1;
#else
				r_occlusion_timeout = occlusion_frame + 9;
#endif
			} else if (r_occlusion_timeout) {
				// Regular timeout, allow occlusion culling
				// to proceed as normal after the delay.
				if (occlusion_frame >= r_occlusion_timeout) {
					r_occlusion_timeout = 0;
				}
			}

			return occluded && !r_occlusion_timeout;
		}

		RID get_debug_texture();
		const Size2i &get_occlusion_buffer_size() const { return occlusion_buffer_size; }

		// Every mip, one after another from the first one (see `get_mip_size()`), for GPU-driven rendering to test
		// occlusion the same way: the distance to the camera of the farthest surface seen in each texel.
		_FORCE_INLINE_ const LocalVector<float> &get_data() const { return data; }
		_FORCE_INLINE_ int get_mip_count() const { return sizes.size(); }
		_FORCE_INLINE_ const Size2i &get_mip_size(int p_mip) const { return sizes[p_mip]; }

		virtual ~HZBuffer() {}
	};

	static RendererSceneOcclusionCull *get_singleton() { return singleton; }

	void _print_warning() {
		WARN_PRINT_ONCE("Occlusion culling using occluders is disabled at build-time (HZB occlusion culling is still available).");
	}

	virtual bool is_occluder(RID p_rid) { return false; }
	virtual RID occluder_allocate() { return RID(); }
	virtual void occluder_initialize(RID p_occluder) {}
	virtual void free_occluder(RID p_occluder) { _print_warning(); }
	virtual void occluder_set_mesh(RID p_occluder, const PackedVector3Array &p_vertices, const PackedInt32Array &p_indices) { _print_warning(); }

	virtual void add_scenario(RID p_scenario) {}
	virtual void remove_scenario(RID p_scenario) {}
	virtual void scenario_set_instance(RID p_scenario, RID p_instance, RID p_occluder, const Transform3D &p_xform, bool p_enabled) { _print_warning(); }
	virtual void scenario_remove_instance(RID p_scenario, RID p_instance) { _print_warning(); }

	// The default implementation of the buffers can't render occluders (this requires the raycast module),
	// but it can still use the depth buffer rendered by the GPU (HZB occlusion culling).
	virtual void add_buffer(RID p_buffer);
	virtual void remove_buffer(RID p_buffer);
	virtual HZBuffer *buffer_get_ptr(RID p_buffer);
	virtual void buffer_set_scenario(RID p_buffer, RID p_scenario) {}
	virtual void buffer_set_size(RID p_buffer, const Vector2i &p_size);
	// `p_depth_readback` is only used if the buffer uses depth readbacks, and can be `nullptr`
	// if no depth buffer has been read back yet. See `HZBuffer::reproject_depth()` for `p_changed_bounds`.
	virtual void buffer_update(RID p_buffer, const Transform3D &p_cam_transform, const Projection &p_cam_projection, bool p_cam_orthogonal, const DepthReadback *p_depth_readback = nullptr, Span<AABB> p_changed_bounds = Span<AABB>());

	virtual RID buffer_get_debug_texture(RID p_buffer);

	void buffer_set_use_occluders(RID p_buffer, bool p_enable);
	void buffer_set_use_depth_readback(RID p_buffer, bool p_enable);

	virtual void set_build_quality(RSE::ViewportOcclusionCullingBuildQuality p_quality) {}

	RendererSceneOcclusionCull() {
		singleton = this;
	}

	virtual ~RendererSceneOcclusionCull() {
		singleton = nullptr;
	}

private:
	HashMap<RID, HZBuffer> buffers;
};
