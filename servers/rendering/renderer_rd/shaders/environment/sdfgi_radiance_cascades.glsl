#[compute]

#version 450

#VERSION_DEFINES

#include "../oct_inc.glsl"

// Experimental replacement for the probe integration step of SDFGI
// (see sdfgi_integrate.glsl), based on Radiance Cascades (Alexander
// Sannikov). Reuses the existing SDFGI voxel cascades as the tracing
// primitive and the existing octahedral RGBE lightprobe texture as the
// output format, so nothing downstream (gi.glsl, scene_forward_gi_inc.glsl)
// needs to change.
//
// Godot's SDFGI spatial cascades already behave like a radiance-cascade
// spatial hierarchy (cascade N covers 2x the world extent of cascade N-1
// with half the probe density), so no new spatial structure is introduced
// here. What changes is the angular axis and the ray interval:
//
// - Each cascade only ray-marches within its OWN voxel bounds (unlike
//   sdfgi_integrate.glsl's MODE_PROCESS, which lets a single ray continue
//   into farther cascades). This gives every cascade a bounded interval
//   that grows geometrically for free, from the existing cascade geometry.
// - Each cascade stores raw per-direction radiance in an octahedral map
//   whose resolution grows with cascade index (MODE_TRACE), instead of a
//   fixed angular resolution (SH) for every cascade regardless of scale.
// - MODE_MERGE combines a cascade with the next (farther, angularly finer)
//   one: for every direction of cascade C, it averages the matching block
//   of child directions in cascade C+1 (spatially trilinear-sampled from
//   the 8 nearest C+1 probes) and adds it, scaled by C's own transmittance.
//   This is the direct 3D generalization of Sannikov's 2D cascade merge.
// - MODE_PROJECT reduces a cascade's fully-merged radiance to order-2 SH
//   (one thread per probe, matching sdfgi_integrate.glsl's MODE_PROCESS
//   dispatch domain).
// - MODE_STORE is dispatched over the final octahedral texel grid, exactly
//   like sdfgi_integrate.glsl's own MODE_STORE, and reuses its RGBE9995 +
//   border-copy encoding unchanged, just reading from MODE_PROJECT's SH
//   buffer instead of a temporally-accumulated history buffer.
//
// Known simplifications (left for follow-up work, out of scope for this
// experimental branch): the angular buffers are allocated at the
// compile-time cap (RC_MAX_OCT_SIZE) for every cascade rather than a
// tightly packed atlas sized per cascade, SH projection is fixed at
// order-2 (9 terms) rather than matching SH_SIZE, and there is no temporal
// amortization here (every update re-traces all cascades in full; temporal
// amortization is instead applied in the screen-probe layer, see
// sdfgi_screen_probes.glsl).

#define MAX_CASCADES 8

layout(set = 0, binding = 1) uniform texture3D sdf_cascades[MAX_CASCADES];
layout(set = 0, binding = 2) uniform texture3D light_cascades[MAX_CASCADES];
layout(set = 0, binding = 3) uniform texture3D aniso0_cascades[MAX_CASCADES];
layout(set = 0, binding = 4) uniform texture3D aniso1_cascades[MAX_CASCADES];

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

// RC_MAX_OCT_SIZE is provided by the shader variant defines (C++ side),
// must be a power of two, and bounds how much angular detail any single
// cascade can carry (the main quality/perf scaling knob for this layer).
#ifndef RC_MAX_OCT_SIZE
#define RC_MAX_OCT_SIZE 8
#endif
#define RC_MAX_ANGULAR_TEXELS (RC_MAX_OCT_SIZE * RC_MAX_OCT_SIZE)

#define SH_TERMS 9

// Intermediate per-direction radiance, one layer per cascade.
// rgb = radiance, a = transmittance (1 = ray escaped this cascade's bounds
// without hitting anything, so MODE_MERGE should pull in the farther
// cascade's contribution for that direction).
layout(rgba16f, set = 0, binding = 10) uniform restrict image2DArray radiance_cascade_trace;
layout(rgba16f, set = 0, binding = 11) uniform restrict image2DArray radiance_cascade_merged;

// SH_TERMS layers (see the y-multiplexing trick below), one group per cascade.
layout(rgba32f, set = 0, binding = 12) uniform restrict image2DArray radiance_cascade_sh;

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
	uint angular_branching_log2; // how fast angular resolution grows per cascade (RC's branching factor)
	uint base_oct_size; // angular resolution (per axis) of cascade 0

	ivec2 image_size;
	float ray_bias;
	uint sky_flags;

	ivec3 world_offset;
	float sky_energy;

	vec3 sky_color_or_orientation;
	float y_mult;

	vec2 sky_irradiance_border_size;
	uint store_ambient_texture;
	uint rc_max_oct_size; // runtime quality cap (<= compile-time RC_MAX_OCT_SIZE, which bounds the buffers)
}
params;

const float PI = 3.14159265f;

uint rc_oct_size(uint p_cascade) {
	uint size = params.base_oct_size << (p_cascade * params.angular_branching_log2);
	// The compile-time RC_MAX_OCT_SIZE is a hard cap (it sizes the trace/merge/SH buffers, see
	// RC_MAX_ANGULAR_TEXELS above); params.rc_max_oct_size is the user-facing quality knob and
	// must never exceed it, but is free to be lower to cut cost without recompiling the shader.
	return clamp(size, params.base_oct_size, min(params.rc_max_oct_size, uint(RC_MAX_OCT_SIZE)));
}

vec3 rc_direction(uint p_local_index, uint p_oct_size) {
	ivec2 local_xy = ivec2(int(p_local_index % p_oct_size), int(p_local_index / p_oct_size));
	vec2 uv = (vec2(local_xy) + 0.5) / float(p_oct_size);
	return oct_to_vec3(uv * 2.0 - 1.0);
}

uint rc_encode(ivec2 p_local_xy, uint p_oct_size) {
	return uint(p_local_xy.x) + uint(p_local_xy.y) * p_oct_size;
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

#if defined(MODE_TRACE) || defined(MODE_MERGE) || defined(MODE_PROJECT)

// One invocation per probe: same dispatch domain as
// sdfgi_integrate.glsl's MODE_PROCESS.
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

	float probe_cell_size = float(params.grid_size.x / float(params.probe_axis_size - 1)) / cascades.data[params.cascade].to_cell;
	vec3 probe_pos = cascades.data[params.cascade].offset + vec3(probe_cell) * probe_cell_size;
	vec3 pos_to_uvw = 1.0 / params.grid_size;

	uint oct_size = rc_oct_size(params.cascade);
	uint dir_count = oct_size * oct_size;

#ifdef MODE_TRACE

	for (uint li = 0; li < dir_count; li++) {
		vec3 ray_dir = rc_direction(li, oct_size);
		ray_dir.y *= params.y_mult;
		ray_dir = normalize(ray_dir);

		vec3 abs_ray_dir = abs(ray_dir);
		vec3 ray_pos = probe_pos + ray_dir * (1.0 / max(abs_ray_dir.x, max(abs_ray_dir.y, abs_ray_dir.z))) * params.ray_bias / cascades.data[params.cascade].to_cell;
		vec3 inv_dir = 1.0 / ray_dir;

		//this cascade's own local bounds only: no cross-cascade continuation
		vec3 local_pos = (ray_pos - cascades.data[params.cascade].offset) * cascades.data[params.cascade].to_cell;

		bool hit = false;
		vec3 uvw = vec3(0.0);

		if (all(greaterThanEqual(local_pos, vec3(0.0))) && all(lessThan(local_pos, params.grid_size))) {
			vec3 t0 = -local_pos * inv_dir;
			vec3 t1 = (params.grid_size - local_pos) * inv_dir;
			vec3 tmax = max(t0, t1);
			float max_advance = min(tmax.x, min(tmax.y, tmax.z));

			float advance = 0.0;
			while (advance < max_advance) {
				uvw = (local_pos + ray_dir * advance) * pos_to_uvw;
				float distance = texture(sampler3D(sdf_cascades[params.cascade], linear_sampler), uvw).r * 255.0 - 1.0;
				if (distance < 0.05) {
					hit = true;
					break;
				}
				advance += distance;
			}
		}

		vec4 light = vec4(0.0);
		float transmittance = 1.0;

		if (hit) {
			const float EPSILON = 0.001;
			vec3 hit_normal = normalize(vec3(
					texture(sampler3D(sdf_cascades[params.cascade], linear_sampler), uvw + vec3(EPSILON, 0.0, 0.0)).r - texture(sampler3D(sdf_cascades[params.cascade], linear_sampler), uvw - vec3(EPSILON, 0.0, 0.0)).r,
					texture(sampler3D(sdf_cascades[params.cascade], linear_sampler), uvw + vec3(0.0, EPSILON, 0.0)).r - texture(sampler3D(sdf_cascades[params.cascade], linear_sampler), uvw - vec3(0.0, EPSILON, 0.0)).r,
					texture(sampler3D(sdf_cascades[params.cascade], linear_sampler), uvw + vec3(0.0, 0.0, EPSILON)).r - texture(sampler3D(sdf_cascades[params.cascade], linear_sampler), uvw - vec3(0.0, 0.0, EPSILON)).r));

			vec3 hit_light = texture(sampler3D(light_cascades[params.cascade], linear_sampler), uvw).rgb;
			vec4 aniso0 = texture(sampler3D(aniso0_cascades[params.cascade], linear_sampler), uvw);
			vec3 hit_aniso0 = aniso0.rgb;
			vec3 hit_aniso1 = vec3(aniso0.a, texture(sampler3D(aniso1_cascades[params.cascade], linear_sampler), uvw).rg);

			light.rgb = hit_light * (dot(max(vec3(0.0), (hit_normal * hit_aniso0)), vec3(1.0)) + dot(max(vec3(0.0), (-hit_normal * hit_aniso1)), vec3(1.0)));
			transmittance = 0.0;
		} else if (params.cascade == params.max_cascades - 1) {
			//nothing farther to merge from: resolve against the sky now
			if (bool(params.sky_flags & SKY_FLAGS_MODE_SKY)) {
				float sky_sign = bool(params.sky_flags & SKY_FLAGS_ORIENTATION_SIGN) ? 1.0 : -1.0;
				vec4 sky_quat = vec4(params.sky_color_or_orientation, sky_sign * sqrt(1.0 - dot(params.sky_color_or_orientation, params.sky_color_or_orientation)));
				vec3 sky_dir = cross(sky_quat.xyz, ray_dir);
				sky_dir = ray_dir + ((sky_dir * sky_quat.w) + cross(sky_quat.xyz, sky_dir)) * 2.0;
#ifdef USE_RADIANCE_OCTMAP_ARRAY
				light.rgb = textureLod(sampler2DArray(sky_irradiance, linear_sampler_mipmaps), vec3(vec3_to_oct_with_border(sky_dir, params.sky_irradiance_border_size), 0.0), 2.0).rgb;
#else
				light.rgb = textureLod(sampler2D(sky_irradiance, linear_sampler_mipmaps), vec3_to_oct_with_border(sky_dir, params.sky_irradiance_border_size), 2.0).rgb;
#endif
				light.rgb *= params.sky_energy;
			} else if (bool(params.sky_flags & SKY_FLAGS_MODE_COLOR)) {
				light.rgb = params.sky_color_or_orientation * params.sky_energy;
			}
			transmittance = 0.0;
		}
		//else: left unresolved (transmittance = 1), MODE_MERGE pulls in cascade+1

		ivec3 store_pos = ivec3(pos.x, pos.y * RC_MAX_ANGULAR_TEXELS + int(li), int(params.cascade));
		imageStore(radiance_cascade_trace, store_pos, vec4(light.rgb, transmittance));
	}

#endif // MODE_TRACE

#ifdef MODE_MERGE

	bool has_next = params.cascade + 1 < params.max_cascades;
	uint next_oct_size = has_next ? rc_oct_size(params.cascade + 1) : oct_size;
	uint ratio = has_next ? max(uint(1), next_oct_size / oct_size) : 1;

	//spatial mapping of this probe into cascade+1's probe grid, for the trilinear neighbor gather
	float cell_to_probe = float(params.grid_size.x / float(params.probe_axis_size - 1));
	vec3 probe_pos_next = vec3(0.0);
	ivec3 probe_posi_next = ivec3(0);
	vec3 trilinear_frac = vec3(0.0);

	if (has_next) {
		float probe_cell_size_next = cell_to_probe / cascades.data[params.cascade + 1].to_cell;
		probe_pos_next = probe_pos - cascades.data[params.cascade + 1].offset;
		probe_pos_next /= probe_cell_size_next;
		probe_posi_next = ivec3(floor(probe_pos_next));
		trilinear_frac = probe_pos_next - vec3(probe_posi_next);
	}

	for (uint li = 0; li < dir_count; li++) {
		ivec3 own_pos = ivec3(pos.x, pos.y * RC_MAX_ANGULAR_TEXELS + int(li), int(params.cascade));
		vec4 own = imageLoad(radiance_cascade_trace, own_pos);

		vec3 merged_rgb = own.rgb;

		if (has_next && own.a > 0.0) {
			ivec2 local_xy = ivec2(int(li % oct_size), int(li / oct_size));
			ivec2 child_base = local_xy * int(ratio);

			vec3 accum = vec3(0.0);
			float accum_w = 0.0;

			for (uint cy = 0; cy < ratio; cy++) {
				for (uint cx = 0; cx < ratio; cx++) {
					uint child_li = rc_encode(child_base + ivec2(int(cx), int(cy)), next_oct_size);

					vec3 sample_accum = vec3(0.0);
					float sample_w = 0.0;

					for (int i = 0; i < 8; i++) {
						ivec3 offset = probe_posi_next + ((ivec3(i) >> ivec3(0, 1, 2)) & ivec3(1, 1, 1));
						if (any(lessThan(offset, ivec3(0))) || any(greaterThanEqual(offset, ivec3(params.probe_axis_size)))) {
							continue;
						}

						vec3 trilinear = vec3(1.0) - abs(trilinear_frac - vec3(offset - probe_posi_next));
						float weight = trilinear.x * trilinear.y * trilinear.z;
						if (weight <= 0.0) {
							continue;
						}

						ivec2 neighbor_pos;
						neighbor_pos = offset.xy;
						neighbor_pos.x += offset.z * int(params.probe_axis_size);

						ivec3 neighbor_read = ivec3(neighbor_pos.x, neighbor_pos.y * RC_MAX_ANGULAR_TEXELS + int(child_li), int(params.cascade + 1));
						vec4 neighbor = imageLoad(radiance_cascade_merged, neighbor_read);

						sample_accum += neighbor.rgb * weight;
						sample_w += weight;
					}

					if (sample_w > 0.0) {
						accum += sample_accum / sample_w;
						accum_w += 1.0;
					}
				}
			}

			if (accum_w > 0.0) {
				merged_rgb += own.a * (accum / accum_w);
			}
		}

		imageStore(radiance_cascade_merged, own_pos, vec4(merged_rgb, own.a));
	}

#endif // MODE_MERGE

#ifdef MODE_PROJECT

	vec3 sh_rgb[SH_TERMS];
	for (uint i = 0; i < SH_TERMS; i++) {
		sh_rgb[i] = vec3(0.0);
	}

	float weight = (4.0 * PI) / float(dir_count);

	for (uint li = 0; li < dir_count; li++) {
		ivec3 own_pos = ivec3(pos.x, pos.y * RC_MAX_ANGULAR_TEXELS + int(li), int(params.cascade));
		vec3 radiance = imageLoad(radiance_cascade_merged, own_pos).rgb;
		vec3 dir = rc_direction(li, oct_size);

		float basis[SH_TERMS];
		sh_basis(dir, basis);
		for (uint i = 0; i < SH_TERMS; i++) {
			sh_rgb[i] += radiance * basis[i] * weight;
		}
	}

	for (uint i = 0; i < SH_TERMS; i++) {
		ivec3 sh_pos = ivec3(pos.x, pos.y * SH_TERMS + int(i), int(params.cascade));
		imageStore(radiance_cascade_sh, sh_pos, vec4(sh_rgb[i], 0.0));
	}

	if (params.store_ambient_texture != 0) {
		vec3 ambient = sh_rgb[0] * 0.88622 / (4.0 * PI); //SHL0, normalized like sdfgi_integrate's ambient output
		imageStore(lightprobe_ambient_texture, ivec3(pos, int(params.cascade)), vec4(ambient, 1.0));
	}

#endif // MODE_PROJECT
}

#endif // MODE_TRACE || MODE_MERGE || MODE_PROJECT

#ifdef MODE_STORE

// One invocation per FINAL octahedral texel: same dispatch domain (and a
// direct copy of the encoding logic) as sdfgi_integrate.glsl's own
// MODE_STORE. params.image_size must be set to the larger, texel-grained
// size for this dispatch, exactly like the legacy pass does.
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
