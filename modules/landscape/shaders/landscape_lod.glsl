#[compute]

#version 450

#VERSION_DEFINES

// GPU quadtree traversal for Landscape3D.
//
// MODE_TRAVERSE runs once per quadtree level (indirect dispatch). Every node of the
// current level is frustum culled, its screen-space error (SSE) is evaluated and the
// node is either split (four children appended to the next level list) or emitted as a
// patch instance straight into the MultiMesh buffer used by the indirect draw.
// A node is only split when the pages holding the heights of its children are resident
// (streamed in), so every emitted patch can read exact heights.
//
// MODE_STITCH runs after the traversal: it finds the LOD of the neighbors of every
// emitted patch (using the split bits written during traversal) so that the vertex
// shader can stitch edges against coarser neighbors, and it writes the final instance
// count into the indirect draw command.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

#define COUNTER_EMITTED 20
#define COUNTER_OVERFLOW 21
#define INVALID_NODE 0xFFFFFFFFu

layout(set = 0, binding = 0, std140) uniform Params {
	vec4 planes[6]; // Frustum planes in landscape local space (xyz = normal, w = d). Normals point outside.
	vec4 camera; // xyz = LOD camera position in local space, w = projection factor (pixels per unit at distance 1).
	vec4 lod; // x = pixel error threshold, y = micro displacement amplitude, z = cull margin, w = 1.0 if orthographic.
	vec4 unit; // x = meters per grid unit, y = max distance (0 = unlimited), zw = unused.
	uvec4 grid; // x = patch quads, y = heightmap levels (finest heightmap level), z = max level, w = micro levels.
	uvec4 limits; // x = max grid coordinate on X, y = max grid coordinate on Z, z = node list capacity per level, w = instance capacity.
	uvec4 flags; // x = frustum culling enabled.
	uvec4 bounds_offsets[4]; // Node offset of each level in the bounds buffer.
	uvec4 split_offsets[4]; // Word offset of each level in the split bit buffer.
	uvec4 page_rows[4]; // First row of each mip level in the page table.
	uvec4 page_tiles[4]; // Tile count of each mip level (x | y << 16).
}
params;

uint get_bounds_offset(uint p_level) {
	return params.bounds_offsets[p_level >> 2][p_level & 3u];
}

uint get_split_offset(uint p_level) {
	return params.split_offsets[p_level >> 2][p_level & 3u];
}

#ifdef MODE_TRAVERSE

layout(set = 0, binding = 1, std430) restrict readonly buffer Bounds {
	vec4 data[];
}
bounds;

layout(set = 0, binding = 2, std430) restrict buffer NodeLists {
	uint data[];
}
node_lists;

layout(set = 0, binding = 3, std430) restrict buffer Counters {
	uint data[];
}
counters;

layout(set = 0, binding = 4, std430) restrict buffer ArgsOut {
	uint data[];
}
args_out;

layout(set = 0, binding = 5, std430) restrict buffer SplitBits {
	uint data[];
}
split_bits;

layout(set = 0, binding = 6, std430) restrict writeonly buffer Instances {
	vec4 data[];
}
instances;

layout(set = 0, binding = 8, r32f) uniform restrict readonly image2D page_table;

#define PAGE_SHIFT 7u
#define PAGE_SLOT_BITS 12

// True if the page of the given mip level holding the texel (in texels of that mip) is resident.
bool is_page_resident(uint p_mip, uvec2 p_texel) {
	uvec2 tile = p_texel >> PAGE_SHIFT;
	uint tiles = params.page_tiles[p_mip >> 2u][p_mip & 3u];
	if (tile.x >= (tiles & 0xFFFFu) || tile.y >= (tiles >> 16u)) {
		return true; // Outside of the landscape, such nodes are skipped anyway.
	}
	uint row = params.page_rows[p_mip >> 2u][p_mip & 3u] + tile.y;
	int entry = int(imageLoad(page_table, ivec2(tile.x, row)).r);
	return entry >= 0 && (entry >> PAGE_SLOT_BITS) == int(p_mip);
}

bool children_resident(uvec2 p_node, uint p_level) {
	uint heightmap_level = params.grid.y;
	uint child_level = p_level + 1u;
	if (child_level > heightmap_level) {
		return true; // Micro levels reuse the mip 0 page of their heightmap level ancestor.
	}
	uint mip = heightmap_level - child_level;
	for (uint c = 0u; c < 4u; c++) {
		uvec2 child = p_node * 2u + uvec2(c & 1u, c >> 1u);
		if (!is_page_resident(mip, child * params.grid.x)) {
			return false;
		}
	}
	return true;
}

layout(push_constant, std430) uniform PushConstant {
	uint level;
	uint pad0;
	uint pad1;
	uint pad2;
}
pc;

void emit_patch(uvec2 p_origin, uint p_level) {
	uint index = atomicAdd(counters.data[COUNTER_EMITTED], 1u);
	if (index >= params.limits.w) {
		atomicOr(counters.data[COUNTER_OVERFLOW], 2u);
		return;
	}
	uint base = index * 4u;
	// Identity instance transform, the patch placement lives in the custom data.
	instances.data[base + 0u] = vec4(1.0, 0.0, 0.0, 0.0);
	instances.data[base + 1u] = vec4(0.0, 1.0, 0.0, 0.0);
	instances.data[base + 2u] = vec4(0.0, 0.0, 1.0, 0.0);
	instances.data[base + 3u] = vec4(float(p_origin.x), float(p_origin.y), float(p_level), 0.0);
}

void main() {
	uint level = pc.level;
	uint capacity = params.limits.z;
	uint count = min(counters.data[level], capacity);
	uint index = gl_GlobalInvocationID.x;
	if (index >= count) {
		return;
	}

	uint packed_node = node_lists.data[level * capacity + index];
	if (packed_node == INVALID_NODE) {
		return;
	}
	uvec2 node = uvec2(packed_node & 0xFFFFu, packed_node >> 16u);

	uint patch_quads = params.grid.x;
	uint heightmap_level = params.grid.y;
	uint max_level = params.grid.z;
	uint node_units = patch_quads << (max_level - level);
	uvec2 origin = node * node_units;

	// Bounds (micro levels reuse the bounds of their finest heightmap ancestor).
	uint bounds_level = min(level, heightmap_level);
	uvec2 bounds_node = node >> (level - bounds_level);
	vec4 node_bounds = bounds.data[get_bounds_offset(bounds_level) + bounds_node.y * (1u << bounds_level) + bounds_node.x];
	if (node_bounds.x > node_bounds.y) {
		return; // Outside of the landscape.
	}

	float amplitude = params.lod.y;
	float error = node_bounds.z;
	if (level >= heightmap_level) {
		// Micro detail: the geometric error is the displacement that can't be represented yet.
		error = amplitude * exp2(-float(level - heightmap_level));
	} else {
		error = max(error, amplitude);
	}

	float unit = params.unit.x;
	float expand = amplitude * 0.5 + params.lod.z;
	vec3 aabb_min = vec3(float(origin.x) * unit, node_bounds.x, float(origin.y) * unit) - vec3(expand);
	vec3 aabb_max = vec3(float(min(origin.x + node_units, params.limits.x)) * unit, node_bounds.y, float(min(origin.y + node_units, params.limits.y)) * unit) + vec3(expand);

	if (params.flags.x != 0u) {
		for (uint i = 0u; i < 6u; i++) {
			vec4 plane = params.planes[i];
			vec3 nearest = vec3(plane.x > 0.0 ? aabb_min.x : aabb_max.x, plane.y > 0.0 ? aabb_min.y : aabb_max.y, plane.z > 0.0 ? aabb_min.z : aabb_max.z);
			if (dot(plane.xyz, nearest) - plane.w > 0.0) {
				return; // Culled.
			}
		}
	}

	vec3 camera = params.camera.xyz;
	float dist = distance(camera, clamp(camera, aabb_min, aabb_max));
	if (params.unit.y > 0.0 && dist > params.unit.y) {
		return; // Beyond the maximum distance.
	}

	float sse = params.lod.w > 0.5 ? error * params.camera.w : error * params.camera.w / max(dist, 1e-4);
	if (level < max_level && sse > params.lod.x && children_resident(node, level)) {
		uint base = atomicAdd(counters.data[level + 1u], 4u);
		uint list_offset = (level + 1u) * capacity;
		if (base + 4u <= capacity) {
			uint bit = node.y * (1u << level) + node.x;
			atomicOr(split_bits.data[get_split_offset(level) + (bit >> 5u)], 1u << (bit & 31u));

			uint child_units = node_units >> 1u;
			for (uint c = 0u; c < 4u; c++) {
				uvec2 child = node * 2u + uvec2(c & 1u, c >> 1u);
				uvec2 child_origin = child * child_units;
				bool valid = child_origin.x < params.limits.x && child_origin.y < params.limits.y;
				node_lists.data[list_offset + base + c] = valid ? (child.x | (child.y << 16u)) : INVALID_NODE;
			}
			atomicMax(args_out.data[(level + 1u) * 4u], (base + 4u + 63u) / 64u);
			return;
		}

		// Out of node list capacity: invalidate the reserved slots and draw this node as is.
		for (uint c = 0u; c < 4u; c++) {
			if (base + c < capacity) {
				node_lists.data[list_offset + base + c] = INVALID_NODE;
			}
		}
		atomicOr(counters.data[COUNTER_OVERFLOW], 1u);
	}

	emit_patch(origin, level);
}

#endif // MODE_TRAVERSE

#ifdef MODE_STITCH

layout(set = 0, binding = 3, std430) restrict readonly buffer Counters {
	uint data[];
}
counters;

layout(set = 0, binding = 5, std430) restrict readonly buffer SplitBits {
	uint data[];
}
split_bits;

layout(set = 0, binding = 6, std430) restrict buffer Instances {
	vec4 data[];
}
instances;

layout(set = 0, binding = 7, std430) restrict writeonly buffer Command {
	uint data[];
}
command;

// Returns the level of the leaf covering the given grid position (limited to p_max_level).
uint find_leaf_level(uvec2 p_pos, uint p_max_level) {
	uint patch_quads = params.grid.x;
	uint max_level = params.grid.z;
	for (uint l = 0u; l < p_max_level; l++) {
		uvec2 node = p_pos / (patch_quads << (max_level - l));
		uint bit = node.y * (1u << l) + node.x;
		if ((split_bits.data[get_split_offset(l) + (bit >> 5u)] & (1u << (bit & 31u))) == 0u) {
			return l;
		}
	}
	return p_max_level;
}

void main() {
	uint emitted = min(counters.data[COUNTER_EMITTED], params.limits.w);
	uint index = gl_GlobalInvocationID.x;
	if (index == 0u) {
		command.data[1] = emitted; // Instance count of the indirect draw.
	}
	if (index >= emitted) {
		return;
	}

	vec4 custom = instances.data[index * 4u + 3u];
	ivec2 origin = ivec2(custom.xy);
	uint level = uint(custom.z);
	int node_units = int(params.grid.x << (params.grid.z - level));
	int half_units = node_units / 2;
	ivec2 limits = ivec2(params.limits.xy);

	// Sample points just outside the middle of each edge: -X, +X, -Z, +Z.
	ivec2 points[4] = ivec2[](
			origin + ivec2(-1, half_units),
			origin + ivec2(node_units, half_units),
			origin + ivec2(half_units, -1),
			origin + ivec2(half_units, node_units));

	uint edges = 0u;
	for (uint e = 0u; e < 4u; e++) {
		ivec2 p = points[e];
		if (any(lessThan(p, ivec2(0))) || any(greaterThanEqual(p, limits))) {
			continue; // No neighbor outside of the landscape.
		}
		uint neighbor_level = find_leaf_level(uvec2(p), level);
		uint delta = min(level - neighbor_level, 15u);
		edges |= delta << (e * 4u);
	}
	instances.data[index * 4u + 3u].w = float(edges);
}

#endif // MODE_STITCH
