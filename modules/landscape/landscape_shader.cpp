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

String LandscapeShader::get_code(bool p_holes) {
	String code = R"(// Built-in Landscape3D material.
// Uniforms prefixed with "ls_" are driven by the Landscape3D node.

shader_type spatial;
render_mode depth_draw_opaque, cull_back, diffuse_burley, specular_schlick_ggx;

group_uniforms landscape_system;
// Streamed terrain data: pages of PAGE_SIZE + 1 texels (see LandscapeGPU) addressed by the page table.
uniform highp sampler2D ls_page_table : filter_nearest, repeat_disable;
uniform highp sampler2DArray ls_page_heights : filter_nearest, repeat_disable;
uniform sampler2DArray ls_page_normals : filter_linear, repeat_disable;
uniform bool ls_normal_rg = false; // Normal X/Z in RG (true) or in RA (false).
uniform sampler2DArray ls_page_weights_0 : filter_linear, repeat_disable;
uniform sampler2DArray ls_page_weights_1 : filter_linear, repeat_disable;
uniform sampler2DArray ls_page_weights_2 : filter_linear, repeat_disable;
uniform sampler2DArray ls_page_weights_3 : filter_linear, repeat_disable;
uniform sampler2DArray ls_page_holes : filter_linear, repeat_disable;
uniform bool ls_holes_enabled = false;
uniform int ls_page_rows[16]; // First page table row of each mip level.
uniform int ls_page_cols[16]; // Tile count of each mip level.
uniform int ls_page_row_count[16];
uniform int ls_root_mip = 0;
uniform float ls_texture_lod_bias = 0.5;
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

const int LS_PAGE_SHIFT = 7;
const int LS_PAGE_SIZE = 128;
const float LS_PAGE_TEXELS = 129.0;
const int LS_SLOT_BITS = 12;

vec3 ls_decode_normal(vec4 p_color) {
	// The normal map stores X and Z, Y is always positive on a height field.
	vec2 xz = (ls_normal_rg ? p_color.rg : p_color.ra) * 2.0 - 1.0;
	return normalize(vec3(xz.x, sqrt(max(1.0 - dot(xz, xz), 0.0)), xz.y));
}

int ls_page_entry(int p_mip, ivec2 p_tile) {
	if (p_tile.x < 0 || p_tile.y < 0 || p_tile.x >= ls_page_cols[p_mip] || p_tile.y >= ls_page_row_count[p_mip]) {
		return -1;
	}
	return int(texelFetch(ls_page_table, ivec2(p_tile.x, ls_page_rows[p_mip] + p_tile.y), 0).r);
}

ivec2 ls_mip_last(int p_mip) {
	return (ls_size + ivec2((1 << p_mip) - 2)) >> ivec2(p_mip);
}

// Slot of the resident page of mip p_mip holding the texel p_texel (in texels of that mip), or -1.
// Texels on the first row/column of a tile are also stored in the border of the previous tile.
int ls_find_page(int p_mip, ivec2 p_texel, bool p_border_x, bool p_border_z, out ivec2 r_local) {
	ivec2 tile = p_texel >> ivec2(LS_PAGE_SHIFT);
	r_local = p_texel - (tile << ivec2(LS_PAGE_SHIFT));
	int entry = ls_page_entry(p_mip, tile);
	if (entry >= 0 && (entry >> LS_SLOT_BITS) == p_mip) {
		return entry & ((1 << LS_SLOT_BITS) - 1);
	}
	bool bx = p_border_x && r_local.x == 0 && tile.x > 0;
	bool bz = p_border_z && r_local.y == 0 && tile.y > 0;
	if (bx) {
		entry = ls_page_entry(p_mip, tile - ivec2(1, 0));
		if (entry >= 0 && (entry >> LS_SLOT_BITS) == p_mip) {
			r_local.x = LS_PAGE_SIZE;
			return entry & ((1 << LS_SLOT_BITS) - 1);
		}
	}
	if (bz) {
		entry = ls_page_entry(p_mip, tile - ivec2(0, 1));
		if (entry >= 0 && (entry >> LS_SLOT_BITS) == p_mip) {
			r_local.y = LS_PAGE_SIZE;
			return entry & ((1 << LS_SLOT_BITS) - 1);
		}
	}
	if (bx && bz) {
		entry = ls_page_entry(p_mip, tile - ivec2(1, 1));
		if (entry >= 0 && (entry >> LS_SLOT_BITS) == p_mip) {
			r_local = ivec2(LS_PAGE_SIZE);
			return entry & ((1 << LS_SLOT_BITS) - 1);
		}
	}
	return -1;
}

// Exact height (and normal) of the terrain at an integer texel (mip 0 texels, may be past the edge),
// read from the finest resident mip level >= p_mip on which the texel is a vertex. Every mip level
// stores exact heights, so vertices shared by patches of different levels always match.
float ls_texel_height(ivec2 p_texel, int p_mip, out vec3 r_normal) {
	r_normal = vec3(0.0, 1.0, 0.0);
	for (int m = p_mip; m <= ls_root_mip; m++) {
		if (((p_texel.x | p_texel.y) & ((1 << m) - 1)) != 0) {
			continue; // Not a vertex of this mip level.
		}
		ivec2 local;
		int slot = ls_find_page(m, min(p_texel >> ivec2(m), ls_mip_last(m)), true, true, local);
		if (slot >= 0) {
			r_normal = ls_decode_normal(texelFetch(ls_page_normals, ivec3(local, slot), 0));
			return texelFetch(ls_page_heights, ivec3(local, slot), 0).r;
		}
	}
	return 0.0;
}

// Height at a grid position expressed in micro units (1 texel = 2^micro_levels units).
float ls_grid_height(ivec2 p_grid, int p_mip, out vec3 r_normal) {
	int m = ls_micro_levels;
	ivec2 t = p_grid >> ivec2(m);
	ivec2 f = p_grid & ivec2((1 << m) - 1);
	if (f.x == 0 && f.y == 0) {
		return ls_texel_height(t, p_mip, r_normal);
	}
	// Below the heightmap resolution: bilinear interpolation of the mip 0 page.
	ivec2 local;
	int slot = ls_find_page(0, min(t, ls_size - 1), f.x == 0, f.y == 0, local);
	if (slot < 0) {
		return ls_texel_height(t, 0, r_normal);
	}
	vec2 a = vec2(f) / float(1 << m);
	ivec2 l1 = min(local + 1, ivec2(LS_PAGE_SIZE));
	float h00 = texelFetch(ls_page_heights, ivec3(local, slot), 0).r;
	float h10 = texelFetch(ls_page_heights, ivec3(l1.x, local.y, slot), 0).r;
	float h01 = texelFetch(ls_page_heights, ivec3(local.x, l1.y, slot), 0).r;
	float h11 = texelFetch(ls_page_heights, ivec3(l1, slot), 0).r;
	r_normal = ls_decode_normal(textureLod(ls_page_normals, vec3((vec2(local) + a + 0.5) / LS_PAGE_TEXELS, float(slot)), 0.0));
	return mix(mix(h00, h10, a.x), mix(h01, h11, a.x), a.y);
}

// Sampling coordinates in the mip 0 page holding a position (mip 0 texels), if resident.
bool ls_mip0_page(vec2 p_t0, out vec3 r_uv) {
	vec2 t = clamp(p_t0, vec2(0.0), vec2(ls_size - 1));
	ivec2 ti = ivec2(floor(t));
	vec2 f = t - vec2(ti);
	ivec2 local;
	int slot = ls_find_page(0, ti, f.x == 0.0, f.y == 0.0, local);
	if (slot < 0) {
		return false;
	}
	r_uv = vec3((vec2(local) + f + 0.5) / LS_PAGE_TEXELS, float(slot));
	return true;
}

// Sampling coordinates in the finest resident page of mip p_mip or coarser (virtual texturing).
vec3 ls_page_uv(vec2 p_t0, int p_mip, out int r_mip) {
	vec2 t0 = clamp(p_t0, vec2(0.0), vec2(ls_size - 1));
	int m = clamp(p_mip, 0, ls_root_mip);
	int entry = ls_page_entry(m, ivec2(floor(t0 / float(1 << m))) >> ivec2(LS_PAGE_SHIFT));
	if (entry < 0) {
		r_mip = -1;
		return vec3(0.0);
	}
	r_mip = entry >> LS_SLOT_BITS;
	vec2 tm = t0 / float(1 << r_mip);
	ivec2 tile = ivec2(floor(tm)) >> ivec2(LS_PAGE_SHIFT);
	vec2 local = tm - vec2(tile << ivec2(LS_PAGE_SHIFT));
	return vec3((local + 0.5) / LS_PAGE_TEXELS, float(entry & ((1 << LS_SLOT_BITS) - 1)));
}

// Weights of the layers [4 * index, 4 * index + 3]. The index is uniform in all callers.
vec4 ls_weights(int p_index, vec3 p_uv) {
	if (p_index == 0) {
		return textureLod(ls_page_weights_0, p_uv, 0.0);
	} else if (p_index == 1) {
		return textureLod(ls_page_weights_1, p_uv, 0.0);
	} else if (p_index == 2) {
		return textureLod(ls_page_weights_2, p_uv, 0.0);
	}
	return textureLod(ls_page_weights_3, p_uv, 0.0);
}

vec2 ls_layer_coords(int p_layer, vec2 p_pos) {
	vec4 uvp = ls_layer_uv[p_layer];
	vec2 uv = p_pos * uvp.x;
	return vec2(uv.x * uvp.y - uv.y * uvp.z, uv.x * uvp.z + uv.y * uvp.y);
}

float ls_displacement(vec3 p_pos, vec3 p_uv, float p_dist) {
	float total = 0.0;
	for (int w = 0; w < ls_weightmap_count; w++) {
		vec4 weights = ls_weights(w, p_uv);
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
vec3 ls_grid_position(ivec2 p_grid, int p_mip, out vec3 r_normal) {
	int scale = 1 << ls_micro_levels;
	ivec2 max_units = (ls_size - ivec2(1)) * scale;
	ivec2 g = clamp(p_grid, ivec2(0), max_units);
	float unit = ls_spacing / float(scale);
	vec3 p = vec3(float(g.x) * unit, ls_grid_height(max(p_grid, ivec2(0)), p_mip, r_normal), float(g.y) * unit);
	if (ls_micro_params.y > 0.0) {
		float dist = distance(p, ls_lod_camera);
		float fade = 1.0 - smoothstep(ls_micro_params.x, ls_micro_params.y, dist);
		vec3 uv;
		if (fade > 0.0 && ls_mip0_page(p.xz / ls_spacing, uv)) {
			vec3 n = ls_decode_normal(textureLod(ls_page_normals, uv, 0.0));
			p += n * (ls_displacement(p, uv, dist) * fade);
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
	// Mip level of the heights of this patch.
	int mip = max(ls_max_level - ls_micro_levels - level, 0);

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

	vec3 vertex_normal;
	vec3 p = ls_grid_position(ga, mip, vertex_normal);
	if (t > 0.0) {
		vec3 normal_b;
		p = mix(p, ls_grid_position(gb, mip, normal_b), t);
		vertex_normal = normalize(mix(vertex_normal, normal_b, t));
	}

	VERTEX = p;
	NORMAL = vertex_normal;
	UV = p.xz / (ls_spacing * vec2(max(ls_size - 1, ivec2(1))));
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
	// Virtual texturing: pick the page mip level matching the screen footprint of a texel.
	vec2 t0 = p.xz / ls_spacing;
	float footprint = max(length(dFdx(t0)), length(dFdy(t0)));
	int page_mip;
	vec3 page_uv = ls_page_uv(t0, int(floor(log2(max(footprint, 1e-6)) + ls_texture_lod_bias)), page_mip);
	bool has_page = page_mip >= 0;
#ifdef LS_HOLES
	if (has_page && ls_holes_enabled && textureLod(ls_page_holes, page_uv, 0.0).r > 0.5) {
		discard; // Visibility tool (holes).
	}
#endif
	vec3 terrain_normal = has_page ? ls_decode_normal(textureLod(ls_page_normals, page_uv, 0.0)) : vec3(0.0, 1.0, 0.0);

	// Select the four most important layers of this pixel.
	int ids[4] = int[](0, 0, 0, 0);
	float weights[4] = float[](0.0, 0.0, 0.0, 0.0);
	float total = 0.0;
	for (int w = 0; w < (has_page ? ls_weightmap_count : 0); w++) {
		vec4 wm = ls_weights(w, page_uv);
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
	} else if (ls_debug_view == 5) {
		// Streaming: mip level of the resident page used for shading.
		albedo = page_mip < 0 ? vec3(1.0, 0.0, 1.0) : ls_debug_color(page_mip + 3);
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
	if (p_holes) {
		// Only landscapes with holes pay for the discard (it disables early depth testing).
		code = code.replace("shader_type spatial;", "shader_type spatial;\n#define LS_HOLES");
	}
	return code;
}
