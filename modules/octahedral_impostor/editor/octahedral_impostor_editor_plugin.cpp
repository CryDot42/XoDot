/**************************************************************************/
/*  octahedral_impostor_editor_plugin.cpp                                 */
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

#include "octahedral_impostor_editor_plugin.h"

#include "octahedral_impostor_baker.h"

#include "core/io/dir_access.h"
#include "core/io/resource_loader.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/gui/editor_file_dialog.h"
#include "editor/gui/editor_toaster.h"
#include "editor/scene/material_editor_plugin.h"
#include "editor/settings/editor_settings.h"
#include "editor/themes/editor_scale.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/gui/box_container.h"
#include "scene/gui/check_box.h"
#include "scene/gui/grid_container.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/option_button.h"
#include "scene/gui/separator.h"
#include "scene/gui/spin_box.h"
#include "scene/gui/subviewport_container.h"
#include "scene/gui/texture_rect.h"
#include "scene/main/timer.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/3d/world_3d.h"
#include "scene/resources/environment.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/packed_scene.h"
#include "servers/rendering/rendering_server.h"

static const int atlas_sizes[] = { 512, 1024, 2048, 4096, 8192 };
static const int supersampling_factors[] = { 1, 2, 4 };
// Largest atlas of the preview, which bakes again at each change of the settings.
static const int PREVIEW_ATLAS_SIZE = 1024;

static Node3D *_instantiate_scene(const String &p_path) {
	const Ref<PackedScene> scene = ResourceLoader::load(p_path, "PackedScene");
	ERR_FAIL_COND_V_MSG(scene.is_null(), nullptr, vformat("Can't load the scene '%s'.", p_path));
	Node *node = scene->instantiate();
	Node3D *node_3d = Object::cast_to<Node3D>(node);
	if (!node_3d && node) {
		memdelete(node);
	}
	ERR_FAIL_NULL_V_MSG(node_3d, nullptr, vformat("The root of the scene '%s' isn't a Node3D.", p_path));
	return node_3d;
}

// Whether the properties of the node can be changed in the edited scene (not a node of an
// instantiated scene without editable children).
static bool _is_node_editable(Node *p_node, Node *p_edited_scene) {
	if (p_node == p_edited_scene) {
		return true;
	}
	Node *owner = p_node->get_owner();
	while (owner) {
		if (owner == p_edited_scene) {
			return true;
		}
		if (!p_edited_scene->is_editable_instance(owner)) {
			return false;
		}
		owner = owner->get_owner();
	}
	return false;
}

/* OctahedralImpostorDialog */

bool OctahedralImpostorDialog::_is_batch() const {
	return sources.size() > 1;
}

Node3D *OctahedralImpostorDialog::_get_source_node(int p_index) {
	ERR_FAIL_INDEX_V(p_index, sources.size(), nullptr);
	const Source &source = sources[p_index];
	if (source.scene_path.is_empty()) {
		return ObjectDB::get_instance<Node3D>(source.node);
	}
	if (p_index == 0) {
		if (!scene_instance) {
			scene_instance = _instantiate_scene(source.scene_path);
		}
		return scene_instance;
	}
	return nullptr;
}

void OctahedralImpostorDialog::_free_scene_instance() {
	if (scene_instance) {
		memdelete(scene_instance);
		scene_instance = nullptr;
	}
}

String OctahedralImpostorDialog::_get_default_path(const Source &p_source, bool p_directory) const {
	String directory;
	if (!p_source.scene_path.is_empty()) {
		directory = p_source.scene_path.get_base_dir();
	} else if (EditorNode::get_singleton()->get_edited_scene()) {
		directory = EditorNode::get_singleton()->get_edited_scene()->get_scene_file_path().get_base_dir();
	}
	if (directory.is_empty()) {
		directory = "res://";
	}
	if (p_directory) {
		return directory;
	}
	return directory.path_join(p_source.name.to_snake_case() + "_impostor.tres");
}

Ref<OctahedralImpostorBaker> OctahedralImpostorDialog::_create_baker() const {
	Ref<OctahedralImpostorBaker> baker;
	baker.instantiate();
	baker->set_layout(OctahedralImpostorMaterial3D::Layout(layout_option->get_selected_id()));
	baker->set_frames(frames_spin->get_value());
	baker->set_atlas_size(atlas_size_option->get_selected_id());
	baker->set_supersampling(supersampling_option->get_selected_id());
	baker->set_bake_orm(orm_check->is_pressed());
	return baker;
}

void OctahedralImpostorDialog::_open(const Vector<Source> &p_sources) {
	_free_scene_instance();
	sources = p_sources;
	ERR_FAIL_COND(sources.is_empty());

	if (_is_batch()) {
		source_label->set_text(vformat(TTR("Sources: %d"), sources.size()));
		String names;
		for (int i = 0; i < sources.size(); i++) {
			names += (i > 0 ? "\n" : "") + sources[i].name;
		}
		source_label->set_tooltip_text(names);
		path_label->set_text(TTR("Output Folder:"));
		path_edit->set_tooltip_text(TTR("The impostors are saved in this folder as \"<name>_impostor.tres\", with their atlases."));
	} else {
		source_label->set_text(vformat(TTR("Source: %s"), sources[0].name));
		source_label->set_tooltip_text(sources[0].scene_path);
		path_label->set_text(TTR("Mesh Path:"));
		path_edit->set_tooltip_text(TTR("The impostor is saved as a quad mesh with its material, the atlases are saved next to it."));
	}
	path_edit->set_text(_get_default_path(sources[0], _is_batch()));

	const bool has_nodes = sources[0].scene_path.is_empty() && EditorNode::get_singleton()->get_edited_scene();
	add_to_scene_check->set_visible(has_nodes);
	_add_to_scene_toggled(add_to_scene_check->is_pressed());
	lod_distance_edited = false;

	preview_baker.unref();
	preview_instance->set_mesh(Ref<Mesh>());
	preview_texture->set_texture(Ref<Texture2D>());
	_update_info();
	popup_centered(Size2(800, 540) * EDSCALE);
	_queue_preview();
}

void OctahedralImpostorDialog::popup_for_nodes(const Vector<Node3D *> &p_nodes) {
	Vector<Source> new_sources;
	for (Node3D *node : p_nodes) {
		Source source;
		source.node = node->get_instance_id();
		source.name = node->get_name();
		new_sources.push_back(source);
	}
	_open(new_sources);
}

void OctahedralImpostorDialog::popup_for_scenes(const Vector<String> &p_paths) {
	Vector<Source> new_sources;
	for (const String &path : p_paths) {
		Source source;
		source.scene_path = path;
		source.name = path.get_file().get_basename();
		new_sources.push_back(source);
	}
	_open(new_sources);
}

void OctahedralImpostorDialog::_settings_changed(int p_value) {
	_update_info();
	_queue_preview();
}

void OctahedralImpostorDialog::_lod_distance_changed(double p_value) {
	lod_distance_edited = true;
}

void OctahedralImpostorDialog::_add_to_scene_toggled(bool p_pressed) {
	const bool enabled = add_to_scene_check->is_visible() && p_pressed;
	lod_distance_spin->set_editable(enabled);
	fade_check->set_disabled(!enabled);
}

void OctahedralImpostorDialog::_update_info() {
	const Ref<OctahedralImpostorBaker> baker = _create_baker();
	const int frame_size = baker->get_frame_size();
	const int atlas_size = baker->get_baked_atlas_size();
	const int maps = baker->is_baking_orm() ? 3 : 2;
	// VRAM compressed at one byte per pixel (BPTC/ASTC 4x4), with mipmaps.
	const float memory = float(atlas_size) * atlas_size * maps * 4.0 / 3.0 / (1024.0 * 1024.0);
	info_label->set_text(vformat(TTR("Frames: %d x %d px. Atlas: %d x %d px.\nVideo memory: %.1f MiB (compressed)."), frame_size, frame_size, atlas_size, atlas_size, memory));

	String warning;
	if (frame_size < 4 * OctahedralImpostorBaker::FRAME_PADDING) {
		warning = TTR("The frames are too small: increase the atlas size or reduce the number of frames.");
	} else if (frame_size < 32) {
		warning = TTR("The frames are small, the impostor will look blurry: increase the atlas size or reduce the number of frames.");
	}
	for (int i = 0; i < sources.size() && warning.is_empty(); i++) {
		if (sources[i].scene_path.is_empty()) {
			Node3D *node = ObjectDB::get_instance<Node3D>(sources[i].node);
			if (!node || !OctahedralImpostorBaker::has_geometry(node)) {
				warning = vformat(TTR("\"%s\" has no visible geometry to bake."), sources[i].name);
			}
		}
	}
	warning_label->set_text(warning);
	warning_label->set_visible(!warning.is_empty());
	get_ok_button()->set_disabled(frame_size < 4 * OctahedralImpostorBaker::FRAME_PADDING);
}

void OctahedralImpostorDialog::_queue_preview() {
	preview_status->set_text(TTR("Baking the preview..."));
	preview_status->show();
	preview_timer->start();
}

void OctahedralImpostorDialog::_update_preview() {
	if (!is_visible() || sources.is_empty()) {
		return;
	}
	Node3D *node = _get_source_node(0);
	if (!node || !OctahedralImpostorBaker::has_geometry(node)) {
		preview_status->set_text(TTR("No visible geometry to bake."));
		preview_status->show();
		return;
	}

	preview_baker = _create_baker();
	preview_baker->set_atlas_size(MIN(preview_baker->get_atlas_size(), PREVIEW_ATLAS_SIZE));
	preview_baker->set_supersampling(MIN(preview_baker->get_supersampling(), 2));
	preview_baker->set_bake_orm(false);
	if (preview_baker->bake(node, false) != OK) {
		preview_baker.unref();
		preview_status->set_text(TTR("The preview couldn't be baked, see the Output panel."));
		preview_status->show();
		return;
	}
	preview_status->hide();

	const Ref<OctahedralImpostorMaterial3D> material = preview_baker->create_material();
	Ref<QuadMesh> mesh;
	mesh.instantiate();
	mesh->set_material(material);
	mesh->set_custom_aabb(material->get_bounds());
	preview_instance->set_mesh(mesh);

	if (!lod_distance_edited) {
		// The distance at which a frame is shown at about its resolution, on a 1080p screen with
		// a vertical field of view of 75 degrees.
		const float pixel_angle = 2.0 * Math::tan(Math::deg_to_rad(75.0 / 2.0)) / 1080.0;
		const int frame_size = _create_baker()->get_frame_size();
		const float distance = 2.0 * preview_baker->get_sphere_radius() / (MAX(frame_size, 1) * pixel_angle);
		lod_distance_spin->set_value_no_signal(Math::snapped(distance, distance > 20.0 ? 1.0 : 0.1));
	}

	_update_preview_camera();
	_update_preview_mode(preview_mode_option->get_selected_id());
}

void OctahedralImpostorDialog::_update_preview_mode(int p_mode) {
	const PreviewMode preview_mode = PreviewMode(preview_mode_option->get_selected_id());
	preview_container->set_visible(preview_mode == PREVIEW_IMPOSTOR);
	preview_texture->set_visible(preview_mode != PREVIEW_IMPOSTOR);
	if (preview_mode == PREVIEW_IMPOSTOR || preview_baker.is_null()) {
		preview_texture->set_texture(Ref<Texture2D>());
		return;
	}

	Ref<Image> image;
	if (preview_mode == PREVIEW_ALBEDO) {
		image = preview_baker->get_albedo_image();
	} else {
		// Normal in RGB, depth in A.
		image = preview_baker->get_normal_depth_image()->duplicate();
		uint8_t *pixels = image->ptrw();
		const int64_t pixel_count = int64_t(image->get_width()) * image->get_height();
		for (int64_t i = 0; i < pixel_count; i++) {
			uint8_t *pixel = pixels + i * 4;
			if (preview_mode == PREVIEW_DEPTH) {
				pixel[0] = pixel[1] = pixel[2] = pixel[3];
			}
			pixel[3] = 255;
		}
	}
	preview_texture->set_texture(ImageTexture::create_from_image(image));
}

void OctahedralImpostorDialog::_update_preview_camera() {
	Vector3 center;
	float radius = 1.0;
	if (preview_baker.is_valid()) {
		center = preview_baker->get_sphere_center();
		radius = preview_baker->get_sphere_radius();
	}
	const float fov = 30.0;
	const float distance = radius / Math::sin(Math::deg_to_rad(fov * 0.5)) * preview_zoom;
	const Basis rotation = Basis::from_euler(Vector3(preview_rotation.x, preview_rotation.y, 0.0));
	preview_camera->set_perspective(fov, distance * 0.05, distance + radius * 4.0);
	preview_camera->set_transform(Transform3D(rotation, center + rotation.xform(Vector3(0, 0, distance))));
}

void OctahedralImpostorDialog::_preview_input(const Ref<InputEvent> &p_event) {
	const Ref<InputEventMouseMotion> motion = p_event;
	if (motion.is_valid() && motion->get_button_mask().has_flag(MouseButtonMask::LEFT)) {
		preview_rotation.x = CLAMP(preview_rotation.x - motion->get_relative().y * 0.01, -Math::PI * 0.49, Math::PI * 0.49);
		preview_rotation.y -= motion->get_relative().x * 0.01;
		_update_preview_camera();
	}
	const Ref<InputEventMouseButton> button = p_event;
	if (button.is_valid() && button->is_pressed()) {
		if (button->get_button_index() == MouseButton::WHEEL_UP) {
			preview_zoom = MAX(preview_zoom / 1.1, 0.2);
			_update_preview_camera();
		} else if (button->get_button_index() == MouseButton::WHEEL_DOWN) {
			preview_zoom = MIN(preview_zoom * 1.1, 20.0);
			_update_preview_camera();
		}
	}
}

void OctahedralImpostorDialog::_browse_path() {
	file_dialog->clear_filters();
	if (_is_batch()) {
		file_dialog->set_file_mode(EditorFileDialog::FILE_MODE_OPEN_DIR);
		file_dialog->set_title(TTR("Select the Folder of the Impostors"));
		file_dialog->set_current_dir(path_edit->get_text());
	} else {
		file_dialog->set_file_mode(EditorFileDialog::FILE_MODE_SAVE_FILE);
		file_dialog->set_title(TTR("Save the Impostor Mesh"));
		file_dialog->add_filter("*.tres", TTR("Text Resource"));
		file_dialog->add_filter("*.res", TTR("Binary Resource"));
		file_dialog->set_current_path(path_edit->get_text());
	}
	file_dialog->popup_file_dialog();
}

void OctahedralImpostorDialog::_path_selected(const String &p_path) {
	path_edit->set_text(p_path);
}

void OctahedralImpostorDialog::_save_settings() {
	EditorSettings *settings = EditorSettings::get_singleton();
	settings->set_project_metadata("octahedral_impostor", "layout", layout_option->get_selected_id());
	settings->set_project_metadata("octahedral_impostor", "frames", int(frames_spin->get_value()));
	settings->set_project_metadata("octahedral_impostor", "atlas_size", atlas_size_option->get_selected_id());
	settings->set_project_metadata("octahedral_impostor", "supersampling", supersampling_option->get_selected_id());
	settings->set_project_metadata("octahedral_impostor", "orm", orm_check->is_pressed());
	settings->set_project_metadata("octahedral_impostor", "add_to_scene", add_to_scene_check->is_pressed());
	settings->set_project_metadata("octahedral_impostor", "fade", fade_check->is_pressed());
}

void OctahedralImpostorDialog::_add_impostor_to_scene(Node3D *p_node, const Ref<Mesh> &p_mesh, const Ref<OctahedralImpostorBaker> &p_baker, int &r_skipped_nodes) {
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	Node *edited_scene = EditorNode::get_singleton()->get_edited_scene();
	const float lod_distance = lod_distance_spin->get_value();
	const float lod_margin = lod_distance * 0.1;
	const GeometryInstance3D::VisibilityRangeFadeMode fade_mode = fade_check->is_pressed() ? GeometryInstance3D::VISIBILITY_RANGE_FADE_SELF : GeometryInstance3D::VISIBILITY_RANGE_FADE_DISABLED;

	// A rebake updates the mesh in place: the impostor already in the node is kept.
	MeshInstance3D *impostor = nullptr;
	for (int i = 0; i < p_node->get_child_count(); i++) {
		MeshInstance3D *child = Object::cast_to<MeshInstance3D>(p_node->get_child(i));
		if (child && child->get_mesh() == p_mesh) {
			impostor = child;
			break;
		}
	}
	if (!impostor) {
		impostor = memnew(MeshInstance3D);
		impostor->set_name("Impostor");
		impostor->set_mesh(p_mesh);
		// The impostor is a level of detail, not part of the baked lighting.
		impostor->set_gi_mode(GeometryInstance3D::GI_MODE_DISABLED);
		impostor->set_visibility_range_begin(lod_distance);
		impostor->set_visibility_range_begin_margin(lod_margin);
		impostor->set_visibility_range_fade_mode(fade_mode);
		undo_redo->add_do_method(p_node, "add_child", impostor, true);
		undo_redo->add_do_method(impostor, "set_owner", edited_scene);
		undo_redo->add_do_reference(impostor);
		undo_redo->add_undo_method(p_node, "remove_child", impostor);
	} else {
		undo_redo->add_do_property(impostor, "visibility_range_begin", lod_distance);
		undo_redo->add_undo_property(impostor, "visibility_range_begin", impostor->get_visibility_range_begin());
		undo_redo->add_do_property(impostor, "visibility_range_begin_margin", lod_margin);
		undo_redo->add_undo_property(impostor, "visibility_range_begin_margin", impostor->get_visibility_range_begin_margin());
		undo_redo->add_do_property(impostor, "visibility_range_fade_mode", fade_mode);
		undo_redo->add_undo_property(impostor, "visibility_range_fade_mode", impostor->get_visibility_range_fade_mode());
	}

	// The baked geometry is hidden where the impostor is shown.
	const LocalVector<OctahedralImpostorBaker::Geometry> geometry = OctahedralImpostorBaker::collect_geometry(p_node);
	for (const OctahedralImpostorBaker::Geometry &item : geometry) {
		GeometryInstance3D *geometry_instance = ObjectDB::get_instance<GeometryInstance3D>(item.node);
		if (!geometry_instance || geometry_instance == impostor) {
			continue;
		}
		if (!_is_node_editable(geometry_instance, edited_scene)) {
			r_skipped_nodes++;
			continue;
		}
		undo_redo->add_do_property(geometry_instance, "visibility_range_end", lod_distance);
		undo_redo->add_undo_property(geometry_instance, "visibility_range_end", geometry_instance->get_visibility_range_end());
		undo_redo->add_do_property(geometry_instance, "visibility_range_end_margin", lod_margin);
		undo_redo->add_undo_property(geometry_instance, "visibility_range_end_margin", geometry_instance->get_visibility_range_end_margin());
		undo_redo->add_do_property(geometry_instance, "visibility_range_fade_mode", fade_mode);
		undo_redo->add_undo_property(geometry_instance, "visibility_range_fade_mode", geometry_instance->get_visibility_range_fade_mode());
	}
}

void OctahedralImpostorDialog::_bake() {
	_save_settings();
	const bool batch = _is_batch();
	const String path = path_edit->get_text().strip_edges();
	if (batch) {
		if (!DirAccess::dir_exists_absolute(path)) {
			EditorNode::get_singleton()->show_warning(vformat(TTR("The folder \"%s\" doesn't exist."), path));
			return;
		}
	} else {
		const String extension = path.get_extension().to_lower();
		if (extension != "tres" && extension != "res") {
			EditorNode::get_singleton()->show_warning(TTR("The impostor mesh must be saved as a resource (\".tres\" or \".res\")."));
			return;
		}
		if (!DirAccess::dir_exists_absolute(path.get_base_dir())) {
			EditorNode::get_singleton()->show_warning(vformat(TTR("The folder \"%s\" doesn't exist."), path.get_base_dir()));
			return;
		}
	}
	EditorSettings::get_singleton()->set_project_metadata("octahedral_impostor", "last_directory", batch ? path : path.get_base_dir());

	Node *edited_scene = EditorNode::get_singleton()->get_edited_scene();
	const bool add_to_scene = add_to_scene_check->is_visible() && add_to_scene_check->is_pressed() && edited_scene;
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	bool action_created = false;
	int skipped_nodes = 0;
	Vector<String> failed;
	String last_mesh_path;

	for (int i = 0; i < sources.size(); i++) {
		const Source &source = sources[i];
		Node3D *node = nullptr;
		bool temporary = false;
		if (source.scene_path.is_empty()) {
			node = ObjectDB::get_instance<Node3D>(source.node);
		} else if (i == 0) {
			node = _get_source_node(0);
		} else {
			node = _instantiate_scene(source.scene_path);
			temporary = true;
		}
		if (!node) {
			failed.push_back(source.name);
			continue;
		}

		const String mesh_path = batch ? path.path_join(source.name.to_snake_case() + "_impostor.tres") : path;
		Ref<OctahedralImpostorBaker> baker = _create_baker();
		const Error err = baker->bake(node, true);
		Ref<Mesh> mesh;
		if (err == OK) {
			mesh = baker->save(mesh_path);
		}
		if (mesh.is_valid() && add_to_scene && !temporary && source.scene_path.is_empty()) {
			if (!action_created) {
				undo_redo->create_action(TTR("Create Octahedral Impostor"));
				action_created = true;
			}
			_add_impostor_to_scene(node, mesh, baker, skipped_nodes);
		}
		if (temporary) {
			memdelete(node);
		}
		if (err == ERR_SKIP) {
			break; // Canceled.
		}
		if (mesh.is_null()) {
			failed.push_back(source.name);
		} else {
			last_mesh_path = mesh_path;
		}
	}
	if (action_created) {
		undo_redo->commit_action();
	}
	_free_scene_instance();

	if (!failed.is_empty()) {
		EditorNode::get_singleton()->show_warning(vformat(TTR("The impostors of these sources couldn't be baked (see the Output panel):\n%s"), String("\n").join(failed)));
	} else if (!last_mesh_path.is_empty()) {
		EditorToaster::get_singleton()->popup_str(vformat(TTR("Octahedral impostor saved to \"%s\"."), batch ? path : last_mesh_path));
	}
	if (skipped_nodes > 0) {
		EditorToaster::get_singleton()->popup_str(vformat(TTR("The visibility range of %d meshes of instantiated scenes wasn't changed: set it up in their scenes, or make their children editable."), skipped_nodes), EditorToaster::SEVERITY_WARNING);
	}
}

void OctahedralImpostorDialog::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_THEME_CHANGED: {
			path_button->set_button_icon(get_editor_theme_icon(SNAME("Folder")));
			warning_label->add_theme_color_override(SceneStringName(font_color), get_theme_color(SNAME("warning_color"), EditorStringName(Editor)));
		} break;

		case NOTIFICATION_VISIBILITY_CHANGED: {
			if (!is_visible()) {
				// The bake happens after the dialog is hidden, it instantiates the scene again if needed.
				_free_scene_instance();
				preview_timer->stop();
				preview_instance->set_mesh(Ref<Mesh>());
				preview_texture->set_texture(Ref<Texture2D>());
				preview_baker.unref();
			}
		} break;
	}
}

OctahedralImpostorDialog::OctahedralImpostorDialog() {
	set_title(TTR("Create Octahedral Impostor"));
	set_ok_button_text(TTR("Bake"));
	connect(SceneStringName(confirmed), callable_mp(this, &OctahedralImpostorDialog::_bake));

	HBoxContainer *main_hbox = memnew(HBoxContainer);
	main_hbox->add_theme_constant_override("separation", 12 * EDSCALE);
	add_child(main_hbox);

	// Settings.
	VBoxContainer *settings_vbox = memnew(VBoxContainer);
	settings_vbox->set_custom_minimum_size(Size2(320, 0) * EDSCALE);
	main_hbox->add_child(settings_vbox);

	source_label = memnew(Label);
	source_label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	source_label->set_mouse_filter(Control::MOUSE_FILTER_PASS);
	settings_vbox->add_child(source_label);

	GridContainer *grid = memnew(GridContainer);
	grid->set_columns(2);
	settings_vbox->add_child(grid);

	EditorSettings *settings = EditorSettings::get_singleton();

	Label *label = memnew(Label(TTR("Layout:")));
	grid->add_child(label);
	layout_option = memnew(OptionButton);
	layout_option->set_accessibility_name(TTRC("Layout:"));
	layout_option->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	layout_option->add_item(TTR("Hemisphere"), OctahedralImpostorMaterial3D::LAYOUT_HEMISPHERE);
	layout_option->set_item_tooltip(-1, TTR("Views from above only (e.g. trees, rocks): more resolution for the views from the sides."));
	layout_option->add_item(TTR("Full Sphere"), OctahedralImpostorMaterial3D::LAYOUT_FULL_SPHERE);
	layout_option->set_item_tooltip(-1, TTR("Views from all directions, for objects also seen from below."));
	layout_option->select(layout_option->get_item_index(settings->get_project_metadata("octahedral_impostor", "layout", OctahedralImpostorMaterial3D::LAYOUT_HEMISPHERE)));
	layout_option->connect(SceneStringName(item_selected), callable_mp(this, &OctahedralImpostorDialog::_settings_changed));
	grid->add_child(layout_option);

	label = memnew(Label(TTR("Frames:")));
	grid->add_child(label);
	frames_spin = memnew(SpinBox);
	frames_spin->set_accessibility_name(TTRC("Frames:"));
	frames_spin->set_tooltip_text(TTR("Number of views per side of the atlas. More views give smoother transitions between them, at the cost of the resolution of each view."));
	frames_spin->set_min(4);
	frames_spin->set_max(32);
	frames_spin->set_value(settings->get_project_metadata("octahedral_impostor", "frames", 12));
	frames_spin->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	frames_spin->connect(SceneStringName(value_changed), callable_mp(this, &OctahedralImpostorDialog::_settings_changed).unbind(1).bind(0));
	grid->add_child(frames_spin);

	label = memnew(Label(TTR("Atlas Size:")));
	grid->add_child(label);
	atlas_size_option = memnew(OptionButton);
	atlas_size_option->set_accessibility_name(TTRC("Atlas Size:"));
	atlas_size_option->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	for (int atlas_size : atlas_sizes) {
		atlas_size_option->add_item(vformat("%d x %d", atlas_size, atlas_size), atlas_size);
	}
	const int atlas_index = atlas_size_option->get_item_index(settings->get_project_metadata("octahedral_impostor", "atlas_size", 2048));
	atlas_size_option->select(atlas_index >= 0 ? atlas_index : 2);
	atlas_size_option->connect(SceneStringName(item_selected), callable_mp(this, &OctahedralImpostorDialog::_settings_changed));
	grid->add_child(atlas_size_option);

	label = memnew(Label(TTR("Supersampling:")));
	grid->add_child(label);
	supersampling_option = memnew(OptionButton);
	supersampling_option->set_accessibility_name(TTRC("Supersampling:"));
	supersampling_option->set_tooltip_text(TTR("Renders the views at a higher resolution, for smooth edges."));
	supersampling_option->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	for (int factor : supersampling_factors) {
		supersampling_option->add_item(factor == 1 ? TTR("Disabled") : vformat("%dx", factor * factor), factor);
	}
	const int supersampling_index = supersampling_option->get_item_index(settings->get_project_metadata("octahedral_impostor", "supersampling", 2));
	supersampling_option->select(supersampling_index >= 0 ? supersampling_index : 1);
	supersampling_option->connect(SceneStringName(item_selected), callable_mp(this, &OctahedralImpostorDialog::_settings_changed));
	grid->add_child(supersampling_option);

	grid->add_child(memnew(Control));
	orm_check = memnew(CheckBox);
	orm_check->set_text(TTR("ORM Atlas"));
	orm_check->set_tooltip_text(TTR("Also bakes the ambient occlusion, roughness and metallic of the materials. Otherwise, the impostor uses their mean roughness and metallic."));
	orm_check->set_pressed(settings->get_project_metadata("octahedral_impostor", "orm", false));
	orm_check->connect(SceneStringName(toggled), callable_mp(this, &OctahedralImpostorDialog::_settings_changed).unbind(1).bind(0));
	grid->add_child(orm_check);

	info_label = memnew(Label);
	info_label->set_theme_type_variation("HeaderSmall");
	settings_vbox->add_child(info_label);

	settings_vbox->add_child(memnew(HSeparator));

	add_to_scene_check = memnew(CheckBox);
	add_to_scene_check->set_text(TTR("Add to Scene as Level of Detail"));
	add_to_scene_check->set_tooltip_text(TTR("Adds the impostor to the node, shown from the distance, and hides the meshes of the node from there (with the visibility ranges)."));
	add_to_scene_check->set_pressed(settings->get_project_metadata("octahedral_impostor", "add_to_scene", true));
	add_to_scene_check->connect(SceneStringName(toggled), callable_mp(this, &OctahedralImpostorDialog::_add_to_scene_toggled));
	settings_vbox->add_child(add_to_scene_check);

	GridContainer *lod_grid = memnew(GridContainer);
	lod_grid->set_columns(2);
	settings_vbox->add_child(lod_grid);

	label = memnew(Label(TTR("Distance:")));
	lod_grid->add_child(label);
	lod_distance_spin = memnew(SpinBox);
	lod_distance_spin->set_accessibility_name(TTRC("Distance:"));
	lod_distance_spin->set_tooltip_text(TTR("Distance from which the impostor replaces the meshes. By default, where the views are shown at about their resolution on a 1080p screen."));
	lod_distance_spin->set_min(0.1);
	lod_distance_spin->set_max(100000);
	lod_distance_spin->set_step(0.1);
	lod_distance_spin->set_allow_greater(true);
	lod_distance_spin->set_suffix("m");
	lod_distance_spin->set_value(50.0);
	lod_distance_spin->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	lod_distance_spin->connect(SceneStringName(value_changed), callable_mp(this, &OctahedralImpostorDialog::_lod_distance_changed));
	lod_grid->add_child(lod_distance_spin);

	lod_grid->add_child(memnew(Control));
	fade_check = memnew(CheckBox);
	fade_check->set_text(TTR("Fade"));
	fade_check->set_tooltip_text(TTR("Cross-fades the impostor and the meshes over 10% of the distance."));
	fade_check->set_pressed(settings->get_project_metadata("octahedral_impostor", "fade", true));
	lod_grid->add_child(fade_check);

	settings_vbox->add_child(memnew(HSeparator));

	path_label = memnew(Label);
	settings_vbox->add_child(path_label);
	HBoxContainer *path_hbox = memnew(HBoxContainer);
	settings_vbox->add_child(path_hbox);
	path_edit = memnew(LineEdit);
	path_edit->set_accessibility_name(TTRC("Path"));
	path_edit->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	path_hbox->add_child(path_edit);
	path_button = memnew(Button);
	path_button->set_accessibility_name(TTRC("Browse"));
	path_button->connect(SceneStringName(pressed), callable_mp(this, &OctahedralImpostorDialog::_browse_path));
	path_hbox->add_child(path_button);

	warning_label = memnew(Label);
	warning_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	warning_label->set_custom_minimum_size(Size2(320, 0) * EDSCALE);
	warning_label->hide();
	settings_vbox->add_child(warning_label);

	file_dialog = memnew(EditorFileDialog);
	file_dialog->set_access(EditorFileDialog::ACCESS_RESOURCES);
	file_dialog->connect("file_selected", callable_mp(this, &OctahedralImpostorDialog::_path_selected));
	file_dialog->connect("dir_selected", callable_mp(this, &OctahedralImpostorDialog::_path_selected));
	add_child(file_dialog);

	// Preview.
	VBoxContainer *preview_vbox = memnew(VBoxContainer);
	preview_vbox->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	preview_vbox->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	main_hbox->add_child(preview_vbox);

	HBoxContainer *preview_hbox = memnew(HBoxContainer);
	preview_vbox->add_child(preview_hbox);
	label = memnew(Label(TTR("Preview:")));
	preview_hbox->add_child(label);
	preview_mode_option = memnew(OptionButton);
	preview_mode_option->set_accessibility_name(TTRC("Preview:"));
	preview_mode_option->add_item(TTR("Impostor"), PREVIEW_IMPOSTOR);
	preview_mode_option->add_item(TTR("Albedo Atlas"), PREVIEW_ALBEDO);
	preview_mode_option->add_item(TTR("Normal Atlas"), PREVIEW_NORMAL);
	preview_mode_option->add_item(TTR("Depth Atlas"), PREVIEW_DEPTH);
	preview_mode_option->connect(SceneStringName(item_selected), callable_mp(this, &OctahedralImpostorDialog::_update_preview_mode));
	preview_hbox->add_child(preview_mode_option);

	Control *preview_panel = memnew(Control);
	preview_panel->set_custom_minimum_size(Size2(400, 400) * EDSCALE);
	preview_panel->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	preview_panel->set_clip_contents(true);
	preview_vbox->add_child(preview_panel);

	preview_container = memnew(SubViewportContainer);
	preview_container->set_stretch(true);
	preview_container->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	preview_container->set_tooltip_text(TTR("Drag to rotate, scroll to zoom. The preview is baked at a lower resolution."));
	preview_container->connect(SceneStringName(gui_input), callable_mp(this, &OctahedralImpostorDialog::_preview_input));
	preview_panel->add_child(preview_container);

	preview_viewport = memnew(SubViewport);
	Ref<World3D> world;
	world.instantiate();
	Ref<Environment> environment;
	environment.instantiate();
	environment->set_background(Environment::BG_COLOR);
	environment->set_bg_color(Color(0.18, 0.2, 0.23));
	environment->set_ambient_source(Environment::AMBIENT_SOURCE_COLOR);
	environment->set_ambient_light_color(Color(0.45, 0.5, 0.6));
	world->set_environment(environment);
	preview_viewport->set_world_3d(world);
	preview_viewport->set_msaa_3d(Viewport::MSAA_4X);
	preview_container->add_child(preview_viewport);

	preview_camera = memnew(Camera3D);
	preview_viewport->add_child(preview_camera);
	preview_camera->make_current();

	DirectionalLight3D *light = memnew(DirectionalLight3D);
	light->set_transform(Transform3D().looking_at(Vector3(-1, -1.5, -0.8), Vector3(0, 1, 0)));
	preview_viewport->add_child(light);

	preview_instance = memnew(MeshInstance3D);
	preview_viewport->add_child(preview_instance);

	preview_texture = memnew(TextureRect);
	preview_texture->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	preview_texture->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
	preview_texture->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
	preview_texture->hide();
	preview_panel->add_child(preview_texture);

	preview_status = memnew(Label);
	preview_status->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	preview_status->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	preview_status->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
	preview_status->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	preview_panel->add_child(preview_status);

	preview_timer = memnew(Timer);
	preview_timer->set_one_shot(true);
	preview_timer->set_wait_time(0.3);
	preview_timer->connect("timeout", callable_mp(this, &OctahedralImpostorDialog::_update_preview));
	add_child(preview_timer);

	_update_preview_camera();
}

OctahedralImpostorDialog::~OctahedralImpostorDialog() {
	_free_scene_instance();
}

/* OctahedralImpostorContextMenuPlugin */

void OctahedralImpostorContextMenuPlugin::setup(OctahedralImpostorDialog *p_dialog, bool p_filesystem) {
	dialog = p_dialog;
	filesystem = p_filesystem;
}

void OctahedralImpostorContextMenuPlugin::get_options(const OptionsData &p_data) {
	EditorContextMenuPlugin::get_options(p_data);
	bool show = false;
	if (filesystem) {
		const PackedStringArray files = p_data.get("selected_files", PackedStringArray());
		EditorFileSystem *file_system = EditorFileSystem::get_singleton();
		for (const String &file : files) {
			if (file_system->get_file_type(file) == "PackedScene") {
				show = true;
				break;
			}
		}
	} else {
		const TypedArray<Node> nodes = p_data.get("selected_nodes", TypedArray<Node>());
		for (int i = 0; i < nodes.size() && !show; i++) {
			Node3D *node = Object::cast_to<Node3D>(nodes[i]);
			show = node && OctahedralImpostorBaker::has_geometry(node);
		}
	}
	if (show) {
		const Ref<Texture2D> icon = EditorNode::get_singleton()->get_class_icon("OctahedralImpostorMaterial3D");
		if (filesystem) {
			add_context_menu_item(TTR("Create Octahedral Impostor..."), callable_mp(this, &OctahedralImpostorContextMenuPlugin::_filesystem_option), icon);
		} else {
			add_context_menu_item(TTR("Create Octahedral Impostor..."), callable_mp(this, &OctahedralImpostorContextMenuPlugin::_scene_tree_option), icon);
		}
	}
}

void OctahedralImpostorContextMenuPlugin::_scene_tree_option(const Dictionary &p_data) {
	const TypedArray<Node> nodes = p_data.get("selected_nodes", TypedArray<Node>());
	Vector<Node3D *> sources;
	for (int i = 0; i < nodes.size(); i++) {
		Node3D *node = Object::cast_to<Node3D>(nodes[i]);
		if (node && OctahedralImpostorBaker::has_geometry(node)) {
			sources.push_back(node);
		}
	}
	if (!sources.is_empty()) {
		dialog->popup_for_nodes(sources);
	}
}

void OctahedralImpostorContextMenuPlugin::_filesystem_option(const Dictionary &p_data) {
	const PackedStringArray files = p_data.get("selected_files", PackedStringArray());
	Vector<String> scenes;
	EditorFileSystem *file_system = EditorFileSystem::get_singleton();
	for (const String &file : files) {
		if (file_system->get_file_type(file) == "PackedScene") {
			scenes.push_back(file);
		}
	}
	if (!scenes.is_empty()) {
		dialog->popup_for_scenes(scenes);
	}
}

/* OctahedralImpostorMaterialConversionPlugin */

String OctahedralImpostorMaterialConversionPlugin::converts_to() const {
	return "ShaderMaterial";
}

bool OctahedralImpostorMaterialConversionPlugin::handles(const Ref<Resource> &p_resource) const {
	const Ref<OctahedralImpostorMaterial3D> material = p_resource;
	return material.is_valid();
}

Ref<Resource> OctahedralImpostorMaterialConversionPlugin::convert(const Ref<Resource> &p_resource) const {
	const Ref<OctahedralImpostorMaterial3D> material = p_resource;
	ERR_FAIL_COND_V(material.is_null(), Ref<Resource>());
	Ref<ShaderMaterial> shader_material = MaterialEditor::make_shader_material(material, false);
	ERR_FAIL_COND_V(shader_material.is_null(), Ref<Resource>());

	// The textures are stored as RIDs in the material, the shader material needs the resources.
	static const char *texture_names[OctahedralImpostorMaterial3D::TEXTURE_MAX] = { "texture_albedo", "texture_normal_depth", "texture_orm" };
	List<PropertyInfo> parameters;
	RS::get_singleton()->get_shader_parameter_list(material->get_shader_rid(), &parameters);
	for (const PropertyInfo &parameter : parameters) {
		bool is_texture = false;
		for (int i = 0; i < OctahedralImpostorMaterial3D::TEXTURE_MAX; i++) {
			if (parameter.name == texture_names[i]) {
				shader_material->set_shader_parameter(parameter.name, material->get_texture(OctahedralImpostorMaterial3D::TextureParam(i)));
				is_texture = true;
			}
		}
		if (!is_texture) {
			shader_material->set_shader_parameter(parameter.name, RS::get_singleton()->material_get_param(material->get_rid(), parameter.name));
		}
	}
	return shader_material;
}

/* OctahedralImpostorEditorPlugin */

void OctahedralImpostorEditorPlugin::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			dialog = memnew(OctahedralImpostorDialog);
			EditorNode::get_singleton()->get_gui_base()->add_child(dialog);

			scene_tree_menu.instantiate();
			scene_tree_menu->setup(dialog, false);
			add_context_menu_plugin(EditorContextMenuPlugin::CONTEXT_SLOT_SCENE_TREE, scene_tree_menu);
			filesystem_menu.instantiate();
			filesystem_menu->setup(dialog, true);
			add_context_menu_plugin(EditorContextMenuPlugin::CONTEXT_SLOT_FILESYSTEM, filesystem_menu);

			material_conversion.instantiate();
			add_resource_conversion_plugin(material_conversion);
		} break;

		case NOTIFICATION_EXIT_TREE: {
			remove_resource_conversion_plugin(material_conversion);
			material_conversion.unref();
			remove_context_menu_plugin(filesystem_menu);
			filesystem_menu.unref();
			remove_context_menu_plugin(scene_tree_menu);
			scene_tree_menu.unref();
			memdelete(dialog);
			dialog = nullptr;
		} break;
	}
}

OctahedralImpostorEditorPlugin::OctahedralImpostorEditorPlugin() {
}
