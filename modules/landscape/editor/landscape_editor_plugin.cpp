/**************************************************************************/
/*  landscape_editor_plugin.cpp                                           */
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

#include "landscape_editor_plugin.h"

#include "core/input/input.h"
#include "core/input/input_event.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/math/triangle_mesh.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "editor/docks/editor_dock_manager.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/gui/editor_file_dialog.h"
#include "editor/inspector/editor_resource_picker.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "editor/scene/3d/node_3d_editor_viewport.h"
#include "editor/themes/editor_scale.h"
#include "scene/3d/camera_3d.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/check_box.h"
#include "scene/gui/grid_container.h"
#include "scene/gui/item_list.h"
#include "scene/gui/label.h"
#include "scene/gui/option_button.h"
#include "scene/gui/scroll_container.h"
#include "scene/gui/separator.h"
#include "scene/gui/slider.h"
#include "scene/gui/spin_box.h"
#include "scene/gui/tab_bar.h"
#include "scene/resources/texture.h"

/* Gizmo */

bool LandscapeGizmoPlugin::has_gizmo(Node3D *p_spatial) {
	return Object::cast_to<Landscape3D>(p_spatial) != nullptr;
}

String LandscapeGizmoPlugin::get_gizmo_name() const {
	return "Landscape3D";
}

int LandscapeGizmoPlugin::get_priority() const {
	return -1;
}

bool LandscapeGizmoPlugin::can_be_hidden() const {
	return false;
}

void LandscapeGizmoPlugin::redraw(EditorNode3DGizmo *p_gizmo) {
	p_gizmo->clear();
	Landscape3D *landscape = Object::cast_to<Landscape3D>(p_gizmo->get_node_3d());
	if (!landscape) {
		return;
	}
	// A coarse version of the terrain, only used to select the node in the viewport.
	Ref<TriangleMesh> mesh = landscape->generate_selection_mesh(64);
	if (mesh.is_valid()) {
		p_gizmo->add_collision_triangles(mesh);
	}
}

/* Editor */

static const int size_presets_values[] = { 257, 513, 1025, 2049, 4097, 8193, 16384 };

static String _format_count(uint64_t p_count) {
	if (p_count >= 1000000) {
		return String::num(p_count / 1000000.0, 1) + "M";
	}
	if (p_count >= 1000) {
		return String::num(p_count / 1000.0, 1) + "K";
	}
	return itos(p_count);
}

Control *LandscapeEditor::_add_setting(VBoxContainer *p_parent, const String &p_label, Control *p_control, const String &p_tooltip) {
	HBoxContainer *hb = memnew(HBoxContainer);
	Label *label = memnew(Label(p_label));
	label->set_h_size_flags(SIZE_EXPAND_FILL);
	label->set_stretch_ratio(0.8);
	label->set_clip_text(true);
	hb->add_child(label);
	p_control->set_h_size_flags(SIZE_EXPAND_FILL);
	hb->add_child(p_control);
	if (!p_tooltip.is_empty()) {
		label->set_tooltip_text(p_tooltip);
		p_control->set_tooltip_text(p_tooltip);
	}
	p_parent->add_child(hb);
	return hb;
}

SpinBox *LandscapeEditor::_make_spin(double p_min, double p_max, double p_step, double p_value, const String &p_suffix, bool p_allow_greater) {
	SpinBox *spin = memnew(SpinBox);
	spin->set_min(p_min);
	spin->set_max(p_max);
	spin->set_step(p_step);
	spin->set_value(p_value);
	spin->set_suffix(p_suffix);
	spin->set_allow_greater(p_allow_greater);
	spin->set_select_all_on_focus(true);
	return spin;
}

Button *LandscapeEditor::_make_tool_button(Container *p_parent, const String &p_text, const String &p_icon, const Ref<ButtonGroup> &p_group, LandscapeBrush::Tool p_tool, const String &p_tooltip) {
	Button *button = memnew(Button);
	button->set_text(p_text);
	button->set_toggle_mode(true);
	button->set_button_group(p_group);
	button->set_tooltip_text(p_tooltip);
	button->set_h_size_flags(SIZE_EXPAND_FILL);
	button->set_meta(SNAME("_landscape_icon"), p_icon);
	button->connect(SceneStringName(pressed), callable_mp(this, &LandscapeEditor::_tool_selected).bind(p_tool));
	p_parent->add_child(button);
	return button;
}

bool LandscapeEditor::_validate_landscape() {
	if (landscape && !ObjectDB::get_instance(landscape_id)) {
		// The node was freed while being edited.
		landscape = nullptr;
		stroking = false;
		undo_tiles.clear();
	}
	return landscape != nullptr;
}

LandscapeBrush::Tool LandscapeEditor::_get_current_tool() const {
	return mode == MODE_PAINT ? paint_tool : sculpt_tool;
}

void LandscapeEditor::_mode_changed(int p_mode) {
	if (stroking) {
		_end_stroke();
	}
	mode = Mode(p_mode);
	manage_panel->set_visible(mode == MODE_MANAGE);
	sculpt_panel->set_visible(mode == MODE_SCULPT);
	paint_panel->set_visible(mode == MODE_PAINT);
	brush_panel->set_visible(mode != MODE_MANAGE);
	_update_tool_settings();
	_update_brush_preview();
}

void LandscapeEditor::_tool_selected(LandscapeBrush::Tool p_tool) {
	if (LandscapeBrush::is_paint_tool(p_tool)) {
		paint_tool = p_tool;
	} else {
		sculpt_tool = p_tool;
	}
	_update_tool_settings();
}

void LandscapeEditor::_update_tool_settings() {
	const LandscapeBrush::Tool tool = _get_current_tool();
	brush->set_tool(tool);
	for (KeyValue<LandscapeBrush::Tool, Vector<Control *>> &kv : tool_controls) {
		for (Control *control : kv.value) {
			control->set_visible(false);
		}
	}
	bool any = false;
	if (mode != MODE_MANAGE && tool_controls.has(tool)) {
		for (Control *control : tool_controls[tool]) {
			control->set_visible(true);
			any = true;
		}
	}
	tool_settings->set_visible(any);
}

void LandscapeEditor::_brush_settings_changed(double p_value) {
	if (updating_ui) {
		return;
	}
	updating_ui = true;
	brush->set_size(brush_size->get_value());
	brush_size_slider->set_value(brush_size->get_value());
	brush->set_falloff(brush_falloff->get_value());
	brush->set_strength(brush_strength->get_value());
	brush->set_falloff_type(LandscapeBrush::FalloffType(falloff_type->get_selected_id()));
	brush->set_alpha_rotation(Math::deg_to_rad(alpha_rotation->get_value()));
	brush->set_smooth_radius(int(smooth_radius->get_value()));
	brush->set_flatten_mode(LandscapeBrush::FlattenMode(flatten_mode->get_selected_id()));
	brush->set_ramp_width(ramp_width->get_value());
	brush->set_noise_scale(noise_scale->get_value());
	brush->set_noise_seed(int(noise_seed->get_value()));
	brush->set_erosion_talus(erosion_talus->get_value());
	brush->set_erosion_iterations(int(erosion_iterations->get_value()));
	brush->set_terrace_height(terrace_height->get_value());
	brush->set_target_weight(paint_target->get_value());
	updating_ui = false;
	_update_brush_preview();
}

void LandscapeEditor::_brush_size_slider_changed(double p_value) {
	if (updating_ui) {
		return;
	}
	brush_size->set_value(p_value);
}

void LandscapeEditor::_alpha_changed(const Ref<Resource> &p_resource) {
	Ref<Texture2D> texture = p_resource;
	brush->set_alpha(texture.is_valid() ? texture->get_image() : Ref<Image>());
}

void LandscapeEditor::_update_info() {
	if (!landscape) {
		info_label->set_text(TTR("No Landscape3D selected."));
		return;
	}
	Ref<LandscapeData> ldata = landscape->get_data();
	if (ldata.is_null() || !ldata->is_valid()) {
		info_label->set_text(TTR("This landscape has no data yet. Create it in the Manage tab."));
		return;
	}
	const Vector2i size = ldata->get_size();
	const Vector2 world = ldata->get_world_size();
	const Vector2 range = ldata->get_height_range();
	info_label->set_text(vformat(TTR("Resolution: %d x %d (%s x %s km), spacing %s m\nHeight range: %s .. %s m\nLayers: %d / %d\nStorage: %s"),
			size.x, size.y, String::num(world.x / 1000.0, 2), String::num(world.y / 1000.0, 2), String::num(ldata->get_vertex_spacing(), 3),
			String::num(range.x, 1), String::num(range.y, 1), landscape->get_layer_count(), Landscape3D::MAX_LAYERS,
			ldata->is_streamed() ? TTR("streamed from the .lsdata file") : TTR("embedded (save it as .lsdata to stream it)")));
	resize_x->set_value(size.x);
	resize_z->set_value(size.y);

	updating_debug = true;
	debug_view->select(int(landscape->get_debug_view()));
	freeze_lod->set_pressed(landscape->is_lod_frozen());
	updating_debug = false;
}

void LandscapeEditor::_debug_view_selected(int p_index) {
	if (!landscape || updating_debug) {
		return;
	}
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(TTR("Set Landscape Debug View"), UndoRedo::MERGE_DISABLE, landscape);
	undo_redo->add_do_property(landscape, "debug_view", p_index);
	undo_redo->add_undo_property(landscape, "debug_view", int(landscape->get_debug_view()));
	undo_redo->add_do_method(this, "_update_info");
	undo_redo->add_undo_method(this, "_update_info");
	undo_redo->commit_action();
}

void LandscapeEditor::_freeze_lod_toggled(bool p_pressed) {
	if (!landscape || updating_debug) {
		return;
	}
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(TTR("Freeze Landscape LOD"), UndoRedo::MERGE_DISABLE, landscape);
	undo_redo->add_do_property(landscape, "freeze_lod", p_pressed);
	undo_redo->add_undo_property(landscape, "freeze_lod", landscape->is_lod_frozen());
	undo_redo->add_do_method(this, "_update_info");
	undo_redo->add_undo_method(this, "_update_info");
	undo_redo->commit_action();
}

void LandscapeEditor::_update_new_world_size(double p_value) {
	const real_t spacing = new_spacing->get_value();
	const real_t wx = (new_size_x->get_value() - 1) * spacing;
	const real_t wz = (new_size_z->get_value() - 1) * spacing;
	String warning;
	if (wx > 16384.0 || wz > 16384.0) {
		warning = TTR(" (exceeds 16 km)");
	}
	new_world_size->set_text(vformat(TTR("World size: %s x %s km"), String::num(wx / 1000.0, 3), String::num(wz / 1000.0, 3)) + warning);
}

void LandscapeEditor::_size_preset_selected(int p_index) {
	if (p_index <= 0) {
		return;
	}
	const int value = size_presets_values[p_index - 1];
	new_size_x->set_value(value);
	new_size_z->set_value(value);
	size_presets->select(0);
}

/* Layers */

uint64_t LandscapeEditor::_compute_layer_hash() const {
	if (!landscape) {
		return 0;
	}
	uint64_t hash = hash_murmur3_one_64(landscape->get_layer_count());
	for (int i = 0; i < landscape->get_layer_count(); i++) {
		Ref<LandscapeLayer> layer = landscape->get_layer(i);
		hash = hash_murmur3_one_64(layer.is_valid() ? uint64_t(layer->get_instance_id()) : 0, hash);
		if (layer.is_valid()) {
			hash = hash_murmur3_one_64(layer->get_layer_name().hash(), hash);
			hash = hash_murmur3_one_64(layer->get_albedo_texture().is_valid() ? uint64_t(layer->get_albedo_texture()->get_instance_id()) : 0, hash);
		}
	}
	return hash;
}

void LandscapeEditor::_update_layer_list() {
	layer_list->clear();
	layer_list_hash = _compute_layer_hash();
	if (!landscape) {
		return;
	}
	for (int i = 0; i < landscape->get_layer_count(); i++) {
		Ref<LandscapeLayer> layer = landscape->get_layer(i);
		String name = vformat(TTR("Layer %d"), i);
		Ref<Texture2D> icon;
		if (layer.is_valid()) {
			if (!layer->get_layer_name().is_empty()) {
				name = vformat("%d: %s", i, layer->get_layer_name());
			}
			icon = layer->get_albedo_texture();
		}
		layer_list->add_item(name, icon);
	}
	if (layer_list->get_item_count() > 0) {
		selected_layer = CLAMP(selected_layer, 0, layer_list->get_item_count() - 1);
		layer_list->select(selected_layer);
	}
	brush->set_layer(selected_layer);
}

void LandscapeEditor::_layer_selected(int p_index) {
	selected_layer = p_index;
	brush->set_layer(selected_layer);
}

void LandscapeEditor::_add_layer() {
	ERR_FAIL_NULL(landscape);
	if (landscape->get_layer_count() >= Landscape3D::MAX_LAYERS) {
		EditorNode::get_singleton()->show_warning(vformat(TTR("A landscape supports up to %d material layers."), Landscape3D::MAX_LAYERS));
		return;
	}
	TypedArray<LandscapeLayer> old_layers = landscape->call(SNAME("get_layers"));
	TypedArray<LandscapeLayer> new_layers = old_layers.duplicate();
	Ref<LandscapeLayer> layer;
	layer.instantiate();
	layer->set_layer_name(vformat("Layer %d", new_layers.size()));
	new_layers.push_back(layer);

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(TTR("Add Landscape Layer"), UndoRedo::MERGE_DISABLE, landscape);
	undo_redo->add_do_property(landscape, "layers", new_layers);
	undo_redo->add_undo_property(landscape, "layers", old_layers);
	undo_redo->add_do_method(this, "_update_layer_list");
	undo_redo->add_undo_method(this, "_update_layer_list");
	undo_redo->commit_action();
	selected_layer = new_layers.size() - 1;
	_update_layer_list();
}

void LandscapeEditor::_remove_layer() {
	ERR_FAIL_NULL(landscape);
	if (selected_layer < 0 || selected_layer >= landscape->get_layer_count()) {
		return;
	}
	TypedArray<LandscapeLayer> old_layers = landscape->call(SNAME("get_layers"));
	TypedArray<LandscapeLayer> new_layers = old_layers.duplicate();
	new_layers.remove_at(selected_layer);

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(TTR("Remove Landscape Layer"), UndoRedo::MERGE_DISABLE, landscape);
	Ref<LandscapeData> ldata = landscape->get_data();
	if (ldata.is_valid() && ldata->is_valid()) {
		// The painted weights of the layer are removed as well.
		const Rect2i full(Point2i(), ldata->get_size());
		Array before;
		before.push_back(ldata->get_region(full, false, true));
		undo_redo->add_do_method(ldata.ptr(), "remove_layer", selected_layer);
		undo_redo->add_undo_method(this, "_apply_regions", ldata, before);
	}
	undo_redo->add_do_property(landscape, "layers", new_layers);
	undo_redo->add_undo_property(landscape, "layers", old_layers);
	undo_redo->add_do_method(this, "_update_layer_list");
	undo_redo->add_undo_method(this, "_update_layer_list");
	undo_redo->commit_action();
	selected_layer = MAX(selected_layer - 1, 0);
	_update_layer_list();
	_set_data_edited();
}

void LandscapeEditor::_fill_layer() {
	ERR_FAIL_NULL(landscape);
	Ref<LandscapeData> ldata = landscape->get_data();
	if (ldata.is_null() || !ldata->is_valid()) {
		return;
	}
	const Rect2i full(Point2i(), ldata->get_size());
	Array before;
	before.push_back(ldata->get_region(full, false, true));
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(TTR("Fill Landscape Layer"), UndoRedo::MERGE_DISABLE, landscape);
	undo_redo->add_do_method(ldata.ptr(), "fill_layer", selected_layer);
	undo_redo->add_undo_method(this, "_apply_regions", ldata, before);
	undo_redo->commit_action();
	_set_data_edited();
}

void LandscapeEditor::_import_weights_pressed() {
	ERR_FAIL_NULL(landscape);
	Ref<LandscapeData> ldata = landscape->get_data();
	if (ldata.is_null() || !ldata->is_valid() || selected_layer < 0 || selected_layer >= MAX(landscape->get_layer_count(), 1)) {
		return;
	}
	weights_dialog->popup_file_dialog();
}

void LandscapeEditor::_import_weights_file_selected(const String &p_path) {
	ERR_FAIL_NULL(landscape);
	Ref<LandscapeData> ldata = landscape->get_data();
	if (ldata.is_null() || !ldata->is_valid()) {
		return;
	}
	const Rect2i full(Point2i(), ldata->get_size());
	Array before;
	before.push_back(ldata->get_region(full, false, true));
	if (ldata->import_layer_weights(selected_layer, p_path) != OK) {
		EditorNode::get_singleton()->show_warning(vformat(TTR("Failed to import weightmap \"%s\"."), p_path));
		return;
	}
	Array after;
	after.push_back(ldata->get_region(full, false, true));
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(TTR("Import Layer Weights"), UndoRedo::MERGE_DISABLE, landscape);
	undo_redo->add_do_method(this, "_apply_regions", ldata, after);
	undo_redo->add_undo_method(this, "_apply_regions", ldata, before);
	undo_redo->commit_action(false);
	_set_data_edited();
}

/* Manage */

void LandscapeEditor::_create_pressed() {
	ERR_FAIL_NULL(landscape);
	String base = "res://landscape_data.lsdata";
	if (landscape->get_owner() && !landscape->get_owner()->get_scene_file_path().is_empty()) {
		const String scene = landscape->get_owner()->get_scene_file_path();
		base = scene.get_base_dir().path_join(scene.get_file().get_basename() + "_" + String(landscape->get_name()).to_snake_case() + ".lsdata");
	}
	create_dialog->set_current_path(base);
	create_dialog->popup_file_dialog();
}

void LandscapeEditor::_create_file_selected(const String &p_path) {
	ERR_FAIL_NULL(landscape);
	Ref<LandscapeData> ldata;
	ldata.instantiate();
	ldata->create(Vector2i(new_size_x->get_value(), new_size_z->get_value()), new_spacing->get_value(), new_height->get_value());
	ldata->ensure_layer_capacity(landscape->get_layer_count());
	const Error err = ResourceSaver::save(ldata, p_path, ResourceSaver::FLAG_CHANGE_PATH);
	if (err != OK) {
		EditorNode::get_singleton()->show_warning(vformat(TTR("Failed to save the landscape data to \"%s\"."), p_path));
		return;
	}
	if (EditorFileSystem::get_singleton()) {
		EditorFileSystem::get_singleton()->update_file(p_path);
	}

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(TTR("Create Landscape Data"), UndoRedo::MERGE_DISABLE, landscape);
	undo_redo->add_do_property(landscape, "data", ldata);
	undo_redo->add_undo_property(landscape, "data", landscape->get_data());
	undo_redo->add_do_method(this, "_update_info");
	undo_redo->add_undo_method(this, "_update_info");
	undo_redo->commit_action();
	mode_tabs->set_current_tab(MODE_SCULPT);
}

void LandscapeEditor::_import_pressed() {
	ERR_FAIL_NULL(landscape);
	import_dialog->popup_file_dialog();
}

void LandscapeEditor::_import_file_selected(const String &p_path) {
	ERR_FAIL_NULL(landscape);
	Ref<LandscapeData> ldata = landscape->get_data();
	const bool created = ldata.is_null();
	if (created) {
		ldata.instantiate();
	}
	Array before;
	if (!created && ldata->is_valid() && !import_resize->is_pressed()) {
		before.push_back(ldata->get_region(Rect2i(Point2i(), ldata->get_size()), true, false));
	}
	const Error err = ldata->import_heightmap(p_path, import_max->get_value() - import_min->get_value(), import_min->get_value(), import_resize->is_pressed() || created);
	if (err != OK) {
		EditorNode::get_singleton()->show_warning(vformat(TTR("Failed to import heightmap \"%s\"."), p_path));
		return;
	}
	if (created) {
		EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
		undo_redo->create_action(TTR("Import Heightmap"), UndoRedo::MERGE_DISABLE, landscape);
		undo_redo->add_do_property(landscape, "data", ldata);
		undo_redo->add_undo_property(landscape, "data", Ref<LandscapeData>());
		undo_redo->commit_action();
	} else if (!before.is_empty()) {
		Array after;
		after.push_back(ldata->get_region(Rect2i(Point2i(), ldata->get_size()), true, false));
		EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
		undo_redo->create_action(TTR("Import Heightmap"), UndoRedo::MERGE_DISABLE, landscape);
		undo_redo->add_do_method(this, "_apply_regions", ldata, after);
		undo_redo->add_undo_method(this, "_apply_regions", ldata, before);
		undo_redo->commit_action(false);
	}
	_set_data_edited();
	_update_info();
}

void LandscapeEditor::_export_pressed() {
	ERR_FAIL_NULL(landscape);
	if (landscape->get_data().is_null() || !landscape->get_data()->is_valid()) {
		return;
	}
	export_dialog->popup_file_dialog();
}

void LandscapeEditor::_export_file_selected(const String &p_path) {
	ERR_FAIL_NULL(landscape);
	Ref<LandscapeData> ldata = landscape->get_data();
	ERR_FAIL_COND(ldata.is_null());
	if (ldata->export_heightmap(p_path) != OK) {
		EditorNode::get_singleton()->show_warning(vformat(TTR("Failed to export heightmap to \"%s\"."), p_path));
	}
}

void LandscapeEditor::_resize_pressed() {
	ERR_FAIL_NULL(landscape);
	Ref<LandscapeData> ldata = landscape->get_data();
	if (ldata.is_null() || !ldata->is_valid()) {
		return;
	}
	const Vector2i new_size(resize_x->get_value(), resize_z->get_value());
	if (new_size == ldata->get_size()) {
		return;
	}
	ldata->resize(new_size);
	_set_data_edited();
	_update_info();
	EditorNode::get_singleton()->show_warning(TTR("The landscape was resampled. This operation can't be undone, reload the scene without saving to revert it."));
}

/* Strokes */

void LandscapeEditor::_set_data_edited() {
	if (!landscape || landscape->get_data().is_null()) {
		return;
	}
	landscape->get_data()->set_edited(true);
}

void LandscapeEditor::_apply_regions(const Ref<LandscapeData> &p_data, const Array &p_regions) {
	ERR_FAIL_COND(p_data.is_null());
	for (int i = 0; i < p_regions.size(); i++) {
		p_data->set_region(p_regions[i]);
	}
	p_data->set_edited(true);
	_update_info();
	if (_validate_landscape()) {
		landscape->update_gizmos();
	}
}

bool LandscapeEditor::_update_cursor(Camera3D *p_camera, const Point2 &p_position) {
	cursor_valid = false;
	if (!landscape || landscape->get_data().is_null() || !landscape->get_data()->is_valid()) {
		return false;
	}
	const Vector3 from = p_camera->project_ray_origin(p_position);
	const Vector3 dir = p_camera->project_ray_normal(p_position);
	Vector3 position;
	Vector3 normal;
	if (landscape->intersect_ray(from, dir, position, normal, p_camera->get_far())) {
		cursor_valid = true;
		cursor_local = landscape->global_to_local(position);
	}
	return cursor_valid;
}

void LandscapeEditor::_update_brush_preview() {
	if (!landscape) {
		return;
	}
	if (!cursor_valid || mode == MODE_MANAGE || !is_visible_in_tree()) {
		landscape->set_brush_preview(false);
		return;
	}
	Color color = Color(0.3, 0.62, 1.0);
	const LandscapeBrush::Tool tool = _get_current_tool();
	if (LandscapeBrush::is_paint_tool(tool)) {
		color = Color(0.35, 1.0, 0.5);
	} else if (tool == LandscapeBrush::TOOL_SMOOTH || tool == LandscapeBrush::TOOL_FLATTEN || tool == LandscapeBrush::TOOL_TERRACE) {
		color = Color(1.0, 0.85, 0.3);
	}
	if (stroke_invert || Input::get_singleton()->is_key_pressed(Key::SHIFT)) {
		color = Color(1.0, 0.35, 0.3);
	}
	real_t radius = brush->get_size();
	if (tool == LandscapeBrush::TOOL_RAMP) {
		radius = brush->get_ramp_width() * 0.5;
	}
	landscape->set_brush_preview(true, cursor_local, radius, brush->get_falloff(), color);
}

void LandscapeEditor::_snapshot(const Rect2i &p_rect) {
	Ref<LandscapeData> ldata = landscape->get_data();
	const bool heights = !LandscapeBrush::is_paint_tool(brush->get_tool());
	const Rect2i rect = ldata->clip_rect(p_rect);
	if (!rect.has_area()) {
		return;
	}
	stroke_rect = stroke_rect.has_area() ? stroke_rect.merge(rect) : rect;
	const Point2i begin = rect.position / UNDO_TILE;
	const Point2i end = (rect.get_end() - Vector2i(1, 1)) / UNDO_TILE;
	for (int z = begin.y; z <= end.y; z++) {
		for (int x = begin.x; x <= end.x; x++) {
			const Vector2i tile(x, z);
			if (undo_tiles.has(tile)) {
				continue;
			}
			undo_tiles[tile] = ldata->get_region(Rect2i(tile * UNDO_TILE, Size2i(UNDO_TILE, UNDO_TILE)), heights, !heights);
		}
	}
}

void LandscapeEditor::_pick(const Vector3 &p_local) {
	Ref<LandscapeData> ldata = landscape->get_data();
	const LandscapeBrush::Tool tool = _get_current_tool();
	const Vector2 texel = landscape->local_to_texel(p_local);
	const int tx = int(Math::round(texel.x));
	const int tz = int(Math::round(texel.y));
	if (LandscapeBrush::is_paint_tool(tool)) {
		if (tool == LandscapeBrush::TOOL_PAINT_FLATTEN) {
			paint_target->set_value(ldata->get_layer_weight(tx, tz, selected_layer));
		} else {
			selected_layer = ldata->get_dominant_layer(tx, tz);
			_update_layer_list();
		}
	} else {
		flatten_use_target->set_pressed(true);
		flatten_target->set_value(ldata->get_height(tx, tz));
	}
}

void LandscapeEditor::_begin_stroke(bool p_invert) {
	stroking = true;
	stroke_invert = p_invert;
	undo_tiles.clear();
	stroke_rect = Rect2i();
	_brush_settings_changed();
	brush->set_invert(p_invert);
	brush->set_layer(selected_layer);

	const LandscapeBrush::Tool tool = brush->get_tool();
	if (tool == LandscapeBrush::TOOL_FLATTEN) {
		brush->set_target_height(flatten_use_target->is_pressed() ? float(flatten_target->get_value()) : float(cursor_local.y));
	}
	if (tool == LandscapeBrush::TOOL_RAMP) {
		ramp_start_local = cursor_local;
		return;
	}
	last_dab_local = cursor_local;
	_apply_dab(cursor_local, 1.0 / 30.0);
}

void LandscapeEditor::_apply_dab(const Vector3 &p_local, real_t p_delta) {
	Ref<LandscapeData> ldata = landscape->get_data();
	const int margin = brush->get_smooth_radius() + 2;
	_snapshot(brush->get_affected_rect(ldata.ptr(), p_local).grow(margin));
	(void)brush->apply(ldata, p_local, p_delta);
}

void LandscapeEditor::_end_stroke() {
	if (!stroking) {
		return;
	}
	stroking = false;
	Ref<LandscapeData> ldata = landscape ? landscape->get_data() : Ref<LandscapeData>();
	if (ldata.is_null() || !ldata->is_valid()) {
		undo_tiles.clear();
		return;
	}

	const LandscapeBrush::Tool tool = brush->get_tool();
	if (tool == LandscapeBrush::TOOL_RAMP && cursor_valid) {
		const real_t half_width = brush->get_ramp_width() * 0.5;
		const real_t spacing = ldata->get_vertex_spacing();
		const Vector2 a(ramp_start_local.x, ramp_start_local.z);
		const Vector2 b(cursor_local.x, cursor_local.z);
		const Vector2 mn = (a.min(b) - Vector2(half_width, half_width)) / spacing;
		const Vector2 mx = (a.max(b) + Vector2(half_width, half_width)) / spacing;
		_snapshot(Rect2i(Point2i(mn.floor()), Size2i((mx - mn).ceil()) + Size2i(3, 3)));
		(void)brush->apply_ramp(ldata, ramp_start_local, cursor_local);
	}

	if (undo_tiles.is_empty()) {
		return;
	}

	const bool heights = !LandscapeBrush::is_paint_tool(tool);
	Array before;
	Array after;
	for (const KeyValue<Vector2i, Dictionary> &kv : undo_tiles) {
		before.push_back(kv.value);
		after.push_back(ldata->get_region(kv.value["rect"], heights, !heights));
	}
	undo_tiles.clear();

	static const char *tool_names[] = { "Sculpt", "Smooth", "Flatten", "Ramp", "Noise", "Erosion", "Terrace", "Visibility", "Paint", "Smooth Layers", "Flatten Layers", "Paint Noise" };
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(vformat(TTR("Landscape: %s"), TTR(tool_names[tool])), UndoRedo::MERGE_DISABLE, landscape);
	undo_redo->add_do_method(this, "_apply_regions", ldata, after);
	undo_redo->add_undo_method(this, "_apply_regions", ldata, before);
	undo_redo->commit_action(false);
	_set_data_edited();
	_update_info();
	landscape->update_gizmos(); // Refresh the selection mesh.
}

void LandscapeEditor::_cancel_stroke() {
	if (stroking) {
		_end_stroke();
	}
}

EditorPlugin::AfterGUIInput LandscapeEditor::forward_3d_gui_input(Camera3D *p_camera, const Ref<InputEvent> &p_event) {
	if (!_validate_landscape() || mode == MODE_MANAGE || landscape->get_data().is_null() || !landscape->get_data()->is_valid()) {
		return EditorPlugin::AFTER_GUI_INPUT_PASS;
	}

	Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_valid()) {
		_update_cursor(p_camera, mm->get_position());
		_update_brush_preview();
		return stroking ? EditorPlugin::AFTER_GUI_INPUT_STOP : EditorPlugin::AFTER_GUI_INPUT_PASS;
	}

	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid() && mb->get_button_index() == MouseButton::LEFT) {
		if (mb->is_pressed()) {
			if (mb->is_alt_pressed()) {
				return EditorPlugin::AFTER_GUI_INPUT_PASS; // Keep Maya-style navigation working.
			}
			if (!_update_cursor(p_camera, mb->get_position())) {
				return EditorPlugin::AFTER_GUI_INPUT_PASS;
			}
			if (mb->is_command_or_control_pressed()) {
				_pick(cursor_local);
				return EditorPlugin::AFTER_GUI_INPUT_STOP;
			}
			_begin_stroke(mb->is_shift_pressed());
			_update_brush_preview();
			return EditorPlugin::AFTER_GUI_INPUT_STOP;
		} else if (stroking) {
			_update_cursor(p_camera, mb->get_position());
			_end_stroke();
			_update_brush_preview();
			return EditorPlugin::AFTER_GUI_INPUT_STOP;
		}
	}

	Ref<InputEventKey> k = p_event;
	if (k.is_valid() && k->is_pressed() && !k->is_echo()) {
		if (k->get_keycode() == Key::BRACKETLEFT) {
			brush_size->set_value(MAX(brush_size->get_value() / 1.25, 0.1));
			return EditorPlugin::AFTER_GUI_INPUT_STOP;
		}
		if (k->get_keycode() == Key::BRACKETRIGHT) {
			brush_size->set_value(brush_size->get_value() * 1.25);
			return EditorPlugin::AFTER_GUI_INPUT_STOP;
		}
		if (k->get_keycode() == Key::SHIFT) {
			_update_brush_preview();
		}
	}
	if (k.is_valid() && !k->is_pressed() && k->get_keycode() == Key::SHIFT) {
		_update_brush_preview();
	}

	return EditorPlugin::AFTER_GUI_INPUT_PASS;
}

void LandscapeEditor::edit(Landscape3D *p_landscape) {
	_validate_landscape();
	if (landscape == p_landscape) {
		return;
	}
	_cancel_stroke();
	if (landscape) {
		landscape->set_brush_preview(false);
	}
	landscape = p_landscape;
	landscape_id = landscape ? landscape->get_instance_id() : ObjectID();
	cursor_valid = false;
	selected_layer = 0;
	_update_layer_list();
	_update_info();
	if (landscape && (landscape->get_data().is_null() || !landscape->get_data()->is_valid())) {
		// Nothing to sculpt yet, start with the creation settings.
		mode_tabs->set_current_tab(MODE_MANAGE);
	}
}

void LandscapeEditor::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_THEME_CHANGED: {
			for (Ref<ButtonGroup> group : { sculpt_tool_group, paint_tool_group }) {
				List<BaseButton *> buttons;
				group->get_buttons(&buttons);
				for (BaseButton *button : buttons) {
					Button *b = Object::cast_to<Button>(button);
					if (b && b->has_meta(SNAME("_landscape_icon"))) {
						b->set_button_icon(get_editor_theme_icon(StringName(b->get_meta(SNAME("_landscape_icon")))));
					}
				}
			}
		} break;

		case NOTIFICATION_PROCESS: {
			if (!_validate_landscape()) {
				break;
			}
			stats_timer -= get_process_delta_time();
			if (stats_timer <= 0.0 && landscape) {
				stats_timer = 0.5;
				const Dictionary stats = landscape->get_statistics();
				String text = vformat(TTR("GPU LOD: %d patches (%s triangles), %d shadow patches, %d + %d levels"), int(stats["patches"]), _format_count(uint64_t(stats["triangles"])), int(stats["shadow_patches"]), int(stats["heightmap_levels"]), int(stats["micro_levels"]));
				if (bool(stats["budget_exceeded"])) {
					text += "\n" + TTR("Patch budget exceeded: increase Max Patches or the LOD Pixel Error.");
				}
				if (stats.has("pages_resident")) {
					text += "\n" + vformat(TTR("Streaming: %d / %d pages resident (%d needed, %d loading), GPU pool %s MB, CPU cache %s MB (%d tiles)"), int(stats["pages_resident"]), int(stats["pages_capacity"]), int(stats["pages_desired"]), int(stats["pages_loading"]), String::num(int64_t(stats["gpu_pool_bytes"]) / 1048576.0, 0), String::num(int64_t(stats["cpu_cache_bytes"]) / 1048576.0, 0), int(stats["cpu_tiles"]));
					if (int(stats["pages_desired"]) >= int(stats["pages_capacity"])) {
						text += "\n" + TTR("The streaming pool is full: increase Streaming Pool Size for more detail.");
					}
				}
				stats_label->set_text(text);
				// Layers may also be edited from the inspector.
				if (_compute_layer_hash() != layer_list_hash) {
					_update_layer_list();
					_update_info();
				}
			}
			if (stroking && !Input::get_singleton()->is_mouse_button_pressed(MouseButton::LEFT)) {
				// The button was released outside of the viewport.
				_end_stroke();
				_update_brush_preview();
			}
			if (!stroking || !landscape || landscape->get_data().is_null() || brush->get_tool() == LandscapeBrush::TOOL_RAMP) {
				break;
			}
			if (!cursor_valid) {
				break;
			}
			// Continuous application while the mouse button is held, dabs are spread along the path.
			const real_t delta = get_process_delta_time();
			const real_t spacing = MAX(brush->get_size() * 0.25, real_t(0.05));
			const real_t distance = last_dab_local.distance_to(cursor_local);
			const int steps = CLAMP(int(Math::ceil(distance / spacing)), 1, 64);
			for (int i = 1; i <= steps; i++) {
				_apply_dab(last_dab_local.lerp(cursor_local, real_t(i) / steps), delta / steps);
			}
			last_dab_local = cursor_local;
		} break;

		case NOTIFICATION_VISIBILITY_CHANGED: {
			_update_brush_preview();
		} break;
	}
}

void LandscapeEditor::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_apply_regions", "data", "regions"), &LandscapeEditor::_apply_regions);
	ClassDB::bind_method(D_METHOD("_update_layer_list"), &LandscapeEditor::_update_layer_list);
	ClassDB::bind_method(D_METHOD("_update_info"), &LandscapeEditor::_update_info);
}

LandscapeEditor::LandscapeEditor() {
	set_name(TTRC("Landscape"));
	set_icon_name("Landscape3D");
	set_default_slot(EditorDock::DOCK_SLOT_RIGHT_BL);
	set_available_layouts(EditorDock::DOCK_LAYOUT_VERTICAL | EditorDock::DOCK_LAYOUT_FLOATING);
	set_global(false);
	set_transient(true);

	brush.instantiate();

	ScrollContainer *scroll = memnew(ScrollContainer);
	scroll->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
	add_child(scroll);
	VBoxContainer *main_vb = memnew(VBoxContainer);
	main_vb->set_h_size_flags(SIZE_EXPAND_FILL);
	scroll->add_child(main_vb);

	mode_tabs = memnew(TabBar);
	mode_tabs->add_tab(TTR("Manage"));
	mode_tabs->add_tab(TTR("Sculpt"));
	mode_tabs->add_tab(TTR("Paint"));
	mode_tabs->set_clip_tabs(false);
	mode_tabs->connect("tab_changed", callable_mp(this, &LandscapeEditor::_mode_changed));
	main_vb->add_child(mode_tabs);

	info_label = memnew(Label);
	info_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	info_label->set_custom_minimum_size(Size2(200 * EDSCALE, 0));
	main_vb->add_child(info_label);
	stats_label = memnew(Label);
	stats_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	stats_label->set_modulate(Color(1, 1, 1, 0.7));
	main_vb->add_child(stats_label);
	main_vb->add_child(memnew(HSeparator));

	/* Manage */
	manage_panel = memnew(VBoxContainer);
	main_vb->add_child(manage_panel);
	{
		Label *new_title = memnew(Label(TTR("New Landscape")));
		new_title->set_theme_type_variation("HeaderSmall");
		manage_panel->add_child(new_title);

		size_presets = memnew(OptionButton);
		size_presets->add_item(TTR("Presets..."));
		for (int value : size_presets_values) {
			size_presets->add_item(vformat("%d x %d", value, value));
		}
		size_presets->connect(SceneStringName(item_selected), callable_mp(this, &LandscapeEditor::_size_preset_selected));
		_add_setting(manage_panel, TTR("Resolution"), size_presets);

		new_size_x = _make_spin(LandscapeData::MIN_RESOLUTION, LandscapeData::MAX_RESOLUTION, 1, 1025);
		new_size_z = _make_spin(LandscapeData::MIN_RESOLUTION, LandscapeData::MAX_RESOLUTION, 1, 1025);
		_add_setting(manage_panel, TTR("Vertices X"), new_size_x, TTR("Number of heightmap samples along X (max 16384)."));
		_add_setting(manage_panel, TTR("Vertices Z"), new_size_z, TTR("Number of heightmap samples along Z (max 16384)."));
		new_spacing = _make_spin(0.01, 100.0, 0.001, 1.0, "m", true);
		_add_setting(manage_panel, TTR("Vertex Spacing"), new_spacing, TTR("Distance between two heightmap samples. The world size is (vertices - 1) * spacing, up to 16 x 16 km."));
		new_height = _make_spin(-10000.0, 10000.0, 0.01, 0.0, "m", true);
		_add_setting(manage_panel, TTR("Initial Height"), new_height);
		new_world_size = memnew(Label);
		manage_panel->add_child(new_world_size);
		new_size_x->connect(SceneStringName(value_changed), callable_mp(this, &LandscapeEditor::_update_new_world_size));
		new_size_z->connect(SceneStringName(value_changed), callable_mp(this, &LandscapeEditor::_update_new_world_size));
		new_spacing->connect(SceneStringName(value_changed), callable_mp(this, &LandscapeEditor::_update_new_world_size));
		_update_new_world_size();

		Button *create = memnew(Button(TTR("Create Landscape Data...")));
		create->connect(SceneStringName(pressed), callable_mp(this, &LandscapeEditor::_create_pressed));
		manage_panel->add_child(create);

		manage_panel->add_child(memnew(HSeparator));
		Label *import_title = memnew(Label(TTR("Heightmap")));
		import_title->set_theme_type_variation("HeaderSmall");
		manage_panel->add_child(import_title);
		import_min = _make_spin(-10000.0, 10000.0, 0.01, 0.0, "m", true);
		import_max = _make_spin(-10000.0, 10000.0, 0.01, 256.0, "m", true);
		_add_setting(manage_panel, TTR("Min Height"), import_min, TTR("Height of the darkest value of 8/16-bit heightmaps (offset added to float heightmaps)."));
		_add_setting(manage_panel, TTR("Max Height"), import_max, TTR("Height of the brightest value of 8/16-bit heightmaps. For float heightmaps (EXR), use Min 0 and Max 1 to keep the stored heights."));
		import_resize = memnew(CheckBox(TTR("Use Heightmap Resolution")));
		import_resize->set_pressed(true);
		import_resize->set_tooltip_text(TTR("Resize the landscape to the resolution of the imported heightmap instead of resampling the heightmap."));
		manage_panel->add_child(import_resize);
		HBoxContainer *io_hb = memnew(HBoxContainer);
		manage_panel->add_child(io_hb);
		Button *import = memnew(Button(TTR("Import...")));
		import->set_h_size_flags(SIZE_EXPAND_FILL);
		import->connect(SceneStringName(pressed), callable_mp(this, &LandscapeEditor::_import_pressed));
		io_hb->add_child(import);
		Button *export_button = memnew(Button(TTR("Export...")));
		export_button->set_h_size_flags(SIZE_EXPAND_FILL);
		export_button->connect(SceneStringName(pressed), callable_mp(this, &LandscapeEditor::_export_pressed));
		io_hb->add_child(export_button);

		manage_panel->add_child(memnew(HSeparator));
		Label *resize_title = memnew(Label(TTR("Change Resolution")));
		resize_title->set_theme_type_variation("HeaderSmall");
		manage_panel->add_child(resize_title);
		resize_x = _make_spin(LandscapeData::MIN_RESOLUTION, LandscapeData::MAX_RESOLUTION, 1, 1025);
		resize_z = _make_spin(LandscapeData::MIN_RESOLUTION, LandscapeData::MAX_RESOLUTION, 1, 1025);
		_add_setting(manage_panel, TTR("Vertices X"), resize_x);
		_add_setting(manage_panel, TTR("Vertices Z"), resize_z);
		Button *resize = memnew(Button(TTR("Resample")));
		resize->set_tooltip_text(TTR("Resample heights and layers to a new resolution, keeping the world size."));
		resize->connect(SceneStringName(pressed), callable_mp(this, &LandscapeEditor::_resize_pressed));
		manage_panel->add_child(resize);

		manage_panel->add_child(memnew(HSeparator));
		Label *debug_title = memnew(Label(TTR("Debug")));
		debug_title->set_theme_type_variation("HeaderSmall");
		manage_panel->add_child(debug_title);
		debug_view = memnew(OptionButton);
		debug_view->add_item(TTR("Disabled"), Landscape3D::DEBUG_VIEW_DISABLED);
		debug_view->add_item(TTR("LOD Levels"), Landscape3D::DEBUG_VIEW_LOD_LEVELS);
		debug_view->add_item(TTR("Patches"), Landscape3D::DEBUG_VIEW_PATCHES);
		debug_view->add_item(TTR("Normals"), Landscape3D::DEBUG_VIEW_NORMALS);
		debug_view->add_item(TTR("Dominant Layer"), Landscape3D::DEBUG_VIEW_LAYERS);
		debug_view->add_item(TTR("Streaming (Page Mip Levels)"), Landscape3D::DEBUG_VIEW_STREAMING);
		debug_view->connect(SceneStringName(item_selected), callable_mp(this, &LandscapeEditor::_debug_view_selected));
		_add_setting(manage_panel, TTR("Debug View"), debug_view, TTR("Colors the landscape by LOD level, patch, normal, dominant layer or by the mip level of the streamed pages."));
		freeze_lod = memnew(CheckBox(TTR("Freeze LOD")));
		freeze_lod->set_tooltip_text(TTR("Keep the current LOD selection and streamed pages while moving the camera, to inspect them."));
		freeze_lod->connect(SceneStringName(toggled), callable_mp(this, &LandscapeEditor::_freeze_lod_toggled));
		manage_panel->add_child(freeze_lod);
	}

	/* Sculpt */
	sculpt_panel = memnew(VBoxContainer);
	main_vb->add_child(sculpt_panel);
	{
		sculpt_tool_group.instantiate();
		GridContainer *grid = memnew(GridContainer);
		grid->set_columns(2);
		sculpt_panel->add_child(grid);
		Button *sculpt = _make_tool_button(grid, TTR("Sculpt"), "ArrowUp", sculpt_tool_group, LandscapeBrush::TOOL_SCULPT, TTR("Raise the terrain. Hold Shift to lower it."));
		_make_tool_button(grid, TTR("Smooth"), "CurveLinear", sculpt_tool_group, LandscapeBrush::TOOL_SMOOTH, TTR("Smooth the terrain heights."));
		_make_tool_button(grid, TTR("Flatten"), "ToolMove", sculpt_tool_group, LandscapeBrush::TOOL_FLATTEN, TTR("Flatten towards the height under the cursor at the start of the stroke. Ctrl+Click picks a target height."));
		_make_tool_button(grid, TTR("Ramp"), "CurveOut", sculpt_tool_group, LandscapeBrush::TOOL_RAMP, TTR("Drag from the start to the end of the ramp."));
		_make_tool_button(grid, TTR("Noise"), "RandomNumberGenerator", sculpt_tool_group, LandscapeBrush::TOOL_NOISE, TTR("Add procedural noise. Hold Shift to invert."));
		_make_tool_button(grid, TTR("Erosion"), "Particles", sculpt_tool_group, LandscapeBrush::TOOL_EROSION, TTR("Thermal erosion: material slides down slopes steeper than the talus angle."));
		_make_tool_button(grid, TTR("Terrace"), "GridLayout", sculpt_tool_group, LandscapeBrush::TOOL_TERRACE, TTR("Create terraces with the given step height."));
		_make_tool_button(grid, TTR("Visibility"), "GuiVisibilityHidden", sculpt_tool_group, LandscapeBrush::TOOL_HOLES, TTR("Paint holes in the landscape (e.g. for cave entrances). Hold Shift to fill them."));
		sculpt->set_pressed(true);
	}

	/* Paint */
	paint_panel = memnew(VBoxContainer);
	main_vb->add_child(paint_panel);
	{
		Label *layers_title = memnew(Label(TTR("Target Layers")));
		layers_title->set_theme_type_variation("HeaderSmall");
		paint_panel->add_child(layers_title);
		layer_list = memnew(ItemList);
		layer_list->set_custom_minimum_size(Size2(0, 160 * EDSCALE));
		layer_list->set_fixed_icon_size(Size2i(32, 32) * EDSCALE);
		layer_list->connect(SceneStringName(item_selected), callable_mp(this, &LandscapeEditor::_layer_selected));
		paint_panel->add_child(layer_list);
		HBoxContainer *layer_hb = memnew(HBoxContainer);
		paint_panel->add_child(layer_hb);
		Button *add = memnew(Button(TTR("Add")));
		add->set_h_size_flags(SIZE_EXPAND_FILL);
		add->connect(SceneStringName(pressed), callable_mp(this, &LandscapeEditor::_add_layer));
		layer_hb->add_child(add);
		Button *remove = memnew(Button(TTR("Remove")));
		remove->set_h_size_flags(SIZE_EXPAND_FILL);
		remove->connect(SceneStringName(pressed), callable_mp(this, &LandscapeEditor::_remove_layer));
		layer_hb->add_child(remove);
		Button *fill = memnew(Button(TTR("Fill")));
		fill->set_h_size_flags(SIZE_EXPAND_FILL);
		fill->set_tooltip_text(TTR("Fill the whole landscape with the selected layer."));
		fill->connect(SceneStringName(pressed), callable_mp(this, &LandscapeEditor::_fill_layer));
		layer_hb->add_child(fill);
		Button *import_weights = memnew(Button(TTR("Import Weights...")));
		import_weights->set_tooltip_text(TTR("Import a grayscale mask (PNG, RAW/R16, EXR...) as the weights of the selected layer."));
		import_weights->connect(SceneStringName(pressed), callable_mp(this, &LandscapeEditor::_import_weights_pressed));
		paint_panel->add_child(import_weights);

		paint_tool_group.instantiate();
		GridContainer *grid = memnew(GridContainer);
		grid->set_columns(2);
		paint_panel->add_child(grid);
		Button *paint = _make_tool_button(grid, TTR("Paint"), "Paint", paint_tool_group, LandscapeBrush::TOOL_PAINT, TTR("Paint the selected layer. Hold Shift to erase it. Ctrl+Click selects the dominant layer under the cursor."));
		_make_tool_button(grid, TTR("Smooth"), "CurveLinear", paint_tool_group, LandscapeBrush::TOOL_PAINT_SMOOTH, TTR("Smooth the layer weights."));
		_make_tool_button(grid, TTR("Flatten"), "ToolMove", paint_tool_group, LandscapeBrush::TOOL_PAINT_FLATTEN, TTR("Set the weight of the selected layer to the target weight. Ctrl+Click picks the weight under the cursor."));
		_make_tool_button(grid, TTR("Noise"), "RandomNumberGenerator", paint_tool_group, LandscapeBrush::TOOL_PAINT_NOISE, TTR("Paint the selected layer modulated by noise."));
		paint->set_pressed(true);
	}

	/* Brush */
	brush_panel = memnew(VBoxContainer);
	main_vb->add_child(brush_panel);
	{
		brush_panel->add_child(memnew(HSeparator));
		Label *brush_title = memnew(Label(TTR("Brush Settings")));
		brush_title->set_theme_type_variation("HeaderSmall");
		brush_panel->add_child(brush_title);

		brush_size = _make_spin(0.1, 8192.0, 0.1, 20.0, "m", true);
		_add_setting(brush_panel, TTR("Brush Size"), brush_size, TTR("Brush radius. Shortcuts: [ and ]."));
		brush_size_slider = memnew(HSlider);
		brush_size_slider->set_min(0.1);
		brush_size_slider->set_max(8192.0);
		brush_size_slider->set_step(0.1);
		brush_size_slider->set_exp_ratio(true);
		brush_size_slider->set_value(20.0);
		brush_size_slider->connect(SceneStringName(value_changed), callable_mp(this, &LandscapeEditor::_brush_size_slider_changed));
		brush_panel->add_child(brush_size_slider);

		brush_falloff = _make_spin(0.0, 1.0, 0.01, 0.5);
		_add_setting(brush_panel, TTR("Brush Falloff"), brush_falloff, TTR("Fraction of the radius used to fade out the brush."));
		brush_strength = _make_spin(0.0, 1.0, 0.01, 0.3, "", true);
		_add_setting(brush_panel, TTR("Tool Strength"), brush_strength);
		falloff_type = memnew(OptionButton);
		falloff_type->add_item(TTR("Smooth"), LandscapeBrush::FALLOFF_SMOOTH);
		falloff_type->add_item(TTR("Linear"), LandscapeBrush::FALLOFF_LINEAR);
		falloff_type->add_item(TTR("Spherical"), LandscapeBrush::FALLOFF_SPHERICAL);
		falloff_type->add_item(TTR("Tip"), LandscapeBrush::FALLOFF_TIP);
		_add_setting(brush_panel, TTR("Falloff Type"), falloff_type);

		alpha_picker = memnew(EditorResourcePicker);
		alpha_picker->set_base_type("Texture2D");
		alpha_picker->connect("resource_changed", callable_mp(this, &LandscapeEditor::_alpha_changed));
		_add_setting(brush_panel, TTR("Alpha Brush"), alpha_picker, TTR("Optional grayscale texture used as brush shape (UE-like alpha brush)."));
		alpha_rotation = _make_spin(-180.0, 180.0, 0.1, 0.0, U"°");
		_add_setting(brush_panel, TTR("Alpha Rotation"), alpha_rotation);

		tool_settings = memnew(VBoxContainer);
		brush_panel->add_child(tool_settings);
		Label *tool_title = memnew(Label(TTR("Tool Settings")));
		tool_title->set_theme_type_variation("HeaderSmall");
		tool_settings->add_child(tool_title);

		smooth_radius = _make_spin(1, 32, 1, 2, "px");
		tool_controls[LandscapeBrush::TOOL_SMOOTH].push_back(_add_setting(tool_settings, TTR("Kernel Radius"), smooth_radius));

		flatten_mode = memnew(OptionButton);
		flatten_mode->add_item(TTR("Both"), LandscapeBrush::FLATTEN_BOTH);
		flatten_mode->add_item(TTR("Raise"), LandscapeBrush::FLATTEN_RAISE);
		flatten_mode->add_item(TTR("Lower"), LandscapeBrush::FLATTEN_LOWER);
		tool_controls[LandscapeBrush::TOOL_FLATTEN].push_back(_add_setting(tool_settings, TTR("Flatten Mode"), flatten_mode));
		flatten_use_target = memnew(CheckBox(TTR("Use Target Height")));
		tool_settings->add_child(flatten_use_target);
		tool_controls[LandscapeBrush::TOOL_FLATTEN].push_back(flatten_use_target);
		flatten_target = _make_spin(-10000.0, 10000.0, 0.01, 0.0, "m", true);
		tool_controls[LandscapeBrush::TOOL_FLATTEN].push_back(_add_setting(tool_settings, TTR("Target Height"), flatten_target));

		ramp_width = _make_spin(0.1, 1000.0, 0.1, 10.0, "m", true);
		tool_controls[LandscapeBrush::TOOL_RAMP].push_back(_add_setting(tool_settings, TTR("Ramp Width"), ramp_width));

		noise_scale = _make_spin(0.1, 1000.0, 0.1, 10.0, "m", true);
		noise_seed = _make_spin(0, 100000, 1, 0);
		tool_controls[LandscapeBrush::TOOL_NOISE].push_back(_add_setting(tool_settings, TTR("Noise Scale"), noise_scale));
		tool_controls[LandscapeBrush::TOOL_NOISE].push_back(_add_setting(tool_settings, TTR("Seed"), noise_seed));

		erosion_talus = _make_spin(1.0, 89.0, 0.1, 35.0, U"°");
		erosion_iterations = _make_spin(1, 64, 1, 4);
		tool_controls[LandscapeBrush::TOOL_EROSION].push_back(_add_setting(tool_settings, TTR("Talus Angle"), erosion_talus));
		tool_controls[LandscapeBrush::TOOL_EROSION].push_back(_add_setting(tool_settings, TTR("Iterations"), erosion_iterations));

		terrace_height = _make_spin(0.01, 1000.0, 0.01, 4.0, "m", true);
		tool_controls[LandscapeBrush::TOOL_TERRACE].push_back(_add_setting(tool_settings, TTR("Step Height"), terrace_height));

		paint_target = _make_spin(0.0, 1.0, 0.01, 1.0);
		tool_controls[LandscapeBrush::TOOL_PAINT].push_back(_add_setting(tool_settings, TTR("Target Weight"), paint_target));

		// The smooth kernel and flatten target also apply to the paint variants of the tools.
		tool_controls[LandscapeBrush::TOOL_PAINT_SMOOTH] = tool_controls[LandscapeBrush::TOOL_SMOOTH];
		tool_controls[LandscapeBrush::TOOL_PAINT_FLATTEN] = tool_controls[LandscapeBrush::TOOL_PAINT];
		tool_controls[LandscapeBrush::TOOL_PAINT_NOISE] = tool_controls[LandscapeBrush::TOOL_NOISE];

		for (SpinBox *spin : { brush_size, brush_falloff, brush_strength, alpha_rotation, smooth_radius, flatten_target, ramp_width, noise_scale, noise_seed, erosion_talus, erosion_iterations, terrace_height, paint_target }) {
			spin->connect(SceneStringName(value_changed), callable_mp(this, &LandscapeEditor::_brush_settings_changed));
		}
		for (OptionButton *option : { falloff_type, flatten_mode }) {
			option->connect(SceneStringName(item_selected), callable_mp(this, &LandscapeEditor::_brush_settings_changed).unbind(1).bind(0.0));
		}

		Label *help = memnew(Label(TTR("LMB: apply  |  Shift+LMB: invert\nCtrl+LMB: pick  |  [ / ]: brush size")));
		help->set_modulate(Color(1, 1, 1, 0.6));
		help->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
		brush_panel->add_child(help);
	}

	create_dialog = memnew(EditorFileDialog);
	create_dialog->set_file_mode(EditorFileDialog::FILE_MODE_SAVE_FILE);
	create_dialog->set_access(EditorFileDialog::ACCESS_RESOURCES);
	create_dialog->add_filter("*.lsdata", TTR("Landscape Data (streamed)"));
	create_dialog->add_filter("*.res", TTR("Binary Resource (loaded entirely)"));
	create_dialog->set_title(TTR("Save Landscape Data As..."));
	create_dialog->connect("file_selected", callable_mp(this, &LandscapeEditor::_create_file_selected));
	add_child(create_dialog);

	import_dialog = memnew(EditorFileDialog);
	import_dialog->set_file_mode(EditorFileDialog::FILE_MODE_OPEN_FILE);
	import_dialog->set_access(EditorFileDialog::ACCESS_FILESYSTEM);
	import_dialog->add_filter("*.png,*.exr,*.r16,*.raw,*.hdr,*.tga,*.jpg,*.webp", TTR("Heightmaps"));
	import_dialog->set_title(TTR("Import Heightmap"));
	import_dialog->connect("file_selected", callable_mp(this, &LandscapeEditor::_import_file_selected));
	add_child(import_dialog);

	export_dialog = memnew(EditorFileDialog);
	export_dialog->set_file_mode(EditorFileDialog::FILE_MODE_SAVE_FILE);
	export_dialog->set_access(EditorFileDialog::ACCESS_FILESYSTEM);
	export_dialog->add_filter("*.exr", TTR("OpenEXR (32-bit float)"));
	export_dialog->add_filter("*.png", TTR("PNG (16-bit, normalized)"));
	export_dialog->add_filter("*.r16,*.raw", TTR("RAW (16-bit, normalized)"));
	export_dialog->set_title(TTR("Export Heightmap"));
	export_dialog->connect("file_selected", callable_mp(this, &LandscapeEditor::_export_file_selected));
	add_child(export_dialog);

	weights_dialog = memnew(EditorFileDialog);
	weights_dialog->set_file_mode(EditorFileDialog::FILE_MODE_OPEN_FILE);
	weights_dialog->set_access(EditorFileDialog::ACCESS_FILESYSTEM);
	weights_dialog->add_filter("*.png,*.exr,*.r16,*.raw,*.tga,*.jpg,*.webp", TTR("Weightmaps"));
	weights_dialog->set_title(TTR("Import Layer Weights"));
	weights_dialog->connect("file_selected", callable_mp(this, &LandscapeEditor::_import_weights_file_selected));
	add_child(weights_dialog);

	mode_tabs->set_current_tab(MODE_SCULPT);
	_mode_changed(MODE_SCULPT);
	_brush_settings_changed();
	_update_info();
}

/* Plugin */

Camera3D *LandscapeEditorPlugin::_get_editor_camera() {
	Node3DEditor *editor = Node3DEditor::get_singleton();
	if (!editor) {
		return nullptr;
	}
	Node3DEditorViewport *viewport = editor->get_last_used_viewport();
	return viewport ? viewport->get_camera_3d() : nullptr;
}

void LandscapeEditorPlugin::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			landscape_editor = memnew(LandscapeEditor);
			EditorDockManager::get_singleton()->add_dock(landscape_editor);
			landscape_editor->close();
			gizmo_plugin.instantiate();
			Node3DEditor::get_singleton()->add_gizmo_plugin(gizmo_plugin);
		} break;

		case NOTIFICATION_EXIT_TREE: {
			Node3DEditor::get_singleton()->remove_gizmo_plugin(gizmo_plugin);
			gizmo_plugin.unref();
			EditorDockManager::get_singleton()->remove_dock(landscape_editor);
			memdelete(landscape_editor);
			landscape_editor = nullptr;
		} break;
	}
}

EditorPlugin::AfterGUIInput LandscapeEditorPlugin::forward_3d_gui_input(Camera3D *p_camera, const Ref<InputEvent> &p_event) {
	if (!landscape_editor || !landscape_editor->is_visible_in_tree()) {
		return EditorPlugin::AFTER_GUI_INPUT_PASS;
	}
	return landscape_editor->forward_3d_gui_input(p_camera, p_event);
}

void LandscapeEditorPlugin::edit(Object *p_object) {
	if (!landscape_editor) {
		return; // Called while the editor shuts down.
	}
	landscape_editor->edit(Object::cast_to<Landscape3D>(p_object));
}

bool LandscapeEditorPlugin::handles(Object *p_object) const {
	return Object::cast_to<Landscape3D>(p_object) != nullptr;
}

void LandscapeEditorPlugin::make_visible(bool p_visible) {
	if (!landscape_editor) {
		return; // Called while the editor shuts down.
	}
	if (p_visible) {
		landscape_editor->make_visible();
		landscape_editor->set_process(true);
	} else {
		if (landscape_editor->get_landscape()) {
			landscape_editor->get_landscape()->set_brush_preview(false);
		}
		landscape_editor->close();
		landscape_editor->set_process(false);
	}
}

LandscapeEditorPlugin::LandscapeEditorPlugin() {
	Landscape3D::editor_camera_callback = &LandscapeEditorPlugin::_get_editor_camera;
}

LandscapeEditorPlugin::~LandscapeEditorPlugin() {
	if (Landscape3D::editor_camera_callback == &LandscapeEditorPlugin::_get_editor_camera) {
		Landscape3D::editor_camera_callback = nullptr;
	}
}
