// Rigid assemblies built from both direct WeldConstraints and legacy Welds.

#include "WorkBudgetInternal.hpp"

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

#include <array>
#include <stdexcept>

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
	const physics::PhysicsWorld &physicsWorld = *world.Store.Resource<physics::PhysicsWorld>();
	CHECK(physicsWorld.RigidAssemblyRoot(first) == first);
	CHECK(physicsWorld.RigidAssemblyRoot(second) == first);
	CHECK(physicsWorld.RigidAssemblyRoot(third) == first);
	CHECK(physicsWorld.RigidAssemblyRoot(ecs::NULL_ENTITY) == ecs::NULL_ENTITY);
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

TEST_CASE("reverse-ordered rigid chains share one active work limit", "[physics][welds][work-budget]") {
	const auto build = [](World &world) {
		const std::array parts{
			world.Part("Root", {10, 0, 0}, true),
			world.Part("First", {0, 0, 0}),
			world.Part("Second", {0, 0, 0}),
			world.Part("Third", {0, 0, 0}),
		};
		// Reverse edge order forces graph expansion to revisit the chain.
		for (size_t next = parts.size() - 1; next > 0; --next) {
			const auto weld = world.Store.CreateInstance(ecs::Classes::Find(core::Name("Weld")));
			REQUIRE(world.Store.SetParent(weld, world.Workspace));
			scene::JointInstance joint;
			joint.Part0 = parts[next - 1];
			joint.Part1 = parts[next];
			joint.C0 = core::CFrame{core::Vector3{1, 0, 0}};
			world.Store.Set(weld, joint);
		}
		world.Store.AdvanceTick(1.0f / 60.0f);
		return parts;
	};
	World reference;
	const auto parts = build(reference);
	physics::BeginPhysicsWorkTick(reference.Store);
	physics::SolveRigidJoints(reference.Store);
	for (size_t index = 0; index < parts.size(); ++index)
		CHECK(
			reference.Store.Get<scene::Transform>(parts[index])->Frame.Position.X ==
			10.0f + static_cast<float>(index)
		);
	const uint64_t complete = physics::PhysicsWorkBudgetOf(reference.Store)->Used;
	REQUIRE(complete > 0);

	World limited;
	build(limited);
	physics::PhysicsWorkLimits limits;
	limits.WorkPerTick = complete - 1;
	physics::SetPhysicsWorkLimits(limited.Store, limits);
	physics::BeginPhysicsWorkTick(limited.Store);
	REQUIRE_THROWS_AS(physics::SolveRigidJoints(limited.Store), std::runtime_error);
	CHECK(physics::PhysicsWorkBudgetOf(limited.Store)->Blocked);
	CHECK(physics::PhysicsWorkBudgetOf(limited.Store)->ExhaustedStage == physics::PhysicsWorkStage::Welds);
	CHECK(physics::PhysicsWorkBudgetOf(limited.Store)->Used <= limits.WorkPerTick);
	REQUIRE_THROWS_AS(physics::SolveRigidJoints(limited.Store), std::runtime_error);
}

TEST_CASE("disconnected rigid assemblies fit one default tick work limit", "[physics][welds][work-budget]") {
	World world;
	constexpr size_t ASSEMBLIES = 256;
	std::array<ecs::Entity, ASSEMBLIES> roots;
	std::array<ecs::Entity, ASSEMBLIES> children;
	for (size_t index = 0; index < ASSEMBLIES; ++index) {
		roots[index] = world.Part("Foundation", {static_cast<float>(index) * 10, 0, 0}, true);
		children[index] = world.Part("Member", {0, 0, 0});
		const auto weld = world.Store.CreateInstance(ecs::Classes::Find(core::Name("Weld")));
		REQUIRE(world.Store.SetParent(weld, world.Workspace));
		scene::JointInstance joint;
		joint.Part0 = roots[index];
		joint.Part1 = children[index];
		joint.C0 = core::CFrame{core::Vector3{1, 0, 0}};
		world.Store.Set(weld, joint);
	}
	world.Store.AdvanceTick(1.0f / 60.0f);
	physics::BeginPhysicsWorkTick(world.Store);
	// The scheduled pipeline projects joints before and after contact solving.
	for (int projection = 0; projection < 2; ++projection) {
		for (const auto root : roots)
			world.Store.GetMutable<scene::Transform>(root)->Frame.Position.Y += 1;
		REQUIRE_NOTHROW(physics::SolveRigidJoints(world.Store));
		for (size_t index = 0; index < ASSEMBLIES; ++index) {
			const auto &frame = world.Store.Get<scene::Transform>(children[index])->Frame;
			CHECK(
				frame.Position ==
				core::Vector3{static_cast<float>(index) * 10 + 1, static_cast<float>(projection + 1), 0}
			);
			CHECK(
				world.Store.Resource<physics::PhysicsWorld>()->RigidAssemblyRoot(children[index]) ==
				roots[index]
			);
		}
	}
	const auto *budget = physics::PhysicsWorkBudgetOf(world.Store);
	CHECK_FALSE(budget->Blocked);
	CHECK(budget->Used <= budget->Limits.WorkPerTick);
}

TEST_CASE("cyclic rigid assemblies retain owner-ordered first-reach poses", "[physics][welds][work-budget]") {
	World world;
	const auto first = world.Part("First", {0, 0, 0});
	const auto second = world.Part("Second", {0, 0, 0});
	const auto anchor = world.Part("Anchor", {7, 3, -4}, true);
	const core::CFrame firstToSecond{{2, 1, -1}, core::CFrame::Angles(0.3f, -0.2f, 0.1f).Rotation()};
	const core::CFrame secondToAnchor{{-1, 3, 2}, core::CFrame::Angles(-0.4f, 0.1f, 0.5f).Rotation()};
	const core::CFrame firstToAnchor{{6, -2, 1}, core::CFrame::Angles(0.2f, 0.7f, -0.3f).Rotation()};
	REQUIRE_FALSE((firstToSecond * secondToAnchor).FuzzyEq(firstToAnchor, 1e-6f));
	const auto link = [&](ecs::Entity part0, ecs::Entity part1, const core::CFrame &relative) {
		const auto weld = world.Store.CreateInstance(ecs::Classes::Find(core::Name("Weld")));
		REQUIRE(world.Store.SetParent(weld, world.Workspace));
		scene::JointInstance joint;
		joint.Part0 = part0;
		joint.Part1 = part1;
		joint.C0 = relative;
		world.Store.Set(weld, joint);
	};
	bool firstLinkWaits = true;
	SECTION("AB precedes BC and waits until after the direct AC link") {
		link(first, second, firstToSecond);
		link(second, anchor, secondToAnchor);
	}
	SECTION("BC precedes AB and propagates through both in one pass") {
		firstLinkWaits = false;
		link(second, anchor, secondToAnchor);
		link(first, second, firstToSecond);
	}
	link(first, anchor, firstToAnchor);
	const scene::Motion anchorMotion{{2, 3, 4}, {0.1f, 0.2f, -0.1f}};
	world.Store.Set(anchor, anchorMotion);
	world.Store.AdvanceTick(1.0f / 60.0f);
	physics::BeginPhysicsWorkTick(world.Store);
	const std::array anchorFrames{
		core::CFrame{{7, 3, -4}, core::CFrame::Angles(0.2f, 0.4f, -0.1f).Rotation()},
		core::CFrame{{-3, 5, 9}, core::CFrame::Angles(-0.1f, 0.3f, 0.6f).Rotation()},
	};
	for (const auto &anchorFrame : anchorFrames) {
		world.Store.Set(anchor, scene::Transform{anchorFrame});
		physics::SolveRigidJoints(world.Store);
		const auto expectedSecond = anchorFrame * secondToAnchor.Inverse();
		const auto expectedFirst =
			firstLinkWaits ? anchorFrame * firstToAnchor.Inverse() : expectedSecond * firstToSecond.Inverse();
		for (const auto &[part, expected] :
			 {std::pair{first, expectedFirst}, std::pair{second, expectedSecond}}) {
			CHECK(world.Store.Get<scene::Transform>(part)->Frame.FuzzyEq(expected, 1e-5f));
			CHECK(world.Store.Resource<physics::PhysicsWorld>()->RigidAssemblyRoot(part) == anchor);
			const auto *motion = world.Store.Get<scene::Motion>(part);
			REQUIRE(motion != nullptr);
			CHECK(motion->Angular == anchorMotion.Angular);
			CHECK(motion->Linear.FuzzyEq(
				anchorMotion.Linear + anchorMotion.Angular.Cross(expected.Position - anchorFrame.Position),
				1e-5f
			));
		}
	}
}
