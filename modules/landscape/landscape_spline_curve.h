/**************************************************************************/
/*  landscape_spline_curve.h                                              */
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

#include "core/math/rect2.h"
#include "core/math/rect2i.h"
#include "core/math/transform_3d.h"
#include "core/templates/local_vector.h"

// Control point of a landscape spline.
struct LandscapeSplinePoint {
	Vector3 position;
	float width = 4.0; // Meters.
	float depth = 1.0; // Water depth below the surface (meters).
	float tilt = 0.0; // Banking around the spline direction (radians).
	float speed = 1.0; // Flow speed (m/s).
};

// Cross section of the tessellated spline.
struct LandscapeSplineSample {
	Vector3 position;
	Vector3 forward; // Unit tangent.
	Vector3 right; // Unit lateral axis, horizontal before banking.
	Vector3 up; // Normal of the (banked) surface.
	real_t distance = 0.0; // Arc length from the start.
	float width = 4.0;
	float depth = 1.0;
	float tilt = 0.0;
	float speed = 1.0;
	int segment = 0; // Control segment (from point `segment` to the next one).
};

// Smooth curve through the control points (centripetal Catmull-Rom, no cusps or loops within
// a segment) with smoothly interpolated point attributes.
class LandscapeSplineCurve {
public:
	struct Settings {
		real_t max_step = 2.0; // Maximum distance between two samples.
		real_t max_angle = 0.06; // Maximum direction change between two samples (radians).
		int max_samples_per_segment = 2048;
	};

	static int get_segment_count(int p_point_count, bool p_closed);
	// Position and derivative on a control segment, u in [0, 1].
	static void evaluate(const LocalVector<LandscapeSplinePoint> &p_points, bool p_closed, int p_segment, real_t p_u, Vector3 &r_position, Vector3 &r_derivative);
	// Samples along the curve. Closed curves end with a copy of the first sample (at the full length).
	// r_segment_starts[i] is the first sample of control segment i (plus a final entry: the sample count).
	static void tessellate(const LocalVector<LandscapeSplinePoint> &p_points, bool p_closed, const Settings &p_settings, LocalVector<LandscapeSplineSample> &r_samples, LocalVector<int> *r_segment_starts = nullptr);
	// Frames (forward, right, up) of the samples, from their positions and tilts.
	static void compute_frames(LocalVector<LandscapeSplineSample> &r_samples, bool p_closed);
	// Closest sample-polyline position to a point (XZ distance), returns the segment and the parameter.
	static bool get_closest(const LocalVector<LandscapeSplineSample> &p_samples, const Vector3 &p_point, int &r_segment, real_t &r_t, real_t &r_distance);
	static LandscapeSplineSample interpolate(const LocalVector<LandscapeSplineSample> &p_samples, int p_segment, real_t p_t);
};

// Terrain modification of a spline, in landscape local space.
struct LandscapeSplineTerrainShape {
	enum Profile {
		PROFILE_ROAD, // Flattens the terrain under the surface, with side and end falloffs.
		PROFILE_CHANNEL, // Carves a channel below the water surface (rivers, streams), with banks.
		PROFILE_LAKE, // Carves the inside of a closed curve below the water level, with banks.
	};

	// A part of the spline (one control segment, or the interior of a lake) used to track changes
	// and to decide which terrain tiles are under the spline.
	struct Block {
		Rect2i rect; // Landscape texels.
		uint64_t hash = 0;
	};

	Profile profile = PROFILE_ROAD;
	bool closed = false;
	LocalVector<LandscapeSplineSample> samples; // Landscape local space.
	LocalVector<float> radii; // Influence radius of each sample (meters).

	bool modify_heights = true;
	bool raise = true;
	bool lower = true;
	float falloff = 6.0;
	float end_falloff = 6.0;
	float offset = 0.05; // Road bed below the surface.
	float bank_height = 0.4; // Water: height of the banks above the water.
	float channel_shape = 2.0; // Water: exponent of the channel profile.
	float shore_width = 10.0; // Lake: distance from the shore to the full depth.
	float lake_level = 0.0;
	float lake_depth = 4.0;
	float strength = 1.0;

	int paint_layer = -1;
	float paint_width = 1.0; // Painted width beyond the core width (meters, may be negative).
	float paint_falloff = 2.0;
	float paint_noise = 0.5;
	float paint_strength = 1.0;
	uint32_t noise_seed = 0;

	LocalVector<Block> blocks;
	bool merge_dirty_blocks = false; // Changes of a block may affect the area between blocks (lakes).
	Rect2 bounds; // Influence area (meters, XZ).

	bool is_empty() const { return samples.size() < 2; }
	// Influence radius of a sample (meters).
	float compute_radius(const LandscapeSplineSample &p_sample) const;
	// Fills the radii, the bounds and the blocks (hashes include every setting and the landscape spacing).
	void finalize(const LocalVector<int> &p_segment_starts, real_t p_spacing, const Vector2i &p_landscape_size, uint64_t p_settings_hash);
};

class LandscapeSplineTerrain {
public:
	// Applies a spline to a rect of landscape texels. Heights: one float per texel. Weights:
	// p_layer_count normalized floats per texel (may be null).
	static void apply(const LandscapeSplineTerrainShape &p_shape, real_t p_spacing, const Rect2i &p_rect, float *r_heights, float *r_weights, int p_layer_count);
	// Target height, height blend and paint blend of the spline at a texel, from the closest point.
	// Exposed for tests.
	static bool evaluate(const LandscapeSplineTerrainShape &p_shape, const Vector2 &p_position, bool p_inside, int p_segment, real_t p_t, float &r_target, float &r_alpha, float &r_paint);
};
