/**************************************************************************/
/*  landscape_foliage_editor.h                                            */
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
#include "../landscape_foliage_3d.h"
#include "../landscape_foliage_type.h"

#include "core/object/undo_redo.h"
#include "editor/inspector/editor_inspector.h"
#include "editor/plugins/editor_plugin.h"
#include "scene/gui/box_container.h"
#include "scene/gui/dialogs.h"

class Button;
class ButtonGroup;
class CheckBox;
class ConfirmationDialog;
class EditorFileDialog;
class EditorResourcePicker;
class GridContainer;
class HSlider;
class Label;
class MenuButton;
class OptionButton;
class SpinBox;
class Tree;

// Distances of the levels of detail of a foliage type, drawn as colored bands on a ruler.
// The boundaries (LOD start distances and the cull distance) can be dragged.
class LandscapeFoliageLodBar : public Control {
	GDCLASS(LandscapeFoliageLodBar, Control);

	Ref<LandscapeFoliageType> type;
	int hovered = -1;
	int dragging = -1;
	float drag_start_value = 0.0;
	float display_range = 100.0;

	float _get_display_range() const;
	float _distance_to_x(float p_distance) const;
	float _x_to_distance(float p_x) const;
	int _get_handle_count() const;
	float _get_handle_distance(int p_handle) const;
	int _get_handle_at(float p_x) const;

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	static Color get_lod_color(int p_lod);

	void set_type(const Ref<LandscapeFoliageType> &p_type);
	void gui_input(const Ref<InputEvent> &p_event) override;
	Size2 get_minimum_size() const override;
};

// The LOD window: levels of detail, cull distance and LOD transition of a foliage type, in a
// window of their own instead of nested arrays in the inspector.
class LandscapeFoliageLodDialog : public AcceptDialog {
	GDCLASS(LandscapeFoliageLodDialog, AcceptDialog);

	static LandscapeFoliageLodDialog *singleton;

	Ref<LandscapeFoliageType> type;
	LandscapeFoliageLodBar *bar = nullptr;
	GridContainer *table = nullptr;
	struct Row {
		Label *name = nullptr;
		EditorResourcePicker *mesh = nullptr;
		SpinBox *start = nullptr;
		Label *end = nullptr;
		CheckBox *shadows = nullptr;
		Label *triangles = nullptr;
		Button *remove = nullptr;
	};
	LocalVector<Row> rows;
	SpinBox *cull_distance = nullptr;
	SpinBox *cull_random = nullptr;
	SpinBox *transition = nullptr;
	Button *add_lod = nullptr;
	Label *warning = nullptr;
	bool updating = false;

	void _type_changed();
	void _rebuild_rows();
	void _update_values();
	static int64_t _count_triangles(const Ref<Mesh> &p_mesh);

	Dictionary _snapshot() const;
	void _commit(const String &p_action, const Dictionary &p_before, UndoRedo::MergeMode p_merge = UndoRedo::MERGE_DISABLE);
	void _apply_snapshot(const Ref<LandscapeFoliageType> &p_type, const Dictionary &p_state);

	void _mesh_changed(const Ref<Resource> &p_mesh, int p_lod);
	void _start_changed(double p_value, int p_lod);
	void _shadows_toggled(bool p_pressed, int p_lod);
	void _remove_lod(int p_lod);
	void _add_lod();
	void _settings_changed(double p_value);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	static LandscapeFoliageLodDialog *get_singleton() { return singleton; }

	// Called by the distance bar while a boundary is dragged (and once more when released).
	void set_boundary(int p_handle, float p_distance, bool p_finished, float p_before);
	void edit(const Ref<LandscapeFoliageType> &p_type);
	Ref<LandscapeFoliageType> get_type() const { return type; }

	LandscapeFoliageLodDialog();
	~LandscapeFoliageLodDialog();
};

// "LODs" row of the inspector of a foliage type: a summary and a button opening the LOD window.
class EditorPropertyFoliageLods : public EditorProperty {
	GDCLASS(EditorPropertyFoliageLods, EditorProperty);

	Label *summary = nullptr;
	Button *edit_button = nullptr;

	void _edit_pressed();

public:
	void update_property() override;

	EditorPropertyFoliageLods();
};

// Compact grid of the 16 landscape layers (with their names as tooltips).
class EditorPropertyFoliageLayers : public EditorProperty {
	GDCLASS(EditorPropertyFoliageLayers, EditorProperty);

	Button *buttons[LandscapeData::MAX_LAYERS] = {};
	bool updating = false;

	void _toggled(bool p_pressed);

protected:
	void _set_read_only(bool p_read_only) override;

public:
	void update_property() override;

	EditorPropertyFoliageLayers();
};

class LandscapeFoliageInspectorPlugin : public EditorInspectorPlugin {
	GDCLASS(LandscapeFoliageInspectorPlugin, EditorInspectorPlugin);

public:
	bool can_handle(Object *p_object) override;
	bool parse_property(Object *p_object, const Variant::Type p_type, const String &p_path, const PropertyHint p_hint, const String &p_hint_text, const BitField<PropertyUsageFlags> p_usage, const bool p_wide = false) override;
};

// Foliage tab of the Landscape dock (UE-like "Foliage Mode"): the palette of foliage types of a
// LandscapeFoliage3D node, the painting tools and the brush.
class LandscapeFoliagePanel : public VBoxContainer {
	GDCLASS(LandscapeFoliagePanel, VBoxContainer);

public:
	enum Tool {
		TOOL_PAINT,
		TOOL_ERASE,
		TOOL_SINGLE,
		TOOL_REAPPLY,
	};

	enum AddMenu {
		ADD_NEW_TYPE,
		ADD_FROM_FILES,
	};

	// Landscape edited by the dock (for the layer names of the inspector).
	static ObjectID edited_landscape;

private:
	Landscape3D *landscape = nullptr;
	ObjectID landscape_id;
	ObjectID foliage_id;
	Tool tool = TOOL_PAINT;
	HashSet<ObjectID> unchecked_types; // Types excluded from painting (per session).
	int selected_type = -1;

	// Cursor and stroke.
	bool cursor_valid = false;
	Vector3 cursor_global;
	bool stroking = false;
	bool stroke_erase = false;
	Vector3 last_dab;
	struct UndoCell {
		int type = 0;
		Rect2 rect;
		PackedFloat32Array before;
	};
	HashMap<uint64_t, UndoCell> undo_cells;

	// UI.
	OptionButton *foliage_select = nullptr;
	uint64_t foliage_list_hash = 0;
	Tree *palette = nullptr;
	uint64_t palette_hash = 0;
	MenuButton *add_type = nullptr;
	Button *remove_type = nullptr;
	Button *edit_type = nullptr;
	Button *edit_lods = nullptr;
	Ref<ButtonGroup> tool_group;
	SpinBox *brush_size = nullptr;
	HSlider *brush_size_slider = nullptr;
	SpinBox *paint_density = nullptr;
	SpinBox *erase_density = nullptr;
	Label *info = nullptr;
	Label *stats = nullptr;
	double stats_timer = 0.0;
	EditorFileDialog *file_dialog = nullptr;
	ConfirmationDialog *fill_confirm = nullptr;
	bool updating = false;

	LandscapeFoliage3D *_get_foliage() const;
	bool _validate_landscape();
	Vector<int> _get_active_types() const;

	void _update_foliage_list();
	void _foliage_selected(int p_index);
	void _create_foliage();
	void _update_palette();
	void _palette_edited();
	void _update_palette_counts();
	void _preview_ready(const String &p_path, const Ref<Texture2D> &p_preview, const Ref<Texture2D> &p_small_preview, ObjectID p_type);
	void _palette_selected();
	void _palette_activated();
	void _update_buttons();
	void _add_menu_pressed(int p_id);
	void _files_selected(const PackedStringArray &p_files);
	void _add_resources(const Vector<Ref<Resource>> &p_resources);
	void _add_type(const Ref<LandscapeFoliageType> &p_type);
	void _remove_type();
	void _edit_type();
	void _edit_lods();
	void _restore_type(Object *p_foliage, int p_index, const Ref<LandscapeFoliageType> &p_type, const PackedByteArray &p_data);
	void _tool_selected(int p_tool);
	void _brush_size_changed(double p_value);
	void _fill_pressed();
	void _fill_confirmed();
	void _clear_pressed();
	void _commit_type_data(const String &p_action, const Vector<int> &p_types, const Vector<PackedByteArray> &p_before);

	Vector<Ref<Resource>> _get_dropped_resources(const Variant &p_data) const;
	bool _can_drop_data_fw(const Point2 &p_point, const Variant &p_data) const;
	void _drop_data_fw(const Point2 &p_point, const Variant &p_data);

	bool _update_cursor(Camera3D *p_camera, const Point2 &p_position);
	void _snapshot(int p_type, const Vector3 &p_global_center, real_t p_radius);
	void _begin_stroke(bool p_erase);
	void _apply_dab(const Vector3 &p_global);
	void _end_stroke();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	EditorPlugin::AfterGUIInput forward_3d_gui_input(Camera3D *p_camera, const Ref<InputEvent> &p_event);
	void process(double p_delta);
	void update_brush_preview();
	void edit(Landscape3D *p_landscape);
	void edit_foliage(LandscapeFoliage3D *p_foliage);
	void cancel_stroke();
	String get_statistics_text() const;

	LandscapeFoliagePanel();
};
