// Real-device regression for the specialised tessellation handler.

#include "RenderFixture.hpp"

#include <engine/assets/Mesh.hpp>
#include <engine/core/Name.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/graph/RenderGraph.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
#include <glm/gtc/packing.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.render.hardrendergpu")
TEST_DEPENDS("engine.graph.pipelinedocument")

namespace {
	using namespace engine;
	using namespace engine::render::test;

	assets::MeshData Triangle() {
		assets::MeshData mesh;
		mesh.Vertices = {
			{{-1, -1, 0}, {0, 0, 1}, {0, 1}},
			{{1, -1, 0}, {0, 0, 1}, {1, 1}},
			{{0, 1, 0}, {0, 0, 1}, {0.5f, 0}}
		};
		mesh.Indices = {0, 1, 2};
		mesh.ComputeBounds();
		return mesh;
	}

	std::string PreviewShader() {
		return R"glsl(#version 450
layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;
layout(set = 0, binding = 0) uniform sampler2D source;
layout(set = 3, binding = 0, std140) uniform Pass {
	mat4 ViewProjection;
	mat4 InverseViewProjection;
	vec4 Target;
	vec4 View;
	uvec4 RenderFeatures;
} pass;
void main() { outColour = texture(source, inUv); }
)glsl";
	}

	std::string SolidShader(std::string_view colour) {
		return std::string(R"glsl(#version 450
layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;
layout(set = 3, binding = 0, std140) uniform Pass {
	mat4 ViewProjection; mat4 InverseViewProjection; vec4 Target; vec4 View; uvec4 RenderFeatures;
} pass;
)glsl") +
			   "void main() { outColour = " + std::string(colour) + "; }\n";
	}

	graph::ResourceId Texture(
		graph::RenderGraph &graph,
		const char *name,
		graph::ResourceFormat format = graph::ResourceFormat::RGBA8
	) {
		const auto id = graph.AddResource(
			{.Name = core::Name(name), .Kind = graph::ResourceKind::Colour, .Format = format}
		);
		REQUIRE(id.IsValid());
		return id;
	}

	void Raster(
		graph::RenderGraph &graph,
		const char *name,
		graph::ResourceId output,
		std::string shader,
		std::initializer_list<graph::ResourceId> reads = {}
	) {
		graph::Node node;
		node.Name = core::Name(name);
		node.Kind = core::Name("raster");
		node.Scope = graph::NodeScope::View;
		node.Reads.assign(reads);
		node.Writes = {output};
		node.Parameters.push_back({core::Name("source"), std::move(shader)});
		REQUIRE(graph.AddNode(std::move(node)).IsValid());
	}

	void Trace(
		graph::RenderGraph &graph,
		const char *name,
		const char *kind,
		std::initializer_list<graph::ResourceId> reads,
		graph::ResourceId output
	) {
		graph::Node node;
		node.Name = core::Name(name);
		node.Kind = core::Name(kind);
		node.Scope = graph::NodeScope::View;
		node.Reads.assign(reads);
		node.Writes = {output};
		REQUIRE(graph.AddNode(std::move(node)).IsValid());
	}

	std::vector<std::byte> CaptureRgba16(
		render::Renderer &renderer, core::Name resource, size_t slot, uint32_t width, uint32_t height
	) {
		auto *device = static_cast<SDL_GPUDevice *>(renderer.Backend().Device);
		auto *texture = static_cast<SDL_GPUTexture *>(renderer.ResourceTexture(resource, slot));
		REQUIRE(device != nullptr);
		REQUIRE(texture != nullptr);
		const size_t row = (size_t(width) * 8 + 255) / 256 * 256;
		SDL_GPUTransferBufferCreateInfo info{};
		info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
		info.size = uint32_t(row * height);
		auto *transfer = SDL_CreateGPUTransferBuffer(device, &info);
		REQUIRE(transfer != nullptr);
		auto *command = SDL_AcquireGPUCommandBuffer(device);
		REQUIRE(command != nullptr);
		auto *copy = SDL_BeginGPUCopyPass(command);
		REQUIRE(copy != nullptr);
		SDL_GPUTextureRegion source{};
		source.texture = texture;
		source.w = width;
		source.h = height;
		source.d = 1;
		SDL_GPUTextureTransferInfo destination{};
		destination.transfer_buffer = transfer;
		destination.pixels_per_row = uint32_t(row / 8);
		destination.rows_per_layer = height;
		SDL_DownloadFromGPUTexture(copy, &source, &destination);
		SDL_EndGPUCopyPass(copy);
		auto *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
		REQUIRE(fence != nullptr);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!SDL_QueryGPUFence(device, fence) && std::chrono::steady_clock::now() < deadline)
			SDL_Delay(1);
		REQUIRE(SDL_QueryGPUFence(device, fence));
		std::vector<std::byte> bytes(row * height);
		const void *mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
		REQUIRE(mapped != nullptr);
		std::memcpy(bytes.data(), mapped, bytes.size());
		SDL_UnmapGPUTransferBuffer(device, transfer);
		SDL_ReleaseGPUFence(device, fence);
		SDL_ReleaseGPUTransferBuffer(device, transfer);
		return bytes;
	}

	double PositiveRadiance(std::span<const std::byte> bytes, uint32_t width, uint32_t height) {
		const size_t row = (size_t(width) * 8 + 255) / 256 * 256;
		double maximum = 0;
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x) {
				uint16_t half = 0;
				std::memcpy(&half, bytes.data() + y * row + x * 8, sizeof(half));
				maximum = std::max(maximum, double(glm::unpackHalf1x16(half)));
			}
		return maximum;
	}

	double SampleCount(std::span<const std::byte> bytes, uint32_t width, uint32_t height) {
		const size_t row = (size_t(width) * 8 + 255) / 256 * 256;
		uint16_t half = 0;
		std::memcpy(&half, bytes.data() + (height / 2) * row + (width / 2) * 8 + 6, sizeof(half));
		return glm::unpackHalf1x16(half);
	}

	void Resource(
		graph::PipelineDocument &document,
		const char *name,
		graph::ResourceKind kind,
		graph::ResourceFormat format,
		uint32_t stride = 0,
		uint32_t width = 0
	) {
		document.Record(
			{.Kind = graph::EditKind::AddResource,
			 .Name = core::Name(name),
			 .Resource = kind,
			 .Format = format,
			 .Width = width,
			 .Height = width == 0 ? 0u : 1u,
			 .BufferStride = stride}
		);
	}

	graph::PipelineDocument TessellationDocument() {
		graph::PipelineDocument result;
		const graph::PipelineDocument base = graph::DefaultPbrDocument();
		Resource(
			result,
			"hard-tessellated-vertices",
			graph::ResourceKind::Buffer,
			graph::ResourceFormat::R8,
			48,
			524288
		);
		Resource(
			result,
			"hard-tessellated-indices",
			graph::ResourceKind::Buffer,
			graph::ResourceFormat::R8,
			4,
			1572864
		);
		Resource(
			result,
			"hard-tessellated-commands",
			graph::ResourceKind::Buffer,
			graph::ResourceFormat::R8,
			20,
			4096
		);
		Resource(
			result, "hard-tessellated-colour", graph::ResourceKind::Colour, graph::ResourceFormat::RGBA16F
		);
		Resource(result, "hard-tessellated-depth", graph::ResourceKind::Depth, graph::ResourceFormat::D32F);
		Resource(
			result, "hard-tessellated-display", graph::ResourceKind::Colour, graph::ResourceFormat::RGBA8
		);
		for (const graph::Edit &edit : base.Edits()) {
			if (edit.Kind == graph::EditKind::AddNode && edit.Name == core::Name("present")) {
				result.Record(
					{.Kind = graph::EditKind::AddNode,
					 .Name = core::Name("hard-adaptive-tessellation"),
					 .NodeKind = core::Name("tessellate"),
					 .Scope = graph::NodeScope::View}
				);
				result.Record(
					{.Kind = graph::EditKind::Reads,
					 .Target = core::Name("lod-instances"),
					 .Key = core::Name("instances")}
				);
				result.Record(
					{.Kind = graph::EditKind::Reads,
					 .Target = core::Name("view-camera"),
					 .Key = core::Name("camera")}
				);
				for (const auto &[name, port] : std::array{
						 std::pair{"hard-tessellated-vertices", "vertices"},
						 std::pair{"hard-tessellated-indices", "indices"},
						 std::pair{"hard-tessellated-commands", "commands"}
					 })
					result.Record(
						{.Kind = graph::EditKind::Writes, .Target = core::Name(name), .Key = core::Name(port)}
					);
				result.Record(
					{.Kind = graph::EditKind::Set, .Key = core::Name("max-subdivisions"), .Value = "2"}
				);
				result.Record(
					{.Kind = graph::EditKind::AddNode,
					 .Name = core::Name("hard-tessellated-draw"),
					 .NodeKind = core::Name("tessellated-draw"),
					 .Scope = graph::NodeScope::View}
				);
				for (const auto &[name, port] : std::array{
						 std::pair{"hard-tessellated-vertices", "vertices"},
						 std::pair{"hard-tessellated-indices", "indices"},
						 std::pair{"hard-tessellated-commands", "commands"}
					 })
					result.Record(
						{.Kind = graph::EditKind::Reads, .Target = core::Name(name), .Key = core::Name(port)}
					);
				result.Record(
					{.Kind = graph::EditKind::Writes,
					 .Target = core::Name("hard-tessellated-colour"),
					 .Key = core::Name("colour")}
				);
				result.Record(
					{.Kind = graph::EditKind::Writes,
					 .Target = core::Name("hard-tessellated-depth"),
					 .Key = core::Name("depth")}
				);
				result.Record(
					{.Kind = graph::EditKind::AddNode,
					 .Name = core::Name("hard-tessellated-preview"),
					 .NodeKind = core::Name("raster"),
					 .Scope = graph::NodeScope::View}
				);
				result.Record(
					{.Kind = graph::EditKind::Reads,
					 .Target = core::Name("hard-tessellated-colour"),
					 .Key = core::Name("colour")}
				);
				result.Record(
					{.Kind = graph::EditKind::Writes,
					 .Target = core::Name("hard-tessellated-display"),
					 .Key = core::Name("colour")}
				);
				result.Record(
					{.Kind = graph::EditKind::Set, .Key = core::Name("source"), .Value = PreviewShader()}
				);
			}
			result.Record(edit);
		}
		return result;
	}

}

TEST_CASE("dedicated tessellation expands and draws a GPU mesh", "[render][gpu][hard-render][.]") {
	FixtureDevice fixture;
	fixture.Initialise();
	graph::RenderGraph graph;
	core::Name offender;
	REQUIRE(graph::Build(TessellationDocument(), graph, offender) == graph::PipelineDocumentStatus::Ok);
	REQUIRE(fixture.Render.SetPipeline(core::Name("hard-tessellation"), graph));
	REQUIRE(fixture.Render.AddMesh(core::Name("hard-triangle"), Triangle()));
	render::SceneTarget target{79, 61};
	scene::DrawInstance instance;
	instance.Source = 1;
	instance.Mesh = core::Name("hard-triangle");
	instance.Frame.Position = {0, 0, -3};
	instance.HalfExtent = {1, 1, .01f};
	instance.Tint = {1, .2f, .1f};
	render::View view;
	view.World = 918;
	view.WorldName = core::Name("hard-tessellation.world");
	view.Pipeline = core::Name("hard-tessellation");
	view.Target = &target;
	view.Camera.FieldOfViewRadians = 1.2f;
	view.Camera.NearPlane = .1f;
	view.Camera.FarPlane = 32;
	view.Instances = std::span(&instance, 1);
	render::OverlayImage overlay;
	const auto frame = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(frame.Ran(core::Name("hard-adaptive-tessellation")));
	REQUIRE(frame.Ran(core::Name("hard-tessellated-draw")));
	CHECK(frame.ComputeDispatches > 0);
	CHECK(frame.ComputeDispatches == 3);
}

TEST_CASE("bounded GI and raytrace write retained GPU images", "[render][gpu][hard-render][.]") {
	FixtureDevice fixture;
	fixture.Initialise();
	graph::RenderGraph graph;
	const auto albedo = Texture(graph, "hard-albedo");
	const auto normal = Texture(graph, "hard-normal", graph::ResourceFormat::RGB10A2);
	const auto material = Texture(graph, "hard-material");
	const auto depth = Texture(graph, "hard-depth", graph::ResourceFormat::R32F);
	const auto occlusion = Texture(graph, "hard-occlusion", graph::ResourceFormat::R8);
	const auto scene = Texture(graph, "hard-scene", graph::ResourceFormat::RGBA16F);
	const auto indirect = graph.AddResource(
		{.Name = core::Name("hard-indirect"),
		 .Kind = graph::ResourceKind::Storage,
		 .Format = graph::ResourceFormat::RGBA16F}
	);
	const auto reflection = graph.AddResource(
		{.Name = core::Name("hard-reflection"),
		 .Kind = graph::ResourceKind::Storage,
		 .Format = graph::ResourceFormat::RGBA16F}
	);
	const auto history = graph.AddResource(
		{.Name = core::Name("hard-path-history"),
		 .Kind = graph::ResourceKind::Storage,
		 .Format = graph::ResourceFormat::RGBA16F,
		 .Lifetime = graph::ResourceLifetime::History}
	);
	const auto path = graph.AddResource(
		{.Name = core::Name("hard-path-radiance"),
		 .Kind = graph::ResourceKind::Storage,
		 .Format = graph::ResourceFormat::RGBA16F}
	);
	const auto giPreview = Texture(graph, "hard-gi-preview");
	const auto rayPreview = Texture(graph, "hard-ray-preview");
	const auto pathPreview = Texture(graph, "hard-path-preview");
	for (const auto id : {indirect, reflection, history, path})
		REQUIRE(id.IsValid());
	Raster(graph, "hard-albedo-fill", albedo, SolidShader("vec4(0.8, 0.3, 0.1, 1.0)"));
	Raster(graph, "hard-normal-fill", normal, SolidShader("vec4(0.5, 1.0, 0.5, 1.0)"));
	Raster(graph, "hard-material-fill", material, SolidShader("vec4(0.2, 0.0, 0.0, 1.0)"));
	Raster(graph, "hard-depth-fill", depth, SolidShader("vec4(3.0, 0.0, 0.0, 1.0)"));
	Raster(graph, "hard-occlusion-fill", occlusion, SolidShader("vec4(0.8, 0.0, 0.0, 1.0)"));
	Raster(graph, "hard-scene-fill", scene, SolidShader("vec4(0.3, 0.2, 0.1, 1.0)"));
	Trace(graph, "hard-gi", "global-illumination", {albedo, normal, material, depth, occlusion}, indirect);
	Trace(graph, "hard-ray", "raytrace", {scene, depth, normal, material, indirect}, reflection);
	Trace(graph, "hard-path", "pathtrace", {albedo, normal, material, depth, indirect, history}, path);
	graph::Node store;
	store.Name = core::Name("hard-path-store");
	store.Kind = core::Name("dispatch");
	store.Scope = graph::NodeScope::View;
	store.Reads = {path};
	store.Writes = {history};
	store.Parameters.push_back({core::Name("shader"), "attachment-copy.comp"});
	store.Parameters.push_back({core::Name("uniforms"), "view"});
	REQUIRE(graph.AddNode(std::move(store)).IsValid());
	graph::Node tonemap;
	tonemap.Name = core::Name("hard-path-tonemap");
	tonemap.Kind = core::Name("tonemap");
	tonemap.Scope = graph::NodeScope::View;
	tonemap.Reads = {path};
	tonemap.Writes = {pathPreview};
	REQUIRE(graph.AddNode(std::move(tonemap)).IsValid());
	Raster(graph, "hard-gi-preview-pass", giPreview, PreviewShader(), {indirect});
	Raster(graph, "hard-ray-preview-pass", rayPreview, PreviewShader(), {reflection});
	const core::Name boundary("hard-trace-boundary");
	graph::NodeKindSpec spec;
	spec.Kind = boundary;
	spec.Scope = graph::NodeScope::Frame;
	spec.Queue = graph::ExecutionQueue::Cpu;
	spec.Category = graph::NodeCategory::Output;
	spec.Inputs = {
		{.Name = core::Name("gi"), .Kind = graph::ResourceKind::Texture},
		{.Name = core::Name("ray"), .Kind = graph::ResourceKind::Texture},
		{.Name = core::Name("path"), .Kind = graph::ResourceKind::Texture}
	};
	REQUIRE(graph::RegisterNodeKind(std::move(spec)));
	REQUIRE(fixture.Render.InstallNodeHandler(boundary, [](const graph::RunContext &) { return true; }));
	graph::Node sink;
	sink.Name = boundary;
	sink.Kind = boundary;
	sink.Scope = graph::NodeScope::Frame;
	sink.Reads = {giPreview, rayPreview, pathPreview};
	REQUIRE(graph.AddNode(std::move(sink)).IsValid());
	REQUIRE(fixture.Render.SetPipeline(core::Name("hard-trace"), graph));
	render::SceneTarget target{65, 65};
	render::View view;
	view.World = 919;
	view.WorldName = core::Name("hard-trace.world");
	view.Pipeline = core::Name("hard-trace");
	view.Target = &target;
	view.Camera.FarPlane = 32;
	render::OverlayImage overlay;
	// The first render allocates graph targets and advances ResourceEpoch. Settle
	// that allocation frame before measuring the temporal contract.
	fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	view.Damage.Scene = true;
	const auto frame = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(frame.Ran(core::Name("hard-gi")));
	REQUIRE(frame.Ran(core::Name("hard-ray")));
	CHECK(
		PositiveRadiance(
			CaptureRgba16(
				fixture.Render, core::Name("hard-indirect"), view.Slot, target.Width, target.Height
			),
			target.Width,
			target.Height
		) > 0.01
	);
	CHECK(
		PositiveRadiance(
			CaptureRgba16(
				fixture.Render, core::Name("hard-reflection"), view.Slot, target.Width, target.Height
			),
			target.Width,
			target.Height
		) > 0.01
	);
	const auto first = CaptureRgba16(
		fixture.Render, core::Name("hard-path-radiance"), view.Slot, target.Width, target.Height
	);
	CHECK(SampleCount(first, target.Width, target.Height) == 1.0);
	view.Damage = {};
	const auto accumulationFrame = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	CHECK(accumulationFrame.Ran(core::Name("hard-path")));
	CHECK(accumulationFrame.Ran(core::Name("hard-path-store")));
	CHECK(accumulationFrame.Ran(core::Name("hard-path-tonemap")));
	CHECK_FALSE(accumulationFrame.Ran(core::Name("hard-albedo-fill")));
	CHECK_FALSE(accumulationFrame.Ran(core::Name("hard-gi")));
	const auto accumulated = CaptureRgba16(
		fixture.Render, core::Name("hard-path-radiance"), view.Slot, target.Width, target.Height
	);
	CHECK(SampleCount(accumulated, target.Width, target.Height) == 2.0);
	view.Damage.Scene = true;
	fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	const auto reset = CaptureRgba16(
		fixture.Render, core::Name("hard-path-radiance"), view.Slot, target.Width, target.Height
	);
	CHECK(SampleCount(reset, target.Width, target.Height) == 1.0);
}
