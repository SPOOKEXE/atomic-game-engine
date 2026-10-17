#include <engine/ecs/Store.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/RemoteEvent.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/scripthost/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <vector>

TEST_SUITE_ID("engine.scripthost.remoteevent")
TEST_DEPENDS("engine.script.instanceshim")
TEST_DEPENDS("engine.script.remoteevent")

TEST_CASE("a RemoteEvent copies its payload across either language adapter", "[script][remote-event]") {
	const auto authorityLanguage =
		GENERATE(engine::script::Language::Luau, engine::script::Language::JavaScript);
	const auto clientLanguage =
		GENERATE(engine::script::Language::Luau, engine::script::Language::JavaScript);
	engine::script::ScriptClass();
	engine::ecs::Store authorityStore("remote_event_authority");
	engine::ecs::Store clientStore("remote_event_client");

	std::vector<std::byte> sent;
	engine::script::RuntimeLimits clientLimits;
	clientLimits.Role = engine::script::HostRole::OfClient();
	clientLimits.RemoteEventSender = [&](std::span<const std::byte> message) {
		sent.assign(message.begin(), message.end());
		return true;
	};
	engine::script::RuntimeLimits authorityLimits;
	authorityLimits.Role = engine::script::HostRole::OfServer();

	auto authority = engine::script::MakeRuntime(authorityStore, authorityLanguage, authorityLimits);
	auto client = engine::script::MakeRuntime(clientStore, clientLanguage, clientLimits);

	REQUIRE(authority->Run(
		authorityLanguage == engine::script::Language::Luau ? R"(
		local remote = Instance.new("RemoteEvent")
		remote.Name = "Damage"
		remote.OnServerEvent:Connect(function(payload)
			assert(payload == "copied payload")
			local proof = Instance.new("Part")
			proof.Name = payload
		end)
	)"
															: R"(
		const remote = Instance.new("RemoteEvent");
		remote.Name = "Damage";
		remote.OnServerEvent.Connect(function(payload, sender) {
			if (payload !== "copied payload" || sender !== null) throw new Error("wrong envelope");
			const proof = Instance.new("Part");
			proof.Name = payload;
		});
	)"
	));
	REQUIRE(client->Run(
		clientLanguage == engine::script::Language::Luau ? R"(
		local remote = Instance.new("RemoteEvent")
		remote.Name = "Damage"
		remote:FireServer("copied payload")
	)"
														 : R"(
		const remote = Instance.new("RemoteEvent");
		remote.Name = "Damage";
		remote.FireServer("copied payload");
	)"
	));
	REQUIRE_FALSE(sent.empty());
	REQUIRE(authority->DeliverRemoteEvent(sent));
	CHECK(authorityStore.FindFirstRoot("copied payload") != engine::ecs::NULL_ENTITY);
}

TEST_CASE("a combined host delivers a RemoteEvent without a network sender", "[script][remote-event]") {
	const auto language = GENERATE(engine::script::Language::Luau, engine::script::Language::JavaScript);
	engine::script::ScriptClass();
	engine::ecs::Store store("remote_event_local");
	engine::scene::InstallServices(store);
	const auto viewer = engine::scene::AddPlayer(store, "viewer");
	store.SetResource(engine::scene::LocalPlayer{viewer});

	engine::script::RuntimeLimits limits;
	limits.Role = engine::script::HostRole::OfBoth();
	auto runtime = engine::script::MakeRuntime(store, language, limits);
	REQUIRE(runtime != nullptr);

	REQUIRE(runtime->Run(
		language == engine::script::Language::Luau ? R"(
		local remote = Instance.new("RemoteEvent")
		remote.Name = "TornadoControl"
		remote.OnServerEvent:Connect(function(payload, sender)
			assert(payload == "EF5" and sender.Name == "viewer")
			local proof = Instance.new("Part")
			proof.Name = "local:" .. payload
		end)
		remote:FireServer("EF5")
	)"
												   : R"(
		const remote = Instance.new("RemoteEvent");
		remote.Name = "TornadoControl";
		remote.OnServerEvent.Connect(function(payload, sender) {
			if (payload !== "EF5" || sender.Name !== "viewer") throw new Error("wrong local sender");
			const proof = Instance.new("Part");
			proof.Name = "local:" + payload;
		});
		remote.FireServer("EF5");
	)"
	));
	CHECK(store.FindFirstRoot("local:EF5") != engine::ecs::NULL_ENTITY);
}

TEST_CASE(
	"RemoteEvent sender identity comes from the host rather than its payload", "[script][remote-event]"
) {
	const auto language = GENERATE(engine::script::Language::Luau, engine::script::Language::JavaScript);
	engine::script::ScriptClass();
	engine::ecs::Store store("remote_event_senders");
	(void)engine::scene::InstallServices(store);
	const auto first = engine::scene::AddPlayer(store, "first");
	const auto second = engine::scene::AddPlayer(store, "second");
	REQUIRE(first != engine::ecs::NULL_ENTITY);
	REQUIRE(second != engine::ecs::NULL_ENTITY);
	engine::script::RuntimeLimits limits;
	limits.Role = engine::script::HostRole::OfServer();
	auto runtime = engine::script::MakeRuntime(store, language, limits);
	REQUIRE(runtime->Run(
		language == engine::script::Language::Luau ? R"(
		local remote = Instance.new("RemoteEvent")
		remote.Name = "ViewPose"
		remote.OnServerEvent:Connect(function(payload, sender)
			assert(sender:IsA("Player"))
			local proof = Instance.new("Part")
			proof.Name = sender.Name .. ":" .. payload
		end)
	)"
												   : R"(
		const remote = Instance.new("RemoteEvent");
		remote.Name = "ViewPose";
		remote.OnServerEvent.Connect(function(payload, sender) {
			if (!sender.IsA("Player")) throw new Error("sender is not admitted player");
			const proof = Instance.new("Part");
			proof.Name = sender.Name + ":" + payload;
		});
	)"
	));
	std::vector<std::byte> message;
	const std::string spoofed = "second";
	REQUIRE(
		engine::script::EncodeRemoteEvent(
			"ViewPose", std::as_bytes(std::span(spoofed.data(), spoofed.size())), message
		)
	);
	REQUIRE(runtime->DeliverRemoteEvent(message, first));
	REQUIRE(runtime->DeliverRemoteEvent(message, second));
	CHECK(store.FindFirstRoot("first:second") != engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstRoot("second:second") != engine::ecs::NULL_ENTITY);
	const auto invalid = store.FindFirstRoot("first:second");
	CHECK_FALSE(runtime->DeliverRemoteEvent(message, invalid));
	store.Destroy(second);
	CHECK_FALSE(runtime->DeliverRemoteEvent(message, second));
}

TEST_CASE(
	"RemoteEvent handlers retain their Script side and die with their source",
	"[script][remote-event][lifetime]"
) {
	using namespace engine;
	const auto language = GENERATE(script::Language::Luau, script::Language::JavaScript);
	(void)script::ScriptClass();
	ecs::Store store("remote_event_source");
	scene::InstallServices(store);
	const auto viewer = scene::AddPlayer(store, "viewer");
	store.SetResource(scene::LocalPlayer{viewer});
	store.SetResource(script::SourceCache{});
	const auto serverPath =
		core::Name(language == script::Language::Luau ? "remote-server.luau" : "remote-server.js");
	const auto clientPath =
		core::Name(language == script::Language::Luau ? "remote-client.luau" : "remote-client.js");
	store.ResourceMutable<script::SourceCache>()->Set(
		serverPath,
		language == script::Language::Luau ? R"(
		local remote = Instance.new('RemoteEvent', workspace)
		remote.Name = 'OwnedRemote'
		remote.OnServerEvent:Connect(function(payload, sender)
			assert(payload == 'request' and sender.Name == 'viewer')
			local run = game:GetService('RunService')
			assert(run:IsServer() and not run:IsClient())
			local proof = Instance.new('Part', workspace)
			proof.Name = 'AuthorityReply'
		end)
	)"
										   : R"(
		const remote = Instance.new('RemoteEvent', workspace);
		remote.Name = 'OwnedRemote';
		remote.OnServerEvent.Connect(function(payload, sender) {
			if (payload !== 'request' || sender.Name !== 'viewer') throw new Error('wrong request');
			const run = game.GetService('RunService');
			if (!run.IsServer() || run.IsClient()) throw new Error('handler lost Script side');
			const proof = Instance.new('Part', workspace);
			proof.Name = 'AuthorityReply';
		});
	)"
	);
	store.ResourceMutable<script::SourceCache>()->Set(
		clientPath,
		language == script::Language::Luau ? R"(
		workspace:FindFirstChild('OwnedRemote'):FireServer('request')
	)"
										   : R"(
		workspace.FindFirstChild('OwnedRemote').FireServer('request');
	)"
	);
	const auto server = script::MakeScript(store, serverPath.Text(), "RemoteOwner");
	const auto client = script::MakeScript(store, clientPath.Text(), "RemoteSender", true);
	REQUIRE(server != ecs::NULL_ENTITY);
	REQUIRE(client != ecs::NULL_ENTITY);
	auto runtime = script::MakeRuntime(store, language, {.Role = script::HostRole::OfBoth()});
	REQUIRE(runtime->RunInstance(server));
	const bool fired = runtime->RunInstance(client);
	INFO(runtime->LastError());
	REQUIRE(fired);
	const auto workspace = scene::WorkspaceOf(store);
	const auto proof = store.FindFirstChild(workspace, "AuthorityReply");
	REQUIRE(proof != ecs::NULL_ENTITY);
	CHECK_FALSE(ecs::Store::IsPredicted(proof));
	const auto remote = store.FindFirstChild(workspace, "OwnedRemote");
	const std::string payload = "request";
	std::vector<std::byte> message;
	REQUIRE(
		script::EncodeRemoteEvent(
			store.GetFullName(remote), std::as_bytes(std::span(payload.data(), payload.size())), message
		)
	);
	store.Destroy(server);
	store.Destroy(proof);
	REQUIRE(runtime->DeliverRemoteEvent(message, viewer));
	CHECK(store.FindFirstChild(workspace, "AuthorityReply") == ecs::NULL_ENTITY);
}

TEST_CASE(
	"a failed RemoteEvent callback does not spend the next delivery budget", "[script][remote-event][budget]"
) {
	using namespace engine;
	const auto language = GENERATE(script::Language::Luau, script::Language::JavaScript);
	(void)script::ScriptClass();
	ecs::Store store("remote_event_budget");
	script::RuntimeLimits limits;
	limits.Role = script::HostRole::OfServer();
	limits.StepBudget = 1000;
	auto runtime = script::MakeRuntime(store, language, limits);
	REQUIRE(runtime->Run(
		language == script::Language::Luau ? R"(
		local remote = Instance.new("RemoteEvent")
		remote.Name = "Bounded"
		remote.OnServerEvent:Connect(function(payload)
			if payload == "exhaust" then while true do end end
			local proof = Instance.new("Part")
			proof.Name = "recovered"
		end)
	)"
										   : R"(
		const remote = Instance.new("RemoteEvent");
		remote.Name = "Bounded";
		remote.OnServerEvent.Connect(function(payload) {
			if (payload === "exhaust") { while (true) {} }
			const proof = Instance.new("Part");
			proof.Name = "recovered";
		});
	)"
	));
	std::vector<std::byte> message;
	const auto request = [&](std::string_view payload) {
		REQUIRE(
			script::EncodeRemoteEvent(
				"Bounded", std::as_bytes(std::span(payload.data(), payload.size())), message
			)
		);
	};
	request("exhaust");
	REQUIRE_FALSE(runtime->DeliverRemoteEvent(message));
	CHECK_FALSE(runtime->LastError().empty());
	request("recover");
	const bool recovered = runtime->DeliverRemoteEvent(message);
	INFO(runtime->LastError());
	CHECK(recovered);
	CHECK(store.FindFirstRoot("recovered") != ecs::NULL_ENTITY);
}
