/**************************************************************************/
/*  landscape_editor_plugin.h                                             */
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

#include "../landscape_3d.h"
#include "../landscape_brush.h"

#include "editor/docks/editor_dock.h"
#include "editor/plugins/editor_plugin.h"
#include "editor/scene/3d/node_3d_editor_gizmos.h"

class Button;
class ButtonGroup;
class CheckBox;
class ConfirmationDialog;
class EditorFileDialog;
class EditorResourcePicker;
class GridContainer;
class HSlider;
class ItemList;
class Label;
class OptionButton;
class SpinBox;
class TabBar;
class VBoxContainer;

class LandscapeGizmoPlugin : public EditorNode3DGizmoPlugin {
	GDCLASS(LandscapeGizmoPlugin, EditorNode3DGizmoPlugin);

public:
	bool has_gizmo(Node3D *p_spatial) override;
	String get_gizmo_name() const override;
	int get_priority() const override;
	bool can_be_hidden() const override;
	void redraw(EditorNode3DGizmo *p_gizmo) override;
};

// UE-like "Landscape Mode" panel: Manage, Sculpt and Paint tools with shared brush settings.
class LandscapeEditor : public EditorDock {
	GDCLASS(LandscapeEditor, EditorDock);

public:
	enum Mode {
		MODE_MANAGE,
		MODE_SCULPT,
		MODE_PAINT,
	};

private:
	static constexpr int UNDO_TILE = 64;

	Landscape3D *landscape = nullptr;
	ObjectID landscape_id;
	Ref<LandscapeBrush> brush;
	Mode mode = MODE_SCULPT;
	LandscapeBrush::Tool sculpt_tool = LandscapeBrush::TOOL_SCULPT;
	LandscapeBrush::Tool paint_tool = LandscapeBrush::TOOL_PAINT;

	// Cursor and stroke state.
	bool cursor_valid = false;
	Vector3 cursor_local;
	bool stroking = false;
	bool stroke_invert = false;
	Vector3 last_dab_local;
	Vector3 ramp_start_local;
	HashMap<Vector2i, Dictionary> undo_tiles;
	Rect2i stroke_rect;

	// UI.
	TabBar *mode_tabs = nullptr;
	VBoxContainer *manage_panel = nullptr;
	VBoxContainer *sculpt_panel = nullptr;
	VBoxContainer *paint_panel = nullptr;
	VBoxContainer *brush_panel = nullptr;
	Label *info_label = nullptr;
	Label *stats_label = nullptr;
	double stats_timer = 0.0;

	// Manage.
	SpinBox *new_size_x = nullptr;
	SpinBox *new_size_z = nullptr;
	OptionButton *size_presets = nullptr;
	SpinBox *new_spacing = nullptr;
	SpinBox *new_height = nullptr;
	Label *new_world_size = nullptr;
	SpinBox *import_min = nullptr;
	SpinBox *import_max = nullptr;
	CheckBox *import_resize = nullptr;
	EditorFileDialog *create_dialog = nullptr;
	EditorFileDialog *import_dialog = nullptr;
	EditorFileDialog *export_dialog = nullptr;
	EditorFileDialog *weights_dialog = nullptr;
	SpinBox *resize_x = nullptr;
	SpinBox *resize_z = nullptr;

	// Tools.
	Ref<ButtonGroup> sculpt_tool_group;
	Ref<ButtonGroup> paint_tool_group;
	ItemList *layer_list = nullptr;
	int selected_layer = 0;
	uint64_t layer_list_hash = 0;
	uint64_t _compute_layer_hash() const;

	// Brush settings.
	SpinBox *brush_size = nullptr;
	HSlider *brush_size_slider = nullptr;
	SpinBox *brush_falloff = nullptr;
	SpinBox *brush_strength = nullptr;
	OptionButton *falloff_type = nullptr;
	EditorResourcePicker *alpha_picker = nullptr;
	SpinBox *alpha_rotation = nullptr;

	// Tool specific settings.
	VBoxContainer *tool_settings = nullptr;
	SpinBox *smooth_radius = nullptr;
	OptionButton *flatten_mode = nullptr;
	CheckBox *flatten_use_target = nullptr;
	SpinBox *flatten_target = nullptr;
	SpinBox *ramp_width = nullptr;
	SpinBox *noise_scale = nullptr;
	SpinBox *noise_seed = nullptr;
	SpinBox *erosion_talus = nullptr;
	SpinBox *erosion_iterations = nullptr;
	SpinBox *terrace_height = nullptr;
	SpinBox *paint_target = nullptr;
	HashMap<LandscapeBrush::Tool, Vector<Control *>> tool_controls;

	bool updating_ui = false;

	Control *_add_setting(VBoxContainer *p_parent, const String &p_label, Control *p_control, const String &p_tooltip = String());
	SpinBox *_make_spin(double p_min, double p_max, double p_step, double p_value, const String &p_suffix = String(), bool p_allow_greater = false);
	Button *_make_tool_button(Container *p_parent, const String &p_text, const String &p_icon, const Ref<ButtonGroup> &p_group, LandscapeBrush::Tool p_tool, const String &p_tooltip);

	void _mode_changed(int p_mode);
	void _tool_selected(LandscapeBrush::Tool p_tool);
	void _update_tool_settings();
	void _brush_settings_changed(double p_value = 0.0);
	void _brush_size_slider_changed(double p_value);
	void _alpha_changed(const Ref<Resource> &p_resource);
	void _update_info();
	void _update_new_world_size(double p_value = 0.0);
	void _size_preset_selected(int p_index);

	void _update_layer_list();
	void _layer_selected(int p_index);
	void _add_layer();
	void _remove_layer();
	void _fill_layer();
	void _import_weights_pressed();
	void _import_weights_file_selected(const String &p_path);

	void _create_pressed();
	void _create_file_selected(const String &p_path);
	void _import_pressed();
	void _import_file_selected(const String &p_path);
	void _export_pressed();
	void _export_file_selected(const String &p_path);
	void _resize_pressed();

	bool _update_cursor(Camera3D *p_camera, const Point2 &p_position);
	void _update_brush_preview();
	void _begin_stroke(bool p_invert);
	void _apply_dab(const Vector3 &p_local, real_t p_delta);
	void _end_stroke();
	void _cancel_stroke();
	void _snapshot(const Rect2i &p_rect);
	void _pick(const Vector3 &p_local);
	LandscapeBrush::Tool _get_current_tool() const;
	bool _validate_landscape();
	void _set_data_edited();

	void _apply_regions(const Ref<LandscapeData> &p_data, const Array &p_regions);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	EditorPlugin::AfterGUIInput forward_3d_gui_input(Camera3D *p_camera, const Ref<InputEvent> &p_event);
	void edit(Landscape3D *p_landscape);
	Landscape3D *get_landscape() const { return landscape; }

	LandscapeEditor();
};

class LandscapeEditorPlugin : public EditorPlugin {
	GDCLASS(LandscapeEditorPlugin, EditorPlugin);

	LandscapeEditor *landscape_editor = nullptr;
	Ref<LandscapeGizmoPlugin> gizmo_plugin;

	static Camera3D *_get_editor_camera();

protected:
	void _notification(int p_what);

public:
	virtual EditorPlugin::AfterGUIInput forward_3d_gui_input(Camera3D *p_camera, const Ref<InputEvent> &p_event) override;
	virtual String get_plugin_name() const override { return "Landscape3D"; }
	virtual bool has_main_screen() const override { return false; }
	virtual void edit(Object *p_object) override;
	virtual bool handles(Object *p_object) const override;
	virtual void make_visible(bool p_visible) override;

	LandscapeEditorPlugin();
	~LandscapeEditorPlugin();
};
