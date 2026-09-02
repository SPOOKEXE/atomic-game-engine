// Rigid assemblies built from both direct WeldConstraints and legacy Welds.

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/physics/Welds.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Constraints.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.physics.welds")

using namespace engine;

namespace {
	struct World {
		ecs::Store Store{"physics_welds"};
		ecs::Entity Workspace;

		World() {
			scene::RegisterSceneClasses();
			Workspace = scene::InstallServices(Store);
			physics::PreparePhysicsWorld(Store, 4.0f);
		}

		ecs::Entity Part(const char *name, const core::Vector3 &position, bool anchored = false) {
			const ecs::Entity part = Store.CreateInstance(scene::PartClass(), name);
			Store.SetParent(part, Workspace);
			Store.Set(part, scene::Transform{core::CFrame(position)});
			if (!anchored) {
				Store.Set(part, scene::Simulated{});
				Store.Set(part, scene::Motion{});
			}
			return part;
		}
	};
}

TEST_CASE("WeldConstraint captures and preserves the current relative pose", "[physics][welds]") {
	World world;
	const ecs::Entity first = world.Part("First", {0.0f, 0.0f, 0.0f});
	const ecs::Entity second = world.Part("Second", {3.0f, 1.0f, 0.0f});
	const ecs::Entity weld =
		world.Store.CreateInstance(ecs::Classes::Find(core::Name("WeldConstraint")), "WeldConstraint");
	world.Store.SetParent(weld, first);
	world.Store.Set(weld, scene::WeldConstraint{first, second});

	physics::SolveRigidJoints(world.Store);
	world.Store.GetMutable<scene::Transform>(first)->Frame.Position = {5.0f, 2.0f, 0.0f};
	physics::SolveRigidJoints(world.Store);

	const core::Vector3 position = world.Store.Get<scene::Transform>(second)->Frame.Position;
	CHECK(position.X == 8.0f);
	CHECK(position.Y == 3.0f);
	CHECK(world.Store.Resource<physics::PhysicsWorld>()->RigidlyConnected(first, second));
}

TEST_CASE("legacy Weld uses C0 and C1 against an anchored root", "[physics][welds]") {
	World world;
	const ecs::Entity root = world.Part("Root", {10.0f, 0.0f, 0.0f}, true);
	const ecs::Entity child = world.Part("Child", {0.0f, 0.0f, 0.0f});
	const ecs::Entity weld = world.Store.CreateInstance(ecs::Classes::Find(core::Name("Weld")), "Weld");
	world.Store.SetParent(weld, root);

	scene::JointInstance joint;
	joint.Part0 = root;
	joint.Part1 = child;
	joint.C0 = core::CFrame(core::Vector3{2.0f, 0.0f, 0.0f});
	joint.C1 = core::CFrame(core::Vector3{0.5f, 0.0f, 0.0f});
	world.Store.Set(weld, joint);

	physics::SolveRigidJoints(world.Store);
	CHECK(world.Store.Get<scene::Transform>(child)->Frame.Position.X == 11.5f);
}

TEST_CASE("a rigid chain propagates from one stable root", "[physics][welds]") {
	World world;
	const ecs::Entity first = world.Part("First", {0.0f, 0.0f, 0.0f});
	const ecs::Entity second = world.Part("Second", {2.0f, 0.0f, 0.0f});
	const ecs::Entity third = world.Part("Third", {5.0f, 0.0f, 0.0f});

	for (const auto &[a, b] : {std::pair{first, second}, std::pair{second, third}}) {
		const ecs::Entity weld =
			world.Store.CreateInstance(ecs::Classes::Find(core::Name("WeldConstraint")), "Link");
		world.Store.SetParent(weld, a);
		world.Store.Set(weld, scene::WeldConstraint{a, b});
	}
	physics::SolveRigidJoints(world.Store);
	world.Store.GetMutable<scene::Transform>(first)->Frame.Position.X = 10.0f;
	physics::SolveRigidJoints(world.Store);

	CHECK(world.Store.Get<scene::Transform>(second)->Frame.Position.X == 12.0f);
	CHECK(world.Store.Get<scene::Transform>(third)->Frame.Position.X == 15.0f);
}

TEST_CASE("disabled and internally connected links produce no constraint collision", "[physics][welds]") {
	World world;
	const ecs::Entity first = world.Part("First", {0.0f, 0.0f, 0.0f});
	const ecs::Entity second = world.Part("Second", {0.5f, 0.0f, 0.0f});
	const ecs::Entity weld =
		world.Store.CreateInstance(ecs::Classes::Find(core::Name("WeldConstraint")), "WeldConstraint");
	world.Store.SetParent(weld, first);
	world.Store.Set(weld, scene::WeldConstraint{first, second});

	physics::SolveRigidJoints(world.Store);
	physics::SyncBroadphase(world.Store);
	physics::BroadPhase(world.Store);
	CHECK(world.Store.Resource<physics::PhysicsWorld>()->Pairs().empty());

	auto disabled = *world.Store.Get<scene::WeldConstraint>(weld);
	disabled.Enabled = false;
	world.Store.Set(weld, disabled);
	physics::SolveRigidJoints(world.Store);
	CHECK_FALSE(world.Store.Resource<physics::PhysicsWorld>()->RigidlyConnected(first, second));
}
