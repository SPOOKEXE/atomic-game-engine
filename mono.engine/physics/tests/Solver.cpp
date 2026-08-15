#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/Contacts.hpp>
#include <engine/physics/Integrate.hpp>
#include <engine/physics/NarrowPhase.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/physics/Solver.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Enums.hpp>
#include <engine/scene/SurfaceTable.hpp>
#include <engine/testing/Suite.hpp>

// Private: the impulse cache and the resting list are what several cases here
// assert on, and no module outside this one has any business reaching them.
#include "PipelineInternals.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>

TEST_SUITE_ID("engine.physics.solver")
// The manifolds it consumes, and the normal convention every impulse is applied
// along.
TEST_DEPENDS("engine.physics.narrowphase")
// The contact and event types it fills in.
TEST_DEPENDS("engine.physics.contacts")
// Friction and restitution come from a row of this, read once per body.
TEST_DEPENDS("engine.scene.surfacetable")
// Mass, damping and body kind, and the fact that there is no sleeping flag on
// the component any more.
TEST_DEPENDS("engine.scene.components")
// Sleeping takes a row's `Motion` away, which is an archetype move.
TEST_DEPENDS("engine.ecs.archetype")

using Catch::Approx;
using engine::core::CFrame;
using engine::core::Name;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::physics::BroadPhase;
using engine::physics::ContactEvent;
using engine::physics::ContactPhase;
using engine::physics::IntegrateMotion;
using engine::physics::NarrowPhase;
using engine::physics::PhysicsWorld;
using engine::physics::PipelineInternals;
using engine::physics::PreparePhysicsWorld;
using engine::physics::Publish;
using engine::physics::SLEEP_SETTLE_SECONDS;
using engine::physics::Solve;
using engine::physics::SyncBroadphase;
using engine::scene::Anchored;
using engine::scene::BodyKind;
using engine::scene::Collider;
using engine::scene::Motion;
using engine::scene::RigidBody;
using engine::scene::ShapeKind;
using engine::scene::Surface;
using engine::scene::SurfaceProperties;
using engine::scene::SurfaceTable;
using engine::scene::Transform;

namespace {
	constexpr float TICK = 1.0f / 60.0f;
	constexpr float GRAVITY = 9.81f;

	// How a body is described to `Place` below.
	struct Description {
		// Braced defaults on every member, including the two vectors that
		// would otherwise be default-constructed anyway: without them a
		// designated initialiser that skips one is a warning, and the `ci`
		// preset makes warnings fatal.
		Vector3 Position{};
		Vector3 Extent{0.5f, 0.5f, 0.5f};
		ShapeKind Shape = ShapeKind::Box;
		Vector3 Velocity{};
		float Mass = 1.0f;
		bool Anchored = false;
		bool Trigger = false;
		const char *Material = nullptr;
	};

	Entity Place(Store &store, const Description &description) {
		const Entity entity = store.Create();
		store.Set<Transform>(entity, Transform{CFrame{description.Position}});

		Collider collider;
		collider.Shape = description.Shape;
		collider.Extent = description.Extent;
		collider.Trigger = description.Trigger;
		store.Set<Collider>(entity, collider);

		if (description.Material != nullptr) {
			store.Set<Surface>(entity, Surface{Name(description.Material)});
		}

		// `Anchored` decides presence rather than setting a flag, exactly as
		// `MakePart` does: an anchored body carries the tag and no `Motion`, and
		// lands in a different archetype. The `RigidBody` is on both, because
		// what a part weighs is not the world's decision about whether it may
		// move it.
		RigidBody body;
		body.Mass = description.Mass;
		store.Set<RigidBody>(entity, body);

		if (description.Anchored) {
			store.Set<Anchored>(entity, Anchored{});
		} else {
			store.Set<Motion>(entity, Motion{description.Velocity, Vector3::Zero});
		}
		return entity;
	}

	// The whole tick, in the order `RegisterPhysicsSystems` composes it.
	//
	// Written out rather than driven through a `Scheduler` so that a case can
	// step gravity, or leave it out, without a system to hang it on. Gravity is
	// deliberately not part of this module - `v02v03v04.md` §3.5 has no gravity
	// row and `RigidBody` has no gravity scale - so a scene that wants weight
	// applies it, which is what this does.
	void StepWorld(Store &store, float delta, bool gravity) {
		store.AdvanceTick(delta);
		if (gravity) {
			store.Each<Motion, const RigidBody>([delta](Entity, Motion &motion, const RigidBody &body) {
				if (body.Kind == BodyKind::Dynamic) {
					motion.Linear.Y -= GRAVITY * delta;
				}
			});
		}
		IntegrateMotion(store);
		SyncBroadphase(store);
		BroadPhase(store);
		NarrowPhase(store);
		Solve(store);
		Publish(store);
	}

	const PhysicsWorld &WorldOf(const Store &store) {
		return *store.Resource<PhysicsWorld>();
	}

	Vector3 VelocityOf(const Store &store, Entity entity) {
		const Motion *motion = store.Get<Motion>(entity);
		return motion != nullptr ? motion->Linear : Vector3::Zero;
	}

	float HeightOf(const Store &store, Entity entity) {
		return store.Get<Transform>(entity)->Frame.Position.Y;
	}
}

TEST_CASE("a contact removes the closing velocity", "[physics][solver]") {
	// The smallest thing a solver has to do. Two boxes overlapping and closing
	// at a metre a second come out separating, or not closing, but never still
	// closing - the sign of the impulse is what decides which, and getting it
	// backwards accelerates them into each other.
	Store store("solver.closing");
	PreparePhysicsWorld(store, 1.0f);

	const Entity left = Place(
		store, Description{.Position = Vector3{-0.45f, 0.0f, 0.0f}, .Velocity = Vector3{1.0f, 0.0f, 0.0f}}
	);
	const Entity right = Place(
		store, Description{.Position = Vector3{0.45f, 0.0f, 0.0f}, .Velocity = Vector3{-1.0f, 0.0f, 0.0f}}
	);

	StepWorld(store, TICK, false);

	const float closing = (VelocityOf(store, right) - VelocityOf(store, left)).X;
	CHECK(closing >= 0.0f);
	CHECK(VelocityOf(store, left).X <= 0.0f);
	CHECK(VelocityOf(store, right).X >= 0.0f);
}

TEST_CASE("an anchored body is not moved by a contact", "[physics][solver]") {
	// A body with no `RigidBody` is not a static body - it is not a body at all
	// - and an infinite mass is how that is expressed. A solver that gave it a
	// finite one would push the floor out from under everything.
	Store store("solver.anchored");
	PreparePhysicsWorld(store, 1.0f);

	const Entity floor = Place(
		store,
		Description{
			.Position = Vector3{0.0f, -0.45f, 0.0f}, .Extent = Vector3{4.0f, 0.5f, 4.0f}, .Anchored = true
		}
	);
	const Entity crate = Place(
		store, Description{.Position = Vector3{0.0f, 0.4f, 0.0f}, .Velocity = Vector3{0.0f, -2.0f, 0.0f}}
	);

	const float before = HeightOf(store, floor);
	StepWorld(store, TICK, false);

	CHECK(HeightOf(store, floor) == Approx(before));

	// Stopped, to within what sixteen sweeps of float arithmetic leave
	// behind. Zero exactly would be asserting that the solver converged
	// completely, which a fixed iteration count never promises.
	CHECK(VelocityOf(store, crate).Y > -0.01f);
	CHECK_FALSE(store.Has<Motion>(floor));
}

TEST_CASE("a trigger produces an event and no impulse", "[physics][solver]") {
	// **The two halves have to be checked together.** A trigger that stopped
	// producing events would look like an empty region, and one that quietly
	// pushed would look like a physics bug a long way from the trigger.
	Store store("solver.trigger");
	PreparePhysicsWorld(store, 1.0f);

	const Entity region = Place(
		store,
		Description{
			.Position = Vector3::Zero, .Extent = Vector3{1.0f, 1.0f, 1.0f}, .Anchored = true, .Trigger = true
		}
	);
	const Entity walker = Place(
		store, Description{.Position = Vector3{0.5f, 0.0f, 0.0f}, .Velocity = Vector3{1.0f, 0.0f, 0.0f}}
	);

	StepWorld(store, TICK, false);

	const auto events = WorldOf(store).Events();
	REQUIRE(events.size() == 1);
	CHECK(events[0].Phase == ContactPhase::Began);
	CHECK((events[0].A == region || events[0].B == region));

	// Straight through, at exactly the speed it arrived with.
	CHECK(VelocityOf(store, walker).X == Approx(1.0f));
	CHECK(VelocityOf(store, walker).Y == Approx(0.0f));

	// And the manifold is reported, so a listener that wants the geometry can
	// still have it.
	CHECK(WorldOf(store).Manifolds().size() == 1);
}

TEST_CASE("contact events say began, persisted and ended", "[physics][solver]") {
	Store store("solver.events");
	PreparePhysicsWorld(store, 1.0f);

	const Entity left = Place(store, Description{.Position = Vector3{-0.4f, 0.0f, 0.0f}});
	const Entity right = Place(store, Description{.Position = Vector3{0.4f, 0.0f, 0.0f}});

	StepWorld(store, TICK, false);
	REQUIRE(WorldOf(store).Events().size() == 1);
	CHECK(WorldOf(store).Events()[0].Phase == ContactPhase::Began);

	StepWorld(store, TICK, false);
	REQUIRE(WorldOf(store).Events().size() == 1);
	CHECK(WorldOf(store).Events()[0].Phase == ContactPhase::Persisted);

	// Pull them apart, which needs the transforms marked so the index notices.
	store.Set<Transform>(left, Transform{CFrame{Vector3{-20.0f, 0.0f, 0.0f}}});
	store.Set<Transform>(right, Transform{CFrame{Vector3{20.0f, 0.0f, 0.0f}}});
	StepWorld(store, TICK, false);

	REQUIRE(WorldOf(store).Events().size() == 1);
	CHECK(WorldOf(store).Events()[0].Phase == ContactPhase::Ended);
	CHECK(WorldOf(store).Events()[0].A == left);
	CHECK(WorldOf(store).Events()[0].B == right);

	// And nothing at all once they have been apart for a tick, because an event
	// is a transition and not a state.
	StepWorld(store, TICK, false);
	CHECK(WorldOf(store).Events().empty());
}

TEST_CASE("the surface table decides friction and restitution", "[physics][solver]") {
	// `Surface` names a row and the row holds the floats - the resource case
	// out of `ecs/AGENTS.md`. A body whose material nobody registered takes the
	// defaults rather than a silent zero, and a bouncy one bounces.
	const auto drop = [](const char *name, float restitution) {
		Store store(name);
		PreparePhysicsWorld(store, 2.0f);

		SurfaceTable table;
		table.Set(Name("rubber"), SurfaceProperties{0.5f, restitution});
		store.SetResource(table);

		Place(
			store,
			Description{
				.Position = Vector3{0.0f, -0.5f, 0.0f},
				.Extent = Vector3{4.0f, 0.5f, 4.0f},
				.Anchored = true,
				.Material = "rubber",
			}
		);
		const Entity ball = Place(
			store,
			Description{
				.Position = Vector3{0.0f, 0.45f, 0.0f},
				.Extent = Vector3{0.5f, 0.0f, 0.0f},
				.Shape = ShapeKind::Sphere,
				.Velocity = Vector3{0.0f, -4.0f, 0.0f},
				.Material = "rubber",
			}
		);

		StepWorld(store, TICK, false);
		return VelocityOf(store, ball).Y;
	};

	// Dead stop: the closing speed is removed and nothing is given back.
	CHECK(drop("solver.dead", 0.0f) == Approx(0.0f).margin(0.05));

	// Eight tenths of four metres a second, back the other way.
	CHECK(drop("solver.bouncy", 0.8f) == Approx(3.2f).margin(0.3));
}

TEST_CASE("friction resists sliding across a contact", "[physics][solver]") {
	const auto slide = [](const char *name, float friction) {
		Store store(name);
		PreparePhysicsWorld(store, 2.0f);

		SurfaceTable table;
		table.Set(Name("ground"), SurfaceProperties{friction, 0.0f});
		store.SetResource(table);

		Place(
			store,
			Description{
				.Position = Vector3{0.0f, -0.5f, 0.0f},
				.Extent = Vector3{8.0f, 0.5f, 8.0f},
				.Anchored = true,
				.Material = "ground",
			}
		);
		const Entity crate = Place(
			store,
			Description{
				.Position = Vector3{0.0f, 0.49f, 0.0f},
				.Velocity = Vector3{3.0f, 0.0f, 0.0f},
				.Material = "ground",
			}
		);

		for (int tick = 0; tick < 60; tick++) {
			StepWorld(store, TICK, true);
		}
		return VelocityOf(store, crate).X;
	};

	const float slippery = slide("solver.slippery", 0.0f);
	const float grippy = slide("solver.grippy", 0.9f);

	CHECK(slippery == Approx(3.0f).margin(0.1));
	CHECK(grippy < slippery);
	CHECK(grippy < 1.0f);
}

// --- the warm start ----------------------------------------------------------

TEST_CASE("the impulse cache survives from one tick to the next", "[physics][solver]") {
	// `v02v03v04.md`'s allocation table calls this a reuse structure rather
	// than an optimisation bolted on, and this is what makes that true: a
	// resting contact is found under the same `(entityA, entityB, feature)` key
	// next tick and starts from the impulse it settled on.
	Store store("solver.warmstart");
	PreparePhysicsWorld(store, 2.0f);

	Place(
		store,
		Description{
			.Position = Vector3{0.0f, -0.5f, 0.0f}, .Extent = Vector3{4.0f, 0.5f, 4.0f}, .Anchored = true
		}
	);
	Place(store, Description{.Position = Vector3{0.0f, 0.499f, 0.0f}});

	StepWorld(store, TICK, true);
	PhysicsWorld &world = *store.ResourceMutable<PhysicsWorld>();
	const size_t cached = PipelineInternals::ImpulseCache(world).size();
	REQUIRE(cached > 0);

	// Sorted, because next tick's warm start binary-searches it. A cache built
	// out of order answers "not found" for contacts that are in it, which is a
	// warm start that silently stopped warming.
	const auto &impulses = PipelineInternals::ImpulseCache(world);
	for (size_t index = 1; index < impulses.size(); index++) {
		CHECK(impulses[index - 1] < impulses[index]);
	}

	// The same keys next tick, so the lookup finds them.
	StepWorld(store, TICK, true);
	CHECK(PipelineInternals::ImpulseCache(world).size() == cached);

	bool anyHolding = false;
	for (const auto &impulse : PipelineInternals::ImpulseCache(world)) {
		anyHolding = anyHolding || impulse.Normal > 0.0f;
	}
	CHECK(anyHolding);
}

TEST_CASE("a tower stands up because of the warm start", "[physics][solver]") {
	// **The case that fails when the warm start is deleted**, which the weaker
	// version of it did not: a single box on a floor settles at sixteen
	// iterations with or without last tick's impulses, so watching one box sink
	// measured nothing. A tower is where it bites - the bottom contact carries
	// five boxes, and finding that impulse from zero every tick takes more
	// sweeps than there are.
	//
	// The comparison is against the same scene with the cache emptied each
	// tick, so deleting the warm start makes the two sides equal and the
	// assertion below false. A fixed number to compare against would have gone
	// stale the first time the iteration count moved.
	const auto tower = [](const char *name, bool warmStart) {
		Store store(name);
		PreparePhysicsWorld(store, 4.0f);
		Place(
			store,
			Description{
				.Position = Vector3{0.0f, -1.0f, 0.0f}, .Extent = Vector3{8.0f, 1.0f, 8.0f}, .Anchored = true
			}
		);

		Entity boxes[6];
		for (int index = 0; index < 6; index++) {
			boxes[index] = Place(
				store, Description{.Position = Vector3{0.0f, 0.5f + static_cast<float>(index) * 1.001f, 0.0f}}
			);
		}

		PhysicsWorld &world = *store.ResourceMutable<PhysicsWorld>();
		for (int tick = 0; tick < 240; tick++) {
			if (!warmStart) {
				PipelineInternals::ImpulseCache(world).clear();
			}
			StepWorld(store, TICK, true);
		}

		float furthest = 0.0f;
		for (const Entity box : boxes) {
			const Vector3 position = store.Get<Transform>(box)->Frame.Position;
			const float sideways = std::sqrt(position.X * position.X + position.Z * position.Z);
			furthest = sideways > furthest ? sideways : furthest;
		}
		return furthest;
	};

	const float warm = tower("solver.tower.warm", true);
	const float cold = tower("solver.tower.cold", false);

	CHECK(warm < 0.05f);
	CHECK(warm < cold * 0.5f);
}

// --- sleeping ----------------------------------------------------------------

TEST_CASE("a settled body falls asleep and leaves the dynamic archetype", "[physics][solver]") {
	// **The archetype move.** `v02v03v04.md`'s allocation table asks for a
	// sleeping row the query never visits; losing `Motion` is what delivers
	// that with the components that already exist, because `ecs::Store` has no
	// "without this component" term for a tag to be excluded by.
	Store store("solver.sleep");
	PreparePhysicsWorld(store, 2.0f);

	Place(
		store,
		Description{
			.Position = Vector3{0.0f, -0.5f, 0.0f}, .Extent = Vector3{4.0f, 0.5f, 4.0f}, .Anchored = true
		}
	);
	const Entity crate = Place(store, Description{.Position = Vector3{0.0f, 0.5f, 0.0f}});

	const int ticks = static_cast<int>(SLEEP_SETTLE_SECONDS / TICK) + 10;
	for (int tick = 0; tick < ticks; tick++) {
		StepWorld(store, TICK, true);
	}

	CHECK(WorldOf(store).Sleeping(crate));
	CHECK(WorldOf(store).SleepingBodies() == 1);
	CHECK_FALSE(store.Has<Motion>(crate));

	// And it is in the static index now, which is the whole benefit: the
	// dynamic query does not visit it.
	CHECK(WorldOf(store).DynamicColliders() == 0);
	CHECK(WorldOf(store).StaticColliders() == 2);

	// It also stays exactly where it fell asleep, forever.
	const float resting = HeightOf(store, crate);
	for (int tick = 0; tick < 120; tick++) {
		StepWorld(store, TICK, true);
	}
	CHECK(HeightOf(store, crate) == Approx(resting));
}

TEST_CASE("a moving neighbour wakes a sleeping body", "[physics][solver]") {
	// A body that could never wake would be a body that fell out of the
	// simulation. The waking pass is one sweep of the manifolds in pair order,
	// so a stack wakes one layer per tick - bounded and the same every run.
	Store store("solver.wake");
	PreparePhysicsWorld(store, 2.0f);

	Place(
		store,
		Description{
			.Position = Vector3{0.0f, -0.5f, 0.0f}, .Extent = Vector3{8.0f, 0.5f, 8.0f}, .Anchored = true
		}
	);
	const Entity crate = Place(store, Description{.Position = Vector3{0.0f, 0.5f, 0.0f}});

	const int ticks = static_cast<int>(SLEEP_SETTLE_SECONDS / TICK) + 10;
	for (int tick = 0; tick < ticks; tick++) {
		StepWorld(store, TICK, true);
	}
	REQUIRE(WorldOf(store).Sleeping(crate));

	// A second crate arrives at speed from the side.
	const Entity missile = Place(
		store, Description{.Position = Vector3{2.0f, 0.5f, 0.0f}, .Velocity = Vector3{-6.0f, 0.0f, 0.0f}}
	);
	for (int tick = 0; tick < 30; tick++) {
		StepWorld(store, TICK, true);
	}

	CHECK_FALSE(WorldOf(store).Sleeping(crate));
	CHECK(store.Has<Motion>(crate));
	CHECK(store.Alive(missile));
}

TEST_CASE("a sleeping contact is not reported as ended", "[physics][solver]") {
	// Falling asleep takes a body out of the dynamic index, and two anchored
	// colliders are never a pair - so the contact holding a resting box up
	// disappears from the broad phase the tick it sleeps. Reporting that as
	// `Ended` would tell a listener the box left the floor, which is the one
	// thing it definitely did not do.
	Store store("solver.sleepevents");
	PreparePhysicsWorld(store, 2.0f);

	Place(
		store,
		Description{
			.Position = Vector3{0.0f, -0.5f, 0.0f}, .Extent = Vector3{4.0f, 0.5f, 4.0f}, .Anchored = true
		}
	);
	const Entity crate = Place(store, Description{.Position = Vector3{0.0f, 0.5f, 0.0f}});

	const int ticks = static_cast<int>(SLEEP_SETTLE_SECONDS / TICK) + 30;
	bool sawEnded = false;
	for (int tick = 0; tick < ticks; tick++) {
		StepWorld(store, TICK, true);
		for (const ContactEvent &event : WorldOf(store).Events()) {
			sawEnded = sawEnded || event.Phase == ContactPhase::Ended;
		}
	}

	REQUIRE(WorldOf(store).Sleeping(crate));
	CHECK_FALSE(sawEnded);

	// And it keeps saying they are touching, because they are.
	REQUIRE(WorldOf(store).Events().size() == 1);
	CHECK(WorldOf(store).Events()[0].Phase == ContactPhase::Persisted);
}

TEST_CASE("a kinematic body never sleeps", "[physics][solver]") {
	// Taking a kinematic body's `Motion` away would stop whatever owns it from
	// moving it, which is a platform that silently stops.
	Store store("solver.kinematic");
	PreparePhysicsWorld(store, 2.0f);

	Place(
		store,
		Description{
			.Position = Vector3{0.0f, -0.5f, 0.0f}, .Extent = Vector3{4.0f, 0.5f, 4.0f}, .Anchored = true
		}
	);
	const Entity platform = Place(store, Description{.Position = Vector3{0.0f, 0.5f, 0.0f}});
	RigidBody body;
	body.Kind = BodyKind::Kinematic;
	store.Set<RigidBody>(platform, body);

	const int ticks = static_cast<int>(SLEEP_SETTLE_SECONDS / TICK) + 30;
	for (int tick = 0; tick < ticks; tick++) {
		StepWorld(store, TICK, false);
	}

	CHECK_FALSE(WorldOf(store).Sleeping(platform));
	CHECK(store.Has<Motion>(platform));
}

TEST_CASE("the solver buffers are cleared and not freed", "[physics][solver]") {
	Store store("solver.capacity");
	PreparePhysicsWorld(store, 2.0f);

	Place(
		store,
		Description{
			.Position = Vector3{0.0f, -0.5f, 0.0f}, .Extent = Vector3{4.0f, 0.5f, 4.0f}, .Anchored = true
		}
	);
	const Entity crate = Place(store, Description{.Position = Vector3{0.0f, 0.5f, 0.0f}});
	StepWorld(store, TICK, true);

	PhysicsWorld &world = *store.ResourceMutable<PhysicsWorld>();
	const size_t bodies = PipelineInternals::Bodies(world).capacity();
	const size_t rows = PipelineInternals::Rows(world).capacity();
	REQUIRE(bodies > 0);
	REQUIRE(rows > 0);

	store.Set<Transform>(crate, Transform{CFrame{Vector3{0.0f, 40.0f, 0.0f}}});
	StepWorld(store, TICK, false);

	CHECK(world.Bodies().empty());
	CHECK(PipelineInternals::Bodies(world).capacity() == bodies);
	CHECK(PipelineInternals::Rows(world).capacity() == rows);
}
