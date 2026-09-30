#[compute]

#version 450

#VERSION_DEFINES

// GPU culling and level of detail selection of the instances of a foliage type
// (LandscapeFoliage3D with gpu_indirect).
//
// The instances are stored per cell in slots of the source buffer: unused instances of a slot
// (holes) have a zero transform and are skipped.
//
// MODE_CLASSIFY: one invocation per instance, once per frame (LOD camera). Chooses its level of
// detail from its distance to the camera (with the same hysteresis and per-instance cull distance
// as the CPU path) and stores it in the state buffer (kept for the hysteresis of the next run).
//
// MODE_EMIT: one dispatch per level of detail and list. The instances of the level are appended to
// the instance buffer of the indirect MultiMesh of that level and list:
// - shadow list (once per frame): every instance within the cull distance,
// - main list (for every camera drawn): the instances whose bounding sphere is in the view frustum
//   and not hidden in the occlusion buffer of the viewport (HZB, the same test as the one of the
//   renderer, see RendererSceneOcclusionCull::HZBuffer).
//
// MODE_COMMAND: writes the instance count into the indirect draw command of every surface.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

#define LOD_CULLED 255u
#define LOD_UNKNOWN 254u
#define LIST_COUNTERS 16u // Per list: count per level, overflow, culled by the frustum, by occlusion.
#define COUNTER_OVERFLOW 8u
#define COUNTER_FRUSTUM_CULLED 9u
#define COUNTER_OCCLUSION_CULLED 10u
#define COMMAND_STRIDE 5u
#define VIEW_FRUSTUM 1u
#define VIEW_OCCLUSION 2u
#define VIEW_ORTHOGONAL 4u
#define MAX_HZB_MIPS 16
#define MAX_HZB_SAMPLES 128

layout(set = 0, binding = 0, std140) uniform Params {
	vec4 camera; // xyz = LOD camera position in the landscape space.
	vec4 lod_starts[2]; // Start distance of each level of detail.
	vec4 cull; // x = cull distance (0 = never), y = closest cull distance of an instance, z = half transition.
	vec4 sphere; // xyz = center of the bounding sphere of the meshes, w = radius.
	uvec4 counts; // x = instance count, y = level count.
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
	if (params.cull.x > 0.0) {
		// Each instance has its own cull distance, between the closest one and the cull distance.
		float cull = params.cull.x - (params.cull.x - params.cull.y) * random;
		float margin = current == LOD_CULLED ? -half_transition : (known ? half_transition : 0.0);
		if (distance_to_camera > cull + margin) {
			state.data[index] = LOD_CULLED;
			return;
		}
	}
	uint lod = 0u;
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
	state.data[index] = lod;
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

// The camera drawn (main list), in the world space.
layout(set = 0, binding = 6, std140) uniform View {
	vec4 planes[6]; // Frustum planes (xyz = normal, w = d). Normals point outside.
	vec4 space[3]; // Landscape space to world space (rows of a 3 x 4 matrix).
	vec4 view[3]; // World space to view space (rows of a 3 x 4 matrix).
	mat4 projection; // View space to clip space.
	vec4 camera; // xyz = camera position, w = near plane.
	vec4 hzb; // xy = size of the first mip of the occlusion buffer, z = mip count, w = scale of the landscape space.
	uvec4 flags; // x = VIEW_* flags.
	uvec4 hzb_mips[MAX_HZB_MIPS]; // x = offset of the mip in the occlusion buffer, y = width, z = height.
}
view;

// Occlusion buffer of the viewport: distance to the camera of the farthest surface seen in each texel.
layout(set = 0, binding = 7, std430) restrict readonly buffer OcclusionBuffer {
	float data[];
}
hzb;

shared uint group_frustum_culled;
shared uint group_occlusion_culled;

// Same test as RendererSceneOcclusionCull::HZBuffer::_is_occluded(), for a bounding sphere.
bool is_occluded(vec3 p_center, float p_radius) {
	float center_distance = distance(p_center, view.camera.xyz);
	if (center_distance <= p_radius) {
		return false; // The camera is inside.
	}
	vec3 center_view = vec3(dot(view.view[0].xyz, p_center) + view.view[0].w, dot(view.view[1].xyz, p_center) + view.view[1].w, dot(view.view[2].xyz, p_center) + view.view[2].w);
	float nearest = -center_view.z - p_radius;
	if (nearest < view.camera.w) {
		return false; // Crosses the near plane.
	}
	float min_depth = (view.flags.x & VIEW_ORTHOGONAL) != 0u ? nearest : center_distance - p_radius;

	// Screen rect of the box around the sphere (in front of the camera).
	vec2 rect_min = vec2(1e30);
	vec2 rect_max = vec2(-1e30);
	for (uint i = 0u; i < 8u; i++) {
		vec3 corner = center_view + vec3((i & 1u) != 0u ? p_radius : -p_radius, (i & 2u) != 0u ? p_radius : -p_radius, (i & 4u) != 0u ? p_radius : -p_radius);
		vec4 clip = view.projection * vec4(corner, 1.0);
		vec2 ndc = clip.xy / clip.w;
		rect_min = min(rect_min, ndc);
		rect_max = max(rect_max, ndc);
	}
	rect_min = clamp(rect_min * 0.5 + 0.5, 0.0, 1.0);
	rect_max = clamp(rect_max * 0.5 + 0.5, 0.0, 1.0);

	vec2 extent = (rect_max - rect_min) * view.hzb.xy;
	int mip_count = int(view.hzb.z);
	int mip = clamp(int(ceil(log2(max(max(extent.x, extent.y), 1.0)))), 0, mip_count - 1);
	// From the mip where the rect covers a few texels, down to finer mips while something may be visible.
	int samples = 0;
	for (; mip >= 0; mip--) {
		uvec4 layout_mip = view.hzb_mips[mip];
		int w = int(layout_mip.y);
		int h = int(layout_mip.z);
		int min_x = clamp(int(rect_min.x * float(w) - 1.0), 0, w - 1);
		int max_x = clamp(int(rect_max.x * float(w) + 1.0), 0, w - 1);
		int min_y = clamp(int(rect_min.y * float(h) - 1.0), 0, h - 1);
		int max_y = clamp(int(rect_max.y * float(h) + 1.0), 0, h - 1);
		samples += (max_x - min_x + 1) * (max_y - min_y + 1);
		if (samples > MAX_HZB_SAMPLES) {
			return false;
		}
		bool visible = false;
		for (int y = min_y; y <= max_y && !visible; y++) {
			uint row = layout_mip.x + uint(y * w);
			for (int x = min_x; x <= max_x; x++) {
				if (hzb.data[row + uint(x)] > min_depth) {
					visible = true;
					break;
				}
			}
		}
		if (!visible) {
			return true;
		}
	}
	return false;
}

// Whether the instance is drawn by the camera of the view (main list).
bool is_in_view(vec4 p_r0, vec4 p_r1, vec4 p_r2) {
	if ((view.flags.x & (VIEW_FRUSTUM | VIEW_OCCLUSION)) == 0u) {
		return true;
	}
	vec3 sphere = params.sphere.xyz;
	vec3 local_center = vec3(dot(p_r0.xyz, sphere) + p_r0.w, dot(p_r1.xyz, sphere) + p_r1.w, dot(p_r2.xyz, sphere) + p_r2.w);
	float scale = sqrt(max(max(dot(vec3(p_r0.x, p_r1.x, p_r2.x), vec3(p_r0.x, p_r1.x, p_r2.x)), dot(vec3(p_r0.y, p_r1.y, p_r2.y), vec3(p_r0.y, p_r1.y, p_r2.y))), dot(vec3(p_r0.z, p_r1.z, p_r2.z), vec3(p_r0.z, p_r1.z, p_r2.z))));
	vec3 center = vec3(dot(view.space[0].xyz, local_center) + view.space[0].w, dot(view.space[1].xyz, local_center) + view.space[1].w, dot(view.space[2].xyz, local_center) + view.space[2].w);
	float radius = params.sphere.w * scale * view.hzb.w;
	if ((view.flags.x & VIEW_FRUSTUM) != 0u) {
		for (uint i = 0u; i < 6u; i++) {
			vec4 plane = view.planes[i];
			if (dot(plane.xyz, center) - plane.w > radius) {
				atomicAdd(group_frustum_culled, 1u);
				return false;
			}
		}
	}
	if ((view.flags.x & VIEW_OCCLUSION) != 0u && is_occluded(center, radius)) {
		atomicAdd(group_occlusion_culled, 1u);
		return false;
	}
	return true;
}

void emit(uint p_index) {
	if ((state.data[p_index] & 0xFFu) != pc.lod) {
		return;
	}
	vec4 r0 = source.data[p_index * 4u + 0u];
	vec4 r1 = source.data[p_index * 4u + 1u];
	vec4 r2 = source.data[p_index * 4u + 2u];
	if (pc.list == 0u && !is_in_view(r0, r1, r2)) {
		return;
	}
	uint base = pc.list * LIST_COUNTERS;
	uint slot = atomicAdd(counters.data[base + pc.lod], 1u);
	if (slot >= pc.capacity) {
		atomicOr(counters.data[base + COUNTER_OVERFLOW], 1u);
		return;
	}
	// Transform (3 x 4, row-major) and custom data (random value, cell, level).
	instances.data[slot * 4u + 0u] = r0;
	instances.data[slot * 4u + 1u] = r1;
	instances.data[slot * 4u + 2u] = r2;
	vec4 extra = source.data[p_index * 4u + 3u];
	instances.data[slot * 4u + 3u] = vec4(extra.x, extra.y, float(pc.lod), 0.0);
}

void main() {
	if (gl_LocalInvocationIndex == 0u) {
		group_frustum_culled = 0u;
		group_occlusion_culled = 0u;
	}
	barrier();
	uint index = instance_index();
	if (index < params.counts.x) {
		emit(index);
	}
	barrier();
	// One atomic per group for the statistics.
	if (gl_LocalInvocationIndex == 0u && pc.list == 0u) {
		if (group_frustum_culled > 0u) {
			atomicAdd(counters.data[COUNTER_FRUSTUM_CULLED], group_frustum_culled);
		}
		if (group_occlusion_culled > 0u) {
			atomicAdd(counters.data[COUNTER_OCCLUSION_CULLED], group_occlusion_culled);
		}
	}
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
	uint count = min(counters.data[pc.list * LIST_COUNTERS + pc.lod], pc.capacity);
	for (uint s = 0u; s < pc.surfaces; s++) {
		command.data[s * COMMAND_STRIDE + 1u] = count;
	}
}

#endif // MODE_COMMAND
