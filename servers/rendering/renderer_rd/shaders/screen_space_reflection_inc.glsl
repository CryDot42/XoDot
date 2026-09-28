// Helpers shared by the screen space reflection passes and the scene shader that composites them.

// The mip level of the roughness filtered reflection is stored normalized to this range.
#define SSR_MIP_LEVEL_RANGE 14.0

// Roughness is stored in the normal-roughness buffer remapped to [0, 127/255], and mirrored around 0.5
// for dynamic objects (see MODE_RENDER_NORMAL_ROUGHNESS in scene_forward_clustered.glsl).
float ssr_decode_roughness(float p_encoded) {
	float roughness = p_encoded > 0.5 ? 1.0 - p_encoded : p_encoded;
	return roughness * (255.0 / 127.0);
}

vec3 ssr_decode_normal(vec3 p_encoded) {
	return normalize(p_encoded * 2.0 - 1.0);
}

// Reversible tone mapping applied to the traced color, so that filtering it into rougher mips
// is less sensitive to very bright samples. The inverse is applied once the reflection is sampled.
const vec3 SSR_LUMINANCE_WEIGHTS = vec3(0.2126, 0.7152, 0.0722);

vec3 ssr_tonemap(vec3 p_color) {
	return p_color / (1.0 + dot(p_color, SSR_LUMINANCE_WEIGHTS));
}

vec3 ssr_inverse_tonemap(vec3 p_color) {
	return p_color / max(1.0 - dot(p_color, SSR_LUMINANCE_WEIGHTS), 1e-4);
}
