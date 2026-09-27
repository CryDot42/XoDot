#[compute]

#version 450

#VERSION_DEFINES

// Derived landscape maps, generated on the GPU after the source data changes:
// MODE_NORMALS computes the per-texel normal map from the heightmap and
// MODE_DOWNSAMPLE builds one mipmap level of the normal map or of a weightmap layer.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(push_constant, std430) uniform Params {
	ivec2 rect_position; // Destination rect.
	ivec2 rect_size;
	ivec2 source_size; // Size of the source image.
	float spacing; // Distance between two heightmap texels (normals only).
	float pad;
}
params;

#ifdef MODE_NORMALS

layout(set = 0, binding = 0, r32f) uniform restrict readonly image2D height_image;
layout(set = 0, binding = 1, rgba8) uniform restrict writeonly image2D normal_image;

float get_height(ivec2 p_pos) {
	return imageLoad(height_image, clamp(p_pos, ivec2(0), params.source_size - 1)).r;
}

void main() {
	ivec2 local = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(local, params.rect_size))) {
		return;
	}
	ivec2 pos = params.rect_position + local;
	float left = get_height(pos + ivec2(-1, 0));
	float right = get_height(pos + ivec2(1, 0));
	float down = get_height(pos + ivec2(0, -1));
	float up = get_height(pos + ivec2(0, 1));
	vec3 normal = normalize(vec3(left - right, 2.0 * params.spacing, down - up));
	imageStore(normal_image, pos, vec4(normal * 0.5 + 0.5, 1.0));
}

#endif // MODE_NORMALS

#ifdef MODE_DOWNSAMPLE

layout(set = 0, binding = 0, rgba8) uniform restrict readonly image2D source_image;
layout(set = 0, binding = 1, rgba8) uniform restrict writeonly image2D dest_image;

void main() {
	ivec2 local = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(local, params.rect_size))) {
		return;
	}
	ivec2 pos = params.rect_position + local;
	ivec2 src = pos * 2;
	ivec2 max_src = params.source_size - 1;
	vec4 color = imageLoad(source_image, min(src, max_src));
	color += imageLoad(source_image, min(src + ivec2(1, 0), max_src));
	color += imageLoad(source_image, min(src + ivec2(0, 1), max_src));
	color += imageLoad(source_image, min(src + ivec2(1, 1), max_src));
	imageStore(dest_image, pos, color * 0.25);
}

#endif // MODE_DOWNSAMPLE
