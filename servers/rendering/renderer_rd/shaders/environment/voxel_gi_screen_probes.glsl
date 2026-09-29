#[compute]

#version 450

#VERSION_DEFINES

#extension GL_EXT_samplerless_texture_functions : enable

// Screen probe gather for VoxelGI: probes are placed on the depth buffer, every probe traces a hemisphere of
// rays that first march the depth buffer (lit by the previous frame) and continue through the VoxelGI volume
// when they leave the screen or pass behind geometry. The result is stored as L1 spherical harmonics.

#define PROBE_RAY_COUNT 64
#define PROBE_RAY_SIDE 8

#ifdef MODE_TRACE
layout(local_size_x = PROBE_RAY_COUNT, local_size_y = 1, local_size_z = 1) in;
#else
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
#endif

#include "screen_probes_inc.glsl"

#define M_PI 3.14159265359
#define MAX_VOXEL_GI_INSTANCES 8

layout(set = 0, binding = 0) uniform texture2D depth_buffer;
layout(set = 0, binding = 1) uniform texture2D normal_roughness_buffer;

#ifdef MODE_TRACE

layout(set = 0, binding = 2) uniform utexture2D voxel_gi_buffer;
layout(set = 0, binding = 3) uniform texture2D last_frame;

struct VoxelGIData {
	mat4 xform; // 64 - 64

	vec3 bounds; // 12 - 76
	float dynamic_range; // 4 - 80

	float bias; // 4 - 84
	float normal_bias; // 4 - 88
	bool blend_ambient; // 4 - 92
	uint mipmaps; // 4 - 96

	vec3 aniso; // 12 - 108 (enabled, slab axis, slab size)
	float exposure_normalization; // 4 - 112
};

layout(set = 0, binding = 4, std140) uniform VoxelGIs {
	VoxelGIData data[MAX_VOXEL_GI_INSTANCES];
}
voxel_gi_instances;

layout(set = 0, binding = 5) uniform texture3D voxel_gi_textures[MAX_VOXEL_GI_INSTANCES];

layout(set = 0, binding = 7) uniform sampler linear_sampler_with_mipmaps;

#endif // MODE_TRACE

layout(set = 0, binding = 6) uniform sampler linear_sampler;

layout(rgba16f, set = 0, binding = 8) uniform restrict writeonly image2DArray probe_sh;

#ifdef MODE_FILTER
layout(set = 0, binding = 11) uniform texture2DArray source_probe_sh;
#endif

layout(set = 0, binding = 9, std140) uniform Params {
	mat4 inv_projection; // Clip to view space.
	mat4 projection; // View to clip space.
	mat4 reprojection; // Camera relative world space to the clip space of the previous frame.
	mat4 cam_basis; // View to camera relative world space (rotation only).

	ivec2 screen_size;
	ivec2 probe_grid_size;

	uint probe_spacing;
	uint frame;
	uint max_voxel_gi_instances;
	uint screen_trace_steps;

	float screen_trace_distance;
	float z_near;
	float pixel_size; // World size of a pixel one unit away from the camera.
	float last_frame_max_lod;

	uint orthogonal;
	uint has_last_frame;
	uint pad0;
	uint pad1;
}
params;

vec3 reconstruct_view_position(vec2 p_uv, float p_depth) {
	vec4 pos = params.inv_projection * vec4(p_uv * 2.0 - 1.0, p_depth, 1.0);
	return pos.xyz / pos.w;
}

#ifdef MODE_TRACE

layout(set = 0, binding = 10) uniform texture3D voxel_gi_aniso_textures[MAX_VOXEL_GI_INSTANCES];

#include "voxel_gi_sample_inc.glsl"

shared vec4 sh_red[PROBE_RAY_COUNT];
shared vec4 sh_green[PROBE_RAY_COUNT];
shared vec4 sh_blue[PROBE_RAY_COUNT];
shared vec4 sh_visibility[PROBE_RAY_COUNT];

float get_view_z(vec2 p_uv) {
	// Nearest depth, filtering would create surfaces between foreground and background.
	ivec2 pixel = clamp(ivec2(p_uv * vec2(params.screen_size)), ivec2(0), params.screen_size - 1);
	float depth = texelFetch(sampler2D(depth_buffer, linear_sampler), pixel, 0).r;
	return reconstruct_view_position((vec2(pixel) + 0.5) / vec2(params.screen_size), depth).z;
}

vec4 voxel_cone_trace(uint p_index, vec3 p_pos, vec3 p_direction, float p_tan_half_angle, float p_start, float p_max_distance, float p_normal_dot) {
	vec3 cell_size = 1.0 / voxel_gi_instances.data[p_index].bounds;
	float dist = p_start;
	vec4 color = vec4(0.0);

	while (dist < p_max_distance && color.a < 0.95) {
		float diameter = max(1.0, 2.0 * p_tan_half_angle * dist);
		vec3 uvw_pos = (p_pos + dist * p_direction) * cell_size;
		float half_diameter = diameter * 0.5;
		if (any(greaterThan(abs(uvw_pos - 0.5), vec3(0.5 + half_diameter * cell_size)))) {
			break;
		}
		vec4 scolor = voxel_gi_sample(p_index, uvw_pos, log2(diameter), p_direction, dist * p_normal_dot);
		color += (1.0 - color.a) * scolor;
		dist += half_diameter;
	}

	return color;
}

// Continues a ray through the VoxelGI volumes covering the probe, from p_start meters away from the origin.
vec4 trace_voxels(uvec2 p_instances, vec3 p_origin, vec3 p_normal, vec3 p_dir, float p_start) {
	vec4 accum = vec4(0.0);
	float blend_accum = 0.0;

	for (uint i = 0; i < params.max_voxel_gi_instances; i++) {
		if (!any(equal(uvec2(i), p_instances))) {
			continue;
		}

		mat4 xform = voxel_gi_instances.data[i].xform;
		vec3 cell_dir = mat3(xform) * p_dir;
		float cells_per_unit = length(cell_dir);
		cell_dir /= cells_per_unit;

		vec3 pos = (xform * vec4(p_origin, 1.0)).xyz;
		pos += normalize(mat3(xform) * p_normal) * voxel_gi_instances.data[i].normal_bias;

		vec3 bounds = voxel_gi_instances.data[i].bounds;
		if (any(lessThan(pos, vec3(0.0))) || any(greaterThan(pos, bounds))) {
			continue;
		}

		vec3 blendv = abs(pos / bounds * 2.0 - 1.0);
		float blend = clamp(1.0 - max(blendv.x, max(blendv.y, blendv.z)), 0.0, 1.0);

		// Each ray covers 1/64 of the hemisphere, which is about a 14 degree half angle cone.
		float start = max(p_start * cells_per_unit, voxel_gi_instances.data[i].bias);
		vec4 light = voxel_cone_trace(i, pos, cell_dir, 0.25, start, length(bounds), max(0.0, dot(p_dir, p_normal)));
		light.rgb *= voxel_gi_instances.data[i].dynamic_range * voxel_gi_instances.data[i].exposure_normalization;
		if (!voxel_gi_instances.data[i].blend_ambient) {
			light.a = 1.0;
		}

		accum += light * blend;
		blend_accum += blend;
	}

	if (blend_accum > 0.0) {
		accum /= blend_accum;
	}
	return accum;
}

void main() {
	ivec2 probe = ivec2(gl_WorkGroupID.xy);
	uint ray = gl_LocalInvocationIndex;

	ivec2 pixel = screen_probe_get_pixel(probe, params.probe_spacing, params.frame, params.screen_size);
	vec2 uv = (vec2(pixel) + 0.5) / vec2(params.screen_size);

	vec4 normal_roughness = texelFetch(sampler2D(normal_roughness_buffer, linear_sampler), pixel, 0);
	bool valid = dot(normal_roughness.xyz, normal_roughness.xyz) > 0.01;

	vec4 radiance = vec4(0.0);
	vec3 ray_dir = vec3(0.0, 0.0, 1.0);
	vec3 normal = vec3(0.0);

	if (valid) {
		float depth = texelFetch(sampler2D(depth_buffer, linear_sampler), pixel, 0).r;
		vec3 view_pos = reconstruct_view_position(uv, depth);
		vec3 view_normal = normalize(normal_roughness.xyz * 2.0 - 1.0);

		normal = normalize(mat3(params.cam_basis) * view_normal);
		vec3 origin = mat3(params.cam_basis) * view_pos;

		// Stratified uniform hemisphere sample, rotated randomly every frame.
		uint h = screen_probe_hash(uvec3(uvec2(probe), params.frame * PROBE_RAY_COUNT + ray));
		uint rotation = screen_probe_hash(uvec3(uvec2(probe), params.frame));
		vec2 jitter = vec2(h & 0xFFFF, h >> 16) / 65536.0;
		float cos_theta = (float(ray % PROBE_RAY_SIDE) + jitter.x) / float(PROBE_RAY_SIDE);
		float phi = 2.0 * M_PI * ((float(ray / PROBE_RAY_SIDE) + jitter.y) / float(PROBE_RAY_SIDE) + float(rotation & 0xFFFF) / 65536.0);
		float sin_theta = sqrt(max(0.0, 1.0 - cos_theta * cos_theta));

		vec3 v0 = abs(normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
		vec3 tangent = normalize(cross(v0, normal));
		vec3 bitangent = cross(normal, tangent);
		ray_dir = normalize(tangent * (sin_theta * cos(phi)) + bitangent * (sin_theta * sin(phi)) + normal * cos_theta);

		// Screen trace against the depth buffer.
		float view_distance = params.orthogonal != 0 ? 1.0 : -view_pos.z;
		vec3 view_dir = transpose(mat3(params.cam_basis)) * ray_dir;
		vec3 view_origin = view_pos + view_normal * (0.01 + view_distance * 0.005);

		float trace_end = 0.0;
		bool hit = false;
		vec3 hit_pos = vec3(0.0);
		float step_jitter = float((h >> 8) & 0xFF) / 256.0;

		if (params.has_last_frame != 0) {
			for (uint i = 0; i < params.screen_trace_steps; i++) {
				float f = (float(i) + step_jitter + 0.5) / float(params.screen_trace_steps);
				float t = params.screen_trace_distance * f * f;
				vec3 q = view_origin + view_dir * t;
				if (params.orthogonal == 0 && q.z > -params.z_near) {
					break;
				}
				vec4 clip = params.projection * vec4(q, 1.0);
				vec2 q_uv = (clip.xy / clip.w) * 0.5 + 0.5;
				if (any(lessThan(q_uv, vec2(0.0))) || any(greaterThan(q_uv, vec2(1.0)))) {
					break;
				}

				float scene_z = get_view_z(q_uv);
				if (q.z < scene_z) {
					// Behind the visible surface, it only counts as a hit if it is close enough to it.
					float q_distance = params.orthogonal != 0 ? 1.0 : -q.z;
					float thickness = max(0.1, q_distance * 0.05) + t * 0.1;
					if (scene_z - q.z < thickness) {
						hit = true;
						hit_pos = q;
					}
					break;
				}
				trace_end = t;
			}
		}

		if (hit) {
			vec4 prev_clip = params.reprojection * vec4(mat3(params.cam_basis) * hit_pos, 1.0);
			vec2 prev_uv = (prev_clip.xy / prev_clip.w) * 0.5 + 0.5;
			if (all(greaterThanEqual(prev_uv, vec2(0.0))) && all(lessThanEqual(prev_uv, vec2(1.0)))) {
				// Pick a mipmap matching the footprint of the ray at the hit point.
				float hit_distance = params.orthogonal != 0 ? 1.0 : -hit_pos.z;
				float footprint = 0.5 * length(hit_pos - view_origin) / max(1e-4, hit_distance * params.pixel_size);
				float lod = clamp(log2(max(1.0, footprint)), 0.0, params.last_frame_max_lod);
				radiance = vec4(textureLod(sampler2D(last_frame, linear_sampler_with_mipmaps), prev_uv, lod).rgb, 1.0);
			} else {
				hit = false;
			}
		}

		if (!hit) {
			uvec2 instances = texelFetch(voxel_gi_buffer, pixel, 0).rg;
			radiance = trace_voxels(instances, origin, normal, ray_dir, trace_end);
		}
	}

	// Project into L1 SH. Uniform hemisphere sampling, so every ray weighs 2 * PI / count.
	vec4 basis = screen_probe_sh_basis(ray_dir) * (2.0 * M_PI / float(PROBE_RAY_COUNT));
	sh_red[ray] = basis * radiance.r;
	sh_green[ray] = basis * radiance.g;
	sh_blue[ray] = basis * radiance.b;
	sh_visibility[ray] = basis * radiance.a;

	groupMemoryBarrier();
	barrier();

	for (uint stride = PROBE_RAY_COUNT / 2; stride > 0; stride >>= 1) {
		if (ray < stride) {
			sh_red[ray] += sh_red[ray + stride];
			sh_green[ray] += sh_green[ray + stride];
			sh_blue[ray] += sh_blue[ray + stride];
			sh_visibility[ray] += sh_visibility[ray + stride];
		}
		groupMemoryBarrier();
		barrier();
	}

	if (ray == 0) {
		imageStore(probe_sh, ivec3(probe, 0), sh_red[0]);
		imageStore(probe_sh, ivec3(probe, 1), sh_green[0]);
		imageStore(probe_sh, ivec3(probe, 2), sh_blue[0]);
		// Only the average visibility is kept, along with the normal the hemisphere was traced around.
		imageStore(probe_sh, ivec3(probe, 3), vec4(sh_visibility[0].x, normal));
	}
}

#endif // MODE_TRACE

#ifdef MODE_FILTER

// Averages every probe with its neighbors lying on the same surface, which removes most of the per probe noise.

bool get_probe_surface(ivec2 p_probe, out vec3 r_position, out vec3 r_normal) {
	ivec2 pixel = screen_probe_get_pixel(p_probe, params.probe_spacing, params.frame, params.screen_size);
	vec4 normal_roughness = texelFetch(sampler2D(normal_roughness_buffer, linear_sampler), pixel, 0);
	if (dot(normal_roughness.xyz, normal_roughness.xyz) < 0.01) {
		return false;
	}
	float depth = texelFetch(sampler2D(depth_buffer, linear_sampler), pixel, 0).r;
	r_position = reconstruct_view_position((vec2(pixel) + 0.5) / vec2(params.screen_size), depth);
	r_normal = normalize(normal_roughness.xyz * 2.0 - 1.0);
	return true;
}

void main() {
	ivec2 probe = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(probe, params.probe_grid_size))) {
		return;
	}

	vec3 position;
	vec3 normal;
	if (!get_probe_surface(probe, position, normal)) {
		for (int i = 0; i < SCREEN_PROBE_SH_LAYERS; i++) {
			imageStore(probe_sh, ivec3(probe, i), vec4(0.0));
		}
		return;
	}

	float surface_scale = max(0.05, length(position) * 0.02);

	vec4 accum[SCREEN_PROBE_SH_LAYERS] = vec4[](vec4(0.0), vec4(0.0), vec4(0.0), vec4(0.0));
	float weight_accum = 0.0;

	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			ivec2 neighbor = probe + ivec2(x, y);
			if (any(lessThan(neighbor, ivec2(0))) || any(greaterThanEqual(neighbor, params.probe_grid_size))) {
				continue;
			}

			float weight = 1.0;
			if (x != 0 || y != 0) {
				vec3 neighbor_position;
				vec3 neighbor_normal;
				if (!get_probe_surface(neighbor, neighbor_position, neighbor_normal)) {
					continue;
				}
				float plane_distance = abs(dot(neighbor_position - position, normal)) / surface_scale;
				weight = 1.0 / (1.0 + plane_distance * plane_distance);
				float normal_similarity = max(0.0, dot(neighbor_normal, normal));
				weight *= normal_similarity * normal_similarity * normal_similarity * normal_similarity;
				weight *= (x != 0 && y != 0) ? 0.5 : 0.75;
			}

			if (weight <= 0.0) {
				continue;
			}

			for (int i = 0; i < SCREEN_PROBE_SH_LAYERS; i++) {
				accum[i] += texelFetch(sampler2DArray(source_probe_sh, linear_sampler), ivec3(neighbor, i), 0) * weight;
			}
			weight_accum += weight;
		}
	}

	for (int i = 0; i < SCREEN_PROBE_SH_LAYERS; i++) {
		imageStore(probe_sh, ivec3(probe, i), accum[i] / weight_accum);
	}
}

#endif // MODE_FILTER
