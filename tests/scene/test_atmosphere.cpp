/**************************************************************************/
/*  test_atmosphere.cpp                                                   */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_atmosphere)

#include "core/object/class_db.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/node_3d.h"
#include "scene/3d/world_environment.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/3d/sky_material.h"
#include "scene/resources/environment.h"
#include "scene/resources/sky.h"
#include "scene/resources/texture.h"
#ifdef RD_ENABLED
#include "servers/rendering/renderer_rd/environment/atmosphere.h"
#endif

namespace TestAtmosphere {

#ifdef RD_ENABLED
TEST_CASE("[Atmosphere] Light through the Earth's atmosphere") {
	RendererEnvironmentStorage::AtmosphereParams earth;
	earth.enabled = true;

	SUBCASE("Straight up, a little of the blue is scattered away, and less of the red") {
		// Optical depths at zenith: Rayleigh's coefficient times its scale
		// height, Mie's likewise, and the ozone tent's area.
		const Color transmittance = RendererRD::AtmosphereRD::light_transmittance(earth, Vector3(), Vector3(0, 1, 0));
		const double ozone = 0.5 * earth.ozone_width;
		CHECK(transmittance.r == doctest::Approx(Math::exp(-(0.175287 * 0.0331 * 8.0 + (0.003996 + 0.000444) * 1.2 + 0.345561 * 0.001881 * ozone))).epsilon(0.01));
		CHECK(transmittance.g == doctest::Approx(Math::exp(-(0.409607 * 0.0331 * 8.0 + (0.003996 + 0.000444) * 1.2 + 0.001881 * ozone))).epsilon(0.01));
		CHECK(transmittance.b == doctest::Approx(Math::exp(-(0.0331 * 8.0 + (0.003996 + 0.000444) * 1.2 + 0.045189 * 0.001881 * ozone))).epsilon(0.01));
	}

	SUBCASE("A low sun is dimmer and redder") {
		const Color high = RendererRD::AtmosphereRD::light_transmittance(earth, Vector3(), Vector3(0, 1, 0));
		const Color low = RendererRD::AtmosphereRD::light_transmittance(earth, Vector3(), Vector3(1, 0.05, 0).normalized());
		CHECK(low.r < high.r);
		CHECK(low.b < high.b);
		CHECK(low.r > low.g);
		CHECK(low.g > low.b);
	}

	SUBCASE("The planet hides a light below the horizon") {
		CHECK(RendererRD::AtmosphereRD::light_transmittance(earth, Vector3(), Vector3(1, -0.1, 0).normalized()) == Color(0, 0, 0));
	}

	SUBCASE("Higher up there is less air in the way") {
		const Color ground = RendererRD::AtmosphereRD::light_transmittance(earth, Vector3(), Vector3(1, 0.1, 0).normalized());
		const Color mountain = RendererRD::AtmosphereRD::light_transmittance(earth, Vector3(0, 4000, 0), Vector3(1, 0.1, 0).normalized());
		CHECK(mountain.b > ground.b);
	}

	SUBCASE("Without an atmosphere, light goes through untouched") {
		earth.enabled = false;
		CHECK(RendererRD::AtmosphereRD::light_transmittance(earth, Vector3(), Vector3(1, -0.1, 0).normalized()) == Color(1, 1, 1));
	}
}
#endif // RD_ENABLED

TEST_CASE("[Atmosphere] Environment parameters") {
	Ref<Environment> environment;
	environment.instantiate();
	CHECK_FALSE(environment->is_atmosphere_enabled());
	CHECK(environment->get_atmosphere_planet_radius() == doctest::Approx(6360.0));
	CHECK(environment->get_atmosphere_rayleigh_scattering_scale() == doctest::Approx(0.0331));
	CHECK(environment->get_atmosphere_mie_anisotropy() == doctest::Approx(0.8));

	environment->set_atmosphere_mie_scattering_scale(0.02);
	CHECK(environment->get_atmosphere_mie_scattering_scale() == doctest::Approx(0.02));
	CHECK(double(environment->get("atmosphere_mie_scattering_scale")) == doctest::Approx(0.02));
}

TEST_CASE("[Atmosphere] Volumetric clouds of the sky material") {
	Ref<AtmosphereSkyMaterial> material;
	material.instantiate();

	SUBCASE("On by default, and switched off by a shader of their own") {
		CHECK(material->is_clouds_enabled());
		CHECK(material->get_clouds_storm() == doctest::Approx(0.0));
		const RID with_clouds = material->get_shader_rid();
		material->set_clouds_enabled(false);
		const RID without_clouds = material->get_shader_rid();
		CHECK(with_clouds.is_valid());
		CHECK(without_clouds.is_valid());
		CHECK(with_clouds != without_clouds);
		material->set_clouds_enabled(true);
		CHECK(material->get_shader_rid() == with_clouds);
	}

	SUBCASE("Their parameters are properties a TimeOfDay track can drive") {
		material->set("clouds_storm", 0.75);
		CHECK(material->get_clouds_storm() == doctest::Approx(0.75));
		material->set_clouds_samples(0);
		CHECK(material->get_clouds_samples() == 1);
	}

	SUBCASE("Without textures of their own, they use the default noise") {
		CHECK(material->get_clouds_shape_texture().is_null());
		const Ref<Texture3D> shape = AtmosphereSkyMaterial::make_default_clouds_shape_texture();
		if (ClassDB::class_exists("NoiseTexture3D")) {
			REQUIRE(shape.is_valid());
			CHECK(shape->is_class("NoiseTexture3D"));
			CHECK(int(shape->get("width")) == 128);
			CHECK(bool(shape->get("seamless")));
			const Ref<Texture3D> detail = AtmosphereSkyMaterial::make_default_clouds_detail_texture();
			REQUIRE(detail.is_valid());
			CHECK(int(detail->get("width")) == 32);
		}
	}
}

TEST_CASE("[Atmosphere] Sky offers the atmosphere's material") {
	Ref<Sky> sky;
	sky.instantiate();
	List<PropertyInfo> properties;
	sky->get_property_list(&properties);
	bool found = false;
	for (const PropertyInfo &property : properties) {
		if (property.name == "sky_material") {
			found = true;
			CHECK(property.hint_string.split(",").has("AtmosphereSkyMaterial"));
		}
	}
	CHECK(found);
}

} // namespace TestAtmosphere
