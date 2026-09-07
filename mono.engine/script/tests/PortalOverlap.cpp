#include <engine/ecs/Classes.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/physics/Query.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/PortalTransfer.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/PortalTransfer.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.script.portaloverlap")
TEST_DEPENDS("engine.script.portaltransfer")
TEST_DEPENDS("engine.physics.query")

using namespace engine;

TEST_CASE(
	"foreign static wall stops the leading shape before portal root crossing", "[script][portal-overlap]"
) {
	for (const bool character : {false, true}) {
		CAPTURE(character);
		scene::RegisterSceneClasses();
		world::Universe worlds;
		world::WorldSettings settings;
		settings.Name = core::Name("source");
		const auto source = worlds.Create(settings);
		settings.Name = core::Name("destination");
		const auto destination = worlds.Create(settings);
		ecs::Entity root, player;
		scene::SeamTransform through;
		scene::Collider moving;
		const float startZ = character ? .7f : 2.2f;
		const float displacement = -.5f;
		const core::CFrame before({0, 2.5f, startZ});
		worlds.Enter(source, [&](ecs::Store &store, ecs::Scheduler &scheduler) {
			scene::InstallServices(store);
			physics::PreparePhysicsWorld(store);
			REQUIRE(script::ConfigurePortalTransfers(store, 101));
			script::RegisterTeleportAdmission(scheduler);
			physics::RegisterPhysicsSystems(scheduler);
			const auto part = ecs::Classes::Find(core::Name("Part"));
			const auto pane = store.CreateInstance(part, "pane");
			store.Set(pane, scene::Transform{core::CFrame({0, 2.5f, 0})});
			store.Set(pane, scene::Bounds{{5, 5, .1f}});
			store.Remove<scene::Collider>(pane);
			const auto beyond = store.CreateInstance(part, "beyond");
			store.Set(beyond, scene::Transform{core::CFrame::LookAt({100, 2.5f, 0}, {100, 2.5f, 1})});
			store.Set(beyond, scene::Bounds{{5, 5, .1f}});
			store.Remove<scene::Collider>(beyond);
			const auto portal =
				store.CreateInstance(ecs::Classes::Find(core::Name("SurfaceCamera")), "portal");
			scene::SurfaceCamera surface;
			surface.Face = scene::NormalId::Back;
			surface.Surface = 0;
			store.Set(portal, surface);
			store.SetParent(portal, pane);
			scene::Portal link{beyond};
			link.DestinationWorld = core::Name("destination");
			store.Set(portal, link);
			std::vector<scene::PortalSeam> seams;
			REQUIRE(scene::GatherPortalSeams(store, seams) == 1);
			scene::PortalHop hop;
			size_t seam = 0;
			REQUIRE(scene::NearestPortalCrossing(seams, {0, 2.5f, 1}, {0, 2.5f, -1}, true, hop, seam));
			through = hop.Through;
			if (character) {
				player = scene::AddPlayer(store, "walker");
				const auto model = scene::LoadCharacter(store, player);
				root = store.Get<scene::Character>(model)->Root;
			} else {
				root = store.CreateInstance(part, "long cargo");
				store.SetParent(root, scene::WorkspaceOf(store));
			}
			moving.Extent = {.5f, 1, character ? .5f : 2};
			store.Set(root, moving);
			store.Set(root, scene::Bounds{moving.Extent});
			store.Set(root, scene::Transform{before});
			store.Set(root, scene::PreviousTransform{before});
			store.Set(root, scene::Motion{{0, 0, displacement * 60}, {}});
			store.Set(root, scene::Simulated{});
			scene::RigidBody body;
			body.Kind = scene::BodyKind::Dynamic;
			body.Mass = 1;
			store.Set(root, body);
			scene::PoseCharacters(store);
		});
		physics::PlacementSweep reference;
		worlds.Enter(destination, [&](ecs::Store &store, ecs::Scheduler &scheduler) {
			scene::InstallServices(store);
			physics::PreparePhysicsWorld(store);
			REQUIRE(script::ConfigurePortalTransfers(store, 202));
			script::RegisterTeleportAdmission(scheduler);
			const auto wall =
				store.CreateInstance(ecs::Classes::Find(core::Name("Part")), "destination wall");
			store.Set(wall, scene::Transform{core::CFrame({100, 2.5f, -.5f})});
			scene::Collider solid;
			solid.Extent = {5, 5, .1f};
			solid.CanQuery = false;
			store.Set(wall, solid);
			physics::SyncBroadphase(store);
			reference = physics::SweepPlacement(
				store, moving, through.Place(before), through.Carry({0, 0, displacement}), {}
			);
			REQUIRE(reference.Complete);
			REQUIRE(reference.Hit);
			REQUIRE(reference.Fraction > 0);
			REQUIRE(reference.Fraction < 1);
		});
		worlds.Tick(1.0f / 60);
		worlds.Enter(source, [&](ecs::Store &store) {
			REQUIRE(store.Alive(root));
			REQUIRE(store.Has<scene::Simulated>(root));
			CHECK_FALSE(
				(character ? script::PortalTransferOfPlayer(store, player).has_value()
						   : script::PortalTransferOfObject(store, root).has_value())
			);
			const auto position = store.Get<scene::Transform>(root)->Frame.Position;
			CHECK(position.Z > .1f);
			CHECK(position.Z >= startZ + displacement * reference.Fraction - .001f);
		});
		worlds.Enter(destination, [&](ecs::Store &store) {
			size_t simulated = 0;
			store.Each<const scene::Simulated>([&](ecs::Entity, const scene::Simulated &) { ++simulated; });
			REQUIRE(simulated == 0);
		});
		core::Vector3 held;
		worlds.Enter(source, [&](ecs::Store &store) {
			held = store.Get<scene::Transform>(root)->Frame.Position;
			store.Set(root, scene::Motion{{0, 0, displacement * 60}, {}});
		});
		REQUIRE(worlds.Destroy(destination) == world::WorldStatus::Ok);
		worlds.Tick(1.0f / 60);
		worlds.Enter(source, [&](ecs::Store &store) {
			CHECK(store.Get<scene::Transform>(root)->Frame.Position == held);
			CHECK(store.Get<scene::Motion>(root)->Linear.MagnitudeSquared() == 0);
		});
	}
}
