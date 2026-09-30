/**************************************************************************/
/*  landscape_horizon.h                                                   */
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

#include "landscape_lod_tree.h"

#include "core/math/aabb.h"
#include "core/math/rect2i.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"

// The horizon of the terrain seen from a point, to cull what the terrain hides (e.g. the foliage
// behind hills) independently of the depth buffer: without latency nor reprojection, it is as
// effective when the camera moves as when it stands still.
//
// For every azimuth around the point and every distance (rings, in geometric progression), the
// steepest elevation (tangent) of the terrain closer than that distance. It is conservative:
// - every sample of the terrain is the lowest height of the nodes of the LOD tree covering it
//   (the drawn terrain doesn't go lower, but for its geometric error: an angular margin of a few
//   pixels is subtracted, and a height margin for the displacement of the micro detail),
// - the nodes with a hole, or outside of the terrain, hide nothing,
// - an object is hidden only if it is below the horizon in every azimuth it covers, before it.
// Everything is in the space of the landscape (heights along its Y axis).
class LandscapeHorizon {
public:
	static constexpr int AZIMUTHS = 1024;
	static constexpr int RINGS = 64;
	static constexpr float NONE = -1e30f; // Nothing hides.

	struct Params {
		Vector3 camera;
		real_t range = 0.0; // Farthest distance of the objects to test.
		float angle_margin = 0.0; // Tangent subtracted from the horizon.
		float height_margin = 0.0; // Subtracted from the heights of the terrain.
		bool operator==(const Params &p_other) const { return camera == p_other.camera && range == p_other.range && angle_margin == p_other.angle_margin && height_margin == p_other.height_margin; }
	};

	// Nodes of the LOD tree that contain a hole: one byte per node and level (a hole in a node is
	// in all its ancestors).
	class Holes {
		int max_level = -1;
		LocalVector<uint8_t> levels[LandscapeLodTree::MAX_LEVELS + 1];
		bool any = false;

		void _update_ancestors(const Rect2i &p_leaves);

	public:
		void clear();
		// Replaces the holes of the leaves in a rect (in leaves) by the given leaves.
		void set_leaves(int p_max_level, const Rect2i &p_leaves, const HashSet<Vector2i> &p_holes);
		bool has_any() const { return any; }
		int get_max_level() const { return max_level; }
		_FORCE_INLINE_ bool has_hole(int p_level, int p_x, int p_z) const {
			return any && p_level <= max_level && levels[p_level][p_z * (1 << p_level) + p_x] != 0;
		}
	};

private:
	Params params; // As requested.
	real_t range = 0.0;
	real_t first_ring = 1.0; // Distance of the first ring.
	real_t ring_scale = 1.0; // Rings per unit of the logarithm of the distance.
	LocalVector<float> table; // AZIMUTHS * RINGS tangents (azimuth major).
	bool valid = false;
	uint64_t version = 0;

	struct BuildContext {
		const LandscapeLodTree *tree = nullptr;
		const Holes *holes = nullptr;
		real_t spacing = 1.0;
		real_t min_step = 1.0;
		LocalVector<real_t> rings;
	};
	void _build_azimuth(uint32_t p_azimuth, const BuildContext *p_context);
	float _get_lowest_height(const BuildContext &p_context, real_t p_min_x, real_t p_min_z, real_t p_max_x, real_t p_max_z) const;

	_FORCE_INLINE_ int _get_ring(real_t p_distance) const {
		if (p_distance < first_ring) {
			return -1;
		}
		return MIN(int(Math::log(p_distance / first_ring) * ring_scale), RINGS - 1);
	}
	bool _is_below(real_t p_azimuth_from, real_t p_azimuth_to, int p_ring, real_t p_tangent) const;

public:
	void build(const LandscapeLodTree &p_tree, real_t p_vertex_spacing, const Params &p_params, const Holes *p_holes);
	void clear();

	bool is_valid() const { return valid; }
	const Params &get_params() const { return params; }
	// Changes with every build (unique among the horizons).
	uint64_t get_version() const { return version; }

	const LocalVector<float> &get_table() const { return table; }
	real_t get_first_ring() const { return first_ring; }
	real_t get_ring_scale() const { return ring_scale; }
	// The tangent of the elevation of the horizon of an azimuth (radians) up to a distance.
	float get_horizon(real_t p_azimuth, real_t p_distance) const;

	// Whether the terrain hides a sphere or a box entirely from the point.
	bool is_sphere_hidden(const Vector3 &p_center, real_t p_radius) const;
	bool is_aabb_hidden(const AABB &p_aabb) const;
};
