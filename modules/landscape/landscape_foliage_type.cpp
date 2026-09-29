/**************************************************************************/
/*  landscape_foliage_type.cpp                                            */
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

#include "landscape_foliage_type.h"

#include "core/object/class_db.h"

/* Rendering */

void LandscapeFoliageType::set_mesh(const Ref<Mesh> &p_mesh) {
	lods[0].mesh = p_mesh;
	emit_changed();
}

void LandscapeFoliageType::set_material_override(const Ref<Material> &p_material) {
	material_override = p_material;
	emit_changed();
}

/* Levels of detail */

void LandscapeFoliageType::set_lod_count(int p_count) {
	p_count = CLAMP(p_count, 1, MAX_LODS);
	if (p_count == int(lods.size())) {
		return;
	}
	const int old = lods.size();
	lods.resize(p_count);
	for (int i = old; i < p_count; i++) {
		// New levels start further than the previous one, with the same shadows.
		const Lod &previous = lods[i - 1];
		lods[i].mesh = Ref<Mesh>();
		lods[i].start_distance = previous.start_distance + (cull_distance > 0.0 ? MAX((cull_distance - previous.start_distance) * 0.5f, 1.0f) : 50.0f);
		lods[i].cast_shadows = previous.cast_shadows;
	}
	notify_property_list_changed();
	emit_changed();
}

void LandscapeFoliageType::add_lod(const Ref<Mesh> &p_mesh, float p_start_distance, bool p_cast_shadows) {
	ERR_FAIL_COND_MSG(int(lods.size()) >= MAX_LODS, vformat("A foliage type has at most %d levels of detail.", MAX_LODS));
	Lod lod;
	lod.mesh = p_mesh;
	lod.start_distance = MAX(p_start_distance, 0.0f);
	lod.cast_shadows = p_cast_shadows;
	lods.push_back(lod);
	notify_property_list_changed();
	emit_changed();
}

void LandscapeFoliageType::remove_lod(int p_index) {
	ERR_FAIL_COND_MSG(p_index == 0, "The first level of detail can't be removed.");
	ERR_FAIL_INDEX(p_index, int(lods.size()));
	lods.remove_at(p_index);
	notify_property_list_changed();
	emit_changed();
}

void LandscapeFoliageType::set_lod_mesh(int p_index, const Ref<Mesh> &p_mesh) {
	ERR_FAIL_INDEX(p_index, int(lods.size()));
	lods[p_index].mesh = p_mesh;
	emit_changed();
}

Ref<Mesh> LandscapeFoliageType::get_lod_mesh(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, int(lods.size()), Ref<Mesh>());
	return lods[p_index].mesh;
}

Ref<Mesh> LandscapeFoliageType::get_lod_effective_mesh(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, int(lods.size()), Ref<Mesh>());
	for (int i = p_index; i >= 0; i--) {
		if (lods[i].mesh.is_valid()) {
			return lods[i].mesh;
		}
	}
	return Ref<Mesh>();
}

void LandscapeFoliageType::set_lod_start_distance(int p_index, float p_distance) {
	ERR_FAIL_INDEX(p_index, int(lods.size()));
	if (p_index == 0) {
		return; // The first level always starts at the camera.
	}
	lods[p_index].start_distance = MAX(p_distance, 0.0f);
	emit_changed();
}

float LandscapeFoliageType::get_lod_start_distance(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, int(lods.size()), 0.0);
	return p_index == 0 ? 0.0f : lods[p_index].start_distance;
}

float LandscapeFoliageType::get_lod_end_distance(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, int(lods.size()), 0.0);
	return p_index + 1 < int(lods.size()) ? lods[p_index + 1].start_distance : cull_distance;
}

void LandscapeFoliageType::set_lod_cast_shadows(int p_index, bool p_enable) {
	ERR_FAIL_INDEX(p_index, int(lods.size()));
	lods[p_index].cast_shadows = p_enable;
	emit_changed();
}

bool LandscapeFoliageType::is_lod_casting_shadows(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, int(lods.size()), false);
	return lods[p_index].cast_shadows;
}

void LandscapeFoliageType::set_cull_distance(float p_distance) {
	cull_distance = MAX(p_distance, 0.0f);
	emit_changed();
}

void LandscapeFoliageType::set_cull_random(float p_fraction) {
	cull_random = CLAMP(p_fraction, 0.0f, 1.0f);
	emit_changed();
}

void LandscapeFoliageType::set_lod_transition(float p_distance) {
	lod_transition = MAX(p_distance, 0.0f);
	emit_changed();
}

AABB LandscapeFoliageType::get_mesh_bounds() const {
	AABB bounds;
	bool first = true;
	for (const Lod &lod : lods) {
		if (lod.mesh.is_null()) {
			continue;
		}
		const AABB aabb = lod.mesh->get_aabb();
		bounds = first ? aabb : bounds.merge(aabb);
		first = false;
	}
	return bounds;
}

bool LandscapeFoliageType::_parse_lod_property(const StringName &p_name, int &r_index, String &r_what) const {
	const String property = p_name;
	if (!property.begins_with("lod_")) {
		return false;
	}
	const int slash = property.find_char('/');
	if (slash < 0) {
		return false;
	}
	const String index = property.substr(4, slash - 4);
	if (!index.is_valid_int()) {
		return false;
	}
	r_index = index.to_int();
	r_what = property.substr(slash + 1);
	return r_index >= 0 && r_index < MAX_LODS;
}

bool LandscapeFoliageType::_set(const StringName &p_name, const Variant &p_value) {
	int index;
	String what;
	if (!_parse_lod_property(p_name, index, what)) {
		return false;
	}
	if (index >= int(lods.size())) {
		// Levels may be loaded before the level count.
		set_lod_count(index + 1);
	}
	if (what == "mesh" && index > 0) {
		set_lod_mesh(index, p_value);
	} else if (what == "start_distance" && index > 0) {
		set_lod_start_distance(index, p_value);
	} else if (what == "cast_shadows") {
		set_lod_cast_shadows(index, p_value);
	} else {
		return false;
	}
	return true;
}

bool LandscapeFoliageType::_get(const StringName &p_name, Variant &r_ret) const {
	int index;
	String what;
	if (!_parse_lod_property(p_name, index, what) || index >= int(lods.size())) {
		return false;
	}
	if (what == "mesh" && index > 0) {
		r_ret = lods[index].mesh;
	} else if (what == "start_distance" && index > 0) {
		r_ret = lods[index].start_distance;
	} else if (what == "cast_shadows") {
		r_ret = lods[index].cast_shadows;
	} else {
		return false;
	}
	return true;
}

void LandscapeFoliageType::_get_property_list(List<PropertyInfo> *p_list) const {
	// Edited in the LOD window of the editor, not in the inspector.
	for (uint32_t i = 0; i < lods.size(); i++) {
		const String prefix = vformat("lod_%d/", i);
		if (i > 0) {
			p_list->push_back(PropertyInfo(Variant::OBJECT, prefix + "mesh", PROPERTY_HINT_RESOURCE_TYPE, Mesh::get_class_static(), PROPERTY_USAGE_NO_EDITOR));
			p_list->push_back(PropertyInfo(Variant::FLOAT, prefix + "start_distance", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR));
		}
		p_list->push_back(PropertyInfo(Variant::BOOL, prefix + "cast_shadows", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR));
	}
}

/* Painting */

void LandscapeFoliageType::set_density(float p_density) {
	density = MAX(p_density, 0.0f);
	emit_changed();
}

void LandscapeFoliageType::set_radius(float p_radius) {
	radius = MAX(p_radius, 0.0f);
	emit_changed();
}

/* Scale */

void LandscapeFoliageType::set_scaling(Scaling p_scaling) {
	scaling = p_scaling;
	notify_property_list_changed();
	emit_changed();
}

void LandscapeFoliageType::set_scale_min(float p_scale) {
	scale_min = MAX(p_scale, 0.001f);
	emit_changed();
}

void LandscapeFoliageType::set_scale_max(float p_scale) {
	scale_max = MAX(p_scale, 0.001f);
	emit_changed();
}

void LandscapeFoliageType::set_vertical_scale_min(float p_scale) {
	vertical_scale_min = MAX(p_scale, 0.001f);
	emit_changed();
}

void LandscapeFoliageType::set_vertical_scale_max(float p_scale) {
	vertical_scale_max = MAX(p_scale, 0.001f);
	emit_changed();
}

/* Placement */

void LandscapeFoliageType::set_offset_min(float p_offset) {
	offset_min = p_offset;
	emit_changed();
}

void LandscapeFoliageType::set_offset_max(float p_offset) {
	offset_max = p_offset;
	emit_changed();
}

void LandscapeFoliageType::set_align_to_normal(bool p_enable) {
	align_to_normal = p_enable;
	notify_property_list_changed();
	emit_changed();
}

void LandscapeFoliageType::set_align_max_angle(float p_radians) {
	align_max_angle = CLAMP(p_radians, 0.0f, float(Math::PI * 0.5));
	emit_changed();
}

void LandscapeFoliageType::set_random_yaw(bool p_enable) {
	random_yaw = p_enable;
	emit_changed();
}

void LandscapeFoliageType::set_random_pitch(float p_radians) {
	random_pitch = CLAMP(p_radians, 0.0f, float(Math::PI * 0.5));
	emit_changed();
}

/* Filters */

void LandscapeFoliageType::set_slope_min(float p_radians) {
	slope_min = CLAMP(p_radians, 0.0f, float(Math::PI * 0.5));
	emit_changed();
}

void LandscapeFoliageType::set_slope_max(float p_radians) {
	slope_max = CLAMP(p_radians, 0.0f, float(Math::PI * 0.5));
	emit_changed();
}

void LandscapeFoliageType::set_height_min(float p_height) {
	height_min = p_height;
	emit_changed();
}

void LandscapeFoliageType::set_height_max(float p_height) {
	height_max = p_height;
	emit_changed();
}

void LandscapeFoliageType::set_layers(uint32_t p_layers) {
	layers = p_layers & 0xFFFF;
	notify_property_list_changed();
	emit_changed();
}

void LandscapeFoliageType::set_exclude_layers(uint32_t p_layers) {
	exclude_layers = p_layers & 0xFFFF;
	notify_property_list_changed();
	emit_changed();
}

void LandscapeFoliageType::set_layer_min_weight(float p_weight) {
	layer_min_weight = CLAMP(p_weight, 0.0f, 1.0f);
	emit_changed();
}

/* Placement */

bool LandscapeFoliageType::accepts(const Vector3 &p_normal, real_t p_height, const float *p_weights) const {
	const float slope = Math::acos(CLAMP(float(p_normal.normalized().y), -1.0f, 1.0f));
	constexpr float SLOPE_EPSILON = 1e-4;
	if (slope < slope_min - SLOPE_EPSILON || slope > slope_max + SLOPE_EPSILON) {
		return false;
	}
	if (p_height < height_min || p_height > height_max) {
		return false;
	}
	if (!uses_layers() || !p_weights) {
		return true;
	}
	float included = 0.0;
	float excluded = 0.0;
	for (int i = 0; i < 16; i++) {
		if (layers & (1u << i)) {
			included += p_weights[i];
		}
		if (exclude_layers & (1u << i)) {
			excluded += p_weights[i];
		}
	}
	if (layers && (included <= 0.0f || included < layer_min_weight)) {
		return false;
	}
	if (exclude_layers && excluded > 0.0f && excluded >= layer_min_weight) {
		return false;
	}
	return true;
}

Vector3 LandscapeFoliageType::get_align_normal(const Vector3 &p_ground_normal) const {
	const Vector3 up(0, 1, 0);
	if (!align_to_normal || p_ground_normal.is_zero_approx()) {
		return up;
	}
	const Vector3 normal = p_ground_normal.normalized();
	const real_t angle = up.angle_to(normal);
	if (angle <= align_max_angle) {
		return normal;
	}
	// Tilted towards the ground normal, at most by the maximum angle.
	const Vector3 axis = up.cross(normal);
	if (axis.is_zero_approx()) {
		return up;
	}
	return up.rotated(axis.normalized(), align_max_angle);
}

LandscapeFoliageType::Placement LandscapeFoliageType::generate(const Vector3 &p_ground, const Vector3 &p_normal, RandomPCG &r_rng) const {
	Placement placement;
	placement.random = r_rng.randf();

	Vector3 scale;
	const float horizontal = r_rng.random(scale_min, scale_max);
	switch (scaling) {
		case SCALING_UNIFORM: {
			scale = Vector3(horizontal, horizontal, horizontal);
		} break;
		case SCALING_FREE: {
			scale = Vector3(horizontal, r_rng.random(vertical_scale_min, vertical_scale_max), r_rng.random(scale_min, scale_max));
		} break;
		case SCALING_LOCK_HORIZONTAL: {
			scale = Vector3(horizontal, r_rng.random(vertical_scale_min, vertical_scale_max), horizontal);
		} break;
	}

	Basis basis = Basis::from_scale(scale);
	if (random_yaw) {
		basis = Basis(Vector3(0, 1, 0), r_rng.randf() * Math::TAU) * basis;
	}
	if (random_pitch > 0.0) {
		const real_t direction = r_rng.randf() * Math::TAU;
		basis = Basis(Vector3(Math::cos(direction), 0, Math::sin(direction)), r_rng.randf() * random_pitch) * basis;
	}
	placement.align_normal = get_align_normal(p_normal);
	if (align_to_normal) {
		basis = Basis(Quaternion(Vector3(0, 1, 0), placement.align_normal)) * basis;
	}
	placement.offset = r_rng.random(offset_min, offset_max);
	placement.transform = Transform3D(basis, p_ground + Vector3(0, placement.offset, 0));
	return placement;
}

uint64_t LandscapeFoliageType::get_render_hash() const {
	uint64_t h = hash_murmur3_one_64(lods.size());
	for (uint32_t i = 0; i < lods.size(); i++) {
		const Ref<Mesh> lod_mesh = get_lod_effective_mesh(i);
		h = hash_murmur3_one_64(lod_mesh.is_valid() ? lod_mesh->get_rid().get_id() : 0, h);
		h = hash_murmur3_one_64(uint64_t(get_lod_start_distance(i) * 1000.0), h);
		h = hash_murmur3_one_64(lods[i].cast_shadows, h);
	}
	h = hash_murmur3_one_64(material_override.is_valid() ? material_override->get_rid().get_id() : 0, h);
	h = hash_murmur3_one_64(uint64_t(cull_distance * 1000.0), h);
	h = hash_murmur3_one_64(uint64_t(cull_random * 1000.0), h);
	h = hash_murmur3_one_64(uint64_t(lod_transition * 1000.0), h);
	return h;
}

void LandscapeFoliageType::_validate_property(PropertyInfo &p_property) const {
	if (p_property.name == "vertical_scale_min" || p_property.name == "vertical_scale_max") {
		if (scaling == SCALING_UNIFORM) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}
	} else if (p_property.name == "align_max_angle") {
		if (!align_to_normal) {
			p_property.usage |= PROPERTY_USAGE_READ_ONLY;
		}
	} else if (p_property.name == "layer_min_weight") {
		if (!uses_layers()) {
			p_property.usage |= PROPERTY_USAGE_READ_ONLY;
		}
	}
}

void LandscapeFoliageType::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_mesh", "mesh"), &LandscapeFoliageType::set_mesh);
	ClassDB::bind_method(D_METHOD("get_mesh"), &LandscapeFoliageType::get_mesh);
	ClassDB::bind_method(D_METHOD("set_material_override", "material"), &LandscapeFoliageType::set_material_override);
	ClassDB::bind_method(D_METHOD("get_material_override"), &LandscapeFoliageType::get_material_override);

	ClassDB::bind_method(D_METHOD("set_lod_count", "count"), &LandscapeFoliageType::set_lod_count);
	ClassDB::bind_method(D_METHOD("get_lod_count"), &LandscapeFoliageType::get_lod_count);
	ClassDB::bind_method(D_METHOD("add_lod", "mesh", "start_distance", "cast_shadows"), &LandscapeFoliageType::add_lod, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("remove_lod", "index"), &LandscapeFoliageType::remove_lod);
	ClassDB::bind_method(D_METHOD("set_lod_mesh", "index", "mesh"), &LandscapeFoliageType::set_lod_mesh);
	ClassDB::bind_method(D_METHOD("get_lod_mesh", "index"), &LandscapeFoliageType::get_lod_mesh);
	ClassDB::bind_method(D_METHOD("get_lod_effective_mesh", "index"), &LandscapeFoliageType::get_lod_effective_mesh);
	ClassDB::bind_method(D_METHOD("set_lod_start_distance", "index", "distance"), &LandscapeFoliageType::set_lod_start_distance);
	ClassDB::bind_method(D_METHOD("get_lod_start_distance", "index"), &LandscapeFoliageType::get_lod_start_distance);
	ClassDB::bind_method(D_METHOD("get_lod_end_distance", "index"), &LandscapeFoliageType::get_lod_end_distance);
	ClassDB::bind_method(D_METHOD("set_lod_cast_shadows", "index", "enable"), &LandscapeFoliageType::set_lod_cast_shadows);
	ClassDB::bind_method(D_METHOD("is_lod_casting_shadows", "index"), &LandscapeFoliageType::is_lod_casting_shadows);
	ClassDB::bind_method(D_METHOD("set_cull_distance", "distance"), &LandscapeFoliageType::set_cull_distance);
	ClassDB::bind_method(D_METHOD("get_cull_distance"), &LandscapeFoliageType::get_cull_distance);
	ClassDB::bind_method(D_METHOD("set_cull_random", "fraction"), &LandscapeFoliageType::set_cull_random);
	ClassDB::bind_method(D_METHOD("get_cull_random"), &LandscapeFoliageType::get_cull_random);
	ClassDB::bind_method(D_METHOD("set_lod_transition", "distance"), &LandscapeFoliageType::set_lod_transition);
	ClassDB::bind_method(D_METHOD("get_lod_transition"), &LandscapeFoliageType::get_lod_transition);
	ClassDB::bind_method(D_METHOD("get_mesh_bounds"), &LandscapeFoliageType::get_mesh_bounds);

	ClassDB::bind_method(D_METHOD("set_density", "density"), &LandscapeFoliageType::set_density);
	ClassDB::bind_method(D_METHOD("get_density"), &LandscapeFoliageType::get_density);
	ClassDB::bind_method(D_METHOD("set_radius", "radius"), &LandscapeFoliageType::set_radius);
	ClassDB::bind_method(D_METHOD("get_radius"), &LandscapeFoliageType::get_radius);

	ClassDB::bind_method(D_METHOD("set_scaling", "scaling"), &LandscapeFoliageType::set_scaling);
	ClassDB::bind_method(D_METHOD("get_scaling"), &LandscapeFoliageType::get_scaling);
	ClassDB::bind_method(D_METHOD("set_scale_min", "scale"), &LandscapeFoliageType::set_scale_min);
	ClassDB::bind_method(D_METHOD("get_scale_min"), &LandscapeFoliageType::get_scale_min);
	ClassDB::bind_method(D_METHOD("set_scale_max", "scale"), &LandscapeFoliageType::set_scale_max);
	ClassDB::bind_method(D_METHOD("get_scale_max"), &LandscapeFoliageType::get_scale_max);
	ClassDB::bind_method(D_METHOD("set_vertical_scale_min", "scale"), &LandscapeFoliageType::set_vertical_scale_min);
	ClassDB::bind_method(D_METHOD("get_vertical_scale_min"), &LandscapeFoliageType::get_vertical_scale_min);
	ClassDB::bind_method(D_METHOD("set_vertical_scale_max", "scale"), &LandscapeFoliageType::set_vertical_scale_max);
	ClassDB::bind_method(D_METHOD("get_vertical_scale_max"), &LandscapeFoliageType::get_vertical_scale_max);

	ClassDB::bind_method(D_METHOD("set_offset_min", "offset"), &LandscapeFoliageType::set_offset_min);
	ClassDB::bind_method(D_METHOD("get_offset_min"), &LandscapeFoliageType::get_offset_min);
	ClassDB::bind_method(D_METHOD("set_offset_max", "offset"), &LandscapeFoliageType::set_offset_max);
	ClassDB::bind_method(D_METHOD("get_offset_max"), &LandscapeFoliageType::get_offset_max);
	ClassDB::bind_method(D_METHOD("set_align_to_normal", "enable"), &LandscapeFoliageType::set_align_to_normal);
	ClassDB::bind_method(D_METHOD("is_aligned_to_normal"), &LandscapeFoliageType::is_aligned_to_normal);
	ClassDB::bind_method(D_METHOD("set_align_max_angle", "radians"), &LandscapeFoliageType::set_align_max_angle);
	ClassDB::bind_method(D_METHOD("get_align_max_angle"), &LandscapeFoliageType::get_align_max_angle);
	ClassDB::bind_method(D_METHOD("set_random_yaw", "enable"), &LandscapeFoliageType::set_random_yaw);
	ClassDB::bind_method(D_METHOD("has_random_yaw"), &LandscapeFoliageType::has_random_yaw);
	ClassDB::bind_method(D_METHOD("set_random_pitch", "radians"), &LandscapeFoliageType::set_random_pitch);
	ClassDB::bind_method(D_METHOD("get_random_pitch"), &LandscapeFoliageType::get_random_pitch);

	ClassDB::bind_method(D_METHOD("set_slope_min", "radians"), &LandscapeFoliageType::set_slope_min);
	ClassDB::bind_method(D_METHOD("get_slope_min"), &LandscapeFoliageType::get_slope_min);
	ClassDB::bind_method(D_METHOD("set_slope_max", "radians"), &LandscapeFoliageType::set_slope_max);
	ClassDB::bind_method(D_METHOD("get_slope_max"), &LandscapeFoliageType::get_slope_max);
	ClassDB::bind_method(D_METHOD("set_height_min", "height"), &LandscapeFoliageType::set_height_min);
	ClassDB::bind_method(D_METHOD("get_height_min"), &LandscapeFoliageType::get_height_min);
	ClassDB::bind_method(D_METHOD("set_height_max", "height"), &LandscapeFoliageType::set_height_max);
	ClassDB::bind_method(D_METHOD("get_height_max"), &LandscapeFoliageType::get_height_max);
	ClassDB::bind_method(D_METHOD("set_layers", "layers"), &LandscapeFoliageType::set_layers);
	ClassDB::bind_method(D_METHOD("get_layers"), &LandscapeFoliageType::get_layers);
	ClassDB::bind_method(D_METHOD("set_exclude_layers", "layers"), &LandscapeFoliageType::set_exclude_layers);
	ClassDB::bind_method(D_METHOD("get_exclude_layers"), &LandscapeFoliageType::get_exclude_layers);
	ClassDB::bind_method(D_METHOD("set_layer_min_weight", "weight"), &LandscapeFoliageType::set_layer_min_weight);
	ClassDB::bind_method(D_METHOD("get_layer_min_weight"), &LandscapeFoliageType::get_layer_min_weight);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "mesh", PROPERTY_HINT_RESOURCE_TYPE, Mesh::get_class_static()), "set_mesh", "get_mesh");
	// Shown as a summary with a button that opens the LOD window of the editor.
	ADD_PROPERTY(PropertyInfo(Variant::INT, "lod_count", PROPERTY_HINT_RANGE, "1,8,1"), "set_lod_count", "get_lod_count");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "material_override", PROPERTY_HINT_RESOURCE_TYPE, "BaseMaterial3D,ShaderMaterial"), "set_material_override", "get_material_override");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cull_distance", PROPERTY_HINT_RANGE, "0,16384,0.1,or_greater,suffix:m", PROPERTY_USAGE_NO_EDITOR), "set_cull_distance", "get_cull_distance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cull_random", PROPERTY_HINT_RANGE, "0,1,0.01", PROPERTY_USAGE_NO_EDITOR), "set_cull_random", "get_cull_random");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lod_transition", PROPERTY_HINT_RANGE, "0,64,0.01,or_greater,suffix:m", PROPERTY_USAGE_NO_EDITOR), "set_lod_transition", "get_lod_transition");

	ADD_GROUP("Painting", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "density", PROPERTY_HINT_RANGE, String::utf8("0,1000,0.01,or_greater,suffix:/ 100 m²")), "set_density", "get_density");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "radius", PROPERTY_HINT_RANGE, "0,64,0.01,or_greater,suffix:m"), "set_radius", "get_radius");

	ADD_GROUP("Scale", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "scaling", PROPERTY_HINT_ENUM, "Uniform,Free,Lock Horizontal"), "set_scaling", "get_scaling");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "scale_min", PROPERTY_HINT_RANGE, "0.001,10,0.001,or_greater"), "set_scale_min", "get_scale_min");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "scale_max", PROPERTY_HINT_RANGE, "0.001,10,0.001,or_greater"), "set_scale_max", "get_scale_max");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "vertical_scale_min", PROPERTY_HINT_RANGE, "0.001,10,0.001,or_greater"), "set_vertical_scale_min", "get_vertical_scale_min");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "vertical_scale_max", PROPERTY_HINT_RANGE, "0.001,10,0.001,or_greater"), "set_vertical_scale_max", "get_vertical_scale_max");

	ADD_GROUP("Placement", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "offset_min", PROPERTY_HINT_RANGE, "-10,10,0.001,or_greater,or_less,suffix:m"), "set_offset_min", "get_offset_min");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "offset_max", PROPERTY_HINT_RANGE, "-10,10,0.001,or_greater,or_less,suffix:m"), "set_offset_max", "get_offset_max");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "align_to_normal"), "set_align_to_normal", "is_aligned_to_normal");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "align_max_angle", PROPERTY_HINT_RANGE, "0,90,0.1,radians_as_degrees"), "set_align_max_angle", "get_align_max_angle");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "random_yaw"), "set_random_yaw", "has_random_yaw");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "random_pitch", PROPERTY_HINT_RANGE, "0,90,0.1,radians_as_degrees"), "set_random_pitch", "get_random_pitch");

	ADD_GROUP("Filters", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "slope_min", PROPERTY_HINT_RANGE, "0,90,0.1,radians_as_degrees"), "set_slope_min", "get_slope_min");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "slope_max", PROPERTY_HINT_RANGE, "0,90,0.1,radians_as_degrees"), "set_slope_max", "get_slope_max");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "height_min", PROPERTY_HINT_RANGE, "-100000,100000,0.01,or_greater,or_less,suffix:m"), "set_height_min", "get_height_min");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "height_max", PROPERTY_HINT_RANGE, "-100000,100000,0.01,or_greater,or_less,suffix:m"), "set_height_max", "get_height_max");
	// Shown as a compact grid of the 16 landscape layers by the editor.
	static const char *layer_flags = "0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15";
	ADD_PROPERTY(PropertyInfo(Variant::INT, "layers", PROPERTY_HINT_FLAGS, layer_flags), "set_layers", "get_layers");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "exclude_layers", PROPERTY_HINT_FLAGS, layer_flags), "set_exclude_layers", "get_exclude_layers");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "layer_min_weight", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_layer_min_weight", "get_layer_min_weight");

	BIND_ENUM_CONSTANT(SCALING_UNIFORM);
	BIND_ENUM_CONSTANT(SCALING_FREE);
	BIND_ENUM_CONSTANT(SCALING_LOCK_HORIZONTAL);

	BIND_CONSTANT(MAX_LODS);
}

LandscapeFoliageType::LandscapeFoliageType() {
	lods.resize(1);
}
