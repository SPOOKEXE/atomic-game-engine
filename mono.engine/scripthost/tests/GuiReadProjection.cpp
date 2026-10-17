// Both VMs read one physical GUI tree through the script's execution side.

#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Layout.hpp>
#include <engine/gui/PlayerGui.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/scripthost/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <string>

TEST_SUITE_ID("engine.scripthost.gui-read-projection")
TEST_DEPENDS("engine.script.instanceshim")

TEST_CASE("script tree methods read their own GUI view in both VMs", "[scripting][playergui][reads]") {
	using namespace engine;
	const auto language = GENERATE(script::Language::Luau, script::Language::JavaScript);
	const bool client = GENERATE(false, true);
	scene::RegisterSceneClasses();
	gui::RegisterGuiClasses();
	ecs::Store store("gui-read-projection");
	scene::InstallServices(store);
	const auto player = scene::AddPlayer(store, "Ada", true);
	const auto container = store.FindFirstChild(player, "PlayerGui");
	const auto source = store.CreateInstance(gui::GuiClass("ScreenGui"), "Hud");
	REQUIRE(store.SetParent(source, container));
	const auto label = store.CreateInstance(gui::GuiClass("TextLabel"), "Status");
	REQUIRE(store.SetParent(label, source));
	store.GetMutable<gui::Label>(label)->Text = "server";
	REQUIRE(gui::RefreshPlayerGuiProjection(store, player, {}) == 1);
	const auto copy = gui::FindPlayerGuiCopy(store, source);
	const auto localLabel = gui::FindPlayerGuiCopy(store, label);
	REQUIRE(store.SetInstanceName(copy, "LocalHud"));
	store.GetMutable<gui::Label>(localLabel)->Text = "client";
	gui::Screen screen;
	screen.Width = 200.0f;
	screen.Height = 200.0f;
	REQUIRE(gui::Layout(store, screen) > 0);
	script::RuntimeLimits limits;
	limits.Role = client ? script::HostRole::OfClient() : script::HostRole::OfServer();
	const auto runtime = script::MakeRuntime(store, language, limits);
	const std::string name = client ? "LocalHud" : "Hud";
	const std::string text = client ? "client" : "server";
	const std::string hidden = client ? "Hud" : "LocalHud";
	std::string checks;
	if (language == script::Language::Luau) {
		checks = "local gui = game:GetService('Players'):FindFirstChild('Ada'):FindFirstChild('PlayerGui')\n"
				 "local name, text, hidden = '" +
				 name + "', '" + text + "', '" + hidden + R"('
local children = gui:GetChildren()
assert(#children == 1 and children[1].Name == name)
local hits = gui:GetGuiObjectsAtPosition(50, 50)
assert(#hits == (text == 'client' and 1 or 0))
if #hits > 0 then assert(hits[1].Text == text) end
local hud = gui:FindFirstChild(name)
assert(hud and gui:FindFirstChild(hidden) == nil)
assert(gui:FindFirstChildOfClass('ScreenGui') == hud)
assert(gui:FindFirstChildWhichIsA('Instance', true) == hud)
local label = gui:FindFirstChild('Status', true)
assert(label.Text == text and label.Parent == hud)
assert(hud:FindFirstChild('Status') == label)
local descendants = gui:GetDescendants()
assert(#descendants == 2 and descendants[1] == hud and descendants[2] == label)
assert(label:FindFirstAncestor(name) == hud)
assert(label:FindFirstAncestorOfClass('ScreenGui') == hud)
assert(label:FindFirstAncestorWhichIsA('LayerCollector') == hud)
assert(label:IsDescendantOf(gui) and hud:IsAncestorOf(label))
assert(string.find(label:GetFullName(), name .. '.Status', 1, true))
)";
	} else {
		checks = "const gui = game.GetService('Players').FindFirstChild('Ada').FindFirstChild('PlayerGui');\n"
				 "const name = '" +
				 name + "', text = '" + text + "', hidden = '" + hidden + R"(';
function check(value) { if (!value) throw new Error('GUI read projection'); }
const children = gui.GetChildren();
check(children.length === 1 && children[0].Name === name);
const hits = gui.GetGuiObjectsAtPosition(50, 50);
check(hits.length === (text === 'client' ? 1 : 0));
if (hits.length > 0) check(hits[0].Text === text);
const hud = gui.FindFirstChild(name);
check(hud !== null && gui.FindFirstChild(hidden) === null);
check(gui.FindFirstChildOfClass('ScreenGui').Equals(hud));
check(gui.FindFirstChildWhichIsA('Instance', true).Equals(hud));
const label = gui.FindFirstChild('Status', true);
check(label.Text === text && label.Parent.Equals(hud));
check(hud.FindFirstChild('Status').Equals(label));
const descendants = gui.GetDescendants();
check(descendants.length === 2 && descendants[0].Equals(hud) && descendants[1].Equals(label));
check(label.FindFirstAncestor(name).Equals(hud));
check(label.FindFirstAncestorOfClass('ScreenGui').Equals(hud));
check(label.FindFirstAncestorWhichIsA('LayerCollector').Equals(hud));
check(label.IsDescendantOf(gui) && hud.IsAncestorOf(label));
check(label.GetFullName().endsWith(name + '.Status'));
)";
	}
	const bool accepted = runtime->Run(checks, "gui-read-side");
	INFO(static_cast<int>(language));
	INFO(client);
	INFO(runtime->LastError());
	REQUIRE(accepted);
}
