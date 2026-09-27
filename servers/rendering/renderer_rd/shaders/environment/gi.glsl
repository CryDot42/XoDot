#[compute]

#version 450

#VERSION_DEFINES

#ifdef SAMPLE_VOXEL_GI_NEAREST
#extension GL_EXT_samplerless_texture_functions : enable
#endif

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#define M_PI 3.141592

/* Specialization Constants (Toggles) */

layout(constant_id = 0) const bool sc_half_res = false;
layout(constant_id = 1) const bool sc_use_full_projection_matrix = false;
layout(constant_id = 2) const bool sc_use_vrs = false;

#define SDFGI_MAX_CASCADES 8

//set 0 for SDFGI and render buffers

layout(set = 0, binding = 1) uniform texture3D sdf_cascades[SDFGI_MAX_CASCADES];
layout(set = 0, binding = 2) uniform texture3D light_cascades[SDFGI_MAX_CASCADES];
layout(set = 0, binding = 3) uniform texture3D aniso0_cascades[SDFGI_MAX_CASCADES];
layout(set = 0, binding = 4) uniform texture3D aniso1_cascades[SDFGI_MAX_CASCADES];
layout(set = 0, binding = 5) uniform texture3D occlusion_texture;

layout(set = 0, binding = 6) uniform sampler linear_sampler;
layout(set = 0, binding = 7) uniform sampler linear_sampler_with_mipmaps;

struct ProbeCascadeData {
	vec3 position;
	float to_probe;
	ivec3 probe_world_offset;
	float to_cell; // 1/bounds * grid_size
	vec3 pad;
	float exposure_normalization;
};

layout(rgba16f, set = 0, binding = 9) uniform restrict writeonly image2D ambient_buffer;
layout(rgba16f, set = 0, binding = 10) uniform restrict writeonly image2D reflection_buffer;

layout(set = 0, binding = 11) uniform texture2DArray lightprobe_texture;

layout(set = 0, binding = 12) uniform texture2D depth_buffer;
layout(set = 0, binding = 13) uniform texture2D normal_roughness_buffer;
layout(set = 0, binding = 14) uniform utexture2D voxel_gi_buffer;

layout(set = 0, binding = 15, std140) uniform SDFGI {
	vec3 grid_size;
	uint max_cascades;

	bool use_occlusion;
	int probe_axis_size;
	float probe_to_uvw;
	float normal_bias;

	vec3 lightprobe_tex_pixel_size;
	float energy;

	vec3 lightprobe_uv_offset;
	float y_mult;

	vec3 occlusion_clamp;
	uint pad3;

	vec3 occlusion_renormalize;
	uint pad4;

	vec3 cascade_probe_size;
	uint pad5;

	ProbeCascadeData cascades[SDFGI_MAX_CASCADES];
}
sdfgi;

#define MAX_VOXEL_GI_INSTANCES 8

struct VoxelGIData {
	mat4 xform; // 64 - 64

	vec3 bounds; // 12 - 76
	float dynamic_range; // 4 - 80

	float bias; // 4 - 84
	float normal_bias; // 4 - 88
	bool blend_ambient; // 4 - 92
	uint mipmaps; // 4 - 96

	vec3 aniso; // 12 - 108 (enabled, slab axis, slab size)
	float exposure_normalization; // 4 - 112
};

layout(set = 0, binding = 16, std140) uniform VoxelGIs {
	VoxelGIData data[MAX_VOXEL_GI_INSTANCES];
}
voxel_gi_instances;

layout(set = 0, binding = 17) uniform texture3D voxel_gi_textures[MAX_VOXEL_GI_INSTANCES];
layout(set = 0, binding = 20) uniform texture3D voxel_gi_aniso_textures[MAX_VOXEL_GI_INSTANCES];

layout(set = 0, binding = 18, std140) uniform SceneData {
	mat4x4 inv_projection[2];
	mat4x4 cam_transform;
	vec4 eye_offset[2];

	ivec2 screen_size;
	float pad1;
	float pad2;
}
scene_data;

#ifdef USE_VRS
layout(r8ui, set = 0, binding = 19) uniform restrict readonly uimage2D vrs_buffer;
#endif

#ifdef USE_SCREEN_PROBES

#include "screen_probes_inc.glsl"

layout(set = 1, binding = 0) uniform texture2DArray probe_sh;
layout(set = 1, binding = 1) uniform texture2D probe_history;
layout(rgba16f, set = 1, binding = 2) uniform restrict writeonly image2D probe_history_out;
layout(set = 1, binding = 3) uniform texture2D probe_history_surface; // Depth and normal.
layout(rgba16f, set = 1, binding = 4) uniform restrict writeonly image2D probe_history_surface_out;

layout(set = 1, binding = 5, std140) uniform ScreenProbeParams {
	mat4 reprojection; // Camera relative world space to the clip space of the previous frame.
	vec4 prev_view_z; // Dot with a camera relative world position to get its depth in the previous frame.

	ivec2 probe_grid_size;
	uint probe_spacing;
	uint frame;

	ivec2 buffer_size; // Size of the GI buffers.
	float history_blend;
	uint history_valid;
}
screen_probes;

#endif

layout(push_constant, std430) uniform Params {
	uint max_voxel_gi_instances;
	bool high_quality_vct;
	bool orthogonal;
	uint view_index;

	vec4 proj_info;

	float z_near;
	float z_far;
	float pad2;
	float pad3;
}
params;

vec2 octahedron_wrap(vec2 v) {
	vec2 signVal;
	signVal.x = v.x >= 0.0 ? 1.0 : -1.0;
	signVal.y = v.y >= 0.0 ? 1.0 : -1.0;
	return (1.0 - abs(v.yx)) * signVal;
}

vec2 octahedron_encode(vec3 n) {
	// https://twitter.com/Stubbesaurus/status/937994790553227264
	n /= (abs(n.x) + abs(n.y) + abs(n.z));
	n.xy = n.z >= 0.0 ? n.xy : octahedron_wrap(n.xy);
	n.xy = n.xy * 0.5 + 0.5;
	return n.xy;
}

vec4 blend_color(vec4 src, vec4 dst) {
	vec4 res;
	float sa = 1.0 - src.a;
	res.a = dst.a * sa + src.a;
	if (res.a == 0.0) {
		res.rgb = vec3(0);
	} else {
		res.rgb = (dst.rgb * dst.a * sa + src.rgb * src.a) / res.a;
	}
	return res;
}

vec3 reconstruct_position(ivec2 screen_pos) {
	if (sc_use_full_projection_matrix) {
		vec4 pos;
		pos.xy = (2.0 * vec2(screen_pos) / vec2(scene_data.screen_size)) - 1.0;
		pos.z = texelFetch(sampler2D(depth_buffer, linear_sampler), screen_pos, 0).r;
		pos.w = 1.0;

		pos = scene_data.inv_projection[params.view_index] * pos;

		return pos.xyz / pos.w;
	} else {
		vec3 pos;
		pos.z = texelFetch(sampler2D(depth_buffer, linear_sampler), screen_pos, 0).r;

		pos.z = pos.z * 2.0 - 1.0;
		if (params.orthogonal) {
			pos.z = -(pos.z * (params.z_far - params.z_near) - (params.z_far + params.z_near)) / 2.0;
		} else {
			pos.z = 2.0 * params.z_near * params.z_far / (params.z_far + params.z_near + pos.z * (params.z_far - params.z_near));
		}
		pos.z = -pos.z;

		pos.xy = vec2(screen_pos) * params.proj_info.xy + params.proj_info.zw;
		if (!params.orthogonal) {
			pos.xy *= pos.z;
		}

		pos.y = -pos.y;

		return pos;
	}
}

void sdfvoxel_gi_process(uint cascade, vec3 cascade_pos, vec3 cam_pos, vec3 cam_normal, vec3 cam_specular_normal, float roughness, out vec3 diffuse_light, out vec3 specular_light) {
	cascade_pos += cam_normal * sdfgi.normal_bias;

	vec3 base_pos = floor(cascade_pos);
	//cascade_pos += mix(vec3(0.0),vec3(0.01),lessThan(abs(cascade_pos-base_pos),vec3(0.01))) * cam_normal;
	ivec3 probe_base_pos = ivec3(base_pos);

	vec4 diffuse_accum = vec4(0.0);
	vec3 specular_accum;

	ivec3 tex_pos = ivec3(probe_base_pos.xy, int(cascade));
	tex_pos.x += probe_base_pos.z * sdfgi.probe_axis_size;
	tex_pos.xy = tex_pos.xy * (SDFGI_OCT_SIZE + 2) + ivec2(1);

	vec3 diffuse_posf = (vec3(tex_pos) + vec3(octahedron_encode(cam_normal) * float(SDFGI_OCT_SIZE), 0.0)) * sdfgi.lightprobe_tex_pixel_size;

	vec3 specular_posf = (vec3(tex_pos) + vec3(octahedron_encode(cam_specular_normal) * float(SDFGI_OCT_SIZE), 0.0)) * sdfgi.lightprobe_tex_pixel_size;

	specular_accum = vec3(0.0);

	vec4 light_accum = vec4(0.0);
	float weight_accum = 0.0;

	for (uint j = 0; j < 8; j++) {
		ivec3 offset = (ivec3(j) >> ivec3(0, 1, 2)) & ivec3(1, 1, 1);
		ivec3 probe_posi = probe_base_pos;
		probe_posi += offset;

		// Compute weight

		vec3 probe_pos = vec3(probe_posi);
		vec3 probe_to_pos = cascade_pos - probe_pos;
		vec3 probe_dir = normalize(-probe_to_pos);

		vec3 trilinear = vec3(1.0) - abs(probe_to_pos);
		float weight = trilinear.x * trilinear.y * trilinear.z * max(0.005, dot(cam_normal, probe_dir));

		// Compute lightprobe occlusion

		if (sdfgi.use_occlusion) {
			ivec3 occ_indexv = abs((sdfgi.cascades[cascade].probe_world_offset + probe_posi) & ivec3(1, 1, 1)) * ivec3(1, 2, 4);
			vec4 occ_mask = mix(vec4(0.0), vec4(1.0), equal(ivec4(occ_indexv.x | occ_indexv.y), ivec4(0, 1, 2, 3)));

			vec3 occ_pos = clamp(cascade_pos, probe_pos - sdfgi.occlusion_clamp, probe_pos + sdfgi.occlusion_clamp) * sdfgi.probe_to_uvw;
			occ_pos.z += float(cascade);
			if (occ_indexv.z != 0) { //z bit is on, means index is >=4, so make it switch to the other half of textures
				occ_pos.x += 1.0;
			}

			occ_pos *= sdfgi.occlusion_renormalize;
			float occlusion = dot(textureLod(sampler3D(occlusion_texture, linear_sampler), occ_pos, 0.0), occ_mask);

			weight *= max(occlusion, 0.01);
		}

		// Compute lightprobe texture position

		vec3 diffuse;
		vec3 pos_uvw = diffuse_posf;
		pos_uvw.xy += vec2(offset.xy) * sdfgi.lightprobe_uv_offset.xy;
		pos_uvw.x += float(offset.z) * sdfgi.lightprobe_uv_offset.z;
		diffuse = textureLod(sampler2DArray(lightprobe_texture, linear_sampler), pos_uvw, 0.0).rgb;

		diffuse_accum += vec4(diffuse * weight * sdfgi.cascades[cascade].exposure_normalization, weight);

		{
			vec3 specular = vec3(0.0);
			vec3 pos_uvw = specular_posf;
			pos_uvw.xy += vec2(offset.xy) * sdfgi.lightprobe_uv_offset.xy;
			pos_uvw.x += float(offset.z) * sdfgi.lightprobe_uv_offset.z;
			if (roughness < 0.99) {
				specular = textureLod(sampler2DArray(lightprobe_texture, linear_sampler), pos_uvw + vec3(0, 0, float(sdfgi.max_cascades)), 0.0).rgb;
			}
			if (roughness > 0.2) {
				specular = mix(specular, textureLod(sampler2DArray(lightprobe_texture, linear_sampler), pos_uvw, 0.0).rgb, (roughness - 0.2) * 1.25);
			}

			specular_accum += specular * weight * sdfgi.cascades[cascade].exposure_normalization;
		}
	}

	if (diffuse_accum.a > 0.0) {
		diffuse_accum.rgb /= diffuse_accum.a;
	}

	diffuse_light = diffuse_accum.rgb;

	if (diffuse_accum.a > 0.0) {
		specular_accum /= diffuse_accum.a;
	}

	specular_light = specular_accum;
}

void sdfgi_process(vec3 vertex, vec3 normal, vec3 reflection, float roughness, out vec4 ambient_light, out vec4 reflection_light) {
	//make vertex orientation the world one, but still align to camera
	vertex.y *= sdfgi.y_mult;
	normal.y *= sdfgi.y_mult;
	reflection.y *= sdfgi.y_mult;

	//renormalize
	normal = normalize(normal);
	reflection = normalize(reflection);

	vec3 cam_pos = vertex;
	vec3 cam_normal = normal;

	vec4 light_accum = vec4(0.0);
	float weight_accum = 0.0;

	vec4 light_blend_accum = vec4(0.0);
	float weight_blend_accum = 0.0;

	float blend = -1.0;

	// helper constants, compute once

	uint cascade = 0xFFFFFFFF;
	vec3 cascade_pos;
	vec3 cascade_normal;

	for (uint i = 0; i < sdfgi.max_cascades; i++) {
		cascade_pos = (cam_pos - sdfgi.cascades[i].position) * sdfgi.cascades[i].to_probe;

		if (any(lessThan(cascade_pos, vec3(0.0))) || any(greaterThanEqual(cascade_pos, sdfgi.cascade_probe_size))) {
			continue; //skip cascade
		}

		cascade = i;
		break;
	}

	if (cascade < SDFGI_MAX_CASCADES) {
		ambient_light = vec4(0, 0, 0, 1);
		reflection_light = vec4(0, 0, 0, 1);

		float blend;
		vec3 diffuse, specular;
		sdfvoxel_gi_process(cascade, cascade_pos, cam_pos, cam_normal, reflection, roughness, diffuse, specular);

		{
			//process blend
			float blend_from = (float(sdfgi.probe_axis_size - 1) / 2.0) - 2.5;
			float blend_to = blend_from + 2.0;

			vec3 inner_pos = cam_pos * sdfgi.cascades[cascade].to_probe;

			float len = length(inner_pos);

			inner_pos = abs(normalize(inner_pos));
			len *= max(inner_pos.x, max(inner_pos.y, inner_pos.z));

			if (len >= blend_from) {
				blend = smoothstep(blend_from, blend_to, len);
			} else {
				blend = 0.0;
			}
		}

		if (blend > 0.0) {
			//blend
			if (cascade == sdfgi.max_cascades - 1) {
				ambient_light.a = 1.0 - blend;
				reflection_light.a = 1.0 - blend;

			} else {
				vec3 diffuse2, specular2;
				cascade_pos = (cam_pos - sdfgi.cascades[cascade + 1].position) * sdfgi.cascades[cascade + 1].to_probe;
				sdfvoxel_gi_process(cascade + 1, cascade_pos, cam_pos, cam_normal, reflection, roughness, diffuse2, specular2);
				diffuse = mix(diffuse, diffuse2, blend);
				specular = mix(specular, specular2, blend);
			}
		}

		ambient_light.rgb = diffuse;

		if (roughness < 0.2) {
			vec3 pos_to_uvw = 1.0 / sdfgi.grid_size;
			vec4 light_accum = vec4(0.0);

			float blend_size = (sdfgi.grid_size.x / float(sdfgi.probe_axis_size - 1)) * 0.5;

			float radius_sizes[SDFGI_MAX_CASCADES];
			cascade = 0xFFFF;

			float base_distance = length(cam_pos);
			for (uint i = 0; i < sdfgi.max_cascades; i++) {
				radius_sizes[i] = (1.0 / sdfgi.cascades[i].to_cell) * (sdfgi.grid_size.x * 0.5 - blend_size);
				if (cascade == 0xFFFF && base_distance < radius_sizes[i]) {
					cascade = i;
				}
			}

			cascade = min(cascade, sdfgi.max_cascades - 1);

			float max_distance = radius_sizes[sdfgi.max_cascades - 1];
			vec3 ray_pos = cam_pos;
			vec3 ray_dir = reflection;

			{
				float prev_radius = cascade > 0 ? radius_sizes[cascade - 1] : 0.0;
				float base_blend = (base_distance - prev_radius) / (radius_sizes[cascade] - prev_radius);
				float bias = (1.0 + base_blend) * 1.1;
				vec3 abs_ray_dir = abs(ray_dir);
				//ray_pos += ray_dir * (bias / sdfgi.cascades[cascade].to_cell); //bias to avoid self occlusion
				ray_pos += (ray_dir * 1.0 / max(abs_ray_dir.x, max(abs_ray_dir.y, abs_ray_dir.z)) + cam_normal * 1.4) * bias / sdfgi.cascades[cascade].to_cell;
			}
			float softness = 0.2 + min(1.0, roughness * 5.0) * 4.0; //approximation to roughness so it does not seem like a hard fade
			uint i = 0;
			bool found = false;
			while (true) {
				if (length(ray_pos) >= max_distance || light_accum.a > 0.99) {
					break;
				}
				if (!found && i >= cascade && length(ray_pos) < radius_sizes[i]) {
					uint next_i = min(i + 1, sdfgi.max_cascades - 1);
					cascade = max(i, cascade); //never go down

					vec3 pos = ray_pos - sdfgi.cascades[i].position;
					pos *= sdfgi.cascades[i].to_cell * pos_to_uvw;

					float fdistance = textureLod(sampler3D(sdf_cascades[i], linear_sampler), pos, 0.0).r * 255.0 - 1.1;

					vec4 hit_light = vec4(0.0);
					if (fdistance < softness) {
						hit_light.rgb = textureLod(sampler3D(light_cascades[i], linear_sampler), pos, 0.0).rgb;
						hit_light.rgb *= 0.5; //approximation given value read is actually meant for anisotropy
						hit_light.a = clamp(1.0 - (fdistance / softness), 0.0, 1.0);
						hit_light.rgb *= hit_light.a;
					}

					fdistance /= sdfgi.cascades[i].to_cell;

					if (i < (sdfgi.max_cascades - 1)) {
						pos = ray_pos - sdfgi.cascades[next_i].position;
						pos *= sdfgi.cascades[next_i].to_cell * pos_to_uvw;

						float fdistance2 = textureLod(sampler3D(sdf_cascades[next_i], linear_sampler), pos, 0.0).r * 255.0 - 1.1;

						vec4 hit_light2 = vec4(0.0);
						if (fdistance2 < softness) {
							hit_light2.rgb = textureLod(sampler3D(light_cascades[next_i], linear_sampler), pos, 0.0).rgb;
							hit_light2.rgb *= 0.5; //approximation given value read is actually meant for anisotropy
							hit_light2.a = clamp(1.0 - (fdistance2 / softness), 0.0, 1.0);
							hit_light2.rgb *= hit_light2.a;
						}

						float prev_radius = i == 0 ? 0.0 : radius_sizes[max(0, i - 1)];
						float blend = clamp((length(ray_pos) - prev_radius) / (radius_sizes[i] - prev_radius), 0.0, 1.0);

						fdistance2 /= sdfgi.cascades[next_i].to_cell;

						hit_light = mix(hit_light, hit_light2, blend);
						fdistance = mix(fdistance, fdistance2, blend);
					}

					light_accum += hit_light;
					ray_pos += ray_dir * fdistance;
					found = true;
				}
				i++;
				if (i == sdfgi.max_cascades) {
					i = 0;
					found = false;
				}
			}

			vec3 light = light_accum.rgb / max(light_accum.a, 0.00001);
			float alpha = min(1.0, light_accum.a);

			float b = min(1.0, roughness * 5.0);

			float sa = 1.0 - b;

			reflection_light.a = alpha * sa + b;
			if (reflection_light.a == 0) {
				specular = vec3(0.0);
			} else {
				specular = (light * alpha * sa + specular * b) / reflection_light.a;
			}
		}

		reflection_light.rgb = specular;

		ambient_light.rgb *= sdfgi.energy;
		reflection_light.rgb *= sdfgi.energy;
	} else {
		ambient_light = vec4(0);
		reflection_light = vec4(0);
	}
}

#include "voxel_gi_sample_inc.glsl"

//standard voxel cone trace
vec4 voxel_cone_trace(uint p_index, vec3 cell_size, vec3 pos, vec3 direction, float tan_half_angle, float max_distance, float p_bias, float p_normal_dot) {
	float dist = p_bias;
	vec4 color = vec4(0.0);

	while (dist < max_distance && color.a < 1.0) {
		float diameter = max(1.0, 2.0 * tan_half_angle * dist);
		vec3 uvw_pos = (pos + dist * direction) * cell_size;
		float half_diameter = diameter * 0.5;
		//check if outside, then break
		if (any(greaterThan(abs(uvw_pos - 0.5), vec3(0.5f + half_diameter * cell_size)))) {
			break;
		}
		vec4 scolor = voxel_gi_sample(p_index, uvw_pos, log2(diameter), direction, dist * p_normal_dot);
		float a = (1.0 - color.a);
		color += a * scolor;
		dist += half_diameter;
	}

	return color;
}

vec4 voxel_cone_trace_45_degrees(uint p_index, vec3 cell_size, vec3 pos, vec3 direction, float max_distance, float p_bias, float p_normal_dot) {
	float dist = p_bias;
	vec4 color = vec4(0.0);
	float radius = max(0.5, dist);
	float lod_level = log2(radius * 2.0);

	while (dist < max_distance && color.a < 1.0) {
		vec3 uvw_pos = (pos + dist * direction) * cell_size;

		//check if outside, then break
		if (any(greaterThan(abs(uvw_pos - 0.5), vec3(0.5f + radius * cell_size)))) {
			break;
		}
		vec4 scolor = voxel_gi_sample(p_index, uvw_pos, lod_level, direction, dist * p_normal_dot);
		lod_level += 1.0;

		float a = (1.0 - color.a);
		scolor *= a;
		color += scolor;
		dist += radius;
		radius = max(0.5, dist);
	}
	return color;
}

void voxel_gi_compute(uint index, vec3 position, vec3 normal, vec3 ref_vec, mat3 normal_xform, float roughness, bool p_diffuse, inout vec4 out_spec, inout vec4 out_diff, inout float out_blend) {
	position = (voxel_gi_instances.data[index].xform * vec4(position, 1.0)).xyz;
	ref_vec = normalize((voxel_gi_instances.data[index].xform * vec4(ref_vec, 0.0)).xyz);
	normal = normalize((voxel_gi_instances.data[index].xform * vec4(normal, 0.0)).xyz);

	position += normal * voxel_gi_instances.data[index].normal_bias;

	//this causes corrupted pixels, i have no idea why..
	if (any(bvec2(any(lessThan(position, vec3(0.0))), any(greaterThan(position, voxel_gi_instances.data[index].bounds))))) {
		return;
	}

	mat3 dir_xform = mat3(voxel_gi_instances.data[index].xform) * normal_xform;

	vec3 blendv = abs(position / voxel_gi_instances.data[index].bounds * 2.0 - 1.0);
	float blend = clamp(1.0 - max(blendv.x, max(blendv.y, blendv.z)), 0.0, 1.0);
	//float blend=1.0;

	float max_distance = length(voxel_gi_instances.data[index].bounds);
	vec3 cell_size = 1.0 / voxel_gi_instances.data[index].bounds;

	//irradiance

	vec4 light = vec4(0.0);

	if (!p_diffuse) {
		// Diffuse comes from elsewhere (screen probes).
	} else if (params.high_quality_vct) {
		const uint cone_dir_count = 6;
		vec3 cone_dirs[cone_dir_count] = vec3[](
				vec3(0.0, 0.0, 1.0),
				vec3(0.866025, 0.0, 0.5),
				vec3(0.267617, 0.823639, 0.5),
				vec3(-0.700629, 0.509037, 0.5),
				vec3(-0.700629, -0.509037, 0.5),
				vec3(0.267617, -0.823639, 0.5));

		float cone_weights[cone_dir_count] = float[](0.25, 0.15, 0.15, 0.15, 0.15, 0.15);
		float cone_angle_tan = 0.577;

		for (uint i = 0; i < cone_dir_count; i++) {
			vec3 dir = normalize(dir_xform * cone_dirs[i]);
			light += cone_weights[i] * voxel_cone_trace(index, cell_size, position, dir, cone_angle_tan, max_distance, voxel_gi_instances.data[index].bias, dot(dir, normal));
		}
	} else {
		const uint cone_dir_count = 4;
		vec3 cone_dirs[cone_dir_count] = vec3[](
				vec3(0.707107, 0.0, 0.707107),
				vec3(0.0, 0.707107, 0.707107),
				vec3(-0.707107, 0.0, 0.707107),
				vec3(0.0, -0.707107, 0.707107));

		float cone_weights[cone_dir_count] = float[](0.25, 0.25, 0.25, 0.25);
		for (int i = 0; i < cone_dir_count; i++) {
			vec3 dir = normalize(dir_xform * cone_dirs[i]);
			light += cone_weights[i] * voxel_cone_trace_45_degrees(index, cell_size, position, dir, max_distance, voxel_gi_instances.data[index].bias, dot(dir, normal));
		}
	}

	light.rgb *= voxel_gi_instances.data[index].dynamic_range * voxel_gi_instances.data[index].exposure_normalization;
	if (!voxel_gi_instances.data[index].blend_ambient) {
		light.a = 1.0;
	}

	out_diff += light * blend;

	//radiance
	vec4 irr_light = voxel_cone_trace(index, cell_size, position, ref_vec, tan(roughness * 0.5 * M_PI * 0.99), max_distance, voxel_gi_instances.data[index].bias, max(0.0, dot(ref_vec, normal)));
	irr_light.rgb *= voxel_gi_instances.data[index].dynamic_range * voxel_gi_instances.data[index].exposure_normalization;
	if (!voxel_gi_instances.data[index].blend_ambient) {
		irr_light.a = 1.0;
	}

	out_spec += irr_light * blend;

	out_blend += blend;
}

vec4 fetch_normal_and_roughness(ivec2 pos) {
	vec4 normal_roughness = texelFetch(sampler2D(normal_roughness_buffer, linear_sampler), pos, 0);
	normal_roughness.xyz = normalize(normal_roughness.xyz * 2.0 - 1.0);
	return normal_roughness;
}

#ifdef USE_SCREEN_PROBES

// Weight of a probe for a surface, based on how close the surface is to the plane of the probe and how similar the normals are.
float screen_probe_surface_weight(ivec2 p_probe, vec3 p_position, vec3 p_normal, float p_surface_scale) {
	ivec2 probe_pixel = screen_probe_get_pixel(p_probe, screen_probes.probe_spacing, screen_probes.frame, scene_data.screen_size);

	vec4 probe_normal_roughness = texelFetch(sampler2D(normal_roughness_buffer, linear_sampler), probe_pixel, 0);
	if (dot(probe_normal_roughness.xyz, probe_normal_roughness.xyz) < 0.01) {
		return 0.0; // No geometry under the probe.
	}
	vec3 probe_normal = normalize(mat3(scene_data.cam_transform) * normalize(probe_normal_roughness.xyz * 2.0 - 1.0));
	vec3 probe_position = mat3(scene_data.cam_transform) * reconstruct_position(probe_pixel);

	float plane_distance = abs(dot(p_position - probe_position, probe_normal)) / p_surface_scale;
	float normal_similarity = max(0.0, dot(probe_normal, p_normal));
	return normal_similarity * normal_similarity / (1.0 + plane_distance * plane_distance);
}

vec4 screen_probe_get_irradiance(ivec2 p_probe, vec3 p_normal) {
	vec4 value;
	value.r = screen_probe_sh_irradiance(texelFetch(sampler2DArray(probe_sh, linear_sampler), ivec3(p_probe, 0), 0), p_normal);
	value.g = screen_probe_sh_irradiance(texelFetch(sampler2DArray(probe_sh, linear_sampler), ivec3(p_probe, 1), 0), p_normal);
	value.b = screen_probe_sh_irradiance(texelFetch(sampler2DArray(probe_sh, linear_sampler), ivec3(p_probe, 2), 0), p_normal);
	value.a = min(1.0, screen_probe_sh_irradiance(texelFetch(sampler2DArray(probe_sh, linear_sampler), ivec3(p_probe, 3), 0), p_normal));
	return value;
}

// Interpolates the 4 closest screen probes, weighted by how well they match the surface of the pixel. When none of them
// does (edges, small objects), looks further around. Returns false when nothing matches, so the caller traces per pixel.
bool screen_probes_gather(ivec2 p_pixel, vec3 p_position, vec3 p_normal, out vec4 r_ambient) {
	vec2 grid_pos = (vec2(p_pixel) + 0.5) / float(screen_probes.probe_spacing) - 0.5;
	ivec2 base = ivec2(floor(grid_pos));
	vec2 f = grid_pos - vec2(base);

	float surface_scale = max(0.05, length(p_position) * 0.02);

	vec4 accum = vec4(0.0);
	float weight_accum = 0.0;

	for (int i = 0; i < 4; i++) {
		ivec2 probe = clamp(base + ivec2(i & 1, i >> 1), ivec2(0), screen_probes.probe_grid_size - 1);
		float weight = (i & 1) != 0 ? f.x : 1.0 - f.x;
		weight *= (i >> 1) != 0 ? f.y : 1.0 - f.y;
		weight *= screen_probe_surface_weight(probe, p_position, p_normal, surface_scale);
		if (weight > 0.0) {
			accum += screen_probe_get_irradiance(probe, p_normal) * weight;
			weight_accum += weight;
		}
	}

	if (weight_accum >= 0.05) {
		r_ambient = accum / weight_accum;
		return true;
	}

	// Ring of 12 probes around the closest 4.
	for (int y = -1; y <= 2; y++) {
		for (int x = -1; x <= 2; x++) {
			if (x >= 0 && x <= 1 && y >= 0 && y <= 1) {
				continue;
			}
			ivec2 probe = base + ivec2(x, y);
			if (any(lessThan(probe, ivec2(0))) || any(greaterThanEqual(probe, screen_probes.probe_grid_size))) {
				continue;
			}
			vec2 offset = vec2(x, y) - f;
			float weight = screen_probe_surface_weight(probe, p_position, p_normal, surface_scale) / (1.0 + dot(offset, offset));
			if (weight > 0.0) {
				accum += screen_probe_get_irradiance(probe, p_normal) * weight;
				weight_accum += weight;
			}
		}
	}

	if (weight_accum >= 0.01) {
		r_ambient = accum / weight_accum;
		return true;
	}

	r_ambient = vec4(0.0);
	return false;
}

// Blends with the reprojected result of the previous frame, and stores the new one for the next frame.
vec4 screen_probes_temporal(ivec2 p_pixel, vec3 p_position, vec3 p_normal, float p_depth, bool p_dynamic_object, vec4 p_ambient) {
	ivec2 buffer_pos = sc_half_res ? p_pixel >> 1 : p_pixel;
	vec4 result = p_ambient;

	if (screen_probes.history_valid != 0) {
		vec4 prev_clip = screen_probes.reprojection * vec4(p_position, 1.0);
		vec2 prev_uv = (prev_clip.xy / prev_clip.w) * 0.5 + 0.5;
		if (all(greaterThanEqual(prev_uv, vec2(0.0))) && all(lessThan(prev_uv, vec2(1.0)))) {
			float prev_depth = dot(screen_probes.prev_view_z, vec4(p_position, 1.0));

			// Positions are reconstructed at the corner of their pixel, so texel N of the history holds the value of the
			// point that projects exactly to pixel N (2 * N at half resolution). Filtering around texel centers instead
			// would shift the history by half a texel every frame, which makes it drift even with a still camera.
			vec2 history_pos = prev_uv * vec2(scene_data.screen_size);
			if (sc_half_res) {
				history_pos *= 0.5;
			}
			ivec2 base = ivec2(floor(history_pos));
			vec2 f = history_pos - vec2(base);

			// Bilinear filtering that only uses texels of the same surface (similar depth and normal), so history
			// doesn't leak across edges when the camera moves.
			vec4 history = vec4(0.0);
			float weight_accum = 0.0;
			for (int i = 0; i < 4; i++) {
				ivec2 offset = ivec2(i & 1, i >> 1);
				ivec2 texel = base + offset;
				if (any(lessThan(texel, ivec2(0))) || any(greaterThanEqual(texel, screen_probes.buffer_size))) {
					continue;
				}
				vec4 history_surface = texelFetch(sampler2D(probe_history_surface, linear_sampler), texel, 0);
				if (abs(history_surface.x - prev_depth) >= prev_depth * 0.05 + 0.02 || dot(history_surface.yzw, p_normal) <= 0.9) {
					continue;
				}
				vec2 weights = mix(1.0 - f, f, vec2(offset));
				float weight = weights.x * weights.y;
				history += texelFetch(sampler2D(probe_history, linear_sampler), texel, 0) * weight;
				weight_accum += weight;
			}

			if (weight_accum > 0.01) {
				history /= weight_accum;
				// Reprojection only follows the camera, so moving objects keep less history.
				float blend = p_dynamic_object ? max(screen_probes.history_blend, 0.5) : screen_probes.history_blend;
				result = mix(history, p_ambient, blend);
			}
		}
	}

	imageStore(probe_history_out, buffer_pos, result);
	imageStore(probe_history_surface_out, buffer_pos, vec4(p_depth, p_normal));
	return result;
}

#endif

void process_gi(ivec2 pos, vec3 vertex, inout vec4 ambient_light, inout vec4 reflection_light) {
	vec4 normal_roughness = fetch_normal_and_roughness(pos);

	vec3 normal = normal_roughness.xyz;

	if (normal.length() > 0.5) {
		//valid normal, can do GI
		float roughness = normal_roughness.w;
		bool dynamic_object = roughness > 0.5;
		if (dynamic_object) {
			roughness = 1.0 - roughness;
		}
		roughness /= (127.0 / 255.0);
		vec3 view = -normalize(mat3(scene_data.cam_transform) * (vertex - scene_data.eye_offset[gl_GlobalInvocationID.z].xyz));
		float view_depth = -vertex.z;
		vertex = mat3(scene_data.cam_transform) * vertex;
		normal = normalize(mat3(scene_data.cam_transform) * normal);
		vec3 reflection = normalize(reflect(-view, normal));

#ifdef USE_SDFGI
		sdfgi_process(vertex, normal, reflection, roughness, ambient_light, reflection_light);
#endif

#ifdef USE_VOXEL_GI_INSTANCES
		{
#ifdef SAMPLE_VOXEL_GI_NEAREST
			uvec2 voxel_gi_tex = texelFetch(voxel_gi_buffer, pos, 0).rg;
#else
			uvec2 voxel_gi_tex = texelFetch(usampler2D(voxel_gi_buffer, linear_sampler), pos, 0).rg;
#endif
			roughness *= roughness;
			//find arbitrary tangent and bitangent, then build a matrix
			vec3 v0 = abs(normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
			vec3 tangent = normalize(cross(v0, normal));
			vec3 bitangent = normalize(cross(tangent, normal));
			mat3 normal_mat = mat3(tangent, bitangent, normal);

			vec4 amb_accum = vec4(0.0);
			vec4 spec_accum = vec4(0.0);
			float blend_accum = 0.0;

			bool diffuse = true;
#ifdef USE_SCREEN_PROBES
			vec4 probe_ambient;
			bool use_probes = screen_probes_gather(pos, vertex, normal, probe_ambient);
			diffuse = !use_probes;
#endif

			for (uint i = 0; i < params.max_voxel_gi_instances; i++) {
				if (any(equal(uvec2(i), voxel_gi_tex))) {
					voxel_gi_compute(i, vertex, normal, reflection, normal_mat, roughness, diffuse, spec_accum, amb_accum, blend_accum);
				}
			}
			if (blend_accum > 0.0) {
				amb_accum /= blend_accum;
				spec_accum /= blend_accum;
			}

#ifdef USE_SCREEN_PROBES
			if (blend_accum > 0.0) {
				// Only inside VoxelGI volumes, so the result matches regular VoxelGI everywhere else.
				if (use_probes) {
					amb_accum = probe_ambient;
				}
				amb_accum = screen_probes_temporal(pos, vertex, normal, view_depth, dynamic_object, amb_accum);
			} else {
				screen_probes_temporal(pos, vertex, normal, view_depth, dynamic_object, amb_accum);
			}
#endif

#ifdef USE_SDFGI
			reflection_light = blend_color(spec_accum, reflection_light);
			ambient_light = blend_color(amb_accum, ambient_light);
#else
			reflection_light = spec_accum;
			ambient_light = amb_accum;
#endif
		}
#endif
	}
}

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);

	uint vrs_x, vrs_y;
#ifdef USE_VRS
	if (sc_use_vrs) {
		ivec2 vrs_pos;

		// Currently we use a 16x16 texel, possibly some day make this configurable.
		if (sc_half_res) {
			vrs_pos = pos >> 3;
		} else {
			vrs_pos = pos >> 4;
		}

		uint vrs_texel = imageLoad(vrs_buffer, vrs_pos).r;
		// note, valid values for vrs_x and vrs_y are 1, 2 and 4.
		vrs_x = 1 << ((vrs_texel >> 2) & 3);
		vrs_y = 1 << (vrs_texel & 3);

		if (mod(pos.x, vrs_x) != 0) {
			return;
		}

		if (mod(pos.y, vrs_y) != 0) {
			return;
		}
	}
#endif

	if (sc_half_res) {
		pos <<= 1;
	}

	if (any(greaterThanEqual(pos, scene_data.screen_size))) { //too large, do nothing
		return;
	}

	vec4 ambient_light = vec4(0.0);
	vec4 reflection_light = vec4(0.0);

	vec3 vertex = reconstruct_position(pos);

	process_gi(pos, vertex, ambient_light, reflection_light);

	if (sc_half_res) {
		pos >>= 1;
	}

	imageStore(ambient_buffer, pos, ambient_light);
	imageStore(reflection_buffer, pos, reflection_light);

#ifdef USE_VRS
	if (sc_use_vrs) {
		if (vrs_x > 1) {
			imageStore(ambient_buffer, pos + ivec2(1, 0), ambient_light);
			imageStore(reflection_buffer, pos + ivec2(1, 0), reflection_light);
		}

		if (vrs_x > 2) {
			imageStore(ambient_buffer, pos + ivec2(2, 0), ambient_light);
			imageStore(reflection_buffer, pos + ivec2(2, 0), reflection_light);

			imageStore(ambient_buffer, pos + ivec2(3, 0), ambient_light);
			imageStore(reflection_buffer, pos + ivec2(3, 0), reflection_light);
		}

		if (vrs_y > 1) {
			imageStore(ambient_buffer, pos + ivec2(0, 1), ambient_light);
			imageStore(reflection_buffer, pos + ivec2(0, 1), reflection_light);
		}

		if (vrs_y > 1 && vrs_x > 1) {
			imageStore(ambient_buffer, pos + ivec2(1, 1), ambient_light);
			imageStore(reflection_buffer, pos + ivec2(1, 1), reflection_light);
		}

		if (vrs_y > 1 && vrs_x > 2) {
			imageStore(ambient_buffer, pos + ivec2(2, 1), ambient_light);
			imageStore(reflection_buffer, pos + ivec2(2, 1), reflection_light);

			imageStore(ambient_buffer, pos + ivec2(3, 1), ambient_light);
			imageStore(reflection_buffer, pos + ivec2(3, 1), reflection_light);
		}

		if (vrs_y > 2) {
			imageStore(ambient_buffer, pos + ivec2(0, 2), ambient_light);
			imageStore(reflection_buffer, pos + ivec2(0, 2), reflection_light);
			imageStore(ambient_buffer, pos + ivec2(0, 3), ambient_light);
			imageStore(reflection_buffer, pos + ivec2(0, 3), reflection_light);
		}

		if (vrs_y > 2 && vrs_x > 1) {
			imageStore(ambient_buffer, pos + ivec2(1, 2), ambient_light);
			imageStore(reflection_buffer, pos + ivec2(1, 2), reflection_light);
			imageStore(ambient_buffer, pos + ivec2(1, 3), ambient_light);
			imageStore(reflection_buffer, pos + ivec2(1, 3), reflection_light);
		}

		if (vrs_y > 2 && vrs_x > 2) {
			imageStore(ambient_buffer, pos + ivec2(2, 2), ambient_light);
			imageStore(reflection_buffer, pos + ivec2(2, 2), reflection_light);
			imageStore(ambient_buffer, pos + ivec2(2, 3), ambient_light);
			imageStore(reflection_buffer, pos + ivec2(2, 3), reflection_light);

			imageStore(ambient_buffer, pos + ivec2(3, 2), ambient_light);
			imageStore(reflection_buffer, pos + ivec2(3, 2), reflection_light);
			imageStore(ambient_buffer, pos + ivec2(3, 3), ambient_light);
			imageStore(reflection_buffer, pos + ivec2(3, 3), reflection_light);
		}
	}
#endif
}
