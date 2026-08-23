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
// test, in both directions: the predicate on its own, and the whole chain -
// world, part, property write, present, draw list - through the same
// `client::InstallPresentation` the editor installs.

#include <engine/ecs/Scheduler.hpp>
#include <engine/effects/ParticleSystem.hpp>
#include <engine/scene/Attachments.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
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
using studio::AdvanceStudioParticlePreview;
using studio::AppendReplicaVisualInstances;
using studio::CollectStudioParticleBatches;
using studio::ParticleEmitterVisibleInStudio;
using studio::PresentationAlpha;
using studio::PresentationCeiling;
using studio::PresentationRates;
using studio::StatusBarSnapshot;
using studio::StudioParticleSelection;
using studio::WorldSelectorLabel;

namespace {
	// An accumulator that is neither of the two answers, so a case that returns
	// it can be told apart from one that returns 0 or 1 by accident.
	constexpr float MIDWAY = 0.25f;

	// A world with the editor's presentation seam installed and one part in it.
	//
	// The part is created with no transform written, exactly as the editor's
	// "insert a Part" does - which is what makes its `PreviousTransform` the
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
			const auto *list = store.Resource<engine::render::DrawList>();
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
	// predicate as `state == Active` answered one for it - stepped motion in a
	// world that was simulating perfectly well. `engine::world::Ticks` is what
	// stopped that being a guess each caller makes.
	CHECK(PresentationAlpha(true, WorldState::Idle, MIDWAY) == MIDWAY);
}

TEST_CASE("the world selector marks runtime activity, not selection", "[studio][presentation]") {
	CHECK(WorldSelectorLabel("MeshGrid", false) == "MeshGrid");
	CHECK(WorldSelectorLabel("MeshGrid", true) == "MeshGrid (ACTIVE)");
	CHECK(WorldSelectorLabel({}, true) == "? (ACTIVE)");
}

TEST_CASE(
	"Studio previews only enabled particle emitters placed in the world", "[studio][presentation][particles]"
) {
	Store store("studio-particle-preview");
	Scheduler systems;
	client::InstallPresentation(store, systems, 64);

	const Entity part = store.CreateInstance(engine::scene::PartClass(), "EmitterPart");
	const engine::ecs::ClassId emitterClass = engine::ecs::Classes::Find(Name("ParticleEmitter"));
	REQUIRE(emitterClass.IsValid());
	const Entity emitter = store.CreateInstance(emitterClass, "PreviewEmitter");
	auto *settings = store.GetMutable<engine::effects::ParticleEmitter>(emitter);
	REQUIRE(settings != nullptr);
	settings->Rate = 10.0f;
	settings->Lifetime = {1.0f, 1.0f};

	REQUIRE(AdvanceStudioParticlePreview(store, 1.0f / 60.0f, false, true));
	engine::render::ParticleFrame frame;
	CHECK(engine::render::CollectParticleBatches(store, frame, StudioParticleSelection(store)) == 0);
	CHECK_FALSE(ParticleEmitterVisibleInStudio(store, emitter, *settings));
	CHECK(store.Resource<engine::effects::ParticleSystem>()->Blocks.empty());

	REQUIRE(store.SetParent(emitter, part));
	REQUIRE(AdvanceStudioParticlePreview(store, 1.0f / 60.0f, false, true));
	CHECK(CollectStudioParticleBatches(store, frame, true) == 1);
	CHECK(ParticleEmitterVisibleInStudio(store, emitter, *settings));
	CHECK(CollectStudioParticleBatches(store, frame, false) == 0);
	CHECK(frame.Batches.empty());

	const Entity attachment = store.CreateInstance(engine::scene::AttachmentClass(), "EmitterAttachment");
	REQUIRE(store.SetParent(attachment, part));
	REQUIRE(store.SetParent(emitter, attachment));
	REQUIRE(AdvanceStudioParticlePreview(store, 1.0f / 60.0f, false, true));
	CHECK(engine::render::CollectParticleBatches(store, frame, StudioParticleSelection(store)) == 1);
	CHECK(ParticleEmitterVisibleInStudio(store, emitter, *settings));

	settings = store.GetMutable<engine::effects::ParticleEmitter>(emitter);
	REQUIRE(settings != nullptr);
	settings->Enabled = false;
	REQUIRE(AdvanceStudioParticlePreview(store, 1.0f / 60.0f, false, true));
	CHECK(engine::render::CollectParticleBatches(store, frame, StudioParticleSelection(store)) == 0);
	CHECK_FALSE(ParticleEmitterVisibleInStudio(store, emitter, *settings));
}

TEST_CASE(
	"hidden or running Studio particles are not advanced by the preview", "[studio][presentation][particles]"
) {
	Store store("studio-particle-preview-gate");
	Scheduler systems;
	client::InstallPresentation(store, systems, 64);

	const auto *before = store.Resource<engine::effects::ParticleSystem>();
	REQUIRE(before != nullptr);
	const uint64_t baseline = before->PresentationRevision;

	CHECK_FALSE(AdvanceStudioParticlePreview(store, 1.0f / 60.0f, false, false));
	CHECK_FALSE(AdvanceStudioParticlePreview(store, 1.0f / 60.0f, true, true));
	const auto *after = store.Resource<engine::effects::ParticleSystem>();
	REQUIRE(after != nullptr);
	CHECK(after->PresentationRevision == baseline);
	CHECK(after->Blocks.empty());
}

TEST_CASE("a running world is not reduced to the input-idle rate", "[studio][presentation]") {
	const PresentationRates rates{120.0f, 20.0f, 120.0f, 10.0f};

	CHECK(PresentationCeiling(rates, true, true, true) == 120.0f);
	CHECK(PresentationCeiling(rates, true, false, true) == 20.0f);
}

TEST_CASE("renderer focus and the active subsystem rates limit presentation", "[studio][presentation]") {
	const PresentationRates rates{120.0f, 20.0f, 100.0f, 10.0f};

	CHECK(PresentationCeiling(rates, true, true, false) == 100.0f);
	CHECK(PresentationCeiling(rates, false, true, false) == 10.0f);
	CHECK(PresentationCeiling(PresentationRates{}, true, true, true) == 0.0f);

	const PresentationRates interfaceUnlimited{0.0f, 0.0f, 165.0f, 60.0f};
	CHECK(PresentationCeiling(interfaceUnlimited, true, true, false) == 165.0f);

	const PresentationRates uncapped{120.0f, 20.0f, 100.0f, 10.0f, true};
	CHECK(PresentationCeiling(uncapped, false, false, true) == 0.0f);
}

TEST_CASE("status counters stay retained between their display deadlines", "[studio][presentation][cache]") {
	StatusBarSnapshot snapshot;
	REQUIRE(snapshot.Refresh(10.0, 0, 300, 12, 400, 0));

	CHECK_FALSE(snapshot.Refresh(10.1, 0, 297, 99, 9000, 0));
	CHECK(snapshot.FramesPerSecond == 300);
	CHECK(snapshot.DrawCalls == 12);
	CHECK(snapshot.Triangles == 400);

	CHECK(snapshot.Refresh(10.25, 0, 297, 99, 9000, 0));
	CHECK(snapshot.FramesPerSecond == 297);
	CHECK(snapshot.DrawCalls == 99);
}

TEST_CASE("changing the focused viewport refreshes status immediately", "[studio][presentation][cache]") {
	StatusBarSnapshot snapshot;
	REQUIRE(snapshot.Refresh(10.0, 0, 300, 12, 0, 0));

	CHECK(snapshot.Refresh(10.01, 1, 300, 4, 0, 0));
	CHECK(snapshot.Viewport == 1);
	CHECK(snapshot.DrawCalls == 4);
}

// --- the hosted client visual scene -----------------------------------------

TEST_CASE("a hosted client view keeps one copy of authority rows", "[studio][presentation]") {
	const Name replicaWorld("studio.presentation.replica");

	engine::scene::DrawInstance authority;
	authority.Source = 42;
	authority.Frame.Position.X = 4.0f;

	engine::scene::DrawInstance replica = authority;
	replica.Frame.Position.X = 3.5f;

	std::vector<engine::scene::DrawInstance> merged;
	merged.push_back(authority);
	AppendReplicaVisualInstances(replicaWorld, {&replica, 1}, merged);

	REQUIRE(merged.size() == 1);
	CHECK(merged[0].Frame.Position.X == 4.0f);
}

TEST_CASE("a hosted client view appends only client-local rows", "[studio][presentation]") {
	const Name replicaWorld("studio.presentation.replica");

	engine::scene::DrawInstance authority;
	authority.Source = 10;

	engine::scene::DrawInstance replicated = authority;
	replicated.SourceWorld = replicaWorld;

	engine::scene::DrawInstance local;
	local.Source = 0x8000'0000ull;
	local.Variant = 2;

	const std::array replica{replicated, local};
	std::vector<engine::scene::DrawInstance> merged;
	merged.push_back(authority);
	AppendReplicaVisualInstances(replicaWorld, replica, merged);

	REQUIRE(merged.size() == 2);
	CHECK(merged[0].Source == authority.Source);
	CHECK(merged[1].Source == local.Source);
	CHECK(merged[1].Variant == local.Variant);
	CHECK(merged[1].SourceWorld == replicaWorld);
}

TEST_CASE("anonymous replica rows cannot duplicate a published authority scene", "[studio][presentation]") {
	const Name replicaWorld("studio.presentation.replica");

	engine::scene::DrawInstance authority;
	authority.Source = 0;
	engine::scene::DrawInstance replica = authority;
	replica.Frame.Position.X = 8.0f;

	std::vector<engine::scene::DrawInstance> merged;
	merged.push_back(authority);
	AppendReplicaVisualInstances(replicaWorld, {&replica, 1}, merged);

	REQUIRE(merged.size() == 1);
	CHECK(merged[0].Frame.Position.X == authority.Frame.Position.X);
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

// **The failure, asserted rather than described.** A never-advanced accumulator
// is zero, zero means "draw the previous frame", and in a world nothing ticks
// the previous frame is however the part was last *drawn* - which is not where
// the editor has since dragged it to. Without this case the one above passes
// for a `PresentationAlpha` that always returns one, and there would be nothing
// to say *why* it must.
//
// **It used to assert the origin, and that was the same bug seen one step
// earlier.** A part the editor had just made was drawn interpolating from the
// identity, because nothing had ever written its `PreviousTransform` - so the
// stale frame and the origin were the same place and this case could not tell
// them apart. `scene::SyncRendered` now seeds the previous frame of a row the
// moment it is first drawn (a thing that has just appeared did not come from
// anywhere), so the part draws where it was rather than at the origin, and this
// case says what it always meant: at alpha zero you get the *last drawn*
// position and not the current one.
TEST_CASE("presenting an unticked world at its accumulator draws a stale frame", "[studio][presentation]") {
	Universe universe;
	const WorldId world = Scene(universe, "studio.presentation.stale");

	const Vector3 placed{1.0f, 2.0f, 3.0f};
	universe.Enter(world, [&placed](Store &store) {
		const Entity part = store.FindFirstChild(engine::scene::WorkspaceOf(store), "Dragged");
		REQUIRE(store.SetProperty(part, Name("Position"), &placed, sizeof(placed)));
	});

	// Drawn once where it was placed, which is what gives it a previous frame
	// to be stale about. A part that has never been drawn has no history, and
	// asserting against one would be asserting against the seeding above.
	const Vector3 first = DrawnAt(universe, world, 1.0f);
	CHECK(first.X == Catch::Approx(placed.X));

	const Vector3 moved{12.0f, 3.0f, -5.0f};
	universe.Enter(world, [&moved](Store &store) {
		const Entity part = store.FindFirstChild(engine::scene::WorkspaceOf(store), "Dragged");
		REQUIRE(store.SetProperty(part, Name("Position"), &moved, sizeof(moved)));
	});

	// The accumulator of a world nothing has ticked.
	CHECK(universe.AlphaOf(world) == 0.0f);

	const Vector3 drawn = DrawnAt(universe, world, universe.AlphaOf(world));

	CHECK(drawn.X == Catch::Approx(placed.X));
	CHECK(drawn.Y == Catch::Approx(placed.Y));
	CHECK(drawn.Z == Catch::Approx(placed.Z));

	// And emphatically not where the editor moved it to, which is the whole
	// point: `PresentationAlpha` returning one is what closes that gap.
	CHECK(drawn.X != Catch::Approx(moved.X));
}
