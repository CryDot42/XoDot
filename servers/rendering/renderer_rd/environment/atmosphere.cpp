/**************************************************************************/
/*  atmosphere.cpp                                                        */
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

#include "atmosphere.h"

#include "core/templates/hashfuncs.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

// Scene units are meters; the atmosphere works in kilometers.
static constexpr float WORLD_TO_KM = 0.001f;

// Sizes of the tables, in texels. Keep in sync with atmosphere_data_inc.glsl.
static const Vector2i TRANSMITTANCE_SIZE(256, 64);
static constexpr int MULTISCATTERING_SIZE = 32;
static const Vector2i SKY_VIEW_SIZE(192, 108);
static constexpr int AERIAL_PERSPECTIVE_SIZE = 32;

void AtmosphereRD::init() {
	Vector<String> modes;
	modes.push_back("\n#define MODE_TRANSMITTANCE\n");
	modes.push_back("\n#define MODE_MULTISCATTERING\n");
	modes.push_back("\n#define MODE_SKY_VIEW\n");
	modes.push_back("\n#define MODE_AERIAL_PERSPECTIVE\n");
	shader.initialize(modes);
	shader_version = shader.version_create();
	for (int i = 0; i < MODE_MAX; i++) {
		pipelines[i].create_compute_pipeline(shader.version_get_shader(shader_version, i));
	}

	uniform_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(AtmosphereData));
	data = {};
	RD::get_singleton()->buffer_update(uniform_buffer, 0, sizeof(AtmosphereData), &data);
	// The sky binds these whether or not there is an atmosphere, so they exist
	// from the start; at their size, that costs next to nothing.
	_create_textures();
}

void AtmosphereRD::free() {
	for (int i = 0; i < MODE_MAX; i++) {
		pipelines[i].free();
	}
	shader.version_free(shader_version);
	_free_textures();
	if (uniform_buffer.is_valid()) {
		RD::get_singleton()->free_rid(uniform_buffer);
		uniform_buffer = RID();
	}
}

void AtmosphereRD::_create_textures() {
	RD::TextureFormat tf;
	tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	tf.texture_type = RD::TEXTURE_TYPE_2D;

	tf.width = TRANSMITTANCE_SIZE.x;
	tf.height = TRANSMITTANCE_SIZE.y;
	transmittance_lut = RD::get_singleton()->texture_create(tf, RD::TextureView());
	RD::get_singleton()->set_resource_name(transmittance_lut, "Atmosphere Transmittance LUT");

	tf.width = MULTISCATTERING_SIZE;
	tf.height = MULTISCATTERING_SIZE;
	multiscattering_lut = RD::get_singleton()->texture_create(tf, RD::TextureView());
	RD::get_singleton()->set_resource_name(multiscattering_lut, "Atmosphere Multiple Scattering LUT");

	tf.width = SKY_VIEW_SIZE.x;
	tf.height = SKY_VIEW_SIZE.y;
	sky_view_lut = RD::get_singleton()->texture_create(tf, RD::TextureView());
	RD::get_singleton()->set_resource_name(sky_view_lut, "Atmosphere Sky View LUT");

	tf.texture_type = RD::TEXTURE_TYPE_3D;
	tf.width = AERIAL_PERSPECTIVE_SIZE;
	tf.height = AERIAL_PERSPECTIVE_SIZE;
	tf.depth = AERIAL_PERSPECTIVE_SIZE;
	aerial_perspective_volume = RD::get_singleton()->texture_create(tf, RD::TextureView());
	RD::get_singleton()->set_resource_name(aerial_perspective_volume, "Atmosphere Aerial Perspective Volume");

	// Until an atmosphere is first rendered: no sky, and nothing in the way.
	RD::get_singleton()->texture_clear(transmittance_lut, Color(1, 1, 1, 1), 0, 1, 0, 1);
	RD::get_singleton()->texture_clear(multiscattering_lut, Color(0, 0, 0, 0), 0, 1, 0, 1);
	RD::get_singleton()->texture_clear(sky_view_lut, Color(0, 0, 0, 0), 0, 1, 0, 1);
	RD::get_singleton()->texture_clear(aerial_perspective_volume, Color(0, 0, 0, 1), 0, 1, 0, 1);
}

void AtmosphereRD::_free_textures() {
	for (RID *texture : { &transmittance_lut, &multiscattering_lut, &sky_view_lut, &aerial_perspective_volume }) {
		if (texture->is_valid()) {
			RD::get_singleton()->free_rid(*texture);
			*texture = RID();
		}
	}
	tables_valid = false;
}

uint32_t AtmosphereRD::_hash_tables(const AtmosphereData &p_data) {
	// Everything up to the eye's position describes the atmosphere itself,
	// plus the two values that come after it.
	uint32_t hash = hash_murmur3_buffer(&p_data, offsetof(AtmosphereData, camera_position));
	hash = hash_murmur3_one_float(p_data.absorption_width, hash);
	hash = hash_murmur3_one_float(p_data.multiscattering_factor, hash);
	return hash;
}

void AtmosphereRD::_dispatch(RD::ComputeListID p_list, Mode p_mode, RID p_dest, const Vector3i &p_threads) {
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	RID shader_rd = shader.version_get_shader(shader_version, p_mode);
	RID sampler = MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID black = TextureStorage::get_singleton()->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_BLACK);

	// A table is never sampled by the pass that writes it.
	RID transmittance = p_mode == MODE_TRANSMITTANCE ? black : transmittance_lut;
	RID multiscattering = (p_mode == MODE_TRANSMITTANCE || p_mode == MODE_MULTISCATTERING) ? black : multiscattering_lut;

	RD::Uniform u_data(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, uniform_buffer);
	RD::Uniform u_transmittance(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ sampler, transmittance }));
	RD::Uniform u_multiscattering(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ sampler, multiscattering }));
	RD::Uniform u_dest(RD::UNIFORM_TYPE_IMAGE, 0, p_dest);

	RD::get_singleton()->compute_list_bind_compute_pipeline(p_list, pipelines[p_mode].get_rid());
	RD::get_singleton()->compute_list_bind_uniform_set(p_list, uniform_set_cache->get_cache(shader_rd, 0, u_data, u_transmittance, u_multiscattering), 0);
	RD::get_singleton()->compute_list_bind_uniform_set(p_list, uniform_set_cache->get_cache(shader_rd, 1, u_dest), 1);
	RD::get_singleton()->compute_list_dispatch_threads(p_list, p_threads.x, p_threads.y, p_threads.z);
}

static void _store_color(const Color &p_color, float p_scale, float *r_dest) {
	r_dest[0] = p_color.r * p_scale;
	r_dest[1] = p_color.g * p_scale;
	r_dest[2] = p_color.b * p_scale;
}

bool AtmosphereRD::update(const RendererEnvironmentStorage::AtmosphereParams &p_params, const Transform3D &p_camera, const Projection &p_projection, const Light *p_lights, uint32_t p_light_count) {
	active = false;
	if (!p_params.enabled) {
		return false;
	}

	AtmosphereData &d = data;
	d = {};
	_store_color(p_params.rayleigh_scattering, p_params.rayleigh_scattering_scale, d.rayleigh_scattering);
	_store_color(p_params.mie_scattering, p_params.mie_scattering_scale, d.mie_scattering);
	for (int i = 0; i < 3; i++) {
		d.mie_extinction[i] = d.mie_scattering[i] + p_params.mie_absorption[i] * p_params.mie_absorption_scale;
	}
	_store_color(p_params.ozone_absorption, p_params.ozone_absorption_scale, d.absorption_extinction);
	_store_color(p_params.ground_albedo, 1.0, d.ground_albedo);
	_store_color(p_params.sky_luminance_factor, 1.0, d.sky_luminance_factor);

	d.bottom_radius = p_params.planet_radius;
	d.top_radius = p_params.planet_radius + p_params.height;
	d.mie_phase_g = p_params.mie_anisotropy;
	d.rayleigh_density_exp_scale = -1.0 / p_params.rayleigh_exponential_distribution;
	d.mie_density_exp_scale = -1.0 / p_params.mie_exponential_distribution;
	d.absorption_tip_altitude = p_params.ozone_tip_altitude;
	d.absorption_width = p_params.ozone_width;
	d.multiscattering_factor = p_params.multiscattering_factor;
	d.aerial_perspective_distance_scale = p_params.aerial_perspective_distance_scale;
	d.aerial_perspective_start_depth = p_params.aerial_perspective_start_depth;
	d.world_to_km = WORLD_TO_KM;
	d.enabled = 1;

	// The planet sits right under the world origin.
	Vector3 eye = p_camera.origin * WORLD_TO_KM + Vector3(0, p_params.planet_radius, 0);
	const float min_height = p_params.planet_radius + 0.0005f;
	if (eye.length() < min_height) {
		eye = eye.normalized() * min_height;
	}
	d.camera_position[0] = eye.x;
	d.camera_position[1] = eye.y;
	d.camera_position[2] = eye.z;

	d.light_count = MIN(p_light_count, MAX_LIGHTS);
	for (uint32_t i = 0; i < d.light_count; i++) {
		const Vector3 direction = p_lights[i].direction.normalized();
		d.light_direction[i][0] = direction.x;
		d.light_direction[i][1] = direction.y;
		d.light_direction[i][2] = direction.z;
		d.light_illuminance[i][0] = p_lights[i].illuminance.r;
		d.light_illuminance[i][1] = p_lights[i].illuminance.g;
		d.light_illuminance[i][2] = p_lights[i].illuminance.b;
	}

	MaterialStorage::store_camera(p_projection.inverse(), d.inv_projection);
	MaterialStorage::store_transform(Transform3D(p_camera.basis.orthonormalized(), Vector3()), d.camera_basis);

	RD::get_singleton()->buffer_update(uniform_buffer, 0, sizeof(AtmosphereData), &d);

	const uint32_t hash = _hash_tables(d);
	const bool rebuild_tables = !tables_valid || hash != tables_hash;

	RD::get_singleton()->draw_command_begin_label("Atmosphere");
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	if (rebuild_tables) {
		_dispatch(compute_list, MODE_TRANSMITTANCE, transmittance_lut, Vector3i(TRANSMITTANCE_SIZE.x, TRANSMITTANCE_SIZE.y, 1));
		RD::get_singleton()->compute_list_add_barrier(compute_list);
		_dispatch(compute_list, MODE_MULTISCATTERING, multiscattering_lut, Vector3i(MULTISCATTERING_SIZE, MULTISCATTERING_SIZE, 1));
		RD::get_singleton()->compute_list_add_barrier(compute_list);
		tables_hash = hash;
		tables_valid = true;
	}
	_dispatch(compute_list, MODE_SKY_VIEW, sky_view_lut, Vector3i(SKY_VIEW_SIZE.x, SKY_VIEW_SIZE.y, 1));
	_dispatch(compute_list, MODE_AERIAL_PERSPECTIVE, aerial_perspective_volume, Vector3i(AERIAL_PERSPECTIVE_SIZE, AERIAL_PERSPECTIVE_SIZE, 1));
	RD::get_singleton()->compute_list_end();
	RD::get_singleton()->draw_command_end_label();

	active = true;
	return true;
}

void AtmosphereRD::deactivate() {
	active = false;
	// Shaders check this before reading any table.
	if (data.enabled) {
		data.enabled = 0;
		RD::get_singleton()->buffer_update(uniform_buffer, offsetof(AtmosphereData, enabled), sizeof(uint32_t), &data.enabled);
	}
}

Color AtmosphereRD::light_transmittance(const RendererEnvironmentStorage::AtmosphereParams &p_params, const Vector3 &p_camera, const Vector3 &p_direction) {
	if (!p_params.enabled) {
		return Color(1, 1, 1);
	}
	const double bottom = p_params.planet_radius;
	const double top = p_params.planet_radius + p_params.height;
	Vector3 origin = p_camera * WORLD_TO_KM + Vector3(0, bottom, 0);
	if (origin.length() < bottom + 0.0005) {
		origin = origin.normalized() * (bottom + 0.0005);
	}
	const Vector3 dir = p_direction.normalized();

	// Where the ray leaves the atmosphere; a light the planet hides sends nothing.
	const double b = origin.dot(dir);
	const double c_ground = origin.length_squared() - bottom * bottom;
	if (b < 0.0 && b * b - c_ground >= 0.0) {
		return Color(0, 0, 0);
	}
	const double c_top = origin.length_squared() - top * top;
	const double delta = b * b - c_top;
	if (delta < 0.0) {
		return Color(1, 1, 1);
	}
	const double length_to_top = MAX(0.0, -b + Math::sqrt(delta));

	const Vector3 rayleigh = Vector3(p_params.rayleigh_scattering.r, p_params.rayleigh_scattering.g, p_params.rayleigh_scattering.b) * p_params.rayleigh_scattering_scale;
	const Vector3 mie = Vector3(p_params.mie_scattering.r, p_params.mie_scattering.g, p_params.mie_scattering.b) * p_params.mie_scattering_scale + Vector3(p_params.mie_absorption.r, p_params.mie_absorption.g, p_params.mie_absorption.b) * p_params.mie_absorption_scale;
	const Vector3 ozone = Vector3(p_params.ozone_absorption.r, p_params.ozone_absorption.g, p_params.ozone_absorption.b) * p_params.ozone_absorption_scale;

	constexpr int SAMPLES = 32;
	const double dt = length_to_top / SAMPLES;
	Vector3 optical_depth;
	for (int i = 0; i < SAMPLES; i++) {
		const double height = MAX(0.0, (origin + dir * ((i + 0.5) * dt)).length() - bottom);
		const double density_rayleigh = Math::exp(-height / p_params.rayleigh_exponential_distribution);
		const double density_mie = Math::exp(-height / p_params.mie_exponential_distribution);
		const double density_ozone = MAX(0.0, 1.0 - Math::abs(height - p_params.ozone_tip_altitude) / (p_params.ozone_width * 0.5));
		optical_depth += (rayleigh * density_rayleigh + mie * density_mie + ozone * density_ozone) * dt;
	}
	return Color(Math::exp(-optical_depth.x), Math::exp(-optical_depth.y), Math::exp(-optical_depth.z));
}
