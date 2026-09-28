///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2016-2021, Intel Corporation
//
// SPDX-License-Identifier: MIT
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// XeGTAO third pass (XeGTAO_Denoise): edge-aware spatial denoiser. It runs one or more times, the last pass
// (FINAL_APPLY) writes the final visibility (and bent normals) in the format sampled by the scene shader.
// See gtao_inc.glsl for details.
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "gtao_inc.glsl"

// Both textures must use a point (nearest) sampler with clamp to edge addressing.
#ifdef USE_BENT_NORMALS
layout(set = 0, binding = 0) uniform usampler2D source_ao_term;
#else
layout(set = 0, binding = 0) uniform sampler2D source_ao_term;
#endif
layout(set = 0, binding = 1) uniform sampler2D source_edges;

#ifdef FINAL_APPLY
#ifdef USE_BENT_NORMALS
// R: visibility, GBA: bent normal in Godot's view space.
layout(rgba8, set = 1, binding = 0) uniform restrict writeonly image2D dest_ao;
#else
layout(r8, set = 1, binding = 0) uniform restrict writeonly image2D dest_ao;
#endif
#else
#ifdef USE_BENT_NORMALS
layout(r32ui, set = 1, binding = 0) uniform restrict writeonly uimage2D dest_ao;
#else
layout(r8, set = 1, binding = 0) uniform restrict writeonly image2D dest_ao;
#endif
#endif

#ifdef USE_BENT_NORMALS
#define AO_TERM_TYPE vec4 // .xyz is bent normal, .w is visibility term.
#else
#define AO_TERM_TYPE float // .x is visibility term.
#endif

void add_sample(AO_TERM_TYPE p_ssao_value, float p_edge_value, inout AO_TERM_TYPE r_sum, inout float r_sum_weight) {
	float weight = p_edge_value;

	r_sum += (weight * p_ssao_value);
	r_sum_weight += weight;
}

void output_value(ivec2 p_pix_coord, AO_TERM_TYPE p_output_value) {
	if (any(greaterThanEqual(p_pix_coord, params.viewport_size))) {
		return;
	}

#ifdef USE_BENT_NORMALS
	vec3 bent_normal = normalize(p_output_value.xyz);
#ifdef FINAL_APPLY
	float visibility = clamp(p_output_value.w * XE_GTAO_OCCLUSION_TERM_SCALE, 0.0, 1.0);
	// Convert back to Godot's view space.
	bent_normal.z = -bent_normal.z;
	imageStore(dest_ao, p_pix_coord, vec4(visibility, bent_normal * 0.5 + 0.5));
#else
	imageStore(dest_ao, p_pix_coord, uvec4(gtao_encode_visibility_bent_normal(p_output_value.w, bent_normal)));
#endif
#else
#ifdef FINAL_APPLY
	p_output_value *= XE_GTAO_OCCLUSION_TERM_SCALE;
#endif
	imageStore(dest_ao, p_pix_coord, vec4(p_output_value));
#endif
}

// Returns the UV of the top left corner of p_texel. Gathering there returns the 2x2 quad of texels from
// p_texel - 1 to p_texel, the same as XeGTAO's GatherRed() with constant offsets from its gather center.
vec2 get_gather_uv(ivec2 p_texel) {
	return vec2(p_texel) * params.viewport_pixel_size;
}

void gather_ao_term(ivec2 p_texel, out AO_TERM_TYPE r_result[4]) {
#ifdef USE_BENT_NORMALS
	uvec4 packed_value = textureGather(source_ao_term, get_gather_uv(p_texel));
	gtao_decode_visibility_bent_normal(packed_value.x, r_result[0].w, r_result[0].xyz);
	gtao_decode_visibility_bent_normal(packed_value.y, r_result[1].w, r_result[1].xyz);
	gtao_decode_visibility_bent_normal(packed_value.z, r_result[2].w, r_result[2].xyz);
	gtao_decode_visibility_bent_normal(packed_value.w, r_result[3].w, r_result[3].xyz);
#else
	vec4 value = textureGather(source_ao_term, get_gather_uv(p_texel));
	r_result[0] = value.x;
	r_result[1] = value.y;
	r_result[2] = value.z;
	r_result[3] = value.w;
#endif
}

void main() {
	// We're computing 2 horizontal pixels at a time (performance optimization).
	const ivec2 pix_coord_base = ivec2(gl_GlobalInvocationID.xy) * ivec2(2, 1);
	if (any(greaterThanEqual(pix_coord_base, params.viewport_size))) {
		return;
	}

#ifdef FINAL_APPLY
	const float blur_amount = params.denoise_blur_beta;
#else
	const float blur_amount = params.denoise_blur_beta / 5.0;
#endif
	const float diag_weight = 0.85 * 0.5;

	vec4 edges_c_lrtb[2];
	float weight_tl[2];
	float weight_tr[2];
	float weight_bl[2];
	float weight_br[2];

	// Gather edge and visibility quads, used later.
	vec4 edges_q0 = textureGather(source_edges, get_gather_uv(pix_coord_base + ivec2(0, 0)));
	vec4 edges_q1 = textureGather(source_edges, get_gather_uv(pix_coord_base + ivec2(2, 0)));
	vec4 edges_q2 = textureGather(source_edges, get_gather_uv(pix_coord_base + ivec2(1, 2)));

	AO_TERM_TYPE vis_q0[4];
	gather_ao_term(pix_coord_base + ivec2(0, 0), vis_q0);
	AO_TERM_TYPE vis_q1[4];
	gather_ao_term(pix_coord_base + ivec2(2, 0), vis_q1);
	AO_TERM_TYPE vis_q2[4];
	gather_ao_term(pix_coord_base + ivec2(0, 2), vis_q2);
	AO_TERM_TYPE vis_q3[4];
	gather_ao_term(pix_coord_base + ivec2(2, 2), vis_q3);

	for (int side = 0; side < 2; side++) {
		const ivec2 pix_coord = ivec2(pix_coord_base.x + side, pix_coord_base.y);

		vec4 edges_l_lrtb = gtao_unpack_edges((side == 0) ? edges_q0.x : edges_q0.y);
		vec4 edges_t_lrtb = gtao_unpack_edges((side == 0) ? edges_q0.z : edges_q1.w);
		vec4 edges_r_lrtb = gtao_unpack_edges((side == 0) ? edges_q1.x : edges_q1.y);
		vec4 edges_b_lrtb = gtao_unpack_edges((side == 0) ? edges_q2.w : edges_q2.z);

		edges_c_lrtb[side] = gtao_unpack_edges((side == 0) ? edges_q0.y : edges_q1.x);

		// Edges aren't perfectly symmetrical: edge detection algorithm does not guarantee that a left edge on the right pixel
		// will match the right edge on the left pixel (although they will match in majority of cases). This line further
		// enforces the symmetricity, creating a slightly sharper blur. Works real nice with TAA.
		edges_c_lrtb[side] *= vec4(edges_l_lrtb.y, edges_r_lrtb.x, edges_t_lrtb.w, edges_b_lrtb.z);

		// This allows some small amount of AO leaking from neighbors if there are 3 or 4 edges;
		// this reduces both spatial and temporal aliasing.
		const float leak_threshold = 2.5;
		const float leak_strength = 0.5;
		float edginess = (clamp(4.0 - leak_threshold - dot(edges_c_lrtb[side], vec4(1.0)), 0.0, 1.0) / (4.0 - leak_threshold)) * leak_strength;
		edges_c_lrtb[side] = clamp(edges_c_lrtb[side] + edginess, 0.0, 1.0);

		// For diagonals; used by first and second pass.
		weight_tl[side] = diag_weight * (edges_c_lrtb[side].x * edges_l_lrtb.z + edges_c_lrtb[side].z * edges_t_lrtb.x);
		weight_tr[side] = diag_weight * (edges_c_lrtb[side].z * edges_t_lrtb.y + edges_c_lrtb[side].y * edges_r_lrtb.z);
		weight_bl[side] = diag_weight * (edges_c_lrtb[side].w * edges_b_lrtb.x + edges_c_lrtb[side].x * edges_l_lrtb.w);
		weight_br[side] = diag_weight * (edges_c_lrtb[side].y * edges_r_lrtb.w + edges_c_lrtb[side].w * edges_b_lrtb.y);

		// First pass.
		AO_TERM_TYPE ssao_value = (side == 0) ? vis_q0[1] : vis_q1[0];
		AO_TERM_TYPE ssao_value_l = (side == 0) ? vis_q0[0] : vis_q0[1];
		AO_TERM_TYPE ssao_value_t = (side == 0) ? vis_q0[2] : vis_q1[3];
		AO_TERM_TYPE ssao_value_r = (side == 0) ? vis_q1[0] : vis_q1[1];
		AO_TERM_TYPE ssao_value_b = (side == 0) ? vis_q2[2] : vis_q3[3];
		AO_TERM_TYPE ssao_value_tl = (side == 0) ? vis_q0[3] : vis_q0[2];
		AO_TERM_TYPE ssao_value_br = (side == 0) ? vis_q3[3] : vis_q3[2];
		AO_TERM_TYPE ssao_value_tr = (side == 0) ? vis_q1[3] : vis_q1[2];
		AO_TERM_TYPE ssao_value_bl = (side == 0) ? vis_q2[3] : vis_q2[2];

		float sum_weight = blur_amount;
		AO_TERM_TYPE sum = ssao_value * sum_weight;

		add_sample(ssao_value_l, edges_c_lrtb[side].x, sum, sum_weight);
		add_sample(ssao_value_r, edges_c_lrtb[side].y, sum, sum_weight);
		add_sample(ssao_value_t, edges_c_lrtb[side].z, sum, sum_weight);
		add_sample(ssao_value_b, edges_c_lrtb[side].w, sum, sum_weight);

		add_sample(ssao_value_tl, weight_tl[side], sum, sum_weight);
		add_sample(ssao_value_tr, weight_tr[side], sum, sum_weight);
		add_sample(ssao_value_bl, weight_bl[side], sum, sum_weight);
		add_sample(ssao_value_br, weight_br[side], sum, sum_weight);

		output_value(pix_coord, sum / sum_weight);
	}
}
