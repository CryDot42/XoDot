#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D source;
layout(r32f, set = 0, binding = 1) uniform restrict writeonly image2D dest;
#ifdef MODE_COPY_SOURCE
// Also writes the source to the first level of the hierarchy, saving a separate copy pass.
layout(r32f, set = 0, binding = 2) uniform restrict writeonly image2D dest_source;
#endif

layout(push_constant, std430) uniform Params {
	ivec2 screen_size;
	ivec2 pad;
}
params;

float fetch(ivec2 pos) {
	float depth = texelFetch(source, pos, 0).x;
#ifdef MODE_COPY_SOURCE
	imageStore(dest_source, pos, vec4(depth, 0.0, 0.0, 0.0));
#endif
	return depth;
}

void main() {
	ivec2 pixel_pos = ivec2(gl_GlobalInvocationID.xy);

	if (any(greaterThanEqual(pixel_pos, params.screen_size))) {
		return;
	}

	ivec2 source_pos = pixel_pos * 2;

	// Keep the closest depth (reverse Z) of the source texels covered by this one.
	float depth = fetch(source_pos);
	depth = max(depth, fetch(source_pos + ivec2(1, 0)));
	depth = max(depth, fetch(source_pos + ivec2(0, 1)));
	depth = max(depth, fetch(source_pos + ivec2(1, 1)));

	// When the source has an odd size, the texels of the last row or column are shared
	// with their neighbors so that the whole source stays covered.
	bvec2 odd = notEqual(textureSize(source, 0) & 1, ivec2(0));

	if (odd.x) {
		depth = max(depth, fetch(source_pos + ivec2(2, 0)));
		depth = max(depth, fetch(source_pos + ivec2(2, 1)));
	}

	if (odd.y) {
		depth = max(depth, fetch(source_pos + ivec2(0, 2)));
		depth = max(depth, fetch(source_pos + ivec2(1, 2)));
	}

	if (all(odd)) {
		depth = max(depth, fetch(source_pos + ivec2(2, 2)));
	}

	imageStore(dest, pixel_pos, vec4(depth, 0.0, 0.0, 0.0));
}
