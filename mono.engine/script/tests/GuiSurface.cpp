// The interface a UI author reaches for, driven end to end in both languages.
//
// **A gui signal that exists and never fires is the failure this module names
// twice**, and until v0.18 the shipped client never routed gui input at all — the
// router was constructed, read for its hover and never `Update`d. So this suite
// does not assert that a connection can be made: it stands a real world up, lays
// it out, compiles the draw list, drives `gui::Router` with a pointer, hands what
// comes out to `Runtime::DeliverGuiEvents` and beats. Every signal below is
// asserted to have *fired*, in Luau and in JavaScript, from one script.
//
// **The tween cases go through the barrier too**, because a `TweenPosition` that
// created a record and never advanced it would pass any check made on the tick it
// was called: `TweenTable::Advance` runs at the head of `Heartbeat`, so the
// assertion is what the property reads after some beats and what the completion
// callback wrote.
//
// The answer crosses as an attribute on the `Workspace` rather than as a local,
// for `ScriptCall.cpp`'s reason one door along: `Runtime::Run` reports whether a
// chunk ran and not what it evaluated to, and each chunk's globals are its own.

#include <engine/core/Name.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Compile.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Input.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/gui/Services.hpp>
#include <engine/gui/Typing.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.script.guisurface")
TEST_DEPENDS("engine.gui.input")
TEST_DEPENDS("engine.script.scriptcall")

using engine::core::Name;
using engine::core::UDim2;
using engine::core::Vector2;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;
using engine::script::Language;
using engine::script::MakeRuntime;
using engine::script::Runtime;

namespace {
	const std::vector<Language> LANGUAGES = {Language::Luau, Language::JavaScript};

	// A world with one occupant, whose own `PlayerGui` holds one `ScreenGui`.
	//
	// **Through `scene::AddPlayer` rather than a hand-parented container**, for
	// the reason `ScriptCall.cpp`'s `OneOccupant` gives: the four containers are
	// what that function *is*, and a `PlayerGui` made by hand is a row the join
	// pipeline never produced. It is also how a script reaches one —
	// `Players.LocalPlayer.PlayerGui` — where a root instance has no route at
	// all.
	//
	// **A player's own copy rather than the `StarterGui` template**, because that
	// is the container `GetGuiObjectsAtPosition` is called on and the one whose
	// scoping the answer depends on. `gui::Layout` draws from either.
	struct Interface {
		Store Data;
		engine::gui::CompileRequest Request;
		engine::gui::Compiled List;
		engine::gui::Router Route;

		Entity PlayerGui = NULL_ENTITY;
		Entity Screen = NULL_ENTITY;

		explicit Interface(const char *name) : Data(name) {
			engine::scene::EnsureClassTree();
			engine::scene::RegisterSceneComponents();
			engine::scene::RegisterSceneClasses();
			engine::gui::RegisterGuiClasses();
			REQUIRE(engine::scene::InstallServices(Data) != NULL_ENTITY);

			// **Both installers, which is what a host does.** `gui`'s services
			// cannot come from `scene`'s — the two modules may not link each
			// other — so `GuiService` is absent from a world that called only one
			// of them, and `game:GetService('GuiService')` then refuses.
			REQUIRE(engine::gui::InstallGuiServices(Data) != NULL_ENTITY);

			Request.Display.Width = 800.0f;
			Request.Display.Height = 600.0f;

			const Entity player = engine::scene::AddPlayer(Data, "Ada", true, 99);
			REQUIRE(player != NULL_ENTITY);

			PlayerGui = Data.FindFirstChild(player, engine::gui::PLAYER_GUI);
			REQUIRE(PlayerGui != NULL_ENTITY);

			Screen = Make("ScreenGui", "ScreenGui", PlayerGui);
		}

		// **Named at creation and never by writing `Name` afterwards.** That
		// property is a `core::Name` and writing one through the by-name setter
		// takes an interned value rather than a `string_view`; `CreateInstance`
		// takes the spelling and is the door every other caller uses.
		Entity Make(std::string_view klass, std::string_view name, Entity parent) {
			const Entity made = Data.CreateInstance(engine::gui::GuiClass(klass), name);
			REQUIRE(made != NULL_ENTITY);
			REQUIRE(Data.SetParent(made, parent));
			return made;
		}

		// A named element with a pixel rectangle, so a script can reach it by
		// name and a pointer can be aimed at it.
		Entity Box(std::string_view klass, std::string_view name, float x, float y, float w, float h) {
			const Entity made = Make(klass, name, Screen);

			engine::gui::Element element;
			element.Position = UDim2{0.0f, x, 0.0f, y};
			element.Size = UDim2{0.0f, w, 0.0f, h};
			Data.Set(made, element);
			return made;
		}

		void Compile() {
			Request.Hovered = Route.Hovered();
			Request.Pressed = Route.Pressed();
			List.Rebuild(Data, Request);
		}

		// One frame of the client's interface loop: compile, route, deliver, beat.
		//
		// **The whole chain and not a hand-built `GuiEvent`.** A test that
		// synthesised events would pass against a router nothing calls, which is
		// exactly the bug that shipped — so the pointer goes in and a signal
		// comes out.
		void Frame(Runtime &runtime, float x, float y, bool down) {
			Compile();

			engine::gui::Pointer pointer;
			pointer.Position = Vector2{x, y};
			pointer.Down = down;

			runtime.DeliverGuiEvents(Route.Update(Data, List.Commands(), pointer));

			Data.FlushSignals();
			INFO(runtime.LastError());
			REQUIRE(runtime.Heartbeat(1.0f / 60.0f));
		}

		// One frame of the client's keyboard half: type, and deliver what Return
		// released.
		//
		// **The composition `Client::Draw` makes, spelled out here for the reason
		// `Frame` spells out the pointer's.** `gui::Type` produces no event —
		// `Router::Update` is where events come from and no press happened — so
		// the caller owes the `FocusReleased` that a script's `FocusLost` is
		// listening for, with `Entered` set. A fixture that fired the signal
		// directly would pass against a runtime that ignored the flag.
		void Typed(Runtime &runtime, const engine::gui::Typing &typing) {
			const engine::gui::TypeResult result = engine::gui::Type(Data, typing);
			if (result.Released) {
				const engine::gui::GuiEvent released{
					engine::gui::EventKind::FocusReleased, result.Instance, {}, {}, true
				};
				runtime.DeliverGuiEvents(std::span<const engine::gui::GuiEvent>(&released, 1));
			}

			Data.FlushSignals();
			INFO(runtime.LastError());
			REQUIRE(runtime.Heartbeat(1.0f / 60.0f));
		}

		// What the chunk wrote, as one string.
		std::string Log() {
			engine::ecs::AttributeValue value;
			if (!engine::ecs::GetAttribute(Data, engine::scene::WorkspaceOf(Data), Name("log"), value)) {
				return {};
			}
			return value.String;
		}

		UDim2 PositionOf(Entity instance) {
			UDim2 value;
			REQUIRE(Data.GetProperty(instance, Name("Position"), &value, sizeof(value)));
			return value;
		}

		UDim2 SizeOf(Entity instance) {
			UDim2 value;
			REQUIRE(Data.GetProperty(instance, Name("Size"), &value, sizeof(value)));
			return value;
		}
	};

	// The two spellings of "append to the log", which is the whole of what the
	// scripts below differ by.
	std::string Note(Language language) {
		if (language == Language::Luau) {
			return "local function note(mark)\n"
				   "	workspace:SetAttribute('log', (workspace:GetAttribute('log') or '') .. mark)\n"
				   "end\n";
		}
		return "function note(mark) {\n"
			   "	workspace.SetAttribute('log', (workspace.GetAttribute('log') || '') + mark)\n"
			   "}\n";
	}
}

TEST_CASE("every gui signal a button has actually fires, in both languages", "[scripting][guisurface]") {
	// **The audit, made a check.** Six signals are offered on every instance and
	// one of them — `MouseButton1Click` — is new; what this asserts is that each
	// of them reaches a handler when a pointer does the thing it is named for,
	// rather than that it can be connected to.
	//
	// The pointer sequence is one interaction: arrive on the button, move within
	// it, press, release, then leave for the other one. That produces every kind
	// `gui::Router` can emit, in the order it emits them.
	for (const Language language : LANGUAGES) {
		INFO((language == Language::Luau ? "luau" : "javascript"));

		Interface world("guisurface.signals");
		const Entity button = world.Box("TextButton", "Button", 0.0f, 0.0f, 100.0f, 100.0f);
		const Entity other = world.Box("TextButton", "Other", 200.0f, 0.0f, 100.0f, 100.0f);
		REQUIRE(button != NULL_ENTITY);
		REQUIRE(other != NULL_ENTITY);

		const auto runtime = MakeRuntime(world.Data, language);
		REQUIRE(runtime != nullptr);

		const std::string connect =
			language == Language::Luau
				? Note(language) +
					  "local button = game:GetService('Players').LocalPlayer:FindFirstChild('Button', true)\n"
					  "button.MouseEnter:Connect(function() note('enter ') end)\n"
					  "button.MouseMoved:Connect(function() note('moved ') end)\n"
					  "button.InputBegan:Connect(function() note('began ') end)\n"
					  "button.InputEnded:Connect(function() note('ended ') end)\n"
					  "button.Activated:Connect(function() note('activated ') end)\n"
					  "button.MouseButton1Click:Connect(function() note('clicked ') end)\n"
					  "button.MouseLeave:Connect(function() note('leave ') end)\n"
				: Note(language) +
					  "let button = game.GetService('Players').LocalPlayer.FindFirstChild('Button', true)\n"
					  "button.MouseEnter.Connect(function () { note('enter ') })\n"
					  "button.MouseMoved.Connect(function () { note('moved ') })\n"
					  "button.InputBegan.Connect(function () { note('began ') })\n"
					  "button.InputEnded.Connect(function () { note('ended ') })\n"
					  "button.Activated.Connect(function () { note('activated ') })\n"
					  "button.MouseButton1Click.Connect(function () { note('clicked ') })\n"
					  "button.MouseLeave.Connect(function () { note('leave ') })\n";

		INFO(connect);
		const bool connected = runtime->Run(connect.c_str());
		INFO(runtime->LastError());
		REQUIRE(connected);

		world.Frame(*runtime, 20.0f, 20.0f, false);	 // arrive
		world.Frame(*runtime, 30.0f, 30.0f, false);	 // move within
		world.Frame(*runtime, 30.0f, 30.0f, true);	 // press
		world.Frame(*runtime, 30.0f, 30.0f, false);	 // release on it
		world.Frame(*runtime, 250.0f, 20.0f, false); // leave for the other

		// **`activated` and `clicked` both**, which is what makes
		// `MouseButton1Click` a second name for one event rather than a member
		// that exists and never fires — the failure this suite is named after.
		CHECK(world.Log() == "enter moved began ended activated clicked leave ");
	}
}

TEST_CASE("a script reads what is under a point, front to back", "[scripting][guisurface]") {
	// **Two overlapping objects, so the order is asserted and not merely the
	// count.** The `ZIndex` decides, and the second half of each case moves it —
	// a binding that answered tree order would pass the first and fail the
	// second.
	for (const Language language : LANGUAGES) {
		INFO((language == Language::Luau ? "luau" : "javascript"));

		Interface world("guisurface.hittest");
		const Entity under = world.Box("Frame", "Under", 0.0f, 0.0f, 200.0f, 200.0f);
		const Entity over = world.Box("Frame", "Over", 0.0f, 0.0f, 200.0f, 200.0f);
		world.Box("Frame", "Away", 400.0f, 400.0f, 50.0f, 50.0f);

		int32_t front = 5;
		REQUIRE(world.Data.SetProperty(over, Name("ZIndex"), &front, sizeof(front)));

		// **Compiled before the chunk runs**, because `Resolved::Order` is what
		// the answer is sorted by and the compile is what writes it. A world
		// nobody has drawn answers by depth, which is the fallback rather than
		// the contract.
		world.Compile();

		const auto runtime = MakeRuntime(world.Data, language);
		REQUIRE(runtime != nullptr);

		const std::string colon = language == Language::Luau ? ":" : ".";
		const std::string source =
			Note(language) +
			(language == Language::Luau
				 ? "local gui = game:GetService('Players').LocalPlayer:FindFirstChild('PlayerGui', true)\n"
				 : "let gui = game.GetService('Players').LocalPlayer.FindFirstChild('PlayerGui', true)\n") +
			(language == Language::Luau
				 ? "for _, found in ipairs(gui:GetGuiObjectsAtPosition(50, 50)) do note(found.Name .. ' ') "
				   "end\n"
				 : "for (const found of gui.GetGuiObjectsAtPosition(50, 50)) { note(found.Name + ' ') }\n") +
			(language == Language::Luau ? "note('| ' .. #gui:GetGuiObjectsAtPosition(700, 50))\n"
										: "note('| ' + gui.GetGuiObjectsAtPosition(700, 50).length)\n");

		INFO(source);
		const bool ran = runtime->Run(source.c_str());
		INFO(runtime->LastError());
		REQUIRE(ran);
		CHECK(world.Log() == "Over Under | 0");

		// The same point, the order swapped by the property that decides it.
		front = -5;
		REQUIRE(world.Data.SetProperty(over, Name("ZIndex"), &front, sizeof(front)));
		world.Compile();

		const std::string again =
			language == Language::Luau
				? "workspace:SetAttribute('log', '')\n"
				  "local gui = game:GetService('Players').LocalPlayer:FindFirstChild('PlayerGui', true)\n"
				  "for _, found in ipairs(gui:GetGuiObjectsAtPosition(50, 50)) do\n"
				  "	workspace:SetAttribute('log', workspace:GetAttribute('log') .. found.Name .. ' ')\n"
				  "end\n"
				: "workspace.SetAttribute('log', '')\n"
				  "let again = game.GetService('Players').LocalPlayer.FindFirstChild('PlayerGui', true)\n"
				  "for (const found of again.GetGuiObjectsAtPosition(50, 50)) {\n"
				  "	workspace.SetAttribute('log', workspace.GetAttribute('log') + found.Name + ' ')\n"
				  "}\n";

		const bool reran = runtime->Run(again.c_str());
		INFO(runtime->LastError());
		REQUIRE(reran);
		CHECK(world.Log() == "Under Over ");

		// Neither answer was ever the frame nowhere near the point, and the
		// handles name the two that were.
		CHECK(under != over);
	}
}

TEST_CASE("a tween on a GuiObject moves it and reports back", "[scripting][guisurface]") {
	// **Through `TweenService`'s table and never beside it**, which is what the
	// last assertion of each language pins: the motion happens at the head of the
	// barrier, on the fixed tick delta, so the property is part way at one beat
	// and arrived at the end — a method that assigned the goal outright would
	// read as arrived on the first.
	for (const Language language : LANGUAGES) {
		INFO((language == Language::Luau ? "luau" : "javascript"));

		Interface world("guisurface.tween");
		const Entity panel = world.Box("Frame", "Panel", 0.0f, 0.0f, 100.0f, 100.0f);

		const auto runtime = MakeRuntime(world.Data, language);
		REQUIRE(runtime != nullptr);

		// A tenth of a second is six beats at sixty hertz, and the case reads the
		// property at three and at eight.
		const std::string source =
			language == Language::Luau
				? Note(language) +
					  "local panel = game:GetService('Players').LocalPlayer:FindFirstChild('Panel', true)\n"
					  "note(tostring(panel:TweenSizeAndPosition(\n"
					  "	UDim2.new(0, 200, 0, 200), UDim2.new(0, 100, 0, 0),\n"
					  "	Enum.EasingDirection.In, Enum.EasingStyle.Linear, 0.1, false,\n"
					  "	function() note('done ') end)))\n"
					  "note(' ')\n"
				: Note(language) +
					  "let panel = game.GetService('Players').LocalPlayer.FindFirstChild('Panel', true)\n"
					  "note(String(panel.TweenSizeAndPosition(\n"
					  "	UDim2.new(0, 200, 0, 200), UDim2.new(0, 100, 0, 0),\n"
					  "	Enum.EasingDirection.In, Enum.EasingStyle.Linear, 0.1, false,\n"
					  "	function () { note('done ') })))\n"
					  "note(' ')\n";

		INFO(source);
		const bool ran = runtime->Run(source.c_str());
		INFO(runtime->LastError());
		REQUIRE(ran);
		CHECK(world.Log() == "true ");

		for (int beat = 0; beat < 3; beat++) {
			REQUIRE(runtime->Heartbeat(1.0f / 60.0f));
		}

		// Part way, which is what says the barrier is driving it.
		const UDim2 halfway = world.PositionOf(panel);
		CHECK(halfway.X.Offset > 0.0f);
		CHECK(halfway.X.Offset < 100.0f);
		CHECK(world.Log() == "true ");

		for (int beat = 0; beat < 5; beat++) {
			REQUIRE(runtime->Heartbeat(1.0f / 60.0f));
		}

		// Arrived, both goals written, and the callback called exactly once.
		CHECK(world.PositionOf(panel).X.Offset == 100.0f);
		CHECK(world.SizeOf(panel).X.Offset == 200.0f);
		CHECK(world.SizeOf(panel).Y.Offset == 200.0f);
		CHECK(world.Log() == "true done ");

		REQUIRE(runtime->Heartbeat(1.0f / 60.0f));
		CHECK(world.Log() == "true done ");
	}
}

TEST_CASE("override decides whether a second tween takes the object", "[scripting][guisurface]") {
	// **Roblox's flag, and the answer is what a caller reads.** A second
	// `TweenPosition` while the first is running is refused with `false` unless
	// the call says to take over — and a refusal must leave nothing behind, or
	// the cap reclaims records for calls that never ran.
	for (const Language language : LANGUAGES) {
		INFO((language == Language::Luau ? "luau" : "javascript"));

		Interface world("guisurface.override");
		const Entity panel = world.Box("Frame", "Panel", 0.0f, 0.0f, 100.0f, 100.0f);

		const auto runtime = MakeRuntime(world.Data, language);
		REQUIRE(runtime != nullptr);

		const std::string tween = language == Language::Luau
									  ? "panel:TweenPosition(UDim2.new(0, %GOAL%, 0, 0), "
										"Enum.EasingDirection.In, Enum.EasingStyle.Linear, 1, %OVER%)"
									  : "panel.TweenPosition(UDim2.new(0, %GOAL%, 0, 0), "
										"Enum.EasingDirection.In, Enum.EasingStyle.Linear, 1, %OVER%)";

		const auto call = [&](const char *goal, const char *over) {
			std::string body = tween;
			body.replace(body.find("%GOAL%"), 6, goal);
			body.replace(body.find("%OVER%"), 6, over);
			return body;
		};

		const std::string source =
			language == Language::Luau
				? Note(language) +
					  "local panel = game:GetService('Players').LocalPlayer:FindFirstChild('Panel', true)\n"
					  "note(tostring(" +
					  call("500", "false") + ") .. ' ')\n" + "note(tostring(" + call("900", "false") +
					  ") .. ' ')\n" + "note(tostring(" + call("300", "true") + ") .. ' ')\n"
				: Note(language) +
					  "let panel = game.GetService('Players').LocalPlayer.FindFirstChild('Panel', true)\n"
					  "note(String(" +
					  call("500", "false") + ") + ' ')\n" + "note(String(" + call("900", "false") +
					  ") + ' ')\n" + "note(String(" + call("300", "true") + ") + ' ')\n";

		INFO(source);
		const bool ran = runtime->Run(source.c_str());
		INFO(runtime->LastError());
		REQUIRE(ran);

		// Taken, refused, taken over.
		CHECK(world.Log() == "true false true ");

		// Sixty beats is one second, which is the whole of the surviving tween.
		for (int beat = 0; beat < 62; beat++) {
			REQUIRE(runtime->Heartbeat(1.0f / 60.0f));
		}

		// **The third goal and not the first**, which is what says the override
		// cancelled rather than ran beside it — two live tweens on one property
		// would leave whichever wrote last, and the refused one's 900 must appear
		// nowhere.
		CHECK(world.PositionOf(panel).X.Offset == 300.0f);
	}
}

TEST_CASE("a tween refuses a property with no midpoint, in both languages", "[scripting][guisurface]") {
	// **The refusal names the property**, for `TweenService:Create`'s reason: a
	// tween that runs for its whole duration and moves nothing reads as a broken
	// engine. `ScreenGui` has no `Position` at all — a `LayerCollector` is not a
	// `GuiObject` — which is the case a flat instance method table has to answer
	// honestly rather than silently.
	for (const Language language : LANGUAGES) {
		Interface world("guisurface.refusal");

		const auto runtime = MakeRuntime(world.Data, language);
		REQUIRE(runtime != nullptr);

		const std::string source =
			language == Language::Luau
				? "local screen = game:GetService('Players').LocalPlayer:FindFirstChild('ScreenGui', true)\n"
				  "screen:TweenPosition(UDim2.new(0, 10, 0, 10))\n"
				: "let screen = game.GetService('Players').LocalPlayer.FindFirstChild('ScreenGui', true)\n"
				  "screen.TweenPosition(UDim2.new(0, 10, 0, 10))\n";

		INFO(source);
		CHECK_FALSE(runtime->Run(source.c_str()));
		CHECK_FALSE(runtime->LastError().empty());
	}
}

TEST_CASE("the collector and the service carry what an author sets", "[scripting][guisurface]") {
	// **The audit half of this work, written as a check rather than a claim.**
	// `ScreenGui` was said to have `ResetOnSpawn`, `DisplayOrder`, `Enabled` and
	// `IgnoreGuiInset` and `GuiService` to have three settings; all seven are
	// declared properties, so what is worth asserting is not that they store a
	// value but that something *reads* one — a property nothing consults is the
	// hollow member every refusal in this module is about.
	//
	// `Enabled` is the one with teeth: switching it off has to empty the hit
	// test, because `Layout` clears `Rendered` under a disabled collector. The
	// other six are round trips, which is all a setting a host reads can be
	// asserted to do from inside a script.
	for (const Language language : LANGUAGES) {
		INFO((language == Language::Luau ? "luau" : "javascript"));

		Interface world("guisurface.settings");
		world.Box("Frame", "Panel", 0.0f, 0.0f, 200.0f, 200.0f);
		world.Compile();

		const auto runtime = MakeRuntime(world.Data, language);
		REQUIRE(runtime != nullptr);

		const std::string reach =
			language == Language::Luau
				? "local gui = game:GetService('Players').LocalPlayer:FindFirstChild('PlayerGui', true)\n"
				  "local screen = gui:FindFirstChildOfClass('ScreenGui')\n"
				  "local service = game:GetService('GuiService')\n"
				: "let gui = game.GetService('Players').LocalPlayer.FindFirstChild('PlayerGui', true)\n"
				  "let screen = gui.FindFirstChildOfClass('ScreenGui')\n"
				  "let service = game.GetService('GuiService')\n";

		const std::string source =
			Note(language) + reach +
			"screen.DisplayOrder = 7\n"
			"screen.ResetOnSpawn = false\n"
			"screen.IgnoreGuiInset = true\n"
			"service.AutoSelectGuiEnabled = false\n"
			"service.SelectedObject = screen\n" +
			(language == Language::Luau
				 ? "note(screen.DisplayOrder .. '/' .. tostring(screen.ResetOnSpawn) .. '/' .. "
				   "tostring(screen.IgnoreGuiInset) .. '/' .. tostring(screen.Enabled) .. '/' .. "
				   "tostring(service.AutoSelectGuiEnabled) .. '/' .. service.SelectedObject.Name)\n"
				 : "note(screen.DisplayOrder + '/' + String(screen.ResetOnSpawn) + '/' + "
				   "String(screen.IgnoreGuiInset) + '/' + String(screen.Enabled) + '/' + "
				   "String(service.AutoSelectGuiEnabled) + '/' + service.SelectedObject.Name)\n");

		INFO(source);
		const bool ran = runtime->Run(source.c_str());
		INFO(runtime->LastError());
		REQUIRE(ran);
		CHECK(world.Log() == "7/false/true/true/false/ScreenGui");

		// The panel is under the pointer while the collector is on.
		std::vector<Entity> found;
		REQUIRE(engine::gui::ElementsAt(world.Data, world.PlayerGui, Vector2{50.0f, 50.0f}, found) == 1);

		// **A fresh chunk rather than a second statement in the first**, because
		// each chunk runs on its own sandboxed thread in Luau and shares one
		// global scope in JavaScript — a second `let screen` is a `SyntaxError`
		// before a line of it runs.
		const std::string off =
			language == Language::Luau
				? "local gui = game:GetService('Players').LocalPlayer:FindFirstChild('PlayerGui', true)\n"
				  "gui:FindFirstChildOfClass('ScreenGui').Enabled = false\n"
				: "gui.FindFirstChildOfClass('ScreenGui').Enabled = false\n";

		const bool switched = runtime->Run(off.c_str());
		INFO(runtime->LastError());
		REQUIRE(switched);

		world.Compile();
		CHECK(engine::gui::ElementsAt(world.Data, world.PlayerGui, Vector2{50.0f, 50.0f}, found) == 0);
	}
}

TEST_CASE(
	"a text box's focus signals fire from a real pointer, in both languages", "[scripting][guisurface]"
) {
	// **The keyboard half of D00117, driven the way the pointer half is.** The
	// pointer goes in, `gui::Router` decides that a press landed on a `TextBox`,
	// and what comes out is `textBox.Focused` in a script — no hand-built event,
	// because a suite that synthesised one would pass against a router that never
	// took focus, which is exactly the bug the pointer half already shipped once.
	//
	// **`GetFocusedTextBox` is asked from inside the handlers**, so what is
	// asserted is that the world and the signals agree: a focus that fired an
	// event and stored nothing would log `Entry?nil` here.
	for (const Language language : LANGUAGES) {
		INFO((language == Language::Luau ? "luau" : "javascript"));

		Interface world("guisurface.focus");
		const Entity box = world.Box("TextBox", "Entry", 0.0f, 0.0f, 100.0f, 100.0f);
		const Entity button = world.Box("TextButton", "Button", 200.0f, 0.0f, 100.0f, 100.0f);
		REQUIRE(box != NULL_ENTITY);
		REQUIRE(button != NULL_ENTITY);

		const auto runtime = MakeRuntime(world.Data, language);
		REQUIRE(runtime != nullptr);

		const std::string connect =
			language == Language::Luau
				? Note(language) +
					  "local UIS = game:GetService('UserInputService')\n"
					  "local box = game:GetService('Players').LocalPlayer:FindFirstChild('Entry', true)\n"
					  "local function who() local it = UIS:GetFocusedTextBox() return it and it.Name or "
					  "'nil' end\n"
					  "box.Focused:Connect(function() note('focused=' .. who() .. ' ') end)\n"
					  "box.FocusLost:Connect(function(entered)\n"
					  "	note('lost=' .. tostring(entered) .. ',' .. who() .. ' ')\n"
					  "end)\n"
				: Note(language) +
					  "let UIS = game.GetService('UserInputService')\n"
					  "let box = game.GetService('Players').LocalPlayer.FindFirstChild('Entry', true)\n"
					  "function who() { let it = UIS.GetFocusedTextBox(); return it ? it.Name : 'nil' }\n"
					  "box.Focused.Connect(function () { note('focused=' + who() + ' ') })\n"
					  "box.FocusLost.Connect(function (entered) {\n"
					  "	note('lost=' + String(entered) + ',' + who() + ' ')\n"
					  "})\n";

		INFO(connect);
		const bool connected = runtime->Run(connect.c_str());
		INFO(runtime->LastError());
		REQUIRE(connected);

		world.Frame(*runtime, 50.0f, 50.0f, false); // arrive over the box
		world.Frame(*runtime, 50.0f, 50.0f, true);	// press: it takes the keyboard
		world.Frame(*runtime, 50.0f, 50.0f, false); // release: it keeps it
		CHECK(engine::gui::FocusedTextBox(world.Data) == box);

		world.Frame(*runtime, 250.0f, 50.0f, false); // move to the button
		world.Frame(*runtime, 250.0f, 50.0f, true);	 // press: it loses the keyboard
		CHECK(engine::gui::FocusedTextBox(world.Data) == NULL_ENTITY);

		// **`enterPressed` is false because a press took the keyboard away**,
		// which is the answer that makes the argument worth passing — the case
		// below is the other one.
		CHECK(world.Log() == "focused=Entry lost=false,nil ");
	}
}

TEST_CASE("typing reaches the box and Return says so, in both languages", "[scripting][guisurface]") {
	// **The rest of D00117, from the script's side.** A press takes the
	// keyboard, characters land in `Label::Text`, and Return releases the box
	// with Roblox's `enterPressed` finally answering something — which is what
	// tells a search field it was submitted rather than abandoned.
	//
	// **The text is read back through the property**, not off the component, so
	// what is asserted is the thing a script sees: one string, in the world,
	// where `gui::Type` put it.
	for (const Language language : LANGUAGES) {
		INFO((language == Language::Luau ? "luau" : "javascript"));

		Interface world("guisurface.typing");
		const Entity box = world.Box("TextBox", "Entry", 0.0f, 0.0f, 100.0f, 100.0f);
		REQUIRE(box != NULL_ENTITY);

		// Off, so the press does not empty the box before anything is typed —
		// the default is on, which is what a search field wants.
		engine::gui::Entry entry;
		entry.ClearTextOnFocus = false;
		world.Data.Set(box, entry);

		const auto runtime = MakeRuntime(world.Data, language);
		REQUIRE(runtime != nullptr);

		const std::string connect =
			language == Language::Luau
				? Note(language) +
					  "local box = game:GetService('Players').LocalPlayer:FindFirstChild('Entry', true)\n"
					  "box.FocusLost:Connect(function(entered)\n"
					  "	note('lost=' .. tostring(entered) .. ',' .. box.Text .. ' ')\n"
					  "end)\n"
				: Note(language) +
					  "let box = game.GetService('Players').LocalPlayer.FindFirstChild('Entry', true)\n"
					  "box.FocusLost.Connect(function (entered) {\n"
					  "	note('lost=' + String(entered) + ',' + box.Text + ' ')\n"
					  "})\n";

		INFO(connect);
		const bool connected = runtime->Run(connect.c_str());
		INFO(runtime->LastError());
		REQUIRE(connected);

		world.Frame(*runtime, 50.0f, 50.0f, false);
		world.Frame(*runtime, 50.0f, 50.0f, true);
		REQUIRE(engine::gui::FocusedTextBox(world.Data) == box);

		// Two frames of characters, because that is how they arrive: a frame's
		// worth at a time, appended at the caret.
		engine::gui::Typing typing;
		typing.Text = "Ad";
		world.Typed(*runtime, typing);

		// Four bytes of `é` and a Backspace over it, so what the box holds is a
		// character count rather than a byte count.
		typing.Text = "\xC3\xA9";
		world.Typed(*runtime, typing);

		typing.Text = {};
		typing.Backspace = true;
		world.Typed(*runtime, typing);
		CHECK(world.Data.Get<engine::gui::Label>(box)->Text == "Ad");

		typing.Backspace = false;
		typing.Text = "a";
		world.Typed(*runtime, typing);

		typing.Text = {};
		typing.Submit = true;
		world.Typed(*runtime, typing);

		CHECK(engine::gui::FocusedTextBox(world.Data) == NULL_ENTITY);
		CHECK(world.Log() == "lost=true,Ada ");
	}
}
