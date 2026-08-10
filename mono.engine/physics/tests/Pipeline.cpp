#include <engine/core/Bytes.hpp>
#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Gravity.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

// Private: whether a read of the resource registered the type is a property of
// the lookup in `src/WorldResource.hpp`, and there is no public way to ask it.
#include "WorldResource.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

TEST_SUITE_ID("engine.physics.pipeline")
// The two systems this registers, and the order they have to run in.
TEST_DEPENDS("engine.physics.integrate")
TEST_DEPENDS("engine.physics.broadphase")
// Phases, and the fact that two systems in one phase have no order between
// them — which is why the steps are composed rather than registered separately.
TEST_DEPENDS("engine.ecs.scheduler")
// The resource is registered under an explicit name with its own writer, and a
// snapshot is what proves the name and the writer are both real.
TEST_DEPENDS("engine.ecs.snapshot")
// The components the systems iterate, and the names they are registered under.
TEST_DEPENDS("engine.scene.registration")

using Catch::Approx;
using engine::core::ByteWriter;
using engine::core::CFrame;
using engine::core::Name;
using engine::core::Vector3;
using engine::ecs::Components;
using engine::ecs::Entity;
using engine::ecs::Phase;
using engine::ecs::Scheduler;
using engine::ecs::Store;
using engine::physics::PhysicsWorld;
using engine::physics::PhysicsWorldRegistered;
using engine::physics::PreparedWorld;
using engine::physics::PreparedWorldMutable;
using engine::physics::PreparePhysicsWorld;
using engine::physics::RegisterPhysicsComponents;
using engine::physics::RegisterPhysicsSystems;
using engine::scene::Collider;
using engine::scene::Motion;
using engine::scene::Transform;

namespace {
	constexpr float TICK = 1.0f / 60.0f;

	// Two boxes closing on each other along X, plus a floor that never moves.
	//
	// Deliberately built with no names: `Store::Save` writes the entity name
	// table, and two scenes meant to compare byte for byte must not differ by
	// what they were called.
	void BuildScene(Store &store) {
		PreparePhysicsWorld(store, 1.0f);

		const auto place = [&store](const Vector3 &position, const Vector3 &linear, bool moving) {
			const Entity entity = store.Create();
			store.Set<Transform>(entity, Transform{CFrame{position}});

			Collider collider;
			collider.Extent = Vector3{0.5f, 0.5f, 0.5f};
			store.Set<Collider>(entity, collider);

			if (moving) {
				store.Set<Motion>(entity, Motion{linear, Vector3{0.0f, 1.0f, 0.0f}});
			}
		};

		place(Vector3{-2.0f, 0.0f, 0.0f}, Vector3{1.0f, 0.0f, 0.0f}, true);
		place(Vector3{2.0f, 0.0f, 0.0f}, Vector3{-1.0f, 0.0f, 0.0f}, true);
		place(Vector3{0.0f, -0.6f, 0.0f}, Vector3::Zero, false);
	}
}

TEST_CASE("the resource is registered under an explicit name", "[physics][pipeline]") {
	// A resource is keyed by a component id too, so one that is never
	// registered is minted by the first `SetResource` under the compiler's
	// spelling of the type — a name that differs between compilers and reaches
	// a snapshot.
	RegisterPhysicsComponents();

	CHECK(Components::Find(Name("physics.PhysicsWorld")).IsValid());

	// And the scene components it iterates, which it registers first so that a
	// caller preparing physics alone cannot mint them under the wrong names.
	CHECK(Components::Find(Name("scene.Transform")).IsValid());
	CHECK(Components::Find(Name("scene.Collider")).IsValid());
	CHECK(Components::Find(Name("scene.Motion")).IsValid());

	// Idempotent: a host that tears a universe down and builds another calls
	// this again.
	RegisterPhysicsComponents();
	CHECK(Components::Find(Name("physics.PhysicsWorld")).IsValid());
}

TEST_CASE("reading an unprepared world registers nothing", "[physics][pipeline]") {
	// **The rule the explicit name exists to protect, checked from the side it
	// gets broken from.** `Store::Resource<T>()` registers `T` under the
	// compiler's spelling in order to tell a caller that it is missing. Do that
	// once — one query against a world nobody prepared — and the registration
	// below is a second name for one type, which `Components::Adopt` refuses by
	// aborting the process.
	//
	// It is order-dependent and therefore easy to mistake for a flake: it needs
	// the unprepared read to come before any registration, so in a shuffled
	// suite it fires on some seeds and not others. The deterministic
	// reproduction, against a build with the bug, is:
	//
	//     test_physics --order decl "a query against a world with no physics
	//     resource finds nothing,an overlap tests the shape and not the bound"
	//
	// Every read of the resource in this module goes through
	// `src/WorldResource.hpp` for this reason.
	Store store("pipeline.unprepared");

	const bool registeredBefore = PhysicsWorldRegistered();
	CHECK(PreparedWorld(store) == nullptr);
	CHECK(PreparedWorldMutable(store) == nullptr);
	CHECK(PhysicsWorldRegistered() == registeredBefore);

	// And the registration that the old behaviour aborted on goes through.
	RegisterPhysicsComponents();
	CHECK(Components::Find(Name(engine::physics::PHYSICS_WORLD_COMPONENT)).IsValid());

	// The compiler's spelling is registered under no circumstances, and this
	// is the line that catches the bug: an accidental registration takes that
	// name, not the explicit one, so the check three lines up stays true while
	// this one goes red. Asked through `TypeNameOf` so the case means the same
	// thing on every compiler.
	CHECK_FALSE(Components::Find(Name(engine::ecs::TypeNameOf<PhysicsWorld>())).IsValid());
}

TEST_CASE("preparing a world installs the resource and the change tracking", "[physics][pipeline]") {
	Store store("pipeline.prepare");
	PreparePhysicsWorld(store);

	REQUIRE(store.HasResource<PhysicsWorld>());

	// Without these two, `Store::Changed` is always false and the sync decides
	// static geometry never moved — an index describing where the world used to
	// be, with nothing to say it.
	CHECK(store.Observed<Transform>());
	CHECK(store.Observed<Collider>());

	// The default cell size, which is the grid's own measured one rather than a
	// second number this module chose.
	CHECK(store.Resource<PhysicsWorld>()->CellSize() == Approx(engine::spatial::HashGrid::DEFAULT_CELL_SIZE));
}

TEST_CASE("the systems land in the phases the plan names", "[physics][pipeline]") {
	Store store("pipeline.phases");
	BuildScene(store);

	Scheduler scheduler;
	RegisterPhysicsSystems(scheduler);
	REQUIRE(scheduler.SystemCount() == 2);

	scheduler.Tick(store, TICK);

	const auto &timings = scheduler.Timings();
	REQUIRE(timings.size() == 2);
	CHECK(timings[0].Name == "physics.simulation");
	CHECK(timings[0].RunPhase == Phase::Simulation);
	CHECK(timings[1].Name == "physics.contacts");
	CHECK(timings[1].RunPhase == Phase::PostSimulation);
}

TEST_CASE("a scheduled tick integrates before it indexes", "[physics][pipeline]") {
	// The reason the steps are composed into one system rather than registered
	// as two in the same phase: the boxes handed to the index have to describe
	// where things ended the tick. If the order inverted, the pair list would
	// lag the world by exactly one tick — which looks like tuning and is not.
	Store store("pipeline.order");
	BuildScene(store);

	Scheduler scheduler;
	RegisterPhysicsSystems(scheduler);

	// Two seconds of closing at one metre per second each, from four metres
	// apart, so the two boxes are overlapping well before the end.
	for (int tick = 0; tick < 120; tick++) {
		scheduler.Tick(store, TICK);
	}

	const PhysicsWorld &world = *store.Resource<PhysicsWorld>();
	CHECK(world.DynamicColliders() == 2);
	CHECK(world.StaticColliders() == 1);

	// Both boxes on the floor, and each other.
	CHECK(world.Pairs().size() == 3);

	// And the index is not a tick behind: the pair list agrees with where the
	// transforms are *now*, which is what the composition buys.
	store.Each<const Transform, const Motion>([](Entity, const Transform &transform, const Motion &) {
		CHECK(transform.Frame.Position.X == Approx(0.0f).margin(0.02));
	});
}

TEST_CASE("two runs of one scene tick to identical bytes", "[physics][pipeline]") {
	// Same binary, same platform, same result — `v02v03v04.md` §2.4 and §3.5.
	// A snapshot rather than a field-by-field comparison, because the snapshot
	// is what `just determinism` compares and it also catches a component whose
	// padding reached the file uninitialised.
	const auto run = [](int ticks) {
		Store store("pipeline.determinism");
		BuildScene(store);

		Scheduler scheduler;
		RegisterPhysicsSystems(scheduler);
		for (int tick = 0; tick < ticks; tick++) {
			scheduler.Tick(store, TICK);
		}

		ByteWriter writer;
		REQUIRE(store.Save(writer));

		const std::span<const std::byte> bytes = writer.Bytes();
		return std::vector<std::byte>(bytes.begin(), bytes.end());
	};

	const std::vector<std::byte> first = run(2);
	const std::vector<std::byte> second = run(2);

	REQUIRE_FALSE(first.empty());
	CHECK(first == second);

	// And a different number of ticks is a different world, so the comparison
	// above is comparing something rather than two copies of an empty buffer.
	CHECK_FALSE(run(3) == first);
}

TEST_CASE("a snapshot carries the cell size and nothing derived", "[physics][pipeline]") {
	// The grids, the proxies and the pair list are all functions of `Transform`
	// and `Collider`, which the same snapshot already carries. Writing them
	// would be a second copy of one fact, and it would tie the format to a
	// grid's internal layout.
	Store store("pipeline.snapshot");
	BuildScene(store);

	Scheduler scheduler;
	RegisterPhysicsSystems(scheduler);
	scheduler.Tick(store, TICK);
	REQUIRE(store.Resource<PhysicsWorld>()->StaticColliders() == 1);

	ByteWriter writer;
	REQUIRE(store.Save(writer));

	Store restored("pipeline.snapshot");
	engine::core::ByteReader reader(writer.Bytes());
	REQUIRE(restored.Load(reader));

	const PhysicsWorld &world = *restored.Resource<PhysicsWorld>();
	CHECK(world.CellSize() == Approx(1.0f));

	// Restored stale, so the first sync after a load rebuilds rather than
	// querying an index describing the world the snapshot was taken from.
	CHECK(world.StaticDirty());
	CHECK(world.StaticColliders() == 0);
	CHECK(world.Pairs().empty());
}

TEST_CASE("a part authored the way a script authors one falls", "[physics][pipeline]") {
	// **The case `D00039` was about, and the one every other case here skips.**
	// Everything above builds its scene by writing components directly, which
	// proves the pipeline and proves nothing about the path a game takes: a
	// script says `Instance.new("Part")` and `part.Anchored = false`, and what
	// that produces is the class table's business rather than this module's.
	//
	// The module was complete, tested, benchmarked and connected to nothing for
	// four versions. Nobody noticed because every world this engine shipped was
	// anchored throughout — so this asserts the join rather than either side.
	engine::scene::RegisterSceneClasses();
	RegisterPhysicsComponents();

	Store store("pipeline.authored");

	const engine::ecs::ClassId partClass = engine::ecs::Classes::Find(Name("Part"));
	REQUIRE(partClass.IsValid());

	// The floor, anchored — which is what an author writes and is why every
	// existing example simulates nothing.
	const Entity floor = store.CreateInstance(partClass);
	REQUIRE(floor != engine::ecs::NULL_ENTITY);
	{
		const bool anchored = true;
		REQUIRE(store.SetProperty(floor, Name("Anchored"), &anchored, sizeof(anchored)));

		const engine::core::Vector3 size{40.0f, 1.0f, 40.0f};
		REQUIRE(store.SetProperty(floor, Name("Size"), &size, sizeof(size)));

		const engine::core::CFrame at{engine::core::Vector3{0.0f, -0.5f, 0.0f}};
		REQUIRE(store.SetProperty(floor, Name("CFrame"), &at, sizeof(at)));
	}

	// And a block above it, unanchored. **Setting this false is what gives a
	// part a rigid body** — `scene::Part` refuses one to an anchored part — so
	// this single line is the difference between a scene that simulates and
	// every scene this repository shipped before v0.13.
	const Entity block = store.CreateInstance(partClass);
	REQUIRE(block != engine::ecs::NULL_ENTITY);
	{
		const bool anchored = false;
		REQUIRE(store.SetProperty(block, Name("Anchored"), &anchored, sizeof(anchored)));

		const engine::core::Vector3 size{2.0f, 2.0f, 2.0f};
		REQUIRE(store.SetProperty(block, Name("Size"), &size, sizeof(size)));

		const engine::core::CFrame at{engine::core::Vector3{0.0f, 12.0f, 0.0f}};
		REQUIRE(store.SetProperty(block, Name("CFrame"), &at, sizeof(at)));
	}

	// The calls a host makes, which until v0.13 no host made.
	PreparePhysicsWorld(store);

	Scheduler scheduler;
	RegisterPhysicsSystems(scheduler);

	// **And the weight, which is a separate feature.** This module deliberately
	// has no gravity — a top-down game should not have to switch one off — so
	// the pipeline alone integrates every body at zero acceleration for ever.
	// That is not a hypothetical: this case failed exactly that way when it was
	// first written against the pipeline alone, which is how the missing half
	// was found.
	engine::scene::PrepareGravity(store);
	engine::scene::RegisterGravitySystem(scheduler);

	const auto heightOf = [&store](Entity instance) {
		const Transform *transform = store.Get<Transform>(instance);
		return transform == nullptr ? 0.0f : transform->Frame.Position.Y;
	};

	const float started = heightOf(block);
	CHECK(started == Approx(12.0f).margin(0.001));

	for (int tick = 0; tick < 240; tick++) {
		scheduler.Tick(store, TICK);
	}

	// **It fell**, which is the whole claim. A block that is still at twelve
	// metres after four seconds is a world whose bodies are not being
	// integrated — the state this engine was in until something called the two
	// functions above.
	CHECK(heightOf(block) < started - 1.0f);

	// **And it stopped**, which is the other half and is the harder one. A
	// block that has fallen through the floor is not simulation working, it is
	// the narrow phase missing — and it looks identical to success in any test
	// that only asserts the fall.
	CHECK(heightOf(block) > -2.0f);

	// The anchored floor did not move, because an anchored part carries no
	// rigid body at all. If this ever fails, "anchored" has stopped meaning what
	// every example in this repository relies on it meaning.
	CHECK(heightOf(floor) == Approx(-0.5f).margin(0.001));

	// **The world's own index is deliberately not asserted here.** The counts
	// move as a body comes to rest, and what this case is about is the join
	// between the class table and the pipeline rather than the pipeline's
	// bookkeeping — which the cases above already pin against a scene they
	// build themselves. Asserting an internal from here would be a second
	// opinion about something this file is not the authority on.
	CHECK(store.Resource<PhysicsWorld>() != nullptr);
}
