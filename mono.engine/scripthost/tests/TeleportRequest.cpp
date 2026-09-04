// The ProcessReceipt-shaped teleport authority callback in both VMs.

#include <engine/core/Bytes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/script/TeleportRequest.hpp>
#include <engine/scripthost/Runtime.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Postbox.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

TEST_SUITE_ID("engine.scripthost.teleportrequest")
TEST_DEPENDS("engine.scene.part")

namespace {
	engine::ecs::Store Fresh(const char *name) {
		engine::scene::EnsureClassTree();
		engine::scene::RegisterSceneComponents();
		engine::script::RegisterScriptComponents();
		return engine::ecs::Store(name);
	}

	std::string
	Handler(engine::script::Language language, std::string_view decision, std::string_view message) {
		if (language == engine::script::Language::Luau) {
			return "TeleportService.TeleportRequested = function(request) "
				   "assert(request.Place == 'lobby') "
				   "assert(request.Data.answer == 7) "
				   "return { Decision = Enum.TeleportRequestDecision." +
				   std::string(decision) + ", Message = '" + std::string(message) + "' } end";
		}
		return "TeleportService.TeleportRequested = request => { "
			   "if (request.Place !== 'lobby') throw new Error('place'); "
			   "if (request.Data.answer !== 7) throw new Error('data'); "
			   "return { Decision: Enum.TeleportRequestDecision." +
			   std::string(decision) + ", Message: '" + std::string(message) + "' }; };";
	}
}

TEST_CASE("TeleportRequested returns its typed authority decision in both VMs", "[scripting][teleport]") {
	for (const engine::script::Language language :
		 {engine::script::Language::Luau, engine::script::Language::JavaScript}) {
		SECTION(language == engine::script::Language::Luau ? "luau" : "javascript") {
			engine::ecs::Store store = Fresh("teleport_request");
			const auto runtime = engine::script::MakeRuntime(store, language);
			REQUIRE(runtime != nullptr);

			const std::string source = Handler(language, "Processed", "accepted");
			INFO(source);
			REQUIRE(runtime->Run(source.c_str()));

			engine::script::ScriptValue data{engine::script::ValueTag::Map};
			engine::script::ScriptValue answer{engine::script::ValueTag::Number};
			answer.Number = 7;
			data.Entries.emplace_back("answer", answer);

			const engine::script::TeleportRequest request{engine::ecs::Entity{9}, "lobby", data};
			const engine::script::TeleportRequestResult result =
				engine::script::DispatchTeleportRequest(*runtime, store, request);
			CHECK(result.Decision == engine::script::TeleportRequestDecision::Processed);
			CHECK(result.Message == "accepted");
		}
	}
}

TEST_CASE("TeleportRequested fails closed on an invalid decision", "[scripting][teleport]") {
	engine::ecs::Store store = Fresh("teleport_request_invalid");
	const auto runtime = engine::script::MakeRuntime(store, engine::script::Language::Luau);
	REQUIRE(runtime != nullptr);
	REQUIRE(
		runtime->Run("TeleportService.TeleportRequested = function() return { Decision = 'Unknown' } end")
	);

	const engine::script::TeleportRequest request{engine::ecs::Entity{9}, "lobby", {}};
	const engine::script::TeleportRequestResult result =
		engine::script::DispatchTeleportRequest(*runtime, store, request);
	CHECK(result.Decision == engine::script::TeleportRequestDecision::NotProcessed);
	CHECK(result.Message.find("unknown decision") != std::string::npos);
}

TEST_CASE("TeleportResult reaches both client runtimes at their next barrier", "[scripting][teleport]") {
	for (const engine::script::Language language :
		 {engine::script::Language::Luau, engine::script::Language::JavaScript}) {
		SECTION(language == engine::script::Language::Luau ? "luau" : "javascript") {
			engine::ecs::Store store = Fresh("teleport_result");
			engine::script::RuntimeLimits limits;
			limits.Role = engine::script::HostRole::OfClient();
			const auto runtime = engine::script::MakeRuntime(store, language, limits);
			REQUIRE(runtime != nullptr);

			const std::string source =
				language == engine::script::Language::Luau
					? "local received = false\n"
					  "TeleportService.TeleportResult:Connect(function(id, decision, message)\n"
					  "  assert(id == 7)\n"
					  "  assert(decision == Enum.TeleportRequestDecision.Denied)\n"
					  "  assert(message == 'no room')\n"
					  "  received = true\n"
					  "end)\n"
					  "RunService.Heartbeat:Connect(function() assert(received) end)"
					: "let received = false;\n"
					  "TeleportService.TeleportResult.Connect((id, decision, message) => {\n"
					  "  if (id !== 7 || decision.Name !== 'Denied' || message !== 'no "
					  "room') {\n"
					  "    throw new Error('wrong result');\n"
					  "  }\n"
					  "  received = true;\n"
					  "});\n"
					  "RunService.Heartbeat.Connect(() => { if (!received) throw new Error('not delivered'); "
					  "});";
			INFO(source);
			REQUIRE(runtime->Run(source.c_str()));

			runtime->DeliverTeleportResult({7, engine::script::TeleportRequestDecision::Denied, "no room"});
			INFO(runtime->LastError());
			CHECK(runtime->Heartbeat(1.0f / 60.0f));
		}
	}
}

TEST_CASE("teleport callbacks and unsent requests do not survive a snapshot", "[scripting][teleport]") {
	engine::ecs::Store source = Fresh("teleport_transient_source");
	const auto runtime = engine::script::MakeRuntime(source, engine::script::Language::Luau);
	REQUIRE(runtime != nullptr);
	REQUIRE(runtime->Run(
		"TeleportService.TeleportRequested = function() return { Decision = "
		"Enum.TeleportRequestDecision.Denied } end"
	));

	engine::script::ScriptValue data{engine::script::ValueTag::String};
	data.Text = "queued";
	std::string failure;
	REQUIRE(engine::script::QueueTeleportRequest(source, "lobby", data, failure));
	REQUIRE(engine::script::PendingTeleportRequests(source).size() == 1);

	engine::core::ByteWriter writer;
	REQUIRE(source.Save(writer));

	engine::ecs::Store restored = Fresh("teleport_transient_restored");
	engine::core::ByteReader reader(writer.Bytes());
	REQUIRE(restored.Load(reader));
	const auto *handler = restored.Resource<engine::script::TeleportRequestHandler>();
	REQUIRE(handler != nullptr);
	CHECK_FALSE(handler->Callback.Valid());
	CHECK(engine::script::PendingTeleportRequests(restored).empty());
}

TEST_CASE("a replica Teleport call queues only its local player's request", "[scripting][teleport]") {
	for (const engine::script::Language language :
		 {engine::script::Language::Luau, engine::script::Language::JavaScript}) {
		SECTION(language == engine::script::Language::Luau ? "luau" : "javascript") {
			engine::ecs::Store store = Fresh("teleport_replica_request");
			engine::world::RegisterMailboxTypes();
			REQUIRE(engine::scene::InstallServices(store) != engine::ecs::NULL_ENTITY);
			const engine::ecs::Entity player = engine::scene::AddPlayer(store, "Ada", true, 7);
			REQUIRE(player != engine::ecs::NULL_ENTITY);
			store.SetResource(engine::world::Replica{});
			store.SetAdoptOnly(true);

			engine::script::RuntimeLimits limits;
			limits.Role = engine::script::HostRole::OfClient();
			const auto runtime = engine::script::MakeRuntime(store, language, limits);
			REQUIRE(runtime != nullptr);
			const std::string source = language == engine::script::Language::Luau
										   ? "TeleportService:Teleport('lobby', "
											 "game:GetService('Players').LocalPlayer, { answer = 7 })"
										   : "TeleportService.Teleport('lobby', "
											 "game.GetService('Players').LocalPlayer, { answer: 7 });";
			INFO(source);
			REQUIRE(runtime->Run(source.c_str()));

			const std::span<const engine::script::PendingTeleportRequest> pending =
				engine::script::PendingTeleportRequests(store);
			REQUIRE(pending.size() == 1);
			CHECK(pending.front().Id == 1);
			CHECK(pending.front().Place == "lobby");
			engine::script::ScriptValue data;
			REQUIRE(engine::script::Decode(pending.front().Data, data) == engine::script::CodecStatus::Ok);
			REQUIRE(data.Tag == engine::script::ValueTag::Map);
			REQUIRE(data.Entries.size() == 1);
			CHECK(data.Entries.front().first == "answer");
			CHECK(data.Entries.front().second.Number == 7.0);
			CHECK(store.Alive(player));

			engine::script::MarkTeleportRequestSent(store);
			CHECK(engine::script::PendingTeleportRequests(store).empty());
			CHECK_FALSE(engine::script::AcceptTeleportResult(store, 2));
			CHECK(engine::script::AcceptTeleportResult(store, 1));
			CHECK_FALSE(engine::script::AcceptTeleportResult(store, 1));
		}
	}
}
