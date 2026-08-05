// Undo and redo, which is the half of the editor a test can reach entirely.
//
// **`CommandLog` was separated from `Editor` so this file could exist**, for
// `PlayLink.cpp`'s reason: everything else about an edit needs a window, a
// device and an imgui frame, and the thing that can be silently wrong is not
// what the panel drew but whether reversing an action actually gave the world
// back. A stack that pops the right label and restores the wrong state would
// pass any test about menus.
//
// The question every case below asks in a different way: **is the world after
// undo the world before the edit?** Not "did undo return true", which a log that
// did nothing at all would also manage.

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/game/Values.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <studio/Commands.hpp>

#include <string>

TEST_SUITE_ID("studio.commands")

using engine::core::Name;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;
using engine::game::PropertyValue;
using engine::world::Universe;
using engine::world::WorldId;
using engine::world::WorldSettings;
using studio::CommandLog;
using studio::EditId;

namespace {
	// A universe with one furnished world, as the editor makes them.
	//
	// `InstallServices` runs because an author's instance is parented into
	// `Workspace` and a world without one has nowhere to put anything — the same
	// call `Editor::AddWorld` makes, for the same reason.
	struct Fixture {
		Universe Worlds;
		WorldId Scene;
		CommandLog Log;

		Fixture() : Log(Worlds) {
			engine::parallel::Jobs::Start(1);
			engine::scene::RegisterSceneComponents();
			engine::scene::RegisterSceneClasses();

			WorldSettings settings;
			settings.Name = Name("Scene");
			Scene = Worlds.Create(settings);

			Worlds.Enter(Scene, [](Store &store) { engine::scene::InstallServices(store); });
		}

		~Fixture() {
			engine::parallel::Jobs::Stop();
		}

		Fixture(const Fixture &) = delete;
		Fixture &operator=(const Fixture &) = delete;

		// The world's `Workspace`, which is what an insert parents into.
		Entity Workspace() {
			Entity found = NULL_ENTITY;
			Worlds.Enter(Scene, [&found](Store &store) { found = engine::scene::WorkspaceOf(store); });
			return found;
		}

		// Creates a part under a parent and records it, the way
		// `Editor::InsertInstance` does.
		Entity Insert(Entity parent, const std::string &name) {
			Entity created = NULL_ENTITY;
			Worlds.Enter(Scene, [&](Store &store) {
				created = store.CreateInstance(engine::scene::PartClass(), name);
				if (created != NULL_ENTITY && parent != NULL_ENTITY) {
					store.SetParent(created, parent);
				}
				Log.RecordCreate(store, Scene, created, "Insert " + name);
			});
			return created;
		}

		// Destroys an instance and records it, the way `Editor::DeleteSelection`
		// does — the record first, because after the destroy there is nothing
		// left to photograph.
		void Destroy(Entity instance, const std::string &what) {
			Worlds.Enter(Scene, [&](Store &store) {
				Log.RecordDestroy(store, Scene, instance, "Delete " + what);
				store.DestroyInstance(instance);
			});
		}

		// Whether an entity is still live.
		bool Alive(Entity instance) {
			bool alive = false;
			Worlds.Enter(Scene, [&](Store &store) { alive = store.Alive(instance); });
			return alive;
		}

		// How many children a parent has, which is how a rebuilt subtree is
		// checked without depending on handles the rebuild changed.
		size_t ChildCount(Entity parent) {
			size_t count = 0;
			Worlds.Enter(Scene, [&](Store &store) {
				store.EachChild(parent, [&count](Entity) { count++; });
			});
			return count;
		}

		// The first child of a parent, which is how a rebuilt root is reached —
		// the handle it had before the rebuild is not the handle it has now.
		Entity FirstChild(Entity parent) {
			Entity first = NULL_ENTITY;
			Worlds.Enter(Scene, [&](Store &store) {
				store.EachChild(parent, [&first](Entity child) {
					if (first == NULL_ENTITY) {
						first = child;
					}
				});
			});
			return first;
		}

		// Reads one property, as the panel does.
		bool Read(Entity instance, Name property, PropertyValue &out) {
			bool ok = false;
			Worlds.Enter(Scene, [&](Store &store) {
				const engine::ecs::ClassId klass = store.ClassOf(instance);
				if (!klass.IsValid()) {
					return;
				}
				for (const engine::ecs::PropertyDescriptor &descriptor :
					 engine::ecs::Classes::Describe(klass).Properties) {
					if (descriptor.Name == property) {
						ok = engine::game::ReadProperty(store, instance, descriptor, out);
						return;
					}
				}
			});
			return ok;
		}

		// Writes one property and records it, the way `DrawProperties` does.
		void Write(Entity instance, Name property, const PropertyValue &value) {
			PropertyValue before;
			Read(instance, property, before);

			Worlds.Enter(Scene, [&](Store &store) {
				const engine::ecs::ClassId klass = store.ClassOf(instance);
				if (!klass.IsValid()) {
					return;
				}
				for (const engine::ecs::PropertyDescriptor &descriptor :
					 engine::ecs::Classes::Describe(klass).Properties) {
					if (descriptor.Name == property) {
						engine::game::WriteProperty(store, instance, descriptor, value);
						return;
					}
				}
			});

			Log.RecordProperty(Scene, instance, property, before, value, "Set " + std::string(property.Text()));
		}

		// A `Vector3` property value, which is what `Size` and `Position` are.
		static PropertyValue OfVector(Vector3 vector) {
			PropertyValue value;
			value.Type = engine::ecs::PropertyType::Vector3;
			value.Vector3 = vector;
			return value;
		}
	};
}

TEST_CASE("an empty log has nothing to undo or redo", "[studio][commands]") {
	Fixture fixture;

	CHECK_FALSE(fixture.Log.CanUndo());
	CHECK_FALSE(fixture.Log.CanRedo());
	CHECK_FALSE(fixture.Log.Undo());
	CHECK_FALSE(fixture.Log.Redo());
	CHECK(fixture.Log.Depth() == 0);
}

TEST_CASE("undoing an insert takes the instance away", "[studio][commands]") {
	Fixture fixture;
	const Entity part = fixture.Insert(fixture.Workspace(), "Part");

	REQUIRE(part != NULL_ENTITY);
	REQUIRE(fixture.Alive(part));
	REQUIRE(fixture.Log.CanUndo());

	REQUIRE(fixture.Log.Undo());
	CHECK_FALSE(fixture.Alive(part));
	CHECK_FALSE(fixture.Log.CanUndo());
	CHECK(fixture.Log.CanRedo());
}

TEST_CASE("redoing an insert brings it back under the same parent", "[studio][commands]") {
	Fixture fixture;
	const Entity workspace = fixture.Workspace();
	const Entity part = fixture.Insert(workspace, "Part");

	REQUIRE(fixture.ChildCount(workspace) == 1);
	REQUIRE(fixture.Log.Undo());
	REQUIRE(fixture.ChildCount(workspace) == 0);

	REQUIRE(fixture.Log.Redo());

	// **The count, not the handle.** A rebuild produces a new entity, which is
	// the whole reason the log does not hold one — asserting the old handle came
	// back would be asserting something the design says is impossible.
	CHECK(fixture.ChildCount(workspace) == 1);
	CHECK_FALSE(fixture.Alive(part));
}

TEST_CASE("undoing a delete rebuilds the subtree, children and all", "[studio][commands]") {
	Fixture fixture;
	const Entity workspace = fixture.Workspace();
	const Entity parent = fixture.Insert(workspace, "Parent");
	fixture.Insert(parent, "ChildA");
	fixture.Insert(parent, "ChildB");

	REQUIRE(fixture.ChildCount(parent) == 2);

	fixture.Destroy(parent, "Parent");
	REQUIRE(fixture.ChildCount(workspace) == 0);

	REQUIRE(fixture.Log.Undo());
	REQUIRE(fixture.ChildCount(workspace) == 1);

	// The rebuilt root is a new handle, so the children are counted through the
	// parent the workspace now has rather than through the one that was deleted.
	const Entity rebuilt = fixture.FirstChild(workspace);

	REQUIRE(rebuilt != NULL_ENTITY);
	CHECK(fixture.ChildCount(rebuilt) == 2);
}

TEST_CASE("the id survives a rebuild, so a second undo still finds its subject", "[studio][commands]") {
	Fixture fixture;
	const Entity workspace = fixture.Workspace();
	const Entity part = fixture.Insert(workspace, "Part");

	const EditId id = fixture.Log.Track(fixture.Scene, part);
	REQUIRE(fixture.Log.Resolve(id) == part);

	fixture.Destroy(part, "Part");

	// **A stale handle, not a null one, and that is the contract.** The log
	// unbinds an id only when it performs the destroy itself; a caller that
	// records and then destroys leaves the id pointing at a dead handle. That is
	// safe rather than sloppy — an entity carries a generation, so a recycled
	// index does not answer `Alive`, and every apply path asks.
	REQUIRE_FALSE(fixture.Alive(fixture.Log.Resolve(id)));

	REQUIRE(fixture.Log.Undo());

	// Rebound to whatever the rebuild produced, which is the property that makes
	// a log deeper than one command possible at all.
	const Entity rebuilt = fixture.Log.Resolve(id);
	CHECK(rebuilt != NULL_ENTITY);
	CHECK(rebuilt != part);
	CHECK(fixture.Alive(rebuilt));
}

TEST_CASE("a property edit undoes to the old value and redoes to the new", "[studio][commands]") {
	Fixture fixture;
	const Entity part = fixture.Insert(fixture.Workspace(), "Part");

	PropertyValue original;
	REQUIRE(fixture.Read(part, Name("Size"), original));

	fixture.Write(part, Name("Size"), Fixture::OfVector(Vector3{7.0f, 8.0f, 9.0f}));

	PropertyValue written;
	REQUIRE(fixture.Read(part, Name("Size"), written));
	REQUIRE(written.Vector3.X == 7.0f);

	REQUIRE(fixture.Log.Undo());

	PropertyValue undone;
	REQUIRE(fixture.Read(part, Name("Size"), undone));
	CHECK(engine::game::ValuesEqual(undone, original));

	REQUIRE(fixture.Log.Redo());

	PropertyValue redone;
	REQUIRE(fixture.Read(part, Name("Size"), redone));
	CHECK(redone.Vector3.X == 7.0f);
	CHECK(redone.Vector3.Y == 8.0f);
	CHECK(redone.Vector3.Z == 9.0f);
}

TEST_CASE("a write that changed nothing is not recorded", "[studio][commands]") {
	Fixture fixture;
	const Entity part = fixture.Insert(fixture.Workspace(), "Part");
	const size_t before = fixture.Log.Depth();

	PropertyValue current;
	REQUIRE(fixture.Read(part, Name("Size"), current));

	// The properties panel submits on every keystroke that parses, so this is
	// the ordinary case rather than a contrived one.
	fixture.Write(part, Name("Size"), current);

	CHECK(fixture.Log.Depth() == before);
}

TEST_CASE("a reparent undoes to the parent it had", "[studio][commands]") {
	Fixture fixture;
	const Entity workspace = fixture.Workspace();
	const Entity first = fixture.Insert(workspace, "First");
	const Entity second = fixture.Insert(workspace, "Second");
	const Entity moving = fixture.Insert(first, "Moving");

	REQUIRE(fixture.ChildCount(first) == 1);
	REQUIRE(fixture.ChildCount(second) == 0);

	fixture.Worlds.Enter(fixture.Scene, [&](Store &store) { store.SetParent(moving, second); });
	fixture.Log.RecordReparent(fixture.Scene, moving, first, second, "Move Moving");

	REQUIRE(fixture.ChildCount(second) == 1);

	REQUIRE(fixture.Log.Undo());
	CHECK(fixture.ChildCount(first) == 1);
	CHECK(fixture.ChildCount(second) == 0);

	REQUIRE(fixture.Log.Redo());
	CHECK(fixture.ChildCount(first) == 0);
	CHECK(fixture.ChildCount(second) == 1);
}

TEST_CASE("recording something new drops the branch that was undone", "[studio][commands]") {
	Fixture fixture;
	const Entity workspace = fixture.Workspace();

	fixture.Insert(workspace, "First");
	REQUIRE(fixture.Log.Undo());
	REQUIRE(fixture.Log.CanRedo());

	fixture.Insert(workspace, "Second");

	// Redoing into a branch the world has moved on from would apply a command
	// against a state it was never recorded against.
	CHECK_FALSE(fixture.Log.CanRedo());
}

TEST_CASE("the log is bounded and drops from the far end", "[studio][commands]") {
	Fixture fixture;
	const Entity workspace = fixture.Workspace();

	for (size_t index = 0; index < CommandLog::DEPTH + 20; index++) {
		fixture.Insert(workspace, "Part" + std::to_string(index));
	}

	CHECK(fixture.Log.Depth() == CommandLog::DEPTH);

	// The most recent is still the one undo reaches first — dropping from the
	// wrong end would silently reverse the order of history.
	CHECK(fixture.Log.NextUndo() == "Insert Part" + std::to_string(CommandLog::DEPTH + 19));
}

TEST_CASE("clearing throws both stacks away", "[studio][commands]") {
	Fixture fixture;
	const Entity workspace = fixture.Workspace();

	fixture.Insert(workspace, "First");
	fixture.Insert(workspace, "Second");
	REQUIRE(fixture.Log.Undo());

	REQUIRE(fixture.Log.CanUndo());
	REQUIRE(fixture.Log.CanRedo());

	// What `BeginRun` calls, because Stop restores a snapshot taken before the
	// run and a stack spanning that boundary describes edits it discarded.
	fixture.Log.Clear();

	CHECK_FALSE(fixture.Log.CanUndo());
	CHECK_FALSE(fixture.Log.CanRedo());
	CHECK(fixture.Log.Depth() == 0);
}

TEST_CASE("undo names what it would reverse", "[studio][commands]") {
	Fixture fixture;
	fixture.Insert(fixture.Workspace(), "Wall");

	CHECK(fixture.Log.NextUndo() == "Insert Wall");
	CHECK(fixture.Log.NextRedo().empty());

	REQUIRE(fixture.Log.Undo());

	CHECK(fixture.Log.NextUndo().empty());
	CHECK(fixture.Log.NextRedo() == "Insert Wall");
}

TEST_CASE("a command whose subject is gone is dropped rather than guessed at", "[studio][commands]") {
	Fixture fixture;
	const Entity part = fixture.Insert(fixture.Workspace(), "Part");

	fixture.Write(part, Name("Size"), Fixture::OfVector(Vector3{2.0f, 2.0f, 2.0f}));
	REQUIRE(fixture.Log.Depth() == 2);

	// Destroyed without recording it, which is what a script deleting an
	// instance during a run looks like from the log's side.
	fixture.Worlds.Enter(fixture.Scene, [&](Store &store) { store.DestroyInstance(part); });

	// The store reuses indices, so applying this to whatever now occupies the
	// row is the ordinary case rather than a remote one.
	CHECK_FALSE(fixture.Log.Undo());
}

TEST_CASE("forgetting a scene drops its commands and leaves the others", "[studio][commands]") {
	Fixture fixture;

	// A second scene, because the whole point of `Forget` is that it is not
	// `Clear` — stopping a run in one world must not throw away the history of
	// another that was edited throughout and never ran.
	engine::world::WorldSettings other;
	other.Name = Name("Other");
	const WorldId second = fixture.Worlds.Create(other);
	fixture.Worlds.Enter(second, [](Store &store) { engine::scene::InstallServices(store); });

	Entity elsewhere = NULL_ENTITY;
	fixture.Worlds.Enter(second, [&](Store &store) {
		elsewhere = store.CreateInstance(engine::scene::PartClass(), "Far");
		store.SetParent(elsewhere, engine::scene::WorkspaceOf(store));
		fixture.Log.RecordCreate(store, second, elsewhere, "Insert Far");
	});

	fixture.Insert(fixture.Workspace(), "Near");
	REQUIRE(fixture.Log.Depth() == 2);

	fixture.Log.Forget(fixture.Scene);

	// Only the other scene's command survives, and it is still the one undo
	// reaches.
	REQUIRE(fixture.Log.Depth() == 1);
	CHECK(fixture.Log.NextUndo() == "Insert Far");

	REQUIRE(fixture.Log.Undo());
	bool alive = true;
	fixture.Worlds.Enter(second, [&](Store &store) { alive = store.Alive(elsewhere); });
	CHECK_FALSE(alive);
}

TEST_CASE("forgetting clears the redo branch for that scene too", "[studio][commands]") {
	Fixture fixture;

	fixture.Insert(fixture.Workspace(), "Part");
	REQUIRE(fixture.Log.Undo());
	REQUIRE(fixture.Log.CanRedo());

	// A restore invalidates the handles a redo would rebuild against just as
	// surely as the ones an undo would.
	fixture.Log.Forget(fixture.Scene);

	CHECK_FALSE(fixture.Log.CanUndo());
	CHECK_FALSE(fixture.Log.CanRedo());
}

TEST_CASE("forgetting a scene nothing was recorded in changes nothing", "[studio][commands]") {
	Fixture fixture;
	fixture.Insert(fixture.Workspace(), "Part");

	fixture.Log.Forget(WorldId{});
	CHECK(fixture.Log.Depth() == 1);
}

TEST_CASE("the history reads oldest first, and the last entry is what undo reverses",
		  "[studio][commands]") {
	// **What the History panel walks.** The stacks are stored as stacks — the
	// back is the top — and a history list reads downwards in the order things
	// happened. Getting that backwards would put the newest edit at the top of
	// the list, which nothing in an editor does, so the order is pinned here
	// rather than left to whoever draws it.
	Fixture fixture;
	const Entity first = fixture.Insert(fixture.Workspace(), "First");
	const Entity second = fixture.Insert(fixture.Workspace(), "Second");
	REQUIRE(first != NULL_ENTITY);
	REQUIRE(second != NULL_ENTITY);

	REQUIRE(fixture.Log.Undoable().size() == 2);
	CHECK(fixture.Log.Redoable().empty());

	// The back is the one `Undo` takes next, and it is the same string the Edit
	// menu shows — so the panel's "you are here" row and the menu agree.
	CHECK(fixture.Log.Undoable().back().Description == fixture.Log.NextUndo());

	REQUIRE(fixture.Log.Undo());

	// One moved across, and the redo stack's back is what `Redo` does next.
	CHECK(fixture.Log.Undoable().size() == 1);
	REQUIRE(fixture.Log.Redoable().size() == 1);
	CHECK(fixture.Log.Redoable().back().Description == fixture.Log.NextRedo());

	REQUIRE(fixture.Log.Redo());
	CHECK(fixture.Log.Undoable().size() == 2);
	CHECK(fixture.Log.Redoable().empty());
}

TEST_CASE("an empty log offers no history", "[studio][commands]") {
	Fixture fixture;
	CHECK(fixture.Log.Undoable().empty());
	CHECK(fixture.Log.Redoable().empty());
}
