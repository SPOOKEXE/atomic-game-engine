#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/render/PortalImageHost.hpp>
#include <engine/render/PortalTopologyHost.hpp>
#include <engine/scene/CameraPortalTopology.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.render.portaltopologyhost")
using namespace engine;
using namespace std::chrono_literals;
namespace {
	constexpr render::PortalTopologyHost::Time START{};
	ecs::Entity Mouth(world::Universe &universe, world::WorldId world) {
		ecs::Entity camera;
		universe.Enter(world, [&](ecs::Store &store) {
			const auto pane = store.CreateInstance(ecs::Classes::Find(core::Name("Part")), "Door");
			const auto far = store.CreateInstance(ecs::Classes::Find(core::Name("Part")), "StandIn");
			store.Set(pane, scene::Transform{});
			store.Set(pane, scene::Bounds{{2, 3, .1f}});
			store.Set(far, scene::Transform{core::CFrame(core::Vector3{10, 0, 0})});
			store.Set(far, scene::Bounds{{4, 6, .2f}});
			camera = store.CreateInstance(ecs::Classes::Find(core::Name("Portal")), "Portal");
			store.SetParent(camera, pane);
			REQUIRE(store.ParentOf(camera) == pane);
			store.Set(camera, scene::SurfaceCamera{});
			store.Set(camera, scene::Portal{.Destination = far, .DestinationWorld = core::Name("near")});
		});
		return camera;
	}
}

TEST_CASE("topology requests restart without extending cached expiry", "[render][portal-transport-restart]") {
	scene::RegisterSceneClasses();
	world::Universe universe;
	const auto near = universe.Create({.Name = core::Name("near")});
	const auto far = universe.Create({.Name = core::Name("far")});
	REQUIRE(universe.ConfigurePresentation(123));
	Mouth(universe, far);
	render::PortalTopologyHost host(universe);
	REQUIRE(host.Request(near, far, START));
	host.Pump(START);
	const auto *snapshot = host.Snapshot(far, START);
	REQUIRE(snapshot);
	const auto *seams = snapshot->Seams.data();
	REQUIRE(host.Request(near, far, START + 250ms));
	host.RestartRequests();
	REQUIRE(host.Snapshot(far, START + 250ms));
	CHECK(host.Snapshot(far, START + 250ms)->Seams.data() == seams);
	REQUIRE(host.Request(near, far, START + 250ms));
	SECTION("no reply keeps the original expiry") {
		CHECK(host.Snapshot(far, START + 1000ms) == nullptr);
	}
	SECTION("the new reply renews the retained snapshot") {
		host.Pump(START + 250ms);
		REQUIRE(host.Snapshot(far, START + 1000ms));
		CHECK(host.Snapshot(far, START + 1000ms)->Seams.data() == seams);
	}
}

TEST_CASE(
	"a current topology renewal can restore an expired snapshot",
	"[render][portal-topology-host][portal-topology-renewal]"
) {
	scene::RegisterSceneClasses();
	world::Universe universe;
	const auto near = universe.Create({.Name = core::Name("near")});
	const auto far = universe.Create({.Name = core::Name("far")});
	REQUIRE(universe.ConfigurePresentation(123));
	Mouth(universe, far);
	render::PortalTopologyHost host(universe);
	REQUIRE(host.Request(near, far, START));
	host.Pump(START);
	const auto *initial = host.Snapshot(far, START);
	REQUIRE(initial);
	const auto revision = initial->Revision;
	const auto *seams = initial->Seams.data();
	REQUIRE(host.Request(near, far, START + 250ms));
	CHECK(host.Snapshot(far, START + 1000ms) == nullptr);
	SECTION("renewal arrives within its request deadline") {
		host.Pump(START + 1000ms);
		const auto *renewed = host.Snapshot(far, START + 1000ms);
		REQUIRE(renewed);
		CHECK(renewed->Revision == revision);
		CHECK(renewed->Seams.data() == seams);
		CHECK(host.Snapshot(far, START + 1999ms) != nullptr);
		CHECK(host.Snapshot(far, START + 2000ms) == nullptr);
	}
	SECTION("an expired request cannot revive the snapshot") {
		host.Pump(START + 1250ms);
		CHECK(host.Snapshot(far, START + 1250ms) == nullptr);
	}
}

TEST_CASE(
	"topology host shares snapshots and renews without another seam payload", "[render][portal-topology-host]"
) {
	scene::RegisterSceneClasses();
	world::Universe universe;
	const auto near = universe.Create({.Name = core::Name("near")});
	const auto far = universe.Create({.Name = core::Name("far")});
	REQUIRE(universe.ConfigurePresentation(123));
	const auto mouth = Mouth(universe, far);
	render::PortalTopologyHost host(universe);
	CHECK(host.Snapshot(far, START) == nullptr);
	REQUIRE(host.Request(near, far, START));
	CHECK_FALSE(host.Request(near, far, START));
	host.Pump(START);
	const auto *snapshot = host.Snapshot(far, START);
	REQUIRE(snapshot != nullptr);
	REQUIRE(snapshot->Seams.size() == 1);
	CHECK(snapshot->Seams[0].Scale == 2);
	CHECK(snapshot->Seams[0].Camera == ecs::NULL_ENTITY);
	CHECK(snapshot->Seams[0].DestinationWorld == core::Name("near"));
	const auto *allocation = snapshot->Seams.data();
	const auto revision = snapshot->Revision;
	const auto bytes = universe.PresentationTrafficCounts().EnqueuedBytes;
	REQUIRE(host.Request(near, far, START + 250ms));
	host.Pump(START + 250ms);
	snapshot = host.Snapshot(far, START + 250ms);
	REQUIRE(snapshot != nullptr);
	CHECK(snapshot->Revision == revision);
	CHECK(snapshot->Seams.data() == allocation);
	CHECK(universe.PresentationTrafficCounts().EnqueuedBytes - bytes == 24);
	universe.Enter(far, [&](ecs::Store &store) {
		auto portal = *store.Get<scene::Portal>(mouth);
		portal.Enabled = false;
		store.Set(mouth, portal);
	});
	REQUIRE(host.Request(near, far, START + 500ms));
	host.Pump(START + 500ms);
	snapshot = host.Snapshot(far, START + 500ms);
	REQUIRE(snapshot != nullptr);
	CHECK(snapshot->Revision > revision);
	CHECK(snapshot->Seams.empty());
	CHECK(host.Snapshot(far, START + 1500ms) == nullptr);
	CHECK(host.Snapshot(far, START) == nullptr);
	host.Clear();
	CHECK(universe.LocalPresentationDirectory().Endpoints.empty());
	CHECK(universe.PresentationQueueUsage().Messages == 0);
}

TEST_CASE(
	"remote topology is bound to authenticated endpoints and world names", "[render][portal-topology-host]"
) {
	scene::RegisterSceneClasses();
	world::Universe consumer;
	world::Universe producer;
	const auto near = consumer.Create({.Name = core::Name("near")});
	const auto remoteFar = consumer.CreateRemote({.Name = core::Name("far")}, core::Name("producer"));
	const auto far = producer.Create({.Name = core::Name("far")});
	producer.CreateRemote({.Name = core::Name("near")}, core::Name("consumer"));
	REQUIRE(consumer.ConfigurePresentation(123));
	REQUIRE(producer.ConfigurePresentation(456));
	Mouth(producer, far);
	render::PortalTopologyHost source(consumer), destination(producer);
	CHECK_FALSE(source.Request(near, remoteFar, START));
	const auto endpoint = destination.Serve(far);
	REQUIRE(
		consumer.RegisterRemotePresentation(core::Name("producer"), endpoint) == world::PresentationStatus::Ok
	);
	REQUIRE(source.Request(near, remoteFar, START));
	const auto replyEndpoint = consumer.LookupPresentation(near, render::PORTAL_TOPOLOGY_REPLIES);
	REQUIRE(
		producer.RegisterRemotePresentation(core::Name("consumer"), replyEndpoint) ==
		world::PresentationStatus::Ok
	);
	const auto requests = consumer.TakePresentationOutbound();
	REQUIRE(requests.size() == 1);
	REQUIRE(
		producer.IngestPresentation(core::Name("consumer"), requests[0].Message) ==
		world::PresentationStatus::Ok
	);
	destination.Pump(START);
	auto replies = producer.TakePresentationOutbound();
	REQUIRE(replies.size() == 1);
	bool valid = true;
	SECTION("correct world") {}
	SECTION("different world payload") {
		scene::CameraPortalTopology topology;
		std::string error;
		REQUIRE(scene::DecodeCameraPortalTopology(replies[0].Message.Payload, topology, error));
		topology.World = "another-world";
		REQUIRE(scene::EncodeCameraPortalTopology(topology, replies[0].Message.Payload, error));
		valid = false;
	}
	REQUIRE(
		consumer.IngestPresentation(core::Name("producer"), replies[0].Message) ==
		world::PresentationStatus::Ok
	);
	source.Pump(START);
	CHECK((source.Snapshot(remoteFar, START) != nullptr) == valid);
	consumer.RetirePresentationHost(core::Name("producer"));
	CHECK(source.Snapshot(remoteFar, START) == nullptr);
	source.Pump(START);
	CHECK_FALSE(source.Request(near, remoteFar, START));
	source.Clear();
	destination.Clear();
	CHECK(consumer.LocalPresentationDirectory().Endpoints.empty());
	CHECK(producer.LocalPresentationDirectory().Endpoints.empty());
}

TEST_CASE(
	"product portal host publishes topology and replaces retired endpoints", "[render][portal-topology-host]"
) {
	scene::RegisterSceneClasses();
	world::Universe universe;
	const auto near = universe.Create({.Name = core::Name("near")});
	const auto far = universe.Create({.Name = core::Name("far")});
	REQUIRE(universe.ConfigurePresentation(123));
	Mouth(universe, far);
	render::Renderer renderer;
	render::PortalImageHost host(universe, renderer);
	REQUIRE(host.Serve(far).Generation != 0);
	const auto first = universe.LookupPresentation(far, render::PORTAL_TOPOLOGY_REQUESTS);
	REQUIRE(first.Generation != 0);
	REQUIRE(host.RequestTopology(near, far, START));
	host.Pump(0, 0, START);
	REQUIRE(host.Topology(far, START) != nullptr);
	REQUIRE(universe.ClosePresentation(first) == world::PresentationStatus::Ok);
	CHECK(host.Topology(far, START) == nullptr);
	REQUIRE(host.Serve(far).Generation != 0);
	CHECK(universe.LookupPresentation(far, render::PORTAL_TOPOLOGY_REQUESTS).Generation != first.Generation);
	REQUIRE(host.RequestTopology(near, far, START));
	host.Pump(0, 0, START);
	REQUIRE(host.Topology(far, START) != nullptr);
	host.RemoveWorld(far);
	CHECK(host.Topology(far, START) == nullptr);
	host.Clear();
	CHECK(universe.LocalPresentationDirectory().Endpoints.empty());
}

TEST_CASE("topology replies recover after transport backpressure", "[render][portal-topology-host]") {
	scene::RegisterSceneClasses();
	world::Universe universe;
	const auto near = universe.Create({.Name = core::Name("near")});
	const auto far = universe.Create({.Name = core::Name("far")});
	world::PresentationLimits limits;
	limits.MessagesPerEndpoint = 1;
	REQUIRE(universe.ConfigurePresentation(123, limits));
	render::PortalTopologyHost host(universe);
	const auto producer = host.Serve(far);
	REQUIRE(host.Request(near, far, START));
	const auto replies = universe.LookupPresentation(near, render::PORTAL_TOPOLOGY_REPLIES);
	const std::array<std::byte, 1> junk{std::byte{0}};
	REQUIRE(universe.SendPresentation(far, producer, replies, 999, junk) == world::PresentationStatus::Ok);
	host.Pump(START);
	CHECK(host.Snapshot(far, START) == nullptr);
	CHECK_FALSE(host.Request(near, far, START + 999ms));
	REQUIRE(host.Request(near, far, START + 1s));
	host.Pump(START + 1s);
	REQUIRE(host.Snapshot(far, START + 1s) != nullptr);
	CHECK(host.Snapshot(far, START + 1s)->Seams.empty());
}
