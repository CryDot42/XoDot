///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2016-2021, Intel Corporation
//
// SPDX-License-Identifier: MIT
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// XeGTAO second pass (XeGTAO_MainPass): computes the ambient occlusion term, edges for the denoiser and,
// optionally, bent normals. See gtao_inc.glsl for details.
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#[compute]

#version 450

#VERSION_DEFINES

// SLICE_COUNT and STEPS_PER_SLICE are defined by the quality level variant.
#ifndef SLICE_COUNT
#define SLICE_COUNT 3
#endif
#ifndef STEPS_PER_SLICE
#define STEPS_PER_SLICE 3
#endif

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "gtao_inc.glsl"

// Must be sampled with a point (nearest) filter for all of min, mag and mip. Linear filtering causes unwanted
// interpolation between neighboring depth values on the same MIP level.
layout(set = 0, binding = 0) uniform sampler2D source_depth_mips;
layout(rgba8, set = 0, binding = 1) uniform restrict readonly image2D source_normal;
layout(set = 0, binding = 2) uniform usampler2D hilbert_lut;

#ifdef USE_BENT_NORMALS
layout(r32ui, set = 1, binding = 0) uniform restrict writeonly uimage2D dest_ao_term;
#else
layout(r8, set = 1, binding = 0) uniform restrict writeonly image2D dest_ao_term;
#endif
layout(r8, set = 1, binding = 1) uniform restrict writeonly image2D dest_edges;

// http://h14s.p5r.org/2012/09/0x5f3759df.html, [Drobot2014a] Low Level Optimizations for GCN, https://blog.selfshadow.com/publications/s2016-shading-course/activision/s2016_pbs_activision_occlusion.pdf slide 63
float fast_sqrt(float p_x) {
	return intBitsToFloat(0x1fbd1df5 + (floatBitsToInt(p_x) >> 1));
}

// Input [-1, 1] and output [0, PI], from https://seblagarde.wordpress.com/2014/12/01/inverse-trigonometric-functions-gpu-optimization-for-amd-gcn-architecture/
float fast_acos(float p_in_x) {
	const float PI = 3.141593;
	const float HALF_PI = 1.570796;
	float x = abs(p_in_x);
	float res = -0.156583 * x + HALF_PI;
	res *= fast_sqrt(1.0 - x);
	return (p_in_x >= 0.0) ? res : PI - res;
}

// "Efficiently building a matrix to rotate one vector to another"
// http://cs.brown.edu/research/pubs/pdfs/1999/Moller-1999-EBA.pdf / https://dl.acm.org/doi/10.1080/10867651.1999.10487509
// Only used with p_from = (0, 0, -1).
mat3 rot_from_to_matrix(vec3 p_from, vec3 p_to) {
	const float e = dot(p_from, p_to);
	const float f = abs(e);

	if (f > (1.0 - 0.0003)) {
		return mat3(1.0);
	}

	const vec3 v = cross(p_from, p_to);
	const float h = 1.0 / (1.0 + e);
	const float hvx = h * v.x;
	const float hvz = h * v.z;
	const float hvxy = hvx * v.y;
	const float hvxz = hvx * v.z;
	const float hvyz = hvz * v.y;

	// GLSL matrices are column-major.
	return mat3(
			vec3(e + hvx * v.x, hvxy + v.z, hvxz - v.y),
			vec3(hvxy - v.z, e + h * v.y * v.y, hvyz + v.x),
			vec3(hvxz + v.y, hvyz - v.x, e + hvz * v.z));
}

vec3 load_normal(ivec2 p_pos) {
	// Normals are stored in Godot's view space, flip Z to get to XeGTAO's view space.
	vec3 normal = normalize(imageLoad(source_normal, p_pos).xyz * 2.0 - 1.0);
	normal.z = -normal.z;
	return normal;
}

// Hilbert curve driving R2 (see https://www.shadertoy.com/view/3tB3z3).
vec2 spatio_temporal_noise(ivec2 p_pix_coord, uint p_temporal_index) {
	uint index = texelFetch(hilbert_lut, p_pix_coord % 64, 0).x;
	// Why 288? Tried out a few and that's the best so far (with a 6 level Hilbert curve) - but there's probably better :)
	index += 288 * (p_temporal_index % 64);
	// R2 sequence - see http://extremelearning.com.au/unreasonable-effectiveness-of-quasirandom-sequences/
	return fract(0.5 + float(index) * vec2(0.75487766624669276005, 0.5698402909980532659114));
}

void output_working_term(ivec2 p_pix_coord, float p_visibility, vec3 p_bent_normal) {
	p_visibility = clamp(p_visibility / XE_GTAO_OCCLUSION_TERM_SCALE, 0.0, 1.0);
#ifdef USE_BENT_NORMALS
	imageStore(dest_ao_term, p_pix_coord, uvec4(gtao_encode_visibility_bent_normal(p_visibility, p_bent_normal)));
#else
	imageStore(dest_ao_term, p_pix_coord, vec4(p_visibility));
#endif
}

float load_depth(ivec2 p_pos, ivec2 p_max_pos) {
	return texelFetch(source_depth_mips, clamp(p_pos, ivec2(0), p_max_pos), 0).r;
}

void main() {
	const ivec2 pix_coord = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pix_coord, params.viewport_size))) {
		return;
	}

	const vec2 normalized_screen_pos = (vec2(pix_coord) + 0.5) * params.viewport_pixel_size;
	const ivec2 max_pos = params.viewport_size - 1;

	// View space Z at the center.
	float viewspace_z = load_depth(pix_coord, max_pos);

	// View space Zs left, top, right, bottom.
	const float pix_lz = load_depth(pix_coord + ivec2(-1, 0), max_pos);
	const float pix_tz = load_depth(pix_coord + ivec2(0, -1), max_pos);
	const float pix_rz = load_depth(pix_coord + ivec2(1, 0), max_pos);
	const float pix_bz = load_depth(pix_coord + ivec2(0, 1), max_pos);

	vec4 edges_lrtb = gtao_calculate_edges(viewspace_z, pix_lz, pix_rz, pix_tz, pix_bz);
	imageStore(dest_edges, pix_coord, vec4(gtao_pack_edges(edges_lrtb)));

	const vec3 viewspace_normal = load_normal(pix_coord);

	// Move center pixel slightly towards camera to avoid imprecision artifacts due to depth buffer imprecision;
	// offset depends on depth texture format used (this is good for a FP16 depth buffer).
	viewspace_z *= 0.99920;

	const vec3 pix_center_pos = gtao_compute_viewspace_position(normalized_screen_pos, viewspace_z);
	const vec3 view_vec = params.orthogonal ? vec3(0.0, 0.0, -1.0) : normalize(-pix_center_pos);

	const float effect_radius = params.effect_radius * params.radius_multiplier;
	const float sample_distribution_power = params.sample_distribution_power;
	const float thin_occluder_compensation = params.thin_occluder_compensation;
	const float falloff_range = params.effect_falloff_range * effect_radius;

	const float falloff_from = effect_radius * (1.0 - params.effect_falloff_range);

	// Fadeout precompute optimization.
	const float falloff_mul = -1.0 / falloff_range;
	const float falloff_add = falloff_from / falloff_range + 1.0;

	float visibility = 0.0;
#ifdef USE_BENT_NORMALS
	vec3 bent_normal = vec3(0.0);
#else
	vec3 bent_normal = viewspace_normal;
#endif

	// See "Algorithm 1" in https://www.activision.com/cdn/research/Practical_Real_Time_Strategies_for_Accurate_Indirect_Occlusion_NEW%20VERSION_COLOR.pdf
	{
		const vec2 local_noise = spatio_temporal_noise(pix_coord, params.noise_index);
		const float noise_slice = local_noise.x;
		const float noise_sample = local_noise.y;

		// If the offset is under approx pixel size (pixel_too_close_threshold), push it out to the minimum distance.
		const float pixel_too_close_threshold = 1.3;

		// Approx view space pixel size at pix_coord; approximation of NDCToViewspace(normalized_screen_pos.xy + viewport_pixel_size.xy, pix_center_pos.z).xy - pix_center_pos.xy.
		const vec2 pixel_dir_rb_viewspace_size_at_center_z = params.orthogonal ? params.ndc_to_view_mul_x_pixel_size : viewspace_z * params.ndc_to_view_mul_x_pixel_size;

		float screenspace_radius = effect_radius / pixel_dir_rb_viewspace_size_at_center_z.x;

		// Fade out for small screen radii.
		visibility += clamp((10.0 - screenspace_radius) / 100.0, 0.0, 1.0) * 0.5;

		// This is the min distance to start sampling from to avoid sampling from the center pixel (no useful data obtained from sampling center pixel).
		const float min_s = pixel_too_close_threshold / screenspace_radius;

		for (int slice_i = 0; slice_i < SLICE_COUNT; slice_i++) {
			const float slice = float(slice_i);
			float slice_k = (slice + noise_slice) / float(SLICE_COUNT);
			// Lines 5, 6 from the paper.
			float phi = slice_k * XE_GTAO_PI;
			float cos_phi = cos(phi);
			float sin_phi = sin(phi);
			vec2 omega = vec2(cos_phi, -sin_phi);

			// Convert to screen units (pixels) for later use.
			omega *= screenspace_radius;

			// Line 8 from the paper.
			const vec3 direction_vec = vec3(cos_phi, sin_phi, 0.0);

			// Line 9 from the paper.
			const vec3 ortho_direction_vec = direction_vec - (dot(direction_vec, view_vec) * view_vec);

			// Line 10 from the paper.
			// axis_vec is orthogonal to direction_vec and view_vec, used to define projected_normal.
			const vec3 axis_vec = normalize(cross(ortho_direction_vec, view_vec));

			// Line 11 from the paper.
			vec3 projected_normal_vec = viewspace_normal - axis_vec * dot(viewspace_normal, axis_vec);

			// Line 13 from the paper.
			float sign_norm = sign(dot(ortho_direction_vec, projected_normal_vec));

			// Line 14 from the paper.
			float projected_normal_vec_length = length(projected_normal_vec);
			float cos_norm = clamp(dot(projected_normal_vec, view_vec) / max(projected_normal_vec_length, 1e-6), 0.0, 1.0);

			// Line 15 from the paper.
			float n = sign_norm * fast_acos(cos_norm);

			// This is a lower weight target; not using -1 as in the original paper because it is under horizon,
			// so a 'weight' has different meaning based on the normal.
			const float low_horizon_cos0 = cos(n + XE_GTAO_PI_HALF);
			const float low_horizon_cos1 = cos(n - XE_GTAO_PI_HALF);

			// Lines 17, 18 from the paper, manually unrolled the 'side' loop.
			float horizon_cos0 = low_horizon_cos0;
			float horizon_cos1 = low_horizon_cos1;

			for (int step_i = 0; step_i < STEPS_PER_SLICE; step_i++) {
				const float step_f = float(step_i);
				// R1 sequence (http://extremelearning.com.au/unreasonable-effectiveness-of-quasirandom-sequences/).
				const float step_base_noise = (slice + step_f * float(STEPS_PER_SLICE)) * 0.6180339887498948482;
				float step_noise = fract(noise_sample + step_base_noise);

				// Approx line 20 from the paper, with added noise.
				float s = (step_f + step_noise) / float(STEPS_PER_SLICE);

				// Additional distribution modifier.
				s = pow(s, sample_distribution_power);

				// Avoid sampling center pixel.
				s += min_s;

				// Approx lines 21-22 from the paper, unrolled.
				vec2 sample_offset = s * omega;

				float sample_offset_length = length(sample_offset);

				// Note: when sampling, using point_point_point or point_point_linear sampler works, but linear_linear_linear
				// will cause unwanted interpolation between neighboring depth values on the same MIP level!
				const float mip_level = clamp(log2(sample_offset_length) - params.depth_mip_sampling_offset, 0.0, float(XE_GTAO_DEPTH_MIP_LEVELS));

				// Snap to pixel center (more correct direction math, avoids artifacts due to sampling pos not matching depth
				// texel center - messes up slope - but adds other artifacts due to them being pushed off the slice).
				sample_offset = round(sample_offset) * params.viewport_pixel_size;

				vec2 sample_screen_pos0 = normalized_screen_pos + sample_offset;
				float sz0 = textureLod(source_depth_mips, sample_screen_pos0, mip_level).x;
				vec3 sample_pos0 = gtao_compute_viewspace_position(sample_screen_pos0, sz0);

				vec2 sample_screen_pos1 = normalized_screen_pos - sample_offset;
				float sz1 = textureLod(source_depth_mips, sample_screen_pos1, mip_level).x;
				vec3 sample_pos1 = gtao_compute_viewspace_position(sample_screen_pos1, sz1);

				vec3 sample_delta0 = sample_pos0 - pix_center_pos;
				vec3 sample_delta1 = sample_pos1 - pix_center_pos;
				float sample_dist0 = length(sample_delta0);
				float sample_dist1 = length(sample_delta1);

				// Approx lines 23, 24 from the paper, unrolled.
				vec3 sample_horizon_vec0 = sample_delta0 / sample_dist0;
				vec3 sample_horizon_vec1 = sample_delta1 / sample_dist1;

				// Any sample out of radius should be discarded - also use falloff range for smooth transitions;
				// this is a modified idea from "4.3 Implementation details, Bounding the sampling area".
				// This is our own thickness heuristic that relies on sooner discarding samples behind the center.
				float falloff_base0 = length(vec3(sample_delta0.x, sample_delta0.y, sample_delta0.z * (1.0 + thin_occluder_compensation)));
				float falloff_base1 = length(vec3(sample_delta1.x, sample_delta1.y, sample_delta1.z * (1.0 + thin_occluder_compensation)));
				float weight0 = clamp(falloff_base0 * falloff_mul + falloff_add, 0.0, 1.0);
				float weight1 = clamp(falloff_base1 * falloff_mul + falloff_add, 0.0, 1.0);

				// Sample horizon cos.
				float shc0 = dot(sample_horizon_vec0, view_vec);
				float shc1 = dot(sample_horizon_vec1, view_vec);

				// Discard unwanted samples.
				// This would be more correct but too expensive: cos(mix(acos(low_horizon_cos0), acos(shc0), weight0)).
				shc0 = mix(low_horizon_cos0, shc0, weight0);
				shc1 = mix(low_horizon_cos1, shc1, weight1);

				// Thickness heuristic - see "4.3 Implementation details, Height-field assumption considerations".
				// This is a version where the thickness heuristic is completely disabled; thin_occluder_compensation is used above instead.
				horizon_cos0 = max(horizon_cos0, shc0);
				horizon_cos1 = max(horizon_cos1, shc1);
			}

			// Counters the slight overdarkening on high slopes.
			projected_normal_vec_length = mix(projected_normal_vec_length, 1.0, 0.05);

			// Line ~27, unrolled.
			float h0 = -fast_acos(horizon_cos1);
			float h1 = fast_acos(horizon_cos0);
			float iarc0 = (cos_norm + 2.0 * h0 * sin(n) - cos(2.0 * h0 - n)) / 4.0;
			float iarc1 = (cos_norm + 2.0 * h1 * sin(n) - cos(2.0 * h1 - n)) / 4.0;
			float local_visibility = projected_normal_vec_length * (iarc0 + iarc1);
			visibility += local_visibility;

#ifdef USE_BENT_NORMALS
			// See "Algorithm 2 Extension that computes bent normals b.".
			float t0 = (6.0 * sin(h0 - n) - sin(3.0 * h0 - n) + 6.0 * sin(h1 - n) - sin(3.0 * h1 - n) + 16.0 * sin(n) - 3.0 * (sin(h0 + n) + sin(h1 + n))) / 12.0;
			float t1 = (-cos(3.0 * h0 - n) - cos(3.0 * h1 - n) + 8.0 * cos(n) - 3.0 * (cos(h0 + n) + cos(h1 + n))) / 12.0;
			vec3 local_bent_normal = vec3(direction_vec.x * t0, direction_vec.y * t0, -t1);
			local_bent_normal = (rot_from_to_matrix(vec3(0.0, 0.0, -1.0), view_vec) * local_bent_normal) * projected_normal_vec_length;
			bent_normal += local_bent_normal;
#endif
		}
		visibility /= float(SLICE_COUNT);
		visibility = pow(max(visibility, 1e-4), params.final_value_power);
		// Disallow total occlusion (which wouldn't make any sense anyhow since pixel is visible but also helps with packing bent normals).
		visibility = max(0.03, visibility);

#ifdef USE_BENT_NORMALS
		bent_normal = normalize(bent_normal);
#endif
	}

	output_working_term(pix_coord, visibility, bent_normal);
}
