/**************************************************************************/
/*  streaming_source_3d.cpp                                               */
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

#include "streaming_source_3d.h"

#ifndef _3D_DISABLED

#include "world_streaming.h"

#include "core/object/class_db.h"
#include "scene/resources/3d/world_3d.h"

void StreamingSource3D::_update_source() {
	WorldStreaming *ws = WorldStreaming::get_singleton();
	if (!ws) {
		return;
	}
	if (enabled && is_inside_tree() && get_world_3d().is_valid()) {
		ws->set_source(get_instance_id(), get_world_3d()->get_scenario(), get_global_position(), range_scale, priority);
	} else {
		ws->remove_source(get_instance_id());
	}
}

void StreamingSource3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_WORLD:
		case NOTIFICATION_TRANSFORM_CHANGED: {
			_update_source();
		} break;

		case NOTIFICATION_EXIT_WORLD: {
			if (WorldStreaming::get_singleton()) {
				WorldStreaming::get_singleton()->remove_source(get_instance_id());
			}
		} break;
	}
}

void StreamingSource3D::set_enabled(bool p_enabled) {
	enabled = p_enabled;
	_update_source();
}

void StreamingSource3D::set_range_scale(real_t p_scale) {
	range_scale = MAX(p_scale, real_t(0.0));
	_update_source();
}

void StreamingSource3D::set_priority(real_t p_priority) {
	priority = p_priority;
	_update_source();
}

void StreamingSource3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &StreamingSource3D::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &StreamingSource3D::is_enabled);
	ClassDB::bind_method(D_METHOD("set_range_scale", "scale"), &StreamingSource3D::set_range_scale);
	ClassDB::bind_method(D_METHOD("get_range_scale"), &StreamingSource3D::get_range_scale);
	ClassDB::bind_method(D_METHOD("set_priority", "priority"), &StreamingSource3D::set_priority);
	ClassDB::bind_method(D_METHOD("get_priority"), &StreamingSource3D::get_priority);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "range_scale", PROPERTY_HINT_RANGE, "0,4,0.01,or_greater"), "set_range_scale", "get_range_scale");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "priority", PROPERTY_HINT_RANGE, "0,16,0.01,or_greater"), "set_priority", "get_priority");
}

StreamingSource3D::StreamingSource3D() {
	set_notify_transform(true);
}

#endif // _3D_DISABLED
