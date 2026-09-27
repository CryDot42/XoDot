/**************************************************************************/
/*  landscape_3d.cpp                                                      */
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

#include "landscape_3d.h"

#include "landscape_shader.h"

#include "core/config/engine.h"
#include "core/math/math_funcs_binary.h"
#include "core/math/triangle_mesh.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "scene/3d/camera_3d.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/world_3d.h"
#ifndef PHYSICS_3D_DISABLED
#include "servers/physics_3d/physics_server_3d.h"
#endif
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_default.h"

Landscape3D::EditorCameraCallback Landscape3D::editor_camera_callback = nullptr;
Ref<Shader> Landscape3D::builtin_shader;

static constexpr int UPLOAD_CHUNK = 1024;
static constexpr int MAX_DIRTY_RECTS = 16;

static void _landscape_add_dirty_rect(LocalVector<Rect2i> &r_rects, const Rect2i &p_rect) {
	for (Rect2i &rect : r_rects) {
		if (rect.grow(1).intersects(p_rect)) {
			rect = rect.merge(p_rect);
			return;
		}
	}
	if (r_rects.size() >= MAX_DIRTY_RECTS) {
		Rect2i merged = p_rect;
		for (const Rect2i &rect : r_rects) {
			merged = merged.merge(rect);
		}
		r_rects.clear();
		r_rects.push_back(merged);
		return;
	}
	r_rects.push_back(p_rect);
}

static uint32_t _landscape_split_words(int p_level) {
	return MAX((1u << (2 * p_level)) / 32u, 1u);
}

/* Resources */

void Landscape3D::_create_render_resources() {
	RenderingServer *rs = RenderingServer::get_singleton();
	rendering_supported = rs->get_rendering_device() != nullptr;

	material = rs->material_create();
	height_texture = rs->texture_2d_placeholder_create();
	normal_texture = rs->texture_2d_placeholder_create();
	weights_texture = rs->texture_2d_layered_placeholder_create(RSE::TEXTURE_LAYERED_2D_ARRAY);
	albedo_height_array.instantiate();
	normal_roughness_array.instantiate();

	for (int i = 0; i < 2; i++) {
		instances[i] = rs->instance_create();
		rs->instance_attach_object_instance_id(instances[i], get_instance_id());
		rs->instance_geometry_set_material_override(instances[i], material);
	}

#ifdef RD_ENABLED
	if (rendering_supported) {
		gpu = memnew(LandscapeGPU);
	}
#endif

	_update_material_shader();
	_rebuild_patch_mesh();
}

void Landscape3D::_free_render_resources() {
	RenderingServer *rs = RenderingServer::get_singleton();
	for (int i = 0; i < 2; i++) {
		if (instances[i].is_valid()) {
			rs->free_rid(instances[i]);
			instances[i] = RID();
		}
		if (multimeshes[i].is_valid()) {
			rs->free_rid(multimeshes[i]);
			multimeshes[i] = RID();
		}
	}
	if (patch_mesh.is_valid()) {
		rs->free_rid(patch_mesh);
		patch_mesh = RID();
	}
	if (material.is_valid()) {
		rs->free_rid(material);
		material = RID();
	}
	RID *textures[] = { &height_texture, &normal_texture, &weights_texture };
	for (RID *texture : textures) {
		if (texture->is_valid()) {
			rs->free_rid(*texture);
			*texture = RID();
		}
	}
	albedo_height_array.unref();
	normal_roughness_array.unref();

#ifdef RD_ENABLED
	if (gpu) {
		// The GPU resources must be released on the rendering thread, after the textures above.
		rs->call_on_render_thread(callable_mp_static(&LandscapeGPU::destroy).bind(gpu));
		gpu = nullptr;
	}
#endif
}

void Landscape3D::_rebuild_patch_mesh() {
	RenderingServer *rs = RenderingServer::get_singleton();

	for (int i = 0; i < 2; i++) {
		if (multimeshes[i].is_valid()) {
			rs->free_rid(multimeshes[i]);
			multimeshes[i] = RID();
		}
	}
	if (patch_mesh.is_valid()) {
		rs->free_rid(patch_mesh);
	}

	// A regular grid of patch_size x patch_size quads. Vertex positions store grid indices,
	// the placement of each patch is resolved in the vertex shader.
	const int n = patch_size;
	PackedVector3Array vertices;
	vertices.resize((n + 1) * (n + 1));
	Vector3 *vw = vertices.ptrw();
	for (int z = 0; z <= n; z++) {
		for (int x = 0; x <= n; x++) {
			vw[z * (n + 1) + x] = Vector3(x, 0, z);
		}
	}
	PackedInt32Array indices;
	indices.resize(n * n * 6);
	int32_t *iw = indices.ptrw();
	int idx = 0;
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const int32_t v00 = z * (n + 1) + x;
			const int32_t v10 = v00 + 1;
			const int32_t v01 = v00 + (n + 1);
			const int32_t v11 = v01 + 1;
			// Alternate the diagonal for a more isotropic triangulation.
			if ((x + z) & 1) {
				iw[idx++] = v00;
				iw[idx++] = v10;
				iw[idx++] = v01;
				iw[idx++] = v10;
				iw[idx++] = v11;
				iw[idx++] = v01;
			} else {
				iw[idx++] = v00;
				iw[idx++] = v10;
				iw[idx++] = v11;
				iw[idx++] = v00;
				iw[idx++] = v11;
				iw[idx++] = v01;
			}
		}
	}

	Array arrays;
	arrays.resize(RSE::ARRAY_MAX);
	arrays[RSE::ARRAY_VERTEX] = vertices;
	arrays[RSE::ARRAY_INDEX] = indices;
	patch_mesh = rs->mesh_create();
	rs->mesh_add_surface_from_arrays(patch_mesh, RSE::PRIMITIVE_TRIANGLES, arrays);

	for (int i = 0; i < 2; i++) {
		multimeshes[i] = rs->multimesh_create();
		// Indirect MultiMesh: the instance buffer and the draw command are written by the GPU traversal.
		rs->multimesh_allocate_data(multimeshes[i], max_patches, RSE::MULTIMESH_TRANSFORM_3D, false, true, true);
		rs->multimesh_set_mesh(multimeshes[i], patch_mesh);
		rs->multimesh_set_custom_aabb(multimeshes[i], _get_local_aabb());
		rs->instance_set_base(instances[i], multimeshes[i]);
	}

	lod_resources_dirty = true;
	lod_dirty = true;
	_update_instances();
}

void Landscape3D::_update_instances() {
	RenderingServer *rs = RenderingServer::get_singleton();
	const bool in_world = is_inside_tree() && get_world_3d().is_valid();
	const RID scenario = in_world ? get_world_3d()->get_scenario() : RID();
	const bool visible = in_world && is_visible_in_tree() && data.is_valid() && data->is_valid() && rendering_supported;

	for (int i = 0; i < 2; i++) {
		if (instances[i].is_null()) {
			continue;
		}
		rs->instance_set_scenario(instances[i], scenario);
		if (in_world) {
			rs->instance_set_transform(instances[i], get_global_transform());
		}
		rs->instance_set_layer_mask(instances[i], render_layers);
		rs->instance_set_visible(instances[i], visible && (i == 0 || cast_shadows));
		rs->instance_geometry_set_cast_shadows_setting(instances[i], i == 0 ? RSE::SHADOW_CASTING_SETTING_OFF : RSE::SHADOW_CASTING_SETTING_SHADOWS_ONLY);
	}
}

void Landscape3D::_update_material_shader() {
	RenderingServer *rs = RenderingServer::get_singleton();
	Ref<Shader> shader = shader_override;
	if (shader.is_null()) {
		if (builtin_shader.is_null()) {
			builtin_shader.instantiate();
			builtin_shader->set_code(LandscapeShader::get_code());
		}
		shader = builtin_shader;
	}
	rs->material_set_shader(material, shader->get_rid());

	rs->material_set_param(material, "ls_heightmap", height_texture);
	rs->material_set_param(material, "ls_normalmap", normal_texture);
	rs->material_set_param(material, "ls_weightmaps", weights_texture);
	rs->material_set_param(material, "ls_albedo_height", albedo_height_array->get_rid());
	rs->material_set_param(material, "ls_normal_roughness", normal_roughness_array->get_rid());
	rs->material_set_param(material, "ls_debug_view", int(debug_view));
	rs->material_set_param(material, "ls_brush", brush_preview);
	rs->material_set_param(material, "ls_brush_color", brush_color);
	layers_dirty = true;
	lod_dirty = true;
}

void Landscape3D::_set_material_param(const StringName &p_name, const Variant &p_value) {
	RenderingServer::get_singleton()->material_set_param(material, p_name, p_value);
}

AABB Landscape3D::_get_local_aabb() const {
	if (data.is_null() || !data->is_valid()) {
		return AABB(Vector3(), Vector3(1, 1, 1));
	}
	const Vector2 world_size = data->get_world_size();
	Vector2 range = lod_tree.is_valid() ? lod_tree.get_height_range() : data->get_height_range();
	const real_t margin = micro_amplitude + 1.0;
	return AABB(Vector3(0, range.x - margin, 0), Vector3(world_size.x, range.y - range.x + margin * 2.0, world_size.y));
}

/* Data */

void Landscape3D::set_data(const Ref<LandscapeData> &p_data) {
	if (data == p_data) {
		return;
	}
	if (data.is_valid()) {
		data->disconnect(SNAME("region_changed"), callable_mp(this, &Landscape3D::_data_region_changed));
		data->disconnect(CoreStringName(changed), callable_mp(this, &Landscape3D::_data_changed));
	}
	data = p_data;
	if (data.is_valid()) {
		data->connect(SNAME("region_changed"), callable_mp(this, &Landscape3D::_data_region_changed));
		data->connect(CoreStringName(changed), callable_mp(this, &Landscape3D::_data_changed));
		data->ensure_layer_capacity(layers.size());
	}
	_data_changed();
	update_configuration_warnings();
	notify_property_list_changed();
}

void Landscape3D::_data_region_changed(const Rect2i &p_rect, int p_flags) {
	if (full_update_pending) {
		return;
	}
	if (p_flags & LandscapeData::CHANGED_HEIGHTS) {
		_landscape_add_dirty_rect(dirty_heights, p_rect);
	}
	if (p_flags & LandscapeData::CHANGED_WEIGHTS) {
		_landscape_add_dirty_rect(dirty_weights, p_rect);
	}
	RenderingServerDefault::redraw_request();
}

void Landscape3D::_data_changed() {
	full_update_pending = true;
	collision_full_rebuild = true;
	dirty_heights.clear();
	dirty_weights.clear();
	_update_instances();
	update_gizmos();
	RenderingServerDefault::redraw_request();
}

/* Layers */

void Landscape3D::_connect_layer(const Ref<LandscapeLayer> &p_layer, bool p_connect) {
	if (p_layer.is_null()) {
		return;
	}
	const Callable changed_cb = callable_mp(this, &Landscape3D::_layer_changed);
	const Callable textures_cb = callable_mp(this, &Landscape3D::_layer_textures_changed);
	if (p_connect) {
		if (!p_layer->is_connected(CoreStringName(changed), changed_cb)) {
			p_layer->connect(CoreStringName(changed), changed_cb);
		}
		if (!p_layer->is_connected(SNAME("textures_changed"), textures_cb)) {
			p_layer->connect(SNAME("textures_changed"), textures_cb);
		}
	} else {
		if (p_layer->is_connected(CoreStringName(changed), changed_cb)) {
			p_layer->disconnect(CoreStringName(changed), changed_cb);
		}
		if (p_layer->is_connected(SNAME("textures_changed"), textures_cb)) {
			p_layer->disconnect(SNAME("textures_changed"), textures_cb);
		}
	}
}

void Landscape3D::set_layers(const Vector<Ref<LandscapeLayer>> &p_layers) {
	for (const Ref<LandscapeLayer> &layer : layers) {
		_connect_layer(layer, false);
	}
	layers = p_layers;
	if (layers.size() > MAX_LAYERS) {
		WARN_PRINT(vformat("Landscape3D supports up to %d material layers, extra layers are ignored.", MAX_LAYERS));
		layers.resize(MAX_LAYERS);
	}
	for (const Ref<LandscapeLayer> &layer : layers) {
		_connect_layer(layer, true);
	}
	if (data.is_valid() && data->is_valid()) {
		const int old_count = data->get_weightmap_count();
		data->ensure_layer_capacity(layers.size());
		if (old_count != data->get_weightmap_count()) {
			full_update_pending = true;
		}
	}
	layers_dirty = true;
	layer_textures_dirty = true;
	RenderingServerDefault::redraw_request();
}

void Landscape3D::_set_layers_bind(const TypedArray<LandscapeLayer> &p_layers) {
	Vector<Ref<LandscapeLayer>> new_layers;
	for (int i = 0; i < p_layers.size(); i++) {
		new_layers.push_back(p_layers[i]);
	}
	set_layers(new_layers);
}

TypedArray<LandscapeLayer> Landscape3D::_get_layers_bind() const {
	TypedArray<LandscapeLayer> result;
	for (const Ref<LandscapeLayer> &layer : layers) {
		result.push_back(layer);
	}
	return result;
}

Ref<LandscapeLayer> Landscape3D::get_layer(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, layers.size(), Ref<LandscapeLayer>());
	return layers[p_index];
}

void Landscape3D::_layer_changed() {
	layers_dirty = true;
	RenderingServerDefault::redraw_request();
}

void Landscape3D::_layer_textures_changed() {
	layer_textures_dirty = true;
	RenderingServerDefault::redraw_request();
}

void Landscape3D::set_layer_texture_size(int p_size) {
	p_size = CLAMP(int(Math::next_power_of_2(uint32_t(MAX(p_size, 16)))), 16, 4096);
	if (layer_texture_size == p_size) {
		return;
	}
	layer_texture_size = p_size;
	layer_textures_dirty = true;
	layers_dirty = true;
}

void Landscape3D::set_shader_override(const Ref<Shader> &p_shader) {
	shader_override = p_shader;
	_update_material_shader();
}

String Landscape3D::get_builtin_shader_code() {
	return LandscapeShader::get_code();
}

void Landscape3D::_update_layer_params() {
	layers_dirty = false;
	Vector<Vector4> colors;
	Vector<Vector4> uvs;
	Vector<Vector4> materials;
	Vector<Vector4> extras;
	colors.resize(MAX_LAYERS);
	uvs.resize(MAX_LAYERS);
	materials.resize(MAX_LAYERS);
	extras.resize(MAX_LAYERS);

	for (int i = 0; i < MAX_LAYERS; i++) {
		Ref<LandscapeLayer> layer = i < layers.size() ? layers[i] : Ref<LandscapeLayer>();
		if (layer.is_valid()) {
			const Color c = layer->get_albedo_color().srgb_to_linear();
			const float rot = layer->get_uv_rotation();
			colors.write[i] = Vector4(c.r, c.g, c.b, layer->get_metallic());
			uvs.write[i] = Vector4(1.0 / layer->get_tile_size(), Math::cos(rot), Math::sin(rot), layer->is_triplanar() ? 1.0 : 0.0);
			materials.write[i] = Vector4(layer->get_roughness(), layer->get_normal_strength(), layer->get_height_blend(), layer->get_displacement());
			extras.write[i] = Vector4(layer->get_ao_strength(), layer->get_triplanar_sharpness(), 0.0, 0.0);
		} else {
			// Neutral default surface (also used when no layer is assigned).
			const Color c = Color(0.42, 0.42, 0.40).srgb_to_linear();
			colors.write[i] = Vector4(c.r, c.g, c.b, 0.0);
			uvs.write[i] = Vector4(0.25, 1.0, 0.0, 0.0);
			materials.write[i] = Vector4(0.9, 1.0, 0.0, 0.0);
			extras.write[i] = Vector4(1.0, 4.0, 0.0, 0.0);
		}
	}

	_set_material_param("ls_layer_color", PackedVector4Array(colors));
	_set_material_param("ls_layer_uv", PackedVector4Array(uvs));
	_set_material_param("ls_layer_material", PackedVector4Array(materials));
	_set_material_param("ls_layer_extra", PackedVector4Array(extras));
	_set_material_param("ls_layer_count", MAX(layers.size(), 1));
	_set_material_param("ls_layer_texture_size", float(layer_texture_size));
	_set_material_param("ls_weightmap_count", data.is_valid() ? MAX(data->get_weightmap_count(), 1) : 1);

	_update_micro_detail();
}

void Landscape3D::_rebuild_layer_textures() {
	layer_textures_dirty = false;
	Vector<Ref<Image>> albedo_images;
	Vector<Ref<Image>> normal_images;
	Ref<LandscapeLayer> default_layer;
	default_layer.instantiate();

	const int count = MAX(layers.size(), 2);
	for (int i = 0; i < count; i++) {
		Ref<LandscapeLayer> layer = i < layers.size() ? layers[i] : Ref<LandscapeLayer>();
		if (layer.is_null()) {
			layer = default_layer;
		}
		albedo_images.push_back(layer->build_albedo_height_image(layer_texture_size));
		normal_images.push_back(layer->build_normal_roughness_image(layer_texture_size));
	}
	albedo_height_array->create_from_images(albedo_images);
	normal_roughness_array->create_from_images(normal_images);
	_set_material_param("ls_albedo_height", albedo_height_array->get_rid());
	_set_material_param("ls_normal_roughness", normal_roughness_array->get_rid());
}

void Landscape3D::_update_micro_detail() {
	float amplitude = 0.0;
	if (micro_detail_levels > 0 && displacement_scale > 0.0) {
		for (const Ref<LandscapeLayer> &layer : layers) {
			if (layer.is_valid()) {
				amplitude = MAX(amplitude, layer->get_displacement());
			}
		}
		amplitude *= displacement_scale;
	}
	const int levels = amplitude > 0.0 ? micro_detail_levels : 0;
	micro_amplitude = amplitude;
	if (levels != micro_levels_in_use) {
		micro_levels_in_use = levels;
		lod_dirty = true;
	}
	max_level_in_use = lod_tree.get_max_level() + micro_levels_in_use;
	_set_material_param("ls_micro_levels", micro_levels_in_use);
	_set_material_param("ls_max_level", max_level_in_use);
	lod_dirty = true;
}

/* Settings */

void Landscape3D::set_patch_size(int p_size) {
	p_size = CLAMP(int(Math::next_power_of_2(uint32_t(MAX(p_size, 8)))), 8, 128);
	if (patch_size == p_size) {
		return;
	}
	patch_size = p_size;
	_rebuild_patch_mesh();
	full_update_pending = true;
}

void Landscape3D::set_lod_pixel_error(float p_error) {
	lod_pixel_error = MAX(p_error, 0.1f);
	lod_dirty = true;
}

void Landscape3D::set_micro_detail_levels(int p_levels) {
	p_levels = CLAMP(p_levels, 0, MAX_MICRO_LEVELS);
	if (micro_detail_levels == p_levels) {
		return;
	}
	micro_detail_levels = p_levels;
	lod_resources_dirty = true;
	layers_dirty = true;
}

void Landscape3D::set_displacement_scale(float p_scale) {
	displacement_scale = MAX(p_scale, 0.0f);
	layers_dirty = true;
}

void Landscape3D::set_max_patches(int p_count) {
	p_count = CLAMP(p_count, 256, 1 << 20);
	if (max_patches == p_count) {
		return;
	}
	max_patches = p_count;
	_rebuild_patch_mesh();
}

void Landscape3D::set_shadow_lod_bias(float p_bias) {
	shadow_lod_bias = MAX(p_bias, 1.0f);
	lod_dirty = true;
}

void Landscape3D::set_shadow_distance(float p_distance) {
	shadow_distance = MAX(p_distance, 0.0f);
	lod_dirty = true;
}

void Landscape3D::set_lod_camera_path(const NodePath &p_path) {
	lod_camera_path = p_path;
	lod_dirty = true;
}

void Landscape3D::set_freeze_lod(bool p_freeze) {
	freeze_lod = p_freeze;
	lod_dirty = true;
}

void Landscape3D::set_debug_view(DebugView p_view) {
	debug_view = p_view;
	_set_material_param("ls_debug_view", int(debug_view));
}

void Landscape3D::set_render_layers(uint32_t p_layers) {
	render_layers = p_layers;
	_update_instances();
}

void Landscape3D::set_cast_shadows(bool p_enable) {
	cast_shadows = p_enable;
	_update_instances();
	lod_dirty = true;
}

/* Frame update */

void Landscape3D::_frame_pre_draw() {
	if (!is_inside_tree() || data.is_null() || !data->is_valid()) {
		return;
	}
	_process_pending_changes();
	if (is_visible_in_tree()) {
		_update_lod();
	}
}

void Landscape3D::_upload_heights(const Rect2i &p_rect) {
#ifdef RD_ENABLED
	if (!gpu) {
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	const Vector2i size = data->get_size();
	const float *heights = data->get_heights_ptr();
	for (int z = p_rect.position.y; z < p_rect.get_end().y; z += UPLOAD_CHUNK) {
		for (int x = p_rect.position.x; x < p_rect.get_end().x; x += UPLOAD_CHUNK) {
			const Rect2i chunk(x, z, MIN(UPLOAD_CHUNK, p_rect.get_end().x - x), MIN(UPLOAD_CHUNK, p_rect.get_end().y - z));
			Vector<uint8_t> bytes;
			bytes.resize(int64_t(chunk.size.x) * chunk.size.y * sizeof(float));
			uint8_t *w = bytes.ptrw();
			for (int row = 0; row < chunk.size.y; row++) {
				memcpy(w + int64_t(row) * chunk.size.x * sizeof(float), heights + int64_t(chunk.position.y + row) * size.x + chunk.position.x, chunk.size.x * sizeof(float));
			}
			rs->call_on_render_thread(callable_mp(gpu, &LandscapeGPU::upload_heights).bind(chunk, bytes));
		}
	}
#endif
}

void Landscape3D::_upload_weights(const Rect2i &p_rect) {
#ifdef RD_ENABLED
	if (!gpu) {
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	const Vector2i size = data->get_size();
	for (int m = 0; m < data->get_weightmap_count(); m++) {
		const uint8_t *weights = data->get_weightmap_ptr(m);
		for (int z = p_rect.position.y; z < p_rect.get_end().y; z += UPLOAD_CHUNK) {
			for (int x = p_rect.position.x; x < p_rect.get_end().x; x += UPLOAD_CHUNK) {
				const Rect2i chunk(x, z, MIN(UPLOAD_CHUNK, p_rect.get_end().x - x), MIN(UPLOAD_CHUNK, p_rect.get_end().y - z));
				Vector<uint8_t> bytes;
				bytes.resize(int64_t(chunk.size.x) * chunk.size.y * 4);
				uint8_t *w = bytes.ptrw();
				for (int row = 0; row < chunk.size.y; row++) {
					memcpy(w + int64_t(row) * chunk.size.x * 4, weights + (int64_t(chunk.position.y + row) * size.x + chunk.position.x) * 4, chunk.size.x * 4);
				}
				rs->call_on_render_thread(callable_mp(gpu, &LandscapeGPU::upload_weights).bind(chunk, m, bytes));
			}
		}
	}
#endif
}

void Landscape3D::_upload_bounds(const LocalVector<LandscapeLodTree::Range> &p_ranges) {
#ifdef RD_ENABLED
	if (!gpu) {
		return;
	}
	const LocalVector<float> &nodes = lod_tree.get_nodes();
	for (const LandscapeLodTree::Range &range : p_ranges) {
		if (range.count == 0) {
			continue;
		}
		Vector<float> chunk;
		chunk.resize(range.count);
		memcpy(chunk.ptrw(), nodes.ptr() + range.offset, range.count * sizeof(float));
		RenderingServer::get_singleton()->call_on_render_thread(callable_mp(gpu, &LandscapeGPU::upload_bounds).bind(int(range.offset), chunk));
	}
#endif
}

void Landscape3D::_full_update() {
	full_update_pending = false;
	dirty_heights.clear();
	dirty_weights.clear();

	lod_tree.build(data.ptr(), patch_size);
	const Vector2i size = data->get_size();
	const Rect2i full(Point2i(), size);

#ifdef RD_ENABLED
	if (gpu) {
		RenderingServer *rs = RenderingServer::get_singleton();
		rs->call_on_render_thread(callable_mp(gpu, &LandscapeGPU::setup_maps).bind(size, data->get_weightmap_count(), height_texture, normal_texture, weights_texture));
		_upload_heights(full);
		_upload_weights(full);
		rs->call_on_render_thread(callable_mp(gpu, &LandscapeGPU::update_normals).bind(full, float(data->get_vertex_spacing())));
		rs->call_on_render_thread(callable_mp(gpu, &LandscapeGPU::update_weight_mips).bind(full));
	}
#endif

	_set_material_param("ls_size", size);
	_set_material_param("ls_spacing", float(data->get_vertex_spacing()));
	_set_material_param("ls_patch_quads", patch_size);
	_set_material_param("ls_patch_quads_log2", int(Math::get_shift_from_power_of_2(uint32_t(patch_size))));
	_set_material_param("ls_weightmap_count", MAX(data->get_weightmap_count(), 1));

	lod_resources_dirty = true;
	layers_dirty = true;
	lod_dirty = true;
	collision_full_rebuild = true;

	for (int i = 0; i < 2; i++) {
		RenderingServer::get_singleton()->multimesh_set_custom_aabb(multimeshes[i], _get_local_aabb());
	}
	_update_instances();
	update_gizmos();
}

void Landscape3D::_process_pending_changes() {
	if (full_update_pending) {
		_full_update();
	}

	if (layers_dirty) {
		_update_layer_params();
	}
	if (layer_textures_dirty) {
		_rebuild_layer_textures();
	}

#ifdef RD_ENABLED
	if (lod_resources_dirty && lod_tree.is_valid() && gpu) {
		lod_resources_dirty = false;
		const int max_level = lod_tree.get_max_level() + micro_detail_levels;
		RenderingServer::get_singleton()->call_on_render_thread(callable_mp(gpu, &LandscapeGPU::setup_lod).bind(int(lod_tree.get_node_count()), max_patches, max_patches, max_level, multimeshes[0], multimeshes[1]));
		LocalVector<LandscapeLodTree::Range> ranges;
		ranges.push_back({ 0, lod_tree.get_nodes().size() });
		_upload_bounds(ranges);
		lod_dirty = true;
	}
#endif

	if (!dirty_heights.is_empty()) {
		const Vector2 old_range = lod_tree.get_height_range();
		for (const Rect2i &rect : dirty_heights) {
			LocalVector<LandscapeLodTree::Range> ranges;
			lod_tree.update(data.ptr(), rect, &ranges);
			_upload_bounds(ranges);
			_upload_heights(rect);
#ifdef RD_ENABLED
			if (gpu) {
				RenderingServer::get_singleton()->call_on_render_thread(callable_mp(gpu, &LandscapeGPU::update_normals).bind(rect, float(data->get_vertex_spacing())));
			}
#endif
			_mark_collision_dirty(rect);
		}
		dirty_heights.clear();
		if (lod_tree.get_height_range() != old_range) {
			for (int i = 0; i < 2; i++) {
				RenderingServer::get_singleton()->multimesh_set_custom_aabb(multimeshes[i], _get_local_aabb());
			}
			update_gizmos();
		}
		lod_dirty = true;
	}

	if (!dirty_weights.is_empty()) {
		for (const Rect2i &rect : dirty_weights) {
			_upload_weights(rect);
#ifdef RD_ENABLED
			if (gpu) {
				RenderingServer::get_singleton()->call_on_render_thread(callable_mp(gpu, &LandscapeGPU::update_weight_mips).bind(rect));
			}
#endif
		}
		dirty_weights.clear();
	}
}

Camera3D *Landscape3D::_get_lod_camera() const {
	Camera3D *camera = nullptr;
	if (!lod_camera_path.is_empty()) {
		camera = Object::cast_to<Camera3D>(get_node_or_null(lod_camera_path));
	}
#ifdef TOOLS_ENABLED
	if (!camera && Engine::get_singleton()->is_editor_hint() && editor_camera_callback && is_part_of_edited_scene()) {
		camera = editor_camera_callback();
	}
#endif
	if (!camera && get_viewport()) {
		camera = get_viewport()->get_camera_3d();
	}
	return camera;
}

void Landscape3D::_update_lod() {
#ifdef RD_ENABLED
	if (!gpu || !lod_tree.is_valid() || freeze_lod) {
		return;
	}
	Camera3D *camera = _get_lod_camera();
	if (!camera || !camera->is_inside_tree() || !camera->get_viewport()) {
		return;
	}

	const Transform3D global = get_global_transform();
	const Transform3D inverse = global.affine_inverse();
	const Transform3D camera_xform = camera->get_camera_transform();
	const Projection projection = camera->get_camera_projection();
	const bool orthogonal = camera->get_projection() == Camera3D::PROJECTION_ORTHOGONAL;
	const real_t viewport_height = MAX(camera->get_viewport()->get_visible_rect().size.y, real_t(1.0));
	const Vector3 scale = global.basis.get_scale();
	const real_t avg_scale = MAX((Math::abs(scale.x) + Math::abs(scale.y) + Math::abs(scale.z)) / 3.0, CMP_EPSILON);

	// Pixels per local unit at distance 1 (perspective) or at any distance (orthogonal).
	real_t projection_factor = viewport_height * 0.5 * projection.columns[1][1];
	if (orthogonal) {
		projection_factor *= avg_scale;
	}

	const Vector3 camera_local = inverse.xform(camera_xform.origin);
	const int micro = micro_levels_in_use;
	const int heightmap_level = lod_tree.get_max_level();
	const int max_level = heightmap_level + micro;
	const Vector2i size = data->get_size();

	LandscapeLodParams params;
	const Vector<Plane> planes = camera->get_frustum();
	for (int i = 0; i < 6 && i < planes.size(); i++) {
		// Transform the world plane into landscape local space.
		const Plane &p = planes[i];
		Vector3 normal = global.basis.xform_inv(p.normal);
		real_t d = p.d - p.normal.dot(global.origin);
		const real_t len = normal.length();
		if (len > CMP_EPSILON) {
			normal /= len;
			d /= len;
		}
		params.planes[i][0] = normal.x;
		params.planes[i][1] = normal.y;
		params.planes[i][2] = normal.z;
		params.planes[i][3] = d;
	}
	params.camera[0] = camera_local.x;
	params.camera[1] = camera_local.y;
	params.camera[2] = camera_local.z;
	params.camera[3] = projection_factor;
	params.lod[0] = lod_pixel_error;
	params.lod[1] = orthogonal ? 0.0 : micro_amplitude;
	params.lod[2] = data->get_vertex_spacing() * 0.5;
	params.lod[3] = orthogonal ? 1.0 : 0.0;
	params.unit[0] = data->get_vertex_spacing() / real_t(1 << micro);
	params.unit[1] = 0.0;
	params.grid[0] = patch_size;
	params.grid[1] = heightmap_level;
	params.grid[2] = max_level;
	params.grid[3] = micro;
	params.limits[0] = uint32_t(size.x - 1) << micro;
	params.limits[1] = uint32_t(size.y - 1) << micro;
	params.limits[2] = max_patches;
	params.limits[3] = max_patches;
	params.flags[0] = 1;
	uint32_t split_offset = 0;
	for (int l = 0; l < 16; l++) {
		params.bounds_offsets[l] = lod_tree.get_level_offset(MIN(l, heightmap_level));
		params.split_offsets[l] = split_offset;
		split_offset += _landscape_split_words(l);
	}

	RenderingServer *rs = RenderingServer::get_singleton();
	for (int list = 0; list < 2; list++) {
		if (list == LandscapeGPU::LIST_SHADOW) {
			if (!cast_shadows) {
				break;
			}
			params.lod[0] = lod_pixel_error * shadow_lod_bias;
			params.flags[0] = 0; // Shadow casters outside of the view frustum still matter.
			params.unit[1] = shadow_distance / avg_scale;
		}
		Vector<uint8_t> bytes;
		bytes.resize(sizeof(LandscapeLodParams));
		memcpy(bytes.ptrw(), &params, sizeof(LandscapeLodParams));
		if (lod_dirty || bytes != last_lod_params[list]) {
			last_lod_params[list] = bytes;
			rs->call_on_render_thread(callable_mp(gpu, &LandscapeGPU::run_lod).bind(list, bytes, max_level));
		}
	}
	lod_dirty = false;

	if (!camera_local.is_equal_approx(last_lod_camera)) {
		last_lod_camera = camera_local;
		_set_material_param("ls_lod_camera", camera_local);
	}

	Vector4 micro_params;
	if (micro > 0 && micro_amplitude > 0.0 && !orthogonal) {
		const real_t fade_end = micro_amplitude * projection_factor / lod_pixel_error;
		micro_params.x = fade_end * 0.5;
		micro_params.y = fade_end;
		micro_params.z = data->get_vertex_spacing() * lod_pixel_error / (micro_amplitude * projection_factor);
		micro_params.w = displacement_scale;
	}
	if (!micro_params.is_equal_approx(last_micro_params)) {
		last_micro_params = micro_params;
		_set_material_param("ls_micro_params", micro_params);
	}
#endif
}

Dictionary Landscape3D::get_statistics() const {
	Dictionary stats;
	uint32_t patches = 0;
	uint32_t shadow_patches = 0;
	uint32_t overflow = 0;
#ifdef RD_ENABLED
	if (gpu) {
		patches = gpu->get_patch_count(LandscapeGPU::LIST_MAIN);
		shadow_patches = cast_shadows ? gpu->get_patch_count(LandscapeGPU::LIST_SHADOW) : 0;
		overflow = gpu->get_overflow_flags(LandscapeGPU::LIST_MAIN) | gpu->get_overflow_flags(LandscapeGPU::LIST_SHADOW);
	}
#endif
	stats["patches"] = patches;
	stats["shadow_patches"] = shadow_patches;
	stats["triangles"] = uint64_t(patches) * patch_size * patch_size * 2;
	stats["budget_exceeded"] = overflow != 0;
	stats["heightmap_levels"] = lod_tree.get_max_level() + 1;
	stats["micro_levels"] = micro_levels_in_use;
	return stats;
}

void Landscape3D::force_update() {
	full_update_pending = true;
	layers_dirty = true;
	layer_textures_dirty = true;
	collision_full_rebuild = true;
	RenderingServerDefault::redraw_request();
}

/* Collision */

void Landscape3D::_clear_collision() {
#ifndef PHYSICS_3D_DISABLED
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	for (KeyValue<Vector2i, CollisionTile> &kv : collision_tiles) {
		if (kv.value.body.is_valid()) {
			ps->free_rid(kv.value.body);
		}
		if (kv.value.shape.is_valid()) {
			ps->free_rid(kv.value.shape);
		}
	}
	collision_tiles.clear();
	collision_last_center = Vector3(Math::INF, Math::INF, Math::INF);
#endif // PHYSICS_3D_DISABLED
}

void Landscape3D::_mark_collision_dirty(const Rect2i &p_rect) {
	if (collision_dirty_rect.has_area()) {
		collision_dirty_rect = collision_dirty_rect.merge(p_rect);
	} else {
		collision_dirty_rect = p_rect;
	}
	collision_timer = Engine::get_singleton()->is_editor_hint() ? 0.3 : 0.0;
}

void Landscape3D::_build_collision_tile(const Vector2i &p_tile, CollisionTile &r_tile) {
#ifndef PHYSICS_3D_DISABLED
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	const Vector2i size = data->get_size();
	const int step = 1 << collision_lod;
	const int x0 = p_tile.x * collision_tile_size;
	const int z0 = p_tile.y * collision_tile_size;
	const int x1 = MIN(x0 + collision_tile_size, size.x - 1);
	const int z1 = MIN(z0 + collision_tile_size, size.y - 1);
	const int width = (x1 - x0 + step - 1) / step + 1;
	const int depth = (z1 - z0 + step - 1) / step + 1;

	PackedFloat32Array heights;
	heights.resize(width * depth);
	float *w = heights.ptrw();
	float min_h = Math::INF;
	float max_h = -Math::INF;
	for (int j = 0; j < depth; j++) {
		const int z = MIN(z0 + j * step, z1);
		for (int i = 0; i < width; i++) {
			const int x = MIN(x0 + i * step, x1);
			const float h = data->get_height_fast(x, z);
			w[j * width + i] = h;
			min_h = MIN(min_h, h);
			max_h = MAX(max_h, h);
		}
	}

	if (r_tile.shape.is_null()) {
		r_tile.shape = ps->heightmap_shape_create();
	}
	Dictionary shape_data;
	shape_data["width"] = width;
	shape_data["depth"] = depth;
	shape_data["heights"] = heights;
	shape_data["min_height"] = min_h;
	shape_data["max_height"] = max_h;
	ps->shape_set_data(r_tile.shape, shape_data);

	const real_t spacing = data->get_vertex_spacing();
	const real_t cell = spacing * step;
	// HeightMapShape3D is centered on its origin.
	const Vector3 center(x0 * spacing + (width - 1) * cell * 0.5, 0.0, z0 * spacing + (depth - 1) * cell * 0.5);
	const Transform3D shape_xform(Basis::from_scale(Vector3(cell, 1.0, cell)), center);

	if (r_tile.body.is_null()) {
		r_tile.body = ps->body_create();
		ps->body_set_mode(r_tile.body, PS3DE::BODY_MODE_STATIC);
		ps->body_attach_object_instance_id(r_tile.body, get_instance_id());
		ps->body_add_shape(r_tile.body, r_tile.shape, shape_xform);
	} else {
		ps->body_set_shape_transform(r_tile.body, 0, shape_xform);
	}
	ps->body_set_collision_layer(r_tile.body, collision_layer);
	ps->body_set_collision_mask(r_tile.body, collision_mask);
	ps->body_set_collision_priority(r_tile.body, collision_priority);
	ps->body_set_state(r_tile.body, PS3DE::BODY_STATE_TRANSFORM, get_global_transform());
	ps->body_set_space(r_tile.body, get_world_3d()->get_space());
	r_tile.dirty = false;
#endif // PHYSICS_3D_DISABLED
}

void Landscape3D::_update_collision() {
#ifndef PHYSICS_3D_DISABLED
	if (!collision_enabled || !is_inside_tree() || data.is_null() || !data->is_valid() || get_world_3d().is_null()) {
		if (!collision_tiles.is_empty()) {
			_clear_collision();
		}
		return;
	}

	const Vector2i size = data->get_size();
	const Vector2i tile_count((size.x - 2) / collision_tile_size + 1, (size.y - 2) / collision_tile_size + 1);

	if (collision_full_rebuild) {
		collision_full_rebuild = false;
		_clear_collision();
		collision_dirty_rect = Rect2i();
	}

	// Apply pending height changes (debounced while editing).
	if (collision_dirty_rect.has_area()) {
		collision_timer -= get_process_delta_time();
		if (collision_timer <= 0.0) {
			// Tiles share their border samples, so also refresh the neighbors of the dirty area.
			const Vector2i begin = (collision_dirty_rect.position - Vector2i(1, 1)).maxi(0) / collision_tile_size;
			const Vector2i end = (collision_dirty_rect.get_end() + Vector2i(1, 1)) / collision_tile_size;
			for (int z = begin.y; z <= MIN(end.y, tile_count.y - 1); z++) {
				for (int x = begin.x; x <= MIN(end.x, tile_count.x - 1); x++) {
					CollisionTile *tile = collision_tiles.getptr(Vector2i(x, z));
					if (tile) {
						tile->dirty = true;
					}
				}
			}
			collision_dirty_rect = Rect2i();
		}
	}

	// Select the tiles that should exist (all of them, or those around the LOD camera).
	Rect2i wanted(Point2i(), tile_count);
	if (collision_radius > 0.0) {
		Vector3 center;
		Camera3D *camera = _get_lod_camera();
		if (camera) {
			center = global_to_local(camera->get_global_transform().origin);
		}
		const real_t tile_world = collision_tile_size * data->get_vertex_spacing();
		const Vector2i begin = Vector2i(Math::floor((center.x - collision_radius) / tile_world), Math::floor((center.z - collision_radius) / tile_world));
		const Vector2i end = Vector2i(Math::floor((center.x + collision_radius) / tile_world), Math::floor((center.z + collision_radius) / tile_world));
		wanted = Rect2i(begin, end - begin + Vector2i(1, 1)).intersection(Rect2i(Point2i(), tile_count));

		LocalVector<Vector2i> to_remove;
		for (const KeyValue<Vector2i, CollisionTile> &kv : collision_tiles) {
			if (!wanted.has_point(kv.key)) {
				to_remove.push_back(kv.key);
			}
		}
		PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
		for (const Vector2i &key : to_remove) {
			CollisionTile &tile = collision_tiles[key];
			ps->free_rid(tile.body);
			ps->free_rid(tile.shape);
			collision_tiles.erase(key);
		}
	}

	for (int z = wanted.position.y; z < wanted.get_end().y; z++) {
		for (int x = wanted.position.x; x < wanted.get_end().x; x++) {
			const Vector2i key(x, z);
			CollisionTile *tile = collision_tiles.getptr(key);
			if (!tile) {
				tile = &collision_tiles.insert(key, CollisionTile())->value;
			}
			if (tile->dirty) {
				_build_collision_tile(key, *tile);
			}
		}
	}
#endif // PHYSICS_3D_DISABLED
}

void Landscape3D::update_collision() {
	collision_full_rebuild = true;
	_update_collision();
}

void Landscape3D::set_collision_enabled(bool p_enabled) {
	collision_enabled = p_enabled;
	collision_full_rebuild = true;
	if (!collision_enabled) {
		_clear_collision();
	}
}

void Landscape3D::set_collision_layer(uint32_t p_layer) {
	collision_layer = p_layer;
#ifndef PHYSICS_3D_DISABLED
	for (KeyValue<Vector2i, CollisionTile> &kv : collision_tiles) {
		PhysicsServer3D::get_singleton()->body_set_collision_layer(kv.value.body, collision_layer);
	}
#endif // PHYSICS_3D_DISABLED
}

void Landscape3D::set_collision_mask(uint32_t p_mask) {
	collision_mask = p_mask;
#ifndef PHYSICS_3D_DISABLED
	for (KeyValue<Vector2i, CollisionTile> &kv : collision_tiles) {
		PhysicsServer3D::get_singleton()->body_set_collision_mask(kv.value.body, collision_mask);
	}
#endif // PHYSICS_3D_DISABLED
}

void Landscape3D::set_collision_priority(real_t p_priority) {
	collision_priority = p_priority;
#ifndef PHYSICS_3D_DISABLED
	for (KeyValue<Vector2i, CollisionTile> &kv : collision_tiles) {
		PhysicsServer3D::get_singleton()->body_set_collision_priority(kv.value.body, collision_priority);
	}
#endif // PHYSICS_3D_DISABLED
}

void Landscape3D::set_collision_tile_size(int p_size) {
	p_size = CLAMP(int(Math::next_power_of_2(uint32_t(MAX(p_size, 16)))), 16, 4096);
	collision_tile_size = p_size;
	collision_full_rebuild = true;
}

void Landscape3D::set_collision_lod(int p_lod) {
	collision_lod = CLAMP(p_lod, 0, 4);
	collision_full_rebuild = true;
}

void Landscape3D::set_collision_radius(real_t p_radius) {
	collision_radius = MAX(p_radius, real_t(0.0));
	collision_full_rebuild = true;
}

/* Queries */

Vector3 Landscape3D::global_to_local(const Vector3 &p_global) const {
	return get_global_transform().affine_inverse().xform(p_global);
}

Vector3 Landscape3D::local_to_global(const Vector3 &p_local) const {
	return get_global_transform().xform(p_local);
}

Vector2 Landscape3D::local_to_texel(const Vector3 &p_local) const {
	if (data.is_null()) {
		return Vector2();
	}
	return Vector2(p_local.x, p_local.z) / data->get_vertex_spacing();
}

real_t Landscape3D::get_height_at(const Vector3 &p_global_position) const {
	ERR_FAIL_COND_V(data.is_null() || !data->is_valid(), 0.0);
	const Vector3 local = global_to_local(p_global_position);
	const real_t h = data->sample_height(local.x, local.z);
	return local_to_global(Vector3(local.x, h, local.z)).y;
}

Vector3 Landscape3D::get_normal_at(const Vector3 &p_global_position) const {
	ERR_FAIL_COND_V(data.is_null() || !data->is_valid(), Vector3(0, 1, 0));
	const Vector3 local = global_to_local(p_global_position);
	const Vector3 n = data->sample_normal(local.x, local.z);
	return get_global_transform().basis.inverse().transposed().xform(n).normalized();
}

int Landscape3D::get_dominant_layer_at(const Vector3 &p_global_position) const {
	ERR_FAIL_COND_V(data.is_null() || !data->is_valid(), 0);
	const Vector2 texel = local_to_texel(global_to_local(p_global_position));
	return data->get_dominant_layer(int(Math::round(texel.x)), int(Math::round(texel.y)));
}

bool Landscape3D::intersect_ray(const Vector3 &p_from, const Vector3 &p_direction, Vector3 &r_position, Vector3 &r_normal, real_t p_max_distance) const {
	if (data.is_null() || !data->is_valid() || p_direction.is_zero_approx()) {
		return false;
	}
	const Transform3D global = get_global_transform();
	const Transform3D inverse = global.affine_inverse();
	const Vector3 from = inverse.xform(p_from);
	Vector3 dir = inverse.basis.xform(p_direction.normalized());
	const real_t local_length_scale = dir.length();
	dir /= local_length_scale;
	const real_t max_distance = p_max_distance * local_length_scale;

	AABB aabb = _get_local_aabb();
	Vector3 clip_from;
	Vector3 clip_to;
	real_t t_begin = 0.0;
	real_t t_end = max_distance;
	{
		// Clip the ray against the landscape bounds (slab test).
		for (int axis = 0; axis < 3; axis++) {
			const real_t origin = from[axis];
			const real_t d = dir[axis];
			const real_t mn = aabb.position[axis];
			const real_t mx = aabb.position[axis] + aabb.size[axis];
			if (Math::abs(d) < CMP_EPSILON) {
				if (origin < mn || origin > mx) {
					return false;
				}
				continue;
			}
			real_t t0 = (mn - origin) / d;
			real_t t1 = (mx - origin) / d;
			if (t0 > t1) {
				SWAP(t0, t1);
			}
			t_begin = MAX(t_begin, t0);
			t_end = MIN(t_end, t1);
			if (t_begin > t_end) {
				return false;
			}
		}
	}

	const real_t step = data->get_vertex_spacing() * 0.5;
	real_t prev_t = t_begin;
	Vector3 p = from + dir * t_begin;
	real_t prev_diff = p.y - data->sample_height(p.x, p.z);
	if (prev_diff <= 0.0) {
		r_position = local_to_global(p);
		r_normal = get_normal_at(r_position);
		return true;
	}
	for (real_t t = t_begin + step; t <= t_end + step; t += step) {
		const real_t tc = MIN(t, t_end);
		p = from + dir * tc;
		const real_t diff = p.y - data->sample_height(p.x, p.z);
		if (diff <= 0.0) {
			// Refine the intersection with a few bisection steps.
			real_t a = prev_t;
			real_t b = tc;
			for (int i = 0; i < 16; i++) {
				const real_t m = (a + b) * 0.5;
				const Vector3 pm = from + dir * m;
				if (pm.y - data->sample_height(pm.x, pm.z) > 0.0) {
					a = m;
				} else {
					b = m;
				}
			}
			const Vector3 hit = from + dir * b;
			r_position = local_to_global(Vector3(hit.x, data->sample_height(hit.x, hit.z), hit.z));
			r_normal = get_normal_at(r_position);
			return true;
		}
		prev_t = tc;
		prev_diff = diff;
		if (tc >= t_end) {
			break;
		}
	}
	return false;
}

Dictionary Landscape3D::raycast(const Vector3 &p_from, const Vector3 &p_direction, real_t p_max_distance) const {
	Dictionary result;
	Vector3 position;
	Vector3 normal;
	if (intersect_ray(p_from, p_direction, position, normal, p_max_distance)) {
		result["position"] = position;
		result["normal"] = normal;
	}
	return result;
}

AABB Landscape3D::get_aabb() const {
	return _get_local_aabb();
}

Ref<TriangleMesh> Landscape3D::generate_selection_mesh(int p_resolution) const {
	Ref<TriangleMesh> mesh;
	if (data.is_null() || !data->is_valid()) {
		return mesh;
	}
	p_resolution = CLAMP(p_resolution, 2, 256);
	const Vector2 world_size = data->get_world_size();
	Vector<Vector3> faces;
	faces.resize(p_resolution * p_resolution * 6);
	Vector3 *w = faces.ptrw();
	int idx = 0;
	auto vertex = [&](int p_x, int p_z) {
		const real_t lx = world_size.x * p_x / p_resolution;
		const real_t lz = world_size.y * p_z / p_resolution;
		return Vector3(lx, data->sample_height(lx, lz), lz);
	};
	for (int z = 0; z < p_resolution; z++) {
		for (int x = 0; x < p_resolution; x++) {
			const Vector3 v00 = vertex(x, z);
			const Vector3 v10 = vertex(x + 1, z);
			const Vector3 v01 = vertex(x, z + 1);
			const Vector3 v11 = vertex(x + 1, z + 1);
			w[idx++] = v00;
			w[idx++] = v10;
			w[idx++] = v11;
			w[idx++] = v00;
			w[idx++] = v11;
			w[idx++] = v01;
		}
	}
	mesh.instantiate();
	mesh->create(faces);
	return mesh;
}

void Landscape3D::set_brush_preview(bool p_visible, const Vector3 &p_local_center, real_t p_radius, real_t p_falloff, const Color &p_color) {
	const Vector4 preview = p_visible ? Vector4(p_local_center.x, p_local_center.z, p_radius, p_falloff) : Vector4();
	if (preview != brush_preview) {
		brush_preview = preview;
		_set_material_param("ls_brush", brush_preview);
	}
	if (p_color != brush_color) {
		brush_color = p_color;
		_set_material_param("ls_brush_color", brush_color);
	}
}

PackedStringArray Landscape3D::get_configuration_warnings() const {
	PackedStringArray warnings = Node3D::get_configuration_warnings();
	if (data.is_null()) {
		warnings.push_back(RTR("No LandscapeData is assigned. Create one in the Landscape editor dock (Manage tab) or assign an existing resource."));
	} else if (!data->is_valid()) {
		warnings.push_back(RTR("The assigned LandscapeData is empty."));
	}
	if (!rendering_supported && RenderingServer::get_singleton()->get_rendering_device() == nullptr && !OS::get_singleton()->get_current_rendering_method().is_empty() && OS::get_singleton()->get_current_rendering_method() == "gl_compatibility") {
		warnings.push_back(RTR("Landscape3D uses GPU compute (quadtree traversal and indirect drawing) and requires the Forward+ or Mobile renderer."));
	}
	return warnings;
}

void Landscape3D::cleanup_shared_resources() {
	builtin_shader.unref();
}

/* Scene */

void Landscape3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			RenderingServer::get_singleton()->connect(SNAME("frame_pre_draw"), callable_mp(this, &Landscape3D::_frame_pre_draw));
		} break;

		case NOTIFICATION_EXIT_TREE: {
			RenderingServer::get_singleton()->disconnect(SNAME("frame_pre_draw"), callable_mp(this, &Landscape3D::_frame_pre_draw));
		} break;

		case NOTIFICATION_ENTER_WORLD: {
			_update_instances();
			collision_full_rebuild = true;
			set_process_internal(true);
		} break;

		case NOTIFICATION_EXIT_WORLD: {
			_update_instances();
			_clear_collision();
			set_process_internal(false);
		} break;

		case NOTIFICATION_TRANSFORM_CHANGED: {
			RenderingServer *rs = RenderingServer::get_singleton();
			for (int i = 0; i < 2; i++) {
				rs->instance_set_transform(instances[i], get_global_transform());
			}
#ifndef PHYSICS_3D_DISABLED
			for (KeyValue<Vector2i, CollisionTile> &kv : collision_tiles) {
				PhysicsServer3D::get_singleton()->body_set_state(kv.value.body, PS3DE::BODY_STATE_TRANSFORM, get_global_transform());
			}
#endif // PHYSICS_3D_DISABLED
			lod_dirty = true;
		} break;

		case NOTIFICATION_VISIBILITY_CHANGED: {
			_update_instances();
		} break;

		case NOTIFICATION_INTERNAL_PROCESS: {
			_update_collision();
		} break;
	}
}

void Landscape3D::_validate_property(PropertyInfo &p_property) const {
	if (p_property.name == "shadow_lod_bias" || p_property.name == "shadow_distance") {
		if (!cast_shadows) {
			p_property.usage |= PROPERTY_USAGE_READ_ONLY;
		}
	}
	if (p_property.name.begins_with("collision_") && p_property.name != "collision_enabled" && !collision_enabled) {
		p_property.usage |= PROPERTY_USAGE_READ_ONLY;
	}
}

void Landscape3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_data", "data"), &Landscape3D::set_data);
	ClassDB::bind_method(D_METHOD("get_data"), &Landscape3D::get_data);
	ClassDB::bind_method(D_METHOD("set_layers", "layers"), &Landscape3D::_set_layers_bind);
	ClassDB::bind_method(D_METHOD("get_layers"), &Landscape3D::_get_layers_bind);
	ClassDB::bind_method(D_METHOD("get_layer_count"), &Landscape3D::get_layer_count);
	ClassDB::bind_method(D_METHOD("get_layer", "index"), &Landscape3D::get_layer);
	ClassDB::bind_method(D_METHOD("set_layer_texture_size", "size"), &Landscape3D::set_layer_texture_size);
	ClassDB::bind_method(D_METHOD("get_layer_texture_size"), &Landscape3D::get_layer_texture_size);
	ClassDB::bind_method(D_METHOD("set_shader_override", "shader"), &Landscape3D::set_shader_override);
	ClassDB::bind_method(D_METHOD("get_shader_override"), &Landscape3D::get_shader_override);
	ClassDB::bind_static_method("Landscape3D", D_METHOD("get_builtin_shader_code"), &Landscape3D::get_builtin_shader_code);

	ClassDB::bind_method(D_METHOD("set_patch_size", "size"), &Landscape3D::set_patch_size);
	ClassDB::bind_method(D_METHOD("get_patch_size"), &Landscape3D::get_patch_size);
	ClassDB::bind_method(D_METHOD("set_lod_pixel_error", "error"), &Landscape3D::set_lod_pixel_error);
	ClassDB::bind_method(D_METHOD("get_lod_pixel_error"), &Landscape3D::get_lod_pixel_error);
	ClassDB::bind_method(D_METHOD("set_micro_detail_levels", "levels"), &Landscape3D::set_micro_detail_levels);
	ClassDB::bind_method(D_METHOD("get_micro_detail_levels"), &Landscape3D::get_micro_detail_levels);
	ClassDB::bind_method(D_METHOD("set_displacement_scale", "scale"), &Landscape3D::set_displacement_scale);
	ClassDB::bind_method(D_METHOD("get_displacement_scale"), &Landscape3D::get_displacement_scale);
	ClassDB::bind_method(D_METHOD("set_max_patches", "count"), &Landscape3D::set_max_patches);
	ClassDB::bind_method(D_METHOD("get_max_patches"), &Landscape3D::get_max_patches);
	ClassDB::bind_method(D_METHOD("set_shadow_lod_bias", "bias"), &Landscape3D::set_shadow_lod_bias);
	ClassDB::bind_method(D_METHOD("get_shadow_lod_bias"), &Landscape3D::get_shadow_lod_bias);
	ClassDB::bind_method(D_METHOD("set_shadow_distance", "distance"), &Landscape3D::set_shadow_distance);
	ClassDB::bind_method(D_METHOD("get_shadow_distance"), &Landscape3D::get_shadow_distance);
	ClassDB::bind_method(D_METHOD("set_lod_camera_path", "path"), &Landscape3D::set_lod_camera_path);
	ClassDB::bind_method(D_METHOD("get_lod_camera_path"), &Landscape3D::get_lod_camera_path);
	ClassDB::bind_method(D_METHOD("set_freeze_lod", "freeze"), &Landscape3D::set_freeze_lod);
	ClassDB::bind_method(D_METHOD("is_lod_frozen"), &Landscape3D::is_lod_frozen);
	ClassDB::bind_method(D_METHOD("set_debug_view", "view"), &Landscape3D::set_debug_view);
	ClassDB::bind_method(D_METHOD("get_debug_view"), &Landscape3D::get_debug_view);

	ClassDB::bind_method(D_METHOD("set_render_layers", "layers"), &Landscape3D::set_render_layers);
	ClassDB::bind_method(D_METHOD("get_render_layers"), &Landscape3D::get_render_layers);
	ClassDB::bind_method(D_METHOD("set_cast_shadows", "enable"), &Landscape3D::set_cast_shadows);
	ClassDB::bind_method(D_METHOD("is_casting_shadows"), &Landscape3D::is_casting_shadows);

	ClassDB::bind_method(D_METHOD("set_collision_enabled", "enabled"), &Landscape3D::set_collision_enabled);
	ClassDB::bind_method(D_METHOD("is_collision_enabled"), &Landscape3D::is_collision_enabled);
	ClassDB::bind_method(D_METHOD("set_collision_layer", "layer"), &Landscape3D::set_collision_layer);
	ClassDB::bind_method(D_METHOD("get_collision_layer"), &Landscape3D::get_collision_layer);
	ClassDB::bind_method(D_METHOD("set_collision_mask", "mask"), &Landscape3D::set_collision_mask);
	ClassDB::bind_method(D_METHOD("get_collision_mask"), &Landscape3D::get_collision_mask);
	ClassDB::bind_method(D_METHOD("set_collision_priority", "priority"), &Landscape3D::set_collision_priority);
	ClassDB::bind_method(D_METHOD("get_collision_priority"), &Landscape3D::get_collision_priority);
	ClassDB::bind_method(D_METHOD("set_collision_tile_size", "size"), &Landscape3D::set_collision_tile_size);
	ClassDB::bind_method(D_METHOD("get_collision_tile_size"), &Landscape3D::get_collision_tile_size);
	ClassDB::bind_method(D_METHOD("set_collision_lod", "lod"), &Landscape3D::set_collision_lod);
	ClassDB::bind_method(D_METHOD("get_collision_lod"), &Landscape3D::get_collision_lod);
	ClassDB::bind_method(D_METHOD("set_collision_radius", "radius"), &Landscape3D::set_collision_radius);
	ClassDB::bind_method(D_METHOD("get_collision_radius"), &Landscape3D::get_collision_radius);
	ClassDB::bind_method(D_METHOD("update_collision"), &Landscape3D::update_collision);

	ClassDB::bind_method(D_METHOD("get_height_at", "global_position"), &Landscape3D::get_height_at);
	ClassDB::bind_method(D_METHOD("get_normal_at", "global_position"), &Landscape3D::get_normal_at);
	ClassDB::bind_method(D_METHOD("get_dominant_layer_at", "global_position"), &Landscape3D::get_dominant_layer_at);
	ClassDB::bind_method(D_METHOD("raycast", "from", "direction", "max_distance"), &Landscape3D::raycast, DEFVAL(1e6));
	ClassDB::bind_method(D_METHOD("get_aabb"), &Landscape3D::get_aabb);
	ClassDB::bind_method(D_METHOD("global_to_local", "global_position"), &Landscape3D::global_to_local);
	ClassDB::bind_method(D_METHOD("local_to_global", "local_position"), &Landscape3D::local_to_global);
	ClassDB::bind_method(D_METHOD("set_brush_preview", "visible", "local_center", "radius", "falloff", "color"), &Landscape3D::set_brush_preview, DEFVAL(Vector3()), DEFVAL(0.0), DEFVAL(0.0), DEFVAL(Color(0.25, 0.6, 1.0)));
	ClassDB::bind_method(D_METHOD("force_update"), &Landscape3D::force_update);
	ClassDB::bind_method(D_METHOD("get_statistics"), &Landscape3D::get_statistics);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "data", PROPERTY_HINT_RESOURCE_TYPE, LandscapeData::get_class_static()), "set_data", "get_data");

	ADD_GROUP("Material", "");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "layers", PROPERTY_HINT_ARRAY_TYPE, MAKE_RESOURCE_TYPE_HINT(LandscapeLayer::get_class_static())), "set_layers", "get_layers");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "layer_texture_size", PROPERTY_HINT_ENUM, "256:256,512:512,1024:1024,2048:2048,4096:4096"), "set_layer_texture_size", "get_layer_texture_size");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "shader_override", PROPERTY_HINT_RESOURCE_TYPE, Shader::get_class_static()), "set_shader_override", "get_shader_override");

	ADD_GROUP("LOD", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "patch_size", PROPERTY_HINT_ENUM, "8x8:8,16x16:16,32x32:32,64x64:64,128x128:128"), "set_patch_size", "get_patch_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lod_pixel_error", PROPERTY_HINT_RANGE, "0.25,32,0.05,suffix:px"), "set_lod_pixel_error", "get_lod_pixel_error");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "micro_detail_levels", PROPERTY_HINT_RANGE, "0,4,1"), "set_micro_detail_levels", "get_micro_detail_levels");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "displacement_scale", PROPERTY_HINT_RANGE, "0,4,0.01,or_greater"), "set_displacement_scale", "get_displacement_scale");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_patches", PROPERTY_HINT_RANGE, "256,1048576,1,or_greater"), "set_max_patches", "get_max_patches");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "shadow_lod_bias", PROPERTY_HINT_RANGE, "1,16,0.1"), "set_shadow_lod_bias", "get_shadow_lod_bias");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "shadow_distance", PROPERTY_HINT_RANGE, "0,16384,1,or_greater,suffix:m"), "set_shadow_distance", "get_shadow_distance");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "lod_camera_path", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Camera3D"), "set_lod_camera_path", "get_lod_camera_path");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "freeze_lod"), "set_freeze_lod", "is_lod_frozen");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "debug_view", PROPERTY_HINT_ENUM, "Disabled,LOD Levels,Patches,Normals,Layers"), "set_debug_view", "get_debug_view");

	ADD_GROUP("Rendering", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "render_layers", PROPERTY_HINT_LAYERS_3D_RENDER), "set_render_layers", "get_render_layers");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "cast_shadows"), "set_cast_shadows", "is_casting_shadows");

	ADD_GROUP("Collision", "collision_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "collision_enabled"), "set_collision_enabled", "is_collision_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_layer", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_layer", "get_collision_layer");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_mask", "get_collision_mask");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "collision_priority"), "set_collision_priority", "get_collision_priority");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_tile_size", PROPERTY_HINT_ENUM, "64:64,128:128,256:256,512:512,1024:1024"), "set_collision_tile_size", "get_collision_tile_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_lod", PROPERTY_HINT_RANGE, "0,4,1"), "set_collision_lod", "get_collision_lod");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "collision_radius", PROPERTY_HINT_RANGE, "0,16384,1,or_greater,suffix:m"), "set_collision_radius", "get_collision_radius");

	BIND_ENUM_CONSTANT(DEBUG_VIEW_DISABLED);
	BIND_ENUM_CONSTANT(DEBUG_VIEW_LOD_LEVELS);
	BIND_ENUM_CONSTANT(DEBUG_VIEW_PATCHES);
	BIND_ENUM_CONSTANT(DEBUG_VIEW_NORMALS);
	BIND_ENUM_CONSTANT(DEBUG_VIEW_LAYERS);

	BIND_CONSTANT(MAX_LAYERS);
}

Landscape3D::Landscape3D() {
	set_notify_transform(true);
	_create_render_resources();
}

Landscape3D::~Landscape3D() {
	_clear_collision();
	for (const Ref<LandscapeLayer> &layer : layers) {
		_connect_layer(layer, false);
	}
	_free_render_resources();
}
