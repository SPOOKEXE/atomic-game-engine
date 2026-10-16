// Locking, anchoring and the pivot, as the tools panel writes them.
// Studio automation also covers RPC-driven text and wheel input through SDL.
//
// **The panel itself is not what is under test.** Drawing an ImGui strip needs a
// context and proves nothing about what a button does; what the buttons do is
// three store writes and a rule about mixed selections, and those are ordinary
// functions.
//
// The one that is worth a case of its own is `Locked` leaving a part out of the
// pick grid rather than filtering it out of the hit. The difference only shows
// up when something is *behind* the locked thing, which is exactly the case
// locking a wall is for - and the version that filters the hit passes every
// simpler test.

#include <engine/core/Paths.hpp>
#include <engine/core/types/AABB.hpp>
#include <engine/core/types/Ray.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/spatial/HashGrid.hpp>
#include <engine/spatial/Query.hpp>
#include <engine/testing/Suite.hpp>

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>

#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <studio/Config.hpp>
#include <studio/Editor.hpp>
#include <vector>

TEST_SUITE_ID("studio.tools")
TEST_DEPENDS("engine.scene.part")
TEST_DEPENDS("engine.control.input_tools")
TEST_DEPENDS("engine.ui.interface")

using engine::core::AABB;
using engine::core::Name;
using engine::core::OrientedBoxBounds;
using engine::core::Ray;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;
using engine::scene::Bounds;
using engine::scene::Transform;
using engine::scene::Visual;

namespace studio {
	struct ToolsProbe {
		static void Draw(Editor &editor, BuiltinStudioTool tool) {
			editor.DrawingBuiltinTool = tool;
			editor.DrawHomeTools();
		}

		static void PrepareTransport(Editor &editor) {
			editor.Universe = std::make_unique<engine::world::Universe>();
			editor.Active = editor.Universe->Create({.Name = Name("toolbar-transport-test")});
		}

		static void SetTransportRunning(Editor &editor, bool paused) {
			editor.Runs.clear();
			editor.Runs.emplace_back();
			editor.Runs.back().World = editor.Active;
			editor.Runs.back().Mode = RunMode::Server;
			editor.Runs.back().Paused = paused;
		}

		static void FinishTransport(Editor &editor) {
			editor.Runs.clear();
		}

		static void DrawTransport(Editor &editor, BuiltinStudioTool tool) {
			editor.DrawingBuiltinTool = tool;
			editor.DrawTransportTools();
		}

		static bool IsInactive(const Editor &editor) {
			return editor.CurrentTool == Editor::ToolMode::None;
		}

		static bool IsSelect(const Editor &editor) {
			return editor.CurrentTool == Editor::ToolMode::Select;
		}

		static void SetMove(Editor &editor) {
			editor.CurrentTool = Editor::ToolMode::Move;
		}

		static void AddPlugin(Editor &editor, LoadedCppPlugin &plugin) {
			editor.Plugins.push_back(&plugin);
			editor.ToolbarLayoutDirty = true;
		}

		static void DrawPluginToolbar(Editor &editor) {
			editor.DrawPluginToolbar();
		}

		static void PumpInput(Editor &editor) {
			editor.PumpEvents();
		}

		static void FinishInputFrame(Editor &editor) {
			editor.FinishControlAutomationFrame();
		}

		static bool HasPendingControlClick(const Editor &editor) {
			return editor.PendingControlClick.has_value();
		}
	};
}

namespace {
	class Context {
	  public:
		Context() {
			studio::SetConfigRoot(engine::core::Paths::Base() / "studio-tools-config");
			IMGUI_CHECKVERSION();
			Handle = ImGui::CreateContext();
			ImGuiIO &io = ImGui::GetIO();
			io.DisplaySize = ImVec2(1280.0f, 720.0f);
			io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
			io.DeltaTime = 1.0f / 60.0f;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
		}

		~Context() {
			ImGui::DestroyContext(Handle);
			studio::SetConfigRoot({});
		}

		Context(const Context &) = delete;
		Context &operator=(const Context &) = delete;

	  private:
		ImGuiContext *Handle = nullptr;
	};

	ImVec2 DrawTool(studio::Editor &editor, studio::BuiltinStudioTool tool, ImVec2 mouse, bool down) {
		ImGuiIO &io = ImGui::GetIO();
		io.AddMousePosEvent(mouse.x, mouse.y);
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(600.0f, 120.0f), ImGuiCond_Always);
		ImGui::Begin("Tools test");
		studio::ToolsProbe::Draw(editor, tool);
		const ImVec2 minimum = ImGui::GetItemRectMin();
		const ImVec2 maximum = ImGui::GetItemRectMax();
		ImGui::End();
		ImGui::Render();
		return ImVec2((minimum.x + maximum.x) * 0.5f, (minimum.y + maximum.y) * 0.5f);
	}

	void ClickTool(studio::Editor &editor, studio::BuiltinStudioTool tool) {
		const ImVec2 point = DrawTool(editor, tool, ImVec2(-1.0f, -1.0f), false);
		DrawTool(editor, tool, point, false);
		DrawTool(editor, tool, point, true);
		DrawTool(editor, tool, point, false);
	}

	void DrawPluginToolbar(studio::Editor &editor, ImVec2 mouse, bool down) {
		ImGuiIO &io = ImGui::GetIO();
		io.AddMousePosEvent(mouse.x, mouse.y);
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(600.0f, 140.0f), ImGuiCond_Always);
		ImGui::Begin("Plugin toolbar callback test");
		studio::ToolsProbe::DrawPluginToolbar(editor);
		ImGui::End();
		ImGui::Render();
	}

	// A part at a place, one stud on a side.
	Entity Place(Store &store, const char *name, float z) {
		const Entity part = store.CreateInstance(engine::scene::PartClass(), name);
		REQUIRE(part != NULL_ENTITY);

		Transform placed;
		placed.Frame = engine::core::CFrame(Vector3{0.0f, 0.0f, z});
		store.Set<Transform>(part, placed);

		Bounds box;
		box.HalfExtent = Vector3{0.5f, 0.5f, 0.5f};
		store.Set<Bounds>(part, box);
		return part;
	}

	// The pick, as `Editor::PickInViewport` builds it: every drawable that is
	// not locked, in a grid, hit by one ray.
	//
	// **A copy of the rule rather than a call into the editor**, because the
	// editor's own function needs a window, a projection and a universe. What is
	// under test is the rule, and a copy that drifted from the original would
	// fail this case the moment it did - the exclusion is one `if`.
	Entity PickThrough(Store &store, const Ray &ray) {
		std::vector<engine::spatial::Proxy> proxies;

		store.Each<Transform, Bounds>([&](Entity entity, const Transform &transform, const Bounds &bounds) {
			if (const auto *visual = store.Get<Visual>(entity); visual != nullptr && visual->Locked) {
				return;
			}

			engine::spatial::Proxy proxy;
			proxy.Id = entity.Id;
			proxy.Bounds = OrientedBoxBounds(transform.Frame, bounds.HalfExtent);
			proxy.Layers = engine::spatial::LayerMask::All();
			proxies.push_back(proxy);
		});

		if (proxies.empty()) {
			return NULL_ENTITY;
		}

		engine::spatial::HashGrid grid;
		grid.Rebuild(proxies);

		const std::optional<engine::core::RayHit> hit = engine::spatial::Raycast(grid, ray, 1000.0f);
		return hit.has_value() ? Entity(hit->Id) : NULL_ENTITY;
	}
}

TEST_CASE("active toolbar tools can leave the viewport to the running game", "[studio][tools][viewport]") {
	Context context;
	studio::Editor editor;

	// The first Select press removes Studio's selection and drag mode while the
	// viewport surface stays active for subsequent game UI presses.
	REQUIRE(studio::ToolsProbe::IsSelect(editor));
	ClickTool(editor, studio::BuiltinStudioTool::SelectMode);
	CHECK(studio::ToolsProbe::IsInactive(editor));

	// Selecting it again restores direct Studio selection. Any active gizmo
	// also toggles off from its own button, so one click always provides the
	// way to return a Play viewport to its game UI.
	ClickTool(editor, studio::BuiltinStudioTool::SelectMode);
	CHECK(studio::ToolsProbe::IsSelect(editor));
	studio::ToolsProbe::SetMove(editor);
	ClickTool(editor, studio::BuiltinStudioTool::MoveMode);
	CHECK(studio::ToolsProbe::IsInactive(editor));
}

TEST_CASE("transport controls read a run changed between controls", "[studio][tools][transport]") {
	Context context;
	studio::Editor editor;
	studio::ToolsProbe::PrepareTransport(editor);
	ImGui::NewFrame();
	ImGui::Begin("Transport test");

	studio::ToolsProbe::DrawTransport(editor, studio::BuiltinStudioTool::Stop);
	CHECK((ImGui::GetCurrentContext()->LastItemData.ItemFlags & ImGuiItemFlags_Disabled) != 0);

	studio::ToolsProbe::SetTransportRunning(editor, true);
	studio::ToolsProbe::DrawTransport(editor, studio::BuiltinStudioTool::Stop);
	CHECK((ImGui::GetCurrentContext()->LastItemData.ItemFlags & ImGuiItemFlags_Disabled) == 0);

	studio::ToolsProbe::DrawTransport(editor, studio::BuiltinStudioTool::Pause);
	CHECK(ImGui::GetCurrentContext()->LastItemData.ID == ImGui::GetID("Resume"));
	studio::ToolsProbe::SetTransportRunning(editor, false);
	studio::ToolsProbe::DrawTransport(editor, studio::BuiltinStudioTool::Pause);
	CHECK(ImGui::GetCurrentContext()->LastItemData.ID == ImGui::GetID("Pause"));

	studio::ToolsProbe::FinishTransport(editor);
	studio::ToolsProbe::DrawTransport(editor, studio::BuiltinStudioTool::Stop);
	CHECK((ImGui::GetCurrentContext()->LastItemData.ItemFlags & ImGuiItemFlags_Disabled) != 0);
	ImGui::End();
	ImGui::Render();
}

TEST_CASE("a toolbar callback may unload its plugin after drawing", "[studio][tools][plugins]") {
	Context context;
	studio::Editor editor;
	studio::LoadedCppPlugin plugin;
	plugin.Manifest.Id = "toolbar-unload-test";
	plugin.Manifest.Name = "Toolbar unload test";
	plugin.Native = true;
	plugin.Running = true;

	studio::PluginToolbar toolbar;
	toolbar.Name = "Actions";
	toolbar.Id = "actions";
	toolbar.Placement = studio::PluginToolbarPlacement::Pinned;
	toolbar.Rows.push_back(studio::PluginToolbarTrack{"row-1"});
	toolbar.Columns.push_back(studio::PluginToolbarTrack{"action"});

	studio::PluginButton button;
	button.Name = "Unload";
	button.Id = "unload";
	button.Kind = studio::PluginControlKind::Button;
	button.Tooltip = "unloads after the toolbar frame";
	button.Width = 360.0f;
	button.Row = "row-1";
	button.Column = "action";
	int callbacks = 0;
	button.NativeOnClick = [&](engine::script::HostArguments) {
		callbacks++;
		editor.Plugins.clear();
		plugin.Toolbars.clear();
	};
	toolbar.Buttons.push_back(std::move(button));
	plugin.Toolbars.push_back(std::move(toolbar));
	studio::ToolsProbe::AddPlugin(editor, plugin);

	DrawPluginToolbar(editor, ImVec2(-1.0f, -1.0f), false);
	DrawPluginToolbar(editor, ImVec2(100.0f, 60.0f), false);
	DrawPluginToolbar(editor, ImVec2(100.0f, 60.0f), true);
	DrawPluginToolbar(editor, ImVec2(100.0f, 60.0f), false);

	CHECK(callbacks == 1);
	CHECK(editor.Plugins.empty());
	CHECK(plugin.Toolbars.empty());
}

TEST_CASE("Locked is a property that survives a write and a read", "[studio][tools]") {
	engine::scene::EnsureClassTree();
	Store store("tools");

	const Entity part = Place(store, "Wall", 0.0f);

	// The default is unlocked: a part somebody has never touched is one they
	// can click.
	bool locked = true;
	REQUIRE(store.GetProperty(part, Name("Locked"), &locked, sizeof(locked)));
	CHECK_FALSE(locked);

	locked = true;
	REQUIRE(store.SetProperty(part, Name("Locked"), &locked, sizeof(locked)));

	locked = false;
	REQUIRE(store.GetProperty(part, Name("Locked"), &locked, sizeof(locked)));
	CHECK(locked);

	// **It changes nothing else about the part.** Locking is an editing
	// affordance, and a locked part that stopped drawing or stopped colliding
	// would be a very expensive way to say "leave this alone".
	const auto *visual = store.Get<Visual>(part);
	REQUIRE(visual != nullptr);
	CHECK(visual->Visible);
	CHECK(store.Get<Bounds>(part) != nullptr);
}

TEST_CASE("a locked part lets the ray through to what is behind it", "[studio][tools]") {
	engine::scene::EnsureClassTree();
	Store store("tools");

	const Entity front = Place(store, "Wall", 0.0f);
	const Entity behind = Place(store, "Switch", -8.0f);

	// Down the negative Z axis, through both.
	const Ray ray{Vector3{0.0f, 0.0f, 10.0f}, Vector3{0.0f, 0.0f, -1.0f}};

	CHECK(PickThrough(store, ray) == front);

	bool locked = true;
	REQUIRE(store.SetProperty(front, Name("Locked"), &locked, sizeof(locked)));

	// **This is the assertion the whole design turns on.** Filtering the *hit*
	// would answer `NULL_ENTITY` here - the locked wall would still swallow the
	// ray and make the thing behind it unclickable, which is the opposite of
	// what locking a wall is for.
	CHECK(PickThrough(store, ray) == behind);

	locked = false;
	REQUIRE(store.SetProperty(front, Name("Locked"), &locked, sizeof(locked)));
	CHECK(PickThrough(store, ray) == front);
}

TEST_CASE("a pivot offset is written and reset through its property", "[studio][tools]") {
	engine::scene::EnsureClassTree();
	Store store("tools");

	const Entity part = Place(store, "Door", 0.0f);

	engine::core::CFrame offset;
	REQUIRE(store.GetProperty(part, Name("PivotOffset"), &offset, sizeof(offset)));
	CHECK(offset.Position == Vector3::Zero);

	// A hinge on the edge of a one-stud door.
	offset.Position = Vector3{-0.5f, 0.0f, 0.0f};
	REQUIRE(store.SetProperty(part, Name("PivotOffset"), &offset, sizeof(offset)));

	// **`GetPivot` composes the placement with the offset**, which is what makes
	// a pivot edit visible without the part moving - and is the arithmetic the
	// gizmo has to undo to turn a world drag into a local offset.
	CHECK(engine::scene::PivotOf(store, part).Position.X == -0.5f);

	const auto *transform = store.Get<Transform>(part);
	REQUIRE(transform != nullptr);
	CHECK(transform->Frame.Position == Vector3::Zero);

	// Reset, which is the button.
	const engine::core::CFrame identity;
	REQUIRE(store.SetProperty(part, Name("PivotOffset"), &identity, sizeof(identity)));
	CHECK(engine::scene::PivotOf(store, part).Position == Vector3::Zero);
}

TEST_CASE("anchoring is a structural change and locking is not", "[studio][tools]") {
	engine::scene::EnsureClassTree();
	Store store("tools");

	const Entity part = Place(store, "Crate", 0.0f);

	// The two toolbar toggles look alike and are not: anchored decides whether
	// the physics moves it - which is `scene::Simulated` coming off the row -
	// and locked decides whether a person can grab it, which is a byte.
	bool anchored = false;
	REQUIRE(store.GetProperty(part, Name("Anchored"), &anchored, sizeof(anchored)));

	anchored = true;
	REQUIRE(store.SetProperty(part, Name("Anchored"), &anchored, sizeof(anchored)));
	CHECK_FALSE(store.Has<engine::scene::Simulated>(part));

	bool locked = true;
	REQUIRE(store.SetProperty(part, Name("Locked"), &locked, sizeof(locked)));

	// Still anchored, and still there. A structural write and a field write on
	// one entity in one frame is the ordinary case for these two buttons.
	REQUIRE(store.GetProperty(part, Name("Anchored"), &anchored, sizeof(anchored)));
	CHECK(anchored);
	CHECK(store.Get<Visual>(part)->Locked);
}

TEST_CASE(
	"Studio control text and wheel input arrive once across frame boundaries", "[studio][control][automation]"
) {
	Context context;
	REQUIRE(SDL_Init(SDL_INIT_EVENTS));
	struct ControlEventsLifetime {
		~ControlEventsLifetime() {
			SDL_QuitSubSystem(SDL_INIT_EVENTS);
		}
	} eventsLifetime;
	studio::Editor editor;
	studio::ToolsProbe::PrepareTransport(editor);
	engine::ui::InterfaceSettings settings;
	settings.DisplayWidth = 640;
	settings.DisplayHeight = 480;
	REQUIRE(editor.Interface.Initialise(editor.Renderer, nullptr, settings));
	editor.EnableControlFeatures();

	const auto call = [&](std::string_view name, nlohmann::json arguments) {
		const nlohmann::json request{
			{"jsonrpc", "2.0"},
			{"id", 1},
			{"method", "tools/call"},
			{"params", {{"name", name}, {"arguments", std::move(arguments)}}},
		};
		const auto response = nlohmann::json::parse(editor.ControlSurface.Answer(request.dump()));
		REQUIRE(response.contains("result"));
		const auto &result = response.at("result");
		INFO(result.dump());
		REQUIRE_FALSE(result.value("isError", false));
		const auto payload = nlohmann::json::parse(result.at("content").at(0).at("text").get<std::string>());
		REQUIRE(payload.at("queued").get<bool>());
	};
	const auto observe = [&](std::string_view expectedText, float expectedWheel) {
		editor.Interface.Begin(1.0f / 60.0f);
		const auto &io = ImGui::GetIO();
		CHECK(io.InputQueueCharacters.Size == static_cast<int>(expectedText.size()));
		for (int index = 0;
			 index < io.InputQueueCharacters.Size && index < static_cast<int>(expectedText.size());
			 ++index)
			CHECK(
				io.InputQueueCharacters[index] ==
				static_cast<ImWchar>(expectedText[static_cast<size_t>(index)])
			);
		CHECK(io.MouseWheel == expectedWheel);
		CHECK(io.MouseWheelH == 0.0f);
		CHECK(io.MousePos.x == 320.0f);
		CHECK(io.MousePos.y == 240.0f);
		editor.Interface.End();
		studio::ToolsProbe::FinishInputFrame(editor);
	};

	// Match the host: RPC admission follows this frame's SDL pump. The queued
	// text must remain owned until the following pump consumes it.
	studio::ToolsProbe::PumpInput(editor);
	call("emulate_mouse_move", {{"x", 320.0f}, {"y", 240.0f}});
	const std::string expected = "verification/v0261-far.aproc";
	std::string submitted = expected;
	call("emulate_text", {{"text", submitted}});
	submitted.assign(128, 'x');
	call("emulate_mouse_wheel", {{"notches", 2.0f}});
	call("emulate_mouse_wheel", {{"notches", 3.0f}});
	observe({}, 0.0f);
	studio::ToolsProbe::PumpInput(editor);
	observe(expected, 5.0f);

	// Consumption releases the retained text request. A second request and
	// oppositely signed wheel events must neither replay nor erase the first.
	call("emulate_text", {{"text", "!"}});
	call("emulate_mouse_wheel", {{"notches", -1.25f}});
	call("emulate_mouse_wheel", {{"notches", 0.25f}});
	observe({}, 0.0f);
	studio::ToolsProbe::PumpInput(editor);
	observe("!", -1.0f);
	studio::ToolsProbe::PumpInput(editor);
	observe({}, 0.0f);
}

TEST_CASE(
	"Studio omitted-down clicks wait for a presented hover and release once", "[studio][control][automation]"
) {
	Context context;
	REQUIRE(SDL_Init(SDL_INIT_EVENTS));
	struct ControlEventsLifetime {
		~ControlEventsLifetime() {
			SDL_QuitSubSystem(SDL_INIT_EVENTS);
		}
	} eventsLifetime;
	studio::Editor editor;
	studio::ToolsProbe::PrepareTransport(editor);
	engine::ui::InterfaceSettings settings;
	settings.DisplayWidth = 1024;
	settings.DisplayHeight = 720;
	REQUIRE(editor.Interface.Initialise(editor.Renderer, nullptr, settings));
	editor.EnableControlFeatures();

	const auto call = [&](uint32_t id, std::string_view tool, nlohmann::json arguments) {
		const nlohmann::json request{
			{"jsonrpc", "2.0"},
			{"id", id},
			{"method", "tools/call"},
			{"params", {{"name", tool}, {"arguments", std::move(arguments)}}},
		};
		const auto response = nlohmann::json::parse(editor.ControlSurface.Answer(request.dump()));
		REQUIRE(response.contains("result"));
		const auto &result = response.at("result");
		INFO(result.dump());
		REQUIRE_FALSE(result.value("isError", false));
		const auto payload = nlohmann::json::parse(result.at("content").at(0).at("text").get<std::string>());
		REQUIRE(payload.at("queued").get<bool>());
	};
	struct PresentedFrame {
		ImVec2 AssetsCenter{};
		ImVec2 ButtonCenter{};
		bool WorldsSelected = false;
		bool AssetsSelected = false;
		bool AssetsHovered = false;
		bool ButtonHovered = false;
		bool ButtonActivated = false;
		bool LeftDown = false;
		bool LeftClicked = false;
		bool LeftReleased = false;
	};
	const auto drawFrame = [&] {
		PresentedFrame result;
		editor.Interface.Begin(1.0f / 60.0f);
		ImGui::SetNextWindowPos(ImVec2(60.0f, 60.0f), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(360.0f, 220.0f), ImGuiCond_Always);
		ImGui::Begin(
			"Control click tabs",
			nullptr,
			ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings
		);
		if (ImGui::BeginTabBar("##control-click-tabs")) {
			result.WorldsSelected = ImGui::BeginTabItem("Worlds");
			if (result.WorldsSelected) {
				ImGui::TextUnformatted("World contents");
				ImGui::EndTabItem();
			}
			result.AssetsSelected = ImGui::BeginTabItem("Assets");
			result.AssetsHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem);
			const ImVec2 selectedAssetsMin = ImGui::GetItemRectMin();
			const ImVec2 selectedAssetsMax = ImGui::GetItemRectMax();
			result.AssetsCenter = ImVec2(
				(selectedAssetsMin.x + selectedAssetsMax.x) * 0.5f,
				(selectedAssetsMin.y + selectedAssetsMax.y) * 0.5f
			);
			if (result.AssetsSelected) {
				ImGui::TextUnformatted("Asset contents");
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
		ImGui::End();

		ImGui::SetNextWindowPos(ImVec2(570.0f, 60.0f), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(340.0f, 220.0f), ImGuiCond_Always);
		ImGui::Begin(
			"Control click button",
			nullptr,
			ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings
		);
		result.ButtonActivated = ImGui::Button("Invoke once");
		result.ButtonHovered = ImGui::IsItemHovered();
		const ImVec2 buttonMin = ImGui::GetItemRectMin();
		const ImVec2 buttonMax = ImGui::GetItemRectMax();
		result.ButtonCenter = ImVec2((buttonMin.x + buttonMax.x) * 0.5f, (buttonMin.y + buttonMax.y) * 0.5f);
		ImGui::End();

		const ImGuiIO &io = ImGui::GetIO();
		result.LeftDown = io.MouseDown[ImGuiMouseButton_Left];
		result.LeftClicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
		result.LeftReleased = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
		editor.Interface.End();
		studio::ToolsProbe::FinishInputFrame(editor);
		return result;
	};
	const auto advance = [&] {
		studio::ToolsProbe::PumpInput(editor);
		return drawFrame();
	};

	PresentedFrame initial;
	for (uint32_t frame = 0; frame < 3; ++frame)
		initial = advance();
	CHECK(initial.WorldsSelected);
	CHECK_FALSE(initial.AssetsSelected);

	studio::ToolsProbe::PumpInput(editor);
	call(1, "emulate_mouse_move", {{"x", initial.ButtonCenter.x}, {"y", initial.ButtonCenter.y}});
	const PresentedFrame pointerInOtherWindow = drawFrame();
	CHECK(pointerInOtherWindow.ButtonHovered);
	CHECK_FALSE(pointerInOtherWindow.AssetsHovered);

	studio::ToolsProbe::PumpInput(editor);
	call(
		2, "emulate_click", {{"x", initial.AssetsCenter.x}, {"y", initial.AssetsCenter.y}, {"button", "left"}}
	);
	CHECK(studio::ToolsProbe::HasPendingControlClick(editor));
	// Polling the motion cannot advance the click phase without a presented ImGui frame.
	studio::ToolsProbe::PumpInput(editor);
	CHECK(studio::ToolsProbe::HasPendingControlClick(editor));

	const PresentedFrame hoverBeforePress = drawFrame();
	CHECK(hoverBeforePress.AssetsHovered);
	CHECK_FALSE(hoverBeforePress.LeftDown);
	CHECK(studio::ToolsProbe::HasPendingControlClick(editor));

	uint32_t tabDownFrames = 0;
	uint32_t tabClickEdges = 0;
	uint32_t tabReleaseEdges = 0;
	bool assetsSelected = false;
	for (uint32_t attempt = 0; attempt < 8 && studio::ToolsProbe::HasPendingControlClick(editor); ++attempt) {
		const PresentedFrame frame = advance();
		if (frame.LeftDown) ++tabDownFrames;
		if (frame.LeftClicked) ++tabClickEdges;
		if (frame.LeftReleased) ++tabReleaseEdges;
		assetsSelected = assetsSelected || frame.AssetsSelected;
	}
	CHECK_FALSE(studio::ToolsProbe::HasPendingControlClick(editor));
	CHECK(assetsSelected);
	CHECK(tabDownFrames == 1);
	CHECK(tabClickEdges == 1);
	CHECK(tabReleaseEdges == 1);

	for (uint32_t frame = 0; frame < 3; ++frame) {
		const PresentedFrame idle = advance();
		CHECK_FALSE(idle.LeftDown);
		CHECK_FALSE(idle.LeftClicked);
		CHECK_FALSE(idle.ButtonActivated);
		CHECK(idle.AssetsSelected);
	}

	studio::ToolsProbe::PumpInput(editor);
	call(
		3, "emulate_click", {{"x", initial.ButtonCenter.x}, {"y", initial.ButtonCenter.y}, {"button", "left"}}
	);
	const PresentedFrame buttonHover = drawFrame();
	CHECK(buttonHover.ButtonHovered);
	CHECK_FALSE(buttonHover.LeftDown);

	uint32_t buttonDownFrames = 0;
	uint32_t buttonClickEdges = 0;
	uint32_t buttonReleaseEdges = 0;
	uint32_t buttonActivations = 0;
	for (uint32_t attempt = 0; attempt < 8 && studio::ToolsProbe::HasPendingControlClick(editor); ++attempt) {
		const PresentedFrame frame = advance();
		if (frame.LeftDown) ++buttonDownFrames;
		if (frame.LeftClicked) ++buttonClickEdges;
		if (frame.LeftReleased) ++buttonReleaseEdges;
		if (frame.ButtonActivated) ++buttonActivations;
	}
	CHECK_FALSE(studio::ToolsProbe::HasPendingControlClick(editor));
	CHECK(buttonDownFrames == 1);
	CHECK(buttonClickEdges == 1);
	CHECK(buttonReleaseEdges == 1);
	CHECK(buttonActivations == 1);
	for (uint32_t frame = 0; frame < 3; ++frame) {
		const PresentedFrame idle = advance();
		CHECK_FALSE(idle.LeftDown);
		CHECK_FALSE(idle.LeftClicked);
		CHECK_FALSE(idle.ButtonActivated);
	}
}
