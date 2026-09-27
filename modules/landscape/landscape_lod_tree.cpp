/**************************************************************************/
/*  landscape_lod_tree.cpp                                                */
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

#include "landscape_lod_tree.h"

#include "landscape_storage.h"

#include "core/math/math_funcs_binary.h"
#include "core/object/worker_thread_pool.h"

static constexpr float INVALID_MIN = 1e30f;
static constexpr float INVALID_MAX = -1e30f;
// Maximum number of samples gathered at once while building a level.
static constexpr int64_t MAX_BAND_SAMPLES = 4 * 1024 * 1024;

int LandscapeLodTree::compute_max_level(const Vector2i &p_data_size, int p_patch_quads) {
	const int quads = MAX(MAX(p_data_size.x, p_data_size.y) - 1, 1);
	int level = 0;
	while ((p_patch_quads << level) < quads && level < MAX_LEVELS - 1) {
		level++;
	}
	return level;
}

void LandscapeLodTree::clear() {
	nodes.clear();
	max_level = 0;
	root_quads = 0;
	data_size = Vector2i();
}

void LandscapeLodTree::_setup(const Vector2i &p_data_size, int p_patch_quads) {
	patch_quads = p_patch_quads;
	data_size = p_data_size;
	max_level = compute_max_level(data_size, patch_quads);
	root_quads = patch_quads << max_level;

	uint32_t offset = 0;
	for (int l = 0; l <= MAX_LEVELS; l++) {
		level_offsets[l] = offset;
		if (l <= max_level) {
			offset += 1u << (2 * l);
		}
	}
	nodes.resize(offset * FLOATS_PER_NODE);
}

void LandscapeLodTree::_compute_node(const Samples &p_samples, int p_level, int p_x, int p_z) {
	float *node = _node(p_level, p_x, p_z);
	const int node_texels = patch_quads << (max_level - p_level);
	const int x0 = p_x * node_texels;
	const int z0 = p_z * node_texels;

	if (x0 >= data_size.x - 1 || z0 >= data_size.y - 1) {
		node[0] = INVALID_MIN;
		node[1] = INVALID_MAX;
		node[2] = 0.0;
		node[3] = 0.0;
		return;
	}

	if (p_level == max_level) {
		// Finest heightmap level: the patch vertices match the heightmap texels exactly (mip 0).
		const int x1 = MIN(x0 + node_texels, data_size.x - 1);
		const int z1 = MIN(z0 + node_texels, data_size.y - 1);
		float mn = INVALID_MIN;
		float mx = INVALID_MAX;
		for (int z = z0; z <= z1; z++) {
			for (int x = x0; x <= x1; x++) {
				const float h = p_samples.get(x, z);
				mn = MIN(mn, h);
				mx = MAX(mx, h);
			}
		}
		node[0] = mn;
		node[1] = mx;
		node[2] = 0.0;
		node[3] = 0.0;
		return;
	}

	// Inner node: merge children bounds and measure how far the node grid deviates
	// from the grid of its children (evaluated at the children vertices, i.e. the texels
	// of the next finer mip level).
	float mn = INVALID_MIN;
	float mx = INVALID_MAX;
	float err = 0.0;
	for (int c = 0; c < 4; c++) {
		const float *child = _node(p_level + 1, p_x * 2 + (c & 1), p_z * 2 + (c >> 1));
		if (child[0] > child[1]) {
			continue; // Invalid (outside of the terrain).
		}
		mn = MIN(mn, child[0]);
		mx = MAX(mx, child[1]);
		err = MAX(err, child[2]);
	}

	const int half = node_texels / patch_quads / 2; // Child vertex spacing in texels (1 << mip).
	const int samples = patch_quads * 2;
	const int sx0 = p_x * samples; // In texels of the sample mip.
	const int sz0 = p_z * samples;
	for (int b = 0; b <= samples; b++) {
		if (z0 + b * half > data_size.y - 1 + half) {
			break;
		}
		const int bz0 = sz0 + (b & ~1);
		const int bz1 = sz0 + ((b + 1) & ~1);
		const float fb = (b & 1) ? 0.5f : 0.0f;
		for (int a = 0; a <= samples; a++) {
			if (!((a | b) & 1)) {
				continue; // Vertex shared with the parent grid.
			}
			if (x0 + a * half > data_size.x - 1 + half) {
				break;
			}
			const int ax0 = sx0 + (a & ~1);
			const int ax1 = sx0 + ((a + 1) & ~1);
			const float fa = (a & 1) ? 0.5f : 0.0f;
			const float h00 = p_samples.get(ax0, bz0);
			const float h10 = p_samples.get(ax1, bz0);
			const float h01 = p_samples.get(ax0, bz1);
			const float h11 = p_samples.get(ax1, bz1);
			const float interp = Math::lerp(Math::lerp(h00, h10, fa), Math::lerp(h01, h11, fa), fb);
			err = MAX(err, Math::abs(p_samples.get(sx0 + a, sz0 + b) - interp));
		}
	}

	node[0] = mn;
	node[1] = mx;
	node[2] = err;
	node[3] = 0.0;
}

void LandscapeLodTree::_compute_row(uint32_t p_index, BuildContext *p_context) {
	const int z = p_context->z_begin + int(p_index);
	for (int x = p_context->x_begin; x < p_context->x_end; x++) {
		_compute_node(*p_context->samples, p_context->level, x, z);
	}
}

void LandscapeLodTree::_compute_level(LandscapeStorage &p_storage, int p_level, const Rect2i &p_nodes) {
	const bool leaf = p_level == max_level;
	// Leaves sample mip 0, inner nodes the mip level of their children.
	const int sample_mip = leaf ? 0 : max_level - p_level - 1;
	const int node_samples = leaf ? patch_quads : patch_quads * 2;
	const int width = p_nodes.size.x * node_samples + 1;
	const int band_rows = CLAMP(int(MAX_BAND_SAMPLES / (int64_t(width) * node_samples)), 1, p_nodes.size.y);

	Samples samples;
	samples.mip = sample_mip;
	for (int z = p_nodes.position.y; z < p_nodes.get_end().y; z += band_rows) {
		const int rows = MIN(band_rows, p_nodes.get_end().y - z);
		samples.rect = Rect2i(p_nodes.position.x * node_samples, z * node_samples, width, rows * node_samples + 1);
		samples.heights.resize(int64_t(samples.rect.size.x) * samples.rect.size.y);
		p_storage.read_region(LandscapeStorage::LAYER_HEIGHTS, sample_mip, samples.rect, samples.heights.ptr());

		BuildContext context;
		context.samples = &samples;
		context.level = p_level;
		context.x_begin = p_nodes.position.x;
		context.x_end = p_nodes.get_end().x;
		context.z_begin = z;
		if (rows > 1 && int64_t(rows) * p_nodes.size.x > 64) {
			WorkerThreadPool::GroupID group = WorkerThreadPool::get_singleton()->add_template_group_task(this, &LandscapeLodTree::_compute_row, &context, rows, -1, true, SNAME("LandscapeLodTree"));
			WorkerThreadPool::get_singleton()->wait_for_group_task_completion(group);
		} else {
			for (int i = 0; i < rows; i++) {
				_compute_row(i, &context);
			}
		}
		p_storage.trim();
	}
}

void LandscapeLodTree::build(LandscapeStorage &p_storage, int p_patch_quads) {
	clear();
	ERR_FAIL_COND(!p_storage.is_valid());
	ERR_FAIL_COND(p_patch_quads < 2 || !Math::is_power_of_2(uint32_t(p_patch_quads)));
	_setup(p_storage.get_size(), p_patch_quads);
	ERR_FAIL_COND_MSG(max_level >= p_storage.get_mip_count() + 3, "Landscape LOD tree deeper than the storage mip levels.");
	for (int l = max_level; l >= 0; l--) {
		const int count = 1 << l;
		_compute_level(p_storage, l, Rect2i(0, 0, count, count));
	}
}

void LandscapeLodTree::get_ranges(const Rect2i &p_texel_rect, LocalVector<Range> &r_ranges) const {
	if (!is_valid()) {
		return;
	}
	const Rect2i rect = p_texel_rect.intersection(Rect2i(Point2i(), data_size));
	if (!rect.has_area()) {
		return;
	}
	for (int l = max_level; l >= 0; l--) {
		const int node_texels = patch_quads << (max_level - l);
		const int count = 1 << l;
		const int nz0 = CLAMP((rect.position.y - 1) / node_texels, 0, count - 1);
		const int nz1 = CLAMP((rect.get_end().y - 1) / node_texels, 0, count - 1);
		Range range;
		range.offset = (level_offsets[l] + uint32_t(nz0) * uint32_t(count)) * FLOATS_PER_NODE;
		range.count = uint32_t(nz1 - nz0 + 1) * uint32_t(count) * FLOATS_PER_NODE;
		r_ranges.push_back(range);
	}
}

void LandscapeLodTree::update(LandscapeStorage &p_storage, const Rect2i &p_texel_rect, LocalVector<Range> *r_ranges) {
	if (!is_valid() || p_storage.get_size() != data_size) {
		build(p_storage, patch_quads);
		if (r_ranges && is_valid()) {
			r_ranges->push_back({ 0, nodes.size() });
		}
		return;
	}

	const Rect2i rect = p_texel_rect.intersection(Rect2i(Point2i(), data_size));
	if (!rect.has_area()) {
		return;
	}
	for (int l = max_level; l >= 0; l--) {
		const int node_texels = patch_quads << (max_level - l);
		const int count = 1 << l;
		// Nodes share their border texels with their neighbors, hence the -1.
		const int nx0 = CLAMP((rect.position.x - 1) / node_texels, 0, count - 1);
		const int nz0 = CLAMP((rect.position.y - 1) / node_texels, 0, count - 1);
		const int nx1 = CLAMP((rect.get_end().x - 1) / node_texels, 0, count - 1);
		const int nz1 = CLAMP((rect.get_end().y - 1) / node_texels, 0, count - 1);
		_compute_level(p_storage, l, Rect2i(nx0, nz0, nx1 - nx0 + 1, nz1 - nz0 + 1));
	}
	if (r_ranges) {
		get_ranges(p_texel_rect, *r_ranges);
	}
}

bool LandscapeLodTree::set_nodes(const Vector2i &p_data_size, int p_patch_quads, const LocalVector<float> &p_nodes) {
	clear();
	_setup(p_data_size, p_patch_quads);
	if (p_nodes.size() != nodes.size()) {
		clear();
		return false;
	}
	memcpy(nodes.ptr(), p_nodes.ptr(), nodes.size() * sizeof(float));
	return true;
}

bool LandscapeLodTree::get_node(int p_level, int p_x, int p_z, float &r_min, float &r_max, float &r_error) const {
	ERR_FAIL_COND_V(!is_valid(), false);
	ERR_FAIL_INDEX_V(p_level, max_level + 1, false);
	const int count = 1 << p_level;
	ERR_FAIL_INDEX_V(p_x, count, false);
	ERR_FAIL_INDEX_V(p_z, count, false);
	const float *node = &nodes[(level_offsets[p_level] + uint32_t(p_z) * count + uint32_t(p_x)) * FLOATS_PER_NODE];
	r_min = node[0];
	r_max = node[1];
	r_error = node[2];
	return node[0] <= node[1];
}

Vector2 LandscapeLodTree::get_height_range() const {
	if (!is_valid() || nodes[0] > nodes[1]) {
		return Vector2();
	}
	return Vector2(nodes[0], nodes[1]);
}

float LandscapeLodTree::get_root_error() const {
	return is_valid() ? nodes[2] : 0.0f;
}
