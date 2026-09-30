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

#include "core/object/object.h"
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/variant/array.h"

// Layout of the uniform buffer consumed by shaders/landscape_foliage_cull.glsl (std140).
struct LandscapeFoliageCullParams {
	float planes[6][4] = {};
	float camera[4] = {};
	float lod_starts[8] = {};
	float cull[4] = {};
	float sphere[4] = {};
	uint32_t counts[4] = {};
};

static_assert(sizeof(LandscapeFoliageCullParams) == 192, "LandscapeFoliageCullParams must match the std140 layout of the foliage culling shader.");

#ifdef RD_ENABLED

#include "servers/rendering/rendering_device.h"

class LandscapeFoliageCullShaderRD;

// GPU culling and LOD selection of the foliage of a LandscapeFoliage3D (gpu_indirect).
//
// Per foliage type ("entry"): the instances of the cells close to the camera in a storage buffer
// (one slot per cell, updated separately), the level of detail of each instance (kept for the
// hysteresis), and counters. Every run classifies the instances and writes the visible ones into
// the instance buffers and draw commands of indirect MultiMeshes (one per level of detail and
// list: main, shadow), without CPU readback.
//
// Every method (except the constructor and the statistics) must be called on the rendering
// thread, typically through RenderingServer::call_on_render_thread().
class LandscapeFoliageGPU : public Object {
	GDCLASS(LandscapeFoliageGPU, Object);

public:
	static constexpr int MAX_LODS = 8;
	static constexpr int LIST_MAX = 2;
	static constexpr int OUTPUT_COUNT = MAX_LODS * LIST_MAX;
	static constexpr int INSTANCE_FLOATS = 16;
	static constexpr uint32_t MAX_OUTPUT_CAPACITY = (1 << 24) - 1;

	struct Stats {
		uint64_t serial = 0; // Run that produced the counts (0: none yet).
		uint32_t counts[OUTPUT_COUNT] = {}; // Instances emitted per output (may exceed its capacity).
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
	};
	static Shared *shared;
	static int shared_users;
	bool initialized = false;

	struct Entry {
		RID source;
		RID state;
		RID counters;
		RID params;
		uint32_t capacity = 0;
	};
	HashMap<uint64_t, Entry> entries; // Rendering thread only.

	mutable Mutex stats_mutex;
	HashMap<uint64_t, Stats> stats;
	static void _stats_received(const Vector<uint8_t> &p_data, ObjectID p_gpu, uint64_t p_id, uint64_t p_serial);

	void _ensure_initialized();
	void _free_entry(Entry &r_entry);

protected:
	static void _bind_methods() {}

public:
	// Recreates the instance buffers of the entry (every instance is a hole).
	void set_capacity(uint64_t p_id, uint32_t p_capacity);
	// Instances at an offset: INSTANCE_FLOATS floats each (3 x 4 transform, random value, cell,
	// unused, 1 for an instance or 0 for a hole). Their levels of detail are reset.
	void update_instances(uint64_t p_id, uint32_t p_offset, const Vector<float> &p_data);
	// p_params: LandscapeFoliageCullParams (counts[0]: instances to process from the start of the
	// buffer), p_outputs: OUTPUT_COUNT MultiMesh RIDs (list * MAX_LODS + lod, invalid when unused),
	// p_layout: per output, the capacity of the MultiMesh | surface count << 24.
	void run(uint64_t p_id, const Vector<uint8_t> &p_params, const Array &p_outputs, const Vector<int32_t> &p_layout, uint64_t p_serial);
	void free_entry(uint64_t p_id);

	void free_resources();
	static void destroy(Object *p_gpu);

	// Thread-safe (read back from the last run).
	Stats get_stats(uint64_t p_id) const;

	LandscapeFoliageGPU();
	~LandscapeFoliageGPU();
};

#endif // RD_ENABLED
