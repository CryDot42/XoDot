/**************************************************************************/
/*  landscape_lod_tree.h                                                  */
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

#include "core/math/rect2i.h"
#include "core/math/vector2.h"
#include "core/templates/local_vector.h"

class LandscapeStorage;

// CPU-side quadtree used by the GPU traversal for screen-space error (SSE) LOD selection.
//
// Level 0 is the root, level `max_level` holds the finest heightmap patches
// (`patch_quads` x `patch_quads` quads at full heightmap resolution).
// Every node stores (min height, max height, geometric error, unused) where the
// geometric error is the maximum vertical deviation between the node mesh and
// the finer representation of the terrain (monotonic: never smaller than any child error).
//
// A node of level l is rendered with the heights of mip level (max_level - l) of the storage,
// its error is measured against the samples of the next finer mip level, so building
// and updating the tree only reads the storage tiles it needs.
class LandscapeLodTree {
public:
	static constexpr int MAX_LEVELS = 16;
	static constexpr int FLOATS_PER_NODE = 4;

	struct Range {
		uint32_t offset = 0; // In floats.
		uint32_t count = 0; // In floats.
	};

private:
	int patch_quads = 32;
	int max_level = 0;
	int root_quads = 0;
	Vector2i data_size;
	LocalVector<float> nodes;
	uint32_t level_offsets[MAX_LEVELS + 1] = {};

	// Heights of a region of one mip level.
	struct Samples {
		int mip = 0;
		Rect2i rect;
		LocalVector<float> heights;
		_FORCE_INLINE_ float get(int p_x, int p_z) const { return heights[(p_z - rect.position.y) * rect.size.x + (p_x - rect.position.x)]; }
	};

	struct BuildContext {
		const Samples *samples = nullptr;
		int level = 0;
		int x_begin = 0;
		int x_end = 0;
		int z_begin = 0;
	};

	_FORCE_INLINE_ float *_node(int p_level, int p_x, int p_z) {
		return &nodes[(level_offsets[p_level] + uint32_t(p_z) * (1u << p_level) + uint32_t(p_x)) * FLOATS_PER_NODE];
	}

	void _setup(const Vector2i &p_data_size, int p_patch_quads);
	void _compute_node(const Samples &p_samples, int p_level, int p_x, int p_z);
	void _compute_row(uint32_t p_index, BuildContext *p_context);
	void _compute_level(LandscapeStorage &p_storage, int p_level, const Rect2i &p_nodes);

public:
	static int compute_max_level(const Vector2i &p_data_size, int p_patch_quads);

	void build(LandscapeStorage &p_storage, int p_patch_quads);
	void update(LandscapeStorage &p_storage, const Rect2i &p_texel_rect, LocalVector<Range> *r_ranges = nullptr);
	// Node ranges (one per level) affected by a change of the given texels.
	void get_ranges(const Rect2i &p_texel_rect, LocalVector<Range> &r_ranges) const;
	// Restores a tree saved with get_nodes(). Returns false if the nodes don't match the layout.
	bool set_nodes(const Vector2i &p_data_size, int p_patch_quads, const LocalVector<float> &p_nodes);
	void clear();

	bool is_valid() const { return !nodes.is_empty(); }
	int get_patch_quads() const { return patch_quads; }
	int get_max_level() const { return max_level; }
	int get_root_quads() const { return root_quads; }
	Vector2i get_data_size() const { return data_size; }
	uint32_t get_level_offset(int p_level) const { return level_offsets[CLAMP(p_level, 0, MAX_LEVELS)]; }
	uint32_t get_node_count() const { return level_offsets[max_level + 1]; }
	const LocalVector<float> &get_nodes() const { return nodes; }

	bool get_node(int p_level, int p_x, int p_z, float &r_min, float &r_max, float &r_error) const;
	Vector2 get_height_range() const;
	float get_root_error() const;
};
