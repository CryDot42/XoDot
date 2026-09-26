#[compute]

#version 450

#VERSION_DEFINES

#include "../oct_inc.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#define MAX_CASCADES 8

layout(set = 0, binding = 1) uniform texture3D sdf_cascades[MAX_CASCADES];
layout(set = 0, binding = 2) uniform texture3D light_cascades[MAX_CASCADES];
layout(set = 0, binding = 3) uniform texture3D aniso0_cascades[MAX_CASCADES];
layout(set = 0, binding = 4) uniform texture3D aniso1_cascades[MAX_CASCADES];

layout(set = 0, binding = 6) uniform sampler linear_sampler;

struct CascadeData {
	vec3 offset; //offset of (0,0,0) in world coordinates
	float to_cell; // 1/bounds * grid_size
	ivec3 probe_world_offset;
	uint pad;
	vec4 pad2;
};

layout(set = 0, binding = 7, std140) uniform Cascades {
	CascadeData data[MAX_CASCADES];
}
cascades;

layout(r32ui, set = 0, binding = 8) uniform restrict uimage2DArray lightprobe_texture_data;

// Probe history. Each probe owns a column of SH_SIZE texels, one per spherical harmonic
// coefficient, at (x, y * SH_SIZE + i). The history texture keeps the last history_size frames of
// every coefficient, one frame per layer, and the average texture their sum (fixed point, see
// HISTORY_BITS) in rgb. The alpha channels of the first few average texels hold the probe's
// history state instead, as float bits (see STATE_*).
layout(rgba16i, set = 0, binding = 9) uniform restrict iimage2DArray lightprobe_history_texture;
layout(rgba32i, set = 0, binding = 10) uniform restrict iimage2D lightprobe_average_texture;

//used for scrolling
layout(rgba16i, set = 0, binding = 11) uniform restrict iimage2DArray lightprobe_history_scroll_texture;
layout(rgba32i, set = 0, binding = 12) uniform restrict iimage2D lightprobe_average_scroll_texture;

layout(rgba32i, set = 0, binding = 13) uniform restrict iimage2D lightprobe_average_parent_texture;

layout(rgba16f, set = 0, binding = 14) uniform restrict writeonly image2DArray lightprobe_ambient_texture;

// Where each probe was placed (see MODE_PROBE_PLACEMENT in sdfgi_preprocess.glsl), one layer per
// cascade: xyz its offset from the grid in voxels, w whether it is usable at all.
layout(set = 0, binding = 15) uniform texture2DArray probe_state_texture;

// Which coefficient texel's alpha holds each piece of a probe's history state.
#define STATE_AGE 0 // How many of the history frames are the probe's own, up to history_size.
#define STATE_SUSPECT 1 // Consecutive frames whose light disagreed with the history's.
#define STATE_PLACEMENT 2 // Where the probe was placed when its history was traced, to notice it moving.
#define STATE_CHANGE_RATE 3 // How much its light has been changing lately, from one history turn to the next.

// A probe that starts from a guess (seeded from the cascade above when it scrolls in) counts
// that guess as this many history frames: enough to hide its first noisy traces (new probes
// scroll in all the time while the camera moves, and should not sparkle at the edges of the
// cascades), few enough that its own light takes over well before a whole convergence period.
#define PRIOR_AGE 6

// Marks, in the alpha channel of a history texel, a frame that is no good for telling whether
// the light changed since (see the adaptive history in MODE_PROCESS): a seeded one, which traced
// nothing, and those traced while the probe was settling, in its first ADAPT_SETTLE_FRAMES
// frames of history or right after it dropped the rest. What those saw was often still on its
// way to what the probe settled at (lights are updated over several frames, and every bounce
// takes a probe update of its own), so comparing against them would drop the history again
// for no reason, throwing the probe back to a handful of noisy frames.
#define HISTORY_FLAG_UNSETTLED 1

// Adaptive history. A probe whose light differs from what the very same rays saw one history
// turn earlier by more than ADAPT_RELATIVE, and by ADAPT_RATE_MARGIN times as much as it has
// lately been changing (and by more than a couple of fixed point steps, ADAPT_ABSOLUTE),
// ADAPT_CONFIRM_FRAMES frames in a row, has seen the light change suddenly: it drops as much of
// its history as the change calls for (see MODE_PROCESS) and catches up. A single differing
// frame is averaged in as usual, and so is light that keeps changing at a steady pace (a light
// moving, the sun turning), which the probe follows the way it would without this, smoothly, a
// little behind.
#define ADAPT_RELATIVE 0.15
#define ADAPT_RATE_MARGIN 3.0
#define ADAPT_ABSOLUTE (2.0 / float(1 << HISTORY_BITS))
#define ADAPT_CONFIRM_FRAMES 3
#define ADAPT_SETTLE_FRAMES 4
// How many frames the pace of change (STATE_CHANGE_RATE) is averaged over.
#define ADAPT_RATE_FRAMES 8.0

#ifdef USE_RADIANCE_OCTMAP_ARRAY
layout(set = 1, binding = 0) uniform texture2DArray sky_irradiance;
#else
layout(set = 1, binding = 0) uniform texture2D sky_irradiance;
#endif
layout(set = 1, binding = 1) uniform sampler linear_sampler_mipmaps;

#define HISTORY_BITS 10

#define SKY_FLAGS_MODE_COLOR 0x01
#define SKY_FLAGS_MODE_SKY 0x02
#define SKY_FLAGS_ORIENTATION_SIGN 0x04

layout(push_constant, std430) uniform Params {
	vec3 grid_size;
	uint max_cascades;

	uint probe_axis_size;
	uint cascade;
	uint history_index;
	uint history_size;

	uint ray_count;
	float ray_bias;
	ivec2 image_size;

	ivec3 world_offset;
	uint sky_flags;

	ivec3 scroll;
	float sky_energy;

	vec3 sky_color_or_orientation;
	float y_mult;

	vec2 sky_irradiance_border_size;
	bool store_ambient_texture;
	uint flags;
}
params;

// MODE_SCROLL: probes that cannot be seeded from a parent cascade start from zero instead of
// keeping what their texel held, which after a full rebuild belongs somewhere else entirely.
#define INTEGRATE_FLAG_RESET 1
// MODE_PROCESS: let probes drop their history when the light they see changes.
#define INTEGRATE_FLAG_ADAPTIVE 2

const float PI = 3.14159265f;
const float GOLDEN_ANGLE = PI * (3.0 - sqrt(5.0));

// Point p_index of a p_count point spherical Fibonacci set, turned by p_offset around the pole.
// z falls linearly with the index, so the set is equal-area, and so is any strided subset of it:
// each frame traces indices history_index + i * history_size, which spread over the whole sphere.
// (The Vogel disk this replaces was bent onto a hemisphere, which crowded the samples towards
// its rim, and picked the hemisphere from the index parity, so with an even history size every
// ray of a frame went into the same half of the sphere, alternating from frame to frame.)
vec3 spherical_fibonacci(uint p_index, uint p_count, float p_offset) {
	float z = 1.0 - (2.0 * float(p_index) + 1.0) / float(p_count);
	float r = sqrt(max(0.0, 1.0 - z * z));
	float phi = float(p_index) * GOLDEN_ANGLE + p_offset;
	return vec3(r * cos(phi), r * sin(phi), z);
}

uvec3 hash3(uvec3 x) {
	x = ((x >> 16) ^ x) * 0x45d9f3b;
	x = ((x >> 16) ^ x) * 0x45d9f3b;
	x = (x >> 16) ^ x;
	return x;
}

float hashf3(vec3 co) {
	return fract(sin(dot(co, vec3(12.9898, 78.233, 137.13451))) * 43758.5453);
}

float get_luminance(vec3 p_color) {
	return dot(p_color, vec3(0.2126, 0.7152, 0.0722));
}

// The history state of the probe whose first coefficient texel is at p_probe_pos.
float get_probe_state(ivec2 p_probe_pos, int p_state) {
	return intBitsToFloat(imageLoad(lightprobe_average_texture, p_probe_pos + ivec2(0, p_state)).a);
}

// The probe's average: the sum of its own history frames over how many there are.
vec3 get_probe_coefficient(ivec4 p_sum, float p_age) {
	return p_age > 0.0 ? vec3(p_sum.rgb) / (p_age * float(1 << HISTORY_BITS)) : vec3(0.0);
}

vec3 octahedron_encode(vec2 f) {
	// https://twitter.com/Stubbesaurus/status/937994790553227264
	f = f * 2.0 - 1.0;
	vec3 n = vec3(f.x, f.y, 1.0f - abs(f.x) - abs(f.y));
	float t = clamp(-n.z, 0.0, 1.0);
	n.x += n.x >= 0 ? -t : t;
	n.y += n.y >= 0 ? -t : t;
	return normalize(n);
}

uint rgbe_encode(vec3 color) {
	const float pow2to9 = 512.0f;
	const float B = 15.0f;
	const float N = 9.0f;
	const float LN2 = 0.6931471805599453094172321215;

	float cRed = clamp(color.r, 0.0, 65408.0);
	float cGreen = clamp(color.g, 0.0, 65408.0);
	float cBlue = clamp(color.b, 0.0, 65408.0);

	float cMax = max(cRed, max(cGreen, cBlue));

	float expp = max(-B - 1.0f, floor(log(cMax) / LN2)) + 1.0f + B;

	float sMax = floor((cMax / pow(2.0f, expp - B - N)) + 0.5f);

	float exps = expp + 1.0f;

	if (0.0 <= sMax && sMax < pow2to9) {
		exps = expp;
	}

	float sRed = floor((cRed / pow(2.0f, exps - B - N)) + 0.5f);
	float sGreen = floor((cGreen / pow(2.0f, exps - B - N)) + 0.5f);
	float sBlue = floor((cBlue / pow(2.0f, exps - B - N)) + 0.5f);
	return (uint(sRed) & 0x1FF) | ((uint(sGreen) & 0x1FF) << 9) | ((uint(sBlue) & 0x1FF) << 18) | ((uint(exps) & 0x1F) << 27);
}

struct SH {
#if (SH_SIZE == 16)
	float c[48];
#else
	float c[28];
#endif
};

shared SH sh_accum[64]; //8x8

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.image_size))) { //too large, do nothing
		return;
	}

	uint probe_index = gl_LocalInvocationID.x + gl_LocalInvocationID.y * 8;

#ifdef MODE_PROCESS

	float probe_cell_size = float(params.grid_size.x / float(params.probe_axis_size - 1)) / cascades.data[params.cascade].to_cell;

	ivec3 probe_cell;
	probe_cell.x = pos.x % int(params.probe_axis_size);
	probe_cell.y = pos.y;
	probe_cell.z = pos.x / int(params.probe_axis_size);

	vec3 probe_pos = cascades.data[params.cascade].offset + vec3(probe_cell) * probe_cell_size;
	vec3 pos_to_uvw = 1.0 / params.grid_size;

	vec4 probe_state = texelFetch(sampler2DArray(probe_state_texture, linear_sampler), ivec3(pos, int(params.cascade)), 0);
	bool probe_valid = probe_state.w > 0.5;
	probe_pos += probe_state.xyz / cascades.data[params.cascade].to_cell;

	// Identifies the placement to within half a voxel, with 0 standing for "none": a probe stuck
	// in geometry, or one only just seeded, whose history was not traced from anywhere yet.
	ivec3 placement_q = clamp(ivec3(round(probe_state.xyz * 2.0)) + ivec3(8), ivec3(0), ivec3(16));
	float placement_code = probe_valid ? float(1 + placement_q.x + placement_q.y * 17 + placement_q.z * 289) : 0.0;

	for (uint i = 0; i < SH_SIZE * 3; i++) {
		sh_accum[probe_index].c[i] = 0.0;
	}

	// quickly ensure each probe has a different "offset" for the ray set, based on integer world position
	uvec3 h3 = hash3(uvec3(params.world_offset + probe_cell));
	float offset = hashf3(vec3(h3 & uvec3(0xFFFFF)));

	// Each frame traces a different strided subset of one set spanning all history frames, so the
	// frames the history averages together cover the sphere evenly between them.
	uint ray_offset = params.history_index;
	uint ray_mult = params.history_size;
	uint ray_total = ray_mult * params.ray_count;

	// A probe stuck in geometry is never sampled, so there is no point tracing it.
	uint ray_count = probe_valid ? params.ray_count : 0;

	for (uint i = 0; i < ray_count; i++) {
		vec3 ray_dir = spherical_fibonacci(ray_offset + i * ray_mult, ray_total, offset * 2.0 * PI);
		ray_dir.y *= params.y_mult;
		ray_dir = normalize(ray_dir);

		//needs to be visible
		vec3 ray_pos = probe_pos;
		vec3 inv_dir = 1.0 / ray_dir;

		bool hit = false;
		uint hit_cascade;

		float bias = params.ray_bias;
		vec3 abs_ray_dir = abs(ray_dir);
		ray_pos += ray_dir * 1.0 / max(abs_ray_dir.x, max(abs_ray_dir.y, abs_ray_dir.z)) * bias / cascades.data[params.cascade].to_cell;
		vec3 uvw;

		for (uint j = params.cascade; j < params.max_cascades; j++) {
			//convert to local bounds
			vec3 pos = ray_pos - cascades.data[j].offset;
			pos *= cascades.data[j].to_cell;

			if (any(lessThan(pos, vec3(0.0))) || any(greaterThanEqual(pos, params.grid_size))) {
				continue; //already past bounds for this cascade, goto next
			}

			//find maximum advance distance (until reaching bounds)
			vec3 t0 = -pos * inv_dir;
			vec3 t1 = (params.grid_size - pos) * inv_dir;
			vec3 tmax = max(t0, t1);
			float max_advance = min(tmax.x, min(tmax.y, tmax.z));

			float advance = 0.0;

			while (advance < max_advance) {
				//read how much to advance from SDF
				uvw = (pos + ray_dir * advance) * pos_to_uvw;

				float distance = texture(sampler3D(sdf_cascades[j], linear_sampler), uvw).r * 255.0 - 1.0;
				if (distance < 0.05) {
					//consider hit
					hit = true;
					break;
				}

				advance += distance;
			}

			if (hit) {
				hit_cascade = j;
				break;
			}

			//change ray origin to collision with bounds
			pos += ray_dir * max_advance;
			pos /= cascades.data[j].to_cell;
			pos += cascades.data[j].offset;
			ray_pos = pos;
		}

		vec4 light;
		if (hit) {
			//avoid reading different texture from different threads
			for (uint j = params.cascade; j < params.max_cascades; j++) {
				if (j == hit_cascade) {
					const float EPSILON = 0.001;
					vec3 hit_normal = normalize(vec3(
							texture(sampler3D(sdf_cascades[hit_cascade], linear_sampler), uvw + vec3(EPSILON, 0.0, 0.0)).r - texture(sampler3D(sdf_cascades[hit_cascade], linear_sampler), uvw - vec3(EPSILON, 0.0, 0.0)).r,
							texture(sampler3D(sdf_cascades[hit_cascade], linear_sampler), uvw + vec3(0.0, EPSILON, 0.0)).r - texture(sampler3D(sdf_cascades[hit_cascade], linear_sampler), uvw - vec3(0.0, EPSILON, 0.0)).r,
							texture(sampler3D(sdf_cascades[hit_cascade], linear_sampler), uvw + vec3(0.0, 0.0, EPSILON)).r - texture(sampler3D(sdf_cascades[hit_cascade], linear_sampler), uvw - vec3(0.0, 0.0, EPSILON)).r));

					vec3 hit_light = texture(sampler3D(light_cascades[hit_cascade], linear_sampler), uvw).rgb;
					vec4 aniso0 = texture(sampler3D(aniso0_cascades[hit_cascade], linear_sampler), uvw);
					vec3 hit_aniso0 = aniso0.rgb;
					vec3 hit_aniso1 = vec3(aniso0.a, texture(sampler3D(aniso1_cascades[hit_cascade], linear_sampler), uvw).rg);

					//one liner magic
					light.rgb = hit_light * (dot(max(vec3(0.0), (hit_normal * hit_aniso0)), vec3(1.0)) + dot(max(vec3(0.0), (-hit_normal * hit_aniso1)), vec3(1.0)));
					light.a = 1.0;
				}
			}

		} else if (bool(params.sky_flags & SKY_FLAGS_MODE_SKY)) {
			// Reconstruct sky orientation as quaternion and rotate ray_dir before sampling.
			float sky_sign = bool(params.sky_flags & SKY_FLAGS_ORIENTATION_SIGN) ? 1.0 : -1.0;
			vec4 sky_quat = vec4(params.sky_color_or_orientation, sky_sign * sqrt(1.0 - dot(params.sky_color_or_orientation, params.sky_color_or_orientation)));
			vec3 sky_dir = cross(sky_quat.xyz, ray_dir);
			sky_dir = ray_dir + ((sky_dir * sky_quat.w) + cross(sky_quat.xyz, sky_dir)) * 2.0;
#ifdef USE_RADIANCE_OCTMAP_ARRAY
			light.rgb = textureLod(sampler2DArray(sky_irradiance, linear_sampler_mipmaps), vec3(vec3_to_oct_with_border(sky_dir, params.sky_irradiance_border_size), 0.0), 2.0).rgb; // Use second mipmap because we don't usually throw a lot of rays, so this compensates.
#else
			light.rgb = textureLod(sampler2D(sky_irradiance, linear_sampler_mipmaps), vec3_to_oct_with_border(sky_dir, params.sky_irradiance_border_size), 2.0).rgb; // Use second mipmap because we don't usually throw a lot of rays, so this compensates.
#endif
			light.rgb *= params.sky_energy;
			light.a = 0.0;

		} else if (bool(params.sky_flags & SKY_FLAGS_MODE_COLOR)) {
			light.rgb = params.sky_color_or_orientation;
			light.rgb *= params.sky_energy;
			light.a = 0.0;
		} else {
			light = vec4(0, 0, 0, 0);
		}

		vec3 ray_dir2 = ray_dir * ray_dir;

#define SH_ACCUM(m_idx, m_value) \
	{ \
		vec3 l = light.rgb * (m_value); \
		sh_accum[probe_index].c[m_idx * 3 + 0] += l.r; \
		sh_accum[probe_index].c[m_idx * 3 + 1] += l.g; \
		sh_accum[probe_index].c[m_idx * 3 + 2] += l.b; \
	}
		SH_ACCUM(0, 0.282095); //l0
		SH_ACCUM(1, 0.488603 * ray_dir.y); //l1n1
		SH_ACCUM(2, 0.488603 * ray_dir.z); //l1n0
		SH_ACCUM(3, 0.488603 * ray_dir.x); //l1p1
		SH_ACCUM(4, 1.092548 * ray_dir.x * ray_dir.y); //l2n2
		SH_ACCUM(5, 1.092548 * ray_dir.y * ray_dir.z); //l2n1
		SH_ACCUM(6, 0.315392 * (3.0 * ray_dir2.z - 1.0)); //l20
		SH_ACCUM(7, 1.092548 * ray_dir.x * ray_dir.z); //l2p1
		SH_ACCUM(8, 0.546274 * (ray_dir2.x - ray_dir2.y)); //l2p2
#if (SH_SIZE == 16)
		SH_ACCUM(9, 0.590043 * ray_dir.y * (3.0f * ray_dir2.x - ray_dir2.y));
		SH_ACCUM(10, 2.890611 * ray_dir.y * ray_dir.x * ray_dir.z);
		SH_ACCUM(11, 0.646360 * ray_dir.y * (-1.0f + 5.0f * ray_dir2.z));
		SH_ACCUM(12, 0.373176 * (5.0f * ray_dir2.z * ray_dir.z - 3.0f * ray_dir.z));
		SH_ACCUM(13, 0.457045 * ray_dir.x * (-1.0f + 5.0f * ray_dir2.z));
		SH_ACCUM(14, 1.445305 * (ray_dir2.x - ray_dir2.y) * ray_dir.z);
		SH_ACCUM(15, 0.590043 * ray_dir.x * (ray_dir2.x - 3.0f * ray_dir2.y));

#endif
	}

	// Fold this frame's trace into the probe's history.
	//
	// The history is a box filter over the last history_size frames, which, with each frame
	// tracing its own strided subset of one fixed ray set, turns over to exactly the same average
	// every history_size frames: a converged probe in a static scene does not flicker at all.
	// On top of that each probe counts how many of those frames are its own (its age), and
	// averages over just those. A probe starting afresh is then usable from its first frame
	// rather than fading in from black over a whole convergence period, one seeded with a guess
	// only counts it as a couple of frames, and one that sees the light change can drop the
	// frames from before the change and catch up at once.
	int history_size = int(params.history_size);
	int history_index = int(params.history_index);
	ivec2 average_pos = ivec2(pos.x, pos.y * SH_SIZE);
	float sample_scale = 4.0 / float(params.ray_count);

	int age = int(get_probe_state(average_pos, STATE_AGE));
	float suspect = get_probe_state(average_pos, STATE_SUSPECT);
	float change_rate = get_probe_state(average_pos, STATE_CHANGE_RATE);

	// How many of the most recent history frames to keep, when not all of them, and whether those
	// are to be marked unsettled (see HISTORY_FLAG_UNSETTLED): they are when what the probe saw
	// changed altogether, and the frames it keeps were traced as that happened.
	int keep = age;
	bool unsettle_kept = true;

	// The frame about to leave the history traced exactly these rays one full turn ago, so unless
	// the light changed since, it saw exactly the same. Comparing against it rather than the
	// average spots a change at once: the average mixes in all the other ray sets, which differ
	// from this one by far more than a light switching off does, with only a handful of rays each.
	// Frames marked unsettled are skipped (see HISTORY_FLAG_UNSETTLED).
	ivec4 previous = imageLoad(lightprobe_history_texture, ivec3(average_pos, history_index));

	if (bool(params.flags & INTEGRATE_FLAG_ADAPTIVE) && age >= history_size && !bool(previous.a & HISTORY_FLAG_UNSETTLED)) {
		vec3 l0 = vec3(sh_accum[probe_index].c[0], sh_accum[probe_index].c[1], sh_accum[probe_index].c[2]) * sample_scale;
		l0 = vec3(clamp(ivec3(l0 * float(1 << HISTORY_BITS)), ivec3(-32768), ivec3(32767))) / float(1 << HISTORY_BITS); // As it will be stored.
		vec3 previous_l0 = vec3(previous.rgb) / float(1 << HISTORY_BITS);

		float lum = max(0.0, get_luminance(l0));
		float previous_lum = max(0.0, get_luminance(previous_l0));
		float change = abs(lum - previous_lum);
		float relative = change / max(max(lum, previous_lum), ADAPT_ABSOLUTE);

		// Only a change that stands out from how the light has been changing lately counts: light
		// that keeps moving on (a lamp carried around, the sun turning) changes every turn, and
		// dropping the history over and over for it left probes with a handful of rays each,
		// flickering out of step with their neighbors for as long as the light moved. Each frame
		// compares a different handful of rays, so while the light moves, how much one of them
		// changed swings well above and below the pace: the margin is a factor, not an offset.
		if (relative > max(ADAPT_RELATIVE, change_rate * ADAPT_RATE_MARGIN) && change > ADAPT_ABSOLUTE) {
			suspect += 1.0;
			if (suspect >= float(ADAPT_CONFIRM_FRAMES)) {
				// It keeps disagreeing: the light changed. Drop as much of the history as the change
				// calls for, going by how much of the light is new: all but the frames since when
				// it all is (a light switched on or off, a door shut), less when only part of it
				// is. The frames kept still carry some of the light from before, so the probe gets
				// there over some frames rather than at once, but it does not flicker for it.
				float kept = float(history_size) * (1.0 - relative) * (1.0 - relative);
				keep = clamp(int(kept), ADAPT_CONFIRM_FRAMES - 1, age - 1);
				unsettle_kept = keep < ADAPT_SETTLE_FRAMES;
				suspect = 0.0;
			}
		} else {
			suspect = 0.0;
		}
		change_rate += (relative - change_rate) / ADAPT_RATE_FRAMES;
	}

	if (!probe_valid) {
		keep = 0; // Traces nothing, holds nothing: whatever it becomes once usable, it starts afresh.
	} else if (get_probe_state(average_pos, STATE_PLACEMENT) != placement_code) {
		// The probe was moved (its cascade was revoxelized with new geometry in reach) or only
		// just seeded, so its history was traced from elsewhere: keep a little of it as a guess.
		keep = min(keep, PRIOR_AGE);
		unsettle_kept = true;
		suspect = 0.0;
	}

	// A frame only leaves the sum when a full history of the probe's own frames is there to
	// drop it from: while the probe is younger than that, the texel about to be overwritten was
	// never counted.
	bool rebuild_sum = keep < age;
	int new_age = probe_valid ? min(keep + 1, history_size) : 0;
	int history_flags = new_age <= ADAPT_SETTLE_FRAMES ? HISTORY_FLAG_UNSETTLED : 0;

	for (int i = 0; i < SH_SIZE; i++) {
		ivec3 slot_pos = ivec3(pos.x, pos.y * SH_SIZE + i, history_index);
		ivec2 coef_pos = average_pos + ivec2(0, i);

		vec3 value = vec3(sh_accum[probe_index].c[i * 3 + 0], sh_accum[probe_index].c[i * 3 + 1], sh_accum[probe_index].c[i * 3 + 2]) * sample_scale;
		ivec3 ivalue = clamp(ivec3(value * float(1 << HISTORY_BITS)), ivec3(-32768), ivec3(32767)); //clamp to 16 bits, so higher values don't break average

		ivec4 average = imageLoad(lightprobe_average_texture, coef_pos);

		if (rebuild_sum) {
			average.rgb = ivec3(0);
			for (int j = 1; j <= keep; j++) {
				ivec3 kept_pos = ivec3(slot_pos.xy, (history_index - j + history_size) % history_size);
				ivec4 kept = imageLoad(lightprobe_history_texture, kept_pos);
				average.rgb += kept.rgb;
				if (unsettle_kept) {
					imageStore(lightprobe_history_texture, kept_pos, ivec4(kept.rgb, HISTORY_FLAG_UNSETTLED));
				}
			}
		} else if (age >= history_size) {
			average.rgb -= imageLoad(lightprobe_history_texture, slot_pos).rgb;
		}
		average.rgb += ivalue;

		if (i == STATE_AGE) {
			average.a = floatBitsToInt(float(new_age));
		} else if (i == STATE_SUSPECT) {
			average.a = floatBitsToInt(suspect);
		} else if (i == STATE_PLACEMENT) {
			average.a = floatBitsToInt(placement_code);
		} else if (i == STATE_CHANGE_RATE) {
			average.a = floatBitsToInt(change_rate);
		}

		imageStore(lightprobe_history_texture, slot_pos, ivec4(ivalue, history_flags));
		imageStore(lightprobe_average_texture, coef_pos, average);

		if (params.store_ambient_texture && i == 0) {
			ivec3 ambient_pos = ivec3(pos, int(params.cascade));
			vec4 ambient_light = vec4(get_probe_coefficient(average, float(new_age)) * 0.88622, 1.0); // SHL0
			imageStore(lightprobe_ambient_texture, ambient_pos, ambient_light);
		}
	}
#endif // MODE PROCESS

#ifdef MODE_STORE

	// converting to octahedral in this step is required because
	// octahedral is much faster to read from the screen than spherical harmonics,
	// despite the very slight quality loss

	ivec2 sh_pos = (pos / OCT_SIZE) * ivec2(1, SH_SIZE);
	ivec2 oct_pos = (pos / OCT_SIZE) * (OCT_SIZE + 2) + ivec2(1);
	ivec2 local_pos = pos % OCT_SIZE;

	//compute the octahedral normal for this texel
	vec3 normal = octahedron_encode(vec2(local_pos) / float(OCT_SIZE));

	// read the spherical harmonic

	vec3 normal2 = normal * normal;
	float c[SH_SIZE] = float[](

			0.282095, //l0
			0.488603 * normal.y, //l1n1
			0.488603 * normal.z, //l1n0
			0.488603 * normal.x, //l1p1
			1.092548 * normal.x * normal.y, //l2n2
			1.092548 * normal.y * normal.z, //l2n1
			0.315392 * (3.0 * normal2.z - 1.0), //l20
			1.092548 * normal.x * normal.z, //l2p1
			0.546274 * (normal2.x - normal2.y) //l2p2
#if (SH_SIZE == 16)
			,
			0.590043 * normal.y * (3.0f * normal2.x - normal2.y),
			2.890611 * normal.y * normal.x * normal.z,
			0.646360 * normal.y * (-1.0f + 5.0f * normal2.z),
			0.373176 * (5.0f * normal2.z * normal.z - 3.0f * normal.z),
			0.457045 * normal.x * (-1.0f + 5.0f * normal2.z),
			1.445305 * (normal2.x - normal2.y) * normal.z,
			0.590043 * normal.x * (normal2.x - 3.0f * normal2.y)

#endif
	);

	const float l_mult[SH_SIZE] = float[](
			1.0,
			2.0 / 3.0,
			2.0 / 3.0,
			2.0 / 3.0,
			1.0 / 4.0,
			1.0 / 4.0,
			1.0 / 4.0,
			1.0 / 4.0,
			1.0 / 4.0
#if (SH_SIZE == 16)
			, // l4 does not contribute to irradiance
			0.0,
			0.0,
			0.0,
			0.0,
			0.0,
			0.0,
			0.0
#endif
	);

	vec3 irradiance = vec3(0.0);
	vec3 radiance = vec3(0.0);

	float age = get_probe_state(sh_pos, STATE_AGE);

	for (uint i = 0; i < SH_SIZE; i++) {
		// read the probe's average
		ivec2 average_pos = sh_pos + ivec2(0, i);
		vec3 sh = get_probe_coefficient(imageLoad(lightprobe_average_texture, average_pos), age);

		vec3 m = sh * c[i] * 4.0;

		irradiance += m * l_mult[i];
		radiance += m;
	}

	//encode RGBE9995 for the final texture

	uint irradiance_rgbe = rgbe_encode(irradiance);
	uint radiance_rgbe = rgbe_encode(radiance);

	//store in octahedral map

	ivec3 texture_pos = ivec3(oct_pos, int(params.cascade));
	ivec3 copy_to[4] = ivec3[](ivec3(-2, -2, -2), ivec3(-2, -2, -2), ivec3(-2, -2, -2), ivec3(-2, -2, -2));
	copy_to[0] = texture_pos + ivec3(local_pos, 0);

	if (local_pos == ivec2(0, 0)) {
		copy_to[1] = texture_pos + ivec3(OCT_SIZE - 1, -1, 0);
		copy_to[2] = texture_pos + ivec3(-1, OCT_SIZE - 1, 0);
		copy_to[3] = texture_pos + ivec3(OCT_SIZE, OCT_SIZE, 0);
	} else if (local_pos == ivec2(OCT_SIZE - 1, 0)) {
		copy_to[1] = texture_pos + ivec3(0, -1, 0);
		copy_to[2] = texture_pos + ivec3(OCT_SIZE, OCT_SIZE - 1, 0);
		copy_to[3] = texture_pos + ivec3(-1, OCT_SIZE, 0);
	} else if (local_pos == ivec2(0, OCT_SIZE - 1)) {
		copy_to[1] = texture_pos + ivec3(-1, 0, 0);
		copy_to[2] = texture_pos + ivec3(OCT_SIZE - 1, OCT_SIZE, 0);
		copy_to[3] = texture_pos + ivec3(OCT_SIZE, -1, 0);
	} else if (local_pos == ivec2(OCT_SIZE - 1, OCT_SIZE - 1)) {
		copy_to[1] = texture_pos + ivec3(0, OCT_SIZE, 0);
		copy_to[2] = texture_pos + ivec3(OCT_SIZE, 0, 0);
		copy_to[3] = texture_pos + ivec3(-1, -1, 0);
	} else if (local_pos.y == 0) {
		copy_to[1] = texture_pos + ivec3(OCT_SIZE - local_pos.x - 1, local_pos.y - 1, 0);
	} else if (local_pos.x == 0) {
		copy_to[1] = texture_pos + ivec3(local_pos.x - 1, OCT_SIZE - local_pos.y - 1, 0);
	} else if (local_pos.y == OCT_SIZE - 1) {
		copy_to[1] = texture_pos + ivec3(OCT_SIZE - local_pos.x - 1, local_pos.y + 1, 0);
	} else if (local_pos.x == OCT_SIZE - 1) {
		copy_to[1] = texture_pos + ivec3(local_pos.x + 1, OCT_SIZE - local_pos.y - 1, 0);
	}

	for (int i = 0; i < 4; i++) {
		if (copy_to[i] == ivec3(-2, -2, -2)) {
			continue;
		}
		imageStore(lightprobe_texture_data, copy_to[i], uvec4(irradiance_rgbe));
		imageStore(lightprobe_texture_data, copy_to[i] + ivec3(0, 0, int(params.max_cascades)), uvec4(radiance_rgbe));
	}

#endif

#ifdef MODE_SCROLL

	ivec3 probe_cell;
	probe_cell.x = pos.x % int(params.probe_axis_size);
	probe_cell.y = pos.y;
	probe_cell.z = pos.x / int(params.probe_axis_size);

	ivec3 read_probe = probe_cell - params.scroll;

	if (all(greaterThanEqual(read_probe, ivec3(0))) && all(lessThan(read_probe, ivec3(params.probe_axis_size)))) {
		// can scroll
		ivec2 tex_pos;
		tex_pos = read_probe.xy;
		tex_pos.x += read_probe.z * int(params.probe_axis_size);

		//scroll
		for (uint j = 0; j < params.history_size; j++) {
			for (int i = 0; i < SH_SIZE; i++) {
				// copy from history texture
				ivec3 src_pos = ivec3(tex_pos.x, tex_pos.y * SH_SIZE + i, int(j));
				ivec3 dst_pos = ivec3(pos.x, pos.y * SH_SIZE + i, int(j));
				ivec4 value = imageLoad(lightprobe_history_texture, src_pos);
				imageStore(lightprobe_history_scroll_texture, dst_pos, value);
			}
		}

		for (int i = 0; i < SH_SIZE; i++) {
			// copy from average texture (which carries the history state along)
			ivec2 src_pos = ivec2(tex_pos.x, tex_pos.y * SH_SIZE + i);
			ivec2 dst_pos = ivec2(pos.x, pos.y * SH_SIZE + i);
			ivec4 value = imageLoad(lightprobe_average_texture, src_pos);
			imageStore(lightprobe_average_scroll_texture, dst_pos, value);
		}
	} else {
		// A probe that is new to the cascade, or that belongs to a cascade rebuilt from scratch.
		// It starts from a guess that only counts as PRIOR_AGE frames of history (see the history
		// update in MODE_PROCESS), so its own traces take over within a few frames. It used to be
		// written into every history frame, which made the guess look fully converged, and the
		// probe then spent a whole convergence period drifting away from it.
		vec3 seed[SH_SIZE];
		for (int i = 0; i < SH_SIZE; i++) {
			seed[i] = vec3(0.0);
		}
		int seed_age = 0; // Nothing to go on: the probe starts empty, and from its first trace.

		if (params.cascade < params.max_cascades - 1) {
			//can't scroll, must look for position in parent cascade

			//to global coords
			float cell_to_probe = float(params.grid_size.x / float(params.probe_axis_size - 1));

			// Not from cascades.data[params.cascade].offset: region updates, which scroll probes, run
			// before the cascade UBO is refreshed for the frame, so that offset is still the one from
			// before this very scroll, one step off the grid being written. world_offset, the
			// cascade's center in probes, already has the new position. The parent's UBO offset is
			// right as it is: when it scrolls this frame too, that happens later in the same loop,
			// so for now its probes still sit where the old offset puts them.
			float probe_cell_size = cell_to_probe / cascades.data[params.cascade].to_cell;
			vec3 probe_pos = (vec3(params.world_offset) - vec3(float(params.probe_axis_size - 1) * 0.5) + vec3(probe_cell)) * probe_cell_size;

			//to parent local coords
			float probe_cell_size_next = cell_to_probe / cascades.data[params.cascade + 1].to_cell;
			probe_pos -= cascades.data[params.cascade + 1].offset;
			probe_pos /= probe_cell_size_next;

			ivec3 probe_posi = ivec3(probe_pos);
			//add up all light, no need to use occlusion here, since occlusion will do its work afterwards

			float total_weight = 0.0;

			for (int i = 0; i < 8; i++) {
				ivec3 offset = probe_posi + ((ivec3(i) >> ivec3(0, 1, 2)) & ivec3(1, 1, 1));

				vec3 trilinear = vec3(1.0) - abs(probe_pos - vec3(offset));
				float weight = trilinear.x * trilinear.y * trilinear.z;

				offset = clamp(offset, ivec3(0), ivec3(params.probe_axis_size - 1));
				ivec2 tex_pos;
				tex_pos = offset.xy;
				tex_pos.x += offset.z * int(params.probe_axis_size);

				ivec2 parent_pos = ivec2(tex_pos.x, tex_pos.y * SH_SIZE);
				float parent_age = intBitsToFloat(imageLoad(lightprobe_average_parent_texture, parent_pos + ivec2(0, STATE_AGE)).a);
				if (parent_age <= 0.0) {
					continue; // Never traced (or stuck in geometry): nothing to pass on.
				}

				for (int j = 0; j < SH_SIZE; j++) {
					ivec4 average = imageLoad(lightprobe_average_parent_texture, parent_pos + ivec2(0, j));
					seed[j] += vec3(average.rgb) / (parent_age * float(1 << HISTORY_BITS)) * weight;
				}

				total_weight += weight;
			}

			if (total_weight > 0.0) {
				for (int i = 0; i < SH_SIZE; i++) {
					seed[i] /= total_weight;
				}
				seed_age = min(PRIOR_AGE, int(params.history_size) - 1);
			}

		} else if (!bool(params.flags & INTEGRATE_FLAG_RESET)) {
			// Scrolling at the edge of the largest cascade, with nothing above it to ask: start from
			// what this texel held, the probe one step behind, as the closest guess available.
			// (A cascade rebuilt from scratch starts from nothing instead, since what it held then
			// belongs somewhere else entirely.)
			ivec2 own_pos = ivec2(pos.x, pos.y * SH_SIZE);
			float own_age = get_probe_state(own_pos, STATE_AGE);
			if (own_age > 0.0) {
				for (int i = 0; i < SH_SIZE; i++) {
					seed[i] = get_probe_coefficient(imageLoad(lightprobe_average_texture, own_pos + ivec2(0, i)), own_age);
				}
				seed_age = min(PRIOR_AGE, int(params.history_size) - 1);
			}
		}

		// The seed goes into the history frames the next trace will be followed by, so that it
		// leaves the history last, after a full turn, while its weight shrinks as the probe's own
		// frames pile up.
		int history_size = int(params.history_size);
		for (int i = 0; i < SH_SIZE; i++) {
			ivec3 ivalue = clamp(ivec3(seed[i] * float(1 << HISTORY_BITS)), ivec3(-32768), ivec3(32767));
			for (int j = 0; j < history_size; j++) {
				int frames_back = (int(params.history_index) - 1 - j + history_size) % history_size;
				ivec3 dst_pos = ivec3(pos.x, pos.y * SH_SIZE + i, j);
				imageStore(lightprobe_history_scroll_texture, dst_pos, frames_back < seed_age ? ivec4(ivalue, HISTORY_FLAG_UNSETTLED) : ivec4(0));
			}

			ivec4 average = ivec4(ivalue * seed_age, floatBitsToInt(i == STATE_AGE ? float(seed_age) : 0.0));
			imageStore(lightprobe_average_scroll_texture, ivec2(pos.x, pos.y * SH_SIZE + i), average);
		}
	}

#endif

#ifdef MODE_SCROLL_STORE

	//do not update probe texture, as these will be updated later

	for (uint j = 0; j < params.history_size; j++) {
		for (int i = 0; i < SH_SIZE; i++) {
			// copy from history texture
			ivec3 spos = ivec3(pos.x, pos.y * SH_SIZE + i, int(j));
			ivec4 value = imageLoad(lightprobe_history_scroll_texture, spos);
			imageStore(lightprobe_history_texture, spos, value);
		}
	}

	for (int i = 0; i < SH_SIZE; i++) {
		// copy from average texture
		ivec2 spos = ivec2(pos.x, pos.y * SH_SIZE + i);
		ivec4 average = imageLoad(lightprobe_average_scroll_texture, spos);
		imageStore(lightprobe_average_texture, spos, average);
	}

#endif
}
