#include <engine/render/PortalGeometry.hpp>
#include <engine/render/PortalImageHost.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>
#include <optional>

TEST_SUITE_ID("engine.render.portalimagehost")
TEST_DEPENDS("engine.render.portalimageruntime")

namespace {
	using namespace engine;
	using namespace engine::render;
	constexpr PortalImageHost::Time START{};
	struct Worlds {
		world::Universe Universe;
		world::WorldId Source = Universe.Create({.Name = core::Name("near")});
		world::WorldId Destination = Universe.Create({.Name = core::Name("far replica")});
		Worlds() {
			REQUIRE(Universe.ConfigurePresentation(123));
		}
		PortalImageDemand Demand(size_t slot = 0) const {
			PortalImageDemand demand;
			demand.DestinationWorld = core::Name("far");
			demand.Request.Key.PortalKey = "Door";
			demand.Request.Key.CameraRevision = 1;
			demand.Request.Width = demand.Request.Height = 8;
			demand.Request.PixelBudget = 64;
			demand.Request.ClipPlane = {0, 0, -1, -1};
			demand.Binding.World = Source.Index;
			demand.Binding.WorldName = core::Name("near");
			demand.Binding.ViewSlot = slot;
			demand.Binding.Portal = core::Name("Door");
			return demand;
		}
		PortalImageDestination Route() const {
			return {core::Name("far"), Destination};
		}
	};
}

TEST_CASE("portal host shares producers while retaining viewport endpoints", "[render][portal-host]") {
	Worlds worlds;
	Renderer renderer;
	PortalImageHost host(worlds.Universe, renderer);
	auto first = worlds.Demand();
	auto second = worlds.Demand(2);
	const auto route = worlds.Route();
	REQUIRE(host.Submit(worlds.Source, 0, std::span(&first, 1), std::span(&route, 1), START) == 1);
	REQUIRE(host.Submit(worlds.Source, 2, std::span(&second, 1), std::span(&route, 1), START) == 1);
	CHECK(host.Submit(worlds.Source, 0, std::span(&first, 1), std::span(&route, 1), START) == 0);
	CHECK(worlds.Universe.PresentationQueueUsage().Messages == 2);
	const auto pumped = host.Pump(0, 0, START);
	CHECK(pumped.Requests == 2);
	CHECK(pumped.Rendered == 0);
	CHECK(pumped.Sent == 2);
	CHECK(worlds.Universe.PresentationQueueUsage().Messages == 0);
	CHECK(host.Image(0, core::Name("Door")) == 0);
	host.RemoveViewport(0);
	CHECK(host.Submit(worlds.Source, 0, std::span(&first, 1), std::span(&route, 1), START) == 1);
	host.RemoveWorld(worlds.Destination);
	CHECK(worlds.Universe.PresentationQueueUsage().Messages == 0);
	CHECK(host.Submit(worlds.Source, 2, std::span(&second, 1), std::span(&route, 1), START) == 1);
	CHECK(host.Pump(0, 0, START).Requests == 1);
	host.Clear();
	CHECK(
		worlds.Universe.OpenPresentation(worlds.Source, PortalReplyChannel(2)).Status ==
		world::PresentationStatus::Ok
	);
	CHECK(
		worlds.Universe.OpenPresentation(worlds.Destination, core::Name(PORTAL_REQUEST_CHANNEL)).Status ==
		world::PresentationStatus::Ok
	);
}

TEST_CASE(
	"portal host rejects mismatched demand and cancels invisible pending work", "[render][portal-host]"
) {
	Worlds worlds;
	Renderer renderer;
	PortalImageHost host(worlds.Universe, renderer);
	auto demand = worlds.Demand();
	const auto route = worlds.Route();
	CHECK(host.Submit(worlds.Source, 2, std::span(&demand, 1), std::span(&route, 1), START) == 0);
	CHECK(worlds.Universe.PresentationQueueUsage().Messages == 0);
	REQUIRE(host.Submit(worlds.Source, 0, std::span(&demand, 1), std::span(&route, 1), START) == 1);
	CHECK(host.Submit(worlds.Source, 0, {}, {}, START) == 0);
	CHECK(worlds.Universe.PresentationQueueUsage().Messages == 0);
	CHECK(host.Pump(0, 0, START).Sent == 0);
	CHECK(host.Image(0, core::Name("Door")) == 0);
	CHECK(host.Submit(worlds.Source, 0, std::span(&demand, 1), {}, START) == 0);
	CHECK(host.Submit(worlds.Source, 0, std::span(&demand, 1), std::span(&route, 1), START) == 1);
	host.RemoveWorld(worlds.Source);
	CHECK(worlds.Universe.PresentationQueueUsage().Messages == 0);
}

#include "RenderFixture.hpp"

#include <engine/render/WorldPresentation.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Interpolation.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Sunlight.hpp>

TEST_CASE(
	"portal host delivers distinct resident images and retires hidden viewports",
	"[render][gpu][portal-host][.]"
) {
	Worlds worlds;
	scene::RegisterSceneClasses();
	worlds.Universe.Enter(worlds.Destination, [](ecs::Store &store) { (void)scene::InstallServices(store); });
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	PortalImageHost host(worlds.Universe, fixture.Render);
	auto first = worlds.Demand();
	auto second = worlds.Demand(2);
	second.Request.Position[0] = 1;
	const auto route = worlds.Route();
	REQUIRE(host.Submit(worlds.Source, 0, std::span(&first, 1), std::span(&route, 1), START) == 1);
	REQUIRE(host.Submit(worlds.Source, 2, std::span(&second, 1), std::span(&route, 1), START) == 1);
	const auto pumped = host.Pump(0, 0, START);
	REQUIRE(pumped.Rendered == 2);
	REQUIRE(pumped.Sent == 2);
	const auto firstImage = host.Image(0, core::Name("Door"));
	const auto secondImage = host.Image(2, core::Name("Door"));
	REQUIRE(firstImage != 0);
	REQUIRE(secondImage != 0);
	CHECK(firstImage != secondImage);
	const auto captured = host.Capture(0, core::Name("Door"));
	const auto otherCapture = host.Capture(2, core::Name("Door"));
	REQUIRE(captured);
	REQUIRE(otherCapture);
	CHECK(captured->Image == firstImage);
	CHECK(otherCapture->Image == secondImage);
	CHECK(captured->Camera.Position == first.Request.Position);
	CHECK(otherCapture->Camera.Position == second.Request.Position);
	CHECK(captured->Binding.ViewSlot == 0);
	CHECK(otherCapture->Binding.ViewSlot == 2);
	CHECK_FALSE(host.Capture(1, core::Name("Door")));
	CHECK_FALSE(host.Capture(0, core::Name("Absent")));
	CHECK(host.CurrentImage(0, core::Name("Door")) == firstImage);
	first.Request.Key.CameraRevision++;
	first.Request.Position[0] = .25f;
	REQUIRE(host.Submit(worlds.Source, 0, std::span(&first, 1), std::span(&route, 1), START) == 1);
	CHECK(host.Image(0, core::Name("Door")) == firstImage);
	CHECK(host.CurrentImage(0, core::Name("Door")) == 0);
	const auto pendingCapture = host.Capture(0, core::Name("Door"));
	REQUIRE(pendingCapture);
	CHECK(pendingCapture->Image == captured->Image);
	CHECK(pendingCapture->Camera.Position == captured->Camera.Position);
	CHECK(pendingCapture->Binding.Expected == captured->Binding.Expected);
	REQUIRE(host.Pump(0, 0, START).Sent == 1);
	CHECK(host.CurrentImage(0, core::Name("Door")) != 0);
	const auto acceptedCapture = host.Capture(0, core::Name("Door"));
	REQUIRE(acceptedCapture);
	CHECK(acceptedCapture->Image == host.CurrentImage(0, core::Name("Door")));
	CHECK(acceptedCapture->Camera.Position == first.Request.Position);
	CHECK(acceptedCapture->Binding.Expected.CameraRevision == first.Request.Key.CameraRevision);
	CHECK(acceptedCapture->Binding.Expected.PortalKey == first.Request.Key.PortalKey);
	CHECK(acceptedCapture->Binding.Expected.RequestId != 0);
	CHECK(acceptedCapture->Binding.Expected.RequestId != captured->Binding.Expected.RequestId);
	CHECK(captured->Camera.Position[0] == 0);
	CHECK(fixture.Render.PortalImageUsage().Images == 2);
	CHECK(fixture.Render.PortalImageUsage().UploadedBytes == 0);
	host.RemoveViewport(0);
	CHECK(fixture.Render.PortalImageUsage().Images == 1);
	CHECK_FALSE(host.Capture(0, core::Name("Door")));
	REQUIRE(host.Capture(2, core::Name("Door")));
	CHECK(host.Image(2, core::Name("Door")) == secondImage);
	const auto expired = START + PortalInboxLimits{}.Timeout;
	(void)host.Pump(0, 0, expired);
	CHECK(host.CurrentImage(2, core::Name("Door")) == 0);
	CHECK(host.Image(2, core::Name("Door")) == 0);
	CHECK_FALSE(host.Capture(2, core::Name("Door")));
	(void)host.Submit(worlds.Source, 2, {}, {}, expired);
	CHECK(fixture.Render.PortalImageUsage().Images == 0);
	View eye;
	SceneTarget target{65, 65};
	eye.Target = &target;
	REQUIRE(host.SubmitEye(worlds.Source, route, eye, {.Width = 65, .Height = 65}, expired) == 1);
	REQUIRE(host.Pump(0, 0, expired).Sent == 1);
	eye.EyeImage = host.Image(eye.Slot, eye.EyeImageKey);
	REQUIRE(eye.EyeImage != 0);
	CHECK(host.SubmitEye(worlds.Source, {}, eye, {.Width = 65, .Height = 65}, expired) == 0);
	CHECK(eye.World == worlds.Source.Index);
	CHECK(eye.WorldName == worlds.Universe.NameOf(worlds.Source));
	CHECK(eye.Pipeline.IsValid());
	CHECK(eye.EyeImageKey.IsValid());
	CHECK(eye.EyeImage == 0);
	CHECK(host.Image(eye.Slot, eye.EyeImageKey) == 0);
	CHECK(fixture.Render.PortalImageUsage().Images == 0);
	OverlayImage overlay;
	(void)fixture.Render.Render(std::span(&eye, 1), overlay, nullptr, false);
	const auto missing = render::test::CaptureResource(
		fixture.Render, core::Name("composed-image"), eye.Slot, 65, 65, render::test::ImageFormat::Rgba8Unorm
	);
	bool black = true;
	for (uint32_t y = 0; y < missing.Height; ++y)
		for (uint32_t x = 0; x < missing.Width; ++x) {
			const auto *pixel = missing.Bytes.data() + y * missing.RowStrideBytes + x * 4;
			black = black && pixel[0] == std::byte{0} && pixel[1] == std::byte{0} && pixel[2] == std::byte{0};
		}
	CHECK(black);
	host.Clear();
}

TEST_CASE("resident portal images stay bounded through world replacement", "[render][gpu][portal-host][.]") {
	const bool reuseName = GENERATE(false, true);
	const bool retireSourceFirst = GENERATE(false, true);
	CAPTURE(reuseName, retireSourceFirst);
	Worlds worlds;
	scene::RegisterSceneClasses();
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	PortalImageHost host(worlds.Universe, fixture.Render);
	SceneTarget target{8, 8};
	View eye;
	eye.Target = &target;
	uint64_t previousImage = 0;
	std::optional<GpuMemoryStatistics> warmMemory;
	for (size_t generation = 0; generation < 2 * MAX_IMPORTED_PORTAL_IMAGES; ++generation) {
		CAPTURE(generation);
		worlds.Universe.Enter(worlds.Destination, [&](ecs::Store &store) {
			const auto workspace = scene::InstallServices(store);
			scene::PartDesc part;
			part.Frame.Position = {0, 0, -4};
			part.Size = {8, 8, .1f};
			const auto wall = scene::MakePart(store, part);
			REQUIRE(store.SetParent(wall, workspace));
			auto visual = *store.Get<scene::Visual>(wall);
			visual.Tint = generation % 2 ? core::Color3{0, 0, 1} : core::Color3{1, 0, 0};
			store.Set(wall, visual);
			store.SetResource(scene::Sun{{0, 0, 1}, {.5f, .5f, .5f}});
		});
		const auto now = START + std::chrono::milliseconds(generation);
		const auto submit = [&] {
			return host.SubmitEye(
				worlds.Source,
				worlds.Route(),
				eye,
				{.Width = 8, .Height = 8, .RecursionDepth = 0, .PixelBudget = 64},
				now
			);
		};
		if (retireSourceFirst) {
			REQUIRE(submit() == 1);
			host.RemoveWorld(worlds.Source);
			REQUIRE(worlds.Universe.Destroy(worlds.Source) == world::WorldStatus::Ok);
			worlds.Source = worlds.Universe.Create({.Name = core::Name("near")});
			CHECK(host.Image(eye.Slot, eye.EyeImageKey) == 0);
		}
		REQUIRE(submit() == 1);
		const auto pumped = host.Pump(0, 0, now);
		REQUIRE(pumped.Rendered == 1);
		REQUIRE(pumped.Sent == 1);
		eye.EyeImage = host.Image(eye.Slot, eye.EyeImageKey);
		REQUIRE(eye.EyeImage != 0);
		CHECK(eye.EyeImage != previousImage);
		previousImage = eye.EyeImage;
		OverlayImage overlay;
		(void)fixture.Render.Render(std::span(&eye, 1), overlay, nullptr, false);
		const auto image = render::test::CaptureResource(
			fixture.Render,
			core::Name("composed-image"),
			eye.Slot,
			8,
			8,
			render::test::ImageFormat::Rgba8Unorm
		);
		const auto format = fixture.Render.Backend().ColourFormat;
		const bool bgra = format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM ||
						  format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB;
		const size_t centre = 4 * image.RowStrideBytes + 4 * 4;
		const auto red = std::to_integer<uint8_t>(image.Bytes[centre + (bgra ? 2 : 0)]);
		const auto blue = std::to_integer<uint8_t>(image.Bytes[centre + (bgra ? 0 : 2)]);
		CHECK((generation % 2 ? blue > red : red > blue));
		CHECK(fixture.Render.PortalImageUsage().Images == 1);
		CHECK(fixture.Render.PortalImageUsage().UploadedBytes == 0);
		host.RemoveWorld(retireSourceFirst ? worlds.Source : worlds.Destination);
		CHECK(host.Image(eye.Slot, eye.EyeImageKey) == 0);
		CHECK(fixture.Render.PortalImageUsage().Images == 0);
		CHECK(fixture.Render.PortalImageUsage().TextureBytes == 0);
		CHECK_FALSE(fixture.Render.DropPortalImage(previousImage));
		if (retireSourceFirst) {
			host.RemoveWorld(worlds.Destination);
			REQUIRE(worlds.Universe.Destroy(worlds.Source) == world::WorldStatus::Ok);
			worlds.Source = worlds.Universe.Create({.Name = core::Name("near")});
		}
		const auto memory = fixture.Render.MemoryStatistics();
		if (generation == 4) warmMemory = memory;
		if (warmMemory) {
			CAPTURE(memory.LiveBytes, warmMemory->LiveBytes);
			CHECK(memory.LiveBytes <= warmMemory->LiveBytes + 3 * 8 * 8 * 8);
		}
		REQUIRE(worlds.Universe.Destroy(worlds.Destination) == world::WorldStatus::Ok);
		worlds.Destination = worlds.Universe.Create(
			{.Name = core::Name(reuseName ? "far replica" : "far replica." + std::to_string(generation + 1))}
		);
	}
	host.Clear();
}

#include <engine/core/Paths.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/parallel/Process.hpp>
#include <engine/parallel/ProcessChannel.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/world/HostLink.hpp>

namespace {
	void InstallProcessPlane(Renderer &renderer) {
		assets::MeshData mesh;
		mesh.Vertices = {
			{{-.5f, -.5f, 0}, {0, 0, 1}, {0, 1}},
			{{.5f, -.5f, 0}, {0, 0, 1}, {1, 1}},
			{{.5f, .5f, 0}, {0, 0, 1}, {1, 0}},
			{{-.5f, .5f, 0}, {0, 0, 1}, {0, 0}}
		};
		mesh.Indices = {0, 1, 2, 0, 2, 3, 2, 1, 0, 3, 2, 0};
		mesh.ComputeBounds();
		REQUIRE(renderer.AddMesh(core::Name("process-plane"), mesh));
		assets::TextureData white;
		white.Width = white.Height = 1;
		white.Format = assets::TextureFormat::RGBA8;
		white.Pixels.assign(4, std::byte{255});
		REQUIRE(renderer.AddTexture(core::Name("process-white"), white));
	}
}

TEST_CASE("portal image producer process child", "[.portal-image-child]") {
	using namespace engine;
	auto channel = parallel::AdoptInheritedChannel();
	REQUIRE(channel != nullptr);
	world::HostLink link(std::move(channel));
	world::Universe universe;
	const auto destination = universe.Create({.Name = core::Name("far")});
	universe.CreateRemote({.Name = core::Name("near")}, core::Name("parent"));
	REQUIRE(universe.ConfigurePresentation(456));
	scene::RegisterSceneClasses();
	ecs::Entity movingWall;
	universe.Enter(destination, [&](ecs::Store &store) {
		const auto workspace = scene::InstallServices(store);
		for (size_t index = 0; index < 2; ++index) {
			scene::PartDesc part;
			part.Frame.Position = {0, 0, index == 0 ? -4.0f : -5.0f};
			part.Size = {16, 16, .01f};
			part.Mesh = core::Name("process-plane");
			const auto wall = scene::MakePart(store, part);
			REQUIRE(store.SetParent(wall, workspace));
			auto visual = *store.Get<scene::Visual>(wall);
			visual.Tint = index == 0 ? core::Color3{1, 0, 0} : core::Color3{0, 1, 0};
			store.Set(wall, visual);
			auto appearance = *store.Get<scene::SurfaceAppearance>(wall);
			appearance.ColourMap = core::Name("process-white");
			store.Set(wall, appearance);
			if (index == 0) movingWall = wall;
		}
		store.SetResource(scene::Sun{{0, 0, 1}, {.5f, .5f, .5f}});
	});
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	InstallProcessPlane(fixture.Render);
	PortalImageHost host(universe, fixture.Render);
	const auto endpoint = host.Serve(destination);
	REQUIRE(endpoint.World == "far");
	CHECK(host.Serve(destination) == endpoint);
	world::HostFrame ready;
	ready.Signal = world::HostSignal::Ready;
	REQUIRE(link.Send(ready));
	size_t captures = 0, renewals = 0, requests = 0;
	bool withdrawing = false;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	while (std::chrono::steady_clock::now() < deadline) {
		std::vector<world::HostFrame> frames;
		link.Receive(frames);
		for (const auto &frame : frames) {
			if (frame.Signal == world::HostSignal::Stop) {
				if (!withdrawing) {
					host.Clear();
					REQUIRE(link.PublishPresentationDirectory(universe.LocalPresentationDirectory()));
					withdrawing = true;
					continue;
				}
				CHECK(captures == 2);
				CHECK(renewals == 1);
				return;
			}
			if (frame.Signal == world::HostSignal::PresentationDirectory) {
				REQUIRE(
					universe.ApplyPresentationDirectory(core::Name("parent"), frame.Directory) ==
					world::PresentationStatus::Ok
				);
				continue;
			}
			REQUIRE(frame.Signal == world::HostSignal::Presentation);
			if (++requests == 3) {
				universe.Enter(destination, [&](ecs::Store &store) {
					auto transform = *store.Get<scene::Transform>(movingWall);
					transform.Frame.Position.X = 100;
					store.Set(movingWall, transform);
				});
			}
			REQUIRE(
				universe.IngestPresentation(core::Name("parent"), frame.Presentation) ==
				world::PresentationStatus::Ok
			);
		}
		if (withdrawing) {
			SDL_Delay(1);
			continue;
		}
		const auto progress = host.Pump(0, 1, std::chrono::steady_clock::now());
		captures += progress.Rendered;
		renewals += progress.Reused;
		REQUIRE(link.PublishPresentationDirectory(universe.LocalPresentationDirectory()));
		for (const auto &outgoing : universe.TakePresentationOutbound()) {
			REQUIRE(link.SendPresentation(outgoing.Message));
		}
		SDL_Delay(1);
	}
	FAIL("portal image process did not receive stop before its deadline");
}

TEST_CASE(
	"portal host receives captured pixels and renewal from a real process", "[render][gpu][portal-host][.]"
) {
	using namespace engine;
	const bool wholeEye = GENERATE(false, true);
	world::Universe universe;
	const auto source = universe.Create({.Name = core::Name("near")});
	const auto destination = universe.CreateRemote({.Name = core::Name("far")}, core::Name("child"));
	REQUIRE(universe.ConfigurePresentation(123));
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	InstallProcessPlane(fixture.Render);
	PortalImageHost host(universe, fixture.Render);
	CHECK(host.Serve(destination).World.empty());
	CHECK(host.Serve({}).World.empty());
	auto pair = parallel::MakeProcessChannel();
	REQUIRE(pair.Valid());
	parallel::Process child;
	REQUIRE(child.Start(
		core::Paths::Base() / core::Paths::Program("test_render"),
		{"portal image producer process child"},
		std::move(pair.Remote)
	));
	world::HostLink link(std::move(pair.Local));
	const auto readyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	bool ready = false;
	while ((!ready || universe.LookupPresentation(destination, PORTAL_REQUEST_CHANNEL).World.empty()) &&
		   std::chrono::steady_clock::now() < readyDeadline) {
		std::vector<world::HostFrame> frames;
		link.Receive(frames);
		for (const auto &frame : frames) {
			if (frame.Signal == world::HostSignal::Ready)
				ready = true;
			else {
				REQUIRE(frame.Signal == world::HostSignal::PresentationDirectory);
				REQUIRE(
					universe.ApplyPresentationDirectory(core::Name("child"), frame.Directory) ==
					world::PresentationStatus::Ok
				);
			}
		}
		SDL_Delay(1);
	}
	REQUIRE(ready);
	REQUIRE_FALSE(universe.LookupPresentation(destination, PORTAL_REQUEST_CHANNEL).World.empty());
	PortalImageDemand demand;
	demand.DestinationWorld = core::Name("far");
	demand.Request.Key = {0, "Door", 1, 1};
	demand.Request.Width = demand.Request.Height = 32;
	demand.Request.PixelBudget = 1024;
	demand.Request.ClipPlane = {0, 0, -1, -1};
	demand.Binding.World = source.Index;
	demand.Binding.WorldName = core::Name("near");
	demand.Binding.Portal = core::Name("Door");
	const PortalImageDestination route{core::Name("far"), destination};
	auto document = graph::DefaultPbrDocument();
	document.Record(
		{.Kind = graph::EditKind::AddNode,
		 .Name = core::Name("process-check"),
		 .NodeKind = core::Name("capture"),
		 .Scope = graph::NodeScope::Frame}
	);
	document.Record(
		{.Kind = graph::EditKind::Reads, .Target = core::Name("portaled"), .Key = core::Name("source")}
	);
	graph::RenderGraph pipeline;
	core::Name offender;
	REQUIRE(graph::Build(document, pipeline, offender) == graph::PipelineDocumentStatus::Ok);
	REQUIRE(fixture.Render.SetPipeline(core::Name("process-source"), pipeline));
	scene::DrawInstance pane;
	pane.Source = 1;
	pane.Mesh = core::Name("process-plane");
	pane.Texture = core::Name("process-white");
	pane.HalfExtent = {1.5f, 1.2f, .01f};
	pane.Tint = {0, 0, 1};
	pane.Surface = 0;
	pane.CastShadow = false;
	PortalView portal;
	portal.ExternalImage = true;
	portal.ImagePortal = core::Name("Door");
	portal.Normal = {0, 0, 1};
	portal.First = {1.5f, 0, 0};
	portal.Second = {0, 1.2f, 0};
	SceneTarget target{65, 65};
	View view;
	view.World = source.Index;
	view.WorldName = core::Name("near");
	view.Pipeline = core::Name("process-source");
	view.Target = &target;
	view.Instances = std::span(&pane, 1);
	view.Portals = std::span(&portal, 1);
	view.CameraFrame.Position = {0, 0, 4};
	view.Camera.FieldOfViewRadians = 1.0471975512f;
	view.OverrideLighting = true;
	view.Lighting.Ambient = {0, 0, 2};
	view.Lighting.Direct = {};
	const auto imageKey = core::Name(wholeEye ? "viewport-eye" : "Door");
	const auto submit = [&] {
		if (wholeEye)
			return host.SubmitEye(
				source,
				route,
				view,
				{.Width = 32, .Height = 32, .RecursionDepth = 0, .PixelBudget = 1024},
				std::chrono::steady_clock::now()
			);
		return host.Submit(
			source, 0, std::span(&demand, 1), std::span(&route, 1), std::chrono::steady_clock::now()
		);
	};
	uint64_t originalImage = 0;
	for (size_t iteration = 0; iteration < 3; ++iteration) {
		REQUIRE(submit() == 1);
		REQUIRE(link.PublishPresentationDirectory(universe.LocalPresentationDirectory()));
		for (const auto &outgoing : universe.TakePresentationOutbound()) {
			REQUIRE(link.SendPresentation(outgoing.Message));
		}
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		bool received = false;
		while (!received && std::chrono::steady_clock::now() < deadline) {
			std::vector<world::HostFrame> frames;
			link.Receive(frames);
			for (const auto &frame : frames) {
				REQUIRE(frame.Signal == world::HostSignal::Presentation);
				std::string error;
				if (iteration != 1) {
					PortalImageReply image;
					REQUIRE(DecodePortalImageReply(frame.Presentation.Payload, image, error));
					REQUIRE(image.Status == PortalImageStatus::Ok);
					CHECK(image.Pixels.size() == 32 * 32 * 8);
					CHECK(image.Depth.size() == 32 * 32 * 4);
				} else {
					PortalResidentReceipt renewal;
					REQUIRE(DecodePortalImageRenewal(frame.Presentation.Payload, renewal, error));
				}
				REQUIRE(
					universe.IngestPresentation(core::Name("child"), frame.Presentation) ==
					world::PresentationStatus::Ok
				);
				received = true;
			}
			host.Pump(0, 1, std::chrono::steady_clock::now());
			SDL_Delay(1);
		}
		REQUIRE(received);
		REQUIRE(host.Image(0, imageKey) != 0);
		if (iteration == 0)
			originalImage = host.Image(0, imageKey);
		else if (iteration == 1)
			CHECK(host.Image(0, imageKey) == originalImage);
		portal.ImportedImage = host.Image(0, imageKey);
		if (wholeEye) view.EyeImage = host.Image(0, imageKey);
		OverlayImage overlay;
		const auto rendered = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
		if (!wholeEye) REQUIRE(rendered.SurfaceInstances > 0);
		const auto displayed = render::test::CaptureResource(
			fixture.Render,
			core::Name(wholeEye ? "composed-image" : "portaled"),
			0,
			65,
			65,
			render::test::ImageFormat::Rgba8Unorm
		);
		CAPTURE(iteration);
		CAPTURE(wholeEye, view.Pipeline.Text(), view.EyeImage);
		const double radiance = .5;
		const double mapped = radiance * (2.51 * radiance + .03) / (radiance * (2.43 * radiance + .59) + .14);
		const double gamma = std::pow(mapped, 1 / 2.2);
		const auto format = static_cast<SDL_GPUTextureFormat>(fixture.Render.Backend().ColourFormat);
		const bool bgra = wholeEye && (format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM ||
									   format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB);
		const bool srgb = !wholeEye || format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB ||
						  format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
		const int expected = int(std::lround((srgb ? 1.055 * std::pow(gamma, 1 / 2.4) - .055 : gamma) * 255));
		const size_t red = bgra ? 2 : 0, blue = bgra ? 0 : 2;
		const auto samples = wholeEye ? std::array<size_t, 3>{0, 32, 64} : std::array<size_t, 3>{31, 32, 33};
		for (const size_t y : samples) {
			for (const size_t x : samples) {
				const auto pixel = y * displayed.RowStrideBytes + x * 4;
				CHECK(
					std::abs(
						int(std::to_integer<uint8_t>(displayed.Bytes[pixel + (iteration == 2 ? 1 : red)])) -
						expected
					) <= 2
				);
				CHECK(std::to_integer<uint8_t>(displayed.Bytes[pixel + (iteration == 2 ? red : 1)]) <= 2);
				CHECK(std::to_integer<uint8_t>(displayed.Bytes[pixel + blue]) <= 2);
			}
		}
		CHECK(fixture.Render.PortalImageUsage().Uploads == (iteration == 2 ? 4 : 2));
	}
	world::HostFrame stop;
	stop.Signal = world::HostSignal::Stop;
	REQUIRE(link.Send(stop));
	const auto stopDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	bool withdrawn = false;
	auto ended = child.Poll();
	while (ended.Alive() && std::chrono::steady_clock::now() < stopDeadline) {
		std::vector<world::HostFrame> frames;
		link.Receive(frames);
		for (const auto &frame : frames) {
			REQUIRE(frame.Signal == world::HostSignal::PresentationDirectory);
			REQUIRE(frame.Directory.Endpoints.empty());
			REQUIRE(
				universe.ApplyPresentationDirectory(core::Name("child"), frame.Directory) ==
				world::PresentationStatus::Ok
			);
			CHECK(universe.LookupPresentation(destination, PORTAL_REQUEST_CHANNEL).World.empty());
			CHECK(submit() == 0);
			CHECK(host.Image(0, imageKey) == 0);
			withdrawn = true;
			REQUIRE(link.Send(stop));
		}
		SDL_Delay(1);
		ended = child.Poll();
	}
	REQUIRE_FALSE(ended.Alive());
	REQUIRE(withdrawn);
	CHECK(ended.Reason == parallel::ExitReason::Exited);
	CHECK(ended.Code == 0);
	host.Clear();
	CHECK(fixture.Render.PortalImageUsage().Images == 0);
}

TEST_CASE("standalone portal producer releases and reopens its endpoint", "[render][portal-host]") {
	Worlds worlds;
	Renderer renderer;
	PortalImageHost host(worlds.Universe, renderer);
	const auto first = host.Serve(worlds.Destination);
	REQUIRE(first.World == "far replica");
	CHECK(host.Serve(worlds.Destination) == first);
	CHECK(worlds.Universe.PresentationQueueUsage().Messages == 0);
	host.RemoveWorld(worlds.Destination);
	const auto second = host.Serve(worlds.Destination);
	REQUIRE(second.World == first.World);
	CHECK(second.Generation != first.Generation);
	host.Clear();
	const auto reopened =
		worlds.Universe.OpenPresentation(worlds.Destination, core::Name(PORTAL_REQUEST_CHANNEL));
	CHECK(reopened.Status == world::PresentationStatus::Ok);
}

TEST_CASE("portal host discovers registered remote endpoint generations", "[render][portal-host]") {
	world::Universe universe;
	const auto near = universe.Create({.Name = core::Name("near")});
	const auto far = universe.CreateRemote({.Name = core::Name("far")}, core::Name("far-host"));
	REQUIRE(universe.ConfigurePresentation(1));
	Renderer renderer;
	PortalImageHost host(universe, renderer);
	PortalImageDemand demand;
	demand.DestinationWorld = core::Name("far");
	demand.Request.Key = {0, "Door", 1, 1};
	demand.Request.Width = demand.Request.Height = 8;
	demand.Request.PixelBudget = 64;
	demand.Request.ClipPlane = {0, 0, -1, -1};
	demand.Binding.World = near.Index;
	demand.Binding.WorldName = core::Name("near");
	demand.Binding.Portal = core::Name("Door");
	const PortalImageDestination route{core::Name("far"), far};
	CHECK(host.Submit(near, 0, std::span(&demand, 1), std::span(&route, 1), START) == 0);
	world::PresentationAddress remote{"far", std::string(PORTAL_REQUEST_CHANNEL), 2, 1};
	for (size_t generation = 1; generation <= 2; ++generation) {
		remote.Generation = generation;
		REQUIRE(
			universe.RegisterRemotePresentation(core::Name("far-host"), remote) ==
			world::PresentationStatus::Ok
		);
		REQUIRE(host.Submit(near, 0, std::span(&demand, 1), std::span(&route, 1), START) == 1);
		const auto outgoing = universe.TakePresentationOutbound();
		REQUIRE(outgoing.size() == 1);
		CHECK(outgoing.front().Message.To == remote);
		REQUIRE(universe.ClosePresentation(remote) == world::PresentationStatus::Ok);
		CHECK(host.Submit(near, 0, std::span(&demand, 1), std::span(&route, 1), START) == 0);
	}
}

TEST_CASE("remote eyes exclude only their selected player body", "[render][gpu][portal-host][eye-body][.]") {
	const bool blended = GENERATE(false, true);
	Worlds worlds;
	scene::RegisterSceneClasses();
	worlds.Universe.Enter(worlds.Destination, [&](ecs::Store &store) {
		const auto workspace = scene::InstallServices(store);
		scene::PartDesc wall;
		wall.Frame.Position = {0, 0, -16};
		wall.Size = {40, 40, .1f};
		const auto backdrop = scene::MakePart(store, wall);
		REQUIRE(store.SetParent(backdrop, workspace));
		store.GetMutable<scene::Visual>(backdrop)->Tint = {.2f, 0, 0};
		for (const int id : {91, 92}) {
			const auto player = scene::AddPlayer(store, "viewer", false, id);
			scene::CharacterDesc character;
			character.Frame.Position = {id == 91 ? 0.f : 5.f, -2.5f, -10};
			character.TorsoColour = character.LegColour = character.SkinColour =
				id == 91 ? core::Color3{0, 0, 1} : core::Color3{0, 1, 0};
			const auto model = scene::MakeCharacter(store, character);
			REQUIRE(scene::SetPlayerCharacter(store, player, model));
			if (blended)
				store.EachDescendant(model, [&](ecs::Entity entity) {
					if (auto *visual = store.GetMutable<scene::Visual>(entity)) visual->Transparency = .5f;
				});
		}
		scene::PoseCharacters(store);
		scene::CapturePreviousTransforms(store);
		store.SetResource(scene::Sun{{0, 0, 1}, {.5f, .5f, .5f}});
	});
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	PortalImageHost host(worlds.Universe, fixture.Render);
	SceneTarget target{65, 65};
	View eye;
	eye.Target = &target;
	const auto format = fixture.Render.Backend().ColourFormat;
	const bool bgra =
		format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM || format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB;
	for (const int selected : {0, 91, 92, 91, 0}) {
		CAPTURE(blended, selected);
		eye.EyePlayer = selected == 0 ? std::nullopt : std::optional<int64_t>(selected);
		REQUIRE(
			host.SubmitEye(
				worlds.Source,
				worlds.Route(),
				eye,
				{.Width = 65, .Height = 65, .RecursionDepth = 0, .PixelBudget = 65 * 65},
				START
			) == 1
		);
		CHECK(host.CurrentImage(eye.Slot, eye.EyeImageKey) == 0);
		const auto pumped = host.Pump(0, 0, START);
		REQUIRE(pumped.Rendered == 1);
		REQUIRE(pumped.Sent == 1);
		eye.EyeImage = host.Image(eye.Slot, eye.EyeImageKey);
		REQUIRE(eye.EyeImage != 0);
		OverlayImage overlay;
		(void)fixture.Render.Render(std::span(&eye, 1), overlay, nullptr, false);
		const auto image = render::test::CaptureResource(
			fixture.Render,
			core::Name("composed-image"),
			eye.Slot,
			65,
			65,
			render::test::ImageFormat::Rgba8Unorm
		);
		const auto rgb = [&](uint32_t x) {
			const auto *pixel = image.Bytes.data() + 32 * image.RowStrideBytes + x * 4;
			return std::array{
				std::to_integer<int>(pixel[bgra ? 2 : 0]),
				std::to_integer<int>(pixel[1]),
				std::to_integer<int>(pixel[bgra ? 0 : 2])
			};
		};
		const auto centre = rgb(32), other = rgb(55);
		auto preview = image;
		if (bgra)
			for (uint32_t y = 0; y < preview.Height; ++y)
				for (uint32_t x = 0; x < preview.Width; ++x) {
					auto *pixel = preview.Bytes.data() + y * preview.RowStrideBytes + 4 * x;
					std::swap(pixel[0], pixel[2]);
				}
		render::test::WriteImagePreview(
			core::Paths::Base() / (std::string("eye-body-") + (blended ? "blended-" : "opaque-") +
								   std::to_string(selected) + ".ppm"),
			preview.View()
		);
		CAPTURE(centre, other);
		CHECK((selected == 91 ? centre[0] > centre[2] : centre[2] > centre[0]));
		CHECK((selected == 92 ? other[0] > other[1] : other[1] > other[0]));
		CHECK(fixture.Render.PortalImageUsage().Images == 1);
		CHECK(fixture.Render.PortalImageUsage().UploadedBytes == 0);
	}
}

TEST_CASE(
	"whole-eye hosts map crossing geometry from a distinct body world",
	"[render][gpu][portal-host][eye-geometry][.]"
) {
	Worlds worlds;
	scene::RegisterSceneClasses();
	const auto bodyWorld = worlds.Universe.Create({.Name = core::Name("body origin")});
	std::array<scene::DrawInstance, 1> body;
	core::CFrame destinationEye;
	core::Vector3 backdropPosition;
	worlds.Universe.Enter(bodyWorld, [&](ecs::Store &store) {
		const auto workspace = scene::InstallServices(store);
		scene::PartDesc pane;
		pane.Frame.Position = {20, 0, -6};
		pane.Size = {8, 8, .1f};
		const auto entrance = scene::MakePart(store, pane);
		REQUIRE(store.SetParent(entrance, workspace));
		pane.Frame.Position = {0, 0, -10};
		const auto exit = scene::MakePart(store, pane);
		REQUIRE(store.SetParent(exit, workspace));
		const auto portal = store.CreateInstance(ecs::Classes::Find(core::Name("Portal")), "Door");
		REQUIRE(store.SetParent(portal, entrance));
		auto link = *store.Get<scene::Portal>(portal);
		link.Destination = exit;
		link.DestinationWorld = core::Name("far");
		store.Set(portal, link);
		std::vector<scene::PortalSeam> seams;
		scene::GatherPortalSeams(store, seams);
		REQUIRE(seams.size() == 1);
		const auto player = scene::AddPlayer(store, "viewer", false, 91);
		const auto model = scene::LoadCharacter(store, player);
		const auto *rig = store.Get<scene::Character>(model);
		REQUIRE(rig != nullptr);
		body[0].Source = body[0].Rig = rig->Root.Id;
		body[0].Frame.Position = seams[0].Centre;
		body[0].HalfExtent = {1, 1, 1};
		body[0].Tint = {0, 0, 1};
		store.GetMutable<scene::Transform>(rig->Root)->Frame = body[0].Frame;
		const auto through = scene::SeamMapping(seams[0]);
		const auto centre = through.Point(body[0].Frame.Position);
		const auto farNormal = through.Rotate(seams[0].Normal) * -1.f;
		destinationEye = core::CFrame::LookAt(centre + farNormal * 10, centre);
		backdropPosition = centre - farNormal * 6;
	});
	worlds.Universe.Enter(worlds.Destination, [&](ecs::Store &store) {
		const auto workspace = scene::InstallServices(store);
		scene::PartDesc wall;
		wall.Frame.Position = backdropPosition;
		wall.Size = {40, 40, .1f};
		const auto backdrop = scene::MakePart(store, wall);
		REQUIRE(store.SetParent(backdrop, workspace));
		store.GetMutable<scene::Visual>(backdrop)->Tint = {.2f, 0, 0};
		scene::CapturePreviousTransforms(store);
		store.SetResource(scene::Sun{{0, 0, 1}, {.5f, .5f, .5f}});
	});
	worlds.Universe.Enter(bodyWorld, [&](ecs::Store &store) {
		std::vector<std::byte> bytes;
		std::string error;
		REQUIRE(CollectPortalEyeGeometry(store, core::Name("far"), body, {}, bytes, error));
		PortalGeometry geometry;
		REQUIRE(DecodePortalGeometry(bytes, geometry, error));
		REQUIRE(geometry.Rows.size() == 1);
		const auto &row = geometry.Rows[0];
		CHECK(row.Player == "91");
		CHECK(std::abs(row.Pose[0]) < .001f);
		CHECK(std::abs(row.Pose[1]) < .001f);
		CHECK(std::abs(row.Pose[2] + 10) < .1f);
	});

	render::test::FixtureDevice fixture;
	fixture.Initialise();
	PortalImageHost host(worlds.Universe, fixture.Render);
	SceneTarget target{65, 65};
	View eye;
	eye.Target = &target;
	eye.CameraFrame = destinationEye;
	uint64_t lastImage = 0;
	for (int phase = 0; phase < 6; ++phase) {
		CAPTURE(phase);
		eye.EyePlayer = phase == 2 ? std::optional<int64_t>(91) : std::nullopt;
		if (phase == 4) body[0].Frame.Position.X += 1000;
		if (phase == 5) {
			body[0].Frame.Position.X -= 1000;
			body[0].SkinCount = 1;
		}
		const PortalEyeGeometrySource geometry{
			bodyWorld, phase == 0 ? std::span<const scene::DrawInstance>{} : std::span(body), {}
		};
		const auto issued = host.SubmitEye(
			worlds.Source,
			worlds.Route(),
			eye,
			{.Width = 65, .Height = 65, .RecursionDepth = 0, .PixelBudget = 65 * 65},
			START,
			geometry
		);
		if (phase == 5) {
			CHECK(issued == 0);
			CHECK(eye.EyeImage == lastImage);
			CHECK(host.Pump(0, 1, START).Rendered == 0);
			continue;
		}
		REQUIRE(issued == 1);
		const auto pumped = host.Pump(0, 1, START);
		REQUIRE(pumped.Rendered == 1);
		REQUIRE(pumped.Sent == 1);
		eye.EyeImage = host.Image(eye.Slot, eye.EyeImageKey);
		REQUIRE(eye.EyeImage != 0);
		lastImage = eye.EyeImage;
		OverlayImage overlay;
		fixture.Render.Render(std::span(&eye, 1), overlay, nullptr, false);
		const auto image = render::test::CaptureResource(
			fixture.Render, core::Name("composed-image"), 0, 65, 65, render::test::ImageFormat::Bgra8Unorm
		);
		const auto *pixel = image.Bytes.data() + 32 * image.RowStrideBytes + 32 * 4;
		const int blue = std::to_integer<int>(pixel[0]), red = std::to_integer<int>(pixel[2]);
		CAPTURE(red, blue);
		CHECK((phase == 1 || phase == 3 ? blue > red : red > blue));
		CHECK(fixture.Render.PortalImageUsage().Images == 1);
		CHECK(fixture.Render.PortalImageUsage().UploadedBytes == 0);
		render::test::WriteImagePreview(
			core::Paths::Base() / ("eye-crossing-" + std::to_string(phase) + ".ppm"), image.View()
		);
	}
}

TEST_CASE(
	"portal host submits prefetched layer groups through an unrelated view",
	"[render][gpu][portal-host][layer-host][.]"
) {
	Worlds worlds;
	scene::RegisterSceneClasses();
	worlds.Universe.Enter(worlds.Destination, [](ecs::Store &store) { (void)scene::InstallServices(store); });
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	PortalImageHost host(worlds.Universe, fixture.Render);
	auto demand = worlds.Demand(2);
	demand.Request.OrderedLayers = true;
	demand.Request.Scope = PortalImageScope::OpaqueLighting;
	demand.Request.PixelBudget = 4 * 64;
	const auto route = worlds.Route();
	CHECK_FALSE(host.HasPendingUploads());
	REQUIRE(host.Submit(worlds.Source, 2, std::span(&demand, 1), std::span(&route, 1), START) == 1);
	for (unsigned attempt = 0; attempt < 1000 && !host.HasPendingUploads(); ++attempt)
		(void)host.Pump(0, 0, START);
	REQUIRE(host.HasPendingUploads());
	CHECK_FALSE(host.Capture(2, core::Name("Door")));
	SceneTarget target{8, 8};
	View view;
	view.World = worlds.Destination.Index;
	view.WorldName = core::Name("far replica");
	view.Target = &target;
	OverlayImage overlay;
	REQUIRE(
		fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("portal-capture"))
	);
	CHECK(fixture.Render.PortalImageUsage().PendingCpuBytes == 0);
	(void)host.Pump(0, 0, START);
	CHECK_FALSE(host.HasPendingUploads());
	const auto capture = host.Capture(2, core::Name("Door"));
	REQUIRE(capture);
	CHECK(capture->TransparentImages[0] != 0);
	CHECK(capture->TransparentImages[1] != 0);
	CHECK(capture->Binding.ViewSlot == 2);
	view.World = worlds.Source.Index;
	view.WorldName = core::Name("near");
	view.Slot = 2;
	const auto composed = host.ComposeBodyImage(core::Name("Door"), view);
	REQUIRE(composed != 0);
	CHECK(host.Image(2, core::Name("Door")) == capture->Image);
	CHECK(fixture.Render.PortalImageUsage().Images == 4);
	const auto replaced = host.ComposeBodyImage(core::Name("Door"), view);
	REQUIRE(replaced != 0);
	CHECK_FALSE(fixture.Render.DropPortalImage(composed));
	CHECK(fixture.Render.PortalImageUsage().Images == 4);
	view.WorldName = core::Name("wrong-body-owner");
	CHECK(host.ComposeBodyImage(core::Name("Door"), view) == 0);
	CHECK(fixture.Render.PortalImageUsage().Images == 4);
	const int retirement = GENERATE(0, 1, 2, 3, 4, 5);
	CAPTURE(retirement);
	switch (retirement) {
	case 0:
		host.RemoveViewport(2);
		break;
	case 1:
		host.RemoveWorld(worlds.Source);
		break;
	case 2:
		host.RemoveWorld(worlds.Destination);
		break;
	case 3:
		(void)host.Submit(worlds.Source, 2, {}, {}, START);
		break;
	case 4:
		(void)host.Pump(0, 0, START + PortalInboxLimits{}.Timeout);
		break;
	case 5:
		REQUIRE(worlds.Universe.ClosePresentation(capture->Producer) == world::PresentationStatus::Ok);
		(void)host.Pump(0, 0, START);
		break;
	}
	CHECK_FALSE(fixture.Render.DropPortalImage(replaced));
	CHECK_FALSE(host.Capture(2, core::Name("Door")));
	CHECK_FALSE(host.HasPendingUploads());
	CHECK(fixture.Render.PortalImageUsage().Images == 0);
}
