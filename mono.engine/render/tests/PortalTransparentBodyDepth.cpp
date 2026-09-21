#include "RenderFixture.hpp"

#include <engine/core/Bytes.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/render/PortalImageImport.hpp>
#include <engine/render/ResourceImage.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/generators/catch_generators.hpp>
#include <glm/packing.hpp>

TEST_SUITE_ID("engine.render.portaltransparentbodydepth")

namespace {
	using namespace engine;
	using core::Name;
	using namespace graph;

	void Install(render::Renderer &renderer, bool imported) {
		PipelineDocument document;
		const auto base = DefaultWorldHdrDocument();
		for (const auto &edit : base.Edits()) {
			if (edit.Kind == EditKind::AddNode && edit.Scope == NodeScope::Frame) break;
			document.Record(edit);
		}
		const auto resource =
			[&](const char *name, ResourceFormat format, ResourceKind kind = ResourceKind::Colour) {
				document.Record(
					{.Kind = EditKind::AddResource, .Name = Name(name), .Resource = kind, .Format = format}
				);
			};
		const auto node = [&](const char *name, const char *kind, NodeScope scope = NodeScope::View) {
			document.Record(
				{.Kind = EditKind::AddNode, .Name = Name(name), .NodeKind = Name(kind), .Scope = scope}
			);
		};
		const auto edge = [&](EditKind kind, const char *resource, const char *port) {
			document.Record({.Kind = kind, .Target = Name(resource), .Key = Name(port)});
		};
		resource("pane", ResourceFormat::RGBA16F);
		resource("pane-depth", ResourceFormat::R32F);
		if (imported) {
			node("pane-image", "eye-image");
			document.Record({.Kind = EditKind::Set, .Key = Name("scope"), .Value = "opaque-lighting"});
			document.Record({.Kind = EditKind::Set, .Key = Name("projection"), .Value = "eye"});
			edge(EditKind::Writes, "pane", "colour");
			edge(EditKind::Writes, "pane-depth", "depth");
			resource("composed", ResourceFormat::RGBA16F);
			resource("composed-depth", ResourceFormat::R32F);
			node("compose", "depth-compose");
			document.Record({.Kind = EditKind::Set, .Key = Name("mode"), .Value = "premultiplied"});
			edge(EditKind::Reads, "pane", "foreground");
			edge(EditKind::Reads, "pane-depth", "foreground-depth");
			edge(EditKind::Reads, "lens-b", "background");
			edge(EditKind::Reads, "linear-depth", "background-depth");
			edge(EditKind::Writes, "composed", "colour");
			edge(EditKind::Writes, "composed-depth", "depth");
		} else {
			resource("pane-z", ResourceFormat::D32F, ResourceKind::Depth);
			node("pane-peel", "transparent-layer");
			edge(EditKind::Reads, "depth", "opaque-z");
			edge(EditKind::Reads, "ordered-entities", "entities");
			edge(EditKind::Reads, "view-instances", "instances");
			edge(EditKind::Reads, "shadow", "shadow");
			edge(EditKind::Writes, "pane", "colour");
			edge(EditKind::Writes, "pane-depth", "depth");
			edge(EditKind::Writes, "pane-z", "z");
		}
		node("pane-export", "capture", NodeScope::Frame);
		edge(EditKind::Reads, "pane", "source");
		edge(EditKind::Reads, "pane-depth", "depth");
		node("result-export", "capture", NodeScope::Frame);
		edge(EditKind::Reads, imported ? "composed" : "lens-b", "source");
		edge(EditKind::Reads, "linear-depth", "depth");
		RenderGraph graph;
		Name offender;
		REQUIRE(Build(document, graph, offender) == PipelineDocumentStatus::Ok);
		REQUIRE(renderer.SetPipeline(Name(imported ? "retained-tie" : "native-tie"), graph));
	}

	render::ResourceImage Capture(render::Renderer &renderer, render::View &view, const char *node) {
		INFO("capture pipeline=" << view.Pipeline.Text() << " node=" << node);
		view.Damage = {.Scene = true, .Objects = true, .Environment = true, .Portals = true};
		const auto token = renderer.QueueResourceImage(view.Pipeline, Name(node));
		REQUIRE(token != 0);
		render::OverlayImage overlay;
		REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(Name(node)));
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (std::chrono::steady_clock::now() < deadline) {
			if (auto image = renderer.TakeResourceImage(token)) {
				REQUIRE(image->Status == render::ResourceImageStatus::Ok);
				return std::move(*image);
			}
			SDL_Delay(1);
		}
		FAIL("capture did not complete");
		return {};
	}

	std::vector<glm::vec4> Pixels(const render::ResourceImage &image) {
		core::ByteReader reader(image.Pixels);
		std::vector<glm::vec4> pixels;
		while (!reader.AtEnd()) {
			const auto rg = glm::unpackHalf2x16(reader.ReadUInt32());
			const auto ba = glm::unpackHalf2x16(reader.ReadUInt32());
			pixels.emplace_back(rg, ba);
		}
		return pixels;
	}
}

TEST_CASE(
	"retained glass respects a newly inserted opaque body's exact depth",
	"[render][gpu][transparent-body-depth][.]"
) {
	const bool transformed = GENERATE(false, true);
	const float scale = GENERATE(.37f, 1.f, 3.1f);
	const float offset = GENERATE(-.05f, 0.f, .05f);
	CAPTURE(transformed, offset, scale);
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	Install(renderer, false);
	Install(renderer, true);
	assets::MeshData plane;
	plane.Vertices = {
		{{-1, -1, 0}, {0, 0, 1}, {0, 1}},
		{{1, -1, 0}, {0, 0, 1}, {1, 1}},
		{{1, 1, 0}, {0, 0, 1}, {1, 0}},
		{{-1, 1, 0}, {0, 0, 1}, {0, 0}}
	};
	plane.Indices = {0, 1, 2, 0, 2, 3};
	plane.ComputeBounds();
	const Name mesh("tie-plane"), whiteName("tie-white");
	REQUIRE(renderer.AddMesh(mesh, plane));
	assets::TextureData white;
	white.Width = white.Height = 1;
	white.Format = assets::TextureFormat::RGBA8;
	white.Pixels.assign(4, std::byte{255});
	REQUIRE(renderer.AddTexture(whiteName, white));
	render::SceneTarget target{65, 65};
	render::View view;
	view.Target = &target;
	view.WorldName = Name("tie-world");
	view.Pipeline = Name("native-tie");
	view.Camera.NearPlane *= scale;
	view.Camera.FarPlane *= scale;
	if (transformed)
		view.CameraFrame = core::CFrame(core::Vector3{7 * scale, 2 * scale, 3 * scale}) *
						   core::CFrame::Angles(-.2f, .37f, .1f);
	view.OverrideLighting = true;
	view.Lighting.Ambient = {};
	view.Lighting.OutdoorAmbient = {};
	view.Lighting.Direct = {};
	scene::DrawInstance pane;
	pane.Source = 1;
	pane.Mesh = mesh;
	pane.Frame =
		view.CameraFrame * core::CFrame(core::Vector3{0, 0, -3 * scale}) * core::CFrame::Angles(0, .23f, 0);
	pane.HalfExtent = {scale, scale, scale};
	pane.Transparency = .5f;
	pane.CastShadow = false;
	pane.Tint = {};
	pane.EmissiveMap = whiteName;
	pane.EmissiveTint = {1, 0, 0};
	view.Instances = std::span(&pane, 1);
	const auto retained = Capture(renderer, view, "pane-export");
	const auto retainedPixels = Pixels(retained);
	REQUIRE(retainedPixels[32 * 65 + 32].r > .4f);
	render::PortalImageBinding binding;
	binding.World = view.World;
	binding.WorldName = view.WorldName;
	binding.Portal = Name("retained-pane");
	binding.Expected = {1, "retained-pane", 1, 1};
	binding.ExpectedScope = render::PortalImageScope::OpaqueLighting;
	binding.ExpectedProjection = render::PortalImageProjection::Eye;
	binding.Sampling = scene::ResolveCamera(view.CameraFrame, view.Camera, 1).ViewProjection;
	render::PortalImageReply reply;
	reply.Key = binding.Expected;
	reply.Scope = binding.ExpectedScope;
	reply.Status = render::PortalImageStatus::Ok;
	reply.Width = retained.Width;
	reply.Height = retained.Height;
	reply.RowStride = retained.RowStride;
	reply.Pixels = retained.Pixels;
	reply.Depth = retained.Depth;
	reply.PixelHash = assets::Hasher::Of(reply.Pixels);
	reply.DepthHash = assets::Hasher::Of(reply.Depth);
	const auto handle = renderer.QueuePortalImage(binding, std::move(reply));
	REQUIRE(handle != 0);
	auto body = pane;
	body.Source = 2;
	body.Transparency = 0;
	body.EmissiveTint = {0, 0, 1};
	body.Frame.Position = body.Frame.Position + view.CameraFrame.VectorToWorldSpace({0, 0, offset * scale});
	const std::array rows{pane, body};
	view.Instances = rows;
	const auto direct = Capture(renderer, view, "result-export");
	view.Instances = std::span(&body, 1);
	view.Pipeline = Name("retained-tie");
	view.EyeImage = handle;
	view.EyeImageKey = binding.Portal;
	const auto composed = Capture(renderer, view, "result-export");
	const auto expected = Pixels(direct), actual = Pixels(composed);
	const auto centre = 32 * 65 + 32;
	CHECK((expected[centre].r > .4f) == (offset < 0));
	CHECK(expected[centre].b > .4f);
	core::ByteReader paneDepth(retained.Depth), bodyDepth(composed.Depth);
	size_t falselyNear = 0;
	for (size_t pixel = 0; pixel < expected.size(); ++pixel) {
		const float front = paneDepth.ReadFloat(), back = bodyDepth.ReadFloat();
		if (offset == 0 && front > 0 && back > 0 && front < back) ++falselyNear;
	}
	INFO("exact-tie samples classified in front: " << falselyNear);
	render::test::ImageTolerance tolerance;
	tolerance.Absolute = .002;
	render::test::CheckImage(
		renderer,
		"transparent-body-depth",
		"radiance-scale-" + std::to_string(scale) + (transformed ? "-transformed-" : "-identity-") +
			std::to_string(offset),
		std::to_string(offset) + (transformed ? " transformed" : " identity"),
		{65, 65, render::test::ImageFormat::Rgba32Float, std::as_bytes(std::span(expected)), 0},
		{65, 65, render::test::ImageFormat::Rgba32Float, std::as_bytes(std::span(actual)), 0},
		tolerance
	);
	REQUIRE(renderer.DropPortalImage(handle));
}
