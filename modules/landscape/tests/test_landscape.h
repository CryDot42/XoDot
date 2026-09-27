/**************************************************************************/
/*  test_landscape.h                                                      */
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

#include "../landscape_brush.h"
#include "../landscape_data.h"
#include "../landscape_lod_tree.h"

#include "tests/test_macros.h"

namespace TestLandscape {

static Ref<LandscapeData> make_data(int p_size, float p_height = 0.0) {
	Ref<LandscapeData> data;
	data.instantiate();
	data->create(Vector2i(p_size, p_size), 1.0, p_height);
	return data;
}

TEST_CASE("[Landscape][LandscapeData] Creation and height sampling") {
	Ref<LandscapeData> data = make_data(65, 2.0);
	CHECK(data->is_valid());
	CHECK(data->get_size() == Vector2i(65, 65));
	CHECK(data->get_world_size().is_equal_approx(Vector2(64, 64)));
	CHECK(data->get_height(10, 10) == doctest::Approx(2.0));
	CHECK(data->get_height_range().is_equal_approx(Vector2(2.0, 2.0)));

	data->set_height(10, 10, 4.0);
	data->set_height(11, 10, 6.0);
	// Bilinear interpolation between texels.
	CHECK(data->sample_height(10.5, 10.0) == doctest::Approx(5.0));
	CHECK(data->sample_height(10.0, 10.0) == doctest::Approx(4.0));
	CHECK(data->get_height_range().is_equal_approx(Vector2(2.0, 6.0)));
	// Out of bounds coordinates are clamped.
	CHECK(data->get_height(-5, 1000) == doctest::Approx(2.0));

	// A flat landscape has vertical normals.
	Ref<LandscapeData> flat = make_data(17);
	CHECK(flat->sample_normal(8.0, 8.0).is_equal_approx(Vector3(0, 1, 0)));
}

TEST_CASE("[Landscape][LandscapeData] Layer weights stay normalized") {
	Ref<LandscapeData> data = make_data(33);
	CHECK(data->get_weightmap_count() == 1);
	CHECK(data->get_layer_weight(5, 5, 0) == doctest::Approx(1.0));

	data->set_layer_weight(5, 5, 6, 0.5);
	CHECK(data->get_weightmap_count() == 2);
	CHECK(data->get_layer_weight(5, 5, 6) == doctest::Approx(0.5).epsilon(0.01));
	CHECK(data->get_layer_weight(5, 5, 0) == doctest::Approx(0.5).epsilon(0.01));

	float weights[LandscapeData::MAX_LAYERS];
	data->get_weights(5, 5, weights);
	float total = 0.0;
	for (float w : weights) {
		total += w;
	}
	CHECK(total == doctest::Approx(1.0).epsilon(0.001));
	CHECK(data->get_dominant_layer(0, 0) == 0);

	data->fill_layer(3);
	CHECK(data->get_layer_weight(20, 20, 3) == doctest::Approx(1.0));
	CHECK(data->get_layer_weight(20, 20, 0) == doctest::Approx(0.0));

	// Removing a layer shifts the following layers down.
	data->remove_layer(1);
	CHECK(data->get_layer_weight(20, 20, 2) == doctest::Approx(1.0));
	CHECK(data->get_layer_weight(20, 20, 3) == doctest::Approx(0.0));
}

TEST_CASE("[Landscape][LandscapeData] Region snapshots") {
	Ref<LandscapeData> data = make_data(33, 1.0);
	const Rect2i rect(4, 4, 8, 8);
	const Dictionary before = data->get_region(rect);
	CHECK(Rect2i(before["rect"]) == rect);

	for (int z = 0; z < 33; z++) {
		for (int x = 0; x < 33; x++) {
			data->set_height(x, z, 10.0);
		}
	}
	data->set_region(before);
	CHECK(data->get_height(5, 5) == doctest::Approx(1.0));
	CHECK(data->get_height(20, 20) == doctest::Approx(10.0));

	// Regions are clipped to the landscape.
	const Dictionary clipped = data->get_region(Rect2i(30, 30, 10, 10), true, false);
	CHECK(Rect2i(clipped["rect"]) == Rect2i(30, 30, 3, 3));
	CHECK(PackedFloat32Array(clipped["heights"]).size() == 9);
	CHECK_FALSE(clipped.has("weightmaps"));
}

TEST_CASE("[Landscape][LandscapeData] Holes") {
	Ref<LandscapeData> data = make_data(33, 1.0);
	CHECK_FALSE(data->has_holes());
	const Dictionary before = data->get_region(Rect2i(0, 0, 16, 16), true, false);
	CHECK_FALSE(before.has("holes"));

	Ref<LandscapeBrush> brush;
	brush.instantiate();
	brush->set_tool(LandscapeBrush::TOOL_HOLES);
	brush->set_size(3.0);
	brush->set_falloff(0.0);
	(void)brush->apply(data, Vector3(8, 0, 8));
	CHECK(data->has_holes());
	CHECK(data->is_hole(8, 8));
	CHECK_FALSE(data->is_hole(20, 20));

	// Holes are part of the height snapshots.
	const Dictionary with_holes = data->get_region(Rect2i(0, 0, 16, 16), true, false);
	CHECK(with_holes.has("holes"));

	// Restoring a snapshot taken before any hole existed fills the holes again.
	data->set_region(before);
	CHECK_FALSE(data->is_hole(8, 8));
	data->set_region(with_holes);
	CHECK(data->is_hole(8, 8));

	brush->set_invert(true);
	(void)brush->apply(data, Vector3(8, 0, 8));
	CHECK_FALSE(data->is_hole(8, 8));
}

TEST_CASE("[Landscape][LandscapeLodTree] Bounds and monotonic error") {
	Ref<LandscapeData> data = make_data(129, 0.0);
	LandscapeLodTree tree;
	tree.build(data.ptr(), 16);
	REQUIRE(tree.is_valid());
	CHECK(tree.get_max_level() == 3); // 16 << 3 = 128 quads.
	CHECK(tree.get_root_quads() == 128);
	CHECK(tree.get_node_count() == 1 + 4 + 16 + 64);
	CHECK(tree.get_root_error() == doctest::Approx(0.0));

	// A single spike creates an error in every ancestor of the leaf containing it.
	data->set_height(37, 41, 8.0);
	LocalVector<LandscapeLodTree::Range> ranges;
	tree.update(data.ptr(), Rect2i(37, 41, 1, 1), &ranges);
	CHECK(ranges.size() == 4); // One range per level.
	CHECK(tree.get_height_range().is_equal_approx(Vector2(0.0, 8.0)));
	CHECK(tree.get_root_error() == doctest::Approx(8.0));

	float mn = 0.0;
	float mx = 0.0;
	float err = 0.0;
	// The leaf (level 3, 16 texels) containing the spike is exact at full resolution.
	CHECK(tree.get_node(3, 37 / 16, 41 / 16, mn, mx, err));
	CHECK(mx == doctest::Approx(8.0));
	CHECK(err == doctest::Approx(0.0));
	// Its parent can't represent the spike (odd texel coordinates).
	CHECK(tree.get_node(2, 37 / 32, 41 / 32, mn, mx, err));
	CHECK(err == doctest::Approx(8.0));
	// A node far from the spike is unaffected.
	CHECK(tree.get_node(3, 7, 7, mn, mx, err));
	CHECK(mx == doctest::Approx(0.0));
	CHECK(err == doctest::Approx(0.0));

	// Errors never decrease towards the root.
	for (int l = 0; l < tree.get_max_level(); l++) {
		const int count = 1 << l;
		for (int z = 0; z < count; z++) {
			for (int x = 0; x < count; x++) {
				float parent_min, parent_max, parent_error;
				tree.get_node(l, x, z, parent_min, parent_max, parent_error);
				for (int c = 0; c < 4; c++) {
					float child_min, child_max, child_error;
					if (tree.get_node(l + 1, x * 2 + (c & 1), z * 2 + (c >> 1), child_min, child_max, child_error)) {
						CHECK(parent_error >= child_error);
						CHECK(parent_min <= child_min);
						CHECK(parent_max >= child_max);
					}
				}
			}
		}
	}
}

TEST_CASE("[Landscape][LandscapeLodTree] Non power of two sizes") {
	Ref<LandscapeData> data;
	data.instantiate();
	data->create(Vector2i(100, 40), 2.0, 3.0);
	LandscapeLodTree tree;
	tree.build(data.ptr(), 32);
	REQUIRE(tree.is_valid());
	CHECK(tree.get_root_quads() >= 99);
	CHECK(tree.get_height_range().is_equal_approx(Vector2(3.0, 3.0)));
	float mn, mx, err;
	// Nodes outside of the landscape are marked invalid.
	CHECK_FALSE(tree.get_node(tree.get_max_level(), 3, 3, mn, mx, err));
}

TEST_CASE("[Landscape][LandscapeBrush] Sculpt, flatten and paint") {
	Ref<LandscapeData> data = make_data(129, 10.0);
	Ref<LandscapeBrush> brush;
	brush.instantiate();

	CHECK(brush->get_weight(0.0, 0.0) == doctest::Approx(1.0));
	CHECK(brush->get_weight(1.5, 0.0) == doctest::Approx(0.0));
	brush->set_falloff(1.0);
	brush->set_falloff_type(LandscapeBrush::FALLOFF_LINEAR);
	CHECK(brush->get_weight(0.5, 0.0) == doctest::Approx(0.5));

	brush->set_tool(LandscapeBrush::TOOL_SCULPT);
	brush->set_size(10.0);
	brush->set_strength(1.0);
	const Rect2i rect = brush->apply(data, Vector3(64, 0, 64), 1.0);
	CHECK(rect.has_point(Point2i(64, 64)));
	CHECK(data->get_height(64, 64) > 10.0);
	CHECK(data->get_height(0, 0) == doctest::Approx(10.0));

	brush->set_invert(true);
	const float raised = data->get_height(64, 64);
	(void)brush->apply(data, Vector3(64, 0, 64), 0.5);
	CHECK(data->get_height(64, 64) < raised);
	brush->set_invert(false);

	brush->set_tool(LandscapeBrush::TOOL_FLATTEN);
	brush->set_falloff(0.0);
	brush->set_target_height(3.0);
	for (int i = 0; i < 10; i++) {
		(void)brush->apply(data, Vector3(30, 0, 30), 1.0);
	}
	CHECK(data->get_height(30, 30) == doctest::Approx(3.0));

	brush->set_tool(LandscapeBrush::TOOL_PAINT);
	brush->set_layer(2);
	for (int i = 0; i < 10; i++) {
		(void)brush->apply(data, Vector3(90, 0, 90), 1.0);
	}
	CHECK(data->get_layer_weight(90, 90, 2) == doctest::Approx(1.0));
	CHECK(data->get_layer_weight(90, 90, 0) == doctest::Approx(0.0));
	CHECK(data->get_layer_weight(10, 10, 0) == doctest::Approx(1.0));

	brush->set_invert(true);
	for (int i = 0; i < 10; i++) {
		(void)brush->apply(data, Vector3(90, 0, 90), 1.0);
	}
	CHECK(data->get_layer_weight(90, 90, 2) == doctest::Approx(0.0));
}

TEST_CASE("[Landscape][LandscapeBrush] Ramp") {
	Ref<LandscapeData> data = make_data(129, 0.0);
	Ref<LandscapeBrush> brush;
	brush.instantiate();
	brush->set_ramp_width(8.0);
	brush->set_falloff(0.0);
	(void)brush->apply_ramp(data, Vector3(20, 0, 64), Vector3(100, 40, 64));
	CHECK(data->get_height(20, 64) == doctest::Approx(0.0));
	CHECK(data->get_height(60, 64) == doctest::Approx(20.0));
	CHECK(data->get_height(100, 64) == doctest::Approx(40.0));
	CHECK(data->get_height(60, 100) == doctest::Approx(0.0)); // Outside of the ramp.
}

} // namespace TestLandscape
