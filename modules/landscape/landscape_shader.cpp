/**************************************************************************/
/*  landscape_shader.cpp                                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "landscape_shader.h"

String LandscapeShader::get_code() {
	return R"(// Built-in Landscape3D material.
// Uniforms prefixed with "ls_" are driven by the Landscape3D node.

shader_type spatial;
render_mode depth_draw_opaque, cull_back, diffuse_burley, specular_schlick_ggx;

group_uniforms landscape_system;
uniform highp sampler2D ls_heightmap : filter_nearest, repeat_disable;
uniform sampler2D ls_normalmap : filter_linear_mipmap_anisotropic, repeat_disable;
uniform sampler2DArray ls_weightmaps : filter_linear_mipmap, repeat_disable;
uniform sampler2DArray ls_albedo_height : source_color, filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2DArray ls_normal_roughness : filter_linear_mipmap_anisotropic, repeat_enable;

uniform int ls_patch_quads = 32;
uniform int ls_patch_quads_log2 = 5;
uniform int ls_max_level = 0;
uniform int ls_micro_levels = 0;
uniform ivec2 ls_size = ivec2(2, 2);
uniform float ls_spacing = 1.0;
uniform vec3 ls_lod_camera = vec3(0.0);
// x: displacement fade start, y: displacement fade end, z: vertex spacing per meter of distance, w: displacement scale.
uniform vec4 ls_micro_params = vec4(0.0);

uniform int ls_layer_count = 1;
uniform int ls_weightmap_count = 1;
uniform float ls_layer_texture_size = 1024.0;
uniform vec4 ls_layer_color[16]; // rgb: albedo tint, a: metallic.
uniform vec4 ls_layer_uv[16]; // x: uv scale (1 / tile size), y: cos(rotation), z: sin(rotation), w: triplanar.
uniform vec4 ls_layer_material[16]; // x: roughness, y: normal strength, z: height blend, w: displacement (m).
uniform vec4 ls_layer_extra[16]; // x: ao strength, y: triplanar sharpness.

uniform int ls_debug_view = 0;
uniform vec4 ls_brush = vec4(0.0); // xy: center (local), z: radius (0 = hidden), w: falloff.
uniform vec4 ls_brush_color : source_color = vec4(0.25, 0.6, 1.0, 1.0);
group_uniforms;

varying vec3 v_local;
varying flat int v_level;
varying flat vec2 v_patch;

float ls_texel_height(int p_x, int p_z) {
	return texelFetch(ls_heightmap, ivec2(clamp(p_x, 0, ls_size.x - 1), clamp(p_z, 0, ls_size.y - 1)), 0).r;
}

// Height at a grid position expressed in micro units (1 texel = 2^micro_levels units).
float ls_grid_height(ivec2 p_grid) {
	int m = ls_micro_levels;
	int tx = p_grid.x >> m;
	int tz = p_grid.y >> m;
	int mask = (1 << m) - 1;
	int fx = p_grid.x & mask;
	int fz = p_grid.y & mask;
	float h00 = ls_texel_height(tx, tz);
	if (fx == 0 && fz == 0) {
		return h00;
	}
	float inv = 1.0 / float(1 << m);
	float h10 = ls_texel_height(tx + 1, tz);
	float h01 = ls_texel_height(tx, tz + 1);
	float h11 = ls_texel_height(tx + 1, tz + 1);
	float ax = float(fx) * inv;
	float az = float(fz) * inv;
	return mix(mix(h00, h10, ax), mix(h01, h11, ax), az);
}

vec2 ls_heightmap_uv(vec2 p_local_xz) {
	return (p_local_xz / ls_spacing + 0.5) / vec2(ls_size);
}

vec3 ls_decode_normal(vec4 p_color) {
	return normalize(p_color.xyz * 2.0 - 1.0);
}

vec2 ls_layer_coords(int p_layer, vec2 p_pos) {
	vec4 uvp = ls_layer_uv[p_layer];
	vec2 uv = p_pos * uvp.x;
	return vec2(uv.x * uvp.y - uv.y * uvp.z, uv.x * uvp.z + uv.y * uvp.y);
}

float ls_displacement(vec3 p_pos, vec2 p_hm_uv, float p_dist) {
	float total = 0.0;
	for (int w = 0; w < ls_weightmap_count; w++) {
		vec4 weights = textureLod(ls_weightmaps, vec3(p_hm_uv, float(w)), 0.0);
		for (int c = 0; c < 4; c++) {
			int layer = w * 4 + c;
			float weight = weights[c];
			float amplitude = ls_layer_material[layer].w;
			if (layer >= ls_layer_count || weight < 0.004 || amplitude <= 0.0) {
				continue;
			}
			// Prefilter the height texture to roughly match the local vertex density.
			float texel_size = 1.0 / max(ls_layer_uv[layer].x * ls_layer_texture_size, 1e-6);
			float lod = log2(max(ls_micro_params.z * p_dist / texel_size, 1.0));
			float h = textureLod(ls_albedo_height, vec3(ls_layer_coords(layer, p_pos.xz), float(layer)), lod).a;
			total += weight * (h - 0.5) * amplitude;
		}
	}
	return total * ls_micro_params.w;
}

// Local position of a grid vertex. Only depends on the grid position (and the LOD camera),
// which guarantees that vertices shared between patches are bit-exact.
vec3 ls_grid_position(ivec2 p_grid) {
	int scale = 1 << ls_micro_levels;
	ivec2 max_units = (ls_size - ivec2(1)) * scale;
	ivec2 g = clamp(p_grid, ivec2(0), max_units);
	float unit = ls_spacing / float(scale);
	vec3 p = vec3(float(g.x) * unit, ls_grid_height(g), float(g.y) * unit);
	if (ls_micro_params.y > 0.0) {
		float dist = distance(p, ls_lod_camera);
		float fade = 1.0 - smoothstep(ls_micro_params.x, ls_micro_params.y, dist);
		if (fade > 0.0) {
			vec2 hm_uv = ls_heightmap_uv(p.xz);
			vec3 n = ls_decode_normal(textureLod(ls_normalmap, hm_uv, 0.0));
			p += n * (ls_displacement(p, hm_uv, dist) * fade);
		}
	}
	return p;
}

void vertex() {
	int n = ls_patch_quads;
	ivec2 vi = ivec2(int(round(VERTEX.x)), int(round(VERTEX.z)));
	ivec2 origin = ivec2(int(INSTANCE_CUSTOM.x), int(INSTANCE_CUSTOM.y));
	int level = int(INSTANCE_CUSTOM.z);
	int edges = int(INSTANCE_CUSTOM.w);
	int grid_step = 1 << (ls_max_level - level);
	ivec2 g = origin + vi * grid_step;

	// Stitch edges shared with coarser neighbors. Edge vertices either collapse onto the
	// coarse grid, or (for very large LOD differences) are interpolated along the coarse edge.
	ivec2 ga = g;
	ivec2 gb = g;
	float t = 0.0;
	int dx = 0;
	if (vi.x == 0) {
		dx = edges & 15;
	} else if (vi.x == n) {
		dx = (edges >> 4) & 15;
	}
	if (dx > 0) {
		int s = grid_step << dx;
		int c0 = (g.y / s) * s;
		if (c0 != g.y) {
			ga.y = c0;
			if (dx <= ls_patch_quads_log2) {
				gb.y = c0;
			} else {
				gb.y = c0 + s;
				t = float(g.y - c0) / float(s);
			}
		}
	}
	int dz = 0;
	if (vi.y == 0) {
		dz = (edges >> 8) & 15;
	} else if (vi.y == n) {
		dz = (edges >> 12) & 15;
	}
	if (dz > 0) {
		int s = grid_step << dz;
		int c0 = (g.x / s) * s;
		if (c0 != g.x) {
			ga.x = c0;
			if (dz <= ls_patch_quads_log2) {
				gb.x = c0;
			} else {
				gb.x = c0 + s;
				t = float(g.x - c0) / float(s);
			}
		}
	}

	vec3 p = ls_grid_position(ga);
	if (t > 0.0) {
		p = mix(p, ls_grid_position(gb), t);
	}

	VERTEX = p;
	NORMAL = ls_decode_normal(textureLod(ls_normalmap, ls_heightmap_uv(p.xz), 0.0));
	UV = ls_heightmap_uv(p.xz);
	v_local = p;
	v_level = level;
	v_patch = vec2(origin) / float(n << (ls_max_level - level));
}


vec3 ls_unpack_normal(vec4 p_sample, float p_strength) {
	vec2 xy = (p_sample.xy * 2.0 - 1.0) * p_strength;
	return vec3(xy, sqrt(max(1.0 - dot(xy, xy), 0.0)));
}

// Returns albedo+height and the blended normal (in landscape local space, packed in xyz of the second value)
// with roughness/ao in the remaining channels.
void ls_sample_layer(int p_layer, vec3 p_pos, vec3 p_normal, out vec4 r_albedo_height, out vec3 r_normal, out vec2 r_rough_ao) {
	vec4 uvp = ls_layer_uv[p_layer];
	float strength = ls_layer_material[p_layer].y;
	float layer = float(p_layer);
	if (uvp.w > 0.5) {
		// Triplanar projection, used for cliffs.
		vec3 blend = pow(abs(p_normal), vec3(ls_layer_extra[p_layer].y));
		blend /= max(dot(blend, vec3(1.0)), 1e-5);
		vec2 uv_x = p_pos.zy * uvp.x;
		vec2 uv_y = p_pos.xz * uvp.x;
		vec2 uv_z = p_pos.xy * uvp.x;
		vec4 ah_x = texture(ls_albedo_height, vec3(uv_x, layer));
		vec4 ah_y = texture(ls_albedo_height, vec3(uv_y, layer));
		vec4 ah_z = texture(ls_albedo_height, vec3(uv_z, layer));
		vec4 nr_x = texture(ls_normal_roughness, vec3(uv_x, layer));
		vec4 nr_y = texture(ls_normal_roughness, vec3(uv_y, layer));
		vec4 nr_z = texture(ls_normal_roughness, vec3(uv_z, layer));
		r_albedo_height = ah_x * blend.x + ah_y * blend.y + ah_z * blend.z;
		vec4 nr = nr_x * blend.x + nr_y * blend.y + nr_z * blend.z;
		r_rough_ao = nr.ba;
		// Whiteout blend of the three tangent space normals (swizzled per axis).
		vec3 tn_x = ls_unpack_normal(nr_x, strength);
		vec3 tn_y = ls_unpack_normal(nr_y, strength);
		vec3 tn_z = ls_unpack_normal(nr_z, strength);
		vec3 axis_sign = sign(p_normal);
		tn_x = vec3(tn_x.xy + p_normal.zy, abs(tn_x.z) * abs(p_normal.x));
		tn_y = vec3(tn_y.xy + p_normal.xz, abs(tn_y.z) * abs(p_normal.y));
		tn_z = vec3(tn_z.xy + p_normal.xy, abs(tn_z.z) * abs(p_normal.z));
		tn_x.z *= axis_sign.x;
		tn_y.z *= axis_sign.y;
		tn_z.z *= axis_sign.z;
		r_normal = normalize(tn_x.zyx * blend.x + tn_y.xzy * blend.y + tn_z.xyz * blend.z);
	} else {
		vec2 uv = ls_layer_coords(p_layer, p_pos.xz);
		r_albedo_height = texture(ls_albedo_height, vec3(uv, layer));
		vec4 nr = texture(ls_normal_roughness, vec3(uv, layer));
		r_rough_ao = nr.ba;
		vec3 tn = ls_unpack_normal(nr, strength);
		// U follows the rotated X axis, the green channel (OpenGL convention) points towards -V.
		vec3 tangent = vec3(uvp.y, 0.0, -uvp.z);
		vec3 binormal = -vec3(uvp.z, 0.0, uvp.y);
		tangent = normalize(tangent - p_normal * dot(p_normal, tangent));
		binormal = normalize(binormal - p_normal * dot(p_normal, binormal));
		r_normal = normalize(tangent * tn.x + binormal * tn.y + p_normal * tn.z);
	}
}

vec3 ls_debug_color(int p_index) {
	float h = fract(float(p_index) * 0.61803398875);
	vec3 c = clamp(abs(fract(h + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
	return mix(vec3(1.0), c, 0.75);
}

void fragment() {
	vec3 p = v_local;
	vec2 hm_uv = ls_heightmap_uv(p.xz);
	vec3 terrain_normal = ls_decode_normal(texture(ls_normalmap, hm_uv));

	// Select the four most important layers of this pixel.
	int ids[4] = int[](0, 0, 0, 0);
	float weights[4] = float[](0.0, 0.0, 0.0, 0.0);
	float total = 0.0;
	for (int w = 0; w < ls_weightmap_count; w++) {
		vec4 wm = texture(ls_weightmaps, vec3(hm_uv, float(w)));
		for (int c = 0; c < 4; c++) {
			int layer = w * 4 + c;
			float weight = wm[c];
			if (layer >= ls_layer_count || weight <= 0.002) {
				continue;
			}
			total += weight;
			if (weight > weights[3]) {
				int pos = 3;
				while (pos > 0 && weight > weights[pos - 1]) {
					weights[pos] = weights[pos - 1];
					ids[pos] = ids[pos - 1];
					pos--;
				}
				weights[pos] = weight;
				ids[pos] = layer;
			}
		}
	}
	if (total <= 0.002) {
		weights[0] = 1.0;
		ids[0] = 0;
	}

	vec3 albedo = vec3(0.0);
	vec3 normal = vec3(0.0);
	float roughness = 0.0;
	float ao = 0.0;
	float metallic = 0.0;
	float weight_sum = 0.0;
	for (int i = 0; i < 4; i++) {
		if (weights[i] <= 0.0) {
			continue;
		}
		int layer = ids[i];
		vec4 albedo_height;
		vec3 layer_normal;
		vec2 rough_ao;
		ls_sample_layer(layer, p, terrain_normal, albedo_height, layer_normal, rough_ao);
		// UE-style height blend: lerp(-1, 1, weight) + height, faded in by the layer's height blend amount.
		float height_weight = clamp(weights[i] * 2.0 - 1.0 + albedo_height.a, 0.0001, 1.0);
		float blend = mix(weights[i], height_weight, ls_layer_material[layer].z);
		albedo += albedo_height.rgb * ls_layer_color[layer].rgb * blend;
		normal += layer_normal * blend;
		roughness += rough_ao.x * ls_layer_material[layer].x * blend;
		ao += mix(1.0, rough_ao.y, ls_layer_extra[layer].x) * blend;
		metallic += ls_layer_color[layer].a * blend;
		weight_sum += blend;
	}
	weight_sum = max(weight_sum, 1e-5);
	albedo /= weight_sum;
	roughness /= weight_sum;
	ao /= weight_sum;
	metallic /= weight_sum;
	normal = normalize(normal);

	if (ls_debug_view == 1) {
		albedo = ls_debug_color(v_level);
	} else if (ls_debug_view == 2) {
		albedo = ls_debug_color(int(v_patch.x) * 7919 + int(v_patch.y) * 104729 + v_level * 31);
	} else if (ls_debug_view == 3) {
		albedo = terrain_normal * 0.5 + 0.5;
		normal = terrain_normal;
	} else if (ls_debug_view == 4) {
		albedo = ls_debug_color(ids[0]) * (0.5 + 0.5 * weights[0]);
	}

	// Editor brush preview.
	if (ls_brush.z > 0.0) {
		float d = distance(p.xz, ls_brush.xy);
		float radius = ls_brush.z;
		float inner = radius * (1.0 - ls_brush.w);
		float aa = max(fwidth(d), 1e-4) * 1.5;
		float outer_ring = 1.0 - smoothstep(0.0, aa, abs(d - radius));
		float inner_ring = (ls_brush.w > 0.001) ? (1.0 - smoothstep(0.0, aa, abs(d - inner))) * 0.6 : 0.0;
		float falloff = d < inner ? 1.0 : clamp(1.0 - (d - inner) / max(radius - inner, 1e-4), 0.0, 1.0);
		float fill = d < radius ? falloff * 0.18 : 0.0;
		float mask = max(max(outer_ring, inner_ring), fill);
		albedo = mix(albedo, ls_brush_color.rgb, mask);
		EMISSION = ls_brush_color.rgb * max(outer_ring, inner_ring) * 0.5;
	}

	ALBEDO = albedo;
	ROUGHNESS = clamp(roughness, 0.0, 1.0);
	METALLIC = clamp(metallic, 0.0, 1.0);
	AO = ao;
	AO_LIGHT_AFFECT = 0.0;
	NORMAL = normalize(mat3(VIEW_MATRIX) * (MODEL_NORMAL_MATRIX * normal));
}
)";
}
