/**************************************************************************/
/*  landscape_spline_3d.h                                                 */
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

#include "landscape_spline_curve.h"

#include "scene/3d/node_3d.h"
#include "scene/resources/material.h"

class ArrayMesh;
class Curve3D;
class Landscape3D;
class TriangleMesh;

// Road, river, stream or lake of a Landscape3D (UE-like landscape spline / water body).
//
// A smooth curve through control points with per-point width, depth, banking and flow speed:
// - a mesh is extruded along the curve (road surface, river surface) or fills it (lake surface),
//   split in chunks that are culled, LODed and only built near the camera,
// - the terrain of the parent Landscape3D is modified under it (flattened road bed, carved channel
//   or lake basin with banks) and a material layer is painted, non-destructively.
class LandscapeSpline3D : public Node3D {
	GDCLASS(LandscapeSpline3D, Node3D);

public:
	enum SplineType {
		TYPE_ROAD,
		TYPE_RIVER,
		TYPE_STREAM,
		TYPE_LAKE,
	};

	// Chunks built per frame and time budget of the lazy mesh building.
	static constexpr int MAX_CHUNK_BUILDS_PER_FRAME = 16;
	static constexpr uint64_t CHUNK_BUILD_BUDGET_USEC = 3000;

private:
	SplineType type = TYPE_ROAD;
	PackedVector3Array points;
	PackedFloat32Array point_widths;
	PackedFloat32Array point_depths;
	PackedFloat32Array point_tilts;
	PackedFloat32Array point_flow_speeds;
	bool closed = false;
	float width = 6.0; // Defaults of new points.
	float depth = 1.5;
	float flow_speed = 1.5;
	int64_t spline_id = 0;

	// Mesh.
	bool mesh_enabled = true;
	Ref<Material> material;
	float sample_spacing = 2.0;
	float lateral_spacing = 3.0;
	float water_overlap = 1.5;
	float skirt_depth = 0.3;
	float chunk_length = 128.0;
	float visibility_range = 0.0;
	float lod_bias = 1.0;
	uint32_t render_layers = 1;
	bool cast_shadows = false;

	// Terrain.
	bool terrain_enabled = true;
	int terrain_priority = 30;
	float terrain_strength = 1.0;
	float terrain_falloff = 6.0;
	float terrain_end_falloff = 6.0;
	float terrain_offset = 0.05;
	bool terrain_raise = true;
	bool terrain_lower = true;
	float bank_height = 0.4;
	float channel_shape = 2.0;
	float shore_width = 12.0;

	// Paint.
	int paint_layer = -1;
	float paint_width = 1.0;
	float paint_falloff = 2.0;
	float paint_noise = 0.5;
	float paint_strength = 1.0;

	// Curve (local space).
	LocalVector<LandscapeSplineSample> samples;
	LocalVector<int> segment_starts;
	bool curve_dirty = true;
	void _update_curve();
	void _build_points(LocalVector<LandscapeSplinePoint> &r_points) const;

	// Terrain shape (landscape space).
	Landscape3D *landscape = nullptr;
	LandscapeSplineTerrainShape terrain_shape;
	bool terrain_shape_dirty = true;
	Transform3D terrain_transform; // Spline to landscape transform of the terrain shape.
	uint64_t _compute_settings_hash(real_t p_spacing) const;

	// Mesh chunks.
	struct Chunk {
		int first = 0; // Ribbons: first and last sample (rows).
		int last = 0;
		Rect2i cells; // Lakes: grid cells.
		AABB aabb; // Local space.
		RID mesh;
		RID instance;
		bool built = false;
		int vertex_count = 0;
		int triangle_count = 0;
	};
	LocalVector<Chunk> chunks;
	bool chunks_dirty = true;
	// Lake surface grid.
	real_t lake_cell = 4.0;
	Vector2 lake_origin;
	Vector2i lake_cells;
	LocalVector<uint8_t> lake_mask; // Cells covered by the lake surface.
	int lateral_vertices = 2;

	void _free_chunk(Chunk &r_chunk);
	void _clear_chunks();
	void _rebuild_chunks();
	void _build_chunk(Chunk &r_chunk);
	void _build_ribbon_chunk(Chunk &r_chunk, Array &r_arrays, Dictionary &r_lods) const;
	void _build_lake_chunk(Chunk &r_chunk, Array &r_arrays) const;
	void _update_chunk_instances();
	void _process_chunks();
	bool _get_view_position(Vector3 &r_global) const;
	RID _get_material_rid() const;
	bool _is_water() const { return type != TYPE_ROAD; }

	// Changes.
	void _changed(bool p_shape = true);
	void _points_changed();
	void _resize_point_arrays();
	Transform3D _get_terrain_transform() const;

	Landscape3D *_find_landscape() const;

protected:
	void _notification(int p_what);
	static void _bind_methods();
	void _validate_property(PropertyInfo &p_property) const;

public:
	void set_type(SplineType p_type);
	SplineType get_type() const { return type; }
	void apply_type_defaults();

	// Points.
	void set_points(const PackedVector3Array &p_points);
	PackedVector3Array get_points() const { return points; }
	void set_point_widths(const PackedFloat32Array &p_widths);
	PackedFloat32Array get_point_widths() const { return point_widths; }
	void set_point_depths(const PackedFloat32Array &p_depths);
	PackedFloat32Array get_point_depths() const { return point_depths; }
	void set_point_tilts(const PackedFloat32Array &p_tilts);
	PackedFloat32Array get_point_tilts() const { return point_tilts; }
	void set_point_flow_speeds(const PackedFloat32Array &p_speeds);
	PackedFloat32Array get_point_flow_speeds() const { return point_flow_speeds; }

	int get_point_count() const { return points.size(); }
	void add_point(const Vector3 &p_position, int p_index = -1);
	void remove_point(int p_index);
	void clear_points();
	void set_point_position(int p_index, const Vector3 &p_position);
	Vector3 get_point_position(int p_index) const;
	void set_point_width(int p_index, float p_width);
	float get_point_width(int p_index) const;
	void set_point_depth(int p_index, float p_depth);
	float get_point_depth(int p_index) const;
	void set_point_tilt(int p_index, float p_tilt);
	float get_point_tilt(int p_index) const;
	void set_point_flow_speed(int p_index, float p_speed);
	float get_point_flow_speed(int p_index) const;

	void set_closed(bool p_closed);
	bool is_closed() const { return closed || type == TYPE_LAKE; }
	void set_width(float p_width);
	float get_width() const { return width; }
	void set_depth(float p_depth);
	float get_depth() const { return depth; }
	void set_flow_speed(float p_speed);
	float get_flow_speed() const { return flow_speed; }
	void set_spline_id(int64_t p_id);
	int64_t get_spline_id() const { return spline_id; }
	void regenerate_spline_id();

	// Mesh.
	void set_mesh_enabled(bool p_enabled);
	bool is_mesh_enabled() const { return mesh_enabled; }
	void set_material(const Ref<Material> &p_material);
	Ref<Material> get_material() const { return material; }
	void set_sample_spacing(float p_spacing);
	float get_sample_spacing() const { return sample_spacing; }
	void set_lateral_spacing(float p_spacing);
	float get_lateral_spacing() const { return lateral_spacing; }
	void set_water_overlap(float p_overlap);
	float get_water_overlap() const { return water_overlap; }
	void set_skirt_depth(float p_depth);
	float get_skirt_depth() const { return skirt_depth; }
	void set_chunk_length(float p_length);
	float get_chunk_length() const { return chunk_length; }
	void set_visibility_range(float p_range);
	float get_visibility_range() const { return visibility_range; }
	void set_lod_bias(float p_bias);
	float get_lod_bias() const { return lod_bias; }
	void set_render_layers(uint32_t p_layers);
	uint32_t get_render_layers() const { return render_layers; }
	void set_cast_shadows(bool p_enable);
	bool is_casting_shadows() const { return cast_shadows; }

	// Terrain.
	void set_terrain_enabled(bool p_enabled);
	bool is_terrain_enabled() const { return terrain_enabled; }
	void set_terrain_priority(int p_priority);
	int get_terrain_priority() const { return terrain_priority; }
	void set_terrain_strength(float p_strength);
	float get_terrain_strength() const { return terrain_strength; }
	void set_terrain_falloff(float p_falloff);
	float get_terrain_falloff() const { return terrain_falloff; }
	void set_terrain_end_falloff(float p_falloff);
	float get_terrain_end_falloff() const { return terrain_end_falloff; }
	void set_terrain_offset(float p_offset);
	float get_terrain_offset() const { return terrain_offset; }
	void set_terrain_raise(bool p_raise);
	bool is_terrain_raising() const { return terrain_raise; }
	void set_terrain_lower(bool p_lower);
	bool is_terrain_lowering() const { return terrain_lower; }
	void set_bank_height(float p_height);
	float get_bank_height() const { return bank_height; }
	void set_channel_shape(float p_shape);
	float get_channel_shape() const { return channel_shape; }
	void set_shore_width(float p_width);
	float get_shore_width() const { return shore_width; }

	// Paint.
	void set_paint_layer(int p_layer);
	int get_paint_layer() const { return paint_layer; }
	void set_paint_width(float p_width);
	float get_paint_width() const { return paint_width; }
	void set_paint_falloff(float p_falloff);
	float get_paint_falloff() const { return paint_falloff; }
	void set_paint_noise(float p_noise);
	float get_paint_noise() const { return paint_noise; }
	void set_paint_strength(float p_strength);
	float get_paint_strength() const { return paint_strength; }

	// Curve queries (local space).
	const LocalVector<LandscapeSplineSample> &get_samples();
	// Cross section of the curve at a control point.
	LandscapeSplineSample get_point_frame(int p_index);
	real_t get_length();
	Transform3D sample_transform(real_t p_distance);
	Vector3 sample_position(real_t p_distance);
	real_t get_closest_distance(const Vector3 &p_local_position);
	int get_closest_segment(const Vector3 &p_local_position, real_t *r_t = nullptr);
	Dictionary get_surface_info(const Vector3 &p_global_position);
	bool is_point_in_water(const Vector3 &p_global_position);
	Ref<Curve3D> create_curve();
	PackedVector3Array get_tessellated_points();

	// Terrain.
	Landscape3D *get_landscape() const { return landscape; }
	const LandscapeSplineTerrainShape &get_terrain_shape();
	void snap_to_terrain(float p_offset = 0.0);
	void level_lake_to_shore(float p_margin = 0.2);
	void make_downhill(float p_min_drop = 0.0);
	void reverse();

	// The whole mesh (all chunks, LOD 0) in a single surface, e.g. for navigation or export.
	Ref<ArrayMesh> create_mesh();

	// Editor support.
	Ref<TriangleMesh> generate_selection_mesh();
	void rebuild();
	Dictionary get_statistics() const;
	void _set_landscape(Landscape3D *p_landscape) { landscape = p_landscape; }
	void _terrain_invalidated(); // The landscape (transform, data) changed.

	PackedStringArray get_configuration_warnings() const override;

	static Ref<Material> get_default_material(SplineType p_type);
	static void cleanup_shared_resources();

	LandscapeSpline3D();
	~LandscapeSpline3D();
};

VARIANT_ENUM_CAST(LandscapeSpline3D::SplineType);
