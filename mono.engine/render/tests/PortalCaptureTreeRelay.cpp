#include "RenderFixture.hpp"

#include <engine/assets/Mesh.hpp>
#include <engine/parallel/Channel.hpp>
#include <engine/render/PortalCaptureTreeImport.hpp>
#include <engine/render/PortalImageHost.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/HostLink.hpp>
#include <engine/world/PresentationRelay.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/generators/catch_generators.hpp>

TEST_SUITE_ID("engine.render.portalcapturetreerelay")
TEST_DEPENDS("engine.world.presentationrelay")
TEST_DEPENDS("engine.render.portalimagehost")

namespace {
	using namespace engine;
	using namespace engine::render;
	PortalImageRequest RecursiveRequest(uint64_t revision = 1, bool nested = false) {
		PortalImageRequest request;
		request.Key = {0, "Door", revision, 1};
		request.Scope = PortalImageScope::OpaqueLighting;
		request.OrderedLayers = true;
		request.RecursionDepth = 1;
		request.Projection = PortalImageProjection::Eye;
		request.ClipPlane = {};
		request.Frustum = {-.1f, .1f, -.1f, .1f, .1f, 100};
		request.Width = request.Height = 8;
		// Four captures per room include overflow; neither room authors top GUI.
		request.PixelBudget = (nested ? 2 : 1) * 4 * 8 * 8;
		return request;
	}
	PortalImageBinding Binding(world::Universe &universe, world::WorldId owner) {
		PortalImageBinding binding;
		binding.World = owner.Index;
		binding.WorldName = universe.NameOf(owner);
		binding.Portal = core::Name("Door");
		return binding;
	}
}

TEST_CASE(
	"delegated capture trees cross the real host relay with published endpoint ownership",
	"[render][gpu][portal-tree-relay][.]"
) {
	const bool nested = GENERATE(false, true);
	CAPTURE(nested);
	world::Universe parent, child;
	const auto near = parent.Create({.Name = core::Name("relay-near")});
	const auto far = parent.Create({.Name = core::Name("relay-far")});
	const auto replica = child.Create({.Name = core::Name("relay-far")});
	REQUIRE(parent.ConfigurePresentation(100));
	REQUIRE(child.ConfigurePresentation(200));
	scene::RegisterSceneClasses();
	child.Enter(replica, [](ecs::Store &store) { scene::InstallServices(store); });
	test::FixtureDevice device;
	device.Initialise();
	world::WorldId deep;
	world::PresentationAddress deepEndpoint;
	std::unique_ptr<PortalImageProducer> deepProducer;
	if (nested) {
		deep = parent.Create({.Name = core::Name("relay-deep")});
		parent.Enter(deep, [](ecs::Store &store) { scene::InstallServices(store); });
		deepEndpoint = parent.OpenPresentation(deep, core::Name(PORTAL_REQUEST_CHANNEL)).Address;
		REQUIRE(deepEndpoint.Generation != 0);
		deepProducer = std::make_unique<PortalImageProducer>(parent, device.Render, deep, deepEndpoint);
		assets::MeshData plane;
		plane.Vertices = {
			{{-1, -1, 0}, {0, 0, 1}, {0, 1}},
			{{1, -1, 0}, {0, 0, 1}, {1, 1}},
			{{1, 1, 0}, {0, 0, 1}, {1, 0}},
			{{-1, 1, 0}, {0, 0, 1}, {0, 0}}
		};
		plane.Indices = {0, 1, 2, 0, 2, 3};
		plane.ComputeBounds();
		REQUIRE(device.Render.AddMesh(core::Name("relay-mouth"), plane));
		child.Enter(replica, [](ecs::Store &store) {
			const auto workspace = scene::InstallServices(store);
			scene::PartDesc part;
			part.Frame.Position = {0, 0, -2};
			part.Size = {4, 4, .01f};
			part.Mesh = core::Name("relay-mouth");
			const auto entrance = scene::MakePart(store, part);
			REQUIRE(store.SetParent(entrance, workspace));
			part.Frame = core::CFrame::Angles(0, 3.14159265358979323846f, 0);
			const auto exit = scene::MakePart(store, part);
			auto hidden = *store.Get<scene::Visual>(exit);
			hidden.Transparency = 1;
			store.Set(exit, hidden);
			const auto entity = store.CreateInstance(ecs::Classes::Find(core::Name("Portal")), "Nested");
			REQUIRE(store.SetParent(entity, entrance));
			auto portal = *store.Get<scene::Portal>(entity);
			portal.Destination = exit;
			portal.DestinationWorld = core::Name("relay-deep");
			store.Set(entity, portal);
		});
	}
	PortalImageHost childHost(child, device.Render);
	auto channel = parallel::MakeLocalChannel({1024 * 1024, 1024 * 1024});
	world::HostLink driverLink(std::move(channel.first)), childLink(std::move(channel.second));
	std::vector<std::string> channels{
		std::string(PORTAL_REQUEST_CHANNEL), std::string(PORTAL_TOPOLOGY_REQUESTS)
	};
	// Match ServerPresentation's bounded producer delegation, including child reply slots.
	for (size_t slot = 0; slot < 16; ++slot)
		channels.push_back("portal-image-replies/nested/" + std::to_string(slot));
	world::PresentationRelay relay(parent, far, 200, std::move(channels));
	auto localProducer = childHost.Serve(replica);
	REQUIRE(localProducer.Session == 200);
	const auto replies = parent.OpenPresentation(near, PortalReplyChannel()).Address;
	REQUIRE(replies.Generation != 0);
	PortalImageSource source(parent, device.Render, near, replies);
	const auto now = [] { return std::chrono::steady_clock::now(); };

	// Enabling the inherited link must gate recursive exports until its binding arrives.
	REQUIRE(childHost.PumpDriverLink(childLink));
	const auto probeReplies = child.OpenPresentation(replica, PortalReplyChannel()).Address;
	{
		PortalImageSource probe(
			child,
			device.Render,
			replica,
			probeReplies,
			{},
			nullptr,
			PortalImageSourceDelivery::CapturePayloads
		);
		REQUIRE(
			probe.Issue(localProducer, RecursiveRequest(), Binding(child, replica), now()).Status ==
			PortalInboxStatus::Issued
		);
		const auto progress = childHost.Pump(0, 0, now());
		CHECK(progress.Rendered == 0);
		const auto refused = probe.Poll(now());
		REQUIRE(refused.size() == 1);
		CHECK(refused[0].Status == PortalImageStatus::Unavailable);
		CHECK(refused[0].Diagnostic == "delegated capture endpoint binding unavailable");
		CHECK_FALSE(probe.TakeTree("Door", now()));
	}
	REQUIRE(child.ClosePresentation(probeReplies) == world::PresentationStatus::Ok);
	const auto control = [&] {
		REQUIRE(childHost.PumpDriverLink(childLink));
		REQUIRE(relay.Pump(driverLink));
		REQUIRE(childHost.PumpDriverLink(childLink));
	};
	control();
	SceneTarget target{8, 8};
	View upload;
	upload.World = near.Index;
	upload.WorldName = parent.NameOf(near);
	upload.Target = &target;
	OverlayImage overlay;
	uint64_t previousTree = 0;
	world::PresentationAddress previousPublished;
	for (uint64_t generation = 1; generation <= 2; ++generation) {
		CAPTURE(generation);
		const auto published = parent.LookupPresentation(far, PORTAL_REQUEST_CHANNEL);
		REQUIRE(published.Session == 100);
		CHECK(published != localProducer);
		if (generation > 1) CHECK(published != previousPublished);
		REQUIRE(
			source.Issue(published, RecursiveRequest(generation, nested), Binding(parent, near), now())
				.Status == PortalInboxStatus::Issued
		);
		std::vector<PortalRuntimeCompletion> completed;
		const auto deadline = now() + std::chrono::seconds(10);
		while (completed.empty() && now() < deadline) {
			REQUIRE(relay.Pump(driverLink));
			REQUIRE(childHost.PumpDriverLink(childLink));
			childHost.Pump(0, 0, now());
			REQUIRE(childHost.PumpDriverLink(childLink));
			REQUIRE(relay.Pump(driverLink));
			if (deepProducer) deepProducer->Pump(0, 0, now());
			completed = source.Poll(now());
			if (source.HasPendingUploads())
				device.Render.Render(std::span(&upload, 1), overlay, nullptr, false);
			if (completed.empty()) completed = source.Poll(now());
			if (completed.empty()) SDL_Delay(1);
		}
		REQUIRE(completed.size() == 1);
		INFO(completed[0].Diagnostic);
		REQUIRE(completed[0].Status == PortalImageStatus::Ok);
		const auto capture = source.Capture("Door");
		REQUIRE(capture);
		REQUIRE(capture->Tree != 0);
		CHECK(capture->Tree != previousTree);
		CHECK(capture->Producer == published);
		const auto *tree = device.Render.FindPortalCaptureTree(capture->Tree);
		REQUIRE(tree);
		REQUIRE(tree->Nodes.size() == (nested ? 2 : 1));
		CHECK(tree->Edges.size() == (nested ? 1 : 0));
		const auto &root = tree->Nodes.front();
		CHECK(root.Producer.World == published.World);
		CHECK(root.Producer.Channel == published.Channel);
		CHECK(root.Producer.Session == published.Session);
		CHECK(root.Producer.Generation == published.Generation);
		CHECK(root.Producer.Session != localProducer.Session);
		CHECK(root.Images[0] == capture->Image);
		if (nested) {
			const auto &descendant = tree->Nodes[1].Producer;
			CHECK(descendant.World == deepEndpoint.World);
			CHECK(descendant.Channel == deepEndpoint.Channel);
			CHECK(descendant.Session == deepEndpoint.Session);
			CHECK(descendant.Generation == deepEndpoint.Generation);
			CHECK(descendant.World != root.Producer.World);
			CHECK(tree->Edges[0].Parent == 0);
			CHECK(tree->Edges[0].Child == 1);
		}
		CHECK(source.CurrentImage("Door") == capture->Image);
		CHECK(relay.Refused() == 0);
		previousTree = capture->Tree;
		previousPublished = published;
		if (nested && generation == 2) {
			deepProducer.reset();
			REQUIRE(parent.ClosePresentation(deepEndpoint) == world::PresentationStatus::Ok);
			source.Poll(now());
			CHECK_FALSE(source.Capture("Door"));
			CHECK_FALSE(device.Render.PortalCaptureTreeReady(previousTree));
			CHECK(parent.LookupPresentation(far, PORTAL_REQUEST_CHANNEL) == published);
		}
		childHost.RemoveWorld(replica);
		control();
		CHECK(parent.LookupPresentation(far, PORTAL_REQUEST_CHANNEL).Generation == 0);
		source.Poll(now());
		CHECK_FALSE(source.Capture("Door"));
		CHECK_FALSE(device.Render.PortalCaptureTreeReady(previousTree));
		CHECK(device.Render.PortalImageUsage().Images == 0);
		if (generation == 1) {
			localProducer = childHost.Serve(replica);
			REQUIRE(localProducer.Generation != 0);
			control();
		}
	}
}
