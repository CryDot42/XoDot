/**************************************************************************/
/*  landscape_gpu.h                                                       */
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

#include "core/object/object.h"
#include "core/templates/local_vector.h"
#include "core/templates/safe_refcount.h"

// Layout of the uniform buffer consumed by shaders/landscape_lod.glsl (std140).
struct LandscapeLodParams {
	float planes[6][4] = {};
	float camera[4] = {};
	float lod[4] = {};
	float unit[4] = {};
	uint32_t grid[4] = {};
	uint32_t limits[4] = {};
	uint32_t flags[4] = {};
	uint32_t bounds_offsets[16] = {};
	uint32_t split_offsets[16] = {};
};

static_assert(sizeof(LandscapeLodParams) == 320, "LandscapeLodParams must match the std140 layout of the LOD shader.");

#ifdef RD_ENABLED

#include "servers/rendering/rendering_device.h"

class LandscapeLodShaderRD;
class LandscapeMapsShaderRD;

// Owns all RenderingDevice resources of a Landscape3D.
//
// Every method (except the constructor) must be called on the rendering thread,
// typically through RenderingServer::call_on_render_thread().
class LandscapeGPU : public Object {
	GDCLASS(LandscapeGPU, Object);

public:
	enum ListType {
		LIST_MAIN,
		LIST_SHADOW,
		LIST_MAX,
	};

	static constexpr int MAX_LEVELS = 16;
	static constexpr int COUNTER_COUNT = 32;
	static constexpr int MAX_WEIGHTMAPS = 4;

private:
	enum LodShaderMode {
		LOD_MODE_TRAVERSE,
		LOD_MODE_STITCH,
		LOD_MODE_MAX,
	};

	enum MapsShaderMode {
		MAPS_MODE_NORMALS_RG8,
		MAPS_MODE_NORMALS_RGBA8,
		MAPS_MODE_DOWNSAMPLE_RGBA8,
		MAPS_MODE_DOWNSAMPLE_RG8,
		MAPS_MODE_MAX,
	};

	struct Shared {
		LandscapeLodShaderRD *lod_shader = nullptr;
		RID lod_version;
		RID lod_shaders[LOD_MODE_MAX];
		RID lod_pipelines[LOD_MODE_MAX];

		LandscapeMapsShaderRD *maps_shader = nullptr;
		RID maps_version;
		RID maps_shaders[MAPS_MODE_MAX];
		RID maps_pipelines[MAPS_MODE_MAX];
	};

	static Shared *shared;
	static int shared_users;

	bool initialized = false;

	// Maps.
	struct Maps {
		Vector2i size;
		int mipmaps = 1;
		RID height; // R32F.
		RID normal; // Normal X and Z in RG (RG8, or RGBA8 if RG8 storage isn't supported), with mipmaps.
		RenderingDevice::DataFormat normal_format = RenderingDevice::DATA_FORMAT_R8G8_UNORM;
		RID weights[MAX_WEIGHTMAPS]; // RGBA8 with mipmaps, 1x1 when unused.
		int weightmap_count = 0;
		RID holes; // R8, 1x1 when the landscape has no holes.
		bool has_holes = false;
		LocalVector<RID> normal_views; // One per mipmap.
		LocalVector<RID> weight_views[MAX_WEIGHTMAPS];
	};
	Maps maps;
	bool maps_valid = false;

	// LOD.
	RID bounds_buffer;
	uint32_t bounds_size = 0;

	struct LodList {
		RID multimesh;
		RID params_buffer;
		RID node_lists;
		RID counters;
		RID args[2];
		RID split_bits;
		uint32_t split_bits_size = 0;
		uint32_t list_capacity = 0;
		uint32_t instance_capacity = 0;
	};
	LodList lists[LIST_MAX];

	// Statistics read back asynchronously from the traversal counters.
	SafeNumeric<uint32_t> stat_patches[LIST_MAX];
	SafeNumeric<uint32_t> stat_overflow[LIST_MAX];
	uint32_t stat_counter[LIST_MAX] = {};
	void _stats_received(const Vector<uint8_t> &p_data, int p_list);

	void _ensure_initialized();
	void _free_maps(Maps &r_maps);
	RID _create_map_texture(RenderingDevice::DataFormat p_format, const Vector2i &p_size, int p_mipmaps, bool p_storage, const String &p_name);
	void _free_lod();
	void _upload_rect(RID p_texture, RenderingDevice::DataFormat p_format, const Rect2i &p_rect, int p_layer, const Vector<uint8_t> &p_data);
	void _downsample(const LocalVector<RID> &p_views, int p_view_offset, const Rect2i &p_rect, bool p_rg8);

protected:
	static void _bind_methods() {}

public:
	void setup_maps(const Vector2i &p_size, int p_weightmap_count, bool p_holes, RID p_height_rs, RID p_normal_rs, const Array &p_weights_rs, RID p_holes_rs);
	void upload_heights(const Rect2i &p_rect, const Vector<uint8_t> &p_data);
	void upload_weights(const Rect2i &p_rect, int p_weightmap, const Vector<uint8_t> &p_data);
	void upload_holes(const Rect2i &p_rect, const Vector<uint8_t> &p_data);
	void update_normals(const Rect2i &p_rect, float p_spacing);
	void update_weight_mips(const Rect2i &p_rect);

	void setup_lod(int p_node_count, int p_list_capacity, int p_instance_capacity, int p_max_level, RID p_multimesh_main, RID p_multimesh_shadow);
	void upload_bounds(int p_offset, const Vector<float> &p_data);
	void run_lod(int p_list, const Vector<uint8_t> &p_params, int p_max_level);

	void free_resources();
	static void destroy(Object *p_gpu);

	// Thread-safe, may be called from any thread.
	uint32_t get_patch_count(int p_list) const { return stat_patches[CLAMP(p_list, 0, LIST_MAX - 1)].get(); }
	uint32_t get_overflow_flags(int p_list) const { return stat_overflow[CLAMP(p_list, 0, LIST_MAX - 1)].get(); }

	LandscapeGPU();
	~LandscapeGPU();
};

#endif // RD_ENABLED
