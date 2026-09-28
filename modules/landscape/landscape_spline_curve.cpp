/**************************************************************************/
/*  landscape_spline_curve.cpp                                            */
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

#include "landscape_spline_curve.h"

#include "core/templates/hashfuncs.h"

// Distance over which water banks rise from the water line to their full height.
static constexpr real_t BANK_RISE = 2.0;

/* Curve */

int LandscapeSplineCurve::get_segment_count(int p_point_count, bool p_closed) {
	if (p_closed) {
		return p_point_count >= 3 ? p_point_count : 0;
	}
	return p_point_count >= 2 ? p_point_count - 1 : 0;
}

static Vector3 _spline_point(const LocalVector<LandscapeSplinePoint> &p_points, bool p_closed, int p_index) {
	const int n = p_points.size();
	if (p_closed) {
		return p_points[((p_index % n) + n) % n].position;
	}
	// Open ends continue straight (reflected neighbor), so the curve leaves the end points along the first segment.
	if (p_index < 0) {
		return p_points[0].position * 2.0 - p_points[1].position;
	}
	if (p_index >= n) {
		return p_points[n - 1].position * 2.0 - p_points[n - 2].position;
	}
	return p_points[p_index].position;
}

void LandscapeSplineCurve::evaluate(const LocalVector<LandscapeSplinePoint> &p_points, bool p_closed, int p_segment, real_t p_u, Vector3 &r_position, Vector3 &r_derivative) {
	const Vector3 p0 = _spline_point(p_points, p_closed, p_segment - 1);
	const Vector3 p1 = _spline_point(p_points, p_closed, p_segment);
	const Vector3 p2 = _spline_point(p_points, p_closed, p_segment + 1);
	const Vector3 p3 = _spline_point(p_points, p_closed, p_segment + 2);
	// Centripetal knot intervals (square root of the chord lengths).
	const real_t d01 = MAX(Math::sqrt(p0.distance_to(p1)), real_t(1e-4));
	const real_t d12 = MAX(Math::sqrt(p1.distance_to(p2)), real_t(1e-4));
	const real_t d23 = MAX(Math::sqrt(p2.distance_to(p3)), real_t(1e-4));
	// Tangents of the equivalent Hermite segment, for u in [0, 1].
	const Vector3 m1 = ((p1 - p0) / d01 - (p2 - p0) / (d01 + d12) + (p2 - p1) / d12) * d12;
	const Vector3 m2 = ((p2 - p1) / d12 - (p3 - p1) / (d12 + d23) + (p3 - p2) / d23) * d12;
	const real_t u = p_u;
	const real_t u2 = u * u;
	const real_t u3 = u2 * u;
	r_position = p1 * (2.0 * u3 - 3.0 * u2 + 1.0) + m1 * (u3 - 2.0 * u2 + u) + p2 * (-2.0 * u3 + 3.0 * u2) + m2 * (u3 - u2);
	r_derivative = p1 * (6.0 * u2 - 6.0 * u) + m1 * (3.0 * u2 - 4.0 * u + 1.0) + p2 * (-6.0 * u2 + 6.0 * u) + m2 * (3.0 * u2 - 2.0 * u);
}

static void _spline_interpolate_attributes(const LandscapeSplinePoint &p_a, const LandscapeSplinePoint &p_b, real_t p_u, LandscapeSplineSample &r_sample) {
	// Smoothstep: continuous and without overshoot (widths and depths stay in range).
	const float w = float(p_u * p_u * (3.0 - 2.0 * p_u));
	r_sample.width = Math::lerp(p_a.width, p_b.width, w);
	r_sample.depth = Math::lerp(p_a.depth, p_b.depth, w);
	r_sample.tilt = Math::lerp(p_a.tilt, p_b.tilt, w);
	r_sample.speed = Math::lerp(p_a.speed, p_b.speed, w);
}

void LandscapeSplineCurve::tessellate(const LocalVector<LandscapeSplinePoint> &p_points, bool p_closed, const Settings &p_settings, LocalVector<LandscapeSplineSample> &r_samples, LocalVector<int> *r_segment_starts) {
	r_samples.clear();
	if (r_segment_starts) {
		r_segment_starts->clear();
	}
	const int segments = get_segment_count(p_points.size(), p_closed);
	if (segments == 0) {
		return;
	}
	const int n = p_points.size();
	static constexpr int TABLE = 32;
	const real_t max_step = MAX(p_settings.max_step, real_t(0.01));
	const real_t max_angle = MAX(p_settings.max_angle, real_t(0.001));

	real_t distance = 0.0;
	Vector3 last_position = p_points[0].position;
	for (int s = 0; s < segments; s++) {
		if (r_segment_starts) {
			r_segment_starts->push_back(r_samples.size());
		}
		// Arc length table of the segment, also used to measure how much it turns.
		Vector3 table_positions[TABLE + 1];
		real_t table_lengths[TABLE + 1];
		Vector3 derivative;
		evaluate(p_points, p_closed, s, 0.0, table_positions[0], derivative);
		table_lengths[0] = 0.0;
		real_t turn = 0.0;
		Vector3 previous_direction;
		for (int k = 1; k <= TABLE; k++) {
			evaluate(p_points, p_closed, s, real_t(k) / TABLE, table_positions[k], derivative);
			const Vector3 chord = table_positions[k] - table_positions[k - 1];
			const real_t chord_length = chord.length();
			table_lengths[k] = table_lengths[k - 1] + chord_length;
			if (chord_length > CMP_EPSILON) {
				const Vector3 direction = chord / chord_length;
				if (previous_direction != Vector3()) {
					turn += previous_direction.angle_to(direction);
				}
				previous_direction = direction;
			}
		}
		const real_t length = table_lengths[TABLE];
		const int count = CLAMP(MAX(int(Math::ceil(length / max_step)), int(Math::ceil(turn / max_angle))), 1, MAX(p_settings.max_samples_per_segment, 1));

		// Samples evenly spaced along the arc length.
		int cursor = 0;
		for (int k = 0; k < count; k++) {
			const real_t target = length * real_t(k) / real_t(count);
			while (cursor < TABLE - 1 && table_lengths[cursor + 1] < target) {
				cursor++;
			}
			const real_t span = table_lengths[cursor + 1] - table_lengths[cursor];
			const real_t f = span > CMP_EPSILON ? CLAMP((target - table_lengths[cursor]) / span, real_t(0.0), real_t(1.0)) : 0.0;
			const real_t u = (real_t(cursor) + f) / real_t(TABLE);
			LandscapeSplineSample sample;
			evaluate(p_points, p_closed, s, u, sample.position, sample.forward);
			distance += sample.position.distance_to(last_position);
			last_position = sample.position;
			sample.distance = distance;
			sample.segment = s;
			_spline_interpolate_attributes(p_points[s], p_points[(s + 1) % n], u, sample);
			r_samples.push_back(sample);
		}
	}

	// Last sample: the end of the curve, or a copy of the first sample for closed curves.
	LandscapeSplineSample end;
	if (p_closed) {
		end = r_samples[0];
	} else {
		evaluate(p_points, p_closed, segments - 1, 1.0, end.position, end.forward);
		_spline_interpolate_attributes(p_points[n - 1], p_points[n - 1], 1.0, end);
	}
	end.segment = segments - 1;
	distance += end.position.distance_to(last_position);
	end.distance = distance;
	r_samples.push_back(end);
	if (r_segment_starts) {
		r_segment_starts->push_back(r_samples.size() - 1);
	}
	compute_frames(r_samples, p_closed);
}

void LandscapeSplineCurve::compute_frames(LocalVector<LandscapeSplineSample> &r_samples, bool p_closed) {
	const int count = r_samples.size();
	Vector3 last_right(1, 0, 0);
	for (int i = 0; i < count; i++) {
		LandscapeSplineSample &sample = r_samples[i];
		Vector3 forward = sample.forward;
		if (forward.length_squared() < CMP_EPSILON2) {
			// Degenerate derivative (duplicated points): use the neighbors.
			const int prev = i > 0 ? i - 1 : (p_closed ? count - 2 : 0);
			const int next = i < count - 1 ? i + 1 : (p_closed ? 1 : count - 1);
			forward = r_samples[next].position - r_samples[prev].position;
		}
		forward = forward.length_squared() > CMP_EPSILON2 ? forward.normalized() : Vector3(0, 0, 1);
		Vector3 right = forward.cross(Vector3(0, 1, 0));
		if (right.length_squared() < 1e-6) {
			right = last_right; // Vertical direction, keep the previous lateral axis.
		} else {
			right.normalize();
		}
		last_right = right;
		const Vector3 up = right.cross(forward).normalized();
		// Banking rotates the cross section around the direction (positive raises the right side).
		const real_t c = Math::cos(real_t(sample.tilt));
		const real_t s = Math::sin(real_t(sample.tilt));
		const Vector3 banked_right = (right * c + up * s).normalized();
		sample.forward = forward;
		sample.right = banked_right;
		sample.up = banked_right.cross(forward).normalized();
	}
}

bool LandscapeSplineCurve::get_closest(const LocalVector<LandscapeSplineSample> &p_samples, const Vector3 &p_point, int &r_segment, real_t &r_t, real_t &r_distance) {
	r_segment = -1;
	r_t = 0.0;
	r_distance = Math::INF;
	const Vector2 p(p_point.x, p_point.z);
	for (uint32_t i = 0; i + 1 < p_samples.size(); i++) {
		const Vector2 a(p_samples[i].position.x, p_samples[i].position.z);
		const Vector2 b(p_samples[i + 1].position.x, p_samples[i + 1].position.z);
		const Vector2 ab = b - a;
		const real_t len2 = ab.length_squared();
		const real_t t = len2 > 0.0 ? CLAMP((p - a).dot(ab) / len2, real_t(0.0), real_t(1.0)) : 0.0;
		const real_t d = p.distance_to(a + ab * t);
		if (d < r_distance) {
			r_distance = d;
			r_segment = i;
			r_t = t;
		}
	}
	return r_segment >= 0;
}

LandscapeSplineSample LandscapeSplineCurve::interpolate(const LocalVector<LandscapeSplineSample> &p_samples, int p_segment, real_t p_t) {
	const LandscapeSplineSample &a = p_samples[p_segment];
	const LandscapeSplineSample &b = p_samples[MIN(p_segment + 1, int(p_samples.size()) - 1)];
	LandscapeSplineSample r = a;
	const float t = float(p_t);
	r.position = a.position.lerp(b.position, p_t);
	const Vector3 forward = a.forward.lerp(b.forward, p_t);
	r.forward = forward.length_squared() > CMP_EPSILON2 ? forward.normalized() : a.forward;
	const Vector3 right = a.right.lerp(b.right, p_t);
	r.right = right.length_squared() > CMP_EPSILON2 ? right.normalized() : a.right;
	const Vector3 up = a.up.lerp(b.up, p_t);
	r.up = up.length_squared() > CMP_EPSILON2 ? up.normalized() : a.up;
	r.distance = Math::lerp(a.distance, b.distance, p_t);
	r.width = Math::lerp(a.width, b.width, t);
	r.depth = Math::lerp(a.depth, b.depth, t);
	r.tilt = Math::lerp(a.tilt, b.tilt, t);
	r.speed = Math::lerp(a.speed, b.speed, t);
	return r;
}

/* Terrain shape */

float LandscapeSplineTerrainShape::compute_radius(const LandscapeSplineSample &p_sample) const {
	float core = profile == PROFILE_LAKE ? 0.0f : p_sample.width * 0.5f;
	float extent = 0.0;
	if (modify_heights) {
		extent = MAX(falloff, 0.0f);
		if (profile == PROFILE_LAKE) {
			extent = MAX(extent, shore_width);
		}
	}
	if (paint_layer >= 0) {
		extent = MAX(extent, paint_width + MAX(paint_falloff, 0.0f) * (1.0f + paint_noise));
		if (profile == PROFILE_LAKE) {
			// Inside the lake, the paint fades in from the shore.
			extent = MAX(extent, -paint_width + MAX(paint_falloff, 0.0f) * (1.0f + paint_noise));
		}
	}
	return core + MAX(extent, 0.0f);
}

static _FORCE_INLINE_ uint64_t _spline_hash_real(real_t p_value, uint64_t p_hash) {
	// Quantized to 0.1 mm: identical inputs give identical hashes on every platform.
	return hash_murmur3_one_64(uint64_t(int64_t(Math::round(double(p_value) * 10000.0))), p_hash);
}

void LandscapeSplineTerrainShape::finalize(const LocalVector<int> &p_segment_starts, real_t p_spacing, const Vector2i &p_landscape_size, uint64_t p_settings_hash) {
	blocks.clear();
	radii.resize(samples.size());
	bounds = Rect2();
	if (samples.size() < 2) {
		return;
	}
	const real_t spacing = MAX(p_spacing, real_t(CMP_EPSILON));
	const Rect2i landscape_rect(Point2i(), p_landscape_size);
	auto to_texels = [&](const Rect2 &p_rect) {
		const Point2i begin(int(Math::floor(p_rect.position.x / spacing)) - 1, int(Math::floor(p_rect.position.y / spacing)) - 1);
		const Point2i end(int(Math::ceil(p_rect.get_end().x / spacing)) + 2, int(Math::ceil(p_rect.get_end().y / spacing)) + 2);
		return Rect2i(begin, end - begin).intersection(landscape_rect);
	};

	bool first = true;
	for (uint32_t i = 0; i < samples.size(); i++) {
		real_t r = compute_radius(samples[i]);
		if (!closed && (i == 0 || i == samples.size() - 1)) {
			r += MAX(end_falloff, 0.0f);
		}
		radii[i] = r;
		const Rect2 sample_rect(Vector2(samples[i].position.x, samples[i].position.z) - Vector2(r, r), Vector2(r, r) * 2.0);
		bounds = first ? sample_rect : bounds.merge(sample_rect);
		first = false;
	}

	// One block per control segment. Its hash covers every sample that influences its area
	// (including the neighbor samples, whose segments reach into it).
	const int segment_count = int(p_segment_starts.size()) - 1;
	for (int s = 0; s < segment_count; s++) {
		const int begin = MAX(p_segment_starts[s] - 1, 0);
		const int end = MIN(p_segment_starts[s + 1] + 1, int(samples.size()) - 1);
		Rect2 rect;
		uint64_t hash = hash_murmur3_one_64(p_settings_hash, uint64_t(s));
		for (int i = begin; i <= end; i++) {
			const LandscapeSplineSample &sample = samples[i];
			const Rect2 sample_rect(Vector2(sample.position.x, sample.position.z) - Vector2(radii[i], radii[i]), Vector2(radii[i], radii[i]) * 2.0);
			rect = i == begin ? sample_rect : rect.merge(sample_rect);
			hash = _spline_hash_real(sample.position.x, hash);
			hash = _spline_hash_real(sample.position.y, hash);
			hash = _spline_hash_real(sample.position.z, hash);
			hash = _spline_hash_real(sample.forward.x, hash);
			hash = _spline_hash_real(sample.forward.z, hash);
			hash = _spline_hash_real(sample.width, hash);
			hash = _spline_hash_real(sample.depth, hash);
			hash = _spline_hash_real(sample.tilt, hash);
		}
		Block block;
		block.rect = to_texels(rect);
		block.hash = hash;
		blocks.push_back(block);
	}
	if (profile == PROFILE_LAKE) {
		// The carved interior: depends on the settings (depth, level) and on the extent of the lake.
		Block interior;
		interior.rect = to_texels(bounds);
		interior.hash = hash_murmur3_one_64(p_settings_hash, 0x1a4e);
		blocks.push_back(interior);
	}
	merge_dirty_blocks = profile == PROFILE_LAKE;
}

/* Terrain application */

static _FORCE_INLINE_ float _spline_smoothstep(float p_edge, float p_x) {
	if (p_edge <= 0.0f) {
		return p_x > 0.0f ? 1.0f : 0.0f;
	}
	const float t = CLAMP(p_x / p_edge, 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

static _FORCE_INLINE_ float _spline_value(int p_x, int p_z, uint32_t p_seed) {
	uint32_t h = uint32_t(p_x) * 374761393u + uint32_t(p_z) * 668265263u + p_seed * 2246822519u;
	h = (h ^ (h >> 13)) * 1274126177u;
	h ^= h >> 16;
	return (h & 0xFFFFFF) / float(0xFFFFFF);
}

// Smooth value noise in [0, 1], two octaves (roughly 6 m and 2 m features).
static float _spline_noise(const Vector2 &p_position, uint32_t p_seed) {
	float result = 0.0;
	float amplitude = 0.65;
	real_t frequency = 1.0 / 6.0;
	for (int octave = 0; octave < 2; octave++) {
		const real_t x = p_position.x * frequency;
		const real_t z = p_position.y * frequency;
		const int ix = int(Math::floor(x));
		const int iz = int(Math::floor(z));
		float fx = float(x - ix);
		float fz = float(z - iz);
		fx = fx * fx * (3.0f - 2.0f * fx);
		fz = fz * fz * (3.0f - 2.0f * fz);
		const uint32_t seed = p_seed + uint32_t(octave) * 1013u;
		const float v00 = _spline_value(ix, iz, seed);
		const float v10 = _spline_value(ix + 1, iz, seed);
		const float v01 = _spline_value(ix, iz + 1, seed);
		const float v11 = _spline_value(ix + 1, iz + 1, seed);
		result += Math::lerp(Math::lerp(v00, v10, fx), Math::lerp(v01, v11, fx), fz) * amplitude;
		amplitude *= 0.54;
		frequency *= 3.0;
	}
	return result;
}

bool LandscapeSplineTerrain::evaluate(const LandscapeSplineTerrainShape &p_shape, const Vector2 &p_position, bool p_inside, int p_segment, real_t p_t, float &r_target, float &r_alpha, float &r_paint) {
	r_target = 0.0;
	r_alpha = 0.0;
	r_paint = 0.0;
	const bool paint = p_shape.paint_layer >= 0 && p_shape.paint_strength > 0.0f;
	const float noise = (paint && p_shape.paint_noise > 0.0f) ? (_spline_noise(p_position, p_shape.noise_seed) * 2.0f - 1.0f) * p_shape.paint_noise * MAX(p_shape.paint_falloff, 0.5f) : 0.0f;

	if (p_segment < 0) {
		// Deep inside a lake, away from the shore.
		if (!p_inside) {
			return false;
		}
		r_target = p_shape.lake_level - p_shape.lake_depth;
		r_alpha = p_shape.strength;
		r_paint = paint ? p_shape.paint_strength : 0.0f;
		return true;
	}

	const LandscapeSplineSample sample = LandscapeSplineCurve::interpolate(p_shape.samples, p_segment, p_t);
	const Vector2 center(sample.position.x, sample.position.z);
	Vector2 forward(sample.forward.x, sample.forward.z);
	forward = forward.length_squared() > CMP_EPSILON2 ? forward.normalized() : Vector2(0, 1);
	const Vector2 right(-forward.y, forward.x); // Horizontal right axis (forward x up).
	const Vector2 delta = p_position - center;
	const real_t along = delta.dot(forward);
	const real_t side = delta.dot(right);

	// Beyond the ends of open curves, the profile is extended along the end direction and faded out.
	real_t beyond = 0.0;
	if (!p_shape.closed) {
		const int last = int(p_shape.samples.size()) - 2;
		if (p_segment == 0 && p_t <= 0.0 && along < 0.0) {
			beyond = -along;
		} else if (p_segment == last && p_t >= 1.0 && along > 0.0) {
			beyond = along;
		}
	}
	const real_t lateral = beyond > 0.0 ? Math::abs(side) : delta.length();
	const float end_fade = beyond > 0.0 ? 1.0f - _spline_smoothstep(p_shape.end_falloff, float(beyond)) : 1.0f;
	if (end_fade <= 0.0f) {
		return false;
	}

	const float half_width = MAX(sample.width * 0.5f, 0.01f);
	float paint_edge = 0.0; // Signed distance to the edge of the painted area.
	switch (p_shape.profile) {
		case LandscapeSplineTerrainShape::PROFILE_ROAD: {
			const float edge = float(lateral) - half_width;
			// Banked surface, extended flat beyond the edges.
			const float lateral_on_surface = CLAMP(float(side >= 0.0 ? lateral : -lateral), -half_width, half_width);
			const float bank = Math::tan(CLAMP(sample.tilt, -1.2f, 1.2f));
			r_target = float(sample.position.y) + lateral_on_surface * bank - p_shape.offset;
			r_alpha = edge <= 0.0f ? 1.0f : 1.0f - _spline_smoothstep(p_shape.falloff, edge);
			paint_edge = edge - p_shape.paint_width;
		} break;
		case LandscapeSplineTerrainShape::PROFILE_CHANNEL: {
			const float surface = float(sample.position.y);
			const float edge = float(lateral) - half_width;
			if (edge <= 0.0f) {
				const float x = float(lateral) / half_width;
				r_target = surface - sample.depth * (1.0f - Math::pow(x, MAX(p_shape.channel_shape, 0.1f)));
				r_alpha = 1.0f;
			} else {
				r_target = surface + p_shape.bank_height * MIN(edge / float(BANK_RISE), 1.0f);
				r_alpha = 1.0f - _spline_smoothstep(p_shape.falloff, edge);
			}
			paint_edge = edge - p_shape.paint_width;
		} break;
		case LandscapeSplineTerrainShape::PROFILE_LAKE: {
			const float shore = float(lateral);
			if (p_inside) {
				const float u = MIN(shore / MAX(p_shape.shore_width, 0.01f), 1.0f);
				r_target = p_shape.lake_level - p_shape.lake_depth * (1.0f - (1.0f - u) * (1.0f - u));
				r_alpha = 1.0f;
				paint_edge = -shore - p_shape.paint_width;
			} else {
				r_target = p_shape.lake_level + p_shape.bank_height * MIN(shore / float(BANK_RISE), 1.0f);
				r_alpha = 1.0f - _spline_smoothstep(p_shape.falloff, shore);
				paint_edge = shore - p_shape.paint_width;
			}
		} break;
	}
	r_alpha *= end_fade * p_shape.strength;
	if (!p_shape.modify_heights) {
		r_alpha = 0.0;
	}
	if (paint) {
		r_paint = (1.0f - _spline_smoothstep(p_shape.paint_falloff, paint_edge + noise)) * end_fade * p_shape.paint_strength;
	}
	return r_alpha > 0.0f || r_paint > 0.0f;
}

void LandscapeSplineTerrain::apply(const LandscapeSplineTerrainShape &p_shape, real_t p_spacing, const Rect2i &p_rect, float *r_heights, float *r_weights, int p_layer_count) {
	if (p_shape.is_empty() || !p_rect.has_area() || p_spacing <= 0.0) {
		return;
	}
	const bool paint = r_weights && p_shape.paint_layer >= 0 && p_shape.paint_layer < p_layer_count && p_shape.paint_strength > 0.0f;
	const bool heights = r_heights && p_shape.modify_heights;
	if (!paint && !heights) {
		return;
	}
	const Rect2 rect_meters(Vector2(p_rect.position) * p_spacing, Vector2(p_rect.size - Vector2i(1, 1)) * p_spacing);
	if (!rect_meters.grow(p_spacing).intersects(p_shape.bounds)) {
		return;
	}

	const int w = p_rect.size.x;
	const int h = p_rect.size.y;
	const int count = w * h;
	LocalVector<float> best_d2;
	LocalVector<int> best_segment;
	LocalVector<float> best_t;
	best_d2.resize(count);
	best_segment.resize(count);
	best_t.resize(count);
	for (int i = 0; i < count; i++) {
		best_d2[i] = Math::INF;
		best_segment[i] = -1;
		best_t[i] = 0.0;
	}

	// Closest segment of the sample polyline for every texel within the influence radius.
	const LocalVector<LandscapeSplineSample> &samples = p_shape.samples;
	const int segment_count = int(samples.size()) - 1;
	const real_t inv_spacing = 1.0 / p_spacing;
	for (int i = 0; i < segment_count; i++) {
		const Vector2 a(samples[i].position.x, samples[i].position.z);
		const Vector2 b(samples[i + 1].position.x, samples[i + 1].position.z);
		const real_t r = MAX(p_shape.radii[i], p_shape.radii[i + 1]);
		const int x0 = MAX(int(Math::floor((MIN(a.x, b.x) - r) * inv_spacing)), p_rect.position.x);
		const int x1 = MIN(int(Math::ceil((MAX(a.x, b.x) + r) * inv_spacing)), p_rect.get_end().x - 1);
		const int z0 = MAX(int(Math::floor((MIN(a.y, b.y) - r) * inv_spacing)), p_rect.position.y);
		const int z1 = MIN(int(Math::ceil((MAX(a.y, b.y) + r) * inv_spacing)), p_rect.get_end().y - 1);
		if (x0 > x1 || z0 > z1) {
			continue;
		}
		const Vector2 ab = b - a;
		const real_t len2 = ab.length_squared();
		const real_t inv_len2 = len2 > 0.0 ? 1.0 / len2 : 0.0;
		const float r2 = float(r * r);
		for (int z = z0; z <= z1; z++) {
			const real_t pz = z * p_spacing;
			const int row = (z - p_rect.position.y) * w - p_rect.position.x;
			for (int x = x0; x <= x1; x++) {
				const Vector2 p(x * p_spacing, pz);
				const real_t t = CLAMP((p - a).dot(ab) * inv_len2, real_t(0.0), real_t(1.0));
				const float d2 = float((a + ab * t).distance_squared_to(p));
				const int index = row + x;
				if (d2 <= r2 && d2 < best_d2[index]) {
					best_d2[index] = d2;
					best_segment[index] = i;
					best_t[index] = float(t);
				}
			}
		}
	}

	// Inside of lakes: even-odd rule on each texel row.
	LocalVector<uint8_t> inside;
	if (p_shape.profile == LandscapeSplineTerrainShape::PROFILE_LAKE) {
		inside.resize(count);
		memset(inside.ptr(), 0, count);
		LocalVector<real_t> crossings;
		for (int z = 0; z < h; z++) {
			const real_t pz = (p_rect.position.y + z) * p_spacing;
			crossings.clear();
			for (int i = 0; i < segment_count; i++) {
				const Vector3 &a = samples[i].position;
				const Vector3 &b = samples[i + 1].position;
				if ((a.z <= pz) != (b.z <= pz)) {
					crossings.push_back(a.x + (b.x - a.x) * (pz - a.z) / (b.z - a.z));
				}
			}
			if (crossings.size() < 2) {
				continue;
			}
			crossings.sort();
			for (uint32_t k = 0; k + 1 < crossings.size(); k += 2) {
				const int x0 = MAX(int(Math::ceil(crossings[k] * inv_spacing)), p_rect.position.x);
				const int x1 = MIN(int(Math::floor(crossings[k + 1] * inv_spacing)), p_rect.get_end().x - 1);
				for (int x = x0; x <= x1; x++) {
					inside[z * w + (x - p_rect.position.x)] = 1;
				}
			}
		}
	}

	for (int z = 0; z < h; z++) {
		for (int x = 0; x < w; x++) {
			const int index = z * w + x;
			const bool is_inside = !inside.is_empty() && inside[index];
			if (best_segment[index] < 0 && !is_inside) {
				continue;
			}
			const Vector2 position((p_rect.position.x + x) * p_spacing, (p_rect.position.y + z) * p_spacing);
			float target;
			float alpha;
			float paint_alpha;
			if (!evaluate(p_shape, position, is_inside, best_segment[index], best_t[index], target, alpha, paint_alpha)) {
				continue;
			}
			if (heights && alpha > 0.0f) {
				float &height = r_heights[index];
				float result = Math::lerp(height, target, MIN(alpha, 1.0f));
				if (!p_shape.raise) {
					result = MIN(result, height);
				}
				if (!p_shape.lower) {
					result = MAX(result, height);
				}
				height = result;
			}
			if (paint && paint_alpha > 0.0f) {
				// Weight-blended layers: the painted layer takes its share from all the others.
				const float a = MIN(paint_alpha, 1.0f);
				float *weights = r_weights + int64_t(index) * p_layer_count;
				for (int l = 0; l < p_layer_count; l++) {
					weights[l] *= 1.0f - a;
				}
				weights[p_shape.paint_layer] += a;
			}
		}
	}
}
