#[compute]

#version 450

#VERSION_DEFINES

// Downsamples the depth buffer for HZB (Hierarchical Z-Buffer) occlusion culling.
// Every texel of the destination buffer stores the farthest depth found in the area it covers,
// so that the result stays conservative (an object is only considered occluded if every depth sample
// covering it is in front of the object). The result is read back and reprojected on the CPU.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#ifdef MODE_MSAA
layout(set = 0, binding = 0) uniform sampler2DMS source_depth;
#else
layout(set = 0, binding = 0) uniform sampler2D source_depth;
#endif

layout(set = 1, binding = 0, std430) restrict writeonly buffer DestDepth {
	float data[];
}
dest_depth;

layout(push_constant, std430) uniform Params {
	ivec2 source_size;
	ivec2 dest_size;
	uint dest_offset;
	int sample_count;
	uint pad[2];
}
params;

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.dest_size))) {
		return;
	}

	// Source area covered by this texel, rounded outwards so that no source pixel is skipped.
	ivec2 from = (pos * params.source_size) / params.dest_size;
	ivec2 to = min(((pos + 1) * params.source_size + params.dest_size - 1) / params.dest_size, params.source_size);

	// Reversed Z: 1.0 is the near plane and 0.0 the far plane, so the farthest depth is the smallest value.
	float farthest = 1.0;

	for (int y = from.y; y < to.y; y++) {
		for (int x = from.x; x < to.x; x++) {
#ifdef MODE_MSAA
			for (int i = 0; i < params.sample_count; i++) {
				farthest = min(farthest, texelFetch(source_depth, ivec2(x, y), i).r);
			}
#else
			farthest = min(farthest, texelFetch(source_depth, ivec2(x, y), 0).r);
#endif
		}
	}

	dest_depth.data[params.dest_offset + uint(pos.y * params.dest_size.x + pos.x)] = farthest;
}
