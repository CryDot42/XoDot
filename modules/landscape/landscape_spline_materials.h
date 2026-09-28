/**************************************************************************/
/*  landscape_spline_materials.h                                          */
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
#include "scene/resources/image_texture.h"
#include "scene/resources/material.h"

// Base of the built-in materials of landscape splines: a fixed shader shared by all instances,
// whose uniforms are exposed as typed properties (like FogMaterial or StandardMaterial3D).
class LandscapeSplineMaterial : public Material {
	GDCLASS(LandscapeSplineMaterial, Material);

public:
	struct ParameterInfo {
		const char *name; // Property and shader uniform name.
		Variant::Type type;
		PropertyHint hint;
		const char *hint_string;
		const char *group;
	};

protected:
	Vector<Variant> parameters;
	mutable bool shader_set = false;

	static void _bind_methods();
	static void _bind_parameters(const StringName &p_class, const ParameterInfo *p_infos, int p_count);
	void _init_parameters();
	void _apply_parameter(int p_index);

	virtual int _get_parameter_count() const { return 0; }
	virtual const ParameterInfo *_get_parameter_infos() const { return nullptr; }
	virtual Variant _get_parameter_default(int p_index) const { return Variant(); }
	// Texture bound when a texture parameter is null (built-in textures).
	virtual RID _get_default_texture(int p_index) const { return RID(); }
	virtual RID _get_shader() const { return RID(); }
	virtual String _get_source_code() const { return String(); }
	static RID _create_shader(const String &p_code);

	// Typed getters of the parameters, so that the properties have their real type in the API.
	bool _get_parameter_bool(int p_index) const { return get_parameter_by_index(p_index); }
	int _get_parameter_int(int p_index) const { return get_parameter_by_index(p_index); }
	float _get_parameter_float(int p_index) const { return get_parameter_by_index(p_index); }
	Vector2 _get_parameter_vector2(int p_index) const { return get_parameter_by_index(p_index); }
	Color _get_parameter_color(int p_index) const { return get_parameter_by_index(p_index); }
	Ref<Texture2D> _get_parameter_texture(int p_index) const { return get_parameter_by_index(p_index); }

public:
	void set_parameter_by_index(int p_index, const Variant &p_value);
	Variant get_parameter_by_index(int p_index) const;
	int find_parameter(const StringName &p_name) const;
	int get_parameter_count() const { return _get_parameter_count(); }
	String get_parameter_name(int p_index) const;

	virtual Shader::Mode get_shader_mode() const override { return Shader::MODE_SPATIAL; }
	virtual RID get_shader_rid() const override { return _get_shader(); }
	virtual RID get_rid() const override;
	// Shader code of the material, e.g. to create a customized ShaderMaterial from it.
	String get_shader_code() const;

	static void cleanup_shared_resources();

	LandscapeSplineMaterial();
	~LandscapeSplineMaterial();
};

// Built-in material of rivers, streams and lakes: ripples that follow the per-vertex flow velocity
// (flow mapping with two crossfaded phases), light absorption with depth, refraction, shore and
// rapids foam, soft intersection with the ground and an optional swell.
class LandscapeWaterMaterial : public LandscapeSplineMaterial {
	GDCLASS(LandscapeWaterMaterial, LandscapeSplineMaterial);

	static Mutex shader_mutex;
	static RID shader;
	static Ref<ImageTexture> default_normal;
	static Ref<ImageTexture> default_foam;

protected:
	static void _bind_methods();

	virtual int _get_parameter_count() const override;
	virtual const ParameterInfo *_get_parameter_infos() const override;
	virtual Variant _get_parameter_default(int p_index) const override;
	virtual RID _get_default_texture(int p_index) const override;
	virtual RID _get_shader() const override;
	virtual String _get_source_code() const override { return get_builtin_shader_code(); }

public:
	static String get_builtin_shader_code();
	static Ref<Texture2D> get_default_normal_texture();
	static Ref<Texture2D> get_default_foam_texture();
	static void cleanup_shared_resources();

	LandscapeWaterMaterial();
};

// Built-in material of roads: asphalt, dirt or gravel surface with procedural grain, wheel tracks
// and patches, lane markings, dirty and irregular edges blending into the terrain, wetness and
// puddles. Optional albedo and normal textures (U across the road, V along it).
class LandscapeRoadMaterial : public LandscapeSplineMaterial {
	GDCLASS(LandscapeRoadMaterial, LandscapeSplineMaterial);

	static Mutex shader_mutex;
	static RID shader;

public:
	enum Preset {
		PRESET_ASPHALT,
		PRESET_DIRT,
		PRESET_GRAVEL,
	};

protected:
	static void _bind_methods();

	virtual int _get_parameter_count() const override;
	virtual const ParameterInfo *_get_parameter_infos() const override;
	virtual Variant _get_parameter_default(int p_index) const override;
	virtual RID _get_shader() const override;
	virtual String _get_source_code() const override { return get_builtin_shader_code(); }

public:
	void apply_preset(Preset p_preset);
	static String get_builtin_shader_code();
	static void cleanup_shared_resources();

	LandscapeRoadMaterial();
};

VARIANT_ENUM_CAST(LandscapeRoadMaterial::Preset);
