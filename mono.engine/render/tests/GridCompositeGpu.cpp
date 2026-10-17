// Real-device parity between the old copy plus depth-tested grid and the
// grid-only fullscreen composite. Odd extents exercise rounded scene targets.

#include "RenderFixture.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>
#include <span>
#include <string>

TEST_SUITE_ID("engine.render.gridcompositegpu")
TEST_DEPENDS("engine.graph.pipelinedocument")

namespace {
	using namespace engine;
	using namespace engine::render::test;

	// A prepared but empty spatial hook selects the existing transparent path
	// without adding any pixels. No production switch exists just for a test.
	class EmptySpatialHook final : public render::FrameOverlayHook {
	  public:
		bool Prepare(void *) override {
			return true;
		}
		bool AffectsScene() const override {
			return true;
		}
		void Record(void *, void *) override {}
	};

	double Counter(const char *name) {
		const core::Name key(name);
		for (const auto &counter : core::Metrics::Snapshot().Counters)
			if (counter.Name == key) return counter.Value;
		return 0;
	}

	void Install(render::Renderer &renderer) {
		auto document = graph::DefaultPbrDocument();
		const core::Name kind("grid-composite-test-boundary");
		graph::NodeKindSpec boundary;
		boundary.Kind = kind;
		boundary.Scope = graph::NodeScope::Frame;
		boundary.Queue = graph::ExecutionQueue::Cpu;
		boundary.Category = graph::NodeCategory::Output;
		for (const char *resource : {"tonemapped", "depth"})
			boundary.Inputs.push_back({.Name = core::Name(resource), .Kind = graph::ResourceKind::Texture});
		REQUIRE(graph::RegisterNodeKind(std::move(boundary)));
		REQUIRE(renderer.InstallNodeHandler(kind, [](const graph::RunContext &) { return true; }));
		document.Record(
			{.Kind = graph::EditKind::AddNode,
			 .Name = kind,
			 .NodeKind = kind,
			 .Scope = graph::NodeScope::Frame}
		);
		for (const char *resource : {"tonemapped", "depth"})
			document.Record(
				{.Kind = graph::EditKind::Reads, .Target = core::Name(resource), .Key = core::Name(resource)}
			);
		graph::RenderGraph pipeline;
		core::Name offender;
		REQUIRE(graph::Build(document, pipeline, offender) == graph::PipelineDocumentStatus::Ok);
		REQUIRE(renderer.SetPipeline(core::Name("grid-composite-test"), pipeline));
	}

	struct Capture {
		CapturedImage Colour;
		CapturedImage Depth;
		double CompositePasses = 0;
		double RasterPasses = 0;
		double CopyBytes = 0;
	};
}

TEST_CASE(
	"grid composite preserves grid coverage opaque depth and source pixels",
	"[render][gpu][grid-composite][.]"
) {
	using namespace engine;
	using namespace engine::render::test;
	FixtureDevice fixture;
	fixture.Initialise();
	Install(fixture.Render);
	const render::SceneTarget target{193, 117};
	render::View view;
	view.World = 319;
	view.WorldName = core::Name("grid-composite-world");
	view.Pipeline = core::Name("grid-composite-test");
	view.Target = &target;
	view.Camera.NearPlane = .1f;
	view.Camera.FarPlane = 128;
	view.Camera.FieldOfViewRadians = 1.0471975512f;
	view.Grid.Enabled = true;
	view.Grid.Step = 1;
	view.Grid.Major = 4;
	view.Grid.Reach = 64;
	view.Grid.Offset = {.3f, 0, .4f};
	view.Grid.Colour = {1, .2f, .6f};
	view.Grid.Alpha = .7f;
	view.Grid.AxisAlpha = .8f;
	view.OverrideLighting = true;
	view.Lighting.Ambient = {.2f, .2f, .2f};
	view.Lighting.OutdoorAmbient = view.Lighting.Ambient;
	view.Lighting.Direct = {};
	render::OverlayImage overlay;
	EmptySpatialHook referenceHook;
	const auto capture = [&](bool reference) {
		const double composites = Counter("render.grid_composite.commands");
		const double rasters = Counter("render.transparent.raster_passes");
		const double copies = Counter("render.identity_copy.bytes");
		view.Damage.Scene = true;
		fixture.Render.Render(std::span(&view, 1), overlay, reference ? &referenceHook : nullptr, false);
		Capture result;
		result.CompositePasses = Counter("render.grid_composite.commands") - composites;
		result.RasterPasses = Counter("render.transparent.raster_passes") - rasters;
		result.CopyBytes = Counter("render.identity_copy.bytes") - copies;
		result.Colour = CaptureResource(
			fixture.Render, core::Name("tonemapped"), 0, target.Width, target.Height, ImageFormat::Rgba8Unorm
		);
		result.Depth = CaptureResource(
			fixture.Render, core::Name("depth"), 0, target.Width, target.Height, ImageFormat::R32Float
		);
		return result;
	};
	std::array<scene::DrawInstance, 1> occluder;
	occluder[0].Source = 1;
	occluder[0].Frame.Position = {0, 1, 0};
	occluder[0].HalfExtent = {1.3f, 1, .3f};
	occluder[0].Tint = {.1f, .8f, .2f};
	for (const char *scenario : {"visible", "horizon", "oblique-occluder", "high-alpha", "zero-strength"}) {
		INFO(scenario);
		view.CameraFrame = core::CFrame::LookAt({4, 3, 6}, {0, 0, 0});
		view.Projection.reset();
		view.Instances = {};
		view.Grid.Strength = 1;
		view.Grid.Colour = {1, .2f, .6f};
		view.Grid.Alpha = .7f;
		view.Grid.AxisAlpha = .8f;
		if (std::string_view(scenario) == "high-alpha") {
			view.Grid.Colour = {1.4f, .2f, .6f};
			view.Grid.Strength = 2;
			view.Grid.Alpha = 2;
			view.Grid.AxisAlpha = 2;
		}
		if (std::string_view(scenario) == "horizon")
			view.CameraFrame = core::CFrame::LookAt({0, 2, 6}, {0, 2, 0});
		if (std::string_view(scenario) == "oblique-occluder") {
			view.Instances = occluder;
			const auto matrices = scene::ResolveCamera(
				view.CameraFrame, view.Camera, static_cast<float>(target.Width) / target.Height
			);
			// Clip in front of the wall, retaining the wall and ground behind it.
			view.Projection = scene::ObliqueProjection(matrices.Projection, view.CameraFrame, {0, 0, -1}, -2);
		}
		if (std::string_view(scenario) == "zero-strength") view.Grid.Strength = 0;
		const auto reference = capture(true);
		const auto composite = capture(false);
		if (std::string_view(scenario) == "visible") {
			view.Grid.Strength = 0;
			const auto withoutGrid = capture(false);
			CHECK(reference.Colour.Bytes != withoutGrid.Colour.Bytes);
			view.Grid.Strength = 1;
		}
		if (std::string_view(scenario) == "oblique-occluder") {
			size_t opaquePixels = 0;
			for (uint32_t y = 0; y < target.Height; ++y) {
				for (uint32_t x = 0; x < target.Width; ++x) {
					float depth;
					std::memcpy(
						&depth,
						composite.Depth.Bytes.data() + y * composite.Depth.RowStrideBytes + x * 4,
						sizeof(depth)
					);
					opaquePixels += depth < 1;
				}
			}
			CHECK(opaquePixels > 100);
		}
		ImageTolerance colourTolerance;
		// Normalized byte subtraction can exceed one byte by floating-point rounding.
		colourTolerance.Absolute = 1.0 / 255.0 + std::numeric_limits<double>::epsilon();
		CheckImage(
			fixture.Render,
			"grid-composite",
			scenario,
			"copy + grid versus fused grid",
			reference.Colour.View(),
			composite.Colour.View(),
			colourTolerance
		);
		CheckImage(
			fixture.Render,
			"grid-composite",
			"opaque-depth",
			scenario,
			reference.Depth.View(),
			composite.Depth.View()
		);
		CHECK(reference.CompositePasses == 0);
		if (view.Grid.Strength != 0) {
			CHECK(reference.RasterPasses == 1);
			CHECK(composite.RasterPasses == 1);
			CHECK(composite.CompositePasses == 1);
			CHECK(reference.CopyBytes - composite.CopyBytes >= double(target.Width) * target.Height * 8);
		} else {
			CHECK(composite.CompositePasses == 0);
			CHECK(composite.RasterPasses == 0);
		}
	}
}
