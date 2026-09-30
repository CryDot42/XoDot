/**************************************************************************/
/*  test_landscape_foliage.h                                              */
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
#include "../landscape_data.h"
#include "../landscape_foliage_3d.h"
#include "../landscape_foliage_data.h"
#include "../landscape_foliage_type.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/resource_saver.h"
#include "scene/3d/camera_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "tests/test_macros.h"

namespace TestLandscapeFoliage {

static Ref<LandscapeFoliageType> make_type(float p_density = 10.0) {
	Ref<LandscapeFoliageType> type;
	type.instantiate();
	Ref<BoxMesh> box;
	box.instantiate();
	type->set_mesh(box);
	type->set_density(p_density);
	type->set_random_yaw(false);
	type->set_align_to_normal(false);
	return type;
}

// A flat 128 x 128 m landscape at the given height, with a foliage node.
static Landscape3D *make_landscape(LandscapeFoliage3D *&r_foliage, float p_height = 2.0) {
	Landscape3D *landscape = memnew(Landscape3D);
	Ref<LandscapeData> data;
	data.instantiate();
	data->create(Vector2i(129, 129), 1.0, p_height);
	landscape->set_data(data);
	Vector<Ref<LandscapeLayer>> layers;
	for (int i = 0; i < 2; i++) {
		layers.push_back(memnew(LandscapeLayer));
	}
	landscape->set_layers(layers);
	SceneTree::get_singleton()->get_root()->add_child(landscape);
	r_foliage = memnew(LandscapeFoliage3D);
	landscape->add_child(r_foliage);
	return landscape;
}

static int count_in_circle(LandscapeFoliage3D *p_foliage, int p_type, const Vector2 &p_center, real_t p_radius) {
	int count = 0;
	for (const Variant &xform : p_foliage->get_instance_transforms(p_type)) {
		const Vector3 origin = Transform3D(xform).origin;
		count += Vector2(origin.x, origin.z).distance_to(p_center) <= p_radius ? 1 : 0;
	}
	return count;
}

TEST_CASE("[Landscape][Foliage] Foliage type levels of detail") {
	Ref<LandscapeFoliageType> type = make_type();
	CHECK(type->get_lod_count() == 1);
	CHECK(type->get_lod_end_distance(0) == doctest::Approx(0.0)); // Never culled.

	type->set_cull_distance(200.0);
	type->set_lod_count(3);
	CHECK(type->get_lod_count() == 3);
	CHECK(type->get_lod_start_distance(0) == doctest::Approx(0.0));
	CHECK(type->get_lod_start_distance(1) > 0.0);
	CHECK(type->get_lod_start_distance(2) > type->get_lod_start_distance(1));
	CHECK(type->get_lod_start_distance(2) < 200.0);
	CHECK(type->get_lod_end_distance(0) == doctest::Approx(type->get_lod_start_distance(1)));
	CHECK(type->get_lod_end_distance(2) == doctest::Approx(200.0));

	// Levels without a mesh draw the mesh of the previous level.
	Ref<BoxMesh> small;
	small.instantiate();
	type->set_lod_mesh(2, small);
	CHECK(type->get_lod_effective_mesh(1) == type->get_mesh());
	CHECK(type->get_lod_effective_mesh(2) == small);

	// Stored as lod_<n>/* properties, which may be loaded before the level count.
	Ref<LandscapeFoliageType> loaded;
	loaded.instantiate();
	loaded->set("lod_2/start_distance", 80.0);
	loaded->set("lod_2/cast_shadows", false);
	CHECK(loaded->get_lod_count() == 3);
	CHECK(loaded->get_lod_start_distance(2) == doctest::Approx(80.0));
	CHECK_FALSE(loaded->is_lod_casting_shadows(2));
	CHECK(bool(loaded->get("lod_0/cast_shadows")));
	loaded->set("lod_count", 3);
	CHECK(loaded->get_lod_count() == 3);

	type->remove_lod(1);
	CHECK(type->get_lod_count() == 2);
	CHECK(type->get_lod_mesh(1) == small);
	ERR_PRINT_OFF;
	type->remove_lod(0); // The first level stays.
	ERR_PRINT_ON;
	CHECK(type->get_lod_count() == 2);
}

TEST_CASE("[Landscape][Foliage] Placement rules of a foliage type") {
	Ref<LandscapeFoliageType> type = make_type();
	float weights[16] = {};
	weights[0] = 1.0;

	CHECK(type->accepts(Vector3(0, 1, 0), 0.0, weights));
	type->set_slope_max(Math::deg_to_rad(20.0));
	CHECK_FALSE(type->accepts(Vector3(1, 1, 0).normalized(), 0.0, weights)); // 45 degrees.
	CHECK(type->accepts(Vector3(0.2, 1, 0).normalized(), 0.0, weights));
	type->set_height_min(5.0);
	CHECK_FALSE(type->accepts(Vector3(0, 1, 0), 0.0, weights));
	CHECK(type->accepts(Vector3(0, 1, 0), 6.0, weights));

	// Landscape layers.
	type->set_layers(1 << 3);
	CHECK_FALSE(type->accepts(Vector3(0, 1, 0), 6.0, weights));
	weights[0] = 0.4;
	weights[3] = 0.6;
	CHECK(type->accepts(Vector3(0, 1, 0), 6.0, weights));
	type->set_exclude_layers(1 << 0);
	type->set_layer_min_weight(0.3);
	CHECK_FALSE(type->accepts(Vector3(0, 1, 0), 6.0, weights));

	// Scale and alignment.
	type->set_scale_min(0.5);
	type->set_scale_max(2.0);
	type->set_align_to_normal(true);
	type->set_align_max_angle(Math::deg_to_rad(10.0));
	RandomPCG rng(42);
	const Vector3 steep = Vector3(1, 1, 0).normalized();
	bool scales = true;
	bool aligned = true;
	for (int i = 0; i < 64; i++) {
		const LandscapeFoliageType::Placement placement = type->generate(Vector3(1, 2, 3), steep, rng);
		const Vector3 scale = placement.transform.basis.get_scale();
		scales = scales && scale.x >= 0.5 - 1e-4 && scale.x <= 2.0 + 1e-4 && Math::is_equal_approx(scale.x, scale.y);
		// Tilted towards the ground normal by the maximum angle only.
		aligned = aligned && Math::is_equal_approx(placement.align_normal.angle_to(Vector3(0, 1, 0)), real_t(Math::deg_to_rad(10.0)), real_t(1e-3));
		aligned = aligned && placement.transform.basis.get_column(1).normalized().is_equal_approx(placement.align_normal);
		aligned = aligned && placement.transform.origin.is_equal_approx(Vector3(1, 2, 3));
	}
	CHECK(scales);
	CHECK(aligned);
}

TEST_CASE("[SceneTree][Landscape][Foliage] Painting and erasing reach the density") {
	LandscapeFoliage3D *foliage = nullptr;
	Landscape3D *landscape = make_landscape(foliage);
	const int type = foliage->add_foliage_type(make_type(10.0)); // 0.1 per m².
	CHECK(foliage->get_landscape() == landscape);

	// A circle of 20 m: 125.7 instances.
	const Vector3 center(64, 2, 64);
	const int added = foliage->paint(type, center, 20.0);
	CHECK(added >= 125);
	CHECK(added <= 126);
	CHECK(foliage->get_instance_count(type) == added);
	CHECK(count_in_circle(foliage, type, Vector2(64, 64), 20.0) == added);
	// On the ground.
	bool on_ground = true;
	for (const Variant &xform : foliage->get_instance_transforms(type)) {
		on_ground = on_ground && Math::is_equal_approx(Transform3D(xform).origin.y, real_t(2.0));
	}
	CHECK(on_ground);

	// Painting again doesn't add more than the density.
	foliage->paint(type, center, 20.0);
	CHECK(foliage->get_instance_count(type) <= 127);
	// Half the density.
	foliage->erase(type, center, 20.0, 0.5);
	const int half = foliage->get_instance_count(type);
	CHECK(half >= 62);
	CHECK(half <= 63);
	// Everything in the brush.
	CHECK(foliage->remove_instances_in_radius(type, center, 20.0) == half);
	CHECK(foliage->get_instance_count(type) == 0);
	CHECK(int(foliage->get_statistics()["cells"]) == 0);

	// Outside of the landscape.
	CHECK(foliage->paint(type, Vector3(-100, 0, -100), 10.0) == 0);

	memdelete(landscape);
}

TEST_CASE("[SceneTree][Landscape][Foliage] Filters and spacing") {
	LandscapeFoliage3D *foliage = nullptr;
	Landscape3D *landscape = make_landscape(foliage);
	Ref<LandscapeData> data = landscape->get_data();
	// Layer 1 on the left half of the landscape.
	for (int z = 0; z < 129; z++) {
		for (int x = 0; x < 64; x++) {
			data->set_layer_weight(x, z, 1, 1.0);
		}
	}
	data->notify_region_changed(Rect2i(0, 0, 129, 129), LandscapeData::CHANGED_WEIGHTS);

	Ref<LandscapeFoliageType> grass = make_type(20.0);
	grass->set_layers(1 << 1);
	const int type = foliage->add_foliage_type(grass);
	const int filled = foliage->fill(type);
	CHECK(filled > 1000);
	bool on_layer = true;
	for (const Variant &xform : foliage->get_instance_transforms(type)) {
		on_layer = on_layer && Transform3D(xform).origin.x < 64.0;
	}
	CHECK(on_layer);
	// Filling again completes the density only.
	CHECK(foliage->fill(type) < filled / 10);

	// Minimum distance between the instances of a type.
	Ref<LandscapeFoliageType> trees = make_type(100.0);
	trees->set_radius(3.0);
	const int tree_type = foliage->add_foliage_type(trees);
	foliage->fill(tree_type, Rect2(10, 10, 30, 30));
	const TypedArray<Transform3D> xforms = foliage->get_instance_transforms(tree_type);
	CHECK(xforms.size() > 20);
	real_t min_distance = 1e6;
	for (int i = 0; i < xforms.size(); i++) {
		for (int j = i + 1; j < xforms.size(); j++) {
			min_distance = MIN(min_distance, Transform3D(xforms[i]).origin.distance_to(Transform3D(xforms[j]).origin));
		}
	}
	CHECK(min_distance >= 3.0);

	// Reapplied filters remove the instances that don't match anymore.
	grass->set_slope_min(Math::deg_to_rad(10.0));
	const int64_t before = foliage->get_instance_count(type);
	CHECK(foliage->reapply(type, Vector3(32, 2, 64), 10.0) > 0);
	CHECK(foliage->get_instance_count(type) < before);
	CHECK(foliage->get_instances_in_radius(type, Vector3(32, 2, 64), 10.0).is_empty());

	memdelete(landscape);
}

TEST_CASE("[SceneTree][Landscape][Foliage] Instances follow the terrain") {
	LandscapeFoliage3D *foliage = nullptr;
	Landscape3D *landscape = make_landscape(foliage, 0.0);
	Ref<LandscapeFoliageType> type = make_type();
	type->set_offset_min(-0.5);
	type->set_offset_max(-0.5);
	type->set_align_to_normal(true);
	type->set_align_max_angle(Math::deg_to_rad(90.0));
	foliage->add_foliage_type(type);
	REQUIRE(foliage->place_instance(0, Vector3(40.3, 10, 40.6)));
	Transform3D xform = foliage->get_instance_transforms(0)[0];
	CHECK(xform.origin.is_equal_approx(Vector3(40.3, -0.5, 40.6)));

	// A slope under the instance.
	Ref<LandscapeData> data = landscape->get_data();
	for (int z = 0; z < 129; z++) {
		for (int x = 0; x < 129; x++) {
			data->set_height(x, z, x * 0.5);
		}
	}
	data->notify_region_changed(Rect2i(0, 0, 129, 129), LandscapeData::CHANGED_HEIGHTS);
	foliage->notification(Node::NOTIFICATION_INTERNAL_PROCESS);
	xform = foliage->get_instance_transforms(0)[0];
	CHECK(xform.origin.is_equal_approx(Vector3(40.3, 40.3 * 0.5 - 0.5, 40.6)));
	const Vector3 normal = landscape->get_normal_at(xform.origin);
	CHECK(xform.basis.get_column(1).normalized().is_equal_approx(normal));

	// Without following.
	foliage->set_follow_terrain(false);
	data->set_height(40, 40, 10.0);
	data->set_height(41, 40, 10.0);
	data->set_height(40, 41, 10.0);
	data->set_height(41, 41, 10.0);
	data->notify_region_changed(Rect2i(40, 40, 2, 2), LandscapeData::CHANGED_HEIGHTS);
	foliage->notification(Node::NOTIFICATION_INTERNAL_PROCESS);
	CHECK(Transform3D(foliage->get_instance_transforms(0)[0]).origin.is_equal_approx(xform.origin));
	// Until snapped explicitly.
	foliage->snap_to_terrain();
	CHECK(Transform3D(foliage->get_instance_transforms(0)[0]).origin.y == doctest::Approx(9.5));

	// Instances added by scripts keep their height above the ground.
	foliage->set_follow_terrain(true);
	foliage->add_instance(0, Transform3D(Basis(), Vector3(100, 60, 100)));
	data->set_height(100, 100, 60.0);
	data->notify_region_changed(Rect2i(100, 100, 1, 1), LandscapeData::CHANGED_HEIGHTS);
	foliage->notification(Node::NOTIFICATION_INTERNAL_PROCESS);
	const TypedArray<Transform3D> near = foliage->get_instances_in_radius(0, Vector3(100, 0, 100), 1.0);
	REQUIRE(near.size() == 1);
	CHECK(Transform3D(near[0]).origin.y == doctest::Approx(60.0 + (60.0 - 50.0)));

	memdelete(landscape);
}

TEST_CASE("[SceneTree][Landscape][Foliage] Instance data, undo snapshots and chunks") {
	LandscapeFoliage3D *foliage = nullptr;
	Landscape3D *landscape = make_landscape(foliage);
	foliage->add_foliage_type(make_type(20.0));
	foliage->add_foliage_type(make_type(5.0));
	foliage->fill(0);
	foliage->fill(1, Rect2(0, 0, 64, 64));
	const int64_t count0 = foliage->get_instance_count(0);
	const int64_t count1 = foliage->get_instance_count(1);
	CHECK(count0 > 2000);
	CHECK(count1 > 100);
	const PackedFloat32Array all = foliage->get_instances_in_rect(0, Rect2(0, 0, 200, 200));
	CHECK(all.size() == count0 * LandscapeFoliage3D::INSTANCE_FLOATS);

	// Compressed data.
	const PackedByteArray blob = foliage->get_type_data(0);
	CHECK(blob.size() > 16);
	CHECK(blob.size() < all.size() * 4);

	// Saved properties (duplicate).
	LandscapeFoliage3D *copy = Object::cast_to<LandscapeFoliage3D>(foliage->duplicate());
	REQUIRE(copy);
	CHECK(copy->get_foliage_type_count() == 2);
	CHECK(copy->get_instance_count(0) == count0);
	CHECK(copy->get_instance_count(1) == count1);
	CHECK(copy->get_type_data(0) == blob);
	memdelete(copy);

	// Snapshot of a rect, erased and restored (undo/redo of the brushes).
	const Rect2 rect(32, 32, 32, 32);
	const PackedFloat32Array before = foliage->get_instances_in_rect(0, rect);
	CHECK(before.size() > 0);
	foliage->erase(0, Vector3(48, 2, 48), 12.0);
	CHECK(foliage->get_instance_count(0) < count0);
	foliage->set_instances_in_rect(0, rect, before);
	CHECK(foliage->get_instance_count(0) == count0);
	CHECK(foliage->get_instances_in_rect(0, rect).size() == before.size());

	// Other chunk sizes keep the instances.
	foliage->set_chunk_size(16.0);
	CHECK(foliage->get_instance_count(0) == count0);
	CHECK(int(foliage->get_statistics()["cells"]) > 64);

	// Types are replaced keeping their instances, removed with them.
	foliage->set_foliage_type(1, make_type(1.0));
	CHECK(foliage->get_instance_count(1) == count1);
	foliage->remove_foliage_type(0);
	CHECK(foliage->get_foliage_type_count() == 1);
	CHECK(foliage->get_instance_count() == count1);
	foliage->clear();
	CHECK(foliage->get_instance_count() == 0);

	memdelete(landscape);
}

TEST_CASE("[SceneTree][Landscape][Foliage] Chunked rendering with levels of detail") {
	LandscapeFoliage3D *foliage = nullptr;
	Landscape3D *landscape = make_landscape(foliage, 0.0);
	Camera3D *camera = memnew(Camera3D);
	landscape->add_child(camera);
	camera->set_position(Vector3(0, 0, 64));
	landscape->set_lod_camera_path(landscape->get_path_to(camera));

	Ref<LandscapeFoliageType> type = make_type();
	type->set_cull_distance(60.0);
	type->set_lod_transition(0.0);
	Ref<BoxMesh> far_mesh;
	far_mesh.instantiate();
	type->add_lod(far_mesh, 20.0, false);
	foliage->add_foliage_type(type);
	// A row of instances every 10 m, 5 to 125 m from the camera.
	for (int i = 0; i < 13; i++) {
		foliage->add_instance(0, Transform3D(Basis(), Vector3(5 + i * 10, 0, 64)));
	}
	foliage->notification(Node::NOTIFICATION_INTERNAL_PROCESS);
	Dictionary stats = foliage->get_statistics();
	// Cells of 32 m: [0, 32) crosses the LOD distance, [32, 64) is entirely at LOD 1, the others are culled.
	CHECK(int64_t(stats["cells"]) == 4);
	CHECK(int64_t(stats["cells_rendered"]) == 2);
	CHECK(int64_t(stats["cells_mixed"]) == 1);
	CHECK(int64_t(stats["batches"]) == 3);
	CHECK(int64_t(stats["instances_drawn"]) == 6);

	// The camera at the other end.
	camera->set_position(Vector3(130, 0, 64));
	foliage->notification(Node::NOTIFICATION_INTERNAL_PROCESS);
	stats = foliage->get_statistics();
	CHECK(int64_t(stats["cells_rendered"]) == 2);
	CHECK(int64_t(stats["instances_drawn"]) == 6);

	// Farther levels.
	foliage->set_lod_distance_scale(10.0);
	foliage->notification(Node::NOTIFICATION_INTERNAL_PROCESS);
	stats = foliage->get_statistics();
	CHECK(int64_t(stats["instances_drawn"]) == 13);
	CHECK(int64_t(stats["cells_rendered"]) == 4);

	// Hidden: no updates, resources freed when leaving the tree.
	landscape->remove_child(foliage);
	stats = foliage->get_statistics();
	CHECK(int64_t(stats["batches"]) == 0);
	CHECK(foliage->get_instance_count() == 13);
	landscape->add_child(foliage);
	foliage->notification(Node::NOTIFICATION_INTERNAL_PROCESS);
	CHECK(int64_t(foliage->get_statistics()["instances_drawn"]) == 13);

	memdelete(landscape);
}

TEST_CASE("[SceneTree][Landscape][Foliage] Debug views and frozen levels of detail") {
	LandscapeFoliage3D *foliage = nullptr;
	Landscape3D *landscape = make_landscape(foliage, 0.0);
	Camera3D *camera = memnew(Camera3D);
	landscape->add_child(camera);
	camera->set_position(Vector3(0, 0, 64));
	camera->set_rotation(Vector3(0, -Math::PI / 2, 0)); // Looks at the instances (+X).
	landscape->set_lod_camera_path(landscape->get_path_to(camera));
	Ref<LandscapeFoliageType> type = make_type();
	type->set_cull_distance(60.0);
	type->set_lod_transition(0.0);
	type->add_lod(Ref<Mesh>(), 20.0, false);
	foliage->add_foliage_type(type);
	for (int i = 0; i < 13; i++) {
		foliage->add_instance(0, Transform3D(Basis(), Vector3(5 + i * 10, 0, 64)));
	}
	foliage->notification(Node::NOTIFICATION_INTERNAL_PROCESS);
	CHECK(int64_t(foliage->get_statistics()["debug_bounds"]) == 0);

	// Bounds of the drawn cells.
	foliage->set_debug_view(LandscapeFoliage3D::DEBUG_VIEW_CELL_STATE);
	foliage->notification(Node::NOTIFICATION_INTERNAL_PROCESS);
	Dictionary stats = foliage->get_statistics();
	CHECK(int64_t(stats["debug_bounds"]) == 2);
	CHECK(int64_t(stats["instances_drawn"]) == 6);

	// The colors of the landscape debug views.
	CHECK(LandscapeFoliage3D::get_debug_color(0).is_equal_approx(Color(1.0, 0.25, 0.25)));
	CHECK_FALSE(LandscapeFoliage3D::get_debug_color(1).is_equal_approx(LandscapeFoliage3D::get_debug_color(2)));
	CHECK_FALSE(LandscapeFoliage3D::get_debug_state_color(true).is_equal_approx(LandscapeFoliage3D::get_debug_state_color(false)));

	// With the LOD of the landscape frozen, the foliage keeps its levels when the camera leaves.
	landscape->set_freeze_lod(true);
	camera->set_position(Vector3(64, 0, 200));
	foliage->notification(Node::NOTIFICATION_INTERNAL_PROCESS);
	stats = foliage->get_statistics();
	CHECK(int64_t(stats["cells_rendered"]) == 2);
	CHECK(int64_t(stats["cells_out_of_view"]) == 0);
	CHECK(int64_t(stats["instances_drawn"]) == 6);
	landscape->set_freeze_lod(false);
	foliage->notification(Node::NOTIFICATION_INTERNAL_PROCESS);
	stats = foliage->get_statistics();
	CHECK(int64_t(stats["cells_rendered"]) == 0); // Everything is beyond the cull distance.
	CHECK(int64_t(stats["debug_bounds"]) == 0);

	camera->set_position(Vector3(0, 0, 64));
	foliage->notification(Node::NOTIFICATION_INTERNAL_PROCESS);
	CHECK(int64_t(foliage->get_statistics()["debug_bounds"]) == 2);
	foliage->set_debug_view(LandscapeFoliage3D::DEBUG_VIEW_DISABLED);
	foliage->notification(Node::NOTIFICATION_INTERNAL_PROCESS);
	CHECK(int64_t(foliage->get_statistics()["debug_bounds"]) == 0);
	CHECK(int64_t(foliage->get_statistics()["instances_drawn"]) == 6);

	memdelete(landscape);
}

TEST_CASE("[SceneTree][Landscape][Foliage] Culling with the frozen view of the camera") {
	LandscapeFoliage3D *foliage = nullptr;
	Landscape3D *landscape = make_landscape(foliage, 0.0);
	Camera3D *camera = memnew(Camera3D);
	landscape->add_child(camera);
	camera->set_position(Vector3(32, 0, 64));
	camera->set_rotation(Vector3(0, -Math::PI / 2, 0)); // Looks along +X.
	landscape->set_lod_camera_path(landscape->get_path_to(camera));
	Ref<LandscapeFoliageType> type = make_type();
	type->set_cull_distance(60.0);
	type->set_lod_transition(0.0);
	foliage->add_foliage_type(type);
	for (int i = 0; i < 13; i++) {
		foliage->add_instance(0, Transform3D(Basis(), Vector3(5 + i * 10, 0, 64)));
	}
	// The cells within the cull distance: [0, 32) behind the camera, [32, 64) and [64, 96) in front of it.
	// The renderer culls them for every camera.
	foliage->notification(Node::NOTIFICATION_INTERNAL_PROCESS);
	Dictionary stats = foliage->get_statistics();
	CHECK(int64_t(stats["cells_rendered"]) == 3);
	CHECK(int64_t(stats["cells_out_of_view"]) == 0);
	CHECK(int64_t(stats["instances_drawn"]) == 9);

	// With the LOD of the landscape frozen, the cells out of the view of its camera are hidden like its
	// patches (they keep their rendering resources for the shadows).
	landscape->set_freeze_lod(true);
	foliage->notification(Node::NOTIFICATION_INTERNAL_PROCESS);
	stats = foliage->get_statistics();
	CHECK(int64_t(stats["cells_rendered"]) == 3);
	CHECK(int64_t(stats["cells_out_of_view"]) == 1);
	CHECK(int64_t(stats["instances_drawn"]) == 6);

	// The view stays frozen when the camera moves and turns around.
	camera->set_position(Vector3(40, 0, 64));
	camera->set_rotation(Vector3(0, Math::PI / 2, 0));
	foliage->notification(Node::NOTIFICATION_INTERNAL_PROCESS);
	stats = foliage->get_statistics();
	CHECK(int64_t(stats["cells_out_of_view"]) == 1);
	CHECK(int64_t(stats["instances_drawn"]) == 6);

	// Instances added out of the frozen view are hidden too.
	foliage->add_instance(0, Transform3D(Basis(), Vector3(20, 0, 20)));
	foliage->notification(Node::NOTIFICATION_INTERNAL_PROCESS);
	stats = foliage->get_statistics();
	CHECK(int64_t(stats["cells_out_of_view"]) == 2);
	CHECK(int64_t(stats["instances_drawn"]) == 6);

	// Unfrozen: the renderer culls the cells again. From 40 m, the instances up to 95 m are drawn.
	landscape->set_freeze_lod(false);
	foliage->notification(Node::NOTIFICATION_INTERNAL_PROCESS);
	stats = foliage->get_statistics();
	CHECK(int64_t(stats["cells_out_of_view"]) == 0);
	CHECK(int64_t(stats["instances_drawn"]) == 11);

	memdelete(landscape);
}

static void process_streaming(LandscapeFoliage3D *p_foliage) {
	// Requests the needed cells, waits for their loads and installs them.
	p_foliage->notification(Node::NOTIFICATION_INTERNAL_PROCESS);
	if (p_foliage->get_data().is_valid()) {
		p_foliage->get_data()->wait_for_loads();
	}
	p_foliage->notification(Node::NOTIFICATION_INTERNAL_PROCESS);
	p_foliage->notification(Node::NOTIFICATION_INTERNAL_PROCESS); // Rendering updates over the budget of a frame.
}

TEST_CASE("[SceneTree][Landscape][Foliage] Streamed foliage data") {
	const String path = TestUtils::get_temp_path("foliage_streaming.lfdata");
	const String path2 = TestUtils::get_temp_path("foliage_streaming_2.lfdata");
	LandscapeFoliage3D *foliage = nullptr;
	Landscape3D *landscape = make_landscape(foliage, 0.0);
	Camera3D *camera = memnew(Camera3D);
	landscape->add_child(camera);
	camera->set_position(Vector3(0, 0, 64));
	landscape->set_lod_camera_path(landscape->get_path_to(camera));
	Ref<LandscapeFoliageType> type = make_type(10.0);
	type->set_cull_distance(20.0);
	type->set_lod_transition(0.0);
	foliage->add_foliage_type(type);
	foliage->add_foliage_type(make_type(1.0)); // Never culled: always loaded.
	foliage->fill(0);
	foliage->fill(1);
	const int64_t count0 = foliage->get_instance_count(0);
	const int64_t count1 = foliage->get_instance_count(1);
	CHECK(count0 > 1000);
	CHECK(count1 > 50);
	const TypedArray<Transform3D> far_before = foliage->get_instances_in_radius(0, Vector3(110, 0, 20), 10.0);

	// Saved to a file: the loaded cells are kept, then the far ones are released.
	REQUIRE(foliage->save_to_data_file(path) == OK);
	Ref<LandscapeFoliageData> data = foliage->get_data();
	REQUIRE(data.is_valid());
	CHECK(data->is_streamed());
	CHECK_FALSE(data->has_unsaved_changes());
	CHECK(data->get_layer_count() == 2);
	CHECK(data->get_instance_count(0) == count0);
	CHECK(Array(foliage->get("instance_data")).is_empty()); // Not in the scene anymore.
	CHECK(int64_t(foliage->get_statistics()["cells_loaded"]) == 32);
	process_streaming(foliage);
	CHECK(int64_t(foliage->get_statistics()["cells_loaded"]) < 32);
	CHECK(foliage->get_instance_count(0) == count0);

	// Another node streams the same file: only the cells within the cull distance of the camera
	// (cells of 32 m, 20 m of cull distance) are loaded, and every cell of the other type.
	LandscapeFoliage3D *streamed = memnew(LandscapeFoliage3D);
	landscape->add_child(streamed);
	streamed->add_foliage_type(type);
	streamed->add_foliage_type(make_type(1.0));
	Ref<LandscapeFoliageData> loaded;
	loaded.instantiate();
	REQUIRE(loaded->load_from_file(path) == OK);
	streamed->set_data(loaded);
	CHECK(streamed->get_instance_count(0) == count0);
	CHECK(streamed->get_instance_count(1) == count1);
	CHECK(int64_t(streamed->get_statistics()["cells_loaded"]) == 0);
	process_streaming(streamed);
	Dictionary stats = streamed->get_statistics();
	CHECK(int64_t(stats["cells_loading"]) == 0);
	CHECK(int64_t(stats["cells_loaded"]) == 6 + 16);
	CHECK(int64_t(stats["loaded_memory"]) > 0);
	// Instances drawn from the loaded cells only.
	CHECK(int64_t(stats["instances_drawn"]) > 0);
	CHECK(int64_t(stats["instances_drawn"]) < count0);

	// Queries and edits load the cells they need.
	CHECK(streamed->get_instances_in_radius(0, Vector3(110, 0, 20), 10.0).size() == far_before.size());
	CHECK(streamed->remove_instances_in_radius(0, Vector3(110, 0, 20), 10.0) == far_before.size());
	CHECK(streamed->get_instance_count(0) == count0 - far_before.size());
	streamed->notification(Node::NOTIFICATION_INTERNAL_PROCESS);
	CHECK(loaded->has_unsaved_changes());
	CHECK(loaded->get_instance_count(0) == count0 - far_before.size());

	// The camera moves: the cells follow, over a tiny budget only the needed cells stay.
	ProjectSettings::get_singleton()->set_setting("rendering/landscape/streaming/foliage_cache_size_mb", 0);
	camera->set_position(Vector3(128, 0, 64));
	process_streaming(streamed);
	ProjectSettings::get_singleton()->set_setting("rendering/landscape/streaming/foliage_cache_size_mb", 256);
	stats = streamed->get_statistics();
	CHECK(int64_t(stats["cells_loaded"]) == 6 + 16);
	CHECK(streamed->get_instance_count(0) == count0 - far_before.size());

	// Saved to another file (with the edited cell), then read by a third data.
	REQUIRE(ResourceSaver::save(loaded, path2) == OK);
	CHECK_FALSE(loaded->has_unsaved_changes());
	CHECK(loaded->is_streamed());
	Ref<LandscapeFoliageData> reloaded;
	reloaded.instantiate();
	REQUIRE(reloaded->load_from_file(path2) == OK);
	CHECK(reloaded->get_instance_count(0) == count0 - far_before.size());
	CHECK(reloaded->get_instance_count(1) == count1);

	// Embedded in a scene: the whole layers.
	Ref<LandscapeFoliageData> embedded;
	embedded.instantiate();
	embedded->set("chunk_size", reloaded->get("chunk_size"));
	embedded->set("_layers_data", reloaded->get("_layers_data"));
	CHECK(embedded->get_instance_count() == reloaded->get_instance_count());
	CHECK_FALSE(embedded->is_streamed());

	// Types added and removed keep the layers aligned.
	streamed->add_foliage_type(make_type(1.0));
	CHECK(loaded->get_layer_count() == 3);
	streamed->remove_foliage_type(0);
	CHECK(loaded->get_layer_count() == 2);
	CHECK(streamed->get_instance_count(0) == count1);
	CHECK(loaded->get_instance_count(0) == count1);

	// Detached: every instance goes back to the node.
	streamed->set_data(Ref<LandscapeFoliageData>());
	CHECK(streamed->get_instance_count() == count1);
	CHECK(Array(streamed->get("instance_data")).size() == 2);

	memdelete(landscape);
	data.unref();
	loaded.unref();
	reloaded.unref();
	DirAccess::remove_absolute(path);
	DirAccess::remove_absolute(path2);
}

TEST_CASE("[SceneTree][Landscape][Foliage] GPU indirect rendering falls back to the CPU") {
	LandscapeFoliage3D *foliage = nullptr;
	Landscape3D *landscape = make_landscape(foliage, 0.0);
	Camera3D *camera = memnew(Camera3D);
	landscape->add_child(camera);
	camera->set_position(Vector3(0, 0, 64));
	landscape->set_lod_camera_path(landscape->get_path_to(camera));
	Ref<LandscapeFoliageType> type = make_type();
	type->set_cull_distance(60.0);
	type->set_lod_transition(0.0);
	foliage->add_foliage_type(type);
	for (int i = 0; i < 13; i++) {
		foliage->add_instance(0, Transform3D(Basis(), Vector3(5 + i * 10, 0, 64)));
	}
	foliage->set_gpu_indirect(true);
	CHECK(foliage->is_gpu_indirect());
	// No rendering device without a display (and with the Compatibility renderer).
	CHECK_FALSE(LandscapeFoliage3D::is_gpu_indirect_supported());
	CHECK_FALSE(foliage->is_gpu_indirect_active());
	CHECK(foliage->get_configuration_warnings().size() == 1);
	foliage->notification(Node::NOTIFICATION_INTERNAL_PROCESS);
	const Dictionary stats = foliage->get_statistics();
	CHECK_FALSE(bool(stats["gpu_indirect"]));
	CHECK(int64_t(stats["instances_drawn"]) == 6);

	foliage->set_gpu_max_instances(10);
	CHECK(foliage->get_gpu_max_instances() == 1024);
	CHECK(foliage->is_gpu_frustum_culling());
	CHECK(foliage->is_gpu_occlusion_culling());
	foliage->set_gpu_occlusion_culling(false);
	CHECK_FALSE(foliage->is_gpu_occlusion_culling());
	foliage->set_gpu_indirect(false);
	CHECK(foliage->get_configuration_warnings().is_empty());

	memdelete(landscape);
}

} // namespace TestLandscapeFoliage
