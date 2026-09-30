/**************************************************************************/
/*  sky_material.h                                                        */
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

#include "core/templates/rid.h"
#include "scene/resources/material.h"

class Texture3D;

class ProceduralSkyMaterial : public Material {
	GDCLASS(ProceduralSkyMaterial, Material);

private:
	Color sky_top_color;
	Color sky_horizon_color;
	float sky_curve = 0.0f;
	float sky_energy_multiplier = 0.0f;
	Ref<Texture2D> sky_cover;
	Color sky_cover_modulate;

	Color ground_bottom_color;
	Color ground_horizon_color;
	float ground_curve = 0.0f;
	float ground_energy_multiplier = 0.0f;

	float sun_angle_max = 0.0f;
	float sun_curve = 0.0f;
	bool use_debanding = true;
	float global_energy_multiplier = 1.0f;

	static Mutex shader_mutex;
	static RID shader_cache[4];
	static void _update_shader(bool p_use_debanding, bool p_use_sky_cover);
	mutable bool shader_set = false;

	RID get_shader_cache() const;

protected:
	static void _bind_methods();
	void _validate_property(PropertyInfo &property) const;

public:
	void set_sky_top_color(const Color &p_sky_top);
	Color get_sky_top_color() const;

	void set_sky_horizon_color(const Color &p_sky_horizon);
	Color get_sky_horizon_color() const;

	void set_sky_curve(float p_curve);
	float get_sky_curve() const;

	void set_sky_energy_multiplier(float p_multiplier);
	float get_sky_energy_multiplier() const;

	void set_sky_cover(const Ref<Texture2D> &p_sky_cover);
	Ref<Texture2D> get_sky_cover() const;

	void set_sky_cover_modulate(const Color &p_sky_cover_modulate);
	Color get_sky_cover_modulate() const;

	void set_ground_bottom_color(const Color &p_ground_bottom);
	Color get_ground_bottom_color() const;

	void set_ground_horizon_color(const Color &p_ground_horizon);
	Color get_ground_horizon_color() const;

	void set_ground_curve(float p_curve);
	float get_ground_curve() const;

	void set_ground_energy_multiplier(float p_energy);
	float get_ground_energy_multiplier() const;

	void set_sun_angle_max(float p_angle);
	float get_sun_angle_max() const;

	void set_sun_curve(float p_curve);
	float get_sun_curve() const;

	void set_use_debanding(bool p_use_debanding);
	bool get_use_debanding() const;

	void set_energy_multiplier(float p_multiplier);
	float get_energy_multiplier() const;

	virtual Shader::Mode get_shader_mode() const override;
	virtual RID get_shader_rid() const override;
	virtual RID get_rid() const override;

	static void cleanup_shader();

	ProceduralSkyMaterial();
	~ProceduralSkyMaterial();
};

//////////////////////////////////////////////////////
/* PanoramaSkyMaterial */

class PanoramaSkyMaterial : public Material {
	GDCLASS(PanoramaSkyMaterial, Material);

private:
	Ref<Texture2D> panorama;
	float energy_multiplier = 1.0f;

	static Mutex shader_mutex;
	static RID shader_cache[2];
	static void _update_shader(bool p_filter);
	mutable bool shader_set = false;

	bool filter = true;

protected:
	static void _bind_methods();

public:
	void set_panorama(const Ref<Texture2D> &p_panorama);
	Ref<Texture2D> get_panorama() const;

	void set_filtering_enabled(bool p_enabled);
	bool is_filtering_enabled() const;

	void set_energy_multiplier(float p_multiplier);
	float get_energy_multiplier() const;

	virtual Shader::Mode get_shader_mode() const override;
	virtual RID get_shader_rid() const override;
	virtual RID get_rid() const override;

	static void cleanup_shader();

	PanoramaSkyMaterial();
	~PanoramaSkyMaterial();
};

//////////////////////////////////////////////////////
/* PanoramaSkyMaterial */

class PhysicalSkyMaterial : public Material {
	GDCLASS(PhysicalSkyMaterial, Material);

private:
	static Mutex shader_mutex;
	static RID shader_cache[4];

	RID get_shader_cache() const;

	float rayleigh = 0.0f;
	Color rayleigh_color;
	float mie = 0.0f;
	float mie_eccentricity = 0.0f;
	Color mie_color;
	float turbidity = 0.0f;
	float sun_disk_scale = 0.0f;
	Color ground_color;
	float energy_multiplier = 1.0f;
	bool use_debanding = true;
	Ref<Texture2D> night_sky;
	static void _update_shader(bool p_use_debanding, bool p_use_night_sky);
	mutable bool shader_set = false;

protected:
	static void _bind_methods();
	void _validate_property(PropertyInfo &property) const;

public:
	void set_rayleigh_coefficient(float p_rayleigh);
	float get_rayleigh_coefficient() const;

	void set_rayleigh_color(Color p_rayleigh_color);
	Color get_rayleigh_color() const;

	void set_turbidity(float p_turbidity);
	float get_turbidity() const;

	void set_mie_coefficient(float p_mie);
	float get_mie_coefficient() const;

	void set_mie_eccentricity(float p_eccentricity);
	float get_mie_eccentricity() const;

	void set_mie_color(Color p_mie_color);
	Color get_mie_color() const;

	void set_sun_disk_scale(float p_sun_disk_scale);
	float get_sun_disk_scale() const;

	void set_ground_color(Color p_ground_color);
	Color get_ground_color() const;

	void set_energy_multiplier(float p_multiplier);
	float get_energy_multiplier() const;

	void set_exposure_value(float p_exposure);
	float get_exposure_value() const;

	void set_use_debanding(bool p_use_debanding);
	bool get_use_debanding() const;

	void set_night_sky(const Ref<Texture2D> &p_night_sky);
	Ref<Texture2D> get_night_sky() const;

	virtual Shader::Mode get_shader_mode() const override;
	virtual RID get_shader_rid() const override;

	static void cleanup_shader();
	virtual RID get_rid() const override;

	PhysicalSkyMaterial();
	~PhysicalSkyMaterial();
};

// A sky drawn from the Environment's atmosphere (see Environment.atmosphere_enabled):
// the scattered light of every directional light, their disks seen through
// the air, the stars behind them, and a layer of volumetric clouds lit by the
// same lights through the same air.
//
// The clouds are ray marched through two tiling 3D noise textures (a coarse
// one for their shape and a fine one to erode their edges), at half the
// screen's resolution. Both default to NoiseTexture3Ds built once and shared
// by every material; any Texture3D can take their place.
class AtmosphereSkyMaterial : public Material {
	GDCLASS(AtmosphereSkyMaterial, Material);

private:
	enum ShaderFlags {
		SHADER_DEBANDING = 1,
		SHADER_CLOUDS = 2,
		SHADER_MAX = 4,
	};

	static Mutex shader_mutex;
	static RID shader_cache[SHADER_MAX];
	static Ref<Texture3D> default_clouds_shape_texture;
	static Ref<Texture3D> default_clouds_detail_texture;

	float sun_disk_scale = 1.0f;
	float sun_disk_intensity = 1.0f;
	Ref<Texture2D> night_sky;
	float night_sky_energy = 1.0f;
	float energy_multiplier = 1.0f;
	bool use_debanding = true;

	bool clouds_enabled = true;
	float clouds_coverage = 0.45f;
	float clouds_density = 1.0f;
	float clouds_bottom_altitude = 1500.0f;
	float clouds_thickness = 2500.0f;
	float clouds_extinction = 0.04f;
	float clouds_anisotropy = 0.6f;
	float clouds_ambient_strength = 1.0f;
	Ref<Texture3D> clouds_shape_texture;
	float clouds_shape_scale = 12000.0f;
	float clouds_coverage_scale = 60000.0f;
	Ref<Texture3D> clouds_detail_texture;
	float clouds_detail_scale = 1800.0f;
	float clouds_detail_strength = 0.35f;
	Vector2 clouds_wind_direction = Vector2(1, 0);
	float clouds_wind_speed = 10.0f;
	float clouds_fade_distance = 40000.0f;
	int clouds_samples = 48;
	int clouds_light_samples = 6;
	float clouds_storm = 0.0f;
	float clouds_lightning_intensity = 1.0f;
	float clouds_lightning_frequency = 0.3f;
	Color clouds_lightning_color = Color(0.75, 0.8, 1.0);

	mutable bool shader_set = false;

	int _get_shader_flags() const;
	RID get_shader_cache() const;
	static void _update_shader(int p_flags);
	void _update_shader_rid();
	void _update_clouds_textures();
	static void _create_default_clouds_textures();

protected:
	static void _bind_methods();

public:
	void set_sun_disk_scale(float p_scale);
	float get_sun_disk_scale() const;

	void set_sun_disk_intensity(float p_intensity);
	float get_sun_disk_intensity() const;

	void set_night_sky(const Ref<Texture2D> &p_night_sky);
	Ref<Texture2D> get_night_sky() const;

	void set_night_sky_energy(float p_energy);
	float get_night_sky_energy() const;

	void set_energy_multiplier(float p_multiplier);
	float get_energy_multiplier() const;

	void set_use_debanding(bool p_use_debanding);
	bool get_use_debanding() const;

	void set_clouds_enabled(bool p_enabled);
	bool is_clouds_enabled() const;

	void set_clouds_coverage(float p_coverage);
	float get_clouds_coverage() const;

	void set_clouds_density(float p_density);
	float get_clouds_density() const;

	void set_clouds_bottom_altitude(float p_altitude);
	float get_clouds_bottom_altitude() const;

	void set_clouds_thickness(float p_thickness);
	float get_clouds_thickness() const;

	void set_clouds_extinction(float p_extinction);
	float get_clouds_extinction() const;

	void set_clouds_anisotropy(float p_anisotropy);
	float get_clouds_anisotropy() const;

	void set_clouds_ambient_strength(float p_strength);
	float get_clouds_ambient_strength() const;

	void set_clouds_shape_texture(const Ref<Texture3D> &p_texture);
	Ref<Texture3D> get_clouds_shape_texture() const;

	void set_clouds_shape_scale(float p_scale);
	float get_clouds_shape_scale() const;

	void set_clouds_coverage_scale(float p_scale);
	float get_clouds_coverage_scale() const;

	void set_clouds_detail_texture(const Ref<Texture3D> &p_texture);
	Ref<Texture3D> get_clouds_detail_texture() const;

	void set_clouds_detail_scale(float p_scale);
	float get_clouds_detail_scale() const;

	void set_clouds_detail_strength(float p_strength);
	float get_clouds_detail_strength() const;

	void set_clouds_wind_direction(const Vector2 &p_direction);
	Vector2 get_clouds_wind_direction() const;

	void set_clouds_wind_speed(float p_speed);
	float get_clouds_wind_speed() const;

	void set_clouds_fade_distance(float p_distance);
	float get_clouds_fade_distance() const;

	void set_clouds_samples(int p_samples);
	int get_clouds_samples() const;

	void set_clouds_light_samples(int p_samples);
	int get_clouds_light_samples() const;

	void set_clouds_storm(float p_storm);
	float get_clouds_storm() const;

	void set_clouds_lightning_intensity(float p_intensity);
	float get_clouds_lightning_intensity() const;

	void set_clouds_lightning_frequency(float p_frequency);
	float get_clouds_lightning_frequency() const;

	void set_clouds_lightning_color(const Color &p_color);
	Color get_clouds_lightning_color() const;

	// New copies of the noise the clouds use when no texture is set, to start
	// from when making one's own.
	static Ref<Texture3D> make_default_clouds_shape_texture();
	static Ref<Texture3D> make_default_clouds_detail_texture();

	virtual Shader::Mode get_shader_mode() const override;
	virtual RID get_shader_rid() const override;
	virtual RID get_rid() const override;

	static void cleanup_shader();

	AtmosphereSkyMaterial();
};
