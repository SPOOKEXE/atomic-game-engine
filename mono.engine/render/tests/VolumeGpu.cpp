// Real-device proof that coincident local media are integrated as one field.

#include "RenderFixture.hpp"

#include <engine/assets/Mesh.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scene/CloudDensity.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/scene/Sunlight.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
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

	void InstallCapturePipeline(Renderer &renderer, core::Name pipeline, core::Name kind) {
		graph::PipelineDocument document = graph::DefaultPbrDocument();
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
		REQUIRE(renderer.SetPipeline(pipeline, graph));
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

	void SetVolumeOrder(View &view, bool reversed) {
		const std::array colours{
			core::Color3{1.0f, 0.05f, 0.0f},
			core::Color3{0.0f, 0.15f, 1.0f},
			core::Color3{0.1f, 1.0f, 0.0f},
			core::Color3{1.0f, 0.7f, 0.0f},
			core::Color3{0.8f, 0.0f, 0.8f},
			core::Color3{0.0f, 0.9f, 0.8f},
			core::Color3{0.9f, 0.4f, 0.1f},
			core::Color3{0.3f, 0.4f, 1.0f},
			core::Color3{0.65f, 0.25f, 0.12f},
			core::Color3{0.12f, 0.55f, 0.34f},
			core::Color3{0.88f, 0.22f, 0.51f},
			core::Color3{0.31f, 0.73f, 0.25f},
			core::Color3{0.41f, 0.36f, 0.93f},
			core::Color3{0.91f, 0.75f, 0.18f},
			core::Color3{0.17f, 0.76f, 0.71f},
			core::Color3{0.72f, 0.47f, 0.29f},
		};
		view.Lighting.VolumeCount = colours.size();
		for (size_t index = 0; index < colours.size(); index++) {
			const size_t source = reversed ? colours.size() - index - 1 : index;
			view.Lighting.Volumes[index] = Volume(colours[source]);
			if (source >= 2) {
				view.Lighting.Volumes[index].Frame.Position.X = -1.5f + float(source - 2) * 0.22f;
				view.Lighting.Volumes[index].HalfExtent = {0.55f, 3.0f, 2.0f};
			}
		}
	}

	// A deliberately saturated environment makes every producer visible in the
	// final LDR capture: generated sky and scattering feed clouds, clouds feed
	// sky, local media shades the receiver, and the projected sun feeds shafts.
	void ConfigureCombinedLighting(View &view) {
		scene::WorldLighting &lighting = view.Lighting;
		lighting.Direction = {0.0f, 0.0f, 1.0f};
		lighting.FogStart = 1'000.0f;
		lighting.FogEnd = 1'001.0f;
		lighting.GodRayIntensity = 1.5f;
		lighting.GodRayThreshold = 0.15f;
		lighting.GodRayRadius = 40.0f;

		scene::Environment &environment = lighting.EnvironmentState;
		environment.Skybox = scene::SkyboxSource::Compute;
		environment.SkyCompute.Zenith = {0.03f, 0.08f, 0.28f};
		environment.SkyCompute.Horizon = {1.0f, 0.38f, 0.05f};
		environment.SkyCompute.Ground = {0.01f, 0.015f, 0.025f};
		environment.SkyCompute.SunSize = 0.18f;
		environment.SkyCompute.Seed = 67;
		environment.SkyCompute.Shader = scene::SkyboxComputeShader::Sunset;
		environment.SkyCompute.Enabled = true;
		environment.HasAtmosphere = true;
		environment.HasAtmosphereCompute = true;
		environment.Air.Colour = {0.82f, 0.49f, 0.27f};
		environment.Air.Decay = {0.18f, 0.09f, 0.04f};
		environment.Air.Density = 0.72f;
		environment.Air.Glare = 3.0f;
		environment.Air.Haze = 4.0f;
		environment.AirCompute.Rayleigh = 1.3f;
		environment.AirCompute.Mie = 0.8f;
		environment.AirCompute.Samples = 24;
		environment.AirCompute.Enabled = true;
		environment.HasClouds = true;
		environment.HasCloudCompute = true;
		environment.CloudLayer.Colour = {1.0f, 0.72f, 0.46f};
		environment.CloudLayer.Cover = 0.36f;
		environment.CloudLayer.Density = 0.92f;
		environment.CloudLayer.WindSpeed = 8.0f;
		environment.CloudLayer.WindDirection = {0.6f, 0.8f};
		environment.CloudLayer.Enabled = true;
		environment.CloudVolume.CellSize = 0.14f;
		environment.CloudVolume.Detail = 0.8f;
		environment.CloudVolume.Height = 0.25f;
		environment.CloudVolume.Thickness = 0.2f;
		environment.CloudVolume.Seed = 91;
		environment.CloudVolume.Steps = 32;
		environment.CloudVolume.Shader = scene::CloudComputeShader::Storm;
		environment.CloudVolume.Enabled = true;
		environment.CloudTime = 0.75;
	}

	CapturedImage RenderOrder(Renderer &renderer, View &view, bool reversed) {
		SetVolumeOrder(view, reversed);
		OverlayImage overlay;
		REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("fog")));
		return CaptureResource(
			renderer, core::Name("tonemapped"), view.Slot, EXTENT, EXTENT, ImageFormat::Rgba8Unorm
		);
	}
}

TEST_CASE(
	"overlapping local fog volumes are invariant to resolved order", "[render][gpu][volume][volume-stress][.]"
) {
	FixtureDevice fixture;
	fixture.Initialise();
	InstallCapturePipeline(fixture.Render, PIPELINE, core::Name("volume-test-boundary"));

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
	scene::CloudDensityGpuNode cloudNode;
	cloudNode.ChildrenLow.fill(UINT32_MAX);
	cloudNode.ChildrenHigh.fill(UINT32_MAX);
	cloudNode.Values[0] = 1.0f;
	view.CloudDensity = scene::CloudDensitySnapshot{
		.Centre = {},
		.Config = {.RootMinimum = {-3.0f, -3.0f, -7.0f}, .RootSize = {6.0f, 6.0f, 6.0f}, .MaximumDepth = 1},
		.Nodes = {cloudNode},
	};
	REQUIRE(fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("fog")));
	const CapturedImage sparseCloud = CaptureResource(
		fixture.Render, core::Name("tonemapped"), view.Slot, EXTENT, EXTENT, ImageFormat::Rgba8Unorm
	);
	const ImageComparison cloudEffect = CompareImages(withoutVolumes.View(), sparseCloud.View());
	INFO(
		"cloud changed pixels=" << cloudEffect.MismatchedPixels
								<< ", max=" << cloudEffect.MaximumAbsoluteError
	);
	CHECK(cloudEffect.MismatchedPixels > size_t(EXTENT) * EXTENT / 4);
	view.CloudDensity.reset();
	const CapturedImage forward = RenderOrder(fixture.Render, view, false);
	const CapturedImage reverse = RenderOrder(fixture.Render, view, true);
	SetVolumeOrder(view, false);
	view.Lighting.VolumeCount = 8;
	REQUIRE(fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("fog")));
	const CapturedImage firstEight = CaptureResource(
		fixture.Render, core::Name("tonemapped"), view.Slot, EXTENT, EXTENT, ImageFormat::Rgba8Unorm
	);
	view.Lighting.VolumeCount = 10;
	REQUIRE(fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("fog")));
	const CapturedImage firstTen = CaptureResource(
		fixture.Render, core::Name("tonemapped"), view.Slot, EXTENT, EXTENT, ImageFormat::Rgba8Unorm
	);
	const ImageComparison ninthAndTenth = CompareImages(firstEight.View(), firstTen.View());
	CHECK(ninthAndTenth.MismatchedPixels > 0);

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
		"two coincident and fourteen overlapping displaced volumes reversed in resolved order",
		forward.View(),
		reverse.View(),
		orderTolerance
	);

	const char *configuredFrames = std::getenv("MONO_VOLUME_STRESS_FRAMES");
	if (configuredFrames == nullptr) return;
	const unsigned long requested = std::strtoul(configuredFrames, nullptr, 10);
	REQUIRE(requested > 0);
	const uint32_t frames = static_cast<uint32_t>(std::min(requested, 10'000ul));
	for (uint32_t frame = 0; frame < 8; frame++) {
		SetVolumeOrder(view, (frame & 1u) != 0);
		REQUIRE(fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false).Submitted);
	}
	auto *device = static_cast<SDL_GPUDevice *>(fixture.Render.Backend().Device);
	REQUIRE(device != nullptr);
	REQUIRE(SDL_WaitForGPUIdle(device));
	const auto started = std::chrono::steady_clock::now();
	for (uint32_t frame = 0; frame < frames; frame++) {
		SetVolumeOrder(view, (frame & 1u) != 0);
		REQUIRE(fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false).Submitted);
	}
	REQUIRE(SDL_WaitForGPUIdle(device));
	const double milliseconds =
		std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
	std::cout << "volume stress frames=" << frames
			  << ", end-to-end ms/frame=" << milliseconds / double(frames) << '\n';
}

TEST_CASE(
	"combined atmosphere clouds local fog and god rays change one Vulkan capture",
	"[render][gpu][volume][lighting]"
) {
	FixtureDevice fixture;
	fixture.Initialise();
	if (!fixture.Render.Capabilities().HasCompute) SKIP("the selected GPU has no compute support");
	const core::Name pipeline("volume.gpu.lighting");
	InstallCapturePipeline(fixture.Render, pipeline, core::Name("volume-lighting-test-boundary"));

	const core::Name receiver("volume.gpu.lighting.receiver");
	REQUIRE(fixture.Render.AddMesh(receiver, Receiver()));
	scene::DrawInstance instance;
	instance.Source = 1;
	instance.Mesh = receiver;
	instance.Frame.Position = {0.0f, 0.0f, -6.0f};
	// Leave a broad sky border. The first version filled the 90-degree view with
	// this receiver, which reduced the generated environment to a few edge
	// pixels and made its cloud and shaft captures meaningless.
	instance.HalfExtent = {2.0f, 2.0f, 0.01f};
	instance.Tint = {0.65f, 0.65f, 0.65f};
	instance.CastShadow = false;

	SceneTarget target{EXTENT, EXTENT};
	View view;
	view.World = 222;
	view.WorldName = core::Name("volume.gpu.lighting.world");
	view.Pipeline = pipeline;
	view.Target = &target;
	view.Camera.FieldOfViewRadians = 1.5707963267948966f;
	view.Camera.NearPlane = 0.25f;
	view.Camera.FarPlane = 32.0f;
	view.Instances = std::span(&instance, 1);
	view.OverrideLighting = true;
	view.Lighting.Ambient = {1.0f, 1.0f, 1.0f};
	view.Lighting.OutdoorAmbient = {};
	view.Lighting.Direct = {};
	SetVolumeOrder(view, false);
	ConfigureCombinedLighting(view);
	OverlayImage overlay;

	const FrameResult combinedFrame = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	// Compute handlers do not open a render pass, so `FrameResult::Ran` has no
	// entry for them. Their two real environment dispatches are the execution
	// evidence, while the captured images prove their pixels reached the view.
	CHECK(combinedFrame.ComputeDispatches >= 2);
	CHECK(combinedFrame.Ran(core::Name("fog")));
	CHECK(combinedFrame.Ran(core::Name("god-rays")));
	const CapturedImage combined = CaptureResource(
		fixture.Render, core::Name("tonemapped"), view.Slot, EXTENT, EXTENT, ImageFormat::Rgba8Unorm
	);

	view.Lighting.VolumeCount = 0;
	fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	const CapturedImage withoutVolumes = CaptureResource(
		fixture.Render, core::Name("tonemapped"), view.Slot, EXTENT, EXTENT, ImageFormat::Rgba8Unorm
	);
	CHECK(CompareImages(combined.View(), withoutVolumes.View()).MismatchedPixels > EXTENT * EXTENT / 8);

	SetVolumeOrder(view, false);
	view.Lighting.GodRayIntensity = 0.0f;
	fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	const CapturedImage withoutGodRays = CaptureResource(
		fixture.Render, core::Name("tonemapped"), view.Slot, EXTENT, EXTENT, ImageFormat::Rgba8Unorm
	);
	CHECK(CompareImages(combined.View(), withoutGodRays.View()).MismatchedPixels > EXTENT / 2);

	view.Lighting.GodRayIntensity = 1.5f;
	view.Lighting.EnvironmentState.HasCloudCompute = false;
	fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	const CapturedImage withoutCloudCompute = CaptureResource(
		fixture.Render, core::Name("tonemapped"), view.Slot, EXTENT, EXTENT, ImageFormat::Rgba8Unorm
	);
	CHECK(CompareImages(combined.View(), withoutCloudCompute.View()).MismatchedPixels > EXTENT * EXTENT / 8);

	view.Lighting.EnvironmentState.HasCloudCompute = true;
	view.Lighting.EnvironmentState.HasAtmosphere = false;
	view.Lighting.EnvironmentState.HasAtmosphereCompute = false;
	fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	const CapturedImage withoutAtmosphere = CaptureResource(
		fixture.Render, core::Name("tonemapped"), view.Slot, EXTENT, EXTENT, ImageFormat::Rgba8Unorm
	);
	CHECK(CompareImages(combined.View(), withoutAtmosphere.View()).MismatchedPixels > EXTENT * EXTENT / 8);
}
