#include "RenderFixture.hpp"

#include <engine/ecs/Store.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/render/InterfacePass.hpp>
#include <engine/render/ResourceImage.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/generators/catch_generators.hpp>
#include <glm/packing.hpp>

#include <array>
#include <cmath>
#include <span>

TEST_SUITE_ID("engine.render.mirrorfixtures")

namespace {
	constexpr float HALF_FLOAT_TOLERANCE = .002f;

	float HalfChannel(std::span<const std::byte> pixels, size_t offset) {
		const auto low = std::to_integer<uint32_t>(pixels[offset]);
		const auto high = std::to_integer<uint32_t>(pixels[offset + 1]);
		return glm::unpackHalf2x16(low | high << 8).x;
	}

	bool MatchesHalfPixels(std::span<const std::byte> pixels, const std::array<float, 4> &expected) {
		if (pixels.empty() || pixels.size() % 8 != 0) return false;
		for (size_t pixel = 0; pixel < pixels.size() / 8; ++pixel) {
			for (size_t channel = 0; channel < expected.size(); ++channel) {
				if (std::abs(HalfChannel(pixels, pixel * 8 + channel * 2) - expected[channel]) >
					HALF_FLOAT_TOLERANCE)
					return false;
			}
		}
		return true;
	}
}

TEST_CASE("mirror capture preserves declared HDR radiance", "[render][gpu][mirror-fixture][.]") {
	using namespace engine;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	graph::PipelineDocument document;
	const auto basis = graph::DefaultWorldHdrDocument();
	for (auto edit : basis.Edits()) {
		if (edit.Kind == graph::EditKind::AddResource && edit.Name == core::Name("mirror-views")) {
			edit.Format = graph::ResourceFormat::RGBA16F;
		}
		document.Record(std::move(edit));
	}
	document.Record(
		{.Kind = graph::EditKind::AddNode,
		 .Name = core::Name("mirror-export"),
		 .NodeKind = core::Name("capture"),
		 .Scope = graph::NodeScope::Frame}
	);
	document.Record(
		{.Kind = graph::EditKind::Reads, .Target = core::Name("mirror-views"), .Key = core::Name("source")}
	);
	graph::RenderGraph pipeline;
	core::Name offender;
	REQUIRE(graph::Build(document, pipeline, offender) == graph::PipelineDocumentStatus::Ok);
	const core::Name pipelineName("mirror.fixture");
	REQUIRE(renderer.SetPipeline(pipelineName, pipeline));

	assets::MeshData mesh;
	mesh.Vertices = {
		{{-1, -1, 0}, {0, 0, 1}, {0, 1}},
		{{1, -1, 0}, {0, 0, 1}, {1, 1}},
		{{1, 1, 0}, {0, 0, 1}, {1, 0}},
		{{-1, 1, 0}, {0, 0, 1}, {0, 0}}
	};
	mesh.Indices = {0, 1, 2, 0, 2, 3};
	mesh.ComputeBounds();
	REQUIRE(renderer.AddMesh(core::Name("mirror.plane"), mesh));
	assets::TextureData white;
	white.Width = white.Height = 1;
	white.Format = assets::TextureFormat::RGBA8;
	white.Pixels.assign(4, std::byte{255});
	REQUIRE(renderer.AddTexture(core::Name("mirror.white"), white));
	std::array<scene::DrawInstance, 2> instances;
	for (size_t index = 0; index < instances.size(); ++index) {
		instances[index].Source = static_cast<uint32_t>(index + 1);
		instances[index].Mesh = core::Name("mirror.plane");
		instances[index].Texture = core::Name("mirror.white");
		instances[index].CastShadow = false;
	}
	instances[0].Frame.Position = {0, 0, -4};
	instances[0].HalfExtent = {2, 2, .01f};
	instances[0].Surface = 0;
	instances[1].Frame = core::CFrame({0, 0, 2}) * core::CFrame::Angles(0, 3.14159265358979323846f, 0);
	instances[1].HalfExtent = {100, 100, .01f};
	instances[1].Tint = {1, 0, 0};
	scene::SurfacePane pane;
	pane.Centre = {0, 0, -4};
	pane.Normal = {0, 0, 1};
	pane.First = {2, 0, 0};
	pane.Second = {0, 2, 0};
	const auto eye = scene::ReflectCamera(pane, {}, {});
	REQUIRE(eye.Renders);
	render::SurfaceView surface;
	surface.Frame = eye.Frame;
	surface.Projection = scene::SurfaceProjection(eye.Lens, eye.Frame);
	surface.PaneCentre = pane.Centre;
	surface.PaneNormal = pane.Normal;
	surface.PaneFirst = pane.First;
	surface.PaneSecond = pane.Second;
	surface.Width = 65;
	surface.Height = 37;
	render::SceneTarget target{65, 37};
	render::View view;
	view.Pipeline = pipelineName;
	view.Target = &target;
	view.Instances = instances;
	view.Surfaces = std::span(&surface, 1);
	view.OverrideLighting = true;
	view.Lighting.Ambient = {2, 2, 2};
	view.Lighting.OutdoorAmbient = {2, 2, 2};
	view.Lighting.Direct = {};
	view.Lighting.RenderFeatures.Disable |= scene::FeatureBit(scene::RenderFeature::AmbientOcclusion);

	SECTION("primary body selection preserves reflected pixels and cache hits") {
		const bool imported = GENERATE(false, true);
		const std::array<uint32_t, 1> hiddenRows{1};
		instances[1].Rig = imported ? 0 : 7;
		if (imported) instances[1].SourceWorld = core::Name("foreign.body");
		std::vector<std::byte> expected;
		for (uint64_t token = 1; token <= 3; ++token) {
			view.EyeRig = token == 2 && !imported ? 7 : 0;
			view.EyeHiddenRows = token == 2 && imported ? std::span(hiddenRows) : std::span<const uint32_t>{};
			REQUIRE(renderer.RequestResourceImage({token, pipelineName, core::Name("mirror-export"), 0}));
			render::OverlayImage overlay;
			const auto frame = renderer.Render(std::span(&view, 1), overlay, nullptr, false);
			CHECK((token == 1 ? frame.SurfacePasses > 0 : frame.SurfacePasses == 0));
			std::optional<render::ResourceImage> captured;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			while (!captured && std::chrono::steady_clock::now() < deadline) {
				captured = renderer.TakeResourceImage(token);
				if (!captured) SDL_Delay(1);
			}
			REQUIRE(captured.has_value());
			REQUIRE(captured->Status == render::ResourceImageStatus::Ok);
			if (token == 1) {
				expected = captured->Pixels;
				REQUIRE(expected.size() >= 8);
				CHECK(HalfChannel(expected, 0) > 1.0f);
				CHECK(MatchesHalfPixels(expected, {2.0f, 0.0f, 0.0f, 1.0f}));
			} else
				CHECK(captured->Pixels == expected);
		}
	}

	SECTION("same-name texture replacement invalidates retained mirrors") {
		instances[1].Tint = {1, 1, 1};
		for (uint64_t token = 1; token <= 2; ++token) {
			if (token == 2) {
				auto green = white;
				green.Pixels[0] = green.Pixels[2] = std::byte{0};
				REQUIRE(renderer.AddTexture(core::Name("mirror.white"), green));
			}
			REQUIRE(renderer.RequestResourceImage({token, pipelineName, core::Name("mirror-export"), 0}));
			render::OverlayImage overlay;
			CHECK(renderer.Render(std::span(&view, 1), overlay, nullptr, false).SurfacePasses > 0);
			std::optional<render::ResourceImage> image;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			while (!image && std::chrono::steady_clock::now() < deadline) {
				image = renderer.TakeResourceImage(token);
				if (!image) SDL_Delay(1);
			}
			REQUIRE(image.has_value());
			REQUIRE(image->Status == render::ResourceImageStatus::Ok);
			REQUIRE(image->Pixels.size() == size_t(image->Width) * image->Height * 8);
			const std::array expected{token == 1 ? 2.0f : 0.0f, 2.0f, token == 1 ? 2.0f : 0.0f, 1.0f};
			INFO("texture revision " << token);
			CHECK(HalfChannel(image->Pixels, 2) > 1.0f);
			CHECK(MatchesHalfPixels(image->Pixels, expected));
			CHECK(renderer.Render(std::span(&view, 1), overlay, nullptr, false).SurfacePasses == 0);
		}
	}

	SECTION("retained spatial UI remains in a scene-only redraw") {
		ecs::Store interfaceWorld("mirror.fixture.ui");
		gui::RegisterGuiClasses();
		const auto collector = interfaceWorld.CreateInstance(gui::GuiClass("SurfaceGui"), "Sign");
		gui::SpatialCanvas spatial;
		spatial.Size = {100, 100};
		spatial.Origin = {-100, 100, 1};
		spatial.AxisX = {200, 0, 0};
		spatial.AxisY = {0, -200, 0};
		spatial.Normal = {0, 0, -1};
		spatial.Brightness = 2;
		interfaceWorld.Set(collector, spatial);
		gui::DrawCommand rectangle;
		rectangle.Collector = collector;
		rectangle.Spatial = true;
		rectangle.Bounds = {{0, 0}, {100, 100}};
		rectangle.Clip = rectangle.Bounds;
		rectangle.Tint = {1, 0, 0};
		gui::DrawList list;
		list.Commands.push_back(rectangle);
		render::InterfacePass interface;
		const auto backend = renderer.Backend();
		REQUIRE(interface.Initialise(backend.Device, backend.ColourFormat));
		interface.Submit(list, {65, 37}, {65, 37}, interfaceWorld);
		instances[1].Tint = {};
		for (uint64_t token = 1; token <= 2; ++token) {
			if (token == 2) {
				view.Damage.GameInterface = false;
				view.CameraFrame.Position.X = .1f;
				const auto moved = scene::ReflectCamera(pane, view.CameraFrame, {});
				surface.Frame = moved.Frame;
				surface.Projection = scene::SurfaceProjection(moved.Lens, moved.Frame);
			}
			REQUIRE(renderer.RequestResourceImage({token, pipelineName, core::Name("mirror-export"), 0}));
			render::OverlayImage overlay;
			CHECK(renderer.Render(std::span(&view, 1), overlay, &interface, false).SurfacePasses > 0);
			std::optional<render::ResourceImage> image;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			while (!image && std::chrono::steady_clock::now() < deadline) {
				image = renderer.TakeResourceImage(token);
				if (!image) SDL_Delay(1);
			}
			REQUIRE(image.has_value());
			REQUIRE(image->Status == render::ResourceImageStatus::Ok);
			CHECK(HalfChannel(image->Pixels, 0) > 1.0f);
			CHECK(MatchesHalfPixels(image->Pixels, {2.0f, 0.0f, 0.0f, 1.0f}));
		}
	}

	SECTION("visible mirrors refuse insufficient depth or pixels") {
		for (const render::View::SurfaceCaptureBudget budget :
			 {render::View::SurfaceCaptureBudget{0, 65 * 37}, render::View::SurfaceCaptureBudget{1, 1}}) {
			view.SurfaceBudget = budget;
			render::OverlayImage overlay;
			const auto result = renderer.Render(std::span(&view, 1), overlay, nullptr, false);
			CHECK(result.SurfaceBudgetExceeded);
			CHECK(result.SurfacePasses == 0);
		}
	}
	SECTION("nested mirrors debit their own capture pixels") {
		std::vector<scene::DrawInstance> corridor(instances.begin(), instances.end());
		auto second = instances[0];
		second.Source = 3;
		second.Surface = 1;
		second.Frame = core::CFrame({0, 0, 1}) * core::CFrame::Angles(0, 3.14159265358979323846f, 0);
		second.HalfExtent = {.5f, .5f, .01f};
		corridor.push_back(second);
		auto opposite = pane;
		opposite.Centre = {0, 0, 1};
		opposite.Normal = {0, 0, -1};
		opposite.First = {.5f, 0, 0};
		opposite.Second = {0, .5f, 0};
		const auto secondEye = scene::ReflectCamera(opposite, {}, {});
		REQUIRE(secondEye.Renders);
		std::array surfaces{surface, surface};
		surfaces[1].Index = 1;
		surfaces[1].Frame = secondEye.Frame;
		surfaces[1].Projection = scene::SurfaceProjection(secondEye.Lens, secondEye.Frame);
		surfaces[1].PaneCentre = opposite.Centre;
		surfaces[1].PaneNormal = opposite.Normal;
		surfaces[1].PaneFirst = opposite.First;
		surfaces[1].PaneSecond = opposite.Second;
		view.Surfaces = surfaces;
		view.Instances = corridor;
		renderer.SetSurfaceBounces(2);
		view.SurfaceBudget = render::View::SurfaceCaptureBudget{2, 4 * 65 * 37};
		render::OverlayImage overlay;
		const auto complete = renderer.Render(std::span(&view, 1), overlay, nullptr, false);
		CHECK_FALSE(complete.SurfaceBudgetExceeded);
		// The shared plan captures the visible root and its child.
		const uint32_t captures = 2;
		CHECK(complete.SurfacePasses == captures);
		view.Lighting.Ambient = {.25f, .25f, .25f};
		view.SurfaceBudget->Pixels = (captures - 1) * 65 * 37;
		const auto bounded = renderer.Render(std::span(&view, 1), overlay, nullptr, false);
		CHECK(bounded.SurfaceBudgetExceeded);
		CHECK(bounded.SurfacePasses < captures);
	}

	SECTION("HDR lighting edits invalidate retained mirrors") {

		uint64_t token = 0;
		for (const float ambient : {2.0f, 0.25f}) {
			++token;
			view.Lighting.Ambient = {ambient, ambient, ambient};
			view.Lighting.OutdoorAmbient = view.Lighting.Ambient;
			REQUIRE(renderer.RequestResourceImage({token, pipelineName, core::Name("mirror-export"), 0}));
			render::OverlayImage overlay;
			const auto result = renderer.Render(std::span(&view, 1), overlay, nullptr, false);
			CHECK(result.SurfacePasses > 0);
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			std::optional<render::ResourceImage> image;
			while (!image && std::chrono::steady_clock::now() < deadline) {
				image = renderer.TakeResourceImage(token);
				if (!image) SDL_Delay(1);
			}
			REQUIRE(image.has_value());
			REQUIRE(image->Status == render::ResourceImageStatus::Ok);
			REQUIRE(image->Pixels.size() == size_t(image->Width) * image->Height * 8);
			// RGBA16F preserves the declared magnitude within one half-float step.
			// A lighting-only edit must invalidate the retained capture without changed geometry.
			if (ambient > 1.0f) CHECK(HalfChannel(image->Pixels, 0) > 1.0f);
			CHECK(MatchesHalfPixels(image->Pixels, {ambient, 0.0f, 0.0f, 1.0f}));
			CHECK(renderer.Render(std::span(&view, 1), overlay, nullptr, false).SurfacePasses == 0);
		}
	}
}
