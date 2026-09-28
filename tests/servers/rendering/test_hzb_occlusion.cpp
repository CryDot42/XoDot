/**************************************************************************/
/*  test_hzb_occlusion.cpp                                                */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_hzb_occlusion)

#include "servers/rendering/renderer_scene_occlusion_cull.h"

namespace TestHZBOcclusion {

class TestHZBuffer : public RendererSceneOcclusionCull::HZBuffer {
public:
	bool test_occluded(const AABB &p_aabb, const Transform3D &p_cam_transform, const Projection &p_cam_projection, bool p_cam_orthogonal) const {
		const Vector3 end = p_aabb.get_end();
		const real_t bounds[6] = { p_aabb.position.x, p_aabb.position.y, p_aabb.position.z, end.x, end.y, end.z };
		return _is_occluded(bounds, p_cam_transform.origin, p_cam_transform.affine_inverse(), p_cam_projection, p_cam_projection.get_z_near(), p_cam_orthogonal);
	}

	float get_texel(int p_x, int p_y) const {
		return mips[0][p_y * sizes[0].x + p_x];
	}

	void fill(float p_value) {
		for (int i = 0; i < sizes[0].x * sizes[0].y; i++) {
			mips[0][i] = p_value;
		}
	}
};

struct Wall {
	real_t z; // Walls face +Z.
	Rect2 rect; // On the XY plane.
};

// Simulates the depth readback of a scene made of walls, rendered the same way as the RenderingDevice
// renderers do (reversed Z, flipped Y).
static RendererSceneOcclusionCull::DepthReadback make_readback(const Transform3D &p_cam_transform, const Projection &p_cam_projection, const Size2i &p_size, const Vector<Wall> &p_walls) {
	Projection correction;
	correction.set_depth_correction(true);
	const Projection render_projection = correction * p_cam_projection;
	const Projection inv_render_projection = render_projection.inverse();
	const Transform3D cam_inv_transform = p_cam_transform.affine_inverse();

	RendererSceneOcclusionCull::DepthReadback readback;
	readback.size = p_size;
	readback.view_count = 1;
	readback.cam_transform = p_cam_transform;
	readback.cam_orthogonal = p_cam_projection.is_orthogonal();
	readback.inv_projection[0] = inv_render_projection;
	readback.depth.resize(p_size.x * p_size.y);

	for (int y = 0; y < p_size.y; y++) {
		for (int x = 0; x < p_size.x; x++) {
			const real_t ndc_x = (x + 0.5f) / p_size.x * 2.0f - 1.0f;
			const real_t ndc_y = (y + 0.5f) / p_size.y * 2.0f - 1.0f;
			const Vector3 near_point = p_cam_transform.xform(inv_render_projection.xform(Vector3(ndc_x, ndc_y, 1.0f)));
			const Vector3 far_point = p_cam_transform.xform(inv_render_projection.xform(Vector3(ndc_x, ndc_y, 0.0f)));

			float depth = 0.0f; // Background.
			for (const Wall &wall : p_walls) {
				const real_t t = (wall.z - near_point.z) / (far_point.z - near_point.z);
				if (t < 0.0f || t > 1.0f) {
					continue;
				}
				const Vector3 hit = near_point.lerp(far_point, t);
				if (wall.rect.has_point(Vector2(hit.x, hit.y))) {
					depth = MAX(depth, float(render_projection.xform(cam_inv_transform.xform(hit)).z)); // Reversed Z, keep the nearest.
				}
			}
			readback.depth[y * p_size.x + x] = depth;
		}
	}

	return readback;
}

// A square wall centered on the Z axis.
static RendererSceneOcclusionCull::DepthReadback make_wall_readback(const Transform3D &p_cam_transform, const Projection &p_cam_projection, const Size2i &p_size, real_t p_wall_z, real_t p_wall_half_size) {
	return make_readback(p_cam_transform, p_cam_projection, p_size, { { p_wall_z, Rect2(-p_wall_half_size, -p_wall_half_size, p_wall_half_size * 2.0f, p_wall_half_size * 2.0f) } });
}

static const Size2i buffer_size = Size2i(128, 72);

TEST_CASE("[HZBOcclusion] Reprojecting from the same camera") {
	const Projection projection = Projection::create_perspective(70.0f, 16.0f / 9.0f, 0.05f, 100.0f);
	const Transform3D camera;

	TestHZBuffer buffer;
	buffer.resize(buffer_size);
	const RendererSceneOcclusionCull::DepthReadback readback = make_wall_readback(camera, projection, buffer_size, -10.0f, 5.0f);
	REQUIRE(readback.is_valid());
	CHECK(buffer.reproject_depth(readback, camera, projection, false, false));
	buffer.update_mips();

	// The texel at the center of the screen should contain the distance to the wall.
	CHECK(buffer.get_texel(buffer_size.x / 2, buffer_size.y / 2) == doctest::Approx(10.0f).epsilon(0.02));

	CHECK_MESSAGE(buffer.test_occluded(AABB(Vector3(-1, -1, -16), Vector3(2, 2, 2)), camera, projection, false), "Objects behind the wall should be occluded.");
	CHECK_MESSAGE(!buffer.test_occluded(AABB(Vector3(-1, -1, -6), Vector3(2, 2, 2)), camera, projection, false), "Objects in front of the wall should not be occluded.");
	CHECK_MESSAGE(!buffer.test_occluded(AABB(Vector3(-1, -1, -11), Vector3(2, 2, 2)), camera, projection, false), "Objects intersecting the wall should not be occluded.");
	CHECK_MESSAGE(!buffer.test_occluded(AABB(Vector3(-1, 14, -26), Vector3(2, 2, 2)), camera, projection, false), "Objects visible above the wall should not be occluded.");
	CHECK_MESSAGE(!buffer.test_occluded(AABB(Vector3(20, -1, -25), Vector3(2, 2, 2)), camera, projection, false), "Objects visible next to the wall should not be occluded.");
}

TEST_CASE("[HZBOcclusion] Reprojecting to a moved camera") {
	const Projection projection = Projection::create_perspective(70.0f, 16.0f / 9.0f, 0.05f, 100.0f);
	const Transform3D old_camera;
	const RendererSceneOcclusionCull::DepthReadback readback = make_wall_readback(old_camera, projection, buffer_size, -10.0f, 5.0f);

	// This object is hidden by the right edge of the wall when seen from the origin.
	const AABB edge_object = AABB(Vector3(5.0f, -0.5f, -15.5f), Vector3(1.0f, 1.0f, 1.0f));
	const AABB center_object = AABB(Vector3(-1, -1, -16), Vector3(2, 2, 2));

	TestHZBuffer buffer;
	buffer.resize(buffer_size);
	REQUIRE(buffer.reproject_depth(readback, old_camera, projection, false, false));
	buffer.update_mips();
	CHECK(buffer.test_occluded(edge_object, old_camera, projection, false));

	SUBCASE("Strafing reveals objects behind the edge of the wall") {
		const Transform3D new_camera = Transform3D(Basis(), Vector3(4.0f, 0.0f, 0.0f));
		REQUIRE(buffer.reproject_depth(readback, new_camera, projection, false, false));
		buffer.update_mips();

		CHECK_MESSAGE(!buffer.test_occluded(edge_object, new_camera, projection, false), "Objects revealed by the camera movement should not be occluded.");
		CHECK_MESSAGE(buffer.test_occluded(center_object, new_camera, projection, false), "Objects still hidden by the wall should be occluded.");
	}

	SUBCASE("Moving closer keeps the wall occluding") {
		// Surfaces get bigger on screen when moving closer, which leaves cracks between the reprojected texels.
		const Transform3D new_camera = Transform3D(Basis(), Vector3(0.0f, 0.0f, -1.5f));
		REQUIRE(buffer.reproject_depth(readback, new_camera, projection, false, false));
		buffer.update_mips();

		CHECK_MESSAGE(buffer.test_occluded(center_object, new_camera, projection, false), "Cracks in the reprojection should be filled.");
		CHECK(buffer.get_texel(buffer_size.x / 2, buffer_size.y / 2) == doctest::Approx(8.5f).epsilon(0.05));
	}

	SUBCASE("Turning around discards the readback") {
		const Transform3D new_camera = Transform3D(Basis(Vector3(0, 1, 0), Math::PI), Vector3());
		CHECK_MESSAGE(!buffer.reproject_depth(readback, new_camera, projection, false, false), "Nothing should be visible from the new camera.");
	}

	SUBCASE("Rotating reveals objects outside of the old view") {
		const Transform3D new_camera = Transform3D(Basis(Vector3(0, 1, 0), Math::deg_to_rad(-60.0f)), Vector3());
		REQUIRE(buffer.reproject_depth(readback, new_camera, projection, false, false));
		buffer.update_mips();

		// Right of the old view, fully visible from the new camera. No depth is known there.
		const AABB outside_object = AABB(Vector3(20.0f, -1.0f, -6.0f), Vector3(2.0f, 2.0f, 2.0f));
		CHECK(!buffer.test_occluded(outside_object, new_camera, projection, false));
	}
}

TEST_CASE("[HZBOcclusion] Gaps opening between objects") {
	const Projection projection = Projection::create_perspective(70.0f, 16.0f / 9.0f, 0.05f, 100.0f);
	const Transform3D old_camera;
	// Seen from the origin, the right edge of the near wall is aligned with the left edge of the far wall.
	const RendererSceneOcclusionCull::DepthReadback readback = make_readback(old_camera, projection, buffer_size,
			{ { -10.0f, Rect2(-5.0f, -5.0f, 5.0f, 10.0f) }, { -14.0f, Rect2(0.0f, -5.0f, 10.0f, 10.0f) } });
	// This object is hidden by the near wall when seen from the origin.
	const AABB object = AABB(Vector3(-1.0f, -0.5f, -20.5f), Vector3(0.8f, 1.0f, 1.0f));

	TestHZBuffer buffer;
	buffer.resize(buffer_size);
	REQUIRE(buffer.reproject_depth(readback, old_camera, projection, false, false));
	buffer.update_mips();
	CHECK(buffer.test_occluded(object, old_camera, projection, false));

	SUBCASE("Strafing right opens a gap smaller than a texel between the walls") {
		const Transform3D new_camera = Transform3D(Basis(), Vector3(0.5f, 0.0f, 0.0f));
		REQUIRE(buffer.reproject_depth(readback, new_camera, projection, false, false));
		buffer.update_mips();
		CHECK_MESSAGE(!buffer.test_occluded(object, new_camera, projection, false), "Objects visible through the gap should not be occluded.");
	}

	SUBCASE("Strafing left makes the walls overlap") {
		const Transform3D new_camera = Transform3D(Basis(), Vector3(-0.5f, 0.0f, 0.0f));
		REQUIRE(buffer.reproject_depth(readback, new_camera, projection, false, false));
		buffer.update_mips();
		CHECK(buffer.test_occluded(AABB(Vector3(-1.5f, -0.5f, -20.5f), Vector3(0.8f, 1.0f, 1.0f)), new_camera, projection, false));
	}
}

TEST_CASE("[HZBOcclusion] Objects that moved since the depth buffer was rendered") {
	const Projection projection = Projection::create_perspective(70.0f, 16.0f / 9.0f, 0.05f, 100.0f);
	const Transform3D camera;
	const RendererSceneOcclusionCull::DepthReadback readback = make_readback(camera, projection, buffer_size,
			{ { -10.0f, Rect2(-5.0f, -5.0f, 5.0f, 10.0f) }, { -12.0f, Rect2(0.0f, -6.0f, 7.0f, 12.0f) } });
	const AABB behind_left = AABB(Vector3(-2.5f, -1.0f, -20.0f), Vector3(1.0f, 2.0f, 1.0f));
	const AABB behind_right = AABB(Vector3(2.0f, -1.0f, -20.0f), Vector3(1.0f, 2.0f, 1.0f));

	TestHZBuffer buffer;
	buffer.resize(buffer_size);
	REQUIRE(buffer.reproject_depth(readback, camera, projection, false, false));
	buffer.update_mips();
	CHECK(buffer.test_occluded(behind_left, camera, projection, false));
	CHECK(buffer.test_occluded(behind_right, camera, projection, false));

	// The left wall moved away (or was hidden) after the depth buffer was rendered.
	const AABB left_wall_bounds = AABB(Vector3(-5.0f, -5.0f, -10.0f), Vector3(5.0f, 10.0f, 0.0f));
	REQUIRE(buffer.reproject_depth(readback, camera, projection, false, false, Span<AABB>(&left_wall_bounds, 1)));
	buffer.update_mips();
	CHECK_MESSAGE(!buffer.test_occluded(behind_left, camera, projection, false), "Objects revealed by moving objects should not be occluded.");
	CHECK_MESSAGE(buffer.test_occluded(behind_right, camera, projection, false), "Objects hidden by objects that didn't move should remain occluded.");

	// Bounds crossing the near plane.
	const AABB near_bounds = AABB(Vector3(-4.0f, -1.0f, -5.0f), Vector3(4.0f, 2.0f, 10.0f));
	REQUIRE(buffer.reproject_depth(readback, camera, projection, false, false, Span<AABB>(&near_bounds, 1)));
	buffer.update_mips();
	CHECK(!buffer.test_occluded(behind_left, camera, projection, false));
	CHECK(buffer.test_occluded(behind_right, camera, projection, false));
}

TEST_CASE("[HZBOcclusion] Orthogonal camera") {
	const Projection projection = Projection::create_orthogonal_aspect(20.0f, 16.0f / 9.0f, 0.05f, 100.0f);
	const Transform3D camera;
	const RendererSceneOcclusionCull::DepthReadback readback = make_wall_readback(camera, projection, buffer_size, -10.0f, 5.0f);

	TestHZBuffer buffer;
	buffer.resize(buffer_size);
	REQUIRE(buffer.reproject_depth(readback, camera, projection, true, false));
	buffer.update_mips();

	CHECK(buffer.get_texel(buffer_size.x / 2, buffer_size.y / 2) == doctest::Approx(10.0f).epsilon(0.02));
	CHECK(buffer.test_occluded(AABB(Vector3(-1, -1, -16), Vector3(2, 2, 2)), camera, projection, true));
	CHECK(!buffer.test_occluded(AABB(Vector3(-1, -1, -6), Vector3(2, 2, 2)), camera, projection, true));
	CHECK(!buffer.test_occluded(AABB(Vector3(8, -1, -16), Vector3(2, 2, 2)), camera, projection, true));

	// Moving an orthogonal camera sideways shifts the wall on screen.
	const Transform3D new_camera = Transform3D(Basis(), Vector3(4.0f, 0.0f, 0.0f));
	REQUIRE(buffer.reproject_depth(readback, new_camera, projection, true, false));
	buffer.update_mips();
	CHECK(!buffer.test_occluded(AABB(Vector3(-8.5f, -1, -16), Vector3(2, 2, 2)), new_camera, projection, true));
	CHECK(buffer.test_occluded(AABB(Vector3(-3.5f, -1, -16), Vector3(2, 2, 2)), new_camera, projection, true));
}

TEST_CASE("[HZBOcclusion] Merging with occluders") {
	const Projection projection = Projection::create_perspective(70.0f, 16.0f / 9.0f, 0.05f, 100.0f);
	const Transform3D camera;
	const RendererSceneOcclusionCull::DepthReadback readback = make_wall_readback(camera, projection, buffer_size, -10.0f, 5.0f);

	TestHZBuffer buffer;
	buffer.resize(buffer_size);

	// Pretend that an occluder covers the whole screen at a distance of 20.
	buffer.fill(20.0f);
	REQUIRE(buffer.reproject_depth(readback, camera, projection, false, true));
	buffer.update_mips();

	CHECK_MESSAGE(buffer.get_texel(buffer_size.x / 2, buffer_size.y / 2) == doctest::Approx(10.0f).epsilon(0.02), "The nearest of the depth buffer and occluders should be kept.");
	CHECK_MESSAGE(buffer.get_texel(0, 0) == doctest::Approx(20.0f), "The background of the depth buffer should not remove occluders.");
	CHECK(buffer.test_occluded(AABB(Vector3(-1, -1, -16), Vector3(2, 2, 2)), camera, projection, false));
	CHECK(buffer.test_occluded(AABB(Vector3(20, -1, -30), Vector3(2, 2, 2)), camera, projection, false));
	CHECK(!buffer.test_occluded(AABB(Vector3(8, -1, -12), Vector3(2, 2, 2)), camera, projection, false));
}

TEST_CASE("[HZBOcclusion] Invalid readbacks") {
	const Projection projection = Projection::create_perspective(70.0f, 16.0f / 9.0f, 0.05f, 100.0f);
	const Transform3D camera;

	TestHZBuffer buffer;
	buffer.resize(buffer_size);

	RendererSceneOcclusionCull::DepthReadback readback;
	CHECK_FALSE(readback.is_valid());
	CHECK_FALSE(buffer.reproject_depth(readback, camera, projection, false, false));

	readback = make_wall_readback(camera, projection, Size2i(16, 9), -10.0f, 5.0f);
	readback.depth.resize(10);
	CHECK_FALSE(readback.is_valid());
	CHECK_FALSE(buffer.reproject_depth(readback, camera, projection, false, false));
}

} // namespace TestHZBOcclusion
