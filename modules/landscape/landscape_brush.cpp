/**************************************************************************/
/*  landscape_brush.cpp                                                   */
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

#include "landscape_brush.h"

#include "landscape_data.h"

#include "core/object/class_db.h"

static _FORCE_INLINE_ uint32_t _landscape_hash(int p_x, int p_z, int p_seed) {
	uint32_t h = uint32_t(p_x) * 374761393u + uint32_t(p_z) * 668265263u + uint32_t(p_seed) * 2246822519u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return h ^ (h >> 16);
}

static _FORCE_INLINE_ float _landscape_value(int p_x, int p_z, int p_seed) {
	return (_landscape_hash(p_x, p_z, p_seed) & 0xFFFFFF) / float(0xFFFFFF);
}

float LandscapeBrush::_noise(real_t p_x, real_t p_z) const {
	// Two octaves of smooth value noise in [0, 1].
	float result = 0.0;
	float amplitude = 0.65;
	real_t frequency = 1.0 / MAX(noise_scale, real_t(0.01));
	for (int octave = 0; octave < 2; octave++) {
		const real_t x = p_x * frequency;
		const real_t z = p_z * frequency;
		const int ix = int(Math::floor(x));
		const int iz = int(Math::floor(z));
		float fx = float(x - ix);
		float fz = float(z - iz);
		fx = fx * fx * (3.0f - 2.0f * fx);
		fz = fz * fz * (3.0f - 2.0f * fz);
		const int seed = noise_seed + octave * 1013;
		const float v00 = _landscape_value(ix, iz, seed);
		const float v10 = _landscape_value(ix + 1, iz, seed);
		const float v01 = _landscape_value(ix, iz + 1, seed);
		const float v11 = _landscape_value(ix + 1, iz + 1, seed);
		result += Math::lerp(Math::lerp(v00, v10, fx), Math::lerp(v01, v11, fx), fz) * amplitude;
		amplitude *= 0.35;
		frequency *= 2.3;
	}
	return result;
}

float LandscapeBrush::_falloff_weight(real_t p_distance_norm) const {
	if (p_distance_norm >= 1.0) {
		return 0.0;
	}
	const real_t inner = 1.0 - falloff;
	if (p_distance_norm <= inner) {
		return 1.0;
	}
	const float t = float((p_distance_norm - inner) / MAX(falloff, real_t(1e-5)));
	switch (falloff_type) {
		case FALLOFF_LINEAR:
			return 1.0f - t;
		case FALLOFF_SPHERICAL:
			return Math::sqrt(MAX(1.0f - t * t, 0.0f));
		case FALLOFF_TIP: {
			const float s = 1.0f - t;
			return 1.0f - Math::sqrt(MAX(1.0f - s * s, 0.0f));
		}
		case FALLOFF_SMOOTH:
		default:
			return 1.0f - t * t * (3.0f - 2.0f * t);
	}
}

float LandscapeBrush::_alpha_weight(real_t p_u, real_t p_v) const {
	if (alpha_cache.is_null()) {
		return 1.0;
	}
	// Rotate around the brush center, then map [-1, 1] to the image.
	const real_t c = Math::cos(alpha_rotation);
	const real_t s = Math::sin(alpha_rotation);
	const real_t u = p_u * c - p_v * s;
	const real_t v = p_u * s + p_v * c;
	if (u < -1.0 || u > 1.0 || v < -1.0 || v > 1.0) {
		return 0.0;
	}
	const int w = alpha_cache->get_width();
	const int h = alpha_cache->get_height();
	const real_t fx = (u * 0.5 + 0.5) * (w - 1);
	const real_t fy = (v * 0.5 + 0.5) * (h - 1);
	const int x0 = CLAMP(int(fx), 0, w - 1);
	const int y0 = CLAMP(int(fy), 0, h - 1);
	const int x1 = MIN(x0 + 1, w - 1);
	const int y1 = MIN(y0 + 1, h - 1);
	const float tx = float(fx - x0);
	const float ty = float(fy - y0);
	const float a = Math::lerp(alpha_cache->get_pixel(x0, y0).r, alpha_cache->get_pixel(x1, y0).r, tx);
	const float b = Math::lerp(alpha_cache->get_pixel(x0, y1).r, alpha_cache->get_pixel(x1, y1).r, tx);
	return Math::lerp(a, b, ty);
}

float LandscapeBrush::get_weight(real_t p_offset_x, real_t p_offset_z) const {
	if (alpha_cache.is_valid()) {
		return _alpha_weight(p_offset_x, p_offset_z);
	}
	return _falloff_weight(Math::sqrt(p_offset_x * p_offset_x + p_offset_z * p_offset_z));
}

Rect2i LandscapeBrush::get_affected_rect(const LandscapeData *p_data, const Vector3 &p_local_center) const {
	ERR_FAIL_NULL_V(p_data, Rect2i());
	const real_t spacing = p_data->get_vertex_spacing();
	const real_t radius = size / spacing * (alpha_cache.is_valid() ? Math::SQRT2 : 1.0);
	const Vector2 center(p_local_center.x / spacing, p_local_center.z / spacing);
	const Point2i begin(Math::floor(center.x - radius), Math::floor(center.y - radius));
	const Point2i end(Math::ceil(center.x + radius) + 1, Math::ceil(center.y + radius) + 1);
	return p_data->clip_rect(Rect2i(begin, end - begin));
}

Rect2i LandscapeBrush::apply(const Ref<LandscapeData> &p_data, const Vector3 &p_local_center, real_t p_delta) {
	ERR_FAIL_COND_V(p_data.is_null() || !p_data->is_valid(), Rect2i());
	const Vector2 center(p_local_center.x / p_data->get_vertex_spacing(), p_local_center.z / p_data->get_vertex_spacing());
	if (is_paint_tool(tool)) {
		return _apply_weights(p_data.ptr(), center, p_delta);
	}
	return _apply_heights(p_data.ptr(), center, p_delta, Vector2(), Vector2(), 0.0, 0.0);
}

Rect2i LandscapeBrush::apply_ramp(const Ref<LandscapeData> &p_data, const Vector3 &p_local_from, const Vector3 &p_local_to) {
	ERR_FAIL_COND_V(p_data.is_null() || !p_data->is_valid(), Rect2i());
	const real_t spacing = p_data->get_vertex_spacing();
	const Vector2 from(p_local_from.x / spacing, p_local_from.z / spacing);
	const Vector2 to(p_local_to.x / spacing, p_local_to.z / spacing);
	const Tool previous = tool;
	tool = TOOL_RAMP;
	const Rect2i rect = _apply_heights(p_data.ptr(), (from + to) * 0.5, 1.0, from, to, p_local_from.y, p_local_to.y);
	tool = previous;
	return rect;
}

Rect2i LandscapeBrush::_apply_heights(LandscapeData *p_data, const Vector2 &p_center, real_t p_delta, const Vector2 &p_ramp_from, const Vector2 &p_ramp_to, float p_ramp_from_height, float p_ramp_to_height) {
	const Vector2i size_texels = p_data->get_size();
	const real_t spacing = p_data->get_vertex_spacing();
	const real_t radius = MAX(size / spacing, real_t(0.5));

	Rect2i rect;
	if (tool == TOOL_RAMP) {
		const real_t half_width = MAX(ramp_width * 0.5 / spacing, real_t(0.5));
		const Vector2 mn = p_ramp_from.min(p_ramp_to) - Vector2(half_width, half_width);
		const Vector2 mx = p_ramp_from.max(p_ramp_to) + Vector2(half_width, half_width);
		rect = Rect2i(Point2i(Math::floor(mn.x), Math::floor(mn.y)), Size2i(Math::ceil(mx.x - mn.x) + 2, Math::ceil(mx.y - mn.y) + 2));
	} else {
		const real_t extent = radius * (alpha_cache.is_valid() ? Math::SQRT2 : 1.0);
		rect = Rect2i(Point2i(Math::floor(p_center.x - extent), Math::floor(p_center.y - extent)), Size2i(Math::ceil(extent * 2.0) + 2, Math::ceil(extent * 2.0) + 2));
	}
	rect = p_data->clip_rect(rect);
	if (!rect.has_area()) {
		return Rect2i();
	}

	if (tool == TOOL_HOLES) {
		// Visibility: carve holes (or fill them when inverted) where the brush weight is above one half.
		if (invert && !p_data->has_holes()) {
			return Rect2i();
		}
		uint8_t *holes = p_data->get_holes_ptrw();
		for (int z = rect.position.y; z < rect.get_end().y; z++) {
			for (int x = rect.position.x; x < rect.get_end().x; x++) {
				if (get_weight((x - p_center.x) / radius, (z - p_center.y) / radius) > 0.5f) {
					holes[int64_t(z) * size_texels.x + x] = invert ? 0 : 1;
				}
			}
		}
		p_data->notify_region_changed(rect, LandscapeData::CHANGED_HOLES);
		return rect;
	}

	float *heights = p_data->get_heights_ptrw();
	const float sign = invert ? -1.0f : 1.0f;
	const float rate = float(strength * p_delta);

	// Source copy for tools that read neighbors.
	const int margin = (tool == TOOL_SMOOTH) ? smooth_radius : 1;
	const Rect2i src_rect = p_data->clip_rect(rect.grow(margin));
	LocalVector<float> src;
	if (tool == TOOL_SMOOTH || tool == TOOL_EROSION) {
		src.resize(src_rect.size.x * src_rect.size.y);
		for (int z = 0; z < src_rect.size.y; z++) {
			memcpy(&src[z * src_rect.size.x], heights + int64_t(src_rect.position.y + z) * size_texels.x + src_rect.position.x, sizeof(float) * src_rect.size.x);
		}
	}

	// Separable box blur for the smooth tool.
	LocalVector<float> blurred;
	if (tool == TOOL_SMOOTH) {
		const int k = MAX(smooth_radius, 1);
		LocalVector<float> tmp;
		tmp.resize(src.size());
		blurred.resize(src.size());
		const int w = src_rect.size.x;
		const int h = src_rect.size.y;
		for (int z = 0; z < h; z++) {
			for (int x = 0; x < w; x++) {
				float sum = 0.0;
				for (int i = -k; i <= k; i++) {
					sum += src[z * w + CLAMP(x + i, 0, w - 1)];
				}
				tmp[z * w + x] = sum / (2 * k + 1);
			}
		}
		for (int z = 0; z < h; z++) {
			for (int x = 0; x < w; x++) {
				float sum = 0.0;
				for (int i = -k; i <= k; i++) {
					sum += tmp[CLAMP(z + i, 0, h - 1) * w + x];
				}
				blurred[z * w + x] = sum / (2 * k + 1);
			}
		}
	}

	if (tool == TOOL_EROSION) {
		// Thermal erosion: material slides down slopes steeper than the talus angle.
		const float talus = Math::tan(Math::deg_to_rad(CLAMP(erosion_talus, 1.0f, 89.0f))) * float(spacing);
		const int w = src_rect.size.x;
		const int h = src_rect.size.y;
		LocalVector<float> current(src);
		LocalVector<float> next;
		static const int offsets[8][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 }, { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } };
		for (int it = 0; it < erosion_iterations; it++) {
			next = LocalVector<float>(current);
			for (int z = 1; z < h - 1; z++) {
				for (int x = 1; x < w - 1; x++) {
					const int gx = src_rect.position.x + x;
					const int gz = src_rect.position.y + z;
					const float weight = get_weight((gx - p_center.x) / radius, (gz - p_center.y) / radius);
					if (weight <= 0.0f) {
						continue;
					}
					const float hc = current[z * w + x];
					float max_diff = 0.0;
					int best = -1;
					for (int o = 0; o < 8; o++) {
						const float dist = (o < 4) ? 1.0f : float(Math::SQRT2);
						const float diff = (hc - current[(z + offsets[o][1]) * w + x + offsets[o][0]]) / dist;
						if (diff > max_diff) {
							max_diff = diff;
							best = o;
						}
					}
					if (best >= 0 && max_diff > talus) {
						const float amount = (max_diff - talus) * 0.5f * weight * MIN(rate * 4.0f, 1.0f);
						next[z * w + x] -= amount;
						next[(z + offsets[best][1]) * w + x + offsets[best][0]] += amount;
					}
				}
			}
			current = LocalVector<float>(next);
		}
		// Only write back the interior of the source rect (inside the brush rect).
		for (int z = rect.position.y; z < rect.get_end().y; z++) {
			for (int x = rect.position.x; x < rect.get_end().x; x++) {
				heights[int64_t(z) * size_texels.x + x] = current[(z - src_rect.position.y) * w + (x - src_rect.position.x)];
			}
		}
		p_data->notify_region_changed(rect, LandscapeData::CHANGED_HEIGHTS);
		return rect;
	}

	const real_t ramp_half_width = MAX(ramp_width * 0.5 / spacing, real_t(0.5));
	const Vector2 ramp_dir = p_ramp_to - p_ramp_from;
	const real_t ramp_len_sq = ramp_dir.length_squared();

	for (int z = rect.position.y; z < rect.get_end().y; z++) {
		for (int x = rect.position.x; x < rect.get_end().x; x++) {
			float &h = heights[int64_t(z) * size_texels.x + x];

			if (tool == TOOL_RAMP) {
				const Vector2 p(x, z);
				const real_t t = ramp_len_sq > 0.0 ? CLAMP((p - p_ramp_from).dot(ramp_dir) / ramp_len_sq, real_t(0.0), real_t(1.0)) : 0.0;
				const real_t d = p.distance_to(p_ramp_from + ramp_dir * t) / ramp_half_width;
				const float weight = _falloff_weight(d) * float(strength > 0.0 ? 1.0 : 0.0);
				if (weight > 0.0f) {
					h = Math::lerp(h, Math::lerp(p_ramp_from_height, p_ramp_to_height, float(t)), weight);
				}
				continue;
			}

			const float weight = get_weight((x - p_center.x) / radius, (z - p_center.y) / radius);
			if (weight <= 0.0f) {
				continue;
			}
			switch (tool) {
				case TOOL_SCULPT: {
					h += sign * weight * rate * float(MAX(size, real_t(1.0))) * 0.5f;
				} break;
				case TOOL_SMOOTH: {
					const float target = blurred[(z - src_rect.position.y) * src_rect.size.x + (x - src_rect.position.x)];
					h = Math::lerp(h, target, MIN(weight * rate * 8.0f, 1.0f));
				} break;
				case TOOL_FLATTEN: {
					if ((flatten_mode == FLATTEN_RAISE && h >= target_height) || (flatten_mode == FLATTEN_LOWER && h <= target_height)) {
						break;
					}
					h = Math::lerp(h, target_height, MIN(weight * rate * 8.0f, 1.0f));
				} break;
				case TOOL_NOISE: {
					const float n = _noise(x * spacing, z * spacing) * 2.0f - 1.0f;
					h += sign * n * weight * rate * float(MAX(size, real_t(1.0))) * 0.25f;
				} break;
				case TOOL_TERRACE: {
					const float step = float(MAX(terrace_height, real_t(0.01)));
					const float level = Math::floor(h / step);
					const float frac = h / step - level;
					// Soft steps: flat shelves with steep risers.
					const float shaped = (level + Math::smoothstep(0.35f, 0.65f, frac)) * step;
					h = Math::lerp(h, shaped, MIN(weight * rate * 4.0f, 1.0f));
				} break;
				default:
					break;
			}
		}
	}

	p_data->notify_region_changed(rect, LandscapeData::CHANGED_HEIGHTS);
	return rect;
}

Rect2i LandscapeBrush::_apply_weights(LandscapeData *p_data, const Vector2 &p_center, real_t p_delta) {
	ERR_FAIL_INDEX_V(layer, LandscapeData::MAX_LAYERS, Rect2i());
	p_data->ensure_layer_capacity(layer + 1);

	const real_t spacing = p_data->get_vertex_spacing();
	const real_t radius = MAX(size / spacing, real_t(0.5));
	const real_t extent = radius * (alpha_cache.is_valid() ? Math::SQRT2 : 1.0);
	const Rect2i rect = p_data->clip_rect(Rect2i(Point2i(Math::floor(p_center.x - extent), Math::floor(p_center.y - extent)), Size2i(Math::ceil(extent * 2.0) + 2, Math::ceil(extent * 2.0) + 2)));
	if (!rect.has_area()) {
		return Rect2i();
	}
	const float rate = float(strength * p_delta);
	const int layers = p_data->get_layer_capacity();

	// Weights of the (expanded) rect, used by the smooth tool.
	const int k = MAX(smooth_radius, 1);
	const Rect2i src_rect = p_data->clip_rect(rect.grow(k));
	LocalVector<float> blurred;
	if (tool == TOOL_PAINT_SMOOTH) {
		const int w = src_rect.size.x;
		const int h = src_rect.size.y;
		LocalVector<float> src;
		src.resize(w * h * layers);
		float tmp_weights[LandscapeData::MAX_LAYERS];
		for (int z = 0; z < h; z++) {
			for (int x = 0; x < w; x++) {
				p_data->get_weights(src_rect.position.x + x, src_rect.position.y + z, tmp_weights);
				for (int l = 0; l < layers; l++) {
					src[(z * w + x) * layers + l] = tmp_weights[l];
				}
			}
		}
		LocalVector<float> tmp;
		tmp.resize(src.size());
		blurred.resize(src.size());
		for (int z = 0; z < h; z++) {
			for (int x = 0; x < w; x++) {
				for (int l = 0; l < layers; l++) {
					float sum = 0.0;
					for (int i = -k; i <= k; i++) {
						sum += src[(z * w + CLAMP(x + i, 0, w - 1)) * layers + l];
					}
					tmp[(z * w + x) * layers + l] = sum / (2 * k + 1);
				}
			}
		}
		for (int z = 0; z < h; z++) {
			for (int x = 0; x < w; x++) {
				for (int l = 0; l < layers; l++) {
					float sum = 0.0;
					for (int i = -k; i <= k; i++) {
						sum += tmp[(CLAMP(z + i, 0, h - 1) * w + x) * layers + l];
					}
					blurred[(z * w + x) * layers + l] = sum / (2 * k + 1);
				}
			}
		}
	}

	float weights[LandscapeData::MAX_LAYERS];
	for (int z = rect.position.y; z < rect.get_end().y; z++) {
		for (int x = rect.position.x; x < rect.get_end().x; x++) {
			const float brush = get_weight((x - p_center.x) / radius, (z - p_center.y) / radius);
			if (brush <= 0.0f) {
				continue;
			}
			p_data->get_weights(x, z, weights);
			const float amount = MIN(brush * rate * 4.0f, 1.0f);
			switch (tool) {
				case TOOL_PAINT_SMOOTH: {
					const int base = ((z - src_rect.position.y) * src_rect.size.x + (x - src_rect.position.x)) * layers;
					for (int l = 0; l < layers; l++) {
						weights[l] = Math::lerp(weights[l], blurred[base + l], amount);
					}
					p_data->set_weights(x, z, weights);
				} break;
				case TOOL_PAINT:
				case TOOL_PAINT_FLATTEN:
				case TOOL_PAINT_NOISE: {
					float target = (tool == TOOL_PAINT) ? (invert ? 0.0f : target_weight) : target_weight;
					float step = amount;
					if (tool == TOOL_PAINT_NOISE) {
						const float n = _noise(x * spacing, z * spacing);
						target = invert ? 0.0f : 1.0f;
						step = MIN(amount * n * n * 2.0f, 1.0f);
					}
					const float current = weights[layer];
					const float new_weight = Math::lerp(current, target, step);
					if (Math::is_equal_approx(new_weight, current)) {
						break;
					}
					// Weight-blended layers: the other layers share the remaining weight.
					float others = 0.0;
					for (int l = 0; l < layers; l++) {
						if (l != layer) {
							others += weights[l];
						}
					}
					for (int l = 0; l < layers; l++) {
						if (l == layer) {
							weights[l] = new_weight;
						} else if (others > 0.0f) {
							weights[l] = weights[l] / others * (1.0f - new_weight);
						}
					}
					if (others <= 0.0f && new_weight < 1.0f) {
						// Nothing else painted here: give the remaining weight to the first other layer.
						weights[layer == 0 ? 1 : 0] = 1.0f - new_weight;
					}
					p_data->set_weights(x, z, weights);
				} break;
				default:
					break;
			}
		}
	}

	p_data->notify_region_changed(rect, LandscapeData::CHANGED_WEIGHTS);
	return rect;
}

void LandscapeBrush::set_tool(Tool p_tool) {
	ERR_FAIL_INDEX(p_tool, TOOL_MAX);
	tool = p_tool;
}

void LandscapeBrush::set_size(real_t p_size) {
	size = MAX(p_size, real_t(0.01));
}

void LandscapeBrush::set_falloff(real_t p_falloff) {
	falloff = CLAMP(p_falloff, real_t(0.0), real_t(1.0));
}

void LandscapeBrush::set_strength(real_t p_strength) {
	strength = CLAMP(p_strength, real_t(0.0), real_t(10.0));
}

void LandscapeBrush::set_falloff_type(FalloffType p_type) {
	falloff_type = p_type;
}

void LandscapeBrush::set_alpha(const Ref<Image> &p_alpha) {
	alpha = p_alpha;
	alpha_cache.unref();
	if (alpha.is_valid() && !alpha->is_empty()) {
		alpha_cache = alpha->duplicate();
		if (alpha_cache->is_compressed()) {
			alpha_cache->decompress();
		}
		alpha_cache->clear_mipmaps();
		alpha_cache->convert(Image::FORMAT_RF);
	}
}

void LandscapeBrush::set_alpha_rotation(real_t p_radians) {
	alpha_rotation = p_radians;
}

void LandscapeBrush::set_invert(bool p_invert) {
	invert = p_invert;
}

void LandscapeBrush::set_layer(int p_layer) {
	layer = CLAMP(p_layer, 0, LandscapeData::MAX_LAYERS - 1);
}

void LandscapeBrush::set_target_weight(float p_weight) {
	target_weight = CLAMP(p_weight, 0.0f, 1.0f);
}

void LandscapeBrush::set_target_height(float p_height) {
	target_height = p_height;
}

void LandscapeBrush::set_flatten_mode(FlattenMode p_mode) {
	flatten_mode = p_mode;
}

void LandscapeBrush::set_smooth_radius(int p_radius) {
	smooth_radius = CLAMP(p_radius, 1, 32);
}

void LandscapeBrush::set_noise_scale(real_t p_scale) {
	noise_scale = MAX(p_scale, real_t(0.01));
}

void LandscapeBrush::set_noise_seed(int p_seed) {
	noise_seed = p_seed;
}

void LandscapeBrush::set_erosion_talus(float p_degrees) {
	erosion_talus = CLAMP(p_degrees, 1.0f, 89.0f);
}

void LandscapeBrush::set_erosion_iterations(int p_iterations) {
	erosion_iterations = CLAMP(p_iterations, 1, 64);
}

void LandscapeBrush::set_terrace_height(real_t p_height) {
	terrace_height = MAX(p_height, real_t(0.01));
}

void LandscapeBrush::set_ramp_width(real_t p_width) {
	ramp_width = MAX(p_width, real_t(0.01));
}

void LandscapeBrush::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_tool", "tool"), &LandscapeBrush::set_tool);
	ClassDB::bind_method(D_METHOD("get_tool"), &LandscapeBrush::get_tool);
	ClassDB::bind_method(D_METHOD("set_size", "size"), &LandscapeBrush::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &LandscapeBrush::get_size);
	ClassDB::bind_method(D_METHOD("set_falloff", "falloff"), &LandscapeBrush::set_falloff);
	ClassDB::bind_method(D_METHOD("get_falloff"), &LandscapeBrush::get_falloff);
	ClassDB::bind_method(D_METHOD("set_strength", "strength"), &LandscapeBrush::set_strength);
	ClassDB::bind_method(D_METHOD("get_strength"), &LandscapeBrush::get_strength);
	ClassDB::bind_method(D_METHOD("set_falloff_type", "type"), &LandscapeBrush::set_falloff_type);
	ClassDB::bind_method(D_METHOD("get_falloff_type"), &LandscapeBrush::get_falloff_type);
	ClassDB::bind_method(D_METHOD("set_alpha", "alpha"), &LandscapeBrush::set_alpha);
	ClassDB::bind_method(D_METHOD("get_alpha"), &LandscapeBrush::get_alpha);
	ClassDB::bind_method(D_METHOD("set_alpha_rotation", "radians"), &LandscapeBrush::set_alpha_rotation);
	ClassDB::bind_method(D_METHOD("get_alpha_rotation"), &LandscapeBrush::get_alpha_rotation);
	ClassDB::bind_method(D_METHOD("set_invert", "invert"), &LandscapeBrush::set_invert);
	ClassDB::bind_method(D_METHOD("is_inverted"), &LandscapeBrush::is_inverted);
	ClassDB::bind_method(D_METHOD("set_layer", "layer"), &LandscapeBrush::set_layer);
	ClassDB::bind_method(D_METHOD("get_layer"), &LandscapeBrush::get_layer);
	ClassDB::bind_method(D_METHOD("set_target_weight", "weight"), &LandscapeBrush::set_target_weight);
	ClassDB::bind_method(D_METHOD("get_target_weight"), &LandscapeBrush::get_target_weight);
	ClassDB::bind_method(D_METHOD("set_target_height", "height"), &LandscapeBrush::set_target_height);
	ClassDB::bind_method(D_METHOD("get_target_height"), &LandscapeBrush::get_target_height);
	ClassDB::bind_method(D_METHOD("set_flatten_mode", "mode"), &LandscapeBrush::set_flatten_mode);
	ClassDB::bind_method(D_METHOD("get_flatten_mode"), &LandscapeBrush::get_flatten_mode);
	ClassDB::bind_method(D_METHOD("set_smooth_radius", "radius"), &LandscapeBrush::set_smooth_radius);
	ClassDB::bind_method(D_METHOD("get_smooth_radius"), &LandscapeBrush::get_smooth_radius);
	ClassDB::bind_method(D_METHOD("set_noise_scale", "scale"), &LandscapeBrush::set_noise_scale);
	ClassDB::bind_method(D_METHOD("get_noise_scale"), &LandscapeBrush::get_noise_scale);
	ClassDB::bind_method(D_METHOD("set_noise_seed", "seed"), &LandscapeBrush::set_noise_seed);
	ClassDB::bind_method(D_METHOD("get_noise_seed"), &LandscapeBrush::get_noise_seed);
	ClassDB::bind_method(D_METHOD("set_erosion_talus", "degrees"), &LandscapeBrush::set_erosion_talus);
	ClassDB::bind_method(D_METHOD("get_erosion_talus"), &LandscapeBrush::get_erosion_talus);
	ClassDB::bind_method(D_METHOD("set_erosion_iterations", "iterations"), &LandscapeBrush::set_erosion_iterations);
	ClassDB::bind_method(D_METHOD("get_erosion_iterations"), &LandscapeBrush::get_erosion_iterations);
	ClassDB::bind_method(D_METHOD("set_terrace_height", "height"), &LandscapeBrush::set_terrace_height);
	ClassDB::bind_method(D_METHOD("get_terrace_height"), &LandscapeBrush::get_terrace_height);
	ClassDB::bind_method(D_METHOD("set_ramp_width", "width"), &LandscapeBrush::set_ramp_width);
	ClassDB::bind_method(D_METHOD("get_ramp_width"), &LandscapeBrush::get_ramp_width);

	ClassDB::bind_method(D_METHOD("get_weight", "offset_x", "offset_z"), &LandscapeBrush::get_weight);
	ClassDB::bind_method(D_METHOD("apply", "data", "local_center", "delta"), &LandscapeBrush::apply, DEFVAL(1.0));
	ClassDB::bind_method(D_METHOD("apply_ramp", "data", "local_from", "local_to"), &LandscapeBrush::apply_ramp);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "tool", PROPERTY_HINT_ENUM, "Sculpt,Smooth,Flatten,Ramp,Noise,Erosion,Terrace,Holes,Paint,Paint Smooth,Paint Flatten,Paint Noise"), "set_tool", "get_tool");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "size", PROPERTY_HINT_RANGE, "0.01,4096,0.01,or_greater,suffix:m"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "falloff", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_falloff", "get_falloff");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "strength", PROPERTY_HINT_RANGE, "0,10,0.01"), "set_strength", "get_strength");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "falloff_type", PROPERTY_HINT_ENUM, "Smooth,Linear,Spherical,Tip"), "set_falloff_type", "get_falloff_type");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "alpha", PROPERTY_HINT_RESOURCE_TYPE, Image::get_class_static()), "set_alpha", "get_alpha");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "alpha_rotation", PROPERTY_HINT_RANGE, "-180,180,0.1,radians_as_degrees"), "set_alpha_rotation", "get_alpha_rotation");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "invert"), "set_invert", "is_inverted");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "layer", PROPERTY_HINT_RANGE, "0,15,1"), "set_layer", "get_layer");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "target_weight", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_target_weight", "get_target_weight");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "target_height", PROPERTY_HINT_NONE, "suffix:m"), "set_target_height", "get_target_height");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "flatten_mode", PROPERTY_HINT_ENUM, "Both,Raise,Lower"), "set_flatten_mode", "get_flatten_mode");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "smooth_radius", PROPERTY_HINT_RANGE, "1,32,1"), "set_smooth_radius", "get_smooth_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "noise_scale", PROPERTY_HINT_RANGE, "0.01,1000,0.01,or_greater,suffix:m"), "set_noise_scale", "get_noise_scale");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "noise_seed"), "set_noise_seed", "get_noise_seed");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "erosion_talus", PROPERTY_HINT_RANGE, "1,89,0.1,degrees"), "set_erosion_talus", "get_erosion_talus");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "erosion_iterations", PROPERTY_HINT_RANGE, "1,64,1"), "set_erosion_iterations", "get_erosion_iterations");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "terrace_height", PROPERTY_HINT_RANGE, "0.01,1000,0.01,or_greater,suffix:m"), "set_terrace_height", "get_terrace_height");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ramp_width", PROPERTY_HINT_RANGE, "0.01,1000,0.01,or_greater,suffix:m"), "set_ramp_width", "get_ramp_width");

	BIND_ENUM_CONSTANT(TOOL_SCULPT);
	BIND_ENUM_CONSTANT(TOOL_SMOOTH);
	BIND_ENUM_CONSTANT(TOOL_FLATTEN);
	BIND_ENUM_CONSTANT(TOOL_RAMP);
	BIND_ENUM_CONSTANT(TOOL_NOISE);
	BIND_ENUM_CONSTANT(TOOL_EROSION);
	BIND_ENUM_CONSTANT(TOOL_TERRACE);
	BIND_ENUM_CONSTANT(TOOL_HOLES);
	BIND_ENUM_CONSTANT(TOOL_PAINT);
	BIND_ENUM_CONSTANT(TOOL_PAINT_SMOOTH);
	BIND_ENUM_CONSTANT(TOOL_PAINT_FLATTEN);
	BIND_ENUM_CONSTANT(TOOL_PAINT_NOISE);
	BIND_ENUM_CONSTANT(TOOL_MAX);

	BIND_ENUM_CONSTANT(FALLOFF_SMOOTH);
	BIND_ENUM_CONSTANT(FALLOFF_LINEAR);
	BIND_ENUM_CONSTANT(FALLOFF_SPHERICAL);
	BIND_ENUM_CONSTANT(FALLOFF_TIP);

	BIND_ENUM_CONSTANT(FLATTEN_BOTH);
	BIND_ENUM_CONSTANT(FLATTEN_RAISE);
	BIND_ENUM_CONSTANT(FLATTEN_LOWER);
}
