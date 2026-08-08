// Which alpha the editor presents a world at, and what drawing at the wrong
// one looks like.
//
// **The bug this file was written for: move a part in Edit mode and the mesh
// stays at the origin.** The selection outline reads `Transform` directly and
// followed the mouse; the draw list interpolates from `PreviousTransform`,
// which nothing had ever written, and stayed at the identity. Two halves of one
// frame disagreeing about where a part is reads as a renderer fault, and it was
// eight lines of arithmetic in `Editor::PresentWorld` that no test could reach.
//
// So the arithmetic moved into `studio/Presentation.hpp` and this is it under
// test, in both directions: the predicate on its own, and the whole chain —
// world, part, property write, present, draw list — through the same
// `client::InstallPresentation` the editor installs.

#include <engine/ecs/Scheduler.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <client/Scene.hpp>
#include <studio/Presentation.hpp>

TEST_SUITE_ID("studio.presentation")

using engine::core::Name;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Scheduler;
using engine::ecs::Store;
using engine::world::Universe;
using engine::world::WorldId;
using engine::world::WorldSettings;
using engine::world::WorldState;
using studio::PresentationAlpha;

namespace {
	// An accumulator that is neither of the two answers, so a case that returns
	// it can be told apart from one that returns 0 or 1 by accident.
	constexpr float MIDWAY = 0.25f;

	// A world with the editor's presentation seam installed and one part in it.
	//
	// The part is created with no transform written, exactly as the editor's
	// "insert a Part" does — which is what makes its `PreviousTransform` the
	// identity and the origin the wrong answer this file is about.
	WorldId Scene(Universe &universe, std::string_view name) {
		engine::scene::RegisterSceneClasses();

		WorldSettings settings;
		settings.Name = Name(name);

		const WorldId id = universe.Create(settings);
		universe.Enter(id, [](Store &store, Scheduler &systems) {
			client::InstallPresentation(store, systems, 16);
		});
		universe.Enter(id, [](Store &store) {
			const Entity part = store.CreateInstance(engine::scene::PartClass(), "Dragged");
			store.SetParent(part, engine::scene::InstallServices(store));
		});
		return id;
	}

	// Where the draw list says the part is, after presenting at `alpha`.
	Vector3 DrawnAt(Universe &universe, WorldId world, float alpha) {
		universe.Present(world, 1.0f / 60.0f, alpha);

		Vector3 where;
		universe.Enter(world, [&where](Store &store) {
			const auto *list = store.Resource<client::DrawList>();
			REQUIRE(list != nullptr);
			REQUIRE(list->Instances.size() == 1);
			where = list->Instances[0].Frame.Position;
		});
		return where;
	}
}

// --- the predicate -----------------------------------------------------------

// **The case that was wrong, and it is the editor's ordinary one.**
// `Editor::SyncWorldStates` leaves every world `Active` when nothing is
// running, so a state test alone says "ticking" for a world sitting in Edit
// mode with `Editor::Simulate` returning before `Universe::Tick`.
TEST_CASE("an active world in a universe nothing ticks is drawn at one", "[studio][presentation]") {
	CHECK(PresentationAlpha(false, WorldState::Active, MIDWAY) == 1.0f);
	CHECK(PresentationAlpha(false, WorldState::Idle, MIDWAY) == 1.0f);
}

// The other half, which the old code did get right: the universe is ticking and
// this world is not part of it.
TEST_CASE("a world the driver skips is drawn at one", "[studio][presentation]") {
	CHECK(PresentationAlpha(true, WorldState::Suspended, MIDWAY) == 1.0f);
	CHECK(PresentationAlpha(true, WorldState::Faulted, MIDWAY) == 1.0f);
	CHECK(PresentationAlpha(true, WorldState::Remote, MIDWAY) == 1.0f);
}

// A world that really is being advanced keeps its accumulator, because that is
// what buys smooth motion at 300 frames a second over a 60 Hz tick.
// `client/tests/Presentation.cpp` holds the case that refuted the other fix.
TEST_CASE("a world being ticked keeps its accumulator", "[studio][presentation]") {
	CHECK(PresentationAlpha(true, WorldState::Active, MIDWAY) == MIDWAY);

	// **`Idle` ticks, slowly**, and the version of this that spelled the
	// predicate as `state == Active` answered one for it — stepped motion in a
	// world that was simulating perfectly well. `engine::world::Ticks` is what
	// stopped that being a guess each caller makes.
	CHECK(PresentationAlpha(true, WorldState::Idle, MIDWAY) == MIDWAY);
}

// --- the chain the predicate is the end of -----------------------------------

TEST_CASE("a part moved in Edit mode is drawn where it was moved to", "[studio][presentation]") {
	// Edit mode, spelled out: the world is `Active` because nothing is running,
	// and nothing is ticking it.
	Universe universe;
	const WorldId world = Scene(universe, "studio.presentation.edit");

	const Vector3 moved{12.0f, 3.0f, -5.0f};
	universe.Enter(world, [&moved](Store &store) {
		const Entity part = store.FindFirstChild(engine::scene::WorkspaceOf(store), "Dragged");
		REQUIRE(part != engine::ecs::NULL_ENTITY);
		REQUIRE(store.SetProperty(part, Name("Position"), &moved, sizeof(moved)));
	});

	const float alpha = PresentationAlpha(false, universe.StateOf(world), universe.AlphaOf(world));
	const Vector3 drawn = DrawnAt(universe, world, alpha);

	CHECK(drawn.X == Catch::Approx(moved.X));
	CHECK(drawn.Y == Catch::Approx(moved.Y));
	CHECK(drawn.Z == Catch::Approx(moved.Z));
}

// **The failure, asserted rather than described.** This is what the editor did:
// a never-advanced accumulator is zero, zero means "draw the previous frame",
// and the previous frame of a part the editor made is the identity. Without
// this case the one above passes for a `PresentationAlpha` that always returns
// one, and there would be nothing to say *why* it must.
TEST_CASE("presenting an unticked world at its accumulator draws the origin", "[studio][presentation]") {
	Universe universe;
	const WorldId world = Scene(universe, "studio.presentation.stale");

	universe.Enter(world, [](Store &store) {
		const Entity part = store.FindFirstChild(engine::scene::WorkspaceOf(store), "Dragged");
		const Vector3 moved{12.0f, 3.0f, -5.0f};
		REQUIRE(store.SetProperty(part, Name("Position"), &moved, sizeof(moved)));
	});

	// The accumulator of a world nothing has ticked.
	CHECK(universe.AlphaOf(world) == 0.0f);

	const Vector3 drawn = DrawnAt(universe, world, universe.AlphaOf(world));

	CHECK(drawn.X == Catch::Approx(0.0f));
	CHECK(drawn.Y == Catch::Approx(0.0f));
	CHECK(drawn.Z == Catch::Approx(0.0f));
}
