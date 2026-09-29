#define LIGHT_BAKE_DISABLED 0
#define LIGHT_BAKE_STATIC 1
#define LIGHT_BAKE_DYNAMIC 2

struct LightData { //this structure needs to be as packed as possible
	vec3 position;
	float inv_radius;

	vec3 direction;
	float size;

	vec3 color;
	float attenuation;

	mediump vec3 area_width;
	float cone_attenuation; // area lights: 1 / (range + diagonal/2)

	mediump vec3 area_height;
	float cone_angle; // area lights: max mipmaps

	float specular_amount;
	float shadow_opacity;
	float pad[2];

	vec4 atlas_rect; // rect in the shadow atlas
	mat4 shadow_matrix;
	float shadow_bias;
	float shadow_normal_bias;
	float transmittance_bias;
	float soft_shadow_size; // for spot, it's the size in uv coordinates of the light, for omni it's the span angle
	float soft_shadow_scale; // scales the shadow kernel for blurrier shadows
	uint mask;
	float volumetric_fog_energy;
	uint bake_mode;
	vec4 projector_rect; //projector rect in srgb decal atlas
};

#define REFLECTION_AMBIENT_DISABLED 0
#define REFLECTION_AMBIENT_ENVIRONMENT 1
#define REFLECTION_AMBIENT_COLOR 2

struct ReflectionData {
	vec3 box_extents;
	float index;
	vec3 box_offset;
	uint mask;
	vec3 ambient; // ambient color
	float intensity;
	float blend_distance;
	bool exterior;
	bool box_project;
	uint ambient_mode;
	float exposure_normalization;
	float pad0;
	float pad1;
	float pad2;
	//0-8 is intensity,8-9 is ambient, mode
	mat4 local_matrix; // up to here for spot and omni, rest is for directional
	// notes: for ambientblend, use distance to edge to blend between already existing global environment
};

// Cascades below this index are rendered into `directional_shadow_atlas`, the others into `directional_shadow_atlas_cached`.
#define DIRECTIONAL_LIGHT_DYNAMIC_CASCADES 4u

struct DirectionalLightData {
	vec3 direction;
	float energy; // needs to be highp to avoid NaNs being created with high energy values (i.e. when using physical light units and over-exposing the image)
	vec3 color;
	float size;
	float specular;
	uint mask;
	float softshadow_angle;
	float soft_shadow_scale;
	bool blend_splits;
	float shadow_opacity;
	float fade_from;
	float fade_to;
	uint sscs_index;
	uint shadow_cascade_count;
	uint bake_mode;
	float volumetric_fog_energy;
	// The values of each cascade are packed 4 per vec4, use DIRECTIONAL_LIGHT_CASCADE_VALUE() to get one.
	vec4 shadow_bias[2];
	vec4 shadow_normal_bias[2];
	vec4 shadow_transmittance_bias[2];
	vec4 shadow_z_range[2];
	vec4 shadow_range_begin[2];
	vec4 shadow_split_offsets[2]; // Distance from the camera to the end of each cascade, 0 for the cascades that are not used.
	mat4 shadow_matrix[8];
	vec4 uv_scale[4]; // Packed 2 per vec4, use DIRECTIONAL_LIGHT_CASCADE_UV_SCALE().
};

#define DIRECTIONAL_LIGHT_CASCADE_VALUE(m_array, m_cascade) m_array[(m_cascade) >> 2u][(m_cascade) & 3u]
#define DIRECTIONAL_LIGHT_CASCADE_UV_SCALE(m_array, m_cascade) (((m_cascade) & 1u) == 0u ? m_array[(m_cascade) >> 1u].xy : m_array[(m_cascade) >> 1u].zw)

// Index of the cascade that covers a given view depth: the first one that ends beyond it, or the last one used if there is none.
uint directional_light_cascade_from_depth(vec4 p_split_offsets0, vec4 p_split_offsets1, uint p_cascade_count, float p_depth) {
	uvec4 passed0 = uvec4(greaterThanEqual(vec4(p_depth), p_split_offsets0)) & uvec4(greaterThan(p_split_offsets0, vec4(0.0)));
	uvec4 passed1 = uvec4(greaterThanEqual(vec4(p_depth), p_split_offsets1)) & uvec4(greaterThan(p_split_offsets1, vec4(0.0)));
	uint passed = passed0.x + passed0.y + passed0.z + passed0.w + passed1.x + passed1.y + passed1.z + passed1.w;
	return min(passed, max(p_cascade_count, 1u) - 1u);
}

// Colors used to visualize the cascades.
vec3 directional_light_cascade_tint(uint p_cascade) {
	const vec3 tints[8] = vec3[](
			vec3(1.0, 0.0, 0.0),
			vec3(0.0, 1.0, 0.0),
			vec3(0.0, 0.0, 1.0),
			vec3(1.0, 1.0, 0.0),
			vec3(1.0, 0.0, 1.0),
			vec3(0.0, 1.0, 1.0),
			vec3(1.0, 0.5, 0.0),
			vec3(0.5, 0.5, 0.5));
	return tints[p_cascade & 7u];
}
