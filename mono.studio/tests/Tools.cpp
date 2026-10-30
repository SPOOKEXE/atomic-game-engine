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
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/game/Game.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Shaders.hpp>
#include <engine/script/Host.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/spatial/HashGrid.hpp>
#include <engine/spatial/Query.hpp>
#include <engine/testing/Suite.hpp>

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <fstream>
#include <functional>
#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <studio/Config.hpp>
#include <studio/Editor.hpp>
#include <studio/PlayLink.hpp>
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

		static engine::world::WorldId StartReplica(Editor &editor, engine::world::WorldId authority) {
			auto link = std::make_unique<PlayLink>();
			std::string error;
			REQUIRE(link->Start(*editor.Universe, authority, 60, error));
			const auto replica = link->ReplicaWorld();
			auto &run = editor.Runs.emplace_back();
			run.World = authority;
			run.Links.push_back(std::move(link));
			return replica;
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

		static void PreparePanelShortcuts(Editor &editor) {
			editor.RegisterOperators();
			editor.ShowHeap = false;
			editor.ShowSettings = false;
		}

		static bool HeapVisible(const Editor &editor) {
			return editor.ShowHeap;
		}
		static bool SettingsVisible(const Editor &editor) {
			return editor.ShowSettings;
		}

		static void ResolveFocusedViewport(Editor &editor) {
			editor.ResolveFocusedViewport();
		}

		static bool AskingSaveAs(const Editor &editor) {
			return editor.AskingSaveAs;
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

		static bool SpawnPlayer(Editor &editor, WorldId world) {
			return editor.SpawnPlayer(world);
		}

		static bool RemovePlayer(Editor &editor, WorldId world) {
			return editor.RemovePlayer(world);
		}

		static void StopReplica(Editor &editor, WorldId authority, WorldId replica) {
			auto *run = editor.RunOf(authority);
			REQUIRE(run != nullptr);
			for (auto &link : run->Links) {
				if (link->ReplicaWorld() == replica) {
					editor.StopPlayLink(*link);
					return;
				}
			}
			FAIL("replica link missing");
		}

		static Entity ClientPlayer(Editor &editor, WorldId world) {
			auto *run = editor.RunOf(world);
			REQUIRE(run != nullptr);
			REQUIRE(run->Links.size() == 1);
			return run->Links.front()->Player();
		}

		static void FollowTeleports(Editor &editor) {
			editor.FollowTeleports();
		}

		static std::vector<WorldId> ReplicaWorlds(Editor &editor, WorldId world) {
			std::vector<WorldId> result;
			auto *run = editor.RunOf(world);
			if (run == nullptr) return result;
			for (const auto &link : run->Links)
				result.push_back(link->ReplicaWorld());
			return result;
		}

		static std::vector<uint64_t> ReplicaIdentities(Editor &editor, WorldId world) {
			std::vector<uint64_t> result;
			auto *run = editor.RunOf(world);
			if (run == nullptr) return result;
			for (const auto &link : run->Links)
				result.push_back(link->ReplicaIdentity());
			return result;
		}

		static bool HasOpenViewport(const Editor &editor, WorldId world) {
			for (const auto &view : editor.Extras) {
				if (view.Open && view.World == world) return true;
			}
			return false;
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

		static bool DuplicateWorldWithEdits(Editor &editor, engine::world::WorldId world) {
			return editor.DuplicateWorld(world);
		}

		static Entity DuplicateInstance(Editor &editor, engine::world::WorldId world, Entity instance) {
			editor.SelectionWorld = world;
			editor.Selection = {instance};
			editor.DuplicateSelection();
			return editor.Selection.empty() ? NULL_ENTITY : editor.Selection.front();
		}

		static std::vector<Entity>
		DuplicateSelection(Editor &editor, engine::world::WorldId world, std::vector<Entity> instances) {
			editor.SelectionWorld = world;
			editor.Selection = std::move(instances);
			editor.DuplicateSelection();
			return editor.Selection;
		}

		static bool RenameWorld(Editor &editor, engine::world::WorldId world) {
			return editor.RenameWorld(world, Name("renamed"));
		}
		static bool MoveScriptParent(
			Editor &editor, engine::world::WorldId source, Entity parent, engine::world::WorldId target
		) {
			return editor.MoveInstanceToWorld(source, parent, target, NULL_ENTITY);
		}

		static void StopWorld(Editor &editor, engine::world::WorldId world) {
			editor.EndRun(world);
		}

		static void OpenRuntimeOnlyScript(Editor &editor, engine::world::WorldId world, bool hasPath = true) {
			Entity script;
			editor.Universe->Enter(world, [&](Store &store) {
				script = store.CreateInstance(engine::script::ScriptClass(), "RuntimeOnly");
				if (hasPath) {
					const Name path("Scripts/runtime-only.luau");
					engine::script::SetSourcePath(store, script, path);
					if (store.Resource<engine::script::SourceCache>() == nullptr)
						store.SetResource(engine::script::SourceCache{});
					if (store.Resource<engine::script::SourceCache>()->Find(path) == nullptr)
						store.ResourceMutable<engine::script::SourceCache>()->Set(path, "");
				}
			});
			editor.OpenScriptTab(world, script);
			editor.Scripts.back().Text = "unsaved source";
			editor.Scripts.back().Modified = true;
		}

		static void OpenMoveScript(Editor &editor, engine::world::WorldId world, Entity instance) {
			editor.OpenScriptTab(world, instance);
			editor.Scripts.back().Text = "edited javascript";
			editor.Scripts.back().Modified = true;
		}

		static bool DuplicateScriptEditsIntact(const Editor &editor) {
			return editor.Scripts.size() == 2 && editor.Scripts[0].Modified &&
				   editor.Scripts[0].Text == "edited javascript" && editor.Scripts[1].Modified &&
				   editor.Scripts[1].Text == "edited shader";
		}

		static bool TwoScriptEditsDirty(const Editor &editor) {
			return editor.Scripts.size() == 2 && editor.Scripts[0].Modified && editor.Scripts[1].Modified;
		}

		static bool MoveScriptEditIntact(const Editor &editor) {
			return editor.Scripts.size() == 1 && editor.Scripts.front().Modified &&
				   editor.Scripts.front().Text == "edited javascript";
		}

		static Entity LastMoveScriptInstance(const Editor &editor) {
			return editor.Scripts.back().Instance;
		}

		static void SetLastMoveScriptText(Editor &editor, std::string text) {
			editor.Scripts.back().Text = std::move(text);
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

		static void DeleteSelection(Editor &editor, WorldId world, Entity instance) {
			editor.SelectionWorld = world;
			editor.Selection = {instance};
			editor.DeleteSelection();
		}

		static void DeleteSelection(Editor &editor, WorldId world, const std::vector<Entity> &instances) {
			editor.SelectionWorld = world;
			editor.Selection = instances;
			editor.DeleteSelection();
		}

		static void ApplySelectionTool(
			Editor &editor, WorldId world, const std::vector<Entity> &instances, const char *property
		) {
			editor.SelectionWorld = world;
			editor.Selection = instances;
			if (property == nullptr) {
				editor.ResetSelectionPivot();
			} else {
				editor.SetSelectionFlag(property, true, property);
			}
		}

		static void DrawScripts(Editor &editor) {
			editor.DrawScripts();
		}

		static void ShowScriptTabs(Editor &editor) {
			editor.ShowScripts = true;
		}

		static void ReopenScript(Editor &editor, WorldId world, Entity instance) {
			editor.OpenScriptTab(world, instance);
			editor.Scripts.back().Text = "unsaved source";
			editor.Scripts.back().Modified = true;
		}

		static void UpdateCompletion(Editor &editor, OpenScript &tab, bool changed) {
			editor.UpdateScriptCompletion(tab, changed, false);
		}

		static bool CompletionOpen(const Editor &editor) {
			return editor.ScriptPopupOpen;
		}

		static bool CompletionContains(const Editor &editor, std::string_view value) {
			return std::any_of(
				editor.ScriptCompletions.begin(), editor.ScriptCompletions.end(), [&](const auto &entry) {
					return entry.Text == value;
				}
			);
		}

		static void SeedCompletionOwner(Editor &editor, size_t owner) {
			for (size_t index = 0; index < editor.Scripts.size(); ++index)
				editor.Scripts[index].Instance = Entity(index + 1);
			editor.ScriptPopupWorld = editor.Scripts[owner].World;
			editor.ScriptPopupInstance = editor.Scripts[owner].Instance;
			editor.ScriptPopupOpen = true;
			editor.ScriptHoverWorld = editor.Scripts[owner].World;
			editor.ScriptHoverInstance = editor.Scripts[owner].Instance;
			editor.ScriptHoverWord = "cached";
		}

		static void DrawScriptHover(Editor &editor, OpenScript &tab, ImVec2 origin) {
			editor.DrawScriptHover(tab, origin, true, 0);
		}

		static const std::string &ScriptHoverWord(const Editor &editor) {
			return editor.ScriptHoverWord;
		}

		static const std::string &ScriptHoverText(const Editor &editor) {
			return editor.ScriptHoverText;
		}

		static bool HasUnsavedScript(const Editor &editor) {
			return editor.Scripts.size() == 1 && editor.Scripts.front().Modified &&
				   editor.Scripts.front().Text == "unsaved source";
		}

		static Entity FirstScriptInstance(const Editor &editor) {
			return editor.Scripts.empty() ? NULL_ENTITY : editor.Scripts.front().Instance;
		}

		static void OpenTab(Editor &editor, engine::world::WorldId world, Entity instance) {
			editor.OpenScriptTab(world, instance);
		}

		static void CloseTab(Editor &editor, size_t index) {
			editor.CloseScriptTab(index);
		}

		static Entity ScriptAt(const Editor &editor, size_t index) {
			return editor.Scripts.at(index).Instance;
		}

		static bool UnsavedAt(const Editor &editor, size_t index) {
			return editor.Scripts.at(index).Modified && editor.Scripts.at(index).Text == "unsaved source";
		}

		static void ClearHistoryAndTrack(Editor &editor, engine::world::WorldId world, Entity instance) {
			editor.Commands->Clear();
			editor.Commands->Track(world, instance);
		}

		static void RebindTabs(Editor &editor) {
			editor.RebindScriptTabs();
		}

		static void MarkFirstScriptClean(Editor &editor) {
			editor.Scripts.front().Modified = false;
		}

		static void MarkFirstScriptDirty(Editor &editor) {
			editor.Scripts.front().Modified = true;
		}

		static size_t ScriptCount(const Editor &editor) {
			return editor.Scripts.size();
		}

		static void PrepareCommands(Editor &editor) {
			editor.Commands = std::make_unique<CommandLog>(*editor.Universe);
		}

		static void RecordCreate(Editor &editor, engine::world::WorldId world, Entity root, Entity tracked) {
			editor.Universe->Enter(world, [&](Store &store) {
				if (tracked != NULL_ENTITY) editor.Commands->Track(world, tracked);
				editor.Commands->RecordCreate(store, world, root, "Create instance");
			});
		}

		static void UndoEdit(Editor &editor) {
			editor.UndoEdit();
		}

		static void RedoEdit(Editor &editor) {
			editor.RedoEdit();
		}

		static void WatchRedo(Editor &editor, std::function<void()> callback) {
			CommandLog::Watcher watcher;
			watcher.Redone = [callback = std::move(callback)](std::string_view) { callback(); };
			editor.Commands->Watch(std::move(watcher));
		}

		static void WatchDeleteHistory(
			Editor &editor,
			size_t &commits,
			size_t &commands,
			size_t &starts,
			size_t &finishes,
			bool &inside,
			std::function<void()> onCommit = {}
		) {
			CommandLog::Watcher watcher;
			watcher.Committed = [&,
								 onCommit = std::move(onCommit)](uint64_t, std::span<const Command> batch) {
				++commits;
				commands += batch.size();
				inside = editor.Universe->IsEntering();
				if (onCommit) onCommit();
			};
			watcher.RecordingStarted = [&](const Recording &) { ++starts; };
			watcher.RecordingFinished = [&](const Recording &, FinishOperation) { ++finishes; };
			editor.Commands->Watch(std::move(watcher));
		}

		static bool RecordingActive(const Editor &editor, std::string_view id) {
			return editor.Commands->IsRecordingInProgress(id);
		}

		static void ClearHistoryWatch(Editor &editor) {
			editor.Commands->Watch({});
		}

		static void WatchCommits(Editor &editor, std::function<void(std::span<const Command>)> callback) {
			CommandLog::Watcher watcher;
			watcher.Committed = [callback =
									 std::move(callback)](uint64_t, std::span<const Command> commands) {
				callback(commands);
			};
			editor.Commands->Watch(std::move(watcher));
		}

		static void ResetHistory(Editor &editor) {
			editor.Commands->ResetWaypoints();
		}

		static void ClearHistory(Editor &editor) {
			editor.Commands->Clear();
		}

		static void ForgetHistoryWorld(Editor &editor, WorldId world) {
			editor.Commands->Forget(world);
		}

		static void SetHistoryEnabled(Editor &editor, bool enabled) {
			editor.Commands->SetEnabled(enabled);
		}

		static bool HistoryEnabled(const Editor &editor) {
			return editor.Commands->Enabled();
		}

		static void WatchFinish(Editor &editor, std::function<void()> callback) {
			CommandLog::Watcher watcher;
			watcher.RecordingFinished = [callback = std::move(callback)](const Recording &, FinishOperation) {
				callback();
			};
			editor.Commands->Watch(std::move(watcher));
		}

		static bool CancelEditorRecording(Editor &editor, std::string_view id) {
			return editor.FinishRecording(id, FinishOperation::Cancel);
		}

		static bool FinishEditorRecording(Editor &editor, std::string_view id, FinishOperation operation) {
			return editor.FinishRecording(id, operation);
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

		static void SelectRoot(Editor &editor, engine::world::WorldId world, bool universe) {
			editor.Selection.clear();
			editor.SelectionWorld = world;
			editor.UniverseSelected = universe;
			editor.SelectedWorldRow = universe ? engine::world::WorldId{} : world;
		}

		static bool HasRootSelection(const Editor &editor) {
			return editor.UniverseSelected || editor.SelectedWorldRow.IsValid();
		}

		static engine::world::WorldId SelectionWorld(const Editor &editor) {
			return editor.SelectionWorld;
		}

		static void QueueWorldActivation(Editor &editor, engine::world::WorldId world) {
			editor.PendingActivate = world;
			editor.PendingRuns.push_back({world, RunMode::Edit, false});
			editor.PendingDuplicateWorld = world;
			editor.PendingTeleport = world;
			editor.PendingWorldState = world;
			editor.PendingRenameWorld = world;
			editor.PendingRenameTo = "stale";
			editor.RenamingWorld = world;
		}

		static bool HasQueuedWorldAction(const Editor &editor) {
			return editor.PendingActivate.IsValid() || !editor.PendingRuns.empty() ||
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

		static void PumpStudioPlugins(Editor &editor) {
			editor.PumpPlugins(0.016f);
		}

		static void ClearFirstScriptRecovery(Editor &editor) {
			editor.Scripts.front().RecoveryId = {};
		}

		static bool FirstScriptHasRecovery(const Editor &editor) {
			return static_cast<bool>(editor.Scripts.front().RecoveryId);
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

TEST_CASE("plugin selection keeps the focused viewport world", "[studio][tools][plugins]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto mainWorld = editor.Universe->Worlds().front();
	const auto otherWorld = editor.Universe->Create({.Name = Name("plugin-selection-other")});
	studio::ToolsProbe::SetActiveWorld(editor, mainWorld);
	editor.Universe->Enter(otherWorld, [](Store &store) { engine::scene::InstallServices(store); });
	studio::ToolsProbe::SelectWorldWorkspace(editor, otherWorld);
	REQUIRE(studio::ToolsProbe::HasWorldSelection(editor, otherWorld));

	studio::LoadedPlugin plugin;
	std::unique_ptr<engine::script::HostSurface> surface;
	Entity workspace;
	editor.Universe->Enter(otherWorld, [&](Store &store) {
		workspace = engine::scene::WorkspaceOf(store);
		surface = studio::MakePluginSurface(editor, plugin, store);
	});
	const auto selection = engine::script::HostValue::List({engine::script::HostValue::Of(workspace)});
	engine::script::HostValue result;
	std::string failure;
	REQUIRE(surface->Call("Selection.Set", std::span(&selection, 1), result, failure));
	CHECK(studio::ToolsProbe::HasWorldSelection(editor, otherWorld));
	studio::ToolsProbe::SelectWorldWorkspace(editor, otherWorld);
	REQUIRE(surface->Call("Selection.Add", std::span(&selection, 1), result, failure));
	CHECK(studio::ToolsProbe::HasWorldSelection(editor, otherWorld));
}

TEST_CASE("plugin selection add replaces root selection", "[studio][tools][plugins]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto world = editor.Universe->Worlds().front();
	studio::ToolsProbe::SetActiveWorld(editor, world);
	studio::LoadedPlugin plugin;
	std::unique_ptr<engine::script::HostSurface> surface;
	Entity workspace;
	editor.Universe->Enter(world, [&](Store &store) {
		engine::scene::InstallServices(store);
		workspace = engine::scene::WorkspaceOf(store);
		surface = studio::MakePluginSurface(editor, plugin, store);
	});
	const auto selection = engine::script::HostValue::List({engine::script::HostValue::Of(workspace)});
	engine::script::HostValue result;
	std::string failure;
	for (bool universe : {true, false}) {
		studio::ToolsProbe::SelectRoot(editor, world, universe);
		REQUIRE(studio::ToolsProbe::HasRootSelection(editor));
		const auto empty = engine::script::HostValue::List({});
		REQUIRE(surface->Call("Selection.Add", std::span(&empty, 1), result, failure));
		CHECK(studio::ToolsProbe::HasRootSelection(editor));
		REQUIRE(surface->Call("Selection.Add", std::span(&selection, 1), result, failure));
		CHECK(studio::ToolsProbe::HasWorldSelection(editor, world));
		CHECK_FALSE(studio::ToolsProbe::HasRootSelection(editor));
	}
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

TEST_CASE("an inactive script does not close another script's completion", "[studio][tools][scripts]") {
	Context context;
	studio::Editor editor;
	studio::OpenScript active;
	active.Instance = Entity(1);
	active.Path = Name("Scripts/first.luau");
	active.Text = "prin";
	active.Edit.Active = true;
	active.Edit.Caret = 4;
	studio::OpenScript inactive;
	inactive.Instance = Entity(2);
	inactive.Path = Name("Scripts/second.luau");
	studio::ToolsProbe::UpdateCompletion(editor, active, true);
	REQUIRE(studio::ToolsProbe::CompletionOpen(editor));
	studio::ToolsProbe::UpdateCompletion(editor, inactive, false);
	CHECK(studio::ToolsProbe::CompletionOpen(editor));
	active.Edit.Active = false;
	inactive.Edit.Active = true;
	inactive.Edit.Caret = 4;
	inactive.Text = "func";
	studio::ToolsProbe::UpdateCompletion(editor, inactive, false);
	CHECK(studio::ToolsProbe::CompletionOpen(editor));
	CHECK(studio::ToolsProbe::CompletionContains(editor, "function"));
	CHECK_FALSE(studio::ToolsProbe::CompletionContains(editor, "print"));
}

TEST_CASE("closing a script retires only its own completion", "[studio][tools][scripts]") {
	studio::Editor editor;
	studio::ToolsProbe::PrepareScriptTabs(editor, 2, 0);
	studio::ToolsProbe::SeedCompletionOwner(editor, 0);
	studio::ToolsProbe::CloseScriptTab(editor, 1);
	CHECK(studio::ToolsProbe::CompletionOpen(editor));
	CHECK(studio::ToolsProbe::ScriptHoverWord(editor) == "cached");
	studio::ToolsProbe::CloseScriptTab(editor, 0);
	CHECK_FALSE(studio::ToolsProbe::CompletionOpen(editor));
	CHECK(studio::ToolsProbe::ScriptHoverWord(editor).empty());
}

TEST_CASE("completion in one script does not hide another script's hover", "[studio][tools][scripts]") {
	Context context;
	studio::Editor editor;
	studio::ToolsProbe::PrepareScriptTabs(editor, 2, 0);
	studio::ToolsProbe::SeedCompletionOwner(editor, 0);
	studio::OpenScript other;
	other.Instance = Entity(2);
	other.Path = Name("Scripts/other.luau");
	other.Text = "print";
	auto &io = ImGui::GetIO();
	io.AddMousePosEvent(30, 30);
	ImGui::NewFrame();
	ImGui::Begin("hover test");
	studio::ToolsProbe::DrawScriptHover(editor, other, ImVec2(20, 20));
	CHECK(studio::ToolsProbe::ScriptHoverWord(editor) == "print");
	studio::OpenScript owner;
	owner.Instance = Entity(1);
	owner.Path = Name("Scripts/owner.luau");
	owner.Text = "print";
	studio::ToolsProbe::DrawScriptHover(editor, owner, ImVec2(20, 20));
	CHECK(studio::ToolsProbe::ScriptHoverWord(editor) == "print");
	ImGui::End();
	ImGui::Render();
}

TEST_CASE("deleting an instance keeps same-ID scripts in other worlds", "[studio][tools][scripts]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto first = editor.Universe->Worlds().front();
	const auto second = editor.Universe->Create({.Name = Name("other")});
	Entity firstInstance;
	editor.Universe->Enter(first, [&](Store &store) {
		firstInstance = store.CreateInstance(engine::scene::PartClass(), "Part");
	});
	studio::ToolsProbe::OpenRuntimeOnlyScript(editor, second);
	const Entity secondInstance = studio::ToolsProbe::FirstScriptInstance(editor);
	REQUIRE(firstInstance == secondInstance);
	REQUIRE(studio::ToolsProbe::HasUnsavedScript(editor));
	studio::ToolsProbe::DeleteSelection(editor, first, firstInstance);
	CHECK(studio::ToolsProbe::HasUnsavedScript(editor));
}

TEST_CASE("deleting a script retains its unsaved editor buffer", "[studio][tools][scripts]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto world = editor.Universe->Worlds().front();
	studio::ToolsProbe::OpenRuntimeOnlyScript(editor, world);
	const Entity script = studio::ToolsProbe::FirstScriptInstance(editor);
	REQUIRE(studio::ToolsProbe::HasUnsavedScript(editor));
	studio::ToolsProbe::DeleteSelection(editor, world, script);
	CHECK(studio::ToolsProbe::HasUnsavedScript(editor));
	editor.Universe->Enter(world, [&](Store &store) { CHECK_FALSE(store.Alive(script)); });
}

TEST_CASE("deleting a parent closes a clean descendant script tab", "[studio][tools][scripts]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto world = editor.Universe->Worlds().front();
	Entity parent;
	editor.Universe->Enter(world, [&](Store &store) {
		parent = store.CreateInstance(engine::scene::PartClass(), "Parent");
	});
	studio::ToolsProbe::OpenRuntimeOnlyScript(editor, world);
	const Entity script = studio::ToolsProbe::FirstScriptInstance(editor);
	studio::ToolsProbe::MarkFirstScriptClean(editor);
	editor.Universe->Enter(world, [&](Store &store) { REQUIRE(store.SetParent(script, parent)); });
	studio::ToolsProbe::DeleteSelection(editor, world, parent);
	CHECK(studio::ToolsProbe::ScriptCount(editor) == 0);
}

TEST_CASE("moving a subtree preserves an unsaved descendant script", "[studio][tools][scripts]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto source = editor.Universe->Worlds().front();
	const auto target = editor.Universe->Create({.Name = Name("script-move-target")});
	Entity parent;
	editor.Universe->Enter(source, [&](Store &store) {
		parent = store.CreateInstance(engine::scene::PartClass(), "ScriptParent");
		if (store.Resource<engine::script::SourceCache>() == nullptr)
			store.SetResource(engine::script::SourceCache{});
		store.ResourceMutable<engine::script::SourceCache>()->Set(
			Name("Scripts/runtime-only.luau"), "saved source"
		);
	});
	studio::ToolsProbe::OpenRuntimeOnlyScript(editor, source);
	const Entity script = studio::ToolsProbe::FirstScriptInstance(editor);
	editor.Universe->Enter(source, [&](Store &store) { REQUIRE(store.SetParent(script, parent)); });
	REQUIRE(studio::ToolsProbe::HasUnsavedScript(editor));
	CHECK_FALSE(studio::ToolsProbe::MoveScriptParent(editor, source, parent, engine::world::WorldId(123456)));
	CHECK(studio::ToolsProbe::HasUnsavedScript(editor));
	editor.Universe->Enter(source, [&](Store &store) {
		std::string text;
		std::string error;
		REQUIRE(engine::script::ReadSource(store, Name("Scripts/runtime-only.luau"), text, error));
		CHECK(text == "saved source");
	});
	REQUIRE(studio::ToolsProbe::MoveScriptParent(editor, source, parent, target));
	editor.Universe->Enter(target, [&](Store &store) {
		const Entity moved = store.FindFirstRoot("ScriptParent");
		REQUIRE(moved != NULL_ENTITY);
		const Entity movedScript = store.FindFirstChild(moved, "RuntimeOnly");
		REQUIRE(movedScript != NULL_ENTITY);
		std::string text;
		std::string error;
		REQUIRE(
			engine::script::ReadSource(store, engine::script::ActiveSourceOf(store, movedScript), text, error)
		);
		CHECK(text == "unsaved source");
	});
}

TEST_CASE("moving scripts preserves conflicting source paths and edits", "[studio][tools][scripts]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto source = editor.Universe->Worlds().front();
	const auto target = editor.Universe->Create({.Name = Name("script-move-conflict")});
	const Name path("Scripts/runtime-only.luau");
	Entity parent;
	editor.Universe->Enter(source, [&](Store &store) {
		parent = store.CreateInstance(engine::scene::PartClass(), "ScriptParent");
		if (store.Resource<engine::script::SourceCache>() == nullptr)
			store.SetResource(engine::script::SourceCache{});
		store.ResourceMutable<engine::script::SourceCache>()->Set(path, "saved source");
	});
	studio::ToolsProbe::OpenRuntimeOnlyScript(editor, source);
	const Entity first = studio::ToolsProbe::LastMoveScriptInstance(editor);
	editor.Universe->Enter(source, [&](Store &store) { REQUIRE(store.SetParent(first, parent)); });

	SECTION("target source is different") {
		Entity targetScript;
		editor.Universe->Enter(target, [&](Store &store) {
			targetScript = store.CreateInstance(engine::script::ScriptClass(), "Existing");
			engine::script::SetSourcePath(store, targetScript, path);
			if (store.Resource<engine::script::SourceCache>() == nullptr)
				store.SetResource(engine::script::SourceCache{});
			store.ResourceMutable<engine::script::SourceCache>()->Set(path, "target source");
		});
		REQUIRE(studio::ToolsProbe::MoveScriptParent(editor, source, parent, target));
		editor.Universe->Enter(target, [&](Store &store) {
			std::string text;
			std::string error;
			CHECK(store.Alive(targetScript));
			REQUIRE(engine::script::ReadSource(store, path, text, error));
			CHECK(text == "target source");
			const Entity moved = store.FindFirstRoot("ScriptParent");
			REQUIRE(moved != NULL_ENTITY);
			const Entity script = store.FindFirstChild(moved, "RuntimeOnly");
			REQUIRE(script != NULL_ENTITY);
			REQUIRE(
				engine::script::ReadSource(store, engine::script::ActiveSourceOf(store, script), text, error)
			);
			CHECK(text == "unsaved source");
		});
	}
	SECTION("source path changes after opening the tab") {
		const Name newPath("Scripts/renamed.luau");
		editor.Universe->Enter(source, [&](Store &store) {
			engine::script::SetSourcePath(store, first, newPath);
			store.ResourceMutable<engine::script::SourceCache>()->Set(newPath, "renamed source");
		});
		CHECK_FALSE(studio::ToolsProbe::MoveScriptParent(editor, source, parent, target));
		CHECK(studio::ToolsProbe::HasUnsavedScript(editor));
		editor.Universe->Enter(source, [&](Store &store) {
			CHECK(store.Alive(first));
			CHECK(engine::script::ActiveSourceOf(store, first) == newPath);
			std::string text;
			std::string error;
			REQUIRE(engine::script::ReadSource(store, newPath, text, error));
			CHECK(text == "renamed source");
		});
		editor.Universe->Enter(target, [&](Store &store) {
			CHECK(store.FindFirstRoot("ScriptParent") == NULL_ENTITY);
		});
	}

	SECTION("dirty target tab does not later alter the moved script") {
		editor.Universe->Enter(target, [&](Store &store) {
			if (store.Resource<engine::script::SourceCache>() == nullptr)
				store.SetResource(engine::script::SourceCache{});
			store.ResourceMutable<engine::script::SourceCache>()->Set(path, "unsaved source");
		});
		studio::ToolsProbe::OpenRuntimeOnlyScript(editor, target);
		studio::ToolsProbe::SetLastMoveScriptText(editor, "target unsaved source");
		REQUIRE(studio::ToolsProbe::MoveScriptParent(editor, source, parent, target));
		REQUIRE(studio::ToolsProbe::SaveFirstScript(editor));
		editor.Universe->Enter(target, [&](Store &store) {
			const Entity moved = store.FindFirstRoot("ScriptParent");
			REQUIRE(moved != NULL_ENTITY);
			const Entity script = store.FindFirstChild(moved, "RuntimeOnly");
			REQUIRE(script != NULL_ENTITY);
			std::string text;
			std::string error;
			REQUIRE(
				engine::script::ReadSource(store, engine::script::ActiveSourceOf(store, script), text, error)
			);
			CHECK(text == "unsaved source");
			REQUIRE(engine::script::ReadSource(store, path, text, error));
			CHECK(text == "target unsaved source");
		});
	}
	SECTION("both languages originally share one path") {
		editor.Universe->Enter(source, [&](Store &store) {
			store.Set(first, engine::script::JavaScriptSourceContainer{.Path = path});
			store.Set(
				first,
				engine::script::CodeSourceContainerSelector{.Active = engine::script::Language::JavaScript}
			);
		});
		REQUIRE(studio::ToolsProbe::MoveScriptParent(editor, source, parent, target));
		editor.Universe->Enter(target, [&](Store &store) {
			const Entity moved = store.FindFirstRoot("ScriptParent");
			REQUIRE(moved != NULL_ENTITY);
			const Entity script = store.FindFirstChild(moved, "RuntimeOnly");
			REQUIRE(script != NULL_ENTITY);
			const auto *lua = store.Get<engine::script::LuaSourceContainer>(script);
			const auto *js = store.Get<engine::script::JavaScriptSourceContainer>(script);
			REQUIRE(lua != nullptr);
			REQUIRE(js != nullptr);
			std::string text;
			std::string error;
			REQUIRE(engine::script::ReadSource(store, lua->Path, text, error));
			CHECK(text == "unsaved source");
			REQUIRE(engine::script::ReadSource(store, js->Path, text, error));
			CHECK(text == "saved source");
			CHECK(engine::script::ActiveLanguageOf(store, script) == engine::script::Language::JavaScript);
		});
	}
	SECTION("inactive language source is retained") {
		const Name jsPath("Scripts/active.js");
		editor.Universe->Enter(source, [&](Store &store) {
			engine::script::SetSourcePath(store, first, jsPath);
			store.ResourceMutable<engine::script::SourceCache>()->Set(jsPath, "javascript source");
		});
		studio::ToolsProbe::MarkFirstScriptClean(editor);
		REQUIRE(studio::ToolsProbe::MoveScriptParent(editor, source, parent, target));
		editor.Universe->Enter(target, [&](Store &store) {
			const Entity moved = store.FindFirstRoot("ScriptParent");
			REQUIRE(moved != NULL_ENTITY);
			const Entity script = store.FindFirstChild(moved, "RuntimeOnly");
			REQUIRE(script != NULL_ENTITY);
			const auto *lua = store.Get<engine::script::LuaSourceContainer>(script);
			REQUIRE(lua != nullptr);
			std::string text;
			std::string error;
			REQUIRE(engine::script::ReadSource(store, lua->Path, text, error));
			CHECK(text == "saved source");
			REQUIRE(
				engine::script::ReadSource(store, engine::script::ActiveSourceOf(store, script), text, error)
			);
			CHECK(text == "javascript source");
		});
	}
	SECTION("active language changes after opening the tab") {
		const Name jsPath("Scripts/switched.js");
		editor.Universe->Enter(source, [&](Store &store) {
			engine::script::SetSourcePath(store, first, jsPath);
			store.ResourceMutable<engine::script::SourceCache>()->Set(jsPath, "javascript source");
		});
		REQUIRE(studio::ToolsProbe::MoveScriptParent(editor, source, parent, target));
		editor.Universe->Enter(target, [&](Store &store) {
			const Entity moved = store.FindFirstRoot("ScriptParent");
			REQUIRE(moved != NULL_ENTITY);
			const Entity script = store.FindFirstChild(moved, "RuntimeOnly");
			REQUIRE(script != NULL_ENTITY);
			CHECK(engine::script::ActiveSourceOf(store, script) == jsPath);
			const auto *lua = store.Get<engine::script::LuaSourceContainer>(script);
			REQUIRE(lua != nullptr);
			std::string text;
			std::string error;
			REQUIRE(engine::script::ReadSource(store, lua->Path, text, error));
			CHECK(text == "unsaved source");
			REQUIRE(engine::script::ReadSource(store, jsPath, text, error));
			CHECK(text == "javascript source");
		});
	}
	SECTION("dirty scripts share a source path") {
		studio::ToolsProbe::OpenRuntimeOnlyScript(editor, source);
		const Entity second = studio::ToolsProbe::LastMoveScriptInstance(editor);
		studio::ToolsProbe::SetLastMoveScriptText(editor, "second unsaved source");
		editor.Universe->Enter(source, [&](Store &store) { REQUIRE(store.SetParent(second, parent)); });
		REQUIRE(studio::ToolsProbe::MoveScriptParent(editor, source, parent, target));
		editor.Universe->Enter(target, [&](Store &store) {
			const Entity moved = store.FindFirstRoot("ScriptParent");
			REQUIRE(moved != NULL_ENTITY);
			bool firstText = false;
			bool secondText = false;
			store.EachChild(moved, [&](Entity child) {
				std::string text;
				std::string error;
				REQUIRE(
					engine::script::ReadSource(
						store, engine::script::ActiveSourceOf(store, child), text, error
					)
				);
				firstText |= text == "unsaved source";
				secondText |= text == "second unsaved source";
			});
			CHECK(firstText);
			CHECK(secondText);
		});
	}
}

TEST_CASE(
	"moving a pathless dirty script after changing language preserves both bodies", "[studio][tools][scripts]"
) {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto source = editor.Universe->Worlds().front();
	const auto target = editor.Universe->Create({.Name = Name("pathless-target")});
	studio::ToolsProbe::OpenRuntimeOnlyScript(editor, source, false);
	const Entity script = studio::ToolsProbe::FirstScriptInstance(editor);
	const Name jsPath("Scripts/switched.js");
	editor.Universe->Enter(source, [&](Store &store) {
		engine::script::SetSourcePath(store, script, jsPath);
		if (store.Resource<engine::script::SourceCache>() == nullptr)
			store.SetResource(engine::script::SourceCache{});
		store.ResourceMutable<engine::script::SourceCache>()->Set(jsPath, "javascript source");
	});
	REQUIRE(studio::ToolsProbe::MoveScriptParent(editor, source, script, target));
	editor.Universe->Enter(target, [&](Store &store) {
		const Entity moved = store.FindFirstRoot("RuntimeOnly");
		REQUIRE(moved != NULL_ENTITY);
		CHECK(engine::script::ActiveLanguageOf(store, moved) == engine::script::Language::JavaScript);
		const auto *lua = store.Get<engine::script::LuaSourceContainer>(moved);
		REQUIRE(lua != nullptr);
		CHECK(lua->Path.IsValid());
		std::string text;
		std::string error;
		REQUIRE(engine::script::ReadSource(store, lua->Path, text, error));
		CHECK(text == "unsaved source");
		REQUIRE(engine::script::ReadSource(store, jsPath, text, error));
		CHECK(text == "javascript source");
	});
}

TEST_CASE(
	"moving a JavaScript tab with a shared language path preserves both bodies", "[studio][tools][scripts]"
) {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto source = editor.Universe->Worlds().front();
	const auto target = editor.Universe->Create({.Name = Name("shared-language-target")});
	const Name path("Scripts/shared.js");
	Entity script;
	editor.Universe->Enter(source, [&](Store &store) {
		script = store.CreateInstance(engine::script::ScriptClass(), "SharedScript");
		engine::script::SetSourcePath(store, script, path);
		if (store.Resource<engine::script::SourceCache>() == nullptr)
			store.SetResource(engine::script::SourceCache{});
		store.ResourceMutable<engine::script::SourceCache>()->Set(path, "original source");
	});
	studio::ToolsProbe::OpenMoveScript(editor, source, script);
	editor.Universe->Enter(source, [&](Store &store) {
		store.Set(script, engine::script::LuaSourceContainer{.Path = path});
		store.Set(
			script, engine::script::CodeSourceContainerSelector{.Active = engine::script::Language::Luau}
		);
	});
	REQUIRE(studio::ToolsProbe::MoveScriptParent(editor, source, script, target));
	editor.Universe->Enter(target, [&](Store &store) {
		const Entity moved = store.FindFirstRoot("SharedScript");
		REQUIRE(moved != NULL_ENTITY);
		const auto *lua = store.Get<engine::script::LuaSourceContainer>(moved);
		const auto *js = store.Get<engine::script::JavaScriptSourceContainer>(moved);
		REQUIRE(lua != nullptr);
		REQUIRE(js != nullptr);
		std::string text;
		std::string error;
		REQUIRE(engine::script::ReadSource(store, lua->Path, text, error));
		CHECK(text == "original source");
		REQUIRE(engine::script::ReadSource(store, js->Path, text, error));
		CHECK(text == "edited javascript");
		CHECK(engine::script::ActiveLanguageOf(store, moved) == engine::script::Language::Luau);
	});
}

TEST_CASE("saving a pathless tab after a language switch preserves both bodies", "[studio][tools][scripts]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto world = editor.Universe->Worlds().front();
	studio::ToolsProbe::OpenRuntimeOnlyScript(editor, world, false);
	const Entity script = studio::ToolsProbe::FirstScriptInstance(editor);
	const Name jsPath("Scripts/switched.js");
	editor.Universe->Enter(world, [&](Store &store) {
		engine::script::SetSourcePath(store, script, jsPath);
		if (store.Resource<engine::script::SourceCache>() == nullptr)
			store.SetResource(engine::script::SourceCache{});
		store.ResourceMutable<engine::script::SourceCache>()->Set(jsPath, "javascript source");
	});
	REQUIRE(studio::ToolsProbe::SaveFirstScript(editor));
	editor.Universe->Enter(world, [&](Store &store) {
		CHECK(engine::script::ActiveLanguageOf(store, script) == engine::script::Language::JavaScript);
		const auto *lua = store.Get<engine::script::LuaSourceContainer>(script);
		REQUIRE(lua != nullptr);
		CHECK(lua->Path.IsValid());
		std::string text;
		std::string error;
		REQUIRE(engine::script::ReadSource(store, lua->Path, text, error));
		CHECK(text == "unsaved source");
		REQUIRE(engine::script::ReadSource(store, jsPath, text, error));
		CHECK(text == "javascript source");
	});
}

TEST_CASE("saving a tab with a shared path preserves the other language", "[studio][tools][scripts]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto world = editor.Universe->Worlds().front();
	const Name path("Scripts/shared.js");
	Entity script;
	editor.Universe->Enter(world, [&](Store &store) {
		script = store.CreateInstance(engine::script::ScriptClass(), "SharedScript");
		engine::script::SetSourcePath(store, script, path);
		if (store.Resource<engine::script::SourceCache>() == nullptr)
			store.SetResource(engine::script::SourceCache{});
		store.ResourceMutable<engine::script::SourceCache>()->Set(path, "original source");
	});
	studio::ToolsProbe::OpenMoveScript(editor, world, script);
	editor.Universe->Enter(world, [&](Store &store) {
		store.Set(script, engine::script::LuaSourceContainer{.Path = path});
		store.Set(
			script, engine::script::CodeSourceContainerSelector{.Active = engine::script::Language::Luau}
		);
	});
	REQUIRE(studio::ToolsProbe::SaveFirstScript(editor));
	editor.Universe->Enter(world, [&](Store &store) {
		CHECK(engine::script::ActiveLanguageOf(store, script) == engine::script::Language::Luau);
		const auto *lua = store.Get<engine::script::LuaSourceContainer>(script);
		const auto *js = store.Get<engine::script::JavaScriptSourceContainer>(script);
		REQUIRE(lua != nullptr);
		REQUIRE(js != nullptr);
		std::string text;
		std::string error;
		REQUIRE(engine::script::ReadSource(store, lua->Path, text, error));
		CHECK(text == "original source");
		REQUIRE(engine::script::ReadSource(store, js->Path, text, error));
		CHECK(text == "edited javascript");
	});
}

TEST_CASE("saving a stale source tab preserves a newly assigned program", "[studio][tools][scripts]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto world = editor.Universe->Worlds().front();
	studio::ToolsProbe::OpenRuntimeOnlyScript(editor, world);
	const Entity script = studio::ToolsProbe::FirstScriptInstance(editor);
	const Name newPath("Scripts/reassigned.luau");
	editor.Universe->Enter(world, [&](Store &store) {
		engine::script::SetSourcePath(store, script, newPath);
		if (store.Resource<engine::script::SourceCache>() == nullptr)
			store.SetResource(engine::script::SourceCache{});
		store.ResourceMutable<engine::script::SourceCache>()->Set(newPath, "reassigned source");
	});
	CHECK_FALSE(studio::ToolsProbe::SaveFirstScript(editor));
	CHECK(studio::ToolsProbe::HasUnsavedScript(editor));
	editor.Universe->Enter(world, [&](Store &store) {
		CHECK(engine::script::ActiveSourceOf(store, script) == newPath);
		std::string text;
		std::string error;
		REQUIRE(engine::script::ReadSource(store, newPath, text, error));
		CHECK(text == "reassigned source");
	});
}

TEST_CASE("source changed while a dirty tab is open is not overwritten", "[studio][tools][scripts]") {
	const auto exercise = [](bool shader) {
		Context context;
		TemporaryConfig config;
		engine::scene::RegisterSceneClasses();
		studio::Editor editor;
		studio::ToolsProbe::PrepareUnsavedGame(editor);
		const auto source = editor.Universe->Worlds().front();
		const auto target = editor.Universe->Create({.Name = Name("changed-source-target")});
		const Name path("Scripts/changed.js");
		Entity script;
		editor.Universe->Enter(source, [&](Store &store) {
			script = store.CreateInstance(
				shader ? engine::scene::ShaderScriptClass() : engine::script::ScriptClass(), "EditedSource"
			);
			if (shader)
				REQUIRE(engine::scene::SetShaderSource(store, script, "original source"));
			else {
				engine::script::SetSourcePath(store, script, path);
				if (store.Resource<engine::script::SourceCache>() == nullptr)
					store.SetResource(engine::script::SourceCache{});
				store.ResourceMutable<engine::script::SourceCache>()->Set(path, "original source");
			}
		});
		studio::ToolsProbe::OpenMoveScript(editor, source, script);
		editor.Universe->Enter(source, [&](Store &store) {
			if (shader)
				REQUIRE(engine::scene::SetShaderSource(store, script, "new source"));
			else
				store.ResourceMutable<engine::script::SourceCache>()->Set(path, "new source");
		});
		SECTION("save") {
			CHECK_FALSE(studio::ToolsProbe::SaveFirstScript(editor));
		}
		SECTION("move") {
			CHECK_FALSE(studio::ToolsProbe::MoveScriptParent(editor, source, script, target));
			editor.Universe->Enter(target, [&](Store &store) {
				CHECK(store.FindFirstRoot("EditedSource") == NULL_ENTITY);
			});
		}
		CHECK(studio::ToolsProbe::MoveScriptEditIntact(editor));
		editor.Universe->Enter(source, [&](Store &store) {
			CHECK(store.Alive(script));
			if (shader) {
				const auto *sourceRow = store.Get<engine::scene::ShaderSource>(script);
				REQUIRE(sourceRow != nullptr);
				CHECK(sourceRow->Code == "new source");
			} else {
				std::string text;
				std::string error;
				REQUIRE(engine::script::ReadSource(store, path, text, error));
				CHECK(text == "new source");
			}
		});
	};
	SECTION("program") {
		exercise(false);
	}
	SECTION("shader") {
		exercise(true);
	}
}

TEST_CASE("an unreadable source cannot be overwritten by save or move", "[studio][tools][scripts]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto source = editor.Universe->Worlds().front();
	const auto target = editor.Universe->Create({.Name = Name("unreadable-target")});
	const Name path("Scripts/nonexistent-baseline-source-9a8.luau");
	Entity script;
	editor.Universe->Enter(source, [&](Store &store) {
		script = store.CreateInstance(engine::script::ScriptClass(), "Unreadable");
		engine::script::SetSourcePath(store, script, path);
		std::string text;
		std::string error;
		REQUIRE_FALSE(engine::script::ReadSource(store, path, text, error));
	});
	studio::ToolsProbe::OpenMoveScript(editor, source, script);
	SECTION("save") {
		CHECK_FALSE(studio::ToolsProbe::SaveFirstScript(editor));
	}
	SECTION("move") {
		CHECK_FALSE(studio::ToolsProbe::MoveScriptParent(editor, source, script, target));
	}
	CHECK(studio::ToolsProbe::MoveScriptEditIntact(editor));
	editor.Universe->Enter(source, [&](Store &store) {
		CHECK(store.Alive(script));
		const auto *cache = store.Resource<engine::script::SourceCache>();
		CHECK((cache == nullptr || cache->Find(path) == nullptr));
	});
}

TEST_CASE(
	"moving an implicit Luau script preserves its language and containers", "[studio][tools][scripts]"
) {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto source = editor.Universe->Worlds().front();
	const auto target = editor.Universe->Create({.Name = Name("implicit-language-target")});
	const Name path("Scripts/implicit.js");
	Entity script;
	editor.Universe->Enter(source, [&](Store &store) {
		script = store.CreateInstance(engine::script::ScriptClass(), "ImplicitLanguage");
		store.Set(script, engine::script::LuaSourceContainer{.Path = path});
		REQUIRE(store.Get<engine::script::CodeSourceContainerSelector>(script) == nullptr);
		REQUIRE(store.Get<engine::script::JavaScriptSourceContainer>(script) == nullptr);
		if (store.Resource<engine::script::SourceCache>() == nullptr)
			store.SetResource(engine::script::SourceCache{});
		store.ResourceMutable<engine::script::SourceCache>()->Set(path, "luau body");
	});
	REQUIRE(studio::ToolsProbe::MoveScriptParent(editor, source, script, target));
	editor.Universe->Enter(target, [&](Store &store) {
		const Entity moved = store.FindFirstRoot("ImplicitLanguage");
		REQUIRE(moved != NULL_ENTITY);
		CHECK(engine::script::ActiveLanguageOf(store, moved) == engine::script::Language::Luau);
		CHECK(store.Get<engine::script::JavaScriptSourceContainer>(moved) == nullptr);
		const auto *lua = store.Get<engine::script::LuaSourceContainer>(moved);
		REQUIRE(lua != nullptr);
		std::string text;
		std::string error;
		REQUIRE(engine::script::ReadSource(store, lua->Path, text, error));
		CHECK(text == "luau body");
	});
}

TEST_CASE("deleting a selected parent and child is one undo step", "[studio][tools][delete]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::PrepareCommands(editor);
	const auto world = editor.Universe->Worlds().front();
	Entity parent;
	Entity child;
	editor.Universe->Enter(world, [&](Store &store) {
		parent = store.CreateInstance(engine::scene::PartClass(), "SelectedParent");
		child = store.CreateInstance(engine::scene::PartClass(), "SelectedChild");
		REQUIRE(store.SetParent(child, parent));
	});
	std::vector<Entity> selected;
	SECTION("parent first") {
		selected = {parent, child};
	}
	SECTION("child first") {
		selected = {child, parent};
	}
	studio::ToolsProbe::DeleteSelection(editor, world, selected);
	editor.Universe->Enter(world, [&](Store &store) {
		CHECK_FALSE(store.Alive(parent));
		CHECK_FALSE(store.Alive(child));
	});
	REQUIRE(studio::ToolsProbe::CanUndo(editor));
	studio::ToolsProbe::UndoEdit(editor);
	editor.Universe->Enter(world, [&](Store &store) {
		const Entity restored = store.FindFirstRoot("SelectedParent");
		REQUIRE(restored != NULL_ENTITY);
		CHECK(store.FindFirstChild(restored, "SelectedChild") != NULL_ENTITY);
	});
	CHECK_FALSE(studio::ToolsProbe::CanUndo(editor));
}

TEST_CASE("selection tools undo a multi-part action in one step", "[studio][tools][history]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::PrepareCommands(editor);
	const auto world = editor.Universe->Worlds().front();
	Entity previous;
	Entity first;
	Entity second;
	const char *property = nullptr;
	SECTION("anchor") {
		property = "Anchored";
	}
	SECTION("lock") {
		property = "Locked";
	}
	SECTION("reset pivot") {}
	editor.Universe->Enter(world, [&](Store &store) {
		previous = store.CreateInstance(engine::scene::PartClass(), "EarlierEdit");
		first = store.CreateInstance(engine::scene::PartClass(), "First");
		second = store.CreateInstance(engine::scene::PartClass(), "Second");
		if (property != nullptr) {
			const bool value = false;
			REQUIRE(store.SetProperty(first, Name(property), &value, sizeof(value)));
			REQUIRE(store.SetProperty(second, Name(property), &value, sizeof(value)));
		} else {
			engine::core::CFrame offset;
			offset.Position = Vector3{1.0f, 0.0f, 0.0f};
			REQUIRE(store.SetProperty(first, Name("PivotOffset"), &offset, sizeof(offset)));
			offset.Position = Vector3{2.0f, 0.0f, 0.0f};
			REQUIRE(store.SetProperty(second, Name("PivotOffset"), &offset, sizeof(offset)));
		}
	});
	studio::ToolsProbe::RecordCreate(editor, world, previous, NULL_ENTITY);
	size_t commits = 0;
	size_t commands = 0;
	studio::ToolsProbe::WatchCommits(editor, [&](std::span<const studio::Command> batch) {
		++commits;
		commands += batch.size();
	});
	studio::ToolsProbe::ApplySelectionTool(editor, world, {first, second}, property);
	CHECK(commits == 1);
	CHECK(commands == 2);
	REQUIRE(studio::ToolsProbe::CanUndo(editor));
	studio::ToolsProbe::UndoEdit(editor);
	editor.Universe->Enter(world, [&](Store &store) {
		CHECK(store.Alive(previous));
		if (property != nullptr) {
			bool a = true;
			bool b = true;
			REQUIRE(store.GetProperty(first, Name(property), &a, sizeof(a)));
			REQUIRE(store.GetProperty(second, Name(property), &b, sizeof(b)));
			CHECK_FALSE(a);
			CHECK_FALSE(b);
		} else {
			engine::core::CFrame a;
			engine::core::CFrame b;
			REQUIRE(store.GetProperty(first, Name("PivotOffset"), &a, sizeof(a)));
			REQUIRE(store.GetProperty(second, Name("PivotOffset"), &b, sizeof(b)));
			CHECK(a.Position.X == 1.0f);
			CHECK(b.Position.X == 2.0f);
		}
	});
	REQUIRE(studio::ToolsProbe::CanUndo(editor));
	studio::ToolsProbe::UndoEdit(editor);
	editor.Universe->Enter(world, [&](Store &store) { CHECK_FALSE(store.Alive(previous)); });
	studio::ToolsProbe::ClearHistoryWatch(editor);
}

TEST_CASE("selection tools preserve redo on a no-op and join an open recording", "[studio][tools][history]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::PrepareCommands(editor);
	const auto world = editor.Universe->Worlds().front();
	Entity first;
	Entity second;
	Entity previous;
	editor.Universe->Enter(world, [&](Store &store) {
		first = store.CreateInstance(engine::scene::PartClass(), "First");
		second = store.CreateInstance(engine::scene::PartClass(), "Second");
		previous = store.CreateInstance(engine::scene::PartClass(), "Previous");
	});
	SECTION("no-op preserves redo") {
		const bool value = true;
		editor.Universe->Enter(world, [&](Store &store) {
			REQUIRE(store.SetProperty(first, Name("Locked"), &value, sizeof(value)));
			REQUIRE(store.SetProperty(second, Name("Locked"), &value, sizeof(value)));
		});
		studio::ToolsProbe::RecordCreate(editor, world, previous, NULL_ENTITY);
		studio::ToolsProbe::UndoEdit(editor);
		REQUIRE(studio::ToolsProbe::CanRedo(editor));
		studio::ToolsProbe::ApplySelectionTool(editor, world, {first, second}, "Locked");
		CHECK(studio::ToolsProbe::CanRedo(editor));
	}
	SECTION("an open recording owns the change") {
		const bool value = false;
		editor.Universe->Enter(world, [&](Store &store) {
			REQUIRE(store.SetProperty(first, Name("Locked"), &value, sizeof(value)));
			REQUIRE(store.SetProperty(second, Name("Locked"), &value, sizeof(value)));
		});
		const auto recording = studio::ToolsProbe::BeginRecording(editor);
		REQUIRE(recording);
		studio::ToolsProbe::ApplySelectionTool(editor, world, {first, second}, "Locked");
		CHECK(studio::ToolsProbe::RecordingActive(editor, *recording));
		REQUIRE(
			studio::ToolsProbe::FinishEditorRecording(editor, *recording, studio::FinishOperation::Commit)
		);
		studio::ToolsProbe::UndoEdit(editor);
		editor.Universe->Enter(world, [&](Store &store) {
			bool a = true;
			bool b = true;
			REQUIRE(store.GetProperty(first, Name("Locked"), &a, sizeof(a)));
			REQUIRE(store.GetProperty(second, Name("Locked"), &b, sizeof(b)));
			CHECK_FALSE(a);
			CHECK_FALSE(b);
		});
	}
}

TEST_CASE("selection tools do not record replica edits in authority history", "[studio][tools][history]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::PrepareCommands(editor);
	const auto authority = editor.Universe->Worlds().front();
	Entity previous;
	editor.Universe->Enter(authority, [&](Store &store) {
		previous = store.CreateInstance(engine::scene::PartClass(), "Previous");
	});
	studio::ToolsProbe::RecordCreate(editor, authority, previous, NULL_ENTITY);
	const auto replica = studio::ToolsProbe::StartReplica(editor, authority);
	Entity first;
	Entity second;
	editor.Universe->Enter(replica, [&](Store &store) {
		first = store.CreatePredictedInstance(engine::scene::PartClass(), "First");
		second = store.CreatePredictedInstance(engine::scene::PartClass(), "Second");
	});
	studio::ToolsProbe::ApplySelectionTool(editor, replica, {first, second}, "Locked");
	studio::ToolsProbe::UndoEdit(editor);
	editor.Universe->Enter(authority, [&](Store &store) { CHECK_FALSE(store.Alive(previous)); });
	editor.Universe->Enter(replica, [&](Store &store) {
		bool a = false;
		bool b = false;
		REQUIRE(store.GetProperty(first, Name("Locked"), &a, sizeof(a)));
		REQUIRE(store.GetProperty(second, Name("Locked"), &b, sizeof(b)));
		CHECK(a);
		CHECK(b);
	});
}

TEST_CASE("deleting mixed selected subtrees preserves the preceding undo step", "[studio][tools][delete]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::PrepareCommands(editor);
	const auto world = editor.Universe->Worlds().front();
	Entity previous;
	Entity parent;
	Entity child;
	Entity sibling;
	editor.Universe->Enter(world, [&](Store &store) {
		previous = store.CreateInstance(engine::scene::PartClass(), "EarlierEdit");
		parent = store.CreateInstance(engine::scene::PartClass(), "SelectedParent");
		child = store.CreateInstance(engine::scene::PartClass(), "SelectedChild");
		sibling = store.CreateInstance(engine::scene::PartClass(), "SelectedSibling");
		REQUIRE(store.SetParent(child, parent));
	});
	studio::ToolsProbe::RecordCreate(editor, world, previous, NULL_ENTITY);
	size_t commits = 0;
	size_t commands = 0;
	size_t starts = 0;
	size_t finishes = 0;
	bool inside = true;
	bool completeAtCommit = false;
	studio::ToolsProbe::WatchDeleteHistory(editor, commits, commands, starts, finishes, inside, [&] {
		editor.Universe->Enter(world, [&](Store &store) {
			completeAtCommit =
				store.Alive(previous) && !store.Alive(parent) && !store.Alive(child) && !store.Alive(sibling);
		});
	});
	studio::ToolsProbe::DeleteSelection(editor, world, {child, sibling, parent});
	CHECK(commits == 1);
	CHECK(commands == 2);
	CHECK(starts == 0);
	CHECK(finishes == 0);
	CHECK_FALSE(inside);
	CHECK(completeAtCommit);
	studio::ToolsProbe::UndoEdit(editor);
	editor.Universe->Enter(world, [&](Store &store) {
		CHECK(store.Alive(previous));
		const Entity restored = store.FindFirstRoot("SelectedParent");
		REQUIRE(restored != NULL_ENTITY);
		CHECK(store.FindFirstChild(restored, "SelectedChild") != NULL_ENTITY);
		CHECK(store.FindFirstRoot("SelectedSibling") != NULL_ENTITY);
	});
	REQUIRE(studio::ToolsProbe::CanUndo(editor));
	studio::ToolsProbe::UndoEdit(editor);
	editor.Universe->Enter(world, [&](Store &store) { CHECK_FALSE(store.Alive(previous)); });
	studio::ToolsProbe::ClearHistoryWatch(editor);
}

TEST_CASE("deleting a selection respects an open recording", "[studio][tools][delete]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::PrepareCommands(editor);
	const auto world = editor.Universe->Worlds().front();
	Entity first;
	Entity second;
	editor.Universe->Enter(world, [&](Store &store) {
		first = store.CreateInstance(engine::scene::PartClass(), "First");
		second = store.CreateInstance(engine::scene::PartClass(), "Second");
	});
	const auto recording = studio::ToolsProbe::BeginRecording(editor);
	REQUIRE(recording.has_value());
	size_t commits = 0;
	size_t commands = 0;
	size_t starts = 0;
	size_t finishes = 0;
	bool inside = false;
	studio::ToolsProbe::WatchDeleteHistory(editor, commits, commands, starts, finishes, inside);
	studio::ToolsProbe::DeleteSelection(editor, world, {first, second});
	CHECK(studio::ToolsProbe::RecordingActive(editor, *recording));
	CHECK(commits == 0);
	CHECK(starts == 0);
	CHECK(finishes == 0);
	REQUIRE(studio::ToolsProbe::CancelRecording(editor, *recording));
	editor.Universe->Enter(world, [&](Store &store) {
		CHECK(store.FindFirstRoot("First") != NULL_ENTITY);
		CHECK(store.FindFirstRoot("Second") != NULL_ENTITY);
	});
	studio::ToolsProbe::ClearHistoryWatch(editor);
}

TEST_CASE("a protected-only delete preserves redo", "[studio][tools][delete]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::PrepareCommands(editor);
	const auto world = editor.Universe->Worlds().front();
	Entity previous;
	Entity protectedPart;
	editor.Universe->Enter(world, [&](Store &store) {
		previous = store.CreateInstance(engine::scene::PartClass(), "EarlierEdit");
		protectedPart = store.CreateInstance(engine::scene::PartClass(), "ProtectedPart");
		store.Protect(protectedPart);
	});
	studio::ToolsProbe::RecordCreate(editor, world, previous, NULL_ENTITY);
	studio::ToolsProbe::UndoEdit(editor);
	REQUIRE(studio::ToolsProbe::CanRedo(editor));
	studio::ToolsProbe::DeleteSelection(editor, world, protectedPart);
	CHECK(studio::ToolsProbe::CanRedo(editor));
	studio::ToolsProbe::RedoEdit(editor);
	editor.Universe->Enter(world, [&](Store &store) {
		CHECK(store.FindFirstRoot("EarlierEdit") != NULL_ENTITY);
		CHECK(store.Alive(protectedPart));
	});
}

TEST_CASE("deleting a selection blocks history reentry during removal", "[studio][tools][delete]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::PrepareCommands(editor);
	const auto world = editor.Universe->Worlds().front();
	Entity previous;
	Entity parent;
	Entity first;
	Entity second;
	editor.Universe->Enter(world, [&](Store &store) {
		previous = store.CreateInstance(engine::scene::PartClass(), "EarlierEdit");
		parent = store.CreateInstance(engine::scene::PartClass(), "Parent");
		first = store.CreateInstance(engine::scene::PartClass(), "First");
		second = store.CreateInstance(engine::scene::PartClass(), "Second");
		REQUIRE(store.SetParent(first, parent));
		REQUIRE(store.SetParent(second, parent));
	});
	studio::ToolsProbe::RecordCreate(editor, world, previous, NULL_ENTITY);
	studio::ToolsProbe::OpenRuntimeOnlyScript(editor, world);
	studio::ToolsProbe::ClearFirstScriptRecovery(editor);
	std::optional<std::string> recording;
	std::optional<studio::FinishOperation> finishDuringRemoval;
	SECTION("without an existing recording") {}
	SECTION("with an existing recording") {
		recording = studio::ToolsProbe::BeginRecording(editor);
		REQUIRE(recording.has_value());
	}
	SECTION("commit during removal") {
		recording = studio::ToolsProbe::BeginRecording(editor);
		REQUIRE(recording.has_value());
		finishDuringRemoval = studio::FinishOperation::Commit;
	}
	SECTION("cancel during removal") {
		recording = studio::ToolsProbe::BeginRecording(editor);
		REQUIRE(recording.has_value());
		finishDuringRemoval = studio::FinishOperation::Cancel;
	}
	int calls = 0;
	bool undoAvailable = false;
	bool redoAvailable = false;
	bool priorAlive = false;
	bool finishedDuringRemoval = false;
	editor.Universe->Enter(world, [&](Store &store) {
		store.OnDescendantRemoving([&](Entity, Entity) {
			++calls;
			undoAvailable = undoAvailable || studio::ToolsProbe::CanUndo(editor);
			redoAvailable = redoAvailable || studio::ToolsProbe::CanRedo(editor);
			studio::ToolsProbe::UndoEdit(editor);
			if (recording && finishDuringRemoval) {
				finishedDuringRemoval |=
					studio::ToolsProbe::FinishEditorRecording(editor, *recording, *finishDuringRemoval);
			}
			priorAlive = store.Alive(previous);
		});
	});
	studio::ToolsProbe::DeleteSelection(editor, world, {first, second});
	editor.Universe->Enter(world, [&](Store &store) { store.ClearDescendantRemoving(); });
	CHECK(calls == 2);
	CHECK_FALSE(undoAvailable);
	CHECK_FALSE(redoAvailable);
	CHECK_FALSE(finishedDuringRemoval);
	CHECK(priorAlive);
	CHECK_FALSE(studio::ToolsProbe::FirstScriptHasRecovery(editor));
	if (recording) {
		CHECK(studio::ToolsProbe::RecordingActive(editor, *recording));
		REQUIRE(studio::ToolsProbe::CancelRecording(editor, *recording));
	} else {
		studio::ToolsProbe::UndoEdit(editor);
	}
	editor.Universe->Enter(world, [&](Store &store) {
		CHECK(store.Alive(previous));
		CHECK(store.FindFirstChild(parent, "First") != NULL_ENTITY);
		CHECK(store.FindFirstChild(parent, "Second") != NULL_ENTITY);
	});
}

TEST_CASE("replica deletion blocks reentrant history without recording it", "[studio][tools][delete]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto authority = editor.Universe->Worlds().front();
	const auto replica = studio::ToolsProbe::StartReplica(editor, authority);
	REQUIRE(replica.IsValid());
	studio::ToolsProbe::PrepareCommands(editor);
	Entity previous;
	editor.Universe->Enter(authority, [&](Store &store) {
		previous = store.CreateInstance(engine::scene::PartClass(), "EarlierEdit");
	});
	studio::ToolsProbe::RecordCreate(editor, authority, previous, NULL_ENTITY);
	studio::ToolsProbe::OpenRuntimeOnlyScript(editor, authority);
	studio::ToolsProbe::ClearFirstScriptRecovery(editor);
	Entity parent;
	Entity child;
	bool undoAvailable = false;
	int calls = 0;
	editor.Universe->Enter(replica, [&](Store &store) {
		parent = store.CreatePredictedInstance(engine::scene::PartClass(), "Parent");
		child = store.CreatePredictedInstance(engine::scene::PartClass(), "Child");
		REQUIRE(store.SetParent(child, parent));
		store.OnDescendantRemoving([&](Entity, Entity) {
			++calls;
			undoAvailable = studio::ToolsProbe::CanUndo(editor);
			studio::ToolsProbe::UndoEdit(editor);
		});
	});
	studio::ToolsProbe::DeleteSelection(editor, replica, {child});
	editor.Universe->Enter(replica, [&](Store &store) {
		store.ClearDescendantRemoving();
		CHECK_FALSE(store.Alive(child));
	});
	CHECK(calls == 1);
	CHECK_FALSE(undoAvailable);
	CHECK_FALSE(studio::ToolsProbe::FirstScriptHasRecovery(editor));
	REQUIRE(studio::ToolsProbe::CanUndo(editor));
	studio::ToolsProbe::UndoEdit(editor);
	editor.Universe->Enter(authority, [&](Store &store) { CHECK_FALSE(store.Alive(previous)); });
}

TEST_CASE("deletion blocks reentrant history reset and disable", "[studio][tools][delete]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::PrepareCommands(editor);
	const auto world = editor.Universe->Worlds().front();
	Entity previous;
	Entity parent;
	Entity first;
	Entity second;
	editor.Universe->Enter(world, [&](Store &store) {
		previous = store.CreateInstance(engine::scene::PartClass(), "EarlierEdit");
		parent = store.CreateInstance(engine::scene::PartClass(), "Parent");
		first = store.CreateInstance(engine::scene::PartClass(), "First");
		second = store.CreateInstance(engine::scene::PartClass(), "Second");
		REQUIRE(store.SetParent(first, parent));
		REQUIRE(store.SetParent(second, parent));
	});
	studio::ToolsProbe::RecordCreate(editor, world, previous, NULL_ENTITY);
	enum class ReentrantChange { Reset, Disable, Clear, Forget };
	ReentrantChange change = ReentrantChange::Reset;
	SECTION("reset") {
		change = ReentrantChange::Reset;
	}
	SECTION("disable") {
		change = ReentrantChange::Disable;
	}
	SECTION("clear") {
		change = ReentrantChange::Clear;
	}
	SECTION("forget") {
		change = ReentrantChange::Forget;
	}
	int calls = 0;
	editor.Universe->Enter(world, [&](Store &store) {
		store.OnDescendantRemoving([&](Entity, Entity) {
			++calls;
			switch (change) {
			case ReentrantChange::Reset:
				studio::ToolsProbe::ResetHistory(editor);
				break;
			case ReentrantChange::Disable:
				studio::ToolsProbe::SetHistoryEnabled(editor, false);
				break;
			case ReentrantChange::Clear:
				studio::ToolsProbe::ClearHistory(editor);
				break;
			case ReentrantChange::Forget:
				studio::ToolsProbe::ForgetHistoryWorld(editor, world);
				break;
			}
		});
	});
	studio::ToolsProbe::DeleteSelection(editor, world, {first, second});
	editor.Universe->Enter(world, [&](Store &store) { store.ClearDescendantRemoving(); });
	CHECK(calls == 2);
	REQUIRE(studio::ToolsProbe::HistoryEnabled(editor));
	REQUIRE(studio::ToolsProbe::CanUndo(editor));
	studio::ToolsProbe::UndoEdit(editor);
	editor.Universe->Enter(world, [&](Store &store) {
		CHECK(store.Alive(previous));
		CHECK(store.FindFirstChild(parent, "First") != NULL_ENTITY);
		CHECK(store.FindFirstChild(parent, "Second") != NULL_ENTITY);
	});
	REQUIRE(studio::ToolsProbe::CanUndo(editor));
	studio::ToolsProbe::UndoEdit(editor);
	editor.Universe->Enter(world, [&](Store &store) { CHECK_FALSE(store.Alive(previous)); });
}

TEST_CASE("a throwing delete listener still marks the game modified", "[studio][tools][delete]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::PrepareCommands(editor);
	const auto world = editor.Universe->Worlds().front();
	Entity part;
	editor.Universe->Enter(world, [&](Store &store) {
		part = store.CreateInstance(engine::scene::PartClass(), "Part");
	});
	studio::ToolsProbe::SetModified(editor, false);
	studio::ToolsProbe::WatchCommits(editor, [](std::span<const studio::Command>) {
		throw std::runtime_error("listener failed");
	});
	try {
		studio::ToolsProbe::DeleteSelection(editor, world, part);
	} catch (const std::runtime_error &) {}
	studio::ToolsProbe::ClearHistoryWatch(editor);
	editor.Universe->Enter(world, [&](Store &store) { CHECK_FALSE(store.Alive(part)); });
	CHECK(studio::ToolsProbe::GameModified(editor));
	REQUIRE(studio::ToolsProbe::CanUndo(editor));
	studio::ToolsProbe::UndoEdit(editor);
	editor.Universe->Enter(world, [&](Store &store) { CHECK(store.FindFirstRoot("Part") != NULL_ENTITY); });
}

TEST_CASE("plugin history reports reset and disable refusal during deletion", "[studio][tools][delete]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::PrepareCommands(editor);
	const auto world = editor.Universe->Worlds().front();
	studio::LoadedPlugin plugin;
	std::unique_ptr<engine::script::HostSurface> surface;
	Entity parent;
	Entity child;
	bool accepted = true;
	std::string failure;
	bool reset = false;
	SECTION("reset") {
		reset = true;
	}
	SECTION("disable") {
		reset = false;
	}
	editor.Universe->Enter(world, [&](Store &store) {
		parent = store.CreateInstance(engine::scene::PartClass(), "Parent");
		child = store.CreateInstance(engine::scene::PartClass(), "Child");
		REQUIRE(store.SetParent(child, parent));
		surface = studio::MakePluginSurface(editor, plugin, store);
		store.OnDescendantRemoving([&](Entity, Entity) {
			engine::script::HostValue result;
			const auto argument = engine::script::HostValue::Of(false);
			accepted = reset ? surface->Call("ChangeHistoryService.ResetWaypoints", {}, result, failure)
							 : surface->Call(
								   "ChangeHistoryService.SetEnabled", std::span(&argument, 1), result, failure
							   );
		});
	});
	studio::ToolsProbe::DeleteSelection(editor, world, child);
	editor.Universe->Enter(world, [&](Store &store) { store.ClearDescendantRemoving(); });
	CHECK_FALSE(accepted);
	CHECK(failure.find("editor action") != std::string::npos);
	CHECK(studio::ToolsProbe::HistoryEnabled(editor));
	CHECK(studio::ToolsProbe::CanUndo(editor));
}

TEST_CASE("shutdown requested during deletion waits for the world entry", "[studio][tools][delete]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::PrepareCommands(editor);
	const auto world = editor.Universe->Worlds().front();
	Entity parent;
	Entity child;
	bool called = false;
	bool universeAliveInCallback = false;
	editor.Universe->Enter(world, [&](Store &store) {
		parent = store.CreateInstance(engine::scene::PartClass(), "Parent");
		child = store.CreateInstance(engine::scene::PartClass(), "Child");
		REQUIRE(store.SetParent(child, parent));
		store.OnDescendantRemoving([&](Entity, Entity) {
			called = true;
			editor.Shutdown();
			universeAliveInCallback = editor.Universe != nullptr;
		});
	});
	studio::ToolsProbe::DeleteSelection(editor, world, child);
	CHECK(called);
	CHECK(universeAliveInCallback);
	REQUIRE(editor.Universe != nullptr);
	CHECK(editor.Run() == 0);
	CHECK(editor.Universe == nullptr);
}

TEST_CASE("shutdown requested by a world state observer waits for the barrier", "[studio][tools][world]") {
	Context context;
	TemporaryConfig config;
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto world = editor.Universe->Worlds().front();
	bool called = false;
	bool ticking = false;
	bool entering = true;
	bool liveAfterRequest = false;
	REQUIRE(editor.Universe->OnState([&](auto, Store &, auto) {
		called = true;
		ticking = editor.Universe->TickInFlight();
		entering = editor.Universe->IsEntering();
		editor.Shutdown();
		liveAfterRequest = editor.Universe != nullptr;
	}));
	CHECK(
		editor.Universe->SetState(world, engine::world::WorldState::Suspended) ==
		engine::world::WorldStatus::Ok
	);
	CHECK(called);
	CHECK(ticking);
	CHECK_FALSE(entering);
	CHECK(liveAfterRequest);
	REQUIRE(editor.Universe != nullptr);
	CHECK(editor.Run() == 0);
	CHECK(editor.Universe == nullptr);
}

TEST_CASE("plugin pump waits for a world state observer to return", "[studio][tools][world]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto world = editor.Universe->Worlds().front();
	studio::ToolsProbe::SetActiveWorld(editor, world);
	editor.Universe->Enter(world, [](Store &store) { engine::scene::InstallServices(store); });
	int heartbeats = 0;
	bool duringObserver = false;
	studio::CppPluginDefinition definition;
	definition.Manifest.Id = "test.state-observer-pump";
	definition.Manifest.Name = "State observer pump";
	definition.Open = [](studio::CppPluginContext &, std::string &) { return true; };
	definition.Heartbeat = [&](studio::CppPluginContext &, float, std::string &) {
		++heartbeats;
		duringObserver |= editor.Universe->TickInFlight();
		return true;
	};
	std::string error;
	auto registration = studio::RegisterCppPlugin(definition, error);
	INFO(error);
	REQUIRE(registration.IsOpen());
	editor.LoadPlugins();
	bool called = false;
	REQUIRE(editor.Universe->OnState([&](auto, Store &, auto) {
		called = true;
		editor.PumpPlugins(0.0f);
	}));
	CHECK(
		editor.Universe->SetState(world, engine::world::WorldState::Suspended) ==
		engine::world::WorldStatus::Ok
	);
	CHECK(called);
	CHECK_FALSE(duringObserver);
	CHECK(heartbeats == 0);
	editor.PumpPlugins(0.0f);
	CHECK(heartbeats == 1);
}

TEST_CASE("plugin reload waits for a world state observer to return", "[studio][tools][world]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto world = editor.Universe->Worlds().front();
	studio::ToolsProbe::SetActiveWorld(editor, world);
	editor.Universe->Enter(world, [](Store &store) { engine::scene::InstallServices(store); });
	int opened = 0;
	studio::CppPluginDefinition definition;
	definition.Manifest.Id = "test.state-observer-reload";
	definition.Manifest.Name = "State observer reload";
	definition.Open = [&](studio::CppPluginContext &, std::string &) {
		++opened;
		return true;
	};
	std::string error;
	auto registration = studio::RegisterCppPlugin(definition, error);
	INFO(error);
	REQUIRE(registration.IsOpen());
	editor.LoadPlugins();
	REQUIRE(opened == 1);
	bool called = false;
	REQUIRE(editor.Universe->OnState([&](auto, Store &, auto) {
		called = true;
		editor.LoadPlugins();
	}));
	CHECK(
		editor.Universe->SetState(world, engine::world::WorldState::Suspended) ==
		engine::world::WorldStatus::Ok
	);
	CHECK(called);
	CHECK(opened == 1);
	editor.PumpPlugins(0.0f);
	CHECK(opened == 2);
}

TEST_CASE("world removal cannot destroy a world during its deletion callback", "[studio][tools][delete]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::PrepareCommands(editor);
	const auto world = editor.Universe->Create({.Name = Name("RemovalTarget")});
	REQUIRE(world.IsValid());
	Entity parent;
	Entity child;
	bool called = false;
	bool liveInCallback = false;
	editor.Universe->Enter(world, [&](Store &store) {
		parent = store.CreateInstance(engine::scene::PartClass(), "Parent");
		child = store.CreateInstance(engine::scene::PartClass(), "Child");
		REQUIRE(store.SetParent(child, parent));
		store.OnDescendantRemoving([&](Entity, Entity) {
			called = true;
			studio::ToolsProbe::RemoveWorld(editor, world);
			liveInCallback = editor.Universe->NameOf(world).IsValid();
		});
	});
	studio::ToolsProbe::DeleteSelection(editor, world, child);
	CHECK(called);
	CHECK(liveInCallback);
	REQUIRE(editor.Universe->NameOf(world).IsValid());
	editor.Universe->Enter(world, [&](Store &store) {
		store.ClearDescendantRemoving();
		CHECK(store.Alive(parent));
		CHECK_FALSE(store.Alive(child));
	});
	REQUIRE(studio::ToolsProbe::CanUndo(editor));
	studio::ToolsProbe::UndoEdit(editor);
	editor.Universe->Enter(world, [&](Store &store) {
		CHECK(store.FindFirstChild(parent, "Child") != NULL_ENTITY);
	});
}

TEST_CASE("new game cannot replace a world during its deletion callback", "[studio][tools][delete]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::PrepareCommands(editor);
	const auto world = editor.Universe->Worlds().front();
	Entity parent;
	Entity child;
	bool called = false;
	bool originalAliveInCallback = false;
	editor.Universe->Enter(world, [&](Store &store) {
		parent = store.CreateInstance(engine::scene::PartClass(), "Parent");
		child = store.CreateInstance(engine::scene::PartClass(), "Child");
		REQUIRE(store.SetParent(child, parent));
		store.OnDescendantRemoving([&](Entity, Entity) {
			called = true;
			editor.NewGame();
			originalAliveInCallback = editor.Universe->NameOf(world).IsValid();
		});
	});
	studio::ToolsProbe::DeleteSelection(editor, world, child);
	CHECK(called);
	CHECK(originalAliveInCallback);
	REQUIRE(editor.Universe->NameOf(world).IsValid());
	editor.Universe->Enter(world, [&](Store &store) {
		store.ClearDescendantRemoving();
		CHECK(store.Alive(parent));
		CHECK_FALSE(store.Alive(child));
	});
	REQUIRE(studio::ToolsProbe::CanUndo(editor));
	studio::ToolsProbe::UndoEdit(editor);
	editor.Universe->Enter(world, [&](Store &store) {
		CHECK(store.FindFirstChild(parent, "Child") != NULL_ENTITY);
	});
}

TEST_CASE("opening a game cannot replace a world during its deletion callback", "[studio][tools][delete]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	const auto path = config.Root / "replacement.agame";
	engine::world::Universe replacement;
	replacement.Create({.Name = Name("Replacement")});
	std::string error;
	REQUIRE(engine::game::SaveGame(replacement, Name("Replacement"), path, error));
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::PrepareCommands(editor);
	const auto world = editor.Universe->Worlds().front();
	Entity parent;
	Entity child;
	bool called = false;
	bool opened = true;
	editor.Universe->Enter(world, [&](Store &store) {
		parent = store.CreateInstance(engine::scene::PartClass(), "Parent");
		child = store.CreateInstance(engine::scene::PartClass(), "Child");
		REQUIRE(store.SetParent(child, parent));
		store.OnDescendantRemoving([&](Entity, Entity) {
			called = true;
			opened = editor.OpenGame(path);
		});
	});
	studio::ToolsProbe::DeleteSelection(editor, world, child);
	CHECK(called);
	CHECK_FALSE(opened);
	REQUIRE(editor.Universe->NameOf(world).IsValid());
	editor.Universe->Enter(world, [&](Store &store) {
		store.ClearDescendantRemoving();
		CHECK(store.Alive(parent));
		CHECK_FALSE(store.Alive(child));
	});
	REQUIRE(studio::ToolsProbe::CanUndo(editor));
	studio::ToolsProbe::UndoEdit(editor);
	editor.Universe->Enter(world, [&](Store &store) {
		CHECK(store.FindFirstChild(parent, "Child") != NULL_ENTITY);
	});
}

TEST_CASE("stopping a run cannot replace a world during its deletion callback", "[studio][tools][delete]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto world = editor.Universe->Worlds().front();
	const auto replica = studio::ToolsProbe::StartReplica(editor, world);
	REQUIRE(replica.IsValid());
	studio::ToolsProbe::PrepareCommands(editor);
	Entity parent;
	Entity child;
	bool called = false;
	editor.Universe->Enter(world, [&](Store &store) {
		parent = store.CreateInstance(engine::scene::PartClass(), "Parent");
		child = store.CreateInstance(engine::scene::PartClass(), "Child");
		REQUIRE(store.SetParent(child, parent));
		store.OnDescendantRemoving([&](Entity, Entity) {
			called = true;
			editor.EndRun(world);
		});
	});
	studio::ToolsProbe::DeleteSelection(editor, world, child);
	CHECK(called);
	CHECK(editor.IsRunning(world));
	editor.Universe->Enter(world, [&](Store &store) {
		store.ClearDescendantRemoving();
		CHECK(store.Alive(parent));
		CHECK_FALSE(store.Alive(child));
	});
	REQUIRE(studio::ToolsProbe::CanUndo(editor));
	studio::ToolsProbe::UndoEdit(editor);
	editor.Universe->Enter(world, [&](Store &store) {
		CHECK(store.FindFirstChild(parent, "Child") != NULL_ENTITY);
	});
}

TEST_CASE("shutdown stops running worlds outside callbacks", "[studio][tools][delete]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto world = editor.Universe->Worlds().front();
	REQUIRE(studio::ToolsProbe::StartReplica(editor, world).IsValid());
	REQUIRE(editor.IsRunning(world));
	editor.Shutdown();
	CHECK(editor.Universe == nullptr);
}

TEST_CASE("stopping all runs returns during a deletion callback", "[studio][tools][delete]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto world = editor.Universe->Worlds().front();
	REQUIRE(studio::ToolsProbe::StartReplica(editor, world).IsValid());
	studio::ToolsProbe::PrepareCommands(editor);
	Entity parent;
	Entity child;
	bool returned = false;
	editor.Universe->Enter(world, [&](Store &store) {
		parent = store.CreateInstance(engine::scene::PartClass(), "Parent");
		child = store.CreateInstance(engine::scene::PartClass(), "Child");
		REQUIRE(store.SetParent(child, parent));
		store.OnDescendantRemoving([&](Entity, Entity) {
			editor.EndAllRuns();
			returned = true;
		});
	});
	studio::ToolsProbe::DeleteSelection(editor, world, child);
	CHECK(returned);
	CHECK(editor.IsRunning(world));
	editor.Universe->Enter(world, [&](Store &store) {
		store.ClearDescendantRemoving();
		CHECK(store.Alive(parent));
		CHECK_FALSE(store.Alive(child));
	});
	studio::ToolsProbe::ApplyWorldActions(editor);
	CHECK_FALSE(editor.IsRunning(world));
}

TEST_CASE("shutdown requested by delete publication waits for the caller", "[studio][tools][delete]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::PrepareCommands(editor);
	const auto world = editor.Universe->Worlds().front();
	Entity part;
	editor.Universe->Enter(world, [&](Store &store) {
		part = store.CreateInstance(engine::scene::PartClass(), "Part");
	});
	size_t commits = 0;
	size_t commands = 0;
	size_t starts = 0;
	size_t finishes = 0;
	bool inside = false;
	bool universeAliveInCallback = false;
	studio::ToolsProbe::WatchDeleteHistory(editor, commits, commands, starts, finishes, inside, [&] {
		editor.Shutdown();
		universeAliveInCallback = editor.Universe != nullptr;
	});
	studio::ToolsProbe::DeleteSelection(editor, world, part);
	CHECK(commits == 1);
	CHECK_FALSE(inside);
	CHECK(universeAliveInCallback);
	REQUIRE(editor.Universe != nullptr);
	studio::ToolsProbe::ClearHistoryWatch(editor);
	editor.Shutdown();
	CHECK(editor.Universe == nullptr);
}

TEST_CASE("nested commit preserves the outer delete publication", "[studio][tools][delete]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::PrepareCommands(editor);
	const auto world = editor.Universe->Worlds().front();
	Entity first;
	Entity second;
	editor.Universe->Enter(world, [&](Store &store) {
		first = store.CreateInstance(engine::scene::PartClass(), "First");
		second = store.CreateInstance(engine::scene::PartClass(), "Second");
	});
	bool created = false;
	std::vector<std::string> events;
	studio::ToolsProbe::WatchCommits(editor, [&](std::span<const studio::Command> commands) {
		if (created) {
			events.push_back("nested");
			REQUIRE(commands.size() == 1);
			CHECK(commands[0].Kind == studio::CommandKind::Create);
			return;
		}
		REQUIRE(commands.size() == 2);
		const std::string firstDescription = commands[0].Description;
		const std::string secondDescription = commands[1].Description;
		events.push_back("outer before");
		created = true;
		Entity extra;
		editor.Universe->Enter(world, [&](Store &store) {
			extra = store.CreateInstance(engine::scene::PartClass(), "FromCommit");
		});
		studio::ToolsProbe::RecordCreate(editor, world, extra, NULL_ENTITY);
		events.push_back("outer after");
		CHECK(commands[0].Kind == studio::CommandKind::Destroy);
		CHECK(commands[1].Kind == studio::CommandKind::Destroy);
		CHECK(commands[0].Description == firstDescription);
		CHECK(commands[1].Description == secondDescription);
	});
	studio::ToolsProbe::DeleteSelection(editor, world, {first, second});
	CHECK(created);
	CHECK((events == std::vector<std::string>{"outer before", "nested", "outer after"}));
	studio::ToolsProbe::ClearHistoryWatch(editor);
}

TEST_CASE("deleting a child selected with its protected parent keeps the parent", "[studio][tools][delete]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::PrepareCommands(editor);
	const auto world = editor.Universe->Worlds().front();
	Entity parent;
	Entity child;
	editor.Universe->Enter(world, [&](Store &store) {
		parent = store.CreateInstance(engine::scene::PartClass(), "ProtectedParent");
		child = store.CreateInstance(engine::scene::PartClass(), "SelectedChild");
		REQUIRE(store.SetParent(child, parent));
		store.Protect(parent);
	});
	studio::ToolsProbe::DeleteSelection(editor, world, {child, parent});
	editor.Universe->Enter(world, [&](Store &store) {
		CHECK(store.Alive(parent));
		CHECK_FALSE(store.Alive(child));
	});
	studio::ToolsProbe::UndoEdit(editor);
	editor.Universe->Enter(world, [&](Store &store) {
		CHECK(store.Alive(parent));
		CHECK(store.FindFirstChild(parent, "SelectedChild") != NULL_ENTITY);
	});
}

TEST_CASE("duplicating a parent and child selection copies each subtree once", "[studio][tools][duplicate]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::PrepareCommands(editor);
	const auto world = editor.Universe->Worlds().front();
	Entity parent;
	Entity child;
	editor.Universe->Enter(world, [&](Store &store) {
		parent = store.CreateInstance(engine::scene::PartClass(), "OriginalParent");
		child = store.CreateInstance(engine::scene::PartClass(), "OriginalChild");
		REQUIRE(store.SetParent(child, parent));
	});
	std::vector<Entity> selected;
	SECTION("parent selected first") {
		selected = {parent, child};
	}
	SECTION("child selected first") {
		selected = {child, parent};
	}
	SECTION("duplicate selection handles") {
		selected = {parent, child, parent};
	}
	const auto copies = studio::ToolsProbe::DuplicateSelection(editor, world, selected);
	REQUIRE(copies.size() == 1);
	editor.Universe->Enter(world, [&](Store &store) {
		size_t originalChildren = 0;
		store.EachChild(parent, [&](Entity) { ++originalChildren; });
		CHECK(originalChildren == 1);
		const Entity duplicateChild = store.FindFirstChild(copies.front(), "OriginalChild");
		REQUIRE(duplicateChild != NULL_ENTITY);
		CHECK(duplicateChild != child);
	});
	REQUIRE(studio::ToolsProbe::CanUndo(editor));
	studio::ToolsProbe::UndoEdit(editor);
	editor.Universe->Enter(world, [&](Store &store) {
		CHECK_FALSE(store.Alive(copies.front()));
		CHECK(store.Alive(parent));
		CHECK(store.Alive(child));
	});
	REQUIRE(studio::ToolsProbe::CanRedo(editor));
	studio::ToolsProbe::RedoEdit(editor);
	editor.Universe->Enter(world, [&](Store &store) {
		size_t matchingRoots = 0;
		store.EachRoot([&](Entity root) {
			if (store.InstanceNameOf(root) == Name("OriginalParent")) {
				++matchingRoots;
				CHECK(store.FindFirstChild(root, "OriginalChild") != NULL_ENTITY);
			}
		});
		CHECK(matchingRoots == 2);
		size_t originalChildren = 0;
		store.EachChild(parent, [&](Entity) { ++originalChildren; });
		CHECK(originalChildren == 1);
	});
}

TEST_CASE(
	"duplicating a selected replica hierarchy creates one predicted subtree", "[studio][tools][duplicate]"
) {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto authority = editor.Universe->Worlds().front();
	const auto replica = studio::ToolsProbe::StartReplica(editor, authority);
	REQUIRE(replica.IsValid());
	Entity parent;
	Entity child;
	editor.Universe->Enter(replica, [&](Store &store) {
		parent = store.CreatePredictedInstance(engine::scene::PartClass(), "Parent");
		child = store.CreatePredictedInstance(engine::scene::PartClass(), "Child");
		REQUIRE(parent != NULL_ENTITY);
		REQUIRE(child != NULL_ENTITY);
		REQUIRE(store.SetParent(child, parent));
	});
	std::vector<Entity> selected;
	SECTION("parent selected first") {
		selected = {parent, child};
	}
	SECTION("child selected first") {
		selected = {child, parent};
	}
	const auto copies = studio::ToolsProbe::DuplicateSelection(editor, replica, selected);
	REQUIRE(copies.size() == 1);
	editor.Universe->Enter(replica, [&](Store &store) {
		CHECK(Store::IsPredicted(copies.front()));
		const Entity copiedChild = store.FindFirstChild(copies.front(), "Child");
		REQUIRE(copiedChild != NULL_ENTITY);
		CHECK(Store::IsPredicted(copiedChild));
		size_t originalChildren = 0;
		store.EachChild(parent, [&](Entity) { ++originalChildren; });
		CHECK(originalChildren == 1);
	});
}

TEST_CASE("duplicating selected children across non-archivable ancestors", "[studio][tools][duplicate]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto world = editor.Universe->Worlds().front();
	Entity parent;
	Entity child;
	Entity intermediate = NULL_ENTITY;
	editor.Universe->Enter(world, [&](Store &store) {
		parent = store.CreateInstance(engine::scene::PartClass(), "Parent");
		child = store.CreateInstance(engine::scene::PartClass(), "Child");
	});
	SECTION("selected parent cannot be cloned") {
		editor.Universe->Enter(world, [&](Store &store) {
			store.Set(parent, engine::ecs::NotArchivable{});
			REQUIRE(store.SetParent(child, parent));
		});
		const auto copies = studio::ToolsProbe::DuplicateSelection(editor, world, {parent, child});
		REQUIRE(copies.size() == 1);
		editor.Universe->Enter(world, [&](Store &store) {
			CHECK(store.Alive(copies.front()));
			CHECK(copies.front() != child);
			CHECK(store.ParentOf(copies.front()) == parent);
		});
	}
	SECTION("intermediate subtree cannot be cloned") {
		editor.Universe->Enter(world, [&](Store &store) {
			intermediate = store.CreateInstance(engine::scene::PartClass(), "Intermediate");
			store.Set(intermediate, engine::ecs::NotArchivable{});
			REQUIRE(store.SetParent(intermediate, parent));
			REQUIRE(store.SetParent(child, intermediate));
		});
		const auto copies = studio::ToolsProbe::DuplicateSelection(editor, world, {parent, child});
		REQUIRE(copies.size() == 2);
		editor.Universe->Enter(world, [&](Store &store) {
			CHECK(store.FindFirstChild(copies.front(), "Intermediate") == NULL_ENTITY);
			CHECK(store.ParentOf(copies.back()) == intermediate);
			CHECK(copies.back() != child);
		});
	}
}

TEST_CASE("duplicating instances includes unsaved program and shader edits", "[studio][tools][scripts]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto world = editor.Universe->Worlds().front();
	const Name path("Scripts/duplicate-selection.js");
	Entity parent;
	Entity program;
	Entity shader;
	editor.Universe->Enter(world, [&](Store &store) {
		parent = store.CreateInstance(engine::scene::PartClass(), "SourceParent");
		program = store.CreateInstance(engine::script::ScriptClass(), "SourceProgram");
		shader = store.CreateInstance(engine::scene::ShaderScriptClass(), "SourceShader");
		REQUIRE(store.SetParent(program, parent));
		REQUIRE(store.SetParent(shader, parent));
		engine::script::SetSourcePath(store, program, path);
		if (store.Resource<engine::script::SourceCache>() == nullptr)
			store.SetResource(engine::script::SourceCache{});
		store.ResourceMutable<engine::script::SourceCache>()->Set(path, "saved program");
		REQUIRE(engine::scene::SetShaderSource(store, shader, "saved shader"));
	});
	studio::ToolsProbe::OpenMoveScript(editor, world, program);
	studio::ToolsProbe::OpenMoveScript(editor, world, shader);
	studio::ToolsProbe::SetLastMoveScriptText(editor, "edited shader");
	Entity source = parent;
	SECTION("program") {
		source = program;
	}
	SECTION("shader") {
		source = shader;
	}
	SECTION("parent") {}
	const Entity copy = studio::ToolsProbe::DuplicateInstance(editor, world, source);
	REQUIRE(copy != NULL_ENTITY);
	REQUIRE(copy != source);
	CHECK(studio::ToolsProbe::DuplicateScriptEditsIntact(editor));
	editor.Universe->Enter(world, [&](Store &store) {
		const Entity copiedProgram = source == parent ? store.FindFirstChild(copy, "SourceProgram") : copy;
		const Entity copiedShader = source == parent ? store.FindFirstChild(copy, "SourceShader") : copy;
		if (source != shader) {
			REQUIRE(copiedProgram != NULL_ENTITY);
			std::string text;
			std::string error;
			REQUIRE(
				engine::script::ReadSource(
					store, engine::script::ActiveSourceOf(store, copiedProgram), text, error
				)
			);
			CHECK(text == "edited javascript");
			REQUIRE(engine::script::ReadSource(store, path, text, error));
			CHECK(text == "saved program");
		}
		if (source != program) {
			REQUIRE(copiedShader != NULL_ENTITY);
			const auto *copiedSource = store.Get<engine::scene::ShaderSource>(copiedShader);
			REQUIRE(copiedSource != nullptr);
			CHECK(copiedSource->Code == "edited shader");
			const auto *originalSource = store.Get<engine::scene::ShaderSource>(shader);
			REQUIRE(originalSource != nullptr);
			CHECK(originalSource->Code == "saved shader");
		}
	});
}

TEST_CASE("duplicating unsaved instances separates shared source paths", "[studio][tools][scripts]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto world = editor.Universe->Worlds().front();
	const Name path("Scripts/duplicate-shared.js");
	Entity parent;
	Entity first;
	Entity second;
	editor.Universe->Enter(world, [&](Store &store) {
		parent = store.CreateInstance(engine::scene::PartClass(), "SharedParent");
		first = store.CreateInstance(engine::script::ScriptClass(), "First");
		second = store.CreateInstance(engine::script::ScriptClass(), "Second");
		REQUIRE(store.SetParent(first, parent));
		REQUIRE(store.SetParent(second, parent));
		engine::script::SetSourcePath(store, first, path);
		engine::script::SetSourcePath(store, second, path);
		if (store.Resource<engine::script::SourceCache>() == nullptr)
			store.SetResource(engine::script::SourceCache{});
		store.ResourceMutable<engine::script::SourceCache>()->Set(path, "original source");
	});
	studio::ToolsProbe::OpenMoveScript(editor, world, first);
	studio::ToolsProbe::OpenMoveScript(editor, world, second);
	studio::ToolsProbe::SetLastMoveScriptText(editor, "second edit");
	const Entity copy = studio::ToolsProbe::DuplicateInstance(editor, world, parent);
	REQUIRE(copy != parent);
	CHECK(studio::ToolsProbe::TwoScriptEditsDirty(editor));
	editor.Universe->Enter(world, [&](Store &store) {
		const Entity copiedFirst = store.FindFirstChild(copy, "First");
		const Entity copiedSecond = store.FindFirstChild(copy, "Second");
		REQUIRE(copiedFirst != NULL_ENTITY);
		REQUIRE(copiedSecond != NULL_ENTITY);
		const Name firstPath = engine::script::ActiveSourceOf(store, copiedFirst);
		const Name secondPath = engine::script::ActiveSourceOf(store, copiedSecond);
		CHECK(firstPath != path);
		CHECK(secondPath != path);
		CHECK(firstPath != secondPath);
		std::string text;
		std::string error;
		REQUIRE(engine::script::ReadSource(store, firstPath, text, error));
		CHECK(text == "edited javascript");
		REQUIRE(engine::script::ReadSource(store, secondPath, text, error));
		CHECK(text == "second edit");
		REQUIRE(engine::script::ReadSource(store, path, text, error));
		CHECK(text == "original source");
	});
}

TEST_CASE("duplicate refuses changed source without leaving a copy", "[studio][tools][scripts]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto world = editor.Universe->Worlds().front();
	const Name path("Scripts/duplicate-conflict.js");
	Entity parent;
	Entity program;
	Entity shader;
	editor.Universe->Enter(world, [&](Store &store) {
		parent = store.CreateInstance(engine::scene::PartClass(), "ConflictParent");
		program = store.CreateInstance(engine::script::ScriptClass(), "Program");
		shader = store.CreateInstance(engine::scene::ShaderScriptClass(), "Shader");
		REQUIRE(store.SetParent(program, parent));
		REQUIRE(store.SetParent(shader, parent));
		engine::script::SetSourcePath(store, program, path);
		if (store.Resource<engine::script::SourceCache>() == nullptr)
			store.SetResource(engine::script::SourceCache{});
		store.ResourceMutable<engine::script::SourceCache>()->Set(path, "original source");
		REQUIRE(engine::scene::SetShaderSource(store, shader, "original shader"));
	});
	studio::ToolsProbe::OpenMoveScript(editor, world, shader);
	studio::ToolsProbe::OpenMoveScript(editor, world, program);
	editor.Universe->Enter(world, [&](Store &store) {
		store.ResourceMutable<engine::script::SourceCache>()->Set(path, "changed source");
	});
	CHECK(studio::ToolsProbe::DuplicateInstance(editor, world, parent) == parent);
	CHECK(studio::ToolsProbe::TwoScriptEditsDirty(editor));
	editor.Universe->Enter(world, [&](Store &store) {
		int parents = 0;
		store.EachRoot([&](Entity root) {
			if (store.InstanceNameOf(root) == Name("ConflictParent")) ++parents;
		});
		CHECK(parents == 1);
		std::string text;
		std::string error;
		REQUIRE(engine::script::ReadSource(store, path, text, error));
		CHECK(text == "changed source");
		const auto *originalShader = store.Get<engine::scene::ShaderSource>(shader);
		REQUIRE(originalShader != nullptr);
		CHECK(originalShader->Code == "original shader");
	});
}

TEST_CASE("duplicating a world includes unsaved program and shader edits", "[studio][tools][scripts]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto source = editor.Universe->Worlds().front();
	const Name path("Scripts/duplicate.js");
	Entity program;
	Entity shader;
	editor.Universe->Enter(source, [&](Store &store) {
		program = store.CreateInstance(engine::script::ScriptClass(), "DuplicatedProgram");
		shader = store.CreateInstance(engine::scene::ShaderScriptClass(), "DuplicatedShader");
		engine::script::SetSourcePath(store, program, path);
		if (store.Resource<engine::script::SourceCache>() == nullptr)
			store.SetResource(engine::script::SourceCache{});
		store.ResourceMutable<engine::script::SourceCache>()->Set(path, "saved program");
		REQUIRE(engine::scene::SetShaderSource(store, shader, "saved shader"));
	});
	studio::ToolsProbe::OpenMoveScript(editor, source, program);
	studio::ToolsProbe::OpenMoveScript(editor, source, shader);
	studio::ToolsProbe::SetLastMoveScriptText(editor, "edited shader");
	const auto copyName = Name(std::string(editor.Universe->NameOf(source).Text()) + " 2");
	SECTION("both edits are copied") {
		REQUIRE(studio::ToolsProbe::DuplicateWorldWithEdits(editor, source));
		CHECK(studio::ToolsProbe::DuplicateScriptEditsIntact(editor));
		editor.Universe->Enter(source, [&](Store &store) {
			std::string text;
			std::string error;
			REQUIRE(engine::script::ReadSource(store, path, text, error));
			CHECK(text == "saved program");
			const auto *shaderSource = store.Get<engine::scene::ShaderSource>(shader);
			REQUIRE(shaderSource != nullptr);
			CHECK(shaderSource->Code == "saved shader");
		});
		const auto copy = editor.Universe->Find(copyName);
		REQUIRE(copy.IsValid());
		editor.Universe->Enter(copy, [&](Store &store) {
			const Entity copiedProgram = store.FindFirstRoot("DuplicatedProgram");
			const Entity copiedShader = store.FindFirstRoot("DuplicatedShader");
			REQUIRE(copiedProgram != NULL_ENTITY);
			REQUIRE(copiedShader != NULL_ENTITY);
			std::string text;
			std::string error;
			REQUIRE(
				engine::script::ReadSource(
					store, engine::script::ActiveSourceOf(store, copiedProgram), text, error
				)
			);
			CHECK(text == "edited javascript");
			const auto *shaderSource = store.Get<engine::scene::ShaderSource>(copiedShader);
			REQUIRE(shaderSource != nullptr);
			CHECK(shaderSource->Code == "edited shader");
		});
	}
	SECTION("conflicting source refuses duplication") {
		editor.Universe->Enter(source, [&](Store &store) {
			store.ResourceMutable<engine::script::SourceCache>()->Set(path, "new program");
		});
		CHECK_FALSE(studio::ToolsProbe::DuplicateWorldWithEdits(editor, source));
		CHECK_FALSE(editor.Universe->Find(copyName).IsValid());
		CHECK(studio::ToolsProbe::DuplicateScriptEditsIntact(editor));
		editor.Universe->Enter(source, [&](Store &store) {
			std::string text;
			std::string error;
			REQUIRE(engine::script::ReadSource(store, path, text, error));
			CHECK(text == "new program");
			const auto *shaderSource = store.Get<engine::scene::ShaderSource>(shader);
			REQUIRE(shaderSource != nullptr);
			CHECK(shaderSource->Code == "saved shader");
		});
	}
}

TEST_CASE("duplicating a world preserves clean script language containers", "[studio][tools][scripts]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto source = editor.Universe->Worlds().front();
	const Name luaPath("Scripts/clean-implicit.js");
	const Name jsPath("Scripts/clean-dual.js");
	editor.Universe->Enter(source, [&](Store &store) {
		const Entity implicit = store.CreateInstance(engine::script::ScriptClass(), "Implicit");
		store.Set(implicit, engine::script::LuaSourceContainer{.Path = luaPath});
		REQUIRE(store.Get<engine::script::JavaScriptSourceContainer>(implicit) == nullptr);
		REQUIRE(store.Get<engine::script::CodeSourceContainerSelector>(implicit) == nullptr);
		const Entity dual = store.CreateInstance(engine::script::ScriptClass(), "Dual");
		store.Set(dual, engine::script::LuaSourceContainer{.Path = luaPath});
		engine::script::SetSourcePath(store, dual, jsPath);
		if (store.Resource<engine::script::SourceCache>() == nullptr)
			store.SetResource(engine::script::SourceCache{});
		store.ResourceMutable<engine::script::SourceCache>()->Set(luaPath, "inactive luau");
		store.ResourceMutable<engine::script::SourceCache>()->Set(jsPath, "active javascript");
	});
	REQUIRE(studio::ToolsProbe::DuplicateWorldWithEdits(editor, source));
	const auto copy = editor.Universe->Find(Name(std::string(editor.Universe->NameOf(source).Text()) + " 2"));
	REQUIRE(copy.IsValid());
	editor.Universe->Enter(copy, [&](Store &store) {
		const Entity implicit = store.FindFirstRoot("Implicit");
		const Entity dual = store.FindFirstRoot("Dual");
		REQUIRE(implicit != NULL_ENTITY);
		REQUIRE(dual != NULL_ENTITY);
		CHECK(engine::script::ActiveLanguageOf(store, implicit) == engine::script::Language::Luau);
		CHECK(store.Get<engine::script::JavaScriptSourceContainer>(implicit) == nullptr);
		const auto *lua = store.Get<engine::script::LuaSourceContainer>(dual);
		const auto *js = store.Get<engine::script::JavaScriptSourceContainer>(dual);
		REQUIRE(lua != nullptr);
		REQUIRE(js != nullptr);
		CHECK(lua->Path == luaPath);
		CHECK(js->Path == jsPath);
		CHECK(engine::script::ActiveLanguageOf(store, dual) == engine::script::Language::JavaScript);
		std::string text;
		std::string error;
		REQUIRE(engine::script::ReadSource(store, lua->Path, text, error));
		CHECK(text == "inactive luau");
	});
}

TEST_CASE("undo restores an unsaved descendant script tab", "[studio][tools][scripts]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::PrepareCommands(editor);
	const auto world = editor.Universe->Worlds().front();
	Entity parent;
	editor.Universe->Enter(world, [&](Store &store) {
		parent = store.CreateInstance(engine::scene::PartClass(), "Parent");
	});
	studio::ToolsProbe::OpenRuntimeOnlyScript(editor, world);
	const Entity script = studio::ToolsProbe::FirstScriptInstance(editor);
	editor.Universe->Enter(world, [&](Store &store) { REQUIRE(store.SetParent(script, parent)); });
	studio::ToolsProbe::DeleteSelection(editor, world, parent);
	REQUIRE(studio::ToolsProbe::HasUnsavedScript(editor));
	studio::ToolsProbe::UndoEdit(editor);
	REQUIRE(studio::ToolsProbe::HasUnsavedScript(editor));
	const Entity restored = studio::ToolsProbe::FirstScriptInstance(editor);
	editor.Universe->Enter(world, [&](Store &store) {
		CHECK(store.Alive(restored));
		CHECK(store.IsA(restored, engine::script::ScriptClass()));
	});
	studio::ToolsProbe::RedoEdit(editor);
	REQUIRE(studio::ToolsProbe::HasUnsavedScript(editor));
	editor.Universe->Enter(world, [&](Store &store) { CHECK_FALSE(store.Alive(restored)); });
	studio::ToolsProbe::UndoEdit(editor);
	REQUIRE(studio::ToolsProbe::HasUnsavedScript(editor));
	const Entity restoredAgain = studio::ToolsProbe::FirstScriptInstance(editor);
	editor.Universe->Enter(world, [&](Store &store) {
		CHECK(store.Alive(restoredAgain));
		CHECK(store.IsA(restoredAgain, engine::script::ScriptClass()));
	});
}

TEST_CASE("redo creation restores an unsaved script tab", "[studio][tools][scripts]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::PrepareCommands(editor);
	const auto world = editor.Universe->Worlds().front();
	studio::ToolsProbe::OpenRuntimeOnlyScript(editor, world);
	const Entity script = studio::ToolsProbe::FirstScriptInstance(editor);
	studio::ToolsProbe::RecordCreate(editor, world, script, NULL_ENTITY);
	studio::ToolsProbe::UndoEdit(editor);
	REQUIRE(studio::ToolsProbe::HasUnsavedScript(editor));
	editor.Universe->Enter(world, [&](Store &store) { REQUIRE_FALSE(store.Alive(script)); });
	studio::ToolsProbe::RedoEdit(editor);
	REQUIRE(studio::ToolsProbe::HasUnsavedScript(editor));
	const Entity restored = studio::ToolsProbe::FirstScriptInstance(editor);
	editor.Universe->Enter(world, [&](Store &store) {
		CHECK(store.Alive(restored));
		CHECK(store.IsA(restored, engine::script::ScriptClass()));
	});
}

TEST_CASE("redo creation reconnects an unsaved descendant tab", "[studio][tools][scripts]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::PrepareCommands(editor);
	const auto world = editor.Universe->Worlds().front();
	Entity parent;
	editor.Universe->Enter(world, [&](Store &store) {
		parent = store.CreateInstance(engine::scene::PartClass(), "Parent");
	});
	studio::ToolsProbe::OpenRuntimeOnlyScript(editor, world);
	const Entity script = studio::ToolsProbe::FirstScriptInstance(editor);
	editor.Universe->Enter(world, [&](Store &store) { REQUIRE(store.SetParent(script, parent)); });
	studio::ToolsProbe::RecordCreate(editor, world, parent, script);
	studio::ToolsProbe::DeleteSelection(editor, world, parent);
	studio::ToolsProbe::UndoEdit(editor);
	REQUIRE(studio::ToolsProbe::HasUnsavedScript(editor));
	const Entity afterDeleteUndo = studio::ToolsProbe::FirstScriptInstance(editor);
	editor.Universe->Enter(world, [&](Store &store) { REQUIRE(store.Alive(afterDeleteUndo)); });
	studio::ToolsProbe::UndoEdit(editor);
	editor.Universe->Enter(world, [&](Store &store) { REQUIRE_FALSE(store.Alive(afterDeleteUndo)); });
	studio::ToolsProbe::RedoEdit(editor);
	REQUIRE(studio::ToolsProbe::HasUnsavedScript(editor));
	const Entity restored = studio::ToolsProbe::FirstScriptInstance(editor);
	editor.Universe->Enter(world, [&](Store &store) {
		CHECK(store.Alive(restored));
		CHECK(store.IsA(restored, engine::script::ScriptClass()));
	});
}

TEST_CASE("redo listeners see the restored unsaved script tab", "[studio][tools][scripts]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::PrepareCommands(editor);
	const auto world = editor.Universe->Worlds().front();
	studio::ToolsProbe::OpenRuntimeOnlyScript(editor, world);
	const Entity script = studio::ToolsProbe::FirstScriptInstance(editor);
	studio::ToolsProbe::RecordCreate(editor, world, script, NULL_ENTITY);
	studio::ToolsProbe::UndoEdit(editor);

	bool invoked = false;
	bool aliveDuringListener = false;
	studio::ToolsProbe::WatchRedo(editor, [&] {
		invoked = true;
		const Entity restored = studio::ToolsProbe::FirstScriptInstance(editor);
		editor.Universe->Enter(world, [&](Store &store) { aliveDuringListener = store.Alive(restored); });
	});
	studio::ToolsProbe::RedoEdit(editor);
	CHECK(invoked);
	CHECK(aliveDuringListener);
	CHECK(studio::ToolsProbe::HasUnsavedScript(editor));
}

TEST_CASE(
	"plugin heartbeat redo reconnects an unsaved descendant before listeners",
	"[studio][tools][scripts][plugins]"
) {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	const auto plugin = config.Root / "plugins" / "history-heartbeat";
	std::filesystem::create_directories(plugin);
	std::ofstream(plugin / "plugin.json") << R"({"name":"History Heartbeat"})";
	std::ofstream(plugin / "main.luau") << "local done = false\n"
										   "game:GetService('RunService').Heartbeat:Connect(function()\n"
										   "  if done then return end\n"
										   "  done = true\n"
										   "  ChangeHistoryService:Redo()\n"
										   "end)\n";
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::PrepareCommands(editor);
	const auto world = editor.Universe->Worlds().front();
	studio::ToolsProbe::SetActiveWorld(editor, world);
	Entity parent;
	editor.Universe->Enter(world, [&](Store &store) {
		parent = store.CreateInstance(engine::scene::PartClass(), "Parent");
	});
	studio::ToolsProbe::OpenRuntimeOnlyScript(editor, world);
	const Entity script = studio::ToolsProbe::FirstScriptInstance(editor);
	editor.Universe->Enter(world, [&](Store &store) { REQUIRE(store.SetParent(script, parent)); });
	studio::ToolsProbe::RecordCreate(editor, world, parent, script);
	studio::ToolsProbe::UndoEdit(editor);
	REQUIRE(studio::ToolsProbe::CanRedo(editor));
	studio::ToolsProbe::LoadStudioPlugins(editor);
	SECTION("outside a world entry") {
		bool invoked = false;
		bool liveDuringListener = false;
		studio::ToolsProbe::WatchRedo(editor, [&] {
			invoked = true;
			const Entity restored = studio::ToolsProbe::FirstScriptInstance(editor);
			editor.Universe->Enter(world, [&](Store &store) { liveDuringListener = store.Alive(restored); });
		});
		studio::ToolsProbe::PumpStudioPlugins(editor);
		CHECK_FALSE(studio::ToolsProbe::CanRedo(editor));
		CHECK(invoked);
		CHECK(liveDuringListener);
		CHECK(studio::ToolsProbe::HasUnsavedScript(editor));
		const Entity restored = studio::ToolsProbe::FirstScriptInstance(editor);
		editor.Universe->Enter(world, [&](Store &store) {
			CHECK(store.Alive(restored));
			CHECK(store.IsA(restored, engine::script::ScriptClass()));
		});
	}
	SECTION("inside a world entry") {
		REQUIRE(studio::ToolsProbe::FirstScriptHasRecovery(editor));
		const Entity before = studio::ToolsProbe::FirstScriptInstance(editor);
		bool invoked = false;
		bool listenerInsideEntry = false;
		bool liveDuringListener = false;
		studio::ToolsProbe::WatchRedo(editor, [&] {
			invoked = true;
			listenerInsideEntry = editor.Universe->IsEntering();
			if (!listenerInsideEntry) {
				const Entity restored = studio::ToolsProbe::FirstScriptInstance(editor);
				editor.Universe->Enter(world, [&](Store &store) {
					liveDuringListener = store.Alive(restored);
				});
			}
		});
		editor.Universe->Enter(world, [&](Store &) { studio::ToolsProbe::PumpStudioPlugins(editor); });
		CHECK(studio::ToolsProbe::CanRedo(editor));
		CHECK_FALSE(invoked);
		CHECK(studio::ToolsProbe::FirstScriptHasRecovery(editor));
		CHECK(studio::ToolsProbe::FirstScriptInstance(editor) == before);
		studio::ToolsProbe::PumpStudioPlugins(editor);
		CHECK_FALSE(studio::ToolsProbe::CanRedo(editor));
		CHECK(invoked);
		CHECK_FALSE(listenerInsideEntry);
		CHECK(liveDuringListener);
		CHECK(studio::ToolsProbe::FirstScriptHasRecovery(editor));
		const Entity restored = studio::ToolsProbe::FirstScriptInstance(editor);
		editor.Universe->Enter(world, [&](Store &store) { CHECK(store.Alive(restored)); });
	}
}

TEST_CASE("play stop keeps same-named shader tabs distinct", "[studio][tools][scripts]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::PrepareCommands(editor);
	const auto world = editor.Universe->Worlds().front();
	Entity first;
	Entity second;
	Entity firstParent;
	editor.Universe->Enter(world, [&](Store &store) {
		engine::scene::InstallServices(store);
		firstParent = store.CreateInstance(engine::scene::PartClass(), "First");
		const Entity secondParent = store.CreateInstance(engine::scene::PartClass(), "Second");
		first = store.CreateInstance(engine::scene::ShaderScriptClass(), "Shared");
		second = store.CreateInstance(engine::scene::ShaderScriptClass(), "Shared");
		REQUIRE(store.SetParent(first, firstParent));
		REQUIRE(store.SetParent(second, secondParent));
	});
	studio::ToolsProbe::OpenTab(editor, world, first);
	studio::ToolsProbe::OpenTab(editor, world, second);
	REQUIRE(studio::ToolsProbe::ScriptCount(editor) == 2);
	REQUIRE(studio::ToolsProbe::StartServer(editor, world));
	SECTION("history rebuilds an open instance during the run") {
		studio::ToolsProbe::MarkFirstScriptDirty(editor);
		studio::ToolsProbe::DeleteSelection(editor, world, firstParent);
		studio::ToolsProbe::UndoEdit(editor);
		CHECK(studio::ToolsProbe::ScriptAt(editor, 0) != first);
	}
	SECTION("no edits during the run") {}
	studio::ToolsProbe::StopWorld(editor, world);
	const auto restoredWorld = editor.Universe->Worlds().front();
	const Entity restoredFirst = studio::ToolsProbe::ScriptAt(editor, 0);
	const Entity restoredSecond = studio::ToolsProbe::ScriptAt(editor, 1);
	CHECK(restoredFirst != restoredSecond);
	editor.Universe->Enter(restoredWorld, [&](Store &store) {
		REQUIRE(store.Alive(restoredFirst));
		REQUIRE(store.Alive(restoredSecond));
		CHECK(store.InstanceNameOf(store.ParentOf(restoredFirst)) == Name("First"));
		CHECK(store.InstanceNameOf(store.ParentOf(restoredSecond)) == Name("Second"));
	});
}

TEST_CASE("script recovery does not alias cleared or destroyed instances", "[studio][tools][scripts]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::PrepareCommands(editor);
	const auto world = editor.Universe->Worlds().front();
	studio::ToolsProbe::OpenRuntimeOnlyScript(editor, world);
	const Entity original = studio::ToolsProbe::FirstScriptInstance(editor);
	Entity replacement;
	editor.Universe->Enter(world, [&](Store &store) {
		store.DestroyInstance(original);
		REQUIRE_FALSE(store.Alive(original));
		replacement = store.CreateInstance(engine::script::ScriptClass(), "Replacement");
		REQUIRE(replacement != NULL_ENTITY);
	});
	SECTION("after an external deletion") {
		studio::ToolsProbe::RebindTabs(editor);
		CHECK(studio::ToolsProbe::FirstScriptInstance(editor) == original);
		CHECK(studio::ToolsProbe::FirstScriptInstance(editor) != replacement);
	}
	SECTION("after clearing history") {
		studio::ToolsProbe::ClearHistoryAndTrack(editor, world, replacement);
		studio::ToolsProbe::RebindTabs(editor);
		CHECK(studio::ToolsProbe::FirstScriptInstance(editor) == original);
		CHECK(studio::ToolsProbe::FirstScriptInstance(editor) != replacement);
	}
	CHECK(studio::ToolsProbe::HasUnsavedScript(editor));
}

TEST_CASE("play stop restores a tab opened after undo during the run", "[studio][tools][scripts]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::PrepareCommands(editor);
	const auto world = editor.Universe->Worlds().front();
	Entity parent;
	Entity original;
	editor.Universe->Enter(world, [&](Store &store) {
		engine::scene::InstallServices(store);
		parent = store.CreateInstance(engine::scene::PartClass(), "Parent");
		original = store.CreateInstance(engine::scene::ShaderScriptClass(), "Shader");
		REQUIRE(store.SetParent(original, parent));
	});
	REQUIRE(studio::ToolsProbe::StartServer(editor, world));
	SECTION("a second run clears the shared history") {
		const auto other = editor.Universe->Create({.Name = Name("other-run")});
		editor.Universe->Enter(other, [&](Store &store) { engine::scene::InstallServices(store); });
		REQUIRE(studio::ToolsProbe::StartServer(editor, other));
	}
	SECTION("one run") {}
	studio::ToolsProbe::DeleteSelection(editor, world, parent);
	studio::ToolsProbe::UndoEdit(editor);
	Entity rebuilt;
	editor.Universe->Enter(world, [&](Store &store) {
		const Entity restoredParent = store.FindFirstRoot("Parent");
		REQUIRE(restoredParent != NULL_ENTITY);
		rebuilt = store.FindFirstChild(restoredParent, "Shader");
		REQUIRE(rebuilt != NULL_ENTITY);
		CHECK(rebuilt != original);
	});
	studio::ToolsProbe::OpenTab(editor, world, rebuilt);
	REQUIRE(studio::ToolsProbe::ScriptCount(editor) == 1);
	studio::ToolsProbe::CloseTab(editor, 0);
	studio::ToolsProbe::OpenTab(editor, world, rebuilt);
	REQUIRE(studio::ToolsProbe::ScriptCount(editor) == 1);
	studio::ToolsProbe::StopWorld(editor, world);
	const Entity restored = studio::ToolsProbe::FirstScriptInstance(editor);
	editor.Universe->Enter(editor.Universe->Worlds().front(), [&](Store &store) {
		REQUIRE(store.Alive(restored));
		CHECK(store.InstanceNameOf(store.ParentOf(restored)) == Name("Parent"));
	});
}

TEST_CASE("play stop keeps an absent reopened tab across another run", "[studio][tools][scripts]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::PrepareCommands(editor);
	const auto world = editor.Universe->Worlds().front();
	Entity parent;
	editor.Universe->Enter(world, [&](Store &store) {
		engine::scene::InstallServices(store);
		parent = store.CreateInstance(engine::scene::PartClass(), "Parent");
		const Entity shader = store.CreateInstance(engine::scene::ShaderScriptClass(), "Shader");
		REQUIRE(store.SetParent(shader, parent));
	});
	REQUIRE(studio::ToolsProbe::StartServer(editor, world));
	studio::ToolsProbe::DeleteSelection(editor, world, parent);
	studio::ToolsProbe::UndoEdit(editor);
	Entity rebuilt;
	editor.Universe->Enter(world, [&](Store &store) {
		const Entity restoredParent = store.FindFirstRoot("Parent");
		REQUIRE(restoredParent != NULL_ENTITY);
		rebuilt = store.FindFirstChild(restoredParent, "Shader");
		REQUIRE(rebuilt != NULL_ENTITY);
	});
	studio::ToolsProbe::OpenTab(editor, world, rebuilt);
	studio::ToolsProbe::MarkFirstScriptDirty(editor);
	studio::ToolsProbe::DeleteSelection(editor, world, rebuilt);
	const auto other = editor.Universe->Create({.Name = Name("other-run")});
	editor.Universe->Enter(other, [&](Store &store) { engine::scene::InstallServices(store); });
	REQUIRE(studio::ToolsProbe::StartServer(editor, other));
	studio::ToolsProbe::StopWorld(editor, world);
	const Entity restored = studio::ToolsProbe::FirstScriptInstance(editor);
	editor.Universe->Enter(editor.Universe->Worlds().front(), [&](Store &store) {
		REQUIRE(store.Alive(restored));
		CHECK(store.InstanceNameOf(store.ParentOf(restored)) == Name("Parent"));
	});
}

TEST_CASE("startup history listeners see restored unsaved scripts", "[studio][tools][scripts][plugins]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	const auto plugin = config.Root / "plugins" / "history-startup";
	std::filesystem::create_directories(plugin);
	std::ofstream(plugin / "plugin.json") << R"({"name":"History Startup"})";
	std::ofstream(plugin / "main.luau") << "ChangeHistoryService:Redo()\n";
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::PrepareCommands(editor);
	const auto world = editor.Universe->Worlds().front();
	studio::ToolsProbe::SetActiveWorld(editor, world);
	Entity parent;
	editor.Universe->Enter(world, [&](Store &store) {
		parent = store.CreateInstance(engine::scene::PartClass(), "Parent");
	});
	studio::ToolsProbe::OpenRuntimeOnlyScript(editor, world);
	const Entity script = studio::ToolsProbe::FirstScriptInstance(editor);
	studio::ToolsProbe::OpenRuntimeOnlyScript(editor, world);
	const Entity sibling = studio::ToolsProbe::ScriptAt(editor, 1);
	const auto other = editor.Universe->Create({.Name = Name("history-other")});
	studio::ToolsProbe::OpenRuntimeOnlyScript(editor, other);
	const Entity otherScript = studio::ToolsProbe::ScriptAt(editor, 2);
	editor.Universe->Enter(world, [&](Store &store) {
		REQUIRE(store.SetParent(script, parent));
		REQUIRE(store.SetParent(sibling, parent));
	});
	studio::ToolsProbe::RecordCreate(editor, world, parent, script);
	studio::ToolsProbe::UndoEdit(editor);
	REQUIRE(studio::ToolsProbe::CanRedo(editor));
	bool invoked = false;
	bool liveDuringListener = false;
	bool siblingDuringListener = false;
	studio::ToolsProbe::WatchRedo(editor, [&] {
		invoked = true;
		const Entity restored = studio::ToolsProbe::FirstScriptInstance(editor);
		const Entity restoredSibling = studio::ToolsProbe::ScriptAt(editor, 1);
		editor.Universe->Enter(world, [&](Store &store) {
			liveDuringListener = store.Alive(restored);
			siblingDuringListener = store.Alive(restoredSibling) && restoredSibling != restored &&
									store.ParentOf(restored) == store.ParentOf(restoredSibling);
		});
	});
	studio::ToolsProbe::LoadStudioPlugins(editor);
	CHECK_FALSE(studio::ToolsProbe::CanRedo(editor));
	CHECK(invoked);
	CHECK(liveDuringListener);
	CHECK(siblingDuringListener);
	CHECK(studio::ToolsProbe::ScriptAt(editor, 2) == otherScript);
	for (size_t index = 0; index < 3; ++index)
		CHECK(studio::ToolsProbe::UnsavedAt(editor, index));
}

TEST_CASE("cancel listeners see the restored unsaved script tab", "[studio][tools][scripts]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::PrepareCommands(editor);
	const auto world = editor.Universe->Worlds().front();
	studio::ToolsProbe::OpenRuntimeOnlyScript(editor, world);
	const Entity script = studio::ToolsProbe::FirstScriptInstance(editor);
	const auto recording = studio::ToolsProbe::BeginRecording(editor);
	REQUIRE(recording);
	studio::ToolsProbe::DeleteSelection(editor, world, script);

	bool invoked = false;
	bool aliveDuringListener = false;
	studio::ToolsProbe::WatchFinish(editor, [&] {
		invoked = true;
		const Entity restored = studio::ToolsProbe::FirstScriptInstance(editor);
		editor.Universe->Enter(world, [&](Store &store) { aliveDuringListener = store.Alive(restored); });
	});
	REQUIRE(studio::ToolsProbe::CancelEditorRecording(editor, *recording));
	CHECK(invoked);
	CHECK(aliveDuringListener);
	CHECK(studio::ToolsProbe::HasUnsavedScript(editor));
}

TEST_CASE("configured heap and preferences shortcuts open their panels", "[studio][tools][shortcuts]") {
	Context context;
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::PreparePanelShortcuts(editor);
	const auto bindings = studio::Keybinds::All();
	std::vector<studio::Chord> originalChords;
	for (const auto &binding : bindings)
		originalChords.push_back(binding.Keys);
	const auto oldScope = studio::Keybinds::CurrentScope();
	studio::Keybinds::Set(studio::Action::ShowHeap, studio::Chord{ImGuiKey_F23});
	studio::Keybinds::Set(studio::Action::ShowPreferences, studio::Chord{ImGuiKey_F24});
	studio::Keybinds::SetScope(studio::Scope::Global);
	const auto draw = [&]() {
		ImGui::NewFrame();
		studio::ToolsProbe::DrawShortcuts(editor);
		ImGui::Render();
	};
	draw();
	ImGui::GetIO().AddKeyEvent(ImGuiKey_F23, true);
	draw();
	CHECK(studio::ToolsProbe::HeapVisible(editor));
	ImGui::GetIO().AddKeyEvent(ImGuiKey_F23, false);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_F24, true);
	draw();
	CHECK(studio::ToolsProbe::SettingsVisible(editor));
	ImGui::GetIO().AddKeyEvent(ImGuiKey_F24, false);
	draw();
	ImGui::GetIO().AddKeyEvent(ImGuiKey_F23, true);
	draw();
	CHECK_FALSE(studio::ToolsProbe::HeapVisible(editor));
	ImGui::GetIO().AddKeyEvent(ImGuiKey_F23, false);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_F24, true);
	draw();
	CHECK_FALSE(studio::ToolsProbe::SettingsVisible(editor));
	for (size_t i = 0; i < bindings.size(); ++i)
		bindings[i].Keys = originalChords[i];
	studio::Keybinds::SetScope(oldScope);
}

TEST_CASE("the script editor uses the configured save shortcut", "[studio][tools][scripts]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto world = editor.Universe->Worlds().front();
	studio::ToolsProbe::OpenRuntimeOnlyScript(editor, world);
	studio::ToolsProbe::ShowScriptTabs(editor);
	REQUIRE(studio::ToolsProbe::HasUnsavedScript(editor));
	const auto original = studio::Keybinds::Of(studio::Action::Save);
	studio::Keybinds::Set(studio::Action::Save, studio::Chord{ImGuiKey_F24});
	const auto draw = [&]() {
		ImGui::NewFrame();
		ImGui::SetNextWindowFocus();
		studio::ToolsProbe::DrawScripts(editor);
		studio::ToolsProbe::DrawShortcuts(editor);
		ImGui::Render();
	};
	draw();
	ImGui::GetIO().AddKeyEvent(ImGuiKey_F24, true);
	draw();
	studio::Keybinds::Set(studio::Action::Save, original);
	CHECK_FALSE(studio::ToolsProbe::HasUnsavedScript(editor));
	CHECK_FALSE(studio::ToolsProbe::AskingSaveAs(editor));
}

TEST_CASE("restoring a script keeps its editor window identity", "[studio][tools][scripts]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::PrepareCommands(editor);
	const auto world = editor.Universe->Worlds().front();
	studio::ToolsProbe::OpenRuntimeOnlyScript(editor, world);
	studio::ToolsProbe::ShowScriptTabs(editor);
	const Entity script = studio::ToolsProbe::FirstScriptInstance(editor);
	const auto draw = [&]() {
		ImGui::NewFrame();
		ImGui::SetNextWindowFocus();
		studio::ToolsProbe::DrawScripts(editor);
		studio::ToolsProbe::ResolveFocusedViewport(editor);
		const bool scriptScope = studio::Keybinds::CurrentScope() == studio::Scope::Script;
		studio::Keybinds::SetScope(studio::Scope::Viewport);
		ImGuiID id = 0;
		for (const ImGuiWindow *window : GImGui->Windows) {
			if (window->Active && std::string_view(window->Name).starts_with("RuntimeOnly") &&
				(window->Flags & ImGuiWindowFlags_ChildWindow) == 0)
				id = window->ID;
		}
		ImGui::Render();
		CHECK(scriptScope);
		return id;
	};
	const ImGuiID original = draw();
	REQUIRE(original != 0);
	studio::ToolsProbe::CloseScriptTab(editor, 0);
	studio::ToolsProbe::ReopenScript(editor, world, script);
	CHECK(draw() == original);
	studio::ToolsProbe::DeleteSelection(editor, world, script);
	studio::ToolsProbe::UndoEdit(editor);
	const Entity restored = studio::ToolsProbe::FirstScriptInstance(editor);
	REQUIRE(restored != script);
	CHECK(draw() == original);
}

TEST_CASE("refused deletion keeps a protected script tab", "[studio][tools][scripts]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto world = editor.Universe->Worlds().front();
	studio::ToolsProbe::OpenRuntimeOnlyScript(editor, world);
	const Entity script = studio::ToolsProbe::FirstScriptInstance(editor);
	studio::ToolsProbe::MarkFirstScriptClean(editor);
	editor.Universe->Enter(world, [&](Store &store) { store.Protect(script); });
	studio::ToolsProbe::DeleteSelection(editor, world, script);
	CHECK(studio::ToolsProbe::ScriptCount(editor) == 1);
	editor.Universe->Enter(world, [&](Store &store) { CHECK(store.Alive(script)); });
}

TEST_CASE("new game retires script popup and hover state", "[studio][tools][scripts]") {
	Context context;
	TemporaryConfig config;
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::PrepareScriptTabs(editor, 1, 0);
	studio::ToolsProbe::SeedCompletionOwner(editor, 0);
	studio::ToolsProbe::RequestNew(editor);
	REQUIRE(studio::ToolsProbe::PendingDiscard(editor));
	studio::ToolsProbe::Discard(editor);
	CHECK_FALSE(studio::ToolsProbe::CompletionOpen(editor));
	CHECK(studio::ToolsProbe::ScriptHoverWord(editor).empty());
}

TEST_CASE("opening a game retires script popup and hover state", "[studio][tools][scripts]") {
	Context context;
	TemporaryConfig config;
	const auto path = config.Root / "replacement.agame";
	engine::world::Universe source;
	source.Create({.Name = Name("replacement")});
	std::string error;
	REQUIRE(engine::game::SaveGame(source, Name("Replacement"), path, error));
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	studio::ToolsProbe::PrepareScriptTabs(editor, 1, 0);
	studio::ToolsProbe::SeedCompletionOwner(editor, 0);
	studio::ToolsProbe::RequestOpen(editor, path);
	REQUIRE(studio::ToolsProbe::PendingDiscard(editor));
	studio::ToolsProbe::Discard(editor);
	CHECK(studio::ToolsProbe::GamePath(editor) == path);
	CHECK_FALSE(studio::ToolsProbe::CompletionOpen(editor));
	CHECK(studio::ToolsProbe::ScriptHoverWord(editor).empty());
}

TEST_CASE("script hover cache follows the selected world", "[studio][tools][scripts]") {
	Context context;
	studio::Editor editor;
	editor.Universe = std::make_unique<engine::world::Universe>();
	const auto jsWorld = editor.Universe->Create({.Name = Name("hover-js")});
	const auto luaWorld = editor.Universe->Create({.Name = Name("hover-lua")});
	studio::OpenScript js;
	studio::OpenScript lua;
	const auto create = [&](auto world, Name path, studio::OpenScript &tab) {
		tab.World = world;
		tab.Path = path;
		tab.Text = "let";
		editor.Universe->Enter(world, [&](Store &store) {
			tab.Instance = store.CreateInstance(engine::script::ScriptClass(), "Script");
			engine::script::SetSourcePath(store, tab.Instance, path);
		});
	};
	create(jsWorld, Name("Scripts/a.js"), js);
	create(luaWorld, Name("Scripts/a.luau"), lua);
	REQUIRE(js.Instance == lua.Instance);
	auto &io = ImGui::GetIO();
	io.AddMousePosEvent(30, 30);
	ImGui::NewFrame();
	ImGui::Begin("hover world test");
	studio::ToolsProbe::DrawScriptHover(editor, js, ImVec2(20, 20));
	REQUIRE_FALSE(studio::ToolsProbe::ScriptHoverText(editor).empty());
	studio::ToolsProbe::DrawScriptHover(editor, lua, ImVec2(20, 20));
	CHECK(studio::ToolsProbe::ScriptHoverText(editor).empty());
	ImGui::End();
	ImGui::Render();
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

TEST_CASE("a client viewport follows its player after teleport", "[studio][tools][plugins]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto world = editor.Universe->Worlds().front();
	editor.Universe->Enter(world, [](Store &store) { engine::scene::InstallServices(store); });
	const auto destination = editor.Universe->Create({.Name = Name("destination")});
	editor.Universe->Enter(destination, [](Store &store) { engine::scene::InstallServices(store); });
	REQUIRE(studio::ToolsProbe::BeginServer(editor));
	REQUIRE(studio::ToolsProbe::SpawnPlayer(editor, world));
	const auto oldReplicas = studio::ToolsProbe::ReplicaWorlds(editor, world);
	REQUIRE(oldReplicas.size() == 1);
	const auto oldReplica = oldReplicas.front();
	REQUIRE(studio::ToolsProbe::HasOpenViewport(editor, oldReplica));
	const Entity player = studio::ToolsProbe::ClientPlayer(editor, world);
	editor.Universe->Enter(world, [&](Store &store) { store.DestroyInstance(player); });
	editor.Universe->Enter(destination, [&](Store &store) {
		REQUIRE(engine::scene::AddPlayer(store, "client 1") != NULL_ENTITY);
	});
	studio::ToolsProbe::FollowTeleports(editor);
	const auto newReplicas = studio::ToolsProbe::ReplicaWorlds(editor, world);
	REQUIRE(newReplicas.size() == 1);
	CHECK(newReplicas.front() != oldReplica);
	CHECK(studio::ToolsProbe::HasOpenViewport(editor, newReplicas.front()));
	CHECK_FALSE(studio::ToolsProbe::HasOpenViewport(editor, oldReplica));
}

TEST_CASE("a remove request during teleport follows the client", "[studio][tools][plugins]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto world = editor.Universe->Worlds().front();
	studio::ToolsProbe::SetActiveWorld(editor, world);
	editor.Universe->Enter(world, [](Store &store) { engine::scene::InstallServices(store); });
	const auto destination = editor.Universe->Create({.Name = Name("destination")});
	editor.Universe->Enter(destination, [](Store &store) { engine::scene::InstallServices(store); });
	REQUIRE(studio::ToolsProbe::BeginServer(editor));
	bool requested = false;
	bool accepted = false;
	studio::CppPluginDefinition definition;
	definition.Manifest.Id = "test.teleport-close-removal";
	definition.Manifest.Name = "Teleport close removal";
	definition.Manifest.Runs = studio::PluginTarget(studio::PluginRunTarget::PlaytestClient);
	definition.Open = [](studio::CppPluginContext &, std::string &) { return true; };
	definition.Close = [&](studio::CppPluginContext &client) {
		if (!requested) {
			requested = true;
			accepted = studio::ToolsProbe::RemovePlayer(*client.Owner, client.World);
		}
	};
	std::string error;
	auto registration = studio::RegisterCppPlugin(definition, error);
	INFO(error);
	REQUIRE(registration.IsOpen());
	editor.LoadPlugins();
	REQUIRE(studio::ToolsProbe::SpawnPlayer(editor, world));
	const auto oldReplicas = studio::ToolsProbe::ReplicaWorlds(editor, world);
	REQUIRE(oldReplicas.size() == 1);
	const auto oldReplica = oldReplicas.front();
	REQUIRE(studio::ToolsProbe::HasOpenViewport(editor, oldReplica));
	const Entity player = studio::ToolsProbe::ClientPlayer(editor, world);
	editor.Universe->Enter(world, [&](Store &store) { store.DestroyInstance(player); });
	editor.Universe->Enter(destination, [&](Store &store) {
		REQUIRE(engine::scene::AddPlayer(store, "client 1") != NULL_ENTITY);
	});
	studio::ToolsProbe::FollowTeleports(editor);
	REQUIRE(requested);
	REQUIRE(accepted);
	const auto moved = studio::ToolsProbe::ReplicaWorlds(editor, world);
	REQUIRE(moved.size() == 1);
	REQUIRE(studio::ToolsProbe::HasOpenViewport(editor, moved.front()));
	studio::ToolsProbe::ApplyWorldActions(editor);
	CHECK(studio::ToolsProbe::ReplicaWorlds(editor, world).empty());
	CHECK_FALSE(studio::ToolsProbe::HasOpenViewport(editor, moved.front()));
}

TEST_CASE(
	"toolbar remove player waits for a safe point and removes the selected replica",
	"[studio][tools][plugins]"
) {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto world = editor.Universe->Worlds().front();
	editor.Universe->Enter(world, [](Store &store) { engine::scene::InstallServices(store); });
	REQUIRE(studio::ToolsProbe::BeginServer(editor));
	REQUIRE(studio::ToolsProbe::SpawnPlayer(editor, world));
	REQUIRE(studio::ToolsProbe::SpawnPlayer(editor, world));
	const auto original = studio::ToolsProbe::ReplicaWorlds(editor, world);
	const auto originalIdentities = studio::ToolsProbe::ReplicaIdentities(editor, world);
	REQUIRE(original.size() == 2);
	const auto selected = original.front();
	const auto kept = original.back();
	REQUIRE(studio::ToolsProbe::HasOpenViewport(editor, selected));
	bool removeBoth = false;
	bool removeOtherBeforeDrain = false;
	bool restartBeforeRemove = false;
	bool stopSelectedFirst = false;
	bool stopSelectedBeforeDrain = false;
	SECTION("selected replica") {}
	SECTION("two sequential scene requests") {
		removeBoth = true;
	}
	SECTION("selected replica remains fixed after links change") {
		removeOtherBeforeDrain = true;
	}
	SECTION("a restarted run does not remove its new replica") {
		restartBeforeRemove = true;
	}
	SECTION("stopping then removing the same replica erases the link") {
		stopSelectedFirst = true;
	}
	SECTION("removing a replica stopped before the drain erases the link") {
		stopSelectedBeforeDrain = true;
	}

	studio::LoadedCppPlugin plugin;
	plugin.Manifest.Id = "toolbar-remove-player-test";
	plugin.Manifest.Name = "Toolbar remove player test";
	plugin.Native = true;
	plugin.Running = true;
	studio::PluginToolbar toolbar;
	toolbar.Name = "Actions";
	toolbar.Id = "actions";
	toolbar.Placement = studio::PluginToolbarPlacement::Pinned;
	toolbar.Rows.push_back(studio::PluginToolbarTrack{"row-1"});
	toolbar.Columns.push_back(studio::PluginToolbarTrack{"action"});
	studio::PluginButton button;
	button.Name = "Remove";
	button.Id = "remove";
	button.Kind = studio::PluginControlKind::Button;
	button.Width = 360.0f;
	button.Row = "row-1";
	button.Column = "action";
	int callbacks = 0;
	bool accepted = false;
	bool unchangedDuringCallback = false;
	button.NativeOnClick = [&](engine::script::HostArguments) {
		++callbacks;
		if (restartBeforeRemove) {
			editor.EndAllRuns();
			editor.SetRunMode(world, studio::RunMode::Play);
		}
		if (stopSelectedFirst) studio::ToolsProbe::StopReplica(editor, world, selected);
		accepted = studio::ToolsProbe::RemovePlayer(editor, removeBoth ? world : selected);
		if (removeBoth) accepted = studio::ToolsProbe::RemovePlayer(editor, world) && accepted;
		unchangedDuringCallback = studio::ToolsProbe::ReplicaWorlds(editor, world) == original;
	};
	toolbar.Buttons.push_back(std::move(button));
	plugin.Toolbars.push_back(std::move(toolbar));
	studio::ToolsProbe::AddPlugin(editor, plugin);
	DrawPluginToolbar(editor, ImVec2(-1.0f, -1.0f), false);
	DrawPluginToolbar(editor, ImVec2(100.0f, 60.0f), false);
	DrawPluginToolbar(editor, ImVec2(100.0f, 60.0f), true);
	DrawPluginToolbar(editor, ImVec2(100.0f, 60.0f), false);
	CHECK(callbacks == 1);
	CHECK(accepted);
	CHECK(unchangedDuringCallback);
	if (removeOtherBeforeDrain) {
		REQUIRE(studio::ToolsProbe::RemovePlayer(editor, kept));
		CHECK(studio::ToolsProbe::ReplicaWorlds(editor, world) == std::vector<studio::WorldId>{selected});
	}
	if (stopSelectedBeforeDrain) studio::ToolsProbe::StopReplica(editor, world, selected);
	studio::ToolsProbe::ApplyWorldActions(editor);
	if (restartBeforeRemove) {
		const auto restarted = studio::ToolsProbe::ReplicaWorlds(editor, world);
		const auto restartedIdentities = studio::ToolsProbe::ReplicaIdentities(editor, world);
		REQUIRE(restarted.size() == 1);
		REQUIRE(restartedIdentities.size() == 1);
		CHECK(restartedIdentities.front() != originalIdentities.front());
		CHECK(restartedIdentities.front() != originalIdentities.back());
	} else if (removeBoth || removeOtherBeforeDrain) {
		CHECK(studio::ToolsProbe::ReplicaWorlds(editor, world).empty());
	} else {
		CHECK(studio::ToolsProbe::ReplicaWorlds(editor, world) == std::vector<studio::WorldId>{kept});
	}
	if (!restartBeforeRemove) {
		CHECK_FALSE(editor.Universe->NameOf(selected).IsValid());
		CHECK_FALSE(studio::ToolsProbe::HasOpenViewport(editor, selected));
		CHECK(editor.Universe->NameOf(kept).IsValid() == !(removeBoth || removeOtherBeforeDrain));
	}
	editor.Plugins.clear();
}

TEST_CASE("client close reserves a different pending removal", "[studio][tools][plugins]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto world = editor.Universe->Worlds().front();
	studio::ToolsProbe::SetActiveWorld(editor, world);
	editor.Universe->Enter(world, [](Store &store) { engine::scene::InstallServices(store); });
	REQUIRE(studio::ToolsProbe::BeginServer(editor));
	bool requestedFromClose = false;
	bool acceptedFromClose = false;
	studio::CppPluginDefinition definition;
	definition.Manifest.Id = "test.client-close-removal";
	definition.Manifest.Name = "Client close removal";
	definition.Manifest.Runs = studio::PluginTarget(studio::PluginRunTarget::PlaytestClient);
	definition.Open = [](studio::CppPluginContext &, std::string &) { return true; };
	definition.Close = [&](studio::CppPluginContext &context) {
		if (!requestedFromClose) {
			requestedFromClose = true;
			acceptedFromClose = studio::ToolsProbe::RemovePlayer(*context.Owner, world);
		}
	};
	std::string error;
	auto registration = studio::RegisterCppPlugin(definition, error);
	INFO(error);
	REQUIRE(registration.IsOpen());
	editor.LoadPlugins();
	for (int i = 0; i < 3; ++i)
		REQUIRE(studio::ToolsProbe::SpawnPlayer(editor, world));
	const auto original = studio::ToolsProbe::ReplicaWorlds(editor, world);
	REQUIRE(original.size() == 3);
	// Keep the test control at the same coordinates as the other toolbar fixtures.
	editor.Plugins.clear();

	studio::LoadedCppPlugin plugin;
	plugin.Manifest.Id = "toolbar-multi-remove-test";
	plugin.Manifest.Name = "Toolbar multi remove test";
	plugin.Native = true;
	plugin.Running = true;
	studio::PluginToolbar toolbar;
	toolbar.Name = "Actions";
	toolbar.Id = "actions";
	toolbar.Placement = studio::PluginToolbarPlacement::Pinned;
	toolbar.Rows.push_back(studio::PluginToolbarTrack{"row-1"});
	toolbar.Columns.push_back(studio::PluginToolbarTrack{"action"});
	studio::PluginButton button;
	button.Name = "Remove";
	button.Id = "remove";
	button.Kind = studio::PluginControlKind::Button;
	button.Width = 360.0f;
	button.Row = "row-1";
	button.Column = "action";
	bool accepted = false;
	button.NativeOnClick = [&](engine::script::HostArguments) {
		accepted = studio::ToolsProbe::RemovePlayer(editor, world);
		accepted = studio::ToolsProbe::RemovePlayer(editor, world) && accepted;
	};
	toolbar.Buttons.push_back(std::move(button));
	plugin.Toolbars.push_back(std::move(toolbar));
	studio::ToolsProbe::AddPlugin(editor, plugin);
	DrawPluginToolbar(editor, ImVec2(-1.0f, -1.0f), false);
	DrawPluginToolbar(editor, ImVec2(100.0f, 60.0f), false);
	DrawPluginToolbar(editor, ImVec2(100.0f, 60.0f), true);
	DrawPluginToolbar(editor, ImVec2(100.0f, 60.0f), false);
	REQUIRE(accepted);
	CHECK(studio::ToolsProbe::ReplicaWorlds(editor, world) == original);
	studio::ToolsProbe::ApplyWorldActions(editor);
	CHECK(requestedFromClose);
	CHECK(acceptedFromClose);
	studio::ToolsProbe::ApplyWorldActions(editor);
	CHECK(studio::ToolsProbe::ReplicaWorlds(editor, world).empty());
	for (const auto replica : original)
		CHECK_FALSE(editor.Universe->NameOf(replica).IsValid());
	editor.Plugins.clear();
}

TEST_CASE("toolbar callback can change a running world's mode", "[studio][tools][plugins]") {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto world = editor.Universe->Worlds().front();
	editor.Universe->Enter(world, [](Store &store) { engine::scene::InstallServices(store); });
	REQUIRE(studio::ToolsProbe::BeginServer(editor));
	REQUIRE(editor.IsRunning(world));
	studio::RunMode requested = studio::RunMode::Edit;
	engine::world::WorldId second;
	bool stopFirst = false;
	bool stopAllFirst = false;
	const auto startSecond = [&] {
		second = editor.Universe->Create({.Name = Name("other")});
		editor.Universe->Enter(second, [](Store &store) { engine::scene::InstallServices(store); });
		REQUIRE(editor.BeginRun(second, studio::RunMode::Server));
	};
	SECTION("stop") {
		requested = studio::RunMode::Edit;
	}
	SECTION("switch to play") {
		requested = studio::RunMode::Play;
	}
	SECTION("stop two worlds") {
		startSecond();
	}
	SECTION("stop then play the same world") {
		stopFirst = true;
		requested = studio::RunMode::Play;
	}
	SECTION("stop all then play one world") {
		startSecond();
		stopAllFirst = true;
		requested = studio::RunMode::Play;
	}
	studio::LoadedCppPlugin plugin;
	plugin.Manifest.Id = "toolbar-stop-test";
	plugin.Manifest.Name = "Toolbar stop test";
	plugin.Native = true;
	plugin.Running = true;
	studio::PluginToolbar toolbar;
	toolbar.Name = "Actions";
	toolbar.Id = "actions";
	toolbar.Placement = studio::PluginToolbarPlacement::Pinned;
	toolbar.Rows.push_back(studio::PluginToolbarTrack{"row-1"});
	toolbar.Columns.push_back(studio::PluginToolbarTrack{"action"});
	studio::PluginButton button;
	button.Name = "Stop";
	button.Id = "stop";
	button.Kind = studio::PluginControlKind::Button;
	button.Width = 360.0f;
	button.Row = "row-1";
	button.Column = "action";
	int callbacks = 0;
	button.NativeOnClick = [&](engine::script::HostArguments) {
		++callbacks;
		if (stopAllFirst) editor.EndAllRuns();
		if (stopFirst) editor.SetRunMode(world, studio::RunMode::Edit);
		editor.SetRunMode(world, requested);
		if (second.IsValid() && !stopAllFirst) editor.SetRunMode(second, studio::RunMode::Edit);
	};
	toolbar.Buttons.push_back(std::move(button));
	plugin.Toolbars.push_back(std::move(toolbar));
	studio::ToolsProbe::AddPlugin(editor, plugin);
	DrawPluginToolbar(editor, ImVec2(-1.0f, -1.0f), false);
	DrawPluginToolbar(editor, ImVec2(100.0f, 60.0f), false);
	DrawPluginToolbar(editor, ImVec2(100.0f, 60.0f), true);
	DrawPluginToolbar(editor, ImVec2(100.0f, 60.0f), false);
	CHECK(callbacks == 1);
	CHECK(editor.IsRunning(world));
	studio::ToolsProbe::ApplyWorldActions(editor);
	CHECK(editor.ModeOf(world) == requested);
	if (second.IsValid()) CHECK_FALSE(editor.IsRunning(second));
	editor.Plugins.clear();
}

TEST_CASE(
	"a plugin heartbeat transport request drains without a rendered frame", "[studio][tools][plugins]"
) {
	Context context;
	TemporaryConfig config;
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	studio::ToolsProbe::PrepareUnsavedGame(editor);
	const auto world = editor.Universe->Worlds().front();
	studio::ToolsProbe::SetActiveWorld(editor, world);
	editor.Universe->Enter(world, [](Store &store) { engine::scene::InstallServices(store); });
	REQUIRE(studio::ToolsProbe::BeginServer(editor));
	bool requested = false;
	studio::CppPluginDefinition definition;
	definition.Manifest.Id = "test.heartbeat-transport";
	definition.Manifest.Name = "Heartbeat transport";
	definition.Open = [](studio::CppPluginContext &, std::string &) { return true; };
	definition.Heartbeat = [&](studio::CppPluginContext &context, float, std::string &) {
		if (!requested) {
			requested = true;
			context.Owner->SetRunMode(world, studio::RunMode::Edit);
		}
		return true;
	};
	std::string error;
	auto registration = studio::RegisterCppPlugin(definition, error);
	INFO(error);
	REQUIRE(registration.IsOpen());
	editor.LoadPlugins();
	editor.PumpPlugins(0.0f);
	REQUIRE(requested);
	editor.PumpPlugins(0.0f);
	CHECK_FALSE(editor.IsRunning(world));
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
