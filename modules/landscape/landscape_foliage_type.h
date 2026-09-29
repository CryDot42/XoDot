/**************************************************************************/
/*  landscape_foliage_type.h                                              */
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

#include "core/io/resource.h"
#include "core/math/random_pcg.h"
#include "core/templates/local_vector.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"

// A kind of foliage painted on a Landscape3D by LandscapeFoliage3D (UE-like foliage type):
// the meshes of its levels of detail, how densely and how it is placed (scale, rotation,
// alignment to the ground, offset) and where it may grow (slope, height, landscape layers).
//
// The LOD levels (mesh, start distance and shadows of each level) and the cull distance are
// stored as `lod_count` and `lod_<n>/*` properties. The editor edits them in a separate window
// (the "LODs" row of the inspector), so they don't clutter the inspector.
class LandscapeFoliageType : public Resource {
	GDCLASS(LandscapeFoliageType, Resource);

public:
	static constexpr int MAX_LODS = 8;

	enum Scaling {
		SCALING_UNIFORM, // One random scale for all axes.
		SCALING_FREE, // Independent random scales for X, Y (vertical range) and Z.
		SCALING_LOCK_HORIZONTAL, // One random scale for X and Z, an independent one for Y.
	};

	struct Lod {
		Ref<Mesh> mesh; // Null: the mesh of the previous level (e.g. a level that only drops the shadows).
		float start_distance = 0.0; // Distance from which the level is used (0 for the first level).
		bool cast_shadows = true;
	};

	// Where an instance of the type is placed and how it is oriented, in the landscape space.
	struct Placement {
		Transform3D transform;
		Vector3 align_normal = Vector3(0, 1, 0); // Up vector of the alignment (clamped ground normal).
		float offset = 0.0; // Vertical offset from the ground.
		float random = 0.0; // Per-instance random value in [0, 1).
	};

private:
	// Rendering.
	LocalVector<Lod> lods; // lods[0].mesh is `mesh`.
	Ref<Material> material_override;
	float cull_distance = 0.0;
	float cull_random = 0.0;
	float lod_transition = 2.0;

	// Painting.
	float density = 10.0; // Instances per 100 m² (10 x 10 m).
	float radius = 0.0;

	// Scale.
	Scaling scaling = SCALING_UNIFORM;
	float scale_min = 1.0;
	float scale_max = 1.0;
	float vertical_scale_min = 1.0;
	float vertical_scale_max = 1.0;

	// Placement.
	float offset_min = 0.0;
	float offset_max = 0.0;
	bool align_to_normal = true;
	float align_max_angle = Math::deg_to_rad(30.0);
	bool random_yaw = true;
	float random_pitch = 0.0;

	// Filters.
	float slope_min = 0.0;
	float slope_max = Math::deg_to_rad(90.0);
	float height_min = -100000.0;
	float height_max = 100000.0;
	uint32_t layers = 0;
	uint32_t exclude_layers = 0;
	float layer_min_weight = 0.5;

	bool _parse_lod_property(const StringName &p_name, int &r_index, String &r_what) const;

protected:
	bool _set(const StringName &p_name, const Variant &p_value);
	bool _get(const StringName &p_name, Variant &r_ret) const;
	void _get_property_list(List<PropertyInfo> *p_list) const;
	void _validate_property(PropertyInfo &p_property) const;
	static void _bind_methods();

public:
	// Rendering.
	void set_mesh(const Ref<Mesh> &p_mesh);
	Ref<Mesh> get_mesh() const { return lods[0].mesh; }
	void set_material_override(const Ref<Material> &p_material);
	Ref<Material> get_material_override() const { return material_override; }

	// Levels of detail.
	void set_lod_count(int p_count);
	int get_lod_count() const { return lods.size(); }
	void add_lod(const Ref<Mesh> &p_mesh, float p_start_distance, bool p_cast_shadows = true);
	void remove_lod(int p_index);
	void set_lod_mesh(int p_index, const Ref<Mesh> &p_mesh);
	Ref<Mesh> get_lod_mesh(int p_index) const;
	Ref<Mesh> get_lod_effective_mesh(int p_index) const; // The mesh drawn at this level.
	void set_lod_start_distance(int p_index, float p_distance);
	float get_lod_start_distance(int p_index) const;
	float get_lod_end_distance(int p_index) const; // Start of the next level, or cull distance (0: never).
	void set_lod_cast_shadows(int p_index, bool p_enable);
	bool is_lod_casting_shadows(int p_index) const;
	void set_cull_distance(float p_distance);
	float get_cull_distance() const { return cull_distance; }
	void set_cull_random(float p_fraction);
	float get_cull_random() const { return cull_random; }
	void set_lod_transition(float p_distance);
	float get_lod_transition() const { return lod_transition; }
	// Local bounds of the meshes of every level.
	AABB get_mesh_bounds() const;

	// Painting.
	void set_density(float p_density);
	float get_density() const { return density; }
	void set_radius(float p_radius);
	float get_radius() const { return radius; }

	// Scale.
	void set_scaling(Scaling p_scaling);
	Scaling get_scaling() const { return scaling; }
	void set_scale_min(float p_scale);
	float get_scale_min() const { return scale_min; }
	void set_scale_max(float p_scale);
	float get_scale_max() const { return scale_max; }
	void set_vertical_scale_min(float p_scale);
	float get_vertical_scale_min() const { return vertical_scale_min; }
	void set_vertical_scale_max(float p_scale);
	float get_vertical_scale_max() const { return vertical_scale_max; }

	// Placement.
	void set_offset_min(float p_offset);
	float get_offset_min() const { return offset_min; }
	void set_offset_max(float p_offset);
	float get_offset_max() const { return offset_max; }
	void set_align_to_normal(bool p_enable);
	bool is_aligned_to_normal() const { return align_to_normal; }
	void set_align_max_angle(float p_radians);
	float get_align_max_angle() const { return align_max_angle; }
	void set_random_yaw(bool p_enable);
	bool has_random_yaw() const { return random_yaw; }
	void set_random_pitch(float p_radians);
	float get_random_pitch() const { return random_pitch; }

	// Filters.
	void set_slope_min(float p_radians);
	float get_slope_min() const { return slope_min; }
	void set_slope_max(float p_radians);
	float get_slope_max() const { return slope_max; }
	void set_height_min(float p_height);
	float get_height_min() const { return height_min; }
	void set_height_max(float p_height);
	float get_height_max() const { return height_max; }
	void set_layers(uint32_t p_layers);
	uint32_t get_layers() const { return layers; }
	void set_exclude_layers(uint32_t p_layers);
	uint32_t get_exclude_layers() const { return exclude_layers; }
	void set_layer_min_weight(float p_weight);
	float get_layer_min_weight() const { return layer_min_weight; }

	// Whether the type grows on ground with this normal (landscape space), global height and
	// the weights of the 16 landscape layers.
	bool accepts(const Vector3 &p_normal, real_t p_height, const float *p_weights) const;
	bool uses_layers() const { return (layers | exclude_layers) != 0; }
	// A random instance on the ground at this point (landscape space).
	Placement generate(const Vector3 &p_ground, const Vector3 &p_normal, RandomPCG &r_rng) const;
	// The normal the instances are aligned to on ground with this normal.
	Vector3 get_align_normal(const Vector3 &p_ground_normal) const;
	// Hash of what the rendering depends on (meshes, material, LODs).
	uint64_t get_render_hash() const;

	LandscapeFoliageType();
};

VARIANT_ENUM_CAST(LandscapeFoliageType::Scaling);
