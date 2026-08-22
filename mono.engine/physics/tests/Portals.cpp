// What a body standing in a hole is standing on.
//
// **The half of a portal `scene` cannot test**, because `scene` may not link
// this module and the answer is an overlap query. The picture half - a body cut
// at the plane with its far half drawn in the room beyond - is
// `scene::CutAndCloneSeams` and has its own suite; this is the contact half, and
// the two are deliberately different mechanisms.
//
// The arrangement under test is the one that is not obvious: the far room's
// colliders are copied *into* the near room through the inverse seam, rather
// than a twin of the body being placed on the far side. `physics/Portals.hpp`
// carries the argument - mapping this way needs no impulse mapped back, because
// nothing crossed.

#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/Integrate.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/physics/Portals.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

TEST_SUITE_ID("engine.physics.portals")
// The overlap this asks the far room with.
TEST_DEPENDS("engine.physics.query")
// The seams, the maps and the straddle test, all of which are stated once in
// `scene` and asked here rather than re-derived.
TEST_DEPENDS("engine.scene.surfacecameras")

using Catch::Approx;
using engine::core::CFrame;
using engine::core::Name;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::physics::GhostPortalBodies;
using engine::physics::RetirePortalProxies;
using engine::scene::Bounds;
using engine::scene::Collider;
using engine::scene::Motion;
using engine::scene::PortalProxy;
using engine::scene::Transform;

namespace {
	// Two panes a hundred units apart, a body in the near one, and a slab in the
	// far room for it to find.
	//
	// The geometry is `physics/tests/Query.cpp`'s, so the two suites agree about
	// which side of a pane a hole opens onto: the map carries a pane's front
	// hemisphere to the far pane's *back* one, so the half of a body that has
	// pushed through A meets whatever stands behind B.
	struct Doorway {
		Store World{"physics.portals"};
		Entity PaneA;
		Entity PaneB;
		Entity Slab;
		Entity Body;

		Entity Part(const char *name, const Vector3 &at, const Vector3 &half) {
			const Entity entity = World.CreateInstance(engine::ecs::Classes::Find(Name("Part")), name);
			World.Set<Transform>(entity, Transform{CFrame(at)});
			World.Set<Bounds>(entity, Bounds{half});

			Collider collider;
			collider.Extent = half;
			World.Set<Collider>(entity, collider);
			return entity;
		}

		Doorway() {
			engine::scene::RegisterSceneClasses();
			engine::physics::PreparePhysicsWorld(World, 2.0f);

			PaneA = Part("PaneA", Vector3::Zero, Vector3{8.0f, 4.5f, 0.2f});
			PaneB = Part("PaneB", Vector3{100.0f, 0.0f, 0.0f}, Vector3{8.0f, 4.5f, 0.2f});

			const Entity hole =
				World.CreateInstance(engine::ecs::Classes::Find(Name("SurfaceCamera")), "Hole");
			World.Set<engine::scene::SurfaceCamera>(hole, engine::scene::SurfaceCamera{});
			World.Set<engine::scene::Portal>(hole, engine::scene::Portal{PaneB});
			World.SetParent(hole, PaneA);

			const Entity mouth =
				World.CreateInstance(engine::ecs::Classes::Find(Name("SurfaceCamera")), "Mouth");
			World.Set<engine::scene::SurfaceCamera>(mouth, engine::scene::SurfaceCamera{});
			World.Set<engine::scene::Portal>(mouth, engine::scene::Portal{PaneA});
			World.SetParent(mouth, PaneB);

			// **The far room's floor, behind B's face, which is the side the hole
			// opens onto.** The map carries a pane's front hemisphere to the far
			// pane's *back* one, so what a body's far half meets is what stands
			// behind B - `physics/tests/Query.cpp` puts its slab there for the
			// same reason and measures a ray landing on it.
			//
			// Close enough that the body's own reach touches it: a proxy is only
			// worth making for something a contact could actually be generated
			// against, and the sphere this pass queries with is the body's
			// bounding one.
			Slab = Part("Slab", Vector3{100.0f, 0.0f, 0.8f}, Vector3{4.0f, 4.0f, 0.5f});

			// A body standing in the near pane. `Motion` is what makes it a thing
			// that can be held up rather than the room.
			Body = Part("Body", Vector3{0.0f, 0.0f, -0.1f}, Vector3{0.5f, 1.0f, 0.5f});
			World.Set<Motion>(Body, Motion{});

			Index();
		}

		void Index() {
			engine::physics::SyncBroadphase(World);
			engine::physics::BroadPhase(World);
		}

		size_t Proxies() {
			size_t count = 0;
			World.Each<const PortalProxy>([&count](Entity, const PortalProxy &) { count++; });
			return count;
		}
	};
}

TEST_CASE("the far room's floor is put under a body standing in a seam", "[physics][portals]") {
	// **A pane is a hole and a body may be halfway through one**, so the floor
	// under its far half is in the other room - and the solver only ever knew
	// about this one. What that looks like is a crate resting in a doorway whose
	// far room's floor is a stud higher: it clips, or it hangs, and both read as
	// physics rather than as a missing feature.
	Doorway world;

	REQUIRE(world.Proxies() == 0);

	const size_t placed = GhostPortalBodies(world.World);
	CHECK(placed >= 1);
	CHECK(world.Proxies() == placed);

	// **Copied into the near room, not the body into the far one.** The slab
	// stands behind B's face; its proxy stands the same distance in front of A's,
	// which is where the body's far half is.
	bool found = false;
	world.World.Each<const PortalProxy, const Transform, const Collider>(
		[&](Entity, const PortalProxy &proxy, const Transform &placement, const Collider &shape) {
			if (proxy.Owner != world.Body) {
				return;
			}

			// **In the near room, which is the whole claim.** The slab is a
			// hundred units away and its copy is beside the body - anything at
			// `x = 100` would be a floor in a room nobody is in, and is what the
			// obvious arrangement (a twin of the body on the far side) produces.
			CHECK(placement.Frame.Position.X == Approx(0.0f).margin(1.0f));

			// The map is rigid for a matched pair, so the copy is the slab's own
			// size rather than something scaled by accident.
			CHECK(shape.Extent.X == Approx(4.0f).margin(1e-3f));
			found = true;
		}
	);
	CHECK(found);

	// **And a proxy never outlives its tick.** A body may walk out of the seam -
	// or through it - in the tick that just ran, and a proxy left behind is a
	// piece of another room standing invisibly in this one.
	CHECK(RetirePortalProxies(world.World) == placed);
	CHECK(world.Proxies() == 0);
}

TEST_CASE("nothing is proxied for a body clear of every hole", "[physics][portals]") {
	// The cost of this pass in a scene with a portal in it and nobody near it,
	// which is nearly every tick of nearly every scene.
	Doorway world;

	world.World.Set<Transform>(world.Body, Transform{CFrame(Vector3{0.0f, 0.0f, -40.0f})});
	world.Index();

	CHECK(GhostPortalBodies(world.World) == 0);
	CHECK(world.Proxies() == 0);
}

TEST_CASE("a proxy is not made for a pane, a proxy or a moving body", "[physics][portals]") {
	// **Three things that would each be wrong in their own way.** A pane solves
	// nothing and a copy of the far one lands on the near one; a proxy of a proxy
	// is a room copied into itself; and a dynamic body on the far side is already
	// simulated there, so a copy of it here is one body with two momenta pushing
	// through the wall between them.
	Doorway world;

	// A crate on the far side that can move, beside the slab.
	const Entity mover = world.Part("Mover", Vector3{100.0f, 0.0f, 1.6f}, Vector3{0.5f, 0.5f, 0.5f});
	world.World.Set<Motion>(mover, Motion{});
	world.Index();

	const size_t placed = GhostPortalBodies(world.World);
	REQUIRE(placed >= 1);

	world.World.Each<const PortalProxy, const Bounds>([&](Entity, const PortalProxy &, const Bounds &box) {
		// The mover is half a metre on a side and the slab is four; nothing this
		// pass made may be the mover's size.
		CHECK(box.HalfExtent.X == Approx(4.0f).margin(1e-3f));
	});

	// And running it twice does not proxy the proxies: they carry no `Motion`,
	// so they are not straddlers, and the pass refuses them by name as well.
	const size_t again = GhostPortalBodies(world.World);
	CHECK(again == placed);

	CHECK(RetirePortalProxies(world.World) == placed * 2);
}

TEST_CASE("a body asleep in a seam has no proxies and cannot fall through one", "[physics][portals]") {
	// **Sleeping is the absence of `scene::Motion`, and this pass is gated on
	// it**, so a body that settles in a doorway stops being given the far room's
	// floor on the very tick it goes quiet. That reads like a body about to drop
	// through the world, and it is not one. The same absence that takes it out of
	// the walk here takes it out of `IntegrateMotion`: nothing is moving it, so
	// there is nothing for a floor to hold up.
	//
	// Written because `scene/AGENTS.md` called the interaction untested and
	// asked for this case first. It pins a state rather than a fix.
	Doorway world;

	REQUIRE(GhostPortalBodies(world.World) >= 1);
	REQUIRE(RetirePortalProxies(world.World) >= 1);

	// What `physics::Publish` does to a body the solver has put to sleep.
	world.World.Remove<Motion>(world.Body);
	world.Index();

	CHECK(GhostPortalBodies(world.World) == 0);
	CHECK(world.Proxies() == 0);

	// **And it stays exactly where it settled.** A sleeping row is not in
	// `IntegrateMotion`'s query either, so taking the far room's floor away from
	// under it is taking a floor from under something that was never falling.
	const Vector3 settled = world.World.Get<Transform>(world.Body)->Frame.Position;
	engine::physics::IntegrateMotion(world.World);
	CHECK(world.World.Get<Transform>(world.Body)->Frame.Position.Z == Approx(settled.Z).margin(1e-6f));

	// **And waking gives them straight back, in the same tick.** Waking *is*
	// putting `Motion` back - `WakeMovingCharacters` says so - and it runs in
	// `character.control`, which is registered before `portal.ghost` in the same
	// `PreSimulation` phase. Registration order inside a phase is the scheduler's
	// contract, so the body has its floor again before the solver sees it and
	// never spends a tick unsupported.
	world.World.Set<Motion>(world.Body, Motion{});
	world.Index();

	CHECK(GhostPortalBodies(world.World) >= 1);
	CHECK(RetirePortalProxies(world.World) >= 1);
}
