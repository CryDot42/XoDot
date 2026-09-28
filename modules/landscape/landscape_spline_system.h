/**************************************************************************/
/*  landscape_spline_system.h                                             */
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

#include "landscape_data.h"
#include "landscape_spline_curve.h"

#include "core/templates/hash_set.h"

class LandscapeSpline3D;

// Applies the splines of a Landscape3D to its terrain data, non-destructively.
//
// The tiles under splines keep a base layer (see LandscapeData). Their final data is composited
// from the base and every spline covering them, by priority. The data remembers what was applied
// (one hash and one texel rect per block of each spline, see LandscapeSplineTerrainShape): when
// splines are added, removed, duplicated, moved or edited (also while the scene was closed), only
// the tiles of the blocks that changed are composited again.
class LandscapeSplineSystem : public LandscapeDataCompositor {
	Ref<LandscapeData> data;
	LocalVector<LandscapeSpline3D *> splines;
	HashSet<LandscapeSpline3D *> forced;
	bool dirty = true;
	bool force_all = false;
	uint64_t first_change_usec = 0;
	uint64_t last_change_usec = 0;
	int composited_tiles = 0;

	struct SortedSpline {
		int priority = 0;
		int64_t id = 0;
		const LandscapeSplineTerrainShape *shape = nullptr;
		bool operator<(const SortedSpline &p_other) const { return priority != p_other.priority ? priority < p_other.priority : id < p_other.id; }
	};
	void _get_sorted_shapes(LocalVector<SortedSpline> &r_shapes) const;
	void _composite_rect(const Rect2i &p_rect, int p_flags, const LocalVector<SortedSpline> &p_shapes);
	static PackedInt64Array _encode_record(const LandscapeSplineTerrainShape &p_shape);
	static void _add_record_rects(const PackedInt64Array &p_record, LocalVector<Rect2i> &r_rects);

public:
	// Delays of the terrain update while splines are edited (the mesh follows immediately).
	static constexpr uint64_t EDIT_IDLE_USEC = 80000;
	static constexpr uint64_t EDIT_MAX_DELAY_USEC = 250000;

	void set_data(const Ref<LandscapeData> &p_data);
	void register_spline(LandscapeSpline3D *p_spline);
	void unregister_spline(LandscapeSpline3D *p_spline);
	void spline_changed(LandscapeSpline3D *p_spline, bool p_force);
	void invalidate_all(); // Settings of the landscape or its data changed.
	void force_rebuild();
	const LocalVector<LandscapeSpline3D *> &get_splines() const { return splines; }
	bool has_pending_changes() const { return dirty; }

	// Applies the pending changes (immediately with p_immediate, otherwise after a short delay
	// while splines are being edited). Returns true when the terrain changed.
	bool process(bool p_immediate);
	int get_composited_tile_count() const { return composited_tiles; }

	virtual void composite_region(LandscapeData *p_data, const Rect2i &p_rect, int p_flags) override;

	~LandscapeSplineSystem();
};
