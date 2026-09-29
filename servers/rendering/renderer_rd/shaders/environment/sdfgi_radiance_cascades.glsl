#[compute]

#version 450

#VERSION_DEFINES

#include "../oct_inc.glsl"

// Radiance Cascades probe backend for SDFGI (an alternative to sdfgi_integrate.glsl), after
// Alexander Sannikov's Radiance Cascades. It reuses SDFGI's voxel cascades as the tracing primitive
// and writes the same octahedral RGBE lightprobe texture sdfgi_integrate.glsl writes, so everything
// downstream (gi.glsl, the direct light bounce feedback, volumetric fog) reads it unchanged.
//
// Every SDFGI cascade's probe grid is one radiance cascade. Cascade c traces oct_size^2 directions
// from its probes out to ray_reach world units (y-scaled space). Probe spacing doubles per cascade
// on its own; reach and angular resolution grow with it, as computed on the C++ side
// (GI::SDFGI::update_probes_radiance_cascades). The last cascade's rays are unbounded and end in
// the sky. Beyond a cascade's reach, the next cascade's merged radiance takes over.
//
// Unlike textbook Radiance Cascades, rays always start at the probe instead of at the end of the
// previous cascade's interval. SDFGI probes are sparse (8 cells apart), so the next cascade's
// probes are as far apart as that interval start would be: its rays would start behind walls the
// nearer cascade's rays never reach, and bring the lighting behind them in (a sealed room would
// light up from outside). Starting at the probe, what the next cascade contributes is only as
// wrong as interpolating between SDFGI probes already is, which the occlusion weights below keep
// in check the same way gi.glsl does. It also means every cascade holds radiance from all
// distances, which gi.glsl needs, as it reads every cascade directly in its own clipmap region.
//
// - MODE_TRACE: march out to this cascade's reach (moving on to coarser SDF cascades once the ray
//   leaves the current one, like sdfgi_integrate.glsl does) along a direction jittered inside each
//   octahedral texel every frame, and fold the result into an exponential moving average read from
//   last frame's buffer, following cascade scrolling. rgb = radiance found within reach,
//   a = transmittance (fraction of the texel's rays that reached ray_reach unobstructed).
// - MODE_MERGE: from the farthest cascade to the nearest, add the next cascade's merged radiance
//   in proportion to each direction's transmittance. The next cascade's 8 surrounding probes are
//   weighted trilinearly times SDFGI's probe visibility (occlusion texture), and probes embedded in
//   geometry are rejected: without both, light leaks through walls into the merged result.
// - MODE_PROJECT: project the merged radiance to order-2 SH.
// - MODE_STORE: same RGBE9995 octahedral encoding as sdfgi_integrate.glsl's MODE_STORE.
//
// Directions live in SDFGI's y-scaled space throughout (the space the SDF volume is in and the
// space gi.glsl looks probes up in), so the direction a texel is traced along is the direction its
// SH basis is evaluated at.

#define MAX_CASCADES 8

layout(set = 0, binding = 1) uniform texture3D sdf_cascades[MAX_CASCADES];
layout(set = 0, binding = 2) uniform texture3D light_cascades[MAX_CASCADES];
layout(set = 0, binding = 3) uniform texture3D aniso0_cascades[MAX_CASCADES];
layout(set = 0, binding = 4) uniform texture3D aniso1_cascades[MAX_CASCADES];
layout(set = 0, binding = 5) uniform texture3D occlusion_texture;

layout(set = 0, binding = 6) uniform sampler linear_sampler;

struct CascadeData {
	vec3 offset; //offset of (0,0,0) in world coordinates
	float to_cell; // 1/bounds * grid_size
	ivec3 probe_world_offset;
	uint pad;
	vec4 pad2;
};

layout(set = 0, binding = 7, std140) uniform Cascades {
	CascadeData data[MAX_CASCADES];
}
cascades;

layout(r32ui, set = 0, binding = 8) uniform restrict uimage2DArray lightprobe_texture_data;
layout(rgba16f, set = 0, binding = 9) uniform restrict writeonly image2DArray lightprobe_ambient_texture;

// RC_MAX_OCT_SIZE is provided by the shader variant defines (C++ side, SDFGI::RC_MAX_OCT_SIZE) and
// sizes the per-direction buffers; the per-cascade oct_size in the push constant never exceeds it.
#ifndef RC_MAX_OCT_SIZE
#define RC_MAX_OCT_SIZE 8
#endif
#define RC_MAX_ANGULAR_TEXELS (RC_MAX_OCT_SIZE * RC_MAX_OCT_SIZE)

#define SH_TERMS 9

// Upper bound on sphere tracing steps inside one SDF cascade, so a ray grazing a surface for a long
// stretch cannot stall the GPU. Such a ray is treated as having reached its end unobstructed.
#define RC_MAX_MARCH_STEPS 256

// Per-direction buffers, one layer per cascade, RC_MAX_ANGULAR_TEXELS rows per probe.
// radiance_cascade_trace (this frame) and
// radiance_cascade_trace_history (last frame) are swapped every frame on the C++ side. In
// radiance_cascade_merged, a = -1 flags a probe embedded in geometry.
layout(rgba16f, set = 0, binding = 10) uniform restrict image2DArray radiance_cascade_trace;
layout(rgba16f, set = 0, binding = 11) uniform restrict image2DArray radiance_cascade_merged;
layout(rgba32f, set = 0, binding = 12) uniform restrict image2DArray radiance_cascade_sh;
layout(rgba16f, set = 0, binding = 13) uniform restrict readonly image2DArray radiance_cascade_trace_history;

#ifdef USE_RADIANCE_OCTMAP_ARRAY
layout(set = 1, binding = 0) uniform texture2DArray sky_irradiance;
#else
layout(set = 1, binding = 0) uniform texture2D sky_irradiance;
#endif
layout(set = 1, binding = 1) uniform sampler linear_sampler_mipmaps;

#define SKY_FLAGS_MODE_COLOR 0x01
#define SKY_FLAGS_MODE_SKY 0x02
#define SKY_FLAGS_ORIENTATION_SIGN 0x04

layout(push_constant, std430) uniform Params {
	vec3 grid_size;
	uint max_cascades;

	uint probe_axis_size;
	uint cascade;
	uint oct_size; // angular resolution (per axis) of this cascade
	uint next_oct_size; // angular resolution of cascade + 1 (== oct_size for the last cascade)

	ivec2 image_size;
	float ray_reach; // world units from the probe, negative = unbounded (last cascade)
	float ray_bias;

	ivec3 history_scroll; // add to a probe cell to find where it was stored last frame
	float history_blend; // weight of this frame's sample in the moving average, 1 = no history

	vec3 sky_color_or_orientation;
	float sky_energy;

	vec2 sky_irradiance_border_size;
	uint sky_flags;
	uint frame;

	float y_mult;
	uint store_ambient_texture;
	uint pad0;
	uint pad1;
}
params;

const float PI = 3.14159265f;

// Octahedral texel -> direction in y-scaled space. p_jitter is the sub-texel position, 0.5 = centre.
vec3 rc_direction(uint p_local_index, uint p_oct_size, vec2 p_jitter) {
	vec2 local_xy = vec2(float(p_local_index % p_oct_size), float(p_local_index / p_oct_size));
	vec2 uv = (local_xy + p_jitter) / float(p_oct_size);
	return oct_to_vec3(uv * 2.0 - 1.0);
}

uint rc_encode(ivec2 p_local_xy, uint p_oct_size) {
	return uint(p_local_xy.x) + uint(p_local_xy.y) * p_oct_size;
}

// Probe cell <-> position in the per-probe buffers (same layout as sdfgi_integrate.glsl's probes).
ivec2 rc_probe_texel(ivec3 p_cell) {
	return ivec2(p_cell.x + p_cell.z * int(params.probe_axis_size), p_cell.y);
}

uint rc_hash(uvec3 v) {
	// PCG-style 3D hash.
	v = v * 1664525u + 1013904223u;
	v.x += v.y * v.z;
	v.y += v.z * v.x;
	v.z += v.x * v.y;
	v ^= v >> 16u;
	v.x += v.y * v.z;
	v.y += v.z * v.x;
	v.z += v.x * v.y;
	return v.x ^ v.y ^ v.z;
}

// A probe whose own position is inside (or right at) solid voxels: its rays start in geometry and
// return the wall's own light in every direction, so it must not feed any other probe.
bool rc_probe_embedded(vec3 p_probe_pos) {
	vec3 local_pos = (p_probe_pos - cascades.data[params.cascade].offset) * cascades.data[params.cascade].to_cell;
	vec3 uvw = clamp(local_pos / params.grid_size, vec3(0.0), vec3(1.0));
	float distance = textureLod(sampler3D(sdf_cascades[params.cascade], linear_sampler), uvw, 0.0).r * 255.0 - 1.0;
	return distance < 0.05;
}

uint rgbe_encode(vec3 color) {
	const float pow2to9 = 512.0f;
	const float B = 15.0f;
	const float N = 9.0f;
	const float LN2 = 0.6931471805599453094172321215;

	float cRed = clamp(color.r, 0.0, 65408.0);
	float cGreen = clamp(color.g, 0.0, 65408.0);
	float cBlue = clamp(color.b, 0.0, 65408.0);

	float cMax = max(cRed, max(cGreen, cBlue));

	float expp = max(-B - 1.0f, floor(log(cMax) / LN2)) + 1.0f + B;

	float sMax = floor((cMax / pow(2.0f, expp - B - N)) + 0.5f);

	float exps = expp + 1.0f;

	if (0.0 <= sMax && sMax < pow2to9) {
		exps = expp;
	}

	float sRed = floor((cRed / pow(2.0f, exps - B - N)) + 0.5f);
	float sGreen = floor((cGreen / pow(2.0f, exps - B - N)) + 0.5f);
	float sBlue = floor((cBlue / pow(2.0f, exps - B - N)) + 0.5f);
	return (uint(sRed) & 0x1FF) | ((uint(sGreen) & 0x1FF) << 9) | ((uint(sBlue) & 0x1FF) << 18) | ((uint(exps) & 0x1F) << 27);
}

void sh_basis(vec3 n, out float c[SH_TERMS]) {
	vec3 n2 = n * n;
	c[0] = 0.282095;
	c[1] = 0.488603 * n.y;
	c[2] = 0.488603 * n.z;
	c[3] = 0.488603 * n.x;
	c[4] = 1.092548 * n.x * n.y;
	c[5] = 1.092548 * n.y * n.z;
	c[6] = 0.315392 * (3.0 * n2.z - 1.0);
	c[7] = 1.092548 * n.x * n.z;
	c[8] = 0.546274 * (n2.x - n2.y);
}

const float sh_l_mult[SH_TERMS] = float[](
		1.0,
		2.0 / 3.0,
		2.0 / 3.0,
		2.0 / 3.0,
		1.0 / 4.0,
		1.0 / 4.0,
		1.0 / 4.0,
		1.0 / 4.0,
		1.0 / 4.0);

#ifdef MODE_TRACE

// Sky radiance for a y-scaled space direction.
vec3 rc_sky(vec3 p_dir) {
	vec3 world_dir = normalize(p_dir * vec3(1.0, 1.0 / params.y_mult, 1.0));

	if (bool(params.sky_flags & SKY_FLAGS_MODE_SKY)) {
		// Reconstruct sky orientation as quaternion and rotate the direction before sampling.
		float sky_sign = bool(params.sky_flags & SKY_FLAGS_ORIENTATION_SIGN) ? 1.0 : -1.0;
		vec4 sky_quat = vec4(params.sky_color_or_orientation, sky_sign * sqrt(1.0 - dot(params.sky_color_or_orientation, params.sky_color_or_orientation)));
		vec3 sky_dir = cross(sky_quat.xyz, world_dir);
		sky_dir = world_dir + ((sky_dir * sky_quat.w) + cross(sky_quat.xyz, sky_dir)) * 2.0;
#ifdef USE_RADIANCE_OCTMAP_ARRAY
		return textureLod(sampler2DArray(sky_irradiance, linear_sampler_mipmaps), vec3(vec3_to_oct_with_border(sky_dir, params.sky_irradiance_border_size), 0.0), 2.0).rgb * params.sky_energy;
#else
		return textureLod(sampler2D(sky_irradiance, linear_sampler_mipmaps), vec3_to_oct_with_border(sky_dir, params.sky_irradiance_border_size), 2.0).rgb * params.sky_energy;
#endif
	} else if (bool(params.sky_flags & SKY_FLAGS_MODE_COLOR)) {
		return params.sky_color_or_orientation * params.sky_energy;
	}
	return vec3(0.0);
}

// Traces from p_probe_pos along p_dir (unit length, y-scaled space) out to p_reach world units,
// p_reach < 0 meaning unbounded. Returns rgb = radiance found within reach, a = 1 if nothing was hit.
vec4 rc_trace(vec3 p_probe_pos, vec3 p_dir, float p_reach) {
	vec3 inv_dir = 1.0 / (p_dir + vec3(equal(p_dir, vec3(0.0))) * 1e-6);
	vec3 pos_to_uvw = 1.0 / params.grid_size;

	// Same origin offset sdfgi_integrate.glsl applies, to step out of the probe's own voxel.
	vec3 abs_dir = abs(p_dir);
	float bias = (1.0 / max(abs_dir.x, max(abs_dir.y, abs_dir.z))) * params.ray_bias / cascades.data[params.cascade].to_cell;
	vec3 ray_pos = p_probe_pos + p_dir * bias;
	float remaining = p_reach < 0.0 ? 1e20 : max(p_reach - bias, 0.0);

	bool hit = false;
	bool reached_end = false;
	uint hit_cascade = 0;
	vec3 uvw = vec3(0.0);

	for (uint j = params.cascade; j < params.max_cascades; j++) {
		vec3 pos = (ray_pos - cascades.data[j].offset) * cascades.data[j].to_cell;
		if (any(lessThan(pos, vec3(0.0))) || any(greaterThanEqual(pos, params.grid_size))) {
			continue; // not inside this cascade (anymore), try the next, coarser one
		}

		vec3 t0 = -pos * inv_dir;
		vec3 t1 = (params.grid_size - pos) * inv_dir;
		vec3 tmax = max(t0, t1);
		float max_advance = min(tmax.x, min(tmax.y, tmax.z));
		float remaining_cells = remaining * cascades.data[j].to_cell;
		float limit = min(max_advance, remaining_cells);

		float advance = 0.0;
		uint steps = 0;
		while (advance < limit) {
			uvw = (pos + p_dir * advance) * pos_to_uvw;
			float distance = textureLod(sampler3D(sdf_cascades[j], linear_sampler), uvw, 0.0).r * 255.0 - 1.0;
			if (distance < 0.05) {
				hit = true;
				break;
			}
			advance += distance;
			steps++;
			if (steps >= RC_MAX_MARCH_STEPS) {
				reached_end = true;
				break;
			}
		}

		if (hit) {
			hit_cascade = j;
			break;
		}
		if (reached_end || remaining_cells <= max_advance) {
			reached_end = true;
			break;
		}

		// Left this cascade before reaching the end, continue from its boundary in the next one.
		float advance_world = max_advance / cascades.data[j].to_cell;
		ray_pos += p_dir * advance_world;
		remaining -= advance_world;
	}

	if (hit) {
		vec3 light = vec3(0.0);
		// Index the texture arrays only with a value uniform across the invocations executing the
		// access (the loop counter, compared against the hit cascade), like sdfgi_integrate.glsl.
		for (uint j = params.cascade; j < params.max_cascades; j++) {
			if (j == hit_cascade) {
				const float EPSILON = 0.001;
				vec3 gradient = vec3(
						textureLod(sampler3D(sdf_cascades[j], linear_sampler), uvw + vec3(EPSILON, 0.0, 0.0), 0.0).r - textureLod(sampler3D(sdf_cascades[j], linear_sampler), uvw - vec3(EPSILON, 0.0, 0.0), 0.0).r,
						textureLod(sampler3D(sdf_cascades[j], linear_sampler), uvw + vec3(0.0, EPSILON, 0.0), 0.0).r - textureLod(sampler3D(sdf_cascades[j], linear_sampler), uvw - vec3(0.0, EPSILON, 0.0), 0.0).r,
						textureLod(sampler3D(sdf_cascades[j], linear_sampler), uvw + vec3(0.0, 0.0, EPSILON), 0.0).r - textureLod(sampler3D(sdf_cascades[j], linear_sampler), uvw - vec3(0.0, 0.0, EPSILON), 0.0).r);
				// Deep inside solid voxels the gradient vanishes; don't let that turn into a NaN,
				// which the moving average would then keep forever.
				float gradient_len = length(gradient);
				vec3 hit_normal = gradient_len > 1e-6 ? gradient / gradient_len : -p_dir;

				vec3 hit_light = textureLod(sampler3D(light_cascades[j], linear_sampler), uvw, 0.0).rgb;
				vec4 aniso0 = textureLod(sampler3D(aniso0_cascades[j], linear_sampler), uvw, 0.0);
				vec3 hit_aniso0 = aniso0.rgb;
				vec3 hit_aniso1 = vec3(aniso0.a, textureLod(sampler3D(aniso1_cascades[j], linear_sampler), uvw, 0.0).rg);

				light = hit_light * (dot(max(vec3(0.0), (hit_normal * hit_aniso0)), vec3(1.0)) + dot(max(vec3(0.0), (-hit_normal * hit_aniso1)), vec3(1.0)));
			}
		}
		return vec4(light, 0.0);
	}

	if (reached_end) {
		return vec4(0.0, 0.0, 0.0, 1.0); // nothing within reach, farther cascades fill it in
	}

	// Left the outermost cascade before reaching the end: only the sky lies beyond.
	return vec4(rc_sky(p_dir), 0.0);
}

#endif // MODE_TRACE

#if defined(MODE_TRACE) || defined(MODE_MERGE) || defined(MODE_PROJECT)

// One invocation per probe: same dispatch domain as sdfgi_integrate.glsl's MODE_PROCESS.
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.image_size))) {
		return;
	}

	ivec3 probe_cell;
	probe_cell.x = pos.x % int(params.probe_axis_size);
	probe_cell.y = pos.y;
	probe_cell.z = pos.x / int(params.probe_axis_size);

	float probe_spacing_cells = params.grid_size.x / float(params.probe_axis_size - 1);
	vec3 probe_pos = cascades.data[params.cascade].offset + vec3(probe_cell) * (probe_spacing_cells / cascades.data[params.cascade].to_cell);

	uint dir_count = params.oct_size * params.oct_size;

#ifdef MODE_TRACE

	bool embedded = rc_probe_embedded(probe_pos);
	uvec3 world_probe = uvec3(cascades.data[params.cascade].probe_world_offset + probe_cell);

	ivec3 history_cell = probe_cell + params.history_scroll;
	bool has_history = params.history_blend < 1.0 && all(greaterThanEqual(history_cell, ivec3(0))) && all(lessThan(history_cell, ivec3(params.probe_axis_size)));
	ivec2 history_texel = rc_probe_texel(history_cell);

	for (uint li = 0; li < dir_count; li++) {
		// Embedded probes trace nothing and stay black, opaque in every direction. Whatever lies
		// around them is as likely to be on the far side of the wall as on the near one (typically
		// the sunlit outside of a room), so passing anything in from the next cascade would leak it
		// into the room along the walls wherever gi.glsl blends them in.
		vec4 result = vec4(0.0);

		if (!embedded) {
			uint h = rc_hash(world_probe ^ uvec3(li * 0x9E3779B9u, params.frame * 0x85EBCA6Bu, params.cascade * 0xC2B2AE35u));
			vec2 jitter = vec2(h & 0xFFFFu, h >> 16u) / 65536.0;
			result = rc_trace(probe_pos, rc_direction(li, params.oct_size, jitter), params.ray_reach);
		}

		if (has_history) {
			vec4 previous = imageLoad(radiance_cascade_trace_history, ivec3(history_texel.x, history_texel.y * RC_MAX_ANGULAR_TEXELS + int(li), int(params.cascade)));
			result = mix(previous, result, params.history_blend);
		}

		if (embedded) {
			result = vec4(0.0); // no history either: a probe that just got buried must not keep its light
		} else if (any(isnan(result)) || any(isinf(result))) {
			result = vec4(0.0, 0.0, 0.0, 1.0);
		}

		imageStore(radiance_cascade_trace, ivec3(pos.x, pos.y * RC_MAX_ANGULAR_TEXELS + int(li), int(params.cascade)), result);
	}

#endif // MODE_TRACE

#ifdef MODE_MERGE

	bool embedded = rc_probe_embedded(probe_pos);
	bool has_next = params.cascade + 1 < params.max_cascades;
	uint ratio = has_next ? max(1u, params.next_oct_size / params.oct_size) : 1u;

	// Weights of the 8 next-cascade probes around this one: trilinear, times SDFGI's per-voxel
	// probe visibility (the occlusion texture, which the preprocess pass always computes, whatever
	// the Environment's use_occlusion says), with embedded probes rejected outright. Same occlusion
	// lookup gi.glsl's sdfvoxel_gi_process() does for surfaces, here from this probe's position.
	float neighbor_weight[8];
	ivec2 neighbor_texel[8];
	float weight_sum = 0.0;

	for (int i = 0; i < 8; i++) {
		neighbor_weight[i] = 0.0;
		neighbor_texel[i] = ivec2(0);
	}

	if (has_next) {
		uint next = params.cascade + 1;
		vec3 next_pos = (probe_pos - cascades.data[next].offset) * cascades.data[next].to_cell / probe_spacing_cells; // next cascade's probe units
		ivec3 next_base = ivec3(floor(next_pos));

		float probe_to_uvw = 1.0 / float(params.probe_axis_size - 1);
		float occlusion_clamp = (probe_spacing_cells - 0.5) / probe_spacing_cells;
		vec3 occlusion_renormalize = vec3(0.5, 1.0, 1.0 / float(params.max_cascades));

		for (int i = 0; i < 8; i++) {
			ivec3 offset = (ivec3(i) >> ivec3(0, 1, 2)) & ivec3(1, 1, 1);
			ivec3 cell = next_base + offset;
			if (any(lessThan(cell, ivec3(0))) || any(greaterThanEqual(cell, ivec3(params.probe_axis_size)))) {
				continue;
			}

			ivec2 texel = rc_probe_texel(cell);
			if (imageLoad(radiance_cascade_merged, ivec3(texel.x, texel.y * RC_MAX_ANGULAR_TEXELS, int(next))).a < 0.0) {
				continue; // embedded in geometry
			}

			vec3 cell_pos = vec3(cell);
			vec3 trilinear = vec3(1.0) - abs(next_pos - cell_pos);
			// Every other probe here sits exactly on a next-cascade probe (weights 1, 0, 0, ...). The
			// floor keeps the others in play if that one is rejected, and is negligible otherwise.
			float weight = max(trilinear.x * trilinear.y * trilinear.z, 1e-3);

			ivec3 occ_indexv = abs((cascades.data[next].probe_world_offset + cell) & ivec3(1, 1, 1)) * ivec3(1, 2, 4);
			vec4 occ_mask = mix(vec4(0.0), vec4(1.0), equal(ivec4(occ_indexv.x | occ_indexv.y), ivec4(0, 1, 2, 3)));
			vec3 occ_pos = clamp(next_pos, cell_pos - occlusion_clamp, cell_pos + occlusion_clamp) * probe_to_uvw;
			occ_pos.z += float(next);
			if (occ_indexv.z != 0) { //z bit is on, means index is >=4, so make it switch to the other half of textures
				occ_pos.x += 1.0;
			}
			occ_pos *= occlusion_renormalize;
			float occlusion = dot(textureLod(sampler3D(occlusion_texture, linear_sampler), occ_pos, 0.0), occ_mask);
			weight *= max(occlusion, 0.01);

			neighbor_weight[i] = weight;
			neighbor_texel[i] = texel;
			weight_sum += weight;
		}
	}

	for (uint li = 0; li < dir_count; li++) {
		ivec3 own_pos = ivec3(pos.x, pos.y * RC_MAX_ANGULAR_TEXELS + int(li), int(params.cascade));
		vec4 own = imageLoad(radiance_cascade_trace, own_pos);

		vec3 merged = own.rgb;

		if (weight_sum > 0.0 && own.a > 0.0) {
			// This texel's solid angle is covered by a ratio x ratio block of the next cascade's.
			ivec2 child_base = ivec2(int(li % params.oct_size), int(li / params.oct_size)) * int(ratio);
			vec3 far_field = vec3(0.0);

			for (uint cy = 0; cy < ratio; cy++) {
				for (uint cx = 0; cx < ratio; cx++) {
					uint child_li = rc_encode(child_base + ivec2(int(cx), int(cy)), params.next_oct_size);
					for (int i = 0; i < 8; i++) {
						if (neighbor_weight[i] <= 0.0) {
							continue;
						}
						ivec3 read_pos = ivec3(neighbor_texel[i].x, neighbor_texel[i].y * RC_MAX_ANGULAR_TEXELS + int(child_li), int(params.cascade + 1));
						far_field += imageLoad(radiance_cascade_merged, read_pos).rgb * neighbor_weight[i];
					}
				}
			}

			merged += own.a * far_field / (weight_sum * float(ratio * ratio));
		}

		imageStore(radiance_cascade_merged, own_pos, vec4(merged, embedded ? -1.0 : own.a));
	}

#endif // MODE_MERGE

#ifdef MODE_PROJECT

	vec3 sh_rgb[SH_TERMS];
	for (uint i = 0; i < SH_TERMS; i++) {
		sh_rgb[i] = vec3(0.0);
	}

	float weight = (4.0 * PI) / float(dir_count);

	for (uint li = 0; li < dir_count; li++) {
		vec3 radiance = imageLoad(radiance_cascade_merged, ivec3(pos.x, pos.y * RC_MAX_ANGULAR_TEXELS + int(li), int(params.cascade))).rgb;

		vec3 dir = rc_direction(li, params.oct_size, vec2(0.5)); // texel centre: what the texel's average represents

		float basis[SH_TERMS];
		sh_basis(dir, basis);
		for (uint i = 0; i < SH_TERMS; i++) {
			sh_rgb[i] += radiance * basis[i] * weight;
		}
	}

	for (uint i = 0; i < SH_TERMS; i++) {
		imageStore(radiance_cascade_sh, ivec3(pos.x, pos.y * SH_TERMS + int(i), int(params.cascade)), vec4(sh_rgb[i], 0.0));
	}

	if (params.store_ambient_texture != 0) {
		// Mean radiance over the sphere (c0 * Y0), the quantity sdfgi_integrate.glsl's ambient holds.
		vec3 ambient = sh_rgb[0] * 0.282095;
		imageStore(lightprobe_ambient_texture, ivec3(pos, int(params.cascade)), vec4(ambient, 1.0));
	}

#endif // MODE_PROJECT
}

#endif // MODE_TRACE || MODE_MERGE || MODE_PROJECT

#ifdef MODE_STORE

// One invocation per FINAL octahedral texel: same dispatch domain (and a direct copy of the
// encoding logic) as sdfgi_integrate.glsl's own MODE_STORE. params.image_size must be set to the
// larger, texel-grained size for this dispatch, exactly like the legacy pass does.
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.image_size))) {
		return;
	}

	ivec2 sh_pos = (pos / OCT_SIZE) * ivec2(1, SH_TERMS);
	ivec2 oct_pos = (pos / OCT_SIZE) * (OCT_SIZE + 2) + ivec2(1);
	ivec2 local_pos = pos % OCT_SIZE;

	vec3 normal = oct_to_vec3(vec2(local_pos) / float(OCT_SIZE) * 2.0 - 1.0);

	float c[SH_TERMS];
	sh_basis(normal, c);

	vec3 irradiance = vec3(0.0);
	vec3 radiance = vec3(0.0);

	for (uint i = 0; i < SH_TERMS; i++) {
		ivec3 sh_read_pos = ivec3(sh_pos.x, sh_pos.y + int(i), int(params.cascade));
		vec3 m = imageLoad(radiance_cascade_sh, sh_read_pos).rgb * c[i];

		irradiance += m * sh_l_mult[i];
		radiance += m;
	}

	uint irradiance_rgbe = rgbe_encode(irradiance);
	uint radiance_rgbe = rgbe_encode(radiance);

	ivec3 texture_pos = ivec3(oct_pos, int(params.cascade));
	ivec3 copy_to[4] = ivec3[](ivec3(-2, -2, -2), ivec3(-2, -2, -2), ivec3(-2, -2, -2), ivec3(-2, -2, -2));
	copy_to[0] = texture_pos + ivec3(local_pos, 0);

	if (local_pos == ivec2(0, 0)) {
		copy_to[1] = texture_pos + ivec3(OCT_SIZE - 1, -1, 0);
		copy_to[2] = texture_pos + ivec3(-1, OCT_SIZE - 1, 0);
		copy_to[3] = texture_pos + ivec3(OCT_SIZE, OCT_SIZE, 0);
	} else if (local_pos == ivec2(OCT_SIZE - 1, 0)) {
		copy_to[1] = texture_pos + ivec3(0, -1, 0);
		copy_to[2] = texture_pos + ivec3(OCT_SIZE, OCT_SIZE - 1, 0);
		copy_to[3] = texture_pos + ivec3(-1, OCT_SIZE, 0);
	} else if (local_pos == ivec2(0, OCT_SIZE - 1)) {
		copy_to[1] = texture_pos + ivec3(-1, 0, 0);
		copy_to[2] = texture_pos + ivec3(OCT_SIZE - 1, OCT_SIZE, 0);
		copy_to[3] = texture_pos + ivec3(OCT_SIZE, -1, 0);
	} else if (local_pos == ivec2(OCT_SIZE - 1, OCT_SIZE - 1)) {
		copy_to[1] = texture_pos + ivec3(0, OCT_SIZE, 0);
		copy_to[2] = texture_pos + ivec3(OCT_SIZE, 0, 0);
		copy_to[3] = texture_pos + ivec3(-1, -1, 0);
	} else if (local_pos.y == 0) {
		copy_to[1] = texture_pos + ivec3(OCT_SIZE - local_pos.x - 1, local_pos.y - 1, 0);
	} else if (local_pos.x == 0) {
		copy_to[1] = texture_pos + ivec3(local_pos.x - 1, OCT_SIZE - local_pos.y - 1, 0);
	} else if (local_pos.y == OCT_SIZE - 1) {
		copy_to[1] = texture_pos + ivec3(OCT_SIZE - local_pos.x - 1, local_pos.y + 1, 0);
	} else if (local_pos.x == OCT_SIZE - 1) {
		copy_to[1] = texture_pos + ivec3(local_pos.x + 1, OCT_SIZE - local_pos.y - 1, 0);
	}

	for (int i = 0; i < 4; i++) {
		if (copy_to[i] == ivec3(-2, -2, -2)) {
			continue;
		}
		imageStore(lightprobe_texture_data, copy_to[i], uvec4(irradiance_rgbe));
		imageStore(lightprobe_texture_data, copy_to[i] + ivec3(0, 0, int(params.max_cascades)), uvec4(radiance_rgbe));
	}
}

#endif // MODE_STORE
