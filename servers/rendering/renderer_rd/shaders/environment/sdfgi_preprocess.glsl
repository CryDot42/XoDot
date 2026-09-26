#[compute]

#version 450

#VERSION_DEFINES

#ifdef MODE_JUMPFLOOD_OPTIMIZED
#define GROUP_SIZE 8

layout(local_size_x = GROUP_SIZE, local_size_y = GROUP_SIZE, local_size_z = GROUP_SIZE) in;

#elif defined(MODE_OCCLUSION) || defined(MODE_SCROLL)
//buffer layout
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

#else
//grid layout
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

#endif

#if defined(MODE_INITIALIZE_JUMP_FLOOD) || defined(MODE_INITIALIZE_JUMP_FLOOD_HALF)
layout(r16ui, set = 0, binding = 1) uniform restrict readonly uimage3D src_color;
layout(rgba8ui, set = 0, binding = 2) uniform restrict writeonly uimage3D dst_positions;
#endif

#ifdef MODE_UPSCALE_JUMP_FLOOD
layout(r16ui, set = 0, binding = 1) uniform restrict readonly uimage3D src_color;
layout(rgba8ui, set = 0, binding = 2) uniform restrict readonly uimage3D src_positions_half;
layout(rgba8ui, set = 0, binding = 3) uniform restrict writeonly uimage3D dst_positions;
#endif

#if defined(MODE_JUMPFLOOD) || defined(MODE_JUMPFLOOD_OPTIMIZED)
layout(rgba8ui, set = 0, binding = 1) uniform restrict readonly uimage3D src_positions;
layout(rgba8ui, set = 0, binding = 2) uniform restrict writeonly uimage3D dst_positions;
#endif

#ifdef MODE_JUMPFLOOD_OPTIMIZED

shared uvec4 group_positions[(GROUP_SIZE + 2) * (GROUP_SIZE + 2) * (GROUP_SIZE + 2)]; //4x4x4 with margins

void group_store(ivec3 p_pos, uvec4 p_value) {
	uint offset = uint(p_pos.z * (GROUP_SIZE + 2) * (GROUP_SIZE + 2) + p_pos.y * (GROUP_SIZE + 2) + p_pos.x);
	group_positions[offset] = p_value;
}

uvec4 group_load(ivec3 p_pos) {
	uint offset = uint(p_pos.z * (GROUP_SIZE + 2) * (GROUP_SIZE + 2) + p_pos.y * (GROUP_SIZE + 2) + p_pos.x);
	return group_positions[offset];
}

#endif

#ifdef MODE_OCCLUSION

layout(r16ui, set = 0, binding = 1) uniform restrict readonly uimage3D src_color;
layout(r8, set = 0, binding = 2) uniform restrict image3D dst_occlusion[8];
layout(r32ui, set = 0, binding = 3) uniform restrict readonly uimage3D src_facing;

#define OCC_REGION_SIZE (OCCLUSION_SIZE * 2)

shared uint occlusion_facing[(OCC_REGION_SIZE * OCC_REGION_SIZE * OCC_REGION_SIZE) / 4];

uint get_facing(ivec3 p_pos) {
	uint ofs = uint(p_pos.z * OCC_REGION_SIZE * OCC_REGION_SIZE + p_pos.y * OCC_REGION_SIZE + p_pos.x);
	uint v = occlusion_facing[ofs / 4];
	return (v >> ((ofs % 4) * 8)) & 0xFF;
}

// Whether the probe at p_to (region voxel coordinates, where voxel v spans [v, v + 1)) can see
// the point p_from, walking the voxels in between (Amanatides & Woo) and stopping at the first
// solid one. The voxel p_from lies in is not tested, the one the probe is reached through is: a
// probe hidden in geometry on that side cannot see past it.
float occlusion_trace(vec3 p_from, vec3 p_to) {
	vec3 ray = p_to - p_from;
	ivec3 cell = ivec3(floor(p_from));
	ivec3 cell_step = ivec3(sign(ray));
	vec3 t_delta = 1.0 / max(abs(ray), vec3(1e-6));
	vec3 t_max = abs(vec3(cell) + max(vec3(cell_step), vec3(0.0)) - p_from) * t_delta;

	// A segment between two points of the region can cross at most this many voxel boundaries.
	for (int i = 0; i < OCC_REGION_SIZE * 3; i++) {
		if (min(t_max.x, min(t_max.y, t_max.z)) >= 1.0 - 1e-4) {
			return 1.0; // Reached the probe without meeting anything solid.
		}
		if (t_max.x <= t_max.y && t_max.x <= t_max.z) {
			cell.x += cell_step.x;
			t_max.x += t_delta.x;
		} else if (t_max.y <= t_max.z) {
			cell.y += cell_step.y;
			t_max.y += t_delta.y;
		} else {
			cell.z += cell_step.z;
			t_max.z += t_delta.z;
		}
		if (any(lessThan(cell, ivec3(0))) || any(greaterThanEqual(cell, ivec3(OCC_REGION_SIZE)))) {
			return 1.0;
		}
		if (get_facing(cell) != 0) {
			return 0.0;
		}
	}
	return 1.0;
}

#endif

#ifdef MODE_STORE

layout(rgba8ui, set = 0, binding = 1) uniform restrict readonly uimage3D src_positions;
layout(r16ui, set = 0, binding = 2) uniform restrict readonly uimage3D src_albedo;
layout(r8, set = 0, binding = 3) uniform restrict readonly image3D src_occlusion[8];
layout(r32ui, set = 0, binding = 4) uniform restrict readonly uimage3D src_light;
layout(r32ui, set = 0, binding = 5) uniform restrict readonly uimage3D src_light_aniso;
layout(r32ui, set = 0, binding = 6) uniform restrict readonly uimage3D src_facing;

layout(r8, set = 0, binding = 7) uniform restrict writeonly image3D dst_sdf;
layout(r16ui, set = 0, binding = 8) uniform restrict writeonly uimage3D dst_occlusion;

layout(set = 0, binding = 10, std430) restrict buffer DispatchData {
	uint x;
	uint y;
	uint z;
	uint total_count;
}
dispatch_data;

struct ProcessVoxel {
	uint position; // xyz 7 bit packed, extra 11 bits for neighbors.
	uint albedo; //rgb bits 0-15 albedo, bits 16-21 are normal bits (set if geometry exists toward that side), extra 11 bits for neighbors
	uint light; //rgbe8985 encoded total saved light, extra 2 bits for neighbors
	uint light_aniso; //55555 light anisotropy, extra 2 bits for neighbors
	//total neighbors: 26
};

layout(set = 0, binding = 11, std430) restrict buffer writeonly ProcessVoxels {
	ProcessVoxel data[];
}
dst_process_voxels;

shared ProcessVoxel store_positions[4 * 4 * 4];
shared uint store_position_count;
shared uint store_from_index;
#endif

#ifdef MODE_SCROLL

layout(r16ui, set = 0, binding = 1) uniform restrict writeonly uimage3D dst_albedo;
layout(r32ui, set = 0, binding = 2) uniform restrict writeonly uimage3D dst_facing;
layout(r32ui, set = 0, binding = 3) uniform restrict writeonly uimage3D dst_light;
layout(r32ui, set = 0, binding = 4) uniform restrict writeonly uimage3D dst_light_aniso;

layout(set = 0, binding = 5, std430) restrict buffer readonly DispatchData {
	uint x;
	uint y;
	uint z;
	uint total_count;
}
dispatch_data;

struct ProcessVoxel {
	uint position; // xyz 7 bit packed, extra 11 bits for neighbors.
	uint albedo; //rgb bits 0-15 albedo, bits 16-21 are normal bits (set if geometry exists toward that side), extra 11 bits for neighbors
	uint light; //rgbe8985 encoded total saved light, extra 2 bits for neighbors
	uint light_aniso; //55555 light anisotropy, extra 2 bits for neighbors
	//total neighbors: 26
};

layout(set = 0, binding = 6, std430) restrict buffer readonly ProcessVoxels {
	ProcessVoxel data[];
}
src_process_voxels;

#endif

#ifdef MODE_SCROLL_OCCLUSION

layout(r8, set = 0, binding = 1) uniform restrict image3D dst_occlusion[8];
layout(r16ui, set = 0, binding = 2) uniform restrict readonly uimage3D src_occlusion;

#endif

layout(push_constant, std430) uniform Params {
	ivec3 scroll;

	int grid_size;

	ivec3 probe_offset;
	int step_size;

	bool half_size;
	uint occlusion_index;
	int cascade;
	uint pad;
}
params;

void main() {
#ifdef MODE_SCROLL

	// Pixel being shaded
	int index = int(gl_GlobalInvocationID.x);
	if (index >= dispatch_data.total_count) { //too big
		return;
	}

	ivec3 read_pos = (ivec3(src_process_voxels.data[index].position) >> ivec3(0, 7, 14)) & ivec3(0x7F);
	ivec3 write_pos = read_pos + params.scroll;

	if (any(lessThan(write_pos, ivec3(0))) || any(greaterThanEqual(write_pos, ivec3(params.grid_size)))) {
		return; // Fits outside the 3D texture, don't do anything.
	}

	uint albedo = ((src_process_voxels.data[index].albedo & 0x7FFF) << 1) | 1; //add solid bit
	imageStore(dst_albedo, write_pos, uvec4(albedo));

	uint facing = (src_process_voxels.data[index].albedo >> 15) & 0x3F; //6 anisotropic facing bits
	imageStore(dst_facing, write_pos, uvec4(facing));

	uint light = src_process_voxels.data[index].light & 0x3fffffff; //30 bits of RGBE8985
	imageStore(dst_light, write_pos, uvec4(light));

	uint light_aniso = src_process_voxels.data[index].light_aniso & 0x3fffffff; //30 bits of 6 anisotropic 5 bits values
	imageStore(dst_light_aniso, write_pos, uvec4(light_aniso));

#endif

#ifdef MODE_SCROLL_OCCLUSION

	ivec3 pos = ivec3(gl_GlobalInvocationID.xyz);
	if (any(greaterThanEqual(pos, ivec3(params.grid_size) - abs(params.scroll)))) { //too large, do nothing
		return;
	}

	ivec3 read_pos = pos + max(ivec3(0), -params.scroll);
	ivec3 write_pos = pos + max(ivec3(0), params.scroll);

	read_pos.z += params.cascade * params.grid_size;
	uint occlusion = imageLoad(src_occlusion, read_pos).r;
	read_pos.x += params.grid_size;
	occlusion |= imageLoad(src_occlusion, read_pos).r << 16;

	const uint occlusion_shift[8] = uint[](4, 8, 12, 0, 20, 24, 28, 16); // Channel i lands in r, g, b, a of B4G4R4A4 (see create()).

	for (uint i = 0; i < 8; i++) {
		float o = float((occlusion >> occlusion_shift[i]) & 0xF) / 15.0;
		imageStore(dst_occlusion[i], write_pos, vec4(o));
	}

#endif

#ifdef MODE_INITIALIZE_JUMP_FLOOD

	ivec3 pos = ivec3(gl_GlobalInvocationID.xyz);

	uint c = imageLoad(src_color, pos).r;
	uvec4 v;
	if (bool(c & 0x1)) {
		//bit set means this is solid
		v.xyz = uvec3(pos);
		v.w = 255; //not zero means used
	} else {
		v.xyz = uvec3(0);
		v.w = 0; // zero means unused
	}

	imageStore(dst_positions, pos, v);
#endif

#ifdef MODE_INITIALIZE_JUMP_FLOOD_HALF

	ivec3 pos = ivec3(gl_GlobalInvocationID.xyz);
	ivec3 base_pos = pos * 2;

	//since we store in half size, lets kind of randomize what we store, so
	//the half size jump flood has a bit better chance to find something
	uvec4 closest[8];
	int closest_count = 0;

	for (uint i = 0; i < 8; i++) {
		ivec3 src_pos = base_pos + ((ivec3(i) >> ivec3(0, 1, 2)) & ivec3(1, 1, 1));
		uint c = imageLoad(src_color, src_pos).r;
		if (bool(c & 1)) {
			uvec4 v = uvec4(uvec3(src_pos), 255);
			closest[closest_count] = v;
			closest_count++;
		}
	}

	if (closest_count == 0) {
		imageStore(dst_positions, pos, uvec4(0));
	} else {
		ivec3 indexv = (pos & ivec3(1, 1, 1)) * ivec3(1, 2, 4);
		int index = (indexv.x | indexv.y | indexv.z) % closest_count;
		imageStore(dst_positions, pos, closest[index]);
	}

#endif

#ifdef MODE_JUMPFLOOD

	//regular jumpflood, efficient for large steps, inefficient for small steps
	ivec3 pos = ivec3(gl_GlobalInvocationID.xyz);

	vec3 posf = vec3(pos);

	if (params.half_size) {
		posf = posf * 2.0 + 0.5;
	}

	uvec4 p = imageLoad(src_positions, pos);

	if (!params.half_size && p == uvec4(uvec3(pos), 255)) {
		imageStore(dst_positions, pos, p);
		return; //points to itself and valid, nothing better can be done, just pass
	}

	float p_dist;

	if (p.w != 0) {
		p_dist = distance(posf, vec3(p.xyz));
	} else {
		p_dist = 0.0; //should not matter
	}

	const uint offset_count = 26;
	const ivec3 offsets[offset_count] = ivec3[](
			ivec3(-1, -1, -1),
			ivec3(-1, -1, 0),
			ivec3(-1, -1, 1),
			ivec3(-1, 0, -1),
			ivec3(-1, 0, 0),
			ivec3(-1, 0, 1),
			ivec3(-1, 1, -1),
			ivec3(-1, 1, 0),
			ivec3(-1, 1, 1),
			ivec3(0, -1, -1),
			ivec3(0, -1, 0),
			ivec3(0, -1, 1),
			ivec3(0, 0, -1),
			ivec3(0, 0, 1),
			ivec3(0, 1, -1),
			ivec3(0, 1, 0),
			ivec3(0, 1, 1),
			ivec3(1, -1, -1),
			ivec3(1, -1, 0),
			ivec3(1, -1, 1),
			ivec3(1, 0, -1),
			ivec3(1, 0, 0),
			ivec3(1, 0, 1),
			ivec3(1, 1, -1),
			ivec3(1, 1, 0),
			ivec3(1, 1, 1));

	for (uint i = 0; i < offset_count; i++) {
		ivec3 ofs = pos + offsets[i] * params.step_size;
		if (any(lessThan(ofs, ivec3(0))) || any(greaterThanEqual(ofs, ivec3(params.grid_size)))) {
			continue;
		}
		uvec4 q = imageLoad(src_positions, ofs);

		if (q.w == 0) {
			continue; //was not initialized yet, ignore
		}

		float q_dist = distance(posf, vec3(q.xyz));
		if (p.w == 0 || q_dist < p_dist) {
			p = q; //just replace because current is unused
			p_dist = q_dist;
		}
	}

	imageStore(dst_positions, pos, p);
#endif

#ifdef MODE_JUMPFLOOD_OPTIMIZED
	//optimized version using shared compute memory

	ivec3 group_offset = ivec3(gl_WorkGroupID.xyz) % params.step_size;
	ivec3 group_pos = group_offset + (ivec3(gl_WorkGroupID.xyz) / params.step_size) * ivec3(GROUP_SIZE * params.step_size);

	//load data into local group memory

	if (all(lessThan(ivec3(gl_LocalInvocationID.xyz), ivec3((GROUP_SIZE + 2) / 2)))) {
		//use this thread for loading, this method uses less threads for this but its simpler and less divergent
		ivec3 base_pos = ivec3(gl_LocalInvocationID.xyz) * 2;
		for (uint i = 0; i < 8; i++) {
			ivec3 load_pos = base_pos + ((ivec3(i) >> ivec3(0, 1, 2)) & ivec3(1, 1, 1));
			ivec3 load_global_pos = group_pos + (load_pos - ivec3(1)) * params.step_size;
			uvec4 q;
			if (all(greaterThanEqual(load_global_pos, ivec3(0))) && all(lessThan(load_global_pos, ivec3(params.grid_size)))) {
				q = imageLoad(src_positions, load_global_pos);
			} else {
				q = uvec4(0); //unused
			}

			group_store(load_pos, q);
		}
	}

	//sync
	groupMemoryBarrier();
	barrier();

	ivec3 global_pos = group_pos + ivec3(gl_LocalInvocationID.xyz) * params.step_size;

	if (any(lessThan(global_pos, ivec3(0))) || any(greaterThanEqual(global_pos, ivec3(params.grid_size)))) {
		return; //do nothing else, end here because outside range
	}

	ivec3 local_pos = ivec3(gl_LocalInvocationID.xyz) + ivec3(1);

	const uint offset_count = 27;
	const ivec3 offsets[offset_count] = ivec3[](
			ivec3(-1, -1, -1),
			ivec3(-1, -1, 0),
			ivec3(-1, -1, 1),
			ivec3(-1, 0, -1),
			ivec3(-1, 0, 0),
			ivec3(-1, 0, 1),
			ivec3(-1, 1, -1),
			ivec3(-1, 1, 0),
			ivec3(-1, 1, 1),
			ivec3(0, -1, -1),
			ivec3(0, -1, 0),
			ivec3(0, -1, 1),
			ivec3(0, 0, -1),
			ivec3(0, 0, 0),
			ivec3(0, 0, 1),
			ivec3(0, 1, -1),
			ivec3(0, 1, 0),
			ivec3(0, 1, 1),
			ivec3(1, -1, -1),
			ivec3(1, -1, 0),
			ivec3(1, -1, 1),
			ivec3(1, 0, -1),
			ivec3(1, 0, 0),
			ivec3(1, 0, 1),
			ivec3(1, 1, -1),
			ivec3(1, 1, 0),
			ivec3(1, 1, 1));

	//only makes sense if point is inside screen
	uvec4 closest = uvec4(0);
	float closest_dist = 0.0;

	vec3 posf = vec3(global_pos);

	if (params.half_size) {
		posf = posf * 2.0 + 0.5;
	}

	for (uint i = 0; i < offset_count; i++) {
		uvec4 point = group_load(local_pos + offsets[i]);

		if (point.w == 0) {
			continue; //was not initialized yet, ignore
		}

		float dist = distance(posf, vec3(point.xyz));
		if (closest.w == 0 || dist < closest_dist) {
			closest = point;
			closest_dist = dist;
		}
	}

	imageStore(dst_positions, global_pos, closest);

#endif

#ifdef MODE_UPSCALE_JUMP_FLOOD

	ivec3 pos = ivec3(gl_GlobalInvocationID.xyz);

	uint c = imageLoad(src_color, pos).r;
	uvec4 v;
	if (bool(c & 1)) {
		//bit set means this is solid
		v.xyz = uvec3(pos);
		v.w = 255; //not zero means used
	} else {
		v = imageLoad(src_positions_half, pos >> 1);
		float d = length(vec3(ivec3(v.xyz) - pos));

		ivec3 vbase = ivec3(v.xyz - (v.xyz & uvec3(1)));

		//search around if there is a better candidate from the same block
		for (int i = 0; i < 8; i++) {
			ivec3 bits = ((ivec3(i) >> ivec3(0, 1, 2)) & ivec3(1, 1, 1));
			ivec3 p = vbase + bits;

			float d2 = length(vec3(p - pos));
			if (d2 < d) { //check valid distance before test so we avoid a read
				uint c2 = imageLoad(src_color, p).r;
				if (bool(c2 & 1)) {
					v.xyz = uvec3(p);
					d = d2;
				}
			}
		}

		//could validate better position..
	}

	imageStore(dst_positions, pos, v);

#endif

#ifdef MODE_OCCLUSION

	// One group per probe, working on the (2 * OCCLUSION_SIZE)^3 voxels around it: the eight probe
	// cells the probe is a corner of. Each voxel stores, in the channel for this probe's index
	// parity, whether the probe can see it (see sdfvoxel_gi_process() in gi.glsl for the lookup).
	//
	// This used to propagate visibility outwards from the probe as a wavefront, each voxel taking
	// the average of its three neighbors towards the probe. That behaves like diffusion: it seeps
	// around corners and fans out through doorways, so the probe was partly "visible" well into
	// places it has no line of sight to. Walking the actual line from each voxel to the probe does
	// not, and costs about the same, from the same shared-memory copy of the geometry.

	uint invocation_idx = uint(gl_LocalInvocationID.x);
	ivec3 region = ivec3(gl_WorkGroupID);

	ivec3 region_offset = -ivec3(OCCLUSION_SIZE);
	region_offset += region * OCCLUSION_SIZE * 2;
	region_offset += params.probe_offset * OCCLUSION_SIZE;

	bool region_out_of_bounds = false;

	if (params.scroll != ivec3(0)) {
		//validate scroll region
		ivec3 region_offset_to = region_offset + ivec3(OCCLUSION_SIZE * 2);
		uvec3 scroll_mask = uvec3(notEqual(params.scroll, ivec3(0))); //save which axes acre scrolling
		ivec3 scroll_from = mix(ivec3(0), ivec3(params.grid_size) + params.scroll, lessThan(params.scroll, ivec3(0)));
		ivec3 scroll_to = mix(ivec3(params.grid_size), params.scroll, greaterThan(params.scroll, ivec3(0)));

		if ((uvec3(lessThanEqual(region_offset_to, scroll_from)) | uvec3(greaterThanEqual(region_offset, scroll_to))) * scroll_mask == scroll_mask) { //all axes that scroll are out, exit
			region_out_of_bounds = true; //region outside scroll bounds, quit
		}
	}

#define OCC_HALF_SIZE (OCCLUSION_SIZE / 2)

	ivec3 local_ofs = ivec3(uvec3(invocation_idx % OCC_HALF_SIZE, (invocation_idx % (OCC_HALF_SIZE * OCC_HALF_SIZE)) / OCC_HALF_SIZE, invocation_idx / (OCC_HALF_SIZE * OCC_HALF_SIZE))) * 4;

	if (!region_out_of_bounds) {
		for (int i = 0; i < 16; i++) { //skip x, so it can be packed

			ivec3 offset = local_ofs + ((ivec3(i * 4) >> ivec3(0, 2, 4)) & ivec3(3, 3, 3));

			uint facing_pack = 0;
			for (int j = 0; j < 4; j++) {
				ivec3 foffset = region_offset + offset + ivec3(j, 0, 0);
				if (all(greaterThanEqual(foffset, ivec3(0))) && all(lessThan(foffset, ivec3(params.grid_size)))) {
					uint f = imageLoad(src_facing, foffset).r;
					facing_pack |= f << (j * 8);
				}
			}

			occlusion_facing[(offset.z * (OCCLUSION_SIZE * 2 * OCCLUSION_SIZE * 2) + offset.y * (OCCLUSION_SIZE * 2) + offset.x) / 4] = facing_pack;
		}
	}

	//sync occlusion saved
	groupMemoryBarrier();
	barrier();

	// There are no more barriers after this, so we can early return.
	if (region_out_of_bounds) {
		return;
	}

	// The probe sits on the corner shared by the region's eight middle voxels.
	vec3 probe_pos = vec3(OCCLUSION_SIZE);

	const ivec3 facing_dirs[6] = ivec3[](ivec3(1, 0, 0), ivec3(0, 1, 0), ivec3(0, 0, 1), ivec3(-1, 0, 0), ivec3(0, -1, 0), ivec3(0, 0, -1));

	for (int i = 0; i < 64; i++) {
		ivec3 local_offset = local_ofs + ((ivec3(i) >> ivec3(0, 2, 4)) & ivec3(3, 3, 3));
		ivec3 offset = region_offset + local_offset;

		if (any(lessThan(offset, ivec3(0))) || any(greaterThanEqual(offset, ivec3(params.grid_size)))) {
			continue;
		}

		float occ = 0.0;
		uint facing = get_facing(local_offset);

		if (facing == 0) {
			occ = occlusion_trace(vec3(local_offset) + vec3(0.5), probe_pos);
		} else {
			// A surface voxel. Shading points on it look it up (after their normal bias) mostly
			// from its open side, so give it what the probe looks like from the free voxels it
			// faces, rather than from inside the geometry, where nothing is visible. Take the
			// least visible of them: geometry thinner than a voxel faces both ways, and the
			// average of its two sides would leave every probe half visible through it, which
			// points near the wall pick up through filtering (the light that used to seep in
			// along the edges of thin walls, floors and ceilings).
			occ = 1.0;
			bool any_side = false;
			for (int k = 0; k < 6; k++) {
				if (!bool(facing & (1 << k))) {
					continue;
				}
				ivec3 side = local_offset + facing_dirs[k];
				if (any(lessThan(side, ivec3(0))) || any(greaterThanEqual(side, ivec3(OCC_REGION_SIZE))) || get_facing(side) != 0) {
					continue;
				}
				occ = min(occ, occlusion_trace(vec3(side) + vec3(0.5), probe_pos));
				any_side = true;
			}
			if (!any_side) {
				occ = 0.0;
			}
		}

		imageStore(dst_occlusion[params.occlusion_index], offset, vec4(occ));
	}

#endif

#ifdef MODE_STORE

	ivec3 local = ivec3(gl_LocalInvocationID.xyz);
	ivec3 pos = ivec3(gl_GlobalInvocationID.xyz);
	// store SDF
	uvec4 p = imageLoad(src_positions, pos);

	bool solid = false;
	float d;
	if (ivec3(p.xyz) == pos) {
		//solid block
		d = 0;
		solid = true;
	} else {
		//distance block
		d = 1.0 + length(vec3(p.xyz) - vec3(pos));
	}

	d /= 255.0;

	imageStore(dst_sdf, pos, vec4(d));

	// STORE OCCLUSION

	uint occlusion = 0;
	const uint occlusion_shift[8] = uint[](4, 8, 12, 0, 20, 24, 28, 16); // Channel i lands in r, g, b, a of B4G4R4A4 (see create()).
	for (int i = 0; i < 8; i++) {
		float occ = imageLoad(src_occlusion[i], pos).r;
		occlusion |= uint(clamp(occ * 15.0, 0.0, 15.0)) << occlusion_shift[i];
	}
	{
		ivec3 occ_pos = pos;
		occ_pos.z += params.cascade * params.grid_size;
		imageStore(dst_occlusion, occ_pos, uvec4(occlusion & 0xFFFF));
		occ_pos.x += params.grid_size;
		imageStore(dst_occlusion, occ_pos, uvec4(occlusion >> 16));
	}

	// STORE POSITIONS

	if (local == ivec3(0)) {
		store_position_count = 0; //base one stores as zero, the others wait
	}

	groupMemoryBarrier();
	barrier();

	if (solid) {
		uint index = atomicAdd(store_position_count, 1);
		// At least do the conversion work in parallel
		store_positions[index].position = uint(pos.x | (pos.y << 7) | (pos.z << 14));

		//see around which voxels point to this one, add them to the list
		uint bit_index = 0;
		uint neighbour_bits = 0;
		for (int i = -1; i <= 1; i++) {
			for (int j = -1; j <= 1; j++) {
				for (int k = -1; k <= 1; k++) {
					if (i == 0 && j == 0 && k == 0) {
						continue;
					}
					ivec3 npos = pos + ivec3(i, j, k);
					if (all(greaterThanEqual(npos, ivec3(0))) && all(lessThan(npos, ivec3(params.grid_size)))) {
						p = imageLoad(src_positions, npos);
						if (ivec3(p.xyz) == pos) {
							neighbour_bits |= (1 << bit_index);
						}
					}
					bit_index++;
				}
			}
		}

		uint rgb = imageLoad(src_albedo, pos).r;
		uint facing = imageLoad(src_facing, pos).r;

		store_positions[index].albedo = rgb >> 1; //store as it comes (555) to avoid precision loss (and move away the alpha bit)
		store_positions[index].albedo |= (facing & 0x3F) << 15; // store facing in bits 15-21

		store_positions[index].albedo |= neighbour_bits << 21; //store lower 11 bits of neighbors with remaining albedo
		store_positions[index].position |= (neighbour_bits >> 11) << 21; //store 11 bits more of neighbors with position

		store_positions[index].light = imageLoad(src_light, pos).r;
		store_positions[index].light_aniso = imageLoad(src_light_aniso, pos).r;
		//add neighbors
		store_positions[index].light |= (neighbour_bits >> 22) << 30; //store 2 bits more of neighbors with light
		store_positions[index].light_aniso |= (neighbour_bits >> 24) << 30; //store 2 bits more of neighbors with aniso
	}

	groupMemoryBarrier();
	barrier();

	// global increment only once per group, to reduce pressure

	if (local == ivec3(0) && store_position_count > 0) {
		store_from_index = atomicAdd(dispatch_data.total_count, store_position_count);
		uint group_count = (store_from_index + store_position_count - 1) / 64 + 1;
		atomicMax(dispatch_data.x, group_count);
	}

	groupMemoryBarrier();
	barrier();

	uint read_index = uint(local.z * 4 * 4 + local.y * 4 + local.x);
	uint write_index = store_from_index + read_index;

	if (read_index < store_position_count) {
		dst_process_voxels.data[write_index] = store_positions[read_index];
	}

	if (pos == ivec3(0)) {
		//this thread clears y and z
		dispatch_data.y = 1;
		dispatch_data.z = 1;
	}
#endif
}
