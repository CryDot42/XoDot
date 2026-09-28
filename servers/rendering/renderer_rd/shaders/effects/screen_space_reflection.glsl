#[compute]

#version 450

#VERSION_DEFINES

#include "../screen_space_reflection_inc.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D source_last_frame;
layout(set = 0, binding = 1) uniform sampler2D source_hiz;
layout(set = 0, binding = 2) uniform sampler2D source_normal_roughness;
layout(rgba16f, set = 0, binding = 3) uniform restrict writeonly image2D output_color;
layout(r8, set = 0, binding = 4) uniform restrict writeonly image2D output_mip_level;

layout(set = 0, binding = 5, std140) uniform SceneData {
	mat4 projection[2];
	mat4 inv_projection[2];
	mat4 reprojection[2];
	vec4 eye_offset[2];
}
scene_data;

layout(push_constant, std430) uniform Params {
	ivec2 screen_size;
	int mipmaps;
	int num_steps;
	float distance_fade;
	float curve_fade_in;
	float depth_tolerance;
	bool orthogonal;
	int view_index;
	int pad1;
	int pad2;
	int pad3;
}
params;

#define M_PI 3.14159265359
#define FLT_MAX 3.402823466e+38

// How far the ray origin is pushed away from the surface to avoid self intersections,
// in distances between neighboring pixels on the surface.
#define SELF_INTERSECTION_BIAS 8.0
// Limits how much grazing angles and depth discontinuities can stretch that distance, relative to a screen pixel.
#define SELF_INTERSECTION_MAX_STRETCH 16.0

vec2 compute_cell_count(int level) {
	int cell_count_x = max(1, params.screen_size.x >> level);
	int cell_count_y = max(1, params.screen_size.y >> level);
	return vec2(cell_count_x, cell_count_y);
}

float linearize_depth(float depth) {
	vec4 pos = vec4(0.0, 0.0, depth, 1.0);
	pos = scene_data.inv_projection[params.view_index] * pos;
	return pos.z / pos.w;
}

vec3 compute_view_pos(vec3 screen_pos) {
	vec4 pos;
	pos.xy = screen_pos.xy * 2.0 - 1.0;
	pos.z = screen_pos.z;
	pos.w = 1.0;
	pos = scene_data.inv_projection[params.view_index] * pos;
	return pos.xyz / pos.w;
}

vec3 compute_screen_pos(vec3 pos) {
	vec4 screen_pos = scene_data.projection[params.view_index] * vec4(pos, 1.0);
	screen_pos.xyz /= screen_pos.w;
	screen_pos.xy = screen_pos.xy * 0.5 + 0.5;
	return screen_pos.xyz;
}

float fetch_depth(ivec2 pixel_pos) {
	return texelFetch(source_hiz, clamp(pixel_pos, ivec2(0), params.screen_size - 1), 0).x;
}

// https://habr.com/ru/articles/744336/
vec3 compute_geometric_normal(ivec2 pixel_pos, float depth_c, vec3 view_c, out float r_surface_pixel_size) {
	vec4 H = vec4(
			fetch_depth(pixel_pos + ivec2(-1, 0)),
			fetch_depth(pixel_pos + ivec2(-2, 0)),
			fetch_depth(pixel_pos + ivec2(1, 0)),
			fetch_depth(pixel_pos + ivec2(2, 0)));

	vec4 V = vec4(
			fetch_depth(pixel_pos + ivec2(0, -1)),
			fetch_depth(pixel_pos + ivec2(0, -2)),
			fetch_depth(pixel_pos + ivec2(0, 1)),
			fetch_depth(pixel_pos + ivec2(0, 2)));

	vec2 he = abs((2.0 * H.xz - H.yw) - depth_c);
	vec2 ve = abs((2.0 * V.xz - V.yw) - depth_c);

	// Never pick a side whose samples fall outside of the screen.
	he = mix(he, vec2(FLT_MAX), bvec2(pixel_pos.x < 2, pixel_pos.x >= params.screen_size.x - 2));
	ve = mix(ve, vec2(FLT_MAX), bvec2(pixel_pos.y < 2, pixel_pos.y >= params.screen_size.y - 2));

	int h_sign = he.x < he.y ? -1 : 1;
	int v_sign = ve.x < ve.y ? -1 : 1;

	vec3 view_h = compute_view_pos(vec3((pixel_pos + vec2(h_sign, 0) + 0.5) / params.screen_size, H[1 + h_sign]));
	vec3 view_v = compute_view_pos(vec3((pixel_pos + vec2(0, v_sign) + 0.5) / params.screen_size, V[1 + v_sign]));

	vec3 h_der = h_sign * (view_h - view_c);
	vec3 v_der = v_sign * (view_v - view_c);

	r_surface_pixel_size = max(length(h_der), length(v_der));

	return cross(v_der, h_der);
}

void main() {
	ivec2 pixel_pos = ivec2(gl_GlobalInvocationID.xy);

	if (any(greaterThanEqual(pixel_pos, params.screen_size))) {
		return;
	}

	vec3 screen_pos;
	screen_pos.xy = vec2(pixel_pos + 0.5) / params.screen_size;
	screen_pos.z = texelFetch(source_hiz, pixel_pos, 0).x;

	vec4 normal_roughness = texelFetch(source_normal_roughness, pixel_pos, 0);
	float roughness = ssr_decode_roughness(normal_roughness.w);

	// Nothing to reflect on the sky. Rough materials are skipped to improve performance at the cost
	// of subtle artifacting, the scene shader fades reflections out before reaching this cutoff.
	if (screen_pos.z == 0.0 || roughness >= 0.7) {
		imageStore(output_color, pixel_pos, vec4(0.0));
		imageStore(output_mip_level, pixel_pos, vec4(0.0));
		return;
	}

	vec3 normal = ssr_decode_normal(normal_roughness.xyz);
	vec3 pos = compute_view_pos(screen_pos);
	float surface_pixel_size;
	vec3 geom_normal = normalize(compute_geometric_normal(pixel_pos, screen_pos.z, pos, surface_pixel_size));

	// Push the origin away from the surface when the shading normal deviates from the geometry, which bends the ray
	// towards the surface and causes self intersections. The offset is measured in distances between pixels on the surface,
	// so it does not depend on the scale of the scene or the distance to the camera, and grows at grazing angles where
	// the depth buffer is less precise.
	float pixel_size = distance(compute_view_pos(vec3(screen_pos.x + 1.0 / params.screen_size.x, screen_pos.yz)), pos);
	float bias = SELF_INTERSECTION_BIAS * min(surface_pixel_size, pixel_size * SELF_INTERSECTION_MAX_STRETCH);
	pos += geom_normal * (bias * (1.0 - pow(clamp(dot(normal, geom_normal), 0.0, 1.0), 8.0)));
	screen_pos = compute_screen_pos(pos);

	vec3 view_dir = params.orthogonal ? vec3(0.0, 0.0, -1.0) : normalize(pos - scene_data.eye_offset[params.view_index].xyz);
	vec3 ray_dir = normalize(reflect(view_dir, normal));

	// Check if the ray is immediately intersecting with itself. If so, bounce!
	if (dot(ray_dir, geom_normal) < 0.0) {
		ray_dir = normalize(reflect(ray_dir, geom_normal));
	}

	vec3 end_pos = pos + ray_dir;

	// Clip to near plane. Add a small bias so we don't go to infinity.
	if (end_pos.z > 0.0) {
		end_pos -= ray_dir / ray_dir.z * (end_pos.z + 0.00001);
	}

	vec3 screen_end_pos = compute_screen_pos(end_pos);

	// Parametric T tracing, as suggested here:
	// https://hacksoflife.blogspot.com/2020/10/a-tip-for-hiz-ssr-parametric-t-tracing.html
	// The direction is normalized so that T is the distance traveled in UV space. Unlike normalizing
	// the depth component, this stays well defined for rays running parallel to the screen plane
	// (common with orthogonal cameras).
	vec3 screen_ray_dir = screen_end_pos - screen_pos;
	float screen_ray_len = length(screen_ray_dir.xy);

	if (!(screen_ray_len > 1e-6)) {
		// The ray points straight at (or away from) the camera, so it stays on this pixel.
		imageStore(output_color, pixel_pos, vec4(0.0));
		imageStore(output_mip_level, pixel_pos, vec4(0.0));
		return;
	}

	screen_ray_dir /= screen_ray_len;

	// Components that are zero never let the ray reach the corresponding planes.
	vec3 inv_ray_dir = mix(1.0 / screen_ray_dir, vec3(FLT_MAX), equal(screen_ray_dir, vec3(0.0)));

	bool facing_camera = screen_ray_dir.z >= 0.0;

	// Stop tracing where the ray leaves the screen or the depth range.
	vec2 t0 = (vec2(0.0) - screen_pos.xy) * inv_ray_dir.xy;
	vec2 t1 = (vec2(1.0) - screen_pos.xy) * inv_ray_dir.xy;
	vec2 t2 = max(t0, t1);
	float t_max = min(t2.x, t2.y);
	if (screen_ray_dir.z != 0.0) {
		t_max = min(t_max, ((facing_camera ? 1.0 : 0.0) - screen_pos.z) * inv_ray_dir.z);
	}

	vec2 cell_step = vec2(screen_ray_dir.x < 0.0 ? -1.0 : 1.0, screen_ray_dir.y < 0.0 ? -1.0 : 1.0);
	vec2 cell_offset = max(cell_step, vec2(0.0));
	vec2 cell_bias = cell_step * 0.000001;

	// Advance the start point to the closest next cell to prevent immediate self intersection.
	float t;
	{
		vec2 cell_index = floor(screen_pos.xy * params.screen_size);
		vec2 new_cell_pos = (cell_index + cell_offset) / params.screen_size + cell_bias;
		vec2 pos_t = (new_cell_pos - screen_pos.xy) * inv_ray_dir.xy;
		t = min(pos_t.x, pos_t.y);
	}

	int cur_level = 0;
	int cur_iteration = params.num_steps;

	while (cur_level >= 0 && cur_iteration > 0 && t < t_max) {
		vec3 cur_screen_pos = screen_pos + screen_ray_dir * t;

		vec2 cell_count = compute_cell_count(cur_level);
		vec2 cell_index = min(floor(cur_screen_pos.xy * cell_count), cell_count - 1.0);
		float cell_depth = texelFetch(source_hiz, ivec2(cell_index), cur_level).x;
		float depth_t = (cell_depth - screen_pos.z) * inv_ray_dir.z;

		vec2 new_cell_pos = (cell_index + cell_offset) / cell_count + cell_bias;
		vec2 pos_t = (new_cell_pos - screen_pos.xy) * inv_ray_dir.xy;
		float edge_t = min(pos_t.x, pos_t.y);

		bool hit = facing_camera ? (t <= depth_t) : (depth_t <= edge_t);
		int mip_offset = hit ? -1 : +1;

		if (cur_level == 0) {
			float z0 = linearize_depth(cell_depth);
			float z1 = linearize_depth(cur_screen_pos.z);

			if ((z0 - z1) > params.depth_tolerance) {
				hit = false;
				mip_offset = 0; // Keep the mip index the same to prevent it from decreasing and increasing in repeat.
			}
		}

		if (hit) {
			if (!facing_camera) {
				t = max(t, depth_t);
			}
		} else {
			t = edge_t;
		}

		cur_level = min(cur_level + mip_offset, params.mipmaps - 1);
		--cur_iteration;
	}

	vec3 hit_screen_pos = screen_pos + screen_ray_dir * t;
	float ray_len = t; // Distance traveled in UV space.

	// Instead of hard rejecting samples, write sample validity to the alpha channel.
	// This allows invalid samples to write mip levels to let valid samples have smoother roughness transitions.
	// Hit validation logic is referenced from here:
	// https://github.com/GPUOpen-Effects/FidelityFX-SSSR/blob/master/ffx-sssr/ffx_sssr.h
	float validity = t < t_max ? 1.0 : 0.0;

	ivec2 hit_pixel_pos = clamp(ivec2(hit_screen_pos.xy * params.screen_size), ivec2(0), params.screen_size - 1);
	float hit_depth = texelFetch(source_hiz, hit_pixel_pos, 0).x;
	if (hit_depth == 0.0) {
		validity = 0.0;
	}

	if (all(lessThan(abs(screen_ray_dir.xy * t), 2.0 / params.screen_size))) {
		vec3 hit_normal = texelFetch(source_normal_roughness, hit_pixel_pos, 0).xyz * 2.0 - 1.0;
		if (dot(ray_dir, hit_normal) >= 0.0) {
			validity = 0.0;
		}
	}

	vec3 cur_pos = compute_view_pos(hit_screen_pos);
	vec3 hit_pos = compute_view_pos(vec3(hit_screen_pos.xy, hit_depth));

	float delta = length(cur_pos - hit_pos);
	float confidence = 1.0 - smoothstep(0.0, params.depth_tolerance, delta);
	validity *= clamp(confidence * confidence, 0.0, 1.0);

	// Find where the surface that was hit was on the previous frame, that is where its color is read from.
	vec4 reprojected_pos = scene_data.reprojection[params.view_index] * vec4(hit_screen_pos.xy * 2.0 - 1.0, hit_depth, 1.0);
	if (reprojected_pos.w <= 0.0) {
		validity = 0.0; // Behind the previous camera.
	}
	reprojected_pos.xy = reprojected_pos.xy / reprojected_pos.w * 0.5 + 0.5;

	// Fade out when getting close to the edges of the previous frame, using a uniform margin.
	// Samples outside of it are rejected, as it has no data there.
	vec2 reprojected_pixel_pos = reprojected_pos.xy * params.screen_size;
	vec2 edge_dist = min(reprojected_pixel_pos, params.screen_size - reprojected_pixel_pos);
	float margin = (params.screen_size.x + params.screen_size.y) * 0.05;
	float margin_blend = all(greaterThan(edge_dist, vec2(0.0))) ? smoothstep(0.0, margin * margin, edge_dist.x * edge_dist.y) : 0.0;

	// Fade In / Fade Out
	float fade_in = params.curve_fade_in == 0.0 ? 1.0 : pow(clamp(ray_len, 0.0, 1.0), params.curve_fade_in);
	float fade_out = params.distance_fade == 0.0 ? 1.0 : pow(clamp(1.0 - ray_len, 0.0, 1.0), params.distance_fade);
	float fade = fade_in * fade_out;

	// Ensure that precision errors do not introduce any fade. Even if it is just slightly below 1.0,
	// strong specular light can leak through the reflection.
	if (fade > 0.999) {
		fade = 1.0;
	}

	validity *= fade * margin_blend;

	vec4 color = vec4(0.0);
	if (validity > 0.0) {
		// Negative values can make the tone mapping below divide by zero.
		vec3 hit_color = max(textureLod(source_last_frame, reprojected_pos.xy, 0.0).rgb, vec3(0.0));
		color = vec4(ssr_tonemap(hit_color * validity), validity);
	}

	float mip_level = 0.0;
	if (roughness > 0.001) {
		// Fit a sphere inside the reflection cone, ending at its base, something like this:
		// ___
		// \O/
		//  V
		//
		// as it avoids bleeding from beyond the reflection as much as possible. As a plus
		// it also makes the rough reflection more elongated.
		// For a cone of length h and half angle a the radius of that sphere is h * sin(a) / (1 + sin(a)).
		float cone_angle = min(roughness, 0.999) * M_PI * 0.5;
		float sin_cone_angle = sin(cone_angle);
		float blur_radius = ray_len * sin_cone_angle / (1.0 + sin_cone_angle);

		// We approximate the integration in world space with a blur in screen space,
		// and use a mip bias, `log2(/* screen_space_blur_radius */ + 1.0)`, to approximate the screen space blur.
		// This + 1.0 is needed because mip level is logarithmic to the diameter (in pixels) of sampling region,
		// which is 1 pixel when no blur is applied (log2(1) = 0).
		mip_level = clamp(log2(blur_radius * max(params.screen_size.x, params.screen_size.y) / 16.0 + 1.0), 0.0, float(params.mipmaps - 1));
	}

	// Because we still write mip level for invalid pixels to allow for smooth roughness transitions,
	// this sometimes ends up creating a pyramid-like shape at very rough levels.
	// We can fade the mip level near the end to make it significantly less visible.
	mip_level *= pow(clamp(1.25 - ray_len, 0.0, 1.0), 0.2);

	imageStore(output_color, pixel_pos, color);
	imageStore(output_mip_level, pixel_pos, vec4(mip_level / SSR_MIP_LEVEL_RANGE, 0.0, 0.0, 0.0));
}
