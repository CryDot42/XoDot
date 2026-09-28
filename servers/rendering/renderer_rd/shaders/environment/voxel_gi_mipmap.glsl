#[compute]

#version 450

#VERSION_DEFINES

// Region operations on the VoxelGI 3D texture, used to update only the parts touched by dynamic objects.

layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

layout(push_constant, std430) uniform Params {
	ivec3 offset; // First texel of the region, in destination texels.
	float propagation;
	ivec3 size; // Region size, in destination texels.
	int slab_axis; // Anisotropic mipmaps store their 6 directions side by side along this axis.
	ivec3 src_size; // Size of the source mipmap (a single direction for anisotropic mipmaps).
	int dst_slab_size;
	int src_slab_size;
	uint pad0;
	uint pad1;
	uint pad2;
}
params;

layout(rgba8, set = 0, binding = 0) uniform restrict writeonly image3D dst_tex;

#if defined(MODE_DOWNSAMPLE) || defined(MODE_ANISO_FIRST) || defined(MODE_ANISO)
layout(rgba8, set = 0, binding = 1) uniform restrict readonly image3D src_tex;
#endif

#ifdef MODE_ANISO_FIRST
layout(r8ui, set = 0, binding = 2) uniform restrict readonly uimage3D src_normal_mask;
#endif

#if defined(MODE_ANISO_FIRST) || defined(MODE_ANISO)
// What a ray traveling along the direction in p_slot (+X, -X, +Y, -Y, +Z, -Z) sees of a source voxel.
vec4 load_source(ivec3 p_pos, int p_slot) {
#ifdef MODE_ANISO_FIRST
	vec4 src = imageLoad(src_tex, p_pos);
	// A ray traveling the same way the surface faces only sees its back, which is not lit from this side.
	// It still occludes. Surfaces seen at grazing angles keep their light (their radiance does not depend on the angle).
	uint mask = imageLoad(src_normal_mask, p_pos).r;
	if ((mask & (1u << uint(p_slot))) != 0u) {
		src.rgb = vec3(0.0);
	}
	return src;
#else
	p_pos[params.slab_axis] += p_slot * params.src_slab_size;
	return imageLoad(src_tex, p_pos);
#endif
}
#endif

void main() {
	ivec3 local_pos = ivec3(gl_GlobalInvocationID);
	if (any(greaterThanEqual(local_pos, params.size))) {
		return;
	}
	ivec3 pos = params.offset + local_pos;

#ifdef MODE_CLEAR
	imageStore(dst_tex, pos, vec4(0.0));
#endif

#ifdef MODE_DOWNSAMPLE
	// Same reduction the octree uses for its upper levels (see MODE_UPDATE_MIPMAPS in voxel_gi.glsl),
	// so static voxels downsample to exactly what the static mipmaps contain.
	vec3 light = vec3(0.0);
	float alpha = 0.0;
	float count = 0.0;
	for (int i = 0; i < 8; i++) {
		ivec3 src_pos = pos * 2 + ivec3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
		if (any(greaterThanEqual(src_pos, params.src_size))) {
			continue;
		}
		vec4 src = imageLoad(src_tex, src_pos);
		if (src.a > 0.0) {
			light += src.rgb;
			count += 1.0;
		}
		alpha += src.a;
	}

	vec3 result = count > 0.0 ? light / mix(8.0, count, params.propagation) : vec3(0.0);
	imageStore(dst_tex, pos, vec4(result, alpha / 8.0));
#endif

#if defined(MODE_ANISO_FIRST) || defined(MODE_ANISO)
	for (int slot = 0; slot < 6; slot++) {
		int axis = slot >> 1;
		bool positive = (slot & 1) == 0;
		int u_axis = (axis + 1) % 3;
		int v_axis = (axis + 2) % 3;

		vec3 light = vec3(0.0);
		float alpha = 0.0;
		float count = 0.0;

		// The 4 columns of children along the direction are composited front to back, so near voxels hide
		// what is behind them instead of being averaged with it.
		for (int column = 0; column < 4; column++) {
			vec4 accum = vec4(0.0);
			for (int i = 0; i < 2; i++) {
				ivec3 src_pos = pos * 2;
				src_pos[axis] += positive ? i : 1 - i;
				src_pos[u_axis] += column & 1;
				src_pos[v_axis] += column >> 1;
				if (any(greaterThanEqual(src_pos, params.src_size))) {
					continue;
				}
				vec4 src = load_source(src_pos, slot);
				accum.rgb += (1.0 - accum.a) * src.rgb;
				accum.a += (1.0 - accum.a) * src.a;
			}
			if (accum.a > 0.0) {
				light += accum.rgb;
				count += 1.0;
			}
			alpha += accum.a;
		}

		vec3 result = count > 0.0 ? light / mix(4.0, count, params.propagation) : vec3(0.0);
		ivec3 dst_pos = pos;
		dst_pos[params.slab_axis] += slot * params.dst_slab_size;
		imageStore(dst_tex, dst_pos, vec4(result, alpha / 4.0));
	}
#endif
}
