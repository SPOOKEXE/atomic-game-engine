// A message addressed to one channel on one world, delivered to a script in
// either language.
//
// **The half of `CrossWorldService` a parity case in `engine.script.scriptcall`
// cannot reach.** `OpenChannel` answers on the spot and can be checked against a
// bare store; a delivery and a `SendAsync` reply need a second world, a router
// and a barrier - so they are here, over a real `Universe`, and asserted in both
// languages because that is the whole claim the v0.16 row makes.
//
// **What the suite is written to catch is drift between the two pumps.** The
// service was Luau's alone until v0.16, and closing it took describing the
// service once and teaching `PumpJsDeliveries` about `world::BusKind::Channel`.
// Only the second is testable from a script, and without it the signal would have
// been reachable, connectable and permanently silent - the failure
// `script/AGENTS.md` names twice. v0.17 put a *name* on the channel and the same
// exposure came back one level down: a pump that ignored the filter would deliver
// every channel to every listener, which looks like the feature working until a
// world opens its second channel.

#include <engine/core/Name.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Codec.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/scripthost/Runtime.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Postbox.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.scripthost.crossworldservice")

using engine::core::Name;
using engine::ecs::Store;
using engine::script::Language;
using engine::script::MakeRuntime;
using engine::script::Runtime;
using engine::world::Postbox;
using engine::world::Universe;
using engine::world::WorldId;
using engine::world::WorldSettings;

namespace {
	WorldSettings Named(const char *name) {
		WorldSettings settings;
		settings.Name = Name(name);
		settings.TickRate = 60.0;
		return settings;
	}

	const char *Spelling(Language language) {
		return language == Language::Luau ? "luau" : "javascript";
	}

	// The chunk that opens two channels and connects to each.
	//
	// **The handlers write into `workspace.Name` and a part's name**, which is
	// `engine.script.scriptcall`'s wire and works here for its reason: a property
	// is already neutral, so the one channel this suite is not testing is the one
	// it can read an answer back through.
	//
	// **Two channels rather than one, because one proves nothing about the
	// filter.** A pump that ignored the channel entirely would fire both handlers
	// for a single arrival, and only the second name catches it.
	const char *Listener(Language language) {
		if (language == Language::Luau) {
			return "CrossWorldService:OpenChannel('scores'):Connect(function(message, from)\n"
				   "	workspace.Name = message.score .. '/' .. from\n"
				   "end)\n"
				   "local chat = Instance.new('Part')\n"
				   "chat.Name = 'chat:none'\n"
				   "chat.Parent = workspace\n"
				   "CrossWorldService:OpenChannel('chat'):Connect(function(message, from)\n"
				   "	chat.Name = 'chat:' .. tostring(message.score)\n"
				   "end)\n";
		}
		return "CrossWorldService.OpenChannel('scores').Connect(function (message, from) {\n"
			   "	workspace.Name = message.score + '/' + from\n"
			   "})\n"
			   "const chat = Instance.new('Part')\n"
			   "chat.Name = 'chat:none'\n"
			   "chat.Parent = workspace\n"
			   "CrossWorldService.OpenChannel('chat').Connect(function (message, from) {\n"
			   "	chat.Name = 'chat:' + message.score\n"
			   "})\n";
	}

	// The chunk that sends and records the status the bus answered with.
	//
	// **`SendAsync` suspends**, which is what it gained in v0.17 and the reason
	// the reply is readable at all: the previous `Send` answered a boolean about
	// its own budget and threw the bus's verdict away, so a script could not tell
	// a message that arrived from one addressed to a channel nobody had opened.
	const char *Sender(Language language) {
		if (language == Language::Luau) {
			return "task.spawn(function()\n"
				   "	local _, ok = CrossWorldService:SendAsync('script.channel.receiver', 'scores', "
				   "{ score = 12 })\n"
				   "	local _, missing = CrossWorldService:SendAsync('script.channel.receiver', 'nope', "
				   "{ score = 1 })\n"
				   "	workspace.Name = ok .. '/' .. missing\n"
				   "end)\n";
		}
		return "(async () => {\n"
			   "	const ok = await CrossWorldService.SendAsync('script.channel.receiver', 'scores', "
			   "{ score: 12 })\n"
			   "	const missing = await CrossWorldService.SendAsync('script.channel.receiver', 'nope', "
			   "{ score: 1 })\n"
			   "	workspace.Name = ok.Status + '/' + missing.Status\n"
			   "})()\n";
	}

}

TEST_CASE("a channel message reaches only its own listeners, in either language", "[scripting][crossworld]") {
	for (const Language language : {Language::Luau, Language::JavaScript}) {
		engine::scene::EnsureClassTree();
		engine::scene::RegisterSceneComponents();

		Universe universe;
		const WorldId sender = universe.Create(Named("script.channel.sender"));
		const WorldId receiver = universe.Create(Named("script.channel.receiver"));

		// **The runtime outlives the `Enter`, and that is safe**: the universe
		// owns the store, and `Enter` hands out a reference to it rather than a
		// copy. The runtime is built inside so that the services install against
		// the world they will run on.
		std::shared_ptr<Runtime> runtime;
		engine::ecs::Entity workspace = engine::ecs::NULL_ENTITY;
		universe.Enter(receiver, [&](Store &store) {
			runtime = MakeRuntime(store, language);
			REQUIRE(runtime != nullptr);

			workspace = engine::scene::WorkspaceOf(store);
			REQUIRE(workspace != engine::ecs::NULL_ENTITY);

			INFO(runtime->LastError());
			REQUIRE(runtime->Run(Listener(language)));
		});

		// The open reaches the bus at the next barrier, which is why the send is
		// a tick later - a message addressed to a channel that did not exist when
		// it was sent is refused, and that is the honest answer rather than a
		// race.
		universe.Tick(1.0f / 60.0f);

		// **Encoded through the shared codec**, because that is what a script's
		// own `SendAsync` would have done - a raw byte string would decode to
		// nothing and the handler would read `nil.score`.
		universe.Enter(sender, [](Store &store) {
			engine::script::ScriptValue score{engine::script::ValueTag::Number};
			score.Number = 12.0;

			engine::script::ScriptValue message{engine::script::ValueTag::Map};
			message.Entries.emplace_back("score", score);

			std::vector<std::byte> payload;
			REQUIRE(Encode(message, payload) == engine::script::CodecStatus::Ok);
			REQUIRE(Postbox(store).SendTo("script.channel.receiver", "scores", payload).Expected());
		});

		universe.Tick(1.0f / 60.0f);

		// The delivery lands in the inbox at the barrier and the runtime's own
		// beat is what hands it to the connection - which is the first of the
		// four steps `LuauRuntime::Heartbeat` runs, in both languages.
		universe.Enter(receiver, [&](Store &store) {
			INFO(runtime->LastError());
			REQUIRE(runtime->Heartbeat(1.0f / 60.0f));

			INFO(Spelling(language));
			CHECK(std::string(store.InstanceNameOf(workspace).Text()) == "12/script.channel.sender");

			// **The other channel heard nothing**, which is the whole of what
			// naming a channel buys: two subsystems in one world do not read each
			// other's traffic. A pump that ignored the filter would have renamed
			// this marker to `chat:12`.
			CHECK(store.FindFirstChild(workspace, "chat:none") != engine::ecs::NULL_ENTITY);
			CHECK(store.FindFirstChild(workspace, "chat:12") == engine::ecs::NULL_ENTITY);
		});
	}
}

TEST_CASE("a script reads the status a channel send answered with", "[scripting][crossworld]") {
	for (const Language language : {Language::Luau, Language::JavaScript}) {
		engine::scene::EnsureClassTree();
		engine::scene::RegisterSceneComponents();

		Universe universe;
		const WorldId sender = universe.Create(Named("script.channel.asker"));
		const WorldId receiver = universe.Create(Named("script.channel.receiver"));

		universe.Enter(receiver, [](Store &store) {
			REQUIRE(Postbox(store).OpenChannel("scores").Expected());
		});
		universe.Tick(1.0f / 60.0f);

		std::shared_ptr<Runtime> runtime;
		engine::ecs::Entity workspace = engine::ecs::NULL_ENTITY;
		universe.Enter(sender, [&](Store &store) {
			runtime = MakeRuntime(store, language);
			REQUIRE(runtime != nullptr);

			workspace = engine::scene::WorkspaceOf(store);
			REQUIRE(workspace != engine::ecs::NULL_ENTITY);

			INFO(runtime->LastError());
			REQUIRE(runtime->Run(Sender(language)));
		});

		// Each barrier carries one reply and each resume issues the next send, so
		// the pair of calls takes several beats to finish - the shape every
		// suspending store call in `engine.script.scripting` has.
		for (int beat = 0; beat < 6; beat++) {
			universe.Tick(1.0f / 60.0f);
			universe.Enter(sender, [&](Store &) { runtime->Heartbeat(1.0f / 60.0f); });
		}

		universe.Enter(sender, [&](Store &store) {
			INFO(Spelling(language));
			INFO(runtime->LastError());

			// **`NoSuchChannel` and not silence**, which is the distinction this
			// service exists for beside `MessagingService`: a publish nobody
			// wanted and a publish nobody heard look identical, and these do not.
			CHECK(std::string(store.InstanceNameOf(workspace).Text()) == "Ok/NoSuchChannel");
		});
	}
}
