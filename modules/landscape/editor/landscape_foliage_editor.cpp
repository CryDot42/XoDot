/**************************************************************************/
/*  landscape_foliage_editor.cpp                                          */
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

#include "landscape_foliage_editor.h"

#include "core/input/input.h"
#include "core/input/input_event.h"
#include "core/io/resource_loader.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/gui/editor_file_dialog.h"
#include "editor/inspector/editor_resource_picker.h"
#include "editor/inspector/editor_resource_preview.h"
#include "editor/themes/editor_scale.h"
#include "scene/3d/camera_3d.h"
#include "scene/gui/button.h"
#include "scene/gui/check_box.h"
#include "scene/gui/grid_container.h"
#include "scene/gui/label.h"
#include "scene/gui/menu_button.h"
#include "scene/gui/option_button.h"
#include "scene/gui/popup_menu.h"
#include "scene/gui/separator.h"
#include "scene/gui/slider.h"
#include "scene/gui/spin_box.h"
#include "scene/gui/tree.h"
#include "scene/resources/font.h"

namespace {

String _foliage_type_name(const Ref<LandscapeFoliageType> &p_type) {
	if (p_type.is_null()) {
		return TTR("(Empty)");
	}
	if (!p_type->get_name().is_empty()) {
		return p_type->get_name();
	}
	if (p_type->get_path().is_resource_file()) {
		return p_type->get_path().get_file().get_basename();
	}
	const Ref<Mesh> mesh = p_type->get_mesh();
	if (mesh.is_valid()) {
		if (!mesh->get_name().is_empty()) {
			return mesh->get_name();
		}
		if (mesh->get_path().is_resource_file()) {
			return mesh->get_path().get_file().get_basename();
		}
	}
	return TTR("Foliage Type");
}

String _format_count(int64_t p_count) {
	if (p_count >= 1000000) {
		return String::num(p_count / 1000000.0, 1) + "M";
	}
	if (p_count >= 1000) {
		return String::num(p_count / 1000.0, 1) + "K";
	}
	return itos(p_count);
}

String _format_distance(float p_distance) {
	return vformat("%s m", String::num(p_distance, p_distance < 10.0 ? 1 : 0));
}

} // namespace

/* LOD bar */

Color LandscapeFoliageLodBar::get_lod_color(int p_lod) {
	static const Color colors[LandscapeFoliageType::MAX_LODS] = {
		Color(0.36, 0.72, 0.36),
		Color(0.35, 0.6, 0.85),
		Color(0.9, 0.72, 0.3),
		Color(0.85, 0.45, 0.35),
		Color(0.65, 0.45, 0.85),
		Color(0.35, 0.8, 0.8),
		Color(0.85, 0.5, 0.7),
		Color(0.6, 0.6, 0.6),
	};
	return colors[CLAMP(p_lod, 0, LandscapeFoliageType::MAX_LODS - 1)];
}

void LandscapeFoliageLodBar::set_type(const Ref<LandscapeFoliageType> &p_type) {
	type = p_type;
	hovered = -1;
	dragging = -1;
	queue_redraw();
}

Size2 LandscapeFoliageLodBar::get_minimum_size() const {
	return Size2(200, 58) * EDSCALE;
}

float LandscapeFoliageLodBar::_get_display_range() const {
	if (type.is_null()) {
		return 100.0;
	}
	float range = 50.0;
	for (int i = 1; i < type->get_lod_count(); i++) {
		range = MAX(range, type->get_lod_start_distance(i) * 1.3f);
	}
	if (type->get_cull_distance() > 0.0) {
		range = MAX(range, type->get_cull_distance() * 1.15f);
	}
	return range;
}

float LandscapeFoliageLodBar::_distance_to_x(float p_distance) const {
	const float margin = 8.0 * EDSCALE;
	return margin + (get_size().x - margin * 2.0) * CLAMP(p_distance / display_range, 0.0f, 1.0f);
}

float LandscapeFoliageLodBar::_x_to_distance(float p_x) const {
	const float margin = 8.0 * EDSCALE;
	return MAX((p_x - margin) / MAX(get_size().x - margin * 2.0f, 1.0f) * display_range, 0.0f);
}

int LandscapeFoliageLodBar::_get_handle_count() const {
	// LOD start distances (from level 1), then the cull distance.
	return type.is_null() ? 0 : type->get_lod_count() + (type->get_cull_distance() > 0.0 ? 1 : 0);
}

float LandscapeFoliageLodBar::_get_handle_distance(int p_handle) const {
	return p_handle < type->get_lod_count() ? type->get_lod_start_distance(p_handle) : type->get_cull_distance();
}

int LandscapeFoliageLodBar::_get_handle_at(float p_x) const {
	int best = -1;
	float best_distance = 6.0 * EDSCALE;
	for (int handle = 1; handle < _get_handle_count(); handle++) {
		const float distance = Math::abs(_distance_to_x(_get_handle_distance(handle)) - p_x);
		if (distance <= best_distance) {
			best = handle;
			best_distance = distance;
		}
	}
	return best;
}

void LandscapeFoliageLodBar::gui_input(const Ref<InputEvent> &p_event) {
	if (type.is_null()) {
		return;
	}
	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid() && mb->get_button_index() == MouseButton::LEFT) {
		if (mb->is_pressed()) {
			dragging = _get_handle_at(mb->get_position().x);
			if (dragging >= 0) {
				drag_start_value = _get_handle_distance(dragging);
				accept_event();
			}
		} else if (dragging >= 0) {
			const int handle = dragging;
			dragging = -1;
			if (LandscapeFoliageLodDialog::get_singleton()) {
				LandscapeFoliageLodDialog::get_singleton()->set_boundary(handle, _get_handle_distance(handle), true, drag_start_value);
			}
			display_range = _get_display_range();
			queue_redraw();
			accept_event();
		}
		return;
	}
	Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_valid()) {
		if (dragging >= 0) {
			// Between the neighbor boundaries, in steps of 0.5 m (1 m when far).
			float distance = _x_to_distance(mm->get_position().x);
			const float step = distance < 100.0 ? 0.5 : 1.0;
			distance = Math::snapped(distance, step);
			const float low = dragging < type->get_lod_count() ? type->get_lod_start_distance(dragging - 1) : type->get_lod_start_distance(type->get_lod_count() - 1);
			float high = display_range;
			if (dragging + 1 < type->get_lod_count()) {
				high = type->get_lod_start_distance(dragging + 1);
			} else if (dragging < type->get_lod_count() && type->get_cull_distance() > 0.0) {
				high = type->get_cull_distance();
			}
			distance = CLAMP(distance, low, MAX(low, high));
			if (LandscapeFoliageLodDialog::get_singleton()) {
				LandscapeFoliageLodDialog::get_singleton()->set_boundary(dragging, distance, false, drag_start_value);
			}
			queue_redraw();
			accept_event();
			return;
		}
		const int handle = _get_handle_at(mm->get_position().x);
		if (handle != hovered) {
			hovered = handle;
			queue_redraw();
		}
	}
}

void LandscapeFoliageLodBar::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_MOUSE_EXIT: {
			hovered = -1;
			queue_redraw();
		} break;

		case NOTIFICATION_DRAW: {
			const Ref<Font> font = get_theme_font(SceneStringName(font), SNAME("Label"));
			const int font_size = get_theme_font_size(SceneStringName(font_size), SNAME("Label"));
			const Color font_color = get_theme_color(SceneStringName(font_color), SNAME("Label"));
			const float bar_top = 4.0 * EDSCALE;
			const float bar_height = 24.0 * EDSCALE;
			const float margin = 8.0 * EDSCALE;
			draw_rect(Rect2(margin, bar_top, get_size().x - margin * 2.0, bar_height), Color(0, 0, 0, 0.3));
			if (type.is_null()) {
				break;
			}
			if (dragging < 0) {
				// The scale doesn't change while a boundary is dragged.
				display_range = _get_display_range();
			}
			const int lods = type->get_lod_count();
			const float cull = type->get_cull_distance();
			for (int i = 0; i < lods; i++) {
				const float start = type->get_lod_start_distance(i);
				float end = type->get_lod_end_distance(i);
				if (end <= 0.0) {
					end = display_range;
				}
				const float x0 = _distance_to_x(start);
				const float x1 = _distance_to_x(end);
				if (x1 <= x0) {
					continue;
				}
				Color color = get_lod_color(i);
				if (!type->is_lod_casting_shadows(i)) {
					color = color.darkened(0.25);
				}
				draw_rect(Rect2(x0, bar_top, x1 - x0, bar_height), color);
				const String text = vformat("LOD %d", i);
				const Size2 text_size = font->get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size);
				if (text_size.x + 4.0 * EDSCALE < x1 - x0) {
					draw_string(font, Point2((x0 + x1 - text_size.x) * 0.5, bar_top + (bar_height + text_size.y) * 0.5 - font->get_descent(font_size)), text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, Color(0, 0, 0, 0.85));
				}
			}
			if (cull > 0.0) {
				// Instances disappear randomly between the closest cull distance and the cull distance.
				const float fade_begin = cull * (1.0 - type->get_cull_random());
				const float xf = _distance_to_x(fade_begin);
				const float xc = _distance_to_x(cull);
				if (xc > xf) {
					const int steps = 8;
					for (int s = 0; s < steps; s++) {
						const float a = float(s + 1) / steps;
						draw_rect(Rect2(xf + (xc - xf) * s / steps, bar_top, (xc - xf) / steps + 1.0, bar_height), Color(0.15, 0.15, 0.15, a * 0.8));
					}
				}
				const float x_end = _distance_to_x(display_range);
				draw_rect(Rect2(xc, bar_top, x_end - xc, bar_height), Color(0.15, 0.15, 0.15, 0.9));
				const String text = TTR("Culled");
				const Size2 text_size = font->get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size);
				if (text_size.x + 4.0 * EDSCALE < x_end - xc) {
					draw_string(font, Point2((xc + x_end - text_size.x) * 0.5, bar_top + (bar_height + text_size.y) * 0.5 - font->get_descent(font_size)), text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, font_color * Color(1, 1, 1, 0.6));
				}
			}
			// Boundaries with their distances below the bar.
			const float label_y = bar_top + bar_height + font->get_ascent(font_size) + 2.0 * EDSCALE;
			draw_string(font, Point2(margin, label_y), "0", HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, font_color * Color(1, 1, 1, 0.6));
			float last_label_end = margin + font->get_string_size("0", HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x;
			for (int handle = 1; handle < _get_handle_count(); handle++) {
				const float x = _distance_to_x(_get_handle_distance(handle));
				const bool active = handle == hovered || handle == dragging;
				draw_line(Point2(x, bar_top - 2.0 * EDSCALE), Point2(x, bar_top + bar_height + 2.0 * EDSCALE), active ? Color(1, 1, 1) : Color(1, 1, 1, 0.7), (active ? 3.0 : 2.0) * EDSCALE);
				const String text = _format_distance(_get_handle_distance(handle));
				const float width = font->get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x;
				const float text_x = CLAMP(x - width * 0.5f, 0.0f, get_size().x - width);
				if (text_x > last_label_end + 4.0 * EDSCALE || active) {
					draw_string(font, Point2(text_x, label_y), text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, active ? font_color : font_color * Color(1, 1, 1, 0.75));
					last_label_end = text_x + width;
				}
			}
		} break;
	}
}

void LandscapeFoliageLodBar::_bind_methods() {
}

/* LOD window */

LandscapeFoliageLodDialog *LandscapeFoliageLodDialog::singleton = nullptr;

int64_t LandscapeFoliageLodDialog::_count_triangles(const Ref<Mesh> &p_mesh) {
	if (p_mesh.is_null()) {
		return 0;
	}
	int64_t triangles = 0;
	for (int i = 0; i < p_mesh->get_surface_count(); i++) {
		if (p_mesh->surface_get_primitive_type(i) != Mesh::PRIMITIVE_TRIANGLES) {
			continue;
		}
		const int indices = p_mesh->surface_get_array_index_len(i);
		triangles += (indices > 0 ? indices : p_mesh->surface_get_array_len(i)) / 3;
	}
	return triangles;
}

Dictionary LandscapeFoliageLodDialog::_snapshot() const {
	Dictionary state;
	if (type.is_null()) {
		return state;
	}
	state["lod_count"] = type->get_lod_count();
	state["mesh"] = type->get_mesh();
	for (int i = 0; i < type->get_lod_count(); i++) {
		if (i > 0) {
			state[vformat("lod_%d/mesh", i)] = type->get_lod_mesh(i);
			state[vformat("lod_%d/start_distance", i)] = type->get_lod_start_distance(i);
		}
		state[vformat("lod_%d/cast_shadows", i)] = type->is_lod_casting_shadows(i);
	}
	state["cull_distance"] = type->get_cull_distance();
	state["cull_random"] = type->get_cull_random();
	state["lod_transition"] = type->get_lod_transition();
	return state;
}

void LandscapeFoliageLodDialog::_apply_snapshot(const Ref<LandscapeFoliageType> &p_type, const Dictionary &p_state) {
	ERR_FAIL_COND(p_type.is_null());
	// The level count first, so that the levels exist.
	p_type->set_lod_count(p_state.get("lod_count", 1));
	for (const KeyValue<Variant, Variant> &kv : p_state) {
		if (String(kv.key) != "lod_count") {
			p_type->set(kv.key, kv.value);
		}
	}
}

void LandscapeFoliageLodDialog::_commit(const String &p_action, const Dictionary &p_before, UndoRedo::MergeMode p_merge) {
	// The change is already applied: record the state before and after it.
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(p_action, p_merge, type.ptr());
	undo_redo->add_do_method(this, "_apply_snapshot", type, _snapshot());
	undo_redo->add_undo_method(this, "_apply_snapshot", type, p_before);
	undo_redo->commit_action(false);
}

void LandscapeFoliageLodDialog::_type_changed() {
	if (type.is_null()) {
		return;
	}
	if (int(rows.size()) != type->get_lod_count()) {
		_rebuild_rows();
	}
	_update_values();
}

void LandscapeFoliageLodDialog::_rebuild_rows() {
	for (Row &row : rows) {
		for (Control *control : { (Control *)row.name, (Control *)row.mesh, (Control *)row.start, (Control *)row.end, (Control *)row.shadows, (Control *)row.triangles, (Control *)row.remove }) {
			table->remove_child(control);
			control->queue_free();
		}
	}
	rows.clear();
	if (type.is_null()) {
		return;
	}
	for (int i = 0; i < type->get_lod_count(); i++) {
		Row row;
		row.name = memnew(Label(vformat("LOD %d", i)));
		row.name->set_modulate(LandscapeFoliageLodBar::get_lod_color(i).lightened(0.3));
		table->add_child(row.name);

		row.mesh = memnew(EditorResourcePicker);
		row.mesh->set_base_type(Mesh::get_class_static());
		row.mesh->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		row.mesh->set_custom_minimum_size(Size2(180, 0) * EDSCALE);
		row.mesh->set_tooltip_text(i == 0 ? TTR("Mesh of the first level (the Mesh of the foliage type).") : TTR("Mesh of this level. Empty: the mesh of the previous level (e.g. a level that only disables the shadows)."));
		row.mesh->connect("resource_changed", callable_mp(this, &LandscapeFoliageLodDialog::_mesh_changed).bind(i));
		table->add_child(row.mesh);

		row.start = memnew(SpinBox);
		row.start->set_min(0.0);
		row.start->set_max(16384.0);
		row.start->set_step(0.1);
		row.start->set_allow_greater(true);
		row.start->set_suffix("m");
		row.start->set_select_all_on_focus(true);
		row.start->set_editable(i > 0);
		row.start->set_tooltip_text(i == 0 ? TTR("The first level starts at the camera.") : TTR("Distance from the camera at which this level replaces the previous one."));
		row.start->connect(SceneStringName(value_changed), callable_mp(this, &LandscapeFoliageLodDialog::_start_changed).bind(i));
		table->add_child(row.start);

		row.end = memnew(Label);
		row.end->set_custom_minimum_size(Size2(70, 0) * EDSCALE);
		table->add_child(row.end);

		row.shadows = memnew(CheckBox);
		row.shadows->set_tooltip_text(TTR("Cast shadows at this level."));
		row.shadows->connect(SceneStringName(toggled), callable_mp(this, &LandscapeFoliageLodDialog::_shadows_toggled).bind(i));
		table->add_child(row.shadows);

		row.triangles = memnew(Label);
		row.triangles->set_custom_minimum_size(Size2(60, 0) * EDSCALE);
		row.triangles->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_RIGHT);
		table->add_child(row.triangles);

		row.remove = memnew(Button);
		row.remove->set_flat(true);
		row.remove->set_button_icon(get_editor_theme_icon(SNAME("Remove")));
		row.remove->set_tooltip_text(TTR("Remove this level."));
		row.remove->set_disabled(i == 0);
		row.remove->set_modulate(Color(1, 1, 1, i == 0 ? 0.0 : 1.0));
		row.remove->connect(SceneStringName(pressed), callable_mp(this, &LandscapeFoliageLodDialog::_remove_lod).bind(i));
		table->add_child(row.remove);

		rows.push_back(row);
	}
}

void LandscapeFoliageLodDialog::_update_values() {
	if (type.is_null()) {
		return;
	}
	updating = true;
	const int lods = type->get_lod_count();
	String problems;
	for (int i = 0; i < int(rows.size()) && i < lods; i++) {
		Row &row = rows[i];
		row.mesh->set_edited_resource(i == 0 ? Ref<Resource>(type->get_mesh()) : Ref<Resource>(type->get_lod_mesh(i)));
		row.start->set_value(type->get_lod_start_distance(i));
		const float end = type->get_lod_end_distance(i);
		row.end->set_text(end > 0.0 ? String::utf8("→ ") + _format_distance(end) : String::utf8("→ ∞"));
		row.shadows->set_pressed_no_signal(type->is_lod_casting_shadows(i));
		const Ref<Mesh> mesh = type->get_lod_effective_mesh(i);
		row.triangles->set_text(mesh.is_valid() ? _format_count(_count_triangles(mesh)) : String("-"));
		row.triangles->set_tooltip_text(mesh.is_valid() ? vformat(TTR("%d triangles"), _count_triangles(mesh)) : TTR("No mesh: nothing is drawn."));
		if (i > 0 && type->get_lod_start_distance(i) < type->get_lod_start_distance(i - 1)) {
			problems += vformat(TTR("LOD %d starts before LOD %d: it is used from %s."), i, i - 1, _format_distance(type->get_lod_start_distance(i - 1))) + "\n";
		}
		if (i > 0 && type->get_cull_distance() > 0.0 && type->get_lod_start_distance(i) >= type->get_cull_distance()) {
			problems += vformat(TTR("LOD %d starts beyond the cull distance: it is never drawn."), i) + "\n";
		}
	}
	if (type->get_mesh().is_null()) {
		problems += TTR("The first level has no mesh: nothing is drawn.") + "\n";
	}
	cull_distance->set_value(type->get_cull_distance());
	cull_random->set_value(type->get_cull_random() * 100.0);
	cull_random->set_editable(type->get_cull_distance() > 0.0);
	transition->set_value(type->get_lod_transition());
	add_lod->set_disabled(lods >= LandscapeFoliageType::MAX_LODS);
	warning->set_text(problems.strip_edges());
	warning->set_visible(!problems.is_empty());
	bar->queue_redraw();
	updating = false;
}

void LandscapeFoliageLodDialog::_mesh_changed(const Ref<Resource> &p_mesh, int p_lod) {
	if (updating || type.is_null()) {
		return;
	}
	const Dictionary before = _snapshot();
	if (p_lod == 0) {
		type->set_mesh(p_mesh);
	} else {
		type->set_lod_mesh(p_lod, p_mesh);
	}
	_commit(TTR("Set Foliage LOD Mesh"), before);
}

void LandscapeFoliageLodDialog::_start_changed(double p_value, int p_lod) {
	if (updating || type.is_null() || p_lod == 0) {
		return;
	}
	const Dictionary before = _snapshot();
	type->set_lod_start_distance(p_lod, p_value);
	_commit(TTR("Set Foliage LOD Distance"), before, UndoRedo::MERGE_ENDS);
}

void LandscapeFoliageLodDialog::_shadows_toggled(bool p_pressed, int p_lod) {
	if (updating || type.is_null()) {
		return;
	}
	const Dictionary before = _snapshot();
	type->set_lod_cast_shadows(p_lod, p_pressed);
	_commit(TTR("Set Foliage LOD Shadows"), before);
}

void LandscapeFoliageLodDialog::_remove_lod(int p_lod) {
	if (type.is_null() || p_lod <= 0 || p_lod >= type->get_lod_count()) {
		return;
	}
	const Dictionary before = _snapshot();
	type->remove_lod(p_lod);
	_commit(TTR("Remove Foliage LOD"), before);
}

void LandscapeFoliageLodDialog::_add_lod() {
	if (type.is_null() || type->get_lod_count() >= LandscapeFoliageType::MAX_LODS) {
		return;
	}
	const Dictionary before = _snapshot();
	type->set_lod_count(type->get_lod_count() + 1);
	_commit(TTR("Add Foliage LOD"), before);
}

void LandscapeFoliageLodDialog::_settings_changed(double p_value) {
	if (updating || type.is_null()) {
		return;
	}
	const Dictionary before = _snapshot();
	type->set_cull_distance(cull_distance->get_value());
	type->set_cull_random(cull_random->get_value() / 100.0);
	type->set_lod_transition(transition->get_value());
	_commit(TTR("Set Foliage Cull Distance"), before, UndoRedo::MERGE_ENDS);
}

void LandscapeFoliageLodDialog::set_boundary(int p_handle, float p_distance, bool p_finished, float p_before) {
	if (type.is_null() || p_handle <= 0) {
		return;
	}
	const bool lod = p_handle < type->get_lod_count();
	const String property = lod ? vformat("lod_%d/start_distance", p_handle) : String("cull_distance");
	if (!p_finished) {
		// Live preview while dragging, recorded once when released.
		type->set(property, p_distance);
		return;
	}
	type->set(property, p_before);
	const Dictionary before = _snapshot();
	type->set(property, p_distance);
	_commit(lod ? TTR("Set Foliage LOD Distance") : TTR("Set Foliage Cull Distance"), before);
}

void LandscapeFoliageLodDialog::edit(const Ref<LandscapeFoliageType> &p_type) {
	const Callable callback = callable_mp(this, &LandscapeFoliageLodDialog::_type_changed);
	if (type.is_valid() && type->is_connected(CoreStringName(changed), callback)) {
		type->disconnect(CoreStringName(changed), callback);
	}
	type = p_type;
	if (type.is_valid()) {
		type->connect(CoreStringName(changed), callback);
	}
	set_title(vformat(TTR("Foliage LODs: %s"), _foliage_type_name(type)));
	bar->set_type(type);
	_rebuild_rows();
	_update_values();
	if (type.is_valid() && !is_visible()) {
		popup_centered(Size2(760, 480) * EDSCALE);
	}
}

void LandscapeFoliageLodDialog::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_THEME_CHANGED: {
			add_lod->set_button_icon(get_editor_theme_icon(SNAME("Add")));
			for (Row &row : rows) {
				row.remove->set_button_icon(get_editor_theme_icon(SNAME("Remove")));
			}
		} break;

		case NOTIFICATION_VISIBILITY_CHANGED: {
			if (!is_visible() && type.is_valid()) {
				edit(Ref<LandscapeFoliageType>());
			}
		} break;
	}
}

void LandscapeFoliageLodDialog::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_apply_snapshot", "type", "state"), &LandscapeFoliageLodDialog::_apply_snapshot);
}

LandscapeFoliageLodDialog::LandscapeFoliageLodDialog() {
	singleton = this;
	set_title(TTR("Foliage LODs"));
	set_ok_button_text(TTR("Close"));

	VBoxContainer *vb = memnew(VBoxContainer);
	add_child(vb);

	Label *bar_title = memnew(Label(TTR("Distances (drag the boundaries)")));
	bar_title->set_theme_type_variation("HeaderSmall");
	vb->add_child(bar_title);
	bar = memnew(LandscapeFoliageLodBar);
	bar->set_tooltip_text(TTR("Levels of detail by distance from the camera. Darker bands don't cast shadows. Beyond the cull distance, instances aren't drawn; with Cull Random, they disappear gradually before it."));
	vb->add_child(bar);

	Label *levels_title = memnew(Label(TTR("Levels of Detail")));
	levels_title->set_theme_type_variation("HeaderSmall");
	vb->add_child(levels_title);
	table = memnew(GridContainer);
	table->set_columns(7);
	vb->add_child(table);
	for (const String &header : { TTR("Level"), TTR("Mesh"), TTR("From"), TTR("To"), TTR("Shadows"), TTR("Triangles"), String() }) {
		Label *label = memnew(Label(header));
		label->set_modulate(Color(1, 1, 1, 0.65));
		table->add_child(label);
	}
	add_lod = memnew(Button(TTR("Add LOD")));
	add_lod->set_h_size_flags(Control::SIZE_SHRINK_BEGIN);
	add_lod->connect(SceneStringName(pressed), callable_mp(this, &LandscapeFoliageLodDialog::_add_lod));
	vb->add_child(add_lod);

	vb->add_child(memnew(HSeparator));
	Label *cull_title = memnew(Label(TTR("Culling")));
	cull_title->set_theme_type_variation("HeaderSmall");
	vb->add_child(cull_title);
	GridContainer *settings = memnew(GridContainer);
	settings->set_columns(2);
	vb->add_child(settings);
	auto add_setting = [&](const String &p_label, SpinBox *p_spin, const String &p_tooltip) {
		Label *label = memnew(Label(p_label));
		label->set_tooltip_text(p_tooltip);
		label->set_mouse_filter(Control::MOUSE_FILTER_PASS);
		settings->add_child(label);
		p_spin->set_tooltip_text(p_tooltip);
		p_spin->set_custom_minimum_size(Size2(140, 0) * EDSCALE);
		p_spin->set_select_all_on_focus(true);
		p_spin->connect(SceneStringName(value_changed), callable_mp(this, &LandscapeFoliageLodDialog::_settings_changed));
		settings->add_child(p_spin);
	};
	cull_distance = memnew(SpinBox);
	cull_distance->set_max(16384.0);
	cull_distance->set_step(0.1);
	cull_distance->set_allow_greater(true);
	cull_distance->set_suffix("m");
	add_setting(TTR("Cull Distance"), cull_distance, TTR("Instances beyond this distance aren't drawn (0: never culled). Cells beyond it have no rendering resources."));
	cull_random = memnew(SpinBox);
	cull_random->set_max(100.0);
	cull_random->set_step(1.0);
	cull_random->set_suffix("%");
	add_setting(TTR("Cull Random"), cull_random, TTR("Instances disappear gradually over this fraction of the cull distance (each one at its own distance), instead of all at once."));
	transition = memnew(SpinBox);
	transition->set_max(64.0);
	transition->set_step(0.1);
	transition->set_allow_greater(true);
	transition->set_suffix("m");
	add_setting(TTR("LOD Transition"), transition, TTR("Hysteresis of the level changes: an instance changes its level this far beyond the boundary, so that it doesn't flicker when the camera moves around the boundary."));

	warning = memnew(Label);
	warning->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	warning->add_theme_color_override(SceneStringName(font_color), Color(1.0, 0.7, 0.3));
	warning->hide();
	vb->add_child(warning);

	Label *help = memnew(Label(TTR("The level of each instance is chosen from its distance to the camera. Cells (chunks) entirely within one level are drawn as a whole, the others are sorted per instance. LOD Distance Scale of the LandscapeFoliage3D node multiplies all these distances.")));
	help->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	help->set_custom_minimum_size(Size2(400, 0) * EDSCALE);
	help->set_modulate(Color(1, 1, 1, 0.6));
	vb->add_child(help);
}

LandscapeFoliageLodDialog::~LandscapeFoliageLodDialog() {
	if (singleton == this) {
		singleton = nullptr;
	}
}

/* Inspector */

void EditorPropertyFoliageLods::_edit_pressed() {
	LandscapeFoliageType *type = Object::cast_to<LandscapeFoliageType>(get_edited_object());
	if (type && LandscapeFoliageLodDialog::get_singleton()) {
		LandscapeFoliageLodDialog::get_singleton()->edit(Ref<LandscapeFoliageType>(type));
	}
}

void EditorPropertyFoliageLods::update_property() {
	LandscapeFoliageType *type = Object::cast_to<LandscapeFoliageType>(get_edited_object());
	if (!type) {
		return;
	}
	// Also refreshed when the LOD window changes the type.
	const Callable callback = callable_mp(this, &EditorPropertyFoliageLods::update_property);
	if (!type->is_connected(CoreStringName(changed), callback)) {
		type->connect(CoreStringName(changed), callback);
	}
	// Short enough for the width of the inspector, the details in the tooltip.
	const int lods = type->get_lod_count();
	const float cull = type->get_cull_distance();
	summary->set_text(vformat(lods == 1 ? TTR("%d LOD") : TTR("%d LODs"), lods) + String::utf8(" · ") + (cull > 0.0 ? _format_distance(cull) : String::utf8("∞")));
	String tooltip = lods == 1 ? TTR("1 level of detail") : vformat(TTR("%d levels of detail"), lods);
	for (int i = 1; i < lods; i++) {
		tooltip += "\n" + vformat(TTR("LOD %d from %s"), i, _format_distance(type->get_lod_start_distance(i)));
	}
	tooltip += "\n" + (cull > 0.0 ? vformat(TTR("Culled at %s"), _format_distance(cull)) : TTR("Never culled"));
	summary->set_tooltip_text(tooltip);
}

EditorPropertyFoliageLods::EditorPropertyFoliageLods() {
	HBoxContainer *hb = memnew(HBoxContainer);
	add_child(hb);
	summary = memnew(Label);
	summary->set_h_size_flags(SIZE_EXPAND_FILL);
	summary->set_clip_text(true);
	summary->set_mouse_filter(MOUSE_FILTER_PASS);
	hb->add_child(summary);
	edit_button = memnew(Button(TTR("Edit...")));
	edit_button->set_tooltip_text(TTR("Open the levels of detail and the cull distance in a separate window."));
	edit_button->connect(SceneStringName(pressed), callable_mp(this, &EditorPropertyFoliageLods::_edit_pressed));
	hb->add_child(edit_button);
	add_focusable(edit_button);
}

void EditorPropertyFoliageLayers::_toggled(bool p_pressed) {
	if (updating) {
		return;
	}
	uint32_t mask = 0;
	for (int i = 0; i < LandscapeData::MAX_LAYERS; i++) {
		if (buttons[i]->is_pressed()) {
			mask |= 1u << i;
		}
	}
	emit_changed(get_edited_property(), mask);
}

void EditorPropertyFoliageLayers::_set_read_only(bool p_read_only) {
	for (Button *button : buttons) {
		button->set_disabled(p_read_only);
	}
}

void EditorPropertyFoliageLayers::update_property() {
	const uint32_t mask = uint32_t(int64_t(get_edited_property_value()));
	const Landscape3D *landscape = ObjectDB::get_instance<Landscape3D>(LandscapeFoliagePanel::edited_landscape);
	updating = true;
	for (int i = 0; i < LandscapeData::MAX_LAYERS; i++) {
		buttons[i]->set_pressed(mask & (1u << i));
		String tooltip = vformat(TTR("Layer %d"), i);
		if (landscape && i < landscape->get_layer_count() && landscape->get_layer(i).is_valid() && !landscape->get_layer(i)->get_layer_name().is_empty()) {
			tooltip += ": " + landscape->get_layer(i)->get_layer_name();
		}
		buttons[i]->set_tooltip_text(tooltip);
	}
	updating = false;
}

EditorPropertyFoliageLayers::EditorPropertyFoliageLayers() {
	GridContainer *grid = memnew(GridContainer);
	grid->set_columns(8);
	grid->add_theme_constant_override("h_separation", 1 * EDSCALE);
	grid->add_theme_constant_override("v_separation", 1 * EDSCALE);
	add_child(grid);
	for (int i = 0; i < LandscapeData::MAX_LAYERS; i++) {
		buttons[i] = memnew(Button(itos(i)));
		buttons[i]->set_toggle_mode(true);
		buttons[i]->set_h_size_flags(SIZE_EXPAND_FILL);
		buttons[i]->set_custom_minimum_size(Size2(20, 0) * EDSCALE);
		buttons[i]->set_clip_text(true);
		buttons[i]->connect(SceneStringName(toggled), callable_mp(this, &EditorPropertyFoliageLayers::_toggled));
		grid->add_child(buttons[i]);
		add_focusable(buttons[i]);
	}
}

bool LandscapeFoliageInspectorPlugin::can_handle(Object *p_object) {
	return Object::cast_to<LandscapeFoliageType>(p_object) != nullptr;
}

bool LandscapeFoliageInspectorPlugin::parse_property(Object *p_object, const Variant::Type p_type, const String &p_path, const PropertyHint p_hint, const String &p_hint_text, const BitField<PropertyUsageFlags> p_usage, const bool p_wide) {
	if (p_path == "lod_count") {
		add_property_editor(p_path, memnew(EditorPropertyFoliageLods), false, TTR("LODs"));
		return true;
	}
	if (p_path == "layers" || p_path == "exclude_layers") {
		add_property_editor(p_path, memnew(EditorPropertyFoliageLayers));
		return true;
	}
	return false;
}

/* Foliage panel */

ObjectID LandscapeFoliagePanel::edited_landscape;

LandscapeFoliage3D *LandscapeFoliagePanel::_get_foliage() const {
	LandscapeFoliage3D *foliage = ObjectDB::get_instance<LandscapeFoliage3D>(foliage_id);
	return (foliage && foliage->is_inside_tree()) ? foliage : nullptr;
}

bool LandscapeFoliagePanel::_validate_landscape() {
	if (landscape && !ObjectDB::get_instance(landscape_id)) {
		landscape = nullptr;
		stroking = false;
		undo_cells.clear();
	}
	return landscape != nullptr;
}

Vector<int> LandscapeFoliagePanel::_get_active_types() const {
	Vector<int> types;
	const LandscapeFoliage3D *foliage = _get_foliage();
	if (!foliage) {
		return types;
	}
	for (int i = 0; i < foliage->get_foliage_type_count(); i++) {
		const Ref<LandscapeFoliageType> type = foliage->get_foliage_type(i);
		if (type.is_valid() && !unchecked_types.has(type->get_instance_id())) {
			types.push_back(i);
		}
	}
	return types;
}

void LandscapeFoliagePanel::_update_foliage_list() {
	uint64_t hash = hash_murmur3_one_64(uint64_t(foliage_id));
	const bool valid = _validate_landscape();
	if (valid) {
		for (LandscapeFoliage3D *foliage : landscape->get_foliages()) {
			hash = hash_murmur3_one_64(uint64_t(foliage->get_instance_id()), hash);
			hash = hash_murmur3_one_64(String(foliage->get_name()).hash(), hash);
		}
	}
	if (hash == foliage_list_hash) {
		return;
	}
	foliage_list_hash = hash;
	updating = true;
	foliage_select->clear();
	if (valid) {
		// Keep the edited node if it is still under the landscape, the first one otherwise.
		bool found = false;
		for (LandscapeFoliage3D *foliage : landscape->get_foliages()) {
			found = found || foliage->get_instance_id() == foliage_id;
		}
		if (!found) {
			foliage_id = landscape->get_foliages().is_empty() ? ObjectID() : landscape->get_foliages()[0]->get_instance_id();
		}
		for (LandscapeFoliage3D *foliage : landscape->get_foliages()) {
			foliage_select->add_icon_item(get_editor_theme_icon(SNAME("LandscapeFoliage3D")), foliage->get_name());
			foliage_select->set_item_metadata(-1, foliage->get_instance_id());
			if (foliage->get_instance_id() == foliage_id) {
				foliage_select->select(foliage_select->get_item_count() - 1);
			}
		}
	} else {
		foliage_id = ObjectID();
	}
	foliage_select->set_disabled(foliage_select->get_item_count() == 0);
	updating = false;
	palette_hash = 0;
	_update_palette();
}

void LandscapeFoliagePanel::_foliage_selected(int p_index) {
	if (updating) {
		return;
	}
	foliage_id = ObjectID(uint64_t(foliage_select->get_item_metadata(p_index)));
	selected_type = -1;
	palette_hash = 0;
	_update_palette();
}

void LandscapeFoliagePanel::_create_foliage() {
	if (!_validate_landscape()) {
		EditorNode::get_singleton()->show_warning(TTR("Select a Landscape3D to add foliage to it."));
		return;
	}
	LandscapeFoliage3D *foliage = memnew(LandscapeFoliage3D);
	foliage->set_name("Foliage");
	Node *owner = EditorNode::get_singleton()->get_edited_scene();
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(TTR("Create Landscape Foliage"), UndoRedo::MERGE_DISABLE, landscape);
	undo_redo->add_do_method(landscape, "add_child", foliage, true);
	undo_redo->add_do_method(foliage, "set_owner", owner);
	undo_redo->add_do_reference(foliage);
	undo_redo->add_undo_method(landscape, "remove_child", foliage);
	undo_redo->commit_action();
	foliage_id = foliage->get_instance_id();
	foliage_list_hash = 0;
	_update_foliage_list();
}

void LandscapeFoliagePanel::_update_palette() {
	LandscapeFoliage3D *foliage = _get_foliage();
	uint64_t hash = hash_murmur3_one_64(uint64_t(foliage_id));
	if (foliage) {
		for (int i = 0; i < foliage->get_foliage_type_count(); i++) {
			const Ref<LandscapeFoliageType> type = foliage->get_foliage_type(i);
			hash = hash_murmur3_one_64(type.is_valid() ? uint64_t(type->get_instance_id()) : 0, hash);
			hash = hash_murmur3_one_64(_foliage_type_name(type).hash(), hash);
			hash = hash_murmur3_one_64(type.is_valid() && type->get_mesh().is_valid() ? uint64_t(type->get_mesh()->get_instance_id()) : 0, hash);
		}
	}
	if (hash == palette_hash) {
		_update_palette_counts();
		return;
	}
	palette_hash = hash;
	updating = true;
	palette->clear();
	TreeItem *root = palette->create_item();
	if (foliage) {
		if (selected_type >= foliage->get_foliage_type_count()) {
			selected_type = foliage->get_foliage_type_count() - 1;
		}
		for (int i = 0; i < foliage->get_foliage_type_count(); i++) {
			const Ref<LandscapeFoliageType> type = foliage->get_foliage_type(i);
			TreeItem *item = palette->create_item(root);
			item->set_cell_mode(0, TreeItem::CELL_MODE_CHECK);
			item->set_editable(0, type.is_valid());
			item->set_checked(0, type.is_valid() && !unchecked_types.has(type->get_instance_id()));
			item->set_text(0, _foliage_type_name(type));
			item->set_icon(0, get_editor_theme_icon(SNAME("LandscapeFoliageType")));
			item->set_icon_max_width(0, 32 * EDSCALE);
			item->set_metadata(0, i);
			item->set_text_alignment(1, HORIZONTAL_ALIGNMENT_RIGHT);
			item->set_selectable(1, false);
			if (type.is_valid() && type->get_mesh().is_valid()) {
				EditorResourcePreview::get_singleton()->queue_edited_resource_preview(type->get_mesh(), callable_mp(this, &LandscapeFoliagePanel::_preview_ready).bind(type->get_instance_id()));
			}
			if (i == selected_type) {
				item->select(0);
			}
		}
	}
	updating = false;
	_update_palette_counts();
	_update_buttons();
}

void LandscapeFoliagePanel::_preview_ready(const String &p_path, const Ref<Texture2D> &p_preview, const Ref<Texture2D> &p_small_preview, ObjectID p_type) {
	if (p_preview.is_null()) {
		return;
	}
	const LandscapeFoliage3D *foliage = _get_foliage();
	TreeItem *root = palette->get_root();
	if (!foliage || !root) {
		return;
	}
	for (TreeItem *item = root->get_first_child(); item; item = item->get_next()) {
		const Ref<LandscapeFoliageType> type = foliage->get_foliage_type(item->get_metadata(0));
		if (type.is_valid() && type->get_instance_id() == p_type) {
			item->set_icon(0, p_preview);
		}
	}
}

void LandscapeFoliagePanel::_update_palette_counts() {
	const LandscapeFoliage3D *foliage = _get_foliage();
	TreeItem *root = palette->get_root();
	if (!foliage || !root) {
		return;
	}
	for (TreeItem *item = root->get_first_child(); item; item = item->get_next()) {
		const int index = item->get_metadata(0);
		if (index < foliage->get_foliage_type_count()) {
			item->set_text(1, _format_count(foliage->get_instance_count(index)));
		}
	}
}

void LandscapeFoliagePanel::_palette_edited() {
	if (updating) {
		return;
	}
	TreeItem *item = palette->get_edited();
	const LandscapeFoliage3D *foliage = _get_foliage();
	if (!item || !foliage) {
		return;
	}
	const Ref<LandscapeFoliageType> type = foliage->get_foliage_type(item->get_metadata(0));
	if (type.is_null()) {
		return;
	}
	if (item->is_checked(0)) {
		unchecked_types.erase(type->get_instance_id());
	} else {
		unchecked_types.insert(type->get_instance_id());
	}
}

void LandscapeFoliagePanel::_palette_selected() {
	TreeItem *item = palette->get_selected();
	selected_type = item ? int(item->get_metadata(0)) : -1;
	_update_buttons();
}

void LandscapeFoliagePanel::_palette_activated() {
	_edit_type();
}

void LandscapeFoliagePanel::_update_buttons() {
	const LandscapeFoliage3D *foliage = _get_foliage();
	const bool has_type = foliage && selected_type >= 0 && selected_type < foliage->get_foliage_type_count();
	const bool valid_type = has_type && foliage->get_foliage_type(selected_type).is_valid();
	remove_type->set_disabled(!has_type);
	edit_type->set_disabled(!valid_type);
	edit_lods->set_disabled(!valid_type);
}

void LandscapeFoliagePanel::_add_menu_pressed(int p_id) {
	if (p_id == ADD_NEW_TYPE) {
		Ref<LandscapeFoliageType> type;
		type.instantiate();
		_add_type(type);
		EditorNode::get_singleton()->push_item(type.ptr(), "", true);
	} else if (p_id == ADD_FROM_FILES) {
		file_dialog->popup_file_dialog();
	}
}

void LandscapeFoliagePanel::_files_selected(const PackedStringArray &p_files) {
	Vector<Ref<Resource>> resources;
	for (const String &path : p_files) {
		const Ref<Resource> resource = ResourceLoader::load(path);
		if (resource.is_valid()) {
			resources.push_back(resource);
		}
	}
	_add_resources(resources);
}

void LandscapeFoliagePanel::_add_resources(const Vector<Ref<Resource>> &p_resources) {
	// Meshes become new foliage types, foliage types are added as they are (shared).
	Vector<Ref<LandscapeFoliageType>> types;
	for (const Ref<Resource> &resource : p_resources) {
		const Ref<LandscapeFoliageType> foliage_type = resource;
		if (foliage_type.is_valid()) {
			types.push_back(foliage_type);
			continue;
		}
		const Ref<Mesh> mesh = resource;
		if (mesh.is_valid()) {
			Ref<LandscapeFoliageType> type;
			type.instantiate();
			type->set_mesh(mesh);
			if (mesh->get_path().is_resource_file()) {
				type->set_name(mesh->get_path().get_file().get_basename());
			}
			types.push_back(type);
		}
	}
	if (types.is_empty()) {
		EditorNode::get_singleton()->show_warning(TTR("Drop or select meshes or LandscapeFoliageType resources."));
		return;
	}
	if (!_get_foliage()) {
		_create_foliage();
	}
	LandscapeFoliage3D *foliage = _get_foliage();
	if (!foliage) {
		return;
	}
	const int base = foliage->get_foliage_type_count();
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(TTR("Add Foliage Types"), UndoRedo::MERGE_DISABLE, foliage);
	for (const Ref<LandscapeFoliageType> &type : types) {
		undo_redo->add_do_method(foliage, "add_foliage_type", type);
		undo_redo->add_undo_method(foliage, "remove_foliage_type", base);
	}
	undo_redo->add_do_method(this, "_update_palette");
	undo_redo->add_undo_method(this, "_update_palette");
	undo_redo->commit_action();
	selected_type = base;
	palette_hash = 0;
	_update_palette();
}

void LandscapeFoliagePanel::_add_type(const Ref<LandscapeFoliageType> &p_type) {
	Vector<Ref<Resource>> resources;
	resources.push_back(p_type);
	_add_resources(resources);
}

void LandscapeFoliagePanel::_remove_type() {
	LandscapeFoliage3D *foliage = _get_foliage();
	if (!foliage || selected_type < 0 || selected_type >= foliage->get_foliage_type_count()) {
		return;
	}
	const Ref<LandscapeFoliageType> type = foliage->get_foliage_type(selected_type);
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(TTR("Remove Foliage Type"), UndoRedo::MERGE_DISABLE, foliage);
	undo_redo->add_do_method(foliage, "remove_foliage_type", selected_type);
	undo_redo->add_undo_method(this, "_restore_type", foliage, selected_type, type, foliage->get_type_data(selected_type));
	undo_redo->add_do_method(this, "_update_palette");
	undo_redo->add_undo_method(this, "_update_palette");
	undo_redo->commit_action();
}

void LandscapeFoliagePanel::_restore_type(Object *p_foliage, int p_index, const Ref<LandscapeFoliageType> &p_type, const PackedByteArray &p_data) {
	LandscapeFoliage3D *foliage = Object::cast_to<LandscapeFoliage3D>(p_foliage);
	ERR_FAIL_NULL(foliage);
	const int index = foliage->add_foliage_type(p_type);
	foliage->move_foliage_type(index, p_index);
	foliage->set_type_data(p_index, p_data);
}

void LandscapeFoliagePanel::_edit_type() {
	const LandscapeFoliage3D *foliage = _get_foliage();
	if (!foliage || selected_type < 0 || selected_type >= foliage->get_foliage_type_count()) {
		return;
	}
	const Ref<LandscapeFoliageType> type = foliage->get_foliage_type(selected_type);
	if (type.is_valid()) {
		// In the inspector only: the Landscape dock stays open.
		EditorNode::get_singleton()->push_item(type.ptr(), "", true);
	}
}

void LandscapeFoliagePanel::_edit_lods() {
	const LandscapeFoliage3D *foliage = _get_foliage();
	if (!foliage || selected_type < 0 || selected_type >= foliage->get_foliage_type_count()) {
		return;
	}
	const Ref<LandscapeFoliageType> type = foliage->get_foliage_type(selected_type);
	if (type.is_valid() && LandscapeFoliageLodDialog::get_singleton()) {
		LandscapeFoliageLodDialog::get_singleton()->edit(type);
	}
}

void LandscapeFoliagePanel::_tool_selected(int p_tool) {
	tool = Tool(p_tool);
	update_brush_preview();
}

void LandscapeFoliagePanel::_brush_size_changed(double p_value) {
	if (updating) {
		return;
	}
	updating = true;
	brush_size->set_value(p_value);
	brush_size_slider->set_value(p_value);
	updating = false;
	update_brush_preview();
}

void LandscapeFoliagePanel::_commit_type_data(const String &p_action, const Vector<int> &p_types, const Vector<PackedByteArray> &p_before) {
	LandscapeFoliage3D *foliage = _get_foliage();
	ERR_FAIL_NULL(foliage);
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(p_action, UndoRedo::MERGE_DISABLE, foliage);
	for (int i = 0; i < p_types.size(); i++) {
		undo_redo->add_do_method(foliage, "set_type_data", p_types[i], foliage->get_type_data(p_types[i]));
		undo_redo->add_undo_method(foliage, "set_type_data", p_types[i], p_before[i]);
	}
	undo_redo->add_do_method(this, "_update_palette");
	undo_redo->add_undo_method(this, "_update_palette");
	undo_redo->commit_action(false);
	_update_palette();
}

void LandscapeFoliagePanel::_fill_pressed() {
	LandscapeFoliage3D *foliage = _get_foliage();
	const Vector<int> types = _get_active_types();
	if (!foliage || types.is_empty()) {
		EditorNode::get_singleton()->show_warning(TTR("Check the foliage types to fill the landscape with."));
		return;
	}
	int64_t estimate = 0;
	for (int type : types) {
		estimate += foliage->estimate_fill_count(type);
	}
	String text = vformat(TTR("Fill the whole landscape with %d foliage types, at their density where their filters allow them (up to %s instances)?"), types.size(), _format_count(estimate));
	if (estimate > 5000000) {
		text += "\n\n" + TTR("This is a lot of instances: consider a lower density, layer or slope filters, or painting only where they are needed.");
	}
	fill_confirm->set_text(text);
	fill_confirm->popup_centered();
}

void LandscapeFoliagePanel::_fill_confirmed() {
	LandscapeFoliage3D *foliage = _get_foliage();
	const Vector<int> types = _get_active_types();
	if (!foliage || types.is_empty()) {
		return;
	}
	Vector<PackedByteArray> before;
	for (int type : types) {
		before.push_back(foliage->get_type_data(type));
	}
	for (int type : types) {
		foliage->fill(type);
	}
	_commit_type_data(TTR("Fill Landscape With Foliage"), types, before);
}

void LandscapeFoliagePanel::_clear_pressed() {
	LandscapeFoliage3D *foliage = _get_foliage();
	const Vector<int> types = _get_active_types();
	if (!foliage || types.is_empty()) {
		return;
	}
	Vector<PackedByteArray> before;
	for (int type : types) {
		before.push_back(foliage->get_type_data(type));
		foliage->clear(type);
	}
	_commit_type_data(TTR("Clear Foliage"), types, before);
}

Vector<Ref<Resource>> LandscapeFoliagePanel::_get_dropped_resources(const Variant &p_data) const {
	Vector<Ref<Resource>> resources;
	const Dictionary drop = p_data;
	if (String(drop.get("type", "")) == "files") {
		const PackedStringArray files = drop["files"];
		for (const String &path : files) {
			const String type = ResourceLoader::get_resource_type(path);
			if (ClassDB::is_parent_class(type, Mesh::get_class_static()) || ClassDB::is_parent_class(type, LandscapeFoliageType::get_class_static())) {
				resources.push_back(Ref<Resource>()); // Loaded when dropped.
			}
		}
	} else if (String(drop.get("type", "")) == "resource") {
		const Ref<Resource> resource = drop["resource"];
		if (Object::cast_to<Mesh>(resource.ptr()) || Object::cast_to<LandscapeFoliageType>(resource.ptr())) {
			resources.push_back(resource);
		}
	}
	return resources;
}

bool LandscapeFoliagePanel::_can_drop_data_fw(const Point2 &p_point, const Variant &p_data) const {
	return !_get_dropped_resources(p_data).is_empty() && landscape != nullptr;
}

void LandscapeFoliagePanel::_drop_data_fw(const Point2 &p_point, const Variant &p_data) {
	const Dictionary drop = p_data;
	if (String(drop.get("type", "")) == "files") {
		_files_selected(drop["files"]);
		return;
	}
	_add_resources(_get_dropped_resources(p_data));
}

/* Painting */

bool LandscapeFoliagePanel::_update_cursor(Camera3D *p_camera, const Point2 &p_position) {
	cursor_valid = false;
	if (!_validate_landscape() || landscape->get_data().is_null() || !landscape->get_data()->is_valid()) {
		return false;
	}
	Vector3 position;
	Vector3 normal;
	if (landscape->intersect_ray(p_camera->project_ray_origin(p_position), p_camera->project_ray_normal(p_position), position, normal, p_camera->get_far())) {
		cursor_valid = true;
		cursor_global = position;
	}
	return cursor_valid;
}

void LandscapeFoliagePanel::update_brush_preview() {
	if (!_validate_landscape()) {
		return;
	}
	if (!cursor_valid || !is_visible_in_tree() || !_get_foliage()) {
		landscape->set_brush_preview(false);
		return;
	}
	Color color = Color(0.4, 1.0, 0.45);
	real_t radius = brush_size->get_value();
	if (tool == TOOL_ERASE || stroke_erase || (tool == TOOL_PAINT && Input::get_singleton()->is_key_pressed(Key::SHIFT))) {
		color = Color(1.0, 0.35, 0.3);
	} else if (tool == TOOL_SINGLE) {
		color = Color(1.0, 0.85, 0.3);
		radius = 0.5;
	} else if (tool == TOOL_REAPPLY) {
		color = Color(0.35, 0.7, 1.0);
	}
	landscape->set_brush_preview(true, landscape->global_to_local(cursor_global), radius, 0.0, color);
}

void LandscapeFoliagePanel::_snapshot(int p_type, const Vector3 &p_global_center, real_t p_radius) {
	LandscapeFoliage3D *foliage = _get_foliage();
	const Vector3 center = foliage->global_to_landscape(p_global_center);
	const Rect2 area = Rect2(Vector2(center.x, center.z), Vector2()).grow(p_radius + 0.01);
	for (const Rect2 &rect : foliage->get_cell_rects(area)) {
		uint64_t key = hash_murmur3_one_64(uint64_t(p_type));
		key = hash_murmur3_one_64(uint64_t(int64_t(Math::round(rect.position.x * 16.0))), key);
		key = hash_murmur3_one_64(uint64_t(int64_t(Math::round(rect.position.y * 16.0))), key);
		if (undo_cells.has(key)) {
			continue;
		}
		UndoCell cell;
		cell.type = p_type;
		cell.rect = rect;
		cell.before = foliage->get_instances_in_rect(p_type, rect);
		undo_cells.insert(key, cell);
	}
}

void LandscapeFoliagePanel::_apply_dab(const Vector3 &p_global) {
	LandscapeFoliage3D *foliage = _get_foliage();
	if (!foliage) {
		return;
	}
	const real_t radius = brush_size->get_value();
	for (int type : _get_active_types()) {
		_snapshot(type, p_global, radius);
		if (tool == TOOL_REAPPLY) {
			foliage->reapply(type, p_global, radius);
		} else if (stroke_erase) {
			foliage->erase(type, p_global, radius, erase_density->get_value());
		} else {
			foliage->paint(type, p_global, radius, paint_density->get_value());
		}
	}
	last_dab = p_global;
}

void LandscapeFoliagePanel::_begin_stroke(bool p_erase) {
	stroking = true;
	stroke_erase = p_erase;
	undo_cells.clear();
	if (tool == TOOL_SINGLE) {
		// One instance of one of the checked types.
		LandscapeFoliage3D *foliage = _get_foliage();
		const Vector<int> types = _get_active_types();
		if (foliage && !types.is_empty()) {
			const int type = types[Math::rand() % types.size()];
			_snapshot(type, cursor_global, 0.01);
			foliage->place_instance(type, cursor_global);
		}
		_end_stroke();
		return;
	}
	_apply_dab(cursor_global);
}

void LandscapeFoliagePanel::_end_stroke() {
	if (!stroking) {
		return;
	}
	stroking = false;
	stroke_erase = false;
	LandscapeFoliage3D *foliage = _get_foliage();
	if (!foliage || undo_cells.is_empty()) {
		undo_cells.clear();
		return;
	}
	static const char *action_names[] = { TTRC("Paint Foliage"), TTRC("Erase Foliage"), TTRC("Place Foliage Instance"), TTRC("Reapply Foliage Settings") };
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	bool changed = false;
	for (const KeyValue<uint64_t, UndoCell> &kv : undo_cells) {
		const PackedFloat32Array after = foliage->get_instances_in_rect(kv.value.type, kv.value.rect);
		if (after == kv.value.before) {
			continue;
		}
		if (!changed) {
			undo_redo->create_action(TTR(action_names[tool]), UndoRedo::MERGE_DISABLE, foliage);
			changed = true;
		}
		undo_redo->add_do_method(foliage, "set_instances_in_rect", kv.value.type, kv.value.rect, after);
		undo_redo->add_undo_method(foliage, "set_instances_in_rect", kv.value.type, kv.value.rect, kv.value.before);
	}
	undo_cells.clear();
	if (changed) {
		undo_redo->add_do_method(this, "_update_palette");
		undo_redo->add_undo_method(this, "_update_palette");
		undo_redo->commit_action(false);
	}
	_update_palette_counts();
}

void LandscapeFoliagePanel::cancel_stroke() {
	if (stroking) {
		_end_stroke();
	}
}

EditorPlugin::AfterGUIInput LandscapeFoliagePanel::forward_3d_gui_input(Camera3D *p_camera, const Ref<InputEvent> &p_event) {
	if (!_validate_landscape() || !_get_foliage() || landscape->get_data().is_null() || !landscape->get_data()->is_valid()) {
		return EditorPlugin::AFTER_GUI_INPUT_PASS;
	}
	Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_valid()) {
		_update_cursor(p_camera, mm->get_position());
		update_brush_preview();
		return stroking ? EditorPlugin::AFTER_GUI_INPUT_STOP : EditorPlugin::AFTER_GUI_INPUT_PASS;
	}
	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid() && mb->get_button_index() == MouseButton::LEFT) {
		if (mb->is_pressed()) {
			if (mb->is_alt_pressed() || !_update_cursor(p_camera, mb->get_position())) {
				return EditorPlugin::AFTER_GUI_INPUT_PASS;
			}
			if (_get_active_types().is_empty()) {
				EditorNode::get_singleton()->show_warning(TTR("Add foliage types to the palette and check the ones to paint."));
				return EditorPlugin::AFTER_GUI_INPUT_STOP;
			}
			_begin_stroke(tool == TOOL_ERASE || (tool == TOOL_PAINT && mb->is_shift_pressed()));
			update_brush_preview();
			return EditorPlugin::AFTER_GUI_INPUT_STOP;
		} else if (stroking) {
			_end_stroke();
			update_brush_preview();
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
	}
	if (k.is_valid() && k->get_keycode() == Key::SHIFT) {
		update_brush_preview();
	}
	return EditorPlugin::AFTER_GUI_INPUT_PASS;
}

String LandscapeFoliagePanel::get_statistics_text() const {
	const LandscapeFoliage3D *foliage = _get_foliage();
	if (!foliage) {
		return String();
	}
	const Dictionary s = foliage->get_statistics();
	return vformat(TTR("Foliage: %s instances in %d cells, %d cells drawn (%d sorted per instance), %s instances in %d batches, update %s ms"), _format_count(int64_t(s["instances"])), int64_t(s["cells"]), int64_t(s["cells_rendered"]), int64_t(s["cells_mixed"]), _format_count(int64_t(s["instances_drawn"])), int64_t(s["batches"]), String::num(int64_t(s["update_usec"]) / 1000.0, 2));
}

void LandscapeFoliagePanel::process(double p_delta) {
	_validate_landscape();
	if (stroking && !Input::get_singleton()->is_mouse_button_pressed(MouseButton::LEFT)) {
		// Released outside of the viewport.
		_end_stroke();
		update_brush_preview();
	}
	if (stroking && cursor_valid) {
		// Dabs spread along the path of the cursor.
		const real_t spacing = MAX(real_t(brush_size->get_value()) * 0.5f, real_t(0.25));
		const real_t distance = last_dab.distance_to(cursor_global);
		if (distance >= spacing) {
			const Vector3 from = last_dab;
			const int steps = CLAMP(int(Math::ceil(distance / spacing)), 1, 32);
			for (int i = 1; i <= steps; i++) {
				_apply_dab(from.lerp(cursor_global, real_t(i) / steps));
			}
		}
	}
	stats_timer -= p_delta;
	if (stats_timer <= 0.0) {
		// Nodes and types may also be changed in the scene tree and the inspector.
		stats_timer = 0.5;
		_update_foliage_list();
		_update_palette();
		String text;
		if (!landscape) {
			text = TTR("Select a Landscape3D or a LandscapeFoliage3D.");
		} else if (landscape->get_data().is_null() || !landscape->get_data()->is_valid()) {
			text = TTR("This landscape has no data yet. Create it in the Manage tab.");
		} else if (!_get_foliage()) {
			text = TTR("Create a LandscapeFoliage3D node to paint foliage on this landscape.");
		} else if (_get_foliage()->get_foliage_type_count() == 0) {
			text = TTR("Add foliage types: drop meshes or foliage types on the palette, or use the Add menu.");
		}
		info->set_text(text);
		info->set_visible(!text.is_empty());
		stats->set_text(get_statistics_text());
	}
}

void LandscapeFoliagePanel::edit(Landscape3D *p_landscape) {
	_validate_landscape();
	if (p_landscape == landscape) {
		return;
	}
	cancel_stroke();
	if (landscape) {
		landscape->set_brush_preview(false);
	}
	landscape = p_landscape;
	landscape_id = landscape ? landscape->get_instance_id() : ObjectID();
	edited_landscape = landscape_id;
	cursor_valid = false;
	selected_type = -1;
	foliage_list_hash = 0;
	_update_foliage_list();
}

void LandscapeFoliagePanel::edit_foliage(LandscapeFoliage3D *p_foliage) {
	if (p_foliage && p_foliage->get_landscape()) {
		edit(p_foliage->get_landscape());
	}
	if (p_foliage && p_foliage->get_instance_id() != foliage_id) {
		cancel_stroke();
		foliage_id = p_foliage->get_instance_id();
		selected_type = -1;
	}
	foliage_list_hash = 0;
	_update_foliage_list();
}

void LandscapeFoliagePanel::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_THEME_CHANGED: {
			List<BaseButton *> buttons;
			tool_group->get_buttons(&buttons);
			for (BaseButton *button : buttons) {
				Button *b = Object::cast_to<Button>(button);
				if (b && b->has_meta(SNAME("_foliage_icon"))) {
					b->set_button_icon(get_editor_theme_icon(StringName(b->get_meta(SNAME("_foliage_icon")))));
				}
			}
			add_type->set_button_icon(get_editor_theme_icon(SNAME("Add")));
			remove_type->set_button_icon(get_editor_theme_icon(SNAME("Remove")));
			palette_hash = 0;
		} break;

		case NOTIFICATION_VISIBILITY_CHANGED: {
			if (!is_visible_in_tree()) {
				cancel_stroke();
			}
			update_brush_preview();
		} break;
	}
}

void LandscapeFoliagePanel::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_update_palette"), &LandscapeFoliagePanel::_update_palette);
	ClassDB::bind_method(D_METHOD("_restore_type", "foliage", "index", "type", "data"), &LandscapeFoliagePanel::_restore_type);
}

LandscapeFoliagePanel::LandscapeFoliagePanel() {
	/* Node */
	HBoxContainer *node_hb = memnew(HBoxContainer);
	add_child(node_hb);
	Label *node_label = memnew(Label(TTR("Foliage Node")));
	node_hb->add_child(node_label);
	foliage_select = memnew(OptionButton);
	foliage_select->set_h_size_flags(SIZE_EXPAND_FILL);
	foliage_select->set_clip_text(true);
	foliage_select->set_tooltip_text(TTR("The LandscapeFoliage3D node painted (a child of the landscape)."));
	foliage_select->connect(SceneStringName(item_selected), callable_mp(this, &LandscapeFoliagePanel::_foliage_selected));
	node_hb->add_child(foliage_select);
	Button *create = memnew(Button(TTR("New")));
	create->set_tooltip_text(TTR("Add a LandscapeFoliage3D node to the landscape."));
	create->connect(SceneStringName(pressed), callable_mp(this, &LandscapeFoliagePanel::_create_foliage));
	node_hb->add_child(create);

	info = memnew(Label);
	info->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	info->set_custom_minimum_size(Size2(200 * EDSCALE, 0));
	add_child(info);

	/* Palette */
	Label *palette_title = memnew(Label(TTR("Foliage Types")));
	palette_title->set_theme_type_variation("HeaderSmall");
	add_child(palette_title);
	palette = memnew(Tree);
	palette->set_columns(2);
	palette->set_column_expand(1, false);
	palette->set_column_custom_minimum_width(1, 56 * EDSCALE);
	palette->set_hide_root(true);
	palette->set_custom_minimum_size(Size2(0, 180 * EDSCALE));
	palette->set_tooltip_text(TTR("Checked types are painted and erased. Drop meshes or foliage types here to add them. Double-click to edit a type."));
	palette->connect("item_edited", callable_mp(this, &LandscapeFoliagePanel::_palette_edited));
	palette->connect("item_selected", callable_mp(this, &LandscapeFoliagePanel::_palette_selected));
	palette->connect("item_activated", callable_mp(this, &LandscapeFoliagePanel::_palette_activated));
	palette->set_drag_forwarding(Callable(), callable_mp(this, &LandscapeFoliagePanel::_can_drop_data_fw), callable_mp(this, &LandscapeFoliagePanel::_drop_data_fw));
	add_child(palette);

	HBoxContainer *palette_hb = memnew(HBoxContainer);
	add_child(palette_hb);
	add_type = memnew(MenuButton);
	add_type->set_text(TTR("Add"));
	add_type->set_flat(false);
	add_type->set_tooltip_text(TTR("Add a new foliage type, or foliage types from meshes or saved foliage types."));
	add_type->get_popup()->add_item(TTR("New Foliage Type"), ADD_NEW_TYPE);
	add_type->get_popup()->add_item(TTR("From Meshes or Foliage Types..."), ADD_FROM_FILES);
	add_type->get_popup()->connect(SceneStringName(id_pressed), callable_mp(this, &LandscapeFoliagePanel::_add_menu_pressed));
	palette_hb->add_child(add_type);
	remove_type = memnew(Button);
	remove_type->set_tooltip_text(TTR("Remove the selected foliage type and its instances."));
	remove_type->connect(SceneStringName(pressed), callable_mp(this, &LandscapeFoliagePanel::_remove_type));
	palette_hb->add_child(remove_type);
	edit_type = memnew(Button(TTR("Edit")));
	edit_type->set_tooltip_text(TTR("Edit the selected foliage type in the inspector."));
	edit_type->connect(SceneStringName(pressed), callable_mp(this, &LandscapeFoliagePanel::_edit_type));
	palette_hb->add_child(edit_type);
	edit_lods = memnew(Button(TTR("LODs...")));
	edit_lods->set_tooltip_text(TTR("Open the levels of detail and the cull distance of the selected foliage type."));
	edit_lods->set_h_size_flags(SIZE_EXPAND_FILL);
	edit_lods->connect(SceneStringName(pressed), callable_mp(this, &LandscapeFoliagePanel::_edit_lods));
	palette_hb->add_child(edit_lods);

	/* Tools */
	Label *tools_title = memnew(Label(TTR("Tools")));
	tools_title->set_theme_type_variation("HeaderSmall");
	add_child(tools_title);
	tool_group.instantiate();
	GridContainer *tools = memnew(GridContainer);
	tools->set_columns(2);
	add_child(tools);
	struct ToolInfo {
		const char *name;
		const char *icon;
		const char *tooltip;
	};
	static const ToolInfo tool_infos[] = {
		{ TTRC("Paint"), "Paint", TTRC("Paint the checked types up to their density in the brush. Hold Shift to erase.") },
		{ TTRC("Erase"), "Eraser", TTRC("Remove instances of the checked types down to the erase density.") },
		{ TTRC("Single"), "Add", TTRC("Place one instance of a checked type at the cursor.") },
		{ TTRC("Reapply"), "Reload", TTRC("Apply the current placement settings and filters of the checked types to their instances in the brush.") },
	};
	for (int i = 0; i < 4; i++) {
		// Translated automatically (TTRC marks the strings for extraction).
		Button *button = memnew(Button(tool_infos[i].name));
		button->set_toggle_mode(true);
		button->set_button_group(tool_group);
		button->set_tooltip_text(tool_infos[i].tooltip);
		button->set_h_size_flags(SIZE_EXPAND_FILL);
		button->set_meta(SNAME("_foliage_icon"), tool_infos[i].icon);
		button->connect(SceneStringName(pressed), callable_mp(this, &LandscapeFoliagePanel::_tool_selected).bind(i));
		button->set_pressed(i == TOOL_PAINT);
		tools->add_child(button);
	}
	HBoxContainer *actions = memnew(HBoxContainer);
	add_child(actions);
	Button *fill = memnew(Button(TTR("Fill Landscape...")));
	fill->set_tooltip_text(TTR("Place the checked types over the whole landscape, at their density where their filters allow them."));
	fill->set_h_size_flags(SIZE_EXPAND_FILL);
	fill->connect(SceneStringName(pressed), callable_mp(this, &LandscapeFoliagePanel::_fill_pressed));
	actions->add_child(fill);
	Button *clear = memnew(Button(TTR("Clear")));
	clear->set_tooltip_text(TTR("Remove all the instances of the checked types."));
	clear->set_h_size_flags(SIZE_EXPAND_FILL);
	clear->connect(SceneStringName(pressed), callable_mp(this, &LandscapeFoliagePanel::_clear_pressed));
	actions->add_child(clear);

	/* Brush */
	add_child(memnew(HSeparator));
	Label *brush_title = memnew(Label(TTR("Brush Settings")));
	brush_title->set_theme_type_variation("HeaderSmall");
	add_child(brush_title);
	GridContainer *brush = memnew(GridContainer);
	brush->set_columns(2);
	add_child(brush);
	auto add_setting = [&](GridContainer *p_grid, const String &p_label, Control *p_control, const String &p_tooltip) {
		Label *label = memnew(Label(p_label));
		label->set_tooltip_text(p_tooltip);
		label->set_mouse_filter(MOUSE_FILTER_PASS);
		label->set_h_size_flags(SIZE_EXPAND_FILL);
		label->set_stretch_ratio(0.8);
		p_grid->add_child(label);
		p_control->set_tooltip_text(p_tooltip);
		p_control->set_h_size_flags(SIZE_EXPAND_FILL);
		p_grid->add_child(p_control);
	};
	brush_size = memnew(SpinBox);
	brush_size->set_min(0.1);
	brush_size->set_max(4096.0);
	brush_size->set_step(0.1);
	brush_size->set_allow_greater(true);
	brush_size->set_suffix("m");
	brush_size->set_value(8.0);
	brush_size->set_select_all_on_focus(true);
	brush_size->connect(SceneStringName(value_changed), callable_mp(this, &LandscapeFoliagePanel::_brush_size_changed));
	add_setting(brush, TTR("Brush Size"), brush_size, TTR("Brush radius. Shortcuts: [ and ]."));
	brush_size_slider = memnew(HSlider);
	brush_size_slider->set_min(0.1);
	brush_size_slider->set_max(512.0);
	brush_size_slider->set_step(0.1);
	brush_size_slider->set_exp_ratio(true);
	brush_size_slider->set_value(8.0);
	brush_size_slider->connect(SceneStringName(value_changed), callable_mp(this, &LandscapeFoliagePanel::_brush_size_changed));
	add_child(brush_size_slider);
	GridContainer *densities = memnew(GridContainer);
	densities->set_columns(2);
	add_child(densities);
	paint_density = memnew(SpinBox);
	paint_density->set_max(1.0);
	paint_density->set_step(0.01);
	paint_density->set_value(0.5);
	paint_density->set_select_all_on_focus(true);
	add_setting(densities, TTR("Paint Density"), paint_density, TTR("Fraction of the density of the types reached by painting."));
	erase_density = memnew(SpinBox);
	erase_density->set_max(1.0);
	erase_density->set_step(0.01);
	erase_density->set_value(0.0);
	erase_density->set_select_all_on_focus(true);
	add_setting(densities, TTR("Erase Density"), erase_density, TTR("Fraction of the density of the types left by erasing (0 removes every instance in the brush)."));

	Label *help = memnew(Label(TTR("LMB: paint  |  Shift+LMB: erase  |  [ / ]: brush size\nInstances follow the terrain when it is sculpted.")));
	help->set_modulate(Color(1, 1, 1, 0.6));
	help->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	help->set_custom_minimum_size(Size2(200 * EDSCALE, 0));
	add_child(help);

	stats = memnew(Label);
	stats->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	stats->set_custom_minimum_size(Size2(200 * EDSCALE, 0));
	stats->set_modulate(Color(1, 1, 1, 0.7));
	add_child(stats);

	file_dialog = memnew(EditorFileDialog);
	file_dialog->set_file_mode(EditorFileDialog::FILE_MODE_OPEN_FILES);
	file_dialog->set_access(EditorFileDialog::ACCESS_RESOURCES);
	file_dialog->set_title(TTR("Add Foliage Types"));
	List<String> extensions;
	ResourceLoader::get_recognized_extensions_for_type(Mesh::get_class_static(), &extensions);
	ResourceLoader::get_recognized_extensions_for_type(LandscapeFoliageType::get_class_static(), &extensions);
	HashSet<String> added;
	for (const String &extension : extensions) {
		if (!added.has(extension)) {
			added.insert(extension);
			file_dialog->add_filter("*." + extension);
		}
	}
	file_dialog->connect("files_selected", callable_mp(this, &LandscapeFoliagePanel::_files_selected));
	add_child(file_dialog);

	fill_confirm = memnew(ConfirmationDialog);
	fill_confirm->set_title(TTR("Fill Landscape"));
	fill_confirm->set_autowrap(true);
	fill_confirm->get_ok_button()->set_text(TTR("Fill"));
	fill_confirm->connect(SceneStringName(confirmed), callable_mp(this, &LandscapeFoliagePanel::_fill_confirmed));
	add_child(fill_confirm);

	_update_buttons();
}
