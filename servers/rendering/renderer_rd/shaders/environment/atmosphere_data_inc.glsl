// The atmosphere's parameters and table sizes, shared by the passes that build
// its lookup tables and everything that samples them. Include it before
// declaring the uniform block atmosphere_inc.glsl reads `atmosphere` from.

#define ATMOSPHERE_PI 3.14159265359

#define ATMOSPHERE_TRANSMITTANCE_LUT_SIZE vec2(256.0, 64.0)
#define ATMOSPHERE_MULTISCATTERING_LUT_SIZE 32.0
#define ATMOSPHERE_SKY_VIEW_LUT_SIZE vec2(192.0, 108.0)
#define ATMOSPHERE_AERIAL_PERSPECTIVE_SIZE 32.0
// Kilometers the aerial perspective volume reaches, at a distance scale of 1.
#define ATMOSPHERE_AERIAL_PERSPECTIVE_DEPTH 32.0

// Keep in sync with AtmosphereRD::AtmosphereData.
struct AtmosphereData {
	vec3 rayleigh_scattering;
	float bottom_radius;

	vec3 mie_scattering;
	float top_radius;

	vec3 mie_extinction;
	float mie_phase_g;

	vec3 absorption_extinction;
	float rayleigh_density_exp_scale;

	vec3 ground_albedo;
	float mie_density_exp_scale;

	vec3 sky_luminance_factor;
	float absorption_tip_altitude;

	vec3 camera_position; // Planet-centered, kilometers.
	float absorption_width;

	float multiscattering_factor;
	float aerial_perspective_distance_scale;
	float aerial_perspective_start_depth;
	uint light_count;

	vec4 light_direction[2]; // Towards the light.
	vec4 light_illuminance[2];

	mat4 inv_projection;
	mat4 camera_basis;

	float world_to_km;
	uint enabled;
	float pad0;
	float pad1;
};
