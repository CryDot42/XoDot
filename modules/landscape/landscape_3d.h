/**************************************************************************/
/*  landscape_3d.h                                                        */
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

#include "landscape_data.h"
#include "landscape_gpu.h"
#include "landscape_layer.h"
#include "landscape_lod_tree.h"
#include "landscape_spline_system.h"
#include "landscape_streamer.h"

#include "core/templates/hash_map.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/shader.h"

class Camera3D;
class LandscapeSpline3D;
class TriangleMesh;

// Built-in terrain node.
//
// The terrain is rendered with a GPU quadtree: every frame a compute shader walks the
// quadtree, selects patches by screen-space error, frustum culls them and writes them
// straight into an indirect MultiMesh (no CPU readback). The patch vertex shader stitches
// LOD transitions and optionally subdivides below the heightmap resolution to add micro
// detail displacement from the painted material layers.
//
// The terrain data is streamed: only the pages (tiles of the data mip levels) needed by the
// current view are resident on the GPU, in a pool of fixed size (see LandscapeStreamer).
//
// LandscapeSpline3D descendants (roads, rivers, streams, lakes) modify the terrain
// non-destructively (see LandscapeSplineSystem).
class Landscape3D : public Node3D {
	GDCLASS(Landscape3D, Node3D);

public:
	static constexpr int MAX_LAYERS = LandscapeData::MAX_LAYERS;
	static constexpr int MAX_MICRO_LEVELS = 4;

	enum DebugView {
		DEBUG_VIEW_DISABLED,
		DEBUG_VIEW_LOD_LEVELS,
		DEBUG_VIEW_PATCHES,
		DEBUG_VIEW_NORMALS,
		DEBUG_VIEW_LAYERS,
		DEBUG_VIEW_STREAMING,
	};

	typedef Camera3D *(*EditorCameraCallback)();
	static EditorCameraCallback editor_camera_callback;

private:
	// Data and material.
	Ref<LandscapeData> data;
	Vector<Ref<LandscapeLayer>> layers;
	int layer_texture_size = 1024;
	Ref<Shader> shader_override;

	// LOD.
	int patch_size = 32;
	float lod_pixel_error = 2.0;
	int micro_detail_levels = 2;
	float displacement_scale = 1.0;
	int max_patches = 16384;
	int streaming_pool_size = 256; // MB.
	float texture_lod_bias = 0.5;
	float shadow_lod_bias = 1.0;
	float shadow_distance = 0.0;
	NodePath lod_camera_path;
	bool freeze_lod = false;
	DebugView debug_view = DEBUG_VIEW_DISABLED;

	// Rendering.
	uint32_t render_layers = 1;
	bool cast_shadows = true;

	// Collision.
	bool collision_enabled = true;
	uint32_t collision_layer = 1;
	uint32_t collision_mask = 1;
	real_t collision_priority = 1.0;
	int collision_tile_size = 256;
	int collision_lod = 0;
	real_t collision_radius = 0.0;

	// Rendering resources (RenderingServer).
	RID patch_mesh;
	RID multimeshes[2];
	RID instances[2];
	RID material;
	RID page_heights_texture;
	RID page_normals_texture;
	RID page_weights_textures[LandscapeData::MAX_WEIGHTMAPS];
	RID page_holes_texture;
	RID page_table_texture;
	bool gpu_holes = false;
	Ref<Texture2DArray> albedo_height_array;
	Ref<Texture2DArray> normal_roughness_array;
	static Ref<Shader> builtin_shader;
	static Ref<Shader> builtin_shader_holes;

#ifdef RD_ENABLED
	LandscapeGPU *gpu = nullptr;
	LandscapeStreamer streamer;
#endif
	bool rendering_supported = false;

	// CPU state.
	const LandscapeLodTree *_get_tree() const;
	int micro_levels_in_use = 0;
	int max_level_in_use = 0;
	float micro_amplitude = 0.0;

	bool full_update_pending = true;
	bool layers_dirty = true;
	bool layer_textures_dirty = true;
	bool lod_resources_dirty = true;
	bool lod_dirty = true;
	LocalVector<Rect2i> dirty_heights;
	LocalVector<Rect2i> dirty_weights;
	LocalVector<Rect2i> dirty_holes;
	Vector<uint8_t> last_lod_params[2];
	Vector3 last_lod_camera;
	Vector4 last_micro_params;
	int material_micro_levels = -1;
	int material_max_level = -1;

	// Camera state used for the LOD selection (kept while the LOD is frozen).
	struct LodCameraState {
		bool valid = false;
		Plane planes[6];
		Vector3 position;
		real_t projection_factor = 1.0;
		real_t scale = 1.0;
		bool orthogonal = false;
	} lod_camera;
	bool _update_lod_camera();
	Vector4 brush_preview;
	Color brush_color = Color(0.25, 0.6, 1.0);

	// Collision tiles.
	struct CollisionTile {
		RID body;
		RID shape;
		bool dirty = true;
	};
	HashMap<Vector2i, CollisionTile> collision_tiles;
	bool collision_full_rebuild = true;
	Rect2i collision_dirty_rect;
	double collision_timer = 0.0;
	Vector3 collision_last_center = Vector3(Math::INF, Math::INF, Math::INF);

	void _create_render_resources();
	void _free_render_resources();
	void _rebuild_patch_mesh();
	void _update_instances();
	void _update_material_shader();
	void _set_material_param(const StringName &p_name, const Variant &p_value);

	void _data_region_changed(const Rect2i &p_rect, int p_flags);
	void _data_changed();
	void _layer_changed();
	void _layer_textures_changed();
	void _connect_layer(const Ref<LandscapeLayer> &p_layer, bool p_connect);

	void _frame_pre_draw();
	void _process_pending_changes();
	void _full_update();
	void _upload_bounds(const LocalVector<LandscapeLodTree::Range> &p_ranges);
	void _page_region_changed(const Rect2i &p_rect);
	void _update_layer_params();
	void _rebuild_layer_textures();
	void _update_lod();
	Camera3D *_get_lod_camera() const;
	void _update_micro_detail();
	AABB _get_local_aabb() const;

	// Splines.
	LandscapeSplineSystem spline_system;
	void _update_splines(bool p_immediate);
	TypedArray<LandscapeSpline3D> _get_splines_bind() const;

	void _clear_collision();
	void _update_collision();
	void _mark_collision_dirty(const Rect2i &p_rect);
	bool _build_collision_tile(const Vector2i &p_tile, CollisionTile &r_tile);

	void _set_layers_bind(const TypedArray<LandscapeLayer> &p_layers);
	TypedArray<LandscapeLayer> _get_layers_bind() const;

protected:
	void _notification(int p_what);
	static void _bind_methods();
	void _validate_property(PropertyInfo &p_property) const;

public:
	void set_data(const Ref<LandscapeData> &p_data);
	Ref<LandscapeData> get_data() const { return data; }

	void set_layers(const Vector<Ref<LandscapeLayer>> &p_layers);
	const Vector<Ref<LandscapeLayer>> &get_layers() const { return layers; }
	int get_layer_count() const { return layers.size(); }
	Ref<LandscapeLayer> get_layer(int p_index) const;
	void set_layer_texture_size(int p_size);
	int get_layer_texture_size() const { return layer_texture_size; }
	void set_shader_override(const Ref<Shader> &p_shader);
	Ref<Shader> get_shader_override() const { return shader_override; }
	static String get_builtin_shader_code(bool p_holes = false);

	void set_patch_size(int p_size);
	int get_patch_size() const { return patch_size; }
	void set_lod_pixel_error(float p_error);
	float get_lod_pixel_error() const { return lod_pixel_error; }
	void set_micro_detail_levels(int p_levels);
	int get_micro_detail_levels() const { return micro_detail_levels; }
	void set_displacement_scale(float p_scale);
	float get_displacement_scale() const { return displacement_scale; }
	void set_max_patches(int p_count);
	int get_max_patches() const { return max_patches; }
	void set_streaming_pool_size(int p_megabytes);
	int get_streaming_pool_size() const { return streaming_pool_size; }
	void set_texture_lod_bias(float p_bias);
	float get_texture_lod_bias() const { return texture_lod_bias; }
	void set_shadow_lod_bias(float p_bias);
	float get_shadow_lod_bias() const { return shadow_lod_bias; }
	void set_shadow_distance(float p_distance);
	float get_shadow_distance() const { return shadow_distance; }
	void set_lod_camera_path(const NodePath &p_path);
	NodePath get_lod_camera_path() const { return lod_camera_path; }
	void set_freeze_lod(bool p_freeze);
	bool is_lod_frozen() const { return freeze_lod; }
	void set_debug_view(DebugView p_view);
	DebugView get_debug_view() const { return debug_view; }

	void set_render_layers(uint32_t p_layers);
	uint32_t get_render_layers() const { return render_layers; }
	void set_cast_shadows(bool p_enable);
	bool is_casting_shadows() const { return cast_shadows; }

	void set_collision_enabled(bool p_enabled);
	bool is_collision_enabled() const { return collision_enabled; }
	void set_collision_layer(uint32_t p_layer);
	uint32_t get_collision_layer() const { return collision_layer; }
	void set_collision_mask(uint32_t p_mask);
	uint32_t get_collision_mask() const { return collision_mask; }
	void set_collision_priority(real_t p_priority);
	real_t get_collision_priority() const { return collision_priority; }
	void set_collision_tile_size(int p_size);
	int get_collision_tile_size() const { return collision_tile_size; }
	void set_collision_lod(int p_lod);
	int get_collision_lod() const { return collision_lod; }
	void set_collision_radius(real_t p_radius);
	real_t get_collision_radius() const { return collision_radius; }
	void update_collision();

	// Queries.
	real_t get_height_at(const Vector3 &p_global_position) const;
	Vector3 get_normal_at(const Vector3 &p_global_position) const;
	bool intersect_ray(const Vector3 &p_from, const Vector3 &p_direction, Vector3 &r_position, Vector3 &r_normal, real_t p_max_distance = 1e6) const;
	Dictionary raycast(const Vector3 &p_from, const Vector3 &p_direction, real_t p_max_distance = 1e6) const;
	int get_dominant_layer_at(const Vector3 &p_global_position) const;
	AABB get_aabb() const;
	Ref<TriangleMesh> generate_selection_mesh(int p_resolution = 64) const;

	// Coordinates.
	Vector3 global_to_local(const Vector3 &p_global) const;
	Vector3 local_to_global(const Vector3 &p_local) const;
	Vector2 local_to_texel(const Vector3 &p_local) const;

	// Splines.
	void _register_spline(LandscapeSpline3D *p_spline);
	void _unregister_spline(LandscapeSpline3D *p_spline);
	void _spline_changed(LandscapeSpline3D *p_spline, bool p_force = false);
	const LocalVector<LandscapeSpline3D *> &get_splines() const { return spline_system.get_splines(); }
	void update_splines(); // Applies the pending spline changes to the terrain now.
	void rebuild_splines(); // Applies every spline again.
	bool has_pending_spline_changes() const { return spline_system.has_pending_changes(); }
	// Position of the camera used for the LOD (and to build spline meshes around it).
	bool get_view_position(Vector3 &r_global) const;

	// Editor support.
	void set_brush_preview(bool p_visible, const Vector3 &p_local_center = Vector3(), real_t p_radius = 0.0, real_t p_falloff = 0.0, const Color &p_color = Color(0.25, 0.6, 1.0));
	void force_update();
	Dictionary get_statistics() const;

	PackedStringArray get_configuration_warnings() const override;

	static void cleanup_shared_resources();

#ifdef RD_ENABLED
	void _page_built(LandscapeStreamer::BuildJob *p_job);
#endif

	Landscape3D();
	~Landscape3D();
};

VARIANT_ENUM_CAST(Landscape3D::DebugView);
