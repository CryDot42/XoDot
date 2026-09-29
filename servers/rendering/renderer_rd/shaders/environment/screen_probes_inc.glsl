// Shared by the screen probe trace pass and the GI pass that integrates the probes.

// Probes store irradiance as L1 spherical harmonics, one texture layer per color channel (red, green, blue), with
// coefficients in (L0, L1 -1, L1 0, L1 1) order. The last layer holds the L0 coefficient of the visibility (the alpha
// of the rays) followed by the normal the hemisphere of rays was traced around. The spatial filter averages it like
// the other layers, so it stays consistent with the filtered color.
#define SCREEN_PROBE_SH_LAYERS 4

// L0 coefficient of a hemisphere of ones (2 * PI * 0.282095): the visibility of a probe whose rays are all opaque.
#define SCREEN_PROBE_SH_HEMISPHERE_L0 1.772454

uint screen_probe_hash(uvec3 v) {
	// PCG-style 3D hash.
	v = v * 1664525u + 1013904223u;
	v.x += v.y * v.z;
	v.y += v.z * v.x;
	v.z += v.x * v.y;
	v ^= v >> 16u;
	v.x += v.y * v.z;
	v.y += v.z * v.x;
	v.z += v.x * v.y;
	return v.x ^ v.y ^ v.z;
}

// Pixel a probe is placed on this frame. Probes move around their tile every frame, so the temporal
// accumulation sees every surface in the tile over time.
ivec2 screen_probe_get_pixel(ivec2 p_probe, uint p_spacing, uint p_frame, ivec2 p_screen_size) {
	uint h = screen_probe_hash(uvec3(uvec2(p_probe), p_frame));
	ivec2 offset = ivec2(h % p_spacing, (h / p_spacing) % p_spacing);
	return min(p_probe * int(p_spacing) + offset, p_screen_size - 1);
}

vec4 screen_probe_sh_basis(vec3 p_dir) {
	return vec4(0.282095, 0.488603 * p_dir.y, 0.488603 * p_dir.z, 0.488603 * p_dir.x);
}

// Irradiance divided by PI (cosine-weighted average radiance) for the given normal, which is what the
// scene shader expects in the ambient buffer.
float screen_probe_sh_irradiance(vec4 p_sh, vec3 p_normal) {
	// Convolution with the clamped cosine lobe: A0 = PI, A1 = 2 * PI / 3. Dividing by PI leaves 1 and 2 / 3.
	vec4 basis = screen_probe_sh_basis(p_normal);
	return max(0.0, p_sh.x * basis.x + (2.0 / 3.0) * dot(p_sh.yzw, basis.yzw));
}
