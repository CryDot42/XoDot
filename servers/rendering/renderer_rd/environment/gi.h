/**************************************************************************/
/*  gi.h                                                                  */
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

#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid_owner.h"
#include "servers/rendering/environment/renderer_gi.h"
#include "servers/rendering/renderer_compositor.h"
#include "servers/rendering/renderer_rd/environment/sky.h"
#include "servers/rendering/renderer_rd/pipeline_deferred_rd.h"
#include "servers/rendering/renderer_rd/shaders/environment/gi.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/sdfgi_debug.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/sdfgi_debug_probes.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/sdfgi_direct_light.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/sdfgi_integrate.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/sdfgi_preprocess.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/voxel_gi.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/voxel_gi_debug.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/voxel_gi_mipmap.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/voxel_gi_screen_probes.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_buffer_custom_data_rd.h"
#include "servers/rendering/renderer_scene_render.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/storage/utilities.h"

#define RB_SCOPE_GI SNAME("rbgi")
#define RB_SCOPE_SDFGI SNAME("sdfgi")

#define RB_TEX_AMBIENT SNAME("ambient")
#define RB_TEX_REFLECTION SNAME("reflection")

// Ping-ponged copies of the two buffers above plus the linear depth they were traced
// at, so a frame can reproject the previous frame's result. Two of each: the pass
// samples one set at reprojected coordinates while writing the other at the current
// pixel, which cannot be the same texture.
#define RB_TEX_GI_HISTORY_AMBIENT_0 SNAME("gi_history_ambient_0")
#define RB_TEX_GI_HISTORY_AMBIENT_1 SNAME("gi_history_ambient_1")
#define RB_TEX_GI_HISTORY_REFLECTION_0 SNAME("gi_history_reflection_0")
#define RB_TEX_GI_HISTORY_REFLECTION_1 SNAME("gi_history_reflection_1")
#define RB_TEX_GI_HISTORY_DEPTH_0 SNAME("gi_history_depth_0")
#define RB_TEX_GI_HISTORY_DEPTH_1 SNAME("gi_history_depth_1")

// Screen probes (rendering/global_illumination/sdfgi/screen_probes): where each probe sits,
// the normal of the surface it sits on, and the irradiance its rays found, as traced and as
// filtered with its neighbors. See MODE_SCREEN_PROBE_TRACE in gi.glsl.
#define RB_TEX_SCREEN_PROBE_POSITION SNAME("screen_probe_position")
#define RB_TEX_SCREEN_PROBE_NORMAL SNAME("screen_probe_normal")
#define RB_TEX_SCREEN_PROBE_SH SNAME("screen_probe_sh")
#define RB_TEX_SCREEN_PROBE_SH_FILTERED SNAME("screen_probe_sh_filtered")
// The adaptive probe of each tile of half (1) and a quarter (2) of the uniform probes' tile size, as
// an index, or -1 for none.
#define RB_TEX_SCREEN_PROBE_ADAPTIVE_INDEX_1 SNAME("screen_probe_adaptive_index_1")
#define RB_TEX_SCREEN_PROBE_ADAPTIVE_INDEX_2 SNAME("screen_probe_adaptive_index_2")

// SNAME caches per call site, so the pair cannot live in an array; these pick between the
// two names for a ping-pong index instead.
static _FORCE_INLINE_ StringName gi_history_ambient_name(uint32_t p_index) {
	return p_index ? RB_TEX_GI_HISTORY_AMBIENT_1 : RB_TEX_GI_HISTORY_AMBIENT_0;
}

static _FORCE_INLINE_ StringName gi_history_reflection_name(uint32_t p_index) {
	return p_index ? RB_TEX_GI_HISTORY_REFLECTION_1 : RB_TEX_GI_HISTORY_REFLECTION_0;
}

static _FORCE_INLINE_ StringName gi_history_depth_name(uint32_t p_index) {
	return p_index ? RB_TEX_GI_HISTORY_DEPTH_1 : RB_TEX_GI_HISTORY_DEPTH_0;
}

// Forward declare RenderDataRD and RendererSceneRenderRD so we can pass it into some of our methods, these classes are pretty tightly bound
class RenderDataRD;
class RendererSceneRenderRD;

namespace RendererRD {

class GI : public RendererGI {
private:
	struct VoxelGIPushConstant;

public:
	/* VOXEL GI STORAGE */

	struct VoxelGI {
		RID octree_buffer;
		RID data_buffer;
		RID sdf_texture;

		uint32_t octree_buffer_size = 0;
		uint32_t data_buffer_size = 0;

		Vector<int> level_counts;
		// Index of the first leaf cell for each X coordinate, plus one past the last one. Leaf cells are
		// sorted by X, so this gives the cells of any X range as one contiguous range.
		LocalVector<uint32_t> leaf_x_offsets;

		int cell_count = 0;

		Transform3D to_cell_xform;
		AABB bounds;
		Vector3i octree_size;

		float dynamic_range = 2.0;
		float energy = 1.0;
		float baked_exposure = 1.0;
		float bias = 1.4;
		float normal_bias = 0.0;
		float propagation = 0.5;
		bool interior = false;
		bool use_two_bounces = true;

		uint32_t version = 1;
		uint32_t data_version = 1;

		Dependency dependency;
	};

	/* VOXEL_GI INSTANCE */

	//@TODO VoxelGIInstance is still directly used in the render code, we'll address this when we refactor the render code itself.

	struct VoxelGIInstance {
		// access to our containers
		GI *gi = nullptr;

		RID probe;
		RID texture;
		RID write_buffer;

		struct Mipmap {
			RID texture;
			RID uniform_set;
			RID second_bounce_uniform_set;
			RID write_uniform_set;
			RID clear_uniform_set; // Region clear of this mipmap.
			RID downsample_uniform_set; // Region downsample from the previous mipmap into this one (not on mipmap 0).
			uint32_t level;
			uint32_t cell_offset;
			uint32_t cell_count;
		};
		Vector<Mipmap> mipmaps;
		int octree_mipmap_count = 0; // Mipmaps matching octree levels, the others are downsampled.

		// Dynamic objects are rendered from the 6 axes into an oversampled map, which is then shrunk
		// until it matches the voxel size and plotted into mipmap 0. Maps are sized for the largest object seen.
		struct DynamicMap {
			RID texture; //color normally, or emission on first pass
			RID fb_depth; //actual depth buffer for the first pass, float depth for later passes
			RID depth; //actual depth buffer for the first pass, float depth for later passes
			RID normal; //normal buffer for the first pass
			RID albedo; //emission buffer for the first pass
			RID orm; //orm buffer for the first pass
			RID fb; //used for rendering, only valid on first map
			RID uniform_set; // Lighting on the first map, shrink from the previous map on the others.
			RID plot_uniform_set; // Shrinks this map and plots the result into mipmap 0.
			uint32_t size;
		};

		Vector<DynamicMap> dynamic_maps;
		uint32_t dynamic_map_size = 0;

		struct CellBox {
			Vector3i begin;
			Vector3i end; // Exclusive.

			_FORCE_INLINE_ bool is_empty() const { return begin.x >= end.x || begin.y >= end.y || begin.z >= end.z; }
			_FORCE_INLINE_ bool intersects(const CellBox &p_box) const {
				return begin.x < p_box.end.x && p_box.begin.x < end.x && begin.y < p_box.end.y && p_box.begin.y < end.y && begin.z < p_box.end.z && p_box.begin.z < end.z;
			}
		};

		// Dynamic objects currently plotted into the texture, so only the parts that change get updated.
		struct DynamicObject {
			Transform3D transform;
			AABB aabb;
			CellBox box; // Region of mipmap 0 the object was plotted into.
			uint64_t last_seen_frame = 0;
			uint32_t refresh_phase = 0;
		};

		enum {
			MAX_DYNAMIC_OVERSAMPLE_SHIFT = 4, // Objects are rendered at up to 16x the voxel resolution.
			MIN_DYNAMIC_MAP_SIZE = 64,
			MAX_DYNAMIC_MAP_SIZE = 2048, // Larger objects are rendered with less oversampling.
		};

		HashMap<RenderGeometryInstance *, DynamicObject> dynamic_objects;

		// Anisotropic mipmaps: from mipmap 1 on, every voxel stores what is seen when traveling along each of
		// the 6 axis directions, which keeps walls from leaking light at coarse mipmaps.
		struct AnisoMipmap {
			RID texture;
			RID uniform_set; // Builds this level from the previous one (from mipmap 0 and the normal mask for the first).
		};
		RID aniso_texture;
		RID aniso_normal_mask; // Directions each static voxel of mipmap 0 faces.
		Vector<AnisoMipmap> aniso_mipmaps;
		int aniso_slab_axis = 0; // The 6 directions are stored side by side along this axis.

		// Amortized static light updates, see _update_static_light().
		enum StaticBounce {
			STATIC_BOUNCE_NONE,
			STATIC_BOUNCE_SECOND, // One extra bounce on top of direct light (VoxelGIData.use_two_bounces).
			STATIC_BOUNCE_FEEDBACK, // Bounces gathered again and again, converging over frames.
		};
		enum {
			FEEDBACK_BOUNCE_ITERATIONS = 4, // Passes over all cells after a light change.
		};
		RID direct_light_buffer; // Direct light of every cell, only used with bounce feedback.
		RID direct_light_uniform_set;
		RID feedback_uniform_set;
		uint32_t light_frames_left = 0;
		uint32_t bounce_frames_left = 0;
		float bounce_feedback = 0.0;

		uint64_t update_frame = 0;
		Transform3D last_update_transform;
		bool static_data_written = false;

		int slot = -1;
		uint32_t last_probe_version = 0;
		uint32_t last_probe_data_version = 0;

		//uint64_t last_pass = 0;
		uint32_t render_index = 0;

		bool has_dynamic_object_data = false;

		Transform3D transform;

		void update(bool p_update_light_instances, const Vector<RID> &p_light_instances, const PagedArray<RenderGeometryInstance *> &p_dynamic_objects);
		static CellBox _get_dynamic_object_box(const Transform3D &p_to_cell, const AABB &p_aabb, const Vector3i &p_octree_size);
		void _create_dynamic_maps(uint32_t p_size);
		void _free_dynamic_maps();
		void _restore_static_region(RD::ComputeListID p_compute_list, const CellBox &p_box);
		void _downsample_region(RD::ComputeListID p_compute_list, const CellBox &p_box, int p_from_level = 1);
		void _plot_dynamic_object(RenderGeometryInstance *p_instance, uint32_t p_oversample_shift, uint32_t p_light_count, RID p_area_light_atlas_uniform_set);
		void _dispatch_cells(RD::ComputeListID p_compute_list, VoxelGIPushConstant &p_push_constant, uint32_t p_cell_offset, uint32_t p_cell_count, uint32_t p_stride, uint32_t p_phase);
		void _update_static_light(uint32_t p_light_count, bool p_compute_light, StaticBounce p_bounce, float p_bounce_feedback, uint32_t p_stride, uint32_t p_phase);
		void _create_feedback();
		void _free_feedback();
		void _create_aniso();
		void _free_aniso();
		void _build_aniso_region(RD::ComputeListID p_compute_list, const CellBox &p_box);
		void debug(RD::DrawListID p_draw_list, RID p_framebuffer, const Projection &p_camera_with_transform, bool p_lighting, bool p_emission, float p_alpha);
		void free_resources();
	};

private:
	static GI *singleton;

	/* VOXEL GI STORAGE */

	mutable RID_Owner<VoxelGI, true> voxel_gi_owner;

	/* VOXEL_GI INSTANCE */

	mutable RID_Owner<VoxelGIInstance> voxel_gi_instance_owner;

	struct VoxelGILight {
		uint32_t type;
		float energy;
		float radius;
		float attenuation;

		float color[3];
		float cos_spot_angle;

		float position[3];
		float inv_spot_attenuation;

		float direction[3];
		uint32_t has_shadow;

		float area_width[4];
		float area_height[4];
		float area_projector_rect[4];
	};

	struct VoxelGIPushConstant {
		int32_t limits[3];
		uint32_t stack_size;

		float emission_scale;
		float propagation;
		float dynamic_range;
		uint32_t light_count;

		uint32_t cell_offset;
		uint32_t cell_count;
		float bounce_feedback;
		float cell_size;

		int32_t region_begin[3];
		uint32_t cell_stride;
		int32_t region_end[3];
		uint32_t cell_phase;
	};

	struct VoxelGIMipmapPushConstant {
		int32_t offset[3];
		float propagation;
		int32_t size[3];
		int32_t slab_axis;
		int32_t src_size[3];
		int32_t dst_slab_size;
		int32_t src_slab_size;
		uint32_t pad[3];
	};

	struct VoxelGIDynamicPushConstant {
		int32_t limits[3];
		uint32_t light_count;
		int32_t x_dir[3];
		float z_base;
		int32_t y_dir[3];
		float z_sign;
		int32_t z_dir[3];
		float pos_multiplier;
		uint32_t rect_pos[2];
		uint32_t rect_size[2];
		uint32_t prev_rect_ofs[2];
		uint32_t prev_rect_size[2];
		uint32_t flip_x;
		uint32_t flip_y;
		float dynamic_range;
		uint32_t on_mipmap;
		float propagation;
		float cell_size;
		float pad[2];
	};

	VoxelGILight *voxel_gi_lights = nullptr;
	uint32_t voxel_gi_max_lights = 32;
	RID voxel_gi_lights_uniform;

	enum {
		VOXEL_GI_SHADER_VERSION_COMPUTE_LIGHT,
		VOXEL_GI_SHADER_VERSION_COMPUTE_SECOND_BOUNCE,
		VOXEL_GI_SHADER_VERSION_COMPUTE_MIPMAP,
		VOXEL_GI_SHADER_VERSION_WRITE_TEXTURE,
		VOXEL_GI_SHADER_VERSION_WRITE_TEXTURE_REGION,
		VOXEL_GI_SHADER_VERSION_DYNAMIC_OBJECT_LIGHTING,
		VOXEL_GI_SHADER_VERSION_DYNAMIC_SHRINK_WRITE,
		VOXEL_GI_SHADER_VERSION_DYNAMIC_SHRINK_PLOT,
		VOXEL_GI_SHADER_VERSION_WRITE_NORMAL_MASK,
		VOXEL_GI_SHADER_VERSION_FEEDBACK_BOUNCE,
		VOXEL_GI_SHADER_VERSION_MAX
	};

	VoxelGiShaderRD voxel_gi_shader;
	RID voxel_gi_lighting_shader_version;
	RID voxel_gi_lighting_shader_version_shaders[VOXEL_GI_SHADER_VERSION_MAX];
	PipelineDeferredRD voxel_gi_lighting_shader_version_pipelines[VOXEL_GI_SHADER_VERSION_MAX];

	enum {
		VOXEL_GI_MIPMAP_CLEAR,
		VOXEL_GI_MIPMAP_DOWNSAMPLE,
		VOXEL_GI_MIPMAP_ANISO_FIRST,
		VOXEL_GI_MIPMAP_ANISO,
		VOXEL_GI_MIPMAP_MAX
	};

	VoxelGiMipmapShaderRD voxel_gi_mipmap_shader;
	RID voxel_gi_mipmap_shader_version;
	RID voxel_gi_mipmap_shader_version_shaders[VOXEL_GI_MIPMAP_MAX];
	PipelineDeferredRD voxel_gi_mipmap_pipelines[VOXEL_GI_MIPMAP_MAX];

	// Stationary dynamic objects are only plotted again after this many frames.
	uint32_t voxel_gi_dynamic_object_refresh_frames = 4;

	enum {
		VOXEL_GI_DEBUG_COLOR,
		VOXEL_GI_DEBUG_LIGHT,
		VOXEL_GI_DEBUG_EMISSION,
		VOXEL_GI_DEBUG_LIGHT_FULL,
		VOXEL_GI_DEBUG_MAX
	};

	struct VoxelGIDebugPushConstant {
		float projection[16];
		uint32_t cell_offset;
		float dynamic_range;
		float alpha;
		uint32_t level;
		int32_t bounds[3];
		uint32_t pad;
	};

	VoxelGiDebugShaderRD voxel_gi_debug_shader;
	RID voxel_gi_debug_shader_version;
	RID voxel_gi_debug_shader_version_shaders[VOXEL_GI_DEBUG_MAX];
	PipelineCacheRD voxel_gi_debug_shader_version_pipelines[VOXEL_GI_DEBUG_MAX];
	RID voxel_gi_debug_uniform_set;

	/* SDFGI */

	struct SDFGIShader {
		enum SDFGIPreprocessShaderVersion {
			PRE_PROCESS_SCROLL,
			PRE_PROCESS_SCROLL_OCCLUSION,
			PRE_PROCESS_JUMP_FLOOD_INITIALIZE,
			PRE_PROCESS_JUMP_FLOOD_INITIALIZE_HALF,
			PRE_PROCESS_JUMP_FLOOD,
			PRE_PROCESS_JUMP_FLOOD_OPTIMIZED,
			PRE_PROCESS_JUMP_FLOOD_UPSCALE,
			PRE_PROCESS_OCCLUSION,
			PRE_PROCESS_STORE,
			PRE_PROCESS_PROBE_PLACEMENT,
			PRE_PROCESS_MAX
		};

		struct PreprocessPushConstant {
			int32_t scroll[3];
			int32_t grid_size;

			int32_t probe_offset[3];
			int32_t step_size;

			int32_t half_size;
			uint32_t occlusion_index;
			int32_t cascade;
			float min_distance; // PRE_PROCESS_PROBE_PLACEMENT: clearance to keep probes at, in voxels.

			// Cells of the cascade revoxelized this frame for dynamic objects (empty when none): the
			// scroll leaves the old voxels out of them, and occlusion is recomputed around them.
			int32_t box_from[3];
			uint32_t pad;
			int32_t box_to[3];
			uint32_t pad2;
		};

		SdfgiPreprocessShaderRD preprocess;
		RID preprocess_shader;
		PipelineDeferredRD preprocess_pipeline[PRE_PROCESS_MAX];

		struct DebugPushConstant {
			float grid_size[3];
			uint32_t max_cascades;

			int32_t screen_size[2];
			float y_mult;

			float z_near;

			float inv_projection[3][4];
			float cam_basis[3][3];
			float cam_origin[3];
		};

		SdfgiDebugShaderRD debug;
		RID debug_shader;
		RID debug_shader_version;
		PipelineDeferredRD debug_pipeline;

		enum ProbeDebugMode {
			PROBE_DEBUG_PROBES,
			PROBE_DEBUG_PROBES_MULTIVIEW,
			PROBE_DEBUG_VISIBILITY,
			PROBE_DEBUG_VISIBILITY_MULTIVIEW,
			PROBE_DEBUG_MAX
		};

		struct DebugProbesSceneData {
			float projection[2][16];
		};

		struct DebugProbesPushConstant {
			uint32_t band_power;
			uint32_t sections_in_band;
			uint32_t band_mask;
			float section_arc;

			float grid_size[3];
			uint32_t cascade;

			uint32_t pad;
			float y_mult;
			int32_t probe_debug_index;
			int32_t probe_axis_size;
		};

		SdfgiDebugProbesShaderRD debug_probes;
		RID debug_probes_shader;
		RID debug_probes_shader_version;

		PipelineCacheRD debug_probes_pipeline[PROBE_DEBUG_MAX];

		struct Light {
			float color[3];
			float energy;

			float direction[3];
			uint32_t has_shadow;

			float position[3];
			float attenuation;

			uint32_t type;
			float cos_spot_angle;
			float inv_spot_attenuation;
			float radius;

			float area_width[4];
			float area_height[4];
			float area_projector_rect[4];
		};

		struct DirectLightPushConstant {
			float grid_size[3];
			uint32_t max_cascades;

			uint32_t cascade;
			uint32_t light_count;
			uint32_t process_offset;
			uint32_t process_increment;

			int32_t probe_axis_size;
			float bounce_feedback;
			float y_mult;
			uint32_t use_occlusion;

			// Only voxels in these cells are lit (the ones just voxelized, for static lights).
			int32_t process_from[3];
			uint32_t pad;
			int32_t process_to[3];
			uint32_t pad2;
		};

		enum {
			DIRECT_LIGHT_MODE_STATIC,
			DIRECT_LIGHT_MODE_DYNAMIC,
			DIRECT_LIGHT_MODE_MAX
		};
		SdfgiDirectLightShaderRD direct_light;
		RID direct_light_shader;
		PipelineDeferredRD direct_light_pipeline[DIRECT_LIGHT_MODE_MAX];

		enum {
			INTEGRATE_MODE_PROCESS,
			INTEGRATE_MODE_STORE,
			INTEGRATE_MODE_SCROLL,
			INTEGRATE_MODE_SCROLL_STORE,
			INTEGRATE_MODE_MAX
		};
		struct IntegratePushConstant {
			enum {
				SKY_FLAGS_MODE_COLOR = 0x01,
				SKY_FLAGS_MODE_SKY = 0x02,
				SKY_FLAGS_ORIENTATION_SIGN = 0x04,
			};

			enum { // INTEGRATE_FLAG_* in sdfgi_integrate.glsl.
				FLAG_RESET = 0x01,
				FLAG_ADAPTIVE = 0x02,
			};

			float grid_size[3];
			uint32_t max_cascades;

			uint32_t probe_axis_size;
			uint32_t cascade;
			uint32_t history_index;
			uint32_t history_size;

			uint32_t ray_count;
			float ray_bias;
			int32_t image_size[2];

			int32_t world_offset[3];
			uint32_t sky_flags;

			int32_t scroll[3];
			float sky_energy;

			float sky_color_or_orientation[3];
			float y_mult;

			float sky_irradiance_border_size[2];
			uint32_t store_ambient_texture;
			uint32_t flags;
		};

		SdfgiIntegrateShaderRD integrate;
		RID integrate_shader;
		PipelineDeferredRD integrate_pipeline[INTEGRATE_MODE_MAX];

		RID integrate_default_sky_uniform_set;

	} sdfgi_shader;

public:
	static GI *get_singleton() { return singleton; }

	/* GI */

	enum {
		MAX_VOXEL_GI_INSTANCES = 8
	};

	// Struct for use in render buffer
	class RenderBuffersGI : public RenderBufferCustomDataRD {
		GDCLASS(RenderBuffersGI, RenderBufferCustomDataRD)

	private:
		RID voxel_gi_buffer;

	public:
		RID voxel_gi_textures[MAX_VOXEL_GI_INSTANCES];
		RID voxel_gi_aniso_textures[MAX_VOXEL_GI_INSTANCES];

		RID full_buffer;
		RID full_dispatch;
		RID full_mask;

		/* GI buffers */
		bool using_half_size_gi = false;
		bool using_temporal_gi = false;
		// Whether the SDFGI screen probe buffers were allocated at full size, and which frame of
		// their placement jitter comes next.
		bool using_sdfgi_screen_probes = false;
		Size2i sdfgi_screen_probe_grid;
		uint32_t sdfgi_screen_probe_frame = 0;
		// The previous frame's image the uniform sets bind for the SDFGI screen probes' screen
		// traces, null while there is none.
		RID sdfgi_screen_probe_last_frame[RendererSceneRender::MAX_RENDER_VIEWS];
		// How many adaptive SDFGI screen probes there are, written on the GPU as the arguments of
		// the indirect dispatches that trace and filter them, and the copy those read it from.
		RID sdfgi_screen_probe_count_buffer;
		RID sdfgi_screen_probe_dispatch_buffer;
		RID get_sdfgi_screen_probe_count_buffer();

		// Alternates every frame: index 0 of the pair is sampled and index 1 written, or the
		// other way round. The uniform sets bind those textures, so there is one set per
		// parity per view rather than one per view.
		uint32_t history_frame = 0;
		// False until a frame has actually written history. Reset on every (re)allocation, so
		// a fresh texture is never read back: relying on a clear alone would make correctness
		// depend on the clear succeeding, and a failed one is silent at render time.
		bool history_valid = false;
		Transform3D prev_cam_transform;
		Projection prev_projection;

		// How many more frames this view's GI takes to settle after the last change, and requests
		// of its own when the engine only draws on changes (see GI::_request_settle_frames()).
		uint32_t settle_frames_left = 0;

		/* VoxelGI screen probes */
		uint32_t screen_probe_frame = 0;
		uint32_t screen_probe_history_index = 0;
		bool screen_probe_history_valid = false;
		RID screen_probe_last_frame; // Last frame buffer seen, to know whether it holds a previous frame.
		Projection screen_probe_prev_projection;
		Transform3D screen_probe_prev_transform;
		RID screen_probe_trace_ubo;
		RID screen_probe_gi_ubo;

		RID uniform_set[2][RendererSceneRender::MAX_RENDER_VIEWS];
		RID scene_data_ubo;

		RID get_voxel_gi_buffer();

		virtual void configure(RenderSceneBuffersRD *p_render_buffers) override {}
		virtual void free_data() override;
	};

	/* VOXEL GI API */

	bool owns_voxel_gi(RID p_rid) { return voxel_gi_owner.owns(p_rid); }

	virtual RID voxel_gi_allocate() override;
	virtual void voxel_gi_free(RID p_voxel_gi) override;
	virtual void voxel_gi_initialize(RID p_voxel_gi) override;

	virtual void voxel_gi_allocate_data(RID p_voxel_gi, const Transform3D &p_to_cell_xform, const AABB &p_aabb, const Vector3i &p_octree_size, const Vector<uint8_t> &p_octree_cells, const Vector<uint8_t> &p_data_cells, const Vector<uint8_t> &p_distance_field, const Vector<int> &p_level_counts) override;

	virtual AABB voxel_gi_get_bounds(RID p_voxel_gi) const override;
	virtual Vector3i voxel_gi_get_octree_size(RID p_voxel_gi) const override;
	virtual Vector<uint8_t> voxel_gi_get_octree_cells(RID p_voxel_gi) const override;
	virtual Vector<uint8_t> voxel_gi_get_data_cells(RID p_voxel_gi) const override;
	virtual Vector<uint8_t> voxel_gi_get_distance_field(RID p_voxel_gi) const override;

	virtual Vector<int> voxel_gi_get_level_counts(RID p_voxel_gi) const override;
	virtual Transform3D voxel_gi_get_to_cell_xform(RID p_voxel_gi) const override;

	virtual void voxel_gi_set_dynamic_range(RID p_voxel_gi, float p_range) override;
	virtual float voxel_gi_get_dynamic_range(RID p_voxel_gi) const override;

	virtual void voxel_gi_set_propagation(RID p_voxel_gi, float p_range) override;
	virtual float voxel_gi_get_propagation(RID p_voxel_gi) const override;

	virtual void voxel_gi_set_energy(RID p_voxel_gi, float p_energy) override;
	virtual float voxel_gi_get_energy(RID p_voxel_gi) const override;

	virtual void voxel_gi_set_baked_exposure_normalization(RID p_voxel_gi, float p_baked_exposure) override;
	virtual float voxel_gi_get_baked_exposure_normalization(RID p_voxel_gi) const override;

	virtual void voxel_gi_set_bias(RID p_voxel_gi, float p_bias) override;
	virtual float voxel_gi_get_bias(RID p_voxel_gi) const override;

	virtual void voxel_gi_set_normal_bias(RID p_voxel_gi, float p_range) override;
	virtual float voxel_gi_get_normal_bias(RID p_voxel_gi) const override;

	virtual void voxel_gi_set_interior(RID p_voxel_gi, bool p_enable) override;
	virtual bool voxel_gi_is_interior(RID p_voxel_gi) const override;

	virtual void voxel_gi_set_use_two_bounces(RID p_voxel_gi, bool p_enable) override;
	virtual bool voxel_gi_is_using_two_bounces(RID p_voxel_gi) const override;

	virtual uint32_t voxel_gi_get_version(RID p_probe) const override;
	uint32_t voxel_gi_get_data_version(RID p_probe);

	const LocalVector<uint32_t> &voxel_gi_get_leaf_x_offsets(RID p_voxel_gi) const;
	RID voxel_gi_get_octree_buffer(RID p_voxel_gi) const;
	RID voxel_gi_get_data_buffer(RID p_voxel_gi) const;

	RID voxel_gi_get_sdf_texture(RID p_voxel_gi);

	Dependency *voxel_gi_get_dependency(RID p_voxel_gi) const;

	/* VOXEL_GI INSTANCE */

	_FORCE_INLINE_ RID voxel_gi_instance_get_texture(RID p_probe) {
		VoxelGIInstance *voxel_gi = voxel_gi_instance_owner.get_or_null(p_probe);
		ERR_FAIL_NULL_V(voxel_gi, RID());
		return voxel_gi->texture;
	}

	_FORCE_INLINE_ void voxel_gi_instance_set_render_index(RID p_probe, uint32_t p_index) {
		VoxelGIInstance *voxel_gi = voxel_gi_instance_owner.get_or_null(p_probe);
		ERR_FAIL_NULL(voxel_gi);

		voxel_gi->render_index = p_index;
	}

	bool voxel_gi_instance_owns(RID p_rid) const {
		return voxel_gi_instance_owner.owns(p_rid);
	}

	void voxel_gi_instance_free(RID p_rid);

	RSE::VoxelGIQuality voxel_gi_quality = RSE::VOXEL_GI_QUALITY_LOW;

	/* SDFGI */

	class SDFGI : public RenderBufferCustomDataRD {
		GDCLASS(SDFGI, RenderBufferCustomDataRD)

	public:
		enum {
			MAX_CASCADES = 8,
			DYNAMIC_OBJECT_CASCADE_DELAY = 4, // Frames; see update().
			CASCADE_SIZE = 128,
			PROBE_DIVISOR = 16,
			ANISOTROPY_SIZE = 6,
			MAX_DYNAMIC_LIGHTS = 128,
			MAX_STATIC_LIGHTS = 1024,
			LIGHTPROBE_OCT_SIZE = 6,
			SH_SIZE = 16
		};

		struct Cascade {
			struct UBO {
				float offset[3];
				float to_cell;
				int32_t probe_offset[3];
				uint32_t pad;
				float pad2[4];
			};

			//cascade blocks are full-size for volume (128^3), half size for albedo/emission
			RID sdf_tex;
			RID light_tex;
			RID light_aniso_0_tex;
			RID light_aniso_1_tex;

			RID light_data;
			RID light_aniso_0_data;
			RID light_aniso_1_data;

			struct SolidCell { // this struct is unused, but remains as reference for size
				uint32_t position;
				uint32_t albedo;
				uint32_t static_light;
				uint32_t static_light_aniso;
			};

			// Buffers for indirect compute dispatch.
			RID solid_cell_dispatch_buffer_storage;
			RID solid_cell_dispatch_buffer_call;
			RID solid_cell_buffer;

			RID lightprobe_history_tex;
			RID lightprobe_average_tex;

			float cell_size;
			Vector3i position;

			static const Vector3i DIRTY_ALL;
			Vector3i dirty_regions; //(0,0,0 is not dirty, negative is refresh from the end, DIRTY_ALL is refresh all.

			RID sdf_store_uniform_set;
			RID sdf_direct_light_static_uniform_set;
			RID sdf_direct_light_dynamic_uniform_set;
			RID scroll_uniform_set;
			RID scroll_occlusion_uniform_set;
			RID integrate_uniform_set;
			RID lights_buffer;

			float baked_exposure_normalization = 1.0;

			bool all_dynamic_lights_dirty = true;

			// Where dynamic objects moved, appeared or went away since the cascade last voxelized
			// that part of the world (in world space), still waiting to be voxelized again, and
			// since when (the cascades waiting longest go first; see update()).
			AABB dirty_box;
			bool has_dirty_box = false;
			uint64_t dirty_box_since = 0;

			// The cells being voxelized again this frame for it, as one more pending region.
			bool updating_box = false;
			Vector3i box_from;
			Vector3i box_to;
		};

		// access to our containers
		GI *gi = nullptr;

		// used for rendering (voxelization)
		RID render_albedo;
		RID render_emission;
		RID render_emission_aniso;
		RID render_occlusion[8];
		RID render_geom_facing;

		RID render_sdf[2];
		RID render_sdf_half[2];

		// used for ping pong processing in cascades
		RID sdf_initialize_uniform_set;
		RID sdf_initialize_half_uniform_set;
		RID jump_flood_uniform_set[2];
		RID jump_flood_half_uniform_set[2];
		RID sdf_upscale_uniform_set;
		int upscale_jfa_uniform_set_index;
		RID occlusion_uniform_set;

		uint32_t cascade_size = 128;

		LocalVector<Cascade> cascades;

		RID lightprobe_texture;
		RID lightprobe_data;
		RID occlusion_texture;
		RID occlusion_data;
		RID ambient_texture; //integrates with volumetric fog

		// Where each probe ended up after relocation, one layer per cascade (see
		// MODE_PROBE_PLACEMENT in sdfgi_preprocess.glsl): xyz its offset from the grid in voxels,
		// w whether it is usable at all.
		RID probe_state_texture;
		RID probe_placement_uniform_set;

		RID lightprobe_history_scroll; //used for scrolling lightprobes
		RID lightprobe_average_scroll; //used for scrolling lightprobes

		uint32_t history_size = 0;
		float solid_cell_ratio = 0;
		uint32_t solid_cell_count = 0;

		int num_cascades = 6;
		float min_cell_size = 0;
		uint32_t probe_axis_count = 0; //amount of probes per axis, this is an odd number because it encloses endpoints

		RID debug_uniform_set[RendererSceneRender::MAX_RENDER_VIEWS];
		RID debug_probes_scene_data_ubo;
		RID debug_probes_uniform_set;
		RID cascades_ubo;

		bool uses_occlusion = false;
		float bounce_feedback = 0.5;
		bool reads_sky = true;
		float energy = 1.0;
		float normal_bias = 1.1;
		float probe_bias = 1.1;
		RSE::EnvironmentSDFGIYScale y_scale_mode = RSE::ENV_SDFGI_Y_SCALE_75_PERCENT;

		float y_mult = 1.0;

		uint32_t version = 0;
		uint32_t render_pass = 0;

		int32_t cascade_dynamic_light_count[SDFGI::MAX_CASCADES]; //used dynamically
		RID integrate_sky_uniform_set;

		virtual void configure(RenderSceneBuffersRD *p_render_buffers) override {}
		virtual void free_data() override;
		~SDFGI();

		void create(RID p_env, const Vector3 &p_world_position, uint32_t p_requested_history_size, GI *p_gi);
		void update(RID p_env, const Vector3 &p_world_position);
		void update_light();
		void update_probes(RID p_env, RendererRD::SkyRD::Sky *p_sky);
		void store_probes();
		// Marks the world-space boxes as needing to be voxelized again (dynamic objects moved there).
		void mark_dirty(const LocalVector<AABB> &p_aabbs);
		void _get_box_cells(const Cascade &p_cascade, const AABB &p_box, Vector3i &r_from, Vector3i &r_to) const;
		int get_pending_region_count() const;
		int get_pending_region_data(int p_region, Vector3i &r_local_offset, Vector3i &r_local_size, AABB &r_bounds) const;
		void update_cascades();
		// Call after this frame's render_region() calls: re-seeds the probes of cascades those rebuilt from scratch.
		void reinit_rebuilt_probes();
		void _scroll_probes(RD::ComputeListID p_compute_list, uint32_t p_cascade, const Vector3i &p_probe_scroll, uint32_t p_flags);

		void debug_draw(uint32_t p_view_count, const Projection *p_projections, const Transform3D &p_transform, int p_width, int p_height, RID p_render_target, RID p_texture, const Vector<RID> &p_texture_views);
		void debug_probes(RID p_framebuffer, const uint32_t p_view_count, const Projection *p_camera_with_transforms);

		void pre_process_gi(const Transform3D &p_transform, RenderDataRD *p_render_data);
		void render_region(Ref<RenderSceneBuffersRD> p_render_buffers, int p_region, const PagedArray<RenderGeometryInstance *> &p_instances, float p_exposure_normalization);
		void render_static_lights(RenderDataRD *p_render_data, Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_cascade_count, const uint32_t *p_cascade_indices, const PagedArray<RID> *p_positional_light_cull_result);
	};

	RSE::EnvironmentSDFGIRayCount sdfgi_ray_count = RSE::ENV_SDFGI_RAY_COUNT_16;
	RSE::EnvironmentSDFGIFramesToConverge sdfgi_frames_to_converge = RSE::ENV_SDFGI_CONVERGE_IN_30_FRAMES;
	RSE::EnvironmentSDFGIFramesToUpdateLight sdfgi_frames_to_update_light = RSE::ENV_SDFGI_UPDATE_LIGHT_IN_4_FRAMES;
	bool sdfgi_adaptive_history = true;
	bool sdfgi_probe_relocation = true;
	float sdfgi_view_bias = 1.0;
	bool sdfgi_per_pixel_visibility = false;
	uint32_t sdfgi_dynamic_object_updates_per_frame = 1;
	bool sdfgi_screen_probes = false;
	uint32_t sdfgi_screen_probe_history_frames = 24;

	float sdfgi_solid_cell_ratio = 0.25;
	Vector3 sdfgi_debug_probe_pos;
	Vector3 sdfgi_debug_probe_dir;
	bool sdfgi_debug_probe_enabled = false;
	Vector3i sdfgi_debug_probe_index;
	uint32_t sdfgi_current_version = 0;

	/* SDFGI UPDATE */

	int sdfgi_get_lightprobe_octahedron_size() const { return SDFGI::LIGHTPROBE_OCT_SIZE; }

	virtual void sdfgi_reset() override;

	struct SDFGIData {
		enum { // SDFGI_FLAG_* in gi.glsl.
			FLAG_PER_PIXEL_VISIBILITY = 0x01,
		};

		float grid_size[3];
		uint32_t max_cascades;

		uint32_t use_occlusion;
		int32_t probe_axis_size;
		float probe_to_uvw;
		float normal_bias;

		float lightprobe_tex_pixel_size[3];
		float energy;

		float lightprobe_uv_offset[3];
		float y_mult;

		float occlusion_clamp[3];
		float view_bias;

		float occlusion_renormalize[3];
		uint32_t flags;

		float cascade_probe_size[3];
		uint32_t pad5;

		struct ProbeCascadeData {
			float position[3]; //offset of (0,0,0) in world coordinates
			float to_probe; // 1/bounds * grid_size
			int32_t probe_world_offset[3];
			float to_cell; // 1/bounds * grid_size
			float pad[3];
			float exposure_normalization;
		};

		ProbeCascadeData cascades[SDFGI::MAX_CASCADES];
	};

	struct VoxelGIData {
		float xform[16]; // 64 - 64

		float bounds[3]; // 12 - 76
		float dynamic_range; // 4 - 80

		float bias; // 4 - 84
		float normal_bias; // 4 - 88
		uint32_t blend_ambient; // 4 - 92
		uint32_t mipmaps; // 4 - 96

		float aniso_enabled; // 4 - 100
		float aniso_slab_axis; // 4 - 104
		float aniso_slab_size; // 4 - 108
		float exposure_normalization; // 4 - 112
	};

	struct SceneData {
		float inv_projection[2][16];
		float cam_transform[16];
		float eye_offset[2][4];

		// Maps a point from this frame's view space into the previous frame's clip space, to
		// find where it was on screen, and into the previous frame's view space, to compare
		// its depth against what was stored there. Only filled for single-view rendering,
		// which is the only case temporal accumulation runs in.
		float reprojection[16];
		float prev_view_from_view[16];

		int32_t screen_size[2];
		float pad1;
		float pad2;
	};

	struct PushConstant {
		uint32_t max_voxel_gi_instances;
		uint32_t high_quality_vct;
		uint32_t orthogonal;
		uint32_t view_index;

		float proj_info[4];

		float z_near;
		float z_far;
		uint32_t temporal_enabled;
		uint32_t trace_slot; // Which of the TEMPORAL_SLOT_COUNT checkerboard slots traces.

		float temporal_blend; // Weight of a fresh trace against valid history.
		uint32_t history_valid; // Whether the textures hold a previous frame worth reading.
		uint32_t screen_probe_frame; // Seeds the directions screen probes trace this frame.
		float screen_probe_blend; // The least weight this frame's screen probe light takes against valid history.

		int32_t screen_probe_grid[2]; // How many screen probes across and down, 0 when they are off.
		int32_t screen_probe_offset[2]; // Where in its tile each probe goes this frame.

		uint32_t screen_probe_flags; // SCREEN_PROBE_FLAG_*
		uint32_t screen_probe_pass; // Which probes a screen probe pass works on (see gi.glsl).
		uint32_t pad3;
		uint32_t pad4;
	};

	RID sdfgi_ubo;

	enum Group {
		GROUP_NORMAL,
		GROUP_VRS,
	};

	enum Mode {
		MODE_VOXEL_GI,
		MODE_VOXEL_GI_WITHOUT_SAMPLER,
		MODE_SDFGI,
		MODE_COMBINED,
		MODE_COMBINED_WITHOUT_SAMPLER,
		MODE_VOXEL_GI_SCREEN_PROBES,
		MODE_VOXEL_GI_SCREEN_PROBES_WITHOUT_SAMPLER,
		MODE_SCREEN_PROBE_PLACE,
		MODE_SCREEN_PROBE_ADAPT,
		MODE_SCREEN_PROBE_TRACE,
		MODE_SCREEN_PROBE_FILTER,
		MODE_MAX
	};

	// Must match Params in voxel_gi_screen_probes.glsl.
	struct ScreenProbeTraceParams {
		float inv_projection[16];
		float projection[16];
		float reprojection[16];
		float cam_basis[16];

		int32_t screen_size[2];
		int32_t probe_grid_size[2];

		uint32_t probe_spacing;
		uint32_t frame;
		uint32_t max_voxel_gi_instances;
		uint32_t screen_trace_steps;

		float screen_trace_distance;
		float z_near;
		float pixel_size;
		float last_frame_max_lod;

		uint32_t orthogonal;
		uint32_t has_last_frame;
		uint32_t pad0;
		uint32_t pad1;
	};

	// Must match ScreenProbeParams in gi.glsl.
	struct ScreenProbeGIParams {
		float reprojection[16];
		float prev_view_z[4];

		int32_t probe_grid_size[2];
		uint32_t probe_spacing;
		uint32_t frame;

		int32_t buffer_size[2];
		float history_blend;
		uint32_t history_valid;
	};

	enum {
		SCREEN_PROBES_TRACE,
		SCREEN_PROBES_FILTER,
		SCREEN_PROBES_MAX
	};

	VoxelGiScreenProbesShaderRD screen_probes_shader;
	RID screen_probes_shader_version;
	PipelineDeferredRD screen_probes_pipelines[SCREEN_PROBES_MAX];

	enum ShaderSpecializations {
		SHADER_SPECIALIZATION_HALF_RES = 1 << 0,
		SHADER_SPECIALIZATION_USE_FULL_PROJECTION_MATRIX = 1 << 1,
		SHADER_SPECIALIZATION_USE_VRS = 1 << 2,
		SHADER_SPECIALIZATION_VARIATIONS = 8,
	};

	RID default_voxel_gi_buffer;

	bool half_resolution = false;

	// Temporal accumulation: each frame only traces the pixels of one checkerboard phase and
	// reprojects the previous frame's result for the rest, so the cone tracing cost is spread
	// over TEMPORAL_SLOT_COUNT frames. Pixels whose history is missing or rejected are always
	// traced, so the result is correct on disocclusion, just more expensive there.
	//
	// Two phases, not more: reuse chains, and each link resamples the value at a fractional
	// offset, so the longer a value can go without being retraced the further a sharp feature
	// creeps along the direction of travel. See the note beside the slot test in gi.glsl.
	enum { TEMPORAL_SLOT_COUNT = 2 };
	bool temporal_accumulation = true;
	float temporal_blend = 1.0;

	// Redraws requested by _request_settle_frames(): how many, and in which frame, so that the
	// next frame can tell whether it was drawn only for them.
	uint64_t settle_request_frame = UINT64_MAX;
	int settle_requests = 0;
	uint64_t settle_checked_frame = UINT64_MAX;
	bool settle_external_change = false;
	uint32_t _get_settle_frames(const Ref<SDFGI> &p_sdfgi, bool p_voxel_gi, bool p_temporal, bool p_sdfgi_screen_probes, bool p_voxel_gi_screen_probes) const;
	void _request_settle_frames(Ref<RenderBuffersGI> p_rbgi, uint32_t p_frames);

	// SDFGI screen probes: one per SCREEN_PROBE_TILE pixels square, placed at a different pixel of
	// their tile every frame over SCREEN_PROBE_JITTER_FRAMES frames, plus one more for a second
	// surface in the tile, where there is one, and adaptive ones on tiles of half and a quarter of
	// the size (SCREEN_PROBE_ADAPTIVE_LEVELS) where pixels are still left without one. What they
	// gather is noisy from one frame to the next, and each pixel averages it over up to
	// sdfgi_screen_probe_history_frames frames.
	enum {
		SCREEN_PROBE_TILE = 16,
		SCREEN_PROBES_PER_TILE = 2,
		SCREEN_PROBE_ADAPTIVE_LEVELS = 2,
		SCREEN_PROBE_JITTER_FRAMES = 16,
	};
	enum { // SCREEN_PROBE_FLAG_* in gi.glsl.
		SCREEN_PROBE_FLAG_SCREEN_TRACES = 1, // The previous frame's image is there to trace rays against.
	};

	GiShaderRD shader;
	RID shader_version;
	PipelineDeferredRD pipelines[SHADER_SPECIALIZATION_VARIATIONS][MODE_MAX];

	GI();
	~GI();

	void init(RendererRD::SkyRD *p_sky);
	void free();

	Ref<SDFGI> create_sdfgi(RID p_env, const Vector3 &p_world_position, uint32_t p_requested_history_size);

	void setup_voxel_gi_instances(RenderDataRD *p_render_data, Ref<RenderSceneBuffersRD> p_render_buffers, const Transform3D &p_transform, const PagedArray<RID> &p_voxel_gi_instances, uint32_t &r_voxel_gi_instances_used);
	// Screen probes need the last frame buffer (RB_SCOPE_SSLF) to be allocated and copied every frame.
	static bool is_using_screen_probes();
	void _setup_screen_probes(Ref<RenderSceneBuffersRD> p_render_buffers, Ref<RenderBuffersGI> p_rbgi, RID p_normal_roughness, RID p_voxel_gi_buffer, const Projection &p_projection, const Transform3D &p_cam_transform, uint32_t p_max_voxel_gi_instances, RID &r_trace_set, RID &r_filter_set, RID &r_gi_set, Size2i &r_probe_grid_size);
	void process_gi(Ref<RenderSceneBuffersRD> p_render_buffers, const RID *p_normal_roughness_slices, RID p_voxel_gi_buffer, RID p_environment, uint32_t p_view_count, const Projection *p_projections, const Vector3 *p_eye_offsets, const Transform3D &p_cam_transform, const PagedArray<RID> &p_voxel_gi_instances, bool p_use_screen_probes = false);

	RID voxel_gi_instance_create(RID p_base);
	void voxel_gi_instance_set_transform_to_data(RID p_probe, const Transform3D &p_xform);
	bool voxel_gi_needs_update(RID p_probe) const;
	void voxel_gi_update(RID p_probe, bool p_update_light_instances, const Vector<RID> &p_light_instances, const PagedArray<RenderGeometryInstance *> &p_dynamic_objects);
	void debug_voxel_gi(RID p_voxel_gi, RD::DrawListID p_draw_list, RID p_framebuffer, const Projection &p_camera_with_transform, bool p_lighting, bool p_emission, float p_alpha);

	void enable_vrs_shader_group();
};

} // namespace RendererRD
