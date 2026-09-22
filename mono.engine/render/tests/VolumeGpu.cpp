// Real-device proof that coincident local media are integrated as one field.

#include "RenderFixture.hpp"

#include <engine/assets/Mesh.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/scene/Sunlight.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <span>

TEST_SUITE_ID("engine.render.volumegpu")
TEST_DEPENDS("engine.graph.pipelinedocument")

namespace {

	using namespace engine;
	using namespace engine::render;
	using namespace engine::render::test;

	constexpr uint32_t EXTENT = 65;
	const core::Name PIPELINE("volume.gpu");

	assets::MeshData Receiver() {
		assets::MeshData mesh;
		mesh.Vertices = {
			{{-1.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},
			{{1.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
			{{1.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
			{{-1.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
		};
		mesh.Indices = {0, 1, 2, 0, 2, 3};
		mesh.ComputeBounds();
		return mesh;
	}

	void InstallCapturePipeline(Renderer &renderer) {
		graph::PipelineDocument document = graph::DefaultPbrDocument();
		const core::Name kind("volume-test-boundary");
		graph::NodeKindSpec boundary;
		boundary.Kind = kind;
		boundary.Scope = graph::NodeScope::Frame;
		boundary.Queue = graph::ExecutionQueue::Cpu;
		boundary.Category = graph::NodeCategory::Output;
		boundary.Inputs.push_back({.Name = core::Name("tonemapped"), .Kind = graph::ResourceKind::Texture});
		REQUIRE(graph::RegisterNodeKind(std::move(boundary)));
		REQUIRE(renderer.InstallNodeHandler(kind, [](const graph::RunContext &) { return true; }));
		document.Record({
			.Kind = graph::EditKind::AddNode,
			.Name = kind,
			.NodeKind = kind,
			.Scope = graph::NodeScope::Frame,
		});
		document.Record({
			.Kind = graph::EditKind::Reads,
			.Target = core::Name("tonemapped"),
			.Key = core::Name("tonemapped"),
		});
		graph::RenderGraph graph;
		core::Name offender;
		REQUIRE(graph::Build(document, graph, offender) == graph::PipelineDocumentStatus::Ok);
		REQUIRE(renderer.SetPipeline(PIPELINE, graph));
	}

	scene::VolumeState Volume(const core::Color3 &colour) {
		scene::VolumeState volume;
		volume.Frame.Position = {0.0f, 0.0f, -3.0f};
		volume.Colour = colour;
		volume.HalfExtent = {3.0f, 3.0f, 2.0f};
		volume.Density = 0.45f;
		volume.Extinction = 0.3f;
		volume.Falloff = 0.0f;
		volume.NoiseStrength = 0.0f;
		volume.Steps = 32;
		volume.ShadowSteps = 1;
		volume.Enabled = true;
		return volume;
	}

	CapturedImage RenderOrder(Renderer &renderer, View &view, bool reversed) {
		view.Lighting.VolumeCount = 2;
		if (!reversed) {
			view.Lighting.Volumes[0] = Volume({1.0f, 0.05f, 0.0f});
			view.Lighting.Volumes[1] = Volume({0.0f, 0.15f, 1.0f});
		} else {
			view.Lighting.Volumes[0] = Volume({0.0f, 0.15f, 1.0f});
			view.Lighting.Volumes[1] = Volume({1.0f, 0.05f, 0.0f});
		}
		OverlayImage overlay;
		REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("fog")));
		return CaptureResource(
			renderer, core::Name("tonemapped"), view.Slot, EXTENT, EXTENT, ImageFormat::Rgba8Unorm
		);
	}
}

TEST_CASE("overlapping local fog volumes are invariant to resolved order", "[render][gpu][volume][.]") {
	FixtureDevice fixture;
	fixture.Initialise();
	InstallCapturePipeline(fixture.Render);

	const core::Name receiver("volume.gpu.receiver");
	REQUIRE(fixture.Render.AddMesh(receiver, Receiver()));
	scene::DrawInstance instance;
	instance.Source = 1;
	instance.Mesh = receiver;
	instance.Frame.Position = {0.0f, 0.0f, -6.0f};
	instance.HalfExtent = {4.0f, 4.0f, 0.01f};
	instance.Tint = {0.65f, 0.65f, 0.65f};
	instance.CastShadow = false;

	SceneTarget target{EXTENT, EXTENT};
	View view;
	view.World = 221;
	view.WorldName = core::Name("volume.gpu.world");
	view.Pipeline = PIPELINE;
	view.Target = &target;
	view.Camera.FieldOfViewRadians = 1.5707963267948966f;
	view.Camera.NearPlane = 0.25f;
	view.Camera.FarPlane = 32.0f;
	view.Instances = std::span(&instance, 1);
	view.OverrideLighting = true;
	view.Lighting.Ambient = {1.0f, 1.0f, 1.0f};
	view.Lighting.OutdoorAmbient = {};
	view.Lighting.Direct = {};

	view.Lighting.VolumeCount = 0;
	OverlayImage overlay;
	REQUIRE(fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("fog")));
	const CapturedImage withoutVolumes = CaptureResource(
		fixture.Render, core::Name("tonemapped"), view.Slot, EXTENT, EXTENT, ImageFormat::Rgba8Unorm
	);
	const CapturedImage forward = RenderOrder(fixture.Render, view, false);
	const CapturedImage reverse = RenderOrder(fixture.Render, view, true);

	const ImageComparison fogEffect = CompareImages(withoutVolumes.View(), forward.View());
	ImageTolerance orderTolerance;
	orderTolerance.Absolute = 1.0 / 255.0;
	orderTolerance.MaximumRootMeanSquareError = 1.0 / 255.0;
	INFO("fog changed pixels=" << fogEffect.MismatchedPixels << ", max=" << fogEffect.MaximumAbsoluteError);
	CHECK(fogEffect.MismatchedPixels > size_t(EXTENT) * EXTENT / 4);
	CheckImage(
		fixture.Render,
		"volume-order",
		"tonemapped",
		"two coincident volumes with red and blue scattering swapped in resolved order",
		forward.View(),
		reverse.View(),
		orderTolerance
	);
}
