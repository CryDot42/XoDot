/**************************************************************************/
/*  landscape_brush.h                                                     */
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

#pragma once

#include "core/io/image.h"
#include "core/object/ref_counted.h"

class LandscapeData;

// Brush settings and sculpt/paint tools operating on LandscapeData.
// Usable from the editor and at runtime (e.g. for terrain deformation in game).
class LandscapeBrush : public RefCounted {
	GDCLASS(LandscapeBrush, RefCounted);

public:
	enum Tool {
		// Sculpt.
		TOOL_SCULPT,
		TOOL_SMOOTH,
		TOOL_FLATTEN,
		TOOL_RAMP,
		TOOL_NOISE,
		TOOL_EROSION,
		TOOL_TERRACE,
		// Paint.
		TOOL_PAINT,
		TOOL_PAINT_SMOOTH,
		TOOL_PAINT_FLATTEN,
		TOOL_PAINT_NOISE,
		TOOL_MAX,
	};

	enum FalloffType {
		FALLOFF_SMOOTH,
		FALLOFF_LINEAR,
		FALLOFF_SPHERICAL,
		FALLOFF_TIP,
	};

	enum FlattenMode {
		FLATTEN_BOTH,
		FLATTEN_RAISE,
		FLATTEN_LOWER,
	};

private:
	Tool tool = TOOL_SCULPT;
	real_t size = 20.0; // Radius, in meters.
	real_t falloff = 0.5;
	real_t strength = 0.3;
	FalloffType falloff_type = FALLOFF_SMOOTH;
	Ref<Image> alpha;
	real_t alpha_rotation = 0.0;
	bool invert = false;

	int layer = 0;
	float target_weight = 1.0;
	float target_height = 0.0;
	FlattenMode flatten_mode = FLATTEN_BOTH;
	int smooth_radius = 2;
	real_t noise_scale = 10.0;
	int noise_seed = 0;
	float erosion_talus = 35.0; // Degrees.
	int erosion_iterations = 4;
	real_t terrace_height = 4.0;
	real_t ramp_width = 10.0;

	Ref<Image> alpha_cache;

	float _falloff_weight(real_t p_distance_norm) const;
	float _alpha_weight(real_t p_u, real_t p_v) const;
	float _noise(real_t p_x, real_t p_z) const;

	Rect2i _apply_heights(LandscapeData *p_data, const Vector2 &p_center, real_t p_delta, const Vector2 &p_ramp_from, const Vector2 &p_ramp_to, float p_ramp_from_height, float p_ramp_to_height);
	Rect2i _apply_weights(LandscapeData *p_data, const Vector2 &p_center, real_t p_delta);

protected:
	static void _bind_methods();

public:
	static bool is_paint_tool(Tool p_tool) { return p_tool >= TOOL_PAINT; }

	void set_tool(Tool p_tool);
	Tool get_tool() const { return tool; }
	void set_size(real_t p_size);
	real_t get_size() const { return size; }
	void set_falloff(real_t p_falloff);
	real_t get_falloff() const { return falloff; }
	void set_strength(real_t p_strength);
	real_t get_strength() const { return strength; }
	void set_falloff_type(FalloffType p_type);
	FalloffType get_falloff_type() const { return falloff_type; }
	void set_alpha(const Ref<Image> &p_alpha);
	Ref<Image> get_alpha() const { return alpha; }
	void set_alpha_rotation(real_t p_radians);
	real_t get_alpha_rotation() const { return alpha_rotation; }
	void set_invert(bool p_invert);
	bool is_inverted() const { return invert; }

	void set_layer(int p_layer);
	int get_layer() const { return layer; }
	void set_target_weight(float p_weight);
	float get_target_weight() const { return target_weight; }
	void set_target_height(float p_height);
	float get_target_height() const { return target_height; }
	void set_flatten_mode(FlattenMode p_mode);
	FlattenMode get_flatten_mode() const { return flatten_mode; }
	void set_smooth_radius(int p_radius);
	int get_smooth_radius() const { return smooth_radius; }
	void set_noise_scale(real_t p_scale);
	real_t get_noise_scale() const { return noise_scale; }
	void set_noise_seed(int p_seed);
	int get_noise_seed() const { return noise_seed; }
	void set_erosion_talus(float p_degrees);
	float get_erosion_talus() const { return erosion_talus; }
	void set_erosion_iterations(int p_iterations);
	int get_erosion_iterations() const { return erosion_iterations; }
	void set_terrace_height(real_t p_height);
	real_t get_terrace_height() const { return terrace_height; }
	void set_ramp_width(real_t p_width);
	real_t get_ramp_width() const { return ramp_width; }

	// Weight of the brush at a normalized offset from its center (-1..1 on both axes).
	float get_weight(real_t p_offset_x, real_t p_offset_z) const;
	Rect2i get_affected_rect(const LandscapeData *p_data, const Vector3 &p_local_center) const;

	// Applies one dab of the current tool at a position in landscape local space.
	// `p_delta` scales the strength (time step in seconds for continuous strokes).
	// Returns the modified texel rect and notifies the data.
	Rect2i apply(const Ref<LandscapeData> &p_data, const Vector3 &p_local_center, real_t p_delta = 1.0);
	// Builds a ramp between two local positions using `ramp_width` and the brush falloff.
	Rect2i apply_ramp(const Ref<LandscapeData> &p_data, const Vector3 &p_local_from, const Vector3 &p_local_to);
};

VARIANT_ENUM_CAST(LandscapeBrush::Tool);
VARIANT_ENUM_CAST(LandscapeBrush::FalloffType);
VARIANT_ENUM_CAST(LandscapeBrush::FlattenMode);
