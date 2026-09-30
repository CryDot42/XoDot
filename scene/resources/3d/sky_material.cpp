/**************************************************************************/
/*  sky_material.cpp                                                      */
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

#include "sky_material.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/object/class_db.h"
#include "core/version.h"
#include "scene/resources/texture.h"
#include "servers/rendering/rendering_server.h"

Mutex ProceduralSkyMaterial::shader_mutex;
RID ProceduralSkyMaterial::shader_cache[4];

void ProceduralSkyMaterial::set_sky_top_color(const Color &p_sky_top) {
	sky_top_color = p_sky_top;
	RS::get_singleton()->material_set_param(_get_material(), "sky_top_color", sky_top_color * sky_energy_multiplier);
}

Color ProceduralSkyMaterial::get_sky_top_color() const {
	return sky_top_color;
}

void ProceduralSkyMaterial::set_sky_horizon_color(const Color &p_sky_horizon) {
	sky_horizon_color = p_sky_horizon;
	RS::get_singleton()->material_set_param(_get_material(), "sky_horizon_color", sky_horizon_color * sky_energy_multiplier);
}

Color ProceduralSkyMaterial::get_sky_horizon_color() const {
	return sky_horizon_color;
}

void ProceduralSkyMaterial::set_sky_curve(float p_curve) {
	sky_curve = p_curve;
	// Actual curve passed to shader includes an ad hoc adjustment because the curve used to be
	// in calculated in angles and now uses cosines.
	RS::get_singleton()->material_set_param(_get_material(), "inv_sky_curve", 0.6 / sky_curve);
}

float ProceduralSkyMaterial::get_sky_curve() const {
	return sky_curve;
}

void ProceduralSkyMaterial::set_sky_energy_multiplier(float p_multiplier) {
	sky_energy_multiplier = p_multiplier;
	RS::get_singleton()->material_set_param(_get_material(), "sky_top_color", sky_top_color * sky_energy_multiplier);
	RS::get_singleton()->material_set_param(_get_material(), "sky_horizon_color", sky_horizon_color * sky_energy_multiplier);
	RS::get_singleton()->material_set_param(_get_material(), "sky_cover_modulate", Color(sky_cover_modulate.r, sky_cover_modulate.g, sky_cover_modulate.b, sky_cover_modulate.a * sky_energy_multiplier));
}

float ProceduralSkyMaterial::get_sky_energy_multiplier() const {
	return sky_energy_multiplier;
}

void ProceduralSkyMaterial::set_sky_cover(const Ref<Texture2D> &p_sky_cover) {
	sky_cover = p_sky_cover;

	if (p_sky_cover.is_valid()) {
		RS::get_singleton()->material_set_param(_get_material(), "sky_cover", p_sky_cover->get_rid());
	} else {
		RS::get_singleton()->material_set_param(_get_material(), "sky_cover", Variant());
	}

	_update_shader(use_debanding, sky_cover.is_valid());

	if (shader_set) {
		RS::get_singleton()->material_set_shader(_get_material(), get_shader_cache());
	}
}

Ref<Texture2D> ProceduralSkyMaterial::get_sky_cover() const {
	return sky_cover;
}

void ProceduralSkyMaterial::set_sky_cover_modulate(const Color &p_sky_cover_modulate) {
	sky_cover_modulate = p_sky_cover_modulate;
	RS::get_singleton()->material_set_param(_get_material(), "sky_cover_modulate", Color(sky_cover_modulate.r, sky_cover_modulate.g, sky_cover_modulate.b, sky_cover_modulate.a * sky_energy_multiplier));
}

Color ProceduralSkyMaterial::get_sky_cover_modulate() const {
	return sky_cover_modulate;
}

void ProceduralSkyMaterial::set_ground_bottom_color(const Color &p_ground_bottom) {
	ground_bottom_color = p_ground_bottom;
	RS::get_singleton()->material_set_param(_get_material(), "ground_bottom_color", ground_bottom_color * ground_energy_multiplier);
}

Color ProceduralSkyMaterial::get_ground_bottom_color() const {
	return ground_bottom_color;
}

void ProceduralSkyMaterial::set_ground_horizon_color(const Color &p_ground_horizon) {
	ground_horizon_color = p_ground_horizon;
	RS::get_singleton()->material_set_param(_get_material(), "ground_horizon_color", ground_horizon_color * ground_energy_multiplier);
}

Color ProceduralSkyMaterial::get_ground_horizon_color() const {
	return ground_horizon_color;
}

void ProceduralSkyMaterial::set_ground_curve(float p_curve) {
	ground_curve = p_curve;
	// Actual curve passed to shader includes an ad hoc adjustment because the curve used to be
	// in calculated in angles and now uses cosines.
	RS::get_singleton()->material_set_param(_get_material(), "inv_ground_curve", 0.6 / ground_curve);
}

float ProceduralSkyMaterial::get_ground_curve() const {
	return ground_curve;
}

void ProceduralSkyMaterial::set_ground_energy_multiplier(float p_multiplier) {
	ground_energy_multiplier = p_multiplier;
	RS::get_singleton()->material_set_param(_get_material(), "ground_bottom_color", ground_bottom_color * ground_energy_multiplier);
	RS::get_singleton()->material_set_param(_get_material(), "ground_horizon_color", ground_horizon_color * ground_energy_multiplier);
}

float ProceduralSkyMaterial::get_ground_energy_multiplier() const {
	return ground_energy_multiplier;
}

void ProceduralSkyMaterial::set_sun_angle_max(float p_angle) {
	sun_angle_max = p_angle;
	RS::get_singleton()->material_set_param(_get_material(), "sun_angle_max", Math::cos(Math::deg_to_rad(sun_angle_max)));
}

float ProceduralSkyMaterial::get_sun_angle_max() const {
	return sun_angle_max;
}

void ProceduralSkyMaterial::set_sun_curve(float p_curve) {
	sun_curve = p_curve;
	// Actual curve passed to shader includes an ad hoc adjustment because the curve used to be
	// in calculated in angles and now uses cosines.
	RS::get_singleton()->material_set_param(_get_material(), "inv_sun_curve", 1.6f / Math::pow(sun_curve, 1.4f));
}

float ProceduralSkyMaterial::get_sun_curve() const {
	return sun_curve;
}

void ProceduralSkyMaterial::set_use_debanding(bool p_use_debanding) {
	use_debanding = p_use_debanding;
	_update_shader(use_debanding, sky_cover.is_valid());
	// Only set if shader already compiled
	if (shader_set) {
		RS::get_singleton()->material_set_shader(_get_material(), get_shader_cache());
	}
}

bool ProceduralSkyMaterial::get_use_debanding() const {
	return use_debanding;
}

void ProceduralSkyMaterial::set_energy_multiplier(float p_multiplier) {
	global_energy_multiplier = p_multiplier;
	RS::get_singleton()->material_set_param(_get_material(), "exposure", global_energy_multiplier);
}

float ProceduralSkyMaterial::get_energy_multiplier() const {
	return global_energy_multiplier;
}

Shader::Mode ProceduralSkyMaterial::get_shader_mode() const {
	return Shader::MODE_SKY;
}

// Internal function to grab the current shader RID.
// Must only be called if the shader is initialized.
RID ProceduralSkyMaterial::get_shader_cache() const {
	return shader_cache[int(use_debanding) + (sky_cover.is_valid() ? 2 : 0)];
}

RID ProceduralSkyMaterial::get_rid() const {
	_update_shader(use_debanding, sky_cover.is_valid());
	if (!shader_set) {
		RS::get_singleton()->material_set_shader(_get_material(), get_shader_cache());
		shader_set = true;
	}
	return _get_material();
}

RID ProceduralSkyMaterial::get_shader_rid() const {
	_update_shader(use_debanding, sky_cover.is_valid());
	return get_shader_cache();
}

void ProceduralSkyMaterial::_validate_property(PropertyInfo &p_property) const {
	if (!Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	if ((p_property.name == "sky_luminance" || p_property.name == "ground_luminance") && !GLOBAL_GET_CACHED(bool, "rendering/lights_and_shadows/use_physical_light_units")) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
}

void ProceduralSkyMaterial::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_sky_top_color", "color"), &ProceduralSkyMaterial::set_sky_top_color);
	ClassDB::bind_method(D_METHOD("get_sky_top_color"), &ProceduralSkyMaterial::get_sky_top_color);

	ClassDB::bind_method(D_METHOD("set_sky_horizon_color", "color"), &ProceduralSkyMaterial::set_sky_horizon_color);
	ClassDB::bind_method(D_METHOD("get_sky_horizon_color"), &ProceduralSkyMaterial::get_sky_horizon_color);

	ClassDB::bind_method(D_METHOD("set_sky_curve", "curve"), &ProceduralSkyMaterial::set_sky_curve);
	ClassDB::bind_method(D_METHOD("get_sky_curve"), &ProceduralSkyMaterial::get_sky_curve);

	ClassDB::bind_method(D_METHOD("set_sky_energy_multiplier", "multiplier"), &ProceduralSkyMaterial::set_sky_energy_multiplier);
	ClassDB::bind_method(D_METHOD("get_sky_energy_multiplier"), &ProceduralSkyMaterial::get_sky_energy_multiplier);

	ClassDB::bind_method(D_METHOD("set_sky_cover", "sky_cover"), &ProceduralSkyMaterial::set_sky_cover);
	ClassDB::bind_method(D_METHOD("get_sky_cover"), &ProceduralSkyMaterial::get_sky_cover);

	ClassDB::bind_method(D_METHOD("set_sky_cover_modulate", "color"), &ProceduralSkyMaterial::set_sky_cover_modulate);
	ClassDB::bind_method(D_METHOD("get_sky_cover_modulate"), &ProceduralSkyMaterial::get_sky_cover_modulate);

	ClassDB::bind_method(D_METHOD("set_ground_bottom_color", "color"), &ProceduralSkyMaterial::set_ground_bottom_color);
	ClassDB::bind_method(D_METHOD("get_ground_bottom_color"), &ProceduralSkyMaterial::get_ground_bottom_color);

	ClassDB::bind_method(D_METHOD("set_ground_horizon_color", "color"), &ProceduralSkyMaterial::set_ground_horizon_color);
	ClassDB::bind_method(D_METHOD("get_ground_horizon_color"), &ProceduralSkyMaterial::get_ground_horizon_color);

	ClassDB::bind_method(D_METHOD("set_ground_curve", "curve"), &ProceduralSkyMaterial::set_ground_curve);
	ClassDB::bind_method(D_METHOD("get_ground_curve"), &ProceduralSkyMaterial::get_ground_curve);

	ClassDB::bind_method(D_METHOD("set_ground_energy_multiplier", "energy"), &ProceduralSkyMaterial::set_ground_energy_multiplier);
	ClassDB::bind_method(D_METHOD("get_ground_energy_multiplier"), &ProceduralSkyMaterial::get_ground_energy_multiplier);

	ClassDB::bind_method(D_METHOD("set_sun_angle_max", "degrees"), &ProceduralSkyMaterial::set_sun_angle_max);
	ClassDB::bind_method(D_METHOD("get_sun_angle_max"), &ProceduralSkyMaterial::get_sun_angle_max);

	ClassDB::bind_method(D_METHOD("set_sun_curve", "curve"), &ProceduralSkyMaterial::set_sun_curve);
	ClassDB::bind_method(D_METHOD("get_sun_curve"), &ProceduralSkyMaterial::get_sun_curve);

	ClassDB::bind_method(D_METHOD("set_use_debanding", "use_debanding"), &ProceduralSkyMaterial::set_use_debanding);
	ClassDB::bind_method(D_METHOD("get_use_debanding"), &ProceduralSkyMaterial::get_use_debanding);

	ClassDB::bind_method(D_METHOD("set_energy_multiplier", "multiplier"), &ProceduralSkyMaterial::set_energy_multiplier);
	ClassDB::bind_method(D_METHOD("get_energy_multiplier"), &ProceduralSkyMaterial::get_energy_multiplier);

	ADD_GROUP("Sky", "sky_");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "sky_top_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_sky_top_color", "get_sky_top_color");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "sky_horizon_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_sky_horizon_color", "get_sky_horizon_color");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sky_curve", PROPERTY_HINT_EXP_EASING), "set_sky_curve", "get_sky_curve");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sky_energy_multiplier", PROPERTY_HINT_RANGE, "0,64,0.01"), "set_sky_energy_multiplier", "get_sky_energy_multiplier");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "sky_cover", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_sky_cover", "get_sky_cover");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "sky_cover_modulate"), "set_sky_cover_modulate", "get_sky_cover_modulate");

	ADD_GROUP("Ground", "ground_");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "ground_bottom_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_ground_bottom_color", "get_ground_bottom_color");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "ground_horizon_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_ground_horizon_color", "get_ground_horizon_color");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ground_curve", PROPERTY_HINT_EXP_EASING), "set_ground_curve", "get_ground_curve");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ground_energy_multiplier", PROPERTY_HINT_RANGE, "0,64,0.01"), "set_ground_energy_multiplier", "get_ground_energy_multiplier");

	ADD_GROUP("Sun", "sun_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sun_angle_max", PROPERTY_HINT_RANGE, "0,360,0.01,degrees"), "set_sun_angle_max", "get_sun_angle_max");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sun_curve", PROPERTY_HINT_EXP_EASING), "set_sun_curve", "get_sun_curve");

	ADD_GROUP("", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_debanding"), "set_use_debanding", "get_use_debanding");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "energy_multiplier", PROPERTY_HINT_RANGE, "0,128,0.01"), "set_energy_multiplier", "get_energy_multiplier");
}

void ProceduralSkyMaterial::cleanup_shader() {
	for (int i = 0; i < 4; i++) {
		if (shader_cache[i].is_valid()) {
			RS::get_singleton()->free_rid(shader_cache[i]);
		}
	}
}

void ProceduralSkyMaterial::_update_shader(bool p_use_debanding, bool p_use_sky_cover) {
	MutexLock shader_lock(shader_mutex);
	int index = int(p_use_debanding) + int(p_use_sky_cover) * 2;
	if (shader_cache[index].is_null()) {
		shader_cache[index] = RS::get_singleton()->shader_create();

		// Add a comment to describe the shader origin (useful when converting to ShaderMaterial).
		RS::get_singleton()->shader_set_code(shader_cache[index], vformat(R"(
// NOTE: Shader automatically converted from )" GODOT_VERSION_NAME " " GODOT_VERSION_FULL_CONFIG R"('s ProceduralSkyMaterial.

shader_type sky;
%s

uniform vec4 sky_top_color : source_color = vec4(0.385, 0.454, 0.55, 1.0);
uniform vec4 sky_horizon_color : source_color = vec4(0.646, 0.656, 0.67, 1.0);
uniform float inv_sky_curve : hint_range(1, 100) = 4.0;
uniform vec4 ground_bottom_color : source_color = vec4(0.2, 0.169, 0.133, 1.0);
uniform vec4 ground_horizon_color : source_color = vec4(0.646, 0.656, 0.67, 1.0);
uniform float inv_ground_curve : hint_range(1, 100) = 30.0;
uniform float sun_angle_max = 0.877;
uniform float inv_sun_curve : hint_range(1, 100) = 22.78;
uniform float exposure : hint_range(0, 128) = 1.0;

uniform sampler2D sky_cover : filter_linear, source_color, hint_default_black;
uniform vec4 sky_cover_modulate : source_color = vec4(1.0, 1.0, 1.0, 1.0);

void sky() {
	float v_angle = clamp(EYEDIR.y, -1.0, 1.0);
	vec3 sky = mix(sky_top_color.rgb, sky_horizon_color.rgb, clamp(pow(1.0 - v_angle, inv_sky_curve), 0.0, 1.0));

	if (LIGHT0_ENABLED) {
		float sun_angle = dot(LIGHT0_DIRECTION, EYEDIR);
		float sun_size = cos(LIGHT0_SIZE);
		if (sun_angle > sun_size) {
			sky = LIGHT0_COLOR * LIGHT0_ENERGY;
		} else if (sun_angle > sun_angle_max) {
			float c2 = (sun_size - sun_angle) / (sun_size - sun_angle_max);
			sky = mix(sky, LIGHT0_COLOR * LIGHT0_ENERGY, clamp(pow(1.0 - c2, inv_sun_curve), 0.0, 1.0));
		}
	}

	if (LIGHT1_ENABLED) {
		float sun_angle = dot(LIGHT1_DIRECTION, EYEDIR);
		float sun_size = cos(LIGHT1_SIZE);
		if (sun_angle > sun_size) {
			sky = LIGHT1_COLOR * LIGHT1_ENERGY;
		} else if (sun_angle > sun_angle_max) {
			float c2 = (sun_size - sun_angle) / (sun_size - sun_angle_max);
			sky = mix(sky, LIGHT1_COLOR * LIGHT1_ENERGY, clamp(pow(1.0 - c2, inv_sun_curve), 0.0, 1.0));
		}
	}

	if (LIGHT2_ENABLED) {
		float sun_angle = dot(LIGHT2_DIRECTION, EYEDIR);
		float sun_size = cos(LIGHT2_SIZE);
		if (sun_angle > sun_size) {
			sky = LIGHT2_COLOR * LIGHT2_ENERGY;
		} else if (sun_angle > sun_angle_max) {
			float c2 = (sun_size - sun_angle) / (sun_size - sun_angle_max);
			sky = mix(sky, LIGHT2_COLOR * LIGHT2_ENERGY, clamp(pow(1.0 - c2, inv_sun_curve), 0.0, 1.0));
		}
	}

	if (LIGHT3_ENABLED) {
		float sun_angle = dot(LIGHT3_DIRECTION, EYEDIR);
		float sun_size = cos(LIGHT3_SIZE);
		if (sun_angle > sun_size) {
			sky = LIGHT3_COLOR * LIGHT3_ENERGY;
		} else if (sun_angle > sun_angle_max) {
			float c2 = (sun_size - sun_angle) / (sun_size - sun_angle_max);
			sky = mix(sky, LIGHT3_COLOR * LIGHT3_ENERGY, clamp(pow(1.0 - c2, inv_sun_curve), 0.0, 1.0));
		}
	}

	%s
	%s
	vec3 ground = mix(ground_bottom_color.rgb, ground_horizon_color.rgb, clamp(pow(1.0 + v_angle, inv_ground_curve), 0.0, 1.0));

	COLOR = mix(ground, sky, step(0.0, EYEDIR.y)) * exposure;
}
)",
																		  p_use_debanding ? "render_mode use_debanding;" : "", p_use_sky_cover ? "vec4 sky_cover_texture = texture(sky_cover, SKY_COORDS);" : "", p_use_sky_cover ? "sky += (sky_cover_texture.rgb * sky_cover_modulate.rgb) * sky_cover_texture.a * sky_cover_modulate.a;" : ""));
	}
}

ProceduralSkyMaterial::ProceduralSkyMaterial() {
	_set_material(RS::get_singleton()->material_create());
	set_sky_top_color(Color(0.385, 0.454, 0.55));
	set_sky_horizon_color(Color(0.6463, 0.6558, 0.6708));
	set_sky_curve(0.15);
	set_sky_energy_multiplier(1.0);
	set_sky_cover_modulate(Color(1, 1, 1));

	set_ground_bottom_color(Color(0.2, 0.169, 0.133));
	set_ground_horizon_color(Color(0.6463, 0.6558, 0.6708));
	set_ground_curve(0.02);
	set_ground_energy_multiplier(1.0);

	set_sun_angle_max(30.0);
	set_sun_curve(0.15);
	set_use_debanding(true);
	set_energy_multiplier(1.0);
}

ProceduralSkyMaterial::~ProceduralSkyMaterial() {
}

/////////////////////////////////////////
/* PanoramaSkyMaterial */

void PanoramaSkyMaterial::set_panorama(const Ref<Texture2D> &p_panorama) {
	panorama = p_panorama;
	if (p_panorama.is_valid()) {
		RS::get_singleton()->material_set_param(_get_material(), "source_panorama", p_panorama->get_rid());
	} else {
		RS::get_singleton()->material_set_param(_get_material(), "source_panorama", Variant());
	}
}

Ref<Texture2D> PanoramaSkyMaterial::get_panorama() const {
	return panorama;
}

void PanoramaSkyMaterial::set_filtering_enabled(bool p_enabled) {
	filter = p_enabled;
	notify_property_list_changed();
	_update_shader(filter);
	// Only set if shader already compiled
	if (shader_set) {
		RS::get_singleton()->material_set_shader(_get_material(), shader_cache[int(filter)]);
	}
}

bool PanoramaSkyMaterial::is_filtering_enabled() const {
	return filter;
}

void PanoramaSkyMaterial::set_energy_multiplier(float p_multiplier) {
	energy_multiplier = p_multiplier;
	RS::get_singleton()->material_set_param(_get_material(), "exposure", energy_multiplier);
}

float PanoramaSkyMaterial::get_energy_multiplier() const {
	return energy_multiplier;
}

Shader::Mode PanoramaSkyMaterial::get_shader_mode() const {
	return Shader::MODE_SKY;
}

RID PanoramaSkyMaterial::get_rid() const {
	_update_shader(filter);
	if (!shader_set) {
		RS::get_singleton()->material_set_shader(_get_material(), shader_cache[int(filter)]);
		shader_set = true;
	}
	return _get_material();
}

RID PanoramaSkyMaterial::get_shader_rid() const {
	_update_shader(filter);
	return shader_cache[int(filter)];
}

void PanoramaSkyMaterial::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_panorama", "texture"), &PanoramaSkyMaterial::set_panorama);
	ClassDB::bind_method(D_METHOD("get_panorama"), &PanoramaSkyMaterial::get_panorama);

	ClassDB::bind_method(D_METHOD("set_filtering_enabled", "enabled"), &PanoramaSkyMaterial::set_filtering_enabled);
	ClassDB::bind_method(D_METHOD("is_filtering_enabled"), &PanoramaSkyMaterial::is_filtering_enabled);

	ClassDB::bind_method(D_METHOD("set_energy_multiplier", "multiplier"), &PanoramaSkyMaterial::set_energy_multiplier);
	ClassDB::bind_method(D_METHOD("get_energy_multiplier"), &PanoramaSkyMaterial::get_energy_multiplier);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "panorama", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_panorama", "get_panorama");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "filter"), "set_filtering_enabled", "is_filtering_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "energy_multiplier", PROPERTY_HINT_RANGE, "0,128,0.01"), "set_energy_multiplier", "get_energy_multiplier");
}

Mutex PanoramaSkyMaterial::shader_mutex;
RID PanoramaSkyMaterial::shader_cache[2];

void PanoramaSkyMaterial::cleanup_shader() {
	for (int i = 0; i < 2; i++) {
		if (shader_cache[i].is_valid()) {
			RS::get_singleton()->free_rid(shader_cache[i]);
		}
	}
}

void PanoramaSkyMaterial::_update_shader(bool p_filter) {
	MutexLock shader_lock(shader_mutex);
	int index = int(p_filter);
	if (shader_cache[index].is_null()) {
		shader_cache[index] = RS::get_singleton()->shader_create();

		// Add a comment to describe the shader origin (useful when converting to ShaderMaterial).
		RS::get_singleton()->shader_set_code(shader_cache[index], vformat(R"(
// NOTE: Shader automatically converted from )" GODOT_VERSION_NAME " " GODOT_VERSION_FULL_CONFIG R"('s PanoramaSkyMaterial.

shader_type sky;

uniform sampler2D source_panorama : %s, source_color, hint_default_black;
uniform float exposure : hint_range(0, 128) = 1.0;

void sky() {
	COLOR = texture(source_panorama, SKY_COORDS).rgb * exposure;
}
)",
																		  p_filter ? "filter_linear" : "filter_nearest"));
	}
}

PanoramaSkyMaterial::PanoramaSkyMaterial() {
	_set_material(RS::get_singleton()->material_create());
	set_energy_multiplier(1.0);
}

PanoramaSkyMaterial::~PanoramaSkyMaterial() {
}

//////////////////////////////////
/* PhysicalSkyMaterial */

void PhysicalSkyMaterial::set_rayleigh_coefficient(float p_rayleigh) {
	rayleigh = p_rayleigh;
	RS::get_singleton()->material_set_param(_get_material(), "rayleigh", rayleigh);
}

float PhysicalSkyMaterial::get_rayleigh_coefficient() const {
	return rayleigh;
}

void PhysicalSkyMaterial::set_rayleigh_color(Color p_rayleigh_color) {
	rayleigh_color = p_rayleigh_color;
	RS::get_singleton()->material_set_param(_get_material(), "rayleigh_color", rayleigh_color);
}

Color PhysicalSkyMaterial::get_rayleigh_color() const {
	return rayleigh_color;
}

void PhysicalSkyMaterial::set_mie_coefficient(float p_mie) {
	mie = p_mie;
	RS::get_singleton()->material_set_param(_get_material(), "mie", mie);
}

float PhysicalSkyMaterial::get_mie_coefficient() const {
	return mie;
}

void PhysicalSkyMaterial::set_mie_eccentricity(float p_eccentricity) {
	mie_eccentricity = p_eccentricity;
	RS::get_singleton()->material_set_param(_get_material(), "mie_eccentricity", mie_eccentricity);
}

float PhysicalSkyMaterial::get_mie_eccentricity() const {
	return mie_eccentricity;
}

void PhysicalSkyMaterial::set_mie_color(Color p_mie_color) {
	mie_color = p_mie_color;
	RS::get_singleton()->material_set_param(_get_material(), "mie_color", mie_color);
}

Color PhysicalSkyMaterial::get_mie_color() const {
	return mie_color;
}

void PhysicalSkyMaterial::set_turbidity(float p_turbidity) {
	turbidity = p_turbidity;
	RS::get_singleton()->material_set_param(_get_material(), "turbidity", turbidity);
}

float PhysicalSkyMaterial::get_turbidity() const {
	return turbidity;
}

void PhysicalSkyMaterial::set_sun_disk_scale(float p_sun_disk_scale) {
	sun_disk_scale = p_sun_disk_scale;
	RS::get_singleton()->material_set_param(_get_material(), "sun_disk_scale", sun_disk_scale);
}

float PhysicalSkyMaterial::get_sun_disk_scale() const {
	return sun_disk_scale;
}

void PhysicalSkyMaterial::set_ground_color(Color p_ground_color) {
	ground_color = p_ground_color;
	RS::get_singleton()->material_set_param(_get_material(), "ground_color", ground_color);
}

Color PhysicalSkyMaterial::get_ground_color() const {
	return ground_color;
}

void PhysicalSkyMaterial::set_energy_multiplier(float p_multiplier) {
	energy_multiplier = p_multiplier;
	RS::get_singleton()->material_set_param(_get_material(), "exposure", energy_multiplier);
}

float PhysicalSkyMaterial::get_energy_multiplier() const {
	return energy_multiplier;
}

void PhysicalSkyMaterial::set_use_debanding(bool p_use_debanding) {
	use_debanding = p_use_debanding;
	_update_shader(use_debanding, night_sky.is_valid());
	// Only set if shader already compiled
	if (shader_set) {
		RS::get_singleton()->material_set_shader(_get_material(), get_shader_cache());
	}
}

bool PhysicalSkyMaterial::get_use_debanding() const {
	return use_debanding;
}

void PhysicalSkyMaterial::set_night_sky(const Ref<Texture2D> &p_night_sky) {
	night_sky = p_night_sky;
	if (p_night_sky.is_valid()) {
		RS::get_singleton()->material_set_param(_get_material(), "night_sky", p_night_sky->get_rid());
	} else {
		RS::get_singleton()->material_set_param(_get_material(), "night_sky", Variant());
	}

	_update_shader(use_debanding, night_sky.is_valid());

	if (shader_set) {
		RS::get_singleton()->material_set_shader(_get_material(), get_shader_cache());
	}
}

Ref<Texture2D> PhysicalSkyMaterial::get_night_sky() const {
	return night_sky;
}

Shader::Mode PhysicalSkyMaterial::get_shader_mode() const {
	return Shader::MODE_SKY;
}

// Internal function to grab the current shader RID.
// Must only be called if the shader is initialized.
RID PhysicalSkyMaterial::get_shader_cache() const {
	return shader_cache[int(use_debanding) + (night_sky.is_valid() ? 2 : 0)];
}

RID PhysicalSkyMaterial::get_rid() const {
	_update_shader(use_debanding, night_sky.is_valid());
	if (!shader_set) {
		RS::get_singleton()->material_set_shader(_get_material(), get_shader_cache());
		shader_set = true;
	}
	return _get_material();
}

RID PhysicalSkyMaterial::get_shader_rid() const {
	_update_shader(use_debanding, night_sky.is_valid());
	return get_shader_cache();
}

void PhysicalSkyMaterial::_validate_property(PropertyInfo &p_property) const {
	if (!Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	if (p_property.name == "exposure_value" && !GLOBAL_GET_CACHED(bool, "rendering/lights_and_shadows/use_physical_light_units")) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
}

Mutex PhysicalSkyMaterial::shader_mutex;
RID PhysicalSkyMaterial::shader_cache[4];

void PhysicalSkyMaterial::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_rayleigh_coefficient", "rayleigh"), &PhysicalSkyMaterial::set_rayleigh_coefficient);
	ClassDB::bind_method(D_METHOD("get_rayleigh_coefficient"), &PhysicalSkyMaterial::get_rayleigh_coefficient);

	ClassDB::bind_method(D_METHOD("set_rayleigh_color", "color"), &PhysicalSkyMaterial::set_rayleigh_color);
	ClassDB::bind_method(D_METHOD("get_rayleigh_color"), &PhysicalSkyMaterial::get_rayleigh_color);

	ClassDB::bind_method(D_METHOD("set_mie_coefficient", "mie"), &PhysicalSkyMaterial::set_mie_coefficient);
	ClassDB::bind_method(D_METHOD("get_mie_coefficient"), &PhysicalSkyMaterial::get_mie_coefficient);

	ClassDB::bind_method(D_METHOD("set_mie_eccentricity", "eccentricity"), &PhysicalSkyMaterial::set_mie_eccentricity);
	ClassDB::bind_method(D_METHOD("get_mie_eccentricity"), &PhysicalSkyMaterial::get_mie_eccentricity);

	ClassDB::bind_method(D_METHOD("set_mie_color", "color"), &PhysicalSkyMaterial::set_mie_color);
	ClassDB::bind_method(D_METHOD("get_mie_color"), &PhysicalSkyMaterial::get_mie_color);

	ClassDB::bind_method(D_METHOD("set_turbidity", "turbidity"), &PhysicalSkyMaterial::set_turbidity);
	ClassDB::bind_method(D_METHOD("get_turbidity"), &PhysicalSkyMaterial::get_turbidity);

	ClassDB::bind_method(D_METHOD("set_sun_disk_scale", "scale"), &PhysicalSkyMaterial::set_sun_disk_scale);
	ClassDB::bind_method(D_METHOD("get_sun_disk_scale"), &PhysicalSkyMaterial::get_sun_disk_scale);

	ClassDB::bind_method(D_METHOD("set_ground_color", "color"), &PhysicalSkyMaterial::set_ground_color);
	ClassDB::bind_method(D_METHOD("get_ground_color"), &PhysicalSkyMaterial::get_ground_color);

	ClassDB::bind_method(D_METHOD("set_energy_multiplier", "multiplier"), &PhysicalSkyMaterial::set_energy_multiplier);
	ClassDB::bind_method(D_METHOD("get_energy_multiplier"), &PhysicalSkyMaterial::get_energy_multiplier);

	ClassDB::bind_method(D_METHOD("set_use_debanding", "use_debanding"), &PhysicalSkyMaterial::set_use_debanding);
	ClassDB::bind_method(D_METHOD("get_use_debanding"), &PhysicalSkyMaterial::get_use_debanding);

	ClassDB::bind_method(D_METHOD("set_night_sky", "night_sky"), &PhysicalSkyMaterial::set_night_sky);
	ClassDB::bind_method(D_METHOD("get_night_sky"), &PhysicalSkyMaterial::get_night_sky);

	ADD_GROUP("Rayleigh", "rayleigh_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rayleigh_coefficient", PROPERTY_HINT_RANGE, "0,64,0.01"), "set_rayleigh_coefficient", "get_rayleigh_coefficient");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "rayleigh_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_rayleigh_color", "get_rayleigh_color");

	ADD_GROUP("Mie", "mie_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "mie_coefficient", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_mie_coefficient", "get_mie_coefficient");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "mie_eccentricity", PROPERTY_HINT_RANGE, "-1,1,0.01"), "set_mie_eccentricity", "get_mie_eccentricity");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "mie_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_mie_color", "get_mie_color");

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "turbidity", PROPERTY_HINT_RANGE, "0,1000,0.01"), "set_turbidity", "get_turbidity");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sun_disk_scale", PROPERTY_HINT_RANGE, "0,360,0.01"), "set_sun_disk_scale", "get_sun_disk_scale");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "ground_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_ground_color", "get_ground_color");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "energy_multiplier", PROPERTY_HINT_RANGE, "0,128,0.01"), "set_energy_multiplier", "get_energy_multiplier");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_debanding"), "set_use_debanding", "get_use_debanding");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "night_sky", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_night_sky", "get_night_sky");
}

void PhysicalSkyMaterial::cleanup_shader() {
	for (int i = 0; i < 4; i++) {
		if (shader_cache[i].is_valid()) {
			RS::get_singleton()->free_rid(shader_cache[i]);
		}
	}
}

void PhysicalSkyMaterial::_update_shader(bool p_use_debanding, bool p_use_night_sky) {
	MutexLock shader_lock(shader_mutex);
	int index = int(p_use_debanding) + int(p_use_night_sky) * 2;
	if (shader_cache[index].is_null()) {
		shader_cache[index] = RS::get_singleton()->shader_create();

		// Add a comment to describe the shader origin (useful when converting to ShaderMaterial).
		RS::get_singleton()->shader_set_code(shader_cache[index], vformat(R"(
// NOTE: Shader automatically converted from )" GODOT_VERSION_NAME " " GODOT_VERSION_FULL_CONFIG R"('s PhysicalSkyMaterial.

shader_type sky;
%s

uniform float rayleigh : hint_range(0, 64) = 2.0;
uniform vec4 rayleigh_color : source_color = vec4(0.3, 0.405, 0.6, 1.0);
uniform float mie : hint_range(0, 1) = 0.005;
uniform float mie_eccentricity : hint_range(-1, 1) = 0.8;
uniform vec4 mie_color : source_color = vec4(0.69, 0.729, 0.812, 1.0);

uniform float turbidity : hint_range(0, 1000) = 10.0;
uniform float sun_disk_scale : hint_range(0, 360) = 1.0;
uniform vec4 ground_color : source_color = vec4(0.1, 0.07, 0.034, 1.0);
uniform float exposure : hint_range(0, 128) = 1.0;

uniform sampler2D night_sky : filter_linear, source_color, hint_default_black;

const vec3 UP = vec3( 0.0, 1.0, 0.0 );

// Optical length at zenith for molecules.
const float rayleigh_zenith_size = 8.4e3;
const float mie_zenith_size = 1.25e3;

float henyey_greenstein(float cos_theta, float g) {
	const float k = 0.0795774715459;
	return k * (1.0 - g * g) / (pow(1.0 + g * g - 2.0 * g * cos_theta, 1.5));
}

void sky() {
	if (LIGHT0_ENABLED) {
		float zenith_angle = clamp( dot(UP, normalize(LIGHT0_DIRECTION)), -1.0, 1.0 );
		float sun_energy = max(0.0, 0.757 * zenith_angle) * LIGHT0_ENERGY;
		float sun_fade = 1.0 - clamp(1.0 - exp(LIGHT0_DIRECTION.y), 0.0, 1.0);

		// Rayleigh coefficients.
		float rayleigh_coefficient = rayleigh - ( 1.0 * ( 1.0 - sun_fade ) );
		vec3 rayleigh_beta = rayleigh_coefficient * rayleigh_color.rgb * 0.0001;
		// mie coefficients from Preetham
		vec3 mie_beta = turbidity * mie * mie_color.rgb * 0.000434;

		// Optical length.
		float zenith = max(0.0, dot(UP, EYEDIR));
		float optical_mass = 1.0 / (zenith + 0.15 * pow(3.885 + 54.5 * zenith, -1.253));
		float rayleigh_scatter = rayleigh_zenith_size * optical_mass;
		float mie_scatter = mie_zenith_size * optical_mass;

		// Light extinction based on thickness of atmosphere.
		vec3 extinction = exp(-(rayleigh_beta * rayleigh_scatter + mie_beta * mie_scatter));

		// In scattering.
		float cos_theta = dot(EYEDIR, normalize(LIGHT0_DIRECTION));

		float rayleigh_phase = (3.0 / (16.0 * PI)) * (1.0 + pow(cos_theta * 0.5 + 0.5, 2.0));
		vec3 betaRTheta = rayleigh_beta * rayleigh_phase;

		float mie_phase = henyey_greenstein(cos_theta, mie_eccentricity);
		vec3 betaMTheta = mie_beta * mie_phase;

		vec3 Lin = pow(sun_energy * ((betaRTheta + betaMTheta) / (rayleigh_beta + mie_beta)) * (1.0 - extinction), vec3(1.5));
		// Hack from https://github.com/mrdoob/three.js/blob/master/examples/jsm/objects/Sky.js
		Lin *= mix(vec3(1.0), pow(sun_energy * ((betaRTheta + betaMTheta) / (rayleigh_beta + mie_beta)) * extinction, vec3(0.5)), clamp(pow(1.0 - zenith_angle, 5.0), 0.0, 1.0));

		// Hack in the ground color.
		Lin  *= mix(ground_color.rgb, vec3(1.0), smoothstep(-0.1, 0.1, dot(UP, EYEDIR)));

		// Solar disk and out-scattering.
		float sunAngularDiameterCos = cos(LIGHT0_SIZE * sun_disk_scale);
		float sunAngularDiameterCos2 = cos(LIGHT0_SIZE * sun_disk_scale * 0.5);
		float sundisk = smoothstep(sunAngularDiameterCos, sunAngularDiameterCos2, cos_theta);
		vec3 L0 = (sun_energy * extinction) * sundisk * LIGHT0_COLOR;
		%s

		vec3 color = Lin + L0;
		COLOR = pow(color, vec3(1.0 / (1.2 + (1.2 * sun_fade))));
		COLOR *= exposure;
	} else {
		// There is no sun, so display night_sky and nothing else.
		%s
		COLOR *= exposure;
	}
}
)",
																		  p_use_debanding ? "render_mode use_debanding;" : "", p_use_night_sky ? "L0 += texture(night_sky, SKY_COORDS).xyz * extinction;" : "", p_use_night_sky ? "COLOR = texture(night_sky, SKY_COORDS).xyz;" : ""));
	}
}

PhysicalSkyMaterial::PhysicalSkyMaterial() {
	_set_material(RS::get_singleton()->material_create());
	set_rayleigh_coefficient(2.0);
	set_rayleigh_color(Color(0.3, 0.405, 0.6));
	set_mie_coefficient(0.005);
	set_mie_eccentricity(0.8);
	set_mie_color(Color(0.69, 0.729, 0.812));
	set_turbidity(10.0);
	set_sun_disk_scale(1.0);
	set_ground_color(Color(0.1, 0.07, 0.034));
	set_energy_multiplier(1.0);
	set_use_debanding(true);
}

PhysicalSkyMaterial::~PhysicalSkyMaterial() {
}

/////////////////////////////////////////
/* AtmosphereSkyMaterial */

void AtmosphereSkyMaterial::set_sun_disk_scale(float p_scale) {
	sun_disk_scale = p_scale;
	RS::get_singleton()->material_set_param(_get_material(), "sun_disk_scale", sun_disk_scale);
}

float AtmosphereSkyMaterial::get_sun_disk_scale() const {
	return sun_disk_scale;
}

void AtmosphereSkyMaterial::set_sun_disk_intensity(float p_intensity) {
	sun_disk_intensity = p_intensity;
	RS::get_singleton()->material_set_param(_get_material(), "sun_disk_intensity", sun_disk_intensity);
}

float AtmosphereSkyMaterial::get_sun_disk_intensity() const {
	return sun_disk_intensity;
}

void AtmosphereSkyMaterial::set_night_sky(const Ref<Texture2D> &p_night_sky) {
	night_sky = p_night_sky;
	RS::get_singleton()->material_set_param(_get_material(), "night_sky", night_sky.is_valid() ? Variant(night_sky->get_rid()) : Variant());
}

Ref<Texture2D> AtmosphereSkyMaterial::get_night_sky() const {
	return night_sky;
}

void AtmosphereSkyMaterial::set_night_sky_energy(float p_energy) {
	night_sky_energy = p_energy;
	RS::get_singleton()->material_set_param(_get_material(), "night_sky_energy", night_sky_energy);
}

float AtmosphereSkyMaterial::get_night_sky_energy() const {
	return night_sky_energy;
}

void AtmosphereSkyMaterial::set_energy_multiplier(float p_multiplier) {
	energy_multiplier = p_multiplier;
	RS::get_singleton()->material_set_param(_get_material(), "exposure", energy_multiplier);
}

float AtmosphereSkyMaterial::get_energy_multiplier() const {
	return energy_multiplier;
}

void AtmosphereSkyMaterial::set_use_debanding(bool p_use_debanding) {
	use_debanding = p_use_debanding;
	_update_shader_rid();
}

bool AtmosphereSkyMaterial::get_use_debanding() const {
	return use_debanding;
}

void AtmosphereSkyMaterial::set_clouds_enabled(bool p_enabled) {
	clouds_enabled = p_enabled;
	_update_shader_rid();
}

bool AtmosphereSkyMaterial::is_clouds_enabled() const {
	return clouds_enabled;
}

void AtmosphereSkyMaterial::set_clouds_coverage(float p_coverage) {
	clouds_coverage = p_coverage;
	RS::get_singleton()->material_set_param(_get_material(), "clouds_coverage", clouds_coverage);
}

float AtmosphereSkyMaterial::get_clouds_coverage() const {
	return clouds_coverage;
}

void AtmosphereSkyMaterial::set_clouds_density(float p_density) {
	clouds_density = p_density;
	RS::get_singleton()->material_set_param(_get_material(), "clouds_density", clouds_density);
}

float AtmosphereSkyMaterial::get_clouds_density() const {
	return clouds_density;
}

void AtmosphereSkyMaterial::set_clouds_bottom_altitude(float p_altitude) {
	clouds_bottom_altitude = p_altitude;
	RS::get_singleton()->material_set_param(_get_material(), "clouds_bottom_altitude", clouds_bottom_altitude);
}

float AtmosphereSkyMaterial::get_clouds_bottom_altitude() const {
	return clouds_bottom_altitude;
}

void AtmosphereSkyMaterial::set_clouds_thickness(float p_thickness) {
	clouds_thickness = p_thickness;
	RS::get_singleton()->material_set_param(_get_material(), "clouds_thickness", clouds_thickness);
}

float AtmosphereSkyMaterial::get_clouds_thickness() const {
	return clouds_thickness;
}

void AtmosphereSkyMaterial::set_clouds_extinction(float p_extinction) {
	clouds_extinction = p_extinction;
	RS::get_singleton()->material_set_param(_get_material(), "clouds_extinction", clouds_extinction);
}

float AtmosphereSkyMaterial::get_clouds_extinction() const {
	return clouds_extinction;
}

void AtmosphereSkyMaterial::set_clouds_anisotropy(float p_anisotropy) {
	clouds_anisotropy = p_anisotropy;
	RS::get_singleton()->material_set_param(_get_material(), "clouds_anisotropy", clouds_anisotropy);
}

float AtmosphereSkyMaterial::get_clouds_anisotropy() const {
	return clouds_anisotropy;
}

void AtmosphereSkyMaterial::set_clouds_ambient_strength(float p_strength) {
	clouds_ambient_strength = p_strength;
	RS::get_singleton()->material_set_param(_get_material(), "clouds_ambient_strength", clouds_ambient_strength);
}

float AtmosphereSkyMaterial::get_clouds_ambient_strength() const {
	return clouds_ambient_strength;
}

void AtmosphereSkyMaterial::set_clouds_shape_scale(float p_scale) {
	clouds_shape_scale = p_scale;
	RS::get_singleton()->material_set_param(_get_material(), "clouds_shape_scale", clouds_shape_scale);
}

float AtmosphereSkyMaterial::get_clouds_shape_scale() const {
	return clouds_shape_scale;
}

void AtmosphereSkyMaterial::set_clouds_coverage_scale(float p_scale) {
	clouds_coverage_scale = p_scale;
	RS::get_singleton()->material_set_param(_get_material(), "clouds_coverage_scale", clouds_coverage_scale);
}

float AtmosphereSkyMaterial::get_clouds_coverage_scale() const {
	return clouds_coverage_scale;
}

void AtmosphereSkyMaterial::set_clouds_detail_scale(float p_scale) {
	clouds_detail_scale = p_scale;
	RS::get_singleton()->material_set_param(_get_material(), "clouds_detail_scale", clouds_detail_scale);
}

float AtmosphereSkyMaterial::get_clouds_detail_scale() const {
	return clouds_detail_scale;
}

void AtmosphereSkyMaterial::set_clouds_detail_strength(float p_strength) {
	clouds_detail_strength = p_strength;
	RS::get_singleton()->material_set_param(_get_material(), "clouds_detail_strength", clouds_detail_strength);
}

float AtmosphereSkyMaterial::get_clouds_detail_strength() const {
	return clouds_detail_strength;
}

void AtmosphereSkyMaterial::set_clouds_wind_speed(float p_speed) {
	clouds_wind_speed = p_speed;
	RS::get_singleton()->material_set_param(_get_material(), "clouds_wind_speed", clouds_wind_speed);
}

float AtmosphereSkyMaterial::get_clouds_wind_speed() const {
	return clouds_wind_speed;
}

void AtmosphereSkyMaterial::set_clouds_fade_distance(float p_distance) {
	clouds_fade_distance = p_distance;
	RS::get_singleton()->material_set_param(_get_material(), "clouds_fade_distance", clouds_fade_distance);
}

float AtmosphereSkyMaterial::get_clouds_fade_distance() const {
	return clouds_fade_distance;
}

void AtmosphereSkyMaterial::set_clouds_storm(float p_storm) {
	clouds_storm = p_storm;
	RS::get_singleton()->material_set_param(_get_material(), "clouds_storm", clouds_storm);
}

float AtmosphereSkyMaterial::get_clouds_storm() const {
	return clouds_storm;
}

void AtmosphereSkyMaterial::set_clouds_lightning_intensity(float p_intensity) {
	clouds_lightning_intensity = p_intensity;
	RS::get_singleton()->material_set_param(_get_material(), "clouds_lightning_intensity", clouds_lightning_intensity);
}

float AtmosphereSkyMaterial::get_clouds_lightning_intensity() const {
	return clouds_lightning_intensity;
}

void AtmosphereSkyMaterial::set_clouds_lightning_frequency(float p_frequency) {
	clouds_lightning_frequency = p_frequency;
	RS::get_singleton()->material_set_param(_get_material(), "clouds_lightning_frequency", clouds_lightning_frequency);
}

float AtmosphereSkyMaterial::get_clouds_lightning_frequency() const {
	return clouds_lightning_frequency;
}

void AtmosphereSkyMaterial::set_clouds_shape_texture(const Ref<Texture3D> &p_texture) {
	clouds_shape_texture = p_texture;
	_update_clouds_textures();
}

Ref<Texture3D> AtmosphereSkyMaterial::get_clouds_shape_texture() const {
	return clouds_shape_texture;
}

void AtmosphereSkyMaterial::set_clouds_detail_texture(const Ref<Texture3D> &p_texture) {
	clouds_detail_texture = p_texture;
	_update_clouds_textures();
}

Ref<Texture3D> AtmosphereSkyMaterial::get_clouds_detail_texture() const {
	return clouds_detail_texture;
}

void AtmosphereSkyMaterial::set_clouds_wind_direction(const Vector2 &p_direction) {
	clouds_wind_direction = p_direction;
	RS::get_singleton()->material_set_param(_get_material(), "clouds_wind_direction", clouds_wind_direction);
}

Vector2 AtmosphereSkyMaterial::get_clouds_wind_direction() const {
	return clouds_wind_direction;
}

void AtmosphereSkyMaterial::set_clouds_samples(int p_samples) {
	clouds_samples = MAX(p_samples, 1);
	RS::get_singleton()->material_set_param(_get_material(), "clouds_samples", clouds_samples);
}

int AtmosphereSkyMaterial::get_clouds_samples() const {
	return clouds_samples;
}

void AtmosphereSkyMaterial::set_clouds_light_samples(int p_samples) {
	clouds_light_samples = MAX(p_samples, 0);
	RS::get_singleton()->material_set_param(_get_material(), "clouds_light_samples", clouds_light_samples);
}

int AtmosphereSkyMaterial::get_clouds_light_samples() const {
	return clouds_light_samples;
}

void AtmosphereSkyMaterial::set_clouds_lightning_color(const Color &p_color) {
	clouds_lightning_color = p_color;
	RS::get_singleton()->material_set_param(_get_material(), "clouds_lightning_color", clouds_lightning_color);
}

Color AtmosphereSkyMaterial::get_clouds_lightning_color() const {
	return clouds_lightning_color;
}

void AtmosphereSkyMaterial::_update_clouds_textures() {
	// Unset textures fall back to the shared default noise.
	const Ref<Texture3D> shape = clouds_shape_texture.is_valid() ? clouds_shape_texture : default_clouds_shape_texture;
	const Ref<Texture3D> detail = clouds_detail_texture.is_valid() ? clouds_detail_texture : default_clouds_detail_texture;
	RS::get_singleton()->material_set_param(_get_material(), "clouds_shape_texture", shape.is_valid() ? Variant(shape->get_rid()) : Variant());
	RS::get_singleton()->material_set_param(_get_material(), "clouds_detail_texture", detail.is_valid() ? Variant(detail->get_rid()) : Variant());
}

// Clouds are made of billows: tiling Worley noise, inverted so that cells are
// bright in the middle, over a few octaves. The shape texture holds about
// four cells across, the detail one as many at a much smaller scale. Both are
// built by the noise module (in the background), when it is there; without
// it, there are no default clouds.
static Ref<Texture3D> _make_clouds_noise_texture(int p_size, double p_frequency, int p_octaves, int p_seed) {
	Ref<Resource> noise = Object::cast_to<Resource>(ClassDB::instantiate("FastNoiseLite"));
	Ref<Texture3D> texture = Object::cast_to<Texture3D>(ClassDB::instantiate("NoiseTexture3D"));
	if (noise.is_null() || texture.is_null()) {
		return Ref<Texture3D>();
	}
	noise->set("noise_type", 2); // Cellular.
	noise->set("seed", p_seed);
	noise->set("frequency", p_frequency);
	noise->set("fractal_type", 1); // FBM.
	noise->set("fractal_octaves", p_octaves);
	noise->set("cellular_return_type", 1); // Distance.

	texture->set("width", p_size);
	texture->set("height", p_size);
	texture->set("depth", p_size);
	texture->set("seamless", true);
	texture->set("invert", true);
	texture->set("noise", noise);
	return texture;
}

Ref<Texture3D> AtmosphereSkyMaterial::make_default_clouds_shape_texture() {
	return _make_clouds_noise_texture(128, 4.0 / 128.0, 3, 0);
}

Ref<Texture3D> AtmosphereSkyMaterial::make_default_clouds_detail_texture() {
	return _make_clouds_noise_texture(32, 4.0 / 32.0, 3, 1);
}

void AtmosphereSkyMaterial::_create_default_clouds_textures() {
	MutexLock shader_lock(shader_mutex);
	if (default_clouds_shape_texture.is_null()) {
		default_clouds_shape_texture = make_default_clouds_shape_texture();
	}
	if (default_clouds_detail_texture.is_null()) {
		default_clouds_detail_texture = make_default_clouds_detail_texture();
	}
}

Shader::Mode AtmosphereSkyMaterial::get_shader_mode() const {
	return Shader::MODE_SKY;
}

int AtmosphereSkyMaterial::_get_shader_flags() const {
	return (use_debanding ? SHADER_DEBANDING : 0) | (clouds_enabled ? SHADER_CLOUDS : 0);
}

RID AtmosphereSkyMaterial::get_shader_cache() const {
	return shader_cache[_get_shader_flags()];
}

void AtmosphereSkyMaterial::_update_shader_rid() {
	_update_shader(_get_shader_flags());
	if (shader_set) {
		RS::get_singleton()->material_set_shader(_get_material(), get_shader_cache());
	}
}

RID AtmosphereSkyMaterial::get_rid() const {
	_update_shader(_get_shader_flags());
	if (!shader_set) {
		RS::get_singleton()->material_set_shader(_get_material(), get_shader_cache());
		shader_set = true;
	}
	return _get_material();
}

RID AtmosphereSkyMaterial::get_shader_rid() const {
	_update_shader(_get_shader_flags());
	return get_shader_cache();
}

Mutex AtmosphereSkyMaterial::shader_mutex;
RID AtmosphereSkyMaterial::shader_cache[SHADER_MAX];
Ref<Texture3D> AtmosphereSkyMaterial::default_clouds_shape_texture;
Ref<Texture3D> AtmosphereSkyMaterial::default_clouds_detail_texture;

void AtmosphereSkyMaterial::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_sun_disk_scale", "scale"), &AtmosphereSkyMaterial::set_sun_disk_scale);
	ClassDB::bind_method(D_METHOD("get_sun_disk_scale"), &AtmosphereSkyMaterial::get_sun_disk_scale);
	ClassDB::bind_method(D_METHOD("set_sun_disk_intensity", "intensity"), &AtmosphereSkyMaterial::set_sun_disk_intensity);
	ClassDB::bind_method(D_METHOD("get_sun_disk_intensity"), &AtmosphereSkyMaterial::get_sun_disk_intensity);
	ClassDB::bind_method(D_METHOD("set_night_sky", "night_sky"), &AtmosphereSkyMaterial::set_night_sky);
	ClassDB::bind_method(D_METHOD("get_night_sky"), &AtmosphereSkyMaterial::get_night_sky);
	ClassDB::bind_method(D_METHOD("set_night_sky_energy", "energy"), &AtmosphereSkyMaterial::set_night_sky_energy);
	ClassDB::bind_method(D_METHOD("get_night_sky_energy"), &AtmosphereSkyMaterial::get_night_sky_energy);
	ClassDB::bind_method(D_METHOD("set_energy_multiplier", "multiplier"), &AtmosphereSkyMaterial::set_energy_multiplier);
	ClassDB::bind_method(D_METHOD("get_energy_multiplier"), &AtmosphereSkyMaterial::get_energy_multiplier);
	ClassDB::bind_method(D_METHOD("set_use_debanding", "use_debanding"), &AtmosphereSkyMaterial::set_use_debanding);
	ClassDB::bind_method(D_METHOD("get_use_debanding"), &AtmosphereSkyMaterial::get_use_debanding);
	ClassDB::bind_method(D_METHOD("set_clouds_enabled", "enabled"), &AtmosphereSkyMaterial::set_clouds_enabled);
	ClassDB::bind_method(D_METHOD("is_clouds_enabled"), &AtmosphereSkyMaterial::is_clouds_enabled);
	ClassDB::bind_method(D_METHOD("set_clouds_coverage", "coverage"), &AtmosphereSkyMaterial::set_clouds_coverage);
	ClassDB::bind_method(D_METHOD("get_clouds_coverage"), &AtmosphereSkyMaterial::get_clouds_coverage);
	ClassDB::bind_method(D_METHOD("set_clouds_density", "density"), &AtmosphereSkyMaterial::set_clouds_density);
	ClassDB::bind_method(D_METHOD("get_clouds_density"), &AtmosphereSkyMaterial::get_clouds_density);
	ClassDB::bind_method(D_METHOD("set_clouds_bottom_altitude", "altitude"), &AtmosphereSkyMaterial::set_clouds_bottom_altitude);
	ClassDB::bind_method(D_METHOD("get_clouds_bottom_altitude"), &AtmosphereSkyMaterial::get_clouds_bottom_altitude);
	ClassDB::bind_method(D_METHOD("set_clouds_thickness", "thickness"), &AtmosphereSkyMaterial::set_clouds_thickness);
	ClassDB::bind_method(D_METHOD("get_clouds_thickness"), &AtmosphereSkyMaterial::get_clouds_thickness);
	ClassDB::bind_method(D_METHOD("set_clouds_extinction", "extinction"), &AtmosphereSkyMaterial::set_clouds_extinction);
	ClassDB::bind_method(D_METHOD("get_clouds_extinction"), &AtmosphereSkyMaterial::get_clouds_extinction);
	ClassDB::bind_method(D_METHOD("set_clouds_anisotropy", "anisotropy"), &AtmosphereSkyMaterial::set_clouds_anisotropy);
	ClassDB::bind_method(D_METHOD("get_clouds_anisotropy"), &AtmosphereSkyMaterial::get_clouds_anisotropy);
	ClassDB::bind_method(D_METHOD("set_clouds_ambient_strength", "strength"), &AtmosphereSkyMaterial::set_clouds_ambient_strength);
	ClassDB::bind_method(D_METHOD("get_clouds_ambient_strength"), &AtmosphereSkyMaterial::get_clouds_ambient_strength);
	ClassDB::bind_method(D_METHOD("set_clouds_shape_scale", "scale"), &AtmosphereSkyMaterial::set_clouds_shape_scale);
	ClassDB::bind_method(D_METHOD("get_clouds_shape_scale"), &AtmosphereSkyMaterial::get_clouds_shape_scale);
	ClassDB::bind_method(D_METHOD("set_clouds_coverage_scale", "scale"), &AtmosphereSkyMaterial::set_clouds_coverage_scale);
	ClassDB::bind_method(D_METHOD("get_clouds_coverage_scale"), &AtmosphereSkyMaterial::get_clouds_coverage_scale);
	ClassDB::bind_method(D_METHOD("set_clouds_detail_scale", "scale"), &AtmosphereSkyMaterial::set_clouds_detail_scale);
	ClassDB::bind_method(D_METHOD("get_clouds_detail_scale"), &AtmosphereSkyMaterial::get_clouds_detail_scale);
	ClassDB::bind_method(D_METHOD("set_clouds_detail_strength", "strength"), &AtmosphereSkyMaterial::set_clouds_detail_strength);
	ClassDB::bind_method(D_METHOD("get_clouds_detail_strength"), &AtmosphereSkyMaterial::get_clouds_detail_strength);
	ClassDB::bind_method(D_METHOD("set_clouds_wind_speed", "speed"), &AtmosphereSkyMaterial::set_clouds_wind_speed);
	ClassDB::bind_method(D_METHOD("get_clouds_wind_speed"), &AtmosphereSkyMaterial::get_clouds_wind_speed);
	ClassDB::bind_method(D_METHOD("set_clouds_fade_distance", "distance"), &AtmosphereSkyMaterial::set_clouds_fade_distance);
	ClassDB::bind_method(D_METHOD("get_clouds_fade_distance"), &AtmosphereSkyMaterial::get_clouds_fade_distance);
	ClassDB::bind_method(D_METHOD("set_clouds_storm", "storm"), &AtmosphereSkyMaterial::set_clouds_storm);
	ClassDB::bind_method(D_METHOD("get_clouds_storm"), &AtmosphereSkyMaterial::get_clouds_storm);
	ClassDB::bind_method(D_METHOD("set_clouds_lightning_intensity", "intensity"), &AtmosphereSkyMaterial::set_clouds_lightning_intensity);
	ClassDB::bind_method(D_METHOD("get_clouds_lightning_intensity"), &AtmosphereSkyMaterial::get_clouds_lightning_intensity);
	ClassDB::bind_method(D_METHOD("set_clouds_lightning_frequency", "frequency"), &AtmosphereSkyMaterial::set_clouds_lightning_frequency);
	ClassDB::bind_method(D_METHOD("get_clouds_lightning_frequency"), &AtmosphereSkyMaterial::get_clouds_lightning_frequency);
	ClassDB::bind_method(D_METHOD("set_clouds_shape_texture", "texture"), &AtmosphereSkyMaterial::set_clouds_shape_texture);
	ClassDB::bind_method(D_METHOD("get_clouds_shape_texture"), &AtmosphereSkyMaterial::get_clouds_shape_texture);
	ClassDB::bind_method(D_METHOD("set_clouds_detail_texture", "texture"), &AtmosphereSkyMaterial::set_clouds_detail_texture);
	ClassDB::bind_method(D_METHOD("get_clouds_detail_texture"), &AtmosphereSkyMaterial::get_clouds_detail_texture);
	ClassDB::bind_method(D_METHOD("set_clouds_wind_direction", "direction"), &AtmosphereSkyMaterial::set_clouds_wind_direction);
	ClassDB::bind_method(D_METHOD("get_clouds_wind_direction"), &AtmosphereSkyMaterial::get_clouds_wind_direction);
	ClassDB::bind_method(D_METHOD("set_clouds_samples", "samples"), &AtmosphereSkyMaterial::set_clouds_samples);
	ClassDB::bind_method(D_METHOD("get_clouds_samples"), &AtmosphereSkyMaterial::get_clouds_samples);
	ClassDB::bind_method(D_METHOD("set_clouds_light_samples", "samples"), &AtmosphereSkyMaterial::set_clouds_light_samples);
	ClassDB::bind_method(D_METHOD("get_clouds_light_samples"), &AtmosphereSkyMaterial::get_clouds_light_samples);
	ClassDB::bind_method(D_METHOD("set_clouds_lightning_color", "color"), &AtmosphereSkyMaterial::set_clouds_lightning_color);
	ClassDB::bind_method(D_METHOD("get_clouds_lightning_color"), &AtmosphereSkyMaterial::get_clouds_lightning_color);
	ClassDB::bind_static_method("AtmosphereSkyMaterial", D_METHOD("make_default_clouds_shape_texture"), &AtmosphereSkyMaterial::make_default_clouds_shape_texture);
	ClassDB::bind_static_method("AtmosphereSkyMaterial", D_METHOD("make_default_clouds_detail_texture"), &AtmosphereSkyMaterial::make_default_clouds_detail_texture);

	ADD_GROUP("Sun", "sun_disk_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sun_disk_scale", PROPERTY_HINT_RANGE, "0,20,0.01,or_greater"), "set_sun_disk_scale", "get_sun_disk_scale");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sun_disk_intensity", PROPERTY_HINT_RANGE, "0,4,0.01,or_greater"), "set_sun_disk_intensity", "get_sun_disk_intensity");
	ADD_GROUP("Night Sky", "night_sky");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "night_sky", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_night_sky", "get_night_sky");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "night_sky_energy", PROPERTY_HINT_RANGE, "0,16,0.001,or_greater"), "set_night_sky_energy", "get_night_sky_energy");

	ADD_GROUP("Clouds", "clouds_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "clouds_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_clouds_enabled", "is_clouds_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "clouds_coverage", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_clouds_coverage", "get_clouds_coverage");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "clouds_density", PROPERTY_HINT_RANGE, "0,4,0.001,or_greater"), "set_clouds_density", "get_clouds_density");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "clouds_bottom_altitude", PROPERTY_HINT_RANGE, "0,10000,1,or_greater,suffix:m"), "set_clouds_bottom_altitude", "get_clouds_bottom_altitude");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "clouds_thickness", PROPERTY_HINT_RANGE, "10,10000,1,or_greater,suffix:m"), "set_clouds_thickness", "get_clouds_thickness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "clouds_extinction", PROPERTY_HINT_RANGE, "0,0.5,0.0001,or_greater,suffix:1/m"), "set_clouds_extinction", "get_clouds_extinction");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "clouds_anisotropy", PROPERTY_HINT_RANGE, "0,0.95,0.001"), "set_clouds_anisotropy", "get_clouds_anisotropy");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "clouds_ambient_strength", PROPERTY_HINT_RANGE, "0,4,0.001,or_greater"), "set_clouds_ambient_strength", "get_clouds_ambient_strength");
	ADD_SUBGROUP("Shape", "clouds_");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "clouds_shape_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture3D::get_class_static()), "set_clouds_shape_texture", "get_clouds_shape_texture");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "clouds_shape_scale", PROPERTY_HINT_RANGE, "100,100000,1,or_greater,suffix:m"), "set_clouds_shape_scale", "get_clouds_shape_scale");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "clouds_coverage_scale", PROPERTY_HINT_RANGE, "100,500000,1,or_greater,suffix:m"), "set_clouds_coverage_scale", "get_clouds_coverage_scale");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "clouds_detail_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture3D::get_class_static()), "set_clouds_detail_texture", "get_clouds_detail_texture");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "clouds_detail_scale", PROPERTY_HINT_RANGE, "10,20000,1,or_greater,suffix:m"), "set_clouds_detail_scale", "get_clouds_detail_scale");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "clouds_detail_strength", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_clouds_detail_strength", "get_clouds_detail_strength");
	ADD_SUBGROUP("Wind", "clouds_wind_");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "clouds_wind_direction"), "set_clouds_wind_direction", "get_clouds_wind_direction");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "clouds_wind_speed", PROPERTY_HINT_RANGE, "0,100,0.01,or_greater,suffix:m/s"), "set_clouds_wind_speed", "get_clouds_wind_speed");
	ADD_SUBGROUP("Storm", "clouds_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "clouds_storm", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_clouds_storm", "get_clouds_storm");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "clouds_lightning_intensity", PROPERTY_HINT_RANGE, "0,16,0.01,or_greater"), "set_clouds_lightning_intensity", "get_clouds_lightning_intensity");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "clouds_lightning_frequency", PROPERTY_HINT_RANGE, "0,4,0.001,or_greater,suffix:/s"), "set_clouds_lightning_frequency", "get_clouds_lightning_frequency");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "clouds_lightning_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_clouds_lightning_color", "get_clouds_lightning_color");
	ADD_SUBGROUP("Quality", "clouds_");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "clouds_samples", PROPERTY_HINT_RANGE, "1,256,1"), "set_clouds_samples", "get_clouds_samples");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "clouds_light_samples", PROPERTY_HINT_RANGE, "0,32,1"), "set_clouds_light_samples", "get_clouds_light_samples");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "clouds_fade_distance", PROPERTY_HINT_RANGE, "1000,200000,1,or_greater,suffix:m"), "set_clouds_fade_distance", "get_clouds_fade_distance");
	ADD_GROUP("", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "energy_multiplier", PROPERTY_HINT_RANGE, "0,128,0.01"), "set_energy_multiplier", "get_energy_multiplier");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_debanding"), "set_use_debanding", "get_use_debanding");
}

void AtmosphereSkyMaterial::cleanup_shader() {
	default_clouds_shape_texture.unref();
	default_clouds_detail_texture.unref();
	for (int i = 0; i < SHADER_MAX; i++) {
		if (shader_cache[i].is_valid()) {
			RS::get_singleton()->free_rid(shader_cache[i]);
		}
	}
}

void AtmosphereSkyMaterial::_update_shader(int p_flags) {
	MutexLock shader_lock(shader_mutex);
	if (shader_cache[p_flags].is_valid()) {
		return;
	}
	shader_cache[p_flags] = RS::get_singleton()->shader_create();

	String render_modes;
	if (p_flags & SHADER_DEBANDING) {
		render_modes += "render_mode use_debanding;\n";
	}
	// The clouds are marched at half resolution, then laid over the sky.
	const bool clouds = p_flags & SHADER_CLOUDS;
	if (clouds) {
		render_modes += "render_mode use_half_res_pass;\n";
	}

	static const char *clouds_code = R"(
uniform float clouds_coverage : hint_range(0, 1) = 0.45;
uniform float clouds_density : hint_range(0, 4) = 1.0;
uniform float clouds_bottom_altitude = 1500.0;
uniform float clouds_thickness = 2500.0;
uniform float clouds_extinction = 0.04;
uniform float clouds_anisotropy : hint_range(0, 0.95) = 0.6;
uniform float clouds_ambient_strength = 1.0;
uniform sampler3D clouds_shape_texture : filter_linear, repeat_enable, hint_default_black;
uniform float clouds_shape_scale = 12000.0;
uniform float clouds_coverage_scale = 60000.0;
uniform sampler3D clouds_detail_texture : filter_linear, repeat_enable, hint_default_black;
uniform float clouds_detail_scale = 1800.0;
uniform float clouds_detail_strength : hint_range(0, 1) = 0.35;
uniform vec2 clouds_wind_direction = vec2(1.0, 0.0);
uniform float clouds_wind_speed = 10.0;
uniform float clouds_fade_distance = 40000.0;
uniform int clouds_samples = 48;
uniform int clouds_light_samples = 6;
uniform float clouds_storm : hint_range(0, 1) = 0.0;
uniform float clouds_lightning_intensity = 1.0;
uniform float clouds_lightning_frequency = 0.3;
uniform vec3 clouds_lightning_color : source_color = vec3(0.75, 0.8, 1.0);

// The same planet as the atmosphere's by default, whose ground is at y = 0.
const float CLOUDS_PLANET_RADIUS = 6360000.0;
// The farthest a ray is marched through the layer, past where it enters it.
const float CLOUDS_MAX_MARCH = 30000.0;

// The layer, as the storm changes it: bottom and thickness in meters,
// coverage, density, then where the wind has blown the clouds.
struct CloudsLayer {
	float bottom;
	float thickness;
	float coverage;
	float density;
	float storm;
	vec3 wind;
};

CloudsLayer clouds_layer(float time) {
	CloudsLayer layer;
	layer.storm = clamp(clouds_storm, 0.0, 1.0);
	// Storm clouds hang lower, tower higher, cover the whole sky, and are
	// much denser, so little light makes it through them.
	layer.bottom = clouds_bottom_altitude * mix(1.0, 0.6, layer.storm);
	layer.thickness = max(clouds_thickness * mix(1.0, 2.5, layer.storm), 1.0);
	layer.coverage = mix(clouds_coverage, max(clouds_coverage, 0.95), layer.storm);
	layer.density = clouds_density * mix(1.0, 3.0, layer.storm);
	vec2 wind_dir = dot(clouds_wind_direction, clouds_wind_direction) > 0.0 ? normalize(clouds_wind_direction) : vec2(0.0);
	float speed = clouds_wind_speed * mix(1.0, 2.5, layer.storm);
	layer.wind = vec3(wind_dir.x, 0.0, wind_dir.y) * speed * time;
	return layer;
}

// Altitude above the curved ground of a point of the world.
float clouds_altitude(vec3 p) {
	return length(p + vec3(0.0, CLOUDS_PLANET_RADIUS, 0.0)) - CLOUDS_PLANET_RADIUS;
}

// Where a ray from p_origin (at p_altitude) meets the sphere at p_sphere_altitude:
// both distances, or a negative far one if it misses. The radii are subtracted
// before squaring them, which keeps the precision of a planet this size.
vec2 clouds_sphere(vec3 origin, float altitude, vec3 dir, float sphere_altitude) {
	float r = CLOUDS_PLANET_RADIUS + altitude;
	float b = dot(origin + vec3(0.0, CLOUDS_PLANET_RADIUS, 0.0), dir);
	float c = (altitude - sphere_altitude) * (2.0 * CLOUDS_PLANET_RADIUS + altitude + sphere_altitude);
	float d = b * b - c;
	if (d < 0.0) {
		return vec2(-1.0);
	}
	d = sqrt(d);
	return vec2(-b - d, -b + d);
}

float clouds_remap(float value, float low, float high, float new_low, float new_high) {
	return new_low + (value - low) / max(high - low, 1e-5) * (new_high - new_low);
}

// The density of the clouds at p, h being how far up into the layer it is.
float clouds_density_at(vec3 p, float h, CloudsLayer layer, bool detail) {
	if (h <= 0.0 || h >= 1.0) {
		return 0.0;
	}
	vec3 q = p - layer.wind;

	// Where the sky is cloudy at all: the shape noise, stretched out much
	// wider. The storm evens it out.
	float weather = textureLod(clouds_shape_texture, vec3(q.x, 0.37 * clouds_coverage_scale, q.z) / clouds_coverage_scale, 0.0).r;
	float coverage = clamp(layer.coverage + (weather - 0.5) * 0.6 * (1.0 - layer.storm), 0.0, 1.0);

	// Rounded bottoms, and tops that reach higher where it is cloudier.
	float top = mix(0.35, 1.0, coverage);
	float profile = smoothstep(0.0, 0.08, h) * (1.0 - smoothstep(top * 0.45, top, h));

	float shape = textureLod(clouds_shape_texture, q / clouds_shape_scale, 0.0).r;
	float cloud = clamp(clouds_remap(shape * profile, 1.0 - coverage, 1.0, 0.0, 1.0), 0.0, 1.0) * coverage;
	if (cloud <= 0.0) {
		return 0.0;
	}

	if (detail) {
		// Wisps at the bottom, billows higher up.
		float fine = textureLod(clouds_detail_texture, (q - layer.wind * 0.2) / clouds_detail_scale, 0.0).r;
		float erosion = mix(1.0 - fine, fine, clamp(h * 4.0, 0.0, 1.0)) * clouds_detail_strength * (1.0 - 0.5 * layer.storm);
		cloud = clamp(clouds_remap(cloud, erosion, 1.0, 0.0, 1.0), 0.0, 1.0);
	}
	return cloud * layer.density;
}

float clouds_henyey_greenstein(float cos_theta, float g) {
	float g2 = g * g;
	return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * cos_theta, 1e-4), 1.5));
}

// Mostly forward, with a little back scattering, for the silver lining and
// the bright side alike.
float clouds_phase(float cos_theta, float g) {
	return mix(clouds_henyey_greenstein(cos_theta, g), clouds_henyey_greenstein(cos_theta, -0.3 * g), 0.25);
}

// The optical depth towards a light from p, through the layer.
float clouds_light_depth(vec3 p, vec3 light_dir, CloudsLayer layer, int samples) {
	if (samples <= 0) {
		return 0.0;
	}
	float depth = 0.0;
	float span = layer.thickness * 0.6;
	float t = 0.0;
	for (int i = 0; i < samples; i++) {
		// Steps grow away from p, where the detail matters most.
		float next = span * pow(float(i + 1) / float(samples), 1.6);
		vec3 s = p + light_dir * (0.5 * (t + next));
		float h = (clouds_altitude(s) - layer.bottom) / layer.thickness;
		depth += clouds_density_at(s, h, layer, false) * (next - t);
		t = next;
	}
	return depth * clouds_extinction;
}

// Light scattered towards the eye by a unit of cloud, from one light, several
// orders of it: each order is dimmer, less forward, and gets through more
// cloud than the last.
vec3 clouds_light_scattering(vec3 light, float depth, float cos_theta) {
	vec3 result = vec3(0.0);
	float a = 1.0;
	float b = 1.0;
	float c = 1.0;
	for (int i = 0; i < 3; i++) {
		result += light * a * clouds_phase(cos_theta, clouds_anisotropy * c) * exp(-depth * b);
		a *= 0.5;
		b *= 0.4;
		c *= 0.5;
	}
	return result;
}

float clouds_hash(float n) {
	return fract(sin(n * 12.9898) * 43758.5453);
}

// A lightning flash inside the clouds, if one is lit right now: where, and how
// bright. Each half second of a storm may hold one, flickering as it dies out.
vec4 clouds_lightning(vec3 origin, CloudsLayer layer, float time) {
	float rate = clouds_lightning_frequency * smoothstep(0.5, 1.0, layer.storm);
	if (rate <= 0.0 || clouds_lightning_intensity <= 0.0) {
		return vec4(0.0);
	}
	const float SLOT = 0.5;
	float slot = floor(time / SLOT);
	if (clouds_hash(slot) > rate * SLOT) {
		return vec4(0.0);
	}
	float t = time - slot * SLOT;
	float flash = exp(-t * 14.0) + 0.7 * step(0.12, t) * exp(-(t - 0.12) * 20.0);
	vec2 offset = (vec2(clouds_hash(slot + 17.0), clouds_hash(slot + 43.0)) - 0.5) * 30000.0;
	vec3 position = vec3(origin.x + offset.x, layer.bottom + layer.thickness * 0.25, origin.z + offset.y);
	return vec4(position, flash * clouds_lightning_intensity);
}

// Marches the clouds along a ray: their light, and how much of what is behind
// them still shows through them.
vec4 clouds_march(vec3 origin, vec3 dir, int samples, int light_samples, float jitter, float time) {
	CloudsLayer layer = clouds_layer(time);
	float top = layer.bottom + layer.thickness;
	float altitude = clouds_altitude(origin);

	float t_start;
	float t_end;
	vec2 bottom_hit = clouds_sphere(origin, altitude, dir, layer.bottom);
	vec2 top_hit = clouds_sphere(origin, altitude, dir, top);
	if (altitude < layer.bottom) {
		// From below, unless the ground is in the way.
		vec2 ground_hit = clouds_sphere(origin, altitude, dir, 0.0);
		if (altitude >= 0.0 && ground_hit.x > 0.0) {
			return vec4(0.0, 0.0, 0.0, 1.0);
		}
		t_start = bottom_hit.y;
		t_end = top_hit.y;
	} else if (altitude < top) {
		// From inside.
		t_start = 0.0;
		t_end = bottom_hit.x > 0.0 ? bottom_hit.x : top_hit.y;
	} else {
		// From above.
		if (top_hit.y < 0.0) {
			return vec4(0.0, 0.0, 0.0, 1.0);
		}
		t_start = max(top_hit.x, 0.0);
		t_end = bottom_hit.x > 0.0 ? bottom_hit.x : top_hit.y;
	}
	t_end = min(t_end, t_start + CLOUDS_MAX_MARCH);
	if (t_end <= t_start) {
		return vec4(0.0, 0.0, 0.0, 1.0);
	}

	// Every light the atmosphere knows, as it reaches the layer.
	vec3 entry = origin + dir * t_start;
	vec3 light_dirs[2];
	vec3 lights[2];
	for (int i = 0; i < 2; i++) {
		light_dirs[i] = atmosphere_light_direction(i);
		lights[i] = atmosphere_light_at(entry, i);
	}
	// Light from everywhere else: the sky above, and the lit ground below.
	vec3 sky_light = atmosphere_sky(vec3(0.0, 1.0, 0.0)) * clouds_ambient_strength;
	vec3 ground_light = (lights[0] * max(light_dirs[0].y, 0.0) + lights[1] * max(light_dirs[1].y, 0.0)) * (0.3 / PI) * clouds_ambient_strength;

	vec4 lightning = clouds_lightning(origin, layer, time);
	// A storm's clouds keep less of the light they scatter.
	float albedo = mix(0.99, 0.8, layer.storm);

	float dt = (t_end - t_start) / float(samples);
	float transmittance = 1.0;
	vec3 luminance = vec3(0.0);
	float depth_sum = 0.0;
	float weight_sum = 0.0;
	for (int i = 0; i < samples; i++) {
		float t = t_start + (float(i) + jitter) * dt;
		vec3 p = origin + dir * t;
		float h = (clouds_altitude(p) - layer.bottom) / layer.thickness;
		float density = clouds_density_at(p, h, layer, true);
		if (density <= 0.0) {
			continue;
		}
		float extinction = density * clouds_extinction;

		vec3 scattering = mix(ground_light, sky_light, clamp(h, 0.0, 1.0)) * mix(0.5, 1.0, h);
		for (int j = 0; j < 2; j++) {
			if (dot(lights[j], lights[j]) > 0.0) {
				float depth = clouds_light_depth(p, light_dirs[j], layer, light_samples);
				scattering += clouds_light_scattering(lights[j], depth, dot(dir, light_dirs[j]));
			}
		}
		if (lightning.w > 0.0) {
			vec3 to_flash = (p - lightning.xyz) / 2500.0;
			scattering += clouds_lightning_color * lightning.w * 20.0 * exp(-dot(to_flash, to_flash));
		}
		scattering *= extinction * albedo;

		// Integrated over the step, as in Hillaire's "Physically Based Sky,
		// Atmosphere and Cloud Rendering in Frostbite".
		float step_transmittance = exp(-extinction * dt);
		luminance += transmittance * (scattering - scattering * step_transmittance) / extinction;
		float weight = transmittance * (1.0 - step_transmittance);
		depth_sum += t * weight;
		weight_sum += weight;
		transmittance *= step_transmittance;
		if (transmittance < 0.01) {
			transmittance = 0.0;
			break;
		}
	}

	// Far clouds fade into the air in front of them.
	if (weight_sum > 0.0) {
		float fade = exp(-(depth_sum / weight_sum) / max(clouds_fade_distance, 1.0));
		luminance = luminance * fade + atmosphere_sky(dir) * (1.0 - transmittance) * (1.0 - fade);
	}
	return vec4(luminance, transmittance);
}
)";

	static const char *clouds_pass_code = R"(	if (AT_HALF_RES_PASS) {
		// Reflections make do with fewer samples.
		int samples = AT_CUBEMAP_PASS ? max(clouds_samples / 2, 1) : clouds_samples;
		int light_samples = AT_CUBEMAP_PASS ? min(clouds_light_samples, 3) : clouds_light_samples;
		// Interleaved gradient noise, to trade banding for fine noise.
		float jitter = fract(52.9829189 * fract(dot(FRAGCOORD.xy, vec2(0.06711056, 0.00583715))));
		vec4 clouds = clouds_march(POSITION, EYEDIR, samples, light_samples, jitter, TIME);
		COLOR = clouds.rgb * exposure;
		ALPHA = clouds.a;
	} else
)";

	static const char *clouds_composite_code = R"(		COLOR = COLOR * HALF_RES_COLOR.a + HALF_RES_COLOR.rgb;
)";

	RS::get_singleton()->shader_set_code(shader_cache[p_flags], vformat(R"(
// NOTE: Shader automatically converted from )" GODOT_VERSION_NAME " " GODOT_VERSION_FULL_CONFIG R"('s AtmosphereSkyMaterial.

shader_type sky;
%s
uniform float sun_disk_scale : hint_range(0, 20) = 1.0;
uniform float sun_disk_intensity : hint_range(0, 4) = 1.0;
uniform sampler2D night_sky : filter_linear, source_color, hint_default_black;
uniform float night_sky_energy : hint_range(0, 16) = 1.0;
uniform float exposure : hint_range(0, 128) = 1.0;

// The sun's angular radius, for a light that doesn't set its own size.
const float SUN_ANGULAR_RADIUS = 0.004651;

// A light's disk: its illuminance spread over the solid angle it covers, so
// that it is as bright as the light it casts, darkening towards its limb.
vec3 light_disk(vec3 eyedir, vec3 direction, vec3 color, float energy, float angular_diameter) {
	float radius = max(angular_diameter * 0.5, SUN_ANGULAR_RADIUS) * sun_disk_scale;
	float cos_angle = dot(eyedir, normalize(direction));
	float cos_radius = cos(radius);
	if (radius <= 0.0 || cos_angle < cos_radius) {
		return vec3(0.0);
	}
	float r = clamp(acos(clamp(cos_angle, -1.0, 1.0)) / radius, 0.0, 1.0);
	float limb = 1.0 - 0.6 * (1.0 - sqrt(max(0.0, 1.0 - r * r)));
	float solid_angle = 2.0 * PI * (1.0 - cos_radius);
	return color * energy / max(solid_angle, 1e-7) * limb * sun_disk_intensity;
}

%s
void sky() {
%s	{
		vec3 transmittance = atmosphere_transmittance(EYEDIR);
		vec3 color = atmosphere_sky(EYEDIR);

		// Reflections get the lights' own specular highlights already.
		if (!AT_CUBEMAP_PASS) {
			vec3 disks = vec3(0.0);
			if (LIGHT0_ENABLED) {
				disks += light_disk(EYEDIR, LIGHT0_DIRECTION, LIGHT0_COLOR, LIGHT0_ENERGY, LIGHT0_SIZE);
			}
			if (LIGHT1_ENABLED) {
				disks += light_disk(EYEDIR, LIGHT1_DIRECTION, LIGHT1_COLOR, LIGHT1_ENERGY, LIGHT1_SIZE);
			}
			color += disks * transmittance;
		}

		color += texture(night_sky, SKY_COORDS).rgb * night_sky_energy * transmittance;

		COLOR = color * exposure;
%s	}
}
)",
																		render_modes, clouds ? clouds_code : "", clouds ? clouds_pass_code : "", clouds ? clouds_composite_code : ""));
}

AtmosphereSkyMaterial::AtmosphereSkyMaterial() {
	_set_material(RS::get_singleton()->material_create());
	set_sun_disk_scale(1.0);
	set_sun_disk_intensity(1.0);
	set_night_sky_energy(1.0);
	set_energy_multiplier(1.0);
	set_use_debanding(true);

	_create_default_clouds_textures();
	set_clouds_coverage(clouds_coverage);
	set_clouds_density(clouds_density);
	set_clouds_bottom_altitude(clouds_bottom_altitude);
	set_clouds_thickness(clouds_thickness);
	set_clouds_extinction(clouds_extinction);
	set_clouds_anisotropy(clouds_anisotropy);
	set_clouds_ambient_strength(clouds_ambient_strength);
	set_clouds_shape_scale(clouds_shape_scale);
	set_clouds_coverage_scale(clouds_coverage_scale);
	set_clouds_detail_scale(clouds_detail_scale);
	set_clouds_detail_strength(clouds_detail_strength);
	set_clouds_wind_direction(clouds_wind_direction);
	set_clouds_wind_speed(clouds_wind_speed);
	set_clouds_fade_distance(clouds_fade_distance);
	set_clouds_samples(clouds_samples);
	set_clouds_light_samples(clouds_light_samples);
	set_clouds_storm(clouds_storm);
	set_clouds_lightning_intensity(clouds_lightning_intensity);
	set_clouds_lightning_frequency(clouds_lightning_frequency);
	set_clouds_lightning_color(clouds_lightning_color);
	_update_clouds_textures();
}
