// A planet's atmosphere, after Sébastien Hillaire, "A Scalable and Production
// Ready Sky and Atmosphere Rendering Technique" (EGSR 2020), which builds on
// Eric Bruneton and Fabrice Neyret, "Precomputed Atmospheric Scattering"
// (EGSR 2008) for its medium and its transmittance parameterization.
//
// This file is shared by everything that looks at the atmosphere: the passes
// that build its lookup tables (atmosphere.glsl), the sky (sky.glsl), the
// aerial perspective applied to the scene, and whatever is added on top of it
// later, such as volumetric clouds, which need the very same transmittance
// and multiple scattering to be lit consistently with the sky around them.
//
// Units: every distance is in kilometers and every coefficient per kilometer.
// The planet's center is below the world origin, so that the ground is at
// y = 0; atmosphere_world_to_planet() takes a world position there.
//
// The includer declares, before including this file:
// - atmosphere_data_inc.glsl, then a uniform block holding an AtmosphereData
//   named `atmosphere`,
// - ATMOSPHERE_TRANSMITTANCE_LUT, ATMOSPHERE_MULTISCATTERING_LUT: expressions
//   sampling those tables at a vec2 uv (only where they are used).

// Geometry.

// Distance to the nearest intersection of a ray with a sphere centered on
// the origin, or -1 if it misses it or the sphere is behind.
float atmosphere_ray_sphere_nearest(vec3 p_origin, vec3 p_dir, float p_radius) {
	float b = dot(p_dir, p_origin);
	float c = dot(p_origin, p_origin) - p_radius * p_radius;
	float delta = b * b - c;
	if (delta < 0.0) {
		return -1.0;
	}
	float sqrt_delta = sqrt(delta);
	float t0 = -b - sqrt_delta;
	float t1 = -b + sqrt_delta;
	if (t0 < 0.0 && t1 < 0.0) {
		return -1.0;
	}
	if (t0 < 0.0) {
		return max(0.0, t1);
	}
	return max(0.0, t0);
}

vec3 atmosphere_world_to_planet(vec3 p_world) {
	vec3 position = p_world * atmosphere.world_to_km + vec3(0.0, atmosphere.bottom_radius, 0.0);
	// Keep the eye a hair above the ground, where all the parameterizations
	// below hold.
	float height = length(position);
	return position * (max(height, atmosphere.bottom_radius + 0.0005) / max(height, 1e-6));
}

// The medium.

struct AtmosphereMedium {
	vec3 scattering;
	vec3 extinction;
	vec3 scattering_mie;
	vec3 scattering_rayleigh;
};

AtmosphereMedium atmosphere_sample_medium(vec3 p_position) {
	float height = max(0.0, length(p_position) - atmosphere.bottom_radius);

	float density_mie = exp(atmosphere.mie_density_exp_scale * height);
	float density_rayleigh = exp(atmosphere.rayleigh_density_exp_scale * height);
	// Ozone: a tent peaking at its tip altitude.
	float density_ozone = max(0.0, 1.0 - abs(height - atmosphere.absorption_tip_altitude) / (atmosphere.absorption_width * 0.5));

	AtmosphereMedium medium;
	medium.scattering_mie = density_mie * atmosphere.mie_scattering;
	medium.scattering_rayleigh = density_rayleigh * atmosphere.rayleigh_scattering;
	medium.scattering = medium.scattering_mie + medium.scattering_rayleigh;
	medium.extinction = density_mie * atmosphere.mie_extinction + medium.scattering_rayleigh + density_ozone * atmosphere.absorption_extinction;
	return medium;
}

float atmosphere_rayleigh_phase(float p_cos_theta) {
	return 3.0 / (16.0 * ATMOSPHERE_PI) * (1.0 + p_cos_theta * p_cos_theta);
}

// Cornette-Shanks, a Henyey-Greenstein with a better fit for the forward peak.
float atmosphere_mie_phase(float p_g, float p_cos_theta) {
	float g2 = p_g * p_g;
	float k = 3.0 / (8.0 * ATMOSPHERE_PI) * (1.0 - g2) / (2.0 + g2);
	return k * (1.0 + p_cos_theta * p_cos_theta) / pow(max(1e-4, 1.0 + g2 - 2.0 * p_g * p_cos_theta), 1.5);
}

// Lookup table parameterizations.

// Maps [0, 1] onto texel centers, so that both ends of the range are sampled
// exactly.
float atmosphere_unit_to_sub_uv(float p_u, float p_resolution) {
	return (p_u + 0.5 / p_resolution) * (p_resolution / (p_resolution + 1.0));
}

float atmosphere_sub_uv_to_unit(float p_u, float p_resolution) {
	return (p_u - 0.5 / p_resolution) * (p_resolution / (p_resolution - 1.0));
}

// Bruneton's transmittance parameterization: height, and the cosine of the
// view's zenith angle, remapped for precision near the horizon.
vec2 atmosphere_transmittance_params_to_uv(float p_height, float p_cos_zenith) {
	float h = sqrt(max(0.0, atmosphere.top_radius * atmosphere.top_radius - atmosphere.bottom_radius * atmosphere.bottom_radius));
	float rho = sqrt(max(0.0, p_height * p_height - atmosphere.bottom_radius * atmosphere.bottom_radius));
	float discriminant = p_height * p_height * (p_cos_zenith * p_cos_zenith - 1.0) + atmosphere.top_radius * atmosphere.top_radius;
	float d = max(0.0, -p_height * p_cos_zenith + sqrt(max(0.0, discriminant)));
	float d_min = atmosphere.top_radius - p_height;
	float d_max = rho + h;
	return vec2((d - d_min) / max(1e-6, d_max - d_min), rho / max(1e-6, h));
}

void atmosphere_uv_to_transmittance_params(vec2 p_uv, out float r_height, out float r_cos_zenith) {
	float h = sqrt(max(0.0, atmosphere.top_radius * atmosphere.top_radius - atmosphere.bottom_radius * atmosphere.bottom_radius));
	float rho = h * p_uv.y;
	r_height = sqrt(rho * rho + atmosphere.bottom_radius * atmosphere.bottom_radius);
	float d_min = atmosphere.top_radius - r_height;
	float d_max = rho + h;
	float d = d_min + p_uv.x * (d_max - d_min);
	r_cos_zenith = d == 0.0 ? 1.0 : (h * h - rho * rho - d * d) / (2.0 * r_height * d);
	r_cos_zenith = clamp(r_cos_zenith, -1.0, 1.0);
}

#ifdef ATMOSPHERE_TRANSMITTANCE_LUT
// Transmittance from p_position to the top of the atmosphere along p_dir.
vec3 atmosphere_transmittance_to_top(vec3 p_position, vec3 p_dir) {
	float height = length(p_position);
	vec2 uv = atmosphere_transmittance_params_to_uv(height, dot(p_position / height, p_dir));
	return ATMOSPHERE_TRANSMITTANCE_LUT(uv).rgb;
}
#endif

#ifdef ATMOSPHERE_MULTISCATTERING_LUT
// The luminance scattered any number of times more than once towards
// p_position, per unit of illuminance coming from p_light_dir (Hillaire's Ψms).
vec3 atmosphere_multiple_scattering(vec3 p_position, vec3 p_light_dir) {
	float height = length(p_position);
	vec2 uv = vec2(dot(p_position / height, p_light_dir) * 0.5 + 0.5, (height - atmosphere.bottom_radius) / (atmosphere.top_radius - atmosphere.bottom_radius));
	uv = clamp(uv, 0.0, 1.0);
	uv = vec2(atmosphere_unit_to_sub_uv(uv.x, ATMOSPHERE_MULTISCATTERING_LUT_SIZE), atmosphere_unit_to_sub_uv(uv.y, ATMOSPHERE_MULTISCATTERING_LUT_SIZE));
	return ATMOSPHERE_MULTISCATTERING_LUT(uv).rgb;
}
#endif

// The sky-view table: every direction around the eye, with its full 360
// degrees of azimuth (so that several lights can share it), and a zenith
// angle packed tighter towards the horizon, where the sky changes fastest.
// The horizon itself dips below 90 degrees as the eye climbs.
vec2 atmosphere_sky_view_dir_to_uv(vec3 p_camera, vec3 p_dir) {
	float height = length(p_camera);
	vec3 up = p_camera / height;
	float horizon = sqrt(max(0.0, height * height - atmosphere.bottom_radius * atmosphere.bottom_radius));
	float beta = acos(clamp(horizon / height, -1.0, 1.0));
	float zenith_horizon_angle = ATMOSPHERE_PI - beta;
	float view_zenith_angle = acos(clamp(dot(up, p_dir), -1.0, 1.0));

	vec2 uv;
	if (view_zenith_angle < zenith_horizon_angle) {
		float coord = view_zenith_angle / zenith_horizon_angle;
		coord = 1.0 - sqrt(max(0.0, 1.0 - coord));
		uv.y = coord * 0.5;
	} else {
		float coord = (view_zenith_angle - zenith_horizon_angle) / max(1e-6, beta);
		uv.y = sqrt(max(0.0, coord)) * 0.5 + 0.5;
	}
	uv.x = atan(p_dir.z, p_dir.x) / (2.0 * ATMOSPHERE_PI) + 0.5;

	return vec2(atmosphere_unit_to_sub_uv(uv.x, ATMOSPHERE_SKY_VIEW_LUT_SIZE.x), atmosphere_unit_to_sub_uv(uv.y, ATMOSPHERE_SKY_VIEW_LUT_SIZE.y));
}

vec3 atmosphere_sky_view_uv_to_dir(vec3 p_camera, vec2 p_uv) {
	vec2 uv = vec2(atmosphere_sub_uv_to_unit(p_uv.x, ATMOSPHERE_SKY_VIEW_LUT_SIZE.x), atmosphere_sub_uv_to_unit(p_uv.y, ATMOSPHERE_SKY_VIEW_LUT_SIZE.y));
	float height = length(p_camera);
	float horizon = sqrt(max(0.0, height * height - atmosphere.bottom_radius * atmosphere.bottom_radius));
	float beta = acos(clamp(horizon / height, -1.0, 1.0));
	float zenith_horizon_angle = ATMOSPHERE_PI - beta;

	float view_zenith_angle;
	if (uv.y < 0.5) {
		float coord = 1.0 - 2.0 * uv.y;
		coord = 1.0 - coord * coord;
		view_zenith_angle = zenith_horizon_angle * coord;
	} else {
		float coord = uv.y * 2.0 - 1.0;
		view_zenith_angle = zenith_horizon_angle + beta * coord * coord;
	}
	float azimuth = (uv.x - 0.5) * 2.0 * ATMOSPHERE_PI;
	float sin_zenith = sin(view_zenith_angle);
	// In the frame of the eye, whose up is the planet's up there; the eye is
	// always close enough to the world origin for that to be world +Y.
	return vec3(sin_zenith * cos(azimuth), cos(view_zenith_angle), sin_zenith * sin(azimuth));
}

// The aerial perspective volume's slices are spread quadratically, finer
// close to the eye where most of the scene is.
float atmosphere_aerial_perspective_slice_to_depth(float p_slice) {
	float x = p_slice / ATMOSPHERE_AERIAL_PERSPECTIVE_SIZE;
	return x * x * ATMOSPHERE_AERIAL_PERSPECTIVE_DEPTH / max(1e-4, atmosphere.aerial_perspective_distance_scale);
}

float atmosphere_aerial_perspective_depth_to_w(float p_depth_km) {
	float x = sqrt(max(0.0, p_depth_km * atmosphere.aerial_perspective_distance_scale / ATMOSPHERE_AERIAL_PERSPECTIVE_DEPTH));
	return x;
}
