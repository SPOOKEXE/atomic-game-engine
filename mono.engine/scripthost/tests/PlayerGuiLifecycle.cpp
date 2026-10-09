#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/PlayerGui.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/gui/Services.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/InstanceShim.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/scripthost/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_SUITE_ID("engine.scripthost.playerguilifecycle")

namespace {
	engine::ecs::Entity
	LocalGuiChild(const engine::ecs::Store &store, engine::ecs::Entity parent, std::string_view name) {
		const auto child = store.FindFirstChild(parent, name);
		return engine::gui::IsPlayerGuiSource(store, child) ? engine::gui::FindPlayerGuiCopy(store, child)
															: child;
	}
}

TEST_CASE(
	"destroyed scripts cancel native waits without cancelling their replacement", "[scripting][lifetime]"
) {
	using namespace engine;
	scene::EnsureClassTree();
	(void)script::ScriptClass();
	for (const auto language : {script::Language::Luau, script::Language::JavaScript}) {
		ecs::Store store("script.native-lifetime");
		scene::InstallServices(store);
		const auto workspace = scene::WorkspaceOf(store);
		const std::string path =
			language == script::Language::Luau ? "native-lifetime.luau" : "native-lifetime.js";
		script::SourceCache cache;
		if (language == script::Language::Luau) {
			cache.Set(core::Name("lifetime-factory.luau"), R"(
return function(owner)
    local function emit(stage)
        local part = Instance.new('Part', workspace)
        part.Name = owner .. '.' .. stage
    end
    task.defer(function() emit('defer') end)
    task.delay(0, function() emit('delay') end)
    task.spawn(function() task.wait(0); emit('wait') end)
    task.spawn(function() workspace:WaitForChild('Wake', 1); emit('child') end)
    game:GetService('RunService').Heartbeat:Once(function() emit('beat') end)
end
)");
			cache.Set(core::Name(path), "require(workspace:FindFirstChild('Factory'))(script.Name)");
			const auto module = script::MakeModule(store, "lifetime-factory.luau", "Factory");
			REQUIRE(store.SetParent(module, workspace));
		} else {
			cache.Set(core::Name(path), R"(
const owner = script.Name;
function emit(stage) {
    const part = Instance.new('Part', workspace);
    part.Name = owner + '.' + stage;
}
task.defer(() => emit('defer'));
task.delay(0, () => emit('delay'));
task.spawn(async () => { await task.wait(0); emit('wait'); });
task.spawn(async () => { await workspace.WaitForChild('Wake', 1); emit('child'); });
game.GetService('RunService').Heartbeat.Once(() => emit('beat'));
)");
		}
		store.SetResource(cache);
		const auto runtime = script::MakeRuntime(store, language, {.Role = script::HostRole::OfBoth()});
		const auto old = script::MakeScript(store, path, "Old", true);
		REQUIRE(runtime->RunInstance(old));
		store.DestroyInstance(old);
		const auto fresh = script::MakeScript(store, path, "Fresh", true);
		const bool started = runtime->RunInstance(fresh);
		INFO(runtime->LastError());
		REQUIRE(started);
		const auto wake = store.CreateInstance(scene::PartClass(), "Wake");
		REQUIRE(store.SetParent(wake, workspace));
		store.AdvanceTick(store.Time().Delta);
		const bool beat = runtime->Heartbeat(store.Time().Delta);
		INFO(runtime->LastError());
		REQUIRE(beat);
		for (const auto stage : {"defer", "delay", "wait", "child", "beat"}) {
			INFO(stage);
			CHECK(store.FindFirstChild(workspace, std::string("Old.") + stage) == ecs::NULL_ENTITY);
			const auto created = store.FindFirstChild(workspace, std::string("Fresh.") + stage);
			REQUIRE(created != ecs::NULL_ENTITY);
			CHECK(ecs::Store::IsPredicted(created));
		}
	}
}

TEST_CASE(
	"reset GUI scripts stop their old heartbeat callbacks in both VMs", "[scripting][playergui][lifetime]"
) {
	using namespace engine;
	scene::RegisterSceneClasses();
	gui::RegisterGuiClasses();
	(void)script::ScriptClass();
	for (const auto language : {script::Language::Luau, script::Language::JavaScript}) {
		ecs::Store store("playergui.script-lifetime");
		scene::InstallServices(store);
		const auto player = scene::AddPlayer(store, "Ada", true);
		const auto target = store.FindFirstChild(player, "PlayerGui");
		const auto persistent = store.CreateInstance(gui::GuiClass("ScreenGui"), "Persistent");
		store.SetParent(persistent, target);
		store.GetMutable<gui::Layer>(persistent)->ResetOnSpawn = false;
		const auto counter = store.CreateInstance(gui::GuiClass("TextLabel"), "Counter");
		store.SetParent(counter, persistent);
		store.GetMutable<gui::Label>(counter)->Text = "0";
		const auto hud = store.CreateInstance(gui::GuiClass("ScreenGui"), "Hud");
		store.SetParent(hud, store.FindFirstRoot("StarterGui"));
		const auto path = language == script::Language::Luau ? "gui-lifetime.luau" : "gui-lifetime.js";
		const auto source = language == script::Language::Luau ? R"(
local counter = game:GetService('Players').LocalPlayer.PlayerGui.Persistent.Counter
game:GetService('RunService').Heartbeat:Connect(function()
    counter.Text = tostring(tonumber(counter.Text) + 1)
end)
)"
															   : R"(
let counter = game.GetService('Players').LocalPlayer.PlayerGui.FindFirstChild('Persistent').FindFirstChild('Counter');
game.GetService('RunService').Heartbeat.Connect(function() {
    counter.Text = String(Number(counter.Text) + 1);
});
)";
		const auto program = script::MakeScript(store, path, "Bootstrap", true);
		store.SetParent(program, hud);
		store.Set(program, script::Program{core::Name(path), source});
		script::RuntimeLimits limits;
		limits.Role = script::HostRole::OfBoth();
		const auto runtime = script::MakeRuntime(store, language, limits);
		for (int life = 0; life < 3; ++life) {
			const auto character = scene::LoadCharacter(store, player);
			REQUIRE(character != ecs::NULL_ENTITY);
			REQUIRE(gui::ResetPlayerGui(store, player) == 1);
			(void)gui::RefreshPlayerGuiProjection(store, player, character);
			REQUIRE(runtime->RunNewScripts(script::ClientScriptsIn(store)) == 1);
			const auto localCounter = gui::FindPlayerGuiCopy(store, counter);
			REQUIRE(localCounter != ecs::NULL_ENTITY);
			const int before = std::stoi(store.Get<gui::Label>(localCounter)->Text);
			const bool beat = runtime->Heartbeat(1.0f / 60.0f);
			INFO(runtime->LastError());
			REQUIRE(beat);
			CHECK(std::stoi(store.Get<gui::Label>(localCounter)->Text) == before + 1);
		}
	}
}

TEST_CASE("shared hosts run local GUI scripts only from the player's live copy", "[scripting][playergui]") {
	using namespace engine;
	scene::RegisterSceneClasses();
	gui::RegisterGuiClasses();
	(void)script::ScriptClass();
	for (const auto language : {script::Language::Luau, script::Language::JavaScript}) {
		ecs::Store store("playergui.template");
		scene::InstallServices(store);
		const auto starter = store.FindFirstRoot("StarterGui");
		const auto screen = store.CreateInstance(gui::GuiClass("ScreenGui"), "Persistent");
		store.SetParent(screen, starter);
		const auto label = store.CreateInstance(gui::GuiClass("TextLabel"), "Status");
		store.SetParent(label, screen);
		store.GetMutable<gui::Label>(label)->Text = "template";
		const auto path = language == script::Language::Luau ? "playergui-copy.luau" : "playergui-copy.js";
		const auto source = language == script::Language::Luau ? R"(
if script.Parent.Parent == game:GetService('StarterGui') then error('template LocalScript ran') end
script.Parent.Status.Text = 'live-copy'
)"
															   : R"(
if (script.Parent.Parent.Equals(game.GetService('StarterGui'))) throw new Error('template LocalScript ran');
script.Parent.FindFirstChild('Status').Text = 'live-copy';
)";
		const auto program = script::MakeScript(store, path, "Bootstrap", true);
		store.SetParent(program, screen);
		store.Set(program, script::Program{core::Name(path), source});
		const auto player = scene::AddPlayer(store, "Ada", true);
		REQUIRE(gui::ResetPlayerGui(store, player) == 1);
		script::RuntimeLimits limits;
		limits.Role = script::HostRole::OfBoth();
		const auto runtime = script::MakeRuntime(store, language, limits);
		const auto started = runtime->RunWorldScripts();
		INFO(runtime->LastError());
		CHECK(runtime->LastError().empty());
		CHECK(started == 1);
		CHECK(store.Get<gui::Label>(label)->Text == "template");
		const auto live = LocalGuiChild(store, store.FindFirstChild(player, "PlayerGui"), "Persistent");
		CHECK(store.Get<gui::Label>(store.FindFirstChild(live, "Status"))->Text == "live-copy");
	}
}

TEST_CASE("scripted respawns replace only resetting player GUI in both VMs", "[scripting][playergui]") {
	using namespace engine;
	scene::RegisterSceneClasses();
	gui::RegisterGuiClasses();
	for (const auto language : {script::Language::Luau, script::Language::JavaScript}) {
		INFO("scripted respawn language " << static_cast<unsigned>(language));
		ecs::Store store("playergui.scripted");
		scene::InstallServices(store);
		const auto starter = store.FindFirstRoot("StarterGui");
		REQUIRE(starter != ecs::NULL_ENTITY);
		for (const bool reset : {false, true}) {
			const auto screen =
				store.CreateInstance(gui::GuiClass("ScreenGui"), reset ? "Hud" : "Persistent");
			store.SetParent(screen, starter);
			store.GetMutable<gui::Layer>(screen)->ResetOnSpawn = reset;
			const auto label = store.CreateInstance(gui::GuiClass("TextLabel"), "Status");
			store.SetParent(label, screen);
			store.GetMutable<gui::Label>(label)->Text = "template";
		}
		const auto player = scene::AddPlayer(store, "Ada", true);
		const auto target = store.FindFirstChild(player, "PlayerGui");
		REQUIRE(target != ecs::NULL_ENTITY);
		const auto runtime = script::MakeRuntime(store, language);
		REQUIRE(runtime != nullptr);
		const auto spawn = language == script::Language::Luau
							   ? "game:GetService('Players').LocalPlayer:LoadCharacter()"
							   : "game.GetService('Players').LocalPlayer.LoadCharacter()";
		const bool entered = runtime->Run(spawn, "first-life");
		INFO(runtime->LastError());
		REQUIRE(entered);
		const auto persistent = LocalGuiChild(store, target, "Persistent");
		const auto firstHud = LocalGuiChild(store, target, "Hud");
		REQUIRE(persistent != ecs::NULL_ENTITY);
		REQUIRE(firstHud != ecs::NULL_ENTITY);
		const auto label = store.FindFirstChild(persistent, "Status");
		const auto runLocal = [&](std::string_view source, std::string_view name) {
			const std::string path =
				std::string(name) + (language == script::Language::Luau ? ".luau" : ".js");
			const auto program = script::MakeScript(store, path, name, true);
			store.SetParent(program, persistent);
			store.Set(program, script::Program{core::Name(path), std::string(source)});
			const bool ran = runtime->RunInstance(program);
			UNSCOPED_INFO(runtime->LastError());
			return ran;
		};
		const auto connect = language == script::Language::Luau ? R"(
local gui = game:GetService('Players').LocalPlayer.PlayerGui
gui.Persistent.Status.Text = 'local-state'
gui.Persistent.Status:GetPropertyChangedSignal('Text'):Connect(function()
    gui.Persistent:SetAttribute('callback', true)
end)
)"
																: R"(
let persistent = game.GetService('Players').LocalPlayer.PlayerGui.FindFirstChild('Persistent');
let status = persistent.FindFirstChild('Status');
status.Text = 'local-state';
status.GetPropertyChangedSignal('Text').Connect(function() {
    persistent.SetAttribute('callback', true);
});
)";
		REQUIRE(runLocal(connect, "persistent-ui-state"));
		store.FlushSignals();
		REQUIRE(runtime->Heartbeat(1.0f / 60.0f));
		REQUIRE(runtime->Run(spawn, "second-life"));
		REQUIRE(runtime->Heartbeat(1.0f / 60.0f));
		CHECK(LocalGuiChild(store, target, "Persistent") == persistent);
		CHECK(store.Get<gui::Label>(label)->Text == "local-state");
		CHECK_FALSE(store.Alive(firstHud));
		const auto secondHud = LocalGuiChild(store, target, "Hud");
		REQUIRE(secondHud != ecs::NULL_ENTITY);
		CHECK(secondHud != firstHud);
		CHECK(store.Get<gui::Label>(store.FindFirstChild(secondHud, "Status"))->Text == "template");
		const auto probe = language == script::Language::Luau ? R"(
local gui = game:GetService('Players').LocalPlayer.PlayerGui
gui.Persistent:SetAttribute('callback', false)
gui.Persistent.Status.Text = 'after-respawn'
)"
															  : R"(
let persistent = game.GetService('Players').LocalPlayer.PlayerGui.FindFirstChild('Persistent');
persistent.SetAttribute('callback', false);
persistent.FindFirstChild('Status').Text = 'after-respawn';
)";
		REQUIRE(runLocal(probe, "retained-callback"));
		store.FlushSignals();
		REQUIRE(runtime->Heartbeat(1.0f / 60.0f));
		const auto assertCallback =
			language == script::Language::Luau
				? "assert(game:GetService('Players').LocalPlayer.PlayerGui.Persistent:GetAttribute('callback'"
				  ") == true)"
				: "if "
				  "(game.GetService('Players').LocalPlayer.PlayerGui.FindFirstChild('Persistent')."
				  "GetAttribute('callback') "
				  "!== true) throw new Error('callback lost');";
		CHECK(store.Get<gui::Label>(label)->Text == "after-respawn");
		CHECK(store.Alive(persistent));
		const bool retainedCallback = runLocal(assertCallback, "retained-callback-result");
		INFO(runtime->LastError());
		REQUIRE(retainedCallback);
		const auto keepHud = language == script::Language::Luau ? R"(
local hud = game:GetService('Players').LocalPlayer.PlayerGui.Hud
hud.ResetOnSpawn = false
hud.Status.Text = 'client-retained-hud'
)"
																: R"(
let hud = game.GetService('Players').LocalPlayer.PlayerGui.FindFirstChild('Hud');
hud.ResetOnSpawn = false;
hud.FindFirstChild('Status').Text = 'client-retained-hud';
)";
		REQUIRE(runLocal(keepHud, "retain-client-hud"));
		REQUIRE(runtime->Run(spawn, "third-life"));
		REQUIRE(runtime->Heartbeat(1.0f / 60.0f));
		CHECK(store.Alive(secondHud));
		CHECK(LocalGuiChild(store, target, "Hud") == secondHud);
		const auto hudLabel = store.FindFirstChild(secondHud, "Status");
		CHECK(store.Get<gui::Label>(hudLabel)->Text == "client-retained-hud");
		const auto serverHud = script::FindInstanceChild(store, target, "Hud", false, false);
		REQUIRE(serverHud != ecs::NULL_ENTITY);
		CHECK_FALSE(ecs::IsClientLocalInstance(store, serverHud));
		store.GetMutable<gui::Label>(store.FindFirstChild(serverHud, "Status"))->Text = "server-updated-hud";
		(void)gui::RefreshPlayerGuiProjection(store, player, scene::CharacterOf(store, player));
		CHECK(store.Get<gui::Label>(hudLabel)->Text == "server-updated-hud");
		const auto destroyHud =
			language == script::Language::Luau
				? "game:GetService('Players').LocalPlayer.PlayerGui.Hud:Destroy()"
				: "game.GetService('Players').LocalPlayer.PlayerGui.FindFirstChild('Hud').Destroy()";
		REQUIRE(runLocal(destroyHud, "destroy-client-hud"));
		CHECK_FALSE(store.Alive(secondHud));
		(void)gui::RefreshPlayerGuiProjection(store, player, scene::CharacterOf(store, player));
		CHECK(LocalGuiChild(store, target, "Hud") == ecs::NULL_ENTITY);
		CHECK(store.Alive(serverHud));
	}
}
