/**************************************************************************/
/*  landscape_foliage_gpu.h                                               */
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

#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "core/object/object.h"
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/variant/array.h"

// Layout of the uniform buffer of the levels of detail consumed by shaders/landscape_foliage_cull.glsl (std140).
struct LandscapeFoliageCullParams {
	float camera[4] = {};
	float lod_starts[8] = {};
	float cull[4] = {};
	float sphere[4] = {};
	uint32_t counts[4] = {};
};

static_assert(sizeof(LandscapeFoliageCullParams) == 96, "LandscapeFoliageCullParams must match the std140 layout of the foliage culling shader.");

#ifdef RD_ENABLED

#include "servers/rendering/rendering_device.h"

class LandscapeFoliageCullShaderRD;

// GPU culling and LOD selection of the foliage of a LandscapeFoliage3D (gpu_indirect).
//
// Per foliage type ("entry"): the instances of the cells close to the camera in a storage buffer
// (one slot per cell, updated separately), the level of detail of each instance (kept for the
// hysteresis), and counters. The instances are written into the instance buffers and draw
// commands of indirect MultiMeshes (one per level of detail and list), without CPU readback:
// - once per frame (run_lod(), camera of the LOD of the landscape): the level of each instance,
//   and the shadow list (every instance within the cull distance),
// - for every camera drawn in the scenario (a callback of the renderer, once the occlusion buffer
//   of its viewport is up to date for it): the main list, the instances in the view frustum that
//   aren't hidden in the occlusion buffer (HZB). While the LOD of the landscape is frozen, the last
//   view of the LOD camera is kept instead.
//
// Every method (except the constructor and the statistics) must be called on the rendering
// thread, typically through RenderingServer::call_on_render_thread().
class LandscapeFoliageGPU : public Object {
	GDCLASS(LandscapeFoliageGPU, Object);

public:
	static constexpr int MAX_LODS = 8;
	static constexpr int LIST_MAIN = 0;
	static constexpr int LIST_SHADOW = 1;
	static constexpr int LIST_MAX = 2;
	static constexpr int OUTPUT_COUNT = MAX_LODS * LIST_MAX;
	static constexpr int INSTANCE_FLOATS = 16;
	static constexpr uint32_t MAX_OUTPUT_CAPACITY = (1 << 24) - 1;

	enum ViewFlags {
		VIEW_FRUSTUM_CULLING = 1,
		VIEW_OCCLUSION_CULLING = 2,
	};

	struct Stats {
		uint64_t serial = 0; // Outputs the counts were emitted with (see set_outputs(), 0: none yet).
		uint32_t lists = 0; // Bit per list with counts.
		uint32_t counts[OUTPUT_COUNT] = {}; // Instances emitted per output (may exceed its capacity).
		// Main list: instances out of the view frustum, hidden by occlusion.
		uint32_t frustum_culled = 0;
		uint32_t occlusion_culled = 0;
	};

private:
	enum Mode {
		MODE_CLASSIFY,
		MODE_EMIT,
		MODE_COMMAND,
		MODE_MAX,
	};

	struct Shared {
		LandscapeFoliageCullShaderRD *shader = nullptr;
		RID version;
		RID shaders[MODE_MAX];
		RID pipelines[MODE_MAX];
		LocalVector<LandscapeFoliageGPU *> users; // Culled for every camera drawn.
	};
	static Shared *shared;
	bool initialized = false;

	struct Output {
		RID multimesh;
		uint32_t capacity = 0;
		uint32_t surfaces = 0;
	};

	struct Entry {
		RID source;
		RID state;
		RID counters;
		RID params;
		uint32_t capacity = 0;
		uint32_t instance_count = 0; // Processed from the start of the source buffer.
		bool classified = false; // Levels chosen since the instance buffers were recreated.
		Output outputs[OUTPUT_COUNT];
		uint64_t serial = 0; // Of the outputs.
		uint64_t main_view = 0; // View that emitted the main list (0: to emit again).
	};
	HashMap<uint64_t, Entry> entries; // Rendering thread only.

	// Views (rendering thread only).
	struct View {
		bool valid = false;
		Transform3D transform;
		Projection projection;
		bool orthogonal = false;
		bool occlusion = false; // Occlusion buffer uploaded (HZB slot of the view).
		Size2i hzb_size;
		uint32_t hzb_mips[16][4] = {};
		int hzb_mip_count = 0;
	};
	RID scenario;
	RID lod_viewport; // Viewport of the camera of the LOD of the landscape.
	bool frozen = false;
	Transform3D space; // Landscape space to world space.
	uint32_t render_layers = 1;
	uint32_t view_flags = VIEW_FRUSTUM_CULLING | VIEW_OCCLUSION_CULLING;
	View lod_view; // Last view of the LOD camera (kept while frozen), occlusion buffer in slot 0.
	uint64_t lod_view_id = 0;
	View last_view; // Last camera drawn.
	RID last_view_viewport;
	uint64_t last_view_id = 0;
	uint64_t view_serial = 0;
	RID view_params;
	RID hzb_buffers[2]; // LOD camera, other cameras.
	uint32_t hzb_capacities[2] = {};

	mutable Mutex stats_mutex;
	struct StatsEntry {
		Stats last; // Last counts of each list (statistics).
		Stats peak; // Most instances emitted since the last take_peak_stats() (capacities).
	};
	HashMap<uint64_t, StatsEntry> stats;
	static void _stats_received(const Vector<uint8_t> &p_data, ObjectID p_gpu, uint64_t p_id, uint64_t p_serial, int p_list);

	void _ensure_initialized();
	void _free_entry(Entry &r_entry);
	void _ensure_view_resources();
	void _emit(Entry &r_entry, int p_list, RD::ComputeListID p_compute_list, RID p_hzb);
	bool _upload_occlusion(RID p_viewport, int p_slot, View &r_view);
	void _draw_view(RID p_viewport, const Transform3D &p_transform, const Projection &p_projection, bool p_orthogonal, uint32_t p_layers);
	static void _camera_drawn(RID p_scenario, RID p_viewport, const Transform3D &p_transform, const Projection &p_projection, bool p_orthogonal, uint32_t p_layers);

protected:
	static void _bind_methods() {}

public:
	// Recreates the instance buffers of the entry (every instance is a hole).
	void set_capacity(uint64_t p_id, uint32_t p_capacity);
	// Instances at an offset: INSTANCE_FLOATS floats each (3 x 4 transform, random value, cell,
	// unused, 1 for an instance or 0 for a hole). Their levels of detail are reset.
	void update_instances(uint64_t p_id, uint32_t p_offset, const Vector<float> &p_data);
	// p_outputs: OUTPUT_COUNT MultiMesh RIDs (list * MAX_LODS + lod, invalid when unused), p_layout: per
	// output, the capacity of the MultiMesh | surface count << 24. Must be called before the MultiMeshes
	// are freed or allocated again. The statistics of the outputs are tagged with p_serial.
	void set_outputs(uint64_t p_id, const Array &p_outputs, const Vector<int32_t> &p_layout, uint64_t p_serial);
	// Chooses the levels of detail and emits the shadow list. p_params: LandscapeFoliageCullParams
	// (counts[0]: instances to process from the start of the buffer).
	void run_lod(uint64_t p_id, const Vector<uint8_t> &p_params);
	// The cameras that cull the main lists: the ones drawn in the scenario that see the layers, the
	// view of the LOD camera (its viewport) while frozen. p_flags: ViewFlags.
	void set_view_settings(RID p_scenario, RID p_lod_viewport, bool p_frozen, const Transform3D &p_space, uint32_t p_render_layers, uint32_t p_flags);
	void free_entry(uint64_t p_id);

	void free_resources();
	static void destroy(Object *p_gpu);

	// Thread-safe (read back from the last runs).
	Stats get_stats(uint64_t p_id) const;
	// Thread-safe: the most instances emitted per output since the last call, for their capacities.
	Stats take_peak_stats(uint64_t p_id);

	LandscapeFoliageGPU();
	~LandscapeFoliageGPU();
};

#endif // RD_ENABLED
