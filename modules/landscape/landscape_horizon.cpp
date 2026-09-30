/**************************************************************************/
/*  landscape_horizon.cpp                                                 */
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

#include "landscape_horizon.h"

#include "core/object/worker_thread_pool.h"
#include "core/templates/safe_refcount.h"

static SafeNumeric<uint64_t> landscape_horizon_versions;

/* Holes */

void LandscapeHorizon::Holes::clear() {
	max_level = -1;
	for (LocalVector<uint8_t> &level : levels) {
		level.clear();
	}
	any = false;
}

void LandscapeHorizon::Holes::_update_ancestors(const Rect2i &p_leaves) {
	Rect2i rect = p_leaves;
	for (int l = max_level - 1; l >= 0; l--) {
		const Vector2i begin = rect.position / 2;
		const Vector2i end = (rect.get_end() - Vector2i(1, 1)) / 2;
		rect = Rect2i(begin, end - begin + Vector2i(1, 1));
		const int count = 1 << l;
		const int child_count = count * 2;
		const uint8_t *children = levels[l + 1].ptr();
		uint8_t *nodes = levels[l].ptr();
		for (int z = rect.position.y; z < rect.get_end().y; z++) {
			for (int x = rect.position.x; x < rect.get_end().x; x++) {
				const int c = (z * 2) * child_count + x * 2;
				nodes[z * count + x] = children[c] | children[c + 1] | children[c + child_count] | children[c + child_count + 1];
			}
		}
	}
}

void LandscapeHorizon::Holes::set_leaves(int p_max_level, const Rect2i &p_leaves, const HashSet<Vector2i> &p_holes) {
	ERR_FAIL_INDEX(p_max_level, LandscapeLodTree::MAX_LEVELS + 1);
	if (p_max_level != max_level) {
		max_level = p_max_level;
		for (int l = 0; l <= LandscapeLodTree::MAX_LEVELS; l++) {
			levels[l].clear();
			if (l <= max_level) {
				levels[l].resize_initialized(1u << (2 * l));
			}
		}
	}
	const int count = 1 << max_level;
	const Rect2i rect = p_leaves.intersection(Rect2i(0, 0, count, count));
	if (!rect.has_area()) {
		return;
	}
	uint8_t *leaves = levels[max_level].ptr();
	for (int z = rect.position.y; z < rect.get_end().y; z++) {
		for (int x = rect.position.x; x < rect.get_end().x; x++) {
			leaves[z * count + x] = 0;
		}
	}
	for (const Vector2i &leaf : p_holes) {
		if (rect.has_point(leaf)) {
			leaves[leaf.y * count + leaf.x] = 1;
		}
	}
	_update_ancestors(rect);
	any = levels[0][0] != 0;
}

/* Horizon */

float LandscapeHorizon::_get_lowest_height(const BuildContext &p_context, real_t p_min_x, real_t p_min_z, real_t p_max_x, real_t p_max_z) const {
	// The lowest terrain in a box (landscape space), NONE if a part of it has no terrain or a hole.
	const LandscapeLodTree &tree = *p_context.tree;
	const Vector2i size = tree.get_data_size();
	const real_t u0 = p_min_x / p_context.spacing;
	const real_t v0 = p_min_z / p_context.spacing;
	const real_t u1 = p_max_x / p_context.spacing;
	const real_t v1 = p_max_z / p_context.spacing;
	if (u0 < 0.0 || v0 < 0.0 || u1 > real_t(size.x - 1) || v1 > real_t(size.y - 1)) {
		return NONE;
	}
	// The finest level whose nodes are at least as large as the box: at most 2 x 2 nodes.
	const int max_level = tree.get_max_level();
	const int patch = tree.get_patch_quads();
	const real_t extent = MAX(u1 - u0, v1 - v0);
	int level = max_level;
	while (level > 0 && real_t(patch << (max_level - level)) < extent) {
		level--;
	}
	const int node_texels = patch << (max_level - level);
	const int count = 1 << level;
	const int x0 = CLAMP(int(u0) / node_texels, 0, count - 1);
	const int x1 = CLAMP(int(u1) / node_texels, 0, count - 1);
	const int z0 = CLAMP(int(v0) / node_texels, 0, count - 1);
	const int z1 = CLAMP(int(v1) / node_texels, 0, count - 1);
	const float *nodes = tree.get_nodes().ptr() + tree.get_level_offset(level) * LandscapeLodTree::FLOATS_PER_NODE;
	float lowest = Math::INF;
	for (int z = z0; z <= z1; z++) {
		for (int x = x0; x <= x1; x++) {
			if (p_context.holes && p_context.holes->has_hole(level, x, z)) {
				return NONE;
			}
			const float *node = nodes + (z * count + x) * LandscapeLodTree::FLOATS_PER_NODE;
			if (node[0] > node[1]) {
				return NONE; // Outside of the terrain.
			}
			lowest = MIN(lowest, node[0]);
		}
	}
	return lowest;
}

void LandscapeHorizon::_build_azimuth(uint32_t p_azimuth, const BuildContext *p_context) {
	// Marches away from the camera through the sector of the azimuth, with samples growing with the
	// distance. The azimuths start at multiples of 90 degrees every AZIMUTHS / 4 sectors, so the
	// corners of a sample bound it.
	const real_t sector = Math::TAU / AZIMUTHS;
	const real_t a0 = p_azimuth * sector;
	const real_t a1 = a0 + sector;
	const real_t c0 = Math::cos(a0);
	const real_t s0 = Math::sin(a0);
	const real_t c1 = Math::cos(a1);
	const real_t s1 = Math::sin(a1);
	const Vector3 &camera = params.camera;
	float *out = table.ptr() + p_azimuth * RINGS;
	float horizon = NONE;
	int ring = 0;
	real_t r = 0.0;
	while (r < range && ring < RINGS) {
		const real_t e = MIN(r + MAX(p_context->min_step, r * sector), range);
		// The rings before the end of the sample only see the terrain before it.
		while (ring < RINGS && p_context->rings[ring] < e) {
			out[ring++] = horizon;
		}
		const real_t xs[4] = { r * c0, r * c1, e * c0, e * c1 };
		const real_t zs[4] = { r * s0, r * s1, e * s0, e * s1 };
		real_t min_x = xs[0];
		real_t max_x = xs[0];
		real_t min_z = zs[0];
		real_t max_z = zs[0];
		for (int i = 1; i < 4; i++) {
			min_x = MIN(min_x, xs[i]);
			max_x = MAX(max_x, xs[i]);
			min_z = MIN(min_z, zs[i]);
			max_z = MAX(max_z, zs[i]);
		}
		const float lowest = _get_lowest_height(*p_context, camera.x + min_x, camera.z + min_z, camera.x + max_x, camera.z + max_z);
		if (lowest > NONE) {
			// The lowest elevation of the sample: farthest when above the camera, nearest below.
			const real_t height = lowest - params.height_margin - camera.y;
			const real_t tangent = height > 0.0 ? height / e : height / MAX(r, real_t(0.001));
			horizon = MAX(horizon, float(tangent));
		}
		r = e;
	}
	while (ring < RINGS) {
		out[ring++] = horizon;
	}
	for (int i = 0; i < RINGS; i++) {
		if (out[i] > NONE) {
			out[i] -= params.angle_margin;
		}
	}
}

void LandscapeHorizon::build(const LandscapeLodTree &p_tree, real_t p_vertex_spacing, const Params &p_params, const Holes *p_holes) {
	ERR_FAIL_COND(!p_tree.is_valid() || p_vertex_spacing <= 0.0);
	BuildContext context;
	context.tree = &p_tree;
	context.holes = p_holes && p_holes->has_any() && p_holes->get_max_level() == p_tree.get_max_level() ? p_holes : nullptr;
	context.spacing = p_vertex_spacing;
	// Half a patch of the finest level: the samples never span more than 2 x 2 nodes of a level.
	context.min_step = MAX(real_t(p_tree.get_patch_quads()) * p_vertex_spacing * 0.5, real_t(0.5));

	params = p_params;
	first_ring = MAX(context.min_step, real_t(1.0));
	range = MAX(params.range, first_ring * 2.0);
	ring_scale = real_t(RINGS - 1) / Math::log(range / first_ring);
	context.rings.resize(RINGS);
	for (int i = 0; i < RINGS; i++) {
		context.rings[i] = first_ring * Math::exp(real_t(i) / ring_scale);
	}
	context.rings[RINGS - 1] = range;

	table.resize(AZIMUTHS * RINGS);
	WorkerThreadPool::GroupID group = WorkerThreadPool::get_singleton()->add_template_group_task(this, &LandscapeHorizon::_build_azimuth, &context, AZIMUTHS, -1, true, SNAME("LandscapeHorizon"));
	WorkerThreadPool::get_singleton()->wait_for_group_task_completion(group);
	valid = true;
	version = landscape_horizon_versions.increment();
}

void LandscapeHorizon::clear() {
	valid = false;
	table.clear();
	version = landscape_horizon_versions.increment();
}

float LandscapeHorizon::get_horizon(real_t p_azimuth, real_t p_distance) const {
	const int ring = _get_ring(p_distance);
	if (!valid || ring < 0) {
		return NONE;
	}
	const int azimuth = int(Math::floor(p_azimuth * (AZIMUTHS / Math::TAU)));
	return table[(((azimuth % AZIMUTHS) + AZIMUTHS) % AZIMUTHS) * RINGS + ring];
}

bool LandscapeHorizon::_is_below(real_t p_azimuth_from, real_t p_azimuth_to, int p_ring, real_t p_tangent) const {
	const real_t scale = AZIMUTHS / Math::TAU;
	const int from = int(Math::floor(p_azimuth_from * scale));
	const int to = int(Math::floor(p_azimuth_to * scale));
	if (to - from >= AZIMUTHS / 8) {
		return false; // Too wide (close to the camera).
	}
	for (int i = from; i <= to; i++) {
		const int azimuth = ((i % AZIMUTHS) + AZIMUTHS) % AZIMUTHS;
		if (p_tangent >= table[azimuth * RINGS + p_ring]) {
			return false;
		}
	}
	return true;
}

bool LandscapeHorizon::is_sphere_hidden(const Vector3 &p_center, real_t p_radius) const {
	if (!valid) {
		return false;
	}
	const Vector3 &camera = params.camera;
	const real_t dx = p_center.x - camera.x;
	const real_t dz = p_center.z - camera.z;
	const real_t distance = Math::sqrt(dx * dx + dz * dz);
	if (distance <= p_radius) {
		return false;
	}
	const real_t nearest = distance - p_radius;
	const int ring = _get_ring(nearest);
	if (ring < 0) {
		return false;
	}
	// The steepest elevation of the sphere: its top, nearest when above the camera, farthest below.
	const real_t top = p_center.y + p_radius - camera.y;
	const real_t tangent = top > 0.0 ? top / nearest : top / (distance + p_radius);
	const real_t azimuth = Math::atan2(dz, dx);
	const real_t half = Math::asin(MIN(p_radius / distance, real_t(1.0)));
	return _is_below(azimuth - half, azimuth + half, ring, tangent);
}

bool LandscapeHorizon::is_aabb_hidden(const AABB &p_aabb) const {
	if (!valid) {
		return false;
	}
	const Vector3 &camera = params.camera;
	const Vector3 begin = p_aabb.position;
	const Vector3 end = p_aabb.position + p_aabb.size;
	if (camera.x >= begin.x && camera.x <= end.x && camera.z >= begin.z && camera.z <= end.z) {
		return false;
	}
	const real_t near_x = MAX(MAX(begin.x - camera.x, camera.x - end.x), real_t(0.0));
	const real_t near_z = MAX(MAX(begin.z - camera.z, camera.z - end.z), real_t(0.0));
	const real_t nearest = Math::sqrt(near_x * near_x + near_z * near_z);
	const int ring = _get_ring(nearest);
	if (ring < 0) {
		return false;
	}
	const real_t far_x = MAX(Math::abs(begin.x - camera.x), Math::abs(end.x - camera.x));
	const real_t far_z = MAX(Math::abs(begin.z - camera.z), Math::abs(end.z - camera.z));
	const real_t farthest = Math::sqrt(far_x * far_x + far_z * far_z);
	const real_t top = end.y - camera.y;
	const real_t tangent = top > 0.0 ? top / nearest : top / farthest;
	// The camera is outside of the box: its corners span less than half a turn around its center.
	const real_t center = Math::atan2((begin.z + end.z) * 0.5 - camera.z, (begin.x + end.x) * 0.5 - camera.x);
	real_t from = 0.0;
	real_t to = 0.0;
	for (int i = 0; i < 4; i++) {
		const real_t x = (i & 1) ? end.x : begin.x;
		const real_t z = (i & 2) ? end.z : begin.z;
		const real_t offset = Math::fposmod(Math::atan2(z - camera.z, x - camera.x) - center + Math::PI, Math::TAU) - Math::PI;
		from = MIN(from, offset);
		to = MAX(to, offset);
	}
	return _is_below(center + from, center + to, ring, tangent);
}
