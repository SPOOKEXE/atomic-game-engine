// A message addressed to one world, delivered to a script in either language.
//
// **The half of `CrossWorldService` a parity case in `engine.script.scriptcall`
// cannot reach.** `Send` answers on the spot and can be checked against a bare
// store; `MessageReceived` needs a second world, a router and a barrier — so it
// is here, over a real `Universe`, and it is asserted in both languages because
// that is the whole claim the v0.16 row makes.
//
// **The JavaScript half is what this suite was written for.** The service was
// Luau's alone until v0.16, and closing it took two things: describing the
// service once, which `ServiceSurface` made possible, and teaching
// `PumpJsDeliveries` about `world::BusKind::Channel`, which it had never
// handled. Only the second is testable from a script, and without it the
// signal would have been reachable, connectable and permanently silent — the
// failure `script/AGENTS.md` names twice.

#include "../src/Codec.hpp"

#include <engine/core/Name.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Postbox.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.script.crossworldservice")

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

	// The chunk that connects, in each language's own spelling.
	//
	// **The handler writes the payload and the sender's name into
	// `workspace.Name`**, which is `engine.script.scriptcall`'s wire and works
	// here for its reason: a property is already neutral, so the one channel
	// this suite is not testing is the one it can read an answer back through.
	const char *Listener(Language language) {
		if (language == Language::Luau) {
			return "CrossWorldService.MessageReceived:Connect(function(message, from)\n"
				   "	workspace.Name = message.score .. '/' .. from\n"
				   "end)\n";
		}
		return "CrossWorldService.MessageReceived.Connect(function (message, from) {\n"
			   "	workspace.Name = message.score + '/' + from\n"
			   "})\n";
	}
}

TEST_CASE("a channel message reaches a script in either language", "[scripting][crossworld]") {
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
		engine::ecs::Entity workspace;
		universe.Enter(receiver, [&](Store &store) {
			runtime = MakeRuntime(store, language);
			REQUIRE(runtime != nullptr);

			workspace = engine::scene::WorkspaceOf(store);
			REQUIRE(workspace != engine::ecs::NULL_ENTITY);

			INFO(runtime->LastError());
			REQUIRE(runtime->Run(Listener(language)));
		});

		// **Encoded through the shared codec**, because that is what a script's
		// own `Send` would have done — a raw byte string would decode to nothing
		// and the handler would read `nil.score`.
		universe.Enter(sender, [](Store &store) {
			engine::script::ScriptValue score{engine::script::ValueTag::Number};
			score.Number = 12.0;

			engine::script::ScriptValue message{engine::script::ValueTag::Map};
			message.Entries.emplace_back("score", score);

			std::vector<std::byte> payload;
			REQUIRE(Encode(message, payload) == engine::script::CodecStatus::Ok);
			REQUIRE(Postbox(store).SendTo("script.channel.receiver", payload).Expected());
		});

		universe.Tick(1.0f / 60.0f);

		// The delivery lands in the inbox at the barrier and the runtime's own
		// beat is what hands it to the connection — which is the first of the
		// four steps `LuauRuntime::Heartbeat` runs, in both languages.
		universe.Enter(receiver, [&](Store &store) {
			INFO(runtime->LastError());
			REQUIRE(runtime->Heartbeat(1.0f / 60.0f));

			INFO((language == Language::Luau ? "luau" : "javascript"));
			CHECK(std::string(store.InstanceNameOf(workspace).Text()) == "12/script.channel.sender");
		});
	}
}
