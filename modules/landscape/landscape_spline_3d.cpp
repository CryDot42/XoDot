/**************************************************************************/
/*  landscape_spline_3d.cpp                                               */
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

#include "landscape_spline_3d.h"

#include "landscape_3d.h"
#include "landscape_spline_materials.h"

#include "core/config/engine.h"
#include "core/math/geometry_2d.h"
#include "core/math/random_pcg.h"
#include "core/math/triangle_mesh.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "scene/3d/camera_3d.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/world_3d.h"
#include "scene/resources/curve.h"
#include "scene/resources/mesh.h"
#include "servers/rendering/rendering_server.h"

static Ref<Material> default_materials[2]; // Road, water.

// Number of cells per side of a lake surface chunk.
static constexpr int LAKE_CHUNK_CELLS = 32;
// Maximum number of cells per side of a lake surface grid.
static constexpr int LAKE_MAX_CELLS = 2048;
// Rows of a ribbon chunk.
static constexpr int MAX_CHUNK_ROWS = 256;

/* Type */

void LandscapeSpline3D::set_type(SplineType p_type) {
	if (type == p_type) {
		return;
	}
	type = p_type;
	notify_property_list_changed();
	update_configuration_warnings();
	_changed();
}

void LandscapeSpline3D::apply_type_defaults() {
	// Typical dimensions and settings of each kind of spline (UE-like defaults).
	switch (type) {
		case TYPE_ROAD: {
			width = 6.0;
			depth = 0.0;
			flow_speed = 0.0;
			sample_spacing = 2.0;
			lateral_spacing = 3.0;
			skirt_depth = 0.3;
			terrain_priority = 30;
			terrain_falloff = 6.0;
			terrain_end_falloff = 6.0;
			terrain_offset = 0.05;
			paint_width = 1.0;
			paint_falloff = 2.0;
			paint_noise = 0.5;
			visibility_range = 0.0;
			cast_shadows = false;
			closed = false;
		} break;
		case TYPE_RIVER: {
			width = 12.0;
			depth = 2.5;
			flow_speed = 1.5;
			sample_spacing = 3.0;
			lateral_spacing = 3.0;
			water_overlap = 2.0;
			terrain_priority = 10;
			terrain_falloff = 8.0;
			terrain_end_falloff = 10.0;
			bank_height = 0.4;
			channel_shape = 2.0;
			paint_width = 2.0;
			paint_falloff = 3.0;
			paint_noise = 0.6;
			visibility_range = 0.0;
			cast_shadows = false;
			closed = false;
		} break;
		case TYPE_STREAM: {
			width = 3.0;
			depth = 0.7;
			flow_speed = 2.5;
			sample_spacing = 1.5;
			lateral_spacing = 1.0;
			water_overlap = 1.0;
			terrain_priority = 20;
			terrain_falloff = 3.0;
			terrain_end_falloff = 4.0;
			bank_height = 0.25;
			channel_shape = 1.5;
			paint_width = 1.0;
			paint_falloff = 1.5;
			paint_noise = 0.7;
			visibility_range = 500.0;
			cast_shadows = false;
			closed = false;
		} break;
		case TYPE_LAKE: {
			width = 0.0;
			depth = 5.0;
			flow_speed = 0.0;
			sample_spacing = 4.0;
			water_overlap = 3.0;
			terrain_priority = 0;
			terrain_falloff = 8.0;
			bank_height = 0.5;
			shore_width = 15.0;
			paint_width = 3.0;
			paint_falloff = 3.0;
			paint_noise = 0.6;
			visibility_range = 0.0;
			cast_shadows = false;
			closed = true;
		} break;
	}
	for (int i = 0; i < points.size(); i++) {
		point_widths.set(i, width);
		point_depths.set(i, depth);
		point_flow_speeds.set(i, flow_speed);
	}
	notify_property_list_changed();
	_changed();
}

/* Points */

void LandscapeSpline3D::_resize_point_arrays() {
	// Attributes of new points take the default values.
	const int count = points.size();
	auto resize = [count](PackedFloat32Array &r_array, float p_default) {
		const int old = r_array.size();
		r_array.resize(count);
		for (int i = old; i < count; i++) {
			r_array.set(i, p_default);
		}
	};
	resize(point_widths, width);
	resize(point_depths, depth);
	resize(point_tilts, 0.0);
	resize(point_flow_speeds, flow_speed);
}

void LandscapeSpline3D::set_points(const PackedVector3Array &p_points) {
	points = p_points;
	_resize_point_arrays();
	_points_changed();
}

void LandscapeSpline3D::set_point_widths(const PackedFloat32Array &p_widths) {
	point_widths = p_widths;
	_resize_point_arrays();
	_changed();
}

void LandscapeSpline3D::set_point_depths(const PackedFloat32Array &p_depths) {
	point_depths = p_depths;
	_resize_point_arrays();
	_changed();
}

void LandscapeSpline3D::set_point_tilts(const PackedFloat32Array &p_tilts) {
	point_tilts = p_tilts;
	_resize_point_arrays();
	_changed();
}

void LandscapeSpline3D::set_point_flow_speeds(const PackedFloat32Array &p_speeds) {
	point_flow_speeds = p_speeds;
	_resize_point_arrays();
	_changed();
}

void LandscapeSpline3D::add_point(const Vector3 &p_position, int p_index) {
	const int count = points.size();
	if (p_index < 0 || p_index > count) {
		p_index = count;
	}
	// A point inserted between two others takes their average attributes.
	float w = width;
	float d = depth;
	float t = 0.0;
	float s = flow_speed;
	if (count > 0) {
		const int a = CLAMP(p_index - 1, 0, count - 1);
		const int b = CLAMP(p_index, 0, count - 1);
		w = (point_widths[a] + point_widths[b]) * 0.5;
		d = (point_depths[a] + point_depths[b]) * 0.5;
		t = (point_tilts[a] + point_tilts[b]) * 0.5;
		s = (point_flow_speeds[a] + point_flow_speeds[b]) * 0.5;
	}
	points.insert(p_index, p_position);
	point_widths.insert(p_index, w);
	point_depths.insert(p_index, d);
	point_tilts.insert(p_index, t);
	point_flow_speeds.insert(p_index, s);
	_points_changed();
}

void LandscapeSpline3D::remove_point(int p_index) {
	ERR_FAIL_INDEX(p_index, points.size());
	points.remove_at(p_index);
	point_widths.remove_at(p_index);
	point_depths.remove_at(p_index);
	point_tilts.remove_at(p_index);
	point_flow_speeds.remove_at(p_index);
	_points_changed();
}

void LandscapeSpline3D::clear_points() {
	points.clear();
	point_widths.clear();
	point_depths.clear();
	point_tilts.clear();
	point_flow_speeds.clear();
	_points_changed();
}

void LandscapeSpline3D::set_point_position(int p_index, const Vector3 &p_position) {
	ERR_FAIL_INDEX(p_index, points.size());
	points.set(p_index, p_position);
	_changed();
}

Vector3 LandscapeSpline3D::get_point_position(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, points.size(), Vector3());
	return points[p_index];
}

void LandscapeSpline3D::set_point_width(int p_index, float p_width) {
	ERR_FAIL_INDEX(p_index, point_widths.size());
	point_widths.set(p_index, MAX(p_width, 0.05f));
	_changed();
}

float LandscapeSpline3D::get_point_width(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, point_widths.size(), 0.0);
	return point_widths[p_index];
}

void LandscapeSpline3D::set_point_depth(int p_index, float p_depth) {
	ERR_FAIL_INDEX(p_index, point_depths.size());
	point_depths.set(p_index, MAX(p_depth, 0.0f));
	_changed();
}

float LandscapeSpline3D::get_point_depth(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, point_depths.size(), 0.0);
	return point_depths[p_index];
}

void LandscapeSpline3D::set_point_tilt(int p_index, float p_tilt) {
	ERR_FAIL_INDEX(p_index, point_tilts.size());
	point_tilts.set(p_index, CLAMP(p_tilt, -1.2f, 1.2f));
	_changed();
}

float LandscapeSpline3D::get_point_tilt(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, point_tilts.size(), 0.0);
	return point_tilts[p_index];
}

void LandscapeSpline3D::set_point_flow_speed(int p_index, float p_speed) {
	ERR_FAIL_INDEX(p_index, point_flow_speeds.size());
	point_flow_speeds.set(p_index, p_speed);
	_changed();
}

float LandscapeSpline3D::get_point_flow_speed(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, point_flow_speeds.size(), 0.0);
	return point_flow_speeds[p_index];
}

void LandscapeSpline3D::set_closed(bool p_closed) {
	closed = p_closed;
	update_configuration_warnings();
	_changed();
}

void LandscapeSpline3D::set_width(float p_width) {
	width = MAX(p_width, 0.0f);
}

void LandscapeSpline3D::set_depth(float p_depth) {
	depth = MAX(p_depth, 0.0f);
	if (type == TYPE_LAKE) {
		_changed(); // The lake basin depth.
	}
}

void LandscapeSpline3D::set_flow_speed(float p_speed) {
	flow_speed = p_speed;
}

void LandscapeSpline3D::set_spline_id(int64_t p_id) {
	if (spline_id == p_id) {
		return;
	}
	spline_id = p_id;
	if (landscape) {
		landscape->_spline_changed(this);
	}
}

void LandscapeSpline3D::regenerate_spline_id() {
	RandomPCG rng(uint64_t(OS::get_singleton()->get_ticks_usec()) ^ uint64_t(get_instance_id()));
	rng.randomize();
	int64_t id = 0;
	while (id == 0) {
		id = int64_t(((uint64_t(rng.rand()) << 32) | uint64_t(rng.rand())) & 0x7FFFFFFFFFFFFFFFull);
	}
	spline_id = id;
}

void LandscapeSpline3D::_points_changed() {
	update_configuration_warnings();
	_changed();
}

/* Change tracking */

void LandscapeSpline3D::_changed(bool p_shape) {
	if (p_shape) {
		curve_dirty = true;
	}
	chunks_dirty = true;
	terrain_shape_dirty = true;
	if (landscape) {
		landscape->_spline_changed(this);
	}
	update_gizmos();
}

void LandscapeSpline3D::_terrain_invalidated() {
	terrain_shape_dirty = true;
}

/* Curve */

void LandscapeSpline3D::_build_points(LocalVector<LandscapeSplinePoint> &r_points) const {
	r_points.resize(points.size());
	for (int i = 0; i < points.size(); i++) {
		LandscapeSplinePoint &p = r_points[i];
		p.position = points[i];
		if (type == TYPE_LAKE) {
			p.position.y = 0.0; // Lakes are flat, at the height of the node.
		}
		p.width = i < point_widths.size() ? point_widths[i] : width;
		p.depth = i < point_depths.size() ? point_depths[i] : depth;
		p.tilt = (type == TYPE_ROAD && i < point_tilts.size()) ? point_tilts[i] : 0.0f;
		p.speed = i < point_flow_speeds.size() ? point_flow_speeds[i] : flow_speed;
	}
}

void LandscapeSpline3D::_update_curve() {
	if (!curve_dirty) {
		return;
	}
	curve_dirty = false;
	LocalVector<LandscapeSplinePoint> curve_points;
	_build_points(curve_points);
	LandscapeSplineCurve::Settings settings;
	settings.max_step = MAX(sample_spacing, 0.1f);
	LandscapeSplineCurve::tessellate(curve_points, is_closed(), settings, samples, &segment_starts);
}

const LocalVector<LandscapeSplineSample> &LandscapeSpline3D::get_samples() {
	_update_curve();
	return samples;
}

LandscapeSplineSample LandscapeSpline3D::get_point_frame(int p_index) {
	_update_curve();
	LandscapeSplineSample frame;
	ERR_FAIL_INDEX_V(p_index, points.size(), frame);
	if (samples.is_empty()) {
		frame.position = points[p_index];
		frame.forward = Vector3(0, 0, 1);
		frame.right = Vector3(-1, 0, 0);
		frame.up = Vector3(0, 1, 0);
		return frame;
	}
	// Control point i starts segment i. The last point of an open curve is the last sample.
	if (p_index < int(segment_starts.size()) - 1) {
		return samples[segment_starts[p_index]];
	}
	return samples[samples.size() - 1];
}

real_t LandscapeSpline3D::get_length() {
	_update_curve();
	return samples.is_empty() ? 0.0 : samples[samples.size() - 1].distance;
}

Transform3D LandscapeSpline3D::sample_transform(real_t p_distance) {
	_update_curve();
	if (samples.is_empty()) {
		return Transform3D();
	}
	if (samples.size() == 1) {
		return Transform3D(Basis(), samples[0].position);
	}
	const real_t distance = CLAMP(p_distance, real_t(0.0), samples[samples.size() - 1].distance);
	// Binary search of the segment holding the distance.
	int lo = 0;
	int hi = samples.size() - 2;
	while (lo < hi) {
		const int mid = (lo + hi + 1) / 2;
		if (samples[mid].distance <= distance) {
			lo = mid;
		} else {
			hi = mid - 1;
		}
	}
	const real_t span = samples[lo + 1].distance - samples[lo].distance;
	const real_t t = span > CMP_EPSILON ? (distance - samples[lo].distance) / span : 0.0;
	const LandscapeSplineSample s = LandscapeSplineCurve::interpolate(samples, lo, t);
	// Basis: X right, Y up, -Z forward (like a Camera3D or PathFollow3D looking along the spline).
	return Transform3D(Basis(s.right, s.up, -s.forward), s.position);
}

Vector3 LandscapeSpline3D::sample_position(real_t p_distance) {
	return sample_transform(p_distance).origin;
}

int LandscapeSpline3D::get_closest_segment(const Vector3 &p_local_position, real_t *r_t) {
	_update_curve();
	int segment;
	real_t t;
	real_t distance;
	if (!LandscapeSplineCurve::get_closest(samples, p_local_position, segment, t, distance)) {
		return -1;
	}
	if (r_t) {
		*r_t = t;
	}
	return segment;
}

real_t LandscapeSpline3D::get_closest_distance(const Vector3 &p_local_position) {
	real_t t = 0.0;
	const int segment = get_closest_segment(p_local_position, &t);
	if (segment < 0) {
		return 0.0;
	}
	return Math::lerp(samples[segment].distance, samples[segment + 1].distance, t);
}

PackedVector3Array LandscapeSpline3D::get_tessellated_points() {
	_update_curve();
	PackedVector3Array result;
	result.resize(samples.size());
	for (uint32_t i = 0; i < samples.size(); i++) {
		result.set(i, samples[i].position);
	}
	return result;
}

Ref<Curve3D> LandscapeSpline3D::create_curve() {
	// Exact Bezier form of the spline: each Catmull-Rom segment is a cubic Hermite curve.
	Ref<Curve3D> curve;
	curve.instantiate();
	LocalVector<LandscapeSplinePoint> curve_points;
	_build_points(curve_points);
	const bool is_curve_closed = is_closed();
	const int segments = LandscapeSplineCurve::get_segment_count(curve_points.size(), is_curve_closed);
	if (segments == 0) {
		return curve;
	}
	const int n = curve_points.size();
	LocalVector<Vector3> in_handles;
	LocalVector<Vector3> out_handles;
	in_handles.resize(n);
	out_handles.resize(n);
	for (int i = 0; i < n; i++) {
		in_handles[i] = Vector3();
		out_handles[i] = Vector3();
	}
	for (int s = 0; s < segments; s++) {
		Vector3 p0;
		Vector3 d0;
		Vector3 p1;
		Vector3 d1;
		LandscapeSplineCurve::evaluate(curve_points, is_curve_closed, s, 0.0, p0, d0);
		LandscapeSplineCurve::evaluate(curve_points, is_curve_closed, s, 1.0, p1, d1);
		out_handles[s] = d0 / 3.0;
		in_handles[(s + 1) % n] = -d1 / 3.0;
	}
	for (int i = 0; i < n; i++) {
		curve->add_point(curve_points[i].position, in_handles[i], out_handles[i]);
		curve->set_point_tilt(i, curve_points[i].tilt);
	}
	curve->set_closed(is_curve_closed);
	return curve;
}

/* Terrain */

Landscape3D *LandscapeSpline3D::_find_landscape() const {
	Node *parent = get_parent();
	while (parent) {
		Landscape3D *found = Object::cast_to<Landscape3D>(parent);
		if (found) {
			return found;
		}
		parent = parent->get_parent();
	}
	return nullptr;
}

Transform3D LandscapeSpline3D::_get_terrain_transform() const {
	if (!landscape || !is_inside_tree() || !landscape->is_inside_tree()) {
		return Transform3D();
	}
	return landscape->get_global_transform().affine_inverse() * get_global_transform();
}

uint64_t LandscapeSpline3D::_compute_settings_hash(real_t p_spacing) const {
	// Bump the version when the terrain rasterization changes: splines are applied again.
	static constexpr uint64_t TERRAIN_VERSION = 1;
	uint64_t h = hash_murmur3_one_64(TERRAIN_VERSION);
	auto add = [&h](double p_value) {
		h = hash_murmur3_one_64(uint64_t(int64_t(Math::round(p_value * 10000.0))), h);
	};
	add(int(type));
	add(is_closed());
	add(p_spacing);
	add(terrain_priority);
	add(terrain_strength);
	add(terrain_falloff);
	add(terrain_end_falloff);
	add(terrain_offset);
	add(terrain_raise);
	add(terrain_lower);
	add(bank_height);
	add(channel_shape);
	add(shore_width);
	const bool paint_valid = landscape && paint_layer >= 0 && paint_layer < landscape->get_layer_count();
	add(paint_valid ? paint_layer : -1);
	add(paint_width);
	add(paint_falloff);
	add(paint_noise);
	add(paint_strength);
	if (type == TYPE_LAKE) {
		add(depth);
		add(terrain_transform.origin.y);
	}
	add(double(spline_id & 0xFFFFFF)); // Noise seed of the paint.
	return h;
}

const LandscapeSplineTerrainShape &LandscapeSpline3D::get_terrain_shape() {
	if (!terrain_shape_dirty) {
		return terrain_shape;
	}
	terrain_shape_dirty = false;
	terrain_shape = LandscapeSplineTerrainShape();
	if (!landscape || !terrain_enabled || !is_inside_tree()) {
		return terrain_shape;
	}
	const Ref<LandscapeData> landscape_data = landscape->get_data();
	if (landscape_data.is_null() || !landscape_data->is_valid()) {
		return terrain_shape;
	}
	_update_curve();
	if (samples.size() < 2) {
		return terrain_shape;
	}

	const Transform3D xform = _get_terrain_transform();
	terrain_transform = xform;
	// Widths follow the horizontal scale, depths the vertical scale.
	const real_t lateral_scale = (xform.basis.get_column(0).length() + xform.basis.get_column(2).length()) * 0.5;
	const real_t vertical_scale = xform.basis.get_column(1).length();
	const real_t level = xform.origin.y;

	LandscapeSplineTerrainShape &shape = terrain_shape;
	shape.profile = type == TYPE_ROAD ? LandscapeSplineTerrainShape::PROFILE_ROAD : (type == TYPE_LAKE ? LandscapeSplineTerrainShape::PROFILE_LAKE : LandscapeSplineTerrainShape::PROFILE_CHANNEL);
	shape.closed = is_closed();
	shape.samples.resize(samples.size());
	for (uint32_t i = 0; i < samples.size(); i++) {
		LandscapeSplineSample s = samples[i];
		s.position = xform.xform(s.position);
		if (type == TYPE_LAKE) {
			s.position.y = level;
		}
		s.forward = xform.basis.xform(s.forward).normalized();
		s.right = xform.basis.xform(s.right).normalized();
		s.up = xform.basis.xform(s.up).normalized();
		s.width *= lateral_scale;
		s.depth *= vertical_scale;
		shape.samples[i] = s;
	}
	shape.modify_heights = true;
	shape.raise = terrain_raise;
	shape.lower = terrain_lower;
	shape.strength = terrain_strength;
	shape.falloff = terrain_falloff * lateral_scale;
	shape.end_falloff = terrain_end_falloff * lateral_scale;
	shape.offset = terrain_offset * vertical_scale;
	shape.bank_height = bank_height * vertical_scale;
	shape.channel_shape = channel_shape;
	shape.shore_width = shore_width * lateral_scale;
	shape.lake_level = level;
	shape.lake_depth = depth * vertical_scale;
	shape.paint_layer = (paint_layer >= 0 && paint_layer < landscape->get_layer_count()) ? paint_layer : -1;
	shape.paint_width = paint_width * lateral_scale;
	shape.paint_falloff = paint_falloff * lateral_scale;
	shape.paint_noise = paint_noise;
	shape.paint_strength = paint_strength;
	shape.noise_seed = uint32_t(spline_id & 0xFFFFFF);
	shape.finalize(segment_starts, landscape_data->get_vertex_spacing(), landscape_data->get_size(), _compute_settings_hash(landscape_data->get_vertex_spacing()));
	return terrain_shape;
}

void LandscapeSpline3D::snap_to_terrain(float p_offset) {
	ERR_FAIL_NULL_MSG(landscape, "LandscapeSpline3D must be a descendant of a Landscape3D to snap to its terrain.");
	const Ref<LandscapeData> landscape_data = landscape->get_data();
	ERR_FAIL_COND(landscape_data.is_null() || !landscape_data->is_valid());
	if (type == TYPE_LAKE) {
		level_lake_to_shore(p_offset);
		return;
	}
	const Transform3D to_landscape = _get_terrain_transform();
	const Transform3D from_landscape = to_landscape.affine_inverse();
	for (int i = 0; i < points.size(); i++) {
		Vector3 p = to_landscape.xform(points[i]);
		// The terrain without splines: snapping doesn't depend on the current deformation.
		p.y = landscape_data->sample_edit_height(p.x, p.z) + p_offset;
		points.set(i, from_landscape.xform(p));
	}
	_changed();
}

void LandscapeSpline3D::level_lake_to_shore(float p_margin) {
	ERR_FAIL_NULL_MSG(landscape, "LandscapeSpline3D must be a descendant of a Landscape3D to level it with its terrain.");
	const Ref<LandscapeData> landscape_data = landscape->get_data();
	ERR_FAIL_COND(landscape_data.is_null() || !landscape_data->is_valid());
	_update_curve();
	if (samples.size() < 2) {
		return;
	}
	// The lowest point of the shore: the water can't overflow the basin.
	const Transform3D to_landscape = _get_terrain_transform();
	real_t lowest = Math::INF;
	for (const LandscapeSplineSample &sample : samples) {
		const Vector3 p = to_landscape.xform(Vector3(sample.position.x, 0.0, sample.position.z));
		lowest = MIN(lowest, real_t(landscape_data->sample_edit_height(p.x, p.z)));
	}
	Vector3 origin = to_landscape.origin;
	origin.y = lowest - p_margin;
	set_global_position(landscape->get_global_transform().xform(origin));
}

void LandscapeSpline3D::make_downhill(float p_min_drop) {
	if (type == TYPE_ROAD || type == TYPE_LAKE || points.size() < 2) {
		return;
	}
	// Water flows from the first point to the last one: each point is at most as high as the previous one.
	for (int i = 1; i < points.size(); i++) {
		Vector3 p = points[i];
		const Vector3 prev = points[i - 1];
		const real_t drop = Vector2(p.x - prev.x, p.z - prev.z).length() * MAX(p_min_drop, 0.0f);
		p.y = MIN(p.y, prev.y - drop);
		points.set(i, p);
	}
	_changed();
}

void LandscapeSpline3D::reverse() {
	points.reverse();
	point_widths.reverse();
	point_depths.reverse();
	point_tilts.reverse();
	for (int i = 0; i < point_tilts.size(); i++) {
		point_tilts.set(i, -point_tilts[i]); // Banking follows the direction.
	}
	point_flow_speeds.reverse();
	_changed();
}

/* Queries */

Dictionary LandscapeSpline3D::get_surface_info(const Vector3 &p_global_position) {
	Dictionary info;
	_update_curve();
	if (samples.size() < 2 || !is_inside_tree()) {
		return info;
	}
	const Transform3D global = get_global_transform();
	const Vector3 local = global.affine_inverse().xform(p_global_position);
	int segment;
	real_t t;
	real_t shore_distance;
	LandscapeSplineCurve::get_closest(samples, local, segment, t, shore_distance);
	const LandscapeSplineSample s = LandscapeSplineCurve::interpolate(samples, segment, t);
	Vector2 forward(s.forward.x, s.forward.z);
	forward = forward.length_squared() > CMP_EPSILON2 ? forward.normalized() : Vector2(0, 1);
	const Vector2 right(-forward.y, forward.x);
	const Vector2 delta(local.x - s.position.x, local.z - s.position.z);

	bool inside = false;
	real_t lateral = delta.dot(right);
	Vector3 surface;
	Vector3 normal = s.up;
	if (type == TYPE_LAKE) {
		Vector<Vector2> polygon;
		for (uint32_t i = 0; i + 1 < samples.size(); i++) {
			polygon.push_back(Vector2(samples[i].position.x, samples[i].position.z));
		}
		inside = Geometry2D::is_point_in_polygon(Vector2(local.x, local.z), polygon);
		lateral = inside ? -shore_distance : shore_distance;
		surface = Vector3(local.x, 0.0, local.z);
		normal = Vector3(0, 1, 0);
	} else {
		bool beyond_end = false;
		if (!is_closed()) {
			const real_t along = delta.dot(forward);
			beyond_end = (segment == 0 && t <= 0.0 && along < 0.0) || (segment == int(samples.size()) - 2 && t >= 1.0 && along > 0.0);
		}
		inside = !beyond_end && Math::abs(lateral) <= s.width * 0.5;
		const real_t bank = type == TYPE_ROAD ? Math::tan(real_t(s.tilt)) : 0.0;
		surface = Vector3(local.x, s.position.y + lateral * bank, local.z);
		if (type != TYPE_ROAD) {
			normal = Vector3(0, 1, 0);
		}
	}
	info["inside"] = inside;
	info["position"] = global.xform(surface);
	info["height"] = global.xform(surface).y;
	info["normal"] = global.basis.xform(normal).normalized();
	info["distance"] = s.distance;
	info["lateral"] = lateral;
	info["width"] = type == TYPE_LAKE ? 0.0f : s.width;
	info["depth"] = type == TYPE_LAKE ? depth : s.depth;
	info["flow"] = (type == TYPE_RIVER || type == TYPE_STREAM) ? global.basis.xform(s.forward * s.speed) : Vector3();
	return info;
}

bool LandscapeSpline3D::is_point_in_water(const Vector3 &p_global_position) {
	if (!_is_water()) {
		return false;
	}
	const Dictionary info = get_surface_info(p_global_position);
	return !info.is_empty() && bool(info["inside"]) && p_global_position.y <= real_t(info["height"]);
}

Ref<TriangleMesh> LandscapeSpline3D::generate_selection_mesh() {
	_update_curve();
	Vector<Vector3> faces;
	if (samples.size() >= 2) {
		if (type == TYPE_LAKE) {
			Vector<Vector2> polygon;
			for (uint32_t i = 0; i + 1 < samples.size(); i++) {
				polygon.push_back(Vector2(samples[i].position.x, samples[i].position.z));
			}
			const Vector<int> indices = Geometry2D::triangulate_polygon(polygon);
			for (int i = 0; i < indices.size(); i++) {
				const Vector2 p = polygon[indices[i]];
				faces.push_back(Vector3(p.x, 0.0, p.y));
			}
		} else {
			for (uint32_t i = 0; i + 1 < samples.size(); i++) {
				const LandscapeSplineSample &a = samples[i];
				const LandscapeSplineSample &b = samples[i + 1];
				const Vector3 al = a.position - a.right * (a.width * 0.5);
				const Vector3 ar = a.position + a.right * (a.width * 0.5);
				const Vector3 bl = b.position - b.right * (b.width * 0.5);
				const Vector3 br = b.position + b.right * (b.width * 0.5);
				faces.push_back(al);
				faces.push_back(bl);
				faces.push_back(ar);
				faces.push_back(ar);
				faces.push_back(bl);
				faces.push_back(br);
			}
		}
	}
	Ref<TriangleMesh> mesh;
	if (!faces.is_empty()) {
		mesh.instantiate();
		mesh->create(faces);
	}
	return mesh;
}

/* Mesh chunks */

static _FORCE_INLINE_ float _spline_smoothstep01(real_t p_x) {
	const float t = CLAMP(float(p_x), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

void LandscapeSpline3D::_free_chunk(Chunk &r_chunk) {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (r_chunk.instance.is_valid()) {
		rs->free_rid(r_chunk.instance);
		r_chunk.instance = RID();
	}
	if (r_chunk.mesh.is_valid()) {
		rs->free_rid(r_chunk.mesh);
		r_chunk.mesh = RID();
	}
	r_chunk.built = false;
	r_chunk.vertex_count = 0;
	r_chunk.triangle_count = 0;
}

void LandscapeSpline3D::_clear_chunks() {
	for (Chunk &chunk : chunks) {
		_free_chunk(chunk);
	}
	chunks.clear();
}

void LandscapeSpline3D::_rebuild_chunks() {
	chunks_dirty = false;
	_clear_chunks();
	lake_mask.clear();
	_update_curve();
	if (!mesh_enabled || samples.size() < 2) {
		return;
	}

	if (type == TYPE_LAKE) {
		// Regular grid covering the lake and the ground under its banks (hidden by the terrain),
		// split in square chunks. Only the cells with water are meshed.
		Vector<Vector2> polygon;
		for (uint32_t i = 0; i + 1 < samples.size(); i++) {
			polygon.push_back(Vector2(samples[i].position.x, samples[i].position.z));
		}
		if (polygon.size() < 3) {
			return;
		}
		Rect2 bounds(polygon[0], Vector2());
		for (const Vector2 &p : polygon) {
			bounds.expand_to(p);
		}
		lake_cell = MAX(sample_spacing * 2.0f, 0.25f);
		bounds = bounds.grow(water_overlap + lake_cell);
		while (bounds.size.x / lake_cell > LAKE_MAX_CELLS || bounds.size.y / lake_cell > LAKE_MAX_CELLS) {
			lake_cell *= 2.0;
		}
		lake_origin = bounds.position;
		lake_cells = Vector2i(MAX(int(Math::ceil(bounds.size.x / lake_cell)), 1), MAX(int(Math::ceil(bounds.size.y / lake_cell)), 1));
		lake_mask.resize(lake_cells.x * lake_cells.y);
		memset(lake_mask.ptr(), 0, lake_mask.size());

		// Cells whose center is inside the shoreline (even-odd rule).
		LocalVector<real_t> crossings;
		for (int cz = 0; cz < lake_cells.y; cz++) {
			const real_t pz = lake_origin.y + (cz + 0.5) * lake_cell;
			crossings.clear();
			for (int i = 0; i < polygon.size(); i++) {
				const Vector2 a = polygon[i];
				const Vector2 b = polygon[(i + 1) % polygon.size()];
				if ((a.y <= pz) != (b.y <= pz)) {
					crossings.push_back(a.x + (b.x - a.x) * (pz - a.y) / (b.y - a.y));
				}
			}
			crossings.sort();
			for (uint32_t k = 0; k + 1 < crossings.size(); k += 2) {
				const int c0 = MAX(int(Math::ceil((crossings[k] - lake_origin.x) / lake_cell - 0.5)), 0);
				const int c1 = MIN(int(Math::floor((crossings[k + 1] - lake_origin.x) / lake_cell - 0.5)), lake_cells.x - 1);
				for (int cx = c0; cx <= c1; cx++) {
					lake_mask[cz * lake_cells.x + cx] = 1;
				}
			}
		}
		// Cells close to the shore: the surface continues a bit under the banks.
		const real_t reach = water_overlap + lake_cell * 0.75;
		for (int i = 0; i < polygon.size(); i++) {
			const Vector2 a = polygon[i];
			const Vector2 b = polygon[(i + 1) % polygon.size()];
			const int x0 = MAX(int(Math::floor((MIN(a.x, b.x) - reach - lake_origin.x) / lake_cell)), 0);
			const int x1 = MIN(int(Math::ceil((MAX(a.x, b.x) + reach - lake_origin.x) / lake_cell)), lake_cells.x - 1);
			const int z0 = MAX(int(Math::floor((MIN(a.y, b.y) - reach - lake_origin.y) / lake_cell)), 0);
			const int z1 = MIN(int(Math::ceil((MAX(a.y, b.y) + reach - lake_origin.y) / lake_cell)), lake_cells.y - 1);
			for (int cz = z0; cz <= z1; cz++) {
				for (int cx = x0; cx <= x1; cx++) {
					const Vector2 center = lake_origin + Vector2(cx + 0.5, cz + 0.5) * lake_cell;
					if (Geometry2D::get_distance_to_segment(center, a, b) <= reach) {
						lake_mask[cz * lake_cells.x + cx] = 1;
					}
				}
			}
		}

		for (int bz = 0; bz < lake_cells.y; bz += LAKE_CHUNK_CELLS) {
			for (int bx = 0; bx < lake_cells.x; bx += LAKE_CHUNK_CELLS) {
				const Rect2i cells(bx, bz, MIN(LAKE_CHUNK_CELLS, lake_cells.x - bx), MIN(LAKE_CHUNK_CELLS, lake_cells.y - bz));
				bool any = false;
				for (int cz = cells.position.y; cz < cells.get_end().y && !any; cz++) {
					for (int cx = cells.position.x; cx < cells.get_end().x && !any; cx++) {
						any = lake_mask[cz * lake_cells.x + cx] != 0;
					}
				}
				if (!any) {
					continue;
				}
				Chunk chunk;
				chunk.cells = cells;
				const Vector2 origin = lake_origin + Vector2(cells.position) * lake_cell;
				chunk.aabb = AABB(Vector3(origin.x, -0.5, origin.y), Vector3(cells.size.x * lake_cell, 1.0, cells.size.y * lake_cell));
				chunks.push_back(chunk);
			}
		}
		return;
	}

	// Ribbons: chunks of consecutive cross sections, sharing their boundary cross section.
	const bool water = _is_water();
	float max_width = 0.0;
	for (const LandscapeSplineSample &sample : samples) {
		max_width = MAX(max_width, sample.width);
	}
	const float extra = water ? water_overlap * 2.0f : 0.0f;
	lateral_vertices = CLAMP(int(Math::ceil((max_width + extra) / MAX(lateral_spacing, 0.1f))), water ? 2 : 1, 64);
	const int count = samples.size();
	int first = 0;
	while (first < count - 1) {
		int last = first + 1;
		const real_t start = samples[first].distance;
		while (last < count - 1 && last - first < MAX_CHUNK_ROWS - 1 && samples[last].distance - start < chunk_length) {
			last++;
		}
		Chunk chunk;
		chunk.first = first;
		chunk.last = last;
		bool has_aabb = false;
		for (int i = first; i <= last; i++) {
			const LandscapeSplineSample &s = samples[i];
			const real_t half = s.width * 0.5 + (water ? water_overlap : 0.0f);
			const Vector3 side = s.right * half;
			for (const Vector3 &p : { s.position - side, s.position + side }) {
				if (!has_aabb) {
					chunk.aabb = AABB(p, Vector3());
					has_aabb = true;
				} else {
					chunk.aabb.expand_to(p);
				}
			}
			if (!water && skirt_depth > 0.0) {
				chunk.aabb.expand_to(s.position - side - Vector3(0, skirt_depth, 0));
				chunk.aabb.expand_to(s.position + side - Vector3(0, skirt_depth, 0));
			}
		}
		chunk.aabb = chunk.aabb.grow(0.1);
		chunks.push_back(chunk);
		first = last;
	}
}

void LandscapeSpline3D::_build_ribbon_chunk(Chunk &r_chunk, Array &r_arrays, Dictionary &r_lods) const {
	const bool water = _is_water();
	const bool skirts = !water && skirt_depth > 0.0;
	const int n = lateral_vertices;
	const int surface_vertices = n + 1;
	const int row_vertices = surface_vertices + (skirts ? 4 : 0);
	const int rows = r_chunk.last - r_chunk.first + 1;
	const int vertex_count = rows * row_vertices;
	const int sample_count = samples.size();
	const real_t total_length = samples[sample_count - 1].distance;

	PackedVector3Array vertices;
	PackedVector3Array normals;
	PackedFloat32Array tangents;
	PackedVector2Array uvs;
	PackedVector2Array uv2s;
	PackedColorArray colors;
	vertices.resize(vertex_count);
	normals.resize(vertex_count);
	tangents.resize(vertex_count * 4);
	uvs.resize(vertex_count);
	uv2s.resize(vertex_count);
	colors.resize(vertex_count);
	Vector3 *vw = vertices.ptrw();
	Vector3 *nw = normals.ptrw();
	float *tw = tangents.ptrw();
	Vector2 *uw = uvs.ptrw();
	Vector2 *u2w = uv2s.ptrw();
	Color *cw = colors.ptrw();
	auto set_tangent = [tw](int p_index, const Vector3 &p_tangent, float p_sign) {
		tw[p_index * 4 + 0] = p_tangent.x;
		tw[p_index * 4 + 1] = p_tangent.y;
		tw[p_index * 4 + 2] = p_tangent.z;
		tw[p_index * 4 + 3] = p_sign;
	};

	for (int r = 0; r < rows; r++) {
		const int index = r_chunk.first + r;
		const LandscapeSplineSample &s = samples[index];
		const int base = r * row_vertices;
		const real_t half = s.width * 0.5;
		Vector3 forward_h(s.forward.x, 0.0, s.forward.z);
		forward_h = forward_h.length_squared() > CMP_EPSILON2 ? forward_h.normalized() : Vector3(0, 0, 1);
		const Vector3 right_h = forward_h.cross(Vector3(0, 1, 0)).normalized();

		if (water) {
			// Flat water surface at the height of the spline, extending under the banks.
			const real_t extent = half + water_overlap;
			// Turbulence from the slope of the surface (rapids).
			const int prev = MAX(index - 1, 0);
			const int next = MIN(index + 1, sample_count - 1);
			const real_t run = samples[next].distance - samples[prev].distance;
			const real_t drop = run > CMP_EPSILON ? (samples[prev].position.y - samples[next].position.y) / run : 0.0;
			const float turbulence = _spline_smoothstep01((drop - 0.01) / 0.07);
			// Open rivers fade in and out at their ends.
			float fade = 1.0;
			if (!is_closed()) {
				const real_t fade_length = MAX(real_t(s.width), real_t(2.0));
				fade = _spline_smoothstep01(s.distance / fade_length) * _spline_smoothstep01((total_length - s.distance) / fade_length);
			}
			for (int j = 0; j <= n; j++) {
				const real_t lateral = -extent + extent * 2.0 * real_t(j) / real_t(n);
				const int v = base + j;
				vw[v] = s.position + right_h * lateral;
				nw[v] = Vector3(0, 1, 0);
				// U along the flow, V to the right: the binormal cross(normal, tangent) * w must be dP/dV.
				set_tangent(v, forward_h, -1.0);
				uw[v] = Vector2(s.distance, lateral);
				u2w[v] = Vector2(s.speed, 0.0);
				cw[v] = Color(turbulence, float(Math::abs(lateral) / MAX(half, real_t(0.01))), 0.0, fade);
			}
		} else {
			for (int j = 0; j <= n; j++) {
				const real_t lateral = -half + half * 2.0 * real_t(j) / real_t(n);
				const int v = base + j;
				vw[v] = s.position + s.right * lateral;
				nw[v] = s.up;
				// U across (left to right), V along the road.
				set_tangent(v, s.right, 1.0);
				uw[v] = Vector2(real_t(j) / real_t(n), s.distance);
				u2w[v] = Vector2(lateral, half);
				cw[v] = Color(1, 1, 1, 1);
			}
			if (skirts) {
				// Vertical skirts along the edges, hidden in the terrain (no gap with the road bed falloff).
				const Vector3 down(0, skirt_depth, 0);
				const Vector3 left = s.position - s.right * half;
				const Vector3 right = s.position + s.right * half;
				const Vector3 edges[4] = { left, left - down, right, right - down };
				for (int k = 0; k < 4; k++) {
					const int v = base + surface_vertices + k;
					vw[v] = edges[k];
					nw[v] = k < 2 ? -right_h : right_h;
					set_tangent(v, forward_h, 1.0);
					uw[v] = Vector2(k < 2 ? 0.0 : 1.0, s.distance);
					u2w[v] = Vector2(k < 2 ? -half : half, half);
					cw[v] = Color(1, 1, 1, (k & 1) ? 0.0 : 1.0);
				}
			}
		}
	}

	// Front faces are clockwise.
	auto emit_rows = [&](const LocalVector<int> &p_rows, PackedInt32Array &r_indices) {
		const int quads = (p_rows.size() - 1) * (n + (skirts ? 2 : 0));
		r_indices.resize(quads * 6);
		int32_t *iw = r_indices.ptrw();
		int idx = 0;
		for (uint32_t k = 0; k + 1 < p_rows.size(); k++) {
			const int a = p_rows[k] * row_vertices;
			const int b = p_rows[k + 1] * row_vertices;
			for (int j = 0; j < n; j++) {
				const int va = a + j;
				const int vb = va + 1;
				const int vc = b + j;
				const int vd = vc + 1;
				iw[idx++] = va;
				iw[idx++] = vc;
				iw[idx++] = vb;
				iw[idx++] = vb;
				iw[idx++] = vc;
				iw[idx++] = vd;
			}
			if (skirts) {
				// Left skirt faces left, right skirt faces right.
				const int la = a + surface_vertices;
				const int lb = b + surface_vertices;
				iw[idx++] = la;
				iw[idx++] = la + 1;
				iw[idx++] = lb;
				iw[idx++] = lb;
				iw[idx++] = la + 1;
				iw[idx++] = lb + 1;
				const int ra = a + surface_vertices + 2;
				const int rb = b + surface_vertices + 2;
				iw[idx++] = ra;
				iw[idx++] = rb;
				iw[idx++] = ra + 1;
				iw[idx++] = rb;
				iw[idx++] = rb + 1;
				iw[idx++] = ra + 1;
			}
		}
	};

	LocalVector<int> all_rows;
	for (int r = 0; r < rows; r++) {
		all_rows.push_back(r);
	}
	PackedInt32Array indices;
	emit_rows(all_rows, indices);

	// LODs: every 2nd, 4th, 8th cross section. The error of a LOD is the largest distance between
	// a dropped cross section and the coarse surface (edges included), in meters.
	real_t last_error = 0.0;
	for (int level = 1; level <= 3; level++) {
		const int step = 1 << level;
		if (rows <= step) {
			break;
		}
		LocalVector<int> kept;
		for (int r = 0; r < rows - 1; r += step) {
			kept.push_back(r);
		}
		kept.push_back(rows - 1);
		real_t error = 0.0;
		for (uint32_t k = 0; k + 1 < kept.size(); k++) {
			const LandscapeSplineSample &sa = samples[r_chunk.first + kept[k]];
			const LandscapeSplineSample &sb = samples[r_chunk.first + kept[k + 1]];
			for (int r = kept[k] + 1; r < kept[k + 1]; r++) {
				const LandscapeSplineSample &sr = samples[r_chunk.first + r];
				const real_t span = sb.distance - sa.distance;
				const real_t f = span > CMP_EPSILON ? (sr.distance - sa.distance) / span : 0.0;
				for (int side = -1; side <= 1; side++) {
					const Vector3 pa = sa.position + sa.right * (sa.width * 0.5 * side);
					const Vector3 pb = sb.position + sb.right * (sb.width * 0.5 * side);
					const Vector3 pr = sr.position + sr.right * (sr.width * 0.5 * side);
					error = MAX(error, pr.distance_to(pa.lerp(pb, f)));
				}
			}
		}
		// LOD errors must increase.
		error = MAX(error, last_error + 0.001);
		PackedInt32Array lod;
		emit_rows(kept, lod);
		r_lods[error] = lod;
		last_error = error;
	}

	r_arrays[RSE::ARRAY_VERTEX] = vertices;
	r_arrays[RSE::ARRAY_NORMAL] = normals;
	r_arrays[RSE::ARRAY_TANGENT] = tangents;
	r_arrays[RSE::ARRAY_TEX_UV] = uvs;
	r_arrays[RSE::ARRAY_TEX_UV2] = uv2s;
	r_arrays[RSE::ARRAY_COLOR] = colors;
	r_arrays[RSE::ARRAY_INDEX] = indices;
	r_chunk.vertex_count = vertex_count;
	r_chunk.triangle_count = indices.size() / 3;
}

void LandscapeSpline3D::_build_lake_chunk(Chunk &r_chunk, Array &r_arrays) const {
	const Rect2i cells = r_chunk.cells;
	const int vw_count = cells.size.x + 1;
	const int vh_count = cells.size.y + 1;
	const int vertex_count = vw_count * vh_count;
	PackedVector3Array vertices;
	PackedVector3Array normals;
	PackedFloat32Array tangents;
	PackedVector2Array uvs;
	PackedVector2Array uv2s;
	PackedColorArray colors;
	vertices.resize(vertex_count);
	normals.resize(vertex_count);
	tangents.resize(vertex_count * 4);
	uvs.resize(vertex_count);
	uv2s.resize(vertex_count);
	colors.resize(vertex_count);
	Vector3 *vw = vertices.ptrw();
	Vector3 *nw = normals.ptrw();
	float *tw = tangents.ptrw();
	Vector2 *uw = uvs.ptrw();
	Vector2 *u2w = uv2s.ptrw();
	Color *cw = colors.ptrw();
	for (int z = 0; z < vh_count; z++) {
		for (int x = 0; x < vw_count; x++) {
			const int v = z * vw_count + x;
			const Vector3 p(lake_origin.x + (cells.position.x + x) * lake_cell, 0.0, lake_origin.y + (cells.position.y + z) * lake_cell);
			vw[v] = p;
			nw[v] = Vector3(0, 1, 0);
			// U along +X, V along +Z.
			tw[v * 4 + 0] = 1.0;
			tw[v * 4 + 1] = 0.0;
			tw[v * 4 + 2] = 0.0;
			tw[v * 4 + 3] = -1.0;
			uw[v] = Vector2(p.x, p.z);
			u2w[v] = Vector2();
			cw[v] = Color(0, 0, 0, 1);
		}
	}
	PackedInt32Array indices;
	for (int z = 0; z < cells.size.y; z++) {
		for (int x = 0; x < cells.size.x; x++) {
			if (!lake_mask[(cells.position.y + z) * lake_cells.x + cells.position.x + x]) {
				continue;
			}
			const int a = z * vw_count + x;
			const int b = a + 1;
			const int c = a + vw_count;
			const int d = c + 1;
			indices.push_back(a);
			indices.push_back(b);
			indices.push_back(c);
			indices.push_back(b);
			indices.push_back(d);
			indices.push_back(c);
		}
	}
	r_arrays[RSE::ARRAY_VERTEX] = vertices;
	r_arrays[RSE::ARRAY_NORMAL] = normals;
	r_arrays[RSE::ARRAY_TANGENT] = tangents;
	r_arrays[RSE::ARRAY_TEX_UV] = uvs;
	r_arrays[RSE::ARRAY_TEX_UV2] = uv2s;
	r_arrays[RSE::ARRAY_COLOR] = colors;
	r_arrays[RSE::ARRAY_INDEX] = indices;
	r_chunk.vertex_count = vertex_count;
	r_chunk.triangle_count = indices.size() / 3;
}

RID LandscapeSpline3D::_get_material_rid() const {
	const Ref<Material> mat = material.is_valid() ? material : get_default_material(type);
	return mat.is_valid() ? mat->get_rid() : RID();
}

void LandscapeSpline3D::_build_chunk(Chunk &r_chunk) {
	_free_chunk(r_chunk);
	r_chunk.built = true; // Also when empty: don't try again until the spline changes.
	Array arrays;
	arrays.resize(RSE::ARRAY_MAX);
	Dictionary lods;
	if (type == TYPE_LAKE) {
		_build_lake_chunk(r_chunk, arrays);
	} else {
		_build_ribbon_chunk(r_chunk, arrays, lods);
	}
	const PackedInt32Array indices = arrays[RSE::ARRAY_INDEX];
	if (indices.is_empty() || !is_inside_tree() || get_world_3d().is_null()) {
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	r_chunk.mesh = rs->mesh_create();
	rs->mesh_add_surface_from_arrays(r_chunk.mesh, RSE::PRIMITIVE_TRIANGLES, arrays, Array(), lods);
	rs->mesh_surface_set_material(r_chunk.mesh, 0, _get_material_rid());
	r_chunk.instance = rs->instance_create2(r_chunk.mesh, get_world_3d()->get_scenario());
	rs->instance_attach_object_instance_id(r_chunk.instance, get_instance_id());
	rs->instance_set_transform(r_chunk.instance, get_global_transform());
	rs->instance_set_layer_mask(r_chunk.instance, render_layers);
	rs->instance_set_visible(r_chunk.instance, is_visible_in_tree());
	rs->instance_geometry_set_cast_shadows_setting(r_chunk.instance, cast_shadows ? RSE::SHADOW_CASTING_SETTING_ON : RSE::SHADOW_CASTING_SETTING_OFF);
	rs->instance_geometry_set_visibility_range(r_chunk.instance, 0.0, visibility_range, 0.0, visibility_range * 0.1, RSE::VISIBILITY_RANGE_FADE_SELF);
	rs->instance_geometry_set_lod_bias(r_chunk.instance, lod_bias);
}

void LandscapeSpline3D::_update_chunk_instances() {
	RenderingServer *rs = RenderingServer::get_singleton();
	const RID material_rid = _get_material_rid();
	for (Chunk &chunk : chunks) {
		if (chunk.instance.is_null()) {
			continue;
		}
		rs->mesh_surface_set_material(chunk.mesh, 0, material_rid);
		rs->instance_set_transform(chunk.instance, get_global_transform());
		rs->instance_set_layer_mask(chunk.instance, render_layers);
		rs->instance_set_visible(chunk.instance, is_visible_in_tree());
		rs->instance_geometry_set_cast_shadows_setting(chunk.instance, cast_shadows ? RSE::SHADOW_CASTING_SETTING_ON : RSE::SHADOW_CASTING_SETTING_OFF);
		rs->instance_geometry_set_visibility_range(chunk.instance, 0.0, visibility_range, 0.0, visibility_range * 0.1, RSE::VISIBILITY_RANGE_FADE_SELF);
		rs->instance_geometry_set_lod_bias(chunk.instance, lod_bias);
	}
}

bool LandscapeSpline3D::_get_view_position(Vector3 &r_global) const {
	if (landscape) {
		return landscape->get_view_position(r_global);
	}
	Camera3D *camera = nullptr;
#ifdef TOOLS_ENABLED
	if (Engine::get_singleton()->is_editor_hint() && Landscape3D::editor_camera_callback && is_part_of_edited_scene()) {
		camera = Landscape3D::editor_camera_callback();
	}
#endif
	if (!camera && get_viewport()) {
		camera = get_viewport()->get_camera_3d();
	}
	if (!camera || !camera->is_inside_tree()) {
		return false;
	}
	r_global = camera->get_camera_transform().origin;
	return true;
}

void LandscapeSpline3D::_process_chunks() {
	if (!is_inside_tree() || get_world_3d().is_null()) {
		return;
	}
	if (chunks_dirty) {
		_rebuild_chunks();
	}
	if (chunks.is_empty()) {
		return;
	}

	// Chunks are only built within the visibility range (plus a margin) of the camera and freed
	// once they are well beyond it: long roads and rivers only use memory around the camera.
	Vector3 view;
	const bool has_view = visibility_range > 0.0 && _get_view_position(view);
	const Transform3D global = get_global_transform();
	const real_t build_range = visibility_range * 1.1 + 32.0;
	const real_t free_range = build_range * 1.25 + 32.0;
	struct Candidate {
		real_t distance;
		int index;
		bool operator<(const Candidate &p_other) const { return distance < p_other.distance; }
	};
	LocalVector<Candidate> to_build;
	for (uint32_t i = 0; i < chunks.size(); i++) {
		Chunk &chunk = chunks[i];
		real_t distance = 0.0;
		if (has_view) {
			const AABB aabb = global.xform(chunk.aabb);
			const Vector3 closest = view.clamp(aabb.position, aabb.position + aabb.size);
			distance = closest.distance_to(view);
		}
		const bool needed = !has_view || distance <= (chunk.built ? free_range : build_range);
		if (needed && !chunk.built) {
			to_build.push_back({ distance, int(i) });
		} else if (!needed && chunk.built) {
			_free_chunk(chunk);
		}
	}
	if (to_build.is_empty()) {
		return;
	}
	to_build.sort();
	// Nearest chunks first, within a time budget (at least a few chunks per frame).
	const uint64_t begin = OS::get_singleton()->get_ticks_usec();
	for (uint32_t k = 0; k < to_build.size(); k++) {
		if (k >= uint32_t(MAX_CHUNK_BUILDS_PER_FRAME) && OS::get_singleton()->get_ticks_usec() - begin > CHUNK_BUILD_BUDGET_USEC) {
			break;
		}
		_build_chunk(chunks[to_build[k].index]);
	}
}

Ref<ArrayMesh> LandscapeSpline3D::create_mesh() {
	// Chunk layout of the current settings, without touching the rendered chunks.
	LocalVector<Chunk> saved_chunks = std::move(chunks);
	const bool saved_dirty = chunks_dirty;
	chunks.clear();
	const bool enabled = mesh_enabled;
	mesh_enabled = true;
	_rebuild_chunks();
	mesh_enabled = enabled;

	PackedVector3Array vertices;
	PackedVector3Array normals;
	PackedFloat32Array tangents;
	PackedVector2Array uvs;
	PackedVector2Array uv2s;
	PackedColorArray colors;
	PackedInt32Array indices;
	for (Chunk &chunk : chunks) {
		Array arrays;
		arrays.resize(RSE::ARRAY_MAX);
		Dictionary lods;
		if (type == TYPE_LAKE) {
			_build_lake_chunk(chunk, arrays);
		} else {
			_build_ribbon_chunk(chunk, arrays, lods);
		}
		const PackedInt32Array chunk_indices = arrays[RSE::ARRAY_INDEX];
		if (chunk_indices.is_empty()) {
			continue;
		}
		const int base = vertices.size();
		vertices.append_array(arrays[RSE::ARRAY_VERTEX]);
		normals.append_array(arrays[RSE::ARRAY_NORMAL]);
		tangents.append_array(arrays[RSE::ARRAY_TANGENT]);
		uvs.append_array(arrays[RSE::ARRAY_TEX_UV]);
		uv2s.append_array(arrays[RSE::ARRAY_TEX_UV2]);
		colors.append_array(arrays[RSE::ARRAY_COLOR]);
		for (int i = 0; i < chunk_indices.size(); i++) {
			indices.push_back(chunk_indices[i] + base);
		}
	}
	chunks = std::move(saved_chunks);
	chunks_dirty = saved_dirty;

	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	if (indices.is_empty()) {
		return mesh;
	}
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = vertices;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_TANGENT] = tangents;
	arrays[Mesh::ARRAY_TEX_UV] = uvs;
	arrays[Mesh::ARRAY_TEX_UV2] = uv2s;
	arrays[Mesh::ARRAY_COLOR] = colors;
	arrays[Mesh::ARRAY_INDEX] = indices;
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	mesh->surface_set_material(0, material.is_valid() ? material : get_default_material(type));
	return mesh;
}

void LandscapeSpline3D::rebuild() {
	chunks_dirty = true;
	curve_dirty = true;
	terrain_shape_dirty = true;
	if (landscape) {
		landscape->_spline_changed(this, true);
	}
	update_gizmos();
}

Dictionary LandscapeSpline3D::get_statistics() const {
	Dictionary stats;
	int built = 0;
	int64_t vertices = 0;
	int64_t triangles = 0;
	for (const Chunk &chunk : chunks) {
		if (chunk.mesh.is_valid()) {
			built++;
			vertices += chunk.vertex_count;
			triangles += chunk.triangle_count;
		}
	}
	stats["samples"] = int(samples.size());
	stats["length"] = samples.is_empty() ? 0.0 : double(samples[samples.size() - 1].distance);
	stats["chunks"] = int(chunks.size());
	stats["chunks_built"] = built;
	stats["vertices"] = vertices;
	stats["triangles"] = triangles;
	return stats;
}

/* Materials */

Ref<Material> LandscapeSpline3D::get_default_material(SplineType p_type) {
	const int index = p_type == TYPE_ROAD ? 0 : 1;
	if (default_materials[index].is_null()) {
		if (index == 0) {
			default_materials[index] = memnew(LandscapeRoadMaterial);
		} else {
			default_materials[index] = memnew(LandscapeWaterMaterial);
		}
	}
	return default_materials[index];
}

void LandscapeSpline3D::cleanup_shared_resources() {
	for (Ref<Material> &mat : default_materials) {
		mat.unref();
	}
}

/* Settings */

void LandscapeSpline3D::set_mesh_enabled(bool p_enabled) {
	mesh_enabled = p_enabled;
	chunks_dirty = true;
	if (!mesh_enabled) {
		_clear_chunks();
	}
}

void LandscapeSpline3D::set_material(const Ref<Material> &p_material) {
	material = p_material;
	_update_chunk_instances();
}

void LandscapeSpline3D::set_sample_spacing(float p_spacing) {
	sample_spacing = CLAMP(p_spacing, 0.1f, 100.0f);
	_changed();
}

void LandscapeSpline3D::set_lateral_spacing(float p_spacing) {
	lateral_spacing = CLAMP(p_spacing, 0.1f, 100.0f);
	chunks_dirty = true;
}

void LandscapeSpline3D::set_water_overlap(float p_overlap) {
	water_overlap = MAX(p_overlap, 0.0f);
	chunks_dirty = true;
	update_gizmos();
}

void LandscapeSpline3D::set_skirt_depth(float p_depth) {
	skirt_depth = MAX(p_depth, 0.0f);
	chunks_dirty = true;
}

void LandscapeSpline3D::set_chunk_length(float p_length) {
	chunk_length = CLAMP(p_length, 8.0f, 4096.0f);
	chunks_dirty = true;
}

void LandscapeSpline3D::set_visibility_range(float p_range) {
	visibility_range = MAX(p_range, 0.0f);
	_update_chunk_instances();
}

void LandscapeSpline3D::set_lod_bias(float p_bias) {
	lod_bias = MAX(p_bias, 0.0001f);
	_update_chunk_instances();
}

void LandscapeSpline3D::set_render_layers(uint32_t p_layers) {
	render_layers = p_layers;
	_update_chunk_instances();
}

void LandscapeSpline3D::set_cast_shadows(bool p_enable) {
	cast_shadows = p_enable;
	_update_chunk_instances();
}

void LandscapeSpline3D::set_terrain_enabled(bool p_enabled) {
	terrain_enabled = p_enabled;
	notify_property_list_changed();
	_changed(false);
}

void LandscapeSpline3D::set_terrain_priority(int p_priority) {
	terrain_priority = p_priority;
	_changed(false);
}

void LandscapeSpline3D::set_terrain_strength(float p_strength) {
	terrain_strength = CLAMP(p_strength, 0.0f, 1.0f);
	_changed(false);
}

void LandscapeSpline3D::set_terrain_falloff(float p_falloff) {
	terrain_falloff = MAX(p_falloff, 0.0f);
	_changed(false);
}

void LandscapeSpline3D::set_terrain_end_falloff(float p_falloff) {
	terrain_end_falloff = MAX(p_falloff, 0.0f);
	_changed(false);
}

void LandscapeSpline3D::set_terrain_offset(float p_offset) {
	terrain_offset = p_offset;
	_changed(false);
}

void LandscapeSpline3D::set_terrain_raise(bool p_raise) {
	terrain_raise = p_raise;
	_changed(false);
}

void LandscapeSpline3D::set_terrain_lower(bool p_lower) {
	terrain_lower = p_lower;
	_changed(false);
}

void LandscapeSpline3D::set_bank_height(float p_height) {
	bank_height = p_height;
	_changed(false);
}

void LandscapeSpline3D::set_channel_shape(float p_shape) {
	channel_shape = CLAMP(p_shape, 0.25f, 8.0f);
	_changed(false);
}

void LandscapeSpline3D::set_shore_width(float p_width) {
	shore_width = MAX(p_width, 0.1f);
	_changed(false);
}

void LandscapeSpline3D::set_paint_layer(int p_layer) {
	paint_layer = CLAMP(p_layer, -1, LandscapeData::MAX_LAYERS - 1);
	notify_property_list_changed();
	update_configuration_warnings();
	_changed(false);
}

void LandscapeSpline3D::set_paint_width(float p_width) {
	paint_width = p_width;
	_changed(false);
}

void LandscapeSpline3D::set_paint_falloff(float p_falloff) {
	paint_falloff = MAX(p_falloff, 0.0f);
	_changed(false);
}

void LandscapeSpline3D::set_paint_noise(float p_noise) {
	paint_noise = CLAMP(p_noise, 0.0f, 1.0f);
	_changed(false);
}

void LandscapeSpline3D::set_paint_strength(float p_strength) {
	paint_strength = CLAMP(p_strength, 0.0f, 1.0f);
	_changed(false);
}

/* Scene */

void LandscapeSpline3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			landscape = _find_landscape();
			terrain_shape_dirty = true;
			chunks_dirty = true;
			if (landscape) {
				landscape->_register_spline(this);
			}
			set_process_internal(true);
		} break;

		case NOTIFICATION_EXIT_TREE: {
			_clear_chunks();
			chunks_dirty = true;
			if (landscape) {
				landscape->_unregister_spline(this);
				landscape = nullptr;
			}
			set_process_internal(false);
		} break;

		case NOTIFICATION_EXIT_WORLD: {
			_clear_chunks();
			chunks_dirty = true;
		} break;

		case NOTIFICATION_TRANSFORM_CHANGED: {
			RenderingServer *rs = RenderingServer::get_singleton();
			for (const Chunk &chunk : chunks) {
				if (chunk.instance.is_valid()) {
					rs->instance_set_transform(chunk.instance, get_global_transform());
				}
			}
			// Moving the landscape moves its splines too: only a relative move changes the terrain.
			if (landscape && terrain_enabled) {
				const Transform3D xform = _get_terrain_transform();
				if (!xform.is_equal_approx(terrain_transform)) {
					terrain_shape_dirty = true;
					landscape->_spline_changed(this);
				}
			}
		} break;

		case NOTIFICATION_VISIBILITY_CHANGED: {
			RenderingServer *rs = RenderingServer::get_singleton();
			for (const Chunk &chunk : chunks) {
				if (chunk.instance.is_valid()) {
					rs->instance_set_visible(chunk.instance, is_visible_in_tree());
				}
			}
		} break;

		case NOTIFICATION_INTERNAL_PROCESS: {
			if (mesh_enabled && is_visible_in_tree()) {
				_process_chunks();
			}
		} break;
	}
}

void LandscapeSpline3D::_validate_property(PropertyInfo &p_property) const {
	const bool water = type != TYPE_ROAD;
	const bool channel = type == TYPE_RIVER || type == TYPE_STREAM;
	const String name = p_property.name;
	bool hidden = false;
	if (name == "point_tilts") {
		hidden = type != TYPE_ROAD;
	} else if (name == "point_flow_speeds" || name == "flow_speed") {
		hidden = !channel;
	} else if (name == "point_widths" || name == "width" || name == "lateral_spacing" || name == "terrain_end_falloff" || name == "chunk_length") {
		hidden = type == TYPE_LAKE;
	} else if (name == "point_depths") {
		hidden = !channel;
	} else if (name == "depth") {
		hidden = type == TYPE_ROAD;
	} else if (name == "skirt_depth" || name == "terrain_offset") {
		hidden = water;
	} else if (name == "water_overlap" || name == "bank_height") {
		hidden = !water;
	} else if (name == "channel_shape") {
		hidden = !channel;
	} else if (name == "shore_width") {
		hidden = type != TYPE_LAKE;
	}
	if (hidden) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		return;
	}
	if (name == "closed" && type == TYPE_LAKE) {
		p_property.usage |= PROPERTY_USAGE_READ_ONLY;
	}
	if ((name.begins_with("terrain_") && name != "terrain_enabled") || name == "bank_height" || name == "channel_shape" || name == "shore_width") {
		if (!terrain_enabled) {
			p_property.usage |= PROPERTY_USAGE_READ_ONLY;
		}
	}
	if (name.begins_with("paint_") && name != "paint_layer" && paint_layer < 0) {
		p_property.usage |= PROPERTY_USAGE_READ_ONLY;
	}
}

PackedStringArray LandscapeSpline3D::get_configuration_warnings() const {
	PackedStringArray warnings = Node3D::get_configuration_warnings();
	const Landscape3D *parent_landscape = _find_landscape();
	if (!parent_landscape) {
		warnings.push_back(RTR("LandscapeSpline3D modifies the terrain of a Landscape3D ancestor. Without one, only its mesh is rendered."));
	}
	const int min_points = is_closed() ? 3 : 2;
	if (points.size() < min_points) {
		warnings.push_back(vformat(RTR("At least %d points are needed (use the Splines tab of the Landscape dock to draw them)."), min_points));
	}
	if (parent_landscape && paint_layer >= parent_landscape->get_layer_count()) {
		warnings.push_back(vformat(RTR("The paint layer %d doesn't exist in the landscape, nothing is painted."), paint_layer));
	}
	if ((type == TYPE_RIVER || type == TYPE_STREAM) && !is_closed() && points.size() >= 2 && points[points.size() - 1].y > points[0].y + 0.01) {
		warnings.push_back(RTR("The water flows from the first point to the last one, but the last point is higher. Reverse the spline or make it downhill."));
	}
	return warnings;
}

void LandscapeSpline3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_type", "type"), &LandscapeSpline3D::set_type);
	ClassDB::bind_method(D_METHOD("get_type"), &LandscapeSpline3D::get_type);
	ClassDB::bind_method(D_METHOD("apply_type_defaults"), &LandscapeSpline3D::apply_type_defaults);

	ClassDB::bind_method(D_METHOD("set_points", "points"), &LandscapeSpline3D::set_points);
	ClassDB::bind_method(D_METHOD("get_points"), &LandscapeSpline3D::get_points);
	ClassDB::bind_method(D_METHOD("set_point_widths", "widths"), &LandscapeSpline3D::set_point_widths);
	ClassDB::bind_method(D_METHOD("get_point_widths"), &LandscapeSpline3D::get_point_widths);
	ClassDB::bind_method(D_METHOD("set_point_depths", "depths"), &LandscapeSpline3D::set_point_depths);
	ClassDB::bind_method(D_METHOD("get_point_depths"), &LandscapeSpline3D::get_point_depths);
	ClassDB::bind_method(D_METHOD("set_point_tilts", "tilts"), &LandscapeSpline3D::set_point_tilts);
	ClassDB::bind_method(D_METHOD("get_point_tilts"), &LandscapeSpline3D::get_point_tilts);
	ClassDB::bind_method(D_METHOD("set_point_flow_speeds", "speeds"), &LandscapeSpline3D::set_point_flow_speeds);
	ClassDB::bind_method(D_METHOD("get_point_flow_speeds"), &LandscapeSpline3D::get_point_flow_speeds);
	ClassDB::bind_method(D_METHOD("get_point_count"), &LandscapeSpline3D::get_point_count);
	ClassDB::bind_method(D_METHOD("add_point", "position", "index"), &LandscapeSpline3D::add_point, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("remove_point", "index"), &LandscapeSpline3D::remove_point);
	ClassDB::bind_method(D_METHOD("clear_points"), &LandscapeSpline3D::clear_points);
	ClassDB::bind_method(D_METHOD("set_point_position", "index", "position"), &LandscapeSpline3D::set_point_position);
	ClassDB::bind_method(D_METHOD("get_point_position", "index"), &LandscapeSpline3D::get_point_position);
	ClassDB::bind_method(D_METHOD("set_point_width", "index", "width"), &LandscapeSpline3D::set_point_width);
	ClassDB::bind_method(D_METHOD("get_point_width", "index"), &LandscapeSpline3D::get_point_width);
	ClassDB::bind_method(D_METHOD("set_point_depth", "index", "depth"), &LandscapeSpline3D::set_point_depth);
	ClassDB::bind_method(D_METHOD("get_point_depth", "index"), &LandscapeSpline3D::get_point_depth);
	ClassDB::bind_method(D_METHOD("set_point_tilt", "index", "tilt"), &LandscapeSpline3D::set_point_tilt);
	ClassDB::bind_method(D_METHOD("get_point_tilt", "index"), &LandscapeSpline3D::get_point_tilt);
	ClassDB::bind_method(D_METHOD("set_point_flow_speed", "index", "speed"), &LandscapeSpline3D::set_point_flow_speed);
	ClassDB::bind_method(D_METHOD("get_point_flow_speed", "index"), &LandscapeSpline3D::get_point_flow_speed);
	ClassDB::bind_method(D_METHOD("set_closed", "closed"), &LandscapeSpline3D::set_closed);
	ClassDB::bind_method(D_METHOD("is_closed"), &LandscapeSpline3D::is_closed);
	ClassDB::bind_method(D_METHOD("set_width", "width"), &LandscapeSpline3D::set_width);
	ClassDB::bind_method(D_METHOD("get_width"), &LandscapeSpline3D::get_width);
	ClassDB::bind_method(D_METHOD("set_depth", "depth"), &LandscapeSpline3D::set_depth);
	ClassDB::bind_method(D_METHOD("get_depth"), &LandscapeSpline3D::get_depth);
	ClassDB::bind_method(D_METHOD("set_flow_speed", "speed"), &LandscapeSpline3D::set_flow_speed);
	ClassDB::bind_method(D_METHOD("get_flow_speed"), &LandscapeSpline3D::get_flow_speed);
	ClassDB::bind_method(D_METHOD("set_spline_id", "id"), &LandscapeSpline3D::set_spline_id);
	ClassDB::bind_method(D_METHOD("get_spline_id"), &LandscapeSpline3D::get_spline_id);

	ClassDB::bind_method(D_METHOD("set_mesh_enabled", "enabled"), &LandscapeSpline3D::set_mesh_enabled);
	ClassDB::bind_method(D_METHOD("is_mesh_enabled"), &LandscapeSpline3D::is_mesh_enabled);
	ClassDB::bind_method(D_METHOD("set_material", "material"), &LandscapeSpline3D::set_material);
	ClassDB::bind_method(D_METHOD("get_material"), &LandscapeSpline3D::get_material);
	ClassDB::bind_method(D_METHOD("set_sample_spacing", "spacing"), &LandscapeSpline3D::set_sample_spacing);
	ClassDB::bind_method(D_METHOD("get_sample_spacing"), &LandscapeSpline3D::get_sample_spacing);
	ClassDB::bind_method(D_METHOD("set_lateral_spacing", "spacing"), &LandscapeSpline3D::set_lateral_spacing);
	ClassDB::bind_method(D_METHOD("get_lateral_spacing"), &LandscapeSpline3D::get_lateral_spacing);
	ClassDB::bind_method(D_METHOD("set_water_overlap", "overlap"), &LandscapeSpline3D::set_water_overlap);
	ClassDB::bind_method(D_METHOD("get_water_overlap"), &LandscapeSpline3D::get_water_overlap);
	ClassDB::bind_method(D_METHOD("set_skirt_depth", "depth"), &LandscapeSpline3D::set_skirt_depth);
	ClassDB::bind_method(D_METHOD("get_skirt_depth"), &LandscapeSpline3D::get_skirt_depth);
	ClassDB::bind_method(D_METHOD("set_chunk_length", "length"), &LandscapeSpline3D::set_chunk_length);
	ClassDB::bind_method(D_METHOD("get_chunk_length"), &LandscapeSpline3D::get_chunk_length);
	ClassDB::bind_method(D_METHOD("set_visibility_range", "range"), &LandscapeSpline3D::set_visibility_range);
	ClassDB::bind_method(D_METHOD("get_visibility_range"), &LandscapeSpline3D::get_visibility_range);
	ClassDB::bind_method(D_METHOD("set_lod_bias", "bias"), &LandscapeSpline3D::set_lod_bias);
	ClassDB::bind_method(D_METHOD("get_lod_bias"), &LandscapeSpline3D::get_lod_bias);
	ClassDB::bind_method(D_METHOD("set_render_layers", "layers"), &LandscapeSpline3D::set_render_layers);
	ClassDB::bind_method(D_METHOD("get_render_layers"), &LandscapeSpline3D::get_render_layers);
	ClassDB::bind_method(D_METHOD("set_cast_shadows", "enable"), &LandscapeSpline3D::set_cast_shadows);
	ClassDB::bind_method(D_METHOD("is_casting_shadows"), &LandscapeSpline3D::is_casting_shadows);

	ClassDB::bind_method(D_METHOD("set_terrain_enabled", "enabled"), &LandscapeSpline3D::set_terrain_enabled);
	ClassDB::bind_method(D_METHOD("is_terrain_enabled"), &LandscapeSpline3D::is_terrain_enabled);
	ClassDB::bind_method(D_METHOD("set_terrain_priority", "priority"), &LandscapeSpline3D::set_terrain_priority);
	ClassDB::bind_method(D_METHOD("get_terrain_priority"), &LandscapeSpline3D::get_terrain_priority);
	ClassDB::bind_method(D_METHOD("set_terrain_strength", "strength"), &LandscapeSpline3D::set_terrain_strength);
	ClassDB::bind_method(D_METHOD("get_terrain_strength"), &LandscapeSpline3D::get_terrain_strength);
	ClassDB::bind_method(D_METHOD("set_terrain_falloff", "falloff"), &LandscapeSpline3D::set_terrain_falloff);
	ClassDB::bind_method(D_METHOD("get_terrain_falloff"), &LandscapeSpline3D::get_terrain_falloff);
	ClassDB::bind_method(D_METHOD("set_terrain_end_falloff", "falloff"), &LandscapeSpline3D::set_terrain_end_falloff);
	ClassDB::bind_method(D_METHOD("get_terrain_end_falloff"), &LandscapeSpline3D::get_terrain_end_falloff);
	ClassDB::bind_method(D_METHOD("set_terrain_offset", "offset"), &LandscapeSpline3D::set_terrain_offset);
	ClassDB::bind_method(D_METHOD("get_terrain_offset"), &LandscapeSpline3D::get_terrain_offset);
	ClassDB::bind_method(D_METHOD("set_terrain_raise", "raise"), &LandscapeSpline3D::set_terrain_raise);
	ClassDB::bind_method(D_METHOD("is_terrain_raising"), &LandscapeSpline3D::is_terrain_raising);
	ClassDB::bind_method(D_METHOD("set_terrain_lower", "lower"), &LandscapeSpline3D::set_terrain_lower);
	ClassDB::bind_method(D_METHOD("is_terrain_lowering"), &LandscapeSpline3D::is_terrain_lowering);
	ClassDB::bind_method(D_METHOD("set_bank_height", "height"), &LandscapeSpline3D::set_bank_height);
	ClassDB::bind_method(D_METHOD("get_bank_height"), &LandscapeSpline3D::get_bank_height);
	ClassDB::bind_method(D_METHOD("set_channel_shape", "shape"), &LandscapeSpline3D::set_channel_shape);
	ClassDB::bind_method(D_METHOD("get_channel_shape"), &LandscapeSpline3D::get_channel_shape);
	ClassDB::bind_method(D_METHOD("set_shore_width", "width"), &LandscapeSpline3D::set_shore_width);
	ClassDB::bind_method(D_METHOD("get_shore_width"), &LandscapeSpline3D::get_shore_width);

	ClassDB::bind_method(D_METHOD("set_paint_layer", "layer"), &LandscapeSpline3D::set_paint_layer);
	ClassDB::bind_method(D_METHOD("get_paint_layer"), &LandscapeSpline3D::get_paint_layer);
	ClassDB::bind_method(D_METHOD("set_paint_width", "width"), &LandscapeSpline3D::set_paint_width);
	ClassDB::bind_method(D_METHOD("get_paint_width"), &LandscapeSpline3D::get_paint_width);
	ClassDB::bind_method(D_METHOD("set_paint_falloff", "falloff"), &LandscapeSpline3D::set_paint_falloff);
	ClassDB::bind_method(D_METHOD("get_paint_falloff"), &LandscapeSpline3D::get_paint_falloff);
	ClassDB::bind_method(D_METHOD("set_paint_noise", "noise"), &LandscapeSpline3D::set_paint_noise);
	ClassDB::bind_method(D_METHOD("get_paint_noise"), &LandscapeSpline3D::get_paint_noise);
	ClassDB::bind_method(D_METHOD("set_paint_strength", "strength"), &LandscapeSpline3D::set_paint_strength);
	ClassDB::bind_method(D_METHOD("get_paint_strength"), &LandscapeSpline3D::get_paint_strength);

	ClassDB::bind_method(D_METHOD("get_length"), &LandscapeSpline3D::get_length);
	ClassDB::bind_method(D_METHOD("sample_transform", "distance"), &LandscapeSpline3D::sample_transform);
	ClassDB::bind_method(D_METHOD("sample_position", "distance"), &LandscapeSpline3D::sample_position);
	ClassDB::bind_method(D_METHOD("get_closest_distance", "local_position"), &LandscapeSpline3D::get_closest_distance);
	ClassDB::bind_method(D_METHOD("get_surface_info", "global_position"), &LandscapeSpline3D::get_surface_info);
	ClassDB::bind_method(D_METHOD("is_point_in_water", "global_position"), &LandscapeSpline3D::is_point_in_water);
	ClassDB::bind_method(D_METHOD("create_curve"), &LandscapeSpline3D::create_curve);
	ClassDB::bind_method(D_METHOD("get_tessellated_points"), &LandscapeSpline3D::get_tessellated_points);
	ClassDB::bind_method(D_METHOD("snap_to_terrain", "offset"), &LandscapeSpline3D::snap_to_terrain, DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("level_lake_to_shore", "margin"), &LandscapeSpline3D::level_lake_to_shore, DEFVAL(0.2));
	ClassDB::bind_method(D_METHOD("make_downhill", "min_drop"), &LandscapeSpline3D::make_downhill, DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("reverse"), &LandscapeSpline3D::reverse);
	ClassDB::bind_method(D_METHOD("create_mesh"), &LandscapeSpline3D::create_mesh);
	ClassDB::bind_method(D_METHOD("rebuild"), &LandscapeSpline3D::rebuild);
	ClassDB::bind_method(D_METHOD("get_statistics"), &LandscapeSpline3D::get_statistics);
	ClassDB::bind_static_method("LandscapeSpline3D", D_METHOD("get_default_material", "type"), &LandscapeSpline3D::get_default_material);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "type", PROPERTY_HINT_ENUM, "Road,River,Stream,Lake"), "set_type", "get_type");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "closed"), "set_closed", "is_closed");
	// Random for every new spline: internal, so that it isn't documented with a random default value.
	ADD_PROPERTY(PropertyInfo(Variant::INT, "spline_id", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR | PROPERTY_USAGE_INTERNAL), "set_spline_id", "get_spline_id");

	ADD_GROUP("Points", "");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_VECTOR3_ARRAY, "points"), "set_points", "get_points");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "point_widths"), "set_point_widths", "get_point_widths");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "point_depths"), "set_point_depths", "get_point_depths");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "point_tilts"), "set_point_tilts", "get_point_tilts");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "point_flow_speeds"), "set_point_flow_speeds", "get_point_flow_speeds");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "width", PROPERTY_HINT_RANGE, "0.1,500,0.01,or_greater,suffix:m"), "set_width", "get_width");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "depth", PROPERTY_HINT_RANGE, "0,100,0.01,or_greater,suffix:m"), "set_depth", "get_depth");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "flow_speed", PROPERTY_HINT_RANGE, "-20,20,0.01,suffix:m/s"), "set_flow_speed", "get_flow_speed");

	ADD_GROUP("Mesh", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "mesh_enabled"), "set_mesh_enabled", "is_mesh_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "material", PROPERTY_HINT_RESOURCE_TYPE, "LandscapeWaterMaterial,LandscapeRoadMaterial,ShaderMaterial,BaseMaterial3D"), "set_material", "get_material");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sample_spacing", PROPERTY_HINT_RANGE, "0.1,32,0.01,or_greater,suffix:m"), "set_sample_spacing", "get_sample_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lateral_spacing", PROPERTY_HINT_RANGE, "0.1,32,0.01,or_greater,suffix:m"), "set_lateral_spacing", "get_lateral_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "water_overlap", PROPERTY_HINT_RANGE, "0,16,0.01,or_greater,suffix:m"), "set_water_overlap", "get_water_overlap");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "skirt_depth", PROPERTY_HINT_RANGE, "0,4,0.01,or_greater,suffix:m"), "set_skirt_depth", "get_skirt_depth");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "chunk_length", PROPERTY_HINT_RANGE, "8,1024,1,or_greater,suffix:m"), "set_chunk_length", "get_chunk_length");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "visibility_range", PROPERTY_HINT_RANGE, "0,16384,1,or_greater,suffix:m"), "set_visibility_range", "get_visibility_range");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lod_bias", PROPERTY_HINT_RANGE, "0.001,128,0.001"), "set_lod_bias", "get_lod_bias");

	ADD_GROUP("Rendering", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "render_layers", PROPERTY_HINT_LAYERS_3D_RENDER), "set_render_layers", "get_render_layers");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "cast_shadows"), "set_cast_shadows", "is_casting_shadows");

	ADD_GROUP("Terrain", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "terrain_enabled"), "set_terrain_enabled", "is_terrain_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "terrain_priority", PROPERTY_HINT_RANGE, "-1000,1000,1"), "set_terrain_priority", "get_terrain_priority");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "terrain_strength", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_terrain_strength", "get_terrain_strength");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "terrain_falloff", PROPERTY_HINT_RANGE, "0,256,0.01,or_greater,suffix:m"), "set_terrain_falloff", "get_terrain_falloff");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "terrain_end_falloff", PROPERTY_HINT_RANGE, "0,256,0.01,or_greater,suffix:m"), "set_terrain_end_falloff", "get_terrain_end_falloff");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "terrain_offset", PROPERTY_HINT_RANGE, "-4,4,0.001,or_greater,or_less,suffix:m"), "set_terrain_offset", "get_terrain_offset");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "terrain_raise"), "set_terrain_raise", "is_terrain_raising");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "terrain_lower"), "set_terrain_lower", "is_terrain_lowering");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "bank_height", PROPERTY_HINT_RANGE, "-4,8,0.01,or_greater,or_less,suffix:m"), "set_bank_height", "get_bank_height");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "channel_shape", PROPERTY_HINT_RANGE, "0.25,8,0.01"), "set_channel_shape", "get_channel_shape");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "shore_width", PROPERTY_HINT_RANGE, "0.1,512,0.01,or_greater,suffix:m"), "set_shore_width", "get_shore_width");

	ADD_GROUP("Paint", "paint_");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "paint_layer", PROPERTY_HINT_RANGE, "-1,15,1"), "set_paint_layer", "get_paint_layer");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "paint_width", PROPERTY_HINT_RANGE, "-64,64,0.01,or_greater,or_less,suffix:m"), "set_paint_width", "get_paint_width");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "paint_falloff", PROPERTY_HINT_RANGE, "0,64,0.01,or_greater,suffix:m"), "set_paint_falloff", "get_paint_falloff");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "paint_noise", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_paint_noise", "get_paint_noise");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "paint_strength", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_paint_strength", "get_paint_strength");

	BIND_ENUM_CONSTANT(TYPE_ROAD);
	BIND_ENUM_CONSTANT(TYPE_RIVER);
	BIND_ENUM_CONSTANT(TYPE_STREAM);
	BIND_ENUM_CONSTANT(TYPE_LAKE);
}

LandscapeSpline3D::LandscapeSpline3D() {
	set_notify_transform(true);
	regenerate_spline_id();
}

LandscapeSpline3D::~LandscapeSpline3D() {
	_clear_chunks();
}
