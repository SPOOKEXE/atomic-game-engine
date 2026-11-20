// Team create's session layer: what an editor announces, what it holds, and
// what it refuses.
//
// The wire is `network`'s and is covered there over a loopback. What belongs
// here is the editor's own shape - that watching announces nothing, that
// hosting produces an invitation somebody can pass on, and that leaving takes
// it all back down.

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/game/Values.hpp>
#include <engine/net/Endpoint.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <span>
#include <string>
#include <studio/Commands.hpp>
#include <studio/InstancePath.hpp>
#include <studio/TeamCreate.hpp>
#include <vector>

TEST_SUITE_ID("studio.teamcreate")
TEST_DEPENDS("network.presence")
TEST_DEPENDS("studio.editstream")

using studio::CommandLog;
using studio::TeamCreate;
using studio::TeamCreateSettings;

namespace {
	// A whole editor's worth of state, because `TeamCreate` borrows the command
	// log and the universe that arriving edits are applied through - hosting is
	// not a thing an editor does beside its document, it is a thing it does
	// *to* its document.
	struct Editor {
		engine::world::Universe Worlds;
		engine::world::WorldId Scene;
		CommandLog Log;
		TeamCreate Team;

		Editor() : Log(Worlds), Team(Log, Worlds) {
			engine::parallel::Jobs::Start(1);
			engine::scene::RegisterSceneComponents();
			engine::scene::RegisterSceneClasses();

			engine::world::WorldSettings settings;
			settings.Name = engine::core::Name("Scene");
			Scene = Worlds.Create(settings);
			Worlds.Enter(Scene, [](engine::ecs::Store &store) { engine::scene::InstallServices(store); });
		}

		~Editor() {
			engine::parallel::Jobs::Stop();
		}

		Editor(const Editor &) = delete;
		Editor &operator=(const Editor &) = delete;
	};
}

TEST_CASE("a fresh editor holds no socket and sees nobody", "[studio][teamcreate]") {
	Editor editor;
	TeamCreate &team = editor.Team;

	// The property that keeps this off every editor's start-up path: nothing
	// is opened until somebody asks.
	CHECK_FALSE(team.Watching());
	CHECK_FALSE(team.Hosting());
	CHECK(team.Peers().empty());
	CHECK(team.Invitation().empty());
	CHECK(team.Fault() == network::PresenceFault::None);

	// And pumping one that was never started is a null check.
	team.Pump(0.0);
	team.Leave(1.0);
	CHECK_FALSE(team.Watching());
}

TEST_CASE("watching announces nothing about this editor", "[studio][teamcreate]") {
	Editor editor;
	TeamCreate &team = editor.Team;
	const std::vector<std::string> noKeys;
	team.Watch({}, noKeys);

	CHECK(team.Watching());

	// **The assertion that matters.** Somebody who opened the panel to look has
	// not decided to invite anybody, and an editor that announced itself the
	// moment the window opened would be publishing a project name they had not
	// chosen to publish.
	CHECK_FALSE(team.Hosting());
	CHECK_FALSE(team.Session().Session.IsValid());
	CHECK(team.Invitation().empty());
}

TEST_CASE("hosting produces a session and an invitation to pass on", "[studio][teamcreate]") {
	Editor editor;
	TeamCreate &team = editor.Team;

	TeamCreateSettings offering;
	offering.Name = "the shared project";
	offering.Project = "Baseplate";
	offering.Secret = "let us both edit";
	offering.Port = 7777;

	std::string trouble;
	REQUIRE(team.Host(offering, trouble));
	CHECK(team.Watching());

	CHECK(team.Session().Use == network::Purpose::Studio);
	CHECK(team.Session().Admits == network::Access::Private);
	CHECK(team.Session().Name == "the shared project");
	CHECK(team.Session().Detail == "Baseplate");
	CHECK(team.Session().At.Port == 7777);
	CHECK(team.Session().Session.IsValid());

	// The key is the invitation, and it has to be copyable out of the panel or
	// a private session is a session of one.
	CHECK(team.Invitation().size() == network::SessionKey::BYTES * 2);

	// The same passphrase derives the same key on the machine at the other end,
	// which is the whole point of the passphrase path.
	auto theirs = network::SessionKey::FromPassphrase("let us both edit");
	REQUIRE(theirs.has_value());
	CHECK(theirs->Text() == team.Invitation());
}

TEST_CASE("a public session says so and carries no invitation", "[studio][teamcreate]") {
	Editor editor;
	TeamCreate &team = editor.Team;

	TeamCreateSettings offering;
	offering.Name = "anybody";
	offering.Port = 7777;

	std::string trouble;
	REQUIRE(team.Host(offering, trouble));
	CHECK(team.Session().Admits == network::Access::Public);
	CHECK(team.Invitation().empty());
}

TEST_CASE("leaving takes the session back down", "[studio][teamcreate]") {
	Editor editor;
	TeamCreate &team = editor.Team;

	TeamCreateSettings offering;
	offering.Name = "briefly";
	offering.Secret = "a passphrase";
	offering.Port = 7777;

	std::string trouble;
	REQUIRE(team.Host(offering, trouble));

	// Hosting is the running session, not whether the subnet carried the
	// advert. A machine with no broadcast still hosts a session guests can dial.
	CHECK(team.Hosting());
	CHECK(team.Port() != 0);

	team.Leave(1.0);
	CHECK_FALSE(team.Watching());
	CHECK_FALSE(team.Hosting());
	CHECK(team.Peers().empty());

	// The invitation goes with it. A key left in a panel after the session it
	// opened is gone is a key somebody sends to a session that is not there.
	CHECK(team.Invitation().empty());
	CHECK_FALSE(team.Session().Session.IsValid());

	// Hosting again is a new session, not a revived one - `ConnectionId`'s rule
	// and `SessionId`'s.
	std::string second;
	REQUIRE(team.Host(offering, second));
	CHECK(team.Hosting());
	CHECK(team.Session().Session.IsValid());
}

TEST_CASE("the collaborator count only moves what it changes", "[studio][teamcreate]") {
	Editor editor;
	TeamCreate &team = editor.Team;

	TeamCreateSettings offering;
	offering.Name = "counting";
	offering.Port = 7777;
	offering.PeerLimit = 4;

	std::string trouble;
	REQUIRE(team.Host(offering, trouble));
	CHECK(team.Session().PeerLimit == 4);

	if (!team.Announced()) {
		// No subnet on this machine. The count has nothing to be announced on
		// and the rest of the suite has already covered what it would say.
		return;
	}

	CHECK(team.Session().Peers == 1);
	team.SetCollaborators(3);
	CHECK(team.Session().Peers == 3);
	CHECK_FALSE(team.Session().IsFull());

	team.SetCollaborators(4);
	CHECK(team.Session().IsFull());
}

TEST_CASE("hosting opens a session other editors can join", "[studio][teamcreate]") {
	Editor host;

	TeamCreateSettings offering;
	offering.Name = "the shared project";
	// Ephemeral, which is what a person hosting from a laptop wants: the
	// announcement carries the port that was bound, so nothing has to be agreed
	// in advance.
	offering.Port = 0;

	std::string trouble;
	REQUIRE(host.Team.Host(offering, trouble));

	// **The advert names the port that was bound**, not the zero that was
	// asked for. An advert carrying zero sends every guest nowhere.
	REQUIRE(host.Team.Edits() != nullptr);
	CHECK(host.Team.Edits()->Hosting());
	CHECK(host.Team.Session().At.Port != 0);

	// A host needs nobody's permission to edit its own document.
	CHECK(host.Team.Edits()->Connected());
	CHECK(host.Team.Edits()->Editors() == 1);
}

TEST_CASE("joining a session with no address is refused with a reason", "[studio][teamcreate]") {
	Editor guest;

	std::string trouble;
	CHECK_FALSE(guest.Team.Join({}, 0.0, trouble));
	CHECK_FALSE(trouble.empty());
	CHECK(guest.Team.Edits() == nullptr);
}

TEST_CASE("the editor limit is enforced by the hosted stream", "[studio][teamcreate]") {
	Editor host;
	Editor guest;

	TeamCreateSettings offering;
	offering.Name = "host only";
	offering.PeerLimit = 1;

	std::string trouble;
	REQUIRE(host.Team.Host(offering, trouble));
	REQUIRE(guest.Team.Join(engine::net::Endpoint::LoopbackIPv4(host.Team.Session().At.Port), 0.0, trouble));

	double now = 0.0;
	for (int step = 0; step < 256; ++step) {
		now += 1.0 / 60.0;
		guest.Team.Pump(now);
		host.Team.Pump(now);
	}

	REQUIRE(host.Team.Edits() != nullptr);
	REQUIRE(guest.Team.Edits() != nullptr);
	CHECK(host.Team.Edits()->Editors() == 1);
	CHECK_FALSE(guest.Team.Edits()->Connected());
	CHECK(host.Team.Session().Peers == 1);
	CHECK(host.Team.Session().IsFull());
}

TEST_CASE("private edit streams require the invitation key after discovery", "[studio][teamcreate]") {
	Editor host;
	Editor invited;
	Editor wrongKey;
	Editor noKey;

	TeamCreateSettings offering;
	offering.Name = "invited editors";
	offering.Secret = "the shared invitation";
	offering.PeerLimit = 2;

	std::string trouble;
	REQUIRE(host.Team.Host(offering, trouble));
	const engine::net::Endpoint address = engine::net::Endpoint::LoopbackIPv4(host.Team.Session().At.Port);
	REQUIRE(invited.Team.Join(address, 0.0, trouble, offering.Secret));
	REQUIRE(wrongKey.Team.Join(address, 0.0, trouble, "somebody else's invitation"));
	REQUIRE(noKey.Team.Join(address, 0.0, trouble));

	double now = 0.0;
	for (int step = 0; step < 512; ++step) {
		now += 1.0 / 60.0;
		invited.Team.Pump(now);
		wrongKey.Team.Pump(now);
		noKey.Team.Pump(now);
		host.Team.Pump(now);
	}

	REQUIRE(host.Team.Edits() != nullptr);
	REQUIRE(invited.Team.Edits() != nullptr);
	REQUIRE(wrongKey.Team.Edits() != nullptr);
	REQUIRE(noKey.Team.Edits() != nullptr);
	CHECK(invited.Team.Edits()->Connected());
	CHECK_FALSE(wrongKey.Team.Edits()->Connected());
	CHECK_FALSE(noKey.Team.Edits()->Connected());
	CHECK(host.Team.Edits()->Editors() == 2);
	CHECK(host.Team.Session().Peers == 2);
}

TEST_CASE("a hosted session carries an edit to a guest", "[studio][teamcreate]") {
	// **The two halves together**, which is the whole point of the panel:
	// discovery hands over an address and the edit stream makes joining mean
	// something. A join that only connected would be a browser.
	Editor host;
	Editor guest;

	TeamCreateSettings offering;
	offering.Name = "shared";
	offering.Port = 0;

	std::string trouble;
	REQUIRE(host.Team.Host(offering, trouble));
	REQUIRE(host.Team.Edits() != nullptr);

	// What a guest reads off a listing: the host's address, resolved.
	const uint16_t port = host.Team.Session().At.Port;
	REQUIRE(port != 0);
	REQUIRE(guest.Team.Join(engine::net::Endpoint::LoopbackIPv4(port), 0.0, trouble));
	REQUIRE(guest.Team.Edits() != nullptr);

	double now = 0.0;
	const auto pump = [&](int ticks) {
		for (int step = 0; step < ticks; ++step) {
			now += 1.0 / 60.0;
			guest.Team.Pump(now);
			host.Team.Pump(now);
			guest.Team.Pump(now);
		}
	};

	// **Both ends, because they do not finish on the same tick.** The guest is
	// admitted one flight before the host can carry anything back, and an edit
	// published in that window is one the host's session refuses.
	const auto ready = [&] { return guest.Team.Edits()->Connected() && host.Team.Edits()->Editors() == 2; };
	for (int step = 0; step < 256 && !ready(); ++step) {
		pump(1);
	}
	REQUIRE(guest.Team.Edits()->Connected());
	CHECK(host.Team.Edits()->Editors() == 2);
	CHECK(host.Team.Session().Peers == 2);

	// The host makes an edit. Nothing wires the two together but the watcher
	// the editor installs, so this suite does what `InstallHistoryWatcher`
	// does.
	CommandLog::Watcher watcher;
	watcher.Committed = [&](uint64_t waypoint, std::span<const studio::Command> group) {
		host.Team.PublishEdits(waypoint, group, now);
	};
	host.Log.Watch(watcher);

	engine::ecs::Entity created;
	host.Worlds.Enter(host.Scene, [&](engine::ecs::Store &store) {
		created = store.CreateInstance(engine::scene::PartClass(), "Part");
		store.SetParent(created, engine::scene::WorkspaceOf(store));
		host.Log.RecordCreate(store, host.Scene, created, "Insert Part");
	});

	pump(16);

	size_t children = 0;
	guest.Worlds.Enter(guest.Scene, [&](engine::ecs::Store &store) {
		store.EachChild(engine::scene::WorkspaceOf(store), [&children](engine::ecs::Entity) { children++; });
	});
	// Workspace keeps its Terrain and world camera beside the replicated part.
	CHECK(children == 3);
	CHECK(host.Team.Edits()->Counters().Sent == 1);
	CHECK(guest.Team.Edits()->Counters().Applied == 1);

	// And it is not in the guest's history.
	CHECK(guest.Log.Depth() == 0);
}

TEST_CASE("leaving takes the session and its socket down", "[studio][teamcreate]") {
	Editor host;

	TeamCreateSettings offering;
	offering.Name = "briefly";
	offering.Port = 0;

	std::string trouble;
	REQUIRE(host.Team.Host(offering, trouble));
	REQUIRE(host.Team.Edits() != nullptr);

	host.Team.Leave(1.0);

	// The stream goes with the announcement. A stream outliving its transport
	// is a dangling reference in a destructor, which is the least debuggable
	// place for one.
	CHECK(host.Team.Edits() == nullptr);
	CHECK_FALSE(host.Team.Watching());

	// And publishing into nothing is a null check rather than a crash.
	host.Team.PublishEdits(0, {}, 2.0);
}

TEST_CASE("thirty-two editors converge on each other's concurrent edits", "[studio][teamcreate]") {
	// The session size Team Create is sized for: a host and 31 guests, every
	// one inserting a part on the same tick, and every editor ending with all
	// 32 parts in its workspace, in the host's order.
	constexpr size_t EDITORS = 32;
	std::vector<std::unique_ptr<Editor>> editors;
	for (size_t index = 0; index < EDITORS; ++index)
		editors.push_back(std::make_unique<Editor>());
	Editor &host = *editors.front();

	TeamCreateSettings offering;
	offering.Name = "crowd";
	offering.PeerLimit = EDITORS;
	std::string trouble;
	REQUIRE(host.Team.Host(offering, trouble));
	const uint16_t port = host.Team.Session().At.Port;
	for (size_t index = 1; index < EDITORS; ++index)
		REQUIRE(editors[index]->Team.Join(engine::net::Endpoint::LoopbackIPv4(port), 0.0, trouble));

	double now = 0.0;
	const auto pump = [&] {
		now += 1.0 / 60.0;
		for (auto &editor : editors)
			editor->Team.Pump(now);
	};
	const auto admitted = [&] {
		if (host.Team.Edits()->Editors() != EDITORS) return false;
		for (size_t index = 1; index < EDITORS; ++index)
			if (!editors[index]->Team.Edits()->Connected()) return false;
		return true;
	};
	int step = 0;
	for (; step < 1024 && !admitted(); ++step)
		pump();
	REQUIRE(admitted());
	CAPTURE(step);
	CHECK(host.Team.Session().Peers == EDITORS);

	std::vector<CommandLog::Watcher> watchers(EDITORS);
	for (size_t index = 0; index < EDITORS; ++index) {
		Editor &editor = *editors[index];
		watchers[index].Committed = [&editor,
									 &now](uint64_t waypoint, std::span<const studio::Command> group) {
			editor.Team.PublishEdits(waypoint, group, now);
		};
		editor.Log.Watch(watchers[index]);
	}
	for (size_t index = 0; index < EDITORS; ++index) {
		Editor &editor = *editors[index];
		editor.Worlds.Enter(editor.Scene, [&](engine::ecs::Store &store) {
			const auto part =
				store.CreateInstance(engine::scene::PartClass(), "Part" + std::to_string(index));
			store.SetParent(part, engine::scene::WorkspaceOf(store));
			editor.Log.RecordCreate(store, editor.Scene, part, "Insert Part");
		});
	}

	const auto parts = [](Editor &editor) {
		size_t count = 0;
		editor.Worlds.Enter(editor.Scene, [&](engine::ecs::Store &store) {
			store.EachChild(engine::scene::WorkspaceOf(store), [&](engine::ecs::Entity child) {
				if (store.InstanceNameOf(child).Text().starts_with("Part")) count++;
			});
		});
		return count;
	};
	const auto converged = [&] {
		for (auto &editor : editors)
			if (parts(*editor) != EDITORS) return false;
		return true;
	};
	int settle = 0;
	for (; settle < 1024 && !converged(); ++settle)
		pump();
	CAPTURE(settle);
	for (size_t index = 0; index < EDITORS; ++index) {
		CAPTURE(index);
		CHECK(parts(*editors[index]) == EDITORS);
		CHECK(editors[index]->Log.Depth() == 1);
	}
	CHECK(host.Team.Edits()->Counters().Relayed == EDITORS - 1);
	// The same order everywhere, since a path picks the first of two
	// same-named siblings and so means whichever comes first.
	const auto order = [](Editor &editor) {
		std::vector<std::string> names;
		editor.Worlds.Enter(editor.Scene, [&](engine::ecs::Store &store) {
			store.EachChild(engine::scene::WorkspaceOf(store), [&](engine::ecs::Entity child) {
				names.emplace_back(store.InstanceNameOf(child).Text());
			});
		});
		return names;
	};
	const auto hosted = order(host);
	for (size_t index = 1; index < EDITORS; ++index) {
		CAPTURE(index);
		CHECK(order(*editors[index]) == hosted);
	}

	// Then all 32 resize the same part on the same tick. Each lands on top of
	// the one ordered before it, and every editor ends on the host's answer.
	const auto sizeOf = [](Editor &editor, engine::game::PropertyValue *write) {
		engine::core::Vector3 size;
		editor.Worlds.Enter(editor.Scene, [&](engine::ecs::Store &store) {
			const auto part = studio::ResolvePath(store, {"Workspace", "Part0"});
			REQUIRE(part != engine::ecs::NULL_ENTITY);
			const engine::ecs::PropertyDescriptor *descriptor = nullptr;
			for (const auto &candidate : engine::ecs::Classes::Describe(store.ClassOf(part)).Properties)
				if (candidate.Name == engine::core::Name("Size")) descriptor = &candidate;
			REQUIRE(descriptor != nullptr);
			engine::game::PropertyValue before;
			REQUIRE(engine::game::ReadProperty(store, part, *descriptor, before));
			size = before.Vector3;
			if (write == nullptr) return;
			REQUIRE(engine::game::WriteProperty(store, part, *descriptor, *write));
			editor.Log.RecordProperty(
				editor.Scene, part, engine::core::Name("Size"), before, *write, "Resize"
			);
		});
		return size;
	};
	for (size_t index = 0; index < EDITORS; ++index) {
		engine::game::PropertyValue after;
		after.Type = engine::ecs::PropertyType::Vector3;
		const float edge = static_cast<float>(index + 1);
		after.Vector3 = {edge, edge, edge};
		(void)sizeOf(*editors[index], &after);
	}
	const auto agreed = [&] {
		const auto expected = sizeOf(host, nullptr);
		for (auto &editor : editors)
			if (!(sizeOf(*editor, nullptr) == expected)) return false;
		return true;
	};
	int resized = 0;
	for (; resized < 1024 && !agreed(); ++resized)
		pump();
	CAPTURE(resized);
	CHECK(agreed());
	CHECK(host.Team.Edits()->Counters().Relayed == 2 * (EDITORS - 1));
}
