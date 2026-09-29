/**************************************************************************/
/*  octahedral_impostor_baker.cpp                                         */
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

#include "octahedral_impostor_baker.h"

#include "core/io/config_file.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/object/class_db.h"
#include "editor/editor_node.h"
#include "editor/file_system/editor_file_system.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/multimesh.h"
#include "scene/resources/shader.h"
#include "servers/rendering/rendering_server.h"

// Full screen pass drawn after the geometry (Forward+): the view space normal of the normal and
// roughness buffer (so it includes the normal maps and custom shaders of any material) and the
// depth of the depth buffer.
static const char *capture_shader_code = R"(
shader_type spatial;
render_mode unshaded, cull_disabled, depth_test_disabled, depth_draw_never, shadows_disabled, fog_disabled;

uniform sampler2D depth_texture : hint_depth_texture, filter_nearest, repeat_disable;
uniform sampler2D normal_roughness_texture : hint_normal_roughness_texture, filter_nearest, repeat_disable;
uniform float depth_near = 0.0;
uniform float depth_range = 1.0;

vec2 octahedral_encode(vec3 n) {
	n /= abs(n.x) + abs(n.y) + abs(n.z);
	vec2 coord = n.xz;
	if (n.y < 0.0) {
		coord = (1.0 - abs(n.zx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.z >= 0.0 ? 1.0 : -1.0);
	}
	return coord;
}

void vertex() {
	POSITION = vec4(VERTEX.xy * 2.0, 1.0, 1.0);
}

void fragment() {
	float depth = texture(depth_texture, SCREEN_UV).r;
	vec4 view = INV_PROJECTION_MATRIX * vec4(SCREEN_UV * 2.0 - 1.0, depth, 1.0);
	float view_depth = -view.z / view.w;
	vec3 normal = normalize(texture(normal_roughness_texture, SCREEN_UV).xyz * 2.0 - 1.0);
	ALBEDO = vec3(octahedral_encode(normal) * 0.5 + 0.5, clamp((view_depth - depth_near) / depth_range, 0.0, 1.0));
	ALPHA = 1.0;
}
)";

// Unshaded quad of a known value, tells whether the data written by unshaded materials is read
// back as is.
static const char *calibration_shader_code = R"(
shader_type spatial;
render_mode unshaded, cull_disabled, depth_test_disabled, depth_draw_never, shadows_disabled, fog_disabled;

void vertex() {
	POSITION = vec4(VERTEX.xy * 2.0, 1.0, 1.0);
}

void fragment() {
	ALBEDO = vec3(0.2);
}
)";

// Replaces the materials of the geometry for the passes that need data the renderer doesn't keep
// (ORM, and the normals without the normal and roughness buffer). Mimics the parameters of
// BaseMaterial3D, and the usual parameter names of the shaders.
static String _get_override_shader_code(const String &p_cull_mode) {
	return String(R"(
shader_type spatial;
render_mode unshaded, depth_draw_opaque, shadows_disabled, fog_disabled, )") +
			p_cull_mode + R"(;

// 0: view space normal (octahedral) and depth. 1: ambient occlusion, roughness and metallic.
uniform int bake_mode = 0;
uniform float depth_near = 0.0;
uniform float depth_range = 1.0;

uniform vec4 albedo = vec4(1.0);
uniform sampler2D texture_albedo : hint_default_white, filter_linear_mipmap;
uniform bool use_vertex_color = false;
uniform float alpha_scissor_threshold = -1.0;
uniform vec3 uv1_scale = vec3(1.0);
uniform vec3 uv1_offset = vec3(0.0);
uniform bool use_normal_map = false;
uniform sampler2D texture_normal : hint_default_black, filter_linear_mipmap;
uniform float normal_scale = 1.0;
uniform float roughness = 1.0;
uniform sampler2D texture_roughness : hint_default_white, filter_linear_mipmap;
uniform vec4 roughness_channel = vec4(1.0, 0.0, 0.0, 0.0);
uniform float metallic = 0.0;
uniform sampler2D texture_metallic : hint_default_white, filter_linear_mipmap;
uniform vec4 metallic_channel = vec4(1.0, 0.0, 0.0, 0.0);
uniform sampler2D texture_ambient_occlusion : hint_default_white, filter_linear_mipmap;
uniform vec4 ao_channel = vec4(1.0, 0.0, 0.0, 0.0);

vec2 octahedral_encode(vec3 n) {
	n /= abs(n.x) + abs(n.y) + abs(n.z);
	vec2 coord = n.xz;
	if (n.y < 0.0) {
		coord = (1.0 - abs(n.zx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.z >= 0.0 ? 1.0 : -1.0);
	}
	return coord;
}

void vertex() {
	UV = UV * uv1_scale.xy + uv1_offset.xy;
}

void fragment() {
	float alpha = albedo.a * texture(texture_albedo, UV).a;
	if (use_vertex_color) {
		alpha *= COLOR.a;
	}
	if (alpha < alpha_scissor_threshold) {
		discard;
	}
	if (bake_mode == 0) {
		vec3 normal = NORMAL;
		if (use_normal_map) {
			vec3 normal_map;
			normal_map.xy = texture(texture_normal, UV).xy * 2.0 - 1.0;
			normal_map.z = sqrt(max(0.0, 1.0 - dot(normal_map.xy, normal_map.xy)));
			normal = normalize(mix(normal, TANGENT * normal_map.x + BINORMAL * normal_map.y + normal * normal_map.z, normal_scale));
		}
		ALBEDO = vec3(octahedral_encode(normalize(normal)) * 0.5 + 0.5, clamp((-VERTEX.z - depth_near) / depth_range, 0.0, 1.0));
	} else {
		float ao = dot(texture(texture_ambient_occlusion, UV), ao_channel);
		float rough = roughness * dot(texture(texture_roughness, UV), roughness_channel);
		float metal = metallic * dot(texture(texture_metallic, UV), metallic_channel);
		ALBEDO = vec3(ao, rough, metal);
	}
}
)";
}

static Vector4 _texture_channel_mask(BaseMaterial3D::TextureChannel p_channel) {
	switch (p_channel) {
		case BaseMaterial3D::TEXTURE_CHANNEL_RED:
			return Vector4(1, 0, 0, 0);
		case BaseMaterial3D::TEXTURE_CHANNEL_GREEN:
			return Vector4(0, 1, 0, 0);
		case BaseMaterial3D::TEXTURE_CHANNEL_BLUE:
			return Vector4(0, 0, 1, 0);
		case BaseMaterial3D::TEXTURE_CHANNEL_ALPHA:
			return Vector4(0, 0, 0, 1);
		default:
			return Vector4(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0, 0);
	}
}

// Value of the first uniform of the shader of the material with one of the names.
static Variant _find_shader_parameter(const Ref<ShaderMaterial> &p_material, const HashSet<StringName> &p_uniforms, std::initializer_list<const char *> p_names, Variant::Type p_type) {
	for (const char *name : p_names) {
		if (!p_uniforms.has(name)) {
			continue;
		}
		Variant value = p_material->get_shader_parameter(name);
		if (value.get_type() == Variant::NIL) {
			value = RS::get_singleton()->shader_get_parameter_default(p_material->get_shader()->get_rid(), name);
		}
		if (value.get_type() == p_type || (p_type == Variant::OBJECT && value.get_type() == Variant::NIL)) {
			return value;
		}
	}
	return Variant();
}

/* Settings */

void OctahedralImpostorBaker::set_layout(OctahedralImpostorMaterial3D::Layout p_layout) {
	ERR_FAIL_INDEX(p_layout, OctahedralImpostorMaterial3D::LAYOUT_MAX);
	layout = p_layout;
}

OctahedralImpostorMaterial3D::Layout OctahedralImpostorBaker::get_layout() const {
	return layout;
}

void OctahedralImpostorBaker::set_frames(int p_frames) {
	frames = CLAMP(p_frames, OctahedralImpostorMaterial3D::MIN_FRAMES, OctahedralImpostorMaterial3D::MAX_FRAMES);
}

int OctahedralImpostorBaker::get_frames() const {
	return frames;
}

void OctahedralImpostorBaker::set_atlas_size(int p_size) {
	atlas_size = CLAMP(p_size, MIN_ATLAS_SIZE, MAX_ATLAS_SIZE);
}

int OctahedralImpostorBaker::get_atlas_size() const {
	return atlas_size;
}

void OctahedralImpostorBaker::set_supersampling(int p_factor) {
	supersampling = CLAMP(p_factor, 1, MAX_SUPERSAMPLING);
}

int OctahedralImpostorBaker::get_supersampling() const {
	return supersampling;
}

void OctahedralImpostorBaker::set_bake_orm(bool p_enable) {
	bake_orm = p_enable;
}

bool OctahedralImpostorBaker::is_baking_orm() const {
	return bake_orm;
}

int OctahedralImpostorBaker::get_frame_size() const {
	// Multiple of 4, the size of the blocks of the compressed formats.
	return (atlas_size / frames) & ~3;
}

int OctahedralImpostorBaker::get_baked_atlas_size() const {
	return get_frame_size() * frames;
}

/* Geometry */

bool OctahedralImpostorBaker::_is_impostor_material(const Ref<Material> &p_material) {
	return Object::cast_to<OctahedralImpostorMaterial3D>(p_material.ptr()) != nullptr;
}

void OctahedralImpostorBaker::_collect_geometry(Node *p_node, const Transform3D &p_transform, bool p_is_root, bool p_first_only, LocalVector<Geometry> &r_geometry) {
	Transform3D transform = p_transform;
	Node3D *node_3d = Object::cast_to<Node3D>(p_node);
	if (node_3d && !p_is_root) {
		if (!node_3d->is_visible()) {
			return;
		}
		transform = p_transform * node_3d->get_transform();
	}

	GeometryInstance3D *geometry_instance = Object::cast_to<GeometryInstance3D>(p_node);
	if (geometry_instance && geometry_instance->get_cast_shadows_setting() != GeometryInstance3D::SHADOW_CASTING_SETTING_SHADOWS_ONLY && geometry_instance->get_visibility_range_begin() <= 0.0) {
		Geometry geometry;
		geometry.node = geometry_instance->get_instance_id();
		geometry.transform = transform;
		geometry.material_override = geometry_instance->get_material_override();
		geometry.material_overlay = geometry_instance->get_material_overlay();
		bool valid = false;

		MeshInstance3D *mesh_instance = Object::cast_to<MeshInstance3D>(p_node);
		MultiMeshInstance3D *multimesh_instance = Object::cast_to<MultiMeshInstance3D>(p_node);
		if (mesh_instance) {
			Ref<Mesh> mesh = mesh_instance->get_mesh();
			if (mesh.is_valid() && mesh->get_surface_count() > 0) {
				geometry.base = mesh->get_rid();
				geometry.base_resource = mesh;
				for (int i = 0; i < mesh->get_surface_count(); i++) {
					Ref<Material> surface_override;
					if (i < mesh_instance->get_surface_override_material_count()) {
						surface_override = mesh_instance->get_surface_override_material(i);
					}
					geometry.surface_override_materials.push_back(surface_override);
					geometry.surface_materials.push_back(surface_override.is_valid() ? surface_override : mesh->surface_get_material(i));
				}
				for (int i = 0; i < mesh_instance->get_blend_shape_count(); i++) {
					geometry.blend_shape_weights.push_back(mesh_instance->get_blend_shape_value(i));
				}
				valid = true;
			}
		} else if (multimesh_instance) {
			Ref<MultiMesh> multimesh = multimesh_instance->get_multimesh();
			if (multimesh.is_valid() && multimesh->get_mesh().is_valid() && multimesh->get_instance_count() > 0 && multimesh->get_transform_format() == MultiMesh::TRANSFORM_3D) {
				geometry.base = multimesh->get_rid();
				geometry.base_resource = multimesh;
				geometry.multimesh = true;
				const Ref<Mesh> mesh = multimesh->get_mesh();
				for (int i = 0; i < mesh->get_surface_count(); i++) {
					geometry.surface_materials.push_back(mesh->surface_get_material(i));
				}
				valid = true;
			}
		} else if (p_node->is_class("CSGShape3D") && bool(p_node->call("is_root_shape"))) {
			// The children shapes are part of the mesh of the root shape.
			const Array meshes = p_node->call("get_meshes");
			if (meshes.size() == 2 && Object::cast_to<Mesh>(meshes[1])) {
				Ref<Mesh> mesh = meshes[1];
				geometry.transform = transform * Transform3D(meshes[0]);
				geometry.base = mesh->get_rid();
				geometry.base_resource = mesh;
				for (int i = 0; i < mesh->get_surface_count(); i++) {
					geometry.surface_override_materials.push_back(Ref<Material>());
					geometry.surface_materials.push_back(mesh->surface_get_material(i));
				}
				valid = mesh->get_surface_count() > 0;
			}
		}

		// Skip the impostors, e.g. of a previous bake of the same node.
		if (_is_impostor_material(geometry.material_override)) {
			valid = false;
		}
		for (const Ref<Material> &material : geometry.surface_materials) {
			if (_is_impostor_material(material)) {
				valid = false;
			}
		}

		if (valid && p_first_only) {
			r_geometry.push_back(geometry);
			return;
		}
		if (valid) {
			List<PropertyInfo> properties;
			geometry_instance->get_property_list(&properties);
			for (const PropertyInfo &property : properties) {
				if (property.name.begins_with("instance_shader_parameters/")) {
					const StringName name = property.name.trim_prefix("instance_shader_parameters/");
					const Variant value = geometry_instance->get_instance_shader_parameter(name);
					if (value.get_type() != Variant::NIL) {
						geometry.instance_parameters.push_back(Pair<StringName, Variant>(name, value));
					}
				}
			}
			r_geometry.push_back(geometry);
		}
	}

	for (int i = 0; i < p_node->get_child_count(); i++) {
		_collect_geometry(p_node->get_child(i), transform, false, p_first_only, r_geometry);
		if (p_first_only && !r_geometry.is_empty()) {
			return;
		}
	}
}

LocalVector<OctahedralImpostorBaker::Geometry> OctahedralImpostorBaker::collect_geometry(Node3D *p_node) {
	LocalVector<Geometry> geometry;
	ERR_FAIL_NULL_V(p_node, geometry);
	_collect_geometry(p_node, Transform3D(), true, false, geometry);
	return geometry;
}

bool OctahedralImpostorBaker::has_geometry(Node3D *p_node) {
	ERR_FAIL_NULL_V(p_node, false);
	LocalVector<Geometry> geometry;
	_collect_geometry(p_node, Transform3D(), true, true, geometry);
	return !geometry.is_empty();
}

bool OctahedralImpostorBaker::compute_bounding_sphere(const LocalVector<Geometry> &p_geometry, Vector3 &r_center, float &r_radius) {
	AABB bounds;
	bool first = true;
	for (const Geometry &geometry : p_geometry) {
		AABB aabb;
		if (geometry.multimesh) {
			aabb = Ref<MultiMesh>(geometry.base_resource)->get_aabb();
		} else {
			aabb = Ref<Mesh>(geometry.base_resource)->get_aabb();
		}
		aabb = geometry.transform.xform(aabb);
		if (first) {
			bounds = aabb;
			first = false;
		} else {
			bounds.merge_with(aabb);
		}
	}
	if (first) {
		return false;
	}

	// Centered on the bounds, with the radius of the farthest vertex (tighter than the bounds for
	// most shapes, e.g. the corners of the bounds of a tree are empty).
	const Vector3 center = bounds.get_center();
	real_t radius_squared = 0.0;
	for (const Geometry &geometry : p_geometry) {
		if (geometry.multimesh) {
			const Ref<MultiMesh> multimesh = geometry.base_resource;
			const AABB mesh_aabb = multimesh->get_mesh()->get_aabb();
			for (int i = 0; i < multimesh->get_instance_count(); i++) {
				const Transform3D transform = geometry.transform * multimesh->get_instance_transform(i);
				for (int j = 0; j < 8; j++) {
					radius_squared = MAX(radius_squared, transform.xform(mesh_aabb.get_endpoint(j)).distance_squared_to(center));
				}
			}
			continue;
		}

		const Ref<Mesh> mesh = geometry.base_resource;
		bool has_vertices = false;
		for (int i = 0; i < mesh->get_surface_count(); i++) {
			const Array arrays = mesh->surface_get_arrays(i);
			if (arrays.size() != Mesh::ARRAY_MAX) {
				continue;
			}
			const PackedVector3Array vertices = arrays[Mesh::ARRAY_VERTEX];
			for (const Vector3 &vertex : vertices) {
				radius_squared = MAX(radius_squared, geometry.transform.xform(vertex).distance_squared_to(center));
			}
			has_vertices = has_vertices || !vertices.is_empty();
		}
		if (!has_vertices) {
			const AABB aabb = mesh->get_aabb();
			for (int j = 0; j < 8; j++) {
				radius_squared = MAX(radius_squared, geometry.transform.xform(aabb.get_endpoint(j)).distance_squared_to(center));
			}
		}
	}

	r_center = center;
	r_radius = Math::sqrt(radius_squared);
	return r_radius > CMP_EPSILON;
}

void OctahedralImpostorBaker::_estimate_surface_parameters(const LocalVector<Geometry> &p_geometry) {
	// Constant roughness and metallic of the impostor without ORM atlas: the mean of the materials.
	float roughness_sum = 0.0;
	float metallic_sum = 0.0;
	int count = 0;
	for (const Geometry &geometry : p_geometry) {
		for (const Ref<Material> &surface_material : geometry.surface_materials) {
			const Ref<BaseMaterial3D> material = geometry.material_override.is_valid() ? geometry.material_override : surface_material;
			if (material.is_valid()) {
				roughness_sum += material->get_roughness();
				metallic_sum += material->get_metallic();
				count++;
			}
		}
	}
	source_roughness = count > 0 ? roughness_sum / count : 1.0;
	source_metallic = count > 0 ? metallic_sum / count : 0.0;
}

/* Rendering */

Ref<ShaderMaterial> OctahedralImpostorBaker::_get_override_material(BakeState &p_state, const Ref<Material> &p_material) {
	if (p_material.is_valid()) {
		const Ref<ShaderMaterial> *cached = p_state.override_materials.getptr(p_material->get_instance_id());
		if (cached) {
			return *cached;
		}
	} else if (p_state.default_override_material.is_valid()) {
		return p_state.default_override_material;
	}

	int cull_mode = 0; // Back, front, disabled.
	Ref<ShaderMaterial> material;
	material.instantiate();

	const Ref<BaseMaterial3D> base_material = p_material;
	const Ref<ShaderMaterial> shader_material = p_material;
	if (base_material.is_valid()) {
		material->set_shader_parameter("albedo", base_material->get_albedo());
		material->set_shader_parameter("texture_albedo", base_material->get_texture(BaseMaterial3D::TEXTURE_ALBEDO));
		material->set_shader_parameter("use_vertex_color", base_material->get_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR));
		switch (base_material->get_transparency()) {
			case BaseMaterial3D::TRANSPARENCY_DISABLED:
				break;
			case BaseMaterial3D::TRANSPARENCY_ALPHA_SCISSOR:
				material->set_shader_parameter("alpha_scissor_threshold", base_material->get_alpha_scissor_threshold());
				break;
			default:
				material->set_shader_parameter("alpha_scissor_threshold", 0.5);
				break;
		}
		material->set_shader_parameter("uv1_scale", base_material->get_uv1_scale());
		material->set_shader_parameter("uv1_offset", base_material->get_uv1_offset());
		if (base_material->get_feature(BaseMaterial3D::FEATURE_NORMAL_MAPPING) && base_material->get_texture(BaseMaterial3D::TEXTURE_NORMAL).is_valid()) {
			material->set_shader_parameter("use_normal_map", true);
			material->set_shader_parameter("texture_normal", base_material->get_texture(BaseMaterial3D::TEXTURE_NORMAL));
			material->set_shader_parameter("normal_scale", base_material->get_normal_scale());
		}
		material->set_shader_parameter("roughness", base_material->get_roughness());
		material->set_shader_parameter("metallic", base_material->get_metallic());
		const Ref<Texture2D> orm_texture = base_material->get_texture(BaseMaterial3D::TEXTURE_ORM);
		if (orm_texture.is_valid()) {
			material->set_shader_parameter("texture_roughness", orm_texture);
			material->set_shader_parameter("roughness_channel", Vector4(0, 1, 0, 0));
			material->set_shader_parameter("texture_metallic", orm_texture);
			material->set_shader_parameter("metallic_channel", Vector4(0, 0, 1, 0));
			material->set_shader_parameter("texture_ambient_occlusion", orm_texture);
		} else {
			material->set_shader_parameter("texture_roughness", base_material->get_texture(BaseMaterial3D::TEXTURE_ROUGHNESS));
			material->set_shader_parameter("roughness_channel", _texture_channel_mask(base_material->get_roughness_texture_channel()));
			material->set_shader_parameter("texture_metallic", base_material->get_texture(BaseMaterial3D::TEXTURE_METALLIC));
			material->set_shader_parameter("metallic_channel", _texture_channel_mask(base_material->get_metallic_texture_channel()));
			if (base_material->get_feature(BaseMaterial3D::FEATURE_AMBIENT_OCCLUSION)) {
				material->set_shader_parameter("texture_ambient_occlusion", base_material->get_texture(BaseMaterial3D::TEXTURE_AMBIENT_OCCLUSION));
				material->set_shader_parameter("ao_channel", _texture_channel_mask(base_material->get_ao_texture_channel()));
			}
		}
		if (base_material->get_cull_mode() == BaseMaterial3D::CULL_FRONT) {
			cull_mode = 1;
		} else if (base_material->get_cull_mode() == BaseMaterial3D::CULL_DISABLED) {
			cull_mode = 2;
		}
	} else if (shader_material.is_valid() && shader_material->get_shader().is_valid()) {
		const Ref<Shader> shader = shader_material->get_shader();
		HashSet<StringName> uniforms;
		List<PropertyInfo> uniform_list;
		shader->get_shader_uniform_list(&uniform_list);
		for (const PropertyInfo &uniform : uniform_list) {
			uniforms.insert(uniform.name);
		}

		const Variant albedo = _find_shader_parameter(shader_material, uniforms, { "albedo", "albedo_color", "base_color", "color" }, Variant::COLOR);
		if (albedo.get_type() == Variant::COLOR) {
			material->set_shader_parameter("albedo", albedo);
		}
		material->set_shader_parameter("texture_albedo", _find_shader_parameter(shader_material, uniforms, { "texture_albedo", "albedo_texture", "texture_base_color", "base_color_texture", "texture_diffuse", "diffuse_texture", "main_texture" }, Variant::OBJECT));
		const Variant alpha_scissor = _find_shader_parameter(shader_material, uniforms, { "alpha_scissor_threshold", "alpha_scissor", "alpha_threshold", "alpha_cutoff", "alpha_clip" }, Variant::FLOAT);
		if (alpha_scissor.get_type() == Variant::FLOAT) {
			material->set_shader_parameter("alpha_scissor_threshold", alpha_scissor);
		}
		const Variant uv1_scale = _find_shader_parameter(shader_material, uniforms, { "uv1_scale", "uv_scale" }, Variant::VECTOR3);
		if (uv1_scale.get_type() == Variant::VECTOR3) {
			material->set_shader_parameter("uv1_scale", uv1_scale);
		}
		const Variant uv1_offset = _find_shader_parameter(shader_material, uniforms, { "uv1_offset", "uv_offset" }, Variant::VECTOR3);
		if (uv1_offset.get_type() == Variant::VECTOR3) {
			material->set_shader_parameter("uv1_offset", uv1_offset);
		}
		const Variant normal_texture = _find_shader_parameter(shader_material, uniforms, { "texture_normal", "normal_texture", "texture_normal_map", "normal_map", "normalmap" }, Variant::OBJECT);
		if (normal_texture.get_type() == Variant::OBJECT) {
			material->set_shader_parameter("use_normal_map", true);
			material->set_shader_parameter("texture_normal", normal_texture);
			const Variant normal_scale = _find_shader_parameter(shader_material, uniforms, { "normal_scale", "normal_strength", "normal_depth", "normal_map_depth" }, Variant::FLOAT);
			if (normal_scale.get_type() == Variant::FLOAT) {
				material->set_shader_parameter("normal_scale", normal_scale);
			}
		}
		const Variant roughness = _find_shader_parameter(shader_material, uniforms, { "roughness" }, Variant::FLOAT);
		if (roughness.get_type() == Variant::FLOAT) {
			material->set_shader_parameter("roughness", roughness);
		}
		const Variant metallic = _find_shader_parameter(shader_material, uniforms, { "metallic" }, Variant::FLOAT);
		if (metallic.get_type() == Variant::FLOAT) {
			material->set_shader_parameter("metallic", metallic);
		}
		const Variant orm_texture = _find_shader_parameter(shader_material, uniforms, { "texture_orm", "orm_texture" }, Variant::OBJECT);
		if (orm_texture.get_type() == Variant::OBJECT) {
			material->set_shader_parameter("texture_roughness", orm_texture);
			material->set_shader_parameter("roughness_channel", Vector4(0, 1, 0, 0));
			material->set_shader_parameter("texture_metallic", orm_texture);
			material->set_shader_parameter("metallic_channel", Vector4(0, 0, 1, 0));
			material->set_shader_parameter("texture_ambient_occlusion", orm_texture);
		} else {
			material->set_shader_parameter("texture_roughness", _find_shader_parameter(shader_material, uniforms, { "texture_roughness", "roughness_texture" }, Variant::OBJECT));
			material->set_shader_parameter("texture_metallic", _find_shader_parameter(shader_material, uniforms, { "texture_metallic", "metallic_texture" }, Variant::OBJECT));
			material->set_shader_parameter("texture_ambient_occlusion", _find_shader_parameter(shader_material, uniforms, { "texture_ambient_occlusion", "ao_texture", "texture_ao" }, Variant::OBJECT));
		}
		const String code = shader->get_code();
		if (code.contains("cull_disabled")) {
			cull_mode = 2;
		} else if (code.contains("cull_front")) {
			cull_mode = 1;
		}
	}

	if (p_state.override_shaders[cull_mode].is_null()) {
		static const char *cull_modes[3] = { "cull_back", "cull_front", "cull_disabled" };
		Ref<Shader> shader;
		shader.instantiate();
		shader->set_code(_get_override_shader_code(cull_modes[cull_mode]));
		p_state.override_shaders[cull_mode] = shader;
	}
	material->set_shader(p_state.override_shaders[cull_mode]);
	material->set_shader_parameter("depth_near", p_state.margin_world);
	material->set_shader_parameter("depth_range", 2.0 * sphere_radius);

	if (p_material.is_valid()) {
		p_state.override_materials.insert(p_material->get_instance_id(), material);
	} else {
		p_state.default_override_material = material;
	}
	return material;
}

void OctahedralImpostorBaker::_setup_instances(BakeState &p_state, const Rect2i &p_tiles) {
	RS *rs = RS::get_singleton();
	const real_t tile_world = 2.0 * sphere_radius;
	for (int y = p_tiles.position.y; y < p_tiles.get_end().y; y++) {
		for (int x = p_tiles.position.x; x < p_tiles.get_end().x; x++) {
			// The camera looks down -Z: the object is rotated so that the view of the frame faces it.
			const Basis to_view = p_state.frame_bases[y * frames + x].transposed();
			const Vector3 tile_center = Vector3((x + 0.5) * tile_world, -(y + 0.5) * tile_world, 0.0);
			const Transform3D frame_transform = Transform3D(to_view, tile_center - to_view.xform(sphere_center));

			for (uint32_t i = 0; i < p_state.geometry.size(); i++) {
				const Geometry &geometry = p_state.geometry[i];
				const RID instance = rs->instance_create2(geometry.base, p_state.scenario);
				rs->instance_set_transform(instance, frame_transform * geometry.transform);
				rs->instance_geometry_set_cast_shadows_setting(instance, RSE::SHADOW_CASTING_SETTING_OFF);
				for (int j = 0; j < geometry.blend_shape_weights.size(); j++) {
					rs->instance_set_blend_shape_weight(instance, j, geometry.blend_shape_weights[j]);
				}
				for (const Pair<StringName, Variant> &parameter : geometry.instance_parameters) {
					rs->instance_geometry_set_shader_parameter(instance, parameter.first, parameter.second);
				}
				p_state.instances.push_back(Pair<RID, int>(instance, i));
			}
		}
	}
}

void OctahedralImpostorBaker::_set_pass(BakeState &p_state, Pass p_pass) {
	RS *rs = RS::get_singleton();

	// The albedo is the unshaded color of the real materials, with their transparency.
	rs->viewport_set_debug_draw(p_state.viewport, p_pass == PASS_ALBEDO ? RSE::VIEWPORT_DEBUG_DRAW_UNSHADED : RSE::VIEWPORT_DEBUG_DRAW_DISABLED);

	if (p_state.capture_instance.is_valid()) {
		rs->instance_set_visible(p_state.capture_instance, p_pass == PASS_GEOMETRY);
	}
	const bool use_override = p_pass == PASS_ORM || (p_pass == PASS_GEOMETRY && !p_state.use_normal_buffer);

	for (const Pair<RID, int> &instance : p_state.instances) {
		Geometry &geometry = p_state.geometry[instance.second];
		if (use_override) {
			if (geometry.multimesh) {
				// Surfaces of multimeshes can't be overridden separately.
				const Ref<Material> material = geometry.material_override.is_valid() ? geometry.material_override : (geometry.surface_materials.is_empty() ? Ref<Material>() : geometry.surface_materials[0]);
				rs->instance_geometry_set_material_override(instance.first, _get_override_material(p_state, material)->get_rid());
			} else if (geometry.material_override.is_valid()) {
				rs->instance_geometry_set_material_override(instance.first, _get_override_material(p_state, geometry.material_override)->get_rid());
			} else {
				rs->instance_geometry_set_material_override(instance.first, RID());
				for (int i = 0; i < geometry.surface_materials.size(); i++) {
					rs->instance_set_surface_override_material(instance.first, i, _get_override_material(p_state, geometry.surface_materials[i])->get_rid());
				}
			}
			rs->instance_geometry_set_material_overlay(instance.first, RID());
		} else {
			for (int i = 0; i < geometry.surface_override_materials.size(); i++) {
				const Ref<Material> &material = geometry.surface_override_materials[i];
				rs->instance_set_surface_override_material(instance.first, i, material.is_valid() ? material->get_rid() : RID());
			}
			rs->instance_geometry_set_material_override(instance.first, geometry.material_override.is_valid() ? geometry.material_override->get_rid() : RID());
			rs->instance_geometry_set_material_overlay(instance.first, geometry.material_overlay.is_valid() ? geometry.material_overlay->get_rid() : RID());
		}
	}

	if (use_override) {
		const int bake_mode = p_pass == PASS_ORM ? 1 : 0;
		for (KeyValue<ObjectID, Ref<ShaderMaterial>> &E : p_state.override_materials) {
			E.value->set_shader_parameter("bake_mode", bake_mode);
		}
		if (p_state.default_override_material.is_valid()) {
			p_state.default_override_material->set_shader_parameter("bake_mode", bake_mode);
		}
	}
}

void OctahedralImpostorBaker::_free_instances(BakeState &p_state) {
	for (const Pair<RID, int> &instance : p_state.instances) {
		RS::get_singleton()->free_rid(instance.first);
	}
	p_state.instances.clear();
}

void OctahedralImpostorBaker::_draw(BakeState &p_state) {
	RS *rs = RS::get_singleton();
	// Draws only the viewport of the bake, not the editor (like the previews of the resources).
	RID root_viewport;
	SceneTree *tree = SceneTree::get_singleton();
	if (tree && tree->get_root()) {
		root_viewport = tree->get_root()->get_viewport_rid();
		rs->viewport_set_active(root_viewport, false);
	}
	rs->viewport_set_update_mode(p_state.viewport, RSE::VIEWPORT_UPDATE_ONCE);
	rs->draw(false);
	if (root_viewport.is_valid()) {
		rs->viewport_set_active(root_viewport, true);
	}
}

bool OctahedralImpostorBaker::_is_output_srgb(BakeState &p_state, Pass p_pass) {
	// The albedo pass renders the colors of lit materials in the unshaded debug mode: some renderers
	// (e.g. Compatibility) keep them in sRGB. The other passes write data with unshaded materials.
	const bool albedo = p_pass == PASS_ALBEDO;
	RS *rs = RS::get_singleton();
	Ref<Material> material;
	if (albedo) {
		Ref<StandardMaterial3D> standard_material;
		standard_material.instantiate();
		standard_material->set_albedo(Color(0.5, 0.5, 0.5)); // 0.214 in linear.
		standard_material->set_specular(0.0);
		standard_material->set_roughness(1.0);
		standard_material->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
		material = standard_material;
	} else {
		Ref<Shader> shader;
		shader.instantiate();
		shader->set_code(calibration_shader_code);
		Ref<ShaderMaterial> shader_material;
		shader_material.instantiate();
		shader_material->set_shader(shader);
		material = shader_material;
	}
	Ref<QuadMesh> quad;
	quad.instantiate();
	const RID instance = rs->instance_create2(quad->get_rid(), p_state.scenario);
	rs->instance_geometry_set_material_override(instance, material->get_rid());
	// Fills the view of the camera.
	rs->instance_set_transform(instance, Transform3D(Basis::from_scale(Vector3(4, 4, 1)), Vector3(0, 0, -1)));
	rs->instance_set_custom_aabb(instance, AABB(Vector3(-1, -1, -1) * 1e6, Vector3(2, 2, 2) * 1e6));

	rs->viewport_set_size(p_state.viewport, 4, 4);
	rs->viewport_set_debug_draw(p_state.viewport, albedo ? RSE::VIEWPORT_DEBUG_DRAW_UNSHADED : RSE::VIEWPORT_DEBUG_DRAW_DISABLED);
	rs->camera_set_transform(p_state.camera, Transform3D());
	rs->camera_set_orthogonal(p_state.camera, 1.0, 0.1, 10.0);
	_draw(p_state);
	const Ref<Image> image = rs->texture_2d_get(rs->viewport_get_texture(p_state.viewport));
	rs->free_rid(instance);
	ERR_FAIL_COND_V(image.is_null() || image->is_empty(), false);
	// About 0.2 when the output is linear, 0.5 when it's sRGB.
	const Color color = image->get_pixel(2, 2);
	print_verbose(vformat("Octahedral impostor baker: output of the renderer for a gray of 0.2 in linear (%s): %s.", albedo ? "albedo" : "data", color));
	return color.r > 0.35;
}

void OctahedralImpostorBaker::_read_region(BakeState &p_state, Pass p_pass, const Rect2i &p_tiles) {
	Ref<Image> image = RS::get_singleton()->texture_2d_get(RS::get_singleton()->viewport_get_texture(p_state.viewport));
	ERR_FAIL_COND(image.is_null() || image->is_empty());
	image->clear_mipmaps();
	image->convert(Image::FORMAT_RGBAF);
	if (p_state.srgb_output[p_pass == PASS_ALBEDO ? 0 : 1]) {
		float *pixels = reinterpret_cast<float *>(image->ptrw());
		const int64_t count = int64_t(image->get_width()) * image->get_height();
		for (int64_t i = 0; i < count; i++) {
			const Color linear = Color(pixels[i * 4], pixels[i * 4 + 1], pixels[i * 4 + 2]).srgb_to_linear();
			pixels[i * 4] = linear.r;
			pixels[i * 4 + 1] = linear.g;
			pixels[i * 4 + 2] = linear.b;
		}
	}
	if (p_pass == PASS_ALBEDO) {
		p_state.region_albedo = image;
	}
	ERR_FAIL_COND(p_state.region_albedo.is_null());

	const int ss = p_state.supersampling;
	const int tile = p_state.tile_size;
	const int ss_tile = p_state.ss_tile_size;
	const int atlas = tile * frames;
	const int width = image->get_width();
	ERR_FAIL_COND(width != p_tiles.size.x * ss_tile || image->get_height() != p_tiles.size.y * ss_tile);
	const float *data = reinterpret_cast<const float *>(image->ptr());
	const float *albedo = reinterpret_cast<const float *>(p_state.region_albedo->ptr());
	const float sample_weight = 1.0 / (ss * ss);

	uint8_t *output = nullptr;
	switch (p_pass) {
		case PASS_ALBEDO:
			output = albedo_image->ptrw();
			break;
		case PASS_GEOMETRY:
			output = normal_depth_image->ptrw();
			break;
		case PASS_ORM:
			output = orm_image->ptrw();
			break;
		default:
			ERR_FAIL();
	}
	uint8_t *coverage = p_state.coverage_mask.ptrw();
	uint8_t *geometry_mask = p_state.geometry_mask.ptrw();
	uint8_t *orm_mask = p_state.orm_mask.ptrw();

	for (int ty = p_tiles.position.y; ty < p_tiles.get_end().y; ty++) {
		for (int tx = p_tiles.position.x; tx < p_tiles.get_end().x; tx++) {
			const Basis &frame_basis = p_state.frame_bases[ty * frames + tx];
			const int origin_x = (tx - p_tiles.position.x) * ss_tile;
			const int origin_y = (ty - p_tiles.position.y) * ss_tile;

			for (int py = 0; py < tile; py++) {
				for (int px = 0; px < tile; px++) {
					float sum[4] = { 0, 0, 0, 0 };
					float weight = 0.0;
					Vector3 normal_sum;
					for (int sy = 0; sy < ss; sy++) {
						for (int sx = 0; sx < ss; sx++) {
							const int offset = ((origin_y + py * ss + sy) * width + origin_x + px * ss + sx) * 4;
							const float *pixel = data + offset;
							const float alpha = CLAMP(albedo[offset + 3], 0.0f, 1.0f);
							if (p_pass == PASS_ALBEDO) {
								// Premultiplied by the coverage (transparent background).
								for (int c = 0; c < 4; c++) {
									sum[c] += pixel[c];
								}
							} else if (p_pass == PASS_GEOMETRY) {
								// Skips the background of the pass (the edges of alpha tested surfaces may
								// differ a bit from the albedo pass), and the surfaces that don't write
								// their depth (e.g. alpha blended).
								if (alpha <= 0.0 || pixel[3] < 0.5 || pixel[2] >= 0.9999) {
									continue;
								}
								const Vector3 view_normal = OctahedralImpostorMaterial3D::octahedral_decode(Vector2(pixel[0], pixel[1]) * 2.0 - Vector2(1, 1), OctahedralImpostorMaterial3D::LAYOUT_FULL_SPHERE);
								normal_sum += frame_basis.xform(view_normal) * alpha;
								sum[3] += pixel[2] * alpha;
								weight += alpha;
							} else if (alpha > 0.0 && pixel[3] >= 0.5) {
								for (int c = 0; c < 3; c++) {
									sum[c] += pixel[c] * alpha;
								}
								weight += alpha;
							}
						}
					}

					const int index = (ty * tile + py) * atlas + tx * tile + px;
					uint8_t *out = output + index * 4;
					if (p_pass == PASS_ALBEDO) {
						const float alpha = CLAMP(sum[3] * sample_weight, 0.0f, 1.0f);
						Color color;
						if (sum[3] > 0.0) {
							color = Color(sum[0] / sum[3], sum[1] / sum[3], sum[2] / sum[3]).clamp().linear_to_srgb();
						}
						out[0] = uint8_t(Math::fast_ftoi(color.r * 255.0));
						out[1] = uint8_t(Math::fast_ftoi(color.g * 255.0));
						out[2] = uint8_t(Math::fast_ftoi(color.b * 255.0));
						out[3] = uint8_t(Math::fast_ftoi(alpha * 255.0));
						coverage[index] = out[3] > 0;
					} else if (p_pass == PASS_GEOMETRY) {
						if (weight > 0.0 && !normal_sum.is_zero_approx()) {
							const Vector3 normal = normal_sum.normalized() * 0.5 + Vector3(0.5, 0.5, 0.5);
							out[0] = uint8_t(Math::fast_ftoi(CLAMP(normal.x, 0.0f, 1.0f) * 255.0));
							out[1] = uint8_t(Math::fast_ftoi(CLAMP(normal.y, 0.0f, 1.0f) * 255.0));
							out[2] = uint8_t(Math::fast_ftoi(CLAMP(normal.z, 0.0f, 1.0f) * 255.0));
							out[3] = uint8_t(Math::fast_ftoi(CLAMP(sum[3] / weight, 0.0f, 1.0f) * 255.0));
							geometry_mask[index] = 1;
						}
					} else if (weight > 0.0) {
						for (int c = 0; c < 3; c++) {
							out[c] = uint8_t(Math::fast_ftoi(CLAMP(sum[c] / weight, 0.0f, 1.0f) * 255.0));
						}
						out[3] = 255;
						orm_mask[index] = 1;
					}
				}
			}
		}
	}
}

void OctahedralImpostorBaker::_dilate(Ref<Image> &p_image, const Vector<uint8_t> &p_mask, int p_frames, int p_tile_size, bool p_keep_alpha) {
	// Fills the empty pixels of each frame with the mean of their filled neighbors, ring by ring,
	// so that filtering and mipmaps don't bring the background color in, without crossing frames.
	const int size = p_image->get_width();
	uint8_t *data = p_image->ptrw();
	Vector<uint8_t> valid = p_mask;
	uint8_t *valid_ptr = valid.ptrw();
	Vector<uint8_t> queued;
	queued.resize_initialized(size * size);
	uint8_t *queued_ptr = queued.ptrw();
	LocalVector<int> ring;
	LocalVector<int> next_ring;
	LocalVector<uint8_t> ring_colors;
	const int channels = p_keep_alpha ? 3 : 4;

	for (int tile_y = 0; tile_y < p_frames; tile_y++) {
		for (int tile_x = 0; tile_x < p_frames; tile_x++) {
			const int x0 = tile_x * p_tile_size;
			const int y0 = tile_y * p_tile_size;
			const int x1 = x0 + p_tile_size;
			const int y1 = y0 + p_tile_size;

			ring.clear();
			for (int y = y0; y < y1; y++) {
				for (int x = x0; x < x1; x++) {
					const int index = y * size + x;
					if (valid_ptr[index]) {
						continue;
					}
					bool border = false;
					for (int dy = -1; dy <= 1 && !border; dy++) {
						for (int dx = -1; dx <= 1; dx++) {
							const int nx = x + dx;
							const int ny = y + dy;
							if (nx >= x0 && nx < x1 && ny >= y0 && ny < y1 && valid_ptr[ny * size + nx]) {
								border = true;
								break;
							}
						}
					}
					if (border) {
						ring.push_back(index);
						queued_ptr[index] = 1;
					}
				}
			}

			while (!ring.is_empty()) {
				ring_colors.resize(ring.size() * 4);
				for (uint32_t i = 0; i < ring.size(); i++) {
					const int x = ring[i] % size;
					const int y = ring[i] / size;
					int sum[4] = { 0, 0, 0, 0 };
					int count = 0;
					for (int dy = -1; dy <= 1; dy++) {
						for (int dx = -1; dx <= 1; dx++) {
							const int nx = x + dx;
							const int ny = y + dy;
							if (nx < x0 || nx >= x1 || ny < y0 || ny >= y1 || !valid_ptr[ny * size + nx]) {
								continue;
							}
							const uint8_t *neighbor = data + (ny * size + nx) * 4;
							for (int c = 0; c < 4; c++) {
								sum[c] += neighbor[c];
							}
							count++;
						}
					}
					for (int c = 0; c < 4; c++) {
						ring_colors[i * 4 + c] = count > 0 ? uint8_t((sum[c] + count / 2) / count) : 0;
					}
				}

				next_ring.clear();
				for (uint32_t i = 0; i < ring.size(); i++) {
					uint8_t *pixel = data + ring[i] * 4;
					for (int c = 0; c < channels; c++) {
						pixel[c] = ring_colors[i * 4 + c];
					}
					valid_ptr[ring[i]] = 1;
				}
				for (uint32_t i = 0; i < ring.size(); i++) {
					const int x = ring[i] % size;
					const int y = ring[i] / size;
					for (int dy = -1; dy <= 1; dy++) {
						for (int dx = -1; dx <= 1; dx++) {
							const int nx = x + dx;
							const int ny = y + dy;
							if (nx < x0 || nx >= x1 || ny < y0 || ny >= y1) {
								continue;
							}
							const int index = ny * size + nx;
							if (!valid_ptr[index] && !queued_ptr[index]) {
								queued_ptr[index] = 1;
								next_ring.push_back(index);
							}
						}
					}
				}
				SWAP(ring, next_ring);
			}
		}
	}
}

void OctahedralImpostorBaker::_cleanup(BakeState &p_state) {
	RS *rs = RS::get_singleton();
	_free_instances(p_state);
	if (p_state.capture_instance.is_valid()) {
		rs->free_rid(p_state.capture_instance);
	}
	if (p_state.camera.is_valid()) {
		rs->free_rid(p_state.camera);
	}
	if (p_state.viewport.is_valid()) {
		rs->free_rid(p_state.viewport);
	}
	if (p_state.scenario.is_valid()) {
		rs->free_rid(p_state.scenario);
	}
	p_state = BakeState();
}

Error OctahedralImpostorBaker::bake(Node3D *p_node, bool p_show_progress) {
	ERR_FAIL_NULL_V(p_node, ERR_INVALID_PARAMETER);
	RS *rs = RS::get_singleton();
	const String rendering_method = rs->get_current_rendering_method();
	ERR_FAIL_COND_V_MSG(rendering_method.is_empty() || rendering_method == "dummy", ERR_UNAVAILABLE, "Baking an octahedral impostor requires a renderer.");

	BakeState state;
	state.geometry = collect_geometry(p_node);
	ERR_FAIL_COND_V_MSG(state.geometry.is_empty(), ERR_INVALID_DATA, vformat("Node '%s' has no visible geometry to bake an octahedral impostor from.", p_node->get_name()));

	Vector3 center;
	float radius = 0.0;
	ERR_FAIL_COND_V_MSG(!compute_bounding_sphere(state.geometry, center, radius), ERR_INVALID_DATA, "The geometry to bake an octahedral impostor from is empty.");

	state.tile_size = get_frame_size();
	ERR_FAIL_COND_V_MSG(state.tile_size < 4 * FRAME_PADDING, ERR_INVALID_PARAMETER, vformat("The frames of the octahedral impostor are too small (%d pixels): increase the atlas size or reduce the number of frames.", state.tile_size));
	// No more supersampling than a large viewport allows for a single frame.
	state.supersampling = supersampling;
	while (state.supersampling > 1 && state.tile_size * state.supersampling > MAX_REGION_SIZE * 2) {
		state.supersampling--;
	}
	state.ss_tile_size = state.tile_size * state.supersampling;
	const int atlas = state.tile_size * frames;

	// Grown so that the frames keep an empty border.
	sphere_center = center;
	sphere_radius = radius * float(state.tile_size) / float(state.tile_size - 2 * FRAME_PADDING);
	state.margin_world = sphere_radius * 0.1;
	_estimate_surface_parameters(state.geometry);

	state.frame_bases.resize(frames * frames);
	for (int y = 0; y < frames; y++) {
		for (int x = 0; x < frames; x++) {
			state.frame_bases[y * frames + x] = OctahedralImpostorMaterial3D::get_frame_basis(OctahedralImpostorMaterial3D::get_frame_direction(Vector2i(x, y), frames, layout));
		}
	}

	albedo_image = Image::create_empty(atlas, atlas, false, Image::FORMAT_RGBA8);
	normal_depth_image = Image::create_empty(atlas, atlas, false, Image::FORMAT_RGBA8);
	orm_image = bake_orm ? Image::create_empty(atlas, atlas, false, Image::FORMAT_RGBA8) : Ref<Image>();
	state.coverage_mask.resize_initialized(atlas * atlas);
	state.geometry_mask.resize_initialized(atlas * atlas);
	if (bake_orm) {
		state.orm_mask.resize_initialized(atlas * atlas);
	}

	// Rendering setup: a scenario of its own, without environment nor lights, rendered to an HDR
	// viewport so that the data isn't quantized (the encoding of the output is calibrated below).
	state.use_normal_buffer = rendering_method == "forward_plus";
	state.scenario = rs->scenario_create();
	state.viewport = rs->viewport_create();
	rs->viewport_set_update_mode(state.viewport, RSE::VIEWPORT_UPDATE_DISABLED);
	rs->viewport_set_scenario(state.viewport, state.scenario);
	rs->viewport_set_transparent_background(state.viewport, true);
	rs->viewport_set_use_hdr_2d(state.viewport, true);
	rs->viewport_set_disable_2d(state.viewport, true);
	rs->viewport_set_msaa_3d(state.viewport, RSE::VIEWPORT_MSAA_DISABLED);
	rs->viewport_set_screen_space_aa(state.viewport, RSE::VIEWPORT_SCREEN_SPACE_AA_DISABLED);
	rs->viewport_set_use_debanding(state.viewport, false);
	rs->viewport_set_scaling_3d_mode(state.viewport, RSE::VIEWPORT_SCALING_3D_MODE_BILINEAR);
	rs->viewport_set_scaling_3d_scale(state.viewport, 1.0);
	rs->viewport_set_mesh_lod_threshold(state.viewport, 0.0);
	rs->viewport_set_use_occlusion_culling(state.viewport, false);
	// Each draw shows other frames: the depth of the previous one mustn't cull anything.
	rs->viewport_set_use_hzb_occlusion_culling(state.viewport, false);
	rs->viewport_set_active(state.viewport, true);
	state.camera = rs->camera_create();
	rs->viewport_attach_camera(state.viewport, state.camera);

	if (state.use_normal_buffer) {
		Ref<QuadMesh> quad;
		quad.instantiate();
		state.capture_mesh = quad;
		Ref<Shader> capture_shader;
		capture_shader.instantiate();
		capture_shader->set_code(capture_shader_code);
		state.capture_material.instantiate();
		state.capture_material->set_shader(capture_shader);
		state.capture_material->set_render_priority(Material::RENDER_PRIORITY_MAX);
		state.capture_material->set_shader_parameter("depth_near", state.margin_world);
		state.capture_material->set_shader_parameter("depth_range", 2.0 * sphere_radius);
		state.capture_instance = rs->instance_create2(quad->get_rid(), state.scenario);
		rs->instance_geometry_set_material_override(state.capture_instance, state.capture_material->get_rid());
		rs->instance_geometry_set_cast_shadows_setting(state.capture_instance, RSE::SHADOW_CASTING_SETTING_OFF);
		rs->instance_set_custom_aabb(state.capture_instance, AABB(Vector3(-1, -1, -1) * 1e6, Vector3(2, 2, 2) * 1e6));
		rs->instance_set_visible(state.capture_instance, false);
	}

	// Some renderers output sRGB values (without HDR 2D, or when they tonemap in the scene shaders).
	state.srgb_output[0] = _is_output_srgb(state, PASS_ALBEDO);
	state.srgb_output[1] = _is_output_srgb(state, PASS_GEOMETRY);

	LocalVector<Pass> passes;
	passes.push_back(PASS_ALBEDO);
	passes.push_back(PASS_GEOMETRY);
	if (bake_orm) {
		passes.push_back(PASS_ORM);
	}

	// All the frames of a region are rendered at once, the object's frames side by side in a grid,
	// seen by an orthographic camera looking down -Z.
	const int tiles_per_region = MAX(1, MAX_REGION_SIZE / state.ss_tile_size);
	const int regions_per_side = (frames + tiles_per_region - 1) / tiles_per_region;
	const real_t tile_world = 2.0 * sphere_radius;
	const int steps = regions_per_side * regions_per_side * passes.size();

	EditorProgress *progress = nullptr;
	if (p_show_progress && EditorNode::get_singleton()) {
		progress = memnew(EditorProgress("bake_octahedral_impostor", TTR("Bake Octahedral Impostor"), steps + 1, true));
	}

	Error err = OK;
	int step = 0;
	for (int region_y = 0; region_y < regions_per_side && err == OK; region_y++) {
		for (int region_x = 0; region_x < regions_per_side && err == OK; region_x++) {
			const Rect2i tiles = Rect2i(region_x * tiles_per_region, region_y * tiles_per_region, MIN(tiles_per_region, frames - region_x * tiles_per_region), MIN(tiles_per_region, frames - region_y * tiles_per_region));
			rs->viewport_set_size(state.viewport, tiles.size.x * state.ss_tile_size, tiles.size.y * state.ss_tile_size);
			const Vector3 camera_position = Vector3((tiles.position.x + tiles.size.x * 0.5) * tile_world, -(tiles.position.y + tiles.size.y * 0.5) * tile_world, sphere_radius + state.margin_world);
			rs->camera_set_transform(state.camera, Transform3D(Basis(), camera_position));
			rs->camera_set_orthogonal(state.camera, tiles.size.y * tile_world, state.margin_world * 0.5, 2.0 * sphere_radius + state.margin_world * 2.0);

			_setup_instances(state, tiles);
			for (Pass pass : passes) {
				if (progress && progress->step(vformat(TTR("Rendering frames (%d/%d)..."), step + 1, steps), step, false)) {
					err = ERR_SKIP;
					break;
				}
				_set_pass(state, pass);
				_draw(state);
				_read_region(state, pass, tiles);
				step++;
			}
			_free_instances(state);
		}
	}
	const Vector<uint8_t> coverage_mask = state.coverage_mask;
	const Vector<uint8_t> geometry_mask = state.geometry_mask;
	const Vector<uint8_t> orm_mask = state.orm_mask;
	const int tile_size = state.tile_size;
	_cleanup(state);

	if (err == OK) {
		if (progress) {
			progress->step(TTR("Filling the borders of the frames..."), steps, false);
		}
		_dilate(albedo_image, coverage_mask, frames, tile_size, true);
		// Also fills the covered pixels without geometry data (e.g. alpha blended surfaces).
		_dilate(normal_depth_image, geometry_mask, frames, tile_size, false);
		if (orm_image.is_valid()) {
			_dilate(orm_image, orm_mask, frames, tile_size, false);
			orm_image->convert(Image::FORMAT_RGB8);
		}
		baked_frames = frames;
		baked_layout = layout;
	} else {
		albedo_image.unref();
		normal_depth_image.unref();
		orm_image.unref();
	}
	if (progress) {
		memdelete(progress);
	}
	return err;
}

/* Results */

void OctahedralImpostorBaker::apply_to_material(const Ref<OctahedralImpostorMaterial3D> &p_material) const {
	ERR_FAIL_COND(p_material.is_null());
	ERR_FAIL_COND_MSG(baked_frames == 0, "Nothing was baked.");
	p_material->set_layout(baked_layout);
	p_material->set_frames(baked_frames);
	p_material->set_sphere_center(sphere_center);
	p_material->set_sphere_radius(sphere_radius);
	if (orm_image.is_valid()) {
		// Multipliers of the ORM atlas.
		p_material->set_roughness(1.0);
		p_material->set_metallic(1.0);
	} else {
		p_material->set_roughness(source_roughness);
		p_material->set_metallic(source_metallic);
	}
}

Ref<OctahedralImpostorMaterial3D> OctahedralImpostorBaker::create_material() const {
	ERR_FAIL_COND_V_MSG(albedo_image.is_null(), Ref<OctahedralImpostorMaterial3D>(), "Nothing was baked.");
	Ref<OctahedralImpostorMaterial3D> material;
	material.instantiate();
	apply_to_material(material);

	const Ref<Image> images[OctahedralImpostorMaterial3D::TEXTURE_MAX] = { albedo_image, normal_depth_image, orm_image };
	for (int i = 0; i < OctahedralImpostorMaterial3D::TEXTURE_MAX; i++) {
		if (images[i].is_null()) {
			continue;
		}
		Ref<Image> image = images[i]->duplicate();
		image->generate_mipmaps();
		material->set_texture(OctahedralImpostorMaterial3D::TextureParam(i), ImageTexture::create_from_image(image));
	}
	return material;
}

static void _write_texture_import_settings(const String &p_path, bool p_alpha_test) {
	const String config_path = p_path + ".import";
	Ref<ConfigFile> config;
	config.instantiate();
	// Keeps the settings changed by the user after a previous bake.
	if (FileAccess::exists(config_path)) {
		config->load(config_path);
	}
	config->set_value("remap", "importer", "texture");
	config->set_value("remap", "type", "CompressedTexture2D");
	if (!config->has_section_key("params", "compress/mode")) {
		config->set_value("params", "compress/mode", 2); // VRAM Compressed.
		config->set_value("params", "compress/high_quality", true);
	}
	// The normals are in object space, not a normal map.
	config->set_value("params", "compress/normal_map", 2);
	config->set_value("params", "roughness/mode", 1);
	config->set_value("params", "mipmaps/generate", true);
	if (p_alpha_test && !config->has_section_key("params", "mipmaps/preserve_alpha_test_coverage")) {
		config->set_value("params", "mipmaps/preserve_alpha_test_coverage", true);
	}
	// The baker fills the borders of the frames.
	config->set_value("params", "process/fix_alpha_border", false);
	config->set_value("params", "detect_3d/compress_to", 0);
	config->save(config_path);
}

Ref<Mesh> OctahedralImpostorBaker::save(const String &p_path) {
	ERR_FAIL_COND_V_MSG(albedo_image.is_null(), Ref<Mesh>(), "Nothing was baked.");
	ERR_FAIL_COND_V(p_path.is_empty(), Ref<Mesh>());
	EditorFileSystem *file_system = EditorFileSystem::get_singleton();
	ERR_FAIL_COND_V_MSG(file_system && file_system->is_importing(), Ref<Mesh>(), "Can't save an octahedral impostor while the editor is importing files, try again when the import is done.");

	const String base_path = p_path.get_basename();
	const Ref<Image> images[OctahedralImpostorMaterial3D::TEXTURE_MAX] = { albedo_image, normal_depth_image, orm_image };
	static const char *suffixes[OctahedralImpostorMaterial3D::TEXTURE_MAX] = { "_albedo.png", "_normal_depth.png", "_orm.png" };
	Vector<String> texture_paths;
	for (int i = 0; i < OctahedralImpostorMaterial3D::TEXTURE_MAX; i++) {
		if (images[i].is_null()) {
			continue;
		}
		const String path = base_path + suffixes[i];
		_write_texture_import_settings(path, i == OctahedralImpostorMaterial3D::TEXTURE_ALBEDO);
		const Error err = images[i]->save_png(path);
		ERR_FAIL_COND_V_MSG(err != OK, Ref<Mesh>(), vformat("Can't save the octahedral impostor atlas to '%s'.", path));
		texture_paths.push_back(path);
	}

	if (file_system) {
		for (const String &path : texture_paths) {
			file_system->update_file(path);
		}
		file_system->reimport_files(texture_paths);
	}

	// A rebake updates the existing mesh in place: the scenes that use it follow, and the settings
	// of its material that aren't baked (e.g. transparency, backlight) are kept.
	Ref<PrimitiveMesh> mesh;
	if (ResourceLoader::exists(p_path)) {
		mesh = ResourceLoader::load(p_path);
	}
	if (mesh.is_null()) {
		Ref<QuadMesh> quad;
		quad.instantiate();
		mesh = quad;
	}
	Ref<OctahedralImpostorMaterial3D> material = mesh->get_material();
	if (material.is_null()) {
		material.instantiate();
	}
	apply_to_material(material);
	for (int i = 0; i < OctahedralImpostorMaterial3D::TEXTURE_MAX; i++) {
		Ref<Texture2D> texture;
		if (images[i].is_valid()) {
			texture = ResourceLoader::load(base_path + suffixes[i], "Texture2D");
			ERR_FAIL_COND_V_MSG(texture.is_null(), Ref<Mesh>(), vformat("Can't load the octahedral impostor atlas '%s'.", base_path + suffixes[i]));
		}
		material->set_texture(OctahedralImpostorMaterial3D::TextureParam(i), texture);
	}
	mesh->set_material(material);
	// The quad turns toward the camera: culled with the bounds of the bounding sphere.
	mesh->set_custom_aabb(material->get_bounds());

	const Error err = ResourceSaver::save(mesh, p_path);
	ERR_FAIL_COND_V_MSG(err != OK, Ref<Mesh>(), vformat("Can't save the octahedral impostor mesh to '%s'.", p_path));
	if (mesh->get_path() != p_path) {
		mesh->set_path(p_path, true);
	}
	if (file_system) {
		file_system->update_file(p_path);
	}
	return mesh;
}

Error OctahedralImpostorBaker::_bake_bind(Node *p_node, bool p_show_progress) {
	Node3D *node = Object::cast_to<Node3D>(p_node);
	ERR_FAIL_NULL_V_MSG(node, ERR_INVALID_PARAMETER, "The node to bake an octahedral impostor from must be a Node3D.");
	return bake(node, p_show_progress);
}

void OctahedralImpostorBaker::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_layout", "layout"), &OctahedralImpostorBaker::set_layout);
	ClassDB::bind_method(D_METHOD("get_layout"), &OctahedralImpostorBaker::get_layout);
	ClassDB::bind_method(D_METHOD("set_frames", "frames"), &OctahedralImpostorBaker::set_frames);
	ClassDB::bind_method(D_METHOD("get_frames"), &OctahedralImpostorBaker::get_frames);
	ClassDB::bind_method(D_METHOD("set_atlas_size", "size"), &OctahedralImpostorBaker::set_atlas_size);
	ClassDB::bind_method(D_METHOD("get_atlas_size"), &OctahedralImpostorBaker::get_atlas_size);
	ClassDB::bind_method(D_METHOD("set_supersampling", "factor"), &OctahedralImpostorBaker::set_supersampling);
	ClassDB::bind_method(D_METHOD("get_supersampling"), &OctahedralImpostorBaker::get_supersampling);
	ClassDB::bind_method(D_METHOD("set_bake_orm", "enable"), &OctahedralImpostorBaker::set_bake_orm);
	ClassDB::bind_method(D_METHOD("is_baking_orm"), &OctahedralImpostorBaker::is_baking_orm);
	ClassDB::bind_method(D_METHOD("get_frame_size"), &OctahedralImpostorBaker::get_frame_size);

	ClassDB::bind_method(D_METHOD("bake", "node", "show_progress"), &OctahedralImpostorBaker::_bake_bind, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("get_albedo_image"), &OctahedralImpostorBaker::get_albedo_image);
	ClassDB::bind_method(D_METHOD("get_normal_depth_image"), &OctahedralImpostorBaker::get_normal_depth_image);
	ClassDB::bind_method(D_METHOD("get_orm_image"), &OctahedralImpostorBaker::get_orm_image);
	ClassDB::bind_method(D_METHOD("get_sphere_center"), &OctahedralImpostorBaker::get_sphere_center);
	ClassDB::bind_method(D_METHOD("get_sphere_radius"), &OctahedralImpostorBaker::get_sphere_radius);
	ClassDB::bind_method(D_METHOD("create_material"), &OctahedralImpostorBaker::create_material);
	ClassDB::bind_method(D_METHOD("save", "path"), &OctahedralImpostorBaker::save);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "layout", PROPERTY_HINT_ENUM, "Hemisphere,Full Sphere"), "set_layout", "get_layout");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "frames", PROPERTY_HINT_RANGE, itos(OctahedralImpostorMaterial3D::MIN_FRAMES) + "," + itos(OctahedralImpostorMaterial3D::MAX_FRAMES) + ",1"), "set_frames", "get_frames");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "atlas_size", PROPERTY_HINT_RANGE, itos(MIN_ATLAS_SIZE) + "," + itos(MAX_ATLAS_SIZE) + ",1,suffix:px"), "set_atlas_size", "get_atlas_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "supersampling", PROPERTY_HINT_RANGE, "1," + itos(MAX_SUPERSAMPLING) + ",1"), "set_supersampling", "get_supersampling");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "bake_orm"), "set_bake_orm", "is_baking_orm");
}

OctahedralImpostorBaker::OctahedralImpostorBaker() {
}
