// Real-device proof that authored mesh LOD choice stays on the GPU.

#include "RenderFixture.hpp"

#include <engine/assets/Mesh.hpp>
#include <engine/core/Name.hpp>
#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <span>

TEST_SUITE_ID("engine.render.lodgpu")
TEST_DEPENDS("engine.graph.pipelinedocument")

namespace {
	using namespace engine;
	using namespace engine::render::test;

	assets::MeshData Quad() {
		assets::MeshData mesh;
		mesh.Vertices = {
			{{-0.5f, -0.5f, 0}, {0, 0, 1}, {0, 1}},
			{{0.5f, -0.5f, 0}, {0, 0, 1}, {1, 1}},
			{{0.5f, 0.5f, 0}, {0, 0, 1}, {1, 0}},
			{{-0.5f, 0.5f, 0}, {0, 0, 1}, {0, 0}},
		};
		mesh.Indices = {0, 1, 2, 0, 2, 3};
		mesh.ComputeBounds();
		return mesh;
	}

	assets::MeshData Triangle() {
		assets::MeshData mesh;
		mesh.Vertices = {
			{{-0.5f, -0.5f, 0}, {0, 0, 1}, {0, 1}},
			{{0.5f, -0.5f, 0}, {0, 0, 1}, {1, 1}},
			{{-0.5f, 0.5f, 0}, {0, 0, 1}, {0, 0}},
		};
		mesh.Indices = {0, 1, 2};
		mesh.ComputeBounds();
		return mesh;
	}

	graph::PipelineDocument KeepAlbedo(render::Renderer &renderer) {
		graph::PipelineDocument document = graph::DefaultPbrDocument();
		const core::Name kind("lod-test-boundary");
		graph::NodeKindSpec boundary;
		boundary.Kind = kind;
		boundary.Scope = graph::NodeScope::Frame;
		boundary.Queue = graph::ExecutionQueue::Cpu;
		boundary.Category = graph::NodeCategory::Output;
		boundary.Inputs.push_back({.Name = core::Name("albedo"), .Kind = graph::ResourceKind::Texture});
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
			.Target = core::Name("albedo"),
			.Key = core::Name("albedo"),
		});
		graph::RenderGraph graph;
		core::Name offender;
		REQUIRE(graph::Build(document, graph, offender) == graph::PipelineDocumentStatus::Ok);
		REQUIRE(renderer.SetPipeline(core::Name("lod.gpu"), graph));
		return document;
	}
}

TEST_CASE("gpu projected area selects an authored mesh level", "[render][gpu][lod][.]") {
	using namespace engine;
	using namespace engine::render::test;

	FixtureDevice fixture;
	fixture.Initialise();
	KeepAlbedo(fixture.Render);

	const std::array names = {
		core::Name("lod.quad"),
		core::Name("lod.triangle.1"),
		core::Name("lod.triangle.2"),
		core::Name("lod.triangle.3"),
	};
	REQUIRE(fixture.Render.AddMesh(names[0], Quad()));
	for (size_t level = 1; level < names.size(); ++level) {
		REQUIRE(fixture.Render.AddMesh(names[level], Triangle()));
	}

	scene::DrawInstance instance;
	instance.Source = 1;
	instance.Frame.Position = {0.0f, 0.0f, -4.0f};
	instance.HalfExtent = {1.0f, 1.0f, 0.01f};
	instance.Tint = {1.0f, 0.0f, 0.0f};
	instance.Mesh = names[0];
	instance.LodStrategyMode = scene::LodStrategy::Authored;
	instance.LodLevels = 4;
	for (size_t level = 1; level < names.size(); ++level) {
		instance.LodMeshes[level - 1] = names[level];
	}

	render::SceneTarget target{65, 65};
	render::View view;
	view.World = 944;
	view.WorldName = core::Name("lod.gpu.world");
	view.Pipeline = core::Name("lod.gpu");
	view.Target = &target;
	view.Camera.FieldOfViewRadians = 1.5707963267948966f;
	view.Camera.NearPlane = 0.25f;
	view.Camera.FarPlane = 32.0f;
	view.Instances = std::span(&instance, 1);
	render::OverlayImage overlay;

	instance.LodTargetQuadArea = 1.0f;
	const auto detailedFrame = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(detailedFrame.Ran(core::Name("select-lod")));
	const CapturedImage detailed = CaptureResource(
		fixture.Render, core::Name("albedo"), view.Slot, target.Width, target.Height, ImageFormat::Rgba8Unorm
	);

	instance.LodTargetQuadArea = 1'000'000.0f;
	view.Damage.Objects = true;
	const auto coarseFrame = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(coarseFrame.Ran(core::Name("select-lod")));
	const CapturedImage coarse = CaptureResource(
		fixture.Render, core::Name("albedo"), view.Slot, target.Width, target.Height, ImageFormat::Rgba8Unorm
	);

	const auto redPixels = [](const CapturedImage &image) {
		size_t count = 0;
		for (uint32_t y = 0; y < image.Height; ++y) {
			for (uint32_t x = 0; x < image.Width; ++x) {
				const size_t offset = y * image.RowStrideBytes + x * 4;
				const auto *pixel = reinterpret_cast<const uint8_t *>(image.Bytes.data() + offset);
				count += pixel[0] > 200 && pixel[1] < 30 && pixel[2] < 30;
			}
		}
		return count;
	};
	const size_t detailedPixels = redPixels(detailed);
	const size_t coarsePixels = redPixels(coarse);
	INFO("detailed red pixels: " << detailedPixels << ", coarse red pixels: " << coarsePixels);
	CHECK(detailedPixels > coarsePixels + 100);
}
