#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/PlayerGui.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <studio/PlayLink.hpp>

TEST_SUITE_ID("studio.playerguilifecycle")

namespace {
	using namespace engine;
	ecs::Entity LocalGuiChild(const ecs::Store &store, ecs::Entity parent, std::string_view name) {
		ecs::Entity found;
		store.EachChild(parent, [&](ecs::Entity child) {
			if (store.InstanceNameOf(child).Text() == name && !gui::IsPlayerGuiSource(store, child))
				found = child;
		});
		return found;
	}

	struct GuiSession {
		world::Universe Worlds;
		world::WorldId Authority;
		std::array<studio::PlayLink, 2> Links;

		GuiSession() {
			parallel::Jobs::Start(1);
			scene::RegisterSceneClasses();
			gui::RegisterGuiClasses();
			(void)script::ScriptClass();
			world::WorldSettings settings;
			settings.Name = core::Name("gui.lifecycle");
			settings.TickRate = 60;
			Authority = Worlds.Create(settings);
			Worlds.Enter(Authority, [](ecs::Store &store) {
				scene::InstallServices(store);
				const auto starter = store.FindFirstRoot("StarterGui");
				for (const bool reset : {false, true}) {
					const auto screen =
						store.CreateInstance(gui::GuiClass("ScreenGui"), reset ? "Hud" : "Persistent");
					store.GetMutable<gui::Layer>(screen)->ResetOnSpawn = reset;
					store.SetParent(screen, starter);
					for (const char *name : {"Status", "Counter"}) {
						const auto label = store.CreateInstance(gui::GuiClass("TextLabel"), name);
						store.GetMutable<gui::Label>(label)->Text = "template";
						store.SetParent(label, screen);
					}
					const auto boot =
						script::MakeScript(store, "player-gui-bootstrap.luau", "Bootstrap", true);
					store.SetParent(boot, screen);
					store.Set(boot, script::Program{core::Name("player-gui-bootstrap.luau"), R"(
local root = script.Parent
local player = game:GetService('Players').LocalPlayer
root.Status.Text = player.Name
root.Counter.Text = '0'
local calls = 0
root.Status:GetPropertyChangedSignal('Text'):Connect(function()
    calls += 1
    root.Counter.Text = tostring(calls)
end)
if not root.ResetOnSpawn then
    for _, resets in {false, true} do
        local screen = Instance.new('ScreenGui')
        screen.Name = resets and 'LocalReset' or 'LocalKeep'
        screen.ResetOnSpawn = resets
        screen.Parent = player.PlayerGui
        local label = Instance.new('TextLabel')
        label.Name = 'Status'
        label.Text = 'client-created'
        label.Parent = screen
    end
end
)"});
				}
				const auto secret = script::MakeScript(store, "server-secret.luau", "ServerSecret");
				store.SetParent(secret, store.FindFirstRoot("ServerStorage"));
			});
		}

		~GuiSession() {
			for (auto &link : Links) {
				if (!link.IsRunning()) continue;
				Worlds.Enter(link.ReplicaWorld(), [](ecs::Store &) {});
				link.Stop(Worlds);
			}
			Worlds.Enter(Authority, [](ecs::Store &) {});
			parallel::Jobs::Stop();
		}

		void Step(int count) {
			std::array<studio::PlayLink *, 2> links{&Links[0], &Links[1]};
			for (int tick = 0; tick < count; ++tick) {
				studio::PlayLink::StepMany(Worlds, links);
				Worlds.Tick(1.0f / 60.0f);
				for (auto &link : Links) {
					if (link.IsRunning()) Worlds.Present(link.ReplicaWorld(), 1.0f / 60.0f, 1.0f);
				}
			}
		}
	};
}

TEST_CASE(
	"Studio clients receive independent mutable GUI and keep persistent callbacks on respawn",
	"[studio][playergui]"
) {
	using namespace engine;
	GuiSession session;
	std::string error;
	REQUIRE(session.Links[0].Start(session.Worlds, session.Authority, 60, error, "Ada"));
	REQUIRE(session.Links[1].Start(session.Worlds, session.Authority, 60, error, "Grace"));
	session.Step(32);
	std::array<ecs::Entity, 2> persistent;
	std::array<ecs::Entity, 2> oldHud;
	std::array<ecs::Entity, 2> localKeep;
	std::array<ecs::Entity, 2> localReset;
	std::array<ecs::Entity, 2> firstCharacter;
	for (size_t index = 0; index < session.Links.size(); ++index) {
		const auto &link = session.Links[index];
		session.Worlds.Enter(link.ReplicaWorld(), [&](ecs::Store &store) {
			const auto target = store.FindFirstChild(link.Player(), "PlayerGui");
			REQUIRE(target != ecs::NULL_ENTITY);
			firstCharacter[index] = scene::CharacterOf(store, link.Player());
			REQUIRE(firstCharacter[index] != ecs::NULL_ENTITY);
			CHECK(store.FindFirstChild(session.Links[1 - index].Player(), "PlayerGui") == ecs::NULL_ENTITY);
			CHECK(store.FindFirstRoot("ServerStorage") == ecs::NULL_ENTITY);
			persistent[index] = LocalGuiChild(store, target, "Persistent");
			oldHud[index] = LocalGuiChild(store, target, "Hud");
			REQUIRE(persistent[index] != ecs::NULL_ENTITY);
			REQUIRE(oldHud[index] != ecs::NULL_ENTITY);
			localKeep[index] = LocalGuiChild(store, target, "LocalKeep");
			localReset[index] = LocalGuiChild(store, target, "LocalReset");
			REQUIRE(localKeep[index] != ecs::NULL_ENTITY);
			REQUIRE(localReset[index] != ecs::NULL_ENTITY);
			CHECK(ecs::Store::IsPredicted(localKeep[index]));
			CHECK(ecs::Store::IsPredicted(localReset[index]));
			CHECK(
				store.Get<gui::Label>(store.FindFirstChild(persistent[index], "Status"))->Text ==
				(index == 0 ? "Ada" : "Grace")
			);
			CHECK(
				store.Get<gui::Label>(store.FindFirstChild(oldHud[index], "Status"))->Text ==
				(index == 0 ? "Ada" : "Grace")
			);
			const auto templateRoot = store.FindFirstChild(store.FindFirstRoot("StarterGui"), "Persistent");
			REQUIRE(templateRoot != ecs::NULL_ENTITY);
			CHECK(store.Get<gui::Label>(store.FindFirstChild(templateRoot, "Status"))->Text == "template");
		});
	}
	for (size_t index = 0; index < session.Links.size(); ++index) {
		session.Worlds.Enter(session.Links[index].ReplicaWorld(), [&](ecs::Store &store) {
			CHECK(ecs::Store::IsPredicted(persistent[index]));
			CHECK(ecs::Store::IsPredicted(oldHud[index]));
			CHECK(scene::PlayerOwning(store, persistent[index]) == session.Links[index].Player());
		});
	}

	// Exercise the same authority reset used by the host's automatic respawn.
	session.Worlds.Enter(session.Authority, [&](ecs::Store &store) {
		for (auto &link : session.Links) {
			const auto *identity = store.Get<scene::PlayerIdentity>(link.Player());
			REQUIRE(identity != nullptr);
			scene::RemoveCharacter(store, link.Player());
			store.GetMutable<scene::PlayerIdentity>(link.Player())->RespawnTime = 0;
		}
	});
	session.Step(32);
	for (size_t index = 0; index < session.Links.size(); ++index) {
		const auto &link = session.Links[index];
		session.Worlds.Enter(link.ReplicaWorld(), [&](ecs::Store &store) {
			const auto target = store.FindFirstChild(link.Player(), "PlayerGui");
			INFO(
				"replica " << index << " PlayerGui=" << target.Id
						   << " character=" << scene::CharacterOf(store, link.Player()).Id
						   << " first character=" << firstCharacter[index].Id
			);
			INFO(
				"persistent alive=" << store.Alive(persistent[index])
									<< " parent=" << store.ParentOf(persistent[index]).Id
									<< " kept parent=" << store.ParentOf(localKeep[index]).Id
			);
			CHECK(scene::CharacterOf(store, link.Player()) != firstCharacter[index]);
			store.EachChild(target, [&](ecs::Entity child) {
				UNSCOPED_INFO(
					"child " << store.InstanceNameOf(child).Text() << " id=" << child.Id
							 << " local=" << ecs::IsClientLocalInstance(store, child)
							 << " source=" << gui::IsPlayerGuiSource(store, child)
				);
			});
			CHECK(LocalGuiChild(store, target, "Persistent") == persistent[index]);
			CHECK(LocalGuiChild(store, target, "LocalKeep") == localKeep[index]);
			CHECK_FALSE(store.Alive(localReset[index]));
			CHECK(LocalGuiChild(store, target, "LocalReset") == ecs::NULL_ENTITY);
			CHECK_FALSE(store.Alive(oldHud[index]));
			const auto freshHud = LocalGuiChild(store, target, "Hud");
			REQUIRE(freshHud != ecs::NULL_ENTITY);
			CHECK(freshHud != oldHud[index]);
			CHECK(
				store.Get<gui::Label>(store.FindFirstChild(persistent[index], "Status"))->Text ==
				(index == 0 ? "Ada" : "Grace")
			);
			const auto label = store.FindFirstChild(persistent[index], "Status");
			const std::string changed("after-respawn");
			REQUIRE(store.SetPropertyAuthored(label, core::Name("Text"), &changed, sizeof(changed)));
		});
	}
	session.Step(2);
	for (size_t index = 0; index < session.Links.size(); ++index) {
		session.Worlds.Enter(session.Links[index].ReplicaWorld(), [&](ecs::Store &store) {
			const auto counter = store.FindFirstChild(persistent[index], "Counter");
			CHECK(store.Get<gui::Label>(counter)->Text != "0");
			CHECK(store.Get<gui::Label>(counter)->Text != "template");
		});
	}
}
