#[compute]

#version 450

#VERSION_DEFINES

// Screen probe gather for SDFGI, mirroring voxel_gi_screen_probes.glsl: probes are placed on the
// depth buffer, every probe traces a hemisphere of rays that first march the depth buffer (lit by
// the previous frame) and fall back to SDFGI's baked probe volume (Radiance Cascades or the legacy
// integrator) when a ray leaves the screen or finds nothing. The result is stored as L1 spherical
// harmonics, in the exact same format gi.glsl's USE_SCREEN_PROBES path already knows how to gather
// and temporally filter for VoxelGI - see the USE_SCREEN_PROBES block under USE_SDFGI there.
//
// The fallback reads the probe volume through the same SDFGI uniform buffer gi.glsl uses (camera
// relative cascade positions, probe-unit normal bias, per-cascade exposure) with the same cascade
// selection, edge blend and trilinear + occlusion probe weighting as sdfvoxel_gi_process(), but
// samples the non-convolved radiance layer along the ray instead of irradiance along the normal.

#extension GL_EXT_samplerless_texture_functions : enable

#define PROBE_RAY_COUNT 64
#define PROBE_RAY_SIDE 8
#define SDFGI_MAX_CASCADES 8

#ifdef MODE_TRACE
layout(local_size_x = PROBE_RAY_COUNT, local_size_y = 1, local_size_z = 1) in;
#else
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
#endif

#include "../oct_inc.glsl"
#include "screen_probes_inc.glsl"

#define M_PI 3.14159265359

layout(set = 0, binding = 0) uniform texture2D depth_buffer;
layout(set = 0, binding = 1) uniform texture2D normal_roughness_buffer;

#ifdef MODE_TRACE

layout(set = 0, binding = 3) uniform texture2D last_frame;
layout(set = 0, binding = 7) uniform sampler linear_sampler_with_mipmaps;

// Must match the SDFGI uniform block in gi.glsl (it is the same buffer, GI::sdfgi_ubo).
struct ProbeCascadeData {
	vec3 position; // camera relative, y-scaled
	float to_probe;
	ivec3 probe_world_offset;
	float to_cell;
	vec3 pad;
	float exposure_normalization;
};

layout(set = 0, binding = 12, std140) uniform SDFGI {
	vec3 grid_size;
	uint max_cascades;

	bool use_occlusion;
	int probe_axis_size;
	float probe_to_uvw;
	float normal_bias; // in probe units

	vec3 lightprobe_tex_pixel_size;
	float energy;

	vec3 lightprobe_uv_offset;
	float y_mult;

	vec3 occlusion_clamp;
	uint pad3;

	vec3 occlusion_renormalize;
	uint pad4;

	vec3 cascade_probe_size;
	uint pad5;

	ProbeCascadeData cascades[SDFGI_MAX_CASCADES];
}
sdfgi;

layout(set = 0, binding = 13) uniform texture2DArray lightprobe_texture;
layout(set = 0, binding = 14) uniform texture3D occlusion_texture;

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
	uint screen_trace_steps;
	float screen_trace_distance;

	float z_near;
	float pixel_size; // World size of a pixel one unit away from the camera.
	float last_frame_max_lod;
	uint orthogonal;

	uint has_last_frame;
	uint pad0;
	uint pad1;
	uint pad2;
}
params;

vec3 reconstruct_view_position(vec2 p_uv, float p_depth) {
	vec4 pos = params.inv_projection * vec4(p_uv * 2.0 - 1.0, p_depth, 1.0);
	return pos.xyz / pos.w;
}

#ifdef MODE_TRACE

float get_view_z(vec2 p_uv) {
	// Nearest depth, filtering would create surfaces between foreground and background.
	ivec2 pixel = clamp(ivec2(p_uv * vec2(params.screen_size)), ivec2(0), params.screen_size - 1);
	float depth = texelFetch(sampler2D(depth_buffer, linear_sampler), pixel, 0).r;
	return reconstruct_view_position((vec2(pixel) + 0.5) / vec2(params.screen_size), depth).z;
}

// Radiance arriving from p_dir, read from one cascade of the baked probe volume. p_cascade_pos is
// in that cascade's probe units, p_normal/p_dir are y-scaled. Same weighting as gi.glsl's
// sdfvoxel_gi_process(); the lookup goes into the radiance half of the lightprobe texture.
vec3 sdfgi_cascade_radiance(uint p_cascade, vec3 p_cascade_pos, vec3 p_normal, vec3 p_dir) {
	p_cascade_pos += p_normal * sdfgi.normal_bias;

	ivec3 probe_base_pos = ivec3(floor(p_cascade_pos));

	ivec3 tex_pos = ivec3(probe_base_pos.xy, int(p_cascade));
	tex_pos.x += probe_base_pos.z * sdfgi.probe_axis_size;
	tex_pos.xy = tex_pos.xy * (SDFGI_OCT_SIZE + 2) + ivec2(1);

	vec3 radiance_posf = (vec3(tex_pos) + vec3(vec3_to_oct(p_dir) * float(SDFGI_OCT_SIZE), float(sdfgi.max_cascades))) * sdfgi.lightprobe_tex_pixel_size;

	vec4 accum = vec4(0.0);

	for (uint j = 0; j < 8; j++) {
		ivec3 offset = (ivec3(j) >> ivec3(0, 1, 2)) & ivec3(1, 1, 1);
		ivec3 probe_posi = probe_base_pos + offset;
		vec3 probe_pos = vec3(probe_posi);
		vec3 probe_to_pos = p_cascade_pos - probe_pos;
		vec3 probe_dir = normalize(-probe_to_pos);

		vec3 trilinear = vec3(1.0) - abs(probe_to_pos);
		float weight = trilinear.x * trilinear.y * trilinear.z * max(0.005, dot(p_normal, probe_dir));

		if (sdfgi.use_occlusion) {
			ivec3 occ_indexv = abs((sdfgi.cascades[p_cascade].probe_world_offset + probe_posi) & ivec3(1, 1, 1)) * ivec3(1, 2, 4);
			vec4 occ_mask = mix(vec4(0.0), vec4(1.0), equal(ivec4(occ_indexv.x | occ_indexv.y), ivec4(0, 1, 2, 3)));

			vec3 occ_pos = clamp(p_cascade_pos, probe_pos - sdfgi.occlusion_clamp, probe_pos + sdfgi.occlusion_clamp) * sdfgi.probe_to_uvw;
			occ_pos.z += float(p_cascade);
			if (occ_indexv.z != 0) { //z bit is on, means index is >=4, so make it switch to the other half of textures
				occ_pos.x += 1.0;
			}

			occ_pos *= sdfgi.occlusion_renormalize;
			float occlusion = dot(textureLod(sampler3D(occlusion_texture, linear_sampler), occ_pos, 0.0), occ_mask);

			weight *= max(occlusion, 0.01);
		}

		vec3 pos_uvw = radiance_posf;
		pos_uvw.xy += vec2(offset.xy) * sdfgi.lightprobe_uv_offset.xy;
		pos_uvw.x += float(offset.z) * sdfgi.lightprobe_uv_offset.z;
		vec3 radiance = textureLod(sampler2DArray(lightprobe_texture, linear_sampler), pos_uvw, 0.0).rgb;

		accum += vec4(radiance * weight * sdfgi.cascades[p_cascade].exposure_normalization, weight);
	}

	return accum.a > 0.0 ? accum.rgb / accum.a : vec3(0.0);
}

// Radiance arriving at p_position (camera relative world space, like gi.glsl's) from p_dir, read
// from SDFGI's baked probe volume for rays the screen trace could not resolve. Cascade selection
// and the blend into the next cascade near the edge follow gi.glsl's sdfgi_process().
vec3 sdfgi_probe_fallback(vec3 p_position, vec3 p_normal, vec3 p_dir) {
	vec3 position = p_position;
	position.y *= sdfgi.y_mult;
	vec3 normal = p_normal;
	normal.y *= sdfgi.y_mult;
	normal = normalize(normal);
	vec3 dir = p_dir;
	dir.y *= sdfgi.y_mult;
	dir = normalize(dir);

	uint cascade = 0xFFFFFFFFu;
	vec3 cascade_pos;

	for (uint i = 0; i < sdfgi.max_cascades; i++) {
		cascade_pos = (position - sdfgi.cascades[i].position) * sdfgi.cascades[i].to_probe;
		if (any(lessThan(cascade_pos, vec3(0.0))) || any(greaterThanEqual(cascade_pos, sdfgi.cascade_probe_size))) {
			continue;
		}
		cascade = i;
		break;
	}

	if (cascade >= SDFGI_MAX_CASCADES) {
		return vec3(0.0);
	}

	vec3 radiance = sdfgi_cascade_radiance(cascade, cascade_pos, normal, dir);

	float blend_from = (float(sdfgi.probe_axis_size - 1) / 2.0) - 2.5;
	float blend_to = blend_from + 2.0;

	vec3 inner_pos = position * sdfgi.cascades[cascade].to_probe;
	float len = length(inner_pos);
	if (len > 0.0) {
		inner_pos = abs(inner_pos / len);
		len *= max(inner_pos.x, max(inner_pos.y, inner_pos.z));
	}

	if (len >= blend_from) {
		float blend = smoothstep(blend_from, blend_to, len);
		if (cascade == sdfgi.max_cascades - 1) {
			radiance *= 1.0 - blend;
		} else {
			vec3 cascade_pos_next = (position - sdfgi.cascades[cascade + 1].position) * sdfgi.cascades[cascade + 1].to_probe;
			radiance = mix(radiance, sdfgi_cascade_radiance(cascade + 1, cascade_pos_next, normal, dir), blend);
		}
	}

	return radiance * sdfgi.energy;
}

shared vec4 sh_red[PROBE_RAY_COUNT];
shared vec4 sh_green[PROBE_RAY_COUNT];
shared vec4 sh_blue[PROBE_RAY_COUNT];
shared vec4 sh_visibility[PROBE_RAY_COUNT];

void main() {
	ivec2 probe = ivec2(gl_WorkGroupID.xy);
	uint ray = gl_LocalInvocationIndex;

	ivec2 pixel = screen_probe_get_pixel(probe, params.probe_spacing, params.frame, params.screen_size);
	vec2 uv = (vec2(pixel) + 0.5) / vec2(params.screen_size);

	vec4 normal_roughness = texelFetch(sampler2D(normal_roughness_buffer, linear_sampler), pixel, 0);
	bool valid = dot(normal_roughness.xyz, normal_roughness.xyz) > 0.01;

	vec4 radiance = vec4(0.0);
	vec3 ray_dir = vec3(0.0, 0.0, 1.0);

	if (valid) {
		float depth = texelFetch(sampler2D(depth_buffer, linear_sampler), pixel, 0).r;
		vec3 view_pos = reconstruct_view_position(uv, depth);
		vec3 view_normal = normalize(normal_roughness.xyz * 2.0 - 1.0);

		vec3 normal = normalize(mat3(params.cam_basis) * view_normal);
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
				float hit_distance = params.orthogonal != 0 ? 1.0 : -hit_pos.z;
				float footprint = 0.5 * length(hit_pos - view_origin) / max(1e-4, hit_distance * params.pixel_size);
				float lod = clamp(log2(max(1.0, footprint)), 0.0, params.last_frame_max_lod);
				radiance = vec4(textureLod(sampler2D(last_frame, linear_sampler_with_mipmaps), prev_uv, lod).rgb, 1.0);
			} else {
				hit = false;
			}
		}

		if (!hit) {
			radiance = vec4(sdfgi_probe_fallback(origin, normal, ray_dir), 1.0);
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
		imageStore(probe_sh, ivec3(probe, 3), sh_visibility[0]);
	}
}

#endif // MODE_TRACE

#ifdef MODE_FILTER

// Identical to voxel_gi_screen_probes.glsl's MODE_FILTER: averages every probe with its neighbors
// lying on the same surface, which removes most of the per-probe noise. Kept as a straight copy
// since it has no dependency on which volume MODE_TRACE fell back to.

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
