// The Find predicate, which is the half of the panel that can be silently
// wrong.
//
// **A filter that quietly matches nothing looks exactly like a scene that
// contains nothing**, and neither the panel nor the person reading it can tell
// the two apart. So `MatchesQuery` is a free function over a `Store` and this
// file exercises it directly; the widgets around it need a window and are
// covered by running the editor.
//
// The property this suite is really pinning: **the predicate names no
// property.** Every case below asks about `Transparency`, `Anchored` or
// `Material` without the matcher having heard of any of them, because they
// arrive through `PropertyDescriptor` like everything else. A change that made
// Find special-case a type would still pass the cases that use that type and
// fail the ones that do not.

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/game/Game.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/script/Host.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <imgui.h>
#include <imgui_internal.h>
#include <new>
#include <string>
#include <studio/Commands.hpp>
#include <studio/Config.hpp>
#include <studio/Editor.hpp>
#include <studio/Keybinds.hpp>
#include <studio/PlayLink.hpp>
#include <studio/Plugins.hpp>

TEST_SUITE_ID("studio.find")
TEST_DEPENDS("engine.scene.part")

using engine::core::Name;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;
using studio::FindQuery;
using studio::MatchesQuery;

namespace studio {
	struct ToolsProbe {
		static void TreePrepare(Editor &editor) {
			SearchPrepare(editor);
			editor.Commands = std::make_unique<CommandLog>(*editor.Universe);
		}
		static ImVec2 TreeDraw(Editor &editor, Entity instance) {
			editor.Interface.Begin(1.0f / 60.0f);
			ImGui::GetIO().IniFilename = nullptr;
			ImGui::SetNextWindowPos({20, 20}, ImGuiCond_Always);
			ImGui::SetNextWindowSize({600, 500}, ImGuiCond_Always);
			ImGui::Begin("Explorer gesture test", nullptr, ImGuiWindowFlags_NoTitleBar);
			const ImVec2 start = ImGui::GetCursorScreenPos();
			ImVec2 point{};
			auto &tree = editor.TreeFor(editor.Active);
			editor.Universe->Enter(editor.Active, [&](Store &store) {
				HierarchyRequest request;
				request.Open = tree.Open;
				tree.View.Rebuild(store, request);
				const size_t row = tree.View.RowOf(instance);
				REQUIRE(row != HierarchyView::NO_ROW);
				point = ImVec2(
					start.x + 60,
					start.y + (static_cast<float>(row) + 0.5f) * ImGui::GetTextLineHeightWithSpacing()
				);
				editor.DrawInstanceRows(store, tree);
			});
			ImGui::End();
			editor.Interface.End();
			return point;
		}
		static size_t SelectedCount(const Editor &editor) {
			return editor.Selection.size();
		}
		static void SelectInWorld(Editor &editor, engine::world::WorldId world, Entity instance) {
			editor.Select(world, instance, false);
			editor.SelectionAnchor = instance;
		}
		static void SelectOutsideExplorer(Editor &editor, engine::world::WorldId world, Entity instance) {
			editor.Select(world, instance, false);
		}
		static void AddInWorld(Editor &editor, engine::world::WorldId world, Entity instance) {
			editor.Select(world, instance, true);
		}
		static void DuplicateSelection(Editor &editor) {
			editor.DuplicateSelection();
		}
		static Entity FirstSelection(const Editor &editor) {
			return editor.Selection.empty() ? NULL_ENTITY : editor.Selection.front();
		}
		static Entity SelectionAnchor(const Editor &editor) {
			return editor.SelectionAnchor;
		}
		static void SelectGalleryUsers(Editor &editor, engine::world::WorldId world, Name asset) {
			GalleryEntry entry;
			entry.World = world;
			entry.Asset = asset;
			editor.Universe->Enter(world, [&](Store &store) { entry.StoreIdentity = store.Identity(); });
			editor.SelectGalleryUsers(entry);
		}
		static GalleryEntry ScanGallery(Editor &editor, Name asset) {
			editor.RebuildGallery();
			for (const GalleryEntry &entry : editor.Gallery) {
				if (entry.Asset == asset) return entry;
			}
			return {};
		}
		static void SelectGalleryEntry(Editor &editor, const GalleryEntry &entry) {
			editor.SelectGalleryUsers(entry);
		}
		static bool GalleryScanned(const Editor &editor) {
			return editor.GalleryScanned;
		}
		static engine::world::WorldId MainViewportWorld(Editor &editor) {
			return editor.ViewportWorld(0);
		}
		static void BeginRename(Editor &editor, Entity instance) {
			editor.BeginRename(editor.SelectionWorld, instance);
		}
		static void QueueRename(Editor &editor, Entity instance) {
			editor.PendingRenameStartWorld = editor.SelectionWorld;
			editor.PendingRenameStart = instance;
		}
		static void DrawInstanceAction(
			Editor &editor, engine::world::WorldId world, Entity instance, const char *action
		) {
			const auto draw = [&] {
				editor.Interface.Begin(1.0f / 60.0f);
				ImGui::SetNextWindowPos({20, 20}, ImGuiCond_Always);
				ImGui::SetNextWindowSize({600, 500}, ImGuiCond_Always);
				ImGui::SetNextWindowFocus();
				ImGui::Begin("Explorer action test", nullptr, ImGuiWindowFlags_NoTitleBar);
				editor.Universe->Enter(world, [&](Store &store) {
					editor.DrawInstanceActions(store, world, instance);
				});
				const ImGuiID id = ImGui::GetID(action);
				ImGui::End();
				editor.Interface.End();
				return id;
			};
			const ImGuiID id = draw();
			ImGui::ActivateItemByID(id);
			draw();
		}
		static std::string
		InstanceActionLabels(Editor &editor, engine::world::WorldId world, Entity instance) {
			editor.Interface.Begin(1.0f / 60.0f);
			ImGui::SetNextWindowPos({20, 20}, ImGuiCond_Always);
			ImGui::SetNextWindowSize({600, 500}, ImGuiCond_Always);
			ImGui::Begin("Explorer action labels", nullptr, ImGuiWindowFlags_NoTitleBar);
			ImGui::LogToBuffer();
			editor.Universe->Enter(world, [&](Store &store) {
				editor.DrawInstanceActions(store, world, instance);
			});
			const std::string labels = GImGui->LogBuffer.c_str();
			ImGui::LogFinish();
			ImGui::End();
			editor.Interface.End();
			return labels;
		}
		static bool DestructiveActionQueued(const Editor &editor) {
			return editor.PendingDuplicate.World.IsValid() || editor.PendingDelete.World.IsValid();
		}
		static void FocusExtraForWorld(Editor &editor, engine::world::WorldId world) {
			editor.Extras.resize(1);
			editor.Extras[0].World = world;
			editor.Extras[0].Title = "Viewport 2";
			editor.Extras[0].Open = true;
			editor.FocusedViewport = 1;
		}
		static void CloseExtra(Editor &editor) {
			editor.Extras[0].Open = false;
		}
		static void CloseMainViewport(Editor &editor) {
			editor.ShowViewport = false;
		}
		static engine::world::WorldId ExtraWorld(Editor &editor) {
			return editor.ViewportWorld(1);
		}
		static void FollowExtra(Editor &editor, Entity camera) {
			editor.Extras[0].Follow = camera;
		}
		static engine::world::WorldId SelectionWorld(const Editor &editor) {
			return editor.SelectionWorld;
		}
		static engine::world::WorldId StartReplica(Editor &editor, engine::world::WorldId authority) {
			auto &run = editor.Runs.emplace_back();
			run.World = authority;
			run.Mode = RunMode::Play;
			auto link = std::make_unique<PlayLink>();
			std::string error;
			REQUIRE(link->Start(*editor.Universe, authority, 60, error, "zoom-test"));
			const auto replica = link->ReplicaWorld();
			run.Links.push_back(std::move(link));
			return replica;
		}
		static void SetActiveWorld(Editor &editor, engine::world::WorldId world) {
			editor.Active = world;
		}
		static Entity MainFollow(const Editor &editor) {
			return editor.FollowCamera;
		}
		static Entity ExtraFollow(const Editor &editor) {
			return editor.Extras[0].Follow;
		}
		static engine::core::CFrame MainFrame(const Editor &editor) {
			return editor.CameraFrame;
		}
		static engine::core::CFrame ExtraFrame(const Editor &editor) {
			return editor.Extras[0].Frame;
		}
		static bool LookQueued(const Editor &editor) {
			return editor.PendingLookThroughSet;
		}
		static void ReleaseWorld(Editor &editor, engine::world::WorldId world) {
			editor.ReleaseWorldResidency(world);
		}
		static bool RenameQueued(const Editor &editor) {
			return editor.PendingRenameStart != NULL_ENTITY;
		}
		static std::array<ImGuiID, 2>
		DrawWorldRows(Editor &editor, const std::array<engine::world::WorldId, 2> &worlds, Entity instance) {
			editor.Interface.Begin(1.0f / 60.0f);
			ImGui::SetNextWindowPos({20, 20}, ImGuiCond_Always);
			ImGui::SetNextWindowSize({600, 500}, ImGuiCond_Always);
			ImGui::SetNextWindowFocus();
			ImGui::Begin("Explorer rename test", nullptr, ImGuiWindowFlags_NoTitleBar);
			std::array<ImGuiID, 2> fields{};
			for (size_t index = 0; index < worlds.size(); index++) {
				const auto world = worlds[index];
				ImGui::PushID(static_cast<int>(world.Index));
				ImGui::PushID(static_cast<int>(instance.Id));
				fields[index] = ImGui::GetID("##rename");
				ImGui::PopID();
				auto &tree = editor.TreeFor(world);
				editor.Universe->Enter(world, [&](Store &store) {
					tree.View.Rebuild(store, {});
					editor.DrawInstanceRows(store, tree);
				});
				ImGui::PopID();
			}
			ImGui::End();
			editor.Interface.End();
			return fields;
		}
		static void ShiftSelectInWorld(Editor &editor, engine::world::WorldId world, Entity instance) {
			auto &tree = editor.TreeFor(world);
			editor.Universe->Enter(world, [&](Store &store) { tree.View.Rebuild(store, {}); });
			editor.SelectRange(world, tree.View, editor.SelectionAnchor, instance, false);
		}
		static bool ReparentPending(const Editor &editor) {
			return editor.PendingReparent.World.IsValid();
		}
		static void QueueReparent(
			Editor &editor, engine::world::WorldId world, Entity parent, std::vector<Entity> instances
		) {
			auto &tree = editor.TreeFor(world);
			editor.Universe->Enter(world, [&](Store &store) { tree.View.Rebuild(store, {}); });
			editor.PendingReparent = {world, std::move(instances), parent};
			editor.Universe->Enter(world, [&](const Store &store) {
				editor.PendingReparent.StoreIdentity = store.Identity();
			});
		}
		static void RecordCreate(Editor &editor, engine::world::WorldId world, Entity instance) {
			editor.Universe->Enter(world, [&](Store &store) {
				editor.Commands->RecordCreate(store, world, instance, "Earlier edit");
			});
		}
		static void RecordReparent(Editor &editor, engine::world::WorldId world, Entity instance) {
			editor.Commands->RecordReparent(world, instance, NULL_ENTITY, NULL_ENTITY, "Independent edit");
		}
		static void RecordPreparedReparent(Editor &editor, engine::world::WorldId world, Entity instance) {
			auto record = editor.Commands->PrepareReparent(
				world, instance, NULL_ENTITY, NULL_ENTITY, "Prepared nested edit"
			);
			editor.Commands->CommitReparent(std::move(record));
		}
		static void WatchCommits(Editor &editor, size_t &calls, size_t &commands, bool &inside) {
			CommandLog::Watcher watcher{};
			watcher.Committed = [&](uint64_t, std::span<const Command> group) {
				++calls;
				commands = group.size();
				inside = editor.Universe->IsEntering();
			};
			editor.Commands->Watch(std::move(watcher));
		}
		static void WatchReentrantCommits(
			Editor &editor,
			engine::world::WorldId world,
			Entity instance,
			std::vector<std::string> &names,
			bool &inside,
			bool throwFirst
		) {
			CommandLog::Watcher watcher{};
			watcher.Committed = [&, world, instance, throwFirst](uint64_t, std::span<const Command> group) {
				inside = inside || editor.Universe->IsEntering();
				names.push_back(group.front().Description);
				if (names.size() == 1) {
					editor.Commands->RecordReparent(
						world, instance, NULL_ENTITY, NULL_ENTITY, "Listener edit"
					);
					if (throwFirst) throw std::runtime_error("listener failed");
				}
			};
			editor.Commands->Watch(std::move(watcher));
		}
		static void WatchNestedReplica(
			Editor &editor,
			engine::world::WorldId replica,
			Entity target,
			Entity child,
			std::vector<std::string> &names,
			bool &inside,
			bool throwFirst
		) {
			CommandLog::Watcher watcher{};
			watcher.Committed =
				[&, replica, target, child, throwFirst](uint64_t, std::span<const Command> group) {
					inside = inside || editor.Universe->IsEntering();
					names.push_back(group.front().Description);
					if (names.size() == 1) {
						QueueReparent(editor, replica, target, {child});
						editor.ApplyPendingActions();
						if (throwFirst) throw std::runtime_error("outer listener failed");
					}
				};
			editor.Commands->Watch(std::move(watcher));
		}
		static void ApplyPending(Editor &editor) {
			editor.ApplyPendingActions();
		}
		static bool Undo(Editor &editor) {
			return editor.Commands->Undo();
		}
		static bool Redo(Editor &editor) {
			return editor.Commands->Redo();
		}
		static bool CanRedo(const Editor &editor) {
			return editor.Commands->CanRedo();
		}
		static bool Modified(const Editor &editor) {
			return editor.Modified;
		}
		static std::optional<std::string> BeginRecording(Editor &editor) {
			return editor.Commands->TryBeginRecording("reparent");
		}
		static bool CommitRecording(Editor &editor, std::string_view id) {
			return editor.Commands->FinishRecording(id, FinishOperation::Commit);
		}
		static bool MoveToWorld(
			Editor &editor, engine::world::WorldId source, Entity instance, engine::world::WorldId target
		) {
			return editor.MoveInstanceToWorld(source, instance, target, NULL_ENTITY);
		}
		static void QueueMove(
			Editor &editor, engine::world::WorldId source, Entity instance, engine::world::WorldId target
		) {
			editor.PendingMove.Source = source;
			editor.PendingMove.Instance = instance;
			editor.PendingMove.Target = target;
			editor.Universe->Enter(source, [&](const Store &store) {
				editor.PendingMove.SourceIdentity = store.Identity();
			});
			editor.Universe->Enter(target, [&](const Store &store) {
				editor.PendingMove.TargetIdentity = store.Identity();
			});
		}
		static void SelectMoveRoot(Editor &editor, engine::world::WorldId world, bool universe) {
			editor.UniverseSelected = universe;
			editor.SelectedWorldRow = universe ? engine::world::WorldId{} : world;
		}
		static bool RootSelected(const Editor &editor) {
			return editor.UniverseSelected || editor.SelectedWorldRow.IsValid();
		}
		static void SearchPrepare(Editor &editor) {
			engine::scene::RegisterSceneClasses();
			editor.Universe = std::make_unique<engine::world::Universe>();
			editor.Active = editor.Universe->Create({.Name = Name("search-cache-world")});
			editor.ExplorerWorld = editor.Active;
		}
		static engine::world::Universe &SearchUniverse(Editor &editor) {
			return *editor.Universe;
		}
		static engine::world::WorldId SearchWorld(Editor &editor) {
			return editor.Active;
		}
		static const std::vector<FindResult> &SearchRun(Editor &editor, FindQuery query) {
			editor.Find = std::move(query);
			editor.RunFind();
			return editor.FindResults;
		}
		static void SearchSelectWorld(Editor &editor, engine::world::WorldId world) {
			editor.ExplorerWorld = world;
		}
		static bool SearchTruncated(const Editor &editor) {
			return editor.FindTruncated;
		}
		static bool SearchInitialise(Editor &editor) {
			engine::ui::InterfaceSettings settings;
			settings.DisplayWidth = 1280;
			settings.DisplayHeight = 720;
			editor.ShowExplorer = true;
			return editor.Interface.Initialise(editor.Renderer, nullptr, settings);
		}
		static ImGuiWindow *SearchChild() {
			auto *parent = ImGui::FindWindowByName("Explorer");
			REQUIRE(parent != nullptr);
			for (auto *child : parent->DC.ChildWindows)
				if (child->ChildId == parent->GetID("##explorer-search-results")) return child;
			FAIL("Explorer search child was not submitted");
			return nullptr;
		}
		static void SearchDraw(Editor &editor, float scroll, bool full = false) {
			const auto *previous = full ? SearchChild() : nullptr;
			const ImVec2 childPos = previous ? previous->Pos : ImVec2{};
			const ImVec2 childSize = previous ? previous->Size : ImVec2{};
			editor.Interface.Begin(1.0f / 60.0f);
			ImGui::GetIO().IniFilename = nullptr;
			ImGui::SetNextWindowPos({20, 20}, ImGuiCond_Always);
			ImGui::SetNextWindowSize({980, 600}, ImGuiCond_Always);
			if (!full) {
				if (auto *parent = ImGui::FindWindowByName("Explorer")) {
					parent->StateStorage.SetBool(parent->GetID("Property search"), true);
					for (auto *child : parent->DC.ChildWindows)
						if (child->ChildId == parent->GetID("##explorer-search-results"))
							ImGui::SetScrollY(child, scroll);
				}
				editor.DrawExplorer();
			} else {
				ImGui::Begin("Explorer");
				ImGui::SetCursorScreenPos(childPos);
				ImGui::SetNextWindowScroll({0, scroll});
				if (ImGui::BeginChild("##explorer-search-results", childSize)) {
					for (size_t index = 0; index < editor.FindResults.size(); ++index) {
						const auto &result = editor.FindResults[index];
						ImGui::PushID(static_cast<int>(index));
						ImGui::Selectable(
							"##result",
							editor.SelectionWorld == result.World && editor.IsSelected(result.Instance)
						);
						ImGui::SameLine();
						ImGui::Text("%s (%s)", result.Name.c_str(), result.Class.c_str());
						ImGui::PopID();
					}
				}
				ImGui::EndChild();
				ImGui::End();
			}
			editor.Interface.End();
		}
		static bool SearchUniform(const Editor &editor) {
			return editor.FindUniformRows;
		}
		static bool SearchSelected(const Editor &editor, Entity entity) {
			return editor.IsSelected(entity);
		}
		static bool SearchCached(const Editor &editor) {
			return editor.FindFingerprintReady;
		}
	};
}

TEST_CASE("Explorer refuses to move a protected instance between worlds", "[studio][explorer][worlds]") {
	studio::Editor editor;
	studio::ToolsProbe::SearchPrepare(editor);
	auto &universe = studio::ToolsProbe::SearchUniverse(editor);
	const auto source = studio::ToolsProbe::SearchWorld(editor);
	const auto target = universe.Create({.Name = Name("protected-move-target")});
	Entity instance;
	universe.Enter(source, [&](Store &store) {
		instance = store.CreateInstance(engine::ecs::Classes::Find(Name("Folder")), "protected-move");
		store.Protect(instance);
	});
	REQUIRE(instance != NULL_ENTITY);
	CHECK_FALSE(studio::ToolsProbe::MoveToWorld(editor, source, instance, target));
	universe.Enter(source, [&](Store &store) { CHECK(store.Alive(instance)); });
	universe.Enter(target, [&](Store &store) {
		CHECK(store.FindFirstRoot("protected-move") == NULL_ENTITY);
	});
}

TEST_CASE("Explorer refuses cross-world moves involving a play replica", "[studio][explorer][worlds]") {
	studio::Editor editor;
	studio::ToolsProbe::SearchPrepare(editor);
	auto &universe = studio::ToolsProbe::SearchUniverse(editor);
	const auto authored = studio::ToolsProbe::SearchWorld(editor);
	const auto authority = universe.Create({.Name = Name("move-replica-authority")});
	const auto replica = studio::ToolsProbe::StartReplica(editor, authority);
	const auto folder = engine::ecs::Classes::Find(Name("Folder"));
	Entity authoredInstance;
	Entity replicaInstance;
	universe.Enter(authored, [&](Store &store) {
		authoredInstance = store.CreateInstance(folder, "authored-move");
	});
	universe.Enter(replica, [&](Store &store) {
		replicaInstance = store.CreatePredictedInstance(folder, "replica-move");
	});
	REQUIRE(authoredInstance != NULL_ENTITY);
	REQUIRE(replicaInstance != NULL_ENTITY);
	CHECK_FALSE(studio::ToolsProbe::MoveToWorld(editor, authored, authoredInstance, replica));
	CHECK_FALSE(studio::ToolsProbe::MoveToWorld(editor, replica, replicaInstance, authored));
	universe.Enter(authored, [&](Store &store) {
		CHECK(store.Alive(authoredInstance));
		CHECK(store.FindFirstRoot("replica-move") == NULL_ENTITY);
	});
	universe.Enter(replica, [&](Store &store) {
		CHECK(store.Alive(replicaInstance));
		CHECK(store.FindFirstRoot("authored-move") == NULL_ENTITY);
	});
}

TEST_CASE(
	"Explorer preserves edits made by removal callbacks during a cross-world move",
	"[studio][explorer][worlds]"
) {
	studio::Editor editor;
	studio::ToolsProbe::SearchPrepare(editor);
	auto &universe = studio::ToolsProbe::SearchUniverse(editor);
	const auto source = studio::ToolsProbe::SearchWorld(editor);
	const auto target = universe.Create({.Name = Name("callback-move-target")});
	Entity parent;
	Entity root;
	universe.Enter(source, [&](Store &store) {
		const auto folder = engine::ecs::Classes::Find(Name("Folder"));
		parent = store.CreateInstance(folder, "parent");
		root = store.CreateInstance(folder, "original");
		REQUIRE(store.SetParent(root, parent));
		store.OnDescendantRemoving([&](Entity, Entity subject) {
			if (subject == root) REQUIRE(store.SetInstanceName(root, "callback edit"));
		});
	});
	CHECK_FALSE(studio::ToolsProbe::MoveToWorld(editor, source, root, target));
	universe.Enter(source, [&](Store &store) {
		store.ClearDescendantRemoving();
		CHECK(store.Alive(root));
		CHECK(store.InstanceNameOf(root) == Name("callback edit"));
		CHECK(store.ParentOf(root) == parent);
	});
	universe.Enter(target, [&](Store &store) { CHECK(store.FindFirstRoot("original") == NULL_ENTITY); });
}

TEST_CASE("Explorer rechecks the moved subtree after removal callbacks", "[studio][explorer][worlds]") {
	enum class Callback { AddChild, AddTransient, Reparent, Protect, Unrelated, Throw };
	Callback callback = Callback::Unrelated;
	SECTION("a new descendant") {
		callback = Callback::AddChild;
	}
	SECTION("a new transient descendant") {
		callback = Callback::AddTransient;
	}
	SECTION("the root is reparented") {
		callback = Callback::Reparent;
	}
	SECTION("the root is protected") {
		callback = Callback::Protect;
	}
	SECTION("an unrelated edit") {
		callback = Callback::Unrelated;
	}
	SECTION("a throwing listener") {
		callback = Callback::Throw;
	}
	studio::Editor editor;
	studio::ToolsProbe::SearchPrepare(editor);
	auto &universe = studio::ToolsProbe::SearchUniverse(editor);
	const auto source = studio::ToolsProbe::SearchWorld(editor);
	const auto target = universe.Create({.Name = Name("callback-validation-target")});
	Entity parent{}, root{}, other{}, added{};
	universe.Enter(source, [&](Store &store) {
		const auto folder = engine::ecs::Classes::Find(Name("Folder"));
		parent = store.CreateInstance(folder, "parent");
		root = store.CreateInstance(folder, "root");
		other = store.CreateInstance(folder, "other");
		REQUIRE(store.SetParent(root, parent));
		store.OnDescendantRemoving([&](Entity, Entity subject) {
			if (subject != root) return;
			switch (callback) {
			case Callback::AddChild:
			case Callback::AddTransient:
				added = store.CreateInstance(folder, "added");
				REQUIRE(store.SetParent(added, root));
				if (callback == Callback::AddTransient) store.Set(added, engine::scene::TransientComponent{});
				break;
			case Callback::Reparent:
				REQUIRE(store.SetParent(root, other));
				break;
			case Callback::Protect:
				store.Protect(root);
				break;
			case Callback::Unrelated:
				REQUIRE(store.SetInstanceName(other, "other changed"));
				break;
			case Callback::Throw:
				throw std::runtime_error("removal callback");
			}
		});
	});
	if (callback == Callback::Throw)
		CHECK_THROWS_AS(studio::ToolsProbe::MoveToWorld(editor, source, root, target), std::runtime_error);
	else if (callback == Callback::Unrelated)
		CHECK(studio::ToolsProbe::MoveToWorld(editor, source, root, target));
	else
		CHECK_FALSE(studio::ToolsProbe::MoveToWorld(editor, source, root, target));
	universe.Enter(source, [&](Store &store) {
		store.ClearDescendantRemoving();
		CHECK(store.Alive(root) == (callback != Callback::Unrelated));
		if (callback == Callback::AddChild || callback == Callback::AddTransient)
			CHECK(store.ParentOf(added) == root);
		if (callback == Callback::Reparent) CHECK(store.ParentOf(root) == other);
		if (callback == Callback::Protect) CHECK(store.Protected(root));
		if (callback == Callback::Unrelated) CHECK(store.InstanceNameOf(other) == Name("other changed"));
	});
	universe.Enter(target, [&](Store &store) {
		CHECK((store.FindFirstRoot("root") != NULL_ENTITY) == (callback == Callback::Unrelated));
	});
}

TEST_CASE("Explorer preserves external references changed during a move", "[studio][explorer][worlds]") {
	studio::Editor editor;
	studio::ToolsProbe::SearchPrepare(editor);
	auto &universe = studio::ToolsProbe::SearchUniverse(editor);
	const auto source = studio::ToolsProbe::SearchWorld(editor);
	const auto target = universe.Create({.Name = Name("callback-reference-target")});
	Entity camera{}, second{};
	universe.Enter(source, [&](Store &store) {
		const auto folder = engine::ecs::Classes::Find(Name("Folder"));
		const Entity parent = store.CreateInstance(folder, "parent");
		camera = store.CreateInstance(engine::ecs::Classes::Find(Name("SurfaceCamera")), "camera");
		const Entity first = store.CreateInstance(engine::scene::PartClass(), "first");
		second = store.CreateInstance(engine::scene::PartClass(), "second");
		REQUIRE(store.SetParent(camera, parent));
		REQUIRE(store.SetProperty(camera, Name("CameraSubject"), &first, sizeof(first)));
		store.OnDescendantRemoving([&](Entity, Entity subject) {
			if (subject == camera)
				REQUIRE(store.SetProperty(camera, Name("CameraSubject"), &second, sizeof(second)));
		});
	});
	CHECK_FALSE(studio::ToolsProbe::MoveToWorld(editor, source, camera, target));
	universe.Enter(source, [&](Store &store) {
		store.ClearDescendantRemoving();
		REQUIRE(store.Alive(camera));
		Entity subject{};
		REQUIRE(store.GetProperty(camera, Name("CameraSubject"), &subject, sizeof(subject)));
		CHECK(subject == second);
	});
	universe.Enter(target, [&](Store &store) { CHECK(store.FindFirstRoot("camera") == NULL_ENTITY); });
}

TEST_CASE("Explorer detects newly readable default properties during a move", "[studio][explorer][worlds]") {
	studio::Editor editor;
	studio::ToolsProbe::SearchPrepare(editor);
	auto &universe = studio::ToolsProbe::SearchUniverse(editor);
	const auto source = studio::ToolsProbe::SearchWorld(editor);
	const auto target = universe.Create({.Name = Name("callback-property-target")});
	Entity root{};
	universe.Enter(source, [&](Store &store) {
		const Entity parent = store.CreateInstance(engine::ecs::Classes::Find(Name("Folder")), "parent");
		root = store.CreateInstance(engine::scene::PartClass(), "root");
		REQUIRE(store.SetParent(root, parent));
		store.Remove<engine::scene::PhysicsProperties>(root);
		const auto before = engine::game::WriteInstanceDocument(store, root);
		store.Set(root, engine::scene::PhysicsProperties{});
		CHECK(engine::game::WriteInstanceDocument(store, root) == before);
		store.Remove<engine::scene::PhysicsProperties>(root);
		store.OnDescendantRemoving([&](Entity, Entity subject) {
			if (subject == root) store.Set(root, engine::scene::PhysicsProperties{});
		});
	});
	CHECK_FALSE(studio::ToolsProbe::MoveToWorld(editor, source, root, target));
	universe.Enter(source, [&](Store &store) {
		store.ClearDescendantRemoving();
		CHECK(store.Alive(root));
		CHECK(store.Has<engine::scene::PhysicsProperties>(root));
	});
	universe.Enter(target, [&](Store &store) { CHECK(store.FindFirstRoot("root") == NULL_ENTITY); });
}

TEST_CASE("Explorer preserves omitted descendants when moving across worlds", "[studio][explorer][worlds]") {
	studio::Editor editor;
	studio::ToolsProbe::SearchPrepare(editor);
	auto &universe = studio::ToolsProbe::SearchUniverse(editor);
	const auto source = studio::ToolsProbe::SearchWorld(editor);
	const auto target = universe.Create({.Name = Name("omitted-move-target")});
	const auto folder = engine::ecs::Classes::Find(Name("Folder"));
	Entity root;
	Entity child;
	Entity grandchild;
	universe.Enter(source, [&](Store &store) {
		root = store.CreateInstance(folder, "root");
		child = store.CreateInstance(folder, "omitted-child");
		grandchild = store.CreateInstance(folder, "grandchild");
		REQUIRE(store.SetParent(child, root));
		REQUIRE(store.SetParent(grandchild, child));
		SECTION("transient") {
			store.Set(child, engine::scene::TransientComponent{});
		}
		SECTION("client local") {
			store.Set(child, engine::ecs::ClientLocal{});
		}
	});
	CHECK_FALSE(studio::ToolsProbe::MoveToWorld(editor, source, root, target));
	universe.Enter(source, [&](Store &store) {
		CHECK(store.Alive(root));
		CHECK(store.Alive(child));
		CHECK(store.Alive(grandchild));
		CHECK(store.ParentOf(child) == root);
		CHECK(store.ParentOf(grandchild) == child);
	});
	universe.Enter(target, [&](Store &store) { CHECK(store.FindFirstRoot("root") == NULL_ENTITY); });
}

TEST_CASE("Explorer moves a subtree only when its target can accept it", "[studio][explorer][worlds]") {
	studio::Editor editor;
	studio::ToolsProbe::SearchPrepare(editor);
	auto &universe = studio::ToolsProbe::SearchUniverse(editor);
	const auto source = studio::ToolsProbe::SearchWorld(editor);
	const auto target = universe.Create({.Name = Name("move-target")});
	Entity root;
	Entity child;
	universe.Enter(source, [&](Store &store) {
		const auto folder = engine::ecs::Classes::Find(Name("Folder"));
		root = store.CreateInstance(folder, "move-root");
		child = store.CreateInstance(folder, "move-child");
		store.SetParent(child, root);
	});
	REQUIRE(root != NULL_ENTITY);
	REQUIRE(child != NULL_ENTITY);
	CHECK_FALSE(studio::ToolsProbe::MoveToWorld(editor, source, root, engine::world::WorldId(123456)));
	universe.Enter(source, [&](Store &store) {
		CHECK(store.Alive(root));
		CHECK(store.Alive(child));
		CHECK(store.ParentOf(child) == root);
	});
	REQUIRE(studio::ToolsProbe::MoveToWorld(editor, source, root, target));
	universe.Enter(source, [&](Store &store) {
		CHECK_FALSE(store.Alive(root));
		CHECK_FALSE(store.Alive(child));
	});
	universe.Enter(target, [&](Store &store) {
		const Entity moved = store.FindFirstRoot("move-root");
		REQUIRE(moved != NULL_ENTITY);
		CHECK(store.FindFirstChild(moved, "move-child") != NULL_ENTITY);
	});
}

TEST_CASE("moving an instance replaces the root selection", "[studio][explorer][worlds]") {
	for (const bool universeRoot : {false, true}) {
		studio::Editor editor;
		studio::ToolsProbe::SearchPrepare(editor);
		auto &universe = studio::ToolsProbe::SearchUniverse(editor);
		const auto source = studio::ToolsProbe::SearchWorld(editor);
		const auto target = universe.Create({.Name = Name("move-selection-target")});
		Entity instance;
		universe.Enter(source, [&](Store &store) {
			instance = store.CreateInstance(engine::ecs::Classes::Find(Name("Folder")), "move-selection");
		});
		studio::ToolsProbe::SelectMoveRoot(editor, source, universeRoot);
		CHECK_FALSE(
			studio::ToolsProbe::MoveToWorld(editor, source, instance, engine::world::WorldId(123456))
		);
		CHECK(studio::ToolsProbe::RootSelected(editor));
		REQUIRE(studio::ToolsProbe::MoveToWorld(editor, source, instance, target));
		CHECK_FALSE(studio::ToolsProbe::RootSelected(editor));
		CHECK(studio::ToolsProbe::SelectedCount(editor) == 1);
	}
}

TEST_CASE("Explorer shift selection cannot reuse another world's anchor", "[studio][explorer][input]") {
	studio::Editor editor;
	studio::ToolsProbe::SearchPrepare(editor);
	auto &universe = studio::ToolsProbe::SearchUniverse(editor);
	const auto firstWorld = studio::ToolsProbe::SearchWorld(editor);
	const auto secondWorld = universe.Create({.Name = Name("other-selection-world")});
	Entity firstAnchor;
	Entity secondAnchor;
	Entity secondTarget;
	const auto folder = engine::ecs::Classes::Find(Name("Folder"));
	universe.Enter(firstWorld, [&](Store &store) { firstAnchor = store.CreateInstance(folder, "anchor"); });
	universe.Enter(secondWorld, [&](Store &store) {
		secondAnchor = store.CreateInstance(folder, "anchor");
		secondTarget = store.CreateInstance(folder, "target");
	});
	REQUIRE(firstAnchor == secondAnchor);
	studio::ToolsProbe::SelectInWorld(editor, firstWorld, firstAnchor);
	studio::ToolsProbe::ShiftSelectInWorld(editor, secondWorld, secondTarget);
	CHECK(studio::ToolsProbe::SelectedCount(editor) == 1);
	CHECK(studio::ToolsProbe::SearchSelected(editor, secondTarget));
	studio::ToolsProbe::ShiftSelectInWorld(editor, secondWorld, secondAnchor);
	CHECK(studio::ToolsProbe::SelectedCount(editor) == 2);
	CHECK(studio::ToolsProbe::SearchSelected(editor, secondAnchor));
}

TEST_CASE("Explorer shift selection follows a selection made outside the tree", "[studio][explorer][input]") {
	studio::Editor editor;
	studio::ToolsProbe::SearchPrepare(editor);
	auto &universe = studio::ToolsProbe::SearchUniverse(editor);
	const auto firstWorld = studio::ToolsProbe::SearchWorld(editor);
	const auto secondWorld = universe.Create({.Name = Name("external-selection-world")});
	Entity oldAnchor;
	Entity reusedAnchor;
	Entity selected;
	Entity target;
	const auto folder = engine::ecs::Classes::Find(Name("Folder"));
	universe.Enter(firstWorld, [&](Store &store) { oldAnchor = store.CreateInstance(folder, "old"); });
	universe.Enter(secondWorld, [&](Store &store) {
		reusedAnchor = store.CreateInstance(folder, "old");
		selected = store.CreateInstance(folder, "selected");
		target = store.CreateInstance(folder, "target");
	});
	REQUIRE(oldAnchor == reusedAnchor);
	studio::ToolsProbe::SelectInWorld(editor, firstWorld, oldAnchor);
	studio::ToolsProbe::SelectOutsideExplorer(editor, secondWorld, selected);
	studio::ToolsProbe::ShiftSelectInWorld(editor, secondWorld, target);
	CHECK(studio::ToolsProbe::SelectedCount(editor) == 2);
	CHECK_FALSE(studio::ToolsProbe::SearchSelected(editor, reusedAnchor));
	CHECK(studio::ToolsProbe::SearchSelected(editor, selected));
	CHECK(studio::ToolsProbe::SearchSelected(editor, target));
}

TEST_CASE("Explorer shift selection follows plugin and additive selections", "[studio][explorer][input]") {
	studio::Editor editor;
	studio::ToolsProbe::SearchPrepare(editor);
	auto &universe = studio::ToolsProbe::SearchUniverse(editor);
	const auto world = studio::ToolsProbe::SearchWorld(editor);
	Entity first;
	Entity selected;
	Entity target;
	const auto folder = engine::ecs::Classes::Find(Name("Folder"));
	universe.Enter(world, [&](Store &store) {
		first = store.CreateInstance(folder, "first");
		selected = store.CreateInstance(folder, "selected");
		target = store.CreateInstance(folder, "target");
	});
	studio::ToolsProbe::SelectInWorld(editor, world, first);
	SECTION("plugin replaces selection") {
		Store pluginStore("selection-plugin");
		studio::LoadedPlugin plugin;
		auto surface = studio::MakePluginSurface(editor, plugin, pluginStore);
		using engine::script::HostValue;
		const std::array arguments{HostValue::List({HostValue::Of(selected)})};
		HostValue result;
		std::string error;
		REQUIRE(surface->Call("Selection.Set", arguments, result, error));
	}
	SECTION("control selection adds an instance") {
		studio::ToolsProbe::AddInWorld(editor, world, selected);
	}
	SECTION("control selection toggles an instance off") {
		studio::ToolsProbe::AddInWorld(editor, world, selected);
		studio::ToolsProbe::AddInWorld(editor, world, selected);
	}
	studio::ToolsProbe::ShiftSelectInWorld(editor, world, target);
	CHECK(studio::ToolsProbe::SelectedCount(editor) == 2);
	CHECK_FALSE(studio::ToolsProbe::SearchSelected(editor, first));
	CHECK(studio::ToolsProbe::SearchSelected(editor, selected));
	CHECK(studio::ToolsProbe::SearchSelected(editor, target));
}

TEST_CASE("Explorer shift selection follows duplicated instances", "[studio][explorer][input]") {
	studio::Editor editor;
	studio::ToolsProbe::TreePrepare(editor);
	auto &universe = studio::ToolsProbe::SearchUniverse(editor);
	const auto world = studio::ToolsProbe::SearchWorld(editor);
	Entity original;
	Entity target;
	const auto folder = engine::ecs::Classes::Find(Name("Folder"));
	universe.Enter(world, [&](Store &store) {
		original = store.CreateInstance(folder, "first");
		target = store.CreateInstance(folder, "target");
	});
	studio::ToolsProbe::SelectInWorld(editor, world, original);
	studio::ToolsProbe::DuplicateSelection(editor);
	const Entity copy = studio::ToolsProbe::FirstSelection(editor);
	REQUIRE(copy != NULL_ENTITY);
	REQUIRE(copy != original);
	CHECK(studio::ToolsProbe::SelectionAnchor(editor) == copy);
	studio::ToolsProbe::ShiftSelectInWorld(editor, world, target);
	CHECK(studio::ToolsProbe::SearchSelected(editor, copy));
	CHECK(studio::ToolsProbe::SearchSelected(editor, target));
}

TEST_CASE("Explorer shift selection follows gallery users", "[studio][explorer][input]") {
	studio::Editor editor;
	studio::ToolsProbe::SearchPrepare(editor);
	auto &universe = studio::ToolsProbe::SearchUniverse(editor);
	const auto firstWorld = studio::ToolsProbe::SearchWorld(editor);
	const auto secondWorld = universe.Create({.Name = Name("gallery-selection-world")});
	const Name asset("gallery-selection-image");
	Entity oldAnchor;
	Entity reusedAnchor;
	Entity selected;
	Entity target;
	universe.Enter(firstWorld, [&](Store &store) {
		oldAnchor = store.CreateInstance(engine::ecs::Classes::Find(Name("Folder")), "old");
	});
	universe.Enter(secondWorld, [&](Store &store) {
		reusedAnchor = store.CreateInstance(engine::ecs::Classes::Find(Name("Folder")), "old");
		selected = store.CreateInstance(engine::ecs::Classes::Find(Name("MeshPart")), "selected");
		target = store.CreateInstance(engine::ecs::Classes::Find(Name("Folder")), "target");
		REQUIRE(store.SetProperty(selected, Name("TextureID"), &asset, sizeof(asset)));
	});
	REQUIRE(oldAnchor == reusedAnchor);
	bool extraViewport = false;
	SECTION("main viewport") {}
	SECTION("focused extra viewport") {
		extraViewport = true;
		studio::ToolsProbe::FocusExtraForWorld(editor, firstWorld);
	}
	studio::ToolsProbe::SelectInWorld(editor, firstWorld, oldAnchor);
	studio::ToolsProbe::SelectGalleryUsers(editor, secondWorld, asset);
	REQUIRE(studio::ToolsProbe::FirstSelection(editor) == selected);
	if (extraViewport) {
		CHECK(studio::ToolsProbe::MainViewportWorld(editor) == firstWorld);
		CHECK(studio::ToolsProbe::ExtraWorld(editor) == secondWorld);
	} else {
		CHECK(studio::ToolsProbe::MainViewportWorld(editor) == secondWorld);
	}
	CHECK(studio::ToolsProbe::SelectionAnchor(editor) == selected);
	studio::ToolsProbe::ShiftSelectInWorld(editor, secondWorld, target);
	CHECK(studio::ToolsProbe::SearchSelected(editor, selected));
	CHECK(studio::ToolsProbe::SearchSelected(editor, target));
	CHECK_FALSE(studio::ToolsProbe::SearchSelected(editor, reusedAnchor));
}

TEST_CASE("a stale gallery tile cannot select a replacement world", "[studio][gallery][input]") {
	studio::Editor editor;
	studio::ToolsProbe::SearchPrepare(editor);
	auto &universe = studio::ToolsProbe::SearchUniverse(editor);
	const auto selectedWorld = studio::ToolsProbe::SearchWorld(editor);
	const auto oldWorld = universe.Create({.Name = Name("old-gallery-world")});
	const Name asset("retired-gallery-image");
	Entity selected;
	universe.Enter(selectedWorld, [&](Store &store) {
		selected = store.CreateInstance(engine::ecs::Classes::Find(Name("Folder")), "selected");
	});
	universe.Enter(oldWorld, [&](Store &store) {
		const Entity image = store.CreateInstance(engine::ecs::Classes::Find(Name("MeshPart")), "image");
		REQUIRE(store.SetProperty(image, Name("TextureID"), &asset, sizeof(asset)));
	});
	const auto stale = studio::ToolsProbe::ScanGallery(editor, asset);
	REQUIRE(stale.World == oldWorld);
	SECTION("ending residency marks the Gallery for rescan") {
		REQUIRE(studio::ToolsProbe::GalleryScanned(editor));
		studio::ToolsProbe::ReleaseWorld(editor, oldWorld);
		CHECK_FALSE(studio::ToolsProbe::GalleryScanned(editor));
	}
	SECTION("a world slot is reused") {
		REQUIRE(universe.Destroy(oldWorld) == engine::world::WorldStatus::Ok);
		const auto replacement = universe.Create({.Name = Name("replacement-gallery-world")});
		REQUIRE(replacement == oldWorld);
		universe.Enter(replacement, [&](Store &store) {
			const Entity image =
				store.CreateInstance(engine::ecs::Classes::Find(Name("MeshPart")), "replacement");
			REQUIRE(store.SetProperty(image, Name("TextureID"), &asset, sizeof(asset)));
		});
		studio::ToolsProbe::SelectInWorld(editor, selectedWorld, selected);
		studio::ToolsProbe::SelectGalleryEntry(editor, stale);
		CHECK(studio::ToolsProbe::SelectionWorld(editor) == selectedWorld);
		CHECK(studio::ToolsProbe::SearchSelected(editor, selected));
		CHECK(studio::ToolsProbe::MainViewportWorld(editor) == selectedWorld);
	}
}

TEST_CASE("Explorer shift selection replaces a deleted anchor", "[studio][explorer][input]") {
	studio::Editor editor;
	studio::ToolsProbe::SearchPrepare(editor);
	auto &universe = studio::ToolsProbe::SearchUniverse(editor);
	const auto world = studio::ToolsProbe::SearchWorld(editor);
	Entity anchor;
	Entity first;
	Entity second;
	const auto folder = engine::ecs::Classes::Find(Name("Folder"));
	universe.Enter(world, [&](Store &store) {
		anchor = store.CreateInstance(folder, "anchor");
		first = store.CreateInstance(folder, "first");
		second = store.CreateInstance(folder, "second");
	});
	studio::ToolsProbe::SelectInWorld(editor, world, anchor);
	universe.Enter(world, [&](Store &store) { store.Destroy(anchor); });
	studio::ToolsProbe::ShiftSelectInWorld(editor, world, first);
	REQUIRE(studio::ToolsProbe::SelectedCount(editor) == 1);
	CHECK(studio::ToolsProbe::SearchSelected(editor, first));
	studio::ToolsProbe::ShiftSelectInWorld(editor, world, second);
	CHECK(studio::ToolsProbe::SelectedCount(editor) == 2);
	CHECK(studio::ToolsProbe::SearchSelected(editor, first));
	CHECK(studio::ToolsProbe::SearchSelected(editor, second));
}

TEST_CASE("Explorer context menu displays configured shortcuts", "[studio][explorer][input]") {
	const auto previous = studio::ConfigRoot();
	const auto scratch = std::filesystem::temp_directory_path() / "studio-explorer-shortcuts";
	std::filesystem::create_directories(scratch);
	studio::SetConfigRoot(scratch);
	struct Restore {
		std::filesystem::path Previous;
		~Restore() {
			studio::SetConfigRoot(Previous);
		}
	} restore{previous};
	studio::Editor editor;
	studio::ToolsProbe::SearchPrepare(editor);
	REQUIRE(studio::ToolsProbe::SearchInitialise(editor));
	const auto world = studio::ToolsProbe::SearchWorld(editor);
	Entity instance;
	studio::ToolsProbe::SearchUniverse(editor).Enter(world, [&](Store &store) {
		instance = store.CreateInstance(engine::ecs::Classes::Find(Name("Folder")), "item");
	});
	struct RestoreBindings {
		std::vector<studio::Chord> Keys;
		RestoreBindings() {
			for (const auto &binding : studio::Keybinds::All())
				Keys.push_back(binding.Keys);
		}
		~RestoreBindings() {
			auto bindings = studio::Keybinds::All();
			for (size_t index = 0; index < Keys.size(); index++)
				bindings[index].Keys = Keys[index];
		}
	} bindings;
	const studio::Chord duplicate{ImGuiKey_F23};
	const studio::Chord remove{ImGuiKey_F24};
	studio::Keybinds::Set(studio::Action::Duplicate, duplicate);
	studio::Keybinds::Set(studio::Action::Delete, remove);
	const std::string labels = studio::ToolsProbe::InstanceActionLabels(editor, world, instance);
	studio::Keybinds::Set(studio::Action::Duplicate, {});
	studio::Keybinds::Set(studio::Action::Delete, {});
	const std::string unbound = studio::ToolsProbe::InstanceActionLabels(editor, world, instance);
	CHECK(labels.find(duplicate.Text()) != std::string::npos);
	CHECK(labels.find(remove.Text()) != std::string::npos);
	CHECK(unbound.find("Duplicate") != std::string::npos);
	CHECK(unbound.find("Delete") != std::string::npos);
	CHECK(unbound.find(duplicate.Text()) == std::string::npos);
	CHECK(unbound.find(remove.Text()) == std::string::npos);
}

TEST_CASE("queued camera view stays with its original viewport and world", "[studio][explorer][input]") {
	const auto previous = studio::ConfigRoot();
	const auto scratch = std::filesystem::temp_directory_path() / "studio-explorer-queued-camera";
	std::filesystem::create_directories(scratch);
	studio::SetConfigRoot(scratch);
	struct Restore {
		std::filesystem::path Previous;
		~Restore() {
			studio::SetConfigRoot(Previous);
		}
	} restore{previous};
	studio::Editor editor;
	studio::ToolsProbe::TreePrepare(editor);
	REQUIRE(studio::ToolsProbe::SearchInitialise(editor));
	auto &universe = studio::ToolsProbe::SearchUniverse(editor);
	const auto firstWorld = studio::ToolsProbe::SearchWorld(editor);
	const auto secondWorld = universe.Create({.Name = Name("camera-other-world")});
	const auto cameraClass = engine::ecs::Classes::Find(Name("Camera"));
	REQUIRE(cameraClass.IsValid());
	Entity first;
	Entity second;
	universe.Enter(firstWorld, [&](Store &store) {
		first = store.CreateInstance(cameraClass, "first-camera");
	});
	universe.Enter(secondWorld, [&](Store &store) {
		second = store.CreateInstance(cameraClass, "second-camera");
	});
	REQUIRE(first == second);

	SECTION("the original viewport remains open") {
		studio::ToolsProbe::DrawInstanceAction(editor, firstWorld, first, "Look Through Camera");
		REQUIRE(studio::ToolsProbe::LookQueued(editor));
		studio::ToolsProbe::ApplyPending(editor);
		CHECK(studio::ToolsProbe::MainFollow(editor) == first);
	}
	SECTION("the viewport changes worlds") {
		studio::ToolsProbe::DrawInstanceAction(editor, firstWorld, first, "Look Through Camera");
		REQUIRE(studio::ToolsProbe::LookQueued(editor));
		studio::ToolsProbe::SetActiveWorld(editor, secondWorld);
		studio::ToolsProbe::ApplyPending(editor);
		CHECK(studio::ToolsProbe::MainFollow(editor) == NULL_ENTITY);
	}
	SECTION("the extra viewport closes") {
		studio::ToolsProbe::FocusExtraForWorld(editor, firstWorld);
		studio::ToolsProbe::DrawInstanceAction(editor, firstWorld, first, "Look Through Camera");
		REQUIRE(studio::ToolsProbe::LookQueued(editor));
		studio::ToolsProbe::CloseExtra(editor);
		studio::ToolsProbe::ApplyPending(editor);
		CHECK(studio::ToolsProbe::MainFollow(editor) == NULL_ENTITY);
		CHECK(studio::ToolsProbe::ExtraFollow(editor) == NULL_ENTITY);
	}
}

TEST_CASE("Explorer zoom targets only a viewport showing the instance world", "[studio][explorer][input]") {
	const auto previous = studio::ConfigRoot();
	const auto scratch = std::filesystem::temp_directory_path() / "studio-explorer-zoom-world";
	std::filesystem::create_directories(scratch);
	studio::SetConfigRoot(scratch);
	struct Restore {
		std::filesystem::path Previous;
		~Restore() {
			studio::SetConfigRoot(Previous);
		}
	} restore{previous};
	studio::Editor editor;
	studio::ToolsProbe::TreePrepare(editor);
	REQUIRE(studio::ToolsProbe::SearchInitialise(editor));
	auto &universe = studio::ToolsProbe::SearchUniverse(editor);
	const auto targetWorld = universe.Create({.Name = Name("zoom-target")});
	const auto otherWorld = universe.Create({.Name = Name("zoom-other")});
	Entity target;
	universe.Enter(targetWorld, [&](Store &store) {
		target = store.CreateInstance(engine::ecs::Classes::Find(Name("Part")), "target");
		store.Set(target, engine::scene::Transform{engine::core::CFrame(engine::core::Vector3{120, 80, 50})});
	});
	REQUIRE(target != NULL_ENTITY);

	SECTION("the target world is not displayed") {
		studio::ToolsProbe::FocusExtraForWorld(editor, otherWorld);
		const auto mainBefore = studio::ToolsProbe::MainFrame(editor).Position;
		const auto extraBefore = studio::ToolsProbe::ExtraFrame(editor).Position;
		studio::ToolsProbe::DrawInstanceAction(editor, targetWorld, target, "Zoom To");
		studio::ToolsProbe::ApplyPending(editor);
		CHECK(studio::ToolsProbe::MainFrame(editor).Position == mainBefore);
		CHECK(studio::ToolsProbe::ExtraFrame(editor).Position != extraBefore);
		CHECK(studio::ToolsProbe::ExtraWorld(editor) == targetWorld);
	}
	SECTION("the target world is displayed") {
		studio::ToolsProbe::FocusExtraForWorld(editor, targetWorld);
		const auto mainBefore = studio::ToolsProbe::MainFrame(editor).Position;
		const auto extraBefore = studio::ToolsProbe::ExtraFrame(editor).Position;
		studio::ToolsProbe::DrawInstanceAction(editor, targetWorld, target, "Zoom To");
		studio::ToolsProbe::ApplyPending(editor);
		CHECK(studio::ToolsProbe::MainFrame(editor).Position == mainBefore);
		CHECK(studio::ToolsProbe::ExtraFrame(editor).Position != extraBefore);
	}
	SECTION("all viewports are closed") {
		studio::ToolsProbe::FocusExtraForWorld(editor, otherWorld);
		studio::ToolsProbe::CloseExtra(editor);
		studio::ToolsProbe::CloseMainViewport(editor);
		const auto mainBefore = studio::ToolsProbe::MainFrame(editor).Position;
		const auto extraBefore = studio::ToolsProbe::ExtraFrame(editor).Position;
		studio::ToolsProbe::DrawInstanceAction(editor, targetWorld, target, "Zoom To");
		studio::ToolsProbe::ApplyPending(editor);
		CHECK(studio::ToolsProbe::MainFrame(editor).Position == mainBefore);
		CHECK(studio::ToolsProbe::ExtraFrame(editor).Position == extraBefore);
		CHECK(studio::ToolsProbe::ExtraWorld(editor) == otherWorld);
	}
	SECTION("zoom leaves a followed scene camera") {
		Entity camera;
		universe.Enter(targetWorld, [&](Store &store) {
			camera = store.CreateInstance(engine::ecs::Classes::Find(Name("Camera")), "camera");
		});
		REQUIRE(camera != NULL_ENTITY);
		studio::ToolsProbe::FocusExtraForWorld(editor, targetWorld);
		studio::ToolsProbe::FollowExtra(editor, camera);
		studio::ToolsProbe::DrawInstanceAction(editor, targetWorld, target, "Zoom To");
		studio::ToolsProbe::ApplyPending(editor);
		CHECK(studio::ToolsProbe::ExtraFollow(editor) == NULL_ENTITY);
	}
	SECTION("a later selection survives a queued zoom") {
		Entity other;
		universe.Enter(otherWorld, [&](Store &store) {
			other = store.CreateInstance(engine::ecs::Classes::Find(Name("Part")), "other");
		});
		REQUIRE(other != NULL_ENTITY);
		studio::ToolsProbe::FocusExtraForWorld(editor, otherWorld);
		studio::ToolsProbe::DrawInstanceAction(editor, targetWorld, target, "Zoom To");
		studio::ToolsProbe::SelectInWorld(editor, otherWorld, other);
		studio::ToolsProbe::ApplyPending(editor);
		CHECK(studio::ToolsProbe::SelectionWorld(editor) == otherWorld);
		CHECK(studio::ToolsProbe::SearchSelected(editor, other));
	}
	SECTION("the runtime camera in a replica cannot be framed") {
		const auto replica = studio::ToolsProbe::StartReplica(editor, targetWorld);
		Entity replicaTarget;
		universe.Enter(replica, [&](Store &store) {
			replicaTarget = store.CreatePredicted("replica target");
			store.Set(
				replicaTarget,
				engine::scene::Transform{engine::core::CFrame(engine::core::Vector3{100, 0, 0})}
			);
		});
		REQUIRE(replicaTarget != NULL_ENTITY);
		studio::ToolsProbe::FocusExtraForWorld(editor, replica);
		const auto extraBefore = studio::ToolsProbe::ExtraFrame(editor).Position;
		studio::ToolsProbe::DrawInstanceAction(editor, replica, replicaTarget, "Zoom To");
		studio::ToolsProbe::ApplyPending(editor);
		CHECK(studio::ToolsProbe::ExtraFrame(editor).Position == extraBefore);
		CHECK(studio::ToolsProbe::ExtraWorld(editor) == replica);
	}
}

TEST_CASE("Explorer queued actions retain the selected world and instances", "[studio][explorer][input]") {
	const auto previous = studio::ConfigRoot();
	const auto scratch = std::filesystem::temp_directory_path() / "studio-explorer-queued-action";
	std::filesystem::create_directories(scratch);
	studio::SetConfigRoot(scratch);
	struct Restore {
		std::filesystem::path Previous;
		~Restore() {
			studio::SetConfigRoot(Previous);
		}
	} restore{previous};
	studio::Editor editor;
	studio::ToolsProbe::TreePrepare(editor);
	REQUIRE(studio::ToolsProbe::SearchInitialise(editor));
	auto &universe = studio::ToolsProbe::SearchUniverse(editor);
	const auto firstWorld = studio::ToolsProbe::SearchWorld(editor);
	const auto secondWorld = universe.Create({.Name = Name("queued-action-second")});
	Entity first;
	Entity second;
	universe.Enter(firstWorld, [&](Store &store) {
		first = store.CreateInstance(engine::ecs::Classes::Find(Name("Folder")), "first");
	});
	universe.Enter(secondWorld, [&](Store &store) {
		second = store.CreateInstance(engine::ecs::Classes::Find(Name("Folder")), "second");
	});
	REQUIRE(first == second);
	studio::ToolsProbe::SelectInWorld(editor, firstWorld, first);
	bool duplicate = false;
	SECTION("duplicate") {
		duplicate = true;
	}
	SECTION("delete") {}
	studio::ToolsProbe::DrawInstanceAction(editor, firstWorld, first, duplicate ? "Duplicate" : "Delete");
	REQUIRE(studio::ToolsProbe::DestructiveActionQueued(editor));
	studio::ToolsProbe::SelectInWorld(editor, secondWorld, second);
	studio::ToolsProbe::ApplyPending(editor);
	universe.Enter(firstWorld, [&](Store &store) {
		CHECK(store.Alive(first) == duplicate);
		size_t copies = 0;
		store.EachEntity([&](Entity entity) {
			if (store.InstanceNameOf(entity) == Name("first")) ++copies;
		});
		CHECK(copies == (duplicate ? 2 : 0));
	});
	universe.Enter(secondWorld, [&](Store &store) {
		CHECK(store.Alive(second));
		size_t copies = 0;
		store.EachEntity([&](Entity entity) {
			if (store.InstanceNameOf(entity) == Name("second")) ++copies;
		});
		CHECK(copies == 1);
	});
	CHECK(studio::ToolsProbe::SearchSelected(editor, second));
}

TEST_CASE("Explorer queued actions keep multiple and stale targets safe", "[studio][explorer][input]") {
	const auto previous = studio::ConfigRoot();
	const auto scratch = std::filesystem::temp_directory_path() / "studio-explorer-queued-multiple";
	std::filesystem::create_directories(scratch);
	studio::SetConfigRoot(scratch);
	struct Restore {
		std::filesystem::path Previous;
		~Restore() {
			studio::SetConfigRoot(Previous);
		}
	} restore{previous};
	studio::Editor editor;
	studio::ToolsProbe::TreePrepare(editor);
	REQUIRE(studio::ToolsProbe::SearchInitialise(editor));
	auto &universe = studio::ToolsProbe::SearchUniverse(editor);
	const auto world = studio::ToolsProbe::SearchWorld(editor);
	Entity first;
	Entity second;
	Entity replacement = NULL_ENTITY;
	universe.Enter(world, [&](Store &store) {
		const auto klass = engine::ecs::Classes::Find(Name("Folder"));
		first = store.CreateInstance(klass, "first");
		second = store.CreateInstance(klass, "second");
	});
	studio::ToolsProbe::SelectInWorld(editor, world, first);
	studio::ToolsProbe::AddInWorld(editor, world, second);
	bool duplicate = false;
	SECTION("duplicate selected instances") {
		duplicate = true;
	}
	SECTION("delete selected instances") {}
	SECTION("deleted target is not substituted") {
		universe.Enter(world, [&](Store &store) {
			store.DestroyInstance(first);
			replacement = store.CreateInstance(engine::ecs::Classes::Find(Name("Folder")), "replacement");
		});
	}
	studio::ToolsProbe::DrawInstanceAction(editor, world, second, duplicate ? "Duplicate" : "Delete");
	REQUIRE(studio::ToolsProbe::DestructiveActionQueued(editor));
	studio::ToolsProbe::SelectInWorld(editor, world, second);
	studio::ToolsProbe::ApplyPending(editor);
	universe.Enter(world, [&](Store &store) {
		if (replacement != NULL_ENTITY) CHECK(store.Alive(replacement));
		CHECK(store.Alive(second) == duplicate);
		size_t firstCount = 0;
		size_t secondCount = 0;
		store.EachEntity([&](Entity entity) {
			if (store.InstanceNameOf(entity) == Name("first")) ++firstCount;
			if (store.InstanceNameOf(entity) == Name("second")) ++secondCount;
		});
		CHECK(firstCount == (duplicate ? 2 : 0));
		CHECK(secondCount == (duplicate ? 2 : 0));
	});
}

TEST_CASE(
	"Explorer queued delete removes destroyed rows from a later selection", "[studio][explorer][input]"
) {
	const auto previous = studio::ConfigRoot();
	const auto scratch = std::filesystem::temp_directory_path() / "studio-explorer-queued-overlap";
	std::filesystem::create_directories(scratch);
	studio::SetConfigRoot(scratch);
	struct Restore {
		std::filesystem::path Previous;
		~Restore() {
			studio::SetConfigRoot(Previous);
		}
	} restore{previous};
	studio::Editor editor;
	studio::ToolsProbe::TreePrepare(editor);
	REQUIRE(studio::ToolsProbe::SearchInitialise(editor));
	auto &universe = studio::ToolsProbe::SearchUniverse(editor);
	const auto world = studio::ToolsProbe::SearchWorld(editor);
	Entity parent;
	Entity child;
	Entity survivor;
	universe.Enter(world, [&](Store &store) {
		const auto klass = engine::ecs::Classes::Find(Name("Folder"));
		parent = store.CreateInstance(klass, "parent");
		child = store.CreateInstance(klass, "child");
		survivor = store.CreateInstance(klass, "survivor");
		store.SetParent(child, parent);
	});
	studio::ToolsProbe::SelectInWorld(editor, world, parent);
	studio::ToolsProbe::DrawInstanceAction(editor, world, parent, "Delete");
	REQUIRE(studio::ToolsProbe::DestructiveActionQueued(editor));
	studio::ToolsProbe::AddInWorld(editor, world, child);
	studio::ToolsProbe::AddInWorld(editor, world, survivor);
	studio::ToolsProbe::ApplyPending(editor);
	universe.Enter(world, [&](Store &store) {
		CHECK_FALSE(store.Alive(parent));
		CHECK_FALSE(store.Alive(child));
		CHECK(store.Alive(survivor));
	});
	CHECK_FALSE(studio::ToolsProbe::SearchSelected(editor, parent));
	CHECK_FALSE(studio::ToolsProbe::SearchSelected(editor, child));
	CHECK(studio::ToolsProbe::SearchSelected(editor, survivor));
	CHECK(studio::ToolsProbe::SelectedCount(editor) == 1);
}

TEST_CASE("Explorer renames the selected world when instance IDs match", "[studio][explorer][input]") {
	const auto previous = studio::ConfigRoot();
	const auto scratch = std::filesystem::temp_directory_path() / "studio-explorer-rename";
	std::filesystem::create_directories(scratch);
	studio::SetConfigRoot(scratch);
	struct Restore {
		std::filesystem::path Previous;
		~Restore() {
			studio::SetConfigRoot(Previous);
		}
	} restore{previous};
	studio::Editor editor;
	studio::ToolsProbe::TreePrepare(editor);
	REQUIRE(studio::ToolsProbe::SearchInitialise(editor));
	auto &universe = studio::ToolsProbe::SearchUniverse(editor);
	const auto firstWorld = studio::ToolsProbe::SearchWorld(editor);
	const auto secondWorld = universe.Create({.Name = Name("rename-second")});
	Entity first;
	Entity second;
	universe.Enter(firstWorld, [&](Store &store) {
		first = store.CreateInstance(engine::ecs::Classes::Find(Name("Folder")), "first");
	});
	universe.Enter(secondWorld, [&](Store &store) {
		second = store.CreateInstance(engine::ecs::Classes::Find(Name("Folder")), "second");
	});
	REQUIRE(first == second);
	studio::ToolsProbe::SelectInWorld(editor, secondWorld, second);
	SECTION("selection stays on the renamed instance") {
		studio::ToolsProbe::BeginRename(editor, second);
	}
	SECTION("selection changes while renaming") {
		studio::ToolsProbe::BeginRename(editor, second);
		studio::ToolsProbe::SelectInWorld(editor, firstWorld, first);
	}
	SECTION("selection changes before a queued rename starts") {
		studio::ToolsProbe::QueueRename(editor, second);
		studio::ToolsProbe::SelectInWorld(editor, firstWorld, first);
		studio::ToolsProbe::ApplyPending(editor);
	}
	SECTION("world residency ends during a rename") {
		studio::ToolsProbe::BeginRename(editor, second);
		studio::ToolsProbe::QueueRename(editor, second);
		studio::ToolsProbe::ReleaseWorld(editor, secondWorld);
		CHECK_FALSE(studio::ToolsProbe::RenameQueued(editor));
		const std::array worlds{firstWorld, secondWorld};
		studio::ToolsProbe::DrawWorldRows(editor, worlds, second);
		const auto fields = studio::ToolsProbe::DrawWorldRows(editor, worlds, second);
		CHECK(ImGui::GetActiveID() != fields[0]);
		CHECK(ImGui::GetActiveID() != fields[1]);
		return;
	}
	const std::array worlds{firstWorld, secondWorld};
	studio::ToolsProbe::DrawWorldRows(editor, worlds, second);
	const auto fields = studio::ToolsProbe::DrawWorldRows(editor, worlds, second);
	CHECK(ImGui::GetActiveID() == fields[1]);
	ImGui::GetIO().AddInputCharactersUTF8("-edited");
	studio::ToolsProbe::DrawWorldRows(editor, worlds, second);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
	studio::ToolsProbe::DrawWorldRows(editor, worlds, second);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
	studio::ToolsProbe::DrawWorldRows(editor, worlds, second);
	studio::ToolsProbe::ApplyPending(editor);
	universe.Enter(firstWorld, [&](Store &store) { CHECK(store.InstanceNameOf(first) == Name("first")); });
	universe.Enter(secondWorld, [&](Store &store) {
		CHECK(std::string(store.InstanceNameOf(second).Text()) == "-edited");
	});
}

TEST_CASE("Explorer drags the selected rows and undo restores their parents", "[studio][explorer][input]") {
	const auto previous = studio::ConfigRoot();
	const auto scratch = std::filesystem::temp_directory_path() / "studio-explorer-drag";
	std::filesystem::create_directories(scratch);
	studio::SetConfigRoot(scratch);
	struct Restore {
		std::filesystem::path Previous;
		~Restore() {
			studio::SetConfigRoot(Previous);
		}
	} restore{previous};
	studio::Editor editor;
	studio::ToolsProbe::TreePrepare(editor);
	REQUIRE(studio::ToolsProbe::SearchInitialise(editor));
	auto &universe = studio::ToolsProbe::SearchUniverse(editor);
	const auto world = studio::ToolsProbe::SearchWorld(editor);
	Entity first;
	Entity second;
	Entity parent;
	universe.Enter(world, [&](Store &store) {
		const auto folder = engine::ecs::Classes::Find(Name("Folder"));
		first = store.CreateInstance(folder, "first");
		second = store.CreateInstance(folder, "second");
		parent = store.CreateInstance(folder, "parent");
	});
	const ImVec2 firstPoint = studio::ToolsProbe::TreeDraw(editor, first);
	const ImVec2 secondPoint = studio::ToolsProbe::TreeDraw(editor, second);
	const ImVec2 parentPoint = studio::ToolsProbe::TreeDraw(editor, parent);
	auto &io = ImGui::GetIO();
	const auto move = [&](ImVec2 point) {
		io.AddMousePosEvent(point.x, point.y);
		studio::ToolsProbe::TreeDraw(editor, first);
	};
	const auto button = [&](bool down) {
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
		studio::ToolsProbe::TreeDraw(editor, first);
	};
	move(firstPoint);
	move(firstPoint);
	button(true);
	button(false);
	REQUIRE(studio::ToolsProbe::SearchSelected(editor, first));
	io.AddKeyEvent(ImGuiMod_Ctrl, true);
	move(secondPoint);
	move(secondPoint);
	button(true);
	button(false);
	io.AddKeyEvent(ImGuiMod_Ctrl, false);
	move(firstPoint);
	move(firstPoint);
	REQUIRE(studio::ToolsProbe::SelectedCount(editor) == 2);
	button(true);
	CHECK(studio::ToolsProbe::SelectedCount(editor) == 2);
	button(false);
	REQUIRE(studio::ToolsProbe::SelectedCount(editor) == 1);
	REQUIRE(studio::ToolsProbe::SearchSelected(editor, first));
	io.AddKeyEvent(ImGuiMod_Ctrl, true);
	move(secondPoint);
	move(secondPoint);
	button(true);
	button(false);
	io.AddKeyEvent(ImGuiMod_Ctrl, false);
	move(firstPoint);
	move(firstPoint);
	REQUIRE(studio::ToolsProbe::SelectedCount(editor) == 2);
	button(true);
	CHECK(studio::ToolsProbe::SelectedCount(editor) == 2);
	move(ImVec2(firstPoint.x + 20, firstPoint.y));
	move(parentPoint);
	move(parentPoint);
	button(false);
	REQUIRE(studio::ToolsProbe::ReparentPending(editor));
	studio::ToolsProbe::ApplyPending(editor);
	universe.Enter(world, [&](Store &store) {
		CHECK(store.ParentOf(first) == parent);
		CHECK(store.ParentOf(second) == parent);
	});
	REQUIRE(studio::ToolsProbe::Undo(editor));
	universe.Enter(world, [&](Store &store) {
		CHECK(store.ParentOf(first) == NULL_ENTITY);
		CHECK(store.ParentOf(second) == NULL_ENTITY);
	});
	CHECK_FALSE(studio::ToolsProbe::Undo(editor));
}

TEST_CASE("Explorer does not reparent a replacement world from a stale drag", "[studio][explorer][input]") {
	studio::Editor editor;
	studio::ToolsProbe::TreePrepare(editor);
	auto &universe = studio::ToolsProbe::SearchUniverse(editor);
	const auto world = studio::ToolsProbe::SearchWorld(editor);
	const auto folder = engine::ecs::Classes::Find(Name("Folder"));
	Entity child;
	Entity parent;
	universe.Enter(world, [&](Store &store) {
		child = store.CreateInstance(folder, "child");
		parent = store.CreateInstance(folder, "parent");
	});
	studio::ToolsProbe::QueueReparent(editor, world, parent, {child});
	REQUIRE(universe.Destroy(world) == engine::world::WorldStatus::Ok);
	const auto replacement = universe.Create({.Name = Name("replacement")});
	REQUIRE(replacement == world);
	universe.Enter(replacement, [&](Store &store) {
		REQUIRE(store.CreateInstance(folder, "replacement-child") == child);
		REQUIRE(store.CreateInstance(folder, "replacement-parent") == parent);
	});
	studio::ToolsProbe::ApplyPending(editor);
	universe.Enter(replacement, [&](Store &store) { CHECK(store.ParentOf(child) == NULL_ENTITY); });
}

TEST_CASE("Explorer keeps the original drag identity across frames", "[studio][explorer][input]") {
	studio::Editor editor;
	studio::ToolsProbe::TreePrepare(editor);
	REQUIRE(studio::ToolsProbe::SearchInitialise(editor));
	auto &universe = studio::ToolsProbe::SearchUniverse(editor);
	const auto world = studio::ToolsProbe::SearchWorld(editor);
	const auto folder = engine::ecs::Classes::Find(Name("Folder"));
	Entity child;
	Entity parent;
	universe.Enter(world, [&](Store &store) {
		child = store.CreateInstance(folder, "child");
		parent = store.CreateInstance(folder, "parent");
	});
	const ImVec2 childPoint = studio::ToolsProbe::TreeDraw(editor, child);
	const ImVec2 parentPoint = studio::ToolsProbe::TreeDraw(editor, parent);
	auto &io = ImGui::GetIO();
	const auto move = [&](ImVec2 point) {
		io.AddMousePosEvent(point.x, point.y);
		studio::ToolsProbe::TreeDraw(editor, child);
	};
	const auto button = [&](bool down) {
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
		studio::ToolsProbe::TreeDraw(editor, child);
	};
	move(childPoint);
	move(childPoint);
	button(true);
	move(ImVec2(childPoint.x + 20, childPoint.y));
	REQUIRE(ImGui::GetDragDropPayload() != nullptr);
	REQUIRE(universe.Destroy(world) == engine::world::WorldStatus::Ok);
	REQUIRE(universe.Create({.Name = Name("replacement")}) == world);
	universe.Enter(world, [&](Store &store) {
		REQUIRE(store.CreateInstance(folder, "child") == child);
		REQUIRE(store.CreateInstance(folder, "parent") == parent);
	});
	move(ImVec2(childPoint.x + 20, childPoint.y));
	move(parentPoint);
	move(parentPoint);
	button(false);
	REQUIRE(studio::ToolsProbe::ReparentPending(editor));
	studio::ToolsProbe::ApplyPending(editor);
	universe.Enter(world, [&](const Store &store) { CHECK(store.ParentOf(child) == NULL_ENTITY); });
}

TEST_CASE("Explorer does not move across replaced worlds from a stale drag", "[studio][explorer][input]") {
	studio::Editor editor;
	studio::ToolsProbe::TreePrepare(editor);
	auto &universe = studio::ToolsProbe::SearchUniverse(editor);
	const auto source = studio::ToolsProbe::SearchWorld(editor);
	const auto target = universe.Create({.Name = Name("drag-target")});
	const auto folder = engine::ecs::Classes::Find(Name("Folder"));
	Entity child;
	universe.Enter(source, [&](Store &store) { child = store.CreateInstance(folder, "child"); });
	studio::ToolsProbe::QueueMove(editor, source, child, target);
	SECTION("source replaced") {
		REQUIRE(universe.Destroy(source) == engine::world::WorldStatus::Ok);
		REQUIRE(universe.Create({.Name = Name("replacement-source")}) == source);
		universe.Enter(source, [&](Store &store) {
			REQUIRE(store.CreateInstance(folder, "replacement-child") == child);
		});
	}
	SECTION("target replaced") {
		REQUIRE(universe.Destroy(target) == engine::world::WorldStatus::Ok);
		REQUIRE(universe.Create({.Name = Name("replacement-target")}) == target);
	}
	studio::ToolsProbe::ApplyPending(editor);
	universe.Enter(source, [&](Store &store) { CHECK(store.Alive(child)); });
	universe.Enter(target, [&](Store &store) {
		CHECK(store.FindFirstRoot("child") == NULL_ENTITY);
		CHECK(store.FindFirstRoot("replacement-child") == NULL_ENTITY);
	});
}

TEST_CASE("Explorer reparents a selection as one undo step", "[studio][explorer][input]") {
	studio::Editor editor;
	studio::ToolsProbe::TreePrepare(editor);
	auto &universe = studio::ToolsProbe::SearchUniverse(editor);
	const auto world = studio::ToolsProbe::SearchWorld(editor);
	Entity first, second, target, prior;
	universe.Enter(world, [&](Store &store) {
		const auto folder = engine::ecs::Classes::Find(Name("Folder"));
		first = store.CreateInstance(folder, "first");
		second = store.CreateInstance(folder, "second");
		target = store.CreateInstance(folder, "target");
		prior = store.CreateInstance(folder, "prior");
	});
	studio::ToolsProbe::RecordCreate(editor, world, prior);
	size_t commits = 0;
	size_t commands = 0;
	bool insideEntry = false;
	studio::ToolsProbe::WatchCommits(editor, commits, commands, insideEntry);
	// Moving the target under itself is refused; the other two moves form one action.
	studio::ToolsProbe::QueueReparent(editor, world, target, {first, second, target});
	studio::ToolsProbe::ApplyPending(editor);
	CHECK(commits == 1);
	CHECK(commands == 2);
	CHECK_FALSE(insideEntry);
	universe.Enter(world, [&](Store &store) {
		CHECK(store.ParentOf(first) == target);
		CHECK(store.ParentOf(second) == target);
	});
	REQUIRE(studio::ToolsProbe::Undo(editor));
	universe.Enter(world, [&](Store &store) {
		CHECK(store.ParentOf(first) == NULL_ENTITY);
		CHECK(store.ParentOf(second) == NULL_ENTITY);
		CHECK(store.Alive(prior));
	});
	REQUIRE(studio::ToolsProbe::Redo(editor));
	universe.Enter(world, [&](Store &store) {
		CHECK(store.ParentOf(first) == target);
		CHECK(store.ParentOf(second) == target);
	});
	REQUIRE(studio::ToolsProbe::Undo(editor));
	REQUIRE(studio::ToolsProbe::Undo(editor));
	universe.Enter(world, [&](Store &store) { CHECK_FALSE(store.Alive(prior)); });
}

TEST_CASE("Explorer reparent no-op preserves redo", "[studio][explorer][input]") {
	studio::Editor editor;
	studio::ToolsProbe::TreePrepare(editor);
	auto &universe = studio::ToolsProbe::SearchUniverse(editor);
	const auto world = studio::ToolsProbe::SearchWorld(editor);
	Entity child, parent, prior;
	universe.Enter(world, [&](Store &store) {
		const auto folder = engine::ecs::Classes::Find(Name("Folder"));
		parent = store.CreateInstance(folder, "parent");
		child = store.CreateInstance(folder, "child");
		REQUIRE(store.SetParent(child, parent));
		prior = store.CreateInstance(folder, "prior");
	});
	studio::ToolsProbe::RecordCreate(editor, world, prior);
	REQUIRE(studio::ToolsProbe::Undo(editor));
	REQUIRE(studio::ToolsProbe::CanRedo(editor));
	CHECK_FALSE(studio::ToolsProbe::Modified(editor));
	studio::ToolsProbe::QueueReparent(editor, world, parent, {child});
	studio::ToolsProbe::ApplyPending(editor);
	CHECK(studio::ToolsProbe::CanRedo(editor));
	CHECK_FALSE(studio::ToolsProbe::Modified(editor));
	REQUIRE(studio::ToolsProbe::Redo(editor));
}

TEST_CASE("Explorer records the parent actually left after a removal callback", "[studio][explorer][input]") {
	studio::Editor editor;
	studio::ToolsProbe::TreePrepare(editor);
	auto &universe = studio::ToolsProbe::SearchUniverse(editor);
	const auto world = studio::ToolsProbe::SearchWorld(editor);
	Entity child, oldParent, intermediate, target;
	universe.Enter(world, [&](Store &store) {
		const auto folder = engine::ecs::Classes::Find(Name("Folder"));
		oldParent = store.CreateInstance(folder, "old");
		intermediate = store.CreateInstance(folder, "intermediate");
		target = store.CreateInstance(folder, "target");
		child = store.CreateInstance(folder, "child");
		REQUIRE(store.SetParent(child, oldParent));
		store.OnDescendantRemoving([&](Entity, Entity subject) {
			if (subject == child) REQUIRE(store.SetParent(child, intermediate));
		});
	});
	studio::ToolsProbe::QueueReparent(editor, world, target, {child});
	studio::ToolsProbe::ApplyPending(editor);
	universe.Enter(world, [&](Store &store) {
		store.ClearDescendantRemoving();
		CHECK(store.ParentOf(child) == target);
	});
	REQUIRE(studio::ToolsProbe::Undo(editor));
	universe.Enter(world, [&](Store &store) { CHECK(store.ParentOf(child) == intermediate); });
}

TEST_CASE("Explorer does not record a move completed by a removal callback", "[studio][explorer][input]") {
	studio::Editor editor;
	studio::ToolsProbe::TreePrepare(editor);
	auto &universe = studio::ToolsProbe::SearchUniverse(editor);
	const auto world = studio::ToolsProbe::SearchWorld(editor);
	Entity child, oldParent, target;
	universe.Enter(world, [&](Store &store) {
		const auto folder = engine::ecs::Classes::Find(Name("Folder"));
		oldParent = store.CreateInstance(folder, "old");
		target = store.CreateInstance(folder, "target");
		child = store.CreateInstance(folder, "child");
		REQUIRE(store.SetParent(child, oldParent));
		store.OnDescendantRemoving([&](Entity, Entity subject) {
			if (subject == child) REQUIRE(store.SetParent(child, target));
		});
	});
	studio::ToolsProbe::QueueReparent(editor, world, target, {child});
	studio::ToolsProbe::ApplyPending(editor);
	universe.Enter(world, [&](Store &store) {
		store.ClearDescendantRemoving();
		CHECK(store.ParentOf(child) == target);
	});
	CHECK_FALSE(studio::ToolsProbe::Modified(editor));
	CHECK_FALSE(studio::ToolsProbe::Undo(editor));
}

TEST_CASE(
	"Explorer retains completed reparent history when a later move fails", "[studio][explorer][input]"
) {
	studio::Editor editor;
	studio::ToolsProbe::TreePrepare(editor);
	auto &universe = studio::ToolsProbe::SearchUniverse(editor);
	const auto world = studio::ToolsProbe::SearchWorld(editor);
	Entity first, second, oldParent, target, prior;
	universe.Enter(world, [&](Store &store) {
		const auto folder = engine::ecs::Classes::Find(Name("Folder"));
		oldParent = store.CreateInstance(folder, "old parent");
		target = store.CreateInstance(folder, "target");
		first = store.CreateInstance(folder, "first");
		second = store.CreateInstance(folder, "second");
		prior = store.CreateInstance(folder, "prior");
		REQUIRE(store.SetParent(first, oldParent));
		REQUIRE(store.SetParent(second, oldParent));
		store.OnDescendantRemoving([&](Entity, Entity subject) {
			if (subject == second) throw std::bad_alloc{};
		});
	});
	studio::ToolsProbe::RecordCreate(editor, world, prior);
	size_t commits = 0;
	size_t commands = 0;
	bool insideEntry = false;
	studio::ToolsProbe::WatchCommits(editor, commits, commands, insideEntry);
	studio::ToolsProbe::QueueReparent(editor, world, target, {first, second});
	CHECK_THROWS_AS(studio::ToolsProbe::ApplyPending(editor), std::bad_alloc);
	universe.Enter(world, [&](Store &store) {
		store.ClearDescendantRemoving();
		CHECK(store.ParentOf(first) == target);
		CHECK(store.ParentOf(second) == oldParent);
	});
	CHECK(studio::ToolsProbe::Modified(editor));
	CHECK(commits == 1);
	CHECK(commands == 1);
	CHECK_FALSE(insideEntry);
	REQUIRE(studio::ToolsProbe::Undo(editor));
	universe.Enter(world, [&](Store &store) {
		CHECK(store.ParentOf(first) == oldParent);
		CHECK(store.Alive(prior));
	});
}

TEST_CASE("Explorer reparent joins an open recording", "[studio][explorer][input]") {
	studio::Editor editor;
	studio::ToolsProbe::TreePrepare(editor);
	auto &universe = studio::ToolsProbe::SearchUniverse(editor);
	const auto world = studio::ToolsProbe::SearchWorld(editor);
	Entity first, second, target;
	universe.Enter(world, [&](Store &store) {
		const auto folder = engine::ecs::Classes::Find(Name("Folder"));
		first = store.CreateInstance(folder, "first");
		second = store.CreateInstance(folder, "second");
		target = store.CreateInstance(folder, "target");
	});
	size_t commits = 0;
	size_t commands = 0;
	bool insideEntry = false;
	studio::ToolsProbe::WatchCommits(editor, commits, commands, insideEntry);
	const auto recording = studio::ToolsProbe::BeginRecording(editor);
	REQUIRE(recording);
	studio::ToolsProbe::QueueReparent(editor, world, target, {first, second});
	studio::ToolsProbe::ApplyPending(editor);
	CHECK(commits == 0);
	REQUIRE(studio::ToolsProbe::CommitRecording(editor, *recording));
	CHECK(commits == 1);
	CHECK(commands == 2);
	CHECK_FALSE(insideEntry);
	REQUIRE(studio::ToolsProbe::Undo(editor));
	universe.Enter(world, [&](Store &store) {
		CHECK(store.ParentOf(first) == NULL_ENTITY);
		CHECK(store.ParentOf(second) == NULL_ENTITY);
	});
}

TEST_CASE("Explorer reparent in a replica does not record authority history", "[studio][explorer][input]") {
	studio::Editor editor;
	studio::ToolsProbe::TreePrepare(editor);
	auto &universe = studio::ToolsProbe::SearchUniverse(editor);
	const auto world = studio::ToolsProbe::SearchWorld(editor);
	const auto replica = studio::ToolsProbe::StartReplica(editor, world);
	Entity child, parent;
	universe.Enter(replica, [&](Store &store) {
		const auto folder = engine::ecs::Classes::Find(Name("Folder"));
		child = store.CreatePredictedInstance(folder, "child");
		parent = store.CreatePredictedInstance(folder, "parent");
		REQUIRE(child != NULL_ENTITY);
		REQUIRE(parent != NULL_ENTITY);
	});
	size_t commits = 0;
	size_t commands = 0;
	bool insideEntry = false;
	studio::ToolsProbe::WatchCommits(editor, commits, commands, insideEntry);
	studio::ToolsProbe::QueueReparent(editor, replica, parent, {child});
	studio::ToolsProbe::ApplyPending(editor);
	universe.Enter(replica, [&](Store &store) { CHECK(store.ParentOf(child) == parent); });
	CHECK(commits == 0);
	CHECK_FALSE(studio::ToolsProbe::Undo(editor));
	CHECK_FALSE(studio::ToolsProbe::Modified(editor));
}

TEST_CASE(
	"an independent history edit in an ungrouped action publishes after the world entry",
	"[studio][explorer][input]"
) {
	studio::Editor editor;
	studio::ToolsProbe::TreePrepare(editor);
	auto &universe = studio::ToolsProbe::SearchUniverse(editor);
	const auto world = studio::ToolsProbe::SearchWorld(editor);
	const auto replica = studio::ToolsProbe::StartReplica(editor, world);
	Entity authored, child, oldParent, target;
	universe.Enter(world, [&](Store &store) {
		authored = store.CreateInstance(engine::ecs::Classes::Find(Name("Folder")), "authored");
	});
	universe.Enter(replica, [&](Store &store) {
		const auto folder = engine::ecs::Classes::Find(Name("Folder"));
		child = store.CreatePredictedInstance(folder, "child");
		oldParent = store.CreatePredictedInstance(folder, "old");
		target = store.CreatePredictedInstance(folder, "target");
		REQUIRE(store.SetParent(child, oldParent));
		store.OnDescendantRemoving([&](Entity, Entity subject) {
			if (subject == child) {
				studio::ToolsProbe::RecordReparent(editor, world, authored);
				studio::ToolsProbe::RecordReparent(editor, world, authored);
			}
		});
	});
	std::vector<std::string> names;
	bool insideEntry = false;
	bool throwFirst = false;
	SECTION("listener succeeds") {}
	SECTION("listener throws") {
		throwFirst = true;
	}
	studio::ToolsProbe::WatchReentrantCommits(editor, world, authored, names, insideEntry, throwFirst);
	studio::ToolsProbe::QueueReparent(editor, replica, target, {child});
	if (throwFirst)
		CHECK_THROWS_AS(studio::ToolsProbe::ApplyPending(editor), std::runtime_error);
	else
		studio::ToolsProbe::ApplyPending(editor);
	REQUIRE(names.size() == 3);
	CHECK(names[0] == "Independent edit");
	CHECK(names[1] == "Independent edit");
	CHECK(names[2] == "Listener edit");
	CHECK_FALSE(insideEntry);
	studio::ToolsProbe::RecordReparent(editor, world, authored);
	REQUIRE(names.size() == 4);
	CHECK(names[3] == "Independent edit");
}

TEST_CASE(
	"a prepared edit raised by a nested replica action publishes after entry", "[studio][explorer][input]"
) {
	studio::Editor editor;
	studio::ToolsProbe::TreePrepare(editor);
	auto &universe = studio::ToolsProbe::SearchUniverse(editor);
	const auto world = studio::ToolsProbe::SearchWorld(editor);
	const auto replica = studio::ToolsProbe::StartReplica(editor, world);
	Entity authored, destination, child, oldParent, target;
	universe.Enter(world, [&](Store &store) {
		const auto folder = engine::ecs::Classes::Find(Name("Folder"));
		authored = store.CreateInstance(folder, "authored");
		destination = store.CreateInstance(folder, "destination");
	});
	universe.Enter(replica, [&](Store &store) {
		const auto folder = engine::ecs::Classes::Find(Name("Folder"));
		child = store.CreatePredictedInstance(folder, "child");
		oldParent = store.CreatePredictedInstance(folder, "old");
		target = store.CreatePredictedInstance(folder, "target");
		REQUIRE(store.SetParent(child, oldParent));
		store.OnDescendantRemoving([&](Entity, Entity subject) {
			if (subject == child) studio::ToolsProbe::RecordPreparedReparent(editor, world, authored);
		});
	});
	std::vector<std::string> names;
	bool insideEntry = false;
	bool throwFirst = false;
	SECTION("outer listener succeeds") {}
	SECTION("outer listener throws") {
		throwFirst = true;
	}
	studio::ToolsProbe::WatchNestedReplica(editor, replica, target, child, names, insideEntry, throwFirst);
	studio::ToolsProbe::QueueReparent(editor, world, destination, {authored});
	if (throwFirst)
		CHECK_THROWS_AS(studio::ToolsProbe::ApplyPending(editor), std::runtime_error);
	else
		studio::ToolsProbe::ApplyPending(editor);
	REQUIRE(names.size() == 2);
	CHECK(names[0] == "Move authored");
	CHECK(names[1] == "Prepared nested edit");
	CHECK_FALSE(insideEntry);
}

TEST_CASE(
	"Explorer search validates names and keeps computed resource predicates live", "[studio][find][cache]"
) {
	const auto previous = studio::ConfigRoot();
	const auto scratch = std::filesystem::temp_directory_path() / "studio-search-cache";
	std::filesystem::create_directories(scratch);
	studio::SetConfigRoot(scratch);
	struct Restore {
		std::filesystem::path Previous;
		~Restore() {
			studio::SetConfigRoot(Previous);
		}
	} restore{previous};
	studio::Editor editor;
	studio::ToolsProbe::SearchPrepare(editor);
	auto &universe = studio::ToolsProbe::SearchUniverse(editor);
	const auto world = studio::ToolsProbe::SearchWorld(editor);
	Entity named;
	Entity workspace;
	universe.Enter(world, [&](Store &store) {
		engine::scene::InstallServices(store);
		workspace = engine::scene::WorkspaceOf(store);
		named = store.CreateInstance(engine::ecs::Classes::Find(Name("Folder")), "search-hit-one");
		REQUIRE(store.SetParent(named, workspace));
	});
	FindQuery query;
	query.Class = "Folder";
	query.Property = "Name";
	query.Value = "search-hit";
	REQUIRE(studio::ToolsProbe::SearchRun(editor, query).size() == 1);
	CHECK(studio::ToolsProbe::SearchCached(editor));
	CHECK(studio::ToolsProbe::SearchRun(editor, query).front().Instance == named);
	universe.Enter(world, [&](Store &store) { REQUIRE(store.SetInstanceName(named, "miss")); });
	CHECK(studio::ToolsProbe::SearchRun(editor, query).empty());
	universe.Enter(world, [&](Store &store) {
		store.Destroy(named);
		named = store.CreateInstance(engine::ecs::Classes::Find(Name("Folder")), "search-hit-new");
		REQUIRE(store.SetParent(named, workspace));
	});
	REQUIRE(studio::ToolsProbe::SearchRun(editor, query).size() == 1);
	CHECK(studio::ToolsProbe::SearchRun(editor, query).front().Name == "search-hit-new");
	query.Exact = true;
	CHECK(studio::ToolsProbe::SearchRun(editor, query).empty());
	query.Value = "search-hit-new";
	REQUIRE(studio::ToolsProbe::SearchRun(editor, query).size() == 1);
	query.Exact = false;
	query.Property = "na";
	REQUIRE(studio::ToolsProbe::SearchRun(editor, query).size() == 1);
	CHECK(studio::ToolsProbe::SearchCached(editor));
	universe.Enter(world, [&](Store &store) { store.Remove<engine::ecs::InstanceName>(named); });
	CHECK(studio::ToolsProbe::SearchRun(editor, query).empty());
	query = {};
	query.Class = "Folder";
	REQUIRE(studio::ToolsProbe::SearchRun(editor, query).size() == 1);
	CHECK(studio::ToolsProbe::SearchCached(editor));
	universe.Enter(world, [&](Store &store) { store.Destroy(named); });
	CHECK(studio::ToolsProbe::SearchRun(editor, query).empty());
	query.Property = "missing-property";
	CHECK(studio::ToolsProbe::SearchRun(editor, query).empty());
	query = {};
	query.Class = "Workspace";
	query.Property = "MaxSurfaces";
	query.Value = "17";
	query.Exact = true;
	universe.Enter(world, [&](Store &store) { store.SetResource(engine::scene::SurfaceLimit{17}); });
	REQUIRE(studio::ToolsProbe::SearchRun(editor, query).size() == 1);
	CHECK_FALSE(studio::ToolsProbe::SearchCached(editor));
	universe.Enter(world, [&](Store &store) { store.SetResource(engine::scene::SurfaceLimit{23}); });
	CHECK(studio::ToolsProbe::SearchRun(editor, query).empty());

	// The bound depends on EachEntity order, including a non-instance row
	// after the last allowed result. Keep that observable truncation state.
	engine::world::WorldId bounded;
	bounded = universe.Create({.Name = Name("search-bounded-world")});
	studio::ToolsProbe::SearchSelectWorld(editor, bounded);
	Entity raw;
	std::vector<Entity> ordered;
	universe.Enter(bounded, [&](Store &store) {
		for (size_t index = 0; index < 500; ++index)
			ordered.push_back(store.CreateInstance(engine::ecs::Classes::Find(Name("Folder")), "bounded"));
	});
	query = {};
	query.Class = "Folder";
	const auto boundedResults = studio::ToolsProbe::SearchRun(editor, query);
	REQUIRE(boundedResults.size() == 500);
	CHECK_FALSE(studio::ToolsProbe::SearchTruncated(editor));
	CHECK(boundedResults.front().Instance == ordered.front());
	CHECK(boundedResults.back().Instance == ordered.back());
	universe.Enter(bounded, [&](Store &store) { raw = store.Create(); });
	REQUIRE(studio::ToolsProbe::SearchRun(editor, query).size() == 500);
	CHECK(studio::ToolsProbe::SearchTruncated(editor));
	universe.Enter(bounded, [&](Store &store) {
		store.Destroy(raw);
		store.Destroy(ordered.front());
		ordered.erase(ordered.begin());
		ordered.push_back(store.CreateInstance(engine::ecs::Classes::Find(Name("Folder")), "bounded"));
	});
	const auto reordered = studio::ToolsProbe::SearchRun(editor, query);
	std::vector<Entity> expected;
	universe.Enter(bounded, [&](Store &store) {
		store.EachEntity([&](Entity entity) {
			std::string matched;
			if (MatchesQuery(store, entity, query, matched)) expected.push_back(entity);
		});
	});
	REQUIRE(reordered.size() == expected.size());
	for (size_t index = 0; index < expected.size(); ++index)
		CHECK(reordered[index].Instance == expected[index]);
}

TEST_CASE(
	"Explorer search clipping preserves full child pixels and result picking", "[studio][find][input]"
) {
	const auto previous = studio::ConfigRoot();
	const auto scratch = std::filesystem::temp_directory_path() / "studio-search-parity";
	std::filesystem::create_directories(scratch);
	studio::SetConfigRoot(scratch);
	struct Restore {
		std::filesystem::path Previous;
		~Restore() {
			studio::SetConfigRoot(Previous);
		}
	} restore{previous};
	studio::Editor editor;
	studio::ToolsProbe::SearchPrepare(editor);
	REQUIRE(studio::ToolsProbe::SearchInitialise(editor));
	auto &universe = studio::ToolsProbe::SearchUniverse(editor);
	const auto world = studio::ToolsProbe::SearchWorld(editor);
	Entity first;
	universe.Enter(world, [&](Store &store) {
		engine::scene::InstallServices(store);
		const auto workspace = engine::scene::WorkspaceOf(store);
		for (size_t index = 0; index < 40; ++index) {
			const auto made = store.CreateInstance(
				engine::ecs::Classes::Find(Name("Folder")), "search-hit-" + std::to_string(index)
			);
			REQUIRE(store.SetParent(made, workspace));
			if (index == 0) first = made;
		}
	});
	FindQuery query;
	query.Class = "Folder";
	query.Property = "Name";
	query.Value = "search-hit";
	REQUIRE(studio::ToolsProbe::SearchRun(editor, query).size() == 40);
	const auto parity = [&](float scroll) {
		for (int warm = 0; warm < 4; ++warm)
			studio::ToolsProbe::SearchDraw(editor, scroll);
		const auto *child = studio::ToolsProbe::SearchChild();
		const auto content = child->ContentSize;
		const auto cursorMaximum = child->DC.CursorMaxPos;
		const auto maximum = child->ScrollMax;
		const std::vector<ImDrawVert> vertices(
			child->DrawList->VtxBuffer.begin(), child->DrawList->VtxBuffer.end()
		);
		const std::vector<ImDrawIdx> indices(
			child->DrawList->IdxBuffer.begin(), child->DrawList->IdxBuffer.end()
		);
		studio::ToolsProbe::SearchDraw(editor, scroll, true);
		child = studio::ToolsProbe::SearchChild();
		CHECK(child->DC.CursorMaxPos.x == cursorMaximum.x);
		CHECK(child->DC.CursorMaxPos.y == cursorMaximum.y);
		CHECK(child->ContentSize.x == content.x);
		CHECK(child->ContentSize.y == content.y);
		CHECK(child->ScrollMax.y == maximum.y);
		CHECK(
			std::equal(
				vertices.begin(),
				vertices.end(),
				child->DrawList->VtxBuffer.begin(),
				child->DrawList->VtxBuffer.end(),
				[](const auto &a, const auto &b) {
					return a.pos.x == b.pos.x && a.pos.y == b.pos.y && a.uv.x == b.uv.x && a.uv.y == b.uv.y &&
						   a.col == b.col;
				}
			)
		);
		CHECK(
			std::equal(
				indices.begin(),
				indices.end(),
				child->DrawList->IdxBuffer.begin(),
				child->DrawList->IdxBuffer.end()
			)
		);
	};
	CHECK(studio::ToolsProbe::SearchUniform(editor));
	parity(0);
	parity(220);
	parity(100000);
	universe.Enter(world, [&](Store &store) {
		REQUIRE(store.SetInstanceName(first, "search-hit-0\nextra"));
	});
	parity(220);
	CHECK_FALSE(studio::ToolsProbe::SearchUniform(editor));
	parity(0);
	const auto *child = studio::ToolsProbe::SearchChild();
	const ImVec2 point(child->InnerRect.Min.x + 30, child->DC.CursorStartPos.y + ImGui::GetFontSize() * .5f);
	auto &io = ImGui::GetIO();
	io.AddMousePosEvent(point.x, point.y);
	studio::ToolsProbe::SearchDraw(editor, 0);
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	studio::ToolsProbe::SearchDraw(editor, 0);
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	studio::ToolsProbe::SearchDraw(editor, 0);
	CHECK(studio::ToolsProbe::SearchSelected(editor, first));
	universe.Enter(world, [&](Store &store) { REQUIRE(store.SetInstanceName(first, "search-hit-0")); });
	parity(0);
	CHECK(studio::ToolsProbe::SearchUniform(editor));
	universe.Enter(world, [&](Store &store) {
		Entity last;
		store.EachEntity([&](Entity entity) {
			if (store.InstanceNameOf(entity) == Name("search-hit-39")) last = entity;
		});
		REQUIRE(last != NULL_ENTITY);
		REQUIRE(store.SetInstanceName(last, "search-hit-" + std::string(300, 'w')));
	});
	// The widest row is below this viewport but must retain the full extent.
	parity(0);
	ImGui::GetStyle().ItemSpacing.x = 3.5f;
	ImGui::GetStyle().FontScaleMain = 1.25f;
	parity(0);
	parity(220);
}

namespace {
	struct Fixture {
		Store World{"find_test"};

		Fixture() {
			engine::scene::EnsureClassTree();
		}

		Entity Part(const char *name) {
			const Entity made = World.CreateInstance(engine::scene::PartClass(), name);
			REQUIRE(made != NULL_ENTITY);
			return made;
		}
	};

	bool Matches(const Store &store, Entity instance, const FindQuery &query) {
		std::string matched;
		return MatchesQuery(store, instance, query, matched);
	}
}

TEST_CASE("an empty query matches every instance", "[studio][find]") {
	// The identity case, and it decides the shape of the control: an empty
	// field does not filter. If an empty query matched nothing, "show me
	// everything" would need a mode of its own.
	Fixture fixture;
	const Entity part = fixture.Part("Anything");

	CHECK(Matches(fixture.World, part, FindQuery{}));
}

TEST_CASE("the class filter is IsA, not an exact name", "[studio][find]") {
	Fixture fixture;
	const Entity part = fixture.Part("Block");

	FindQuery exact;
	exact.Class = "Part";
	CHECK(Matches(fixture.World, part, exact));

	// **The point of using the class tree rather than a string compare.** A
	// `Part` is a `BasePart`, so searching for the base finds the leaf - which
	// is what somebody means by "every part" and what set inclusion already
	// says.
	FindQuery base;
	base.Class = "BasePart";
	CHECK(Matches(fixture.World, part, base));

	FindQuery unrelated;
	unrelated.Class = "Script";
	CHECK_FALSE(Matches(fixture.World, part, unrelated));
}

TEST_CASE("a class nobody registered matches nothing rather than everything", "[studio][find]") {
	// The direction to fail in. A filter that fell back to "match all" when it
	// did not recognise the class would answer a typo with the whole scene.
	Fixture fixture;
	const Entity part = fixture.Part("Block");

	FindQuery query;
	query.Class = "NotAClass";
	CHECK_FALSE(Matches(fixture.World, part, query));
}

TEST_CASE("the name filter is a case-insensitive substring", "[studio][find]") {
	Fixture fixture;
	const Entity part = fixture.Part("RedDoorFrame");

	FindQuery middle;
	middle.Name = "door";
	CHECK(Matches(fixture.World, part, middle));

	FindQuery missing;
	missing.Name = "window";
	CHECK_FALSE(Matches(fixture.World, part, missing));
}

TEST_CASE("a property is found by name with no value asked for", "[studio][find]") {
	// **`Transparency` is never written down in the matcher.** It arrives
	// through `PropertiesOf`, which is why a property declared by any module
	// tomorrow is searchable with nothing here changing.
	Fixture fixture;
	const Entity part = fixture.Part("Block");

	FindQuery query;
	query.Property = "Transparency";

	std::string matched;
	REQUIRE(MatchesQuery(fixture.World, part, query, matched));

	// And the row says what matched, so a result list is readable without
	// clicking every entry.
	CHECK(matched.find("Transparency") != std::string::npos);
}

TEST_CASE("a property nothing declares matches nothing", "[studio][find]") {
	Fixture fixture;
	const Entity part = fixture.Part("Block");

	FindQuery query;
	query.Property = "NoSuchProperty";
	CHECK_FALSE(Matches(fixture.World, part, query));
}

TEST_CASE("a value is matched through its rendered text", "[studio][find]") {
	Fixture fixture;
	const Entity part = fixture.Part("Block");

	// Booleans, numbers and enums all go through `FormatValue`, so one
	// predicate covers every type - and `Anchored` is a *structural* property
	// on top of that, presence of a tag spelled as a bool, which the matcher
	// also never learns.
	const bool anchored = true;
	REQUIRE(fixture.World.SetProperty(part, Name("Anchored"), &anchored, sizeof(anchored)));

	FindQuery query;
	query.Property = "Anchored";
	query.Value = "true";
	CHECK(Matches(fixture.World, part, query));

	FindQuery other;
	other.Property = "Anchored";
	other.Value = "false";
	CHECK_FALSE(Matches(fixture.World, part, other));
}

TEST_CASE("exact compares through the type rather than the text", "[studio][find]") {
	// **The case the `exact` checkbox exists for.** A substring match on the
	// rendered text says 0.5 is in "0.55", which is right for browsing and
	// wrong for "which parts are exactly half transparent".
	Fixture fixture;
	const Entity part = fixture.Part("Block");

	const float transparency = 0.55f;
	fixture.World.SetProperty(part, Name("Transparency"), &transparency, sizeof(transparency));

	FindQuery loose;
	loose.Property = "Transparency";
	loose.Value = "0.5";
	CHECK(Matches(fixture.World, part, loose));

	FindQuery exact = loose;
	exact.Exact = true;
	CHECK_FALSE(Matches(fixture.World, part, exact));

	FindQuery right;
	right.Property = "Transparency";
	right.Value = "0.55";
	right.Exact = true;
	CHECK(Matches(fixture.World, part, right));
}

TEST_CASE("filters combine with and, not or", "[studio][find]") {
	Fixture fixture;
	const Entity part = fixture.Part("Block");

	// Right class, wrong name: not a match. An `or` would return it and make
	// every additional filter widen the search instead of narrowing it.
	FindQuery query;
	query.Class = "Part";
	query.Name = "NotThisOne";
	CHECK_FALSE(Matches(fixture.World, part, query));
}

TEST_CASE("an entity that is not an instance never matches", "[studio][find]") {
	// A module's own storage row has no class, and the explorer does not show
	// it. Find must agree, or the result list contains things nothing can
	// select.
	Fixture fixture;
	const Entity bare = fixture.World.Create();
	REQUIRE(bare != NULL_ENTITY);

	CHECK_FALSE(Matches(fixture.World, bare, FindQuery{}));
}
