///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2016-2021, Intel Corporation
//
// SPDX-License-Identifier: MIT
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// XeGTAO is based on GTAO/GTSO "Jimenez et al. / Practical Real-Time Strategies for Accurate Indirect Occlusion",
// https://www.activision.com/cdn/research/Practical_Real_Time_Strategies_for_Accurate_Indirect_Occlusion_NEW%20VERSION_COLOR.pdf
//
// Implementation:  Filip Strugar (filip.strugar@intel.com), Steve Mccalla <stephen.mccalla@intel.com>
// Version:         1.30
// Details:         https://github.com/GameTechDev/XeGTAO
//
// Ported to GLSL/Vulkan for Godot. Differences from the reference implementation:
// - Full precision floats are used everywhere (no min16float).
// - Orthographic and off-center (XR) projections are supported.
// - The final output stores the visibility in the red channel and the (optional) bent normal
//   in Godot's view space in the green, blue and alpha channels.
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Coordinate conventions: XeGTAO works in a left-handed view space where +Z points away from the camera,
// +Y points up and +X points right. Godot's view space only differs in that +Z points towards the camera,
// so normals and bent normals only need their Z component flipped when converting between the two.

#define XE_GTAO_PI (3.1415926535897932384626433832795)
#define XE_GTAO_PI_HALF (1.5707963267948966192313216916398)

#define XE_GTAO_DEPTH_MIP_LEVELS 5

// For packing in UNORM (because the raw, pre-denoised occlusion term can overshoot 1 but will later average out to 1).
#define XE_GTAO_OCCLUSION_TERM_SCALE 1.5

// This push constant is shared by all XeGTAO passes (equivalent of XeGTAO's GTAOConstants).
layout(push_constant, std430) uniform Params {
	ivec2 viewport_size;
	vec2 viewport_pixel_size; // 1.0 / viewport_size

	vec2 depth_unpack_consts;
	vec2 ndc_to_view_mul;

	vec2 ndc_to_view_add;
	vec2 ndc_to_view_mul_x_pixel_size;

	float effect_radius; // World (view) space maximum size of the occlusion.
	float effect_falloff_range;
	float radius_multiplier;
	float final_value_power;

	float denoise_blur_beta;
	float sample_distribution_power;
	float thin_occluder_compensation;
	float depth_mip_sampling_offset;

	uint noise_index; // frame_index % 64 if using TAA, or 0 otherwise.
	bool orthogonal;
	uint depth_mip_count; // Usually XE_GTAO_DEPTH_MIP_LEVELS, can be less for very small viewports.
	uint pad;
}
params;

// Converts a depth buffer value (reverse Z) to a positive view space depth.
float gtao_screen_space_to_view_space_depth(float p_screen_depth) {
	if (params.orthogonal) {
		return params.depth_unpack_consts.y + params.depth_unpack_consts.x * p_screen_depth;
	}
	// Optimized version of "-cameraClipNear / (cameraClipFar - projDepth * (cameraClipFar - cameraClipNear)) * cameraClipFar".
	return params.depth_unpack_consts.x / (params.depth_unpack_consts.y - p_screen_depth);
}

// Inputs are screen UV and view space depth, output is the view space position.
vec3 gtao_compute_viewspace_position(vec2 p_screen_pos, float p_viewspace_depth) {
	vec3 ret;
	if (params.orthogonal) {
		ret.xy = params.ndc_to_view_mul * p_screen_pos + params.ndc_to_view_add;
	} else {
		ret.xy = (params.ndc_to_view_mul * p_screen_pos + params.ndc_to_view_add) * p_viewspace_depth;
	}
	ret.z = p_viewspace_depth;
	return ret;
}

vec4 gtao_calculate_edges(float p_center_z, float p_left_z, float p_right_z, float p_top_z, float p_bottom_z) {
	vec4 edges_lrtb = vec4(p_left_z, p_right_z, p_top_z, p_bottom_z) - p_center_z;

	float slope_lr = (edges_lrtb.y - edges_lrtb.x) * 0.5;
	float slope_tb = (edges_lrtb.w - edges_lrtb.z) * 0.5;
	vec4 edges_lrtb_slope_adjusted = edges_lrtb + vec4(slope_lr, -slope_lr, slope_tb, -slope_tb);
	edges_lrtb = min(abs(edges_lrtb), abs(edges_lrtb_slope_adjusted));
	return clamp(1.25 - edges_lrtb / (p_center_z * 0.011), 0.0, 1.0);
}

// Packing/unpacking for edges; 2 bits per edge mean 4 gradient values (0, 0.33, 0.66, 1) for smoother transitions!
float gtao_pack_edges(vec4 p_edges_lrtb) {
	p_edges_lrtb = round(clamp(p_edges_lrtb, 0.0, 1.0) * 2.9);
	return dot(p_edges_lrtb, vec4(64.0 / 255.0, 16.0 / 255.0, 4.0 / 255.0, 1.0 / 255.0));
}

vec4 gtao_unpack_edges(float p_packed_val) {
	uint packed_val = uint(p_packed_val * 255.5);
	vec4 edges_lrtb;
	edges_lrtb.x = float((packed_val >> 6) & 0x03) / 3.0;
	edges_lrtb.y = float((packed_val >> 4) & 0x03) / 3.0;
	edges_lrtb.z = float((packed_val >> 2) & 0x03) / 3.0;
	edges_lrtb.w = float((packed_val >> 0) & 0x03) / 3.0;

	return clamp(edges_lrtb, 0.0, 1.0);
}

// Packs the visibility term and bent normal (in XeGTAO view space) into R8G8B8A8_UNORM.
uint gtao_encode_visibility_bent_normal(float p_visibility, vec3 p_bent_normal) {
	return packUnorm4x8(vec4(p_bent_normal * 0.5 + 0.5, p_visibility));
}

void gtao_decode_visibility_bent_normal(uint p_packed_value, out float r_visibility, out vec3 r_bent_normal) {
	vec4 decoded = unpackUnorm4x8(p_packed_value);
	// Could normalize - don't want to since it's done so many times, better to do it at the final step only.
	r_bent_normal = decoded.xyz * 2.0 - 1.0;
	r_visibility = decoded.w;
}
