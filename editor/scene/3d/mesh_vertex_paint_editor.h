/**************************************************************************/
/*  mesh_vertex_paint_editor.h                                            */
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

#include "core/templates/local_vector.h"
#include "core/templates/pair.h"
#include "editor/docks/editor_dock.h"
#include "editor/plugins/editor_plugin.h"
#include "scene/3d/mesh_instance_3d.h"

class Button;
class ButtonGroup;
class CheckBox;
class ColorPickerButton;
class HSlider;
class ImmediateMesh;
class Label;
class OptionButton;
class Shader;
class ShaderMaterial;
class SpinBox;
class StandardMaterial3D;
class VBoxContainer;

// Vertex Paint: a set of tools to paint the vertex colors of the mesh of a MeshInstance3D in the 3D viewport.
// Opened from the "Mesh" menu of the MeshInstance3D editor.
class MeshVertexPaintEditor : public EditorDock {
	GDCLASS(MeshVertexPaintEditor, EditorDock);

public:
	enum Tool {
		TOOL_PAINT,
		TOOL_SMOOTH,
		TOOL_MAX,
	};

	enum BlendMode {
		BLEND_MIX,
		BLEND_ADD,
		BLEND_SUBTRACT,
		BLEND_MULTIPLY,
	};

	enum Preview {
		PREVIEW_MATERIAL,
		PREVIEW_VERTEX_COLORS,
		PREVIEW_RED,
		PREVIEW_GREEN,
		PREVIEW_BLUE,
		PREVIEW_ALPHA,
	};

private:
	// Vertices of a surface. When the surface has colors, they are edited in a copy of its attribute
	// buffer which is uploaded to the rendering server directly: the other attributes are left untouched.
	struct Surface {
		int index = 0;
		Mesh::PrimitiveType primitive = Mesh::PRIMITIVE_TRIANGLES;
		int vertex_count = 0;
		Vector<Vector3> vertices; // Local space.
		Vector<Vector3> normals;
		Vector<int> indices;
		Vector<Vector3> world_vertices;
		AABB world_aabb;

		bool has_colors = false;
		uint32_t stride = 0; // Stride of the interleaved attribute buffer.
		uint32_t color_offset = 0; // Offset of the RGBA8 color in the stride.
		Vector<uint8_t> attributes;

		// Smoothing: the vertices at the same position (split by UV or normal seams) are welded
		// into groups, which are connected by the edges of the primitives.
		bool adjacency_built = false;
		Vector<int> vertex_group;
		Vector<int> group_member_offsets;
		Vector<int> group_members;
		Vector<int> group_neighbor_offsets;
		Vector<int> group_neighbors;

		// Stroke.
		Vector<uint8_t> stroke_before; // RGBA8 colors at the beginning of the stroke.
		Vector<float> stroke_weight; // Highest brush weight of every vertex during the stroke.
		int stroke_begin = INT_MAX; // Range of the vertices changed by the stroke.
		int stroke_end = -1;
		int dirty_begin = INT_MAX; // Range of the vertices to upload.
		int dirty_end = -1;
	};

	MeshInstance3D *node = nullptr;
	ObjectID node_id;
	bool active = false;

	Ref<Mesh> cache_mesh;
	bool cache_valid = false;
	LocalVector<Surface> surfaces;
	Transform3D world_xform;
	Basis world_normal_basis;
	bool world_dirty = true;

	// Cursor.
	bool cursor_valid = false;
	Vector3 cursor_position;
	Vector3 cursor_normal;
	real_t cursor_radius_px = 0.0;
	Vector3 view_origin;
	Vector3 view_direction; // Towards the viewer.
	bool view_orthogonal = false;
	Ref<ImmediateMesh> cursor_mesh;
	Ref<StandardMaterial3D> cursor_line_material;
	Ref<StandardMaterial3D> cursor_point_material;
	RID cursor_instance;
	RID cursor_scenario;

	// Stroke.
	bool stroking = false;
	bool stroke_secondary = false;
	bool lmb_consumed = false;
	Color stroke_color;
	Point2 last_stroke_position;
	double smooth_timer = 0.0;
	double info_timer = 0.0;

	// Preview of the vertex colors, as the material override of the rendering instance (never saved).
	Ref<Shader> preview_shader;
	Ref<ShaderMaterial> preview_material;
	ObjectID preview_node_id;

	// UI.
	Label *info_label = nullptr;
	Label *warning_label = nullptr;
	Button *material_button = nullptr;
	Ref<ButtonGroup> tool_group;
	Button *tool_buttons[TOOL_MAX] = {};
	Tool tool = TOOL_PAINT;
	ColorPickerButton *primary_color = nullptr;
	ColorPickerButton *secondary_color = nullptr;
	Button *swap_button = nullptr;
	OptionButton *blend_mode = nullptr;
	Button *channel_buttons[4] = {};
	SpinBox *brush_size = nullptr;
	HSlider *brush_size_slider = nullptr;
	SpinBox *brush_strength = nullptr;
	SpinBox *brush_hardness = nullptr;
	CheckBox *front_faces_only = nullptr;
	CheckBox *show_vertices = nullptr;
	OptionButton *preview_mode = nullptr;
	Button *fill_button = nullptr;
	bool updating_ui = false;

	bool _validate_node();
	bool _is_painting_enabled() const;
	static bool _is_mesh_foreign(const Ref<Mesh> &p_mesh);
	static bool _needs_conversion(const Ref<Mesh> &p_mesh);
	static Ref<ArrayMesh> _create_paintable_mesh(const Ref<Mesh> &p_mesh);
	bool _prepare_mesh();
	bool _material_uses_vertex_colors() const;
	void _use_vertex_colors_in_material();

	void _clear_cache();
	bool _ensure_cache();
	void _mesh_changed();
	void _update_world_vertices();
	void _build_adjacency(Surface &r_surface);
	Surface *_find_surface(int p_index);

	bool _raycast(Camera3D *p_camera, const Point2 &p_position, Vector3 &r_position, Vector3 &r_normal) const;
	bool _update_cursor(Camera3D *p_camera, const Point2 &p_position);
	void _update_cursor_mesh();
	bool _is_vertex_facing_view(const Surface &p_surface, int p_vertex) const;
	bool _is_picking() const;
	int _get_channel_mask() const;

	void _begin_stroke(bool p_secondary);
	void _stroke_to(Camera3D *p_camera, const Point2 &p_position);
	void _dabs(const LocalVector<Vector3> &p_centers);
	void _smooth(Surface &r_surface, const LocalVector<Pair<int, float>> &p_affected, int p_channels);
	void _begin_surface_stroke(Surface &r_surface);
	void _end_stroke();
	void _clear_stroke();
	void _flush();
	void _pick_color();
	void _fill();
	void _commit_colors(const String &p_action);
	void _apply_colors(const Ref<ArrayMesh> &p_mesh, const Array &p_data);

	void _update_preview();
	void _update_info();
	void _tool_selected(int p_tool);
	void _swap_colors();
	void _brush_size_changed(double p_value);
	void _brush_size_slider_changed(double p_value);
	void _brush_setting_changed(double p_value);
	void _option_changed(int p_index);
	void _toggle_changed(bool p_pressed);
	void _dock_closed();

	Control *_add_setting(VBoxContainer *p_parent, const String &p_label, Control *p_control, const String &p_tooltip = String());
	SpinBox *_make_spin(double p_min, double p_max, double p_step, double p_value, const String &p_suffix = String());
	void _add_title(VBoxContainer *p_parent, const String &p_title);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	EditorPlugin::AfterGUIInput forward_3d_gui_input(Camera3D *p_camera, const Ref<InputEvent> &p_event);
	void edit(MeshInstance3D *p_node);
	void set_active(bool p_active);
	bool is_active() const { return active; }

	MeshVertexPaintEditor();
	~MeshVertexPaintEditor();
};
