#[compute]

#version 450

#VERSION_DEFINES

// Builds the atmosphere's lookup tables, one mode per table:
// - MODE_TRANSMITTANCE: from any height, towards any zenith angle, to space.
// - MODE_MULTISCATTERING: Hillaire's multiple scattering approximation.
// - MODE_SKY_VIEW: the sky's luminance around the eye, for the sky itself.
// - MODE_AERIAL_PERSPECTIVE: luminance and transmittance through the view
//   frustum, for everything seen through the air.
// The first two only change with the atmosphere's parameters; the others
// follow the eye and the lights every frame.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "atmosphere_data_inc.glsl"

layout(set = 0, binding = 0, std140) uniform AtmosphereBlock {
	AtmosphereData atmosphere;
};
layout(set = 0, binding = 1) uniform sampler2D transmittance_lut;
layout(set = 0, binding = 2) uniform sampler2D multiscattering_lut;

#if defined(MODE_AERIAL_PERSPECTIVE)
layout(rgba16f, set = 1, binding = 0) uniform restrict writeonly image3D dest;
#else
layout(rgba16f, set = 1, binding = 0) uniform restrict writeonly image2D dest;
#endif

#define ATMOSPHERE_TRANSMITTANCE_LUT(m_uv) textureLod(transmittance_lut, m_uv, 0.0)
#define ATMOSPHERE_MULTISCATTERING_LUT(m_uv) textureLod(multiscattering_lut, m_uv, 0.0)

#include "atmosphere_inc.glsl"

// How much of a light at p_light_dir reaches p_position: the planet's shadow,
// then the air in between.
vec3 light_transmittance(vec3 p_position, vec3 p_light_dir) {
	if (atmosphere_ray_sphere_nearest(p_position, p_light_dir, atmosphere.bottom_radius) >= 0.0) {
		return vec3(0.0);
	}
	return atmosphere_transmittance_to_top(p_position, p_light_dir);
}

// The length of the ray through the atmosphere, up to the ground or out to
// space, from an eye that may be above it. r_offset is where it enters it.
float ray_length(vec3 p_origin, vec3 p_dir, out float r_offset, out bool r_hits_ground) {
	r_offset = 0.0;
	r_hits_ground = false;
	float height = length(p_origin);
	if (height > atmosphere.top_radius) {
		float entry = atmosphere_ray_sphere_nearest(p_origin, p_dir, atmosphere.top_radius);
		if (entry < 0.0) {
			return 0.0;
		}
		r_offset = entry;
		p_origin += p_dir * entry;
	}
	float ground = atmosphere_ray_sphere_nearest(p_origin, p_dir, atmosphere.bottom_radius);
	float top = atmosphere_ray_sphere_nearest(p_origin, p_dir, atmosphere.top_radius);
	if (ground > 0.0) {
		r_hits_ground = true;
		return ground;
	}
	return max(0.0, top);
}

#if defined(MODE_TRANSMITTANCE)

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, ivec2(ATMOSPHERE_TRANSMITTANCE_LUT_SIZE)))) {
		return;
	}
	vec2 uv = (vec2(pos) + 0.5) / ATMOSPHERE_TRANSMITTANCE_LUT_SIZE;
	float height;
	float cos_zenith;
	atmosphere_uv_to_transmittance_params(uv, height, cos_zenith);

	vec3 origin = vec3(0.0, height, 0.0);
	vec3 dir = vec3(sqrt(max(0.0, 1.0 - cos_zenith * cos_zenith)), cos_zenith, 0.0);
	float length_to_top = max(0.0, atmosphere_ray_sphere_nearest(origin, dir, atmosphere.top_radius));

	const int SAMPLES = 40;
	vec3 optical_depth = vec3(0.0);
	float dt = length_to_top / float(SAMPLES);
	for (int i = 0; i < SAMPLES; i++) {
		optical_depth += atmosphere_sample_medium(origin + dir * ((float(i) + 0.5) * dt)).extinction * dt;
	}
	imageStore(dest, pos, vec4(exp(-optical_depth), 1.0));
}

#elif defined(MODE_MULTISCATTERING)

// Hillaire 2020, section 5.5: light scattered twice, integrated over every
// direction around a point with an isotropic phase, and the geometric series
// of all the orders above it, whose ratio is the fraction of light that gets
// scattered again (f_ms).
void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, ivec2(ATMOSPHERE_MULTISCATTERING_LUT_SIZE)))) {
		return;
	}
	vec2 uv = (vec2(pos) + 0.5) / ATMOSPHERE_MULTISCATTERING_LUT_SIZE;
	uv = vec2(atmosphere_sub_uv_to_unit(uv.x, ATMOSPHERE_MULTISCATTERING_LUT_SIZE), atmosphere_sub_uv_to_unit(uv.y, ATMOSPHERE_MULTISCATTERING_LUT_SIZE));

	float cos_sun_zenith = uv.x * 2.0 - 1.0;
	vec3 sun_dir = vec3(0.0, cos_sun_zenith, sqrt(clamp(1.0 - cos_sun_zenith * cos_sun_zenith, 0.0, 1.0)));
	float height = atmosphere.bottom_radius + clamp(uv.y + 1e-4, 0.0, 1.0) * (atmosphere.top_radius - atmosphere.bottom_radius - 1e-3);
	vec3 origin = vec3(0.0, height, 0.0);

	const float SPHERE_SOLID_ANGLE = 4.0 * ATMOSPHERE_PI;
	const float ISOTROPIC_PHASE = 1.0 / SPHERE_SOLID_ANGLE;
	const int SQRT_DIRECTIONS = 8;
	const int SAMPLES = 20;

	vec3 luminance = vec3(0.0);
	vec3 multi_scattering_as_one = vec3(0.0);
	for (int i = 0; i < SQRT_DIRECTIONS; i++) {
		for (int j = 0; j < SQRT_DIRECTIONS; j++) {
			float theta = 2.0 * ATMOSPHERE_PI * (float(i) + 0.5) / float(SQRT_DIRECTIONS);
			float phi = acos(1.0 - 2.0 * (float(j) + 0.5) / float(SQRT_DIRECTIONS));
			vec3 dir = vec3(cos(theta) * sin(phi), cos(phi), sin(theta) * sin(phi));

			float offset;
			bool hits_ground;
			float length_along = ray_length(origin, dir, offset, hits_ground);
			float dt = length_along / float(SAMPLES);

			vec3 throughput = vec3(1.0);
			vec3 l = vec3(0.0);
			vec3 ms = vec3(0.0);
			for (int s = 0; s < SAMPLES; s++) {
				vec3 p = origin + dir * ((float(s) + 0.3) * dt);
				AtmosphereMedium medium = atmosphere_sample_medium(p);
				vec3 step_transmittance = exp(-medium.extinction * dt);
				vec3 extinction = max(medium.extinction, vec3(1e-7));

				vec3 in_scattering = light_transmittance(p, sun_dir) * medium.scattering * ISOTROPIC_PHASE;
				l += throughput * (in_scattering - in_scattering * step_transmittance) / extinction;
				ms += throughput * (medium.scattering - medium.scattering * step_transmittance) / extinction;
				throughput *= step_transmittance;
			}
			if (hits_ground) {
				vec3 ground = origin + dir * length_along;
				vec3 normal = normalize(ground);
				l += throughput * atmosphere_transmittance_to_top(ground, sun_dir) * clamp(dot(normal, sun_dir), 0.0, 1.0) * atmosphere.ground_albedo / ATMOSPHERE_PI;
			}
			luminance += l;
			multi_scattering_as_one += ms;
		}
	}
	float weight = SPHERE_SOLID_ANGLE / float(SQRT_DIRECTIONS * SQRT_DIRECTIONS);
	luminance *= weight * ISOTROPIC_PHASE;
	vec3 f_ms = multi_scattering_as_one * weight * ISOTROPIC_PHASE;
	vec3 psi = luminance / max(vec3(1e-4), 1.0 - f_ms);

	imageStore(dest, pos, vec4(psi * atmosphere.multiscattering_factor, 1.0));
}

#else

// Single scattering from every light, plus the multiple scattering table, for
// one ray from p_origin. Returns the luminance, and in r_transmittance what
// is left of what lies at the end of it.
vec3 scattered_luminance(vec3 p_origin, vec3 p_dir, float p_start, float p_end, int p_samples, out vec3 r_transmittance) {
	vec3 luminance = vec3(0.0);
	vec3 throughput = vec3(1.0);
	float dt = (p_end - p_start) / float(p_samples);
	for (int s = 0; s < p_samples; s++) {
		vec3 p = p_origin + p_dir * (p_start + (float(s) + 0.3) * dt);
		AtmosphereMedium medium = atmosphere_sample_medium(p);
		vec3 step_transmittance = exp(-medium.extinction * dt);
		vec3 extinction = max(medium.extinction, vec3(1e-7));

		vec3 in_scattering = vec3(0.0);
		for (uint i = 0; i < atmosphere.light_count; i++) {
			vec3 light_dir = atmosphere.light_direction[i].xyz;
			float cos_theta = dot(p_dir, light_dir);
			vec3 phase_scattering = medium.scattering_mie * atmosphere_mie_phase(atmosphere.mie_phase_g, cos_theta) + medium.scattering_rayleigh * atmosphere_rayleigh_phase(cos_theta);
			vec3 multiple = atmosphere_multiple_scattering(p, light_dir) * medium.scattering;
			in_scattering += atmosphere.light_illuminance[i].rgb * (light_transmittance(p, light_dir) * phase_scattering + multiple);
		}
		luminance += throughput * (in_scattering - in_scattering * step_transmittance) / extinction;
		throughput *= step_transmittance;
	}
	r_transmittance = throughput;
	return luminance;
}

#if defined(MODE_SKY_VIEW)

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, ivec2(ATMOSPHERE_SKY_VIEW_LUT_SIZE)))) {
		return;
	}
	vec2 uv = (vec2(pos) + 0.5) / ATMOSPHERE_SKY_VIEW_LUT_SIZE;
	vec3 origin = atmosphere.camera_position;
	vec3 dir = atmosphere_sky_view_uv_to_dir(origin, uv);

	float offset;
	bool hits_ground;
	float length_along = ray_length(origin, dir, offset, hits_ground);
	vec3 luminance = vec3(0.0);
	if (length_along > 0.0) {
		// More samples for the long, grazing rays near the horizon.
		int samples = int(mix(16.0, 32.0, clamp(length_along / 150.0, 0.0, 1.0)));
		vec3 transmittance;
		luminance = scattered_luminance(origin, dir, offset, offset + length_along, samples, transmittance);
	}
	imageStore(dest, pos, vec4(luminance, 1.0));
}

#elif defined(MODE_AERIAL_PERSPECTIVE)

// One thread per column of the volume, marching along it and writing every
// slice it goes through, so that each slice holds what lies between the eye
// and it.
void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	int size = int(ATMOSPHERE_AERIAL_PERSPECTIVE_SIZE);
	if (any(greaterThanEqual(pos, ivec2(size)))) {
		return;
	}
	vec2 uv = (vec2(pos) + 0.5) / ATMOSPHERE_AERIAL_PERSPECTIVE_SIZE;
	// Screen UVs run down from the top; normalized device coordinates run up.
	vec4 view = atmosphere.inv_projection * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
	vec3 dir = normalize(mat3(atmosphere.camera_basis) * normalize(view.xyz / view.w));

	vec3 origin = atmosphere.camera_position;
	float offset;
	bool hits_ground;
	float length_along = ray_length(origin, dir, offset, hits_ground);

	vec3 luminance = vec3(0.0);
	vec3 transmittance = vec3(1.0);
	float previous = atmosphere.aerial_perspective_start_depth;
	for (int slice = 0; slice < size; slice++) {
		float depth = max(atmosphere.aerial_perspective_start_depth, atmosphere_aerial_perspective_slice_to_depth(float(slice) + 0.5));
		float end = min(depth, offset + length_along);
		if (end > previous) {
			vec3 step_transmittance;
			vec3 step_luminance = scattered_luminance(origin, dir, previous, end, 2, step_transmittance);
			luminance += transmittance * step_luminance;
			transmittance *= step_transmittance;
			previous = end;
		}
		imageStore(dest, ivec3(pos, slice), vec4(luminance, dot(transmittance, vec3(1.0 / 3.0))));
	}
}

#endif

#endif
