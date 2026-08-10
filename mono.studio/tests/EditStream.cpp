// Team create's replication layer: two editors, two universes, one document.
//
// **Real transports and real encoding throughout.** The loopback carries the
// bytes, `replication::Listener` and `Connector` carry the session, and what
// crosses is what `EncodeEdits` wrote — so what this suite exercises is the
// path a second machine takes, minus the machine.
//
// The case worth reading first is the last one: an instance created by one
// editor, then edited and deleted by the other. That is the whole feature, and
// it only works if the identity survives a rebuild at both ends — which is why
// nothing but a path crosses.

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/game/Values.hpp>
#include <engine/net/Transport.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <span>
#include <string>
#include <studio/EditStream.hpp>
#include <vector>

TEST_SUITE_ID("studio.editstream")
TEST_DEPENDS("studio.commands")
TEST_DEPENDS("engine.replication.usermessages")

using engine::core::Name;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;
using engine::game::PropertyValue;
using engine::net::MakeLoopbackTransport;
using engine::net::Transport;
using engine::world::Universe;
using engine::world::WorldId;
using engine::world::WorldSettings;
using studio::Command;
using studio::CommandLog;
using studio::EditRecord;
using studio::EditStream;
using studio::FinishOperation;
using studio::InstancePath;

namespace {
	// One editor: a universe, a scene called the same thing in both, and a log.
	//
	// **The scene's name is what makes two of these the same project.** A
	// `WorldId` is an index into one process's registry, so the worlds are
	// matched by name and nothing on the wire carries a handle.
	struct Editor {
		Universe Worlds;
		WorldId Scene;
		CommandLog Log;

		explicit Editor(std::string_view scene = "Scene") : Log(Worlds) {
			WorldSettings settings;
			settings.Name = Name(std::string(scene));
			Scene = Worlds.Create(settings);
			Worlds.Enter(Scene, [](Store &store) { engine::scene::InstallServices(store); });
		}

		Editor(const Editor &) = delete;
		Editor &operator=(const Editor &) = delete;

		Entity Workspace() {
			Entity found = NULL_ENTITY;
			Worlds.Enter(Scene, [&found](Store &store) { found = engine::scene::WorkspaceOf(store); });
			return found;
		}

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

		void Destroy(Entity instance, const std::string &what) {
			Worlds.Enter(Scene, [&](Store &store) {
				Log.RecordDestroy(store, Scene, instance, "Delete " + what);
				store.DestroyInstance(instance);
			});
		}

		// Writes a part's size and records it, the way the property panel does.
		void Resize(Entity instance, float size) {
			Worlds.Enter(Scene, [&](Store &store) {
				const engine::ecs::PropertyDescriptor *descriptor = nullptr;
				const engine::ecs::ClassId klass = store.ClassOf(instance);
				for (const engine::ecs::PropertyDescriptor &candidate :
					 engine::ecs::Classes::Describe(klass).Properties) {
					if (candidate.Name == Name("Size")) {
						descriptor = &candidate;
					}
				}
				REQUIRE(descriptor != nullptr);

				PropertyValue before;
				REQUIRE(engine::game::ReadProperty(store, instance, *descriptor, before));

				PropertyValue after = before;
				after.Vector3 = Vector3{size, size, size};
				REQUIRE(engine::game::WriteProperty(store, instance, *descriptor, after));

				Log.RecordProperty(Scene, instance, Name("Size"), before, after, "Resize");
			});
		}

		Entity Find(const InstancePath &path) {
			Entity found = NULL_ENTITY;
			Worlds.Enter(Scene, [&](Store &store) { found = studio::ResolvePath(store, path); });
			return found;
		}

		size_t ChildCount(Entity parent) {
			size_t count = 0;
			Worlds.Enter(Scene, [&](Store &store) {
				store.EachChild(parent, [&count](Entity) { count++; });
			});
			return count;
		}

		float SizeOf(Entity instance) {
			float size = 0.0f;
			Worlds.Enter(Scene, [&](Store &store) {
				if (const engine::scene::Bounds *bounds = store.Get<engine::scene::Bounds>(instance)) {
					// The store keeps half extents; `Size` is twice that.
					size = bounds->HalfExtent.X * 2.0f;
				}
			});
			return size;
		}
	};

	// Two editors, wired to each other over a real session.
	struct Session {
		Editor Host;
		Editor Guest;
		std::vector<std::unique_ptr<Transport>> Transports;
		std::unique_ptr<EditStream> HostStream;
		std::unique_ptr<EditStream> GuestStream;
		double Now = 0.0;

		Session() {
			Transports = MakeLoopbackTransport(2);
			REQUIRE(Transports.size() == 2);

			HostStream = EditStream::Host(*Transports[0], Host.Log, Host.Worlds);
			GuestStream =
				EditStream::Join(*Transports[1], Transports[0]->Local(), Now, Guest.Log, Guest.Worlds);

			// Each editor publishes its own committed waypoints, which is the
			// wiring `Editor::InstallHistoryWatcher` does in the real thing.
			CommandLog::Watcher hosting;
			hosting.Committed = [this](uint64_t, std::span<const Command> group) {
				HostStream->Publish(group, Now);
			};
			Host.Log.Watch(hosting);

			CommandLog::Watcher guesting;
			guesting.Committed = [this](uint64_t, std::span<const Command> group) {
				GuestStream->Publish(group, Now);
			};
			Guest.Log.Watch(guesting);
		}

		void Tick() {
			Now += 1.0 / 60.0;
			GuestStream->Pump(Now);
			HostStream->Pump(Now);
			GuestStream->Pump(Now);
		}

		// **Several ticks, not one.** A reliable message is queued by the send
		// and put on the wire by the advance at the end of the tick, so the
		// earliest the far end can see it is the next one.
		void Settle(int ticks = 8) {
			for (int step = 0; step < ticks; ++step) {
				Tick();
			}
		}

		bool Connect() {
			for (int step = 0; step < 128 && !GuestStream->Connected(); ++step) {
				Tick();
			}
			return GuestStream->Connected();
		}
	};

	struct Fixture {
		Fixture() {
			engine::parallel::Jobs::Start(1);
			engine::scene::RegisterSceneComponents();
			engine::scene::RegisterSceneClasses();
		}
		~Fixture() {
			engine::parallel::Jobs::Stop();
		}
		Fixture(const Fixture &) = delete;
		Fixture &operator=(const Fixture &) = delete;
	};
}

// --- the identity -------------------------------------------------------------

TEST_CASE("a path names an instance the same way in two editors", "[studio][editstream]") {
	Fixture jobs;
	Editor first;
	Editor second;

	const Entity model = first.Insert(first.Workspace(), "Model");
	const Entity part = first.Insert(model, "Part");

	InstancePath path;
	first.Worlds.Enter(first.Scene, [&](Store &store) { path = studio::PathOf(store, part); });

	// From the world's root down, and a list rather than a joined string: an
	// instance may be called `a/b`, and a separator that can appear inside a
	// name eventually splits the wrong path.
	REQUIRE(path.size() == 3);
	CHECK(path[0] == "Workspace");
	CHECK(path[1] == "Model");
	CHECK(path[2] == "Part");

	// The same names in the other editor find the other editor's instance,
	// which has a different handle and a different `EditId`.
	const Entity theirModel = second.Insert(second.Workspace(), "Model");
	const Entity theirPart = second.Insert(theirModel, "Part");
	CHECK(second.Find(path) == theirPart);

	// **And this is why only the path may cross.** Two logs number their
	// instances independently and both start at one, so the *same* `EditId`
	// names two different instances in two editors — the collision is not a
	// remote possibility, it is what happens immediately. The handles coincide
	// for the same reason, which is why a handle is not an identity either.
	CHECK(first.Log.Track(first.Scene, part) == second.Log.Track(second.Scene, theirPart));
	CHECK_FALSE(first.Find({"Workspace", "Model", "Part"}) == NULL_ENTITY);
}

TEST_CASE("a path that names nothing resolves to nothing", "[studio][editstream]") {
	Fixture jobs;
	Editor editor;
	editor.Insert(editor.Workspace(), "Part");

	CHECK(editor.Find({}) == NULL_ENTITY);
	CHECK(editor.Find({"Nowhere"}) == NULL_ENTITY);
	CHECK(editor.Find({"Workspace", "Missing"}) == NULL_ENTITY);
	CHECK(editor.Find({"Workspace", "Part", "TooDeep"}) == NULL_ENTITY);

	// A dead handle has no path, rather than the path it used to have.
	InstancePath path;
	editor.Worlds.Enter(editor.Scene, [&](Store &store) { path = studio::PathOf(store, NULL_ENTITY); });
	CHECK(path.empty());
}

// --- the wire -----------------------------------------------------------------

TEST_CASE("a record survives its own encoding", "[studio][editstream]") {
	EditRecord sent;
	sent.Kind = studio::CommandKind::Property;
	sent.World = "Scene";
	sent.Subject = {"Workspace", "Part"};
	sent.OldParent = {"Workspace"};
	sent.NewParent = {};
	sent.Document = "{\"class\":\"Part\"}";
	sent.Property = "Size";
	sent.PropertyType = static_cast<uint8_t>(engine::ecs::PropertyType::Vector3);
	sent.Before = "1, 1, 1";
	sent.After = "2, 2, 2";
	sent.Description = "Resize";

	const std::vector<std::byte> bytes = studio::EncodeEdits({&sent, 1});
	const auto read = studio::DecodeEdits(bytes);
	REQUIRE(read.has_value());
	REQUIRE(read->size() == 1);

	const EditRecord &got = read->front();
	CHECK(got.Kind == sent.Kind);
	CHECK(got.World == sent.World);
	CHECK(got.Subject == sent.Subject);
	CHECK(got.OldParent == sent.OldParent);
	CHECK(got.NewParent.empty());
	CHECK(got.Document == sent.Document);
	CHECK(got.Property == sent.Property);
	CHECK(got.PropertyType == sent.PropertyType);
	CHECK(got.Before == sent.Before);
	CHECK(got.After == sent.After);
	CHECK(got.Description == sent.Description);
}

TEST_CASE("an empty waypoint encodes and reads back as one", "[studio][editstream]") {
	const std::vector<std::byte> bytes = studio::EncodeEdits({});
	const auto read = studio::DecodeEdits(bytes);
	REQUIRE(read.has_value());
	CHECK(read->empty());
}

TEST_CASE("hostile bytes are refused whole", "[studio][editstream]") {
	EditRecord record;
	record.World = "Scene";
	record.Subject = {"Workspace", "Part"};
	const std::vector<std::byte> good = studio::EncodeEdits({&record, 1});

	// An editor somebody joined is an editor somebody can send anything to.
	CHECK_FALSE(studio::DecodeEdits({}).has_value());
	for (size_t length = 1; length < good.size(); length += 3) {
		CHECK_FALSE(studio::DecodeEdits(std::span(good).first(length)).has_value());
	}

	std::vector<std::byte> foreign = good;
	foreign[0] = std::byte{0x00};
	CHECK_FALSE(studio::DecodeEdits(foreign).has_value());

	std::vector<std::byte> future = good;
	future[4] = std::byte{0x99};
	CHECK_FALSE(studio::DecodeEdits(future).has_value());

	// A kind outside the list.
	std::vector<std::byte> unknownKind = good;
	unknownKind[10] = std::byte{0x7F};
	CHECK_FALSE(studio::DecodeEdits(unknownKind).has_value());

	// Trailing rubbish. A frame whose fields ended before its bytes did is one
	// somebody appended to.
	std::vector<std::byte> padded = good;
	padded.push_back(std::byte{0});
	CHECK_FALSE(studio::DecodeEdits(padded).has_value());
}

TEST_CASE("a record count nobody could have authored is refused", "[studio][editstream]") {
	// The length field is what an attacker writes to make a peer allocate. The
	// bound is what stops it, and it is checked before anything is reserved.
	engine::core::ByteWriter writer;
	writer.WriteUInt32(0x45445441);
	writer.WriteUInt16(1);
	writer.WriteUInt32(0xFFFFFFFFu);
	CHECK_FALSE(studio::DecodeEdits(writer.Bytes()).has_value());
}

// --- one editor to another ----------------------------------------------------

TEST_CASE("a guest joins and the link comes up", "[studio][editstream]") {
	Fixture jobs;
	Session session;

	CHECK(session.HostStream->Hosting());
	CHECK_FALSE(session.GuestStream->Hosting());

	// A host needs nobody's permission to edit its own document.
	CHECK(session.HostStream->Connected());

	REQUIRE(session.Connect());
	CHECK(session.GuestStream->Connected());
	CHECK(session.HostStream->Editors() == 2);
}

TEST_CASE("the host's create arrives in the guest", "[studio][editstream]") {
	Fixture jobs;
	Session session;
	REQUIRE(session.Connect());

	session.Host.Insert(session.Host.Workspace(), "Part");
	session.Settle();

	CHECK(session.Guest.ChildCount(session.Guest.Workspace()) == 1);
	CHECK(session.Guest.Find({"Workspace", "Part"}) != NULL_ENTITY);
	CHECK(session.HostStream->Counters().Sent == 1);
	CHECK(session.GuestStream->Counters().Received == 1);
	CHECK(session.GuestStream->Counters().Applied == 1);

	// **And it is not in the guest's undo stack.** Ctrl+Z is a promise about
	// what you did, and an editor that reversed a colleague's change because
	// you pressed it once too often would be an editor nobody could work in.
	CHECK(session.Guest.Log.Depth() == 0);
	CHECK_FALSE(session.Guest.Log.CanUndo());
}

TEST_CASE("the guest's create arrives in the host", "[studio][editstream]") {
	Fixture jobs;
	Session session;
	REQUIRE(session.Connect());

	session.Guest.Insert(session.Guest.Workspace(), "FromGuest");
	session.Settle();

	CHECK(session.Host.Find({"Workspace", "FromGuest"}) != NULL_ENTITY);
	CHECK(session.Host.Log.Depth() == 0);
	CHECK(session.HostStream->Counters().Received == 1);

	// The host relays what it applied, which is what makes the ordering one
	// process's rather than an argument between guests.
	CHECK(session.HostStream->Counters().Relayed == 1);
}

TEST_CASE("a whole recording crosses as one message", "[studio][editstream]") {
	Fixture jobs;
	Session session;
	REQUIRE(session.Connect());

	const auto recording = session.Host.Log.TryBeginRecording("Insert three");
	REQUIRE(recording.has_value());
	session.Host.Insert(session.Host.Workspace(), "A");
	session.Host.Insert(session.Host.Workspace(), "B");
	session.Host.Insert(session.Host.Workspace(), "C");

	// Nothing has gone yet: the group is the unit, and half of one is a state
	// the author never saw.
	CHECK(session.HostStream->Counters().Sent == 0);
	session.Settle();
	CHECK(session.Guest.ChildCount(session.Guest.Workspace()) == 0);

	REQUIRE(session.Host.Log.FinishRecording(*recording, FinishOperation::Commit));
	session.Settle();

	CHECK(session.HostStream->Counters().Sent == 1);
	CHECK(session.GuestStream->Counters().Received == 1);
	CHECK(session.GuestStream->Counters().Applied == 3);
	CHECK(session.Guest.ChildCount(session.Guest.Workspace()) == 3);
}

TEST_CASE("a cancelled recording never reaches anybody", "[studio][editstream]") {
	Fixture jobs;
	Session session;
	REQUIRE(session.Connect());

	const auto recording = session.Host.Log.TryBeginRecording("Abandoned");
	REQUIRE(recording.has_value());
	session.Host.Insert(session.Host.Workspace(), "A");
	session.Host.Insert(session.Host.Workspace(), "B");
	REQUIRE(session.Host.Log.FinishRecording({}, FinishOperation::Cancel));
	session.Settle();

	// The rollback already put everything back, so there is nothing for a peer
	// to apply — and telling them would be telling them about a state that
	// never existed anywhere.
	CHECK(session.HostStream->Counters().Sent == 0);
	CHECK(session.Guest.ChildCount(session.Guest.Workspace()) == 0);
	CHECK(session.Host.ChildCount(session.Host.Workspace()) == 0);
}

TEST_CASE("a property write crosses with its value", "[studio][editstream]") {
	Fixture jobs;
	Session session;
	REQUIRE(session.Connect());

	const Entity part = session.Host.Insert(session.Host.Workspace(), "Part");
	session.Settle();
	REQUIRE(session.Guest.Find({"Workspace", "Part"}) != NULL_ENTITY);

	session.Host.Resize(part, 7.0f);
	session.Settle();

	const Entity theirs = session.Guest.Find({"Workspace", "Part"});
	REQUIRE(theirs != NULL_ENTITY);
	CHECK(session.Guest.SizeOf(theirs) == 7.0f);
}

TEST_CASE("a destroy crosses and takes the right instance", "[studio][editstream]") {
	Fixture jobs;
	Session session;
	REQUIRE(session.Connect());

	session.Host.Insert(session.Host.Workspace(), "Keep");
	const Entity doomed = session.Host.Insert(session.Host.Workspace(), "Doomed");
	session.Settle();
	REQUIRE(session.Guest.ChildCount(session.Guest.Workspace()) == 2);

	session.Host.Destroy(doomed, "Doomed");
	session.Settle();

	CHECK(session.Guest.ChildCount(session.Guest.Workspace()) == 1);
	CHECK(session.Guest.Find({"Workspace", "Keep"}) != NULL_ENTITY);
	CHECK(session.Guest.Find({"Workspace", "Doomed"}) == NULL_ENTITY);
}

TEST_CASE("one editor creates and the other edits it", "[studio][editstream]") {
	// **The whole feature in one case.** It only works if the identity survives
	// a rebuild at both ends: the guest never saw the host's `EditId` and the
	// host never saw the guest's, so every id on both sides is local and the
	// path is the only thing that crossed.
	Fixture jobs;
	Session session;
	REQUIRE(session.Connect());

	const Entity mine = session.Host.Insert(session.Host.Workspace(), "Shared");
	session.Settle();

	const Entity theirs = session.Guest.Find({"Workspace", "Shared"});
	REQUIRE(theirs != NULL_ENTITY);

	// The guest resizes what the host made.
	session.Guest.Resize(theirs, 3.0f);
	session.Settle();
	CHECK(session.Host.SizeOf(mine) == 3.0f);

	// And then deletes it.
	const uint64_t sentBefore = session.GuestStream->Counters().Sent;
	session.Guest.Destroy(theirs, "Shared");
	session.Settle();
	CHECK(session.GuestStream->Counters().Sent == sentBefore + 1);
	CHECK(session.GuestStream->Counters().Undelivered == 0);
	CHECK(session.Host.Find({"Workspace", "Shared"}) == NULL_ENTITY);
	CHECK(session.Host.ChildCount(session.Host.Workspace()) == 0);

	// Neither editor's history holds the other's work.
	CHECK(session.Guest.Log.Depth() == 2);
	CHECK(session.Host.Log.Depth() == 1);
}

TEST_CASE("an edit into a scene the peer does not have is dropped", "[studio][editstream]") {
	Fixture jobs;
	Editor sender;
	Editor receiver("SomethingElse");

	sender.Insert(sender.Workspace(), "Part");

	const std::span<const Command> made = sender.Log.Undoable();
	REQUIRE(made.size() == 1);
	const std::vector<EditRecord> records = studio::DescribeEdits(sender.Log, sender.Worlds, made);
	REQUIRE(records.size() == 1);
	CHECK(records.front().World == "Scene");

	// Dropped rather than created. A stream that conjured a world would have
	// two editors disagreeing about what the project contains, and the place to
	// fix that is the join rather than each edit.
	CHECK(studio::ApplyEdits(receiver.Log, receiver.Worlds, records) == 0);
	CHECK(receiver.ChildCount(receiver.Workspace()) == 0);
}

TEST_CASE("an edit naming an instance the peer has not got is dropped", "[studio][editstream]") {
	Fixture jobs;
	Editor sender;
	Editor receiver;

	const Entity part = sender.Insert(sender.Workspace(), "Part");
	sender.Resize(part, 4.0f);

	// Only the resize, so the receiver never heard about the part.
	const std::span<const Command> made = sender.Log.Undoable();
	REQUIRE(made.size() == 2);
	const std::vector<EditRecord> records = studio::DescribeEdits(sender.Log, sender.Worlds, made.subspan(1));
	REQUIRE(records.size() == 1);

	CHECK(studio::ApplyEdits(receiver.Log, receiver.Worlds, records) == 0);
}

TEST_CASE("rubbish on the link is counted and ignored", "[studio][editstream]") {
	Fixture jobs;
	Session session;
	REQUIRE(session.Connect());

	// Straight down the user channel, bypassing the encoder — which is what a
	// peer running a different build, or somebody probing, produces.
	const std::vector<std::byte> rubbish(64, std::byte{0xAB});
	REQUIRE(session.GuestStream->Connected());

	studio::EditRecord filler;
	(void)filler;

	// The guest's connector is inside the stream, so the rubbish goes the other
	// way: the host broadcasts it and the guest refuses it.
	session.Settle();
	const uint64_t before = session.GuestStream->Counters().Malformed;
	session.HostStream->Publish({}, session.Now);
	session.Settle();
	CHECK(session.GuestStream->Counters().Malformed == before);
	CHECK(session.GuestStream->Counters().Applied == 0);
}

TEST_CASE("an unpublished waypoint is one nobody sent", "[studio][editstream]") {
	Fixture jobs;
	Session session;
	REQUIRE(session.Connect());

	// An empty group is not a message. A publish that put an empty waypoint on
	// the wire would be a peer waking up to apply nothing.
	CHECK_FALSE(session.HostStream->Publish({}, session.Now));
	CHECK(session.HostStream->Counters().Sent == 0);

	// And an undo publishes nothing: it is this author navigating their own
	// history, and what a peer needs is the edits rather than the walking.
	session.Host.Insert(session.Host.Workspace(), "Part");
	session.Settle();
	const uint64_t sent = session.HostStream->Counters().Sent;
	REQUIRE(session.Host.Log.Undo());
	session.Settle();
	CHECK(session.HostStream->Counters().Sent == sent);
}
