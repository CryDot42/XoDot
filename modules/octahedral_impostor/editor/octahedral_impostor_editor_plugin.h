/**************************************************************************/
/*  octahedral_impostor_editor_plugin.h                                   */
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

#include "editor/inspector/editor_context_menu_plugin.h"
#include "editor/plugins/editor_plugin.h"
#include "editor/plugins/editor_resource_conversion_plugin.h"
#include "scene/gui/dialogs.h"

class Button;
class Camera3D;
class CheckBox;
class EditorFileDialog;
class Label;
class LineEdit;
class MeshInstance3D;
class Node3D;
class OctahedralImpostorBaker;
class OptionButton;
class SpinBox;
class SubViewport;
class SubViewportContainer;
class TextureRect;
class Timer;

// Settings of the bake of octahedral impostors, with a preview of the result, then the bake of
// each source (nodes of the edited scene or scene files) and the setup of the impostors as far
// levels of detail of the nodes.
class OctahedralImpostorDialog : public ConfirmationDialog {
	GDCLASS(OctahedralImpostorDialog, ConfirmationDialog);

	enum PreviewMode {
		PREVIEW_IMPOSTOR,
		PREVIEW_ALBEDO,
		PREVIEW_NORMAL,
		PREVIEW_DEPTH,
	};

	struct Source {
		ObjectID node; // In the edited scene.
		String scene_path; // Or a scene file.
		String name;
	};

	Vector<Source> sources;
	Node3D *scene_instance = nullptr; // Instance of the first scene file, for the preview.

	Label *source_label = nullptr;
	OptionButton *layout_option = nullptr;
	SpinBox *frames_spin = nullptr;
	OptionButton *atlas_size_option = nullptr;
	OptionButton *supersampling_option = nullptr;
	CheckBox *orm_check = nullptr;
	Label *info_label = nullptr;
	CheckBox *add_to_scene_check = nullptr;
	SpinBox *lod_distance_spin = nullptr;
	CheckBox *fade_check = nullptr;
	Label *path_label = nullptr;
	LineEdit *path_edit = nullptr;
	Button *path_button = nullptr;
	EditorFileDialog *file_dialog = nullptr;
	Label *warning_label = nullptr;

	OptionButton *preview_mode_option = nullptr;
	SubViewportContainer *preview_container = nullptr;
	SubViewport *preview_viewport = nullptr;
	Camera3D *preview_camera = nullptr;
	MeshInstance3D *preview_instance = nullptr;
	TextureRect *preview_texture = nullptr;
	Label *preview_status = nullptr;
	Timer *preview_timer = nullptr;
	Ref<OctahedralImpostorBaker> preview_baker;
	Vector2 preview_rotation = Vector2(-0.3, 0.6);
	float preview_zoom = 1.3;
	bool lod_distance_edited = false;

	bool _is_batch() const;
	Node3D *_get_source_node(int p_index);
	void _free_scene_instance();
	String _get_default_path(const Source &p_source, bool p_directory) const;
	Ref<OctahedralImpostorBaker> _create_baker() const;
	void _open(const Vector<Source> &p_sources);

	void _settings_changed(int p_value = 0);
	void _lod_distance_changed(double p_value);
	void _add_to_scene_toggled(bool p_pressed);
	void _update_info();
	void _queue_preview();
	void _update_preview();
	void _update_preview_mode(int p_mode = 0);
	void _update_preview_camera();
	void _preview_input(const Ref<InputEvent> &p_event);
	void _browse_path();
	void _path_selected(const String &p_path);
	void _save_settings();
	void _bake();
	void _add_impostor_to_scene(Node3D *p_node, const Ref<Mesh> &p_mesh, const Ref<OctahedralImpostorBaker> &p_baker, int &r_skipped_nodes);

protected:
	void _notification(int p_what);

public:
	void popup_for_nodes(const Vector<Node3D *> &p_nodes);
	void popup_for_scenes(const Vector<String> &p_paths);

	OctahedralImpostorDialog();
	~OctahedralImpostorDialog();
};

class OctahedralImpostorContextMenuPlugin : public EditorContextMenuPlugin {
	GDCLASS(OctahedralImpostorContextMenuPlugin, EditorContextMenuPlugin);

	OctahedralImpostorDialog *dialog = nullptr;
	bool filesystem = false;

	void _scene_tree_option(const Dictionary &p_data);
	void _filesystem_option(const Dictionary &p_data);

public:
	using EditorContextMenuPlugin::get_options;
	virtual void get_options(const OptionsData &p_data) override;

	void setup(OctahedralImpostorDialog *p_dialog, bool p_filesystem);
};

class OctahedralImpostorMaterialConversionPlugin : public EditorResourceConversionPlugin {
	GDCLASS(OctahedralImpostorMaterialConversionPlugin, EditorResourceConversionPlugin);

public:
	virtual String converts_to() const override;
	virtual bool handles(const Ref<Resource> &p_resource) const override;
	virtual Ref<Resource> convert(const Ref<Resource> &p_resource) const override;
};

class OctahedralImpostorEditorPlugin : public EditorPlugin {
	GDCLASS(OctahedralImpostorEditorPlugin, EditorPlugin);

	OctahedralImpostorDialog *dialog = nullptr;
	Ref<OctahedralImpostorContextMenuPlugin> scene_tree_menu;
	Ref<OctahedralImpostorContextMenuPlugin> filesystem_menu;
	Ref<OctahedralImpostorMaterialConversionPlugin> material_conversion;

protected:
	void _notification(int p_what);

public:
	virtual String get_plugin_name() const override { return "OctahedralImpostor"; }

	OctahedralImpostorEditorPlugin();
};
