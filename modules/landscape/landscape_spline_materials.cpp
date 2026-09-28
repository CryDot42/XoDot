/**************************************************************************/
/*  landscape_spline_materials.cpp                                        */
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

#include "landscape_spline_materials.h"

#include "core/math/random_pcg.h"
#include "core/object/class_db.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/shader_preprocessor.h"

/* LandscapeSplineMaterial */

void LandscapeSplineMaterial::_init_parameters() {
	const int count = _get_parameter_count();
	parameters.resize(count);
	for (int i = 0; i < count; i++) {
		parameters.write[i] = _get_parameter_default(i);
		_apply_parameter(i);
	}
}

void LandscapeSplineMaterial::_apply_parameter(int p_index) {
	const ParameterInfo &info = _get_parameter_infos()[p_index];
	Variant value = parameters[p_index];
	if (info.type == Variant::OBJECT) {
		const Ref<Texture2D> texture = value;
		if (texture.is_valid()) {
			value = texture->get_rid();
		} else {
			// Null textures use the built-in texture, or the default of the uniform hint.
			const RID fallback = _get_default_texture(p_index);
			value = fallback.is_valid() ? Variant(fallback) : Variant();
		}
	}
	RenderingServer::get_singleton()->material_set_param(_get_material(), info.name, value);
}

void LandscapeSplineMaterial::set_parameter_by_index(int p_index, const Variant &p_value) {
	ERR_FAIL_INDEX(p_index, parameters.size());
	const ParameterInfo &info = _get_parameter_infos()[p_index];
	Variant value = p_value;
	if (info.type != Variant::OBJECT && value.get_type() != info.type && Variant::can_convert(value.get_type(), info.type)) {
		Callable::CallError ce;
		const Variant *args[1] = { &p_value };
		Variant::construct(info.type, value, args, 1, ce);
	}
	parameters.write[p_index] = value;
	_apply_parameter(p_index);
	emit_changed();
}

Variant LandscapeSplineMaterial::get_parameter_by_index(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, parameters.size(), Variant());
	return parameters[p_index];
}

int LandscapeSplineMaterial::find_parameter(const StringName &p_name) const {
	const ParameterInfo *infos = _get_parameter_infos();
	for (int i = 0; i < _get_parameter_count(); i++) {
		if (p_name == StringName(infos[i].name)) {
			return i;
		}
	}
	return -1;
}

String LandscapeSplineMaterial::get_parameter_name(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, _get_parameter_count(), String());
	return _get_parameter_infos()[p_index].name;
}

RID LandscapeSplineMaterial::_create_shader(const String &p_code) {
	// Like Shader::set_code(): the rendering server expects preprocessed code (CURRENT_RENDERER).
	String code;
	ShaderPreprocessor preprocessor;
	const Error err = preprocessor.preprocess(p_code, String(), code);
	ERR_FAIL_COND_V_MSG(err != OK, RID(), "Failed to preprocess the built-in landscape spline shader.");
	const RID shader_rid = RenderingServer::get_singleton()->shader_create();
	RenderingServer::get_singleton()->shader_set_code(shader_rid, code);
	return shader_rid;
}

RID LandscapeSplineMaterial::get_rid() const {
	if (!shader_set) {
		const RID shader_rid = _get_shader();
		if (shader_rid.is_valid()) {
			RenderingServer::get_singleton()->material_set_shader(_get_material(), shader_rid);
			shader_set = true;
		}
	}
	return _get_material();
}

String LandscapeSplineMaterial::get_shader_code() const {
	return _get_source_code(); // Before preprocessing: valid for every renderer.
}

void LandscapeSplineMaterial::_bind_parameters(const StringName &p_class, const ParameterInfo *p_infos, int p_count) {
	const char *group = nullptr;
	for (int i = 0; i < p_count; i++) {
		const ParameterInfo &info = p_infos[i];
		if (info.group && (!group || strcmp(group, info.group) != 0)) {
			group = info.group;
			ClassDB::add_property_group(p_class, group, "");
		}
		StringName getter;
		switch (info.type) {
			case Variant::BOOL:
				getter = "_get_parameter_bool";
				break;
			case Variant::INT:
				getter = "_get_parameter_int";
				break;
			case Variant::FLOAT:
				getter = "_get_parameter_float";
				break;
			case Variant::VECTOR2:
				getter = "_get_parameter_vector2";
				break;
			case Variant::COLOR:
				getter = "_get_parameter_color";
				break;
			case Variant::OBJECT:
				getter = "_get_parameter_texture";
				break;
			default:
				ERR_FAIL_MSG(vformat("Unsupported type of the parameter \"%s\".", info.name));
		}
		ClassDB::add_property(p_class, PropertyInfo(info.type, info.name, info.hint, info.hint_string), "_set_parameter", getter, i);
	}
}

void LandscapeSplineMaterial::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_set_parameter", "index", "value"), &LandscapeSplineMaterial::set_parameter_by_index);
	ClassDB::bind_method(D_METHOD("_get_parameter_bool", "index"), &LandscapeSplineMaterial::_get_parameter_bool);
	ClassDB::bind_method(D_METHOD("_get_parameter_int", "index"), &LandscapeSplineMaterial::_get_parameter_int);
	ClassDB::bind_method(D_METHOD("_get_parameter_float", "index"), &LandscapeSplineMaterial::_get_parameter_float);
	ClassDB::bind_method(D_METHOD("_get_parameter_vector2", "index"), &LandscapeSplineMaterial::_get_parameter_vector2);
	ClassDB::bind_method(D_METHOD("_get_parameter_color", "index"), &LandscapeSplineMaterial::_get_parameter_color);
	ClassDB::bind_method(D_METHOD("_get_parameter_texture", "index"), &LandscapeSplineMaterial::_get_parameter_texture);
	ClassDB::bind_method(D_METHOD("get_shader_code"), &LandscapeSplineMaterial::get_shader_code);
}

void LandscapeSplineMaterial::cleanup_shared_resources() {
	LandscapeWaterMaterial::cleanup_shared_resources();
	LandscapeRoadMaterial::cleanup_shared_resources();
}

LandscapeSplineMaterial::LandscapeSplineMaterial() {
	_set_material(RenderingServer::get_singleton()->material_create());
}

LandscapeSplineMaterial::~LandscapeSplineMaterial() {
	RenderingServer::get_singleton()->material_set_shader(_get_material(), RID());
}

/* Built-in textures */

// Tileable ripple normals: a sum of waves with integer frequencies and random directions.
static Ref<Image> _landscape_make_ripple_normals(int p_size) {
	struct Wave {
		real_t kx;
		real_t ky;
		real_t amplitude;
		real_t phase;
	};
	LocalVector<Wave> waves;
	RandomPCG rng(0x5eed1234);
	for (int i = 0; i < 48; i++) {
		const real_t angle = rng.randf() * Math::TAU;
		const real_t magnitude = 3.0 + Math::pow(real_t(rng.randf()), real_t(1.6)) * 21.0;
		const int kx = int(Math::round(Math::cos(angle) * magnitude));
		const int ky = int(Math::round(Math::sin(angle) * magnitude));
		if (kx == 0 && ky == 0) {
			continue;
		}
		const real_t k = Math::sqrt(real_t(kx * kx + ky * ky));
		waves.push_back({ real_t(kx), real_t(ky), real_t(1.0) / Math::pow(k, real_t(1.4)), rng.randf() * real_t(Math::TAU) });
	}
	const int count = p_size * p_size;
	LocalVector<Vector2> slopes;
	slopes.resize(count);
	real_t sum = 0.0;
	for (int y = 0; y < p_size; y++) {
		for (int x = 0; x < p_size; x++) {
			const real_t u = real_t(x) / p_size;
			const real_t v = real_t(y) / p_size;
			Vector2 slope;
			for (const Wave &w : waves) {
				const real_t c = Math::cos(real_t(Math::TAU) * (w.kx * u + w.ky * v) + w.phase) * w.amplitude * real_t(Math::TAU);
				slope += Vector2(w.kx * c, w.ky * c);
			}
			slopes[y * p_size + x] = slope;
			sum += slope.length_squared();
		}
	}
	const real_t scale = 0.4 / MAX(Math::sqrt(sum / count), real_t(1e-6));
	Vector<uint8_t> data;
	data.resize(count * 4);
	uint8_t *w = data.ptrw();
	for (int i = 0; i < count; i++) {
		const Vector3 n = Vector3(-slopes[i].x * scale, -slopes[i].y * scale, 1.0).normalized();
		w[i * 4 + 0] = uint8_t(CLAMP(Math::round((n.x * 0.5 + 0.5) * 255.0), 0.0, 255.0));
		w[i * 4 + 1] = uint8_t(CLAMP(Math::round((n.y * 0.5 + 0.5) * 255.0), 0.0, 255.0));
		w[i * 4 + 2] = uint8_t(CLAMP(Math::round((n.z * 0.5 + 0.5) * 255.0), 0.0, 255.0));
		w[i * 4 + 3] = 255;
	}
	Ref<Image> image = Image::create_from_data(p_size, p_size, false, Image::FORMAT_RGBA8, data);
	image->generate_mipmaps(true);
	return image;
}

// Tileable foam: cellular noise (bright cell borders, like a foam web) with small bubbles.
static Ref<Image> _landscape_make_foam(int p_size) {
	RandomPCG rng(0xf0a3);
	auto layer = [&](int p_cells, Vector<real_t> &r_values) {
		LocalVector<Vector2> features;
		features.resize(p_cells * p_cells);
		for (Vector2 &f : features) {
			f = Vector2(rng.randf(), rng.randf());
		}
		r_values.resize(p_size * p_size);
		for (int y = 0; y < p_size; y++) {
			for (int x = 0; x < p_size; x++) {
				const Vector2 p = Vector2(x + 0.5, y + 0.5) * (real_t(p_cells) / p_size);
				const int cx = int(Math::floor(p.x));
				const int cy = int(Math::floor(p.y));
				real_t f1 = 1e9;
				real_t f2 = 1e9;
				for (int oy = -1; oy <= 1; oy++) {
					for (int ox = -1; ox <= 1; ox++) {
						const int gx = cx + ox;
						const int gy = cy + oy;
						const Vector2 feature = features[((gy % p_cells + p_cells) % p_cells) * p_cells + ((gx % p_cells + p_cells) % p_cells)];
						const real_t d = p.distance_to(Vector2(gx, gy) + feature);
						if (d < f1) {
							f2 = f1;
							f1 = d;
						} else if (d < f2) {
							f2 = d;
						}
					}
				}
				r_values.write[y * p_size + x] = f2 - f1;
			}
		}
	};
	Vector<real_t> web;
	Vector<real_t> bubbles;
	layer(9, web);
	layer(23, bubbles);
	Vector<uint8_t> data;
	data.resize(p_size * p_size);
	uint8_t *w = data.ptrw();
	for (int i = 0; i < p_size * p_size; i++) {
		const real_t a = 1.0 - CLAMP(web[i] / 0.35, 0.0, 1.0);
		const real_t b = 1.0 - CLAMP(bubbles[i] / 0.25, 0.0, 1.0);
		const real_t v = CLAMP(a * a * 0.8 + b * b * 0.45, 0.0, 1.0);
		w[i] = uint8_t(Math::round(v * 255.0));
	}
	Ref<Image> image = Image::create_from_data(p_size, p_size, false, Image::FORMAT_R8, data);
	image->generate_mipmaps();
	return image;
}

/* LandscapeWaterMaterial */

Mutex LandscapeWaterMaterial::shader_mutex;
RID LandscapeWaterMaterial::shader;
Ref<ImageTexture> LandscapeWaterMaterial::default_normal;
Ref<ImageTexture> LandscapeWaterMaterial::default_foam;

enum {
	WATER_SHALLOW_COLOR,
	WATER_DEEP_COLOR,
	WATER_CLARITY,
	WATER_ROUGHNESS,
	WATER_SPECULAR,
	WATER_REFRACTION,
	WATER_NORMAL_TEXTURE,
	WATER_NORMAL_SCALE,
	WATER_NORMAL_STRENGTH,
	WATER_DETAIL_SCALE,
	WATER_DETAIL_STRENGTH,
	WATER_WIND_VELOCITY,
	WATER_FLOW_SPEED_SCALE,
	WATER_FLOW_CYCLE,
	WATER_BANK_SLOWDOWN,
	WATER_FOAM_COLOR,
	WATER_FOAM_TEXTURE,
	WATER_FOAM_SCALE,
	WATER_SHORE_FOAM,
	WATER_SHORE_FOAM_DISTANCE,
	WATER_FLOW_FOAM,
	WATER_EDGE_FADE,
	WATER_WAVE_HEIGHT,
	WATER_WAVE_LENGTH,
	WATER_MAX,
};

static const LandscapeSplineMaterial::ParameterInfo water_parameters[WATER_MAX] = {
	{ "shallow_color", Variant::COLOR, PROPERTY_HINT_COLOR_NO_ALPHA, "", "Water" },
	{ "deep_color", Variant::COLOR, PROPERTY_HINT_COLOR_NO_ALPHA, "", "Water" },
	{ "clarity", Variant::FLOAT, PROPERTY_HINT_RANGE, "0.05,64,0.01,or_greater,suffix:m", "Water" },
	{ "roughness", Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01", "Water" },
	{ "specular", Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01", "Water" },
	{ "refraction", Variant::FLOAT, PROPERTY_HINT_RANGE, "0,0.2,0.001", "Water" },
	{ "normal_texture", Variant::OBJECT, PROPERTY_HINT_RESOURCE_TYPE, "Texture2D", "Ripples" },
	{ "normal_scale", Variant::FLOAT, PROPERTY_HINT_RANGE, "0.1,64,0.01,or_greater,suffix:m", "Ripples" },
	{ "normal_strength", Variant::FLOAT, PROPERTY_HINT_RANGE, "0,2,0.01", "Ripples" },
	{ "detail_scale", Variant::FLOAT, PROPERTY_HINT_RANGE, "0.1,64,0.01,or_greater,suffix:m", "Ripples" },
	{ "detail_strength", Variant::FLOAT, PROPERTY_HINT_RANGE, "0,2,0.01", "Ripples" },
	{ "wind_velocity", Variant::VECTOR2, PROPERTY_HINT_NONE, "suffix:m/s", "Ripples" },
	{ "flow_speed_scale", Variant::FLOAT, PROPERTY_HINT_RANGE, "0,4,0.01,or_greater", "Flow" },
	{ "flow_cycle", Variant::FLOAT, PROPERTY_HINT_RANGE, "0.1,8,0.01,suffix:s", "Flow" },
	{ "bank_slowdown", Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01", "Flow" },
	{ "foam_color", Variant::COLOR, PROPERTY_HINT_COLOR_NO_ALPHA, "", "Foam" },
	{ "foam_texture", Variant::OBJECT, PROPERTY_HINT_RESOURCE_TYPE, "Texture2D", "Foam" },
	{ "foam_scale", Variant::FLOAT, PROPERTY_HINT_RANGE, "0.1,64,0.01,or_greater,suffix:m", "Foam" },
	{ "shore_foam", Variant::FLOAT, PROPERTY_HINT_RANGE, "0,2,0.01", "Foam" },
	{ "shore_foam_distance", Variant::FLOAT, PROPERTY_HINT_RANGE, "0.01,8,0.01,or_greater,suffix:m", "Foam" },
	{ "flow_foam", Variant::FLOAT, PROPERTY_HINT_RANGE, "0,2,0.01", "Foam" },
	{ "edge_fade", Variant::FLOAT, PROPERTY_HINT_RANGE, "0,4,0.01,or_greater,suffix:m", "Shore" },
	{ "wave_height", Variant::FLOAT, PROPERTY_HINT_RANGE, "0,4,0.01,or_greater,suffix:m", "Waves" },
	{ "wave_length", Variant::FLOAT, PROPERTY_HINT_RANGE, "0.5,256,0.01,or_greater,suffix:m", "Waves" },
};

int LandscapeWaterMaterial::_get_parameter_count() const {
	return WATER_MAX;
}

const LandscapeSplineMaterial::ParameterInfo *LandscapeWaterMaterial::_get_parameter_infos() const {
	return water_parameters;
}

Variant LandscapeWaterMaterial::_get_parameter_default(int p_index) const {
	switch (p_index) {
		case WATER_SHALLOW_COLOR:
			return Color(0.30, 0.62, 0.58);
		case WATER_DEEP_COLOR:
			return Color(0.02, 0.11, 0.15);
		case WATER_CLARITY:
			return 3.0;
		case WATER_ROUGHNESS:
			return 0.03;
		case WATER_SPECULAR:
			return 0.5;
		case WATER_REFRACTION:
			return 0.04;
		case WATER_NORMAL_SCALE:
			return 5.0;
		case WATER_NORMAL_STRENGTH:
			return 0.5;
		case WATER_DETAIL_SCALE:
			return 1.6;
		case WATER_DETAIL_STRENGTH:
			return 0.35;
		case WATER_WIND_VELOCITY:
			return Vector2(0.25, 0.1);
		case WATER_FLOW_SPEED_SCALE:
			return 1.0;
		case WATER_FLOW_CYCLE:
			return 1.5;
		case WATER_BANK_SLOWDOWN:
			return 0.5;
		case WATER_FOAM_COLOR:
			return Color(0.9, 0.93, 0.93);
		case WATER_FOAM_SCALE:
			return 2.5;
		case WATER_SHORE_FOAM:
			return 0.5;
		case WATER_SHORE_FOAM_DISTANCE:
			return 0.5;
		case WATER_FLOW_FOAM:
			return 1.0;
		case WATER_EDGE_FADE:
			return 0.25;
		case WATER_WAVE_HEIGHT:
			return 0.0;
		case WATER_WAVE_LENGTH:
			return 14.0;
		default:
			return Variant(); // Textures.
	}
}

RID LandscapeWaterMaterial::_get_default_texture(int p_index) const {
	if (p_index == WATER_NORMAL_TEXTURE) {
		return get_default_normal_texture()->get_rid();
	}
	if (p_index == WATER_FOAM_TEXTURE) {
		return get_default_foam_texture()->get_rid();
	}
	return RID();
}

Ref<Texture2D> LandscapeWaterMaterial::get_default_normal_texture() {
	if (default_normal.is_null()) {
		default_normal = ImageTexture::create_from_image(_landscape_make_ripple_normals(256));
	}
	return default_normal;
}

Ref<Texture2D> LandscapeWaterMaterial::get_default_foam_texture() {
	if (default_foam.is_null()) {
		default_foam = ImageTexture::create_from_image(_landscape_make_foam(256));
	}
	return default_foam;
}

RID LandscapeWaterMaterial::_get_shader() const {
	MutexLock lock(shader_mutex);
	if (shader.is_null()) {
		shader = _create_shader(get_builtin_shader_code());
	}
	return shader;
}

void LandscapeWaterMaterial::cleanup_shared_resources() {
	MutexLock lock(shader_mutex);
	if (shader.is_valid()) {
		RenderingServer::get_singleton()->free_rid(shader);
		shader = RID();
	}
	default_normal.unref();
	default_foam.unref();
}

void LandscapeWaterMaterial::_bind_methods() {
	ClassDB::bind_static_method("LandscapeWaterMaterial", D_METHOD("get_builtin_shader_code"), &LandscapeWaterMaterial::get_builtin_shader_code);
	ClassDB::bind_static_method("LandscapeWaterMaterial", D_METHOD("get_default_normal_texture"), &LandscapeWaterMaterial::get_default_normal_texture);
	ClassDB::bind_static_method("LandscapeWaterMaterial", D_METHOD("get_default_foam_texture"), &LandscapeWaterMaterial::get_default_foam_texture);
	_bind_parameters(get_class_static(), water_parameters, WATER_MAX);
}

LandscapeWaterMaterial::LandscapeWaterMaterial() {
	_init_parameters();
}

String LandscapeWaterMaterial::get_builtin_shader_code() {
	return R"(// Built-in water material of LandscapeSpline3D (rivers, streams, lakes).
// Vertex data: UV (along and across rivers, XZ for lakes, in meters), UV2 (flow velocity in UV units per second),
// COLOR.r (turbulence of rapids), COLOR.g (distance to the center relative to the half width), COLOR.a (fade).

shader_type spatial;
render_mode blend_mix, depth_draw_always, cull_back, diffuse_burley, specular_schlick_ggx;

uniform vec4 shallow_color : source_color = vec4(0.30, 0.62, 0.58, 1.0);
uniform vec4 deep_color : source_color = vec4(0.02, 0.11, 0.15, 1.0);
uniform float clarity : hint_range(0.05, 64.0) = 3.0; // Distance (m) over which the light is mostly absorbed.
uniform float roughness : hint_range(0.0, 1.0) = 0.03;
uniform float specular : hint_range(0.0, 1.0) = 0.5;
uniform float refraction : hint_range(0.0, 0.2) = 0.04;
uniform sampler2D normal_texture : hint_normal, filter_linear_mipmap_anisotropic, repeat_enable;
uniform float normal_scale = 5.0; // Size (m) of the ripple pattern.
uniform float normal_strength : hint_range(0.0, 2.0) = 0.5;
uniform float detail_scale = 1.6;
uniform float detail_strength : hint_range(0.0, 2.0) = 0.35;
uniform vec2 wind_velocity = vec2(0.25, 0.1);
uniform float flow_speed_scale = 1.0;
uniform float flow_cycle = 1.5; // Seconds: the ripples are advected over a cycle, then crossfaded.
uniform float bank_slowdown : hint_range(0.0, 1.0) = 0.5; // Relative speed of the flow along the banks.
uniform vec4 foam_color : source_color = vec4(0.9, 0.93, 0.93, 1.0);
uniform sampler2D foam_texture : hint_default_white, filter_linear_mipmap, repeat_enable;
uniform float foam_scale = 2.5;
uniform float shore_foam : hint_range(0.0, 2.0) = 0.5;
uniform float shore_foam_distance = 0.5;
uniform float flow_foam : hint_range(0.0, 2.0) = 1.0;
uniform float edge_fade = 0.25; // Soft intersection with the ground (m).
uniform float wave_height = 0.0;
uniform float wave_length = 14.0;
uniform sampler2D ls_screen : hint_screen_texture, filter_linear_mipmap, repeat_disable;
uniform sampler2D ls_depth : hint_depth_texture, filter_nearest, repeat_disable;

varying float v_turbulence;
varying float v_lateral;
varying float v_fade;
varying vec2 v_velocity;

void vertex() {
	v_turbulence = COLOR.r;
	v_lateral = COLOR.g;
	v_fade = COLOR.a;
	v_velocity = UV2;
	if (wave_height > 0.0) {
		// Gentle swell: two directional waves in world space (deep water dispersion).
		vec3 world = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
		float k = 6.28318 / max(wave_length, 0.1);
		float c = sqrt(9.81 * k);
		float w = sin(dot(world.xz, vec2(0.94, 0.34)) * k - TIME * c) + 0.5 * sin(dot(world.xz, vec2(-0.37, 0.93)) * k * 1.8 - TIME * c * 1.34);
		VERTEX.y += w * wave_height * 0.5;
	}
}

float ls_view_depth(vec2 p_uv, mat4 p_inv_projection) {
	float depth = textureLod(ls_depth, p_uv, 0.0).r;
#if CURRENT_RENDERER == RENDERER_COMPATIBILITY
	vec4 view = p_inv_projection * vec4(vec3(p_uv, depth) * 2.0 - 1.0, 1.0);
#else
	vec4 view = p_inv_projection * vec4(p_uv * 2.0 - 1.0, depth, 1.0);
#endif
	return -view.z / view.w;
}

// Flow mapping: two phases half a cycle apart, crossfaded. The pattern follows the local flow
// velocity without stretching over time.
vec2 ls_flow_phases(out float r_blend) {
	float cycle = max(flow_cycle, 0.05);
	float phase = fract(TIME / cycle);
	r_blend = abs(1.0 - 2.0 * phase);
	return vec2(phase, fract(phase + 0.5)) * cycle;
}

vec3 ls_ripples(vec2 p_uv, vec2 p_flow, vec2 p_offsets, float p_blend, float p_scale, float p_strength) {
	vec3 n0 = texture(normal_texture, (p_uv - p_flow * p_offsets.x) / p_scale).rgb * 2.0 - 1.0;
	vec3 n1 = texture(normal_texture, (p_uv - p_flow * p_offsets.y) / p_scale + vec2(0.37, 0.61)).rgb * 2.0 - 1.0;
	vec3 n = mix(n0, n1, p_blend);
	return vec3(n.xy * p_strength, max(n.z, 0.1));
}

void fragment() {
	// Slower flow along the banks.
	vec2 flow = v_velocity * flow_speed_scale * mix(1.0, bank_slowdown, smoothstep(0.35, 1.0, v_lateral));
	vec2 drift = wind_velocity * TIME; // Constant drift: plain scrolling.
	float blend;
	vec2 offsets = ls_flow_phases(blend);
	vec3 n1 = ls_ripples(UV - drift, flow, offsets, blend, max(normal_scale, 0.01), normal_strength);
	vec3 n2 = ls_ripples(UV - drift * 1.7 + vec2(0.5), flow * 1.25, offsets, blend, max(detail_scale, 0.01), detail_strength);
	vec3 ts = normalize(vec3(n1.xy + n2.xy, n1.z * n2.z));
	vec3 normal = normalize(TANGENT * ts.x + BINORMAL * ts.y + NORMAL * ts.z);

	// Thickness of water in front of the ground.
	float surface = -VERTEX.z;
	float ground = ls_view_depth(SCREEN_UV, INV_PROJECTION_MATRIX);
	float thickness = max(ground - surface, 0.0);
	vec2 refracted_uv = SCREEN_UV + normal.xy * refraction * clamp(thickness, 0.0, 1.0);
	float refracted_ground = ls_view_depth(refracted_uv, INV_PROJECTION_MATRIX);
	if (refracted_ground < surface) {
		// What is in front of the water is not refracted.
		refracted_uv = SCREEN_UV;
		refracted_ground = ground;
	}
	// Distance travelled by the light in the water (along the view ray).
	float path = max(refracted_ground - surface, 0.0) * length(VERTEX) / max(surface, 1e-4);
	float transmittance = exp(-path / max(clarity, 0.01));
	vec3 background = textureLod(ls_screen, refracted_uv, 0.0).rgb;
	vec3 transmitted = background * mix(shallow_color.rgb, vec3(1.0), transmittance);
	vec3 scattered = mix(shallow_color.rgb, deep_color.rgb, 1.0 - exp(-path / max(clarity * 3.0, 0.01)));

	// Less light comes through the surface at grazing angles (the rest is reflected).
	float fresnel = 0.02 + 0.98 * pow(1.0 - clamp(dot(normal, VIEW), 0.0, 1.0), 5.0);

	// Foam along the shore and in rapids.
	vec2 foam_uv = UV - drift;
	float f0 = texture(foam_texture, (foam_uv - flow * offsets.x) / max(foam_scale, 0.01)).r;
	float f1 = texture(foam_texture, (foam_uv - flow * offsets.y) / max(foam_scale, 0.01) + vec2(0.29, 0.47)).r;
	float foam_noise = mix(f0, f1, blend);
	float shore = 1.0 - smoothstep(0.0, max(shore_foam_distance, 0.01), thickness);
	float amount = clamp(shore * shore_foam + v_turbulence * flow_foam, 0.0, 1.0);
	float foam = clamp((foam_noise - (1.0 - amount)) * 4.0, 0.0, 1.0) * step(0.001, amount);

	ALBEDO = mix(scattered * (1.0 - transmittance) * (1.0 - fresnel), foam_color.rgb, foam);
	EMISSION = transmitted * transmittance * (1.0 - fresnel) * (1.0 - foam);
	ROUGHNESS = mix(roughness, 0.65, foam);
	SPECULAR = specular;
	METALLIC = 0.0;
	NORMAL = normalize(mix(normal, NORMAL, foam * 0.7));
	ALPHA = smoothstep(0.0, max(edge_fade, 0.001), thickness) * v_fade;
}
)";
}

/* LandscapeRoadMaterial */

Mutex LandscapeRoadMaterial::shader_mutex;
RID LandscapeRoadMaterial::shader;

enum {
	ROAD_ALBEDO_COLOR,
	ROAD_ALBEDO_TEXTURE,
	ROAD_NORMAL_TEXTURE,
	ROAD_NORMAL_STRENGTH,
	ROAD_TEXTURE_SCALE,
	ROAD_ROUGHNESS,
	ROAD_GRAIN,
	ROAD_GRAIN_SCALE,
	ROAD_WEAR,
	ROAD_LANE_COUNT,
	ROAD_LANE_MARKINGS,
	ROAD_MARKING_COLOR,
	ROAD_CENTER_LINE,
	ROAD_CENTER_LINE_COLOR,
	ROAD_MARKING_WIDTH,
	ROAD_EDGE_LINE_OFFSET,
	ROAD_DASH_LENGTH,
	ROAD_DASH_GAP,
	ROAD_EDGE_COLOR,
	ROAD_EDGE_WIDTH,
	ROAD_EDGE_NOISE,
	ROAD_WETNESS,
	ROAD_PUDDLES,
	ROAD_MAX,
};

static const LandscapeSplineMaterial::ParameterInfo road_parameters[ROAD_MAX] = {
	{ "albedo_color", Variant::COLOR, PROPERTY_HINT_COLOR_NO_ALPHA, "", "Surface" },
	{ "albedo_texture", Variant::OBJECT, PROPERTY_HINT_RESOURCE_TYPE, "Texture2D", "Surface" },
	{ "normal_texture", Variant::OBJECT, PROPERTY_HINT_RESOURCE_TYPE, "Texture2D", "Surface" },
	{ "normal_strength", Variant::FLOAT, PROPERTY_HINT_RANGE, "0,4,0.01", "Surface" },
	{ "texture_scale", Variant::VECTOR2, PROPERTY_HINT_NONE, "", "Surface" },
	{ "roughness", Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01", "Surface" },
	{ "grain", Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01", "Surface" },
	{ "grain_scale", Variant::FLOAT, PROPERTY_HINT_RANGE, "0.005,1,0.001,or_greater,suffix:m", "Surface" },
	{ "wear", Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01", "Surface" },
	{ "lane_count", Variant::INT, PROPERTY_HINT_RANGE, "1,8,1", "Markings" },
	{ "lane_markings", Variant::BOOL, PROPERTY_HINT_NONE, "", "Markings" },
	{ "marking_color", Variant::COLOR, PROPERTY_HINT_COLOR_NO_ALPHA, "", "Markings" },
	{ "center_line", Variant::INT, PROPERTY_HINT_ENUM, "None,Dashed,Solid,Double Solid", "Markings" },
	{ "center_line_color", Variant::COLOR, PROPERTY_HINT_COLOR_NO_ALPHA, "", "Markings" },
	{ "marking_width", Variant::FLOAT, PROPERTY_HINT_RANGE, "0.01,1,0.001,suffix:m", "Markings" },
	{ "edge_line_offset", Variant::FLOAT, PROPERTY_HINT_RANGE, "0,4,0.01,suffix:m", "Markings" },
	{ "dash_length", Variant::FLOAT, PROPERTY_HINT_RANGE, "0.1,32,0.01,suffix:m", "Markings" },
	{ "dash_gap", Variant::FLOAT, PROPERTY_HINT_RANGE, "0,32,0.01,suffix:m", "Markings" },
	{ "edge_color", Variant::COLOR, PROPERTY_HINT_COLOR_NO_ALPHA, "", "Edges" },
	{ "edge_width", Variant::FLOAT, PROPERTY_HINT_RANGE, "0,4,0.01,suffix:m", "Edges" },
	{ "edge_noise", Variant::FLOAT, PROPERTY_HINT_RANGE, "0,2,0.01,suffix:m", "Edges" },
	{ "wetness", Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01", "Weather" },
	{ "puddles", Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01", "Weather" },
};

int LandscapeRoadMaterial::_get_parameter_count() const {
	return ROAD_MAX;
}

const LandscapeSplineMaterial::ParameterInfo *LandscapeRoadMaterial::_get_parameter_infos() const {
	return road_parameters;
}

Variant LandscapeRoadMaterial::_get_parameter_default(int p_index) const {
	switch (p_index) {
		case ROAD_ALBEDO_COLOR:
			return Color(0.16, 0.16, 0.17);
		case ROAD_NORMAL_STRENGTH:
			return 1.0;
		case ROAD_TEXTURE_SCALE:
			return Vector2(1.0, 4.0);
		case ROAD_ROUGHNESS:
			return 0.82;
		case ROAD_GRAIN:
			return 0.35;
		case ROAD_GRAIN_SCALE:
			return 0.06;
		case ROAD_WEAR:
			return 0.25;
		case ROAD_LANE_COUNT:
			return 2;
		case ROAD_LANE_MARKINGS:
			return true;
		case ROAD_MARKING_COLOR:
			return Color(0.86, 0.86, 0.82);
		case ROAD_CENTER_LINE:
			return 1;
		case ROAD_CENTER_LINE_COLOR:
			return Color(0.86, 0.86, 0.82);
		case ROAD_MARKING_WIDTH:
			return 0.12;
		case ROAD_EDGE_LINE_OFFSET:
			return 0.25;
		case ROAD_DASH_LENGTH:
			return 3.0;
		case ROAD_DASH_GAP:
			return 6.0;
		case ROAD_EDGE_COLOR:
			return Color(0.3, 0.26, 0.2);
		case ROAD_EDGE_WIDTH:
			return 0.5;
		case ROAD_EDGE_NOISE:
			return 0.3;
		case ROAD_WETNESS:
			return 0.0;
		case ROAD_PUDDLES:
			return 0.0;
		default:
			return Variant(); // Textures.
	}
}

void LandscapeRoadMaterial::apply_preset(Preset p_preset) {
	for (int i = 0; i < ROAD_MAX; i++) {
		if (road_parameters[i].type != Variant::OBJECT) {
			set_parameter_by_index(i, _get_parameter_default(i));
		}
	}
	switch (p_preset) {
		case PRESET_ASPHALT:
			break;
		case PRESET_DIRT: {
			set_parameter_by_index(ROAD_ALBEDO_COLOR, Color(0.36, 0.28, 0.2));
			set_parameter_by_index(ROAD_ROUGHNESS, 0.95);
			set_parameter_by_index(ROAD_GRAIN, 0.5);
			set_parameter_by_index(ROAD_GRAIN_SCALE, 0.15);
			set_parameter_by_index(ROAD_WEAR, 0.6);
			set_parameter_by_index(ROAD_LANE_MARKINGS, false);
			set_parameter_by_index(ROAD_EDGE_COLOR, Color(0.3, 0.33, 0.18));
			set_parameter_by_index(ROAD_EDGE_WIDTH, 0.8);
			set_parameter_by_index(ROAD_EDGE_NOISE, 0.6);
		} break;
		case PRESET_GRAVEL: {
			set_parameter_by_index(ROAD_ALBEDO_COLOR, Color(0.45, 0.43, 0.4));
			set_parameter_by_index(ROAD_ROUGHNESS, 0.9);
			set_parameter_by_index(ROAD_GRAIN, 0.8);
			set_parameter_by_index(ROAD_GRAIN_SCALE, 0.04);
			set_parameter_by_index(ROAD_WEAR, 0.4);
			set_parameter_by_index(ROAD_LANE_MARKINGS, false);
			set_parameter_by_index(ROAD_EDGE_COLOR, Color(0.35, 0.33, 0.28));
			set_parameter_by_index(ROAD_EDGE_WIDTH, 0.4);
			set_parameter_by_index(ROAD_EDGE_NOISE, 0.4);
		} break;
	}
	notify_property_list_changed();
}

RID LandscapeRoadMaterial::_get_shader() const {
	MutexLock lock(shader_mutex);
	if (shader.is_null()) {
		shader = _create_shader(get_builtin_shader_code());
	}
	return shader;
}

void LandscapeRoadMaterial::cleanup_shared_resources() {
	MutexLock lock(shader_mutex);
	if (shader.is_valid()) {
		RenderingServer::get_singleton()->free_rid(shader);
		shader = RID();
	}
}

void LandscapeRoadMaterial::_bind_methods() {
	ClassDB::bind_method(D_METHOD("apply_preset", "preset"), &LandscapeRoadMaterial::apply_preset);
	ClassDB::bind_static_method("LandscapeRoadMaterial", D_METHOD("get_builtin_shader_code"), &LandscapeRoadMaterial::get_builtin_shader_code);
	_bind_parameters(get_class_static(), road_parameters, ROAD_MAX);

	BIND_ENUM_CONSTANT(PRESET_ASPHALT);
	BIND_ENUM_CONSTANT(PRESET_DIRT);
	BIND_ENUM_CONSTANT(PRESET_GRAVEL);
}

LandscapeRoadMaterial::LandscapeRoadMaterial() {
	_init_parameters();
}

String LandscapeRoadMaterial::get_builtin_shader_code() {
	return R"(// Built-in road material of LandscapeSpline3D.
// Vertex data: UV (across the road from 0 to 1, along it in meters), UV2 (distance to the center, half width, in meters),
// COLOR.a (0 at the bottom of the skirts).

shader_type spatial;
render_mode blend_mix, depth_draw_opaque, cull_back, diffuse_burley, specular_schlick_ggx;

uniform vec4 albedo_color : source_color = vec4(0.16, 0.16, 0.17, 1.0);
uniform sampler2D albedo_texture : source_color, hint_default_white, filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D normal_texture : hint_normal, filter_linear_mipmap_anisotropic, repeat_enable;
uniform float normal_strength : hint_range(0.0, 4.0) = 1.0;
uniform vec2 texture_scale = vec2(1.0, 4.0); // Texture size across (1 = road width) and along (m).
uniform float roughness : hint_range(0.0, 1.0) = 0.82;
uniform float grain : hint_range(0.0, 1.0) = 0.35;
uniform float grain_scale = 0.06;
uniform float wear : hint_range(0.0, 1.0) = 0.25;
uniform int lane_count : hint_range(1, 8) = 2;
uniform bool lane_markings = true;
uniform vec4 marking_color : source_color = vec4(0.86, 0.86, 0.82, 1.0);
uniform int center_line : hint_range(0, 3) = 1; // None, dashed, solid, double solid.
uniform vec4 center_line_color : source_color = vec4(0.86, 0.86, 0.82, 1.0);
uniform float marking_width = 0.12;
uniform float edge_line_offset = 0.25;
uniform float dash_length = 3.0;
uniform float dash_gap = 6.0;
uniform vec4 edge_color : source_color = vec4(0.3, 0.26, 0.2, 1.0);
uniform float edge_width = 0.5;
uniform float edge_noise = 0.3;
uniform float wetness : hint_range(0.0, 1.0) = 0.0;
uniform float puddles : hint_range(0.0, 1.0) = 0.0;

float road_hash(vec2 p_p) {
	vec3 p3 = fract(vec3(p_p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

float road_noise(vec2 p_p) {
	vec2 i = floor(p_p);
	vec2 f = fract(p_p);
	vec2 u = f * f * (3.0 - 2.0 * f);
	return mix(mix(road_hash(i), road_hash(i + vec2(1.0, 0.0)), u.x), mix(road_hash(i + vec2(0.0, 1.0)), road_hash(i + vec2(1.0, 1.0)), u.x), u.y);
}

float road_line(float p_x, float p_center, float p_width) {
	float aa = max(fwidth(p_x), 1e-4);
	return 1.0 - smoothstep(p_width * 0.5 - aa, p_width * 0.5 + aa, abs(p_x - p_center));
}

void fragment() {
	float lateral = UV2.x;
	float half_width = max(UV2.y, 0.01);
	float along = UV.y;
	float edge_distance = max(half_width - abs(lateral), 0.0);
	float skirt = 1.0 - COLOR.a;
	vec2 road_pos = vec2(lateral, along);

	vec2 texture_uv = vec2(UV.x / max(texture_scale.x, 0.001), along / max(texture_scale.y, 0.001));
	vec3 albedo = albedo_color.rgb * texture(albedo_texture, texture_uv).rgb;
	float rough = roughness;

	// Aggregate grain and large patches.
	vec2 grain_uv = road_pos / max(grain_scale, 0.001);
	float g = road_noise(grain_uv) * 0.6 + road_noise(grain_uv * 3.1 + 17.0) * 0.4;
	albedo *= 1.0 + (g - 0.5) * grain;
	albedo *= 1.0 + (road_noise(road_pos * 0.15) - 0.5) * 0.25;

	// Wheel tracks (ruts on dirt roads): two per lane.
	float lane_width = 2.0 * half_width / float(max(lane_count, 1));
	float in_lane = fract((lateral + half_width) / lane_width) * lane_width - lane_width * 0.5;
	float tracks = exp(-pow((abs(in_lane) - min(0.85, lane_width * 0.3)) / 0.35, 2.0));
	albedo *= 1.0 - tracks * wear * 0.35;
	rough = mix(rough, rough * 0.8, tracks * wear);

	if (lane_markings) {
		float paint = road_line(abs(lateral), half_width - edge_line_offset, marking_width);
		float period = max(dash_length + dash_gap, 0.01);
		float dash = step(fract(along / period), dash_length / period);
		float center_paint = 0.0;
		for (int i = 1; i < lane_count; i++) {
			float x = -half_width + lane_width * float(i);
			if (lane_count % 2 == 0 && i == lane_count / 2) {
				if (center_line == 1) {
					center_paint = max(center_paint, road_line(lateral, x, marking_width) * dash);
				} else if (center_line == 2) {
					center_paint = max(center_paint, road_line(lateral, x, marking_width));
				} else if (center_line == 3) {
					center_paint = max(center_paint, road_line(lateral, x - marking_width, marking_width));
					center_paint = max(center_paint, road_line(lateral, x + marking_width, marking_width));
				}
			} else {
				paint = max(paint, road_line(lateral, x, marking_width) * dash);
			}
		}
		// Worn paint.
		float worn = 1.0 - wear * 0.7 * smoothstep(0.4, 0.9, road_noise(road_pos * vec2(8.0, 2.0)));
		albedo = mix(albedo, marking_color.rgb, paint * worn);
		albedo = mix(albedo, center_line_color.rgb, center_paint * worn);
		rough = mix(rough, 0.55, max(paint, center_paint) * worn);
	}

	// Dirt along the edges and on the skirts, irregular edges blending into the terrain.
	float n = road_noise(vec2(along, lateral) * 1.3) - 0.5;
	float dirt = max(1.0 - smoothstep(0.0, max(edge_width, 0.01), edge_distance + n * edge_noise), step(0.001, skirt));
	albedo = mix(albedo, edge_color.rgb, dirt);
	rough = mix(rough, 0.95, dirt);
	float cut = road_noise(vec2(along, lateral) * 0.7 + 31.0) - 0.5;
	ALPHA = clamp((edge_distance + cut * edge_noise * 2.0) / 0.05 + 0.5, 0.0, 1.0);
	ALPHA_SCISSOR_THRESHOLD = 0.5;

	// Wet surface and puddles.
	float puddle_mask = road_noise(road_pos * 0.35) * 0.7 + road_noise(road_pos * 1.1) * 0.3;
	float puddle = smoothstep(0.62, 0.68, puddle_mask + puddles * 0.35 - 0.2) * puddles * (1.0 - dirt);
	float wet = max(wetness, puddle);
	albedo *= mix(1.0, 0.55, wet);
	rough = mix(rough, 0.25, wetness);
	rough = mix(rough, 0.03, puddle);

	ALBEDO = albedo;
	ROUGHNESS = clamp(rough, 0.0, 1.0);
	METALLIC = 0.0;
	NORMAL_MAP = mix(texture(normal_texture, texture_uv).rgb, vec3(0.5, 0.5, 1.0), puddle);
	NORMAL_MAP_DEPTH = normal_strength;
}
)";
}
