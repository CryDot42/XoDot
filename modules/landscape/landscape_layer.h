/**************************************************************************/
/*  landscape_layer.h                                                     */
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
#include "scene/resources/texture.h"

// Material layer painted on a Landscape3D (up to 16 per landscape).
// Textures of all layers are packed into two texture arrays by the landscape:
// albedo (rgb) + height (a) and normal (rg) + roughness (b) + ambient occlusion (a).
class LandscapeLayer : public Resource {
	GDCLASS(LandscapeLayer, Resource);

	String layer_name;

	Ref<Texture2D> albedo_texture;
	Color albedo_color = Color(1, 1, 1);
	Ref<Texture2D> normal_texture;
	float normal_strength = 1.0;
	Ref<Texture2D> roughness_texture;
	float roughness = 1.0;
	float metallic = 0.0;
	Ref<Texture2D> ao_texture;
	float ao_strength = 1.0;
	Ref<Texture2D> height_texture;

	float tile_size = 4.0;
	float uv_rotation = 0.0;
	bool triplanar = false;
	float triplanar_sharpness = 4.0;
	float height_blend = 0.5;
	float displacement = 0.0;

	void _texture_changed();
	void _set_texture(Ref<Texture2D> &r_texture, const Ref<Texture2D> &p_texture);

protected:
	static void _bind_methods();

public:
	void set_layer_name(const String &p_name);
	String get_layer_name() const { return layer_name; }

	void set_albedo_texture(const Ref<Texture2D> &p_texture);
	Ref<Texture2D> get_albedo_texture() const { return albedo_texture; }
	void set_albedo_color(const Color &p_color);
	Color get_albedo_color() const { return albedo_color; }

	void set_normal_texture(const Ref<Texture2D> &p_texture);
	Ref<Texture2D> get_normal_texture() const { return normal_texture; }
	void set_normal_strength(float p_strength);
	float get_normal_strength() const { return normal_strength; }

	void set_roughness_texture(const Ref<Texture2D> &p_texture);
	Ref<Texture2D> get_roughness_texture() const { return roughness_texture; }
	void set_roughness(float p_roughness);
	float get_roughness() const { return roughness; }
	void set_metallic(float p_metallic);
	float get_metallic() const { return metallic; }

	void set_ao_texture(const Ref<Texture2D> &p_texture);
	Ref<Texture2D> get_ao_texture() const { return ao_texture; }
	void set_ao_strength(float p_strength);
	float get_ao_strength() const { return ao_strength; }

	void set_height_texture(const Ref<Texture2D> &p_texture);
	Ref<Texture2D> get_height_texture() const { return height_texture; }

	void set_tile_size(float p_size);
	float get_tile_size() const { return tile_size; }
	void set_uv_rotation(float p_radians);
	float get_uv_rotation() const { return uv_rotation; }
	void set_triplanar(bool p_enable);
	bool is_triplanar() const { return triplanar; }
	void set_triplanar_sharpness(float p_sharpness);
	float get_triplanar_sharpness() const { return triplanar_sharpness; }
	void set_height_blend(float p_blend);
	float get_height_blend() const { return height_blend; }
	void set_displacement(float p_displacement);
	float get_displacement() const { return displacement; }

	// Images used to build the landscape texture arrays (decompressed, RGBA8, square, with mipmaps).
	Ref<Image> build_albedo_height_image(int p_size) const;
	Ref<Image> build_normal_roughness_image(int p_size) const;

	LandscapeLayer();
};
