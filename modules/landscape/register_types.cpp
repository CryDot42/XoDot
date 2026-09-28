/**************************************************************************/
/*  register_types.cpp                                                    */
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

#include "register_types.h"

#include "landscape_3d.h"
#include "landscape_brush.h"
#include "landscape_data.h"
#include "landscape_gpu.h"
#include "landscape_layer.h"

#ifdef TOOLS_ENABLED
#include "editor/landscape_editor_plugin.h"
#endif

#include "core/config/project_settings.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/object/class_db.h"

static Ref<ResourceFormatLoaderLandscapeData> landscape_data_loader;
static Ref<ResourceFormatSaverLandscapeData> landscape_data_saver;

void initialize_landscape_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		GLOBAL_DEF(PropertyInfo(Variant::INT, "rendering/landscape/streaming/cpu_cache_size_mb", PROPERTY_HINT_RANGE, "64,16384,1,or_greater,suffix:MB"), 512);

		GDREGISTER_CLASS(LandscapeData);
		GDREGISTER_CLASS(LandscapeLayer);
		GDREGISTER_CLASS(LandscapeBrush);
		GDREGISTER_CLASS(Landscape3D);
#ifdef RD_ENABLED
		GDREGISTER_INTERNAL_CLASS(LandscapeGPU);
#endif

		landscape_data_loader.instantiate();
		ResourceLoader::add_resource_format_loader(landscape_data_loader);
		landscape_data_saver.instantiate();
		ResourceSaver::add_resource_format_saver(landscape_data_saver);
	}
#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		GDREGISTER_INTERNAL_CLASS(LandscapeEditor);
		GDREGISTER_INTERNAL_CLASS(LandscapeEditorPlugin);
		EditorPlugins::add_by_type<LandscapeEditorPlugin>();
	}
#endif
}

void uninitialize_landscape_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		Landscape3D::cleanup_shared_resources();
		ResourceLoader::remove_resource_format_loader(landscape_data_loader);
		landscape_data_loader.unref();
		ResourceSaver::remove_resource_format_saver(landscape_data_saver);
		landscape_data_saver.unref();
	}
}
