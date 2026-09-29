/**************************************************************************/
/*  landscape_layer.cpp                                                   */
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

#include "landscape_layer.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"

static Ref<Image> _landscape_prepare_image(const Ref<Texture2D> &p_texture, int p_size) {
	if (p_texture.is_null()) {
		return Ref<Image>();
	}
	Ref<Image> image = p_texture->get_image();
	if (image.is_null() || image->is_empty()) {
		return Ref<Image>();
	}
	image = image->duplicate();
	if (image->is_compressed()) {
		if (image->decompress() != OK) {
			return Ref<Image>();
		}
	}
	image->clear_mipmaps();
	if (image->get_format() != Image::FORMAT_RGBA8) {
		image->convert(Image::FORMAT_RGBA8);
	}
	if (image->get_width() != p_size || image->get_height() != p_size) {
		image->resize(p_size, p_size, Image::INTERPOLATE_CUBIC);
	}
	return image;
}

void LandscapeLayer::_texture_changed() {
	emit_signal(SNAME("textures_changed"));
}

void LandscapeLayer::_set_texture(Ref<Texture2D> &r_texture, const Ref<Texture2D> &p_texture) {
	if (r_texture == p_texture) {
		return;
	}
	const Callable callback = callable_mp(this, &LandscapeLayer::_texture_changed);
	if (r_texture.is_valid() && r_texture->is_connected(CoreStringName(changed), callback)) {
		r_texture->disconnect(CoreStringName(changed), callback);
	}
	r_texture = p_texture;
	if (r_texture.is_valid()) {
		r_texture->connect(CoreStringName(changed), callback);
	}
	_texture_changed();
	emit_changed();
}

void LandscapeLayer::set_layer_name(const String &p_name) {
	layer_name = p_name;
	emit_changed();
}

void LandscapeLayer::set_albedo_texture(const Ref<Texture2D> &p_texture) {
	_set_texture(albedo_texture, p_texture);
}

void LandscapeLayer::set_albedo_color(const Color &p_color) {
	albedo_color = p_color;
	emit_changed();
}

void LandscapeLayer::set_normal_texture(const Ref<Texture2D> &p_texture) {
	_set_texture(normal_texture, p_texture);
}

void LandscapeLayer::set_normal_strength(float p_strength) {
	normal_strength = p_strength;
	emit_changed();
}

void LandscapeLayer::set_roughness_texture(const Ref<Texture2D> &p_texture) {
	_set_texture(roughness_texture, p_texture);
}

void LandscapeLayer::set_roughness(float p_roughness) {
	roughness = p_roughness;
	emit_changed();
}

void LandscapeLayer::set_metallic(float p_metallic) {
	metallic = p_metallic;
	emit_changed();
}

void LandscapeLayer::set_ao_texture(const Ref<Texture2D> &p_texture) {
	_set_texture(ao_texture, p_texture);
}

void LandscapeLayer::set_ao_strength(float p_strength) {
	ao_strength = p_strength;
	emit_changed();
}

void LandscapeLayer::set_height_texture(const Ref<Texture2D> &p_texture) {
	_set_texture(height_texture, p_texture);
}

void LandscapeLayer::set_tile_size(float p_size) {
	tile_size = MAX(p_size, 0.001f);
	emit_changed();
}

void LandscapeLayer::set_uv_rotation(float p_radians) {
	uv_rotation = p_radians;
	emit_changed();
}

void LandscapeLayer::set_triplanar(bool p_enable) {
	triplanar = p_enable;
	emit_changed();
}

void LandscapeLayer::set_triplanar_sharpness(float p_sharpness) {
	triplanar_sharpness = p_sharpness;
	emit_changed();
}

void LandscapeLayer::set_height_blend(float p_blend) {
	height_blend = CLAMP(p_blend, 0.0f, 1.0f);
	emit_changed();
}

void LandscapeLayer::set_displacement(float p_displacement) {
	displacement = MAX(p_displacement, 0.0f);
	emit_changed();
}

Ref<Image> LandscapeLayer::build_albedo_height_image(int p_size) const {
	Ref<Image> albedo = _landscape_prepare_image(albedo_texture, p_size);
	Ref<Image> height = _landscape_prepare_image(height_texture, p_size);
	const Vector<uint8_t> albedo_data = albedo.is_valid() ? albedo->get_data() : Vector<uint8_t>();
	const Vector<uint8_t> height_data = height.is_valid() ? height->get_data() : Vector<uint8_t>();
	const uint8_t *a = albedo_data.is_empty() ? nullptr : albedo_data.ptr();
	const uint8_t *h = height_data.is_empty() ? nullptr : height_data.ptr();

	Ref<Image> result = Image::create_empty(p_size, p_size, false, Image::FORMAT_RGBA8);
	Vector<uint8_t> data = result->get_data();
	uint8_t *w = data.ptrw();
	const int64_t count = int64_t(p_size) * p_size;
	for (int64_t i = 0; i < count; i++) {
		w[i * 4 + 0] = a ? a[i * 4 + 0] : 255;
		w[i * 4 + 1] = a ? a[i * 4 + 1] : 255;
		w[i * 4 + 2] = a ? a[i * 4 + 2] : 255;
		w[i * 4 + 3] = h ? h[i * 4 + 0] : 128;
	}
	result->set_data(p_size, p_size, false, Image::FORMAT_RGBA8, data);
	result->generate_mipmaps();
	return result;
}

Ref<Image> LandscapeLayer::build_normal_roughness_image(int p_size) const {
	Ref<Image> normal = _landscape_prepare_image(normal_texture, p_size);
	Ref<Image> rough = _landscape_prepare_image(roughness_texture, p_size);
	Ref<Image> ao = _landscape_prepare_image(ao_texture, p_size);
	const Vector<uint8_t> normal_data = normal.is_valid() ? normal->get_data() : Vector<uint8_t>();
	const Vector<uint8_t> rough_data = rough.is_valid() ? rough->get_data() : Vector<uint8_t>();
	const Vector<uint8_t> ao_data = ao.is_valid() ? ao->get_data() : Vector<uint8_t>();
	const uint8_t *n = normal_data.is_empty() ? nullptr : normal_data.ptr();
	const uint8_t *r = rough_data.is_empty() ? nullptr : rough_data.ptr();
	const uint8_t *o = ao_data.is_empty() ? nullptr : ao_data.ptr();

	Ref<Image> result = Image::create_empty(p_size, p_size, false, Image::FORMAT_RGBA8);
	Vector<uint8_t> data = result->get_data();
	uint8_t *w = data.ptrw();
	const int64_t count = int64_t(p_size) * p_size;
	for (int64_t i = 0; i < count; i++) {
		w[i * 4 + 0] = n ? n[i * 4 + 0] : 128;
		w[i * 4 + 1] = n ? n[i * 4 + 1] : 128;
		w[i * 4 + 2] = r ? r[i * 4 + 0] : 255;
		w[i * 4 + 3] = o ? o[i * 4 + 0] : 255;
	}
	result->set_data(p_size, p_size, false, Image::FORMAT_RGBA8, data);
	result->generate_mipmaps();
	return result;
}

void LandscapeLayer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_layer_name", "name"), &LandscapeLayer::set_layer_name);
	ClassDB::bind_method(D_METHOD("get_layer_name"), &LandscapeLayer::get_layer_name);
	ClassDB::bind_method(D_METHOD("set_albedo_texture", "texture"), &LandscapeLayer::set_albedo_texture);
	ClassDB::bind_method(D_METHOD("get_albedo_texture"), &LandscapeLayer::get_albedo_texture);
	ClassDB::bind_method(D_METHOD("set_albedo_color", "color"), &LandscapeLayer::set_albedo_color);
	ClassDB::bind_method(D_METHOD("get_albedo_color"), &LandscapeLayer::get_albedo_color);
	ClassDB::bind_method(D_METHOD("set_normal_texture", "texture"), &LandscapeLayer::set_normal_texture);
	ClassDB::bind_method(D_METHOD("get_normal_texture"), &LandscapeLayer::get_normal_texture);
	ClassDB::bind_method(D_METHOD("set_normal_strength", "strength"), &LandscapeLayer::set_normal_strength);
	ClassDB::bind_method(D_METHOD("get_normal_strength"), &LandscapeLayer::get_normal_strength);
	ClassDB::bind_method(D_METHOD("set_roughness_texture", "texture"), &LandscapeLayer::set_roughness_texture);
	ClassDB::bind_method(D_METHOD("get_roughness_texture"), &LandscapeLayer::get_roughness_texture);
	ClassDB::bind_method(D_METHOD("set_roughness", "roughness"), &LandscapeLayer::set_roughness);
	ClassDB::bind_method(D_METHOD("get_roughness"), &LandscapeLayer::get_roughness);
	ClassDB::bind_method(D_METHOD("set_metallic", "metallic"), &LandscapeLayer::set_metallic);
	ClassDB::bind_method(D_METHOD("get_metallic"), &LandscapeLayer::get_metallic);
	ClassDB::bind_method(D_METHOD("set_ao_texture", "texture"), &LandscapeLayer::set_ao_texture);
	ClassDB::bind_method(D_METHOD("get_ao_texture"), &LandscapeLayer::get_ao_texture);
	ClassDB::bind_method(D_METHOD("set_ao_strength", "strength"), &LandscapeLayer::set_ao_strength);
	ClassDB::bind_method(D_METHOD("get_ao_strength"), &LandscapeLayer::get_ao_strength);
	ClassDB::bind_method(D_METHOD("set_height_texture", "texture"), &LandscapeLayer::set_height_texture);
	ClassDB::bind_method(D_METHOD("get_height_texture"), &LandscapeLayer::get_height_texture);
	ClassDB::bind_method(D_METHOD("set_tile_size", "size"), &LandscapeLayer::set_tile_size);
	ClassDB::bind_method(D_METHOD("get_tile_size"), &LandscapeLayer::get_tile_size);
	ClassDB::bind_method(D_METHOD("set_uv_rotation", "radians"), &LandscapeLayer::set_uv_rotation);
	ClassDB::bind_method(D_METHOD("get_uv_rotation"), &LandscapeLayer::get_uv_rotation);
	ClassDB::bind_method(D_METHOD("set_triplanar", "enable"), &LandscapeLayer::set_triplanar);
	ClassDB::bind_method(D_METHOD("is_triplanar"), &LandscapeLayer::is_triplanar);
	ClassDB::bind_method(D_METHOD("set_triplanar_sharpness", "sharpness"), &LandscapeLayer::set_triplanar_sharpness);
	ClassDB::bind_method(D_METHOD("get_triplanar_sharpness"), &LandscapeLayer::get_triplanar_sharpness);
	ClassDB::bind_method(D_METHOD("set_height_blend", "blend"), &LandscapeLayer::set_height_blend);
	ClassDB::bind_method(D_METHOD("get_height_blend"), &LandscapeLayer::get_height_blend);
	ClassDB::bind_method(D_METHOD("set_displacement", "displacement"), &LandscapeLayer::set_displacement);
	ClassDB::bind_method(D_METHOD("get_displacement"), &LandscapeLayer::get_displacement);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "layer_name"), "set_layer_name", "get_layer_name");

	ADD_GROUP("Albedo", "albedo_");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "albedo_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_albedo_texture", "get_albedo_texture");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "albedo_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_albedo_color", "get_albedo_color");

	ADD_GROUP("Normal", "normal_");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "normal_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_normal_texture", "get_normal_texture");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "normal_strength", PROPERTY_HINT_RANGE, "0,2,0.01"), "set_normal_strength", "get_normal_strength");

	ADD_GROUP("Surface", "");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "roughness_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_roughness_texture", "get_roughness_texture");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "roughness", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_roughness", "get_roughness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "metallic", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_metallic", "get_metallic");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "ao_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_ao_texture", "get_ao_texture");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ao_strength", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_ao_strength", "get_ao_strength");

	ADD_GROUP("Mapping", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tile_size", PROPERTY_HINT_RANGE, "0.01,1000,0.01,or_greater,suffix:m"), "set_tile_size", "get_tile_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "uv_rotation", PROPERTY_HINT_RANGE, "-180,180,0.1,radians_as_degrees"), "set_uv_rotation", "get_uv_rotation");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "triplanar"), "set_triplanar", "is_triplanar");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "triplanar_sharpness", PROPERTY_HINT_RANGE, "1,16,0.1"), "set_triplanar_sharpness", "get_triplanar_sharpness");

	ADD_GROUP("Height", "");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "height_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_height_texture", "get_height_texture");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "height_blend", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_height_blend", "get_height_blend");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "displacement", PROPERTY_HINT_RANGE, "0,4,0.001,or_greater,suffix:m"), "set_displacement", "get_displacement");

	ADD_SIGNAL(MethodInfo("textures_changed"));
}

LandscapeLayer::LandscapeLayer() {
}
