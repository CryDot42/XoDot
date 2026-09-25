/**************************************************************************/
/*  atmosphere.h                                                          */
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

#include "servers/rendering/renderer_rd/pipeline_deferred_rd.h"
#include "servers/rendering/renderer_rd/shaders/environment/atmosphere.glsl.gen.h"
#include "servers/rendering/storage/environment_storage.h"

namespace RendererRD {

// A planet's atmosphere rendered in real time, as in Hillaire's "A Scalable
// and Production Ready Sky and Atmosphere Rendering Technique" (EGSR 2020),
// the technique behind Unreal Engine's Sky Atmosphere.
//
// Four lookup tables are kept here and rebuilt on the GPU:
// - transmittance and multiple scattering, which only depend on the
//   atmosphere's parameters, whenever those change;
// - the sky-view table and the aerial perspective volume, which follow the
//   eye and the lights, once per render that uses them.
// The sky samples the sky-view table, and scenes the aerial perspective
// volume. Anything else that needs to be lit through the air (volumetric
// clouds, for one) can bind the same tables and uniform buffer, and use
// atmosphere_inc.glsl to read them: see get_uniform_buffer() and the texture
// getters.
//
// The tables are shared by every viewport. Each render rebuilds the ones
// that follow the eye right before it draws, and GPU work runs in the order
// it was recorded, so a render never sees another's.
class AtmosphereRD {
public:
	// Keep in sync with AtmosphereData in atmosphere_data_inc.glsl (std140).
	struct AtmosphereData {
		float rayleigh_scattering[3];
		float bottom_radius;

		float mie_scattering[3];
		float top_radius;

		float mie_extinction[3];
		float mie_phase_g;

		float absorption_extinction[3];
		float rayleigh_density_exp_scale;

		float ground_albedo[3];
		float mie_density_exp_scale;

		float sky_luminance_factor[3];
		float absorption_tip_altitude;

		float camera_position[3];
		float absorption_width;

		float multiscattering_factor;
		float aerial_perspective_distance_scale;
		float aerial_perspective_start_depth;
		uint32_t light_count;

		float light_direction[2][4];
		float light_illuminance[2][4];

		float inv_projection[16];
		float camera_basis[16];

		float world_to_km;
		uint32_t enabled;
		float pad[2];
	};

	static constexpr uint32_t MAX_LIGHTS = 2;

	struct Light {
		Vector3 direction; // Towards the light.
		Color illuminance; // Linear color times energy.
	};

private:
	enum Mode {
		MODE_TRANSMITTANCE,
		MODE_MULTISCATTERING,
		MODE_SKY_VIEW,
		MODE_AERIAL_PERSPECTIVE,
		MODE_MAX,
	};

	AtmosphereShaderRD shader;
	RID shader_version;
	PipelineDeferredRD pipelines[MODE_MAX];

	RID uniform_buffer;
	RID transmittance_lut;
	RID multiscattering_lut;
	RID sky_view_lut;
	RID aerial_perspective_volume;

	AtmosphereData data = {};
	// What the transmittance and multiple scattering tables were last built
	// for, and whether they were built at all.
	uint32_t tables_hash = 0;
	bool tables_valid = false;
	bool active = false;

	void _create_textures();
	void _free_textures();
	void _dispatch(RD::ComputeListID p_list, Mode p_mode, RID p_dest, const Vector3i &p_threads);
	static uint32_t _hash_tables(const AtmosphereData &p_data);

public:
	void init();
	void free();

	// Brings the tables up to date for one render, and returns whether the
	// atmosphere is enabled in p_params (they are not touched otherwise).
	bool update(const RendererEnvironmentStorage::AtmosphereParams &p_params, const Transform3D &p_camera, const Projection &p_projection, const Light *p_lights, uint32_t p_light_count);

	// Whether the tables hold an atmosphere for the render in progress.
	bool is_active() const { return active; }
	// Identifies the atmosphere the tables hold, or 0 when there is none.
	uint32_t get_tables_hash() const { return active ? tables_hash : 0; }
	// A render without an atmosphere clears it, so nothing reads stale tables.
	void deactivate();

	RID get_uniform_buffer() const { return uniform_buffer; }
	RID get_transmittance_lut() const { return transmittance_lut; }
	RID get_multiscattering_lut() const { return multiscattering_lut; }
	RID get_sky_view_lut() const { return sky_view_lut; }
	RID get_aerial_perspective_volume() const { return aerial_perspective_volume; }

	// The planet's transmittance from p_camera towards a light at p_direction,
	// computed on the CPU for tinting directional lights (the sun reddens as
	// it sets because this is what reaches the ground).
	static Color light_transmittance(const RendererEnvironmentStorage::AtmosphereParams &p_params, const Vector3 &p_camera, const Vector3 &p_direction);
};

} // namespace RendererRD
