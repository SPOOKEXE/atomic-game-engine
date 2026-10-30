// Locking, anchoring and the pivot, as the tools panel writes them.
// Studio automation also covers RPC-driven text and wheel input through SDL.
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
#include <engine/game/Game.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Shaders.hpp>
#include <engine/script/Instances.hpp>
#include <engine/spatial/HashGrid.hpp>
#include <engine/spatial/Query.hpp>
#include <engine/testing/Suite.hpp>

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>

#include <fstream>
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
		static void PrepareScriptTabs(Editor &editor, size_t count, int active) {
			editor.Scripts.resize(count);
			for (size_t i = 0; i < count; ++i)
				editor.Scripts[i].Text = std::to_string(i);
			editor.ActiveScript = active;
		}

		static void CloseScriptTab(Editor &editor, size_t index) {
			editor.CloseScriptTab(index);
		}

		static int ActiveScript(const Editor &editor) {
			return editor.ActiveScript;
		}

		static const std::string &ActiveScriptText(const Editor &editor) {
			return editor.Scripts[static_cast<size_t>(editor.ActiveScript)].Text;
		}

		static void DrawGutter(Editor &editor, const OpenScript &tab) {
			editor.DrawScriptGutter(tab, 0);
		}

		static void ToggleBreakpoint(Editor &editor, Name path, int line) {
			editor.ToggleBreakpoint(path, line);
		}

		static size_t BreakpointCount(const Editor &editor) {
			return editor.Breakpoints.Breakpoints().size();
		}

		static void PrepareRecentMenu(Editor &editor, const std::filesystem::path &path) {
			editor.Universe = std::make_unique<engine::world::Universe>();
			editor.Recent.Paths.push_back(path);
		}

		static void DrawMenuBar(Editor &editor) {
			editor.DrawMenuBar();
		}

		static bool RecentEmpty(const Editor &editor) {
			return editor.Recent.Paths.empty();
		}

		static void AddRecent(Editor &editor, const std::filesystem::path &path) {
			editor.Recent.Paths.push_back(path);
		}

		static const std::filesystem::path &GamePath(const Editor &editor) {
			return editor.GamePath;
		}

		static void PrepareUnsavedGame(Editor &editor) {
			editor.Universe = std::make_unique<engine::world::Universe>();
			editor.Universe->Create({.Name = Name("unsaved-scene")});
			editor.Modified = true;
		}

		static bool HasUnsavedScene(const Editor &editor) {
			for (auto world : editor.Universe->Worlds()) {
				if (editor.Universe->NameOf(world) == Name("unsaved-scene")) return true;
			}
			return false;
		}

		static bool PendingDiscard(const Editor &editor) {
			return editor.PendingDestructiveAction != Editor::DestructiveAction::None;
		}

		static const std::filesystem::path &PendingPath(const Editor &editor) {
			return editor.PendingDestructivePath;
		}

		static void DrawDialogs(Editor &editor) {
			editor.DrawDialogs();
		}

		static void DrawUiAuthoring(Editor &editor) {
			editor.ShowUiAuthoring = true;
			editor.DrawUiAuthoring();
		}

		static void RequestNew(Editor &editor) {
			editor.RequestDestructiveAction(Editor::DestructiveAction::NewGame);
		}

		static void RequestOpen(Editor &editor, const std::filesystem::path &path) {
			editor.RequestDestructiveAction(Editor::DestructiveAction::OpenGame, path);
		}

		static void RequestExit(Editor &editor) {
			editor.QuitRequested = true;
		}

		static bool QuitRequested(const Editor &editor) {
			return editor.QuitRequested;
		}

		static void RunNewOperator(Editor &editor) {
			editor.RegisterOperators();
			editor.Operators.Run(Action::NewGame);
		}

		static void DrawShortcuts(Editor &editor, bool modalAtFrameStart = false) {
			editor.DrawShortcuts(modalAtFrameStart);
		}

		static bool UniversePermissionPending(const Editor &editor) {
			return editor.AskingUniverseLoadPermissions;
		}

		static void AcceptUniverse(Editor &editor) {
			editor.AcceptUniverseOpen();
		}

		static void Discard(Editor &editor) {
			editor.ExecuteDestructiveAction();
		}

		static void ScriptOnlyDirty(Editor &editor) {
			editor.Modified = false;
			editor.Scripts.emplace_back().Modified = true;
		}

		static void SetModified(Editor &editor, bool modified) {
			editor.Modified = modified;
		}

		static void StartSaveAs(Editor &editor, const std::filesystem::path &path) {
			editor.PathBuffer = path.string();
			editor.AskingSaveAs = true;
		}

		static void StartOpen(Editor &editor, const std::filesystem::path &path) {
			editor.PathBuffer = path.string();
			editor.AskingOpen = true;
		}

		static void SetRunning(Editor &editor, bool running) {
			editor.Running = running;
		}

		static void SetWindow(Editor &editor, SDL_Window *window) {
			editor.Window = window;
		}

		static bool GameModified(const Editor &editor) {
			return editor.Modified;
		}

		static void AddUnsavedScript(Editor &editor) {
			auto &script = editor.Scripts.emplace_back();
			script.Modified = true;
			script.Text = "unsaved source";
		}

		static void PrepareDeletedScript(Editor &editor) {
			engine::scene::RegisterSceneClasses();
			const auto world = editor.Universe->Worlds().front();
			Entity script;
			editor.Universe->Enter(world, [&](Store &store) {
				script = store.CreateInstance(engine::script::ScriptClass(), "Script");
			});
			editor.OpenScriptTab(world, script);
			editor.Scripts.front().Text = "unsaved source";
			editor.Scripts.front().Modified = true;
			editor.ShowScripts = true;
			editor.Universe->Enter(world, [&](Store &store) { store.DestroyInstance(script); });
		}

		static void PrepareLiveUnwritableScript(Editor &editor) {
			engine::scene::RegisterSceneClasses();
			const auto world = editor.Universe->Worlds().front();
			Entity script;
			editor.Universe->Enter(world, [&](Store &store) {
				script = store.CreateInstance(engine::scene::ShaderScriptClass(), "Shader");
			});
			editor.OpenScriptTab(world, script);
			editor.Scripts.front().Text = "unsaved source";
			editor.Scripts.front().Modified = true;
			editor.ShowScripts = true;
			editor.Universe->Enter(world, [&](Store &store) {
				store.Remove<engine::scene::ShaderSource>(script);
			});
		}

		static bool SaveGame(Editor &editor, const std::filesystem::path &path) {
			return editor.SaveGame(path);
		}

		static bool ExportWorld(Editor &editor, const std::filesystem::path &path) {
			return editor.ExportWorldFile(editor.Universe->Worlds().front(), path);
		}

		static bool BeginServer(Editor &editor) {
			return editor.BeginRun(editor.Universe->Worlds().front(), RunMode::Server);
		}

		static void
		ConfigureWorldPanels(Editor &editor, engine::world::WorldId first, engine::world::WorldId second) {
			editor.Active = first;
			editor.AddExtraViewport();
			editor.Extras.front().World = second;
		}

		static void CaptureWorldInput(Editor &editor, size_t index, engine::world::WorldId world) {
			using namespace engine;
			editor.Universe->Enter(world, [&](Store &store) {
				const Entity workspace = scene::InstallServices(store);
				const Entity screen = store.CreateInstance(gui::GuiClass("ScreenGui"), "Screen");
				store.SetParent(screen, store.FindFirstRoot("StarterGui"));
				const Entity button = store.CreateInstance(gui::GuiClass("TextButton"), "Button");
				store.SetParent(button, screen);
				gui::Element element;
				element.Size = core::UDim2{0, 100, 0, 100};
				store.Set(button, element);
				gui::CompileRequest request;
				request.Display.Width = 300;
				request.Display.Height = 220;
				editor.GuiLists[index].Rebuild(store, request);
				gui::Pointer pointer;
				pointer.Position = {20, 20};
				pointer.Down = true;
				editor.GuiRouters[index].Update(store, editor.GuiLists[index].Commands(), pointer);
				REQUIRE(editor.GuiRouters[index].Pressed() == button);
				const Entity part = store.CreateInstance(scene::PartClass(), "Part");
				store.SetParent(part, workspace);
				store.Set(part, scene::Transform{core::CFrame(core::Vector3::Zero)});
				store.Set(part, scene::PreviousTransform{core::CFrame(core::Vector3::Zero)});
				store.Set(part, scene::Bounds{core::Vector3{1, 1, 1}});
				const Entity adornment = store.CreateInstance(gui::GuiClass("SelectionBox"), "Box");
				store.SetParent(adornment, part);
				store.Set(adornment, gui::AdornmentInteraction{.Enabled = true});
				render::AdornmentPointer ray;
				ray.Ray = core::Ray{{1.01f, 1.01f, 5}, {0, 0, -1}};
				ray.PrimaryDown = true;
				REQUIRE_FALSE(editor.AdornmentRouters[index].Update(store, ray, 0.05f).empty());
			});
			editor.GuiRouterWorlds[index] = world;
			editor.Overlays[index].Presented = true;
			editor.Overlays[index].PresentedWorld = world;
			if (index == 0) {
				editor.FollowCamera = Entity{42};
				editor.GuiDragging.World = world;
				editor.PendingGuiEdit.World = world;
				editor.PendingGuiEdit.Wanted = true;
				editor.SurfaceGesture.World = world;
				editor.SurfaceGesture.Active = true;
				editor.PendingPick.Viewport = index;
				editor.PendingPick.Wanted = true;
			} else
				editor.Extras[index - 1].Follow = Entity{42};
		}

		static bool WorldGesturesCleared(const Editor &editor) {
			return !editor.GuiDragging.World.IsValid() && !editor.PendingGuiEdit.Wanted &&
				   !editor.SurfaceGesture.Active && !editor.PendingPick.Wanted;
		}

		static Entity Pressed(const Editor &editor, size_t index) {
			return editor.GuiRouters[index].Pressed();
		}

		static bool Presented(const Editor &editor, size_t index) {
			return editor.Overlays[index].Presented;
		}

		static bool Following(const Editor &editor, size_t index) {
			return (index == 0 ? editor.FollowCamera : editor.Extras[index - 1].Follow) != NULL_ENTITY;
		}

		static size_t ReleaseAdornment(Editor &editor, size_t index, engine::world::WorldId world) {
			size_t count = 0;
			editor.Universe->Enter(world, [&](Store &store) {
				engine::render::AdornmentPointer ray;
				ray.Ray = engine::core::Ray{{1.01f, 1.01f, 5}, {0, 0, -1}};
				count = editor.AdornmentRouters[index].Update(store, ray, 0.05f).size();
			});
			return count;
		}

		static bool RenameWorld(Editor &editor, engine::world::WorldId world) {
			return editor.RenameWorld(world, Name("renamed"));
		}

		static void StopWorld(Editor &editor, engine::world::WorldId world) {
			editor.EndRun(world);
		}

		static void OpenRuntimeOnlyScript(Editor &editor, engine::world::WorldId world) {
			Entity script;
			editor.Universe->Enter(world, [&](Store &store) {
				script = store.CreateInstance(engine::script::ScriptClass(), "RuntimeOnly");
				engine::script::SetSourcePath(store, script, Name("Scripts/runtime-only.luau"));
			});
			editor.OpenScriptTab(world, script);
			editor.Scripts.back().Text = "unsaved source";
			editor.Scripts.back().Modified = true;
		}

		static bool ScriptDetached(const Editor &editor) {
			return !editor.Scripts.empty() && editor.Scripts.back().Instance == NULL_ENTITY &&
				   editor.Scripts.back().Modified && editor.Scripts.back().Text == "unsaved source";
		}

		static bool SaveFirstScript(Editor &editor) {
			return editor.SaveScriptTab(editor.Scripts.front());
		}

		static void AddPinnedViewport(Editor &editor, engine::world::WorldId world) {
			const size_t index = editor.AddExtraViewport();
			editor.Extras[index - 1].World = world;
		}

		static engine::world::WorldId PinnedWorld(const Editor &editor, size_t index) {
			return editor.Extras[index].World;
		}

		static void RemoveWorld(Editor &editor, engine::world::WorldId world) {
			editor.RemoveWorld(world);
		}

		static void DrawScripts(Editor &editor) {
			editor.DrawScripts();
		}

		static bool HasUnsavedScript(const Editor &editor) {
			return editor.Scripts.size() == 1 && editor.Scripts.front().Modified &&
				   editor.Scripts.front().Text == "unsaved source";
		}

		static const engine::world::Universe *UniverseAddress(const Editor &editor) {
			return editor.Universe.get();
		}

		static bool MainViewportShowsFirstWorld(const Editor &editor) {
			return !editor.Universe->Worlds().empty() && editor.Active == editor.Universe->Worlds().front();
		}

		static void PrepareUndoHistory(Editor &editor) {
			engine::scene::RegisterSceneClasses();
			editor.Commands = std::make_unique<CommandLog>(*editor.Universe);
			const auto world = editor.Universe->Worlds().front();
			editor.Universe->Enter(world, [&](Store &store) {
				const auto workspace = engine::scene::InstallServices(store);
				const auto part = store.CreateInstance(engine::scene::PartClass(), "Undoable");
				store.SetParent(part, workspace);
				editor.Commands->RecordCreate(store, world, part, "Insert");
			});
		}

		static bool CanUndo(const Editor &editor) {
			return editor.Commands->CanUndo();
		}

		static bool CanRedo(const Editor &editor) {
			return editor.Commands->CanRedo();
		}

		static bool Undo(Editor &editor) {
			return editor.Commands->Undo();
		}

		static bool Redo(Editor &editor) {
			return editor.Commands->Redo();
		}

		static std::optional<std::string> BeginRecording(Editor &editor) {
			return editor.Commands->TryBeginRecording("open edit");
		}

		static bool CancelRecording(Editor &editor, std::string_view id) {
			return editor.Commands->FinishRecording(id, FinishOperation::Cancel);
		}

		static void RecordWorldPart(Editor &editor, engine::world::WorldId world, const char *description) {
			editor.Universe->Enter(world, [&](Store &store) {
				const auto workspace = engine::scene::InstallServices(store);
				const auto part = store.CreateInstance(engine::scene::PartClass(), description);
				store.SetParent(part, workspace);
				editor.Commands->RecordCreate(store, world, part, description);
			});
		}

		static std::string_view NextUndo(const Editor &editor) {
			return editor.Commands->NextUndo();
		}

		static std::string_view NextRedo(const Editor &editor) {
			return editor.Commands->NextRedo();
		}

		static EditId Track(Editor &editor, engine::world::WorldId world, Entity instance) {
			return editor.Commands->Track(world, instance);
		}

		static Entity Resolve(const Editor &editor, EditId id) {
			return editor.Commands->Resolve(id);
		}

		static void HideOutput(Editor &editor) {
			editor.ShowOutput = false;
			editor.ShowWarnings = false;
		}

		static bool OutputVisible(const Editor &editor) {
			return editor.ShowOutput;
		}

		static bool OutputWillFocus(const Editor &editor) {
			return editor.FocusOutput > 0 && editor.ShowWarnings;
		}

		static void SetActiveWorld(Editor &editor, engine::world::WorldId world) {
			editor.Active = world;
		}

		static void SelectWorldWorkspace(Editor &editor, engine::world::WorldId world) {
			editor.SelectionWorld = world;
			editor.Universe->Enter(world, [&](Store &store) {
				editor.Selection = {engine::scene::WorkspaceOf(store)};
			});
		}

		static bool HasWorldSelection(const Editor &editor, engine::world::WorldId world) {
			return editor.SelectionWorld == world && editor.Selection.size() == 1 &&
				   editor.Selection.front() != NULL_ENTITY;
		}

		static engine::world::WorldId SelectionWorld(const Editor &editor) {
			return editor.SelectionWorld;
		}

		static void QueueWorldActivation(Editor &editor, engine::world::WorldId world) {
			editor.PendingActivate = world;
			editor.PendingRunWorld = world;
			editor.PendingDuplicateWorld = world;
			editor.PendingTeleport = world;
			editor.PendingWorldState = world;
			editor.PendingRenameWorld = world;
			editor.PendingRenameTo = "stale";
			editor.RenamingWorld = world;
		}

		static bool HasQueuedWorldAction(const Editor &editor) {
			return editor.PendingActivate.IsValid() || editor.PendingRunWorld.IsValid() ||
				   editor.PendingDuplicateWorld.IsValid() || editor.PendingTeleport.IsValid() ||
				   editor.PendingWorldState.IsValid() || editor.PendingRenameWorld.IsValid() ||
				   editor.RenamingWorld.IsValid() || !editor.PendingRenameTo.empty();
		}

		static void ApplyWorldActions(Editor &editor) {
			editor.ApplyPendingWorldActions();
		}

		static void LoadStudioPlugins(Editor &editor) {
			editor.LoadPlugins();
		}

		static bool HasStudioPlugins(const Editor &editor) {
			return !editor.Plugins.empty();
		}

		static bool RunCommand(Editor &editor) {
			return editor.RunCommand("local value = 1");
		}

		static engine::world::WorldId CommandWorld(const Editor &editor) {
			return editor.CommandWorld;
		}

		static bool StartServer(Editor &editor, engine::world::WorldId world) {
			return editor.BeginRun(world, RunMode::Server);
		}

		static const void *ServerRuntime(const Editor &editor, engine::world::WorldId world) {
			const auto *run = editor.RunOf(world);
			return run == nullptr ? nullptr : run->Runtime.get();
		}

		static const void *PlaytestPlugins(const Editor &editor, engine::world::WorldId world) {
			for (const auto &set : editor.PlaytestPluginSets) {
				if (set->World == world) return set.get();
			}
			return nullptr;
		}

		static uint64_t PresentationSession(const Editor &editor) {
			return editor.Universe->LocalPresentationDirectory().Session;
		}

		static void PrepareOldViewportState(Editor &editor) {
			const auto world = editor.Universe->Worlds().front();
			editor.AddExtraViewport();
			editor.Extras.front().World = world;
			editor.Extras.front().Follow = Entity{42};
			editor.FollowCamera = Entity{43};
			editor.PendingFrame.push_back(world);
			editor.PendingPick.Wanted = true;
			editor.PendingGuiEdit.Wanted = true;
			editor.GuiDragging.World = world;
			editor.Overlays.front().Presented = true;
			editor.Overlays.front().PresentedWorld = world;
			editor.GuiRouterWorlds.front() = world;
		}

		static bool OldViewportStateCleared(const Editor &editor) {
			return !editor.Extras.front().World.IsValid() && editor.Extras.front().Follow == NULL_ENTITY &&
				   editor.FollowCamera == NULL_ENTITY && editor.PendingFrame.empty() &&
				   !editor.PendingPick.Wanted && !editor.PendingGuiEdit.Wanted &&
				   !editor.GuiDragging.World.IsValid() && !editor.Overlays.front().Presented &&
				   !editor.GuiRouterWorlds.front().IsValid();
		}

		static bool IsRunning(const Editor &editor) {
			return editor.Running;
		}

		static void DrawViewMenu(Editor &editor) {
			editor.DrawViewMenu();
		}

		static bool UiAuthoringOpen(const Editor &editor) {
			return editor.ShowUiAuthoring;
		}

		static bool ProceduralOpen(const Editor &editor) {
			return editor.ShowProcedural;
		}

		static void OpenExtraViewport(Editor &editor) {
			editor.AddExtraViewport();
		}

		static void AllocateExtraViewports(Editor &editor, size_t count) {
			editor.ResizeViewports(count);
		}

		static bool ExtraViewportOpen(const Editor &editor, size_t index = 0) {
			return editor.Extras[index].Open;
		}

		static void CloseExtraViewport(Editor &editor) {
			editor.Extras.front().Open = false;
		}

		static void InvalidateClientViewport(Editor &editor) {
			const engine::world::WorldId replica{7};
			editor.Extras.front().World = replica;
			editor.CloseClientViewports(replica);
		}

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
	struct TemporaryConfig {
		std::filesystem::path Root;

		TemporaryConfig() {
			const auto base = std::filesystem::temp_directory_path();
			for (size_t index = 0;; ++index) {
				Root = base / ("studio-recent-menu-" + std::to_string(SDL_GetTicksNS()) + "-" +
							   std::to_string(index));
				if (std::filesystem::create_directory(Root)) break;
			}
			studio::SetConfigRoot(Root);
		}

		~TemporaryConfig() {
			std::error_code error;
			std::filesystem::remove_all(Root, error);
		}
	};

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

TEST_CASE("view menu shows and hides all panels", "[studio][tools][view_menu]") {
	Context context;
	studio::Editor editor;
	ImGui::GetIO().DisplaySize = ImVec2(1280.0f, 2400.0f);
	const auto draw = [&](ImVec2 mouse, bool down) {
		ImGuiIO &io = ImGui::GetIO();
		io.AddMousePosEvent(mouse.x, mouse.y);
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(400.0f, 2300.0f), ImGuiCond_Always);
		ImGui::Begin("View menu test");
		studio::ToolsProbe::DrawViewMenu(editor);
		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 max = ImGui::GetItemRectMax();
		const ImVec2 reset((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
		ImGui::End();
		ImGui::Render();
		return reset;
	};
	const auto click = [&](int rowsAboveReset) {
		const ImVec2 reset = draw(ImVec2(-1.0f, -1.0f), false);
		const ImVec2 point(reset.x, reset.y - rowsAboveReset * ImGui::GetTextLineHeightWithSpacing());
		draw(point, false);
		draw(point, true);
		draw(point, false);
	};
	click(2);
	CHECK(studio::ToolsProbe::UiAuthoringOpen(editor));
	CHECK(studio::ToolsProbe::ProceduralOpen(editor));
	studio::ToolsProbe::AllocateExtraViewports(editor, 2);
	studio::ToolsProbe::OpenExtraViewport(editor);
	REQUIRE(studio::ToolsProbe::ExtraViewportOpen(editor));
	click(1);
	CHECK_FALSE(studio::ToolsProbe::UiAuthoringOpen(editor));
	CHECK_FALSE(studio::ToolsProbe::ProceduralOpen(editor));
	CHECK_FALSE(studio::ToolsProbe::ExtraViewportOpen(editor));
	click(1);
	click(2);
	CHECK(studio::ToolsProbe::ExtraViewportOpen(editor));
	CHECK_FALSE(studio::ToolsProbe::ExtraViewportOpen(editor, 1));
	click(1);
	studio::ToolsProbe::OpenExtraViewport(editor);
	studio::ToolsProbe::CloseExtraViewport(editor);
	click(2);
	CHECK_FALSE(studio::ToolsProbe::ExtraViewportOpen(editor));
	studio::ToolsProbe::OpenExtraViewport(editor);
	click(1);
	studio::ToolsProbe::InvalidateClientViewport(editor);
	click(2);
	CHECK_FALSE(studio::ToolsProbe::ExtraViewportOpen(editor));
}

TEST_CASE("new game asks before discarding unsaved work", "[studio][tools][unsaved]") {
	Context context;
	TemporaryConfig config;
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto frame = [&](ImVec2 mouse, bool down) {
		auto &io = ImGui::GetIO();
		io.AddMousePosEvent(mouse.x, mouse.y);
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
		ImGui::NewFrame();
		studio::ToolsProbe::DrawMenuBar(editor);
		studio::ToolsProbe::DrawDialogs(editor);
		ImGui::Render();
	};
	const ImVec2 file(15.0f, 10.0f);
	frame(file, false);
	frame(file, true);
	frame(file, false);
	ImGuiWindow *fileMenu = ImGui::FindWindowByName("File###Menu_00");
	REQUIRE(fileMenu != nullptr);
	const ImVec2 item(
		fileMenu->Pos.x + 30.0f,
		fileMenu->Pos.y + ImGui::GetStyle().WindowPadding.y + ImGui::GetTextLineHeightWithSpacing() * 0.5f
	);
	frame(item, false);
	frame(item, true);
	frame(item, false);
	CHECK(studio::ToolsProbe::HasUnsavedScene(editor));
	CHECK(studio::ToolsProbe::PendingDiscard(editor));
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
	frame(item, false);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
	frame(item, false);
	CHECK_FALSE(studio::ToolsProbe::PendingDiscard(editor));
	CHECK(studio::ToolsProbe::HasUnsavedScene(editor));
	studio::ToolsProbe::RequestNew(editor);
	frame(item, false);
	const ImGuiWindow *popup = ImGui::FindWindowByName("Unsaved Changes");
	REQUIRE(popup != nullptr);
	const ImVec2 discard(
		popup->DC.CursorStartPos.x + 25.0f,
		popup->DC.CursorStartPos.y + ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeight() * 0.5f
	);
	frame(discard, false);
	frame(discard, true);
	frame(discard, false);
	CHECK_FALSE(studio::ToolsProbe::PendingDiscard(editor));
	CHECK_FALSE(studio::ToolsProbe::HasUnsavedScene(editor));
}

TEST_CASE("unsaved script buffers protect replacement and exit", "[studio][tools][unsaved]") {
	Context context;
	TemporaryConfig config;
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::ScriptOnlyDirty(editor);
	studio::ToolsProbe::SetRunning(editor, true);
	studio::ToolsProbe::RequestNew(editor);
	CHECK(studio::ToolsProbe::PendingDiscard(editor));
	CHECK(studio::ToolsProbe::HasUnsavedScene(editor));
	studio::ToolsProbe::RequestExit(editor);
	CHECK(studio::ToolsProbe::IsRunning(editor));
	studio::ToolsProbe::SetRunning(editor, false);
	studio::ToolsProbe::Discard(editor);
	CHECK_FALSE(studio::ToolsProbe::HasUnsavedScene(editor));
}

TEST_CASE("unsaved replacement retains its target and exit waits for discard", "[studio][tools][unsaved]") {
	Context context;
	TemporaryConfig config;
	const auto path = config.Root / "target.agame";
	engine::world::Universe source;
	source.Create({.Name = Name("target-scene")});
	std::string error;
	REQUIRE(engine::game::SaveGame(source, Name("Target"), path, error));
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto *originalUniverse = studio::ToolsProbe::UniverseAddress(editor);
	studio::ToolsProbe::RequestOpen(editor, path);
	CHECK(studio::ToolsProbe::HasUnsavedScene(editor));
	CHECK(studio::ToolsProbe::PendingPath(editor) == path);
	studio::ToolsProbe::RequestExit(editor);
	CHECK(studio::ToolsProbe::PendingPath(editor) == path);
	studio::ToolsProbe::Discard(editor);
	CHECK(studio::ToolsProbe::GamePath(editor) == path);
	CHECK(studio::ToolsProbe::UniverseAddress(editor) == originalUniverse);
	CHECK(studio::ToolsProbe::PresentationSession(editor) == 1);
	CHECK_FALSE(studio::ToolsProbe::HasUnsavedScene(editor));
	studio::ToolsProbe::ScriptOnlyDirty(editor);
	studio::ToolsProbe::SetRunning(editor, true);
	studio::ToolsProbe::RequestExit(editor);
	CHECK(studio::ToolsProbe::IsRunning(editor));
	ImGui::NewFrame();
	studio::ToolsProbe::DrawDialogs(editor);
	ImGui::Render();
	CHECK(studio::ToolsProbe::PendingDiscard(editor));
	studio::ToolsProbe::Discard(editor);
	CHECK_FALSE(studio::ToolsProbe::IsRunning(editor));
}

TEST_CASE("unsaved universe open keeps the permission step", "[studio][tools][unsaved]") {
	Context context;
	TemporaryConfig config;
	const auto path = config.Root / "target.auniverse";
	engine::world::Universe source;
	source.Create({.Name = Name("target-scene")});
	std::string error;
	REQUIRE(engine::game::SaveUniverse(source, Name("Target"), {}, path, {}, error));
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::RequestOpen(editor, path);
	CHECK(studio::ToolsProbe::PendingDiscard(editor));
	CHECK_FALSE(studio::ToolsProbe::UniversePermissionPending(editor));
	studio::ToolsProbe::Discard(editor);
	CHECK(studio::ToolsProbe::UniversePermissionPending(editor));
	CHECK(studio::ToolsProbe::HasUnsavedScene(editor));
	studio::ToolsProbe::AcceptUniverse(editor);
	CHECK(studio::ToolsProbe::GamePath(editor) == path);
	CHECK_FALSE(studio::ToolsProbe::HasUnsavedScene(editor));
}

TEST_CASE("new game palette and shortcut ask before replacing work", "[studio][tools][unsaved]") {
	Context context;
	TemporaryConfig config;
	studio::Editor palette;
	studio::ToolsProbe::PrepareUnsavedGame(palette);
	studio::ToolsProbe::RunNewOperator(palette);
	CHECK(studio::ToolsProbe::PendingDiscard(palette));
	CHECK(studio::ToolsProbe::HasUnsavedScene(palette));

	studio::Editor shortcut;
	studio::ToolsProbe::PrepareUnsavedGame(shortcut);
	const auto original = studio::Keybinds::Of(studio::Action::NewGame);
	studio::Keybinds::Set(studio::Action::NewGame, studio::Chord{ImGuiKey_F24});
	ImGui::GetIO().AddKeyEvent(ImGuiKey_F24, true);
	ImGui::NewFrame();
	studio::ToolsProbe::DrawShortcuts(shortcut);
	ImGui::Render();
	studio::Keybinds::Set(studio::Action::NewGame, original);
	CHECK(studio::ToolsProbe::PendingDiscard(shortcut));
	CHECK(studio::ToolsProbe::HasUnsavedScene(shortcut));
}

TEST_CASE("global shortcuts do not act behind universe permission modal", "[studio][tools][unsaved]") {
	Context context;
	TemporaryConfig config;
	const auto path = config.Root / "target.auniverse";
	engine::world::Universe source;
	source.Create({.Name = Name("target-scene")});
	std::string error;
	REQUIRE(engine::game::SaveUniverse(source, Name("Target"), {}, path, {}, error));
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::SetModified(editor, false);
	studio::ToolsProbe::RequestOpen(editor, path);
	REQUIRE(studio::ToolsProbe::UniversePermissionPending(editor));
	studio::ToolsProbe::SetModified(editor, true);
	const auto original = studio::Keybinds::Of(studio::Action::NewGame);
	studio::Keybinds::Set(studio::Action::NewGame, studio::Chord{ImGuiKey_F24});
	ImGui::NewFrame();
	studio::ToolsProbe::DrawDialogs(editor);
	ImGui::Render();
	ImGui::GetIO().AddKeyEvent(ImGuiKey_F24, true);
	ImGui::NewFrame();
	studio::ToolsProbe::DrawDialogs(editor);
	studio::ToolsProbe::DrawShortcuts(editor);
	ImGui::Render();
	CHECK_FALSE(studio::ToolsProbe::PendingDiscard(editor));
	CHECK(studio::ToolsProbe::UniversePermissionPending(editor));
	CHECK(studio::ToolsProbe::HasUnsavedScene(editor));
	ImGui::GetIO().AddKeyEvent(ImGuiKey_F24, false);
	ImGui::NewFrame();
	studio::ToolsProbe::DrawDialogs(editor);
	ImGui::Render();
	ImGui::GetIO().AddKeyEvent(ImGuiKey_F24, true);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
	ImGui::NewFrame();
	const bool modalAtFrameStart = ImGui::GetTopMostPopupModal() != nullptr;
	studio::ToolsProbe::DrawDialogs(editor);
	studio::ToolsProbe::DrawShortcuts(editor, modalAtFrameStart);
	ImGui::Render();
	studio::Keybinds::Set(studio::Action::NewGame, original);
	CHECK_FALSE(studio::ToolsProbe::UniversePermissionPending(editor));
	CHECK_FALSE(studio::ToolsProbe::PendingDiscard(editor));
}

TEST_CASE("window quit asks before discarding work", "[studio][tools][unsaved]") {
	Context context;
	TemporaryConfig config;
	REQUIRE(SDL_Init(SDL_INIT_EVENTS));
	struct EventsLifetime {
		~EventsLifetime() {
			SDL_QuitSubSystem(SDL_INIT_EVENTS);
		}
	} events;
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::SetRunning(editor, true);
	SDL_Event event{};
	event.type = SDL_EVENT_QUIT;
	REQUIRE(SDL_PushEvent(&event));
	studio::ToolsProbe::PumpInput(editor);
	ImGui::NewFrame();
	studio::ToolsProbe::DrawDialogs(editor);
	ImGui::Render();
	CHECK(studio::ToolsProbe::PendingDiscard(editor));
	CHECK(studio::ToolsProbe::IsRunning(editor));
	studio::ToolsProbe::SetRunning(editor, false);
}

TEST_CASE("main window close asks before discarding work", "[studio][tools][unsaved]") {
	Context context;
	TemporaryConfig config;
	struct DummyVideo {
		std::string Previous;
		bool HadPrevious = false;
		DummyVideo() {
			if (const char *previous = SDL_GetHint(SDL_HINT_VIDEO_DRIVER)) {
				Previous = previous;
				HadPrevious = true;
			}
			SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
		}
		~DummyVideo() {
			if (HadPrevious)
				SDL_SetHint(SDL_HINT_VIDEO_DRIVER, Previous.c_str());
			else
				SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
		}
	} video;
	REQUIRE(SDL_Init(SDL_INIT_VIDEO));
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::SetRunning(editor, true);
	SDL_Window *window = SDL_CreateWindow("Studio close test", 100, 100, SDL_WINDOW_HIDDEN);
	REQUIRE(window != nullptr);
	studio::ToolsProbe::SetWindow(editor, window);
	SDL_Event event{};
	event.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
	event.window.windowID = SDL_GetWindowID(window);
	REQUIRE(SDL_PushEvent(&event));
	studio::ToolsProbe::PumpInput(editor);
	ImGui::NewFrame();
	studio::ToolsProbe::DrawDialogs(editor);
	ImGui::Render();
	CHECK(studio::ToolsProbe::PendingDiscard(editor));
	CHECK(studio::ToolsProbe::IsRunning(editor));
	CHECK(studio::ToolsProbe::HasUnsavedScene(editor));
	studio::ToolsProbe::SetRunning(editor, false);
}

TEST_CASE("missing replacement file keeps the unsaved game", "[studio][tools][unsaved]") {
	Context context;
	TemporaryConfig config;
	const auto path = config.Root / "missing.agame";
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::RequestOpen(editor, path);
	REQUIRE(studio::ToolsProbe::PendingDiscard(editor));
	studio::ToolsProbe::Discard(editor);
	CHECK(studio::ToolsProbe::HasUnsavedScene(editor));
	CHECK(studio::ToolsProbe::GameModified(editor));
}

TEST_CASE("malformed replacement preserves unsaved world and script", "[studio][tools][unsaved]") {
	Context context;
	TemporaryConfig config;
	const auto path = config.Root / "broken.agame";
	std::ofstream(
		path
	) << R"(<Game format="1" name="Broken"><World name="Bad"><Item class="NoSuchClassExists" /></World></Game>)";
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::AddUnsavedScript(editor);
	studio::ToolsProbe::PrepareOldViewportState(editor);
	const auto *universe = studio::ToolsProbe::UniverseAddress(editor);
	studio::ToolsProbe::RequestOpen(editor, path);
	REQUIRE(studio::ToolsProbe::PendingDiscard(editor));
	studio::ToolsProbe::Discard(editor);
	CHECK(studio::ToolsProbe::UniverseAddress(editor) == universe);
	CHECK(studio::ToolsProbe::HasUnsavedScene(editor));
	CHECK(studio::ToolsProbe::GameModified(editor));
	CHECK(studio::ToolsProbe::HasUnsavedScript(editor));
	CHECK_FALSE(studio::ToolsProbe::OldViewportStateCleared(editor));
}

TEST_CASE("opening a game clears old viewport world handles", "[studio][tools][unsaved]") {
	Context context;
	TemporaryConfig config;
	const auto path = config.Root / "replacement.agame";
	engine::world::Universe source;
	source.Create({.Name = Name("replacement")});
	std::string error;
	REQUIRE(engine::game::SaveGame(source, Name("Replacement"), path, error));
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::PrepareOldViewportState(editor);
	const auto oldWorld = studio::ToolsProbe::UniverseAddress(editor)->Worlds().front();
	studio::ToolsProbe::RequestOpen(editor, path);
	studio::ToolsProbe::Discard(editor);
	CHECK(studio::ToolsProbe::GamePath(editor) == path);
	CHECK(studio::ToolsProbe::UniverseAddress(editor)->Worlds().front() == oldWorld);
	CHECK(studio::ToolsProbe::MainViewportShowsFirstWorld(editor));
	CHECK(studio::ToolsProbe::ExtraViewportOpen(editor));
	CHECK(studio::ToolsProbe::OldViewportStateCleared(editor));
}

TEST_CASE("rebuilding one world releases only its viewport captures", "[studio][tools][world_rebuild]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	engine::gui::RegisterGuiClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto first = editor.Universe->Worlds().front();
	const auto second = editor.Universe->Create({.Name = Name("other")});
	studio::ToolsProbe::ConfigureWorldPanels(editor, first, second);
	editor.Universe->Enter(first, [](Store &store) { engine::scene::InstallServices(store); });
	SECTION("rename") {
		studio::ToolsProbe::PrepareUndoHistory(editor);
		studio::ToolsProbe::CaptureWorldInput(editor, 0, first);
		studio::ToolsProbe::CaptureWorldInput(editor, 1, second);
		studio::ToolsProbe::SelectWorldWorkspace(editor, first);
		studio::ToolsProbe::OpenRuntimeOnlyScript(editor, first);
		uint64_t identity = 0;
		editor.Universe->Enter(first, [&](Store &store) { identity = store.Identity(); });
		REQUIRE(studio::ToolsProbe::RenameWorld(editor, first));
		CHECK(editor.Universe->NameOf(first) == Name("renamed"));
		editor.Universe->Enter(first, [&](Store &store) { CHECK(store.Identity() == identity); });
		CHECK(studio::ToolsProbe::HasWorldSelection(editor, first));
		CHECK(studio::ToolsProbe::HasUnsavedScript(editor));
		CHECK(studio::ToolsProbe::Pressed(editor, 0) != NULL_ENTITY);
		CHECK(studio::ToolsProbe::Following(editor, 0));
		CHECK_FALSE(studio::ToolsProbe::WorldGesturesCleared(editor));
		CHECK(studio::ToolsProbe::ReleaseAdornment(editor, 0, first) == 1);
		return;
	}
	SECTION("stop") {
		REQUIRE(studio::ToolsProbe::BeginServer(editor));
		studio::ToolsProbe::CaptureWorldInput(editor, 0, first);
		studio::ToolsProbe::CaptureWorldInput(editor, 1, second);
		studio::ToolsProbe::OpenRuntimeOnlyScript(editor, first);
		studio::ToolsProbe::StopWorld(editor, first);
		CHECK(studio::ToolsProbe::ScriptDetached(editor));
	}
	SECTION("remove") {
		studio::ToolsProbe::CaptureWorldInput(editor, 0, first);
		studio::ToolsProbe::CaptureWorldInput(editor, 1, second);
		studio::ToolsProbe::AddPinnedViewport(editor, first);
		studio::ToolsProbe::RemoveWorld(editor, first);
		CHECK_FALSE(studio::ToolsProbe::PinnedWorld(editor, 1).IsValid());
		CHECK(studio::ToolsProbe::PinnedWorld(editor, 0) == second);
		CHECK(editor.Universe->Create({.Name = Name("reused")}) == first);
		CHECK_FALSE(studio::ToolsProbe::PinnedWorld(editor, 1).IsValid());
	}
	CHECK(studio::ToolsProbe::Pressed(editor, 0) == NULL_ENTITY);
	CHECK(studio::ToolsProbe::Pressed(editor, 1) != NULL_ENTITY);
	CHECK_FALSE(studio::ToolsProbe::Presented(editor, 0));
	CHECK(studio::ToolsProbe::Presented(editor, 1));
	CHECK_FALSE(studio::ToolsProbe::Following(editor, 0));
	CHECK(studio::ToolsProbe::Following(editor, 1));
	CHECK(studio::ToolsProbe::WorldGesturesCleared(editor));
	CHECK(studio::ToolsProbe::ReleaseAdornment(editor, 0, first) == 0);
	CHECK(studio::ToolsProbe::ReleaseAdornment(editor, 1, second) == 1);
}

TEST_CASE("removing a world refuses to discard an unsaved script", "[studio][tools][world_rebuild]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto world = editor.Universe->Worlds().front();
	editor.Universe->Create({.Name = Name("other")});
	studio::ToolsProbe::OpenRuntimeOnlyScript(editor, world);
	REQUIRE(studio::ToolsProbe::HasUnsavedScript(editor));
	studio::ToolsProbe::HideOutput(editor);
	studio::ToolsProbe::RemoveWorld(editor, world);
	REQUIRE(studio::ToolsProbe::HasUnsavedScene(editor));
	REQUIRE(studio::ToolsProbe::HasUnsavedScript(editor));
	CHECK(studio::ToolsProbe::OutputVisible(editor));
	CHECK(studio::ToolsProbe::OutputWillFocus(editor));
	REQUIRE(studio::ToolsProbe::SaveFirstScript(editor));
	studio::ToolsProbe::RemoveWorld(editor, world);
	CHECK_FALSE(studio::ToolsProbe::HasUnsavedScene(editor));
	CHECK_FALSE(studio::ToolsProbe::HasUnsavedScript(editor));
}

TEST_CASE("renaming a world updates portal links and remains undoable", "[studio][tools][world_rebuild]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto target = editor.Universe->Worlds().front();
	const auto other = editor.Universe->Create({.Name = Name("other")});
	studio::ToolsProbe::PrepareUndoHistory(editor);
	studio::ToolsProbe::RecordWorldPart(editor, target, "Before rename");
	Entity part = NULL_ENTITY;
	editor.Universe->Enter(other, [&](Store &store) {
		part = store.CreateInstance(engine::scene::PartClass(), "Portal");
		engine::scene::Portal portal;
		portal.DestinationWorld = Name("unsaved-scene");
		store.Set(part, portal);
	});
	const auto destination = [&]() {
		Name result;
		editor.Universe->Enter(other, [&](Store &store) {
			result = store.Get<engine::scene::Portal>(part)->DestinationWorld;
		});
		return result;
	};
	REQUIRE(studio::ToolsProbe::RenameWorld(editor, target));
	CHECK(editor.Universe->NameOf(target) == Name("renamed"));
	CHECK(destination() == Name("renamed"));
	REQUIRE(studio::ToolsProbe::Undo(editor));
	CHECK(editor.Universe->NameOf(target) == Name("unsaved-scene"));
	CHECK(destination() == Name("unsaved-scene"));
	CHECK(studio::ToolsProbe::NextUndo(editor) == "Before rename");
	REQUIRE(studio::ToolsProbe::Redo(editor));
	CHECK(editor.Universe->NameOf(target) == Name("renamed"));
	CHECK(destination() == Name("renamed"));
}

TEST_CASE("renaming a world refuses an active edit recording", "[studio][tools][world_rebuild]") {
	Context context;
	TemporaryConfig config;
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto world = editor.Universe->Worlds().front();
	studio::ToolsProbe::PrepareUndoHistory(editor);
	const auto recording = studio::ToolsProbe::BeginRecording(editor);
	REQUIRE(recording.has_value());
	CHECK_FALSE(studio::ToolsProbe::RenameWorld(editor, world));
	CHECK(studio::ToolsProbe::CancelRecording(editor, *recording));
	CHECK(editor.Universe->NameOf(world) == Name("unsaved-scene"));
	CHECK(studio::ToolsProbe::OutputWillFocus(editor));
}

TEST_CASE("removing a world forgets only its undo and redo history", "[studio][tools][world_rebuild]") {
	Context context;
	TemporaryConfig config;
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto removed = editor.Universe->Worlds().front();
	const auto kept = editor.Universe->Create({.Name = Name("kept")});
	studio::ToolsProbe::PrepareUndoHistory(editor);
	studio::ToolsProbe::RecordWorldPart(editor, kept, "Kept one");
	studio::ToolsProbe::RecordWorldPart(editor, kept, "Kept two");
	studio::ToolsProbe::RecordWorldPart(editor, removed, "Removed");
	const auto oldId = studio::ToolsProbe::Track(editor, removed, Entity{12345});
	const auto keptId = studio::ToolsProbe::Track(editor, kept, Entity{67890});
	REQUIRE(studio::ToolsProbe::Undo(editor));
	REQUIRE(studio::ToolsProbe::Undo(editor));
	studio::ToolsProbe::RemoveWorld(editor, removed);
	REQUIRE(editor.Universe->Create({.Name = Name("reused")}) == removed);
	CHECK(studio::ToolsProbe::Resolve(editor, oldId) == NULL_ENTITY);
	CHECK(studio::ToolsProbe::Resolve(editor, keptId) == Entity{67890});
	CHECK(studio::ToolsProbe::Track(editor, removed, Entity{12345}) != oldId);
	CHECK(studio::ToolsProbe::NextUndo(editor) == "Kept one");
	CHECK(studio::ToolsProbe::NextRedo(editor) == "Kept two");
	REQUIRE(studio::ToolsProbe::Undo(editor));
	CHECK_FALSE(studio::ToolsProbe::CanUndo(editor));
}

TEST_CASE(
	"removing a non-active world clears its selection and queued activation", "[studio][tools][world_rebuild]"
) {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto active = editor.Universe->Worlds().front();
	const auto removed = editor.Universe->Create({.Name = Name("removed")});
	studio::ToolsProbe::SetActiveWorld(editor, active);
	editor.Universe->Enter(removed, [](Store &store) { engine::scene::InstallServices(store); });
	studio::ToolsProbe::SelectWorldWorkspace(editor, removed);
	REQUIRE(studio::ToolsProbe::HasWorldSelection(editor, removed));
	studio::ToolsProbe::QueueWorldActivation(editor, removed);
	studio::ToolsProbe::RemoveWorld(editor, removed);
	CHECK(studio::ToolsProbe::SelectionWorld(editor) == active);
	CHECK_FALSE(studio::ToolsProbe::HasQueuedWorldAction(editor));
	REQUIRE(editor.Universe->Create({.Name = Name("reused")}) == removed);
	studio::ToolsProbe::ApplyWorldActions(editor);
	CHECK(studio::ToolsProbe::SelectionWorld(editor) == active);
}

TEST_CASE("removing a running world is refused", "[studio][tools][world_rebuild]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto world = editor.Universe->Worlds().front();
	editor.Universe->Create({.Name = Name("other")});
	editor.Universe->Enter(world, [](Store &store) { engine::scene::InstallServices(store); });
	REQUIRE(studio::ToolsProbe::BeginServer(editor));
	studio::ToolsProbe::HideOutput(editor);
	CHECK_FALSE(studio::ToolsProbe::RenameWorld(editor, world));
	CHECK(editor.Universe->NameOf(world) == Name("unsaved-scene"));
	studio::ToolsProbe::RemoveWorld(editor, world);
	CHECK(studio::ToolsProbe::HasUnsavedScene(editor));
	CHECK(studio::ToolsProbe::OutputVisible(editor));
	CHECK(studio::ToolsProbe::OutputWillFocus(editor));
	studio::ToolsProbe::StopWorld(editor, world);
}

TEST_CASE("removing a world waits for the current edit recording", "[studio][tools][world_rebuild]") {
	Context context;
	TemporaryConfig config;
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto world = editor.Universe->Worlds().front();
	editor.Universe->Create({.Name = Name("other")});
	studio::ToolsProbe::PrepareUndoHistory(editor);
	const auto recording = studio::ToolsProbe::BeginRecording(editor);
	REQUIRE(recording.has_value());
	studio::ToolsProbe::RemoveWorld(editor, world);
	CHECK(studio::ToolsProbe::HasUnsavedScene(editor));
	REQUIRE(studio::ToolsProbe::CancelRecording(editor, *recording));
	studio::ToolsProbe::RemoveWorld(editor, world);
	CHECK_FALSE(studio::ToolsProbe::HasUnsavedScene(editor));
}

TEST_CASE("removing the active world rebinds its studio services", "[studio][tools][world_rebuild]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto removed = editor.Universe->Worlds().front();
	const auto kept = editor.Universe->Create({.Name = Name("kept")});
	for (const auto world : {removed, kept}) {
		editor.Universe->Enter(world, [](Store &store) { engine::scene::InstallServices(store); });
	}
	studio::ToolsProbe::SetActiveWorld(editor, removed);
	REQUIRE(studio::ToolsProbe::StartServer(editor, kept));
	studio::ToolsProbe::LoadStudioPlugins(editor);
	REQUIRE(studio::ToolsProbe::HasStudioPlugins(editor));
	const void *const runtime = studio::ToolsProbe::ServerRuntime(editor, kept);
	const void *const playtest = studio::ToolsProbe::PlaytestPlugins(editor, kept);
	REQUIRE(runtime != nullptr);
	REQUIRE(playtest != nullptr);
	REQUIRE(studio::ToolsProbe::RunCommand(editor));
	REQUIRE(studio::ToolsProbe::CommandWorld(editor) == removed);
	studio::ToolsProbe::SelectWorldWorkspace(editor, kept);
	REQUIRE(studio::ToolsProbe::HasWorldSelection(editor, kept));
	studio::ToolsProbe::RemoveWorld(editor, removed);
	CHECK(studio::ToolsProbe::HasStudioPlugins(editor));
	CHECK(studio::ToolsProbe::ServerRuntime(editor, kept) == runtime);
	CHECK(studio::ToolsProbe::PlaytestPlugins(editor, kept) == playtest);
	CHECK(studio::ToolsProbe::HasWorldSelection(editor, kept));
	CHECK_FALSE(studio::ToolsProbe::CommandWorld(editor).IsValid());
	REQUIRE(studio::ToolsProbe::RunCommand(editor));
	CHECK(studio::ToolsProbe::CommandWorld(editor) == kept);
	studio::ToolsProbe::StopWorld(editor, kept);
	CHECK_FALSE(studio::ToolsProbe::CommandWorld(editor).IsValid());
}

TEST_CASE("new game clears undo and redo from the old world", "[studio][tools][unsaved]") {
	Context context;
	TemporaryConfig config;
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::PrepareUndoHistory(editor);
	REQUIRE(studio::ToolsProbe::CanUndo(editor));
	SECTION("undo history") {}
	SECTION("redo history") {
		REQUIRE(studio::ToolsProbe::Undo(editor));
		REQUIRE(studio::ToolsProbe::CanRedo(editor));
	}
	studio::ToolsProbe::RequestNew(editor);
	REQUIRE(studio::ToolsProbe::PendingDiscard(editor));
	studio::ToolsProbe::Discard(editor);
	CHECK_FALSE(studio::ToolsProbe::CanUndo(editor));
	CHECK_FALSE(studio::ToolsProbe::CanRedo(editor));
}

TEST_CASE("closing another script tab keeps the active script", "[studio][tools][scripts]") {
	studio::Editor editor;
	studio::ToolsProbe::PrepareScriptTabs(editor, 4, 2);
	studio::ToolsProbe::CloseScriptTab(editor, 0);
	CHECK(studio::ToolsProbe::ActiveScript(editor) == 1);
	CHECK(studio::ToolsProbe::ActiveScriptText(editor) == "2");
	studio::ToolsProbe::CloseScriptTab(editor, 2);
	CHECK(studio::ToolsProbe::ActiveScriptText(editor) == "2");
	studio::ToolsProbe::CloseScriptTab(editor, 1);
	CHECK(studio::ToolsProbe::ActiveScript(editor) == 0);
	studio::ToolsProbe::CloseScriptTab(editor, 0);
	CHECK(studio::ToolsProbe::ActiveScript(editor) == -1);
}

TEST_CASE("failed script flush refuses game save and tab close", "[studio][tools][script_save]") {
	Context context;
	TemporaryConfig config;
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::PrepareDeletedScript(editor);
	REQUIRE(studio::ToolsProbe::HasUnsavedScript(editor));
	const auto path = config.Root / "existing.agame";
	std::ofstream(path) << "unchanged";
	CHECK_FALSE(studio::ToolsProbe::SaveGame(editor, path));
	std::ifstream saved(path);
	std::string contents;
	saved >> contents;
	CHECK(contents == "unchanged");
	CHECK(studio::ToolsProbe::HasUnsavedScript(editor));
	CHECK_FALSE(studio::ToolsProbe::ExportWorld(editor, config.Root / "export.aworld"));
	CHECK_FALSE(std::filesystem::exists(config.Root / "export.aworld"));
	CHECK_FALSE(studio::ToolsProbe::BeginServer(editor));
	const auto frame = [&](ImVec2 mouse, bool down) {
		auto &io = ImGui::GetIO();
		io.AddMousePosEvent(mouse.x, mouse.y);
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
		ImGui::NewFrame();
		ImGui::SetNextWindowSize(ImVec2(600, 300), ImGuiCond_Once);
		studio::ToolsProbe::DrawScripts(editor);
		ImGui::Render();
	};
	frame(ImVec2(-1, -1), false);
	frame(ImVec2(-1, -1), false);
	ImGuiWindow *window = nullptr;
	for (ImGuiWindow *candidate : GImGui->Windows) {
		if (std::string_view(candidate->Name).starts_with("(deleted)") &&
			(candidate->Flags & ImGuiWindowFlags_ChildWindow) == 0)
			window = candidate;
	}
	REQUIRE(window != nullptr);
	const ImVec2 close(
		window->Pos.x + window->Size.x - ImGui::GetStyle().FramePadding.x - ImGui::GetFontSize() * 0.5f,
		window->Pos.y + window->TitleBarHeight * 0.5f
	);
	frame(close, false);
	frame(close, true);
	frame(close, false);
	CHECK(studio::ToolsProbe::HasUnsavedScript(editor));
	const ImVec2 discard(
		window->DC.CursorStartPos.x + 80.0f,
		window->DC.CursorStartPos.y + ImGui::GetTextLineHeightWithSpacing() + 160.0f +
			ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeight() * 0.5f
	);
	frame(discard, false);
	frame(discard, true);
	frame(discard, false);
	CHECK_FALSE(studio::ToolsProbe::HasUnsavedScript(editor));
}

TEST_CASE("removing a gutter breakpoint updates its marker immediately", "[studio][tools][debugger]") {
	Context context;
	studio::Editor editor;
	studio::OpenScript tab;
	tab.Path = Name("gutter-test.luau");
	tab.Text = "first\nsecond";
	studio::ToolsProbe::ToggleBreakpoint(editor, tab.Path, 1);
	studio::ToolsProbe::ToggleBreakpoint(editor, tab.Path, 2);
	REQUIRE(studio::ToolsProbe::BreakpointCount(editor) == 2);

	const auto frame = [&](ImVec2 mouse, bool down) {
		auto &io = ImGui::GetIO();
		io.AddMousePosEvent(mouse.x, mouse.y);
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(300, 200), ImGuiCond_Always);
		ImGui::Begin("gutter test", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings);
		studio::ToolsProbe::DrawGutter(editor, tab);
		ImGui::End();
		ImGui::Render();
		return ImGui::GetDrawData()->TotalVtxCount;
	};
	frame(ImVec2(-1, -1), false);
	ImGuiWindow *gutter = nullptr;
	for (ImGuiWindow *candidate : GImGui->Windows) {
		if (std::string_view(candidate->Name).find("##gutter") != std::string_view::npos) gutter = candidate;
	}
	REQUIRE(gutter != nullptr);
	const ImVec2 point(
		gutter->DC.CursorStartPos.x + 4.0f, gutter->DC.CursorStartPos.y + ImGui::GetTextLineHeight() * 0.5f
	);
	frame(point, false);
	frame(point, true);
	const int clickedVertices = frame(point, false);
	REQUIRE(studio::ToolsProbe::BreakpointCount(editor) == 1);
	const int nextVertices = frame(point, false);
	CHECK(clickedVertices == nextVertices);
}

TEST_CASE("a live unwritable script offers an explicit discard", "[studio][tools][script_save]") {
	Context context;
	TemporaryConfig config;
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::PrepareLiveUnwritableScript(editor);
	REQUIRE(studio::ToolsProbe::HasUnsavedScript(editor));
	REQUIRE_FALSE(studio::ToolsProbe::SaveGame(editor, config.Root / "game.agame"));
	ImGui::NewFrame();
	ImGui::SetNextWindowSize(ImVec2(600, 300), ImGuiCond_Once);
	ImGui::LogToBuffer();
	studio::ToolsProbe::DrawScripts(editor);
	const std::string output = GImGui->LogBuffer.c_str();
	ImGui::LogFinish();
	ImGui::Render();
	CHECK(output.find("Discard unsaved text and close") != std::string::npos);
	CHECK(studio::ToolsProbe::HasUnsavedScript(editor));
	ImGuiWindow *window = nullptr;
	for (ImGuiWindow *candidate : GImGui->Windows) {
		if (std::string_view(candidate->Name).starts_with("Shader *") &&
			(candidate->Flags & ImGuiWindowFlags_ChildWindow) == 0)
			window = candidate;
	}
	REQUIRE(window != nullptr);
	const ImVec2 discard(
		window->DC.CursorStartPos.x + 80.0f, window->DC.CursorStartPos.y + ImGui::GetFrameHeight() * 0.5f
	);
	const auto frame = [&](bool down) {
		ImGui::GetIO().AddMousePosEvent(discard.x, discard.y);
		ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, down);
		ImGui::NewFrame();
		studio::ToolsProbe::DrawScripts(editor);
		ImGui::Render();
	};
	frame(false);
	frame(true);
	frame(false);
	CHECK_FALSE(studio::ToolsProbe::HasUnsavedScript(editor));
}

TEST_CASE("quit confirmation remains available over universe permissions", "[studio][tools][unsaved]") {
	Context context;
	TemporaryConfig config;
	const auto path = config.Root / "target.auniverse";
	engine::world::Universe source;
	source.Create({.Name = Name("target-scene")});
	std::string error;
	REQUIRE(engine::game::SaveUniverse(source, Name("Target"), {}, path, {}, error));
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::SetModified(editor, false);
	studio::ToolsProbe::RequestOpen(editor, path);
	const auto frame = [&] {
		ImGui::NewFrame();
		studio::ToolsProbe::DrawDialogs(editor);
		ImGui::Render();
	};
	frame();
	REQUIRE(studio::ToolsProbe::UniversePermissionPending(editor));
	studio::ToolsProbe::SetModified(editor, true);
	studio::ToolsProbe::RequestExit(editor);
	frame();
	REQUIRE_FALSE(GImGui->OpenPopupStack.empty());
	REQUIRE(GImGui->OpenPopupStack[0].Window != nullptr);
	CHECK(std::string(GImGui->OpenPopupStack[0].Window->Name) == "Load Universe");
	CHECK(studio::ToolsProbe::UniversePermissionPending(editor));
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
	frame();
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
	frame();
	CHECK(studio::ToolsProbe::PendingDiscard(editor));
	CHECK_FALSE(studio::ToolsProbe::UniversePermissionPending(editor));
	REQUIRE_FALSE(GImGui->OpenPopupStack.empty());
	REQUIRE(GImGui->OpenPopupStack[0].Window != nullptr);
	CHECK(std::string(GImGui->OpenPopupStack[0].Window->Name) == "Unsaved Changes");
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
	frame();
	CHECK_FALSE(studio::ToolsProbe::PendingDiscard(editor));
	CHECK(studio::ToolsProbe::HasUnsavedScene(editor));
}

TEST_CASE("cancelled quit preserves the save dialog selection", "[studio][tools][unsaved]") {
	Context context;
	TemporaryConfig config;
	const auto target = config.Root / "selected.agame";
	std::ofstream(target).put('\n');
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::StartSaveAs(editor, config.Root / "original.agame");
	const auto frame = [&](ImVec2 mouse, bool down) {
		auto &io = ImGui::GetIO();
		io.AddMousePosEvent(mouse.x, mouse.y);
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
		ImGui::NewFrame();
		studio::ToolsProbe::DrawDialogs(editor);
		ImGui::Render();
	};
	frame(ImVec2(-1, -1), false);
	frame(ImVec2(-1, -1), false);
	frame(ImVec2(-1, -1), false);
	ImGuiWindow *popup = ImGui::FindWindowByName("Save Game As");
	REQUIRE(popup != nullptr);
	ImGuiWindow *rows = nullptr;
	for (ImGuiWindow *window : GImGui->Windows) {
		if (window->ParentWindow == popup && (window->Flags & ImGuiWindowFlags_ChildWindow)) rows = window;
	}
	REQUIRE(rows != nullptr);
	const float row = ImGui::GetTextLineHeightWithSpacing();
	const ImVec2 selected(rows->DC.CursorStartPos.x + 30.0f, rows->DC.CursorStartPos.y + row * 1.5f);
	frame(selected, false);
	frame(selected, true);
	frame(selected, false);
	studio::ToolsProbe::SetRunning(editor, true);
	studio::ToolsProbe::RequestExit(editor);
	studio::ToolsProbe::RequestExit(editor);
	frame(selected, false);
	CHECK(studio::ToolsProbe::QuitRequested(editor));
	CHECK_FALSE(studio::ToolsProbe::PendingDiscard(editor));
	popup = ImGui::FindWindowByName("Save Game As");
	REQUIRE(popup != nullptr);
	const ImVec2 save(
		popup->DC.CursorStartPos.x + 60.0f,
		popup->Pos.y + popup->Size.y - ImGui::GetStyle().WindowPadding.y - ImGui::GetFrameHeight() * 0.5f
	);
	frame(save, false);
	frame(save, true);
	frame(save, false);
	CHECK(studio::ToolsProbe::GamePath(editor) == target);
	CHECK_FALSE(studio::ToolsProbe::IsRunning(editor));
}

TEST_CASE("quit waits for a panel file picker", "[studio][tools][unsaved]") {
	Context context;
	TemporaryConfig config;
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto frame = [&](ImVec2 mouse, bool down) {
		auto &io = ImGui::GetIO();
		io.AddMousePosEvent(mouse.x, mouse.y);
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
		ImGui::NewFrame();
		studio::ToolsProbe::DrawUiAuthoring(editor);
		studio::ToolsProbe::DrawDialogs(editor);
		ImGui::Render();
	};
	frame(ImVec2(-1, -1), false);
	frame(ImVec2(-1, -1), false);
	const ImGuiWindow *panel = ImGui::FindWindowByName("UI Authoring");
	REQUIRE(panel != nullptr);
	const ImVec2 button(
		panel->DC.CursorStartPos.x + 40, panel->DC.CursorStartPos.y + ImGui::GetFrameHeight() * 0.5f
	);
	frame(button, false);
	frame(button, true);
	frame(button, false);
	REQUIRE_FALSE(GImGui->OpenPopupStack.empty());
	REQUIRE(GImGui->OpenPopupStack[0].Window != nullptr);
	CHECK(std::string(GImGui->OpenPopupStack[0].Window->Name) == "Import UI Document");
	studio::ToolsProbe::RequestExit(editor);
	frame(button, false);
	REQUIRE_FALSE(GImGui->OpenPopupStack.empty());
	REQUIRE(GImGui->OpenPopupStack[0].Window != nullptr);
	CHECK(std::string(GImGui->OpenPopupStack[0].Window->Name) == "Import UI Document");
	studio::ToolsProbe::RequestExit(editor);
	frame(button, false);
	CHECK(studio::ToolsProbe::QuitRequested(editor));
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
	frame(button, false);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
	frame(button, false);
	CHECK(studio::ToolsProbe::PendingDiscard(editor));
	frame(button, false);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
	frame(button, false);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
	frame(button, false);
	CHECK_FALSE(studio::ToolsProbe::PendingDiscard(editor));
	CHECK_FALSE(studio::ToolsProbe::QuitRequested(editor));
}

TEST_CASE("queued quit lets the open dialog finish its replacement", "[studio][tools][unsaved]") {
	Context context;
	TemporaryConfig config;
	const auto target = config.Root / "target.agame";
	engine::world::Universe source;
	source.Create({.Name = Name("target-scene")});
	std::string error;
	REQUIRE(engine::game::SaveGame(source, Name("Target"), target, error));
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::SetRunning(editor, true);
	studio::ToolsProbe::StartOpen(editor, target);
	const auto frame = [&](ImVec2 mouse, bool down) {
		auto &io = ImGui::GetIO();
		io.AddMousePosEvent(mouse.x, mouse.y);
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
		ImGui::NewFrame();
		studio::ToolsProbe::DrawDialogs(editor);
		ImGui::Render();
	};
	frame(ImVec2(-1, -1), false);
	frame(ImVec2(-1, -1), false);
	frame(ImVec2(-1, -1), false);
	studio::ToolsProbe::RequestExit(editor);
	frame(ImVec2(-1, -1), false);
	REQUIRE(studio::ToolsProbe::QuitRequested(editor));
	const ImGuiWindow *popup = ImGui::FindWindowByName("Open Game");
	REQUIRE(popup != nullptr);
	const ImVec2 open(
		popup->DC.CursorStartPos.x + 60.0f,
		popup->Pos.y + popup->Size.y - ImGui::GetStyle().WindowPadding.y - ImGui::GetFrameHeight() * 0.5f
	);
	frame(open, false);
	frame(open, true);
	frame(open, false);
	REQUIRE(studio::ToolsProbe::PendingDiscard(editor));
	CHECK(studio::ToolsProbe::PendingPath(editor) == target);
	CHECK(studio::ToolsProbe::QuitRequested(editor));
	studio::ToolsProbe::SetRunning(editor, false);
	studio::ToolsProbe::Discard(editor);
	CHECK(studio::ToolsProbe::GamePath(editor) == target);
	studio::ToolsProbe::SetRunning(editor, true);
	frame(ImVec2(-1, -1), false);
	CHECK_FALSE(studio::ToolsProbe::IsRunning(editor));
	CHECK_FALSE(studio::ToolsProbe::QuitRequested(editor));
}

TEST_CASE("missing recent game can be removed by right click", "[studio][tools][recent_menu]") {
	Context context;
	TemporaryConfig config;
	studio::Editor editor;
	studio::ToolsProbe::PrepareRecentMenu(editor, config.Root / "missing.age");
	const auto frame = [&](ImVec2 mouse, bool left, bool right) {
		auto &io = ImGui::GetIO();
		io.AddMousePosEvent(mouse.x, mouse.y);
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, left);
		io.AddMouseButtonEvent(ImGuiMouseButton_Right, right);
		ImGui::NewFrame();
		studio::ToolsProbe::DrawMenuBar(editor);
		ImGui::Render();
	};
	const ImVec2 file(15.0f, 10.0f);
	frame(file, false, false);
	frame(file, true, false);
	frame(file, false, false);
	ImGuiWindow *fileMenu = ImGui::FindWindowByName("File###Menu_00");
	REQUIRE(fileMenu != nullptr);
	const float row = ImGui::GetTextLineHeightWithSpacing();
	const ImVec2 recent(
		fileMenu->Pos.x + 30.0f, fileMenu->Pos.y + ImGui::GetStyle().WindowPadding.y + row * 2.5f
	);
	frame(recent, false, false);
	frame(recent, false, false);
	ImGuiWindow *recentMenu = ImGui::FindWindowByName("Open Recent###Menu_01");
	REQUIRE(recentMenu != nullptr);
	const ImVec2 missing(
		recentMenu->Pos.x + 30.0f, recentMenu->Pos.y + ImGui::GetStyle().WindowPadding.y + row * 0.5f
	);
	frame(missing, false, false);
	frame(missing, false, true);
	CHECK(studio::ToolsProbe::RecentEmpty(editor));
}

TEST_CASE(
	"recent games with the same filename have separate menu identities", "[studio][tools][recent_menu]"
) {
	Context context;
	TemporaryConfig config;
	const auto first = config.Root / "first" / "game.agame";
	const auto second = config.Root / "second" / "game.agame";
	std::filesystem::create_directories(first.parent_path());
	std::filesystem::create_directories(second.parent_path());
	engine::world::Universe source;
	source.Create({.Name = Name("recent-test")});
	std::string error;
	REQUIRE(engine::game::SaveGame(source, Name("First"), first, error));
	REQUIRE(engine::game::SaveGame(source, Name("Second"), second, error));
	studio::Editor editor;
	studio::ToolsProbe::PrepareRecentMenu(editor, first);
	studio::ToolsProbe::AddRecent(editor, second);
	const auto frame = [&](ImVec2 mouse, bool left) {
		auto &io = ImGui::GetIO();
		io.AddMousePosEvent(mouse.x, mouse.y);
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, left);
		ImGui::NewFrame();
		studio::ToolsProbe::DrawMenuBar(editor);
		ImGui::Render();
	};
	const auto selectSecond = [&] {
		const ImVec2 file(15.0f, 10.0f);
		frame(file, false);
		frame(file, true);
		frame(file, false);
		ImGuiWindow *fileMenu = ImGui::FindWindowByName("File###Menu_00");
		REQUIRE(fileMenu != nullptr);
		const float row = ImGui::GetTextLineHeightWithSpacing();
		const ImVec2 recent(
			fileMenu->Pos.x + 30.0f, fileMenu->Pos.y + ImGui::GetStyle().WindowPadding.y + row * 2.5f
		);
		frame(recent, false);
		frame(recent, false);
		ImGuiWindow *recentMenu = ImGui::FindWindowByName("Open Recent###Menu_01");
		REQUIRE(recentMenu != nullptr);
		const ImVec2 item(
			recentMenu->Pos.x + 30.0f, recentMenu->Pos.y + ImGui::GetStyle().WindowPadding.y + row * 1.5f
		);
		frame(item, false);
		frame(item, false);
		CHECK(GImGui->HoveredIdPreviousFrameItemCount == 1);
		frame(item, true);
		frame(item, false);
	};
	selectSecond();
	CHECK(studio::ToolsProbe::GamePath(editor) == second);
	selectSecond();
	CHECK(studio::ToolsProbe::GamePath(editor) == first);
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
