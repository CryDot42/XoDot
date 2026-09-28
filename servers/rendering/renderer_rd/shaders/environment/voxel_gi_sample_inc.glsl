// Samples a VoxelGI volume for a cone traveling along p_dir (in cell space). p_plane_distance is how far the sample is
// from the plane of the surface the cone starts on, in cells.
// Expects voxel_gi_instances, voxel_gi_textures, voxel_gi_aniso_textures and linear_sampler_with_mipmaps to be declared.

vec4 voxel_gi_sample(uint p_index, vec3 p_uvw, float p_lod, vec3 p_dir, float p_plane_distance) {
	vec3 aniso = voxel_gi_instances.data[p_index].aniso;
	// Anisotropic voxels assume rays enter them from the outside. Once the filter footprint reaches behind the surface
	// the cone starts on, they would show what is behind it (the other side of a wall), so use isotropic data there.
	// In practice this keeps wide diffuse cones isotropic and uses anisotropy for narrow ones (screen probes, glossy reflections).
	float aniso_amount = aniso.x * smoothstep(1.0, 2.0, p_plane_distance * exp2(-p_lod));
	if (aniso_amount == 0.0 || p_lod <= 0.0) {
		return textureLod(sampler3D(voxel_gi_textures[p_index], linear_sampler_with_mipmaps), p_uvw, p_lod);
	}

	// From mipmap 1 on, every direction (+X, -X, +Y, -Y, +Z, -Z) is stored side by side along one axis.
	// Blend the 3 that face the direction of travel.
	float aniso_lod = max(0.0, p_lod - 1.0);
	int axis = int(aniso.y);
	// Keep trilinear filtering from reading into the neighboring direction.
	float slab_texels = max(1.0, aniso.z * exp2(-floor(aniso_lod) - 1.0));
	float margin = 0.5 / slab_texels;
	float coord = clamp(p_uvw[axis], margin, 1.0 - margin);

	vec3 weights = p_dir * p_dir;
	vec4 result = vec4(0.0);
	for (int i = 0; i < 3; i++) {
		if (weights[i] > 0.0) {
			float slot = float(i * 2 + (p_dir[i] >= 0.0 ? 0 : 1));
			vec3 uvw = p_uvw;
			uvw[axis] = (slot + coord) / 6.0;
			result += textureLod(sampler3D(voxel_gi_aniso_textures[p_index], linear_sampler_with_mipmaps), uvw, aniso_lod) * weights[i];
		}
	}

	if (p_lod < 1.0) {
		vec4 base = textureLod(sampler3D(voxel_gi_textures[p_index], linear_sampler_with_mipmaps), p_uvw, 0.0);
		result = mix(base, result, p_lod);
	}

	if (aniso_amount < 1.0) {
		vec4 isotropic = textureLod(sampler3D(voxel_gi_textures[p_index], linear_sampler_with_mipmaps), p_uvw, p_lod);
		result = mix(isotropic, result, aniso_amount);
	}
	return result;
}
