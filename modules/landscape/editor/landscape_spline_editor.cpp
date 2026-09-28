/**************************************************************************/
/*  landscape_spline_editor.cpp                                           */
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

#include "landscape_spline_editor.h"

#include "../landscape_3d.h"
#include "../landscape_spline_materials.h"

#include "core/math/triangle_mesh.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "editor/scene/material_editor_plugin.h"
#include "editor/themes/editor_scale.h"
#include "scene/3d/camera_3d.h"

bool LandscapeSplineGizmoPlugin::snap_moved_points = false;

bool LandscapeSplineGizmoPlugin::has_gizmo(Node3D *p_spatial) {
	return Object::cast_to<LandscapeSpline3D>(p_spatial) != nullptr;
}

String LandscapeSplineGizmoPlugin::get_gizmo_name() const {
	return "LandscapeSpline3D";
}

int LandscapeSplineGizmoPlugin::get_priority() const {
	return -1;
}

Vector3 LandscapeSplineGizmoPlugin::_get_display_position(LandscapeSpline3D *p_spline, int p_index) {
	Vector3 position = p_spline->get_point_position(p_index);
	if (p_spline->get_type() == LandscapeSpline3D::TYPE_LAKE) {
		position.y = 0.0; // Lakes are flat, at the height of the node.
	}
	return position;
}

void LandscapeSplineGizmoPlugin::redraw(EditorNode3DGizmo *p_gizmo) {
	p_gizmo->clear();
	LandscapeSpline3D *spline = Object::cast_to<LandscapeSpline3D>(p_gizmo->get_node_3d());
	if (!spline) {
		return;
	}
	const LandscapeSpline3D::SplineType type = spline->get_type();
	const bool water = type != LandscapeSpline3D::TYPE_ROAD;
	const LocalVector<LandscapeSplineSample> &samples = spline->get_samples();

	if (samples.size() >= 2) {
		Vector<Vector3> center;
		Vector<Vector3> edges;
		Vector<Vector3> falloff;
		Vector<Vector3> arrows;
		const real_t falloff_width = spline->is_terrain_enabled() ? spline->get_terrain_falloff() : 0.0;
		real_t next_arrow = 0.0;
		for (uint32_t i = 0; i + 1 < samples.size(); i++) {
			const LandscapeSplineSample &a = samples[i];
			const LandscapeSplineSample &b = samples[i + 1];
			center.push_back(a.position);
			center.push_back(b.position);
			if (type == LandscapeSpline3D::TYPE_LAKE) {
				continue;
			}
			for (int side = -1; side <= 1; side += 2) {
				edges.push_back(a.position + a.right * (a.width * 0.5 * side));
				edges.push_back(b.position + b.right * (b.width * 0.5 * side));
				if (falloff_width > 0.0) {
					const Vector3 ra = Vector3(a.right.x, 0.0, a.right.z).normalized();
					const Vector3 rb = Vector3(b.right.x, 0.0, b.right.z).normalized();
					falloff.push_back(a.position + ra * ((a.width * 0.5 + falloff_width) * side));
					falloff.push_back(b.position + rb * ((b.width * 0.5 + falloff_width) * side));
				}
			}
			// Flow direction of rivers and streams.
			if (water && a.distance >= next_arrow) {
				const real_t size = CLAMP(real_t(a.width) * 0.3, real_t(0.5), real_t(4.0));
				next_arrow = a.distance + MAX(real_t(a.width) * 2.0, real_t(8.0));
				const Vector3 tip = a.position + a.forward * size;
				const Vector3 back = a.position - a.forward * (size * 0.5);
				arrows.push_back(tip);
				arrows.push_back(back - a.right * (size * 0.6));
				arrows.push_back(tip);
				arrows.push_back(back + a.right * (size * 0.6));
			}
		}
		p_gizmo->add_lines(center, get_material(water ? "water_line" : "road_line", p_gizmo));
		if (!edges.is_empty()) {
			p_gizmo->add_lines(edges, get_material("edge_line", p_gizmo));
		}
		if (!falloff.is_empty()) {
			p_gizmo->add_lines(falloff, get_material("falloff_line", p_gizmo));
		}
		if (!arrows.is_empty()) {
			p_gizmo->add_lines(arrows, get_material("water_line", p_gizmo));
		}
		p_gizmo->add_collision_segments(center);
		const Ref<TriangleMesh> selection = spline->generate_selection_mesh();
		if (selection.is_valid()) {
			p_gizmo->add_collision_triangles(selection);
		}
	}

	// Control points and width handles.
	const Vector<int> selected = p_gizmo->get_subgizmo_selection();
	Vector<Vector3> points;
	Vector<Vector3> selected_points;
	for (int i = 0; i < spline->get_point_count(); i++) {
		(selected.has(i) ? selected_points : points).push_back(_get_display_position(spline, i));
	}
	if (!points.is_empty()) {
		p_gizmo->add_vertices(points, get_material("points", p_gizmo), Mesh::PRIMITIVE_POINTS);
	}
	if (!selected_points.is_empty()) {
		Ref<StandardMaterial3D> selected_material = get_material("points_selected", p_gizmo);
		selected_material->set_albedo(Color(0.2, 0.55, 1.0));
		p_gizmo->add_vertices(selected_points, selected_material, Mesh::PRIMITIVE_POINTS);
	}
	if (type != LandscapeSpline3D::TYPE_LAKE && samples.size() >= 2) {
		Vector<Vector3> handles;
		Vector<int> ids;
		for (int i = 0; i < spline->get_point_count(); i++) {
			const LandscapeSplineSample frame = spline->get_point_frame(i);
			handles.push_back(frame.position - frame.right * (frame.width * 0.5));
			ids.push_back(i * 2);
			handles.push_back(frame.position + frame.right * (frame.width * 0.5));
			ids.push_back(i * 2 + 1);
		}
		p_gizmo->add_handles(handles, get_material("handles", p_gizmo), ids);
	}
}

String LandscapeSplineGizmoPlugin::get_handle_name(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) const {
	return TTR("Width");
}

Variant LandscapeSplineGizmoPlugin::get_handle_value(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) const {
	LandscapeSpline3D *spline = Object::cast_to<LandscapeSpline3D>(p_gizmo->get_node_3d());
	ERR_FAIL_NULL_V(spline, Variant());
	return spline->get_point_width(p_id / 2);
}

void LandscapeSplineGizmoPlugin::set_handle(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary, Camera3D *p_camera, const Point2 &p_point) {
	LandscapeSpline3D *spline = Object::cast_to<LandscapeSpline3D>(p_gizmo->get_node_3d());
	ERR_FAIL_NULL(spline);
	const int index = p_id / 2;
	ERR_FAIL_INDEX(index, spline->get_point_count());
	// The width follows the mouse on the plane of the cross section.
	const LandscapeSplineSample frame = spline->get_point_frame(index);
	const Transform3D inverse = spline->get_global_transform().affine_inverse();
	const Vector3 from = inverse.xform(p_camera->project_ray_origin(p_point));
	const Vector3 dir = inverse.basis.xform(p_camera->project_ray_normal(p_point)).normalized();
	Vector3 hit;
	if (!Plane(frame.up, frame.position).intersects_ray(from, dir, &hit)) {
		return;
	}
	real_t width = Math::abs((hit - frame.position).dot(frame.right)) * 2.0;
	if (Node3DEditor::get_singleton()->is_snap_enabled()) {
		width = Math::snapped(width, real_t(Node3DEditor::get_singleton()->get_translate_snap()));
	}
	spline->set_point_width(index, MAX(width, real_t(0.1)));
}

void LandscapeSplineGizmoPlugin::commit_handle(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary, const Variant &p_restore, bool p_cancel) {
	LandscapeSpline3D *spline = Object::cast_to<LandscapeSpline3D>(p_gizmo->get_node_3d());
	ERR_FAIL_NULL(spline);
	const int index = p_id / 2;
	if (p_cancel) {
		spline->set_point_width(index, p_restore);
		return;
	}
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(TTR("Set Landscape Spline Width"));
	undo_redo->add_do_method(spline, "set_point_width", index, spline->get_point_width(index));
	undo_redo->add_undo_method(spline, "set_point_width", index, p_restore);
	undo_redo->commit_action();
}

int LandscapeSplineGizmoPlugin::subgizmos_intersect_ray(const EditorNode3DGizmo *p_gizmo, Camera3D *p_camera, const Vector2 &p_point) const {
	LandscapeSpline3D *spline = Object::cast_to<LandscapeSpline3D>(p_gizmo->get_node_3d());
	ERR_FAIL_NULL_V(spline, -1);
	const Transform3D global = spline->get_global_transform();
	int best = -1;
	real_t best_distance = 16.0 * EDSCALE;
	for (int i = 0; i < spline->get_point_count(); i++) {
		const Vector3 position = global.xform(_get_display_position(spline, i));
		if (p_camera->is_position_behind(position)) {
			continue;
		}
		const real_t distance = p_camera->unproject_position(position).distance_to(p_point);
		if (distance < best_distance) {
			best_distance = distance;
			best = i;
		}
	}
	return best;
}

Vector<int> LandscapeSplineGizmoPlugin::subgizmos_intersect_frustum(const EditorNode3DGizmo *p_gizmo, const Camera3D *p_camera, const Vector<Plane> &p_frustum) const {
	Vector<int> result;
	LandscapeSpline3D *spline = Object::cast_to<LandscapeSpline3D>(p_gizmo->get_node_3d());
	ERR_FAIL_NULL_V(spline, result);
	const Transform3D global = spline->get_global_transform();
	for (int i = 0; i < spline->get_point_count(); i++) {
		const Vector3 position = global.xform(_get_display_position(spline, i));
		bool inside = true;
		for (const Plane &plane : p_frustum) {
			if (plane.distance_to(position) > 0.0) {
				inside = false;
				break;
			}
		}
		if (inside) {
			result.push_back(i);
		}
	}
	return result;
}

Transform3D LandscapeSplineGizmoPlugin::get_subgizmo_transform(const EditorNode3DGizmo *p_gizmo, int p_id) const {
	LandscapeSpline3D *spline = Object::cast_to<LandscapeSpline3D>(p_gizmo->get_node_3d());
	ERR_FAIL_NULL_V(spline, Transform3D());
	ERR_FAIL_INDEX_V(p_id, spline->get_point_count(), Transform3D());
	return Transform3D(Basis(), _get_display_position(spline, p_id));
}

void LandscapeSplineGizmoPlugin::set_subgizmo_transform(const EditorNode3DGizmo *p_gizmo, int p_id, Transform3D p_transform) {
	LandscapeSpline3D *spline = Object::cast_to<LandscapeSpline3D>(p_gizmo->get_node_3d());
	ERR_FAIL_NULL(spline);
	ERR_FAIL_INDEX(p_id, spline->get_point_count());
	Vector3 position = p_transform.origin;
	Landscape3D *landscape = spline->get_landscape();
	if (snap_moved_points && landscape && landscape->get_data().is_valid() && landscape->get_data()->is_valid() && spline->get_type() != LandscapeSpline3D::TYPE_LAKE) {
		// Follow the terrain as sculpted (without the splines).
		const Transform3D to_landscape = landscape->get_global_transform().affine_inverse() * spline->get_global_transform();
		Vector3 local = to_landscape.xform(position);
		local.y = landscape->get_data()->sample_edit_height(local.x, local.z);
		position = to_landscape.affine_inverse().xform(local);
	}
	spline->set_point_position(p_id, position);
}

void LandscapeSplineGizmoPlugin::commit_subgizmos(const EditorNode3DGizmo *p_gizmo, const Vector<int> &p_ids, const Vector<Transform3D> &p_restore, bool p_cancel) {
	LandscapeSpline3D *spline = Object::cast_to<LandscapeSpline3D>(p_gizmo->get_node_3d());
	ERR_FAIL_NULL(spline);
	PackedVector3Array before = spline->get_points();
	for (int i = 0; i < p_ids.size(); i++) {
		if (p_ids[i] < before.size()) {
			before.set(p_ids[i], p_restore[i].origin);
		}
	}
	if (p_cancel) {
		spline->set_points(before);
		return;
	}
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(TTR("Move Landscape Spline Points"));
	undo_redo->add_do_property(spline, "points", spline->get_points());
	undo_redo->add_undo_property(spline, "points", before);
	undo_redo->commit_action();
}

LandscapeSplineGizmoPlugin::LandscapeSplineGizmoPlugin() {
	create_material("road_line", Color(1.0, 0.62, 0.25), false, true);
	create_material("water_line", Color(0.35, 0.72, 1.0), false, true);
	create_material("edge_line", Color(0.9, 0.9, 0.9, 0.8));
	create_material("falloff_line", Color(0.6, 0.6, 0.6, 0.45));
	const Ref<Theme> theme = EditorNode::get_singleton()->get_editor_theme();
	create_handle_material("points", false, theme->get_icon(SNAME("EditorPathSmoothHandle"), EditorStringName(EditorIcons)));
	create_handle_material("points_selected", false, theme->get_icon(SNAME("EditorPathSmoothHandle"), EditorStringName(EditorIcons)));
	create_handle_material("handles");
}

/* Material conversion */

String LandscapeSplineMaterialConversionPlugin::converts_to() const {
	return "ShaderMaterial";
}

bool LandscapeSplineMaterialConversionPlugin::handles(const Ref<Resource> &p_resource) const {
	return Object::cast_to<LandscapeSplineMaterial>(p_resource.ptr()) != nullptr;
}

Ref<Resource> LandscapeSplineMaterialConversionPlugin::convert(const Ref<Resource> &p_resource) const {
	Ref<LandscapeSplineMaterial> material = p_resource;
	ERR_FAIL_COND_V(material.is_null(), Ref<Resource>());
	Ref<ShaderMaterial> result = MaterialEditor::make_shader_material(material, false);
	const bool water = Object::cast_to<LandscapeWaterMaterial>(material.ptr()) != nullptr;
	for (int i = 0; i < material->get_parameter_count(); i++) {
		const String name = material->get_parameter_name(i);
		Variant value = material->get_parameter_by_index(i);
		if (water && value.get_type() == Variant::NIL) {
			// Keep the look of the built-in textures.
			if (name == "normal_texture") {
				value = LandscapeWaterMaterial::get_default_normal_texture();
			} else if (name == "foam_texture") {
				value = LandscapeWaterMaterial::get_default_foam_texture();
			}
		}
		result->set_shader_parameter(name, value);
	}
	return result;
}
