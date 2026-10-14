#include "WeatherResidency.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <limits>

TEST_SUITE_ID("engine.render.weatherresidency")
TEST_DEPENDS("engine.weather.assemblies")
TEST_DEPENDS("engine.weather.residency")

TEST_CASE(
	"certified empty coverage has no field payload and revision changes invalidate it immediately",
	"[render][weather]"
) {
	using namespace engine;
	using namespace engine::render;
	scene::WeatherCompute controls;
	controls.Coverage = 0;
	auto compiled = weather::Compile(controls);
	REQUIRE(compiled);
	auto time = weather::ResolveTime(weather::InitialAnchor(compiled->Definition), 100);
	REQUIRE(time);
	WeatherResidencyCache cache;
	const auto empty = BuildWeatherResidency(cache, controls, *compiled, *time, 1, {});
	REQUIRE(empty);
	REQUIRE_FALSE(empty->Pages.empty());
	CHECK(empty->CoarsePages > 0);
	CHECK_FALSE(empty->RequiresProceduralFallback);
	CHECK(empty->FieldWords == 0);
	CHECK(empty->ClassifiedPages == 0);
	CHECK(std::all_of(empty->PageKinds.begin(), empty->PageKinds.end(), [](uint8_t kind) {
		return kind == 2;
	}));
	const auto emptySignature = empty->Signature;
	controls.Coverage = 1;
	controls.VapourMixingRatio = controls.VapourScale;
	++controls.DefinitionRevision;
	compiled = weather::Compile(controls);
	REQUIRE(compiled);
	time = weather::ResolveTime(weather::InitialAnchor(compiled->Definition), 100);
	REQUIRE(time);
	const auto wet = BuildWeatherResidency(cache, controls, *compiled, *time, 2, {});
	REQUIRE(wet);
	CHECK(wet->Rebuilt);
	CHECK(wet->Signature != emptySignature);
	CHECK(wet->FieldWords > 0);
	CHECK(std::any_of(wet->PageKinds.begin(), wet->PageKinds.end(), [](uint8_t kind) { return kind != 2; }));
}

TEST_CASE("uniform nonempty coarse coverage retains half of the dense field payload", "[render][weather]") {
	using namespace engine;
	using namespace engine::render;
	scene::WeatherCompute controls;
	controls.FeatureCells = 1024;
	controls.Coverage = 0.02f;
	controls.VapourMixingRatio = controls.VapourScale;
	controls.TemperatureVariationCelsius = 0;
	controls.PressureVariationHectopascals = 0;
	controls.TurbulenceMetresPerSecond = 0;
	controls.ShearMetresPerSecond = 0;
	controls.LiftMetresPerSecond = 0;
	const auto compiled = weather::Compile(controls);
	REQUIRE(compiled);
	const auto time = weather::ResolveTime(weather::InitialAnchor(compiled->Definition), 100);
	REQUIRE(time);
	WeatherResidencyCache cache;
	std::optional<WeatherResidencyPlan> plan;
	for (uint64_t frame = 1; frame < 96; ++frame) {
		plan = BuildWeatherResidency(cache, controls, *compiled, *time, 1, {}, frame);
		REQUIRE(plan);
		if (!plan->MaintenancePending) break;
	}
	REQUIRE(plan);
	REQUIRE_FALSE(plan->MaintenancePending);
	REQUIRE_FALSE(plan->Pages.empty());
	CHECK(std::all_of(plan->PageKinds.begin(), plan->PageKinds.end(), [](uint8_t kind) {
		return kind == 1;
	}));
	const uint64_t denseBytes = plan->Pages.size() * weather::BRICK_PAYLOAD_BYTES;
	CHECK(static_cast<uint64_t>(plan->FieldWords) * sizeof(uint32_t) * 2 == denseBytes);
	CHECK(plan->MetadataBytes > denseBytes);
	CHECK(plan->MetadataBytes == WeatherResidencyMetadataBytes(cache));
}

TEST_CASE(
	"weather motion margins bound simultaneous turbulent wind on all encoded axes", "[render][weather]"
) {
	using namespace engine;
	using namespace engine::render;
	scene::WeatherCompute controls;
	controls.WindMetresPerSecond = {};
	controls.TurbulenceMetresPerSecond = 3;
	controls.ShearMetresPerSecond = 0;
	controls.LiftMetresPerSecond = 0;
	controls.GrowthRate = 0;
	const auto compiled = weather::Compile(controls);
	REQUIRE(compiled);
	const auto time = weather::ResolveTime(weather::InitialAnchor(compiled->Definition), 0);
	REQUIRE(time);
	WeatherResidencyCache cache;
	const auto plan = BuildWeatherResidency(cache, controls, *compiled, *time, 1, {});
	REQUIRE(plan);
	const float amplitude = static_cast<float>(compiled->Definition.Turbulence) / 127;
	const core::Vector3 componentBounds = compiled->Profile.WindScaleMetresPerSecond * amplitude;
	const float expectedMargin =
		componentBounds.Magnitude() * controls.MaintenanceSeconds + 4 * controls.CellSizeMetres;
	CHECK(plan->MotionMarginMetres >= expectedMargin);
}

TEST_CASE(
	"weather maintenance spends a fixed chunk once per world tick and keeps coarse coverage",
	"[render][weather]"
) {
	using namespace engine;
	using namespace engine::render;
	scene::WeatherCompute controls;
	const auto compiled = weather::Compile(controls);
	REQUIRE(compiled);
	const auto time = weather::ResolveTime(weather::InitialAnchor(compiled->Definition), 100);
	REQUIRE(time);
	const std::array<WeatherResidencyView, 1> views{{{{1, {0, 1200, 0}, 0}, 640, 12000}}};
	WeatherResidencyCache cache;
	const auto first = BuildWeatherResidency(cache, controls, *compiled, *time, 1, views);
	REQUIRE(first);
	CHECK(first->MaintenancePending);
	CHECK(first->CandidateEvaluations <= WEATHER_MAINTENANCE_CHUNK);
	CHECK(first->CoarsePages > 0);
	CHECK_FALSE(first->RequiresProceduralFallback);
	const uint32_t evaluated = cache.WorkEvaluations;
	const auto secondViewport = BuildWeatherResidency(cache, controls, *compiled, *time, 1, views);
	REQUIRE(secondViewport);
	CHECK(secondViewport->CandidateEvaluations == 0);
	CHECK(cache.WorkEvaluations == evaluated);
	CHECK(cache.NextMaintenanceTick > time->Tick);
	CHECK(cache.NextMaintenanceTick - time->Tick <= 100);
}

TEST_CASE(
	"paused weather completes maintenance and a density transition on presentation frames",
	"[render][weather]"
) {
	using namespace engine;
	using namespace engine::render;
	scene::WeatherCompute controls;
	controls.Paused = true;
	const auto compiled = weather::Compile(controls);
	REQUIRE(compiled);
	const auto time = weather::ResolveTime(weather::InitialAnchor(compiled->Definition), 100);
	REQUIRE(time);
	const std::array<WeatherResidencyView, 1> views{{{{1, {0, 1200, 0}, 0}, 640, 12000}}};
	WeatherResidencyCache cache;
	std::optional<WeatherResidencyPlan> plan;
	uint64_t frame = 1;
	for (; frame < 96; ++frame) {
		plan = BuildWeatherResidency(
			cache, controls, *compiled, *time, 1, views, frame, static_cast<double>(frame) / 60
		);
		REQUIRE(plan);
		CHECK(plan->CandidateEvaluations <= WEATHER_MAINTENANCE_CHUNK);
		CHECK(plan->ClassifiedPages <= WEATHER_CLASSIFICATION_CHUNK);
		if (!plan->MaintenancePending) break;
	}
	REQUIRE(frame < 96);
	REQUIRE(plan);
	CHECK_FALSE(plan->MaintenancePending);
	CHECK(plan->PreviousPages.size() > 0);
	const auto sameFrame = BuildWeatherResidency(
		cache, controls, *compiled, *time, 1, views, frame, static_cast<double>(frame) / 60
	);
	REQUIRE(sameFrame);
	CHECK(sameFrame->CandidateEvaluations == 0);
	const auto settled = BuildWeatherResidency(
		cache, controls, *compiled, *time, 1, views, frame + 60, static_cast<double>(frame) / 60 + 1.0
	);
	REQUIRE(settled);
	CHECK(settled->TransitionAlpha == 1.0f);
	CHECK(settled->PreviousPages.empty());
	CHECK(settled->MetadataBytes == WeatherResidencyMetadataBytes(cache));
	uint32_t expectedWords = 0;
	for (const uint8_t kind : settled->PageKinds)
		expectedWords += kind == 2 ? 0 : (kind == 1 ? 512 : 1024);
	CHECK(settled->FieldWords == expectedWords);
}

namespace {
	using namespace engine;
	using namespace engine::render;

	struct Inputs {
		scene::WeatherCompute Controls;
		weather::CompiledWeather Compiled = *weather::Compile(Controls);
		weather::WeatherTime Time = *weather::ResolveTime(weather::InitialAnchor(Compiled.Definition), 100);
		std::array<WeatherResidencyView, 2> Views{
			{{{1, {0, 1200, 0}, 0}, 640, 12000}, {{2, {6000, 1200, 0}, 3}, 640, 12000}}
		};
	};

	WeatherResidencyPlan FinishMaintenance(WeatherResidencyCache &cache, const Inputs &inputs) {
		std::optional<WeatherResidencyPlan> plan;
		for (uint64_t frame = 1; frame < 96; ++frame) {
			plan = BuildWeatherResidency(
				cache,
				inputs.Controls,
				inputs.Compiled,
				inputs.Time,
				1,
				inputs.Views,
				frame,
				static_cast<double>(frame) / 60
			);
			REQUIRE(plan);
			CHECK(plan->CandidateEvaluations <= WEATHER_MAINTENANCE_CHUNK);
			CHECK(plan->ClassifiedPages <= WEATHER_CLASSIFICATION_CHUNK);
			if (!plan->MaintenancePending) break;
		}
		REQUIRE(plan);
		REQUIRE_FALSE(plan->MaintenancePending);
		return *plan;
	}

	bool Covers(const glm::ivec4 &page, weather::CellCoordinate coordinate) {
		const int64_t side = static_cast<int64_t>(weather::BRICK_SIDE) << static_cast<uint32_t>(page.w);
		return coordinate.X >= page.x && coordinate.Y >= page.y && coordinate.Z >= page.z &&
			   coordinate.X < static_cast<int64_t>(page.x) + side &&
			   coordinate.Y < static_cast<int64_t>(page.y) + side &&
			   coordinate.Z < static_cast<int64_t>(page.z) + side;
	}
}

TEST_CASE(
	"renderer sparse packets keep whole-domain coarse coverage and nearby dense detail", "[render][weather]"
) {
	Inputs inputs;
	WeatherResidencyCache cache;
	const auto plan = std::optional(FinishMaintenance(cache, inputs));
	REQUIRE(plan);
	CHECK(plan->Rebuilt);
	CHECK(plan->Pages.size() <= WEATHER_RESIDENCY_PAGES);
	CHECK(plan->CoarsePages <= WEATHER_COARSE_PAGES);
	CHECK(plan->CoarsePages > 0);
	CHECK_FALSE(plan->RequiresProceduralFallback);
	CHECK(std::any_of(plan->Pages.begin(), plan->Pages.end(), [](const auto &page) { return page.w == 0; }));
	CHECK(std::any_of(plan->Pages.begin(), plan->Pages.end(), [](const auto &page) { return page.w >= 4; }));
	for (size_t index = 1; index < plan->Pages.size(); ++index)
		CHECK(plan->Pages[index - 1].w <= plan->Pages[index].w);
	for (const weather::CellCoordinate probe :
		 {weather::CellCoordinate{-128, 15, -128}, {127, 40, 127}, {0, 25, 0}})
		CHECK(std::any_of(plan->Pages.begin(), plan->Pages.end(), [&](const auto &page) {
			return Covers(page, probe);
		}));
	CHECK(plan->MotionMarginMetres >= inputs.Controls.CellSizeMetres * 4);
	CHECK(sizeof(glm::ivec4) == 16);
}

TEST_CASE(
	"renderer residency cache hits retain topology while each field tick stays canonical", "[render][weather]"
) {
	Inputs inputs;
	WeatherResidencyCache cache;
	const auto first = std::optional(FinishMaintenance(cache, inputs));
	REQUIRE(first);
	const uint64_t signature = first->Signature;
	const auto *storage = first->Pages.data();
	const auto sample = weather::Evaluate(inputs.Compiled.Definition, inputs.Time, {3, 25, 9});
	++inputs.Time.Tick;
	REQUIRE(inputs.Time.Tick < cache.NextMaintenanceTick);
	const auto reused =
		BuildWeatherResidency(cache, inputs.Controls, inputs.Compiled, inputs.Time, 1, inputs.Views);
	REQUIRE(reused);
	CHECK_FALSE(reused->Rebuilt);
	CHECK(reused->Signature == signature);
	CHECK(reused->Pages.data() == storage);
	CHECK(reused->MaintenanceAgeTicks == inputs.Time.Tick - cache.Maintenance.Tick);
	--inputs.Time.Tick;
	CHECK(weather::Evaluate(inputs.Compiled.Definition, inputs.Time, {3, 25, 9}) == sample);
	inputs.Time.Tick =
		std::max(cache.NextMaintenanceTick, cache.TransitionStartTick + cache.TransitionDurationTicks);
	const auto expired =
		BuildWeatherResidency(cache, inputs.Controls, inputs.Compiled, inputs.Time, 1, inputs.Views);
	REQUIRE(expired);
	CHECK(expired->Rebuilt);
	CHECK(expired->MaintenanceAgeTicks == 0);
	CHECK(expired->CoarsePages > 0);
	CHECK(expired->CandidateEvaluations <= WEATHER_MAINTENANCE_CHUNK);
}

TEST_CASE(
	"renderer view union ignores order and invalidates new portals teleports and quality", "[render][weather]"
) {
	Inputs inputs;
	WeatherResidencyCache cache;
	const auto first = std::optional(FinishMaintenance(cache, inputs));
	REQUIRE(first);
	const auto signature = first->Signature;
	std::reverse(inputs.Views.begin(), inputs.Views.end());
	const auto reordered =
		BuildWeatherResidency(cache, inputs.Controls, inputs.Compiled, inputs.Time, 1, inputs.Views);
	REQUIRE(reordered);
	CHECK_FALSE(reordered->Rebuilt);
	CHECK(reordered->Signature == signature);
	++inputs.Views[0].View.PortalRevision;
	const auto portal =
		BuildWeatherResidency(cache, inputs.Controls, inputs.Compiled, inputs.Time, 1, inputs.Views);
	REQUIRE(portal);
	CHECK(portal->Rebuilt);
	inputs.Views[0].View.CameraPositionMetres = {-6000, 1200, 0};
	const auto teleport =
		BuildWeatherResidency(cache, inputs.Controls, inputs.Compiled, inputs.Time, 1, inputs.Views);
	REQUIRE(teleport);
	CHECK(teleport->Rebuilt);
	inputs.Views[0].ProjectionPixelsPerRadian *= 2;
	const auto resized =
		BuildWeatherResidency(cache, inputs.Controls, inputs.Compiled, inputs.Time, 1, inputs.Views);
	REQUIRE(resized);
	CHECK(resized->Rebuilt);
	const auto timeline =
		BuildWeatherResidency(cache, inputs.Controls, inputs.Compiled, inputs.Time, 2, inputs.Views);
	REQUIRE(timeline);
	CHECK(timeline->Rebuilt);
	inputs.Views[0].View.Key = inputs.Views[1].View.Key;
	CHECK_FALSE(BuildWeatherResidency(cache, inputs.Controls, inputs.Compiled, inputs.Time, 2, inputs.Views));
}

TEST_CASE(
	"renderer reports guaranteed direct fallback when page budgets cannot hold every parent",
	"[render][weather]"
) {
	Inputs inputs;
	inputs.Controls.MaximumResidentBricks = 1;
	inputs.Compiled = *weather::Compile(inputs.Controls);
	WeatherResidencyCache cache;
	const auto limited = std::optional(FinishMaintenance(cache, inputs));
	REQUIRE(limited);
	CHECK(limited->Pages.size() == 1);
	CHECK(limited->RequiresProceduralFallback);
	CHECK(limited->RefusedRefinements > 0);
	CHECK(limited->CoarsePages == 1);
}

TEST_CASE(
	"renderer page assembly samples and physical fence reservations share the canonical API",
	"[render][weather]"
) {
	Inputs inputs;
	WeatherResidencyCache cache;
	REQUIRE(BuildWeatherResidency(cache, inputs.Controls, inputs.Compiled, inputs.Time, 1, inputs.Views));
	const auto summary = WeatherPageAssembly(inputs.Compiled, inputs.Time, cache.Selected.front());
	REQUIRE(summary);
	CHECK(summary->Revision == inputs.Time.Revision);
	CHECK(summary->Tick == inputs.Time.Tick);
	const auto children = weather::EvaluateChildren(inputs.Compiled.Definition, inputs.Time, summary->Region);
	REQUIRE(children);
	const auto direct = weather::Summarize(summary->Region, inputs.Time, *children);
	REQUIRE(direct);
	CHECK(direct->Scalars[0].Sum == summary->Scalars[0].Sum);
	weather::ResidencyState state;
	state.Slots.resize(cache.Selected.size() * 2);
	const auto generation = ReserveWeatherPages(state, cache, inputs.Time.Tick, inputs.Time.Revision, 0);
	REQUIRE(generation);
	CHECK(generation->Slots.size() == cache.Selected.size());
	REQUIRE(weather::MarkGenerationSubmitted(state, generation->Generation, 5));
	CHECK_FALSE(weather::PublishGeneration(state, generation->Generation, 4));
	REQUIRE(weather::PublishGeneration(state, generation->Generation, 5));
	CHECK(state.PublishedTick == inputs.Time.Tick);
}

TEST_CASE("renderer sparse input failures leave the previous packet available", "[render][weather]") {
	Inputs inputs;
	WeatherResidencyCache cache;
	REQUIRE(BuildWeatherResidency(cache, inputs.Controls, inputs.Compiled, inputs.Time, 1, inputs.Views));
	const auto signature = cache.TopologySignature;
	inputs.Views[0].ProjectionPixelsPerRadian = std::numeric_limits<float>::infinity();
	CHECK_FALSE(BuildWeatherResidency(cache, inputs.Controls, inputs.Compiled, inputs.Time, 1, inputs.Views));
	CHECK(cache.TopologySignature == signature);
	inputs.Views[0].ProjectionPixelsPerRadian = 640;
	++inputs.Time.Revision;
	CHECK_FALSE(BuildWeatherResidency(cache, inputs.Controls, inputs.Compiled, inputs.Time, 1, inputs.Views));
	CHECK(cache.TopologySignature == signature);
}
