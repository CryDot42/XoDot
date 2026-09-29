/**************************************************************************/
/*  mesh_vertex_paint_editor.cpp                                          */
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

#include "mesh_vertex_paint_editor.h"

#include "core/input/input.h"
#include "core/input/input_event.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/math/triangle_mesh.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/templates/hash_set.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "editor/scene/3d/node_3d_editor_viewport.h"
#include "editor/themes/editor_scale.h"
#include "scene/3d/camera_3d.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/check_box.h"
#include "scene/gui/color_picker.h"
#include "scene/gui/label.h"
#include "scene/gui/option_button.h"
#include "scene/gui/scroll_container.h"
#include "scene/gui/separator.h"
#include "scene/gui/slider.h"
#include "scene/gui/spin_box.h"
#include "scene/resources/immediate_mesh.h"
#include "scene/resources/material.h"
#include "servers/rendering/rendering_server.h"

// Shows the painted colors, or one of their channels as a grayscale mask.
static const char *preview_shader_code = R"(
// Vertex Paint preview.

shader_type spatial;
render_mode cull_disabled;

uniform int channel = 0;

void vertex() {
	vec3 color = COLOR.rgb;
	if (channel == 1) {
		color = vec3(COLOR.r);
	} else if (channel == 2) {
		color = vec3(COLOR.g);
	} else if (channel == 3) {
		color = vec3(COLOR.b);
	} else if (channel == 4) {
		color = vec3(COLOR.a);
	}
	// The colors are picked (and displayed) in sRGB.
	if (!OUTPUT_IS_SRGB) {
		color = mix(pow((color + vec3(0.055)) * (1.0 / 1.055), vec3(2.4)), color * (1.0 / 12.92), lessThan(color, vec3(0.04045)));
	}
	COLOR = vec4(color, 1.0);
}

void fragment() {
	ALBEDO = COLOR.rgb;
	ROUGHNESS = 0.9;
	SPECULAR = 0.25;
}
)";

static constexpr double SMOOTH_INTERVAL = 1.0 / 20.0;
static constexpr int MAX_CURSOR_POINTS = 16384;

static _FORCE_INLINE_ float _brush_falloff(real_t p_distance, real_t p_radius, float p_hardness) {
	const float t = p_radius > 0.0 ? float(p_distance / p_radius) : 1.0f;
	if (t >= 1.0f) {
		return 0.0f;
	}
	if (t <= p_hardness) {
		return 1.0f;
	}
	const float x = (t - p_hardness) / MAX(1.0f - p_hardness, 0.0001f);
	return 1.0f - x * x * (3.0f - 2.0f * x);
}

static _FORCE_INLINE_ uint8_t _blend_channel(uint8_t p_before, float p_color, float p_weight, int p_mode) {
	const float before = p_before / 255.0f;
	float result;
	switch (p_mode) {
		case MeshVertexPaintEditor::BLEND_ADD: {
			result = before + p_color * p_weight;
		} break;
		case MeshVertexPaintEditor::BLEND_SUBTRACT: {
			result = before - p_color * p_weight;
		} break;
		case MeshVertexPaintEditor::BLEND_MULTIPLY: {
			result = before * (1.0f + (p_color - 1.0f) * p_weight);
		} break;
		default: {
			result = before + (p_color - before) * p_weight;
		} break;
	}
	return uint8_t(CLAMP(Math::round(result * 255.0f), 0.0f, 255.0f));
}

/* Mesh */

bool MeshVertexPaintEditor::_validate_node() {
	if (node && !ObjectDB::get_instance(node_id)) {
		// The node was freed while being edited.
		node = nullptr;
		node_id = ObjectID();
		stroking = false;
		cursor_valid = false;
		_clear_cache();
	}
	return node != nullptr;
}

bool MeshVertexPaintEditor::_is_painting_enabled() const {
	return active && node && ObjectDB::get_instance(node_id) && node->is_inside_tree() && is_visible_in_tree();
}

bool MeshVertexPaintEditor::_is_mesh_foreign(const Ref<Mesh> &p_mesh) {
	const String path = p_mesh->get_path();
	if (path.is_empty()) {
		return false; // Built-in, saved with the edited scene.
	}
	String base = path;
	const int separator = path.find("::");
	if (separator != -1) {
		base = path.substr(0, separator);
		if (ResourceLoader::get_resource_type(base) == "PackedScene") {
			// Only the sub-resources of the edited scene are saved with it.
			const Node *edited_scene = EditorNode::get_singleton()->get_edited_scene();
			return !edited_scene || edited_scene->get_scene_file_path() != base;
		}
	}
	// The changes of imported resources would be lost when reimported.
	return FileAccess::exists(base + ".import");
}

bool MeshVertexPaintEditor::_needs_conversion(const Ref<Mesh> &p_mesh) {
	const Ref<ArrayMesh> array_mesh = p_mesh;
	if (array_mesh.is_null() || _is_mesh_foreign(p_mesh)) {
		return true;
	}
	for (int i = 0; i < array_mesh->get_surface_count(); i++) {
		const uint64_t format = uint64_t(array_mesh->surface_get_format(i));
		if (!(format & Mesh::ARRAY_FLAG_USE_2D_VERTICES) && !(format & Mesh::ARRAY_FORMAT_COLOR)) {
			return true;
		}
	}
	return false;
}

Ref<ArrayMesh> MeshVertexPaintEditor::_create_paintable_mesh(const Ref<Mesh> &p_mesh) {
	// A copy of the mesh with a color attribute in every surface. Everything else is copied as is
	// (compression, skinning, blend shapes, LODs), so the rendering of the mesh doesn't change.
	Ref<ArrayMesh> result;
	result.instantiate();
	result->set_name(p_mesh->get_name());

	const Ref<ArrayMesh> source = p_mesh;
	if (source.is_valid()) {
		for (int i = 0; i < source->get_blend_shape_count(); i++) {
			result->add_blend_shape(source->get_blend_shape_name(i));
		}
		result->set_blend_shape_mode(source->get_blend_shape_mode());
		result->set_custom_aabb(source->get_custom_aabb());
		result->set_shadow_mesh(source->get_shadow_mesh());
		result->set_lightmap_size_hint(source->get_lightmap_size_hint());
	}

	const RID rid = p_mesh->get_rid();
	for (int i = 0; i < p_mesh->get_surface_count(); i++) {
		RenderingServerTypes::SurfaceData data = RS::get_singleton()->mesh_get_surface(rid, i);
		ERR_FAIL_COND_V(data.vertex_count == 0, Ref<ArrayMesh>());

		if (!(data.format & RSE::ARRAY_FORMAT_COLOR) && !(data.format & RSE::ARRAY_FLAG_USE_2D_VERTICES)) {
			uint32_t offsets[RSE::ARRAY_MAX];
			uint32_t vertex_size = 0;
			uint32_t normal_size = 0;
			uint32_t skin_size = 0;
			uint32_t old_stride = 0;
			uint32_t new_stride = 0;
			RS::get_singleton()->mesh_surface_make_offsets_from_format(data.format, data.vertex_count, data.index_count, offsets, vertex_size, normal_size, old_stride, skin_size);
			const uint64_t format = data.format | RSE::ARRAY_FORMAT_COLOR;
			RS::get_singleton()->mesh_surface_make_offsets_from_format(format, data.vertex_count, data.index_count, offsets, vertex_size, normal_size, new_stride, skin_size);
			// The color is the first attribute of the interleaved attribute buffer.
			ERR_FAIL_COND_V(offsets[RSE::ARRAY_COLOR] != 0 || new_stride != old_stride + 4, Ref<ArrayMesh>());
			ERR_FAIL_COND_V(data.attribute_data.size() < int64_t(old_stride) * data.vertex_count, Ref<ArrayMesh>());

			Vector<uint8_t> attributes;
			attributes.resize(int64_t(new_stride) * data.vertex_count);
			uint8_t *w = attributes.ptrw();
			const uint8_t *r = data.attribute_data.ptr();
			for (uint32_t v = 0; v < data.vertex_count; v++) {
				memset(w + v * new_stride, 255, 4); // Opaque white, as without vertex colors.
				if (old_stride) {
					memcpy(w + v * new_stride + 4, r + v * old_stride, old_stride);
				}
			}
			data.format = format;
			data.attribute_data = attributes;
		}

		result->add_surface(data.format, Mesh::PrimitiveType(data.primitive), data.vertex_data, data.attribute_data, data.skin_data, data.vertex_count, data.index_data, data.index_count, data.aabb, data.blend_shape_data, data.bone_aabbs, data.lods, data.uv_scale);
		result->surface_set_material(i, p_mesh->surface_get_material(i));
		if (source.is_valid()) {
			result->surface_set_name(i, source->surface_get_name(i));
		}
	}
	return result;
}

bool MeshVertexPaintEditor::_prepare_mesh() {
	const Ref<Mesh> mesh = node->get_mesh();
	if (mesh.is_null() || mesh->get_surface_count() == 0) {
		EditorNode::get_singleton()->show_warning(TTR("The MeshInstance3D has no mesh to paint."));
		return false;
	}
	if (!_needs_conversion(mesh)) {
		return true;
	}

	const Ref<ArrayMesh> paintable = _create_paintable_mesh(mesh);
	if (paintable.is_null()) {
		EditorNode::get_singleton()->show_warning(TTR("Vertex colors can't be added to this mesh."));
		return false;
	}
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(TTR("Add Vertex Colors to Mesh"), UndoRedo::MERGE_DISABLE, node);
	undo_redo->add_do_method(node, "set_mesh", paintable);
	undo_redo->add_do_reference(paintable.ptr());
	undo_redo->add_undo_method(node, "set_mesh", mesh);
	undo_redo->add_do_method(this, "_update_info");
	undo_redo->add_undo_method(this, "_update_info");
	undo_redo->commit_action();
	_update_preview(); // The rendering instance may have been recreated.
	return _ensure_cache();
}

bool MeshVertexPaintEditor::_material_uses_vertex_colors() const {
	const Ref<Mesh> mesh = node ? node->get_mesh() : Ref<Mesh>();
	if (mesh.is_null()) {
		return true;
	}
	for (int i = 0; i < mesh->get_surface_count(); i++) {
		const Ref<Material> surface_material = node->get_active_material(i);
		const Ref<BaseMaterial3D> base_material = surface_material;
		// The default material doesn't use them, shaders are assumed to.
		if (surface_material.is_null() || (base_material.is_valid() && !base_material->get_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR))) {
			return false;
		}
	}
	return true;
}

void MeshVertexPaintEditor::_use_vertex_colors_in_material() {
	if (!_validate_node() || node->get_mesh().is_null()) {
		return;
	}
	const Ref<Mesh> mesh = node->get_mesh();
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(TTR("Use Vertex Colors in Material"), UndoRedo::MERGE_DISABLE, node);
	HashSet<BaseMaterial3D *> edited;
	for (int i = 0; i < mesh->get_surface_count(); i++) {
		const Ref<Material> surface_material = node->get_active_material(i);
		if (surface_material.is_null()) {
			Ref<StandardMaterial3D> new_material;
			new_material.instantiate();
			new_material->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
			new_material->set_flag(BaseMaterial3D::FLAG_SRGB_VERTEX_COLOR, true);
			undo_redo->add_do_method(node, "set_surface_override_material", i, new_material);
			undo_redo->add_undo_method(node, "set_surface_override_material", i, node->get_surface_override_material(i));
			continue;
		}
		const Ref<BaseMaterial3D> base_material = surface_material;
		if (base_material.is_null() || base_material->get_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR) || edited.has(base_material.ptr())) {
			continue;
		}
		edited.insert(base_material.ptr());
		undo_redo->add_do_property(base_material.ptr(), "vertex_color_use_as_albedo", true);
		undo_redo->add_undo_property(base_material.ptr(), "vertex_color_use_as_albedo", false);
		// The colors are picked in sRGB.
		if (!base_material->get_flag(BaseMaterial3D::FLAG_SRGB_VERTEX_COLOR)) {
			undo_redo->add_do_property(base_material.ptr(), "vertex_color_is_srgb", true);
			undo_redo->add_undo_property(base_material.ptr(), "vertex_color_is_srgb", false);
		}
	}
	undo_redo->add_do_method(this, "_update_info");
	undo_redo->add_undo_method(this, "_update_info");
	undo_redo->commit_action();
}

/* Cache */

void MeshVertexPaintEditor::_clear_cache() {
	if (cache_mesh.is_valid()) {
		cache_mesh->disconnect_changed(callable_mp(this, &MeshVertexPaintEditor::_mesh_changed));
	}
	cache_mesh.unref();
	cache_valid = false;
	surfaces.clear();
	world_dirty = true;
	stroking = false;
}

bool MeshVertexPaintEditor::_ensure_cache() {
	const Ref<Mesh> mesh = node ? node->get_mesh() : Ref<Mesh>();
	if (cache_valid && mesh == cache_mesh) {
		return !surfaces.is_empty();
	}
	_clear_cache();
	if (mesh.is_null()) {
		return false;
	}

	// The colors can only be edited in place in an ArrayMesh with a color attribute,
	// the vertices of the other meshes are still shown under the brush.
	const Ref<ArrayMesh> array_mesh = mesh;
	for (int i = 0; i < mesh->get_surface_count(); i++) {
		const uint64_t format = uint64_t(mesh->surface_get_format(i));
		if (format & Mesh::ARRAY_FLAG_USE_2D_VERTICES) {
			continue;
		}
		const Array arrays = mesh->surface_get_arrays(i);
		ERR_CONTINUE(arrays.size() != Mesh::ARRAY_MAX);

		Surface surface;
		surface.index = i;
		surface.primitive = mesh->surface_get_primitive_type(i);
		surface.vertices = arrays[Mesh::ARRAY_VERTEX];
		surface.normals = arrays[Mesh::ARRAY_NORMAL];
		surface.indices = arrays[Mesh::ARRAY_INDEX];
		surface.vertex_count = surface.vertices.size();
		if (surface.vertex_count == 0) {
			continue;
		}
		if (surface.normals.size() != surface.vertex_count) {
			surface.normals.clear();
		}

		if (array_mesh.is_valid() && (format & Mesh::ARRAY_FORMAT_COLOR)) {
			const RenderingServerTypes::SurfaceData surface_data = RS::get_singleton()->mesh_get_surface(array_mesh->get_rid(), i);
			uint32_t offsets[RSE::ARRAY_MAX];
			uint32_t vertex_size = 0;
			uint32_t normal_size = 0;
			uint32_t skin_size = 0;
			RS::get_singleton()->mesh_surface_make_offsets_from_format(format, surface.vertex_count, surface_data.index_count, offsets, vertex_size, normal_size, surface.stride, skin_size);
			surface.color_offset = offsets[RSE::ARRAY_COLOR];
			surface.attributes = surface_data.attribute_data;
			surface.has_colors = surface.stride >= surface.color_offset + 4 && surface.attributes.size() >= int64_t(surface.stride) * surface.vertex_count;
			if (!surface.has_colors) {
				surface.attributes.clear();
			}
		}
		surfaces.push_back(surface);
	}

	// Connected after reading the surfaces, which may update a primitive mesh.
	cache_mesh = mesh;
	cache_mesh->connect_changed(callable_mp(this, &MeshVertexPaintEditor::_mesh_changed));
	cache_valid = true;
	world_dirty = true;
	return !surfaces.is_empty();
}

void MeshVertexPaintEditor::_mesh_changed() {
	// Edited from somewhere else (the inspector, a script...), read it again when needed.
	cache_valid = false;
	if (stroking) {
		_clear_stroke();
	}
	callable_mp(this, &MeshVertexPaintEditor::_update_info).call_deferred();
}

void MeshVertexPaintEditor::_update_world_vertices() {
	const Transform3D xform = node->get_global_transform();
	if (!world_dirty && xform == world_xform) {
		return;
	}
	world_dirty = false;
	world_xform = xform;
	world_normal_basis = Math::is_zero_approx(xform.basis.determinant()) ? Basis() : xform.basis.inverse().transposed();
	for (Surface &surface : surfaces) {
		surface.world_vertices.resize(surface.vertex_count);
		Vector3 *w = surface.world_vertices.ptrw();
		const Vector3 *r = surface.vertices.ptr();
		for (int v = 0; v < surface.vertex_count; v++) {
			w[v] = xform.xform(r[v]);
			if (v == 0) {
				surface.world_aabb = AABB(w[v], Vector3());
			} else {
				surface.world_aabb.expand_to(w[v]);
			}
		}
	}
}

void MeshVertexPaintEditor::_build_adjacency(Surface &r_surface) {
	if (r_surface.adjacency_built) {
		return;
	}
	r_surface.adjacency_built = true;
	const int vertex_count = r_surface.vertex_count;

	// Weld the vertices at the same position.
	r_surface.vertex_group.resize(vertex_count);
	int *vertex_group = r_surface.vertex_group.ptrw();
	int group_count = 0;
	{
		HashMap<Vector3, int> positions;
		const Vector3 *vertices = r_surface.vertices.ptr();
		for (int v = 0; v < vertex_count; v++) {
			HashMap<Vector3, int>::Iterator E = positions.find(vertices[v]);
			if (E) {
				vertex_group[v] = E->value;
			} else {
				positions.insert(vertices[v], group_count);
				vertex_group[v] = group_count++;
			}
		}
	}

	// Members of the groups.
	r_surface.group_member_offsets.resize(group_count + 1);
	r_surface.group_member_offsets.fill(0);
	int *member_offsets = r_surface.group_member_offsets.ptrw();
	for (int v = 0; v < vertex_count; v++) {
		member_offsets[vertex_group[v] + 1]++;
	}
	for (int g = 0; g < group_count; g++) {
		member_offsets[g + 1] += member_offsets[g];
	}
	r_surface.group_members.resize(vertex_count);
	{
		int *members = r_surface.group_members.ptrw();
		LocalVector<int> fill;
		fill.resize(group_count);
		for (int g = 0; g < group_count; g++) {
			fill[g] = member_offsets[g];
		}
		for (int v = 0; v < vertex_count; v++) {
			members[fill[vertex_group[v]]++] = v;
		}
	}

	// Edges between the groups.
	LocalVector<uint64_t> edges;
	const int *indices = r_surface.indices.ptr();
	const int index_count = r_surface.indices.size();
	const int element_count = index_count > 0 ? index_count : vertex_count;
	auto element = [&](int p_i) -> int {
		return index_count > 0 ? indices[p_i] : p_i;
	};
	auto add_edge = [&](int p_a, int p_b) {
		if (p_a < 0 || p_b < 0 || p_a >= vertex_count || p_b >= vertex_count) {
			return;
		}
		const uint64_t a = vertex_group[p_a];
		const uint64_t b = vertex_group[p_b];
		if (a != b) {
			edges.push_back((a << 32) | b);
			edges.push_back((b << 32) | a);
		}
	};
	switch (r_surface.primitive) {
		case Mesh::PRIMITIVE_TRIANGLES: {
			for (int i = 0; i + 2 < element_count; i += 3) {
				add_edge(element(i), element(i + 1));
				add_edge(element(i + 1), element(i + 2));
				add_edge(element(i + 2), element(i));
			}
		} break;
		case Mesh::PRIMITIVE_TRIANGLE_STRIP: {
			for (int i = 0; i + 2 < element_count; i++) {
				add_edge(element(i), element(i + 1));
				add_edge(element(i + 1), element(i + 2));
				add_edge(element(i + 2), element(i));
			}
		} break;
		case Mesh::PRIMITIVE_LINES: {
			for (int i = 0; i + 1 < element_count; i += 2) {
				add_edge(element(i), element(i + 1));
			}
		} break;
		case Mesh::PRIMITIVE_LINE_STRIP: {
			for (int i = 0; i + 1 < element_count; i++) {
				add_edge(element(i), element(i + 1));
			}
		} break;
		default: {
		} break;
	}
	edges.sort();

	r_surface.group_neighbor_offsets.resize(group_count + 1);
	r_surface.group_neighbor_offsets.fill(0);
	int *neighbor_offsets = r_surface.group_neighbor_offsets.ptrw();
	LocalVector<int> neighbors;
	for (uint32_t i = 0; i < edges.size(); i++) {
		if (i > 0 && edges[i] == edges[i - 1]) {
			continue;
		}
		neighbor_offsets[(edges[i] >> 32) + 1]++;
		neighbors.push_back(int(edges[i] & 0xFFFFFFFF));
	}
	for (int g = 0; g < group_count; g++) {
		neighbor_offsets[g + 1] += neighbor_offsets[g];
	}
	r_surface.group_neighbors.resize(neighbors.size());
	if (neighbors.size()) {
		memcpy(r_surface.group_neighbors.ptrw(), neighbors.ptr(), neighbors.size() * sizeof(int));
	}
}

MeshVertexPaintEditor::Surface *MeshVertexPaintEditor::_find_surface(int p_index) {
	for (Surface &surface : surfaces) {
		if (surface.index == p_index) {
			return &surface;
		}
	}
	return nullptr;
}

/* Cursor */

bool MeshVertexPaintEditor::_raycast(Camera3D *p_camera, const Point2 &p_position, Vector3 &r_position, Vector3 &r_normal) const {
	const Ref<Mesh> mesh = node->get_mesh();
	if (mesh.is_null()) {
		return false;
	}
	const Ref<TriangleMesh> triangle_mesh = mesh->generate_triangle_mesh();
	if (triangle_mesh.is_null()) {
		return false;
	}
	const Transform3D xform = node->get_global_transform();
	if (Math::is_zero_approx(xform.basis.determinant())) {
		return false;
	}
	const Transform3D inverse = xform.affine_inverse();
	const Vector3 from = inverse.xform(p_camera->project_ray_origin(p_position));
	const Vector3 direction = inverse.basis.xform(p_camera->project_ray_normal(p_position)).normalized();
	Vector3 position;
	Vector3 normal;
	if (!triangle_mesh->intersect_ray(from, direction, position, normal)) {
		return false;
	}
	r_position = xform.xform(position);
	r_normal = xform.basis.inverse().transposed().xform(normal).normalized();
	return true;
}

bool MeshVertexPaintEditor::_update_cursor(Camera3D *p_camera, const Point2 &p_position) {
	const Transform3D camera_xform = p_camera->get_global_transform();
	view_orthogonal = p_camera->get_projection() == Camera3D::PROJECTION_ORTHOGONAL;
	view_origin = camera_xform.origin;
	view_direction = camera_xform.basis.get_column(2).normalized();

	cursor_valid = _raycast(p_camera, p_position, cursor_position, cursor_normal);
	if (cursor_valid) {
		const Vector3 side = camera_xform.basis.get_column(0).normalized() * brush_size->get_value();
		cursor_radius_px = p_camera->unproject_position(cursor_position).distance_to(p_camera->unproject_position(cursor_position + side));
	}
	_update_cursor_mesh();
	return cursor_valid;
}

bool MeshVertexPaintEditor::_is_vertex_facing_view(const Surface &p_surface, int p_vertex) const {
	if (p_surface.normals.is_empty()) {
		return true;
	}
	const Vector3 normal = world_normal_basis.xform(p_surface.normals[p_vertex]);
	const Vector3 to_view = view_orthogonal ? view_direction : view_origin - p_surface.world_vertices[p_vertex];
	return normal.dot(to_view) > 0.0;
}

bool MeshVertexPaintEditor::_is_picking() const {
	return !stroking && Input::get_singleton()->is_key_pressed(Key::CMD_OR_CTRL);
}

int MeshVertexPaintEditor::_get_channel_mask() const {
	int mask = 0;
	for (int i = 0; i < 4; i++) {
		if (channel_buttons[i]->is_pressed()) {
			mask |= 1 << i;
		}
	}
	return mask;
}

void MeshVertexPaintEditor::_update_cursor_mesh() {
	if (!cursor_mesh.is_valid()) {
		return;
	}
	cursor_mesh->clear_surfaces();

	const bool show_cursor = cursor_valid && _is_painting_enabled();
	const RID scenario = (show_cursor && node->get_world_3d().is_valid()) ? node->get_world_3d()->get_scenario() : RID();
	if (scenario != cursor_scenario) {
		RS::get_singleton()->instance_set_scenario(cursor_instance, scenario);
		cursor_scenario = scenario;
	}
	if (!show_cursor) {
		RS::get_singleton()->instance_set_visible(cursor_instance, false);
		return;
	}

	const real_t radius = brush_size->get_value();
	const float hardness = brush_hardness->get_value();
	const bool picking = _is_picking();
	const Vector3 normal = cursor_normal.is_zero_approx() ? Vector3(0, 1, 0) : cursor_normal;
	const Vector3 tangent = normal.cross(Math::abs(normal.y) < 0.99 ? Vector3(0, 1, 0) : Vector3(1, 0, 0)).normalized();
	const Vector3 bitangent = normal.cross(tangent);

	Color color = Color(1, 1, 1);
	if (tool == TOOL_SMOOTH) {
		color = Color(1.0, 0.85, 0.3);
	} else if (!picking) {
		const bool secondary = stroking ? stroke_secondary : Input::get_singleton()->is_key_pressed(Key::SHIFT);
		color = (secondary ? secondary_color : primary_color)->get_pick_color().clamp();
		color.a = 1.0;
	}
	const Color contrast = color.get_luminance() > 0.5 ? Color(0, 0, 0, 0.6) : Color(1, 1, 1, 0.6);

	constexpr int SEGMENTS = 64;
	auto add_circle = [&](real_t p_radius, const Color &p_color) {
		cursor_mesh->surface_set_color(p_color);
		for (int i = 0; i < SEGMENTS; i++) {
			const real_t a = Math::TAU * i / SEGMENTS;
			const real_t b = Math::TAU * (i + 1) / SEGMENTS;
			cursor_mesh->surface_add_vertex(cursor_position + (tangent * Math::cos(a) + bitangent * Math::sin(a)) * p_radius);
			cursor_mesh->surface_add_vertex(cursor_position + (tangent * Math::cos(b) + bitangent * Math::sin(b)) * p_radius);
		}
	};
	cursor_mesh->surface_begin(Mesh::PRIMITIVE_LINES, cursor_line_material);
	add_circle(radius, color);
	add_circle(radius * 0.97, contrast);
	if (!picking && hardness > 0.01 && hardness < 0.99) {
		add_circle(radius * hardness, Color(color, 0.45));
	}
	cursor_mesh->surface_set_color(color);
	cursor_mesh->surface_add_vertex(cursor_position);
	cursor_mesh->surface_add_vertex(cursor_position + normal * radius * 0.35);
	cursor_mesh->surface_end();

	// The vertices under the brush, more opaque where the brush is stronger.
	if (show_vertices->is_pressed() && _ensure_cache()) {
		_update_world_vertices();
		const real_t radius_squared = radius * radius;
		const bool front_only = front_faces_only->is_pressed();
		bool begun = false;
		int count = 0;
		for (const Surface &surface : surfaces) {
			if (count >= MAX_CURSOR_POINTS) {
				break;
			}
			if (!surface.world_aabb.grow(radius).has_point(cursor_position)) {
				continue;
			}
			const Vector3 *world = surface.world_vertices.ptr();
			for (int v = 0; v < surface.vertex_count && count < MAX_CURSOR_POINTS; v++) {
				const real_t distance_squared = world[v].distance_squared_to(cursor_position);
				if (distance_squared > radius_squared || (front_only && !_is_vertex_facing_view(surface, v))) {
					continue;
				}
				if (!begun) {
					cursor_mesh->surface_begin(Mesh::PRIMITIVE_POINTS, cursor_point_material);
					begun = true;
				}
				const float falloff = picking ? 1.0f : _brush_falloff(Math::sqrt(distance_squared), radius, hardness);
				cursor_mesh->surface_set_color(Color(contrast, 0.3 + 0.7 * falloff));
				cursor_mesh->surface_add_vertex(world[v]);
				count++;
			}
		}
		if (begun) {
			cursor_mesh->surface_end();
		}
	}
	RS::get_singleton()->instance_set_visible(cursor_instance, true);
}

/* Painting */

void MeshVertexPaintEditor::_begin_surface_stroke(Surface &r_surface) {
	if (!r_surface.stroke_before.is_empty()) {
		return;
	}
	r_surface.stroke_before.resize(r_surface.vertex_count * 4);
	uint8_t *w = r_surface.stroke_before.ptrw();
	const uint8_t *r = r_surface.attributes.ptr();
	for (int v = 0; v < r_surface.vertex_count; v++) {
		memcpy(w + v * 4, r + v * r_surface.stride + r_surface.color_offset, 4);
	}
	r_surface.stroke_weight.resize(r_surface.vertex_count);
	r_surface.stroke_weight.fill(0.0f);
	r_surface.stroke_begin = INT_MAX;
	r_surface.stroke_end = -1;
}

void MeshVertexPaintEditor::_begin_stroke(bool p_secondary) {
	if (!_prepare_mesh() || !_ensure_cache()) {
		return;
	}
	stroking = true;
	stroke_secondary = p_secondary;
	stroke_color = (p_secondary ? secondary_color : primary_color)->get_pick_color().clamp();
	smooth_timer = SMOOTH_INTERVAL;
	LocalVector<Vector3> centers;
	centers.push_back(cursor_position);
	_dabs(centers);
	_flush();
	_update_cursor_mesh();
}

void MeshVertexPaintEditor::_stroke_to(Camera3D *p_camera, const Point2 &p_position) {
	// Dabs along the path on the screen: interpolating in 3D would paint across the gaps of the mesh.
	const real_t distance = last_stroke_position.distance_to(p_position);
	const real_t spacing = MAX(cursor_radius_px * 0.25, real_t(2.0));
	const int steps = CLAMP(int(Math::ceil(distance / spacing)), 1, 64);
	LocalVector<Vector3> centers;
	for (int i = 1; i <= steps; i++) {
		Vector3 position;
		Vector3 normal;
		if (_raycast(p_camera, last_stroke_position.lerp(p_position, real_t(i) / steps), position, normal)) {
			centers.push_back(position);
		}
	}
	last_stroke_position = p_position;
	_dabs(centers);
}

void MeshVertexPaintEditor::_dabs(const LocalVector<Vector3> &p_centers) {
	if (p_centers.is_empty()) {
		return;
	}
	_update_world_vertices();
	const real_t radius = brush_size->get_value();
	const real_t radius_squared = radius * radius;
	const float hardness = brush_hardness->get_value();
	const float strength = brush_strength->get_value();
	const bool front_only = front_faces_only->is_pressed();
	const int channels = _get_channel_mask();
	const int mode = blend_mode->get_selected_id();
	if (channels == 0 || strength <= 0.0f) {
		return;
	}

	AABB area(p_centers[0], Vector3());
	for (const Vector3 &center : p_centers) {
		area.expand_to(center);
	}
	area = area.grow(radius);

	LocalVector<int> candidates;
	LocalVector<Pair<int, float>> affected;
	for (Surface &surface : surfaces) {
		if (!surface.has_colors || !surface.world_aabb.intersects_inclusive(area)) {
			continue;
		}
		// The vertices which may be painted by the dabs, found once for all of them.
		candidates.clear();
		const Vector3 *world = surface.world_vertices.ptr();
		for (int v = 0; v < surface.vertex_count; v++) {
			if (area.has_point(world[v]) && (!front_only || _is_vertex_facing_view(surface, v))) {
				candidates.push_back(v);
			}
		}
		if (candidates.is_empty()) {
			continue;
		}

		for (const Vector3 &center : p_centers) {
			affected.clear();
			for (const int v : candidates) {
				const real_t distance_squared = world[v].distance_squared_to(center);
				if (distance_squared > radius_squared) {
					continue;
				}
				const float weight = strength * _brush_falloff(Math::sqrt(distance_squared), radius, hardness);
				if (weight > 0.0f) {
					affected.push_back(Pair<int, float>(v, weight));
				}
			}
			if (affected.is_empty()) {
				continue;
			}
			_begin_surface_stroke(surface);

			if (tool == TOOL_SMOOTH) {
				_smooth(surface, affected, channels);
				continue;
			}

			// Not accumulated: during a stroke, every vertex is blended from its color at the beginning
			// of the stroke with the highest weight of the brush over it, as in most painting software.
			uint8_t *attributes = surface.attributes.ptrw();
			float *stroke_weight = surface.stroke_weight.ptrw();
			const uint8_t *before = surface.stroke_before.ptr();
			for (const Pair<int, float> &E : affected) {
				const int v = E.first;
				if (E.second <= stroke_weight[v]) {
					continue;
				}
				stroke_weight[v] = E.second;
				uint8_t *color = attributes + v * surface.stride + surface.color_offset;
				for (int c = 0; c < 4; c++) {
					if (channels & (1 << c)) {
						color[c] = _blend_channel(before[v * 4 + c], stroke_color[c], E.second, mode);
					}
				}
				surface.stroke_begin = MIN(surface.stroke_begin, v);
				surface.stroke_end = MAX(surface.stroke_end, v);
				surface.dirty_begin = MIN(surface.dirty_begin, v);
				surface.dirty_end = MAX(surface.dirty_end, v);
			}
		}
	}
}

void MeshVertexPaintEditor::_smooth(Surface &r_surface, const LocalVector<Pair<int, float>> &p_affected, int p_channels) {
	_build_adjacency(r_surface);
	uint8_t *attributes = r_surface.attributes.ptrw();
	const int *vertex_group = r_surface.vertex_group.ptr();
	const int *member_offsets = r_surface.group_member_offsets.ptr();
	const int *members = r_surface.group_members.ptr();
	const int *neighbor_offsets = r_surface.group_neighbor_offsets.ptr();
	const int *neighbors = r_surface.group_neighbors.ptr();

	HashMap<int, Color> group_colors;
	auto get_group_color = [&](int p_group) -> Color {
		HashMap<int, Color>::Iterator E = group_colors.find(p_group);
		if (E) {
			return E->value;
		}
		Color sum(0, 0, 0, 0);
		for (int i = member_offsets[p_group]; i < member_offsets[p_group + 1]; i++) {
			const uint8_t *c = attributes + members[i] * r_surface.stride + r_surface.color_offset;
			sum += Color(c[0], c[1], c[2], c[3]);
		}
		sum /= MAX(member_offsets[p_group + 1] - member_offsets[p_group], 1) * 255.0f;
		group_colors.insert(p_group, sum);
		return sum;
	};

	// The targets are computed before any change, so the result doesn't depend on the order of the vertices.
	LocalVector<Color> targets;
	targets.resize(p_affected.size());
	for (uint32_t i = 0; i < p_affected.size(); i++) {
		const int group = vertex_group[p_affected[i].first];
		Color sum = get_group_color(group);
		int count = 1;
		for (int n = neighbor_offsets[group]; n < neighbor_offsets[group + 1]; n++) {
			sum += get_group_color(neighbors[n]);
			count++;
		}
		targets[i] = sum / count;
	}

	for (uint32_t i = 0; i < p_affected.size(); i++) {
		const int v = p_affected[i].first;
		const float weight = p_affected[i].second * 0.5f;
		uint8_t *color = attributes + v * r_surface.stride + r_surface.color_offset;
		bool changed = false;
		for (int c = 0; c < 4; c++) {
			if (!(p_channels & (1 << c))) {
				continue;
			}
			const uint8_t result = uint8_t(CLAMP(Math::round(Math::lerp(color[c] / 255.0f, targets[i][c], weight) * 255.0f), 0.0f, 255.0f));
			changed = changed || result != color[c];
			color[c] = result;
		}
		if (changed) {
			r_surface.stroke_begin = MIN(r_surface.stroke_begin, v);
			r_surface.stroke_end = MAX(r_surface.stroke_end, v);
			r_surface.dirty_begin = MIN(r_surface.dirty_begin, v);
			r_surface.dirty_end = MAX(r_surface.dirty_end, v);
		}
	}
}

void MeshVertexPaintEditor::_flush() {
	if (cache_mesh.is_null()) {
		return;
	}
	const RID rid = cache_mesh->get_rid();
	for (Surface &surface : surfaces) {
		if (surface.dirty_end < surface.dirty_begin) {
			continue;
		}
		const int begin = surface.dirty_begin * surface.stride;
		const int end = (surface.dirty_end + 1) * surface.stride;
		RS::get_singleton()->mesh_surface_update_attribute_region(rid, surface.index, begin, surface.attributes.slice(begin, end));
		surface.dirty_begin = INT_MAX;
		surface.dirty_end = -1;
	}
}

void MeshVertexPaintEditor::_end_stroke() {
	if (!stroking) {
		return;
	}
	stroking = false;
	_flush();
	_commit_colors(tool == TOOL_SMOOTH ? TTR("Vertex Paint: Smooth") : TTR("Vertex Paint"));
	_update_cursor_mesh();
}

void MeshVertexPaintEditor::_clear_stroke() {
	stroking = false;
	for (Surface &surface : surfaces) {
		surface.stroke_before.clear();
		surface.stroke_weight.clear();
		surface.stroke_begin = INT_MAX;
		surface.stroke_end = -1;
	}
}

void MeshVertexPaintEditor::_commit_colors(const String &p_action) {
	// Only the range of the changed vertices is kept in the history.
	Array undo_data;
	Array redo_data;
	for (Surface &surface : surfaces) {
		if (surface.stroke_end >= surface.stroke_begin) {
			const int count = surface.stroke_end - surface.stroke_begin + 1;
			Vector<uint8_t> after;
			after.resize(count * 4);
			uint8_t *w = after.ptrw();
			const uint8_t *r = surface.attributes.ptr();
			for (int i = 0; i < count; i++) {
				memcpy(w + i * 4, r + (surface.stroke_begin + i) * surface.stride + surface.color_offset, 4);
			}
			undo_data.push_back(surface.index);
			undo_data.push_back(surface.stroke_begin);
			undo_data.push_back(surface.stroke_before.slice(surface.stroke_begin * 4, (surface.stroke_end + 1) * 4));
			redo_data.push_back(surface.index);
			redo_data.push_back(surface.stroke_begin);
			redo_data.push_back(after);
		}
	}
	_clear_stroke();

	const Ref<ArrayMesh> mesh = cache_mesh;
	if (undo_data.is_empty() || mesh.is_null() || !_validate_node()) {
		return;
	}
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(p_action, UndoRedo::MERGE_DISABLE, node);
	undo_redo->add_do_method(this, "_apply_colors", mesh, redo_data);
	undo_redo->add_undo_method(this, "_apply_colors", mesh, undo_data);
	undo_redo->commit_action(false);
	if (!mesh->get_path().is_empty()) {
		mesh->set_edited(true); // Saved with the scene if it's an external resource.
	}
}

void MeshVertexPaintEditor::_apply_colors(const Ref<ArrayMesh> &p_mesh, const Array &p_data) {
	ERR_FAIL_COND(p_mesh.is_null());
	ERR_FAIL_COND(p_data.size() % 3 != 0);
	if (stroking) {
		_clear_stroke();
	}

	const bool cached = cache_valid && cache_mesh == p_mesh;
	const RID rid = p_mesh->get_rid();
	for (int i = 0; i < p_data.size(); i += 3) {
		const int surface_index = p_data[i];
		const int first = p_data[i + 1];
		const Vector<uint8_t> colors = p_data[i + 2];
		const int count = colors.size() / 4;
		ERR_CONTINUE(surface_index < 0 || surface_index >= p_mesh->get_surface_count());
		const uint64_t format = uint64_t(p_mesh->surface_get_format(surface_index));
		ERR_CONTINUE(!(format & Mesh::ARRAY_FORMAT_COLOR));
		const int vertex_count = p_mesh->surface_get_array_len(surface_index);
		ERR_CONTINUE(first < 0 || count <= 0 || first + count > vertex_count);

		Surface *surface = cached ? _find_surface(surface_index) : nullptr;
		Vector<uint8_t> uncached;
		Vector<uint8_t> *attributes = nullptr;
		uint32_t stride = 0;
		uint32_t color_offset = 0;
		if (surface && surface->has_colors) {
			attributes = &surface->attributes;
			stride = surface->stride;
			color_offset = surface->color_offset;
		} else {
			const RenderingServerTypes::SurfaceData surface_data = RS::get_singleton()->mesh_get_surface(rid, surface_index);
			uint32_t offsets[RSE::ARRAY_MAX];
			uint32_t vertex_size = 0;
			uint32_t normal_size = 0;
			uint32_t skin_size = 0;
			RS::get_singleton()->mesh_surface_make_offsets_from_format(format, vertex_count, surface_data.index_count, offsets, vertex_size, normal_size, stride, skin_size);
			color_offset = offsets[RSE::ARRAY_COLOR];
			uncached = surface_data.attribute_data;
			attributes = &uncached;
		}
		ERR_CONTINUE(attributes->size() < int64_t(stride) * vertex_count);

		uint8_t *w = attributes->ptrw();
		const uint8_t *r = colors.ptr();
		for (int v = 0; v < count; v++) {
			memcpy(w + (first + v) * stride + color_offset, r + v * 4, 4);
		}
		RS::get_singleton()->mesh_surface_update_attribute_region(rid, surface_index, first * stride, attributes->slice(first * stride, (first + count) * stride));
	}
	if (!p_mesh->get_path().is_empty()) {
		p_mesh->set_edited(true);
	}
}

void MeshVertexPaintEditor::_pick_color() {
	if (!cursor_valid || !_ensure_cache()) {
		return;
	}
	_update_world_vertices();
	// The color of the closest vertex, white if the mesh has no vertex colors yet.
	real_t best = Math::INF;
	Color picked = Color(1, 1, 1);
	for (const Surface &surface : surfaces) {
		if (!surface.has_colors) {
			continue;
		}
		const Vector3 *world = surface.world_vertices.ptr();
		for (int v = 0; v < surface.vertex_count; v++) {
			const real_t distance_squared = world[v].distance_squared_to(cursor_position);
			if (distance_squared < best) {
				best = distance_squared;
				const uint8_t *c = surface.attributes.ptr() + v * surface.stride + surface.color_offset;
				picked = Color(c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, c[3] / 255.0f);
			}
		}
	}
	primary_color->set_pick_color(picked);
	_update_cursor_mesh();
}

void MeshVertexPaintEditor::_fill() {
	if (!_validate_node() || stroking) {
		return;
	}
	if (!_prepare_mesh() || !_ensure_cache()) {
		return;
	}
	const Color color = primary_color->get_pick_color().clamp();
	const float strength = brush_strength->get_value();
	const int channels = _get_channel_mask();
	const int mode = blend_mode->get_selected_id();
	for (Surface &surface : surfaces) {
		if (!surface.has_colors) {
			continue;
		}
		_begin_surface_stroke(surface);
		uint8_t *attributes = surface.attributes.ptrw();
		const uint8_t *before = surface.stroke_before.ptr();
		for (int v = 0; v < surface.vertex_count; v++) {
			uint8_t *c = attributes + v * surface.stride + surface.color_offset;
			for (int i = 0; i < 4; i++) {
				if (channels & (1 << i)) {
					c[i] = _blend_channel(before[v * 4 + i], color[i], strength, mode);
				}
			}
		}
		surface.stroke_begin = 0;
		surface.stroke_end = surface.vertex_count - 1;
		surface.dirty_begin = 0;
		surface.dirty_end = surface.vertex_count - 1;
	}
	_flush();
	_commit_colors(TTR("Vertex Paint: Fill"));
}

/* Input */

EditorPlugin::AfterGUIInput MeshVertexPaintEditor::forward_3d_gui_input(Camera3D *p_camera, const Ref<InputEvent> &p_event) {
	if (!_validate_node() || !_is_painting_enabled()) {
		return EditorPlugin::AFTER_GUI_INPUT_PASS;
	}

	Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_valid()) {
		_update_cursor(p_camera, mm->get_position());
		if (stroking) {
			_stroke_to(p_camera, mm->get_position());
			return EditorPlugin::AFTER_GUI_INPUT_STOP;
		}
		return EditorPlugin::AFTER_GUI_INPUT_PASS;
	}

	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid() && mb->get_button_index() == MouseButton::LEFT) {
		if (mb->is_pressed()) {
			if (mb->is_alt_pressed()) {
				return EditorPlugin::AFTER_GUI_INPUT_PASS; // Keep the Maya-style navigation working.
			}
			// Consumed even outside of the mesh, so that a click next to it doesn't deselect it.
			lmb_consumed = true;
			if (!_update_cursor(p_camera, mb->get_position())) {
				return EditorPlugin::AFTER_GUI_INPUT_STOP;
			}
			if (mb->is_command_or_control_pressed()) {
				_pick_color();
				return EditorPlugin::AFTER_GUI_INPUT_STOP;
			}
			last_stroke_position = mb->get_position();
			_begin_stroke(mb->is_shift_pressed());
			return EditorPlugin::AFTER_GUI_INPUT_STOP;
		}
		if (stroking) {
			_update_cursor(p_camera, mb->get_position());
			_end_stroke();
		}
		if (lmb_consumed) {
			lmb_consumed = false;
			return EditorPlugin::AFTER_GUI_INPUT_STOP;
		}
		return EditorPlugin::AFTER_GUI_INPUT_PASS;
	}

	Ref<InputEventKey> k = p_event;
	if (k.is_valid()) {
		const Key keycode = k->get_keycode();
		if (k->is_pressed() && !k->is_echo() && !stroking && !k->is_command_or_control_pressed() && !k->is_alt_pressed()) {
			if (keycode == Key::BRACKETLEFT) {
				brush_size->set_value(brush_size->get_value() / 1.25);
				return EditorPlugin::AFTER_GUI_INPUT_STOP;
			}
			if (keycode == Key::BRACKETRIGHT) {
				brush_size->set_value(brush_size->get_value() * 1.25);
				return EditorPlugin::AFTER_GUI_INPUT_STOP;
			}
		}
		if (keycode == Key::SHIFT || keycode == Key::CTRL || keycode == Key::META) {
			// The color of the cursor shows the effect of the modifiers.
			callable_mp(this, &MeshVertexPaintEditor::_update_cursor_mesh).call_deferred();
		}
	}
	return EditorPlugin::AFTER_GUI_INPUT_PASS;
}

/* UI */

void MeshVertexPaintEditor::_update_preview() {
	const bool painting = _is_painting_enabled();
	const int mode = preview_mode ? preview_mode->get_selected_id() : PREVIEW_MATERIAL;
	bool gizmo_changed = false;

	MeshInstance3D *previewed = ObjectDB::get_instance<MeshInstance3D>(preview_node_id);
	if (previewed && (!painting || previewed != node)) {
		const Ref<Material> material_override = previewed->get_material_override();
		RS::get_singleton()->instance_geometry_set_material_override(previewed->get_instance(), material_override.is_valid() ? material_override->get_rid() : RID());
		gizmo_changed = !previewed->is_transform_gizmo_visible();
		previewed->set_transform_gizmo_visible(true);
	}
	preview_node_id = ObjectID();

	if (painting) {
		RID instance_material;
		if (mode == PREVIEW_MATERIAL) {
			const Ref<Material> material_override = node->get_material_override();
			instance_material = material_override.is_valid() ? material_override->get_rid() : RID();
		} else {
			preview_material->set_shader_parameter("channel", mode - PREVIEW_VERTEX_COLORS);
			instance_material = preview_material->get_rid();
		}
		RS::get_singleton()->instance_geometry_set_material_override(node->get_instance(), instance_material);
		// The transform gizmo would be in the way of the brush.
		gizmo_changed = gizmo_changed || node->is_transform_gizmo_visible();
		node->set_transform_gizmo_visible(false);
		preview_node_id = node->get_instance_id();
	}
	if (gizmo_changed && Node3DEditor::get_singleton()) {
		Node3DEditor::get_singleton()->update_transform_gizmo();
	}
}

void MeshVertexPaintEditor::_update_info() {
	if (!info_label) {
		return;
	}
	_validate_node();
	String info;
	String warning;
	bool material_hint = false;
	const Ref<Mesh> mesh = node ? node->get_mesh() : Ref<Mesh>();
	if (!node) {
		info = TTR("Select a MeshInstance3D to paint the colors of its vertices.");
	} else if (mesh.is_null() || mesh->get_surface_count() == 0) {
		info = vformat(TTR("%s has no mesh to paint."), String(node->get_name()));
	} else {
		int64_t vertex_count = 0;
		for (int i = 0; i < mesh->get_surface_count(); i++) {
			vertex_count += mesh->surface_get_array_len(i);
		}
		info = vformat(TTR("%s: %s vertices, %d surface(s)."), String(node->get_name()), String::num_int64(vertex_count), mesh->get_surface_count());
		if (_needs_conversion(mesh)) {
			if (Ref<ArrayMesh>(mesh).is_null()) {
				warning = TTR("The mesh will be converted to an ArrayMesh with vertex colors when painting starts.");
			} else if (_is_mesh_foreign(mesh)) {
				warning = TTR("The mesh is imported or belongs to another scene: a local copy with vertex colors will be created when painting starts.");
			} else {
				warning = TTR("Vertex colors will be added to the mesh when painting starts.");
			}
		}
		if (!_material_uses_vertex_colors()) {
			material_hint = true;
			if (preview_mode->get_selected_id() == PREVIEW_MATERIAL) {
				warning += (warning.is_empty() ? "" : "\n") + TTR("The material doesn't display the vertex colors: use the Vertex Colors preview, or enable them in the material.");
			}
		}
	}
	info_label->set_text(info);
	warning_label->set_text(warning);
	warning_label->set_visible(!warning.is_empty());
	material_button->set_visible(material_hint);
	fill_button->set_disabled(mesh.is_null());
}

void MeshVertexPaintEditor::_tool_selected(int p_tool) {
	tool = Tool(p_tool);
	blend_mode->set_disabled(tool == TOOL_SMOOTH);
	_update_cursor_mesh();
}

void MeshVertexPaintEditor::_swap_colors() {
	const Color primary = primary_color->get_pick_color();
	primary_color->set_pick_color(secondary_color->get_pick_color());
	secondary_color->set_pick_color(primary);
	_update_cursor_mesh();
}

void MeshVertexPaintEditor::_brush_size_changed(double p_value) {
	if (!updating_ui) {
		updating_ui = true;
		brush_size_slider->set_value(p_value);
		updating_ui = false;
	}
	_update_cursor_mesh();
}

void MeshVertexPaintEditor::_brush_size_slider_changed(double p_value) {
	if (!updating_ui) {
		updating_ui = true;
		brush_size->set_value(p_value);
		updating_ui = false;
		_update_cursor_mesh();
	}
}

void MeshVertexPaintEditor::_brush_setting_changed(double p_value) {
	_update_cursor_mesh();
}

void MeshVertexPaintEditor::_option_changed(int p_index) {
	_update_preview();
	_update_info();
	_update_cursor_mesh();
}

void MeshVertexPaintEditor::_toggle_changed(bool p_pressed) {
	_update_cursor_mesh();
}

void MeshVertexPaintEditor::_dock_closed() {
	set_active(false);
}

Control *MeshVertexPaintEditor::_add_setting(VBoxContainer *p_parent, const String &p_label, Control *p_control, const String &p_tooltip) {
	HBoxContainer *hb = memnew(HBoxContainer);
	Label *label = memnew(Label(p_label));
	label->set_h_size_flags(SIZE_EXPAND_FILL);
	label->set_stretch_ratio(0.8);
	label->set_clip_text(true);
	hb->add_child(label);
	p_control->set_h_size_flags(SIZE_EXPAND_FILL);
	hb->add_child(p_control);
	if (!p_tooltip.is_empty()) {
		label->set_tooltip_text(p_tooltip);
		p_control->set_tooltip_text(p_tooltip);
	}
	p_parent->add_child(hb);
	return hb;
}

SpinBox *MeshVertexPaintEditor::_make_spin(double p_min, double p_max, double p_step, double p_value, const String &p_suffix) {
	SpinBox *spin = memnew(SpinBox);
	spin->set_min(p_min);
	spin->set_max(p_max);
	spin->set_step(p_step);
	spin->set_value(p_value);
	spin->set_suffix(p_suffix);
	spin->set_select_all_on_focus(true);
	return spin;
}

void MeshVertexPaintEditor::_add_title(VBoxContainer *p_parent, const String &p_title) {
	p_parent->add_child(memnew(HSeparator));
	Label *title_label = memnew(Label(p_title));
	title_label->set_theme_type_variation("HeaderSmall");
	p_parent->add_child(title_label);
}

void MeshVertexPaintEditor::edit(MeshInstance3D *p_node) {
	_validate_node();
	if (node == p_node) {
		_update_info();
		return;
	}
	_end_stroke();
	node = p_node;
	node_id = node ? node->get_instance_id() : ObjectID();
	_clear_cache();
	cursor_valid = false;
	_update_preview();
	_update_cursor_mesh();
	_update_info();
}

void MeshVertexPaintEditor::set_active(bool p_active) {
	if (active == p_active) {
		if (active) {
			make_visible();
		}
		return;
	}
	active = p_active;
	if (active) {
		make_visible();
		set_process(true);
	} else {
		_end_stroke();
		cursor_valid = false;
		lmb_consumed = false;
		set_process(false);
		close();
	}
	_update_preview();
	_update_cursor_mesh();
	_update_info();
}

void MeshVertexPaintEditor::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_POSTINITIALIZE: {
			// The signals of the class are only known once it's initialized, after the constructor.
			connect("closed", callable_mp(this, &MeshVertexPaintEditor::_dock_closed));
		} break;

		case NOTIFICATION_THEME_CHANGED: {
			tool_buttons[TOOL_PAINT]->set_button_icon(get_editor_theme_icon(SNAME("Paint")));
			tool_buttons[TOOL_SMOOTH]->set_button_icon(get_editor_theme_icon(SNAME("CurveLinear")));
			swap_button->set_button_icon(get_editor_theme_icon(SNAME("Loop")));
			fill_button->set_button_icon(get_editor_theme_icon(SNAME("Bucket")));
			material_button->set_button_icon(get_editor_theme_icon(SNAME("StandardMaterial3D")));
			warning_label->add_theme_color_override(SceneStringName(font_color), get_theme_color(SNAME("warning_color"), EditorStringName(Editor)));
			static const Color channel_colors[4] = { Color(1.0, 0.4, 0.4), Color(0.45, 1.0, 0.45), Color(0.5, 0.65, 1.0), Color(0.85, 0.85, 0.85) };
			for (int i = 0; i < 4; i++) {
				channel_buttons[i]->add_theme_color_override(SNAME("font_pressed_color"), channel_colors[i]);
				channel_buttons[i]->add_theme_color_override(SNAME("font_hover_pressed_color"), channel_colors[i]);
			}
		} break;

		case NOTIFICATION_PROCESS: {
			if (!_validate_node()) {
				break;
			}
			if (!Input::get_singleton()->is_mouse_button_pressed(MouseButton::LEFT)) {
				// The button was released outside of the viewport.
				_end_stroke();
				lmb_consumed = false;
			}
			if (stroking && tool == TOOL_SMOOTH && cursor_valid) {
				// Smoothing goes on while the button is held.
				smooth_timer -= get_process_delta_time();
				if (smooth_timer <= 0.0) {
					smooth_timer = MAX(smooth_timer + SMOOTH_INTERVAL, 0.0);
					LocalVector<Vector3> centers;
					centers.push_back(cursor_position);
					_dabs(centers);
				}
			}
			_flush();
			info_timer -= get_process_delta_time();
			if (info_timer <= 0.0) {
				// The mesh and its materials may also be edited in the inspector.
				info_timer = 0.5;
				_update_info();
			}
		} break;

		case NOTIFICATION_VISIBILITY_CHANGED: {
			if (!is_visible_in_tree()) {
				_end_stroke();
			} else if (!active) {
				// Only opened with the tools, from the "Mesh" menu.
				callable_mp((EditorDock *)this, &EditorDock::close).call_deferred();
			}
			_update_preview();
			_update_cursor_mesh();
		} break;
	}
}

void MeshVertexPaintEditor::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_apply_colors", "mesh", "data"), &MeshVertexPaintEditor::_apply_colors);
	ClassDB::bind_method(D_METHOD("_update_info"), &MeshVertexPaintEditor::_update_info);
}

MeshVertexPaintEditor::MeshVertexPaintEditor() {
	set_name(TTRC("Vertex Paint"));
	set_icon_name("Paint");
	set_default_slot(EditorDock::DOCK_SLOT_RIGHT_BL);
	set_available_layouts(EditorDock::DOCK_LAYOUT_VERTICAL | EditorDock::DOCK_LAYOUT_FLOATING);
	set_global(false);
	set_transient(true);
	set_closable(true);

	ScrollContainer *scroll = memnew(ScrollContainer);
	scroll->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
	add_child(scroll);
	VBoxContainer *main_vb = memnew(VBoxContainer);
	main_vb->set_h_size_flags(SIZE_EXPAND_FILL);
	scroll->add_child(main_vb);

	info_label = memnew(Label);
	info_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	info_label->set_custom_minimum_size(Size2(200 * EDSCALE, 0));
	main_vb->add_child(info_label);

	warning_label = memnew(Label);
	warning_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	warning_label->set_custom_minimum_size(Size2(200 * EDSCALE, 0));
	warning_label->hide();
	main_vb->add_child(warning_label);

	material_button = memnew(Button(TTR("Use Vertex Colors in Material")));
	material_button->set_tooltip_text(TTR("Enable \"Vertex Color > Use as Albedo\" in the materials of the mesh (a material is created for the surfaces without one)."));
	material_button->connect(SceneStringName(pressed), callable_mp(this, &MeshVertexPaintEditor::_use_vertex_colors_in_material));
	material_button->hide();
	main_vb->add_child(material_button);

	/* Tools */
	_add_title(main_vb, TTR("Tool"));
	{
		HBoxContainer *tools_hb = memnew(HBoxContainer);
		main_vb->add_child(tools_hb);
		tool_group.instantiate();
		const String names[TOOL_MAX] = { TTR("Paint"), TTR("Smooth") };
		const String tooltips[TOOL_MAX] = {
			TTR("Paint the color on the vertices under the brush.\nHold Shift to paint with the secondary color, Ctrl+Click picks the color of the vertex under the cursor."),
			TTR("Blur the colors of the vertices under the brush with their neighbors."),
		};
		for (int i = 0; i < TOOL_MAX; i++) {
			tool_buttons[i] = memnew(Button(names[i]));
			tool_buttons[i]->set_toggle_mode(true);
			tool_buttons[i]->set_button_group(tool_group);
			tool_buttons[i]->set_tooltip_text(tooltips[i]);
			tool_buttons[i]->set_h_size_flags(SIZE_EXPAND_FILL);
			tool_buttons[i]->connect(SceneStringName(pressed), callable_mp(this, &MeshVertexPaintEditor::_tool_selected).bind(i));
			tools_hb->add_child(tool_buttons[i]);
		}
		tool_buttons[TOOL_PAINT]->set_pressed(true);
	}

	/* Color */
	_add_title(main_vb, TTR("Color"));
	{
		HBoxContainer *colors_hb = memnew(HBoxContainer);
		main_vb->add_child(colors_hb);

		primary_color = memnew(ColorPickerButton);
		primary_color->set_pick_color(Color(1, 0, 0));
		primary_color->set_edit_intensity(false);
		primary_color->set_h_size_flags(SIZE_EXPAND_FILL);
		primary_color->set_custom_minimum_size(Size2(0, 26 * EDSCALE));
		primary_color->set_tooltip_text(TTR("Primary color, painted with the left mouse button."));
		primary_color->connect("color_changed", callable_mp(this, &MeshVertexPaintEditor::_update_cursor_mesh).unbind(1));
		colors_hb->add_child(primary_color);

		swap_button = memnew(Button);
		swap_button->set_flat(true);
		swap_button->set_tooltip_text(TTR("Swap the primary and secondary colors."));
		swap_button->connect(SceneStringName(pressed), callable_mp(this, &MeshVertexPaintEditor::_swap_colors));
		colors_hb->add_child(swap_button);

		secondary_color = memnew(ColorPickerButton);
		secondary_color->set_pick_color(Color(1, 1, 1));
		secondary_color->set_edit_intensity(false);
		secondary_color->set_h_size_flags(SIZE_EXPAND_FILL);
		secondary_color->set_custom_minimum_size(Size2(0, 26 * EDSCALE));
		secondary_color->set_tooltip_text(TTR("Secondary color, painted while holding Shift."));
		secondary_color->connect("color_changed", callable_mp(this, &MeshVertexPaintEditor::_update_cursor_mesh).unbind(1));
		colors_hb->add_child(secondary_color);

		blend_mode = memnew(OptionButton);
		blend_mode->add_item(TTR("Mix"), BLEND_MIX);
		blend_mode->add_item(TTR("Add"), BLEND_ADD);
		blend_mode->add_item(TTR("Subtract"), BLEND_SUBTRACT);
		blend_mode->add_item(TTR("Multiply"), BLEND_MULTIPLY);
		_add_setting(main_vb, TTR("Blend Mode"), blend_mode, TTR("How the color is combined with the colors of the vertices."));

		HBoxContainer *channels_hb = memnew(HBoxContainer);
		const char *channel_names[4] = { "R", "G", "B", "A" };
		const String channel_tooltips[4] = { TTR("Paint the red channel."), TTR("Paint the green channel."), TTR("Paint the blue channel."), TTR("Paint the alpha channel.") };
		for (int i = 0; i < 4; i++) {
			channel_buttons[i] = memnew(Button(channel_names[i]));
			channel_buttons[i]->set_toggle_mode(true);
			channel_buttons[i]->set_pressed(true);
			channel_buttons[i]->set_tooltip_text(channel_tooltips[i]);
			channel_buttons[i]->set_h_size_flags(SIZE_EXPAND_FILL);
			channels_hb->add_child(channel_buttons[i]);
		}
		_add_setting(main_vb, TTR("Channels"), channels_hb, TTR("The channels of the vertex colors which are painted, e.g. to paint the masks of a shader."));
	}

	/* Brush */
	_add_title(main_vb, TTR("Brush"));
	{
		brush_size = _make_spin(0.001, 1000.0, 0.001, 0.25, "m");
		brush_size->set_allow_greater(true);
		brush_size->connect(SceneStringName(value_changed), callable_mp(this, &MeshVertexPaintEditor::_brush_size_changed));
		_add_setting(main_vb, TTR("Size"), brush_size, TTR("Radius of the brush in the scene. Shortcuts: [ and ]."));
		brush_size_slider = memnew(HSlider);
		brush_size_slider->set_min(0.01);
		brush_size_slider->set_max(100.0);
		brush_size_slider->set_step(0.001);
		brush_size_slider->set_exp_ratio(true);
		brush_size_slider->set_value(0.25);
		brush_size_slider->connect(SceneStringName(value_changed), callable_mp(this, &MeshVertexPaintEditor::_brush_size_slider_changed));
		main_vb->add_child(brush_size_slider);

		brush_strength = _make_spin(0.0, 1.0, 0.01, 1.0);
		brush_strength->connect(SceneStringName(value_changed), callable_mp(this, &MeshVertexPaintEditor::_brush_setting_changed));
		_add_setting(main_vb, TTR("Strength"), brush_strength, TTR("Opacity of the color at the center of the brush. A stroke never paints more than it, even over the same vertices."));

		brush_hardness = _make_spin(0.0, 1.0, 0.01, 0.5);
		brush_hardness->connect(SceneStringName(value_changed), callable_mp(this, &MeshVertexPaintEditor::_brush_setting_changed));
		_add_setting(main_vb, TTR("Hardness"), brush_hardness, TTR("Part of the radius painted with the full strength, the strength fades out beyond it (inner circle of the cursor)."));

		front_faces_only = memnew(CheckBox(TTR("Front Faces Only")));
		front_faces_only->set_pressed(true);
		front_faces_only->set_tooltip_text(TTR("Only paint the vertices whose normal faces the camera, not the other side of the mesh."));
		front_faces_only->connect(SceneStringName(toggled), callable_mp(this, &MeshVertexPaintEditor::_toggle_changed));
		main_vb->add_child(front_faces_only);

		show_vertices = memnew(CheckBox(TTR("Show Vertices")));
		show_vertices->set_pressed(true);
		show_vertices->set_tooltip_text(TTR("Show the vertices under the brush, more opaque where the brush is stronger."));
		show_vertices->connect(SceneStringName(toggled), callable_mp(this, &MeshVertexPaintEditor::_toggle_changed));
		main_vb->add_child(show_vertices);
	}

	/* Display */
	_add_title(main_vb, TTR("Display"));
	{
		preview_mode = memnew(OptionButton);
		preview_mode->add_item(TTR("Material"), PREVIEW_MATERIAL);
		preview_mode->add_item(TTR("Vertex Colors"), PREVIEW_VERTEX_COLORS);
		preview_mode->add_item(TTR("Red Channel"), PREVIEW_RED);
		preview_mode->add_item(TTR("Green Channel"), PREVIEW_GREEN);
		preview_mode->add_item(TTR("Blue Channel"), PREVIEW_BLUE);
		preview_mode->add_item(TTR("Alpha Channel"), PREVIEW_ALPHA);
		preview_mode->select(PREVIEW_VERTEX_COLORS);
		preview_mode->connect(SceneStringName(item_selected), callable_mp(this, &MeshVertexPaintEditor::_option_changed));
		_add_setting(main_vb, TTR("Preview"), preview_mode, TTR("How the mesh is displayed while painting. The preview is only shown in the editor, the material of the mesh isn't changed."));
	}

	main_vb->add_child(memnew(HSeparator));
	fill_button = memnew(Button(TTR("Fill Mesh")));
	fill_button->set_tooltip_text(TTR("Apply the primary color to all the vertices of the mesh, with the blend mode, strength and channels of the brush."));
	fill_button->connect(SceneStringName(pressed), callable_mp(this, &MeshVertexPaintEditor::_fill));
	main_vb->add_child(fill_button);

	Button *stop_button = memnew(Button(TTR("Stop Painting")));
	stop_button->set_tooltip_text(TTR("Close the Vertex Paint tools and go back to the selection of nodes in the viewport."));
	stop_button->connect(SceneStringName(pressed), callable_mp(this, &MeshVertexPaintEditor::set_active).bind(false));
	main_vb->add_child(stop_button);

	Label *help = memnew(Label(TTR("Left click and drag on the mesh to paint, Shift to paint with the secondary color, Ctrl+Click to pick a color. [ and ] change the size of the brush.")));
	help->set_modulate(Color(1, 1, 1, 0.6));
	help->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	help->set_custom_minimum_size(Size2(200 * EDSCALE, 0));
	main_vb->add_child(help);

	/* Cursor */
	cursor_mesh.instantiate();
	cursor_line_material.instantiate();
	cursor_line_material->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
	cursor_line_material->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
	cursor_line_material->set_flag(BaseMaterial3D::FLAG_DISABLE_DEPTH_TEST, true);
	cursor_line_material->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
	cursor_line_material->set_flag(BaseMaterial3D::FLAG_SRGB_VERTEX_COLOR, true);
	cursor_line_material->set_flag(BaseMaterial3D::FLAG_DISABLE_FOG, true);
	cursor_line_material->set_render_priority(Material::RENDER_PRIORITY_MAX);
	cursor_point_material = cursor_line_material->duplicate();
	cursor_point_material->set_flag(BaseMaterial3D::FLAG_USE_POINT_SIZE, true);
	cursor_point_material->set_point_size(5 * EDSCALE);

	cursor_instance = RS::get_singleton()->instance_create();
	RS::get_singleton()->instance_set_base(cursor_instance, cursor_mesh->get_rid());
	RS::get_singleton()->instance_set_layer_mask(cursor_instance, 1 << Node3DEditorViewport::MISC_TOOL_LAYER);
	RS::get_singleton()->instance_geometry_set_cast_shadows_setting(cursor_instance, RSE::SHADOW_CASTING_SETTING_OFF);
	RS::get_singleton()->instance_set_ignore_culling(cursor_instance, true);
	RS::get_singleton()->instance_set_visible(cursor_instance, false);

	/* Preview */
	preview_shader.instantiate();
	preview_shader->set_code(preview_shader_code);
	preview_material.instantiate();
	preview_material->set_shader(preview_shader);

	_update_info();
}

MeshVertexPaintEditor::~MeshVertexPaintEditor() {
	MeshInstance3D *previewed = ObjectDB::get_instance<MeshInstance3D>(preview_node_id);
	if (previewed) {
		const Ref<Material> material_override = previewed->get_material_override();
		RS::get_singleton()->instance_geometry_set_material_override(previewed->get_instance(), material_override.is_valid() ? material_override->get_rid() : RID());
		previewed->set_transform_gizmo_visible(true);
	}
	_clear_cache();
	if (cursor_instance.is_valid()) {
		RS::get_singleton()->free_rid(cursor_instance);
	}
}
