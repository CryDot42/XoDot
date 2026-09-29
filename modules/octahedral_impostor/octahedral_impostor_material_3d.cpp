/**************************************************************************/
/*  octahedral_impostor_material_3d.cpp                                   */
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

#include "octahedral_impostor_material_3d.h"

#include "core/object/class_db.h"
#include "core/version.h"
#include "servers/rendering/rendering_server.h"

Mutex OctahedralImpostorMaterial3D::shader_mutex;
HashMap<uint32_t, OctahedralImpostorMaterial3D::ShaderData> OctahedralImpostorMaterial3D::shader_map;

/* Octahedral mapping */

static _FORCE_INLINE_ real_t _sign_not_zero(real_t p_value) {
	return p_value >= 0.0 ? 1.0 : -1.0;
}

Vector2 OctahedralImpostorMaterial3D::octahedral_encode(const Vector3 &p_direction, Layout p_layout) {
	Vector3 dir = p_direction;
	if (p_layout == LAYOUT_HEMISPHERE) {
		// Views from below are clamped to the horizon.
		dir.y = MAX(dir.y, (real_t)0.001);
	}
	const real_t sum = Math::abs(dir.x) + Math::abs(dir.y) + Math::abs(dir.z);
	ERR_FAIL_COND_V(sum <= (real_t)CMP_EPSILON, Vector2());
	dir /= sum;
	if (p_layout == LAYOUT_HEMISPHERE) {
		// The upper half of the octahedron, rotated by 45 degrees to fill the square.
		return Vector2(dir.x + dir.z, dir.z - dir.x);
	}
	if (dir.y < 0.0) {
		return Vector2((1.0 - Math::abs(dir.z)) * _sign_not_zero(dir.x), (1.0 - Math::abs(dir.x)) * _sign_not_zero(dir.z));
	}
	return Vector2(dir.x, dir.z);
}

Vector3 OctahedralImpostorMaterial3D::octahedral_decode(const Vector2 &p_coord, Layout p_layout) {
	Vector3 dir;
	if (p_layout == LAYOUT_HEMISPHERE) {
		dir.x = (p_coord.x - p_coord.y) * 0.5;
		dir.z = (p_coord.x + p_coord.y) * 0.5;
		dir.y = 1.0 - Math::abs(dir.x) - Math::abs(dir.z);
	} else {
		dir = Vector3(p_coord.x, 1.0 - Math::abs(p_coord.x) - Math::abs(p_coord.y), p_coord.y);
		if (dir.y < 0.0) {
			const real_t x = dir.x;
			dir.x = (1.0 - Math::abs(dir.z)) * _sign_not_zero(x);
			dir.z = (1.0 - Math::abs(x)) * _sign_not_zero(dir.z);
		}
	}
	return dir.normalized();
}

Vector3 OctahedralImpostorMaterial3D::get_frame_direction(const Vector2i &p_frame, int p_frames, Layout p_layout) {
	ERR_FAIL_COND_V(p_frames < MIN_FRAMES, Vector3(0, 1, 0));
	const Vector2 coord = Vector2(p_frame) / real_t(p_frames - 1) * 2.0 - Vector2(1, 1);
	return octahedral_decode(coord, p_layout);
}

Basis OctahedralImpostorMaterial3D::get_frame_basis(const Vector3 &p_direction) {
	// Must match impostor_basis() in the shader.
	const Vector3 up = Math::abs(p_direction.y) > 0.999 ? Vector3(0, 0, -1) : Vector3(0, 1, 0);
	const Vector3 x = up.cross(p_direction).normalized();
	const Vector3 y = p_direction.cross(x);
	return Basis(x, y, p_direction); // Columns.
}

/* Shader */

OctahedralImpostorMaterial3D::ShaderKey OctahedralImpostorMaterial3D::_compute_key() const {
	ShaderKey key;
	key.full_sphere = layout == LAYOUT_FULL_SPHERE;
	key.blend_frames = blend_frames;
	key.alpha_hash = transparency == TRANSPARENCY_ALPHA_HASH;
	key.alpha_antialiasing = alpha_antialiasing_mode;
	key.parallax = parallax_scale > 0.0;
	key.depth_offset = depth_offset_enabled;
	key.orm = textures[TEXTURE_ORM].is_valid();
	key.vertex_color = vertex_color_use_as_albedo;
	key.backlight = backlight_enabled;
	return key;
}

String OctahedralImpostorMaterial3D::_generate_shader_code(ShaderKey p_key) {
	const bool blend = p_key.blend_frames;
	const bool parallax = p_key.parallax;
	const bool orm = p_key.orm;

	String code = vformat(
			"// NOTE: Shader automatically converted from " GODOT_VERSION_NAME " " GODOT_VERSION_FULL_CONFIG "'s %s.\n\n",
			OctahedralImpostorMaterial3D::get_class_static());

	code += "shader_type spatial;\n";
	code += "render_mode blend_mix, depth_draw_opaque, cull_back, diffuse_burley, specular_schlick_ggx";
	if (p_key.alpha_antialiasing == BaseMaterial3D::ALPHA_ANTIALIASING_ALPHA_TO_COVERAGE) {
		code += ", alpha_to_coverage";
	} else if (p_key.alpha_antialiasing == BaseMaterial3D::ALPHA_ANTIALIASING_ALPHA_TO_COVERAGE_AND_TO_ONE) {
		code += ", alpha_to_coverage_and_one";
	}
	code += ";\n\n";

	code += R"(// Views of the object, frames x frames of them (see OctahedralImpostorBaker).
uniform sampler2D texture_albedo : source_color, hint_default_white, filter_linear_mipmap, repeat_disable;
// Object space normal in RGB, depth in A (0 at the front of the bounding sphere, 1 at its back).
uniform sampler2D texture_normal_depth : hint_default_black, filter_linear_mipmap, repeat_disable;
)";
	if (orm) {
		code += "uniform sampler2D texture_orm : hint_default_white, filter_linear_mipmap, repeat_disable;\n";
	}
	code += R"(uniform int frames = 12;
uniform vec3 sphere_center = vec3(0.0);
uniform float sphere_radius = 1.0;

uniform vec4 albedo : source_color = vec4(1.0);
)";
	if (parallax) {
		code += "uniform float parallax_scale : hint_range(0.0, 1.0) = 1.0;\n";
	}
	code += R"(uniform float metallic : hint_range(0.0, 1.0) = 0.0;
uniform float specular : hint_range(0.0, 1.0, 0.01) = 0.5;
uniform float roughness : hint_range(0.0, 1.0) = 1.0;
)";
	if (orm) {
		code += "uniform float ao_light_affect : hint_range(0.0, 1.0, 0.01) = 0.0;\n";
	}
	if (p_key.alpha_hash) {
		code += "uniform float alpha_hash_scale : hint_range(0.0, 2.0, 0.01) = 1.0;\n";
	} else {
		code += "uniform float alpha_scissor_threshold : hint_range(0.0, 1.0, 0.001) = 0.5;\n";
	}
	if (p_key.alpha_antialiasing != BaseMaterial3D::ALPHA_ANTIALIASING_OFF) {
		code += "uniform float alpha_antialiasing_edge : hint_range(0.0, 1.0, 0.01) = 0.3;\n";
	}
	if (p_key.backlight) {
		code += "uniform vec4 backlight : source_color = vec4(0.0, 0.0, 0.0, 1.0);\n";
	}

	code += R"(
// Frames blended for the current view, and the coordinates (0-1 across the frame) of the view ray
// through the vertex on the plane of each frame.
varying flat vec2 frame_a;
varying vec2 frame_uv_a;
)";
	if (blend) {
		code += R"(varying flat vec2 frame_b;
varying vec2 frame_uv_b;
varying flat vec2 frame_c;
varying vec2 frame_uv_c;
varying vec3 frame_weights;
)";
	}
	if (parallax) {
		code += "// View ray in the space of each frame (X right and Y down in the frame, Z toward its viewer).\n";
		code += "varying vec3 frame_ray_a;\n";
		if (blend) {
			code += "varying vec3 frame_ray_b;\nvarying vec3 frame_ray_c;\n";
		}
	}
	code += "varying mat3 object_to_view_normal;\n";
	if (p_key.depth_offset) {
		code += "varying float sphere_radius_view;\n";
	}

	// Octahedral mapping, must match OctahedralImpostorMaterial3D::octahedral_encode/decode().
	if (p_key.full_sphere) {
		code += R"(
vec2 impostor_encode(vec3 dir) {
	dir /= abs(dir.x) + abs(dir.y) + abs(dir.z);
	vec2 coord = dir.xz;
	if (dir.y < 0.0) {
		coord = (1.0 - abs(dir.zx)) * vec2(dir.x >= 0.0 ? 1.0 : -1.0, dir.z >= 0.0 ? 1.0 : -1.0);
	}
	return coord;
}

vec3 impostor_decode(vec2 coord) {
	vec3 dir = vec3(coord.x, 1.0 - abs(coord.x) - abs(coord.y), coord.y);
	if (dir.y < 0.0) {
		dir.xz = (1.0 - abs(dir.zx)) * vec2(dir.x >= 0.0 ? 1.0 : -1.0, dir.z >= 0.0 ? 1.0 : -1.0);
	}
	return normalize(dir);
}
)";
	} else {
		code += R"(
vec2 impostor_encode(vec3 dir) {
	// Views from below are clamped to the horizon.
	dir.y = max(dir.y, 0.001);
	dir /= abs(dir.x) + abs(dir.y) + abs(dir.z);
	return vec2(dir.x + dir.z, dir.z - dir.x);
}

vec3 impostor_decode(vec2 coord) {
	vec3 dir;
	dir.x = (coord.x - coord.y) * 0.5;
	dir.z = (coord.x + coord.y) * 0.5;
	dir.y = 1.0 - abs(dir.x) - abs(dir.z);
	return normalize(dir);
}
)";
	}

	code += R"(
// X is right and Y is up in the view from the direction, must match get_frame_basis().
void impostor_basis(vec3 dir, out vec3 x, out vec3 y) {
	vec3 up = abs(dir.y) > 0.999 ? vec3(0.0, 0.0, -1.0) : vec3(0.0, 1.0, 0.0);
	x = normalize(cross(up, dir));
	y = cross(dir, x);
}

// Intersects a view ray with the plane of a frame (through the center of the bounding sphere).
vec2 impostor_frame_uv(vec2 frame, vec3 origin, vec3 ray, out vec3 frame_ray) {
	vec3 dir = impostor_decode(frame / float(frames - 1) * 2.0 - 1.0);
	vec3 x;
	vec3 y;
	impostor_basis(dir, x, y);
	float facing = min(dot(ray, dir), -0.0001);
	vec3 hit = origin + ray * (dot(sphere_center - origin, dir) / facing) - sphere_center;
	frame_ray = vec3(dot(ray, x), -dot(ray, y), facing);
	return vec2(dot(hit, x), -dot(hit, y)) / (2.0 * sphere_radius) + 0.5;
}

void vertex() {
	mat4 view_to_object = inverse(MODELVIEW_MATRIX);
	bool orthogonal = PROJECTION_MATRIX[3][3] != 0.0;
	vec3 camera = view_to_object[3].xyz;
	// Also the light in shadow passes, so that shadows are cast by the views from the light.
	vec3 to_camera = normalize(orthogonal ? view_to_object[2].xyz : camera - sphere_center);

	// Frames of the views closest to the camera.
	vec2 grid = (impostor_encode(to_camera) * 0.5 + 0.5) * float(frames - 1);
)";
	if (blend) {
		code += R"(	// Triangle of the grid cell that contains the view, with barycentric weights.
	vec2 grid_floor = clamp(floor(grid), vec2(0.0), vec2(float(frames - 2)));
	vec2 grid_fract = clamp(grid - grid_floor, 0.0, 1.0);
	vec2 view_a = grid_floor;
	vec2 view_b = grid_floor + (grid_fract.x > grid_fract.y ? vec2(1.0, 0.0) : vec2(0.0, 1.0));
	vec2 view_c = grid_floor + vec2(1.0);
	frame_a = view_a;
	frame_b = view_b;
	frame_c = view_c;
	frame_weights = vec3(min(1.0 - grid_fract.x, 1.0 - grid_fract.y), abs(grid_fract.x - grid_fract.y), min(grid_fract.x, grid_fract.y));
)";
	} else {
		code += "	vec2 view_a = clamp(round(grid), vec2(0.0), vec2(float(frames - 1)));\n";
		code += "	frame_a = view_a;\n";
	}

	code += R"(
	// Quad facing the camera, around the bounding sphere.
	vec3 right;
	vec3 up;
	impostor_basis(to_camera, right, up);
	vec2 corner = vec2(UV.x * 2.0 - 1.0, 1.0 - UV.y * 2.0);
)";
	if (p_key.depth_offset) {
		code += "	// In front of the object, the fragments write the depth of the surface behind.\n";
		code += "	float lift = sphere_radius;\n";
	} else {
		code += "	// Toward the camera to limit the intersections, away from the light to avoid self-shadowing.\n";
		code += "	float lift = IN_SHADOW_PASS ? -0.5 * sphere_radius : 0.5 * sphere_radius;\n";
	}
	code += R"(	VERTEX = sphere_center + (right * corner.x + up * corner.y) * sphere_radius + to_camera * lift;
	NORMAL = to_camera;
	TANGENT = right;
	BINORMAL = up;

	vec3 origin = orthogonal ? VERTEX + to_camera * (4.0 * sphere_radius) : camera;
	vec3 ray = normalize(VERTEX - origin);
	vec3 frame_ray;
	frame_uv_a = impostor_frame_uv(view_a, origin, ray, frame_ray);
)";
	if (parallax) {
		code += "	frame_ray_a = frame_ray;\n";
	}
	if (blend) {
		code += "	frame_uv_b = impostor_frame_uv(view_b, origin, ray, frame_ray);\n";
		if (parallax) {
			code += "	frame_ray_b = frame_ray;\n";
		}
		code += "	frame_uv_c = impostor_frame_uv(view_c, origin, ray, frame_ray);\n";
		if (parallax) {
			code += "	frame_ray_c = frame_ray;\n";
		}
	}
	code += "\n	object_to_view_normal = MODELVIEW_NORMAL_MATRIX;\n";
	if (p_key.depth_offset) {
		code += "	sphere_radius_view = sphere_radius * length(MODELVIEW_MATRIX[0].xyz);\n";
	}
	code += "}\n";

	// Fragment.
	code += R"(
vec2 impostor_atlas_uv(vec2 frame, vec2 uv, vec3 frame_ray, vec2 uv_dx, vec2 uv_dy) {
	vec2 tile = vec2(1.0 / float(frames));
	uv = clamp(uv, 0.0, 1.0);
)";
	if (parallax) {
		code += R"(	// Moves along the view ray to the height of the surface, read from the depth of the frame.
	float depth = textureGrad(texture_normal_depth, (frame + uv) * tile, uv_dx, uv_dy).a;
	uv = clamp(uv + frame_ray.xy * ((0.5 - depth) * parallax_scale / min(frame_ray.z, -0.25)), 0.0, 1.0);
)";
	}
	code += "	return (frame + uv) * tile;\n}\n";

	if (blend) {
		code += R"(
vec4 impostor_sample(sampler2D tex, vec2 uv_a, vec2 uv_b, vec2 uv_c, vec3 weights, vec2 uv_dx, vec2 uv_dy) {
	return textureGrad(tex, uv_a, uv_dx, uv_dy) * weights.x + textureGrad(tex, uv_b, uv_dx, uv_dy) * weights.y + textureGrad(tex, uv_c, uv_dx, uv_dy) * weights.z;
}
)";
	} else {
		code += R"(
vec4 impostor_sample(sampler2D tex, vec2 uv_a, vec2 uv_dx, vec2 uv_dy) {
	return textureGrad(tex, uv_a, uv_dx, uv_dy);
}
)";
	}

	code += R"(
void fragment() {
	// Mipmaps from the smooth coordinates of the first frame, limited so that frames keep at least
	// 8 pixels: the lower mipmaps mix neighbor frames.
	vec2 tile = vec2(1.0 / float(frames));
	vec2 atlas_size = vec2(textureSize(texture_albedo, 0));
	vec2 uv_dx = dFdx(frame_uv_a) * tile;
	vec2 uv_dy = dFdy(frame_uv_a) * tile;
	float lod = 0.5 * log2(max(dot(uv_dx * atlas_size, uv_dx * atlas_size), dot(uv_dy * atlas_size, uv_dy * atlas_size)));
	float max_lod = log2(max(atlas_size.x * tile.x, 1.0)) - 3.0;
	float lod_scale = exp2(min(max_lod - lod, 0.0));
	uv_dx *= lod_scale;
	uv_dy *= lod_scale;

)";
	const String no_ray = "vec3(0.0, 0.0, -1.0)";
	code += vformat("	vec2 uv_a = impostor_atlas_uv(frame_a, frame_uv_a, %s, uv_dx, uv_dy);\n", parallax ? "frame_ray_a" : no_ray);
	String samples = "uv_a";
	if (blend) {
		code += vformat("	vec2 uv_b = impostor_atlas_uv(frame_b, frame_uv_b, %s, uv_dx, uv_dy);\n", parallax ? "frame_ray_b" : no_ray);
		code += vformat("	vec2 uv_c = impostor_atlas_uv(frame_c, frame_uv_c, %s, uv_dx, uv_dy);\n", parallax ? "frame_ray_c" : no_ray);
		samples = "uv_a, uv_b, uv_c, frame_weights";
	}
	code += vformat("	vec4 albedo_tex = impostor_sample(texture_albedo, %s, uv_dx, uv_dy);\n", samples);
	code += vformat("	vec4 normal_depth_tex = impostor_sample(texture_normal_depth, %s, uv_dx, uv_dy);\n", samples);
	if (orm) {
		code += vformat("	vec4 orm_tex = impostor_sample(texture_orm, %s, uv_dx, uv_dy);\n", samples);
	}

	code += "\n	ALBEDO = albedo.rgb * albedo_tex.rgb;\n";
	code += "	ALPHA = albedo.a * albedo_tex.a;\n";
	if (p_key.vertex_color) {
		code += "	ALBEDO *= COLOR.rgb;\n";
		code += "	ALPHA *= COLOR.a;\n";
	}
	if (p_key.alpha_hash) {
		code += "	ALPHA_HASH_SCALE = alpha_hash_scale;\n";
	} else {
		code += "	ALPHA_SCISSOR_THRESHOLD = alpha_scissor_threshold;\n";
	}
	if (p_key.alpha_antialiasing != BaseMaterial3D::ALPHA_ANTIALIASING_OFF) {
		code += "	ALPHA_ANTIALIASING_EDGE = alpha_antialiasing_edge;\n";
		code += "	ALPHA_TEXTURE_COORDINATE = uv_a * atlas_size;\n";
	}

	code += R"(
	vec3 normal = normal_depth_tex.rgb * 2.0 - 1.0;
	if (dot(normal, normal) > 1e-6) {
		NORMAL = normalize(object_to_view_normal * normal);
	}
)";
	if (orm) {
		code += R"(	AO = orm_tex.r;
	AO_LIGHT_AFFECT = ao_light_affect;
	ROUGHNESS = orm_tex.g * roughness;
	METALLIC = orm_tex.b * metallic;
)";
	} else {
		code += "	ROUGHNESS = roughness;\n";
		code += "	METALLIC = metallic;\n";
	}
	code += "	SPECULAR = specular;\n";
	if (p_key.backlight) {
		code += "	BACKLIGHT = backlight.rgb;\n";
	}
	if (p_key.depth_offset) {
		code += R"(
	// Depth and lighting position of the surface seen through the pixel, for intersections and shadows.
	vec3 view_ray = PROJECTION_MATRIX[3][3] != 0.0 ? vec3(0.0, 0.0, -1.0) : normalize(VERTEX);
	vec3 surface = VERTEX + view_ray * (2.0 * sphere_radius_view * normal_depth_tex.a);
	LIGHT_VERTEX = surface;
	vec4 surface_clip = PROJECTION_MATRIX * vec4(surface, 1.0);
	DEPTH = (surface_clip.z / surface_clip.w - CLIP_SPACE_FAR) / (1.0 - CLIP_SPACE_FAR);
)";
	}
	code += "}\n";
	return code;
}

void OctahedralImpostorMaterial3D::_update_shader() {
	const ShaderKey key = _compute_key();

	MutexLock lock(shader_mutex);
	if (key_valid && key.key == current_key.key) {
		return;
	}

	ShaderData *data = shader_map.getptr(key.key);
	if (!data) {
		ShaderData new_data;
		new_data.shader = RS::get_singleton()->shader_create();
		RS::get_singleton()->shader_set_code(new_data.shader, _generate_shader_code(key));
		data = &shader_map.insert(key.key, new_data)->value;
	}
	data->users++;
	RS::get_singleton()->material_set_shader(_get_material(), data->shader);

	if (key_valid) {
		ShaderData *old = shader_map.getptr(current_key.key);
		if (old) {
			old->users--;
			if (old->users == 0) {
				RS::get_singleton()->free_rid(old->shader);
				shader_map.erase(current_key.key);
			}
		}
	}
	current_key = key;
	key_valid = true;
}

void OctahedralImpostorMaterial3D::_set_param(const StringName &p_name, const Variant &p_value) {
	RS::get_singleton()->material_set_param(_get_material(), p_name, p_value);
}

void OctahedralImpostorMaterial3D::cleanup_shaders() {
	MutexLock lock(shader_mutex);
	ERR_FAIL_NULL(RenderingServer::get_singleton());
	for (const KeyValue<uint32_t, ShaderData> &E : shader_map) {
		RS::get_singleton()->free_rid(E.value.shader);
	}
	shader_map.clear();
}

Shader::Mode OctahedralImpostorMaterial3D::get_shader_mode() const {
	return Shader::MODE_SPATIAL;
}

RID OctahedralImpostorMaterial3D::get_shader_rid() const {
	MutexLock lock(shader_mutex);
	const ShaderData *data = key_valid ? shader_map.getptr(current_key.key) : nullptr;
	return data ? data->shader : RID();
}

/* Properties */

void OctahedralImpostorMaterial3D::set_layout(Layout p_layout) {
	ERR_FAIL_INDEX(p_layout, LAYOUT_MAX);
	layout = p_layout;
	_update_shader();
}

OctahedralImpostorMaterial3D::Layout OctahedralImpostorMaterial3D::get_layout() const {
	return layout;
}

void OctahedralImpostorMaterial3D::set_frames(int p_frames) {
	frames = CLAMP(p_frames, MIN_FRAMES, MAX_FRAMES);
	_set_param(SNAME("frames"), frames);
}

int OctahedralImpostorMaterial3D::get_frames() const {
	return frames;
}

void OctahedralImpostorMaterial3D::set_blend_frames(bool p_enable) {
	blend_frames = p_enable;
	_update_shader();
}

bool OctahedralImpostorMaterial3D::is_blending_frames() const {
	return blend_frames;
}

void OctahedralImpostorMaterial3D::set_sphere_center(const Vector3 &p_center) {
	sphere_center = p_center;
	_set_param(SNAME("sphere_center"), sphere_center);
}

Vector3 OctahedralImpostorMaterial3D::get_sphere_center() const {
	return sphere_center;
}

void OctahedralImpostorMaterial3D::set_sphere_radius(float p_radius) {
	sphere_radius = MAX(p_radius, 0.001f);
	_set_param(SNAME("sphere_radius"), sphere_radius);
}

float OctahedralImpostorMaterial3D::get_sphere_radius() const {
	return sphere_radius;
}

void OctahedralImpostorMaterial3D::set_texture(TextureParam p_param, const Ref<Texture2D> &p_texture) {
	ERR_FAIL_INDEX(p_param, TEXTURE_MAX);
	textures[p_param] = p_texture;
	static const char *names[TEXTURE_MAX] = { "texture_albedo", "texture_normal_depth", "texture_orm" };
	_set_param(names[p_param], p_texture.is_valid() ? Variant(p_texture->get_rid()) : Variant());
	if (p_param == TEXTURE_ORM) {
		_update_shader();
		notify_property_list_changed();
	}
}

Ref<Texture2D> OctahedralImpostorMaterial3D::get_texture(TextureParam p_param) const {
	ERR_FAIL_INDEX_V(p_param, TEXTURE_MAX, Ref<Texture2D>());
	return textures[p_param];
}

void OctahedralImpostorMaterial3D::set_albedo_color(const Color &p_color) {
	albedo_color = p_color;
	_set_param(SNAME("albedo"), albedo_color);
}

Color OctahedralImpostorMaterial3D::get_albedo_color() const {
	return albedo_color;
}

void OctahedralImpostorMaterial3D::set_parallax_scale(float p_scale) {
	parallax_scale = CLAMP(p_scale, 0.0f, 1.0f);
	_set_param(SNAME("parallax_scale"), parallax_scale);
	_update_shader();
}

float OctahedralImpostorMaterial3D::get_parallax_scale() const {
	return parallax_scale;
}

void OctahedralImpostorMaterial3D::set_depth_offset_enabled(bool p_enable) {
	depth_offset_enabled = p_enable;
	_update_shader();
}

bool OctahedralImpostorMaterial3D::is_depth_offset_enabled() const {
	return depth_offset_enabled;
}

void OctahedralImpostorMaterial3D::set_metallic(float p_metallic) {
	metallic = p_metallic;
	_set_param(SNAME("metallic"), metallic);
}

float OctahedralImpostorMaterial3D::get_metallic() const {
	return metallic;
}

void OctahedralImpostorMaterial3D::set_metallic_specular(float p_specular) {
	metallic_specular = p_specular;
	_set_param(SNAME("specular"), metallic_specular);
}

float OctahedralImpostorMaterial3D::get_metallic_specular() const {
	return metallic_specular;
}

void OctahedralImpostorMaterial3D::set_roughness(float p_roughness) {
	roughness = p_roughness;
	_set_param(SNAME("roughness"), roughness);
}

float OctahedralImpostorMaterial3D::get_roughness() const {
	return roughness;
}

void OctahedralImpostorMaterial3D::set_ao_light_affect(float p_affect) {
	ao_light_affect = p_affect;
	_set_param(SNAME("ao_light_affect"), ao_light_affect);
}

float OctahedralImpostorMaterial3D::get_ao_light_affect() const {
	return ao_light_affect;
}

void OctahedralImpostorMaterial3D::set_transparency(Transparency p_transparency) {
	ERR_FAIL_INDEX(p_transparency, TRANSPARENCY_MAX);
	transparency = p_transparency;
	_update_shader();
	notify_property_list_changed();
}

OctahedralImpostorMaterial3D::Transparency OctahedralImpostorMaterial3D::get_transparency() const {
	return transparency;
}

void OctahedralImpostorMaterial3D::set_alpha_scissor_threshold(float p_threshold) {
	alpha_scissor_threshold = p_threshold;
	_set_param(SNAME("alpha_scissor_threshold"), alpha_scissor_threshold);
}

float OctahedralImpostorMaterial3D::get_alpha_scissor_threshold() const {
	return alpha_scissor_threshold;
}

void OctahedralImpostorMaterial3D::set_alpha_hash_scale(float p_scale) {
	alpha_hash_scale = p_scale;
	_set_param(SNAME("alpha_hash_scale"), alpha_hash_scale);
}

float OctahedralImpostorMaterial3D::get_alpha_hash_scale() const {
	return alpha_hash_scale;
}

void OctahedralImpostorMaterial3D::set_alpha_antialiasing(BaseMaterial3D::AlphaAntiAliasing p_mode) {
	ERR_FAIL_INDEX(p_mode, BaseMaterial3D::ALPHA_ANTIALIASING_MAX);
	alpha_antialiasing_mode = p_mode;
	_update_shader();
	notify_property_list_changed();
}

BaseMaterial3D::AlphaAntiAliasing OctahedralImpostorMaterial3D::get_alpha_antialiasing() const {
	return alpha_antialiasing_mode;
}

void OctahedralImpostorMaterial3D::set_alpha_antialiasing_edge(float p_edge) {
	alpha_antialiasing_edge = p_edge;
	_set_param(SNAME("alpha_antialiasing_edge"), alpha_antialiasing_edge);
}

float OctahedralImpostorMaterial3D::get_alpha_antialiasing_edge() const {
	return alpha_antialiasing_edge;
}

void OctahedralImpostorMaterial3D::set_vertex_color_use_as_albedo(bool p_enable) {
	vertex_color_use_as_albedo = p_enable;
	_update_shader();
}

bool OctahedralImpostorMaterial3D::is_vertex_color_used_as_albedo() const {
	return vertex_color_use_as_albedo;
}

void OctahedralImpostorMaterial3D::set_backlight_enabled(bool p_enable) {
	backlight_enabled = p_enable;
	_update_shader();
	notify_property_list_changed();
}

bool OctahedralImpostorMaterial3D::is_backlight_enabled() const {
	return backlight_enabled;
}

void OctahedralImpostorMaterial3D::set_backlight(const Color &p_backlight) {
	backlight = p_backlight;
	_set_param(SNAME("backlight"), backlight);
}

Color OctahedralImpostorMaterial3D::get_backlight() const {
	return backlight;
}

AABB OctahedralImpostorMaterial3D::get_bounds() const {
	// Corners of the quad, lifted toward the camera (by the radius with the depth offset, by half of it otherwise).
	const real_t lift = depth_offset_enabled ? 1.0 : 0.5;
	const real_t extent = sphere_radius * Math::sqrt(2.0 + lift * lift);
	return AABB(sphere_center - Vector3(extent, extent, extent), Vector3(extent, extent, extent) * 2.0);
}

void OctahedralImpostorMaterial3D::_validate_property(PropertyInfo &p_property) const {
	if (p_property.name == "alpha_scissor_threshold" && transparency != TRANSPARENCY_ALPHA_SCISSOR) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	} else if (p_property.name == "alpha_hash_scale" && transparency != TRANSPARENCY_ALPHA_HASH) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	} else if (p_property.name == "alpha_antialiasing_edge" && alpha_antialiasing_mode == BaseMaterial3D::ALPHA_ANTIALIASING_OFF) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	} else if (p_property.name == "ao_light_affect" && textures[TEXTURE_ORM].is_null()) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	} else if (p_property.name == "backlight" && !backlight_enabled) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
}

void OctahedralImpostorMaterial3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_layout", "layout"), &OctahedralImpostorMaterial3D::set_layout);
	ClassDB::bind_method(D_METHOD("get_layout"), &OctahedralImpostorMaterial3D::get_layout);
	ClassDB::bind_method(D_METHOD("set_frames", "frames"), &OctahedralImpostorMaterial3D::set_frames);
	ClassDB::bind_method(D_METHOD("get_frames"), &OctahedralImpostorMaterial3D::get_frames);
	ClassDB::bind_method(D_METHOD("set_blend_frames", "enable"), &OctahedralImpostorMaterial3D::set_blend_frames);
	ClassDB::bind_method(D_METHOD("is_blending_frames"), &OctahedralImpostorMaterial3D::is_blending_frames);
	ClassDB::bind_method(D_METHOD("set_sphere_center", "center"), &OctahedralImpostorMaterial3D::set_sphere_center);
	ClassDB::bind_method(D_METHOD("get_sphere_center"), &OctahedralImpostorMaterial3D::get_sphere_center);
	ClassDB::bind_method(D_METHOD("set_sphere_radius", "radius"), &OctahedralImpostorMaterial3D::set_sphere_radius);
	ClassDB::bind_method(D_METHOD("get_sphere_radius"), &OctahedralImpostorMaterial3D::get_sphere_radius);
	ClassDB::bind_method(D_METHOD("set_texture", "param", "texture"), &OctahedralImpostorMaterial3D::set_texture);
	ClassDB::bind_method(D_METHOD("get_texture", "param"), &OctahedralImpostorMaterial3D::get_texture);
	ClassDB::bind_method(D_METHOD("set_albedo_color", "color"), &OctahedralImpostorMaterial3D::set_albedo_color);
	ClassDB::bind_method(D_METHOD("get_albedo_color"), &OctahedralImpostorMaterial3D::get_albedo_color);
	ClassDB::bind_method(D_METHOD("set_parallax_scale", "scale"), &OctahedralImpostorMaterial3D::set_parallax_scale);
	ClassDB::bind_method(D_METHOD("get_parallax_scale"), &OctahedralImpostorMaterial3D::get_parallax_scale);
	ClassDB::bind_method(D_METHOD("set_depth_offset_enabled", "enable"), &OctahedralImpostorMaterial3D::set_depth_offset_enabled);
	ClassDB::bind_method(D_METHOD("is_depth_offset_enabled"), &OctahedralImpostorMaterial3D::is_depth_offset_enabled);
	ClassDB::bind_method(D_METHOD("set_metallic", "metallic"), &OctahedralImpostorMaterial3D::set_metallic);
	ClassDB::bind_method(D_METHOD("get_metallic"), &OctahedralImpostorMaterial3D::get_metallic);
	ClassDB::bind_method(D_METHOD("set_metallic_specular", "specular"), &OctahedralImpostorMaterial3D::set_metallic_specular);
	ClassDB::bind_method(D_METHOD("get_metallic_specular"), &OctahedralImpostorMaterial3D::get_metallic_specular);
	ClassDB::bind_method(D_METHOD("set_roughness", "roughness"), &OctahedralImpostorMaterial3D::set_roughness);
	ClassDB::bind_method(D_METHOD("get_roughness"), &OctahedralImpostorMaterial3D::get_roughness);
	ClassDB::bind_method(D_METHOD("set_ao_light_affect", "amount"), &OctahedralImpostorMaterial3D::set_ao_light_affect);
	ClassDB::bind_method(D_METHOD("get_ao_light_affect"), &OctahedralImpostorMaterial3D::get_ao_light_affect);
	ClassDB::bind_method(D_METHOD("set_transparency", "transparency"), &OctahedralImpostorMaterial3D::set_transparency);
	ClassDB::bind_method(D_METHOD("get_transparency"), &OctahedralImpostorMaterial3D::get_transparency);
	ClassDB::bind_method(D_METHOD("set_alpha_scissor_threshold", "threshold"), &OctahedralImpostorMaterial3D::set_alpha_scissor_threshold);
	ClassDB::bind_method(D_METHOD("get_alpha_scissor_threshold"), &OctahedralImpostorMaterial3D::get_alpha_scissor_threshold);
	ClassDB::bind_method(D_METHOD("set_alpha_hash_scale", "scale"), &OctahedralImpostorMaterial3D::set_alpha_hash_scale);
	ClassDB::bind_method(D_METHOD("get_alpha_hash_scale"), &OctahedralImpostorMaterial3D::get_alpha_hash_scale);
	ClassDB::bind_method(D_METHOD("set_alpha_antialiasing", "alpha_aa"), &OctahedralImpostorMaterial3D::set_alpha_antialiasing);
	ClassDB::bind_method(D_METHOD("get_alpha_antialiasing"), &OctahedralImpostorMaterial3D::get_alpha_antialiasing);
	ClassDB::bind_method(D_METHOD("set_alpha_antialiasing_edge", "edge"), &OctahedralImpostorMaterial3D::set_alpha_antialiasing_edge);
	ClassDB::bind_method(D_METHOD("get_alpha_antialiasing_edge"), &OctahedralImpostorMaterial3D::get_alpha_antialiasing_edge);
	ClassDB::bind_method(D_METHOD("set_vertex_color_use_as_albedo", "enable"), &OctahedralImpostorMaterial3D::set_vertex_color_use_as_albedo);
	ClassDB::bind_method(D_METHOD("is_vertex_color_used_as_albedo"), &OctahedralImpostorMaterial3D::is_vertex_color_used_as_albedo);
	ClassDB::bind_method(D_METHOD("set_backlight_enabled", "enable"), &OctahedralImpostorMaterial3D::set_backlight_enabled);
	ClassDB::bind_method(D_METHOD("is_backlight_enabled"), &OctahedralImpostorMaterial3D::is_backlight_enabled);
	ClassDB::bind_method(D_METHOD("set_backlight", "backlight"), &OctahedralImpostorMaterial3D::set_backlight);
	ClassDB::bind_method(D_METHOD("get_backlight"), &OctahedralImpostorMaterial3D::get_backlight);
	ClassDB::bind_method(D_METHOD("get_bounds"), &OctahedralImpostorMaterial3D::get_bounds);

	ADD_GROUP("Frames", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "layout", PROPERTY_HINT_ENUM, "Hemisphere,Full Sphere"), "set_layout", "get_layout");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "frames", PROPERTY_HINT_RANGE, itos(MIN_FRAMES) + "," + itos(MAX_FRAMES) + ",1"), "set_frames", "get_frames");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "blend_frames"), "set_blend_frames", "is_blending_frames");

	ADD_GROUP("Bounding Sphere", "sphere_");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "sphere_center", PROPERTY_HINT_NONE, "suffix:m"), "set_sphere_center", "get_sphere_center");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sphere_radius", PROPERTY_HINT_RANGE, "0.001,1000,0.001,or_greater,suffix:m"), "set_sphere_radius", "get_sphere_radius");

	ADD_GROUP("Albedo", "albedo_");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "albedo_color"), "set_albedo_color", "get_albedo_color");
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "albedo_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_ALBEDO);

	ADD_GROUP("Normal and Depth", "");
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "normal_depth_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_NORMAL_DEPTH);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "parallax_scale", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_parallax_scale", "get_parallax_scale");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "depth_offset_enabled"), "set_depth_offset_enabled", "is_depth_offset_enabled");

	ADD_GROUP("ORM", "orm_");
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "orm_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_ORM);

	ADD_GROUP("Metallic", "metallic_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "metallic", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_metallic", "get_metallic");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "metallic_specular", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_metallic_specular", "get_metallic_specular");

	ADD_GROUP("Roughness", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "roughness", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_roughness", "get_roughness");

	ADD_GROUP("Ambient Occlusion", "ao_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ao_light_affect", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_ao_light_affect", "get_ao_light_affect");

	ADD_GROUP("Transparency", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "transparency", PROPERTY_HINT_ENUM, "Alpha Scissor,Alpha Hash"), "set_transparency", "get_transparency");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "alpha_scissor_threshold", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_alpha_scissor_threshold", "get_alpha_scissor_threshold");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "alpha_hash_scale", PROPERTY_HINT_RANGE, "0,2,0.01"), "set_alpha_hash_scale", "get_alpha_hash_scale");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "alpha_antialiasing_mode", PROPERTY_HINT_ENUM, "Disabled,Alpha Edge Blend,Alpha Edge Clip"), "set_alpha_antialiasing", "get_alpha_antialiasing");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "alpha_antialiasing_edge", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_alpha_antialiasing_edge", "get_alpha_antialiasing_edge");

	ADD_GROUP("Vertex Color", "vertex_color_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "vertex_color_use_as_albedo"), "set_vertex_color_use_as_albedo", "is_vertex_color_used_as_albedo");

	ADD_GROUP("Backlight", "backlight_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "backlight_enabled"), "set_backlight_enabled", "is_backlight_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "backlight", PROPERTY_HINT_COLOR_NO_ALPHA), "set_backlight", "get_backlight");

	BIND_ENUM_CONSTANT(LAYOUT_HEMISPHERE);
	BIND_ENUM_CONSTANT(LAYOUT_FULL_SPHERE);
	BIND_ENUM_CONSTANT(LAYOUT_MAX);

	BIND_ENUM_CONSTANT(TRANSPARENCY_ALPHA_SCISSOR);
	BIND_ENUM_CONSTANT(TRANSPARENCY_ALPHA_HASH);
	BIND_ENUM_CONSTANT(TRANSPARENCY_MAX);

	BIND_ENUM_CONSTANT(TEXTURE_ALBEDO);
	BIND_ENUM_CONSTANT(TEXTURE_NORMAL_DEPTH);
	BIND_ENUM_CONSTANT(TEXTURE_ORM);
	BIND_ENUM_CONSTANT(TEXTURE_MAX);
}

OctahedralImpostorMaterial3D::OctahedralImpostorMaterial3D() {
	_set_material(RS::get_singleton()->material_create());

	set_frames(frames);
	set_sphere_center(sphere_center);
	set_sphere_radius(sphere_radius);
	set_albedo_color(albedo_color);
	set_parallax_scale(parallax_scale);
	set_metallic(metallic);
	set_metallic_specular(metallic_specular);
	set_roughness(roughness);
	set_ao_light_affect(ao_light_affect);
	set_alpha_scissor_threshold(alpha_scissor_threshold);
	set_alpha_hash_scale(alpha_hash_scale);
	set_alpha_antialiasing_edge(alpha_antialiasing_edge);
	set_backlight(backlight);

	_update_shader();
}

OctahedralImpostorMaterial3D::~OctahedralImpostorMaterial3D() {
	ERR_FAIL_NULL(RenderingServer::get_singleton());
	MutexLock lock(shader_mutex);
	RS::get_singleton()->material_set_shader(_get_material(), RID());
	if (key_valid) {
		ShaderData *data = shader_map.getptr(current_key.key);
		if (data) {
			data->users--;
			if (data->users == 0) {
				RS::get_singleton()->free_rid(data->shader);
				shader_map.erase(current_key.key);
			}
		}
	}
}
