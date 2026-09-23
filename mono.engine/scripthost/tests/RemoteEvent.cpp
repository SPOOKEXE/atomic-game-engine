#include <engine/ecs/Store.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/scripthost/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <vector>

TEST_SUITE_ID("engine.scripthost.remoteevent")
TEST_DEPENDS("engine.script.instanceshim")

TEST_CASE("a Luau RemoteEvent copies its payload to an authority callback", "[script][remote-event]") {
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

	auto authority = engine::script::MakeRuntime(
		authorityStore, engine::script::Language::Luau, authorityLimits
	);
	auto client = engine::script::MakeRuntime(clientStore, engine::script::Language::Luau, clientLimits);

	REQUIRE(authority->Run(R"(
		local remote = Instance.new("RemoteEvent")
		remote.Name = "Damage"
		remote.OnServerEvent:Connect(function(payload)
			assert(payload == "copied payload")
			local proof = Instance.new("Part")
			proof.Name = payload
		end)
	)"));
	REQUIRE(client->Run(R"(
		local remote = Instance.new("RemoteEvent")
		remote.Name = "Damage"
		remote:FireServer("copied payload")
	)"));
	REQUIRE_FALSE(sent.empty());
	REQUIRE(authority->DeliverRemoteEvent(sent));
	CHECK(authorityStore.FindFirstRoot("copied payload") != engine::ecs::NULL_ENTITY);
}
