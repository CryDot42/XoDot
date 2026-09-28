/**************************************************************************/
/*  test_landscape_splines.h                                              */
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

#include "../landscape_3d.h"
#include "../landscape_brush.h"
#include "../landscape_data.h"
#include "../landscape_spline_3d.h"
#include "../landscape_spline_curve.h"
#include "../landscape_spline_materials.h"

#include "core/io/dir_access.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/curve.h"
#include "scene/resources/mesh.h"
#include "tests/test_macros.h"
#include "tests/test_utils.h"

namespace TestLandscapeSplines {

static LandscapeSplinePoint make_point(const Vector3 &p_position, float p_width = 6.0, float p_depth = 1.0) {
	LandscapeSplinePoint point;
	point.position = p_position;
	point.width = p_width;
	point.depth = p_depth;
	return point;
}

static LandscapeSplineTerrainShape make_shape(LandscapeSplineTerrainShape::Profile p_profile, const LocalVector<LandscapeSplinePoint> &p_points, bool p_closed, const Vector2i &p_size) {
	LandscapeSplineTerrainShape shape;
	shape.profile = p_profile;
	shape.closed = p_closed;
	LocalVector<int> starts;
	LandscapeSplineCurve::Settings settings;
	settings.max_step = 1.0;
	LandscapeSplineCurve::tessellate(p_points, p_closed, settings, shape.samples, &starts);
	shape.falloff = 4.0;
	shape.end_falloff = 4.0;
	shape.offset = 0.1;
	shape.paint_noise = 0.0;
	shape.finalize(starts, 1.0, p_size, 1234);
	return shape;
}

TEST_CASE("[Landscape][Splines] Curve through the control points") {
	LocalVector<LandscapeSplinePoint> points;
	points.push_back(make_point(Vector3(0, 0, 0), 4.0));
	points.push_back(make_point(Vector3(20, 2, 5), 8.0));
	points.push_back(make_point(Vector3(40, 0, -5), 4.0));
	points.push_back(make_point(Vector3(60, 1, 0), 6.0));

	LocalVector<LandscapeSplineSample> samples;
	LocalVector<int> starts;
	LandscapeSplineCurve::Settings settings;
	settings.max_step = 2.0;
	LandscapeSplineCurve::tessellate(points, false, settings, samples, &starts);
	REQUIRE(starts.size() == 4);
	for (int i = 0; i < 3; i++) {
		CHECK(samples[starts[i]].position.is_equal_approx(points[i].position));
		CHECK(samples[starts[i]].width == doctest::Approx(points[i].width));
	}
	CHECK(samples[samples.size() - 1].position.is_equal_approx(points[3].position));
	CHECK(starts[3] == int(samples.size()) - 1);

	bool increasing = true;
	bool frames = true;
	bool spacing = true;
	for (uint32_t i = 0; i < samples.size(); i++) {
		const LandscapeSplineSample &s = samples[i];
		frames = frames && Math::is_equal_approx(s.forward.length(), real_t(1.0)) && Math::abs(s.forward.dot(s.right)) < 1e-4 && s.up.y > 0.0;
		if (i > 0) {
			increasing = increasing && s.distance > samples[i - 1].distance;
			spacing = spacing && s.position.distance_to(samples[i - 1].position) <= settings.max_step * 1.05;
		}
	}
	CHECK(increasing);
	CHECK(frames);
	CHECK(spacing);
	// Widths are interpolated without overshoot.
	bool widths = true;
	for (const LandscapeSplineSample &s : samples) {
		widths = widths && s.width >= 4.0 - 1e-4 && s.width <= 8.0 + 1e-4;
	}
	CHECK(widths);

	// Closed curves end with a copy of the first sample.
	LocalVector<LandscapeSplineSample> loop;
	LandscapeSplineCurve::tessellate(points, true, settings, loop, &starts);
	CHECK(starts.size() == 5);
	CHECK(loop[loop.size() - 1].position.is_equal_approx(loop[0].position));
	CHECK(loop[loop.size() - 1].distance > samples[samples.size() - 1].distance);

	// Not enough points.
	LocalVector<LandscapeSplinePoint> single;
	single.push_back(points[0]);
	LandscapeSplineCurve::tessellate(single, false, settings, samples);
	CHECK(samples.is_empty());
}

TEST_CASE("[Landscape][Splines] Road, channel and lake profiles") {
	const Vector2i size(65, 65);
	const Rect2i rect(Point2i(), size);
	LocalVector<float> heights;
	heights.resize(size.x * size.y);

	SUBCASE("Road bed with side and end falloffs") {
		LocalVector<LandscapeSplinePoint> points;
		points.push_back(make_point(Vector3(12, 5, 32), 6.0));
		points.push_back(make_point(Vector3(52, 5, 32), 6.0));
		LandscapeSplineTerrainShape shape = make_shape(LandscapeSplineTerrainShape::PROFILE_ROAD, points, false, size);
		for (float &h : heights) {
			h = 0.0;
		}
		LandscapeSplineTerrain::apply(shape, 1.0, rect, heights.ptr(), nullptr, 0);
		auto at = [&](int p_x, int p_z) { return heights[p_z * size.x + p_x]; };
		CHECK(at(32, 32) == doctest::Approx(4.9));
		CHECK(at(32, 34) == doctest::Approx(4.9)); // Inside the 6 m width.
		CHECK(at(32, 37) > 0.1); // Side falloff.
		CHECK(at(32, 37) < 4.9);
		CHECK(at(32, 40) == doctest::Approx(0.0)); // Beyond the falloff.
		CHECK(at(10, 32) > 0.1); // End falloff.
		CHECK(at(6, 32) == doctest::Approx(0.0));

		// Lower only: the road is above the terrain, nothing changes.
		shape.raise = false;
		for (float &h : heights) {
			h = 0.0;
		}
		LandscapeSplineTerrain::apply(shape, 1.0, rect, heights.ptr(), nullptr, 0);
		CHECK(at(32, 32) == doctest::Approx(0.0));
	}

	SUBCASE("River channel below the water surface") {
		LocalVector<LandscapeSplinePoint> points;
		points.push_back(make_point(Vector3(0, 10, 32), 8.0, 2.0));
		points.push_back(make_point(Vector3(64, 10, 32), 8.0, 2.0));
		LandscapeSplineTerrainShape shape = make_shape(LandscapeSplineTerrainShape::PROFILE_CHANNEL, points, false, size);
		shape.bank_height = 0.5;
		for (float &h : heights) {
			h = 10.0;
		}
		LandscapeSplineTerrain::apply(shape, 1.0, rect, heights.ptr(), nullptr, 0);
		auto at = [&](int p_x, int p_z) { return heights[p_z * size.x + p_x]; };
		CHECK(at(32, 32) == doctest::Approx(8.0)); // Full depth at the center.
		CHECK(at(32, 34) < 10.0); // Sloped bed.
		CHECK(at(32, 34) > 8.0);
		CHECK(at(32, 36) == doctest::Approx(10.0)); // Water line at the edge.
		CHECK(at(32, 38) > 10.0); // Bank above the water.
		CHECK(at(32, 50) == doctest::Approx(10.0)); // Untouched.
	}

	SUBCASE("Lake basin and painted shore") {
		LocalVector<LandscapeSplinePoint> points;
		points.push_back(make_point(Vector3(16, 0, 16)));
		points.push_back(make_point(Vector3(48, 0, 16)));
		points.push_back(make_point(Vector3(48, 0, 48)));
		points.push_back(make_point(Vector3(16, 0, 48)));
		LandscapeSplineTerrainShape shape;
		shape.profile = LandscapeSplineTerrainShape::PROFILE_LAKE;
		shape.closed = true;
		LocalVector<int> starts;
		LandscapeSplineCurve::Settings settings;
		settings.max_step = 1.0;
		LandscapeSplineCurve::tessellate(points, true, settings, shape.samples, &starts);
		for (LandscapeSplineSample &s : shape.samples) {
			s.position.y = 10.0;
		}
		shape.lake_level = 10.0;
		shape.lake_depth = 3.0;
		shape.shore_width = 4.0;
		shape.falloff = 4.0;
		shape.bank_height = 0.5;
		shape.paint_layer = 1;
		shape.paint_width = 1.0;
		shape.paint_noise = 0.0;
		shape.finalize(starts, 1.0, size, 99);
		CHECK(shape.merge_dirty_blocks);
		CHECK(shape.blocks.size() == 5); // 4 segments and the interior.

		const int layers = 4;
		LocalVector<float> weights;
		weights.resize(size.x * size.y * layers);
		for (uint32_t i = 0; i < heights.size(); i++) {
			heights[i] = 10.0;
			for (int l = 0; l < layers; l++) {
				weights[i * layers + l] = l == 0 ? 1.0 : 0.0;
			}
		}
		LandscapeSplineTerrain::apply(shape, 1.0, rect, heights.ptr(), weights.ptr(), layers);
		auto at = [&](int p_x, int p_z) { return heights[p_z * size.x + p_x]; };
		auto weight = [&](int p_x, int p_z, int p_layer) { return weights[(p_z * size.x + p_x) * layers + p_layer]; };
		// The curve through the corners bulges outwards: find the shore at x = 32.
		real_t shore_z = 32.0;
		for (const LandscapeSplineSample &s : shape.samples) {
			if (Math::abs(s.position.x - 32.0) < 1.0) {
				shore_z = MIN(shore_z, s.position.z);
			}
		}
		CHECK(shore_z < 16.0);
		CHECK(shore_z > 8.0);
		const int shore = int(Math::round(shore_z));
		CHECK(at(32, 32) == doctest::Approx(7.0)); // Full depth in the middle.
		CHECK(at(32, shore + 2) < 10.0); // Shore slope.
		CHECK(at(32, shore + 2) > 7.0);
		CHECK(at(32, shore + 6) == doctest::Approx(7.0));
		CHECK(at(32, shore - 2) > 10.0); // Bank.
		CHECK(at(32, 2) == doctest::Approx(10.0));
		CHECK(weight(32, 32, 1) == doctest::Approx(1.0));
		CHECK(weight(32, 32, 0) == doctest::Approx(0.0));
		CHECK(weight(32, 2, 0) == doctest::Approx(1.0));
		bool normalized = true;
		for (int i = 0; i < size.x * size.y; i++) {
			float sum = 0.0;
			for (int l = 0; l < layers; l++) {
				sum += weights[i * layers + l];
			}
			normalized = normalized && Math::is_equal_approx(sum, 1.0f);
		}
		CHECK(normalized);
	}
}

TEST_CASE("[Landscape][Splines] Base layer") {
	Ref<LandscapeData> data;
	data.instantiate();
	data->create(Vector2i(300, 300), 1.0, 1.0);
	data->ensure_layer_capacity(4);
	CHECK_FALSE(data->has_base());
	CHECK(data->get_tile_count() == Vector2i(3, 3));

	data->capture_base(Vector2i(1, 1));
	CHECK(data->has_base());
	CHECK(data->tile_has_base(Vector2i(1, 1)));
	CHECK_FALSE(data->tile_has_base(Vector2i(0, 0)));
	CHECK(data->has_base_in_rect(Rect2i(120, 120, 16, 16)));
	CHECK_FALSE(data->has_base_in_rect(Rect2i(0, 0, 100, 100)));

	// The final data changes (e.g. a spline is composited), the base keeps the terrain.
	data->set_height(150, 150, 7.0);
	CHECK(data->get_height(150, 150) == doctest::Approx(7.0));
	CHECK(data->get_edit_height(150, 150) == doctest::Approx(1.0));
	data->set_height(20, 20, 3.0); // No base: the edit layer is the final data.
	CHECK(data->get_edit_height(20, 20) == doctest::Approx(3.0));

	// Edits without compositor write through the base and the final data.
	float edit[4] = { 2.0, 2.0, 2.0, 2.0 };
	data->write_edit_heights(Rect2i(150, 150, 2, 2), edit);
	CHECK(data->get_height(150, 150) == doctest::Approx(2.0));
	CHECK(data->get_edit_height(151, 151) == doctest::Approx(2.0));
	float w[LandscapeData::MAX_LAYERS] = {};
	w[2] = 1.0;
	data->set_edit_weights(140, 140, w);
	float r[LandscapeData::MAX_LAYERS];
	data->get_edit_weights(140, 140, r);
	CHECK(r[2] == doctest::Approx(1.0));

	// Snapshots restore the base too.
	const Rect2i region(140, 140, 20, 20);
	const Dictionary snapshot = data->get_region(region);
	CHECK(snapshot.has("base_heights"));
	CHECK(snapshot.has("base_weightmaps"));
	float flat[400];
	for (float &h : flat) {
		h = 9.0;
	}
	data->write_edit_heights(region, flat);
	CHECK(data->get_edit_height(145, 145) == doctest::Approx(9.0));
	data->set_region(snapshot);
	CHECK(data->get_edit_height(150, 150) == doctest::Approx(2.0));
	CHECK(data->get_edit_height(145, 145) == doctest::Approx(1.0));

	// Releasing the base restores the final data from it.
	data->set_height(160, 160, 50.0);
	data->release_base(Vector2i(1, 1));
	CHECK_FALSE(data->has_base());
	CHECK(data->get_height(160, 160) == doctest::Approx(1.0));
	CHECK(data->get_height(150, 150) == doctest::Approx(2.0));
}

TEST_CASE("[Landscape][Splines] Base layer in streamed files and embedded data") {
	const String path = TestUtils::get_temp_path("landscape_base.lsdata");
	{
		Ref<LandscapeData> data;
		data.instantiate();
		data->create(Vector2i(300, 300), 1.0, 1.0);
		data->capture_base(Vector2i(2, 0));
		data->set_height(270, 10, 4.0);
		Dictionary records;
		PackedInt64Array record;
		record.push_back(0);
		records[int64_t(42)] = record;
		data->set_spline_records(records);
		CHECK(data->save_to_file(path) == OK);
	}
	Ref<LandscapeData> loaded;
	loaded.instantiate();
	REQUIRE(loaded->load_from_file(path) == OK);
	CHECK(loaded->has_base());
	CHECK(loaded->tile_has_base(Vector2i(2, 0)));
	CHECK(loaded->get_height(270, 10) == doctest::Approx(4.0));
	CHECK(loaded->get_edit_height(270, 10) == doctest::Approx(1.0));
	CHECK(loaded->get_spline_records().has(int64_t(42)));
	// Tiles without base are still stored in the version 1 layout.
	CHECK(loaded->get_height(20, 20) == doctest::Approx(1.0));

	// Embedded resources keep the base too.
	Ref<LandscapeData> copy;
	copy.instantiate();
	copy->set("size", loaded->get("size"));
	copy->set("heights", loaded->get("heights"));
	copy->set("weightmaps", loaded->get("weightmaps"));
	copy->set("base_tiles", loaded->get("base_tiles"));
	CHECK(copy->tile_has_base(Vector2i(2, 0)));
	CHECK(copy->get_edit_height(270, 10) == doctest::Approx(1.0));
	CHECK(copy->get_height(270, 10) == doctest::Approx(4.0));
	DirAccess::remove_absolute(path);
}

static LandscapeSpline3D *add_road(Landscape3D *p_landscape, real_t p_z, real_t p_height) {
	LandscapeSpline3D *road = memnew(LandscapeSpline3D);
	road->set_type(LandscapeSpline3D::TYPE_ROAD);
	road->apply_type_defaults();
	road->set_width(6.0);
	road->set_terrain_falloff(4.0);
	road->set_mesh_enabled(false);
	PackedVector3Array points;
	points.push_back(Vector3(20, p_height, p_z));
	points.push_back(Vector3(108, p_height, p_z));
	road->set_points(points);
	p_landscape->add_child(road);
	return road;
}

TEST_CASE("[SceneTree][Landscape][Splines] Splines modify the terrain non-destructively") {
	Landscape3D *landscape = memnew(Landscape3D);
	Ref<LandscapeData> data;
	data.instantiate();
	data->create(Vector2i(129, 129), 1.0, 0.0);
	landscape->set_data(data);
	Vector<Ref<LandscapeLayer>> layers;
	for (int i = 0; i < 2; i++) {
		layers.push_back(memnew(LandscapeLayer));
	}
	landscape->set_layers(layers);
	SceneTree::get_singleton()->get_root()->add_child(landscape);

	LandscapeSpline3D *road = add_road(landscape, 64.0, 2.0);
	road->set_paint_layer(1);
	road->set_paint_noise(0.0);
	landscape->update_splines();
	CHECK(landscape->get_splines().size() == 1);
	CHECK(data->get_height(64, 64) == doctest::Approx(1.95));
	CHECK(data->get_height(64, 100) == doctest::Approx(0.0));
	CHECK(data->get_layer_weight(64, 64, 1) == doctest::Approx(1.0));
	CHECK(data->get_edit_height(64, 64) == doctest::Approx(0.0));
	CHECK(data->has_base());
	CHECK(data->get_spline_records().has(road->get_spline_id()));

	// Moving the road restores the terrain it left.
	PackedVector3Array points = road->get_points();
	points.set(0, Vector3(20, 2.0, 30));
	points.set(1, Vector3(108, 2.0, 30));
	road->set_points(points);
	landscape->update_splines();
	CHECK(data->get_height(64, 64) == doctest::Approx(0.0));
	CHECK(data->get_layer_weight(64, 64, 1) == doctest::Approx(0.0));
	CHECK(data->get_height(64, 30) == doctest::Approx(1.95));

	// Runtime edits are written through (e.g. a crater on the road).
	Ref<LandscapeBrush> brush;
	brush.instantiate();
	brush->set_tool(LandscapeBrush::TOOL_FLATTEN);
	brush->set_target_height(-3.0);
	brush->set_strength(10.0);
	brush->set_size(2.0);
	brush->set_falloff(0.0);
	(void)brush->apply(data, Vector3(64, 0, 30), 1.0);
	CHECK(data->get_height(64, 30) < 0.0);
	CHECK(data->get_edit_height(64, 30) < 0.0);

	// Unchanged splines don't touch the terrain again.
	landscape->rebuild_splines();
	CHECK(data->get_height(64, 30) == doctest::Approx(1.95));
	road->set_terrain_priority(road->get_terrain_priority());
	landscape->update_splines();
	CHECK(int(landscape->get_statistics()["spline_composited_tiles"]) == 0);

	// A copy of the spline (same id) gets a new id.
	LandscapeSpline3D *copy = add_road(landscape, 100.0, 1.0);
	copy->set_spline_id(road->get_spline_id());
	landscape->remove_child(copy);
	landscape->add_child(copy);
	CHECK(copy->get_spline_id() != road->get_spline_id());
	landscape->update_splines();
	CHECK(data->get_height(64, 100) == doctest::Approx(0.95));

	// Removed splines are cleaned up.
	landscape->remove_child(copy);
	memdelete(copy);
	landscape->remove_child(road);
	memdelete(road);
	landscape->update_splines();
	CHECK(data->get_height(64, 100) == doctest::Approx(0.0));
	CHECK(data->get_height(64, 30) < 0.0); // The crater was baked in the base.
	CHECK(data->get_height(20, 20) == doctest::Approx(0.0));
	CHECK_FALSE(data->has_base());
	CHECK(data->get_spline_records().is_empty());

	memdelete(landscape);
}

TEST_CASE("[SceneTree][Landscape][Splines] Records survive reloading the scene") {
	Landscape3D *landscape = memnew(Landscape3D);
	Ref<LandscapeData> data;
	data.instantiate();
	data->create(Vector2i(129, 129), 1.0, 0.0);
	landscape->set_data(data);
	SceneTree::get_singleton()->get_root()->add_child(landscape);
	LandscapeSpline3D *road = add_road(landscape, 64.0, 3.0);
	landscape->update_splines();
	CHECK(data->get_height(64, 64) == doctest::Approx(2.95));
	const int64_t id = road->get_spline_id();
	landscape->remove_child(road);
	memdelete(road);

	// The same spline in a new scene: nothing to do.
	LandscapeSpline3D *reloaded = memnew(LandscapeSpline3D);
	reloaded->set_type(LandscapeSpline3D::TYPE_ROAD);
	reloaded->apply_type_defaults();
	reloaded->set_width(6.0);
	reloaded->set_terrain_falloff(4.0);
	reloaded->set_mesh_enabled(false);
	PackedVector3Array points;
	points.push_back(Vector3(20, 3.0, 64));
	points.push_back(Vector3(108, 3.0, 64));
	reloaded->set_points(points);
	reloaded->set_spline_id(id);
	landscape->add_child(reloaded);
	landscape->update_splines();
	CHECK(int(landscape->get_statistics()["spline_composited_tiles"]) == 0);
	CHECK(data->get_height(64, 64) == doctest::Approx(2.95));

	// Disabling the terrain of a spline restores the terrain.
	reloaded->set_terrain_enabled(false);
	landscape->update_splines();
	CHECK(data->get_height(64, 64) == doctest::Approx(0.0));
	memdelete(landscape);
}

static void check_front_faces(const Ref<ArrayMesh> &p_mesh) {
	REQUIRE(p_mesh.is_valid());
	REQUIRE(p_mesh->get_surface_count() == 1);
	const Array arrays = p_mesh->surface_get_arrays(0);
	const PackedVector3Array vertices = arrays[Mesh::ARRAY_VERTEX];
	const PackedVector3Array normals = arrays[Mesh::ARRAY_NORMAL];
	const PackedInt32Array indices = arrays[Mesh::ARRAY_INDEX];
	REQUIRE(indices.size() > 0);
	int back_faces = 0;
	for (int i = 0; i < indices.size(); i += 3) {
		const Vector3 a = vertices[indices[i]];
		const Vector3 b = vertices[indices[i + 1]];
		const Vector3 c = vertices[indices[i + 2]];
		const Vector3 n = normals[indices[i]] + normals[indices[i + 1]] + normals[indices[i + 2]];
		// Front faces are clockwise: the counter-clockwise normal points away from the viewer.
		if ((b - a).cross(c - a).dot(n) > 0.0) {
			back_faces++;
		}
	}
	CHECK(back_faces == 0);
}

TEST_CASE("[Landscape][Splines] Extruded meshes") {
	LandscapeSpline3D *spline = memnew(LandscapeSpline3D);
	PackedVector3Array points;
	points.push_back(Vector3(0, 0, 0));
	points.push_back(Vector3(50, 2, 20));
	points.push_back(Vector3(100, 0, 0));
	points.push_back(Vector3(150, -1, 40));

	SUBCASE("Road with skirts") {
		spline->set_type(LandscapeSpline3D::TYPE_ROAD);
		spline->apply_type_defaults();
		spline->set_points(points);
		spline->set_point_tilt(1, 0.2);
		const Ref<ArrayMesh> mesh = spline->create_mesh();
		check_front_faces(mesh);
		const Array arrays = mesh->surface_get_arrays(0);
		const PackedVector2Array uvs = arrays[Mesh::ARRAY_TEX_UV];
		real_t max_v = 0.0;
		for (const Vector2 &uv : uvs) {
			max_v = MAX(max_v, uv.y);
		}
		CHECK(max_v == doctest::Approx(spline->get_length()));
		CHECK(mesh->get_aabb().position.y < -0.2); // Skirts.
	}
	SUBCASE("River surface") {
		spline->set_type(LandscapeSpline3D::TYPE_RIVER);
		spline->apply_type_defaults();
		spline->set_points(points);
		const Ref<ArrayMesh> mesh = spline->create_mesh();
		check_front_faces(mesh);
		const Array arrays = mesh->surface_get_arrays(0);
		const PackedVector2Array uv2s = arrays[Mesh::ARRAY_TEX_UV2];
		CHECK(uv2s[0].x == doctest::Approx(spline->get_flow_speed()));
	}
	SUBCASE("Lake surface") {
		spline->set_type(LandscapeSpline3D::TYPE_LAKE);
		spline->apply_type_defaults();
		PackedVector3Array shore;
		shore.push_back(Vector3(0, 3, 0));
		shore.push_back(Vector3(60, 5, 0));
		shore.push_back(Vector3(60, 1, 40));
		shore.push_back(Vector3(0, 2, 40));
		spline->set_points(shore);
		CHECK(spline->is_closed());
		const Ref<ArrayMesh> mesh = spline->create_mesh();
		check_front_faces(mesh);
		const AABB aabb = mesh->get_aabb();
		CHECK(aabb.size.y == doctest::Approx(0.0)); // Flat, at the height of the node.
		real_t shore_min_x = 0.0;
		for (const Vector3 &p : spline->get_tessellated_points()) {
			shore_min_x = MIN(shore_min_x, p.x);
		}
		// Continues under the banks, by the overlap and at most a bit more than a cell of the grid.
		const real_t cell = spline->get_sample_spacing() * 2.0;
		CHECK(aabb.position.x < shore_min_x - spline->get_water_overlap() * 0.5);
		CHECK(aabb.position.x > shore_min_x - spline->get_water_overlap() - cell * 1.25 - 0.01);
		CHECK(aabb.get_end().z > 40.0);
	}
	memdelete(spline);
}

TEST_CASE("[SceneTree][Landscape][Splines] Surface queries") {
	LandscapeSpline3D *river = memnew(LandscapeSpline3D);
	river->set_type(LandscapeSpline3D::TYPE_RIVER);
	river->apply_type_defaults();
	river->set_width(10.0);
	river->set_flow_speed(2.0);
	PackedVector3Array points;
	points.push_back(Vector3(0, 5, 0));
	points.push_back(Vector3(100, 5, 0));
	river->set_points(points);
	SceneTree::get_singleton()->get_root()->add_child(river);

	CHECK(river->get_length() == doctest::Approx(100.0));
	const Dictionary info = river->get_surface_info(Vector3(50, 3, 2));
	CHECK(bool(info["inside"]));
	CHECK(real_t(info["height"]) == doctest::Approx(5.0));
	CHECK(Vector3(info["flow"]).is_equal_approx(Vector3(2, 0, 0)));
	CHECK(river->is_point_in_water(Vector3(50, 3, 2)));
	CHECK_FALSE(river->is_point_in_water(Vector3(50, 6, 2))); // Above the surface.
	CHECK_FALSE(river->is_point_in_water(Vector3(50, 3, 20)));
	CHECK_FALSE(river->is_point_in_water(Vector3(-10, 3, 0))); // Beyond the start.
	CHECK(river->sample_position(25.0).is_equal_approx(Vector3(25, 5, 0)));
	const Transform3D xform = river->sample_transform(25.0);
	CHECK((-xform.basis.get_column(2)).is_equal_approx(Vector3(1, 0, 0)));

	// Exact Bezier form of the curve.
	const Ref<Curve3D> curve = river->create_curve();
	REQUIRE(curve->get_point_count() == 2);
	CHECK(curve->sample(0, 0.5).is_equal_approx(Vector3(50, 5, 0)));

	// Reversing flips the flow.
	river->reverse();
	CHECK(Vector3(river->get_surface_info(Vector3(50, 3, 2))["flow"]).is_equal_approx(Vector3(-2, 0, 0)));

	// Lakes: the surface is at the height of the node.
	LandscapeSpline3D *lake = memnew(LandscapeSpline3D);
	lake->set_type(LandscapeSpline3D::TYPE_LAKE);
	lake->apply_type_defaults();
	PackedVector3Array shore;
	shore.push_back(Vector3(0, 1, 0));
	shore.push_back(Vector3(40, 2, 0));
	shore.push_back(Vector3(40, 0, 40));
	shore.push_back(Vector3(0, 3, 40));
	lake->set_points(shore);
	lake->set_position(Vector3(200, 7, 0));
	SceneTree::get_singleton()->get_root()->add_child(lake);
	CHECK(lake->is_point_in_water(Vector3(220, 6, 20)));
	CHECK_FALSE(lake->is_point_in_water(Vector3(220, 8, 20)));
	CHECK_FALSE(lake->is_point_in_water(Vector3(260, 6, 20)));
	CHECK(real_t(lake->get_surface_info(Vector3(220, 0, 20))["height"]) == doctest::Approx(7.0));

	memdelete(lake);
	memdelete(river);
}

TEST_CASE("[SceneTree][Landscape][Splines] Automatic flow speed from width") {
	LandscapeSpline3D *river = memnew(LandscapeSpline3D);
	river->set_type(LandscapeSpline3D::TYPE_RIVER);
	river->apply_type_defaults();
	river->set_flow_speed(2.0);
	river->set_flow_reference_width(10.0);
	PackedVector3Array points;
	points.push_back(Vector3(0, 5, 0));
	points.push_back(Vector3(100, 5, 0));
	river->set_points(points);
	river->set_point_width(0, 10.0);
	river->set_point_width(1, 5.0); // Narrows to half the reference width.
	SceneTree::get_singleton()->get_root()->add_child(river);

	// Disabled by default: the authored speed applies everywhere, regardless of width.
	CHECK(Vector3(river->get_surface_info(Vector3(0, 3, 0))["flow"]).is_equal_approx(Vector3(2, 0, 0)));
	CHECK(Vector3(river->get_surface_info(Vector3(100, 3, 0))["flow"]).is_equal_approx(Vector3(2, 0, 0)));

	river->set_flow_auto_width(true);
	// At the reference width, the speed is unchanged.
	CHECK(Vector3(river->get_surface_info(Vector3(0, 3, 0))["flow"]).is_equal_approx(Vector3(2, 0, 0)));
	// Half the reference width: twice the speed, keeping width * speed constant (continuity).
	CHECK(Vector3(river->get_surface_info(Vector3(100, 3, 0))["flow"]).is_equal_approx(Vector3(4, 0, 0)));

	// The mesh's UV2 (read by the water shader for flow mapping) carries the same speed.
	const Ref<ArrayMesh> mesh = river->create_mesh();
	const Array arrays = mesh->surface_get_arrays(0);
	const PackedVector2Array uv2s = arrays[Mesh::ARRAY_TEX_UV2];
	real_t max_speed = 0.0;
	for (const Vector2 &uv2 : uv2s) {
		max_speed = MAX(max_speed, uv2.x);
	}
	CHECK(uv2s[0].x == doctest::Approx(2.0));
	CHECK(max_speed == doctest::Approx(4.0));

	memdelete(river);
}

TEST_CASE("[Landscape][Splines] Built-in materials") {
	Ref<LandscapeWaterMaterial> water;
	water.instantiate();
	CHECK(water->get_shader_mode() == Shader::MODE_SPATIAL);
	CHECK(real_t(water->get("clarity")) == doctest::Approx(3.0));
	water->set("clarity", 5.0);
	CHECK(real_t(water->get("clarity")) == doctest::Approx(5.0));
	CHECK(Color(water->get("deep_color")).is_equal_approx(Color(0.04, 0.17, 0.22)));
	CHECK(water->find_parameter("normal_texture") >= 0);
	CHECK(LandscapeWaterMaterial::get_builtin_shader_code().contains("shader_type spatial;"));
	CHECK(LandscapeWaterMaterial::get_default_normal_texture()->get_width() == 256);
	List<PropertyInfo> properties;
	water->get_property_list(&properties);
	bool found = false;
	for (const PropertyInfo &property : properties) {
		found = found || property.name == "shore_foam";
	}
	CHECK(found);

	Ref<LandscapeRoadMaterial> road;
	road.instantiate();
	CHECK(bool(road->get("lane_markings")));
	road->apply_preset(LandscapeRoadMaterial::PRESET_DIRT);
	CHECK_FALSE(bool(road->get("lane_markings")));
	road->set("lane_count", 4.0); // Converted to the type of the parameter.
	CHECK(road->get("lane_count").get_type() == Variant::INT);
	CHECK(int(road->get("lane_count")) == 4);
	CHECK(LandscapeRoadMaterial::get_builtin_shader_code().contains("ALPHA_SCISSOR_THRESHOLD"));
	CHECK(LandscapeSpline3D::get_default_material(LandscapeSpline3D::TYPE_LAKE).is_valid());

	// The shaders compile (their uniforms are known), with the preprocessor directives resolved.
	CHECK(water->get_shader_code().contains("#if CURRENT_RENDERER"));
	for (const Ref<LandscapeSplineMaterial> &material : { Ref<LandscapeSplineMaterial>(water), Ref<LandscapeSplineMaterial>(road) }) {
		List<PropertyInfo> parameters;
		RenderingServer::get_singleton()->get_shader_parameter_list(material->get_shader_rid(), &parameters);
		bool has_roughness = false;
		for (const PropertyInfo &parameter : parameters) {
			has_roughness = has_roughness || parameter.name == "roughness";
		}
		CHECK(has_roughness);
	}
}

} // namespace TestLandscapeSplines
