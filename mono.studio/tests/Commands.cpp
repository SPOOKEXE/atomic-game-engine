#include <stdexcept>
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
#include <engine/game/Game.hpp>
#include <engine/game/Values.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/ProceduralJobs.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <studio/Commands.hpp>
#include <vector>

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
	// `Workspace` and a world without one has nowhere to put anything - the same
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
		// does - the record first, because after the destroy there is nothing
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

		// Finds a named child after undo rebuilds it with a new handle.
		Entity FindChild(Entity parent, std::string_view name) {
			Entity first = NULL_ENTITY;
			Worlds.Enter(Scene, [&](Store &store) {
				store.EachChild(parent, [&first, &store, name](Entity child) {
					if (first == NULL_ENTITY && store.InstanceNameOf(child) == Name(name)) {
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

			Log.RecordProperty(
				Scene, instance, property, before, value, "Set " + std::string(property.Text())
			);
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

TEST_CASE(
	"procedural authority undo restores a fresh authority and leaves published output alone",
	"[studio][commands]"
) {
	Fixture fixture;
	engine::script::RegisterProceduralComponents();
	std::vector<std::byte> after;
	uint64_t configuredIncarnation = 0;
	const engine::procedural::RegionScope scope{0, 0, 16};
	fixture.Worlds.Enter(fixture.Scene, [&](Store &store) {
		engine::procedural::Diagnostic diagnostic;
		REQUIRE(
			engine::script::ConfigureProceduralWorld(
				store,
				"undo-test",
				engine::procedural::ForestVillageDefaults("forest-village", 42),
				std::span(&scope, 1),
				std::span<const engine::script::ProceduralSource>{},
				diagnostic
			)
		);
		const auto *authority = store.Resource<engine::script::ProceduralAuthority>();
		REQUIRE(authority != nullptr);
		configuredIncarnation = authority->Incarnation;
		std::vector<uint8_t> encoded;
		REQUIRE(engine::script::EncodeProceduralAuthority(store, encoded, diagnostic));
		after.assign(
			reinterpret_cast<const std::byte *>(encoded.data()),
			reinterpret_cast<const std::byte *>(encoded.data() + encoded.size())
		);
		engine::script::ProceduralPublished published;
		published.Regions.push_back({scope, {}, {}, {42}, 7, 11});
		store.SetResource(published);
	});

	fixture.Log.RecordProceduralAuthority(fixture.Scene, {}, std::move(after), "Configure Procedural World");
	REQUIRE(fixture.Log.Undo());
	fixture.Worlds.Enter(fixture.Scene, [](Store &store) {
		CHECK(store.Resource<engine::script::ProceduralAuthority>() == nullptr);
		const auto *published = store.Resource<engine::script::ProceduralPublished>();
		REQUIRE(published != nullptr);
		REQUIRE(published->Regions.size() == 1);
		CHECK(published->Regions[0].ArtifactBytes == std::vector<uint8_t>{42});
	});

	REQUIRE(fixture.Log.Redo());
	fixture.Worlds.Enter(fixture.Scene, [configuredIncarnation](Store &store) {
		const auto *authority = store.Resource<engine::script::ProceduralAuthority>();
		REQUIRE(authority != nullptr);
		CHECK(authority->Incarnation != configuredIncarnation);
		const auto *published = store.Resource<engine::script::ProceduralPublished>();
		REQUIRE(published != nullptr);
		REQUIRE(published->Regions.size() == 1);
		CHECK(published->Regions[0].ArtifactBytes == std::vector<uint8_t>{42});
	});
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

	REQUIRE(fixture.ChildCount(workspace) == 3);
	REQUIRE(fixture.Log.Undo());
	REQUIRE(fixture.ChildCount(workspace) == 2);

	REQUIRE(fixture.Log.Redo());

	// **The count, not the handle.** A rebuild produces a new entity, which is
	// the whole reason the log does not hold one - asserting the old handle came
	// back would be asserting something the design says is impossible.
	CHECK(fixture.ChildCount(workspace) == 3);
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
	REQUIRE(fixture.ChildCount(workspace) == 2);

	REQUIRE(fixture.Log.Undo());
	REQUIRE(fixture.ChildCount(workspace) == 3);

	// The rebuilt root is a new handle. Resolve it by name, skipping the
	// Terrain and camera that remain under Workspace throughout the edit.
	const Entity rebuilt = fixture.FindChild(workspace, "Parent");

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
	// safe rather than sloppy - an entity carries a generation, so a recycled
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

TEST_CASE("reference property history follows a rebuilt target", "[studio][commands][reference]") {
	Fixture fixture;
	engine::game::RegisterGameClasses();
	const Entity workspace = fixture.Workspace();
	const Entity first = fixture.Insert(workspace, "First");
	const Entity second = fixture.Insert(workspace, "Second");
	Entity camera = NULL_ENTITY;
	fixture.Worlds.Enter(fixture.Scene, [&](Store &store) {
		camera = store.CreateInstance(engine::ecs::Classes::Find(Name("SurfaceCamera")), "Camera");
		REQUIRE(store.SetParent(camera, workspace));
		REQUIRE(store.SetProperty(camera, Name("CameraSubject"), &first, sizeof(first)));
	});
	fixture.Log.Clear();
	PropertyValue after;
	after.Type = engine::ecs::PropertyType::Reference;
	after.Reference = second;
	fixture.Write(camera, Name("CameraSubject"), after);
	fixture.Destroy(first, "First");
	REQUIRE(fixture.Log.Undo());
	const Entity rebuilt = fixture.FindChild(workspace, "First");
	REQUIRE(rebuilt != NULL_ENTITY);
	REQUIRE(rebuilt != first);
	REQUIRE(fixture.Log.Undo());
	PropertyValue restored;
	REQUIRE(fixture.Read(camera, Name("CameraSubject"), restored));
	CHECK(restored.Reference == rebuilt);
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

	// The most recent is still the one undo reaches first - dropping from the
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
	// `Clear` - stopping a run in one world must not throw away the history of
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

TEST_CASE("forgetting a scene preserves waypoint and recording boundaries", "[studio][commands]") {
	Fixture fixture;
	const WorldId removed = fixture.Worlds.Create({.Name = Name("Removed")});
	fixture.Worlds.Enter(removed, [&](Store &store) {
		engine::scene::InstallServices(store);
		const Entity part = store.CreateInstance(engine::scene::PartClass(), "Removed");
		store.SetParent(part, engine::scene::WorkspaceOf(store));
		fixture.Log.RecordCreate(store, removed, part, "Insert removed");
	});
	const Entity workspace = fixture.Workspace();
	const Entity previous = fixture.Insert(workspace, "Previous");

	SECTION("waypoints group every edit after the surviving cut") {
		fixture.Log.SetWaypoint("Previous edits");
		fixture.Log.Forget(removed);
		const Entity first = fixture.Insert(workspace, "First");
		const Entity second = fixture.Insert(workspace, "Second");
		fixture.Log.SetWaypoint("Both edits");
		REQUIRE(fixture.Log.Undo());
		CHECK(fixture.Alive(previous));
		CHECK_FALSE(fixture.Alive(first));
		CHECK_FALSE(fixture.Alive(second));
	}

	SECTION("cancelling a recording reverses its surviving edits") {
		const auto recording = fixture.Log.TryBeginRecording("Recording");
		REQUIRE(recording.has_value());
		fixture.Worlds.Enter(removed, [&](Store &store) {
			const Entity part = store.CreateInstance(engine::scene::PartClass(), "Recorded removed");
			store.SetParent(part, engine::scene::WorkspaceOf(store));
			fixture.Log.RecordCreate(store, removed, part, "Recorded removed");
		});
		const Entity created = fixture.Insert(workspace, "Recorded");
		fixture.Log.Forget(removed);
		REQUIRE(fixture.Log.FinishRecording(*recording, studio::FinishOperation::Cancel));
		CHECK(fixture.Alive(previous));
		CHECK_FALSE(fixture.Alive(created));
	}

	SECTION("appending a recording joins the preceding surviving edit") {
		const auto recording = fixture.Log.TryBeginRecording("Recording");
		REQUIRE(recording.has_value());
		const Entity created = fixture.Insert(workspace, "Recorded");
		fixture.Log.Forget(removed);
		REQUIRE(fixture.Log.FinishRecording(*recording, studio::FinishOperation::Append));
		REQUIRE(fixture.Log.Undo());
		CHECK_FALSE(fixture.Alive(previous));
		CHECK_FALSE(fixture.Alive(created));
	}
}

TEST_CASE("the history reads oldest first, and the last entry is what undo reverses", "[studio][commands]") {
	// **What the History panel walks.** The stacks are stored as stacks - the
	// back is the top - and a history list reads downwards in the order things
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
	// menu shows - so the panel's "you are here" row and the menu agree.
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

// --- recordings ---------------------------------------------------------------
//
// The grouping layer, which is Roblox's `ChangeHistoryService` shape and is what
// team create replicates. The property to hold onto while reading these: a
// command recorded outside a recording is a waypoint of its own, so everything
// above this line describes the same log as everything below it.

TEST_CASE("a recording is one step however many edits are in it", "[studio][commands]") {
	Fixture fixture;
	const Entity workspace = fixture.Workspace();

	const auto recording = fixture.Log.TryBeginRecording("Insert three", "Insert three parts");
	REQUIRE(recording.has_value());
	CHECK(fixture.Log.IsRecordingInProgress());
	CHECK(fixture.Log.IsRecordingInProgress(*recording));
	CHECK_FALSE(fixture.Log.IsRecordingInProgress("rec-somebody-else"));

	fixture.Insert(workspace, "A");
	fixture.Insert(workspace, "B");
	fixture.Insert(workspace, "C");
	REQUIRE(fixture.ChildCount(workspace) == 5);

	REQUIRE(fixture.Log.FinishRecording(*recording, studio::FinishOperation::Commit));
	CHECK_FALSE(fixture.Log.IsRecordingInProgress());

	// Three commands, one step. Without grouping this is three presses of
	// Ctrl+Z for one action, and three messages on the wire for one action.
	CHECK(fixture.Log.Depth() == 3);
	REQUIRE(fixture.Log.Undo());
	CHECK(fixture.ChildCount(workspace) == 2);
	CHECK_FALSE(fixture.Log.CanUndo());

	REQUIRE(fixture.Log.Redo());
	CHECK(fixture.ChildCount(workspace) == 5);
	CHECK_FALSE(fixture.Log.CanRedo());
}

TEST_CASE("only one recording at a time, and the second is refused", "[studio][commands]") {
	Fixture fixture;

	const auto first = fixture.Log.TryBeginRecording("First");
	REQUIRE(first.has_value());

	// Refused rather than nested. A cancel of an outer recording holding a
	// committed inner one has no answer that is not surprising, so there is no
	// way to ask the question.
	CHECK_FALSE(fixture.Log.TryBeginRecording("Second").has_value());

	// And a finish naming somebody else's identifier does not close it.
	CHECK_FALSE(fixture.Log.FinishRecording("rec-999", studio::FinishOperation::Commit));
	CHECK(fixture.Log.IsRecordingInProgress());

	REQUIRE(fixture.Log.FinishRecording(*first, studio::FinishOperation::Commit));
	CHECK(fixture.Log.TryBeginRecording("Second").has_value());
}

TEST_CASE("cancelling a recording puts back what it changed", "[studio][commands]") {
	Fixture fixture;
	const Entity workspace = fixture.Workspace();

	fixture.Insert(workspace, "Kept");
	REQUIRE(fixture.ChildCount(workspace) == 3);
	const size_t before = fixture.Log.Depth();

	const auto recording = fixture.Log.TryBeginRecording("Insert two");
	REQUIRE(recording.has_value());
	fixture.Insert(workspace, "A");
	fixture.Insert(workspace, "B");
	REQUIRE(fixture.ChildCount(workspace) == 5);

	// **The identifier is ignored for a cancel**, which is Roblox's rule and is
	// the one case where not having kept it is the normal situation: a plugin
	// abandoning its own edit knows it has one open.
	REQUIRE(fixture.Log.FinishRecording({}, studio::FinishOperation::Cancel));

	CHECK(fixture.ChildCount(workspace) == 3);
	CHECK(fixture.Log.Depth() == before);

	// Nothing on the redo stack. A cancel is "this never happened", not "step
	// back", and offering to redo it would offer a state nobody asked for.
	CHECK_FALSE(fixture.Log.CanRedo());

	// And the edit before it is still reachable.
	REQUIRE(fixture.Log.Undo());
	CHECK(fixture.ChildCount(workspace) == 2);
}

TEST_CASE("cancelling a recording at the history limit reverses its edits", "[studio][commands]") {
	Fixture fixture;
	const Entity workspace = fixture.Workspace();
	for (size_t index = 0; index < CommandLog::DEPTH; ++index) {
		fixture.Insert(workspace, "Previous");
	}
	const auto recording = fixture.Log.TryBeginRecording("Insert");
	REQUIRE(recording.has_value());
	const Entity created = fixture.Insert(workspace, "Temporary");
	REQUIRE(fixture.Alive(created));
	REQUIRE(fixture.Log.FinishRecording(*recording, studio::FinishOperation::Cancel));
	CHECK_FALSE(fixture.Alive(created));
}

TEST_CASE("recordings retain whole waypoints at the history limit", "[studio][commands]") {
	Fixture fixture;
	const Entity workspace = fixture.Workspace();
	for (size_t index = 0; index < CommandLog::DEPTH; ++index) {
		fixture.Insert(workspace, "Previous");
	}

	size_t published = 0;
	CommandLog::Watcher watcher;
	watcher.Committed = [&](uint64_t, std::span<const studio::Command> commands) {
		published = commands.size();
	};
	fixture.Log.Watch(std::move(watcher));
	const auto recording = fixture.Log.TryBeginRecording("Large edit");
	REQUIRE(recording.has_value());
	for (size_t index = 0; index < CommandLog::DEPTH + 1; ++index) {
		fixture.Insert(workspace, "Grouped");
	}
	REQUIRE(fixture.Log.Depth() == CommandLog::DEPTH + 1);
	REQUIRE(fixture.Log.FinishRecording(*recording, studio::FinishOperation::Commit));
	CHECK(published == CommandLog::DEPTH + 1);
	REQUIRE(fixture.Log.Undo());
	CHECK(fixture.ChildCount(workspace) == CommandLog::DEPTH + 2);
	REQUIRE(fixture.Log.Redo());
	CHECK(fixture.ChildCount(workspace) == 2 * CommandLog::DEPTH + 3);
	fixture.Insert(workspace, "Later");
	CHECK(fixture.Log.Depth() == 1);
}

TEST_CASE("appending folds a recording into the step before it", "[studio][commands]") {
	Fixture fixture;
	const Entity workspace = fixture.Workspace();

	fixture.Insert(workspace, "First");

	const auto recording = fixture.Log.TryBeginRecording("More");
	REQUIRE(recording.has_value());
	fixture.Insert(workspace, "Second");
	fixture.Insert(workspace, "Third");
	REQUIRE(fixture.Log.FinishRecording(*recording, studio::FinishOperation::Append));

	REQUIRE(fixture.ChildCount(workspace) == 5);
	CHECK(fixture.Log.Depth() == 3);

	// One undo, because the recording was folded into the waypoint before it -
	// which is what a drag that resumes should feel like.
	REQUIRE(fixture.Log.Undo());
	CHECK(fixture.ChildCount(workspace) == 2);
	CHECK_FALSE(fixture.Log.CanUndo());
}

TEST_CASE("a waypoint merges everything since the previous cut", "[studio][commands]") {
	Fixture fixture;
	const Entity workspace = fixture.Workspace();

	fixture.Insert(workspace, "A");
	fixture.Insert(workspace, "B");

	// Roblox's rule: the changes between two waypoints are one undo. Over a log
	// whose default is one command per step, that is a merge.
	fixture.Log.SetWaypoint("Insert two parts");
	CHECK(fixture.Log.NextUndo() == "Insert two parts");

	REQUIRE(fixture.Log.Undo());
	CHECK(fixture.ChildCount(workspace) == 2);
	CHECK_FALSE(fixture.Log.CanUndo());

	// **The editor never calls it, and is unaffected.** Two inserts with no cut
	// between them are still two steps, which is what every case above this
	// line asserts.
	Fixture uncut;
	const Entity theirs = uncut.Workspace();
	uncut.Insert(theirs, "A");
	uncut.Insert(theirs, "B");
	REQUIRE(uncut.Log.Undo());
	CHECK(uncut.ChildCount(theirs) == 3);
}

TEST_CASE("a cut cannot merge across an undo", "[studio][commands]") {
	Fixture fixture;
	const Entity workspace = fixture.Workspace();

	fixture.Insert(workspace, "A");
	fixture.Insert(workspace, "B");
	REQUIRE(fixture.Log.Undo());
	fixture.Insert(workspace, "C");

	// The cut moved with the undo, so this merges the one command made since
	// and not the one the undo left behind. Merging across would name a
	// waypoint that no longer holds what it did when it was made.
	fixture.Log.SetWaypoint("Insert C");
	REQUIRE(fixture.Log.Undo());
	CHECK(fixture.ChildCount(workspace) == 3);
}

TEST_CASE("resetting collapses the history and reverts nothing", "[studio][commands]") {
	Fixture fixture;
	const Entity workspace = fixture.Workspace();

	fixture.Insert(workspace, "A");
	fixture.Insert(workspace, "B");
	REQUIRE(fixture.Log.Undo());
	REQUIRE(fixture.Log.CanUndo());
	REQUIRE(fixture.Log.CanRedo());

	fixture.Log.ResetWaypoints();

	CHECK_FALSE(fixture.Log.CanUndo());
	CHECK_FALSE(fixture.Log.CanRedo());

	// A floor rather than a restore. What is on screen is what was on screen.
	CHECK(fixture.ChildCount(workspace) == 3);
}

TEST_CASE("reset or undo keeps an open recording's boundary current", "[studio][commands]") {
	Fixture fixture;
	const Entity workspace = fixture.Workspace();
	fixture.Insert(workspace, "Before recording");
	const auto recording = fixture.Log.TryBeginRecording("Recording");
	REQUIRE(recording.has_value());

	const auto checkRecording = [&] {
		const Entity first = fixture.Insert(workspace, "First recorded");
		const Entity second = fixture.Insert(workspace, "Second recorded");

		SECTION("cancel reverses all edits made after the new boundary") {
			REQUIRE(fixture.Log.FinishRecording(*recording, studio::FinishOperation::Cancel));
			CHECK_FALSE(fixture.Alive(first));
			CHECK_FALSE(fixture.Alive(second));
		}
		SECTION("append publishes all edits made after the new boundary") {
			size_t published = 0;
			studio::CommandLog::Watcher watcher;
			watcher.Committed = [&](uint64_t, std::span<const studio::Command> commands) {
				published = commands.size();
			};
			fixture.Log.Watch(std::move(watcher));
			REQUIRE(fixture.Log.FinishRecording(*recording, studio::FinishOperation::Append));
			CHECK(published == 2);
			REQUIRE(fixture.Log.Undo());
			CHECK_FALSE(fixture.Alive(first));
			CHECK_FALSE(fixture.Alive(second));
		}
	};
	SECTION("resetting history during a recording") {
		fixture.Log.ResetWaypoints();
		checkRecording();
	}
	SECTION("undoing prior history during a recording") {
		REQUIRE(fixture.Log.Undo());
		checkRecording();
	}
	SECTION("undoing and redoing prior history during a recording") {
		REQUIRE(fixture.Log.Undo());
		REQUIRE(fixture.Log.Redo());
		checkRecording();
	}
	SECTION("clearing history during a recording") {
		fixture.Log.Clear();
		checkRecording();
	}
}

TEST_CASE("redoing an open recording's edits keeps them inside the recording", "[studio][commands]") {
	Fixture fixture;
	const Entity workspace = fixture.Workspace();
	const Entity previous = fixture.Insert(workspace, "Previous");
	const auto recording = fixture.Log.TryBeginRecording("Recording");
	REQUIRE(recording.has_value());
	const Entity created = fixture.Insert(workspace, "Recorded");
	REQUIRE(fixture.Log.Undo());
	CHECK_FALSE(fixture.Alive(created));
	REQUIRE(fixture.Log.Redo());

	SECTION("cancel reverses the redone edit") {
		REQUIRE(fixture.Log.FinishRecording(*recording, studio::FinishOperation::Cancel));
		CHECK(fixture.Alive(previous));
		CHECK(fixture.ChildCount(workspace) == 3);
	}
	SECTION("append folds the redone edit into the previous waypoint") {
		size_t published = 0;
		studio::CommandLog::Watcher watcher;
		watcher.Committed = [&](uint64_t, std::span<const studio::Command> commands) {
			published = commands.size();
		};
		fixture.Log.Watch(std::move(watcher));
		REQUIRE(fixture.Log.FinishRecording(*recording, studio::FinishOperation::Append));
		CHECK(published == 1);
		REQUIRE(fixture.Log.Undo());
		CHECK_FALSE(fixture.Alive(previous));
		CHECK(fixture.ChildCount(workspace) == 2);
	}
}

TEST_CASE("cancelling a recording discards its undone edits", "[studio][commands]") {
	Fixture fixture;
	const Entity workspace = fixture.Workspace();
	const Entity previous = fixture.Insert(workspace, "Previous");
	const auto recording = fixture.Log.TryBeginRecording("Recording");
	REQUIRE(recording.has_value());
	const Entity recorded = fixture.Insert(workspace, "Recorded");
	REQUIRE(fixture.Log.Undo());
	CHECK_FALSE(fixture.Alive(recorded));

	SECTION("cancel cannot redo the cancelled edit") {
		REQUIRE(fixture.Log.FinishRecording(*recording, studio::FinishOperation::Cancel));
		CHECK(fixture.Alive(previous));
		CHECK_FALSE(fixture.Log.CanRedo());
	}
	SECTION("cancel preserves redo for prior history") {
		REQUIRE(fixture.Log.Undo());
		CHECK_FALSE(fixture.Alive(previous));
		REQUIRE(fixture.Log.FinishRecording(*recording, studio::FinishOperation::Cancel));
		REQUIRE(fixture.Log.CanRedo());
		REQUIRE(fixture.Log.Redo());
		CHECK(fixture.ChildCount(workspace) == 3);
		CHECK_FALSE(fixture.Log.CanRedo());
	}
}

TEST_CASE("cancelling a recording preserves prior redo order", "[studio][commands]") {
	Fixture fixture;
	const Entity workspace = fixture.Workspace();
	fixture.Insert(workspace, "First");
	fixture.Insert(workspace, "Second");
	const auto recording = fixture.Log.TryBeginRecording("Recording");
	REQUIRE(recording.has_value());
	fixture.Insert(workspace, "Cancelled");
	REQUIRE(fixture.Log.Undo());
	REQUIRE(fixture.Log.Undo());
	REQUIRE(fixture.Log.Undo());
	REQUIRE(fixture.Log.FinishRecording(*recording, studio::FinishOperation::Cancel));

	REQUIRE(fixture.Log.Redo());
	CHECK(fixture.Log.NextUndo() == "Insert First");
	REQUIRE(fixture.Log.Redo());
	CHECK(fixture.Log.NextUndo() == "Insert Second");
	CHECK_FALSE(fixture.Log.CanRedo());
	CHECK(fixture.ChildCount(workspace) == 4);
}

TEST_CASE("a disabled log records nothing and forgets what it had", "[studio][commands]") {
	Fixture fixture;
	const Entity workspace = fixture.Workspace();

	fixture.Insert(workspace, "Before");
	REQUIRE(fixture.Log.CanUndo());

	fixture.Log.SetEnabled(false);
	CHECK_FALSE(fixture.Log.Enabled());

	// Cleared, and it does not repopulate when it comes back - a log that kept
	// its stacks across a period it was told not to watch would offer to undo
	// across edits it never saw.
	CHECK_FALSE(fixture.Log.CanUndo());

	fixture.Insert(workspace, "During");
	CHECK(fixture.Log.Depth() == 0);
	CHECK_FALSE(fixture.Log.TryBeginRecording("Anything").has_value());

	fixture.Log.SetEnabled(true);
	CHECK_FALSE(fixture.Log.CanUndo());

	fixture.Insert(workspace, "After");
	CHECK(fixture.Log.Depth() == 1);
}

TEST_CASE("a watcher hears a waypoint once, whole", "[studio][commands]") {
	Fixture fixture;
	const Entity workspace = fixture.Workspace();

	size_t started = 0;
	size_t finished = 0;
	std::vector<size_t> commits;
	std::string undone;
	std::string redone;

	studio::CommandLog::Watcher watcher;
	watcher.RecordingStarted = [&](const studio::Recording &) { started++; };
	watcher.RecordingFinished = [&](const studio::Recording &, studio::FinishOperation) { finished++; };
	watcher.Committed = [&](uint64_t, std::span<const studio::Command> group) {
		commits.push_back(group.size());
	};
	watcher.Undone = [&](std::string_view name) { undone = std::string(name); };
	watcher.Redone = [&](std::string_view name) { redone = std::string(name); };
	fixture.Log.Watch(watcher);

	// An ungrouped command publishes on its own, one command at a time.
	fixture.Insert(workspace, "Alone");
	REQUIRE(commits.size() == 1);
	CHECK(commits[0] == 1);

	// A recording publishes once, on the commit, with everything in it. A peer
	// that applied half of a group would show a state the author never saw.
	const auto recording = fixture.Log.TryBeginRecording("Three");
	REQUIRE(recording.has_value());
	fixture.Insert(workspace, "A");
	fixture.Insert(workspace, "B");
	fixture.Insert(workspace, "C");
	CHECK(commits.size() == 1);

	REQUIRE(fixture.Log.FinishRecording(*recording, studio::FinishOperation::Commit));
	REQUIRE(commits.size() == 2);
	CHECK(commits[1] == 3);

	CHECK(started == 1);
	CHECK(finished == 1);

	REQUIRE(fixture.Log.Undo());
	CHECK(undone == "Insert C");
	REQUIRE(fixture.Log.Redo());
	CHECK(redone == "Insert C");

	// Undo and redo publish nothing. They are this author's navigation of their
	// own history, and a peer is told about the edits rather than about the
	// walking.
	CHECK(commits.size() == 2);
}

TEST_CASE("a recording replaced during its start callback is refused", "[studio][commands]") {
	Fixture fixture;
	std::optional<std::string> replacement;
	std::string startedIdentifier;
	studio::CommandLog::Watcher watcher;
	watcher.RecordingStarted = [&](const studio::Recording &recording) {
		if (recording.Name != "A") return;
		fixture.Log.FinishRecording(recording.Identifier, studio::FinishOperation::Cancel);
		startedIdentifier = recording.Identifier;
		replacement = fixture.Log.TryBeginRecording("B");
	};
	fixture.Log.Watch(std::move(watcher));
	const auto first = fixture.Log.TryBeginRecording("A");
	REQUIRE(replacement);
	CHECK_FALSE(first);
	CHECK_FALSE(startedIdentifier.empty());
	CHECK(startedIdentifier != *replacement);
	CHECK(fixture.Log.IsRecordingInProgress(*replacement));
	CHECK(fixture.Log.InFlight().Name == "B");
}

TEST_CASE("recording listeners cannot replace the waypoint being published", "[studio][commands]") {
	Fixture fixture;
	const Entity workspace = fixture.Workspace();
	std::vector<std::string> published;
	bool undone = false;
	studio::CommandLog::Watcher watcher;
	watcher.Committed = [&](uint64_t, std::span<const studio::Command> group) {
		for (const auto &command : group)
			published.push_back(command.Description);
	};
	watcher.RecordingFinished = [&](const studio::Recording &, studio::FinishOperation) {
		undone = fixture.Log.Undo();
		fixture.Insert(workspace, "B");
	};
	fixture.Log.Watch(std::move(watcher));
	const auto recording = fixture.Log.TryBeginRecording("A");
	REQUIRE(recording);
	fixture.Insert(workspace, "A");
	REQUIRE(fixture.Log.FinishRecording(*recording, studio::FinishOperation::Commit));
	CHECK(undone);
	REQUIRE(published.size() == 2);
	CHECK(published[0] == "Insert A");
	CHECK(published[1] == "Insert B");
}

TEST_CASE("recording listeners can commit a new waypoint after cancellation", "[studio][commands]") {
	Fixture fixture;
	const Entity workspace = fixture.Workspace();
	std::vector<std::string> published;
	bool nestedFinished = false;
	studio::CommandLog::Watcher watcher;
	watcher.Committed = [&](uint64_t, std::span<const studio::Command> group) {
		for (const auto &command : group)
			published.push_back(command.Description);
	};
	watcher.RecordingFinished = [&](const studio::Recording &, studio::FinishOperation operation) {
		if (operation != studio::FinishOperation::Cancel) return;
		const auto nested = fixture.Log.TryBeginRecording("B");
		if (!nested) return;
		fixture.Insert(workspace, "B");
		nestedFinished = fixture.Log.FinishRecording(*nested, studio::FinishOperation::Commit);
	};
	fixture.Log.Watch(std::move(watcher));
	const auto recording = fixture.Log.TryBeginRecording("A");
	REQUIRE(recording);
	const Entity canceled = fixture.Insert(workspace, "A");
	REQUIRE(fixture.Log.FinishRecording(*recording, studio::FinishOperation::Cancel));
	CHECK(nestedFinished);
	CHECK_FALSE(fixture.Alive(canceled));
	REQUIRE(published.size() == 1);
	CHECK(published[0] == "Insert B");
}

TEST_CASE("a cancelled recording is never published", "[studio][commands]") {
	Fixture fixture;
	const Entity workspace = fixture.Workspace();

	size_t commits = 0;
	studio::CommandLog::Watcher watcher;
	watcher.Committed = [&](uint64_t, std::span<const studio::Command>) { commits++; };
	fixture.Log.Watch(watcher);

	const auto recording = fixture.Log.TryBeginRecording("Abandoned");
	REQUIRE(recording.has_value());
	fixture.Insert(workspace, "A");
	fixture.Insert(workspace, "B");
	REQUIRE(fixture.Log.FinishRecording({}, studio::FinishOperation::Cancel));

	// The rollback already put everything back, so there is nothing for a peer
	// to apply - and telling them would be telling them about a state that
	// never existed anywhere.
	CHECK(commits == 0);
	CHECK(fixture.ChildCount(workspace) == 2);
}

TEST_CASE("a foreign waypoint lands without entering this author's history", "[studio][commands]") {
	// Two logs over two universes, which is what two editors are. What crosses
	// is what `Watcher::Committed` hands over and nothing else.
	Fixture author;
	Fixture peer;

	std::vector<studio::Command> sent;
	studio::CommandLog::Watcher watcher;
	watcher.Committed = [&](uint64_t, std::span<const studio::Command> group) {
		sent.assign(group.begin(), group.end());
	};
	author.Log.Watch(watcher);

	const Entity theirs = author.Workspace();
	const auto recording = author.Log.TryBeginRecording("Insert two");
	REQUIRE(recording.has_value());
	author.Insert(theirs, "A");
	author.Insert(theirs, "B");
	REQUIRE(author.Log.FinishRecording(*recording, studio::FinishOperation::Commit));
	REQUIRE(sent.size() == 2);

	// The peer's world is its own and the sender's ids mean nothing here, so
	// both are repointed the way a real edit stream does before applying: the
	// world by name, and each id the command *reads* by resolving it locally.
	// A create binds its own subject as a side effect of rebuilding.
	const Entity ours = peer.Workspace();
	for (studio::Command &command : sent) {
		command.World = peer.Scene;
		peer.Log.Adopt(command.OldParent, peer.Scene, ours);
	}

	CHECK(peer.Log.ApplyForeign(sent) == 2);
	CHECK(peer.ChildCount(peer.Workspace()) == 4);

	// **Applied and not recorded.** Ctrl+Z is a promise about what you did, and
	// an editor that reversed a colleague's change because you pressed it once
	// too often would be an editor nobody could work in.
	CHECK(peer.Log.Depth() == 0);
	CHECK_FALSE(peer.Log.CanUndo());
}

TEST_CASE("per-call replay callbacks precede listeners and do not persist", "[studio][commands]") {
	Fixture fixture;
	std::vector<std::string> events;
	CHECK_FALSE(fixture.Log.Undo([&] { events.push_back("unexpected"); }));
	CHECK_FALSE(fixture.Log.Redo([&] { events.push_back("unexpected"); }));
	CHECK(events.empty());

	fixture.Log.SetWorldRenameCoordinator([&](WorldId id, Name expected, Name desired) {
		return fixture.Worlds.NameOf(id) == expected &&
			   fixture.Worlds.Rename(id, desired) == engine::world::WorldStatus::Ok;
	});
	REQUIRE(fixture.Log.RenameWorld(fixture.Scene, Name("NewScene")));
	CommandLog::Watcher watcher;
	watcher.Undone = [&](std::string_view) { events.push_back("undo listener"); };
	watcher.Redone = [&](std::string_view) {
		events.push_back("redo listener");
		CHECK(fixture.Log.Undo());
	};
	fixture.Log.Watch(std::move(watcher));
	REQUIRE(fixture.Log.Undo([&] { events.push_back("undo hook"); }));
	REQUIRE(fixture.Log.Redo([&] { events.push_back("redo hook"); }));
	CHECK((
		events ==
		std::vector<std::string>{"undo hook", "undo listener", "redo hook", "redo listener", "undo listener"}
	));
}

TEST_CASE(
	"world rename moves portal names and refuses failed undo without moving history", "[studio][commands]"
) {
	Fixture fixture;
	const WorldId other = fixture.Worlds.Create({.Name = Name("Other")});
	Entity portal;
	fixture.Worlds.Enter(other, [&](Store &store) {
		portal = store.Create();
		engine::scene::Portal component;
		component.DestinationWorld = Name("Scene");
		store.Set(portal, component);
	});
	bool permitted = true;
	fixture.Log.SetWorldRenameCoordinator([&](WorldId id, Name expected, Name desired) {
		return permitted && fixture.Worlds.NameOf(id) == expected &&
			   fixture.Worlds.Rename(id, desired) == engine::world::WorldStatus::Ok;
	});
	const auto destination = [&] {
		Name value;
		fixture.Worlds.Enter(other, [&](Store &store) {
			value = store.Get<engine::scene::Portal>(portal)->DestinationWorld;
		});
		return value;
	};
	int finalized = 0;
	fixture.Log.SetWorldRenameFinalizer([&](WorldId id, Name previous) {
		CHECK(id == fixture.Scene);
		CHECK(previous == (finalized == 1 ? Name("NewScene") : Name("Scene")));
		++finalized;
		throw std::runtime_error("cleanup failure");
	});
	permitted = false;
	CHECK_FALSE(fixture.Log.RenameWorld(fixture.Scene, Name("NewScene")));
	CHECK_FALSE(fixture.Log.CanUndo());
	CHECK(destination() == Name("Scene"));
	permitted = true;
	REQUIRE(fixture.Log.RenameWorld(fixture.Scene, Name("NewScene")));
	fixture.Log.SetWaypoint("After rename");
	CHECK(finalized == 1);
	CHECK(fixture.Worlds.NameOf(fixture.Scene) == Name("NewScene"));
	CHECK(destination() == Name("NewScene"));
	permitted = false;
	CHECK_FALSE(fixture.Log.Undo());
	CHECK(fixture.Log.CanUndo());
	CHECK_FALSE(fixture.Log.CanRedo());
	CHECK(fixture.Worlds.NameOf(fixture.Scene) == Name("NewScene"));
	CHECK(destination() == Name("NewScene"));
	permitted = true;
	REQUIRE(fixture.Log.Undo());
	CHECK(fixture.Worlds.NameOf(fixture.Scene) == Name("Scene"));
	CHECK(destination() == Name("Scene"));
	CHECK(finalized == 2);
	permitted = false;
	CHECK_FALSE(fixture.Log.Redo());
	CHECK(fixture.Log.CanRedo());
	CHECK(destination() == Name("Scene"));
	permitted = true;
	REQUIRE(fixture.Log.Redo());
	CHECK(finalized == 3);
	CHECK(fixture.Worlds.NameOf(fixture.Scene) == Name("NewScene"));
	CHECK(destination() == Name("NewScene"));
}

TEST_CASE("world rename preserves unrelated existing destination", "[studio][commands]") {
	Fixture fixture;
	const WorldId other = fixture.Worlds.Create({.Name = Name("Other")});
	Entity oldPortal;
	Entity futurePortal;
	fixture.Worlds.Enter(other, [&](Store &store) {
		oldPortal = store.Create();
		futurePortal = store.Create();
		engine::scene::Portal portal;
		portal.DestinationWorld = Name("Scene");
		store.Set(oldPortal, portal);
		portal.DestinationWorld = Name("NewScene");
		store.Set(futurePortal, portal);
	});
	fixture.Log.SetWorldRenameCoordinator([&](WorldId id, Name expected, Name desired) {
		return fixture.Worlds.NameOf(id) == expected &&
			   fixture.Worlds.Rename(id, desired) == engine::world::WorldStatus::Ok;
	});
	REQUIRE(fixture.Log.RenameWorld(fixture.Scene, Name("NewScene")));
	REQUIRE(fixture.Log.Undo());
	fixture.Worlds.Enter(other, [&](Store &store) {
		CHECK(store.Get<engine::scene::Portal>(oldPortal)->DestinationWorld == Name("Scene"));
		CHECK(store.Get<engine::scene::Portal>(futurePortal)->DestinationWorld == Name("NewScene"));
	});
	REQUIRE(fixture.Log.Redo());
}

TEST_CASE("world rename follows a restored descendant portal", "[studio][commands]") {
	Fixture fixture;
	const WorldId destination = fixture.Worlds.Create({.Name = Name("Destination")});
	const Entity parent = fixture.Insert(fixture.Workspace(), "Parent");
	Entity portal = NULL_ENTITY;
	fixture.Worlds.Enter(fixture.Scene, [&](Store &store) {
		portal = store.CreateInstance(engine::ecs::Classes::Find(Name("Portal")), "NestedPortal");
		REQUIRE(portal != NULL_ENTITY);
		REQUIRE(store.SetParent(portal, parent));
		store.GetMutable<engine::scene::Portal>(portal)->DestinationWorld = Name("Destination");
	});
	fixture.Log.SetWorldRenameCoordinator([&](WorldId id, Name expected, Name desired) {
		return fixture.Worlds.NameOf(id) == expected &&
			   fixture.Worlds.Rename(id, desired) == engine::world::WorldStatus::Ok;
	});
	REQUIRE(fixture.Log.RenameWorld(destination, Name("Renamed")));
	fixture.Destroy(parent, "Parent");
	REQUIRE(fixture.Log.Undo());
	const Entity restoredParent = fixture.FindChild(fixture.Workspace(), "Parent");
	REQUIRE(restoredParent != NULL_ENTITY);
	const Entity restoredPortal = fixture.FindChild(restoredParent, "NestedPortal");
	REQUIRE(restoredPortal != NULL_ENTITY);
	REQUIRE(fixture.Log.Undo());
	fixture.Worlds.Enter(fixture.Scene, [&](Store &store) {
		CHECK(store.Get<engine::scene::Portal>(restoredPortal)->DestinationWorld == Name("Destination"));
	});
	REQUIRE(fixture.Log.Redo());
	fixture.Worlds.Enter(fixture.Scene, [&](Store &store) {
		CHECK(store.Get<engine::scene::Portal>(restoredPortal)->DestinationWorld == Name("Renamed"));
	});
	REQUIRE(fixture.Log.Redo());
	REQUIRE(fixture.Log.Undo());
	REQUIRE(fixture.Log.Undo());
}

TEST_CASE("forgetting a world cuts a dependent rename and earlier history", "[studio][commands]") {
	Fixture fixture;
	fixture.Insert(fixture.Workspace(), "Before");
	const WorldId other = fixture.Worlds.Create({.Name = Name("Other")});
	fixture.Worlds.Enter(other, [&](Store &store) {
		const Entity portal = store.Create();
		engine::scene::Portal component;
		component.DestinationWorld = Name("Scene");
		store.Set(portal, component);
	});
	fixture.Log.SetWorldRenameCoordinator([&](WorldId id, Name expected, Name desired) {
		return fixture.Worlds.NameOf(id) == expected &&
			   fixture.Worlds.Rename(id, desired) == engine::world::WorldStatus::Ok;
	});
	REQUIRE(fixture.Log.RenameWorld(fixture.Scene, Name("NewScene")));
	fixture.Insert(fixture.Workspace(), "After");
	REQUIRE(fixture.Log.Depth() == 3);
	fixture.Log.Forget(other);
	REQUIRE(fixture.Log.Depth() == 1);
	REQUIRE(fixture.Log.Undo());
	CHECK(fixture.Worlds.NameOf(fixture.Scene) == Name("NewScene"));
	CHECK_FALSE(fixture.Log.CanUndo());
}
