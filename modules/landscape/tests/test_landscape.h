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
#include "../landscape_horizon.h"
#include "../landscape_lod_tree.h"

#include "core/io/dir_access.h"
#include "core/io/resource_loader.h"
#include "tests/test_macros.h"
#include "tests/test_utils.h"

#include "modules/streaming/world_streaming.h"

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
	tree.build(data->get_storage(), 16);
	REQUIRE(tree.is_valid());
	CHECK(tree.get_max_level() == 3); // 16 << 3 = 128 quads.
	CHECK(tree.get_root_quads() == 128);
	CHECK(tree.get_node_count() == 1 + 4 + 16 + 64);
	CHECK(tree.get_root_error() == doctest::Approx(0.0));

	// A single spike creates an error in every ancestor of the leaf containing it.
	data->set_height(37, 41, 8.0);
	LocalVector<LandscapeLodTree::Range> ranges;
	tree.update(data->get_storage(), Rect2i(37, 41, 1, 1), &ranges);
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
	tree.build(data->get_storage(), 32);
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

TEST_CASE("[Landscape][LandscapeStorage] Mip levels hold exact heights and filtered weights") {
	Ref<LandscapeData> data;
	data.instantiate();
	data->create(Vector2i(300, 201), 1.0, 0.0);
	for (int z = 0; z < 201; z++) {
		for (int x = 0; x < 300; x++) {
			data->set_height(x, z, float((x * 7 + z * 13) % 31));
		}
	}
	data->notify_region_changed(Rect2i(0, 0, 300, 201), LandscapeData::CHANGED_HEIGHTS);

	LandscapeStorage &storage = data->get_storage();
	CHECK(storage.get_domain() == 512);
	CHECK(storage.get_mip_count() == 7); // 512 -> 8 texels.
	for (int m = 1; m < storage.get_mip_count(); m++) {
		const Vector2i last = storage.get_mip_last(m);
		CHECK(last == Vector2i((299 + (1 << m) - 1) >> m, (200 + (1 << m) - 1) >> m));
		bool exact = true;
		for (int z = 0; z <= last.y; z += 3) {
			for (int x = 0; x <= last.x; x += 5) {
				// Mip m texel i is the mip 0 texel min(i << m, size - 1).
				exact = exact && storage.get_mip_height(m, x, z) == data->get_height(MIN(x << m, 299), MIN(z << m, 200));
			}
		}
		CHECK_MESSAGE(exact, vformat("Mip %d heights must be exact.", m));
	}

	// Filtered weights stay normalized.
	data->fill_layer(2);
	uint8_t w[4];
	storage.read_region(LandscapeStorage::LAYER_WEIGHTS_0, 3, Rect2i(5, 5, 1, 1), w);
	CHECK(w[2] == 255);
	CHECK(w[0] == 0);
}

TEST_CASE("[Landscape][LandscapeStorage] Streamed file round trip and eviction") {
	const String path = TestUtils::get_temp_path("landscape_roundtrip.lsdata");
	{
		Ref<LandscapeData> data;
		data.instantiate();
		data->create(Vector2i(1025, 1025), 2.0, 5.0);
		data->ensure_layer_capacity(8);
		data->set_height(700, 300, 42.0);
		data->set_layer_weight(10, 1000, 6, 1.0);
		data->set_hole(512, 512, true);
		data->notify_region_changed(Rect2i(0, 0, 1025, 1025), LandscapeData::CHANGED_ALL);
		CHECK(data->get_lod_tree(32) != nullptr);
		CHECK(data->save_to_file(path) == OK);
		CHECK(data->is_streamed());
		CHECK_FALSE(data->has_unsaved_changes());
	}

	Ref<LandscapeData> loaded;
	loaded.instantiate();
	REQUIRE(loaded->load_from_file(path) == OK);
	CHECK(loaded->get_size() == Vector2i(1025, 1025));
	CHECK(loaded->get_vertex_spacing() == doctest::Approx(2.0));
	CHECK(loaded->get_weightmap_count() == 2);
	CHECK(loaded->get_memory_usage() == 0); // Nothing is loaded until needed.
	CHECK(loaded->get_height(700, 300) == doctest::Approx(42.0));
	CHECK(loaded->get_height(0, 0) == doctest::Approx(5.0));
	CHECK(loaded->get_layer_weight(10, 1000, 6) == doctest::Approx(1.0));
	CHECK(loaded->has_holes());
	CHECK(loaded->is_hole(512, 512));
	CHECK_FALSE(loaded->is_hole(100, 100));
	CHECK(loaded->get_height_range().is_equal_approx(Vector2(5.0, 42.0)));

	// The saved LOD tree matches a rebuilt one.
	const LandscapeLodTree *tree = loaded->get_lod_tree(32);
	REQUIRE(tree != nullptr);
	LandscapeLodTree rebuilt;
	rebuilt.build(loaded->get_storage(), 32);
	CHECK(rebuilt.get_nodes().size() == tree->get_nodes().size());
	CHECK(memcmp(rebuilt.get_nodes().ptr(), tree->get_nodes().ptr(), tree->get_nodes().size() * sizeof(float)) == 0);

	// Modify every tile with a tiny cache: modified tiles are spilled to the overlay file.
	LandscapeStorage &storage = loaded->get_storage();
	storage.set_memory_budget(0); // Clamped to the minimum budget.
	for (int z = 0; z < 1025; z += 64) {
		for (int x = 0; x < 1025; x += 64) {
			loaded->set_height(x, z, float(x + z));
		}
		storage.trim();
	}
	CHECK(storage.get_memory_usage() <= storage.get_memory_budget());
	CHECK(loaded->has_unsaved_changes());
	bool values = true;
	for (int z = 0; z < 1025; z += 64) {
		for (int x = 0; x < 1025; x += 64) {
			values = values && loaded->get_height(x, z) == float(x + z);
		}
	}
	CHECK(values);
	CHECK(loaded->get_height(700, 300) == doctest::Approx(42.0));

	// Saving over the file being streamed.
	CHECK(loaded->save_to_file(path) == OK);
	Ref<LandscapeData> reloaded;
	reloaded.instantiate();
	REQUIRE(reloaded->load_from_file(path) == OK);
	CHECK(reloaded->get_height(128, 64) == doctest::Approx(192.0));
	CHECK(reloaded->get_height(700, 300) == doctest::Approx(42.0));
	CHECK(reloaded->is_hole(512, 512));

	DirAccess::remove_absolute(path);
}

class TestLandscapeJob : public StreamingJob {
public:
	int value = 0;
	bool finished = false;

protected:
	void run() override { value = 42; }
	void finish() override { finished = true; }
};

TEST_CASE("[Landscape][WorldStreaming] Jobs") {
	WorldStreaming *ws = WorldStreaming::get_singleton();
	if (!ws) {
		return;
	}
	Ref<TestLandscapeJob> job;
	job.instantiate();
	ws->submit(job, 1.0);
	ws->wait(job);
	CHECK(job->value == 42);
	CHECK(job->finished);
	CHECK(job->get_state() == StreamingJob::STATE_FINISHED);

	Ref<TestLandscapeJob> cancelled;
	cancelled.instantiate();
	ws->cancel(cancelled); // Not submitted: nothing happens.
	CHECK_FALSE(cancelled->finished);

	ws->set_pool_limit("Test Pool", 100);
	ws->add_pool_usage("Test Pool", 150);
	CHECK(ws->is_pool_over_limit("Test Pool"));
	ws->add_pool_usage("Test Pool", -150);
	CHECK_FALSE(ws->is_pool_over_limit("Test Pool"));
}

// A flat 256 x 256 m landscape with a ridge across it, around x = 128: 40 m high, 48 m wide at the
// top and 112 m at the bottom (the horizon relies on the lowest heights of the patches of the LOD tree).
static Ref<LandscapeData> make_ridge() {
	Ref<LandscapeData> data;
	data.instantiate();
	data->create(Vector2i(257, 257), 1.0, 0.0);
	Ref<Image> heights = Image::create_empty(257, 257, false, Image::FORMAT_RF);
	for (int z = 0; z < 257; z++) {
		for (int x = 0; x < 257; x++) {
			heights->set_pixel(x, z, Color(40.0 * CLAMP((56.0 - Math::abs(x - 128.0)) / 32.0, 0.0, 1.0), 0, 0));
		}
	}
	data->set_heightmap_image(heights);
	return data;
}

// Whether the segment between two points goes below the terrain (sampled every 10 cm).
static bool segment_hits_terrain(const Ref<LandscapeData> &p_data, const Vector3 &p_from, const Vector3 &p_to) {
	const int steps = int(p_from.distance_to(p_to) * 10.0);
	for (int i = 1; i < steps; i++) {
		const Vector3 point = p_from.lerp(p_to, real_t(i) / steps);
		if (point.y < p_data->sample_height(point.x, point.z)) {
			return true;
		}
	}
	return false;
}

TEST_CASE("[Landscape][LandscapeHorizon] Terrain hiding what is behind a ridge") {
	Ref<LandscapeData> data = make_ridge();
	const LandscapeLodTree *tree = data->get_lod_tree(32);
	REQUIRE(tree != nullptr);
	LandscapeHorizon horizon;
	CHECK_FALSE(horizon.is_sphere_hidden(Vector3(230, 1, 128), 1.0));

	LandscapeHorizon::Params params;
	params.camera = Vector3(20, 2, 128);
	params.range = 300.0;
	horizon.build(*tree, 1.0, params, nullptr);
	REQUIRE(horizon.is_valid());
	const uint64_t version = horizon.get_version();

	// Behind the ridge, low: hidden. In front of it or high above it: visible.
	CHECK(horizon.is_sphere_hidden(Vector3(230, 1, 128), 1.0));
	CHECK(horizon.is_sphere_hidden(Vector3(220, 5, 90), 3.0));
	CHECK_FALSE(horizon.is_sphere_hidden(Vector3(60, 1, 128), 1.0));
	CHECK_FALSE(horizon.is_sphere_hidden(Vector3(230, 150, 128), 1.0));
	CHECK(horizon.is_aabb_hidden(AABB(Vector3(200, 0, 100), Vector3(30, 5, 30))));
	CHECK_FALSE(horizon.is_aabb_hidden(AABB(Vector3(40, 0, 100), Vector3(30, 5, 30))));
	CHECK_FALSE(horizon.is_aabb_hidden(AABB(Vector3(10, 0, 120), Vector3(20, 5, 20)))); // Around the camera.

	// Conservative: what is hidden is below the terrain seen from the camera.
	int hidden = 0;
	for (int z = 8; z < 256; z += 12) {
		for (int x = 8; x < 256; x += 12) {
			for (real_t y : { real_t(0.5), real_t(10.0), real_t(30.0) }) {
				const Vector3 top(x, y + data->sample_height(x, z), z);
				if (horizon.is_sphere_hidden(top - Vector3(0, 0.5, 0), 0.5)) {
					hidden++;
					CHECK_MESSAGE(segment_hits_terrain(data, params.camera, top), vformat("%s is hidden but visible from the camera.", top));
				}
			}
		}
	}
	CHECK(hidden > 100);

	// The margins lower the horizon.
	const float before = horizon.get_horizon(0.0, 250.0);
	CHECK(before > 0.0);
	params.angle_margin = 0.05;
	params.height_margin = 1.0;
	horizon.build(*tree, 1.0, params, nullptr);
	CHECK(horizon.get_version() != version);
	CHECK(horizon.get_horizon(0.0, 250.0) < before - 0.05);

	// Seen from above the ridge: nothing hidden. From outside of the terrain: only by the terrain.
	params.angle_margin = 0.0;
	params.height_margin = 0.0;
	params.camera = Vector3(128, 100, 128);
	horizon.build(*tree, 1.0, params, nullptr);
	CHECK_FALSE(horizon.is_sphere_hidden(Vector3(230, 1, 128), 1.0));
	params.camera = Vector3(-100, 2, 128);
	horizon.build(*tree, 1.0, params, nullptr);
	CHECK(horizon.is_sphere_hidden(Vector3(230, 1, 128), 1.0));
	CHECK_FALSE(horizon.is_sphere_hidden(Vector3(-50, 1, 128), 1.0));
	horizon.clear();
	CHECK_FALSE(horizon.is_valid());
}

TEST_CASE("[Landscape][LandscapeHorizon] Holes in the terrain hide nothing") {
	Ref<LandscapeData> data = make_ridge();
	// A tunnel through the ridge.
	for (int z = 124; z <= 132; z++) {
		for (int x = 68; x <= 188; x++) {
			data->set_hole(x, z, true);
		}
	}
	const LandscapeLodTree *tree = data->get_lod_tree(32);
	const int leaves = 1 << tree->get_max_level();
	HashSet<Vector2i> cells;
	data->get_hole_cells(Rect2i(0, 0, 257, 257), 32, cells);
	CHECK(cells.has(Vector2i(4, 3)));
	CHECK(cells.has(Vector2i(3, 4)));
	CHECK_FALSE(cells.has(Vector2i(0, 0)));
	LandscapeHorizon::Holes holes;
	holes.set_leaves(tree->get_max_level(), Rect2i(0, 0, leaves, leaves), cells);
	CHECK(holes.has_any());
	CHECK(holes.has_hole(0, 0, 0));
	CHECK(holes.has_hole(tree->get_max_level(), 4, 3));
	CHECK_FALSE(holes.has_hole(tree->get_max_level(), 0, 0));

	LandscapeHorizon::Params params;
	params.camera = Vector3(20, 2, 128);
	params.range = 300.0;
	LandscapeHorizon horizon;
	horizon.build(*tree, 1.0, params, &holes);
	CHECK_FALSE(horizon.is_sphere_hidden(Vector3(230, 1, 128), 1.0)); // Seen through the tunnel.
	CHECK(horizon.is_sphere_hidden(Vector3(230, 1, 20), 1.0)); // Elsewhere the ridge still hides.

	// Without holes in the leaves.
	holes.set_leaves(tree->get_max_level(), Rect2i(0, 0, leaves, leaves), HashSet<Vector2i>());
	CHECK_FALSE(holes.has_any());
	horizon.build(*tree, 1.0, params, &holes);
	CHECK(horizon.is_sphere_hidden(Vector3(230, 1, 128), 1.0));
}

} // namespace TestLandscape
