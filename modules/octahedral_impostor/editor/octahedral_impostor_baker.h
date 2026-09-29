/**************************************************************************/
/*  octahedral_impostor_baker.h                                           */
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

#include "../octahedral_impostor_material_3d.h"

#include "core/io/image.h"
#include "core/object/ref_counted.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"

class Mesh;
class Node;
class Node3D;
class Shader;
class ShaderMaterial;

// Bakes the views of a node and its visible descendants into the atlases of an octahedral
// impostor, with the rendering server of the editor (one draw per group of frames, all the
// frames of a group rendered at once as rotated copies of the geometry).
//
// Atlases (frames x frames views, view (x, y) at the tile of column x and row y):
// - Albedo: sRGB color and coverage (alpha).
// - Normal and depth: object space normal in RGB, depth in A from the front (0) to the back (1)
//   of the bounding sphere along the view.
// - ORM (optional): ambient occlusion, roughness and metallic.
class OctahedralImpostorBaker : public RefCounted {
	GDCLASS(OctahedralImpostorBaker, RefCounted);

public:
	static constexpr int MIN_ATLAS_SIZE = 64;
	static constexpr int MAX_ATLAS_SIZE = 8192;
	static constexpr int MAX_SUPERSAMPLING = 4;
	// Empty border of the frames, in pixels of the atlas.
	static constexpr int FRAME_PADDING = 2;
	// Largest viewport rendered at once, the atlas is rendered in several regions above it.
	static constexpr int MAX_REGION_SIZE = 2048;

	struct Geometry {
		ObjectID node;
		RID base;
		Ref<Resource> base_resource; // Keeps the mesh or multimesh alive during the bake.
		Transform3D transform; // Relative to the baked node.
		bool multimesh = false;
		Vector<Ref<Material>> surface_override_materials; // May contain null materials.
		Vector<Ref<Material>> surface_materials; // Materials of the surfaces without the material override.
		Ref<Material> material_override;
		Ref<Material> material_overlay;
		Vector<float> blend_shape_weights;
		Vector<Pair<StringName, Variant>> instance_parameters;
	};

private:
	enum Pass {
		PASS_ALBEDO,
		PASS_GEOMETRY,
		PASS_ORM,
		PASS_MAX
	};

	// Settings.
	OctahedralImpostorMaterial3D::Layout layout = OctahedralImpostorMaterial3D::LAYOUT_HEMISPHERE;
	int frames = 12;
	int atlas_size = 2048;
	int supersampling = 2;
	bool bake_orm = false;

	// Results.
	Ref<Image> albedo_image;
	Ref<Image> normal_depth_image;
	Ref<Image> orm_image;
	Vector3 sphere_center;
	float sphere_radius = 0.0;
	float source_roughness = 1.0;
	float source_metallic = 0.0;
	int baked_frames = 0;
	OctahedralImpostorMaterial3D::Layout baked_layout = OctahedralImpostorMaterial3D::LAYOUT_HEMISPHERE;

	// State of a bake.
	struct BakeState {
		int tile_size = 0; // Pixels of a frame in the atlas.
		int ss_tile_size = 0; // Pixels of a frame in the rendered images.
		int supersampling = 1;
		real_t margin_world = 0.0; // Distance from the camera to the front of the bounding sphere.
		bool use_normal_buffer = false;
		bool srgb_output[2] = {}; // Albedo pass, other passes.
		LocalVector<Basis> frame_bases;
		LocalVector<Geometry> geometry;
		RID scenario;
		RID viewport;
		RID camera;
		RID capture_instance;
		Ref<Mesh> capture_mesh;
		Ref<ShaderMaterial> capture_material;
		Ref<Shader> override_shaders[3]; // By cull mode.
		HashMap<ObjectID, Ref<ShaderMaterial>> override_materials;
		Ref<ShaderMaterial> default_override_material;
		LocalVector<Pair<RID, int>> instances; // Instance and index of its geometry.
		// Rendered albedo of the current region, weights the other maps.
		Ref<Image> region_albedo;
		// Pixels of the atlases with coverage, geometry data and ORM data, for the dilation.
		Vector<uint8_t> coverage_mask;
		Vector<uint8_t> geometry_mask;
		Vector<uint8_t> orm_mask;
	};

	static bool _is_impostor_material(const Ref<Material> &p_material);
	static void _collect_geometry(Node *p_node, const Transform3D &p_transform, bool p_is_root, bool p_first_only, LocalVector<Geometry> &r_geometry);
	void _estimate_surface_parameters(const LocalVector<Geometry> &p_geometry);

	Ref<ShaderMaterial> _get_override_material(BakeState &p_state, const Ref<Material> &p_material);
	void _setup_instances(BakeState &p_state, const Rect2i &p_tiles);
	void _set_pass(BakeState &p_state, Pass p_pass);
	void _free_instances(BakeState &p_state);
	void _draw(BakeState &p_state);
	bool _is_output_srgb(BakeState &p_state, Pass p_pass);
	void _read_region(BakeState &p_state, Pass p_pass, const Rect2i &p_tiles);
	void _cleanup(BakeState &p_state);
	static void _dilate(Ref<Image> &p_image, const Vector<uint8_t> &p_mask, int p_frames, int p_tile_size, bool p_keep_alpha);

	Error _bake_bind(Node *p_node, bool p_show_progress);

protected:
	static void _bind_methods();

public:
	void set_layout(OctahedralImpostorMaterial3D::Layout p_layout);
	OctahedralImpostorMaterial3D::Layout get_layout() const;

	void set_frames(int p_frames);
	int get_frames() const;

	void set_atlas_size(int p_size);
	int get_atlas_size() const;

	void set_supersampling(int p_factor);
	int get_supersampling() const;

	void set_bake_orm(bool p_enable);
	bool is_baking_orm() const;

	// Size of a frame and of the atlas that is baked with the current settings (the atlas is
	// rounded down to a multiple of the frames).
	int get_frame_size() const;
	int get_baked_atlas_size() const;

	// Geometry that is baked for the node: its visible meshes, multimeshes and CSG shapes, and
	// those of its descendants. Hidden nodes, shadow-only geometry, far levels of detail
	// (visibility range begin > 0) and impostors are skipped.
	static LocalVector<Geometry> collect_geometry(Node3D *p_node);
	static bool has_geometry(Node3D *p_node);
	// Sphere centered on the bounds of the geometry, through its farthest vertex.
	static bool compute_bounding_sphere(const LocalVector<Geometry> &p_geometry, Vector3 &r_center, float &r_radius);

	Error bake(Node3D *p_node, bool p_show_progress = false);

	Ref<Image> get_albedo_image() const { return albedo_image; }
	Ref<Image> get_normal_depth_image() const { return normal_depth_image; }
	Ref<Image> get_orm_image() const { return orm_image; }
	Vector3 get_sphere_center() const { return sphere_center; }
	float get_sphere_radius() const { return sphere_radius; }

	// Sets the baked parameters of a material (without its textures).
	void apply_to_material(const Ref<OctahedralImpostorMaterial3D> &p_material) const;
	// Material with the baked images as textures, e.g. for previews.
	Ref<OctahedralImpostorMaterial3D> create_material() const;
	// Saves the atlases next to the mesh file ("<name>_albedo.png", ...), imports them, and saves
	// a quad mesh with the material of the impostor. A mesh already loaded from this path is
	// updated in place.
	Ref<Mesh> save(const String &p_path);

	OctahedralImpostorBaker();
};
