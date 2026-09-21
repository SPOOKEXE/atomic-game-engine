// Real-device proof that Studio's wireframe state reaches deferred mesh draws.

#include "RenderFixture.hpp"

#include <engine/assets/Mesh.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/render/EditableMeshes.hpp>
#include <engine/render/ShaderCompiler.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

TEST_SUITE_ID("engine.render.wireframegpu")
TEST_DEPENDS("engine.graph.pipelinedocument")
TEST_DEPENDS("engine.scene.editablemesh")

namespace {
	using namespace engine;
	using namespace engine::render::test;

	assets::MeshData Quad() {
		assets::MeshData mesh;
		mesh.Vertices = {
			{{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},
			{{0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
			{{0.5f, 0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
			{{-0.5f, 0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
		};
		mesh.Indices = {0, 1, 2, 0, 2, 3};
		mesh.ComputeBounds();
		return mesh;
	}

	void InstallCapturePipeline(render::Renderer &renderer) {
		graph::PipelineDocument document = graph::DefaultPbrDocument();
		const core::Name kind("wireframe-test-boundary");
		graph::NodeKindSpec boundary;
		boundary.Kind = kind;
		boundary.Scope = graph::NodeScope::Frame;
		boundary.Queue = graph::ExecutionQueue::Cpu;
		boundary.Category = graph::NodeCategory::Output;
		for (const char *resource : {"albedo", "composed-image"}) {
			boundary.Inputs.push_back({.Name = core::Name(resource), .Kind = graph::ResourceKind::Texture});
		}
		REQUIRE(graph::RegisterNodeKind(std::move(boundary)));
		REQUIRE(renderer.InstallNodeHandler(kind, [](const graph::RunContext &) { return true; }));
		document.Record({
			.Kind = graph::EditKind::AddNode,
			.Name = kind,
			.NodeKind = kind,
			.Scope = graph::NodeScope::Frame,
		});
		for (const char *resource : {"albedo", "composed-image"}) {
			document.Record({
				.Kind = graph::EditKind::Reads,
				.Target = core::Name(resource),
				.Key = core::Name(resource),
			});
		}
		graph::RenderGraph graph;
		core::Name offender;
		REQUIRE(graph::Build(document, graph, offender) == graph::PipelineDocumentStatus::Ok);
		REQUIRE(renderer.SetPipeline(core::Name("wireframe.gpu"), graph));
	}

	struct ColourCounts {
		size_t Red = 0;
		size_t Green = 0;
		size_t Blue = 0;
	};

	ColourCounts CountGeometry(const CapturedImage &image) {
		ColourCounts counts;
		for (uint32_t y = 0; y < image.Height; ++y) {
			for (uint32_t x = 0; x < image.Width; ++x) {
				const size_t offset = y * image.RowStrideBytes + x * 4;
				const auto *pixel = reinterpret_cast<const uint8_t *>(image.Bytes.data() + offset);
				counts.Red += pixel[0] > 150 && pixel[1] < 80;
				counts.Green += pixel[1] > 150 && pixel[0] < 80;
				counts.Blue += pixel[2] > 150 && pixel[0] < 80 && pixel[1] < 80;
			}
		}
		return counts;
	}
}

TEST_CASE(
	"wireframe rasterizes ordinary, authored-shader MeshPart and packed EditableMesh edges in the PBR graph",
	"[render][gpu][wireframe][.]"
) {
	using namespace engine;
	using namespace engine::render::test;

	scene::RegisterSceneClasses();
	FixtureDevice fixture;
	fixture.Initialise();
	InstallCapturePipeline(fixture.Render);

	const core::Name meshPartMesh("wireframe.meshpart");
	REQUIRE(fixture.Render.AddMesh(meshPartMesh, Quad()));

	ecs::Store store("wireframe.editable");
	const ecs::Entity editable = store.CreateInstance(scene::EditableMeshClass(), "editable");
	for (const assets::MeshVertex &vertex : Quad().Vertices) {
		REQUIRE(
			scene::AddVertex(
				store,
				editable,
				{vertex.Position[0], vertex.Position[1], vertex.Position[2]},
				{vertex.Normal[0], vertex.Normal[1], vertex.Normal[2]},
				{vertex.TexCoord[0], vertex.TexCoord[1]}
			)
		);
	}
	REQUIRE(scene::AddTriangle(store, editable, 0, 1, 2));
	REQUIRE(scene::AddTriangle(store, editable, 0, 2, 3));
	scene::EditablePacking packing;
	packing.Attributes = static_cast<uint8_t>(scene::EditablePackingAttribute::Position);
	packing.Format = scene::EditablePackingFormat::Unsigned16;
	REQUIRE(scene::SetEditableMeshPacking(store, editable, packing));
	render::EditableMeshUploader uploader;
	REQUIRE(uploader.Refresh(store, fixture.Render) == 1);

	render::ShaderCompiler compiler;
	const auto program = compiler.Compile(
		"#version 450\nlayout(location=0) out vec4 colour; void main(){colour=vec4(0,0,1,1);}",
		render::ShaderStage::Fragment,
		"wireframe-authored.frag"
	);
	INFO(program.Error);
	REQUIRE_FALSE(program.Failed);
	const core::Name authoredShader("wireframe.authored");
	REQUIRE(fixture.Render.AddShader(authoredShader, program.SpirV));

	std::array<scene::DrawInstance, 3> instances;
	instances[0].Source = 1;
	instances[0].Frame.Position = {-1.4f, 0.0f, -4.0f};
	instances[0].HalfExtent = {0.7f, 0.7f, 0.01f};
	instances[0].Tint = {1.0f, 0.0f, 0.0f};
	instances[0].Mesh = meshPartMesh;
	instances[1].Source = 2;
	instances[1].Frame.Position = {0.0f, 0.0f, -4.0f};
	instances[1].HalfExtent = {0.7f, 0.7f, 0.01f};
	instances[1].Tint = {0.0f, 1.0f, 0.0f};
	instances[1].Mesh = scene::EditableMeshContentName(store, editable);
	instances[2].Source = 3;
	instances[2].Frame.Position = {1.4f, 0.0f, -4.0f};
	instances[2].HalfExtent = {0.7f, 0.7f, 0.01f};
	instances[2].Tint = {0.0f, 0.0f, 1.0f};
	instances[2].Mesh = meshPartMesh;
	instances[2].Shader = authoredShader;

	render::SceneTarget target{193, 97};
	render::View view;
	view.World = 782;
	view.WorldName = core::Name("wireframe.gpu.world");
	view.Pipeline = core::Name("wireframe.gpu");
	view.Target = &target;
	view.Camera.FieldOfViewRadians = 1.5707963267948966f;
	view.Camera.NearPlane = 0.25f;
	view.Camera.FarPlane = 32.0f;
	view.Instances = instances;
	render::OverlayImage overlay;

	fixture.Render.SetWireframe(false);
	fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	const ColourCounts filled = CountGeometry(CaptureResource(
		fixture.Render, core::Name("albedo"), view.Slot, target.Width, target.Height, ImageFormat::Rgba8Unorm
	));
	const ColourCounts filledComposed = CountGeometry(CaptureResource(
		fixture.Render,
		core::Name("composed-image"),
		view.Slot,
		target.Width,
		target.Height,
		ImageFormat::Rgba8Unorm
	));

	fixture.Render.SetWireframe(true);
	view.Damage.Objects = true;
	fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	const ColourCounts wireframe = CountGeometry(CaptureResource(
		fixture.Render, core::Name("albedo"), view.Slot, target.Width, target.Height, ImageFormat::Rgba8Unorm
	));
	const ColourCounts wireframeComposed = CountGeometry(CaptureResource(
		fixture.Render,
		core::Name("composed-image"),
		view.Slot,
		target.Width,
		target.Height,
		ImageFormat::Rgba8Unorm
	));

	INFO(
		"filled red=" << filled.Red << " green=" << filled.Green << ", wireframe red=" << wireframe.Red
					  << " green=" << wireframe.Green << ", authored filled blue=" << filledComposed.Blue
					  << " wireframe blue=" << wireframeComposed.Blue
	);
	CHECK(wireframe.Red > 10);
	CHECK(wireframe.Green > 10);
	CHECK(filled.Red > wireframe.Red * 2);
	CHECK(filled.Green > wireframe.Green * 2);
	CHECK(wireframeComposed.Blue > 10);
	CHECK(filledComposed.Blue > wireframeComposed.Blue * 2);
}
