// The Arena demo and server GUI copies over a real server process. Split from
// Replication.cpp so the runner can run the suites side by side.

#include "fixtures/ReplicationRemote.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("server.replication-arena")
TEST_DEPENDS("engine.scene.registration")

using namespace server_replication_test;

TEST_CASE("Arena Luau validates shots and replicates hits", "[server][replication][arena]") {
	if (!ServerAvailable()) SKIP("the server program is not built into this preset");
	Remote shooter;
	REQUIRE(shooter.Start(0, engine::examples::ExamplePath("Arena.luau")));
	REQUIRE(shooter.Join(400));
	Remote victim;
	REQUIRE(victim.Connect(shooter.Port));
	REQUIRE(victim.Join(400));
	Settle(shooter);
	Settle(victim);

	const Entity victimCharacter = engine::scene::CharacterOf(shooter.World, victim.Mine);
	const Entity shooterCharacter = engine::scene::CharacterOf(shooter.World, shooter.Mine);
	REQUIRE(victimCharacter != engine::ecs::NULL_ENTITY);
	REQUIRE(shooterCharacter != engine::ecs::NULL_ENTITY);
	const Character *victimRig = shooter.World.Get<Character>(victimCharacter);
	const Character *shooterRig = shooter.World.Get<Character>(shooterCharacter);
	REQUIRE(victimRig != nullptr);
	REQUIRE(shooterRig != nullptr);
	const Entity victimRoot = victimRig->Root;
	const Entity victimHumanoid = victimRig->Humanoid;
	const Entity shooterRoot = shooterRig->Root;
	const Entity shooterHumanoid = shooterRig->Humanoid;
	REQUIRE(shooter.World.Get<Humanoid>(victimHumanoid) != nullptr);
	REQUIRE(shooter.World.Get<Humanoid>(shooterHumanoid) != nullptr);
	const Entity fire =
		shooter.World.FindFirstChild(shooter.World.FindFirstRoot("ReplicatedStorage"), "ArenaFire");
	REQUIRE(fire != engine::ecs::NULL_ENTITY);
	CHECK(shooter.World.GetFullName(fire) == "ReplicatedStorage.ArenaFire");

	shooter.LookAlong(0.0f);
	shooter.Hold({engine::scene::KeyCode::W}, 120, &victim);
	const auto *shooterAt = shooter.World.Get<Transform>(shooterRoot);
	const auto *victimAt = shooter.World.Get<Transform>(victimRoot);
	REQUIRE(shooterAt != nullptr);
	REQUIRE(victimAt != nullptr);
	const engine::core::Vector3 muzzle = shooterAt->Frame.Position + engine::core::Vector3{0, 3, 0};
	const engine::core::Vector3 victimPosition = victimAt->Frame.Position;
	const engine::core::Vector3 direction = (victimPosition - muzzle).Unit();
	const float initialHealth = shooter.World.Get<Humanoid>(victimHumanoid)->Health;

	const auto payload = [](const engine::core::Vector3 &origin, const engine::core::Vector3 &aim) {
		return std::string("{\"x\":") + std::to_string(origin.X) + ",\"y\":" + std::to_string(origin.Y) +
			   ",\"z\":" + std::to_string(origin.Z) + ",\"dx\":" + std::to_string(aim.X) +
			   ",\"dy\":" + std::to_string(aim.Y) + ",\"dz\":" + std::to_string(aim.Z) + "}";
	};
	const auto send = [&](std::string_view body) {
		std::vector<std::byte> framed;
		REQUIRE(
			engine::script::EncodeRemoteEvent(
				"ReplicatedStorage.ArenaFire", std::as_bytes(std::span(body.data(), body.size())), framed
			)
		);
		REQUIRE(shooter.Link->SendUser(framed, engine::core::Clock::Seconds()));
	};
	const auto pump = [&](int cycles) {
		for (int tick = 0; tick < cycles; tick++) {
			shooter.Tick();
			victim.Tick();
			std::this_thread::sleep_for(std::chrono::milliseconds(4));
		}
	};

	send("not json");
	send(payload(muzzle + engine::core::Vector3{200, 0, 0}, direction));
	send(payload(muzzle, direction * 4.0f));
	pump(30);
	REQUIRE(shooter.World.Get<Humanoid>(victimHumanoid)->Health == initialHealth);

	// A valid ray into open sky has no consequence.
	send(payload(muzzle, engine::core::Vector3{0, 1, 0}));
	pump(50);
	REQUIRE(shooter.World.Get<Humanoid>(victimHumanoid)->Health == initialHealth);

	// A ray through the shooter's body cannot damage its own humanoid.
	const float shooterHealth = shooter.World.Get<Humanoid>(shooterHumanoid)->Health;
	send(payload(muzzle, engine::core::Vector3{0, -1, 0}));
	pump(50);
	CHECK(shooter.World.Get<Humanoid>(shooterHumanoid)->Health == shooterHealth);

	const std::string valid = payload(muzzle, direction);
	for (int attempt = 0;
		 attempt < 120 && shooter.World.Get<Humanoid>(victimHumanoid)->Health == initialHealth;
		 attempt++) {
		send(valid);
		pump(1);
	}
	INFO(
		"muzzle at " << muzzle.X << ", " << muzzle.Y << ", " << muzzle.Z << "; victim at " << victimPosition.X
					 << ", " << victimPosition.Y << ", " << victimPosition.Z
	);
	INFO("red parts: " << RedParts(shooter.World));
	REQUIRE(shooter.World.Get<Humanoid>(victimHumanoid)->Health < initialHealth);
	const float wounded = shooter.World.Get<Humanoid>(victimHumanoid)->Health;
	const auto *tint = shooter.World.Get<Visual>(victimRoot);
	REQUIRE(tint != nullptr);
	CHECK(tint->Tint.R >= 0.99f);
	CHECK(tint->Tint.G <= 0.21f);
	REQUIRE(victim.Wait(
		[&] {
			shooter.Tick();
			const auto *received = victim.World.Get<Humanoid>(victimHumanoid);
			return received != nullptr && received->Health <= wounded;
		},
		400
	));
	const engine::ecs::PropertyDescriptor *healthProperty = nullptr;
	for (const auto &property : engine::ecs::Classes::Describe(engine::scene::HumanoidClass()).Properties) {
		if (property.Name == Name("Health")) healthProperty = &property;
	}
	REQUIRE(healthProperty != nullptr);
	const float restored = 100.0f;
	CHECK_FALSE(victim.World.SetProperty(victimHumanoid, *healthProperty, &restored, sizeof(restored)));

	// Requests in one interval cannot apply damage twice.
	pump(50);
	const float beforeBurst = shooter.World.Get<Humanoid>(victimHumanoid)->Health;
	send(valid);
	send(valid);
	pump(30);
	CHECK(shooter.World.Get<Humanoid>(victimHumanoid)->Health >= beforeBurst - 25.0f);

	// The standalone scene's own target is damageable through the same raycast.
	const Entity workspace = shooter.World.FindFirstRoot("Workspace");
	const Entity target = shooter.World.FindFirstChild(workspace, "TargetCenter");
	REQUIRE(target != engine::ecs::NULL_ENTITY);
	const Entity targetCore = shooter.World.FindFirstChild(target, "Core");
	const Entity targetHumanoid = shooter.World.FindFirstChild(target, "Humanoid");
	REQUIRE(targetCore != engine::ecs::NULL_ENTITY);
	REQUIRE(targetHumanoid != engine::ecs::NULL_ENTITY);
	REQUIRE(shooter.World.Get<Humanoid>(targetHumanoid) != nullptr);
	const auto *targetAt = shooter.World.Get<Transform>(targetCore);
	REQUIRE(targetAt != nullptr);
	const engine::core::Vector3 targetDirection = (targetAt->Frame.Position - muzzle).Unit();
	pump(50);
	for (int attempt = 0; attempt < 120 && shooter.World.Get<Humanoid>(targetHumanoid)->Health == 100.0f;
		 attempt++) {
		send(payload(muzzle, targetDirection));
		pump(1);
	}
	REQUIRE(shooter.World.Get<Humanoid>(targetHumanoid)->Health < 100.0f);
	REQUIRE(shooter.World.Get<Visual>(targetCore) != nullptr);
	CHECK(shooter.World.Get<Visual>(targetCore)->Tint.R >= 0.99f);
	CHECK(shooter.World.Get<Visual>(targetCore)->Tint.G <= 0.21f);
}

TEST_CASE(
	"Arena client companion fires from a click and the current camera", "[server][replication][arena]"
) {
	if (!ServerAvailable()) SKIP("the server program is not built into this preset");
	Remote viewer;
	REQUIRE(viewer.Start(0, engine::examples::ExamplePath("Arena.luau")));
	REQUIRE(viewer.Join(400));
	Settle(viewer);
	const Entity character = engine::scene::CharacterOf(viewer.World, viewer.Mine);
	const Character *rig = viewer.World.Get<Character>(character);
	REQUIRE(rig != nullptr);
	const Entity workspace = viewer.World.FindFirstRoot("Workspace");
	const Entity target = viewer.World.FindFirstChild(workspace, "TargetCenter");
	REQUIRE(target != engine::ecs::NULL_ENTITY);
	const Entity targetCore = viewer.World.FindFirstChild(target, "Core");
	const Entity targetHumanoid = viewer.World.FindFirstChild(target, "Humanoid");
	const Entity screened = viewer.World.FindFirstChild(workspace, "TargetLeft");
	REQUIRE(screened != engine::ecs::NULL_ENTITY);
	const Entity screenedCore = viewer.World.FindFirstChild(screened, "Core");
	const Entity screenedHumanoid = viewer.World.FindFirstChild(screened, "Humanoid");
	const Entity pillar = viewer.World.FindFirstChild(workspace, "Pillar4");
	REQUIRE(targetCore != engine::ecs::NULL_ENTITY);
	REQUIRE(viewer.World.Get<Humanoid>(targetHumanoid) != nullptr);
	REQUIRE(screenedCore != engine::ecs::NULL_ENTITY);
	REQUIRE(viewer.World.Get<Humanoid>(screenedHumanoid) != nullptr);
	REQUIRE(pillar != engine::ecs::NULL_ENTITY);
	REQUIRE(viewer.World.Get<Visual>(pillar) != nullptr);
	const auto *rootAt = viewer.World.Get<Transform>(rig->Root);
	const auto *targetAt = viewer.World.Get<Transform>(targetCore);
	const auto *screenedAt = viewer.World.Get<Transform>(screenedCore);
	REQUIRE(rootAt != nullptr);
	REQUIRE(targetAt != nullptr);
	REQUIRE(screenedAt != nullptr);
	const engine::core::Vector3 eye = rootAt->Frame.Position + engine::core::Vector3{0, 3, 0};
	const engine::core::Vector3 targetPosition = targetAt->Frame.Position;
	const engine::core::Vector3 screenedPosition = screenedAt->Frame.Position;
	const Entity camera = viewer.World.CreatePredictedInstance(engine::scene::CameraClass(), "ArenaView");
	REQUIRE(camera != engine::ecs::NULL_ENTITY);
	viewer.World.Set(camera, engine::scene::Transform{engine::core::CFrame::LookAt(eye, screenedPosition)});
	viewer.World.Set(camera, engine::scene::Camera{});
	viewer.World.SetParent(camera, workspace);
	engine::scene::ActiveCamera active;
	active.Entity = camera;
	viewer.World.SetResource(active);

	engine::script::RuntimeLimits limits;
	limits.Role = engine::script::HostRole::OfClient();
	limits.RemoteEventSender = [&](std::span<const std::byte> bytes) {
		return viewer.Link->SendUser(bytes, engine::core::Clock::Seconds());
	};
	auto runtime = engine::script::MakeRuntime(viewer.World, engine::script::Language::Luau, limits);
	REQUIRE(runtime != nullptr);
	const Entity playerScripts = viewer.World.FindFirstChild(viewer.Mine, "PlayerScripts");
	REQUIRE(viewer.World.FindFirstChild(playerScripts, "DemoCamera") != engine::ecs::NULL_ENTITY);
	REQUIRE(runtime->RunNewScripts(engine::script::ClientScriptsIn(viewer.World)) >= 1);
	INFO(runtime->LastError());

	auto *input = viewer.World.ResourceMutable<engine::scene::InputState>();
	if (input == nullptr) {
		viewer.World.SetResource(engine::scene::InputState{});
		input = viewer.World.ResourceMutable<engine::scene::InputState>();
	}
	input->Focused = true;
	input->PreviousButtons = 0;
	input->Buttons = static_cast<uint8_t>(1u << static_cast<uint8_t>(engine::scene::MouseButton::Left));
	REQUIRE(runtime->Heartbeat(1.0f / 60.0f));
	INFO(runtime->LastError());
	input->PreviousButtons = input->Buttons;
	input->Buttons = 0;
	REQUIRE(runtime->Heartbeat(1.0f / 60.0f));
	REQUIRE(viewer.Wait(
		[&] {
			const auto *visual = viewer.World.Get<Visual>(pillar);
			return visual != nullptr && visual->Tint.R >= 0.99f && visual->Tint.G <= 0.21f;
		},
		400
	));
	CHECK(viewer.World.Get<Humanoid>(screenedHumanoid)->Health == 100.0f);

	for (int tick = 0; tick < 50; tick++) {
		viewer.Tick();
		std::this_thread::sleep_for(std::chrono::milliseconds(4));
	}
	viewer.World.Set(camera, engine::scene::Transform{engine::core::CFrame::LookAt(eye, targetPosition)});
	input = viewer.World.ResourceMutable<engine::scene::InputState>();
	REQUIRE(input != nullptr);
	input->PreviousButtons = 0;
	input->Buttons = static_cast<uint8_t>(1u << static_cast<uint8_t>(engine::scene::MouseButton::Left));
	REQUIRE(runtime->Heartbeat(1.0f / 60.0f));
	INFO(
		"eye at " << eye.X << ", " << eye.Y << ", " << eye.Z << "; center at " << targetPosition.X << ", "
				  << targetPosition.Y << ", " << targetPosition.Z
	);
	INFO("red parts: " << RedParts(viewer.World));
	REQUIRE(viewer.Wait(
		[&] {
			const auto *wounded = viewer.World.Get<Humanoid>(targetHumanoid);
			return wounded != nullptr && wounded->Health < 100.0f;
		},
		400
	));
}

TEST_CASE(
	"transport admission waits for an explicit fresh player request", "[server][replication][portal-session]"
) {
	if (!ServerAvailable()) SKIP("the server program is not built into this preset");
	const auto path = engine::core::Paths::Base() / "portal-session-fresh.luau";
	{
		std::ofstream source(path);
		REQUIRE(source);
		source << "local floor = Instance.new('Part')\n"
			   << "floor.Anchored = true\n"
			   << "floor.Size = Vector3.new(100, 1, 100)\n"
			   << "floor.Parent = workspace\n";
	}
	Remote remote;
	remote.AutomaticFreshAdmission = false;
	REQUIRE(remote.Start(0, path.string()));
	REQUIRE(remote.Join(1000));
	Settle(remote);
	const auto players = [&] {
		size_t count = 0;
		remote.World.EachEntity([&](Entity entity) {
			if (remote.World.IsA(entity, engine::scene::PlayerClass())) count++;
		});
		return count;
	};
	CHECK(players() == 0);
	CHECK(remote.Mine == engine::ecs::NULL_ENTITY);
	engine::game::PortalSessionMessage freshMessage{};
	freshMessage.Kind = engine::game::PortalSessionKind::Fresh;
	freshMessage.Attempt = 1;
	const auto fresh = engine::game::EncodePortalSession(freshMessage);
	REQUIRE(remote.Link->SendUser(fresh, remote.Now));
	REQUIRE(remote.Wait(
		[&] {
			return remote.Mine != engine::ecs::NULL_ENTITY &&
				   remote.World.IsA(remote.Mine, engine::scene::PlayerClass()) &&
				   engine::scene::CharacterOf(remote.World, remote.Mine) != engine::ecs::NULL_ENTITY;
		},
		1000
	));
	CHECK(players() == 1);
	const auto player = remote.Mine;
	const auto character = engine::scene::CharacterOf(remote.World, player);
	REQUIRE(remote.Link->SendUser(fresh, remote.Now));
	Settle(remote);
	CHECK(players() == 1);
	CHECK(remote.Mine == player);
	CHECK(engine::scene::CharacterOf(remote.World, player) == character);
	std::error_code ignored;
	std::filesystem::remove(path, ignored);
}

TEST_CASE(
	"server GUI copies retain client state and callbacks across scripted respawn", "[server][playergui]"
) {
	using namespace engine;
	if (!ServerAvailable()) SKIP("the server program is not built into this preset");
	const auto directory = std::filesystem::temp_directory_path();
	const auto bootstrap = directory / "server_playergui_bootstrap.luau";
	const auto scenePath = directory / "server_playergui_lifecycle.aworld";
	struct RemoveFiles {
		std::array<std::filesystem::path, 2> Paths;
		~RemoveFiles() {
			for (const auto &path : Paths) {
				std::error_code ignored;
				std::filesystem::remove(path, ignored);
			}
		}
	} cleanup{{bootstrap, scenePath}};
	{
		std::ofstream file(bootstrap);
		REQUIRE(file);
		file << R"(
local root = script.Parent
root.Status.Text = game:GetService('Players').LocalPlayer.Name
root.Counter.Text = '0'
local calls = 0
root.Status:GetPropertyChangedSignal('Text'):Connect(function()
    calls += 1
    root.Counter.Text = tostring(calls)
end)
)";
	}
	{
		std::ofstream file(scenePath);
		REQUIRE(file);
		file << R"(<World format="3" name="GuiLifecycle">
<Sources><Source path="gui-command.luau"><![CDATA[
local command = game:GetService('ReplicatedStorage'):WaitForChild('GuiCommand')
command.OnServerEvent:Connect(function(payload)
    for _, player in game:GetService('Players'):GetPlayers() do
        if payload == 'respawn' then
            player:LoadCharacter()
        else
            player.PlayerGui.Persistent.Status.Text = 'server-update'
        end
    end
end)
]]></Source></Sources>
<Item class="StarterGui" name="StarterGui" id="1">
)";
		uint64_t item = 2;
		for (const bool reset : {false, true}) {
			file << "<Item class=\"ScreenGui\" name=\"" << (reset ? "Hud" : "Persistent") << "\" id=\""
				 << item++ << "\"><Property name=\"ResetOnSpawn\" type=\"bool\">"
				 << (reset ? "true" : "false") << "</Property>\n";
			for (const char *name : {"Status", "Counter"})
				file << "<Item class=\"TextLabel\" name=\"" << name << "\" id=\"" << item++
					 << "\"><Property name=\"Text\" type=\"string\">template</Property></Item>\n";
			file << "<Item class=\"LocalScript\" name=\"Bootstrap\" id=\"" << item++
				 << "\"><Property name=\"Source\" type=\"string\">" << bootstrap.string()
				 << "</Property></Item></Item>\n";
		}
		file << R"(</Item>
<Item class="ReplicatedStorage" name="ReplicatedStorage" id="20"><Item class="RemoteEvent" name="GuiCommand" id="21" /></Item>
<Item class="ServerScriptService" name="ServerScriptService" id="22"><Item class="Script" name="GuiCommands" id="23"><Property name="Source" type="string">gui-command.luau</Property></Item></Item>
</World>)";
	}
	Remote first;
	REQUIRE(first.Start(0, scenePath.string()));
	REQUIRE(first.Join(400));
	Remote second;
	REQUIRE(second.Connect(first.Port));
	REQUIRE(second.Join(400));
	Settle(first);
	Settle(second);
	std::array<Remote *, 2> clients{&first, &second};
	std::array<std::shared_ptr<script::Runtime>, 2> runtimes;
	std::array<Entity, 2> persistent;
	std::array<Entity, 2> oldHud;
	for (size_t index = 0; index < clients.size(); ++index) {
		auto &client = *clients[index];
		client.World.SetAdoptOnly(true);
		script::RuntimeLimits limits;
		limits.Role = script::HostRole::OfClient();
		runtimes[index] = script::MakeRuntime(client.World, script::Language::Luau, limits);
		(void)gui::RefreshPlayerGuiProjection(
			client.World, client.Mine, scene::CharacterOf(client.World, client.Mine)
		);
		const auto started = runtimes[index]->RunNewScripts(script::ClientScriptsIn(client.World));
		INFO(runtimes[index]->LastError());
		REQUIRE(started == 2);
		const auto target = client.World.FindFirstChild(client.Mine, "PlayerGui");
		persistent[index] =
			gui::FindPlayerGuiCopy(client.World, client.World.FindFirstChild(target, "Persistent"));
		oldHud[index] = gui::FindPlayerGuiCopy(client.World, client.World.FindFirstChild(target, "Hud"));
		REQUIRE(persistent[index] != ecs::NULL_ENTITY);
		REQUIRE(oldHud[index] != ecs::NULL_ENTITY);
		CHECK(client.World.FindFirstChild(clients[1 - index]->Mine, "PlayerGui") == ecs::NULL_ENTITY);
		const auto status = client.World.FindFirstChild(persistent[index], "Status");
		CHECK(
			client.World.Get<gui::Label>(status)->Text ==
			std::string(client.World.InstanceNameOf(client.Mine).Text())
		);
		REQUIRE(
			runtimes[index]->Run(
				"game:GetService('Players').LocalPlayer.PlayerGui.Persistent.Status.Text = 'local-state'"
			)
		);
		client.World.FlushSignals();
		REQUIRE(runtimes[index]->Heartbeat(1.0f / 60.0f));
	}
	const auto refresh = [&] {
		for (size_t index = 0; index < clients.size(); ++index) {
			auto &client = *clients[index];
			(void)gui::RefreshPlayerGuiProjection(
				client.World, client.Mine, scene::CharacterOf(client.World, client.Mine)
			);
			(void)runtimes[index]->RunNewScripts(script::ClientScriptsIn(client.World));
			client.World.FlushSignals();
			REQUIRE(runtimes[index]->Heartbeat(1.0f / 60.0f));
		}
	};
	const auto command = [&](std::string_view payload) {
		std::vector<std::byte> bytes;
		REQUIRE(
			script::EncodeRemoteEvent(
				"ReplicatedStorage.GuiCommand",
				std::as_bytes(std::span(payload.data(), payload.size())),
				bytes
			)
		);
		REQUIRE(first.Link->SendUser(bytes, core::Clock::Seconds()));
	};
	const std::array audits{first.Link->ReplicaStats().Audits, second.Link->ReplicaStats().Audits};
	REQUIRE(first.Wait(
		[&] {
			second.Tick();
			refresh();
			return first.Link->ReplicaStats().Audits >= audits[0] + 2 &&
				   second.Link->ReplicaStats().Audits >= audits[1] + 2;
		},
		1200
	));
	for (size_t index = 0; index < clients.size(); ++index) {
		CHECK(
			clients[index]
				->World.Get<gui::Label>(clients[index]->World.FindFirstChild(persistent[index], "Status"))
				->Text == "local-state"
		);
	}
	command("respawn");
	REQUIRE(first.Wait(
		[&] {
			second.Tick();
			refresh();
			return !first.World.Alive(oldHud[0]) && !second.World.Alive(oldHud[1]);
		},
		1200
	));
	for (size_t index = 0; index < clients.size(); ++index) {
		auto &client = *clients[index];
		const auto target = client.World.FindFirstChild(client.Mine, "PlayerGui");
		CHECK(
			gui::FindPlayerGuiCopy(client.World, client.World.FindFirstChild(target, "Persistent")) ==
			persistent[index]
		);
		CHECK(
			gui::FindPlayerGuiCopy(client.World, client.World.FindFirstChild(target, "Hud")) != oldHud[index]
		);
		CHECK(
			client.World.Get<gui::Label>(client.World.FindFirstChild(persistent[index], "Status"))->Text ==
			"local-state"
		);
	}
	command("server-edit");
	REQUIRE(first.Wait(
		[&] {
			second.Tick();
			refresh();
			return first.World.Get<gui::Label>(first.World.FindFirstChild(persistent[0], "Status"))->Text ==
					   "server-update" &&
				   second.World.Get<gui::Label>(second.World.FindFirstChild(persistent[1], "Status"))->Text ==
					   "server-update";
		},
		1200
	));
	for (size_t index = 0; index < clients.size(); ++index) {
		REQUIRE(runtimes[index]->Heartbeat(1.0f / 60.0f));
		CHECK(
			clients[index]
				->World.Get<gui::Label>(clients[index]->World.FindFirstChild(persistent[index], "Counter"))
				->Text != "0"
		);
	}
}
