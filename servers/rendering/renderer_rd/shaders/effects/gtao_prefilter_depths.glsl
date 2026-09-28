///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2016-2021, Intel Corporation
//
// SPDX-License-Identifier: MIT
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// XeGTAO first pass (XeGTAO_PrefilterDepths16x16): converts the depth buffer into a view space depth MIP chain.
// See gtao_inc.glsl for details.
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#[compute]

#version 450

#VERSION_DEFINES

// Hard coded to 8x8; each thread computes 2x2 blocks so each group processes a 16x16 block.
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "gtao_inc.glsl"

layout(set = 0, binding = 0) uniform sampler2D source_depth;

layout(r16f, set = 1, binding = 0) uniform restrict writeonly image2D dest_depth_mip0;
layout(r16f, set = 1, binding = 1) uniform restrict writeonly image2D dest_depth_mip1;
layout(r16f, set = 1, binding = 2) uniform restrict writeonly image2D dest_depth_mip2;
layout(r16f, set = 1, binding = 3) uniform restrict writeonly image2D dest_depth_mip3;
layout(r16f, set = 1, binding = 4) uniform restrict writeonly image2D dest_depth_mip4;

shared float scratch_depths[8][8];

// Weighted average depth filter.
float depth_mip_filter(float p_depth0, float p_depth1, float p_depth2, float p_depth3) {
	float max_depth = max(max(p_depth0, p_depth1), max(p_depth2, p_depth3));

	const float depth_range_scale_factor = 0.75; // Found empirically :)
	float effect_radius = depth_range_scale_factor * params.effect_radius * params.radius_multiplier;
	float falloff_range = params.effect_falloff_range * effect_radius;
	float falloff_from = effect_radius * (1.0 - params.effect_falloff_range);
	// Fadeout precompute optimization.
	float falloff_mul = -1.0 / falloff_range;
	float falloff_add = falloff_from / falloff_range + 1.0;

	float weight0 = clamp((max_depth - p_depth0) * falloff_mul + falloff_add, 0.0, 1.0);
	float weight1 = clamp((max_depth - p_depth1) * falloff_mul + falloff_add, 0.0, 1.0);
	float weight2 = clamp((max_depth - p_depth2) * falloff_mul + falloff_add, 0.0, 1.0);
	float weight3 = clamp((max_depth - p_depth3) * falloff_mul + falloff_add, 0.0, 1.0);

	float weight_sum = weight0 + weight1 + weight2 + weight3;
	return (weight0 * p_depth0 + weight1 * p_depth1 + weight2 * p_depth2 + weight3 * p_depth3) / weight_sum;
}

// This is also a good place to do non-linear depth conversion for cases where one wants the 'radius'
// (effectively the threshold between near-field and far-field GI) to be non-linear (i.e. very large outdoors environments).
float clamp_depth(float p_depth) {
	// The depth MIP chain is stored in a half float texture.
	return clamp(p_depth, 0.0, 65504.0);
}

float load_view_depth(ivec2 p_pos, ivec2 p_max_pos) {
	return clamp_depth(gtao_screen_space_to_view_space_depth(texelFetch(source_depth, min(p_pos, p_max_pos), 0).r));
}

void store_depth_mip(uint p_mip, ivec2 p_pos, float p_depth) {
	if (p_mip >= params.depth_mip_count) {
		// This MIP level doesn't exist, the image bound to it is a lower MIP level.
		return;
	}

	// Groups can overhang the edges of the viewport, so bounds check every write.
	switch (p_mip) {
		case 0u: {
			if (all(lessThan(p_pos, imageSize(dest_depth_mip0)))) {
				imageStore(dest_depth_mip0, p_pos, vec4(p_depth));
			}
		} break;
		case 1u: {
			if (all(lessThan(p_pos, imageSize(dest_depth_mip1)))) {
				imageStore(dest_depth_mip1, p_pos, vec4(p_depth));
			}
		} break;
		case 2u: {
			if (all(lessThan(p_pos, imageSize(dest_depth_mip2)))) {
				imageStore(dest_depth_mip2, p_pos, vec4(p_depth));
			}
		} break;
		case 3u: {
			if (all(lessThan(p_pos, imageSize(dest_depth_mip3)))) {
				imageStore(dest_depth_mip3, p_pos, vec4(p_depth));
			}
		} break;
		case 4u: {
			if (all(lessThan(p_pos, imageSize(dest_depth_mip4)))) {
				imageStore(dest_depth_mip4, p_pos, vec4(p_depth));
			}
		} break;
	}
}

void main() {
	// MIP 0
	const ivec2 base_coord = ivec2(gl_GlobalInvocationID.xy);
	const ivec2 group_thread_id = ivec2(gl_LocalInvocationID.xy);
	const ivec2 pix_coord = base_coord * 2;
	const ivec2 max_pos = textureSize(source_depth, 0) - 1;

	float depth0 = load_view_depth(pix_coord + ivec2(0, 0), max_pos);
	float depth1 = load_view_depth(pix_coord + ivec2(1, 0), max_pos);
	float depth2 = load_view_depth(pix_coord + ivec2(0, 1), max_pos);
	float depth3 = load_view_depth(pix_coord + ivec2(1, 1), max_pos);
	store_depth_mip(0u, pix_coord + ivec2(0, 0), depth0);
	store_depth_mip(0u, pix_coord + ivec2(1, 0), depth1);
	store_depth_mip(0u, pix_coord + ivec2(0, 1), depth2);
	store_depth_mip(0u, pix_coord + ivec2(1, 1), depth3);

	// MIP 1
	float dm1 = depth_mip_filter(depth0, depth1, depth2, depth3);
	store_depth_mip(1u, base_coord, dm1);
	scratch_depths[group_thread_id.x][group_thread_id.y] = dm1;

	memoryBarrierShared();
	barrier();

	// MIP 2
	if (all(equal(group_thread_id % 2, ivec2(0)))) {
		float in_tl = scratch_depths[group_thread_id.x + 0][group_thread_id.y + 0];
		float in_tr = scratch_depths[group_thread_id.x + 1][group_thread_id.y + 0];
		float in_bl = scratch_depths[group_thread_id.x + 0][group_thread_id.y + 1];
		float in_br = scratch_depths[group_thread_id.x + 1][group_thread_id.y + 1];

		float dm2 = depth_mip_filter(in_tl, in_tr, in_bl, in_br);
		store_depth_mip(2u, base_coord / 2, dm2);
		scratch_depths[group_thread_id.x][group_thread_id.y] = dm2;
	}

	memoryBarrierShared();
	barrier();

	// MIP 3
	if (all(equal(group_thread_id % 4, ivec2(0)))) {
		float in_tl = scratch_depths[group_thread_id.x + 0][group_thread_id.y + 0];
		float in_tr = scratch_depths[group_thread_id.x + 2][group_thread_id.y + 0];
		float in_bl = scratch_depths[group_thread_id.x + 0][group_thread_id.y + 2];
		float in_br = scratch_depths[group_thread_id.x + 2][group_thread_id.y + 2];

		float dm3 = depth_mip_filter(in_tl, in_tr, in_bl, in_br);
		store_depth_mip(3u, base_coord / 4, dm3);
		scratch_depths[group_thread_id.x][group_thread_id.y] = dm3;
	}

	memoryBarrierShared();
	barrier();

	// MIP 4
	if (all(equal(group_thread_id % 8, ivec2(0)))) {
		float in_tl = scratch_depths[group_thread_id.x + 0][group_thread_id.y + 0];
		float in_tr = scratch_depths[group_thread_id.x + 4][group_thread_id.y + 0];
		float in_bl = scratch_depths[group_thread_id.x + 0][group_thread_id.y + 4];
		float in_br = scratch_depths[group_thread_id.x + 4][group_thread_id.y + 4];

		float dm4 = depth_mip_filter(in_tl, in_tr, in_bl, in_br);
		store_depth_mip(4u, base_coord / 8, dm4);
	}
}
