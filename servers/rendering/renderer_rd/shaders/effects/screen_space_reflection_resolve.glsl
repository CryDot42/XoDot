#[compute]

#version 450

#VERSION_DEFINES

#include "../screen_space_reflection_inc.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D source_depth;
layout(set = 0, binding = 1) uniform sampler2D source_normal_roughness;
layout(set = 0, binding = 2) uniform sampler2D source_depth_half;
layout(set = 0, binding = 3) uniform sampler2D source_normal_roughness_half;
layout(set = 0, binding = 4) uniform sampler2D source_color;
layout(set = 0, binding = 5) uniform sampler2D source_mip_level;
layout(rgba16f, set = 0, binding = 6) uniform restrict writeonly image2D output_color;

layout(push_constant, std430) uniform Params {
	ivec2 screen_size;
	ivec2 half_screen_size;
}
params;

// Returns the log of the weight of a half resolution sample, based on how similar its surface is to the one being resolved.
// Keeping the log allows normalizing the weights before exponentiating them, so they never all underflow to zero.
float get_sample(float depth, vec3 normal, float roughness, ivec2 pixel_pos, out vec4 color) {
	pixel_pos = clamp(pixel_pos, ivec2(0), params.half_screen_size - 1);

	float sample_depth = texelFetch(source_depth_half, pixel_pos, 0).x;
	vec4 sample_normal_roughness = texelFetch(source_normal_roughness_half, pixel_pos, 0);
	vec3 sample_normal = ssr_decode_normal(sample_normal_roughness.xyz);
	float sample_roughness = ssr_decode_roughness(sample_normal_roughness.w);

	vec2 uv = (pixel_pos + 0.5) / params.half_screen_size;

	float mip_level = texelFetch(source_mip_level, pixel_pos, 0).x * SSR_MIP_LEVEL_RANGE;
	color = textureLod(source_color, uv, mip_level);
	color.rgb = ssr_inverse_tonemap(color.rgb);

	const float DEPTH_FACTOR = 2048.0;
	const float NORMAL_FACTOR = 32.0;
	const float ROUGHNESS_FACTOR = 16.0;

	float depth_diff = abs(depth - sample_depth);
	float normal_diff = clamp(1.0 - dot(normal, sample_normal), 0.0, 1.0);
	float roughness_diff = abs(roughness - sample_roughness);

	return -(depth_diff * DEPTH_FACTOR + normal_diff * NORMAL_FACTOR + roughness_diff * ROUGHNESS_FACTOR);
}

void main() {
	ivec2 pixel_pos = ivec2(gl_GlobalInvocationID.xy);

	if (any(greaterThanEqual(pixel_pos, params.screen_size))) {
		return;
	}

	float depth = texelFetch(source_depth, pixel_pos, 0).x;
	vec4 normal_roughness = texelFetch(source_normal_roughness, pixel_pos, 0);
	float roughness = ssr_decode_roughness(normal_roughness.w);

	// Same cutoff as the trace pass. The sky and rough materials have no reflection to resolve.
	if (depth == 0.0 || roughness >= 0.7) {
		imageStore(output_color, pixel_pos, vec4(0.0));
		return;
	}

	vec3 normal = ssr_decode_normal(normal_roughness.xyz);

	// Bilinear footprint of this pixel in the half resolution buffers.
	// The shift is a floor division, so the first row and column do not get the wrong footprint.
	ivec2 base_pos = (pixel_pos - 1) >> 1;
	vec2 bilinear_weights = fract((pixel_pos + 0.5) * 0.5);

	vec4 color0, color1, color2, color3;
	float log_weight0 = get_sample(depth, normal, roughness, base_pos + ivec2(0, 0), color0);
	float log_weight1 = get_sample(depth, normal, roughness, base_pos + ivec2(1, 0), color1);
	float log_weight2 = get_sample(depth, normal, roughness, base_pos + ivec2(0, 1), color2);
	float log_weight3 = get_sample(depth, normal, roughness, base_pos + ivec2(1, 1), color3);

	// Normalize so that the most similar sample has a weight of 1. Weights only matter relative to each other,
	// and this makes the result fall back to the best matching sample when none of them match well.
	float max_log_weight = max(max(log_weight0, log_weight1), max(log_weight2, log_weight3));

	float weight0 = exp(log_weight0 - max_log_weight) * bilinear_weights.x * bilinear_weights.y;
	float weight1 = exp(log_weight1 - max_log_weight) * (1.0 - bilinear_weights.x) * bilinear_weights.y;
	float weight2 = exp(log_weight2 - max_log_weight) * bilinear_weights.x * (1.0 - bilinear_weights.y);
	float weight3 = exp(log_weight3 - max_log_weight) * (1.0 - bilinear_weights.x) * (1.0 - bilinear_weights.y);

	vec4 result_color = color0 * weight0 + color1 * weight1 + color2 * weight2 + color3 * weight3;
	result_color /= weight0 + weight1 + weight2 + weight3;

	imageStore(output_color, pixel_pos, result_color);
}
