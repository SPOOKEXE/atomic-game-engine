#include "WeatherFallback.hpp"

#include "RenderFixture.hpp"

#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.render.weather-fallback")
TEST_DEPENDS("engine.graph.weatherpipeline")

using namespace engine;
using Catch::Approx;

TEST_CASE("weather volume suppression follows actual enabled pipeline nodes", "[render][weather]") {
	graph::RenderGraph graph;
	graph::RegisterRenderNodeKinds();
	core::Name offender;
	REQUIRE(graph::Build(graph::DefaultPbrDocument(), graph, offender) == graph::PipelineDocumentStatus::Ok);
	CHECK(render::WeatherVolumeGraphAvailable(graph, true, true));
	CHECK_FALSE(render::WeatherVolumeGraphAvailable(graph, false, true));
	CHECK_FALSE(render::WeatherVolumeGraphAvailable(graph, true, false));
	for (uint32_t value = 1; value <= graph.Count(); ++value) {
		const auto *node = graph.Find(graph::NodeId{value});
		if (node && node->Kind == core::Name("weather-integrate"))
			REQUIRE(graph.SetEnabled(graph::NodeId{value}, false));
	}
	CHECK_FALSE(render::WeatherVolumeGraphAvailable(graph, true, true));
	graph::RenderGraph tierB;
	REQUIRE(
		graph::Build(graph::DefaultPbrTierBDocument(), tierB, offender) == graph::PipelineDocumentStatus::Ok
	);
	CHECK_FALSE(render::WeatherVolumeGraphAvailable(tierB, true, true));
}

TEST_CASE("partial weather publication cannot expose unwritten summary products", "[render][weather]") {
	render::WeatherGpuGeneration generation;
	// Metadata checks only inspect non-null identities, never backend storage.
	std::byte identity{};
	generation.Cells = reinterpret_cast<SDL_GPUBuffer *>(&identity);
	generation.Pages = generation.Cells;
	generation.PreviousPages = generation.Cells;
	generation.Summaries = generation.Cells;
	generation.RenderInputs.emplace();
	generation.Residency.Ready = generation.Field.Ready = true;
	generation.Residency.Signature = 7;
	generation.Field.Signature = 11;
	CHECK_FALSE(render::WeatherProductsReady(&generation, &identity));
	generation.Summary.Ready = true;
	generation.Summary.Signature = 11;
	CHECK(render::WeatherProductsReady(&generation, &identity));
	generation.Field.Stage(22, &identity);
	generation.PendingRenderInputs.emplace();
	CHECK_FALSE(render::WeatherProductsReady(&generation, &identity));
	generation.Summary.Stage(22, &identity);
	CHECK(render::WeatherProductsReady(&generation, &identity));
	render::RefuseWeatherPublication(generation);
	CHECK_FALSE(render::WeatherProductsReady(&generation, &identity));
	CHECK(generation.Cells != nullptr);
	CHECK_FALSE(generation.PendingRenderInputs);
	CHECK_FALSE(generation.Field.Pending);
	generation.Residency.Stage(7, &identity);
	generation.Field.Stage(22, &identity);
	generation.Summary.Stage(22, &identity);
	generation.PendingRenderInputs.emplace();
	CHECK(render::WeatherProductsReady(&generation, &identity));
	generation.PendingRenderInputs->Stateful = true;
	generation.PendingRenderInputs->Uniforms.Phase.z = UINT32_MAX;
	CHECK_FALSE(render::WeatherProductsReady(&generation, &identity));
}

TEST_CASE("fragment fallback uses world units and authored coloured lighting", "[render][weather]") {
	scene::WorldLighting lighting;
	auto &environment = lighting.EnvironmentState;
	environment.HasWeatherCompute = environment.HasClouds = true;
	environment.HasAtmosphere = true;
	environment.Weather.BaseHeightMetres = 40;
	environment.Weather.ThicknessMetres = 1000;
	environment.CloudLayer.Cover = .2f;
	environment.CloudLayer.Density = .5f;
	lighting.Direct = {0, 2, 0};
	lighting.Ambient = {.1f, .2f, .3f};
	lighting.OutdoorAmbient = {.2f, .4f, .6f};
	const auto fallback = render::WeatherFallbackOf(lighting, true, true, true, false, 63);
	CHECK(fallback.Minimum.y == 40);
	CHECK(fallback.Maximum.y == 1040);
	CHECK(
		fallback.CloudColourExtinction.w ==
		Approx(weather::ExtinctionPerMetre(environment.Weather.CondensateScale) * .1f)
	);
	CHECK(fallback.Direct.x == 0);
	CHECK(fallback.Direct.y == 2);
	CHECK(fallback.Ambient.z == Approx(.6f));
	CHECK((fallback.Modes.x & 1u) != 0); // Existing environment bypasses a second atmosphere composition.
	CHECK(fallback.Modes.y == 63);
	environment.CloudVolume.Shader = scene::CloudComputeShader::Storm;
	environment.AirCompute.Shader = scene::AtmosphereProceduralShader::Mars;
	const auto styled = render::WeatherFallbackOf(lighting, false, true, true, false, 0);
	CHECK(((styled.Modes.w >> 8) & 255u) == 2);
	CHECK(((styled.Modes.w >> 16) & 255u) == 2);
	CHECK(render::VisibleWeatherMedium(environment));
	environment.CloudLayer.Enabled = false;
	CHECK_FALSE(render::VisibleWeatherMedium(environment));
	lighting.Direct = {};
	lighting.Ambient = {};
	lighting.OutdoorAmbient = {};
	const auto night = render::WeatherFallbackOf(lighting, false, true, true, true, 0);
	CHECK(night.Direct == glm::vec4{});
	CHECK(night.Ambient == glm::vec4{});
}

TEST_CASE(
	"weather fallback submits visible clouds after compute products are refused", "[render][gpu][weather][.]"
) {
	using namespace render;
	test::FixtureDevice fixture;
	fixture.Initialise();
	const core::Name boundary("weather-fallback-output");
	graph::NodeKindSpec spec;
	spec.Kind = boundary;
	spec.Scope = graph::NodeScope::Frame;
	spec.Queue = graph::ExecutionQueue::Cpu;
	spec.Category = graph::NodeCategory::Output;
	spec.Inputs.push_back({.Name = core::Name("tonemapped"), .Kind = graph::ResourceKind::Texture});
	REQUIRE(graph::RegisterNodeKind(std::move(spec)));
	REQUIRE(fixture.Render.InstallNodeHandler(boundary, [](const graph::RunContext &) { return true; }));
	const std::array pipelines{
		core::Name("weather.fallback.tier-b"),
		core::Name("weather.fallback.custom"),
		core::Name("weather.fallback.refused")
	};
	for (size_t scenario = 0; scenario < pipelines.size(); ++scenario) {
		INFO("fallback scenario=" << scenario);
		const auto original = scenario == 0 ? graph::DefaultPbrTierBDocument() : graph::DefaultPbrDocument();
		graph::PipelineDocument document;
		for (auto edit : original.Edits()) {
			// The existing allocator refuses an incompatible stride before requesting device memory.
			if (scenario == 2 && edit.Kind == graph::EditKind::AddResource &&
				edit.Name == core::Name("weather-pages"))
				edit.BufferStride = 1024 * 1024;
			document.Record(std::move(edit));
		}
		document.Record(
			{.Kind = graph::EditKind::AddNode,
			 .Name = boundary,
			 .NodeKind = boundary,
			 .Scope = graph::NodeScope::Frame}
		);
		document.Record(
			{.Kind = graph::EditKind::Reads,
			 .Target = core::Name("tonemapped"),
			 .Key = core::Name("tonemapped")}
		);
		graph::RenderGraph graph;
		core::Name offender;
		REQUIRE(graph::Build(document, graph, offender) == graph::PipelineDocumentStatus::Ok);
		if (scenario == 1)
			for (uint32_t value = 1; value <= graph.Count(); ++value) {
				const auto *node = graph.Find(graph::NodeId{value});
				if (node && node->Kind == core::Name("weather-integrate"))
					REQUIRE(graph.SetEnabled(graph::NodeId{value}, false));
			}
		REQUIRE(fixture.Render.SetPipeline(pipelines[scenario], graph));
		constexpr uint32_t EXTENT = 33;
		SceneTarget target{EXTENT, EXTENT};
		View view;
		view.World = 811 + scenario;
		view.WorldName = pipelines[scenario];
		view.Pipeline = pipelines[scenario];
		view.Target = &target;
		view.OverrideLighting = true;
		view.Lighting.Ambient = {.1f, .1f, .1f};
		view.Lighting.OutdoorAmbient = {};
		view.Lighting.Direct = {1, 1, 1};
		view.Lighting.FogStart = 1000;
		view.Lighting.FogEnd = 1001;
		view.Camera.FarPlane = 32;
		auto &environment = view.Lighting.EnvironmentState;
		environment.HasClouds = environment.HasCloudCompute = environment.HasWeatherCompute = true;
		environment.CloudLayer.Colour = {1, .05f, .01f};
		environment.CloudLayer.Cover = environment.CloudLayer.Density = 1;
		environment.Weather.CellSizeMetres = 1;
		environment.Weather.ExtentMetres = {16, 16, 16};
		environment.Weather.BaseHeightMetres = -8;
		environment.Weather.ThicknessMetres = 16;
		environment.Weather.Coverage = 1;
		environment.Weather.FeatureCells = 8;
		environment.Weather.WindMetresPerSecond = {};
		environment.Weather.Seed = 17;
		environment.Weather.ShadowsEnabled = false;
		environment.CloudLayer.Enabled = false;
		OverlayImage overlay;
		REQUIRE(fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false).Submitted);
		const auto clear = test::CaptureResource(
			fixture.Render, core::Name("tonemapped"), view.Slot, EXTENT, EXTENT, test::ImageFormat::Rgba8Unorm
		);
		environment.CloudLayer.Enabled = true;
		const auto frame = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
		REQUIRE(frame.Submitted);
		CHECK(frame.Ran(core::Name("sky")));
		const auto cloudy = test::CaptureResource(
			fixture.Render, core::Name("tonemapped"), view.Slot, EXTENT, EXTENT, test::ImageFormat::Rgba8Unorm
		);
		CHECK(test::CompareImages(clear.View(), cloudy.View()).MismatchedPixels > 0);
		if (scenario == 0) {
			view.CameraFrame.Position = {0, 0, -20};
			view.Camera.FarPlane = 8;
			REQUIRE(fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false).Submitted);
			const auto clipped = test::CaptureResource(
				fixture.Render,
				core::Name("tonemapped"),
				view.Slot,
				EXTENT,
				EXTENT,
				test::ImageFormat::Rgba8Unorm
			);
			CHECK(test::CompareImages(clear.View(), clipped.View()).MismatchedPixels == 0);
			view.CameraFrame.Position = {};
			view.Camera.FarPlane = 32;
			view.Lighting.Ambient = {};
			view.Lighting.Direct = {};
			REQUIRE(fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false).Submitted);
			const auto night = test::CaptureResource(
				fixture.Render,
				core::Name("tonemapped"),
				view.Slot,
				EXTENT,
				EXTENT,
				test::ImageFormat::Rgba8Unorm
			);
			CHECK(test::CompareImages(cloudy.View(), night.View()).MismatchedPixels > 0);
			view.Lighting.Direct = {0, 1, 0};
			REQUIRE(fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false).Submitted);
			const auto green = test::CaptureResource(
				fixture.Render,
				core::Name("tonemapped"),
				view.Slot,
				EXTENT,
				EXTENT,
				test::ImageFormat::Rgba8Unorm
			);
			CHECK(test::CompareImages(night.View(), green.View()).MismatchedPixels > 0);
		}
	}
}
