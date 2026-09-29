/**************************************************************************/
/*  octahedral_impostor_material_3d.h                                     */
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

#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "scene/resources/material.h"
#include "scene/resources/texture.h"

// Material of an octahedral impostor: a camera-facing quad that shows an object from the
// closest views baked into an atlas. The views are laid out on an octahedron (or a half
// octahedron for objects only seen from above), the three closest ones are blended, their
// depth gives parallax and the depth of the pixels, and their object space normals are lit
// like the real object. See OctahedralImpostorBaker for the layout of the textures.
class OctahedralImpostorMaterial3D : public Material {
	GDCLASS(OctahedralImpostorMaterial3D, Material);

public:
	enum Layout {
		LAYOUT_HEMISPHERE,
		LAYOUT_FULL_SPHERE,
		LAYOUT_MAX
	};

	enum Transparency {
		TRANSPARENCY_ALPHA_SCISSOR,
		TRANSPARENCY_ALPHA_HASH,
		TRANSPARENCY_MAX
	};

	enum TextureParam {
		TEXTURE_ALBEDO,
		TEXTURE_NORMAL_DEPTH,
		TEXTURE_ORM,
		TEXTURE_MAX
	};

	static constexpr int MIN_FRAMES = 2;
	static constexpr int MAX_FRAMES = 64;

private:
	union ShaderKey {
		struct {
			uint32_t full_sphere : 1;
			uint32_t blend_frames : 1;
			uint32_t alpha_hash : 1;
			uint32_t alpha_antialiasing : 2;
			uint32_t parallax : 1;
			uint32_t depth_offset : 1;
			uint32_t orm : 1;
			uint32_t vertex_color : 1;
			uint32_t backlight : 1;
		};
		uint32_t key = 0;
	};

	struct ShaderData {
		RID shader;
		int users = 0;
	};

	static Mutex shader_mutex;
	static HashMap<uint32_t, ShaderData> shader_map;

	ShaderKey current_key;
	bool key_valid = false;

	Layout layout = LAYOUT_HEMISPHERE;
	int frames = 12;
	bool blend_frames = true;
	Vector3 sphere_center;
	float sphere_radius = 1.0;

	Ref<Texture2D> textures[TEXTURE_MAX];

	Color albedo_color = Color(1, 1, 1, 1);
	float parallax_scale = 1.0;
	bool depth_offset_enabled = true;
	float metallic = 0.0;
	float metallic_specular = 0.5;
	float roughness = 1.0;
	float ao_light_affect = 0.0;

	Transparency transparency = TRANSPARENCY_ALPHA_SCISSOR;
	float alpha_scissor_threshold = 0.5;
	float alpha_hash_scale = 1.0;
	BaseMaterial3D::AlphaAntiAliasing alpha_antialiasing_mode = BaseMaterial3D::ALPHA_ANTIALIASING_OFF;
	float alpha_antialiasing_edge = 0.3;

	bool vertex_color_use_as_albedo = false;
	bool backlight_enabled = false;
	Color backlight = Color(0, 0, 0);

	ShaderKey _compute_key() const;
	void _update_shader();
	static String _generate_shader_code(ShaderKey p_key);
	void _set_param(const StringName &p_name, const Variant &p_value);

protected:
	static void _bind_methods();
	void _validate_property(PropertyInfo &p_property) const;

public:
	void set_layout(Layout p_layout);
	Layout get_layout() const;

	void set_frames(int p_frames);
	int get_frames() const;

	void set_blend_frames(bool p_enable);
	bool is_blending_frames() const;

	void set_sphere_center(const Vector3 &p_center);
	Vector3 get_sphere_center() const;

	void set_sphere_radius(float p_radius);
	float get_sphere_radius() const;

	void set_texture(TextureParam p_param, const Ref<Texture2D> &p_texture);
	Ref<Texture2D> get_texture(TextureParam p_param) const;

	void set_albedo_color(const Color &p_color);
	Color get_albedo_color() const;

	void set_parallax_scale(float p_scale);
	float get_parallax_scale() const;

	void set_depth_offset_enabled(bool p_enable);
	bool is_depth_offset_enabled() const;

	void set_metallic(float p_metallic);
	float get_metallic() const;

	void set_metallic_specular(float p_specular);
	float get_metallic_specular() const;

	void set_roughness(float p_roughness);
	float get_roughness() const;

	void set_ao_light_affect(float p_affect);
	float get_ao_light_affect() const;

	void set_transparency(Transparency p_transparency);
	Transparency get_transparency() const;

	void set_alpha_scissor_threshold(float p_threshold);
	float get_alpha_scissor_threshold() const;

	void set_alpha_hash_scale(float p_scale);
	float get_alpha_hash_scale() const;

	void set_alpha_antialiasing(BaseMaterial3D::AlphaAntiAliasing p_mode);
	BaseMaterial3D::AlphaAntiAliasing get_alpha_antialiasing() const;

	void set_alpha_antialiasing_edge(float p_edge);
	float get_alpha_antialiasing_edge() const;

	void set_vertex_color_use_as_albedo(bool p_enable);
	bool is_vertex_color_used_as_albedo() const;

	void set_backlight_enabled(bool p_enable);
	bool is_backlight_enabled() const;

	void set_backlight(const Color &p_backlight);
	Color get_backlight() const;

	// Axis-aligned box that contains the impostor from any view, for the custom AABB of its mesh.
	AABB get_bounds() const;

	virtual Shader::Mode get_shader_mode() const override;
	virtual RID get_shader_rid() const override;

	// Octahedral mapping shared by the baker and the shader. Coordinates are in [-1, 1]; a frame
	// of an atlas with N frames per side is the view from the direction at coordinate
	// (frame / (N - 1)) * 2 - 1, looking at the center of the bounding sphere.
	static Vector2 octahedral_encode(const Vector3 &p_direction, Layout p_layout);
	static Vector3 octahedral_decode(const Vector2 &p_coord, Layout p_layout);
	static Vector3 get_frame_direction(const Vector2i &p_frame, int p_frames, Layout p_layout);
	// Basis of the view of a frame (columns): X is right and Y is up in the image, Z points to the viewer.
	static Basis get_frame_basis(const Vector3 &p_direction);

	static void cleanup_shaders();

	OctahedralImpostorMaterial3D();
	~OctahedralImpostorMaterial3D() override;
};

VARIANT_ENUM_CAST(OctahedralImpostorMaterial3D::Layout)
VARIANT_ENUM_CAST(OctahedralImpostorMaterial3D::Transparency)
VARIANT_ENUM_CAST(OctahedralImpostorMaterial3D::TextureParam)
