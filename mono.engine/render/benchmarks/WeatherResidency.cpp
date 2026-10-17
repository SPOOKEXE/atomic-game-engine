#include "WeatherResidency.hpp"

#include "WeatherGpu.hpp"

#include <engine/testing/Bench.hpp>

#include <array>
#include <stdexcept>

TEST_SUITE_ID("engine.render.bench.weatherresidency")

namespace {
	using namespace engine;
	using namespace engine::render;
	struct ResidencyFixture {
		scene::WeatherCompute Controls;
		weather::CompiledWeather Compiled = *weather::Compile(Controls);
		weather::WeatherTimelineAnchor Anchor = weather::InitialAnchor(Compiled.Definition);
		weather::WeatherTime Time = *weather::ResolveTime(Anchor, 100);
		std::array<WeatherResidencyView, 4> Views{
			{{{1, {0, 1200, 0}, 0}, 640, 12000},
			 {{2, {6000, 1200, 0}, 3}, 640, 12000},
			 {{3, {-6000, 1800, 6000}, 1}, 640, 12000},
			 {{4, {6000, 1800, -6000}, 1}, 640, 12000}}
		};
	};

	const ResidencyFixture &Fixture() {
		static const ResidencyFixture fixture;
		return fixture;
	}

	void ConsumePlan(const std::optional<WeatherResidencyPlan> &plan) {
		if (!plan) throw std::runtime_error("weather residency benchmark planning failed");
		testing::Consume(plan->Signature);
		testing::Consume(plan->Pages.size());
	}

	void CompleteMaintenance(WeatherResidencyCache &cache, const ResidencyFixture &fixture) {
		for (uint64_t frame = 1; frame < 96; ++frame) {
			const auto plan = BuildWeatherResidency(
				cache,
				fixture.Controls,
				fixture.Compiled,
				fixture.Time,
				1,
				fixture.Views,
				frame,
				static_cast<double>(frame) / 60
			);
			ConsumePlan(plan);
			if (!plan->MaintenancePending) return;
		}
		throw std::runtime_error("weather residency benchmark exceeded its bounded maintenance pass");
	}
}

BENCH("sparse residency: first one-view coarse coverage and bounded chunk", 50) {
	const auto &fixture = Fixture();
	for (size_t iteration = 0; iteration < 50; ++iteration) {
		WeatherResidencyCache cache;
		ConsumePlan(BuildWeatherResidency(
			cache, fixture.Controls, fixture.Compiled, fixture.Time, 1, std::span(fixture.Views).first(1)
		));
	}
}

BENCH("sparse residency: first four-view union and bounded chunk", 50) {
	const auto &fixture = Fixture();
	for (size_t iteration = 0; iteration < 50; ++iteration) {
		WeatherResidencyCache cache;
		ConsumePlan(
			BuildWeatherResidency(cache, fixture.Controls, fixture.Compiled, fixture.Time, 1, fixture.Views)
		);
	}
}

BENCH("sparse residency: warm four-view topology reuse", 10000) {
	const auto &fixture = Fixture();
	static WeatherResidencyCache cache;
	if (cache.PageKeys.empty() || cache.WorkActive || cache.ClassificationActive)
		CompleteMaintenance(cache, fixture);
	for (size_t iteration = 0; iteration < 10000; ++iteration)
		ConsumePlan(
			BuildWeatherResidency(cache, fixture.Controls, fixture.Compiled, fixture.Time, 1, fixture.Views)
		);
}

BENCH_PER_ITEM("sparse residency: 180 animated-sun shadow demands per frame", 180) {
	static const auto fixture = [] {
		ResidencyFixture created;
		created.Controls.BaseHeightMetres = 1024;
		created.Controls.ThicknessMetres = 256;
		created.Controls.ExtentMetres = {2048, 1024, 2048};
		created.Controls.ShadowDistanceMetres = 2048;
		created.Controls.ShadowResolution = 256;
		created.Controls.ShadowCascades = 2;
		const auto compiled = weather::Compile(created.Controls);
		if (!compiled) throw std::runtime_error("animated shadow benchmark definition refused");
		created.Compiled = *compiled;
		created.Anchor = weather::InitialAnchor(created.Compiled.Definition);
		created.Time = *weather::ResolveTime(created.Anchor, 100);
		return created;
	}();
	const auto grid = WeatherGridOf(fixture.Controls);
	if (!grid) throw std::runtime_error("animated shadow benchmark grid refused");
	const WeatherGpuInputs gpuInputs{
		WeatherUniformsOf(fixture.Compiled.Definition, fixture.Time, *grid),
		fixture.Compiled,
		fixture.Time,
		false
	};
	WeatherResidencyCache cache;
	uint32_t candidateEvaluations = 0;
	uint32_t classifiedPages = 0;
	for (uint64_t frame = 1; frame <= 180; ++frame) {
		std::array<WeatherResidencyView, weather::MAXIMUM_RESIDENCY_VIEWS> views{};
		views[0] = fixture.Views[0];
		const core::Vector3 sun{-0.35f - static_cast<float>(frame) * 0.000002f, -0.8f, -0.2f};
		const auto shadow =
			BuildWeatherShadowPlan(fixture.Controls, gpuInputs, sun, 1, 1, std::span(views).first(1));
		if (!shadow.Enabled) throw std::runtime_error("animated shadow benchmark footprint refused");
		const auto count = AppendWeatherShadowDemand(views, 1, shadow, fixture.Compiled.Definition.LayerKey);
		const auto plan = BuildWeatherResidency(
			cache,
			fixture.Controls,
			fixture.Compiled,
			fixture.Time,
			1,
			std::span(views).first(count),
			frame,
			static_cast<double>(frame) / 60
		);
		ConsumePlan(plan);
		if (frame >= 96 && plan->MaintenancePending)
			throw std::runtime_error("animated shadow benchmark restarted bounded maintenance");
		candidateEvaluations += plan->CandidateEvaluations;
		classifiedPages += plan->ClassifiedPages;
	}
	testing::Consume(candidateEvaluations);
	testing::Consume(classifiedPages);
}

BENCH("sparse residency: complete four-view maintenance and compression classification", 20) {
	const auto &fixture = Fixture();
	for (size_t iteration = 0; iteration < 20; ++iteration) {
		WeatherResidencyCache cache;
		CompleteMaintenance(cache, fixture);
		testing::Consume(cache.FieldWords);
		testing::Consume(WeatherResidencyMetadataBytes(cache));
	}
}

BENCH("sparse residency: teleport invalidation with four views", 50) {
	const auto &fixture = Fixture();
	WeatherResidencyCache cache;
	auto views = fixture.Views;
	ConsumePlan(BuildWeatherResidency(cache, fixture.Controls, fixture.Compiled, fixture.Time, 1, views));
	for (size_t iteration = 0; iteration < 50; ++iteration) {
		views[0].View.CameraPositionMetres.X = iteration % 2 == 0 ? 6000.0f : -6000.0f;
		ConsumePlan(BuildWeatherResidency(cache, fixture.Controls, fixture.Compiled, fixture.Time, 1, views));
	}
}

BENCH("sparse residency: certified empty first coarse coverage without field allocation", 1000) {
	static const auto fixture = [] {
		ResidencyFixture result;
		result.Controls.Coverage = 0;
		result.Compiled = *weather::Compile(result.Controls);
		result.Time = *weather::ResolveTime(weather::InitialAnchor(result.Compiled.Definition), 100);
		return result;
	}();
	for (size_t iteration = 0; iteration < 1000; ++iteration) {
		WeatherResidencyCache cache;
		const auto plan =
			BuildWeatherResidency(cache, fixture.Controls, fixture.Compiled, fixture.Time, 1, {});
		ConsumePlan(plan);
		if (plan->FieldWords != 0)
			throw std::runtime_error("certified empty field unexpectedly retained payload");
		testing::Consume(plan->MetadataBytes);
	}
}
