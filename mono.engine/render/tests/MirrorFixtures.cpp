#include "RenderFixture.hpp"

#include <engine/core/FrameGraph.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/effects/Ribbon.hpp>
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
#include <iostream>
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

	SECTION("one authored mirror bounce is independent of the portal depth default") {
		constexpr size_t FACETS = 320;
		renderer.SetSurfaceLimit(FACETS);
		renderer.SetSurfaceBounces(1);
		std::vector<render::SurfaceView> surfaces(FACETS, surface);
		for (size_t index = 0; index < surfaces.size(); ++index) {
			surfaces[index].Index = static_cast<int16_t>(index);
			surfaces[index].Width = 4;
			surfaces[index].Height = 2;
		}
		view.Surfaces = surfaces;
		render::OverlayImage overlay;
		const auto frame = renderer.Render(std::span(&view, 1), overlay, nullptr, false);
		REQUIRE_FALSE(frame.SurfaceBudgetExceeded);
		CHECK(frame.SurfacePasses == FACETS);
		const auto image = render::test::CaptureResource(
			renderer,
			core::Name("scene-image"),
			view.Slot,
			target.Width,
			target.Height,
			render::test::ImageFormat::Bgra8Unorm
		);
		const auto centre =
			image.Bytes.data() + (target.Height / 2) * image.RowStrideBytes + (target.Width / 2) * 4;
		CHECK(std::to_integer<uint8_t>(centre[2]) > 128);
		CHECK(std::to_integer<uint8_t>(centre[0]) < 32);
		CHECK(std::to_integer<uint8_t>(centre[1]) < 32);
	}

	SECTION("320 distinct mirror runs preserve reflected radiance") {
		constexpr size_t FACETS = 320;
		renderer.SetSurfaceLimit(FACETS);
		renderer.SetSurfaceBounces(1);
		std::vector<scene::DrawInstance> mirrorRows;
		std::vector<render::SurfaceView> mirrorViews;
		for (size_t index = 0; index < FACETS; ++index) {
			auto row = instances[0];
			row.Source = static_cast<uint32_t>(index + 1);
			row.Surface = static_cast<int16_t>(index);
			row.Frame.Position = {(float(index % 20) - 10) * .18f, (float(index / 20) - 8) * .18f, -4};
			row.HalfExtent = {.085f, .085f, .01f};
			mirrorRows.push_back(row);
			auto geometry = pane;
			geometry.Centre = row.Frame.Position;
			geometry.First = {.085f, 0, 0};
			geometry.Second = {0, .085f, 0};
			const auto reflected = scene::ReflectCamera(geometry, {}, {});
			REQUIRE(reflected.Renders);
			auto capture = surface;
			capture.Index = row.Surface;
			capture.Frame = reflected.Frame;
			capture.Projection = scene::SurfaceProjection(reflected.Lens, reflected.Frame);
			capture.PaneCentre = geometry.Centre;
			capture.PaneFirst = geometry.First;
			capture.PaneSecond = geometry.Second;
			capture.Width = 4;
			capture.Height = 2;
			mirrorViews.push_back(capture);
		}
		auto redWall = instances[1];
		redWall.Source = FACETS + 1;
		mirrorRows.push_back(redWall);
		view.Instances = mirrorRows;
		view.Surfaces = mirrorViews;
		const auto lookupProbes = [] {
			for (const auto &counter : core::Metrics::Snapshot().Counters)
				if (counter.Name == core::Name("render.surface_lookup.probes")) return counter.Value;
			return 0.0;
		};
		const auto probesBefore = lookupProbes();
		const bool profilingBefore = core::FrameGraph::IsEnabled();
		struct RestoreProfiling {
			bool Enabled;
			~RestoreProfiling() {
				core::FrameGraph::SetEnabled(Enabled);
			}
		} restore{profilingBefore};
		core::FrameGraph::SetEnabled(true);
		core::FrameGraph::BeginFrame();
		render::OverlayImage overlay;
		const auto frame = renderer.Render(std::span(&view, 1), overlay, nullptr, false);
		core::FrameGraph::EndFrame();
		double paneMilliseconds = 0;
		for (const auto &span : core::FrameGraph::Spans())
			if (span.Name == "compose capture opaque panes") paneMilliseconds += span.Milliseconds;
		std::cout << "mirror composition facets=" << FACETS << " views=1 backend=SDL_GPU"
				  << " probes=" << lookupProbes() - probesBefore << " opaque_panes_ms=" << paneMilliseconds
				  << " dropped_spans=" << core::FrameGraph::Dropped() << '\n';
		REQUIRE_FALSE(frame.SurfaceBudgetExceeded);
		CHECK(frame.SurfacePasses == FACETS);
		CHECK(lookupProbes() - probesBefore == FACETS * (FACETS - 1));
		const auto image = render::test::CaptureResource(
			renderer,
			core::Name("scene-image"),
			view.Slot,
			target.Width,
			target.Height,
			render::test::ImageFormat::Bgra8Unorm
		);
		const auto centre =
			image.Bytes.data() + (target.Height / 2) * image.RowStrideBytes + (target.Width / 2) * 4;
		CHECK(std::to_integer<uint8_t>(centre[2]) > 128);
		CHECK(std::to_integer<uint8_t>(centre[0]) < 32);
		CHECK(std::to_integer<uint8_t>(centre[1]) < 32);
	}

	SECTION("wide mirror slots preserve the reflected screen image") {
		renderer.SetSurfaceLimit(scene::MAX_SURFACES);
		renderer.SetSurfaceBounces(1);
		render::test::CapturedImage expected;
		for (const int16_t slot :
			 {int16_t{0},
			  int16_t{127},
			  int16_t{128},
			  int16_t{255},
			  int16_t{256},
			  int16_t{319},
			  int16_t{511}}) {
			INFO("surface " << slot);
			REQUIRE(static_cast<size_t>(slot) < scene::MAX_SURFACES);
			instances[0].Surface = slot;
			surface.Index = slot;
			render::OverlayImage overlay;
			const auto frame = renderer.Render(std::span(&view, 1), overlay, nullptr, false);
			REQUIRE(frame.SurfacePasses > 0);
			const auto image = render::test::CaptureResource(
				renderer,
				core::Name("scene-image"),
				view.Slot,
				target.Width,
				target.Height,
				render::test::ImageFormat::Bgra8Unorm
			);
			if (slot == 0) {
				expected = image;
				const auto centre = expected.Bytes.data() + (target.Height / 2) * expected.RowStrideBytes +
									(target.Width / 2) * 4;
				CHECK(std::to_integer<uint8_t>(centre[2]) > 128);
				CHECK(std::to_integer<uint8_t>(centre[0]) < 32);
				CHECK(std::to_integer<uint8_t>(centre[1]) < 32);
			} else {
				render::test::CheckImage(
					renderer,
					"wide-mirror-slots",
					"scene-image",
					"only the surface slot changes",
					expected.View(),
					image.View()
				);
			}
		}
	}

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

	SECTION("rate-limited mirror captures do not upload untouched ribbons twice") {
		const std::array<effects::RibbonVertex, 4> ribbon{
			effects::RibbonVertex{{-.5f, .1f, -2}, {0, 0}, 0xFF00FF00},
			effects::RibbonVertex{{-.5f, -.1f, -2}, {0, 1}, 0xFF00FF00},
			effects::RibbonVertex{{.5f, .1f, -2}, {1, 0}, 0xFF00FF00},
			effects::RibbonVertex{{.5f, -.1f, -2}, {1, 1}, 0xFF00FF00}
		};
		effects::RibbonRun run{};
		run.Count = static_cast<uint32_t>(ribbon.size());
		const std::array runs{run};
		surface.FPS = 1;
		view.RibbonVertices = ribbon;
		view.RibbonRuns = runs;
		render::OverlayImage overlay;
		renderer.SetAnimationTime(1);
		const auto warm = renderer.Render(std::span(&view, 1), overlay, nullptr, false);
		REQUIRE(warm.SurfacePasses > 0);
		REQUIRE(warm.RibbonVertices == ribbon.size());
		renderer.SetAnimationTime(1.01);
		view.RibbonVertices = {};
		view.RibbonRuns = {};
		const auto withoutRibbon = renderer.Render(std::span(&view, 1), overlay, nullptr, false);
		REQUIRE(withoutRibbon.SurfacePasses == 0);
		view.RibbonVertices = ribbon;
		view.RibbonRuns = runs;
		const auto limited = renderer.Render(std::span(&view, 1), overlay, nullptr, false);
		CHECK(limited.SurfacePasses == 0);
		REQUIRE(limited.RibbonVertices == ribbon.size());
		CHECK(limited.UploadedBytes == withoutRibbon.UploadedBytes + sizeof(ribbon));
		renderer.SetAnimationTime(2.01);
		CHECK(renderer.Render(std::span(&view, 1), overlay, nullptr, false).SurfacePasses > 0);
	}

	SECTION("ribbon captures restore source uploads only after replacement") {
		const uint32_t captures = GENERATE(0u, 1u, 3u);
		const std::array<effects::RibbonVertex, 4> ribbon{
			effects::RibbonVertex{{-.5f, .1f, -2}, {0, 0}, 0xFF00FF00},
			effects::RibbonVertex{{-.5f, -.1f, -2}, {0, 1}, 0xFF00FF00},
			effects::RibbonVertex{{.5f, .1f, -2}, {1, 0}, 0xFF00FF00},
			effects::RibbonVertex{{.5f, -.1f, -2}, {1, 1}, 0xFF00FF00}
		};
		effects::RibbonRun run{};
		run.Count = static_cast<uint32_t>(ribbon.size());
		run.FaceCamera = true;
		std::array runs{run};
		std::vector<render::SurfaceView> surfaces(captures, surface);
		for (uint32_t index = 0; index < captures; ++index)
			surfaces[index].Index = int16_t(index);
		view.Surfaces = surfaces;
		renderer.SetSurfaceBounces(1);
		render::OverlayImage overlay;
		(void)renderer.Render(std::span(&view, 1), overlay, nullptr, false);
		const auto withoutRibbon = renderer.Render(std::span(&view, 1), overlay, nullptr, false);
		view.RibbonVertices = ribbon;
		view.RibbonRuns = runs;
		const auto withRibbon = renderer.Render(std::span(&view, 1), overlay, nullptr, false);
		REQUIRE(withRibbon.Submitted);
		REQUIRE(withRibbon.RibbonVertices == ribbon.size());
		CHECK(withRibbon.SurfacePasses == captures);
		const uint32_t uploads = 1 + captures + (captures > 0 ? 1 : 0);
		CHECK(withRibbon.UploadedBytes == withoutRibbon.UploadedBytes + uploads * sizeof(ribbon));
		if (captures > 0) {
			const auto expected = render::test::CaptureResource(
				renderer,
				core::Name("scene-image"),
				view.Slot,
				target.Width,
				target.Height,
				render::test::ImageFormat::Bgra8Unorm
			);
			// A refused capture must not leave a dirty ribbon restoration flag for the retry.
			runs[0].Count = static_cast<uint32_t>(ribbon.size() + 2);
			const auto refused = renderer.Render(std::span(&view, 1), overlay, nullptr, false);
			CHECK(refused.SurfacePasses == 0);
			runs[0] = run;
			const auto retried = renderer.Render(std::span(&view, 1), overlay, nullptr, false);
			REQUIRE(retried.Submitted);
			CHECK(retried.SurfacePasses == captures);
			CHECK(retried.RibbonVertices == ribbon.size());
			const auto retryImage = render::test::CaptureResource(
				renderer,
				core::Name("scene-image"),
				view.Slot,
				target.Width,
				target.Height,
				render::test::ImageFormat::Bgra8Unorm
			);
			render::test::CheckImage(
				renderer,
				"ribbon-capture-retry",
				"scene-image",
				"valid retry restores ribbon pixels",
				expected.View(),
				retryImage.View()
			);
			view.RibbonVertices = {};
			view.RibbonRuns = {};
			const auto freshBaseline = renderer.Render(std::span(&view, 1), overlay, nullptr, false);
			view.RibbonVertices = ribbon;
			view.RibbonRuns = runs;
			const auto steady = renderer.Render(std::span(&view, 1), overlay, nullptr, false);
			REQUIRE(steady.Submitted);
			CHECK(steady.SurfacePasses == captures);
			CHECK(steady.UploadedBytes == freshBaseline.UploadedBytes + uploads * sizeof(ribbon));
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
