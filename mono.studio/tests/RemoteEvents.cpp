#include <engine/ecs/Store.hpp>
#include <engine/game/Game.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
#include <string>
#include <studio/PlayLink.hpp>

TEST_SUITE_ID("studio.remoteevents")
TEST_DEPENDS("engine.scripthost.remoteevent")

namespace {
	using namespace engine;
	struct RemoteSession {
		world::Universe Worlds;
		world::WorldId Authority;
		std::array<studio::PlayLink, 2> Links;
		std::shared_ptr<script::Runtime> AuthorityRuntime;

		explicit RemoteSession(std::string_view clientSource) {
			parallel::Jobs::Start(1);
			scene::RegisterSceneClasses();
			(void)script::ScriptClass();
			world::WorldSettings settings;
			settings.Name = core::Name("remote.studio");
			settings.TickRate = 60;
			Authority = Worlds.Create(settings);
			Worlds.Enter(Authority, [&](ecs::Store &store, ecs::Scheduler &systems) {
				scene::InstallServices(store);
				const auto remote =
					store.CreateInstance(ecs::Classes::Find(core::Name("RemoteEvent")), "ViewPose");
				REQUIRE(remote != ecs::NULL_ENTITY);
				REQUIRE(store.SetParent(remote, store.FindFirstRoot("ReplicatedStorage")));
				const auto server = script::MakeScript(store, "studio-remote-server.luau", "Receiver");
				REQUIRE(store.SetParent(server, store.FindFirstRoot("ServerScriptService")));
				const auto client = script::MakeScript(store, "studio-remote-client.luau", "Sender", true);
				REQUIRE(store.SetParent(client, store.FindFirstRoot("ReplicatedFirst")));
				script::SourceCache sources;
				sources.Set(core::Name("studio-remote-server.luau"), R"(
local remote = game:GetService('ReplicatedStorage').ViewPose
remote.OnServerEvent:Connect(function(payload, sender)
    assert(sender and sender:IsA('Player'))
    local proof = Instance.new('Part')
    proof.Name = sender.Name .. ':' .. payload
    proof.Parent = workspace
end)
)");
				sources.Set(core::Name("studio-remote-client.luau"), clientSource);
				store.SetResource(sources);
				script::SourceMirror mirror;
				script::MirrorSourcePrograms(store, mirror);
				script::RuntimeLimits limits;
				limits.Role = {.Server = true, .Client = false, .Studio = true};
				std::string failure;
				AuthorityRuntime = game::StartWorldScripts(store, systems, limits, failure);
				INFO(failure);
				REQUIRE(AuthorityRuntime != nullptr);
				REQUIRE(failure.empty());
			});
			for (size_t index = 0; index < Links.size(); ++index) {
				std::string failure;
				REQUIRE(Links[index].Start(Worlds, Authority, 60, failure, index == 0 ? "first" : "second"));
				INFO(failure);
			}
		}

		~RemoteSession() {
			for (auto &link : Links)
				link.Stop(Worlds);
			Worlds.Enter(Authority, [&](ecs::Store &) { AuthorityRuntime.reset(); });
			(void)Worlds.Destroy(Authority);
			parallel::Jobs::Stop();
		}

		void Step(size_t count) {
			std::array<studio::PlayLink *, 2> links{&Links[0], &Links[1]};
			for (size_t tick = 0; tick < count; ++tick) {
				studio::PlayLink::StepMany(Worlds, links, [&](world::WorldId world) {
					return world == Authority ? AuthorityRuntime.get() : nullptr;
				});
				Worlds.Tick(1.0f / 60.0f);
			}
		}
	};
}

TEST_CASE(
	"two Studio clients authenticate RemoteEvent senders independently of payload", "[studio][remote-event]"
) {
	RemoteSession session(R"(
game:GetService('ReplicatedStorage').ViewPose:FireServer('second')
)");
	session.Step(32);
	session.Worlds.Enter(session.Authority, [](ecs::Store &store) {
		const auto workspace = scene::WorkspaceOf(store);
		CHECK(store.FindFirstChild(workspace, "first:second") != ecs::NULL_ENTITY);
		CHECK(store.FindFirstChild(workspace, "second:second") != ecs::NULL_ENTITY);
		CHECK(store.FindFirstChild(workspace, "nil:second") == ecs::NULL_ENTITY);
	});
}

TEST_CASE("Studio RemoteEvent queues report bounded backpressure", "[studio][remote-event]") {
	RemoteSession session(R"(
local remote = game:GetService('ReplicatedStorage').ViewPose
for index = 1, 64 do remote:FireServer(tostring(index)) end
assert(not pcall(function() remote:FireServer('overflow') end))
)");
	session.Step(32);
	session.Worlds.Enter(session.Authority, [](ecs::Store &store) {
		const auto workspace = scene::WorkspaceOf(store);
		for (const auto name : {"first:1", "first:64", "second:1", "second:64"})
			CHECK(store.FindFirstChild(workspace, name) != ecs::NULL_ENTITY);
		CHECK(store.FindFirstChild(workspace, "first:overflow") == ecs::NULL_ENTITY);
		CHECK(store.FindFirstChild(workspace, "second:overflow") == ecs::NULL_ENTITY);
	});
}
