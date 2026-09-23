#include <engine/scene/Storm.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>

TEST_SUITE_ID("engine.scene.storm")
TEST_DEPENDS("engine.core.types")

using engine::core::Vector3;
using engine::scene::AdvanceStorm;
using engine::scene::DamageBand;
using engine::scene::EfCategory;
using engine::scene::EfPreset;
using engine::scene::LifecycleEnergy;
using engine::scene::PrepareTornadoField;
using engine::scene::QCategory;
using engine::scene::QPreset;
using engine::scene::QueryStormDamage;
using engine::scene::QueryStormVisibility;
using engine::scene::SampleTornadoField;
using engine::scene::SanitizeTornadoParameters;
using engine::scene::StormState;
using engine::scene::TornadoParameters;
using engine::scene::VisibilityQuery;

namespace {
	bool Finite(const Vector3 &value) {
		return std::isfinite(value.X) && std::isfinite(value.Y) && std::isfinite(value.Z);
	}
}

TEST_CASE("EF and Q tornado presets increase their coupled severity", "[scene][storm]") {
	TornadoParameters previous = EfPreset(EfCategory::EF0);
	for (size_t index = 1; index < 6; index++) {
		const TornadoParameters preset = EfPreset(static_cast<EfCategory>(index));
		CHECK(preset.Energy > previous.Energy);
		CHECK(preset.PeakTangentialSpeed > previous.PeakTangentialSpeed);
		CHECK(preset.PressureDrop > previous.PressureDrop);
		previous = preset;
	}
	CHECK(EfPreset(static_cast<EfCategory>(255)).InfluenceRadius == 520.0f);

	previous = EfPreset(EfCategory::EF5);
	for (size_t index = 0; index < 6; index++) {
		const TornadoParameters preset = QPreset(static_cast<QCategory>(index));
		CHECK(preset.CoreRadius > previous.CoreRadius);
		CHECK(preset.PeakTangentialSpeed > previous.PeakTangentialSpeed);
		CHECK(preset.TopHeight > previous.TopHeight);
		previous = preset;
	}
	CHECK(QPreset(static_cast<QCategory>(255)).PeakTangentialSpeed == 447.0f);
}

TEST_CASE("tornado parameter sanitization closes invalid authored values", "[scene][storm]") {
	TornadoParameters invalid;
	invalid.Energy = 5.0f;
	invalid.CoreRadius = 0.0f;
	invalid.InfluenceRadius = -20.0f;
	invalid.PeakTangentialSpeed = -1.0f;
	invalid.UpperWind.Y = 10.0f;
	invalid.Humidity = -1.0f;
	invalid.VortexTightness = 100.0f;
	invalid.TopHeight = 0.0f;

	const TornadoParameters clean = SanitizeTornadoParameters(invalid);
	CHECK(clean.Energy == 1.0f);
	CHECK(clean.CoreRadius == 1.0f);
	CHECK(clean.InfluenceRadius == Catch::Approx(1.25f));
	CHECK(clean.PeakTangentialSpeed == 0.0f);
	CHECK(clean.UpperWind.Y == 0.0f);
	CHECK(clean.Humidity == 0.0f);
	CHECK(clean.VortexTightness == 18.0f);
	CHECK(clean.TopHeight == 50.0f);
}

TEST_CASE("prepared field matches deterministic reference fixtures", "[scene][storm]") {
	const TornadoParameters parameters;
	const auto prepared = PrepareTornadoField(parameters);
	const std::array<Vector3, 3> points{{{34.0f, 5.0f, 0.0f}, {90.0f, 20.0f, -30.0f}, {0.0f, 100.0f, 0.0f}}};
	const std::array<Vector3, 3> velocity{
		{{-12.424633f, 19.002569f, 64.446610f},
		 {7.729156f, -9.602772f, 43.107094f},
		 {4.602900f, -4.237411f, -0.321400f}}
	};
	const std::array<float, 3> potential{{.533769f, .202896f, .060480f}};

	for (size_t index = 0; index < points.size(); index++) {
		const auto direct = SampleTornadoField(parameters, Vector3::Zero, points[index], 12.5f);
		const auto cached = SampleTornadoField(prepared, Vector3::Zero, points[index], 12.5f);
		CHECK(direct.Velocity.FuzzyEq(cached.Velocity, 1.0e-6f));
		CHECK(direct.Velocity.FuzzyEq(velocity[index], 1.0e-5f));
		CHECK(direct.DamagePotential == Catch::Approx(potential[index]).margin(1.0e-5f));
		CHECK(Finite(direct.Velocity));
	}
}

TEST_CASE("the tornado field retains its two-cell circulation", "[scene][storm]") {
	TornadoParameters circulation;
	circulation.TranslationVelocity = Vector3::Zero;
	circulation.UpperWind = Vector3::Zero;
	circulation.Turbulence = 0.0f;
	const float core = circulation.CoreRadius;

	const auto eye =
		SampleTornadoField(circulation, Vector3::Zero, {0.0f, circulation.TopHeight * .35f, 0.0f}, 0.0f);
	const auto eyewall = SampleTornadoField(
		circulation, Vector3::Zero, {core * .6f, circulation.TopHeight * .35f, 0.0f}, 0.0f
	);
	const auto rain = SampleTornadoField(
		circulation, Vector3::Zero, {core * 4.2f, circulation.TopHeight * .45f, 0.0f}, 0.0f
	);
	const auto upper = SampleTornadoField(
		circulation, Vector3::Zero, {core * .75f, circulation.TopHeight * .94f, 0.0f}, 0.0f
	);

	CHECK(eye.VerticalSpeed < 0.0f);
	CHECK(eyewall.VerticalSpeed > eye.VerticalSpeed);
	CHECK(rain.DowndraftSpeed > rain.UpdraftSpeed);
	CHECK(upper.UpperOutflowSpeed > 0.0f);
	CHECK(upper.Velocity.X > 0.0f);
}

TEST_CASE("storm lifecycle and gameplay queries share the field", "[scene][storm]") {
	StormState storm;
	storm.Parameters.TranslationVelocity = {4.0f, 0.0f, 1.5f};
	storm.LifecycleEnabled = true;
	AdvanceStorm(storm, 18.0f);
	CHECK(storm.Position.FuzzyEq({72.0f, 0.0f, 27.0f}));
	CHECK(storm.Parameters.Energy == Catch::Approx(LifecycleEnergy(18.0f)));

	StormState intense;
	intense.Parameters.Energy = 1.0f;
	intense.Parameters.Humidity = 1.0f;
	intense.Parameters.RainRate = 1.0f;
	intense.Parameters.PressureDrop = 120.0f;
	intense.Parameters.PeakTangentialSpeed = 150.0f;
	intense.Parameters.TranslationVelocity = Vector3::Zero;
	const auto near = QueryStormVisibility(intense, {{intense.Parameters.CoreRadius, 20.0f, 0.0f}, 300.0f});
	const auto far = QueryStormVisibility(intense, {{1000.0f, 20.0f, 0.0f}, 300.0f});
	CHECK(near.Clarity < far.Clarity);
	CHECK(near.EffectiveDistance < 300.0f);
	CHECK(near.CondensationObscuration > far.CondensationObscuration);

	const auto eyewall = QueryStormDamage(intense, {intense.Parameters.CoreRadius, 5.0f, 0.0f});
	const auto distant = QueryStormDamage(intense, {intense.Parameters.InfluenceRadius * 5.0f, 5.0f, 0.0f});
	CHECK(eyewall.Potential > distant.Potential);
	CHECK(static_cast<uint8_t>(eyewall.Band) > static_cast<uint8_t>(distant.Band));
	CHECK(eyewall.Band == DamageBand::Catastrophic);
}
