// What is drawn is the `Workspace` subtree, and the cheap answer to "has that
// changed" has to be wrong in only one direction.
//
// Two things are under test here and they fail differently. The **early-out**
// fails silently: a signature that misses a term leaves a hidden part drawing
// or a new part invisible, on one frame, with nothing in the renderer to say
// why - so every case below that toggles one thing and asserts the tag moved is
// a term of the fold, and deleting one of those terms should turn a case red
// rather than merely make a benchmark faster. The **non-marking write** fails
// loudly but a long way away: `Rendered` written through `GetMutable` advances
// `Store::ChangeVersion` every frame, which permanently falsifies the gate
// `physics::SyncBroadphase` and `gui::Compiled` both open with.

#include <engine/core/Bytes.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Visibility.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

TEST_SUITE_ID("engine.scene.visibility")
// The walk starts at `Workspace`, so a world with no fixtures marks nothing.
TEST_DEPENDS("engine.scene.services")

using engine::core::ByteReader;
using engine::core::ByteWriter;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;
using engine::scene::InstallServices;
using engine::scene::MakePart;
using engine::scene::PartDesc;
using engine::scene::RegisterSceneClasses;
using engine::scene::Rendered;
using engine::scene::RenderedSignature;
using engine::scene::SyncRendered;
using engine::scene::Transform;
using engine::scene::Visual;
using engine::scene::WorkspaceOf;

namespace visibility_test {
	void Ready() {
		RegisterSceneClasses();
	}

	// A part in `Workspace`, which is the only arrangement that draws.
	Entity PartIn(Store &store, Entity parent) {
		const Entity part = MakePart(store, PartDesc{});
		REQUIRE(part != NULL_ENTITY);
		REQUIRE(store.SetParent(part, parent));
		return part;
	}

	// Hides or shows a part the way a script's `Visible` setter does: through
	// the component, without touching one link in the tree. That is precisely
	// the change a `Hierarchy`-only signature cannot see.
	void SetVisible(Store &store, Entity part, bool visible) {
		const Visual *current = store.Get<Visual>(part);
		REQUIRE(current != nullptr);

		Visual updated = *current;
		updated.Visible = visible;
		store.Set(part, updated);
	}
}

TEST_CASE("a visible part in Workspace is tagged and nothing else is", "[scene][visibility]") {
	visibility_test::Ready();

	Store store("visibility_test.basic");
	const Entity workspace = InstallServices(store);
	REQUIRE(workspace != NULL_ENTITY);

	const Entity drawn = visibility_test::PartIn(store, workspace);

	// Storage rather than scene: a complete part by every component test, and
	// not a descendant of `Workspace`. Before `Rendered` existed this drew.
	const Entity stored = MakePart(store, PartDesc{});
	REQUIRE(stored != NULL_ENTITY);

	CHECK(SyncRendered(store) == 1);
	CHECK(store.Has<Rendered>(drawn));
	CHECK_FALSE(store.Has<Rendered>(stored));

	// The tag is a mark-and-sweep and the mark is cleared on the way out, so a
	// snapshot never sees a non-zero one.
	const Rendered *tag = store.Get<Rendered>(drawn);
	REQUIRE(tag != nullptr);
	CHECK(tag->Mark == 0);
}

TEST_CASE("a steady tree takes the early-out", "[scene][visibility]") {
	visibility_test::Ready();

	Store store("visibility_test.steady");
	const Entity workspace = InstallServices(store);
	const Entity part = visibility_test::PartIn(store, workspace);

	REQUIRE(SyncRendered(store) == 1);
	REQUIRE(store.Has<Rendered>(part));

	// The memo is a resource and not a static, because two worlds are ticked by
	// whichever worker claimed them.
	const RenderedSignature *memo = store.Resource<RenderedSignature>();
	REQUIRE(memo != nullptr);
	CHECK(memo->Fresh == 1);
	const uint64_t settled = memo->Stamp;

	// **Deliberately doing the one thing `Visibility.hpp` forbids**, because it
	// is the only way to observe from outside whether the walk ran. Nothing in
	// the signature covers `Rendered` - it is the walk's output, not its input
	// - so if the tag is still gone after another sync, the walk was skipped.
	store.Remove<Rendered>(part);
	CHECK(SyncRendered(store) == 0);
	CHECK_FALSE(store.Has<Rendered>(part));
	CHECK(store.Resource<RenderedSignature>()->Stamp == settled);

	// And it is a memo rather than a latch: one real change and the full walk
	// runs again, which repairs the sabotage above.
	const Entity second = visibility_test::PartIn(store, workspace);
	CHECK(SyncRendered(store) == 2);
	CHECK(store.Has<Rendered>(part));
	CHECK(store.Has<Rendered>(second));
	CHECK(store.Resource<RenderedSignature>()->Stamp != settled);
}

TEST_CASE("hiding a part re-syncs, with no tree change at all", "[scene][visibility]") {
	// **The case that says a `Hierarchy`-only signature is not enough.** A
	// script or a wire delta writes `Visual::Visible` and touches no link, so a
	// fold over the tree alone would match, skip, and leave a hidden part
	// drawing.
	visibility_test::Ready();

	Store store("visibility_test.visible");
	const Entity workspace = InstallServices(store);
	const Entity part = visibility_test::PartIn(store, workspace);

	REQUIRE(SyncRendered(store) == 1);
	REQUIRE(store.Has<Rendered>(part));

	visibility_test::SetVisible(store, part, false);
	CHECK(SyncRendered(store) == 0);
	CHECK_FALSE(store.Has<Rendered>(part));

	visibility_test::SetVisible(store, part, true);
	CHECK(SyncRendered(store) == 1);
	CHECK(store.Has<Rendered>(part));
}

TEST_CASE("reparenting alone re-syncs", "[scene][visibility]") {
	visibility_test::Ready();

	Store store("visibility_test.reparent");
	const Entity workspace = InstallServices(store);
	const Entity part = visibility_test::PartIn(store, workspace);

	REQUIRE(SyncRendered(store) == 1);
	REQUIRE(store.Has<Rendered>(part));

	// Out of the scene and into storage. Nothing about the part changed; only
	// where it sits, which is the whole rule this file exists to enforce.
	REQUIRE(store.SetParent(part, NULL_ENTITY));
	CHECK(SyncRendered(store) == 0);
	CHECK_FALSE(store.Has<Rendered>(part));

	REQUIRE(store.SetParent(part, workspace));
	CHECK(SyncRendered(store) == 1);
	CHECK(store.Has<Rendered>(part));
}

TEST_CASE("reparenting an ancestor moves the whole subtree", "[scene][visibility]") {
	// Ancestry is not local, which is why this is a walk rather than a hook.
	// The signature has to catch a link written on the *model*, not on the part
	// whose answer changed.
	visibility_test::Ready();

	Store store("visibility_test.subtree");
	const Entity workspace = InstallServices(store);

	const Entity model = MakePart(store, PartDesc{});
	REQUIRE(model != NULL_ENTITY);
	REQUIRE(store.SetParent(model, workspace));

	const Entity child = visibility_test::PartIn(store, model);

	REQUIRE(SyncRendered(store) == 2);

	REQUIRE(store.SetParent(model, NULL_ENTITY));
	CHECK(SyncRendered(store) == 0);
	CHECK_FALSE(store.Has<Rendered>(model));
	CHECK_FALSE(store.Has<Rendered>(child));
}

TEST_CASE("gaining a Visual after being parented re-syncs", "[scene][visibility]") {
	// **The other half of the argument for folding the `Visual` pass.**
	// `Instance.new("Part")` and a later `Set<Visual>` change the answer with
	// no hierarchy write at all when the row was already parented, so folding
	// the entity id in that pass is what makes the row's arrival visible.
	visibility_test::Ready();

	Store store("visibility_test.gained");
	const Entity workspace = InstallServices(store);
	const Entity part = visibility_test::PartIn(store, workspace);

	REQUIRE(SyncRendered(store) == 1);

	const Visual *made = store.Get<Visual>(part);
	REQUIRE(made != nullptr);
	const Visual original = *made;

	store.Remove<Visual>(part);
	CHECK(SyncRendered(store) == 0);
	CHECK_FALSE(store.Has<Rendered>(part));

	store.Set(part, original);
	CHECK(SyncRendered(store) == 1);
	CHECK(store.Has<Rendered>(part));
}

TEST_CASE("destroying a part re-syncs", "[scene][visibility]") {
	visibility_test::Ready();

	Store store("visibility_test.destroy");
	const Entity workspace = InstallServices(store);
	const Entity kept = visibility_test::PartIn(store, workspace);
	const Entity gone = visibility_test::PartIn(store, workspace);

	REQUIRE(SyncRendered(store) == 2);

	// A destroyed instance leaves the `Hierarchy` pass whether or not the links
	// around it were tidied, so the count alone would catch this - and
	// `DestroyInstance` unparents on its way out, so the parent's links move
	// too.
	store.DestroyInstance(gone);
	CHECK(SyncRendered(store) == 1);
	CHECK(store.Has<Rendered>(kept));
}

TEST_CASE("a world with no Workspace draws nothing", "[scene][visibility]") {
	// The safe direction: an unfurnished world is an empty screen somebody
	// investigates, not a scene that draws its own storage.
	visibility_test::Ready();

	Store store("visibility_test.bare");
	const Entity orphan = MakePart(store, PartDesc{});
	REQUIRE(orphan != NULL_ENTITY);
	REQUIRE(WorkspaceOf(store) == NULL_ENTITY);

	CHECK(SyncRendered(store) == 0);
	CHECK_FALSE(store.Has<Rendered>(orphan));
}

TEST_CASE("syncing a steady world moves no change version", "[scene][visibility]") {
	// **The invariant `physics/SyncBroadphase.cpp` states and depends on**: an
	// unchanged `ChangeVersion` means nothing authored has happened. Writing
	// the tag through `GetMutable` used to bump it once per rendered entity per
	// frame, so that gate never held and two dirty-bit scans ran every tick for
	// nothing. `gui::Compiled` rests on the same counter.
	visibility_test::Ready();

	Store store("visibility_test.quiet");

	// Before anything is created: a table gains its `DirtyBits` column when it
	// is first computed, so watching afterwards would leave the parts in tables
	// that cannot record a write and the case would pass for the wrong reason.
	store.Observe<Transform>();

	const Entity workspace = InstallServices(store);
	const Entity part = visibility_test::PartIn(store, workspace);

	REQUIRE(SyncRendered(store) == 1);
	store.ClearChanges();

	const uint64_t settled = store.ChangeVersion();
	for (int frame = 0; frame < 4; frame++) {
		REQUIRE(SyncRendered(store) == 1);
	}
	CHECK(store.ChangeVersion() == settled);
	CHECK_FALSE(store.Changed<Transform>(part));
}

TEST_CASE("a non-marking write is the only one that does not move the counter", "[scene][visibility]") {
	// `MarkWritten` marks any component sitting in a table that carries a
	// `DirtyBits` column rather than only an observed one, so `Rendered` on a
	// part counts as a change purely because `Transform` is watched. That is
	// the mechanism `Store::GetUnobserved` exists to step around, and this is
	// the case that pins both halves of it.
	visibility_test::Ready();

	Store store("visibility_test.unobserved");
	store.Observe<Transform>();

	const Entity workspace = InstallServices(store);
	const Entity part = visibility_test::PartIn(store, workspace);

	REQUIRE(SyncRendered(store) == 1);
	store.ClearChanges();

	const uint64_t settled = store.ChangeVersion();
	REQUIRE(store.GetUnobserved<Rendered>(part) != nullptr);
	CHECK(store.ChangeVersion() == settled);

	// The default, and still the right one for anything that might be watched:
	// a change reported that did not happen costs a rebuild, where a change
	// missed costs correctness.
	REQUIRE(store.GetMutable<Rendered>(part) != nullptr);
	CHECK(store.ChangeVersion() > settled);
}

TEST_CASE("the signature never crosses a snapshot alive", "[scene][visibility]") {
	// **Get this wrong and a loaded game renders wrong, once.** A restored
	// world would carry a stamp matching a tree the walk has never actually
	// been run against in that store, so the first sync would match, skip, and
	// leave the tag as whatever the file happened to hold.
	visibility_test::Ready();

	Store source("visibility_test.saved.source");
	const Entity workspace = InstallServices(source);
	const Entity part = visibility_test::PartIn(source, workspace);

	REQUIRE(SyncRendered(source) == 1);
	REQUIRE(source.Resource<RenderedSignature>()->Fresh == 1);

	ByteWriter writer;
	REQUIRE(source.Save(writer));

	Store restored("visibility_test.saved.restored");
	ByteReader reader(writer.Bytes());
	REQUIRE(restored.Load(reader));

	// Registered under an explicit name and written as nothing, which is what
	// makes the restore land here rather than on `Store::Save` refusing the
	// world outright.
	const RenderedSignature *memo = restored.Resource<RenderedSignature>();
	REQUIRE(memo != nullptr);
	CHECK(memo->Stamp == 0);
	CHECK(memo->Fresh == 0);

	// So the first sync on a loaded world is a real one. The sabotage is the
	// same as the steady-tree case: if the early-out had inherited the stamp,
	// this tag would stay missing.
	restored.Remove<Rendered>(part);
	CHECK(SyncRendered(restored) == 1);
	CHECK(restored.Has<Rendered>(part));
}

// --- the frame a row is interpolated from -------------------------------------

// **The flash, asserted.** `client::BuildDrawList` draws
// `PreviousTransform.NLerp(Transform, alpha)`, so a row whose previous frame is
// the identity is drawn sliding in from the origin - which is what every block
// `examples/Slide.luau` spawns did, for the five frames between being parented
// and the next tick's `capture-previous`.
//
// The rule this checks is narrow on purpose: a row that was **not drawn last
// frame** did not travel from anywhere, so the frame it is interpolated from is
// where it is. A row that was already drawn keeps its history, because that
// history *is* the motion - clearing it on every write is the fix that was tried
// first and it turns every scripted animation into stepped motion at the tick
// rate.
TEST_CASE("a row drawn for the first time is not interpolated from the origin", "[scene][visibility]") {
	visibility_test::Ready();

	Store store("visibility_test.arrival");
	const Entity workspace = InstallServices(store);

	// Made outside the tree and placed, exactly as `Instance.new("Part")` and
	// then a `Position` write leave it: a real transform and a previous frame
	// nothing has ever written.
	const Entity part = MakePart(store, PartDesc{});
	REQUIRE(part != NULL_ENTITY);

	Transform placed;
	placed.Frame = engine::core::CFrame(engine::core::Vector3{40.0f, 9.0f, -3.0f});
	store.Set(part, placed);

	const engine::scene::PreviousTransform *before = store.Get<engine::scene::PreviousTransform>(part);
	REQUIRE(before != nullptr);
	CHECK(before->Frame.Position.X == 0.0f);

	// Parenting it is what puts it on screen, and the sync is what notices.
	REQUIRE(store.SetParent(part, workspace));
	CHECK(SyncRendered(store) == 1);

	const engine::scene::PreviousTransform *after = store.Get<engine::scene::PreviousTransform>(part);
	REQUIRE(after != nullptr);
	CHECK(after->Frame.Position.X == 40.0f);
	CHECK(after->Frame.Position.Y == 9.0f);
	CHECK(after->Frame.Position.Z == -3.0f);
}

TEST_CASE("a row already on screen keeps the frame it came from", "[scene][visibility]") {
	visibility_test::Ready();

	Store store("visibility_test.moving");
	const Entity workspace = InstallServices(store);
	const Entity part = visibility_test::PartIn(store, workspace);

	CHECK(SyncRendered(store) == 1);

	// Moved the way a script animating something does, once per tick.
	Transform moved;
	moved.Frame = engine::core::CFrame(engine::core::Vector3{5.0f, 0.0f, 0.0f});
	store.Set(part, moved);

	CHECK(SyncRendered(store) == 1);

	// **Still the old frame**, which is what buys smooth motion between ticks.
	// If this ever reads 5, the seeding above has stopped asking whether the row
	// is new and every animation in the engine is stepping at the tick rate.
	const engine::scene::PreviousTransform *previous = store.Get<engine::scene::PreviousTransform>(part);
	REQUIRE(previous != nullptr);
	CHECK(previous->Frame.Position.X == 0.0f);
}

TEST_CASE("a part shown again after being moved while hidden does not slide", "[scene][visibility]") {
	visibility_test::Ready();

	Store store("visibility_test.hidden");
	const Entity workspace = InstallServices(store);
	const Entity part = visibility_test::PartIn(store, workspace);

	CHECK(SyncRendered(store) == 1);

	visibility_test::SetVisible(store, part, false);
	CHECK(SyncRendered(store) == 0);

	Transform moved;
	moved.Frame = engine::core::CFrame(engine::core::Vector3{0.0f, 100.0f, 0.0f});
	store.Set(part, moved);

	visibility_test::SetVisible(store, part, true);
	CHECK(SyncRendered(store) == 1);

	// It did not travel a hundred studs - nobody was watching, and drawing the
	// journey is inventing motion that never happened.
	const engine::scene::PreviousTransform *previous = store.Get<engine::scene::PreviousTransform>(part);
	REQUIRE(previous != nullptr);
	CHECK(previous->Frame.Position.Y == 100.0f);
}
