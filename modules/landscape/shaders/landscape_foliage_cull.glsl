#[compute]

#version 450

#VERSION_DEFINES

// GPU culling and level of detail selection of the instances of a foliage type
// (LandscapeFoliage3D with gpu_indirect).
//
// The instances are stored per cell in slots of the source buffer: unused instances of a slot
// (holes) have a zero transform and are skipped.
//
// MODE_CLASSIFY: one invocation per instance. Chooses its level of detail from its distance to the
// camera (with the same hysteresis and per-instance cull distance as the CPU path), tests its
// bounding sphere against the frustum, and stores both in the state buffer (the level is kept for
// the hysteresis of the next run).
//
// MODE_EMIT: one dispatch per level of detail and list (main: visible instances, shadow: every
// instance within the cull distance). The instances of the level are appended to the instance
// buffer of the indirect MultiMesh of that level and list.
//
// MODE_COMMAND: writes the instance count into the indirect draw command of every surface.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

#define LOD_CULLED 255u
#define LOD_UNKNOWN 254u
#define STATE_VISIBLE 256u
#define COUNTER_OVERFLOW 16u
#define COMMAND_STRIDE 5u

layout(set = 0, binding = 0, std140) uniform Params {
	vec4 planes[6]; // Frustum planes in the landscape space (xyz = normal, w = d). Normals point outside.
	vec4 camera; // xyz = camera position in the landscape space.
	vec4 lod_starts[2]; // Start distance of each level of detail.
	vec4 cull; // x = cull distance (0 = never), y = closest cull distance of an instance, z = half transition.
	vec4 sphere; // xyz = center of the bounding sphere of the meshes, w = radius.
	uvec4 counts; // x = instance count, y = level count, w = flags (1: frustum culling).
}
params;

layout(push_constant, std430) uniform PushConstant {
	uint lod;
	uint list;
	uint surfaces;
	uint capacity;
}
pc;

float lod_start(uint p_lod) {
	return params.lod_starts[p_lod >> 2u][p_lod & 3u];
}

// 2D dispatches (more than 65535 groups of instances).
uint instance_index() {
	return gl_GlobalInvocationID.y * gl_NumWorkGroups.x * gl_WorkGroupSize.x + gl_GlobalInvocationID.x;
}

#ifdef MODE_CLASSIFY

layout(set = 0, binding = 1, std430) restrict readonly buffer Source {
	vec4 data[];
}
source;

layout(set = 0, binding = 2, std430) restrict buffer State {
	uint data[];
}
state;

void main() {
	uint index = instance_index();
	if (index >= params.counts.x) {
		return;
	}
	vec4 extra = source.data[index * 4u + 3u];
	if (extra.w == 0.0) {
		state.data[index] = LOD_CULLED; // Hole.
		return;
	}
	vec4 r0 = source.data[index * 4u + 0u];
	vec4 r1 = source.data[index * 4u + 1u];
	vec4 r2 = source.data[index * 4u + 2u];
	float random = extra.x;
	vec3 position = vec3(r0.w, r1.w, r2.w);
	float distance_to_camera = distance(params.camera.xyz, position);

	uint current = state.data[index] & 0xFFu;
	bool known = current < 8u;
	float half_transition = params.cull.z;
	uint lod = 0u;
	bool culled = false;
	if (params.cull.x > 0.0) {
		// Each instance has its own cull distance, between the closest one and the cull distance.
		float cull = params.cull.x - (params.cull.x - params.cull.y) * random;
		float margin = current == LOD_CULLED ? -half_transition : (known ? half_transition : 0.0);
		culled = distance_to_camera > cull + margin;
	}
	if (!culled) {
		for (uint k = params.counts.y - 1u; k > 0u; k--) {
			float start = lod_start(k);
			if (known) {
				// Hysteresis: the boundary moves away from the current level.
				start += current >= k ? -half_transition : half_transition;
			}
			if (distance_to_camera >= start) {
				lod = k;
				break;
			}
		}
	}

	bool visible = !culled;
	if (visible && (params.counts.w & 1u) != 0u) {
		vec3 center = vec3(dot(r0.xyz, params.sphere.xyz) + r0.w, dot(r1.xyz, params.sphere.xyz) + r1.w, dot(r2.xyz, params.sphere.xyz) + r2.w);
		float scale = sqrt(max(max(dot(vec3(r0.x, r1.x, r2.x), vec3(r0.x, r1.x, r2.x)), dot(vec3(r0.y, r1.y, r2.y), vec3(r0.y, r1.y, r2.y))), dot(vec3(r0.z, r1.z, r2.z), vec3(r0.z, r1.z, r2.z))));
		float radius = params.sphere.w * scale;
		for (uint i = 0u; i < 6u; i++) {
			vec4 plane = params.planes[i];
			if (dot(plane.xyz, center) - plane.w > radius) {
				visible = false;
				break;
			}
		}
	}
	state.data[index] = (culled ? LOD_CULLED : lod) | (visible ? STATE_VISIBLE : 0u);
}

#endif // MODE_CLASSIFY

#ifdef MODE_EMIT

layout(set = 0, binding = 1, std430) restrict readonly buffer Source {
	vec4 data[];
}
source;

layout(set = 0, binding = 2, std430) restrict readonly buffer State {
	uint data[];
}
state;

layout(set = 0, binding = 3, std430) restrict buffer Counters {
	uint data[];
}
counters;

layout(set = 0, binding = 4, std430) restrict writeonly buffer Instances {
	vec4 data[];
}
instances;

void main() {
	uint index = instance_index();
	if (index >= params.counts.x) {
		return;
	}
	uint s = state.data[index];
	if ((s & 0xFFu) != pc.lod || (pc.list == 0u && (s & STATE_VISIBLE) == 0u)) {
		return;
	}
	uint slot = atomicAdd(counters.data[pc.list * 8u + pc.lod], 1u);
	if (slot >= pc.capacity) {
		atomicOr(counters.data[COUNTER_OVERFLOW], 1u);
		return;
	}
	// Transform (3 x 4, row-major) and custom data (random value, cell, level).
	instances.data[slot * 4u + 0u] = source.data[index * 4u + 0u];
	instances.data[slot * 4u + 1u] = source.data[index * 4u + 1u];
	instances.data[slot * 4u + 2u] = source.data[index * 4u + 2u];
	vec4 extra = source.data[index * 4u + 3u];
	instances.data[slot * 4u + 3u] = vec4(extra.x, extra.y, float(pc.lod), 0.0);
}

#endif // MODE_EMIT

#ifdef MODE_COMMAND

layout(set = 0, binding = 3, std430) restrict readonly buffer Counters {
	uint data[];
}
counters;

layout(set = 0, binding = 5, std430) restrict writeonly buffer Command {
	uint data[];
}
command;

void main() {
	if (gl_GlobalInvocationID.x != 0u) {
		return;
	}
	uint count = min(counters.data[pc.list * 8u + pc.lod], pc.capacity);
	for (uint s = 0u; s < pc.surfaces; s++) {
		command.data[s * COMMAND_STRIDE + 1u] = count;
	}
}

#endif // MODE_COMMAND
