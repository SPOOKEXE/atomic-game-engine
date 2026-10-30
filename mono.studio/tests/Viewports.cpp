// Which panel a "View" press lands in.
//
// **The bug this exists for is the one you cannot see: a view with no way
// back.** A viewport pinned to a client and then closed was recoverable only
// because the replica had a row among the scenes, and the *server's* view - the
// scene itself, drawn by the main panel - had no row anywhere. Live Instances is
// the list, and this is the decision behind its buttons.
//
// Its three failure modes all look like something other than a fault: a second
// panel on one world halves the refresh rate of both and reads as the editor
// being slow; a panel taken from another scene reads as that scene closing; and
// a panel minted while a free one sat there is a `SceneTarget` and a turn in the
// rotation nobody asked to pay.

#include "DataFactoryHost.hpp"
#include "ProceduralPanel.hpp"
#include "ViewportGuiEvents.hpp"

#include <engine/assets/ContentHash.hpp>
#include <engine/control/Surface.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/examples/DemosLoader.hpp>
#include <engine/gui/Compile.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/PlayerGui.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/gui/Services.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/render/PortalImageDemand.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/SpatialCanvas.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/scene/TextureCatalogue.hpp>
#include <engine/scene/ViewEye.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/ProceduralJobs.hpp>
#include <engine/script/ProceduralScene.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <imgui.h>
#include <imgui_internal.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <numbers>
#include <random>
#include <stdexcept>
#include <studio/Config.hpp>
#include <studio/Editor.hpp>
#include <studio/Keybinds.hpp>
#include <studio/PlayLink.hpp>
#include <studio/ProceduralEditor.hpp>
#include <studio/Projection.hpp>
#include <studio/ViewportDiagnostics.hpp>
#include <studio/Viewports.hpp>
#include <tuple>

namespace studio {
	struct ViewportCameraProbe {
		static void PrepareGizmo(Editor &editor, WorldId world, Entity part, int mode) {
			editor.Active = world;
			editor.SelectionWorld = world;
			editor.Selection = {part};
			editor.CurrentTool = mode == 0 || mode == 3	  ? Editor::ToolMode::Move
								 : mode == 1 || mode == 5 ? Editor::ToolMode::Scale
														  : Editor::ToolMode::Rotate;
			editor.PivotEditing = mode == 3 || mode == 4;
			editor.ScaleSides = mode == 5 ? ScaleSide::BothHalf : ScaleSide::Side;
			editor.Commands = std::make_unique<CommandLog>(*editor.Universe);
			editor.Modified = false;
		}

		static void DrawGizmo(Editor &editor, const PanelProjection &panel) {
			editor.Overlays[0].List = ImGui::GetWindowDrawList();
			(void)editor.DrawGizmo(0, panel);
		}

		static bool GizmoActive(const Editor &editor) {
			return editor.Dragging.Axis >= 0;
		}

		static void SelectGizmoWorld(Editor &editor, WorldId world, Entity part) {
			editor.RetargetEditingViewport(0, world);
			editor.Select(world, part, false);
		}

		static void ClearGizmoSelection(Editor &editor) {
			editor.ClearSelection();
		}

		static void CloseGizmoViewport(Editor &editor) {
			editor.ShowViewport = false;
			editor.Overlays[0].Drawn = false;
			editor.DrawViewportOverlays();
		}

		static void ReleaseGizmoWorld(Editor &editor, WorldId world) {
			editor.ReleaseWorldResidency(world);
		}

		static bool
		PrepareGizmoCandidate(Editor &editor, engine::world::Universe &candidate, std::string &detail) {
			return editor.PrepareGizmoCandidate(candidate, detail);
		}

		static bool StartFactory(Editor &editor) {
			return editor.StartDataFactoryHost();
		}

		static engine::world::DataFactorySession &FactorySession(Editor &editor) {
			return *editor.FactoryHost->Session();
		}

		static void InstallFactoryTools(Editor &editor, engine::control::Surface &surface) {
			editor.FactoryHost->InstallTools({.Surface = surface, .RendererReady = true});
		}

		static bool GizmoCanUndo(const Editor &editor) {
			return editor.Commands->CanUndo();
		}

		static void PrepareTypingCamera(Editor &editor, WorldId world, Entity selected) {
			editor.SelectionWorld = world;
			editor.Selection = {selected};
			editor.CameraFrame = engine::core::CFrame(engine::core::Vector3{0.0f, 30.0f, 30.0f});
			editor.CameraYaw = 0.0f;
			editor.CameraPitch = 0.0f;
		}

		static void Drive(Editor &editor) {
			editor.DriveCamera();
		}

		static engine::core::CFrame Camera(const Editor &editor) {
			return editor.CameraFrame;
		}

		static bool TextInputCaptured(const Editor &editor) {
			return editor.GuiTextInputFrame == ImGui::GetFrameCount();
		}

		static void SelectInExtraViewport(Editor &editor, WorldId main, WorldId extra, Entity parent) {
			editor.Active = main;
			editor.ResizeViewports(1);
			editor.RetargetEditingViewport(1, extra);
			editor.Select(extra, parent, false);
		}

		static void SelectWorldRow(Editor &editor, WorldId main, WorldId selected) {
			editor.Active = main;
			editor.FocusedViewport = 0;
			editor.ClearSelection();
			editor.SelectionWorld = {};
			editor.SelectedWorldRow = selected;
		}

		static void DrawScriptTool(Editor &editor) {
			editor.DrawBuiltinStudioTool(BuiltinStudioTool::CreateScript);
		}

		static void DrawInsertTool(Editor &editor) {
			editor.DrawBuiltinStudioTool(BuiltinStudioTool::InsertObject);
		}

		static void DrawMenuBar(Editor &editor) {
			editor.DrawMenuBar();
		}

		static void ApplyPending(Editor &editor) {
			editor.ApplyPendingActions();
		}

		static const char *Window(Editor &editor) {
			return editor.ViewportIdentity(0);
		}

		static void ResolveFocus(Editor &editor) {
			editor.ResolveFocusedViewport();
		}

		static bool HasViewportFocus(const Editor &editor) {
			return editor.FocusedIsViewport;
		}

		static void AddScriptWindow(Editor &editor) {
			auto &script = editor.Scripts.emplace_back();
			script.WindowWorld = WorldId{7};
			script.WindowInstance = Entity{42};
		}

		static void PrepareFullFrame(Editor &editor) {
			editor.Universe = std::make_unique<engine::world::Universe>();
			editor.Active = editor.Universe->Create({.Name = engine::core::Name("focus-test")});
			editor.ShowExplorer = false;
			editor.ShowWorlds = false;
			editor.ShowProperties = false;
			editor.ShowComponents = false;
			editor.ShowOutput = false;
			editor.ShowLiveInstances = false;
			AddScriptWindow(editor);
			editor.ShowScripts = true;
			editor.ActiveScript = 0;
			editor.FocusScripts = 1;
		}

		static void DrawInterface(Editor &editor) {
			editor.DrawInterface();
		}

		static void ShowScriptOnNextFrame(Editor &editor, bool show) {
			editor.ShowScripts = show;
			editor.FocusScripts = show ? 1 : 0;
		}

		static Entity AddFocusedTextBox(Editor &editor) {
			Entity box;
			editor.Universe->Enter(editor.Active, [&](engine::ecs::Store &store) {
				engine::scene::InstallServices(store);
				engine::gui::InstallGuiServices(store);
				const auto screen = store.CreateInstance(engine::gui::GuiClass("ScreenGui"), "Screen");
				REQUIRE(store.SetParent(screen, store.FindFirstRoot("StarterGui")));
				box = store.CreateInstance(engine::gui::GuiClass("TextBox"), "Entry");
				REQUIRE(store.SetParent(box, screen));
				store.GetMutable<engine::gui::Label>(box)->Text.clear();
				REQUIRE(engine::gui::Focus(store, box));
			});
			return box;
		}

		static bool ViewportCompiled(Editor &editor, Entity entity) {
			const auto &commands = editor.GuiLists[0].Commands().Commands;
			return std::any_of(commands.begin(), commands.end(), [&](const auto &command) {
				return command.Source == entity;
			});
		}

		static std::string TextBoxText(Editor &editor, Entity box) {
			std::string text;
			editor.Universe->Enter(editor.Active, [&](const engine::ecs::Store &store) {
				text = store.Get<engine::gui::Label>(box)->Text;
			});
			return text;
		}

		static void PrepareCommand(Editor &editor, WorldId world) {
			editor.Active = world;
			editor.Commands = std::make_unique<CommandLog>(*editor.Universe);
			editor.Modified = false;
		}

		static bool Command(Editor &editor, const std::string &source) {
			return editor.RunCommand(source);
		}

		static bool Modified(const Editor &editor) {
			return editor.Modified;
		}

		static void ClearModified(Editor &editor) {
			editor.Modified = false;
		}

		static void MarkModified(Editor &editor) {
			editor.Modified = true;
		}

		static void PrepareDiff(Editor &editor) {
			editor.ShowDiff = true;
			editor.DiffLoaded = true;
		}

		static bool DiffLoaded(const Editor &editor) {
			return editor.DiffLoaded;
		}

		static void RecordPart(Editor &editor, WorldId world) {
			editor.Universe->Enter(world, [&](engine::ecs::Store &store) {
				const Entity part = store.CreateInstance(engine::scene::PartClass(), "Undoable");
				store.SetParent(part, engine::scene::WorkspaceOf(store));
				editor.Commands->RecordCreate(store, world, part, "Insert");
			});
		}

		static void PrepareShortcuts(Editor &editor) {
			editor.RegisterOperators();
			Keybinds::SetScope(Scope::Viewport);
		}

		static void DrawShortcuts(Editor &editor) {
			editor.DrawShortcuts(false);
		}

		static bool Undo(Editor &editor) {
			return editor.Commands->Undo();
		}

		static bool CanRedo(const Editor &editor) {
			return editor.Commands->CanRedo();
		}

		static void LeaveLiveInstancesSelected(Editor &editor) {
			editor.ShowWorlds = false;
			editor.FocusWorlds = 0;
			editor.FocusInstances = 4;
		}

		static bool WorldsFocusedForEditing(const Editor &editor) {
			return editor.ShowWorlds && editor.FocusWorlds > 0 && editor.FocusInstances == 0;
		}

		static void Stop(Editor &editor) {
			editor.SetRunMode(editor.Active, RunMode::Edit);
		}

		static Entity Insert(Editor &editor, WorldId world, engine::ecs::ClassId klass, Entity parent) {
			return editor.InsertInstance(world, klass, parent);
		}

		static bool Initialise(Editor &editor, size_t viewports) {
			Options options;
			options.Headless = true;
			options.StartViewports = viewports;
			return editor.Initialise(options);
		}

		static void Present(Editor &editor, size_t viewport) {
			editor.PresentViewport(viewport, 1.0f / 60.0f);
		}

		static void RetainCacheImages(Editor &editor) {
			editor.Settings.Headless = false;
			editor.LastGraphViewport = 0;
			editor.LastGraphRenderGeneration = editor.Renderer.RenderGeneration();
			editor.Overlay.MarkUploaded();
			editor.ShowFrameGraph = false;
		}

		static engine::render::PresentationCacheProfile CacheProfile(const Editor &editor) {
			return editor.ViewportPresentations[0].CacheProfile();
		}

		static void CachePage(Editor &editor, bool visible) {
			editor.ShowFrameGraph = visible;
			editor.FrameGraphState.ShowCascadedCaches = visible;
		}

		static engine::render::VisibilitySnapshot BodyVisibility(Editor &editor) {
			editor.WorldTarget = {96, 96};
			Present(editor, 0);
			return editor.Renderer.Visibility();
		}

		static void SetFieldOfView(Editor &editor, size_t viewport, float fieldOfView) {
			const auto &viewer = editor.Viewers[viewport];
			bool updated = false;
			editor.Universe->Enter(viewer.World, [&](engine::ecs::Store &store) {
				if (auto *camera = store.GetMutable<engine::scene::Camera>(viewer.Instance)) {
					camera->FieldOfViewRadians = fieldOfView;
					updated = true;
				}
			});
			REQUIRE(updated);
		}

		static void SetInspectionFrame(Editor &editor, engine::core::CFrame frame) {
			const engine::core::Vector3 angles = frame.ToAngles();
			editor.CameraFrame = frame;
			editor.CameraYaw = angles.Y;
			editor.CameraPitch = angles.X;
		}

		static engine::core::CFrame PresentedFrame(const Editor &editor, size_t viewport) {
			return editor.Overlays[viewport].PresentedFrame;
		}

		static engine::core::CFrame ViewerFrame(Editor &editor, size_t viewport) {
			const Editor::ViewerCamera &viewer = editor.Viewers[viewport];
			engine::core::CFrame frame;
			bool found = false;
			editor.Universe->Enter(viewer.World, [&](const engine::ecs::Store &store) {
				if (const auto *view = store.Resource<engine::scene::ViewEye>()) {
					CHECK(view->Camera == engine::ecs::NULL_ENTITY);
					frame = view->Frame;
					found = true;
				}
			});
			REQUIRE(found);
			return frame;
		}

		static engine::core::CFrame AuthoredCameraFrame(Editor &editor, size_t viewport) {
			const Editor::ViewerCamera &viewer = editor.Viewers[viewport];
			engine::core::CFrame frame;
			editor.Universe->Enter(viewer.World, [&](const engine::ecs::Store &store) {
				const auto *transform = store.Get<engine::scene::Transform>(viewer.Instance);
				REQUIRE(transform != nullptr);
				frame = transform->Frame;
			});
			return frame;
		}

		static float PresentedFieldOfView(const Editor &editor, size_t viewport) {
			return editor.Overlays[viewport].PresentedFieldOfView;
		}

		static engine::world::WorldId PresentedWorld(const Editor &editor, size_t viewport) {
			return editor.Overlays[viewport].PresentedWorld;
		}

		static PanelProjection Projection(Editor &editor, size_t viewport) {
			return editor.ProjectionFor(viewport);
		}

		static void ResetPresentedOverlay(Editor &editor, WorldId world, const engine::core::CFrame &frame) {
			editor.Active = world;
			auto &slot = editor.Overlays[0];
			slot.Presented = true;
			slot.PresentedWorld = world;
			slot.PresentedFrame = frame;
			slot.PresentedFieldOfView = 0.5f;
			slot.Drawn = true;
			slot.ClearFrame();
			CHECK_FALSE(slot.Drawn);
			CHECK(slot.List == nullptr);
		}

		static void SetOverlayRectangle(
			Editor &editor,
			size_t viewport,
			float x,
			float y,
			float width,
			float height,
			uint32_t renderWidth,
			uint32_t renderHeight
		) {
			auto &slot = editor.Overlays[viewport];
			slot.X = x;
			slot.Y = y;
			slot.Width = width;
			slot.Height = height;
			slot.RenderWidth = renderWidth;
			slot.RenderHeight = renderHeight;
			slot.Drawn = true;
		}

		static void SetPresentedPair(
			Editor &editor,
			WorldId world,
			const engine::core::CFrame &first,
			const engine::core::CFrame &second
		) {
			editor.ResizeViewports(1);
			editor.Active = world;
			editor.Extras[0].World = world;
			for (size_t index = 0; index < 2; ++index) {
				auto &slot = editor.Overlays[index];
				slot.X = 20;
				slot.Y = 20;
				slot.Width = 300;
				slot.Height = 220;
				slot.RenderWidth = 600;
				slot.RenderHeight = 440;
				slot.Drawn = true;
				slot.Presented = true;
				slot.PresentedWorld = world;
				slot.PresentedFrame = index == 0 ? first : second;
				slot.PresentedFieldOfView = index == 0 ? 0.7f : 1.1f;
				slot.PresentedNearPlane = index == 0 ? 0.25f : 0.5f;
				slot.PresentedFarPlane = index == 0 ? 200.0f : 400.0f;
			}
		}

		static void
		SetViewerCamera(Editor &editor, size_t viewport, engine::core::CFrame frame, float fieldOfView) {
			const auto &viewer = editor.Viewers[viewport];
			bool updated = false;
			editor.Universe->Enter(viewer.World, [&](engine::ecs::Store &store) {
				auto *transform = store.GetMutable<engine::scene::Transform>(viewer.Instance);
				auto *camera = store.GetMutable<engine::scene::Camera>(viewer.Instance);
				if (transform == nullptr || camera == nullptr) {
					return;
				}
				transform->Frame = frame;
				camera->FieldOfViewRadians = fieldOfView;
				updated = true;
			});
			REQUIRE(updated);
		}

		static engine::world::WorldId StartPlay(Editor &editor) {
			REQUIRE(editor.BeginRun(editor.Active, RunMode::Play));
			REQUIRE_FALSE(editor.Runs.empty());
			REQUIRE_FALSE(editor.Runs.back().Links.empty());
			REQUIRE(editor.Runs.back().Links.front() != nullptr);
			return editor.Runs.back().Links.front()->ReplicaWorld();
		}

		static bool AddDemo(Editor &editor, const engine::examples::DemoEntry &demo) {
			return editor.AddExampleWorld(demo);
		}

		static bool StartServer(Editor &editor) {
			return editor.BeginRun(editor.Active, RunMode::Server);
		}

		static engine::core::Name CaptureProceduralPreview(Editor &editor) {
			editor.ShowProcedural = true;
			editor.ProceduralWorld = editor.Active;
			editor.ProceduralPanel = std::make_unique<ProceduralPanelState>();
			editor.ProceduralPreview =
				std::make_unique<ProceduralEditor>(engine::parallel::JobContext::Serial);
			editor.ProceduralPreviewOwner = engine::core::Name(
				"studio.procedural/" + std::to_string(editor.ProceduralPreview->PreviewStore().Identity())
			);
			editor.ProceduralPreviewTexture =
				engine::core::Name(std::string(editor.ProceduralPreviewOwner.Text()) + "/image");
			REQUIRE(editor.ProceduralPanel->Queue({ProceduralActionKind::Initialize}));
			REQUIRE(editor.ProceduralPanel->Queue({ProceduralActionKind::Preview}));
			editor.ApplyProceduralPending();
			editor.ApplyProceduralPending();
			REQUIRE(editor.ProceduralPanel->Queue({ProceduralActionKind::Activate}));
			editor.ApplyProceduralPending();
			REQUIRE(editor.RenderProceduralPreviewSlot());
			REQUIRE(editor.ProceduralPreviewHandle != nullptr);
			return editor.ProceduralPreviewTexture;
		}

		static void DrawProceduralPreview(Editor &editor) {
			ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
			ImGui::SetNextWindowSize(ImVec2(900, 1000), ImGuiCond_Always);
			editor.DrawProceduralGeneration();
		}

		static bool Save(Editor &editor, const std::filesystem::path &path) {
			return editor.SaveGame(path);
		}

		static bool Open(Editor &editor, const std::filesystem::path &path) {
			return editor.OpenGame(path);
		}

		static bool RetiredProceduralTexture(const Editor &editor, engine::core::Name texture) {
			return std::find(
					   editor.RetiredProceduralPreviewTextures.begin(),
					   editor.RetiredProceduralPreviewTextures.end(),
					   texture
				   ) != editor.RetiredProceduralPreviewTextures.end();
		}

		static bool TryRefreshProceduralPreview(Editor &editor) {
			editor.ProceduralPanel->Orbit += 0.25f;
			editor.ProceduralPreviewWanted = true;
			return editor.RenderProceduralPreviewSlot();
		}

		static auto ProceduralImageState(const Editor &editor) {
			return std::tuple{
				editor.ProceduralPreviewTexture,
				editor.ProceduralPreviewHandle,
				editor.ProceduralRenderedMesh,
				editor.ProceduralRenderedRevision,
				editor.ProceduralRenderedOrbit
			};
		}

		static engine::core::Name RefreshProceduralPreview(Editor &editor) {
			REQUIRE(TryRefreshProceduralPreview(editor));
			return editor.ProceduralPreviewTexture;
		}

		static void PresentFrame(Editor &editor) {
			editor.Present(1.0f / 60.0f);
		}

		static void TickPlay(Editor &editor) {
			for (size_t tick = 0; tick < 24; ++tick) {
				for (auto &run : editor.Runs)
					for (auto &link : run.Links)
						link->Step(*editor.Universe);
				editor.Universe->Tick(1.0f / 60.0f);
			}
		}

		static engine::scene::FlipbookFacts Flipbook(Editor &editor, engine::world::WorldId world) {
			engine::scene::FlipbookFacts facts;
			editor.Universe->Enter(world, [&](const engine::ecs::Store &store) {
				facts = engine::scene::FlipbookOf(store, engine::core::Name("effects/fox_dance.atex"));
			});
			return facts;
		}

		static bool HasPackagedFlipbook(Editor &editor) {
			return editor.Renderer.TextureHandle(engine::core::Name("effects/fox_dance.atex")) != nullptr;
		}

		static void Show(Editor &editor, engine::world::WorldId world) {
			editor.Active = world;
		}

		static void SetReplicaFieldOfView(Editor &editor, engine::world::WorldId world, float fieldOfView) {
			bool updated = false;
			editor.Universe->Enter(world, [&](engine::ecs::Store &store) {
				const engine::ecs::Entity camera = RuntimeCameraOf(store);
				if (auto *lens = store.GetMutable<engine::scene::Camera>(camera)) {
					lens->FieldOfViewRadians = fieldOfView;
					updated = true;
				}
			});
			REQUIRE(updated);
		}

		static void SetReplicaCamera(
			Editor &editor, engine::world::WorldId world, engine::core::CFrame frame, float fieldOfView
		) {
			bool updated = false;
			editor.Universe->Enter(world, [&](engine::ecs::Store &store) {
				const engine::ecs::Entity camera = RuntimeCameraOf(store);
				auto *transform = store.GetMutable<engine::scene::Transform>(camera);
				auto *lens = store.GetMutable<engine::scene::Camera>(camera);
				if (transform == nullptr || lens == nullptr) {
					return;
				}
				transform->Frame = frame;
				lens->FieldOfViewRadians = fieldOfView;
				updated = true;
			});
			REQUIRE(updated);
		}

		static std::array<engine::ecs::Entity, 2> CreateTwo(Editor &editor, engine::world::WorldId world) {
			editor.Viewers.resize(2);
			const ViewportCameraPose pose = DefaultViewportCamera();
			editor.EnsureViewerCamera(0, world, pose.Frame, {}, engine::ecs::NULL_ENTITY);
			editor.EnsureViewerCamera(1, world, pose.Frame, {}, engine::ecs::NULL_ENTITY);
			return {editor.Viewers[0].Instance, editor.Viewers[1].Instance};
		}

		static void Release(Editor &editor, size_t viewport) {
			editor.ReleaseViewerCamera(viewport);
		}

		static Entity Borrow(Editor &editor, WorldId world, const engine::scene::Camera &lens) {
			const ViewportCameraPose pose = DefaultViewportCamera();
			editor.EnsureViewerCamera(0, world, pose.Frame, lens, engine::ecs::NULL_ENTITY, true);
			return editor.Viewers[0].Instance;
		}

		static void SetFollows(Editor &editor, engine::ecs::Entity main, engine::ecs::Entity extra) {
			editor.Extras.resize(1);
			editor.FollowCamera = main;
			editor.Extras[0].Follow = extra;
		}

		static std::array<engine::ecs::Entity, 2> Follows(const Editor &editor) {
			return {editor.FollowCamera, editor.Extras[0].Follow};
		}
	};

	struct ViewportGuiControlsProbe {
		static void
		PrepareAuthoring(Editor &editor, engine::world::WorldId world, engine::ecs::Entity frame) {
			editor.Active = world;
			editor.SelectionWorld = world;
			editor.Selection = {frame};
			editor.FocusedIsViewport = true;
			editor.FocusedViewport = 0;
			editor.CurrentTool = Editor::ToolMode::Select;
		}

		static void DrawAuthoring(Editor &editor, engine::world::WorldId world) {
			editor.ViewportActive = ImGui::IsItemActive();
			auto &slot = editor.Overlays[0];
			slot.List = ImGui::GetWindowDrawList();
			slot.X = slot.Y = 20;
			slot.Width = 300;
			slot.Height = 220;
			slot.Drawn = true;
			if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
				const ImVec2 mouse = ImGui::GetIO().MousePos;
				editor.SurfaceGesture = {
					.Active = true,
					.Viewport = 0,
					.World = world,
					.Start = {mouse.x, mouse.y},
					.StartedAt = ImGui::GetTime()
				};
			}
			editor.DrawViewportGui(0, {});
		}

		static bool Resizing(const Editor &editor) {
			return editor.GuiDragging.Instance != engine::ecs::NULL_ENTITY && editor.GuiDragging.Resize;
		}
		static bool Dragging(const Editor &editor) {
			return editor.GuiDragging.Instance != engine::ecs::NULL_ENTITY;
		}
		static bool SurfaceGestureActive(const Editor &editor) {
			return editor.SurfaceGesture.Active;
		}
		static bool Selected(const Editor &editor, engine::ecs::Entity frame) {
			return editor.Selection.size() == 1 && editor.Selection.front() == frame;
		}
		static void SetProfile(Editor &editor, GuiPreviewProfile profile) {
			editor.MainGuiPreview.Profile = profile;
		}
		static void Pick(Editor &editor, float x, float y) {
			PanelProjection panel;
			panel.ImageMin = {20, 20};
			panel.ImageSize = {300, 220};
			editor.ClearSelection();
			editor.PickInViewport(0, x, y, false, panel);
		}
		static void Apply(Editor &editor) {
			editor.ApplyPendingActions();
		}

		static void DrawInput(Editor &editor, engine::world::WorldId world, bool focused = true) {
			editor.Active = world;
			editor.FocusedIsViewport = focused;
			editor.FocusedViewport = 0;
			editor.CurrentTool = Editor::ToolMode::None;
			editor.ViewportActive = ImGui::IsItemActive();
			auto &slot = editor.Overlays[0];
			slot.List = ImGui::GetWindowDrawList();
			slot.X = slot.Y = 20;
			slot.Width = 300;
			slot.Height = 220;
			slot.Drawn = true;
			editor.DrawViewportGui(0, {});
			editor.RouteViewportGuiKeyboard();
		}

		static bool VisibleByDefault(const Editor &editor) {
			return editor.ShowGuiPreviewControls;
		}

		static bool Draw(Editor &editor, size_t index, glm::vec2 position) {
			return editor.DrawViewportGuiControls(index, position);
		}

		static void SetVisible(Editor &editor, bool visible) {
			editor.ShowGuiPreviewControls = visible;
		}
	};
}

TEST_SUITE_ID("studio.viewports")
TEST_DEPENDS("engine.game.worldscripts")
TEST_DEPENDS("studio.proceduralpaneladapter")

using engine::world::WorldId;
using studio::CameraRelativeMovement;
using studio::CanvasForViewport;
using studio::CarryViewportCamera;
using studio::ChooseViewportFor;
using studio::CreateRuntimeCamera;
using studio::DefaultViewportCamera;
using studio::NO_VIEWPORT;
using studio::PanelView;
using studio::ResolveViewportDirectionControls;
using studio::ResolveViewportTargetSize;
using studio::RuntimeCameraOf;
using studio::SnapViewportCameraDirection;
using studio::ViewportCameraMemory;
using studio::ViewportCameraPose;
using studio::ViewportGuiSource;
using studio::ViewportGuiSourceFor;

namespace {
	struct SavedGameScratch {
		std::filesystem::path Root;

		SavedGameScratch() {
			std::random_device random;
			for (int attempt = 0; attempt < 16; ++attempt) {
				const std::filesystem::path candidate = std::filesystem::temp_directory_path() /
														("atomic-studio-reopen-" + std::to_string(random()));
				if (std::filesystem::create_directory(candidate)) {
					Root = candidate;
					return;
				}
			}
			throw std::runtime_error("could not create Studio reopen test directory");
		}

		~SavedGameScratch() {
			std::error_code ignored;
			std::filesystem::remove_all(Root, ignored);
		}
	};

	// Three scenes and a client view, as an editor mid-play holds them.
	constexpr WorldId SCENE{0};
	constexpr WorldId OTHER{1};
	constexpr WorldId CLIENT{2};

	class GuiContext {
	  public:
		GuiContext() {
			IMGUI_CHECKVERSION();
			Handle = ImGui::CreateContext();
			ImGuiIO &io = ImGui::GetIO();
			io.DisplaySize = ImVec2(640.0f, 480.0f);
			io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
			io.DeltaTime = 1.0f / 60.0f;
			io.IniFilename = nullptr;
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
		}

		~GuiContext() {
			ImGui::DestroyContext(Handle);
		}

		GuiContext(const GuiContext &) = delete;
		GuiContext &operator=(const GuiContext &) = delete;

	  private:
		ImGuiContext *Handle = nullptr;
	};

	TEST_CASE(
		"gizmo drag cancels when its viewport changes or closes", "[studio][viewports][gizmo][gizmo-lifetime]"
	) {
		using namespace engine;
		const int mode = GENERATE(0, 1, 3);
		GuiContext context;
		scene::RegisterSceneClasses();
		studio::Editor editor;
		editor.Universe = std::make_unique<world::Universe>();
		const auto first = editor.Universe->Create({.Name = core::Name("gizmo-first")});
		const auto second = editor.Universe->Create({.Name = core::Name("gizmo-second")});
		ecs::Entity firstPart;
		ecs::Entity secondPart;
		float initialHalfSize = 0.0f;
		editor.Universe->Enter(first, [&](ecs::Store &store) {
			firstPart = store.CreateInstance(scene::PartClass(), "Part");
			const core::CFrame pivot;
			REQUIRE(store.SetPropertyAuthored(firstPart, core::Name("PivotOffset"), &pivot, sizeof(pivot)));
			initialHalfSize = store.Get<scene::Bounds>(firstPart)->HalfExtent.X;
		});
		editor.Universe->Enter(second, [&](ecs::Store &store) {
			secondPart = store.CreateInstance(scene::PartClass(), "Part");
		});
		REQUIRE(firstPart == secondPart);
		studio::ViewportCameraProbe::PrepareGizmo(editor, first, firstPart, mode);

		studio::PanelProjection panel;
		panel.Eye = {0.0f, 0.0f, 10.0f};
		panel.ImageSize = {640.0f, 480.0f};
		panel.Matrix =
			glm::perspective(glm::radians(60.0f), 640.0f / 480.0f, 0.1f, 100.0f) *
			glm::lookAt(glm::vec3(0.0f, 0.0f, 10.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		glm::vec2 handle;
		REQUIRE(panel.WorldToPanel({0.8f, 0.0f, 0.0f}, handle));
		const auto frame = [&](glm::vec2 point, bool down, bool close) {
			ImGuiIO &io = ImGui::GetIO();
			io.AddMousePosEvent(point.x, point.y);
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
			ImGui::NewFrame();
			ImGui::SetNextWindowPos({0.0f, 0.0f});
			ImGui::SetNextWindowSize({640.0f, 480.0f});
			ImGui::Begin("Gizmo viewport", nullptr, ImGuiWindowFlags_NoDecoration);
			if (close) {
				studio::ViewportCameraProbe::CloseGizmoViewport(editor);
			} else {
				studio::ViewportCameraProbe::DrawGizmo(editor, panel);
			}
			ImGui::End();
			ImGui::Render();
		};
		frame(handle, false, false);
		frame(handle, true, false);
		REQUIRE(studio::ViewportCameraProbe::GizmoActive(editor));
		frame({handle.x + 50.0f, handle.y}, true, false);
		editor.Universe->Enter(first, [&](const ecs::Store &store) {
			if (mode == 0) REQUIRE(store.Get<scene::Transform>(firstPart)->Frame.Position.X > 0.1f);
			if (mode == 1) REQUIRE(store.Get<scene::Bounds>(firstPart)->HalfExtent.X > initialHalfSize);
			if (mode == 3) REQUIRE(store.Get<scene::Pivot>(firstPart)->Offset.Position.X > 0.1f);
		});

		SECTION("retargeting does not move an equal handle in another world") {
			studio::ViewportCameraProbe::SelectGizmoWorld(editor, second, secondPart);
			frame({handle.x + 80.0f, handle.y}, true, false);
			editor.Universe->Enter(second, [&](const ecs::Store &store) {
				CHECK(store.Get<scene::Transform>(secondPart)->Frame.Position.X == Catch::Approx(0.0f));
			});
			CHECK_FALSE(studio::ViewportCameraProbe::GizmoActive(editor));
		}
		SECTION("closing the viewport releases the drag") {
			frame({handle.x + 50.0f, handle.y}, false, true);
			CHECK_FALSE(studio::ViewportCameraProbe::GizmoActive(editor));
		}
		SECTION("clearing selection cancels the drag") {
			studio::ViewportCameraProbe::ClearGizmoSelection(editor);
			frame({handle.x + 50.0f, handle.y}, true, false);
			CHECK_FALSE(studio::ViewportCameraProbe::GizmoActive(editor));
		}
		SECTION("a completed drag remains undoable") {
			frame({handle.x + 50.0f, handle.y}, false, false);
			REQUIRE(studio::ViewportCameraProbe::GizmoCanUndo(editor));
			REQUIRE(studio::ViewportCameraProbe::Undo(editor));
		}
		SECTION("candidate preparation leaves the live preview and change version untouched") {
			uint64_t beforeVersion = 0;
			editor.Universe->Enter(first, [&](const ecs::Store &store) {
				beforeVersion = store.ChangeVersion();
			});
			core::ByteWriter bytes;
			REQUIRE(editor.Universe->Save(bytes));
			world::Universe candidate;
			core::ByteReader reader(bytes.Bytes());
			REQUIRE(candidate.Load(reader));
			std::string detail;
			REQUIRE(studio::ViewportCameraProbe::PrepareGizmoCandidate(editor, candidate, detail));
			candidate.Enter(candidate.Find(core::Name("gizmo-first")), [&](const ecs::Store &store) {
				REQUIRE(store.Alive(firstPart));
				CHECK(store.Get<scene::Transform>(firstPart)->Frame.Position.X == Catch::Approx(0.0f));
				CHECK(store.Get<scene::Bounds>(firstPart)->HalfExtent.X == Catch::Approx(initialHalfSize));
				CHECK(store.Get<scene::Pivot>(firstPart)->Offset.Position.X == Catch::Approx(0.0f));
			});
			CHECK(studio::ViewportCameraProbe::GizmoActive(editor));
			editor.Universe->Enter(first, [&](const ecs::Store &store) {
				CHECK(store.ChangeVersion() == beforeVersion);
				if (mode == 0) CHECK(store.Get<scene::Transform>(firstPart)->Frame.Position.X > 0.1f);
				if (mode == 1) CHECK(store.Get<scene::Bounds>(firstPart)->HalfExtent.X > initialHalfSize);
				if (mode == 3) CHECK(store.Get<scene::Pivot>(firstPart)->Offset.Position.X > 0.1f);
			});
			return;
		}
		SECTION("replacing a world cannot restore a drag into its replacement") {
			const bool release = GENERATE(true, false);
			if (release) {
				studio::ViewportCameraProbe::ReleaseGizmoWorld(editor, first);
				CHECK_FALSE(studio::ViewportCameraProbe::GizmoActive(editor));
			}
			REQUIRE(editor.Universe->Destroy(first) == world::WorldStatus::Ok);
			const auto replacement = editor.Universe->Create({.Name = core::Name("gizmo-replacement")});
			REQUIRE(replacement == first);
			ecs::Entity replacementPart;
			editor.Universe->Enter(replacement, [&](ecs::Store &store) {
				replacementPart = store.CreateInstance(scene::PartClass(), "Replacement");
				const core::CFrame position(core::Vector3{5.0f, 0.0f, 0.0f});
				const core::CFrame pivot(core::Vector3{3.0f, 0.0f, 0.0f});
				const core::Vector3 size{8.0f, 2.0f, 2.0f};
				REQUIRE(store.SetPropertyAuthored(
					replacementPart, core::Name("CFrame"), &position, sizeof(position)
				));
				REQUIRE(store.SetPropertyAuthored(
					replacementPart, core::Name("PivotOffset"), &pivot, sizeof(pivot)
				));
				REQUIRE(store.SetPropertyAuthored(replacementPart, core::Name("Size"), &size, sizeof(size)));
			});
			REQUIRE(replacementPart == firstPart);
			if (!release) frame({handle.x + 80.0f, handle.y}, true, false);
			frame({handle.x + 50.0f, handle.y}, false, true);
			CHECK_FALSE(studio::ViewportCameraProbe::GizmoActive(editor));
			editor.Universe->Enter(replacement, [&](const ecs::Store &store) {
				CHECK(store.Get<scene::Transform>(replacementPart)->Frame.Position.X == Catch::Approx(5.0f));
				CHECK(store.Get<scene::Bounds>(replacementPart)->HalfExtent.X == Catch::Approx(4.0f));
				CHECK(store.Get<scene::Pivot>(replacementPart)->Offset.Position.X == Catch::Approx(3.0f));
			});
			return;
		}
		editor.Universe->Enter(first, [&](const ecs::Store &store) {
			CHECK(
				store.Get<scene::Transform>(firstPart)->Frame.Position.X ==
				Catch::Approx(0.0f).margin(0.0001f)
			);
			CHECK(
				store.Get<scene::Bounds>(firstPart)->HalfExtent.X ==
				Catch::Approx(initialHalfSize).margin(0.0001f)
			);
			CHECK(
				store.Get<scene::Pivot>(firstPart)->Offset.Position.X == Catch::Approx(0.0f).margin(0.0001f)
			);
		});
		CHECK_FALSE(studio::ViewportCameraProbe::GizmoCanUndo(editor));
	}

	TEST_CASE(
		"factory requests preserve gizmo previews correctly", "[studio][viewports][gizmo][gizmo-factory]"
	) {
		using namespace engine;
		GuiContext context;
		scene::RegisterSceneClasses();
		studio::Editor editor;
		editor.Universe = std::make_unique<world::Universe>();
		REQUIRE(studio::ViewportCameraProbe::StartFactory(editor));
		auto &session = studio::ViewportCameraProbe::FactorySession(editor);
		world::DataFactoryWorldRequest create{
			.InstanceId = "factory", .Seed = 41, .TickRate = 60.0, .OperationId = "create"
		};
		const auto created = session.CreateWorld(create);
		REQUIRE(created.Status == world::DataFactoryStatus::Ok);
		const auto removed = editor.Universe->Create({.Name = core::Name("gizmo-hole")});
		const auto other = editor.Universe->Create({.Name = core::Name("gizmo-other")});
		REQUIRE(editor.Universe->Destroy(removed) == world::WorldStatus::Ok);
		ecs::Entity part;
		editor.Universe->Enter(other, [&](ecs::Store &store) {
			part = store.CreateInstance(scene::PartClass(), "GizmoPart");
		});
		studio::ViewportCameraProbe::PrepareGizmo(editor, other, part, 0);

		studio::PanelProjection panel;
		panel.Eye = {0.0f, 0.0f, 10.0f};
		panel.ImageSize = {640.0f, 480.0f};
		panel.Matrix =
			glm::perspective(glm::radians(60.0f), 640.0f / 480.0f, 0.1f, 100.0f) *
			glm::lookAt(glm::vec3(0.0f, 0.0f, 10.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		glm::vec2 handle;
		REQUIRE(panel.WorldToPanel({0.8f, 0.0f, 0.0f}, handle));
		const auto frame = [&](glm::vec2 point, bool down) {
			ImGuiIO &io = ImGui::GetIO();
			io.AddMousePosEvent(point.x, point.y);
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
			ImGui::NewFrame();
			ImGui::SetNextWindowPos({0.0f, 0.0f});
			ImGui::SetNextWindowSize({640.0f, 480.0f});
			ImGui::Begin("Factory gizmo", nullptr, ImGuiWindowFlags_NoDecoration);
			studio::ViewportCameraProbe::DrawGizmo(editor, panel);
			ImGui::End();
			ImGui::Render();
		};
		frame(handle, false);
		frame(handle, true);
		frame({handle.x + 50.0f, handle.y}, true);
		REQUIRE(studio::ViewportCameraProbe::GizmoActive(editor));
		editor.Universe->Enter(other, [&](const ecs::Store &store) {
			REQUIRE(store.Get<scene::Transform>(part)->Frame.Position.X > 0.1f);
		});

		SECTION("resetting a different world does not commit the preview") {
			auto reset = create;
			reset.OperationId = "reset";
			reset.ExpectedWorldEpoch = created.WorldEpoch;
			reset.ExpectedWorldVersion = created.WorldVersion;
			reset.ExpectedTick = created.Clock.Tick;
			REQUIRE(session.ResetWorld(reset).Status == world::DataFactoryStatus::Ok);
			const auto restored = editor.Universe->Find(core::Name("gizmo-other"));
			REQUIRE(restored.IsValid());
			CHECK(restored != other);
			editor.Universe->Enter(restored, [&](const ecs::Store &store) {
				const auto restoredPart = store.FindFirstRoot("GizmoPart");
				REQUIRE(store.Alive(restoredPart));
				CHECK(store.Get<scene::Transform>(restoredPart)->Frame.Position.X == Catch::Approx(0.0f));
			});
			CHECK_FALSE(studio::ViewportCameraProbe::GizmoActive(editor));
			CHECK_FALSE(studio::ViewportCameraProbe::GizmoCanUndo(editor));
		}
		SECTION("package transactions preserve gizmo state across success and failure") {
			const int scenario = GENERATE(0, 1, 2, 3, 4);
			const bool stale = scenario == 0;
			const bool runtimeFailure = scenario == 1;
			if (scenario == 2 || scenario == 3) {
				session.SetCandidatePrepare([scenario](world::Universe &, std::string &detail) {
					if (scenario == 3) throw std::runtime_error("candidate refused");
					detail = "candidate refused";
					return false;
				});
			}
			uint64_t changeVersion = 0;
			uint64_t transformVersion = 0;
			editor.Universe->Enter(other, [&](const ecs::Store &store) {
				changeVersion = store.ChangeVersion();
				transformVersion = store.ComponentChangeVersion<scene::Transform>();
			});
			engine::control::Surface surface("studio-test", "test");
			studio::ViewportCameraProbe::InstallFactoryTools(editor, surface);
			const auto tool =
				std::find_if(surface.Registered().begin(), surface.Registered().end(), [](const auto &entry) {
					return entry.Name == "run_script_package";
				});
			REQUIRE(tool != surface.Registered().end());
			const std::string source = runtimeFailure ? "error('x')" : "return";
			const std::string hash =
				assets::Hasher::Of(std::as_bytes(std::span(source.data(), source.size()))).ToHex();
			const std::string manifest =
				"{\"format\":\"atomic.data-script.v1\",\"entry\":\"package.luau\",\"source_hash\":\"" + hash +
				"\",\"assets\":[],\"parameters\":[],\"capabilities\":[],\"budget\":{\"source_bytes\":1024,"
				"\"asset_bytes\":0,\"assets\":0,\"parameters\":0},\"seed\":0}";
			const nlohmann::json request = {
				{"instance_id", "factory"},
				{"expected_tick", created.Clock.Tick},
				{"expected_world_epoch", created.WorldEpoch},
				{"expected_world_version", created.WorldVersion + (stale ? 1 : 0)},
				{"operation_id", "rejected"},
				{"language", "luau"},
				{"manifest", manifest},
				{"source_base64", runtimeFailure ? "ZXJyb3IoJ3gnKQ==" : "cmV0dXJu"},
				{"assets", nlohmann::json::array()}
			};
			std::string failure;
			const auto reply = tool->Call(request, failure);
			REQUIRE(failure.empty());
			if (scenario == 4) {
				REQUIRE(reply["terminal"] == "completed");
				const auto restored = editor.Universe->Find(core::Name("gizmo-other"));
				REQUIRE(restored.IsValid());
				CHECK(restored != other);
				editor.Universe->Enter(restored, [&](const ecs::Store &store) {
					const auto restoredPart = store.FindFirstRoot("GizmoPart");
					REQUIRE(store.Alive(restoredPart));
					CHECK(store.Get<scene::Transform>(restoredPart)->Frame.Position.X == Catch::Approx(0.0f));
				});
				CHECK_FALSE(studio::ViewportCameraProbe::GizmoActive(editor));
				CHECK_FALSE(studio::ViewportCameraProbe::GizmoCanUndo(editor));
				return;
			}
			REQUIRE(reply["terminal"] == "failed");
			if (scenario == 2) CHECK(reply["error"] == "candidate refused");
			if (scenario == 3) CHECK(reply["error"] == "candidate preparation threw");
			CHECK(studio::ViewportCameraProbe::GizmoActive(editor));
			editor.Universe->Enter(other, [&](const ecs::Store &store) {
				CHECK(store.Get<scene::Transform>(part)->Frame.Position.X > 0.1f);
				CHECK(store.ChangeVersion() == changeVersion);
				CHECK(store.ComponentChangeVersion<scene::Transform>() == transformVersion);
			});
		}
		SECTION("candidate preparation failure leaves the live drag unchanged") {
			const bool throws = GENERATE(true, false);
			uint64_t version = 0;
			editor.Universe->Enter(other, [&](const ecs::Store &store) { version = store.ChangeVersion(); });
			session.SetCandidatePrepare([throws](world::Universe &, std::string &detail) {
				if (throws) throw std::runtime_error("candidate refused");
				detail = "candidate refused";
				return false;
			});
			auto reset = create;
			reset.OperationId = "failed-reset";
			reset.ExpectedWorldEpoch = created.WorldEpoch;
			reset.ExpectedWorldVersion = created.WorldVersion;
			reset.ExpectedTick = created.Clock.Tick;
			CHECK(session.ResetWorld(reset).Status == world::DataFactoryStatus::RestoreIncomplete);
			CHECK(studio::ViewportCameraProbe::GizmoActive(editor));
			editor.Universe->Enter(other, [&](const ecs::Store &store) {
				CHECK(store.Get<scene::Transform>(part)->Frame.Position.X > 0.1f);
				CHECK(store.ChangeVersion() == version);
			});
		}
	}

	TEST_CASE("returning a gizmo to its grab point restores the part", "[studio][viewports][gizmo]") {
		using namespace engine;
		const int mode = GENERATE(0, 1, 2, 3, 4, 5);
		GuiContext context;
		scene::RegisterSceneClasses();
		studio::Editor editor;
		editor.Universe = std::make_unique<world::Universe>();
		const auto world = editor.Universe->Create({.Name = core::Name("gizmo-return")});
		ecs::Entity part;
		editor.Universe->Enter(world, [&](ecs::Store &store) {
			part = store.CreateInstance(scene::PartClass(), "Part");
			const core::CFrame pivot;
			REQUIRE(store.SetPropertyAuthored(part, core::Name("PivotOffset"), &pivot, sizeof(pivot)));
		});
		studio::ViewportCameraProbe::PrepareGizmo(editor, world, part, mode);

		studio::PanelProjection panel;
		panel.Eye = {0.0f, 0.0f, 10.0f};
		panel.ImageSize = {640.0f, 480.0f};
		panel.Matrix =
			glm::perspective(glm::radians(60.0f), 640.0f / 480.0f, 0.1f, 100.0f) *
			glm::lookAt(glm::vec3(0.0f, 0.0f, 10.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		glm::vec2 handle;
		const bool rotate = mode == 2 || mode == 4;
		const bool scale = mode == 1 || mode == 5;
		const bool pivot = mode == 3 || mode == 4;
		REQUIRE(panel.WorldToPanel({rotate ? 1.2f : 0.8f, 0.0f, 0.0f}, handle));
		glm::vec2 turn;
		REQUIRE(panel.WorldToPanel({0.0f, 1.2f, 0.0f}, turn));
		const auto frame = [&](glm::vec2 point, bool down, const studio::PanelProjection &projection) {
			ImGuiIO &io = ImGui::GetIO();
			io.AddMousePosEvent(point.x, point.y);
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
			ImGui::NewFrame();
			ImGui::SetNextWindowPos({0.0f, 0.0f});
			ImGui::SetNextWindowSize({640.0f, 480.0f});
			ImGui::Begin("Gizmo viewport", nullptr, ImGuiWindowFlags_NoDecoration);
			studio::ViewportCameraProbe::DrawGizmo(editor, projection);
			ImGui::End();
			ImGui::Render();
		};
		const auto position = [&] {
			float x = 0.0f;
			editor.Universe->Enter(world, [&](const ecs::Store &store) {
				x = store.Get<scene::Transform>(part)->Frame.Position.X;
			});
			return x;
		};
		const auto halfSize = [&] {
			float x = 0.0f;
			editor.Universe->Enter(world, [&](const ecs::Store &store) {
				x = store.Get<scene::Bounds>(part)->HalfExtent.X;
			});
			return x;
		};
		const float beforeHalfSize = halfSize();
		const auto rotationZ = [&] {
			float z = 0.0f;
			editor.Universe->Enter(world, [&](const ecs::Store &store) {
				z = store.Get<scene::Transform>(part)->Frame.QuaternionZ;
			});
			return z;
		};
		const auto pivotFrame = [&] {
			core::CFrame value;
			editor.Universe->Enter(world, [&](const ecs::Store &store) {
				value = store.Get<scene::Pivot>(part)->Offset;
			});
			return value;
		};

		frame(handle, false, panel);
		frame(handle, true, panel);
		REQUIRE(studio::ViewportCameraProbe::GizmoActive(editor));
		frame(rotate ? turn : glm::vec2(handle.x + 50.0f, handle.y), true, panel);
		if (scale) {
			REQUIRE(halfSize() > beforeHalfSize);
		} else if (mode == 0) {
			REQUIRE(position() > 0.1f);
		} else if (mode == 2) {
			REQUIRE(std::abs(rotationZ()) > 0.1f);
		} else if (mode == 3) {
			REQUIRE(pivotFrame().Position.X > 0.1f);
		} else {
			REQUIRE(std::abs(pivotFrame().QuaternionZ) > 0.1f);
		}
		const float movedPosition = position();
		const float movedHalfSize = halfSize();
		const float movedRotation = rotationZ();
		const core::CFrame movedPivot = pivotFrame();
		studio::PanelProjection unresolved = panel;
		unresolved.Eye = {10.0f, 0.0f, 0.0f};
		unresolved.Matrix =
			glm::perspective(glm::radians(60.0f), 640.0f / 480.0f, 0.1f, 100.0f) *
			glm::lookAt(glm::vec3(10.0f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		frame({320.0f, 240.0f}, true, unresolved);
		CHECK(position() == Catch::Approx(movedPosition));
		CHECK(halfSize() == Catch::Approx(movedHalfSize));
		CHECK(rotationZ() == Catch::Approx(movedRotation));
		CHECK(pivotFrame().Position.X == Catch::Approx(movedPivot.Position.X));
		CHECK(pivotFrame().QuaternionZ == Catch::Approx(movedPivot.QuaternionZ));
		frame(handle, true, panel);
		CHECK(position() == Catch::Approx(0.0f).margin(0.0001f));
		CHECK(halfSize() == Catch::Approx(beforeHalfSize).margin(0.0001f));
		CHECK(rotationZ() == Catch::Approx(0.0f).margin(0.0001f));
		if (pivot) {
			CHECK(pivotFrame().Position.X == Catch::Approx(0.0f).margin(0.0001f));
			CHECK(pivotFrame().QuaternionZ == Catch::Approx(0.0f).margin(0.0001f));
		}
		frame(handle, false, panel);
		CHECK_FALSE(studio::ViewportCameraProbe::GizmoCanUndo(editor));
		CHECK_FALSE(studio::ViewportCameraProbe::Modified(editor));
	}

	TEST_CASE("losing window focus releases viewport keyboard ownership", "[studio][viewports][input]") {
		GuiContext context;
		studio::Editor editor;
		ImGui::NewFrame();
		ImGui::SetNextWindowFocus();
		ImGui::Begin(studio::ViewportCameraProbe::Window(editor));
		ImGui::End();
		studio::ViewportCameraProbe::ResolveFocus(editor);
		REQUIRE(studio::ViewportCameraProbe::HasViewportFocus(editor));

		ImGui::SetWindowFocus(nullptr);
		studio::ViewportCameraProbe::ResolveFocus(editor);
		CHECK_FALSE(studio::ViewportCameraProbe::HasViewportFocus(editor));
		CHECK(studio::Keybinds::CurrentScope() == studio::Scope::Global);
		ImGui::Render();
	}

	TEST_CASE("non viewport panels do not own viewport shortcuts", "[studio][viewports][input]") {
		GuiContext context;
		studio::Editor editor;
		const auto previousScope = studio::Keybinds::CurrentScope();
		std::vector<studio::Chord> previousBindings;
		for (const auto &binding : studio::Keybinds::All())
			previousBindings.push_back(binding.Keys);
		studio::Keybinds::Set(studio::Action::ShowHeap, studio::Chord{ImGuiKey_F21});
		studio::Keybinds::Set(studio::Action::Undo, studio::Chord{ImGuiKey_F22});
		studio::Keybinds::Set(studio::Action::ToolMove, studio::Chord{ImGuiKey_F23});
		studio::ViewportCameraProbe::AddScriptWindow(editor);

		const auto focus = [&](const char *title) {
			ImGui::SetNextWindowFocus();
			ImGui::Begin(title);
			ImGui::End();
			studio::ViewportCameraProbe::ResolveFocus(editor);
		};
		ImGuiIO &io = ImGui::GetIO();
		io.ConfigInputTrickleEventQueue = false;
		io.AddKeyEvent(ImGuiKey_F21, true);
		io.AddKeyEvent(ImGuiKey_F22, true);
		io.AddKeyEvent(ImGuiKey_F23, true);
		ImGui::NewFrame();
		focus(studio::ViewportCameraProbe::Window(editor));
		CHECK(studio::ViewportCameraProbe::HasViewportFocus(editor));
		CHECK(studio::Keybinds::CurrentScope() == studio::Scope::Viewport);
		CHECK(studio::Keybinds::Fired(studio::Action::ShowHeap));
		CHECK(studio::Keybinds::Fired(studio::Action::Undo));
		CHECK(studio::Keybinds::Fired(studio::Action::ToolMove));

		focus("Properties");
		CHECK_FALSE(studio::ViewportCameraProbe::HasViewportFocus(editor));
		CHECK(studio::Keybinds::CurrentScope() == studio::Scope::Editor);
		CHECK(studio::Keybinds::Fired(studio::Action::ShowHeap));
		CHECK(studio::Keybinds::Fired(studio::Action::Undo));
		CHECK_FALSE(studio::Keybinds::Fired(studio::Action::ToolMove));

		focus("Script###7-42");
		CHECK_FALSE(studio::ViewportCameraProbe::HasViewportFocus(editor));
		CHECK(studio::Keybinds::CurrentScope() == studio::Scope::Script);
		CHECK(studio::Keybinds::Fired(studio::Action::ShowHeap));
		CHECK_FALSE(studio::Keybinds::Fired(studio::Action::Undo));
		CHECK_FALSE(studio::Keybinds::Fired(studio::Action::ToolMove));
		ImGui::Render();
		auto bindings = studio::Keybinds::All();
		for (size_t i = 0; i < bindings.size(); ++i)
			bindings[i].Keys = previousBindings[i];
		studio::Keybinds::SetScope(previousScope);
	}

	TEST_CASE(
		"script focus during a frame releases viewport shortcut ownership", "[studio][viewports][input]"
	) {
		GuiContext context;
		ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
		studio::Editor editor;
		const auto previousScope = studio::Keybinds::CurrentScope();
		studio::ViewportCameraProbe::PrepareFullFrame(editor);

		ImGui::NewFrame();
		ImGui::SetNextWindowFocus();
		ImGui::Begin(studio::ViewportCameraProbe::Window(editor));
		ImGui::End();
		studio::ViewportCameraProbe::ResolveFocus(editor);
		REQUIRE(studio::ViewportCameraProbe::HasViewportFocus(editor));
		REQUIRE(studio::Keybinds::CurrentScope() == studio::Scope::Viewport);
		studio::ViewportCameraProbe::DrawInterface(editor);
		CHECK(ImGui::FindWindowByName("###7-42") == ImGui::GetCurrentContext()->NavWindow);
		CHECK_FALSE(studio::ViewportCameraProbe::HasViewportFocus(editor));
		CHECK(studio::Keybinds::CurrentScope() == studio::Scope::Script);
		ImGui::Render();
		studio::Keybinds::SetScope(previousScope);
	}

	TEST_CASE(
		"script focus prevents viewport textbox input in the same frame", "[studio][viewports][input]"
	) {
		GuiContext context;
		ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
		engine::scene::RegisterSceneClasses();
		engine::gui::RegisterGuiClasses();
		studio::Editor editor;
		const auto previousScope = studio::Keybinds::CurrentScope();
		studio::ViewportCameraProbe::PrepareFullFrame(editor);
		const auto box = studio::ViewportCameraProbe::AddFocusedTextBox(editor);
		studio::ViewportCameraProbe::ShowScriptOnNextFrame(editor, false);
		for (int frame = 0; frame < 3; ++frame) {
			ImGui::NewFrame();
			studio::ViewportCameraProbe::DrawInterface(editor);
			ImGui::Render();
		}
		REQUIRE(studio::ViewportCameraProbe::ViewportCompiled(editor, box));
		studio::ViewportCameraProbe::ShowScriptOnNextFrame(editor, true);

		ImGui::GetIO().AddInputCharactersUTF8("x");
		ImGui::NewFrame();
		ImGui::SetWindowFocus(studio::ViewportCameraProbe::Window(editor));
		studio::ViewportCameraProbe::ResolveFocus(editor);
		REQUIRE(studio::ViewportCameraProbe::HasViewportFocus(editor));
		studio::ViewportCameraProbe::DrawInterface(editor);
		REQUIRE(studio::ViewportCameraProbe::ViewportCompiled(editor, box));
		REQUIRE(ImGui::FindWindowByName("###7-42") == ImGui::GetCurrentContext()->NavWindow);
		CHECK(studio::ViewportCameraProbe::TextBoxText(editor, box).empty());
		ImGui::Render();
		studio::Keybinds::SetScope(previousScope);
	}

	struct GuiPreviewControlFrame {
		bool ControlsCapture = false;
		bool SurfaceActive = false;
	};

	GuiPreviewControlFrame DrawGuiPreviewControlFrame(studio::Editor &editor, bool down) {
		ImGuiIO &io = ImGui::GetIO();
		io.AddMousePosEvent(140.0f, 110.0f);
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(400.0f, 300.0f), ImGuiCond_Always);
		ImGui::Begin("GUI preview controls");
		GuiPreviewControlFrame frame;
		frame.ControlsCapture = studio::ViewportGuiControlsProbe::Draw(editor, 0, glm::vec2{100.0f, 100.0f});
		ImGui::SetCursorScreenPos(ImVec2(20.0f, 20.0f));
		ImGui::InvisibleButton("##surface", ImVec2(300.0f, 220.0f));
		frame.SurfaceActive = ImGui::IsItemActive();
		ImGui::End();
		ImGui::Render();
		return frame;
	}
}

TEST_CASE("script toolbar inserts into the selected world", "[studio][viewports][toolbar]") {
	using namespace engine;
	GuiContext context;
	studio::Editor editor;
	editor.Universe = std::make_unique<world::Universe>();
	const auto main = editor.Universe->Create({.Name = core::Name("toolbar.main")});
	const auto extra = editor.Universe->Create({.Name = core::Name("toolbar.extra")});
	scene::RegisterSceneClasses();
	script::ScriptClass();
	ecs::Entity parent;
	editor.Universe->Enter(main, [&](ecs::Store &store) { scene::InstallServices(store); });
	editor.Universe->Enter(extra, [&](ecs::Store &store) {
		scene::InstallServices(store);
		parent = store.CreateInstance(scene::PartClass(), "SelectedParent");
		store.SetParent(parent, scene::WorkspaceOf(store));
	});
	SECTION("an instance in an extra viewport is selected") {
		studio::ViewportCameraProbe::SelectInExtraViewport(editor, main, extra, parent);
	}
	SECTION("a world row is selected") {
		studio::ViewportCameraProbe::SelectWorldRow(editor, main, extra);
		editor.Universe->Enter(extra, [&](const ecs::Store &store) { parent = scene::WorkspaceOf(store); });
	}
	const auto draw = [&](ImVec2 mouse, bool down) {
		ImGuiIO &io = ImGui::GetIO();
		io.AddMousePosEvent(mouse.x, mouse.y);
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(300, 120), ImGuiCond_Always);
		ImGui::Begin("Script toolbar test");
		studio::ViewportCameraProbe::DrawScriptTool(editor);
		const ImVec2 lo = ImGui::GetItemRectMin();
		const ImVec2 hi = ImGui::GetItemRectMax();
		ImGui::End();
		ImGui::Render();
		return ImVec2((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);
	};
	const ImVec2 point = draw(ImVec2(-1, -1), false);
	draw(point, false);
	draw(point, true);
	draw(point, false);
	studio::ViewportCameraProbe::ApplyPending(editor);
	editor.Universe->Enter(extra, [&](const ecs::Store &store) {
		const auto created = store.FindFirstChild(parent, "Script");
		REQUIRE(created != ecs::NULL_ENTITY);
		CHECK(store.IsA(created, script::ScriptClass()));
	});
}

TEST_CASE("object insertion targets the selected world", "[studio][viewports][toolbar]") {
	using namespace engine;
	GuiContext context;
	studio::Editor editor;
	editor.Universe = std::make_unique<world::Universe>();
	const auto main = editor.Universe->Create({.Name = core::Name("object.main")});
	const auto extra = editor.Universe->Create({.Name = core::Name("object.extra")});
	scene::RegisterSceneClasses();
	ecs::Entity parent;
	editor.Universe->Enter(main, [&](ecs::Store &store) { scene::InstallServices(store); });
	editor.Universe->Enter(extra, [&](ecs::Store &store) {
		scene::InstallServices(store);
		parent = store.CreateInstance(scene::PartClass(), "SelectedParent");
		store.SetParent(parent, scene::WorkspaceOf(store));
	});
	const bool selectedRow = GENERATE(false, true);
	const bool menu = GENERATE(false, true);
	if (!selectedRow) {
		studio::ViewportCameraProbe::SelectInExtraViewport(editor, main, extra, parent);
	} else {
		studio::ViewportCameraProbe::SelectWorldRow(editor, main, extra);
		editor.Universe->Enter(extra, [&](const ecs::Store &store) { parent = scene::WorkspaceOf(store); });
	}
	const auto frame = [&](bool open, bool type, ImVec2 mouse, bool down) {
		ImGuiIO &io = ImGui::GetIO();
		io.AddMousePosEvent(mouse.x, mouse.y);
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
		if (type) io.AddInputCharactersUTF8("Part");
		ImGui::NewFrame();
		if (menu) {
			if (open && ImGui::BeginMainMenuBar()) {
				ImGui::OpenPopup("Insert");
				ImGui::EndMainMenuBar();
			}
			studio::ViewportCameraProbe::DrawMenuBar(editor);
		} else {
			ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
			ImGui::SetNextWindowSize(ImVec2(300, 120), ImGuiCond_Always);
			ImGui::Begin("Insert toolbar test");
			if (open) ImGui::OpenPopup("insert-object");
			studio::ViewportCameraProbe::DrawInsertTool(editor);
			ImGui::End();
		}
		ImVec2 item(-1, -1);
		for (ImGuiWindow *window : ImGui::GetCurrentContext()->Windows) {
			if (window->Active &&
				std::string_view(window->Name).find("##class-list") != std::string_view::npos) {
				item = ImVec2(
					window->DC.CursorStartPos.x + 10.0f,
					window->DC.CursorStartPos.y + ImGui::GetTextLineHeight() * 0.5f
				);
			}
		}
		ImGui::Render();
		return item;
	};
	frame(true, false, ImVec2(-1, -1), false);
	frame(false, false, ImVec2(-1, -1), false);
	frame(false, true, ImVec2(-1, -1), false);
	REQUIRE(std::string_view(ImGui::GetCurrentContext()->InputTextState.GetText()) == "Part");
	const ImVec2 item = frame(false, false, ImVec2(-1, -1), false);
	REQUIRE(item.x >= 0.0f);
	frame(false, false, item, false);
	frame(false, false, item, true);
	frame(false, false, item, false);
	studio::ViewportCameraProbe::ApplyPending(editor);
	editor.Universe->Enter(main, [&](const ecs::Store &store) {
		CHECK(store.FindFirstChild(scene::WorkspaceOf(store), "Part") == ecs::NULL_ENTITY);
	});
	editor.Universe->Enter(extra, [&](const ecs::Store &store) {
		CHECK(store.FindFirstChild(parent, "Part") != ecs::NULL_ENTITY);
	});
}

TEST_CASE("command bar marks saved content dirty even when scripts fail", "[studio][command]") {
	using namespace engine;
	studio::Editor editor;
	editor.Universe = std::make_unique<world::Universe>();
	world::WorldSettings settings;
	settings.Name = core::Name("command.dirty");
	const auto world = editor.Universe->Create(settings);
	editor.Universe->Enter(world, [&](ecs::Store &store) { scene::InstallServices(store); });
	studio::ViewportCameraProbe::PrepareCommand(editor, world);

	CHECK(studio::ViewportCameraProbe::Command(editor, "print(workspace.Name)"));
	CHECK_FALSE(studio::ViewportCameraProbe::Modified(editor));
	CHECK(
		studio::ViewportCameraProbe::Command(
			editor, "local p = Instance.new('Part'); p.Name = 'Created'; p.Parent = workspace"
		)
	);
	CHECK(studio::ViewportCameraProbe::Modified(editor));

	studio::ViewportCameraProbe::ClearModified(editor);
	CHECK_FALSE(
		studio::ViewportCameraProbe::Command(editor, "workspace.Created.Name = 'Renamed'; error('stop')")
	);
	CHECK(studio::ViewportCameraProbe::Modified(editor));
}

TEST_CASE("command queries preserve redo while edits discard it", "[studio][command]") {
	using namespace engine;
	studio::Editor editor;
	editor.Universe = std::make_unique<world::Universe>();
	world::WorldSettings settings;
	settings.Name = core::Name("command.redo");
	const auto world = editor.Universe->Create(settings);
	editor.Universe->Enter(world, [&](ecs::Store &store) { scene::InstallServices(store); });
	studio::ViewportCameraProbe::PrepareCommand(editor, world);
	studio::ViewportCameraProbe::RecordPart(editor, world);
	REQUIRE(studio::ViewportCameraProbe::Undo(editor));
	REQUIRE(studio::ViewportCameraProbe::CanRedo(editor));
	studio::ViewportCameraProbe::MarkModified(editor);

	CHECK(studio::ViewportCameraProbe::Command(editor, "print(workspace.Name)"));
	CHECK(studio::ViewportCameraProbe::CanRedo(editor));
	CHECK(
		studio::ViewportCameraProbe::Command(editor, "local p = Instance.new('Part'); p.Parent = workspace")
	);
	CHECK_FALSE(studio::ViewportCameraProbe::CanRedo(editor));
}

TEST_CASE("command edits invalidate an open comparison even when already modified", "[studio][command]") {
	using namespace engine;
	studio::Editor editor;
	editor.Universe = std::make_unique<world::Universe>();
	world::WorldSettings settings;
	settings.Name = core::Name("command.diff");
	const auto world = editor.Universe->Create(settings);
	editor.Universe->Enter(world, [&](ecs::Store &store) { scene::InstallServices(store); });
	studio::ViewportCameraProbe::PrepareCommand(editor, world);
	studio::ViewportCameraProbe::MarkModified(editor);
	studio::ViewportCameraProbe::PrepareDiff(editor);
	CHECK(studio::ViewportCameraProbe::Command(editor, "print(workspace.Name)"));
	CHECK(studio::ViewportCameraProbe::DiffLoaded(editor));
	CHECK(
		studio::ViewportCameraProbe::Command(editor, "local p = Instance.new('Part'); p.Parent = workspace")
	);
	CHECK_FALSE(studio::ViewportCameraProbe::DiffLoaded(editor));
}

TEST_CASE("overlay frame reset retains the camera of the displayed image", "[studio][viewports][camera]") {
	using namespace engine;
	studio::Editor editor;
	editor.Universe = std::make_unique<world::Universe>();
	const auto world = editor.Universe->Create({.Name = core::Name("overlay.camera")});
	const core::CFrame recorded(core::Vector3{2.0f, 3.0f, 4.0f});
	studio::ViewportCameraProbe::ResetPresentedOverlay(editor, world, recorded);
	studio::ViewportCameraProbe::SetOverlayRectangle(editor, 0, 0.0f, 0.0f, 800.0f, 400.0f, 800, 400);
	const auto projection = studio::ViewportCameraProbe::Projection(editor, 0);
	CHECK(projection.Eye == recorded.Position);
	scene::Camera lens;
	lens.FieldOfViewRadians = 0.5f;
	const auto expected = scene::ResolveCamera(recorded, lens, 2.0f);
	CHECK(projection.Matrix[0][0] == expected.ViewProjection[0][0]);
}

TEST_CASE("spatial GUI picking uses each viewport's presented camera", "[studio][viewports][gui][input]") {
	using namespace engine;
	studio::Editor editor;
	scene::RegisterSceneClasses();
	gui::RegisterGuiClasses();
	editor.Universe = std::make_unique<world::Universe>();
	const auto world = editor.Universe->Create({.Name = core::Name("viewport.gui.eyes")});
	const core::CFrame first(core::Vector3{0, 0, 0});
	const core::CFrame second(core::Vector3{4, 0, 0});
	studio::ViewportCameraProbe::SetPresentedPair(editor, world, first, second);
	const auto left = studio::ViewportCameraProbe::Projection(editor, 0);
	const auto right = studio::ViewportCameraProbe::Projection(editor, 1);
	CHECK(left.CameraFrame.Position == first.Position);
	CHECK(right.CameraFrame.Position == second.Position);
	CHECK(left.FieldOfViewRadians == 0.7f);
	CHECK(right.FieldOfViewRadians == 1.1f);
	CHECK(left.Near == 0.25f);
	CHECK(right.Near == 0.5f);
	CHECK(left.Far == 200.0f);
	CHECK(right.Far == 400.0f);

	editor.Universe->Enter(world, [&](ecs::Store &store) {
		const auto workspace = scene::InstallServices(store);
		gui::InstallGuiServices(store);
		const auto surface = store.CreateInstance(gui::GuiClass("SurfaceGui"), "Surface");
		store.SetParent(surface, workspace);
		const auto button = store.CreateInstance(gui::GuiClass("TextButton"), "Button");
		store.SetParent(button, surface);
		gui::SpatialCanvas canvas;
		canvas.Size = {100, 100};
		canvas.Origin = {-1, 1, -5};
		canvas.AxisX = {2, 0, 0};
		canvas.AxisY = {0, -2, 0};
		canvas.Normal = {0, 0, 1};
		canvas.Visible = true;
		canvas.Interactive = true;
		store.Set(surface, canvas);
		gui::Resolved resolved;
		resolved.AbsoluteSize = {100, 100};
		resolved.Clip = core::Rect{0, 0, 100, 100};
		resolved.Rendered = true;
		store.Set(button, resolved);
		gui::DrawList list;
		gui::DrawCommand command;
		command.Source = button;
		command.Collector = surface;
		command.Spatial = true;
		command.Bounds = resolved.Clip;
		command.Clip = resolved.Clip;
		list.Commands.push_back(command);
		gui::Screen screen;
		screen.Width = 390;
		screen.Height = 844;
		gui::Pointer pointer;
		pointer.Position = {195, 422};
		pointer.Down = true;
		pointer.ScreenOnly = true;
		gui::Router router;
		const auto route = [&](const studio::PanelProjection &panel, const core::CFrame *geometry = nullptr) {
			scene::Camera camera;
			camera.FieldOfViewRadians = panel.FieldOfViewRadians;
			camera.NearPlane = panel.Near;
			camera.FarPlane = panel.Far;
			return render::RouteGuiPointer(
				store,
				list,
				screen,
				router,
				pointer,
				&camera,
				&panel.CameraFrame,
				panel.RenderSize.x / panel.RenderSize.y,
				geometry
			);
		};
		CHECK(route(left).Collector == surface);
		CHECK(route(right).Collector == ecs::NULL_ENTITY);
		canvas.MaxDistance = 10.0f;
		store.Set(surface, canvas);
		studio::ViewportDiagnostics diagnostics;
		diagnostics.LockFrustum(core::CFrame(core::Vector3{100, 0, 0}));
		const core::CFrame behaviourEye = diagnostics.EffectiveFrustum(left.CameraFrame);
		CHECK(route(left).Collector == surface);
		CHECK(route(left, &behaviourEye).Collector == ecs::NULL_ENTITY);
	});
}

TEST_CASE("GUI resize owns its drag and preserves selection", "[studio][viewports][gui][input]") {
	using namespace engine;
	GuiContext context;
	studio::Editor editor;
	scene::RegisterSceneClasses();
	gui::RegisterGuiClasses();
	editor.Universe = std::make_unique<world::Universe>();
	world::WorldSettings settings;
	settings.Name = core::Name("viewport.gui.resize");
	const auto world = editor.Universe->Create(settings);
	ecs::Entity frame;
	editor.Universe->Enter(world, [&](ecs::Store &store) {
		scene::InstallServices(store);
		const auto screen = store.CreateInstance(gui::GuiClass("ScreenGui"), "Screen");
		store.SetParent(screen, store.FindFirstRoot("StarterGui"));
		frame = store.CreateInstance(gui::GuiClass("Frame"), "Frame");
		store.SetParent(frame, screen);
	});
	studio::ViewportGuiControlsProbe::PrepareAuthoring(editor, world, frame);

	const auto draw = [&](float x, float y, bool down) {
		auto &io = ImGui::GetIO();
		io.AddMousePosEvent(x, y);
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(400, 300), ImGuiCond_Always);
		ImGui::Begin("GUI authoring");
		ImGui::SetCursorScreenPos(ImVec2(20, 20));
		ImGui::InvisibleButton("##surface", ImVec2(300, 220));
		studio::ViewportGuiControlsProbe::DrawAuthoring(editor, world);
		ImGui::End();
		ImGui::Render();
		studio::ViewportGuiControlsProbe::Apply(editor);
	};
	for (int idle = 0; idle < 3; ++idle)
		draw(123, 123, false);
	draw(70, 70, true);
	REQUIRE(studio::ViewportGuiControlsProbe::Dragging(editor));
	CHECK_FALSE(studio::ViewportGuiControlsProbe::Resizing(editor));
	CHECK_FALSE(studio::ViewportGuiControlsProbe::SurfaceGestureActive(editor));
	editor.Universe->Enter(world, [&](const ecs::Store &store) {
		const auto *element = store.Get<gui::Element>(frame);
		REQUIRE(element != nullptr);
		CHECK(element->Position.X.Offset == 0.0f);
		CHECK(element->Position.Y.Offset == 0.0f);
	});
	draw(70, 70, false);
	CHECK(studio::ViewportGuiControlsProbe::Selected(editor, frame));
	CHECK_FALSE(studio::ViewportGuiControlsProbe::Dragging(editor));
	draw(123, 123, false);
	draw(123, 123, true);
	REQUIRE(studio::ViewportGuiControlsProbe::Resizing(editor));
	CHECK_FALSE(studio::ViewportGuiControlsProbe::SurfaceGestureActive(editor));
	draw(163, 153, true);
	CHECK_FALSE(studio::ViewportGuiControlsProbe::SurfaceGestureActive(editor));
	draw(163, 153, false);
	CHECK(studio::ViewportGuiControlsProbe::Selected(editor, frame));
	editor.Universe->Enter(world, [&](const ecs::Store &store) {
		const auto *element = store.Get<gui::Element>(frame);
		REQUIRE(element != nullptr);
		CHECK(element->Size.X.Offset == 140.0f);
		CHECK(element->Size.Y.Offset == 130.0f);
	});
}

TEST_CASE(
	"phone preview selects and resizes GUI in displayed coordinates", "[studio][viewports][gui][input]"
) {
	using namespace engine;
	GuiContext context;
	studio::Editor editor;
	scene::RegisterSceneClasses();
	gui::RegisterGuiClasses();
	editor.Universe = std::make_unique<world::Universe>();
	const auto world = editor.Universe->Create({.Name = core::Name("viewport.gui.phone")});
	ecs::Entity frame;
	editor.Universe->Enter(world, [&](ecs::Store &store) {
		scene::InstallServices(store);
		const auto screen = store.CreateInstance(gui::GuiClass("ScreenGui"), "Screen");
		store.SetParent(screen, store.FindFirstRoot("StarterGui"));
		frame = store.CreateInstance(gui::GuiClass("Frame"), "Frame");
		store.SetParent(frame, screen);
		auto element = *store.Get<gui::Element>(frame);
		element.Position = {0.5f, 0.0f, 0.5f, 0.0f};
		store.Set(frame, element);
	});
	studio::ViewportGuiControlsProbe::PrepareAuthoring(editor, world, frame);
	studio::ViewportGuiControlsProbe::SetProfile(editor, studio::GuiPreviewProfile::Phone);
	const auto draw = [&](float x, float y, bool down) {
		auto &io = ImGui::GetIO();
		io.AddMousePosEvent(x, y);
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(400, 300), ImGuiCond_Always);
		ImGui::Begin("Phone authoring");
		ImGui::SetCursorScreenPos(ImVec2(20, 20));
		ImGui::InvisibleButton("##surface", ImVec2(300, 220));
		studio::ViewportGuiControlsProbe::DrawAuthoring(editor, world);
		ImGui::End();
		ImGui::Render();
		studio::ViewportGuiControlsProbe::Apply(editor);
	};
	for (int idle = 0; idle < 3; ++idle)
		draw(20, 20, false);
	gui::Resolved resolved;
	editor.Universe->Enter(world, [&](const ecs::Store &store) {
		const auto *found = store.Get<gui::Resolved>(frame);
		REQUIRE(found != nullptr);
		resolved = *found;
	});
	const float scaleX = 300.0f / 390.0f;
	const float scaleY = 220.0f / 844.0f;
	const float centerX = 20.0f + (resolved.AbsolutePosition.X + resolved.AbsoluteSize.X * 0.5f) * scaleX;
	const float centerY = 20.0f + (resolved.AbsolutePosition.Y + resolved.AbsoluteSize.Y * 0.5f) * scaleY;
	studio::ViewportGuiControlsProbe::Pick(editor, centerX, centerY);
	REQUIRE(studio::ViewportGuiControlsProbe::Selected(editor, frame));
	const float handleX = 20.0f + (resolved.AbsolutePosition.X + resolved.AbsoluteSize.X) * scaleX;
	const float handleY = 20.0f + (resolved.AbsolutePosition.Y + resolved.AbsoluteSize.Y) * scaleY;
	draw(handleX, handleY, false);
	draw(handleX, handleY, true);
	REQUIRE(studio::ViewportGuiControlsProbe::Resizing(editor));
	draw(handleX + 10.0f, handleY + 10.0f, true);
	draw(handleX + 10.0f, handleY + 10.0f, false);
	editor.Universe->Enter(world, [&](const ecs::Store &store) {
		const auto *element = store.Get<gui::Element>(frame);
		REQUIRE(element != nullptr);
		CHECK(element->Size.X.Offset == Catch::Approx(113.0f));
		CHECK(element->Size.Y.Offset == Catch::Approx(100.0f + 8440.0f / 220.0f));
	});
}

TEST_CASE("viewport pointer routing retains semantic activation events", "[studio][viewports][gui][input]") {
	using namespace engine;
	scene::RegisterSceneClasses();
	gui::RegisterGuiClasses();
	ecs::Store store("viewport.gui.semantic");
	scene::InstallServices(store);
	gui::InstallGuiServices(store);
	const auto screen = store.CreateInstance(gui::GuiClass("ScreenGui"), "Screen");
	store.SetParent(screen, store.FindFirstRoot("StarterGui"));
	const auto button = store.CreateInstance(gui::GuiClass("TextButton"), "Button");
	store.SetParent(button, screen);
	store.GetMutable<gui::Element>(button)->Selectable = true;
	gui::CompileRequest request;
	request.Display.Width = 300.0f;
	request.Display.Height = 220.0f;
	gui::Compiled compiled;
	compiled.Rebuild(store, request);
	gui::Router router;
	const auto semantic =
		router.UpdateSemantic(store, compiled.Commands(), button, gui::SemanticAction::Activate);
	std::vector<gui::GuiEvent> events(semantic.begin(), semantic.end());
	REQUIRE(events.size() == 1);
	REQUIRE(events.front().Kind == gui::EventKind::Activated);
	gui::Pointer outside;
	outside.Inside = false;
	studio::AppendViewportPointerEvents(router, store, compiled.Commands(), outside, events);
	REQUIRE(events.size() == 1);
	CHECK(events.front().Kind == gui::EventKind::Activated);
	CHECK(events.front().Instance == button);
}

TEST_CASE("viewport cancels spatial capture when its surface disappears", "[studio][viewports][gui][input]") {
	using namespace engine;
	scene::RegisterSceneClasses();
	gui::RegisterGuiClasses();
	ecs::Store store("viewport.gui.capture");
	const auto workspace = scene::InstallServices(store);
	gui::InstallGuiServices(store);
	const auto surface = store.CreateInstance(gui::GuiClass("SurfaceGui"), "Surface");
	store.SetParent(surface, workspace);
	const auto button = store.CreateInstance(gui::GuiClass("TextButton"), "Button");
	store.SetParent(button, surface);
	gui::SpatialCanvas canvas;
	canvas.Size = {100, 100};
	canvas.Origin = {-1, 1, -5};
	canvas.AxisX = {2, 0, 0};
	canvas.AxisY = {0, -2, 0};
	canvas.Normal = {0, 0, 1};
	canvas.Visible = true;
	canvas.Interactive = true;
	store.Set(surface, canvas);
	gui::Resolved resolved;
	resolved.AbsoluteSize = {100, 100};
	resolved.Clip = core::Rect{0, 0, 100, 100};
	resolved.Rendered = true;
	store.Set(button, resolved);
	gui::DrawList list;
	gui::DrawCommand command;
	command.Source = button;
	command.Collector = surface;
	command.Spatial = true;
	command.Bounds = resolved.Clip;
	command.Clip = resolved.Clip;
	list.Commands.push_back(command);
	gui::Router router;
	gui::Pointer pressed;
	pressed.Position = {50, 50};
	pressed.Collector = surface;
	pressed.Down = true;
	(void)router.Update(store, list, pressed);
	REQUIRE(router.CapturedCollector(store) == surface);

	gui::Screen screen;
	screen.Width = 300;
	screen.Height = 220;
	gui::Pointer windowPointer;
	windowPointer.Position = {150, 110};
	windowPointer.ScreenOnly = true;
	windowPointer.Down = true;
	const auto captured = render::RouteGuiPointer(store, list, screen, router, windowPointer);
	CHECK(captured.Collector == surface);
	CHECK_FALSE(captured.Cancelled);

	store.Remove<gui::SpatialCanvas>(surface);
	const auto cancelled = render::RouteGuiPointer(store, list, screen, router, windowPointer);
	CHECK(cancelled.Cancelled);
	CHECK_FALSE(cancelled.Inside);
	std::vector<gui::GuiEvent> events;
	studio::AppendViewportPointerEvents(router, store, list, cancelled, events);
	CHECK(router.CapturedCollector(store) == ecs::NULL_ENTITY);
	CHECK(std::none_of(events.begin(), events.end(), [](const gui::GuiEvent &event) {
		return event.Kind == gui::EventKind::Activated;
	}));
}

TEST_CASE(
	"Studio viewport clicks reach only its displayed client GUI across restart",
	"[studio][viewports][gui][input]"
) {
	using namespace engine;
	const SavedGameScratch scratch;
	struct ConfigRestore {
		std::filesystem::path Previous = studio::ConfigRoot();
		~ConfigRestore() {
			studio::SetConfigRoot(Previous);
		}
	} restore;
	studio::SetConfigRoot(scratch.Root);
	GuiContext context;
	studio::Editor editor;
	parallel::Jobs::Start(1);
	scene::RegisterSceneClasses();
	gui::RegisterGuiClasses();
	(void)script::ScriptClass();
	editor.Universe = std::make_unique<world::Universe>();
	world::WorldSettings settings;
	settings.Name = core::Name("viewport.gui.input");
	const auto authority = editor.Universe->Create(settings);
	editor.Universe->Enter(authority, [](ecs::Store &store) {
		scene::InstallServices(store);
		const auto service = gui::InstallGuiServices(store);
		REQUIRE(service != ecs::NULL_ENTITY);
		const auto child = store.CreateInstance(ecs::Classes::Find(core::Name("Folder")), "Private");
		REQUIRE(store.SetParent(child, service));
		CHECK(scene::VisibleToClients(store, service));
		CHECK_FALSE(scene::VisibleToClients(store, child));
		const std::array instances{service, child};
		std::array<uint8_t, 2> accepted{};
		scene::SelectVisibleToPlayer(store, ecs::NULL_ENTITY, instances, accepted);
		CHECK(accepted[0] == 1);
		CHECK(accepted[1] == 0);
		std::vector<scene::ClientVisibilityException> exceptions;
		scene::CollectClientVisibilityExceptions(store, exceptions);
		CHECK(std::none_of(exceptions.begin(), exceptions.end(), [&](const auto &row) {
			return row.EntityId == service.Id;
		}));
		CHECK(std::any_of(exceptions.begin(), exceptions.end(), [&](const auto &row) {
			return row.EntityId == child.Id && row.Kind == scene::ClientVisibilityExceptionKind::Hidden;
		}));
		store.DestroyInstance(child);
		const auto screen = store.CreateInstance(gui::GuiClass("ScreenGui"), "ClickHud");
		store.SetParent(screen, store.FindFirstRoot("StarterGui"));
		const auto boot = script::MakeScript(store, "viewport-input.luau", "Bootstrap", true);
		store.SetParent(boot, screen);
		store.Set(boot, script::Program{core::Name("viewport-input.luau"), R"(
local button = Instance.new('TextButton')
button.Name = 'Count'
button.Position = UDim2.new(0, 20, 0, 20)
button.Size = UDim2.new(0, 100, 0, 50)
button.Text = '0'
button.Selectable = true
button.Parent = script.Parent
local trace = Instance.new('TextLabel')
trace.Name = 'Trace'
trace.Text = ''
trace.Visible = false
trace.Parent = script.Parent
button.MouseButton1Up:Connect(function()
    trace.Text = trace.Text .. 'U'
end)
local count = 0
button.Activated:Connect(function()
    count += 1
    button.Text = tostring(count)
    trace.Text = trace.Text .. 'A'
end)
local entry = Instance.new('TextBox')
entry.Name = 'Entry'
entry.Position = UDim2.new(0, 150, 0, 20)
entry.Size = UDim2.new(0, 100, 0, 50)
entry.Parent = script.Parent
)"});
	});
	auto &run = editor.Runs.emplace_back();
	run.World = authority;
	run.Mode = studio::RunMode::Play;
	for (const auto name : {"Ada", "Grace"}) {
		auto link = std::make_unique<studio::PlayLink>();
		std::string error;
		REQUIRE(link->Start(*editor.Universe, authority, 60, error, name));
		run.Links.push_back(std::move(link));
	}
	const auto step = [&](int ticks) {
		std::array links{run.Links[0].get(), run.Links[1].get()};
		for (int tick = 0; tick < ticks; ++tick) {
			studio::PlayLink::StepMany(*editor.Universe, links);
			editor.Universe->Tick(1.0f / 60.0f);
		}
	};
	const auto textAt = [&](size_t client, const char *name) {
		std::string result;
		editor.Universe->Enter(run.Links[client]->ReplicaWorld(), [&](const ecs::Store &store) {
			const auto playerGui = store.FindFirstChild(run.Links[client]->Player(), "PlayerGui");
			ecs::Entity screen;
			store.EachChild(playerGui, [&](ecs::Entity child) {
				if (store.InstanceNameOf(child) == core::Name("ClickHud") &&
					!gui::IsPlayerGuiSource(store, child))
					screen = child;
			});
			REQUIRE(screen != ecs::NULL_ENTITY);
			const auto *label = store.Get<gui::Label>(store.FindFirstChild(screen, name));
			REQUIRE(label != nullptr);
			result = label->Text;
		});
		return result;
	};
	const auto counter = [&](size_t client) { return textAt(client, "Count"); };
	const auto frame = [&](size_t client,
						   bool down,
						   float x = 60.0f,
						   const char *text = "",
						   bool shortcuts = false,
						   bool focused = true) {
		auto &io = ImGui::GetIO();
		io.AddMousePosEvent(x, 60);
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
		io.AddInputCharactersUTF8(text);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(400, 300), ImGuiCond_Always);
		ImGui::Begin("client input");
		ImGui::SetCursorScreenPos(ImVec2(20, 20));
		ImGui::InvisibleButton("##surface", ImVec2(300, 220));
		studio::ViewportGuiControlsProbe::DrawInput(editor, run.Links[client]->ReplicaWorld(), focused);
		ImGui::End();
		if (shortcuts) studio::ViewportCameraProbe::DrawShortcuts(editor);
		ImGui::Render();
	};
	const auto click = [&](size_t client) {
		for (int idle = 0; idle < 3; ++idle)
			frame(client, false);
		frame(client, true);
		frame(client, false);
		step(2);
	};
	step(32);
	CHECK(counter(0) == "0");
	CHECK(counter(1) == "0");
	const auto selected = [&]() {
		ecs::Entity result;
		editor.Universe->Enter(run.Links[0]->ReplicaWorld(), [&](const ecs::Store &store) {
			const auto *state = store.Get<gui::GuiServiceState>(gui::GuiServiceOf(store));
			REQUIRE(state != nullptr);
			result = state->SelectedObject;
		});
		return result;
	};
	auto &keyboard = ImGui::GetIO();
	keyboard.AddKeyEvent(ImGuiKey_DownArrow, true);
	frame(0, false, 60, "", false, false);
	CHECK(selected() == ecs::NULL_ENTITY);
	keyboard.AddKeyEvent(ImGuiKey_DownArrow, false);
	frame(0, false);
	keyboard.AddKeyEvent(ImGuiKey_DownArrow, true);
	frame(0, false);
	CHECK(selected() != ecs::NULL_ENTITY);
	keyboard.AddKeyEvent(ImGuiKey_DownArrow, false);
	frame(0, false);
	keyboard.AddKeyEvent(ImGuiKey_Enter, true);
	frame(0, false);
	keyboard.AddKeyEvent(ImGuiKey_Enter, false);
	frame(0, false);
	step(2);
	CHECK(counter(0) == "1");
	CHECK(counter(1) == "0");
	editor.Universe->Enter(run.Links[0]->ReplicaWorld(), [](ecs::Store &store) {
		(void)gui::Select(store, ecs::NULL_ENTITY);
	});
	keyboard.BackendFlags |= ImGuiBackendFlags_HasGamepad;
	keyboard.AddKeyEvent(ImGuiKey_GamepadDpadDown, true);
	frame(0, false, 60, "", false, false);
	CHECK(selected() == ecs::NULL_ENTITY);
	keyboard.AddKeyEvent(ImGuiKey_GamepadDpadDown, false);
	frame(0, false);
	keyboard.AddKeyEvent(ImGuiKey_GamepadDpadDown, true);
	frame(0, false);
	CHECK(selected() != ecs::NULL_ENTITY);
	keyboard.AddKeyEvent(ImGuiKey_GamepadDpadDown, false);
	frame(0, false);
	keyboard.AddKeyEvent(ImGuiKey_GamepadFaceDown, true);
	frame(0, false);
	keyboard.AddKeyEvent(ImGuiKey_GamepadFaceDown, false);
	frame(0, false);
	step(2);
	CHECK(counter(0) == "2");
	CHECK(counter(1) == "0");
	click(0);
	CHECK(counter(0) == "3");
	CHECK(counter(1) == "0");
	click(1);
	CHECK(counter(0) == "3");
	CHECK(counter(1) == "1");
	const auto combinedActivation = [&](size_t client) {
		for (int idle = 0; idle < 3; ++idle)
			frame(client, false);
		editor.Universe->Enter(run.Links[client]->ReplicaWorld(), [&](ecs::Store &store) {
			const auto playerGui = store.FindFirstChild(run.Links[client]->Player(), "PlayerGui");
			ecs::Entity screen;
			store.EachChild(playerGui, [&](ecs::Entity child) {
				if (store.InstanceNameOf(child) == core::Name("ClickHud") &&
					!gui::IsPlayerGuiSource(store, child))
					screen = child;
			});
			REQUIRE(screen != ecs::NULL_ENTITY);
			const auto button = store.FindFirstChild(screen, "Count");
			REQUIRE(button != ecs::NULL_ENTITY);
			(void)gui::Select(store, button);
			const auto *state = store.Get<gui::GuiServiceState>(gui::GuiServiceOf(store));
			REQUIRE(state != nullptr);
			REQUIRE(state->SelectedObject == button);
		});
		const std::string previousTrace = textAt(client, "Trace");
		frame(client, true);
		keyboard.AddKeyEvent(ImGuiKey_Enter, true);
		frame(client, false);
		keyboard.AddKeyEvent(ImGuiKey_Enter, false);
		frame(client, false);
		step(2);
		CHECK(textAt(client, "Trace") == previousTrace + "UAA");
	};
	const bool previousTrickle = keyboard.ConfigInputTrickleEventQueue;
	keyboard.ConfigInputTrickleEventQueue = false;
	combinedActivation(0);
	CHECK(counter(0) == "5");
	CHECK(counter(1) == "1");
	combinedActivation(1);
	CHECK(counter(0) == "5");
	CHECK(counter(1) == "3");
	keyboard.ConfigInputTrickleEventQueue = previousTrickle;
	for (int idle = 0; idle < 3; ++idle)
		frame(0, false, 190);
	frame(0, true, 190);
	frame(0, false, 190);
	frame(0, false, 190, "hello");
	frame(0, false, 190, " caf\xC3\xA9 \xF0\x9F\x98\x80");
	editor.Universe->Enter(run.Links[0]->ReplicaWorld(), [](ecs::Store &store) {
		(void)gui::Select(store, ecs::NULL_ENTITY);
	});
	keyboard.AddKeyEvent(ImGuiKey_DownArrow, true);
	frame(0, false, 190);
	CHECK(selected() == ecs::NULL_ENTITY);
	keyboard.AddKeyEvent(ImGuiKey_DownArrow, false);
	frame(0, false, 190);
	step(2);
	for (size_t client = 0; client < 2; ++client) {
		editor.Universe->Enter(run.Links[client]->ReplicaWorld(), [&](const ecs::Store &store) {
			const auto playerGui = store.FindFirstChild(run.Links[client]->Player(), "PlayerGui");
			ecs::Entity screen;
			store.EachChild(playerGui, [&](ecs::Entity child) {
				if (store.InstanceNameOf(child) == core::Name("ClickHud") &&
					!gui::IsPlayerGuiSource(store, child))
					screen = child;
			});
			REQUIRE(screen != ecs::NULL_ENTITY);
			const auto entry = store.FindFirstChild(screen, "Entry");
			const auto *label = store.Get<gui::Label>(entry);
			REQUIRE(label != nullptr);
			CHECK(label->Text == (client == 0 ? "hello caf\xC3\xA9 \xF0\x9F\x98\x80" : ""));
			CHECK(gui::FocusedTextBox(store) == (client == 0 ? entry : ecs::NULL_ENTITY));
		});
	}
	studio::ViewportCameraProbe::PrepareCommand(editor, authority);
	studio::ViewportCameraProbe::RecordPart(editor, authority);
	const auto priorScope = studio::Keybinds::CurrentScope();
	const auto priorUndo = studio::Keybinds::Of(studio::Action::Undo);
	studio::Keybinds::Set(studio::Action::Undo, studio::Chord{ImGuiKey_Z, true, false, false});
	studio::ViewportCameraProbe::PrepareShortcuts(editor);
	frame(0, false, 190);
	auto &io = ImGui::GetIO();
	io.AddKeyEvent(ImGuiMod_Ctrl, true);
	io.AddKeyEvent(ImGuiKey_Z, true);
	frame(0, false, 190, "", true);
	io.AddKeyEvent(ImGuiKey_Z, false);
	io.AddKeyEvent(ImGuiMod_Ctrl, false);
	CHECK_FALSE(studio::ViewportCameraProbe::CanRedo(editor));
	editor.Universe->Enter(authority, [](const ecs::Store &store) {
		CHECK(store.FindFirstChild(scene::WorkspaceOf(store), "Undoable") != ecs::NULL_ENTITY);
	});
	ImGui::NewFrame();
	ImGui::Render();
	ecs::Entity hiddenBox;
	editor.Universe->Enter(run.Links[0]->ReplicaWorld(), [&](ecs::Store &store) {
		hiddenBox = gui::FocusedTextBox(store);
		REQUIRE(hiddenBox != ecs::NULL_ENTITY);
		auto *element = store.GetMutable<gui::Element>(hiddenBox);
		REQUIRE(element != nullptr);
		element->Visible = false;
	});
	io.AddKeyEvent(ImGuiKey_Escape, true);
	frame(0, false, 190);
	io.AddKeyEvent(ImGuiKey_Escape, false);
	frame(0, false, 190);
	editor.Universe->Enter(run.Links[0]->ReplicaWorld(), [&](const ecs::Store &store) {
		CHECK(gui::FocusedTextBox(store) == hiddenBox);
	});
	io.AddKeyEvent(ImGuiMod_Ctrl, true);
	io.AddKeyEvent(ImGuiKey_Z, true);
	frame(0, false, 190, "x", true);
	io.AddKeyEvent(ImGuiKey_Z, false);
	io.AddKeyEvent(ImGuiMod_Ctrl, false);
	CHECK(studio::ViewportCameraProbe::CanRedo(editor));
	editor.Universe->Enter(run.Links[0]->ReplicaWorld(), [&](const ecs::Store &store) {
		const auto *label = store.Get<gui::Label>(hiddenBox);
		REQUIRE(label != nullptr);
		CHECK(label->Text == "hello caf\xC3\xA9 \xF0\x9F\x98\x80");
	});
	studio::Keybinds::Set(studio::Action::Undo, priorUndo);
	studio::Keybinds::SetScope(priorScope);
	editor.Universe->Enter(authority, [](const ecs::Store &store) {
		const auto source = store.FindFirstChild(store.FindFirstRoot("StarterGui"), "ClickHud");
		CHECK(store.FindFirstChild(source, "Count") == ecs::NULL_ENTITY);
	});
	run.Links[0]->Stop(*editor.Universe);
	CHECK_FALSE(run.Links[0]->DeliverGuiEvents({}));
	std::string error;
	REQUIRE(run.Links[0]->Start(*editor.Universe, authority, 60, error, "Ada"));
	step(32);
	CHECK(counter(0) == "0");
	click(0);
	CHECK(counter(0) == "1");
	CHECK(counter(1) == "3");
	for (auto &link : run.Links) {
		link->Stop(*editor.Universe);
		CHECK_FALSE(link->DeliverGuiEvents({}));
	}
	editor.Active = authority;
	editor.Runs.clear();
}

TEST_CASE("a floating panel blocks viewport GUI wheel input", "[studio][viewports][gui][input]") {
	using namespace engine;
	GuiContext context;
	studio::Editor editor;
	scene::RegisterSceneClasses();
	gui::RegisterGuiClasses();
	editor.Universe = std::make_unique<world::Universe>();
	world::WorldSettings settings;
	settings.Name = core::Name("viewport.gui.occlusion");
	const auto world = editor.Universe->Create(settings);
	ecs::Entity frame;
	editor.Universe->Enter(world, [&](ecs::Store &store) {
		scene::InstallServices(store);
		const auto screen = store.CreateInstance(gui::GuiClass("ScreenGui"), "Screen");
		REQUIRE(store.SetParent(screen, store.FindFirstRoot("StarterGui")));
		frame = store.CreateInstance(gui::GuiClass("ScrollingFrame"), "Scroll");
		REQUIRE(store.SetParent(frame, screen));
	});

	const auto draw = [&](int cover, bool nested, float wheel) {
		auto &io = ImGui::GetIO();
		io.AddMousePosEvent(60, 60);
		if (wheel != 0.0f) io.AddMouseWheelEvent(0.0f, wheel);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(400, 300), ImGuiCond_Always);
		ImGui::Begin("viewport");
		ImGui::SetCursorScreenPos(ImVec2(20, 20));
		if (nested) ImGui::BeginChild("viewport child", ImVec2(300, 220));
		ImGui::InvisibleButton("##surface", ImVec2(300, 220));
		studio::ViewportGuiControlsProbe::DrawInput(editor, world);
		if (nested) ImGui::EndChild();
		if (cover == 2) {
			ImGui::SetCursorScreenPos(ImVec2(30, 30));
			ImGui::BeginChild(
				"panel child", ImVec2(160, 160), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollWithMouse
			);
			ImGui::TextUnformatted("Cover");
			ImGui::EndChild();
		}
		ImGui::End();
		if (cover == 1) {
			ImGui::SetNextWindowPos(ImVec2(30, 30), ImGuiCond_Always);
			ImGui::SetNextWindowSize(ImVec2(160, 160), ImGuiCond_Always);
			ImGui::Begin("floating panel", nullptr, ImGuiWindowFlags_NoScrollWithMouse);
			ImGui::TextUnformatted("Cover");
			ImGui::End();
		}
		ImGui::Render();
	};
	const auto position = [&] {
		float result = 0.0f;
		editor.Universe->Enter(world, [&](const ecs::Store &store) {
			const auto *scroll = store.Get<gui::Scrolling>(frame);
			REQUIRE(scroll != nullptr);
			result = scroll->CanvasPosition.Y;
		});
		return result;
	};
	for (int idle = 0; idle < 3; ++idle)
		draw(0, false, 0.0f);
	draw(0, false, -1.0f);
	REQUIRE(position() > 0.0f);
	const float before = position();
	for (int idle = 0; idle < 3; ++idle)
		draw(1, false, 0.0f);
	draw(1, false, -1.0f);
	CHECK(position() == before);
	for (int idle = 0; idle < 3; ++idle)
		draw(2, false, 0.0f);
	draw(2, false, -1.0f);
	CHECK(position() == before);
	editor.Universe->Enter(world, [&](ecs::Store &store) {
		auto *scroll = store.GetMutable<gui::Scrolling>(frame);
		REQUIRE(scroll != nullptr);
		scroll->CanvasPosition.Y = 0.0f;
	});
	for (int idle = 0; idle < 3; ++idle)
		draw(0, true, 0.0f);
	draw(0, true, -1.0f);
	CHECK(position() > 0.0f);
}

TEST_CASE(
	"typing in a viewport text box does not drive or frame the editor camera",
	"[studio][viewports][gui][input]"
) {
	using namespace engine;
	GuiContext context;
	studio::Editor editor;
	scene::RegisterSceneClasses();
	gui::RegisterGuiClasses();
	editor.Universe = std::make_unique<world::Universe>();
	world::WorldSettings settings;
	settings.Name = core::Name("viewport.typing.camera");
	const auto world = editor.Universe->Create(settings);
	ecs::Entity part;
	ecs::Entity box;
	editor.Universe->Enter(world, [&](ecs::Store &store) {
		scene::InstallServices(store);
		gui::InstallGuiServices(store);
		part = store.CreateInstance(scene::PartClass(), "Target");
		REQUIRE(store.SetParent(part, scene::WorkspaceOf(store)));
		const auto screen = store.CreateInstance(gui::GuiClass("ScreenGui"), "Screen");
		REQUIRE(store.SetParent(screen, store.FindFirstRoot("StarterGui")));
		box = store.CreateInstance(gui::GuiClass("TextBox"), "Entry");
		REQUIRE(store.SetParent(box, screen));
		REQUIRE(gui::Focus(store, box));
	});
	studio::ViewportCameraProbe::PrepareTypingCamera(editor, world, part);
	const auto frame = [&](const char *text) {
		ImGui::GetIO().AddMousePosEvent(60, 60);
		ImGui::GetIO().AddInputCharactersUTF8(text);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(400, 300), ImGuiCond_Always);
		ImGui::Begin("authoring input");
		ImGui::SetCursorScreenPos(ImVec2(20, 20));
		ImGui::InvisibleButton("##surface", ImVec2(300, 220));
		studio::ViewportGuiControlsProbe::DrawInput(editor, world);
		ImGui::End();
		studio::ViewportCameraProbe::Drive(editor);
		const bool captured = studio::ViewportCameraProbe::TextInputCaptured(editor);
		ImGui::Render();
		return captured;
	};
	REQUIRE(frame(""));
	auto &io = ImGui::GetIO();
	const auto beforeW = studio::ViewportCameraProbe::Camera(editor).Position;
	io.AddKeyEvent(ImGuiKey_W, true);
	CHECK(frame("w"));
	CHECK(studio::ViewportCameraProbe::Camera(editor).Position == beforeW);
	io.AddKeyEvent(ImGuiKey_W, false);
	frame("");
	const auto beforeF = studio::ViewportCameraProbe::Camera(editor).Position;
	io.AddKeyEvent(ImGuiKey_F, true);
	CHECK(frame("f"));
	CHECK(studio::ViewportCameraProbe::Camera(editor).Position == beforeF);
	io.AddKeyEvent(ImGuiKey_F, false);
	frame("");
	editor.Universe->Enter(world, [&](ecs::Store &store) {
		const auto *label = store.Get<gui::Label>(box);
		REQUIRE(label != nullptr);
		CHECK(label->Text == "wf");
		REQUIRE(gui::Focus(store, ecs::NULL_ENTITY));
	});
	const auto beforeControl = studio::ViewportCameraProbe::Camera(editor).Position;
	io.AddKeyEvent(ImGuiKey_W, true);
	CHECK_FALSE(frame(""));
	CHECK(studio::ViewportCameraProbe::Camera(editor).Position != beforeControl);
	io.AddKeyEvent(ImGuiKey_W, false);
	frame("");
	const auto beforeFrameControl = studio::ViewportCameraProbe::Camera(editor).Position;
	io.AddKeyEvent(ImGuiKey_F, true);
	CHECK_FALSE(frame(""));
	CHECK(studio::ViewportCameraProbe::Camera(editor).Position != beforeFrameControl);
	io.AddKeyEvent(ImGuiKey_F, false);
	frame("");
}

TEST_CASE(
	"game UI uses each panel's logical size instead of its GPU allocation", "[studio][viewports][gui]"
) {
	const studio::ViewportCanvas left = CanvasForViewport(12.0f, 30.0f, 841.0f, 674.0f, 432.0f, 367.0f);
	CHECK(left.Width == 841.0f);
	CHECK(left.Height == 674.0f);
	CHECK(left.PointerX == 420.0f);
	CHECK(left.PointerY == 337.0f);

	const studio::ViewportCanvas right = CanvasForViewport(853.0f, 30.0f, 729.0f, 674.0f, 1217.5f, 367.0f);
	CHECK(right.Width == 729.0f);
	CHECK(right.Height == 674.0f);
	CHECK(right.PointerX == 364.5f);
	CHECK(right.PointerY == 337.0f);
}

TEST_CASE("each viewport has an explicit game UI owner", "[studio][viewports][gui]") {
	// Two edit viewports may both author the template. A client viewport owns
	// its PlayerGui, while the running server view has neither UI nor input.
	CHECK(ViewportGuiSourceFor(false, false) == ViewportGuiSource::StarterGui);
	CHECK(ViewportGuiSourceFor(false, true) == ViewportGuiSource::PlayerGui);
	CHECK(ViewportGuiSourceFor(true, false) == ViewportGuiSource::None);
	CHECK(ViewportGuiSourceFor(true, true) == ViewportGuiSource::PlayerGui);
}

TEST_CASE("runtime worlds keep separate generated cameras", "[studio][viewports][camera]") {
	engine::scene::RegisterSceneClasses();
	engine::ecs::Store server("viewport_runtime_server");
	engine::ecs::Store client("viewport_runtime_client");
	engine::scene::InstallServices(server);
	engine::scene::InstallServices(client);

	ViewportCameraPose serverPose = DefaultViewportCamera();
	serverPose.Frame.Position = {10.0f, 20.0f, 30.0f};
	ViewportCameraPose clientPose = DefaultViewportCamera();
	clientPose.Frame.Position = {-10.0f, 4.0f, 8.0f};

	const engine::ecs::Entity serverCamera = CreateRuntimeCamera(server, serverPose);
	const engine::ecs::Entity clientCamera = CreateRuntimeCamera(client, clientPose);
	REQUIRE(serverCamera != engine::ecs::NULL_ENTITY);
	REQUIRE(clientCamera != engine::ecs::NULL_ENTITY);
	CHECK(RuntimeCameraOf(server) == serverCamera);
	CHECK(RuntimeCameraOf(client) == clientCamera);
	CHECK(server.Get<engine::scene::Transform>(serverCamera)->Frame.Position == serverPose.Frame.Position);
	CHECK(client.Get<engine::scene::Transform>(clientCamera)->Frame.Position == clientPose.Frame.Position);
	CHECK(server.Get<engine::scene::TransientComponent>(serverCamera) != nullptr);
	CHECK(client.Get<engine::scene::TransientComponent>(clientCamera) != nullptr);
	CHECK(engine::ecs::Store::IsPredicted(serverCamera));
	CHECK(engine::ecs::Store::IsPredicted(clientCamera));
	CHECK(server.InstanceNameOf(serverCamera) == engine::core::Name("Camera"));
	CHECK(client.InstanceNameOf(clientCamera) == engine::core::Name("Camera"));
	CHECK(server.Has<engine::ecs::ClientLocal>(serverCamera));
	CHECK(client.Has<engine::ecs::ClientLocal>(clientCamera));
}

TEST_CASE("editor viewports share their world's camera", "[studio][viewports][camera]") {
	engine::scene::RegisterSceneClasses();
	studio::Editor editor;
	editor.Universe = std::make_unique<engine::world::Universe>();
	const WorldId world = editor.Universe->Create({.Name = engine::core::Name("ViewportCameraWorld")});
	const engine::core::CFrame authored(engine::core::Vector3{42.0f, 7.0f, -5.0f});
	editor.Universe->Enter(world, [&](engine::ecs::Store &store) {
		engine::scene::InstallServices(store);
		store.GetMutable<engine::scene::Transform>(engine::scene::WorldCameraOf(store))->Frame = authored;
	});

	const auto cameras = studio::ViewportCameraProbe::CreateTwo(editor, world);
	REQUIRE(cameras[0] != engine::ecs::NULL_ENTITY);
	REQUIRE(cameras[1] != engine::ecs::NULL_ENTITY);
	CHECK(cameras[0] == cameras[1]);
	studio::ViewportCameraProbe::SetFieldOfView(editor, 0, 0.7f);
	CHECK(studio::ViewportCameraProbe::CreateTwo(editor, world) == cameras);
	const auto child =
		studio::ViewportCameraProbe::Insert(editor, world, engine::scene::PartClass(), cameras[0]);
	REQUIRE(child != engine::ecs::NULL_ENTITY);
	editor.Universe->Enter(world, [&](const engine::ecs::Store &store) {
		CHECK_FALSE(engine::ecs::Store::IsPredicted(child));
		CHECK_FALSE(store.Has<engine::ecs::ClientLocal>(child));
		CHECK(store.ParentOf(child) == cameras[0]);
		CHECK(store.Get<engine::scene::Camera>(cameras[0])->FieldOfViewRadians == 0.7f);
		CHECK(store.Get<engine::scene::Transform>(cameras[0])->Frame.Position == authored.Position);
		for (const auto camera : cameras) {
			CHECK_FALSE(engine::ecs::Store::IsPredicted(camera));
			CHECK(store.InstanceNameOf(camera) == engine::core::Name("Camera"));
			CHECK_FALSE(store.Has<engine::ecs::ClientLocal>(camera));
			CHECK_FALSE(store.Has<engine::scene::TransientComponent>(camera));
		}
	});
	studio::ViewportCameraProbe::Release(editor, 0);
	studio::ViewportCameraProbe::Release(editor, 1);
	editor.Universe->Enter(world, [&](const engine::ecs::Store &store) {
		CHECK(store.Alive(cameras[0]));
		CHECK(store.Resource<engine::scene::ActiveCamera>()->Entity == cameras[0]);
	});
	engine::scene::Camera borrowedLens;
	borrowedLens.FieldOfViewRadians = 1.1f;
	const auto borrowed = studio::ViewportCameraProbe::Borrow(editor, world, borrowedLens);
	REQUIRE(borrowed != engine::ecs::NULL_ENTITY);
	CHECK(borrowed != cameras[0]);
	editor.Universe->Enter(world, [&](const engine::ecs::Store &store) {
		CHECK(store.ParentOf(borrowed) == engine::ecs::NULL_ENTITY);
		CHECK(store.Get<engine::scene::Camera>(borrowed)->FieldOfViewRadians == 1.1f);
		CHECK(store.Get<engine::scene::Camera>(cameras[0])->FieldOfViewRadians == 0.7f);
	});
	studio::ViewportCameraProbe::Release(editor, 0);
	editor.Universe->Enter(world, [&](const engine::ecs::Store &store) {
		CHECK_FALSE(store.Alive(borrowed));
		CHECK(store.Resource<engine::scene::ActiveCamera>()->Entity == cameras[0]);
	});

	studio::ViewportCameraProbe::SetFollows(editor, cameras[0], cameras[1]);
	CHECK(studio::ViewportCameraProbe::Follows(editor) == cameras);
}

TEST_CASE(
	"authored field of view reaches each viewport render projection",
	"[studio][viewports][camera][render][gpu]"
) {
	studio::Editor editor;
	REQUIRE(studio::ViewportCameraProbe::Initialise(editor, 2));

	// Both panels project through the same authored world camera.
	studio::ViewportCameraProbe::Present(editor, 0);
	studio::ViewportCameraProbe::Present(editor, 1);
	studio::ViewportCameraProbe::SetFieldOfView(editor, 0, 0.61f);
	studio::ViewportCameraProbe::SetFieldOfView(editor, 1, 1.19f);

	studio::ViewportCameraProbe::Present(editor, 0);
	studio::ViewportCameraProbe::Present(editor, 1);

	CHECK(studio::ViewportCameraProbe::PresentedFieldOfView(editor, 0) == 1.19f);
	CHECK(studio::ViewportCameraProbe::PresentedFieldOfView(editor, 1) == 1.19f);
}

TEST_CASE(
	"frustum lock freezes camera behavior while preserving free inspection projection",
	"[studio][viewports][camera][render][gpu]"
) {
	studio::Editor editor;
	REQUIRE(studio::ViewportCameraProbe::Initialise(editor, 1));
	studio::ViewportCameraProbe::Present(editor, 0);
	const auto authored = studio::ViewportCameraProbe::AuthoredCameraFrame(editor, 0);

	const engine::core::CFrame locked(
		engine::core::Vector3{2.0f, 3.0f, 4.0f}, engine::core::CFrame::Angles(0.2f, 0.4f, 0.1f).Rotation()
	);
	const engine::core::CFrame inspection(
		engine::core::Vector3{9.0f, 8.0f, 7.0f}, engine::core::CFrame::Angles(-0.3f, 1.1f, 0.0f).Rotation()
	);
	studio::ViewportCameraProbe::SetInspectionFrame(editor, inspection);
	editor.MainViewportDiagnostics.LockFrustum(locked);
	studio::ViewportCameraProbe::Present(editor, 0);

	const engine::core::CFrame rendered = studio::ViewportCameraProbe::PresentedFrame(editor, 0);
	CHECK(rendered.Position == inspection.Position);
	CHECK(rendered.LookVector() == inspection.LookVector());
	const engine::core::CFrame behaviour = studio::ViewportCameraProbe::ViewerFrame(editor, 0);
	CHECK(behaviour.Position == locked.Position);
	CHECK(behaviour.LookVector() == inspection.LookVector());
	CHECK(studio::ViewportCameraProbe::AuthoredCameraFrame(editor, 0).Position == authored.Position);

	studio::ViewportCameraProbe::SetOverlayRectangle(editor, 0, 40.0f, 20.0f, 800.0f, 400.0f, 1600, 800);
	const studio::PanelProjection projection = studio::ViewportCameraProbe::Projection(editor, 0);
	CHECK(projection.Eye == inspection.Position);

	const engine::core::CFrame unlocked(
		engine::core::Vector3{-5.0f, 1.0f, 12.0f}, engine::core::CFrame::Angles(0.1f, -0.7f, 0.0f).Rotation()
	);
	studio::ViewportCameraProbe::SetInspectionFrame(editor, unlocked);
	editor.MainViewportDiagnostics.FrustumLocked = false;
	studio::ViewportCameraProbe::Present(editor, 0);
	CHECK(studio::ViewportCameraProbe::PresentedFrame(editor, 0).Position == unlocked.Position);
	CHECK(studio::ViewportCameraProbe::ViewerFrame(editor, 0).Position == unlocked.Position);
	CHECK(studio::ViewportCameraProbe::AuthoredCameraFrame(editor, 0).Position == authored.Position);
}

TEST_CASE(
	"routed virtual camera position drives remote eye demand while inspection stays free",
	"[studio][viewports][camera][portal-demand]"
) {
	using namespace engine;
	studio::ViewportDiagnostics diagnostics;
	const core::CFrame locked(
		core::Vector3{2.0f, 3.0f, 4.0f}, core::CFrame::Angles(.2f, .4f, .1f).Rotation()
	);
	const core::CFrame inspection(
		core::Vector3{90.0f, -12.0f, 45.0f}, core::CFrame::Angles(-.3f, 1.1f, -.2f).Rotation()
	);
	diagnostics.LockFrustum(locked);

	scene::SeamTransform route;
	route.Frame = core::CFrame(core::Vector3{40.0f, 5.0f, -7.0f}) * core::CFrame::Angles(.1f, .8f, .2f);
	const core::CFrame behaviour = diagnostics.ResolveBehaviourFrame(inspection, [&](core::CFrame &frame) {
		frame = route.Place(frame);
	});
	CHECK((inspection.Position == core::Vector3{90.0f, -12.0f, 45.0f}));
	CHECK(behaviour.Position == route.Point(locked.Position));
	CHECK(behaviour.LookVector().Dot(route.Rotate(inspection.LookVector())) > .99999f);

	render::View remote;
	diagnostics.ApplyCameraFrames(remote, inspection, behaviour);
	CHECK(remote.VisibilityFrame.has_value());
	CHECK(remote.VisibilityCameraFrame().Position == behaviour.Position);
	CHECK(remote.VisibilityCameraFrame().QuaternionX == behaviour.QuaternionX);
	CHECK(remote.VisibilityCameraFrame().QuaternionY == behaviour.QuaternionY);
	CHECK(remote.VisibilityCameraFrame().QuaternionZ == behaviour.QuaternionZ);
	CHECK(remote.VisibilityCameraFrame().QuaternionW == behaviour.QuaternionW);
	render::PortalImageDemand demand;
	REQUIRE(
		render::BuildPortalEyeDemand(
			core::Name("viewport-eye"), remote, {.Width = 64, .Height = 64}, demand
		) == render::PortalDemandStatus::Ready
	);
	CHECK(demand.Request.Position[0] == behaviour.Position.X);
	CHECK(demand.Request.Position[1] == behaviour.Position.Y);
	CHECK(demand.Request.Position[2] == behaviour.Position.Z);
	CHECK(remote.CameraFrame.Position == inspection.Position);
	CHECK(remote.CameraFrame.LookVector() == inspection.LookVector());
	CHECK(demand.Request.Orientation[0] == behaviour.Rotation().x);
	CHECK(demand.Request.Orientation[1] == behaviour.Rotation().y);
	CHECK(demand.Request.Orientation[2] == behaviour.Rotation().z);
	CHECK(demand.Request.Orientation[3] == behaviour.Rotation().w);

	const core::CFrame unlocked(
		core::Vector3{-5.0f, 1.0f, 12.0f}, core::CFrame::Angles(.1f, -.7f, 0.0f).Rotation()
	);
	diagnostics.FrustumLocked = false;
	const core::CFrame liveBehaviour =
		diagnostics.ResolveBehaviourFrame(unlocked, [&](core::CFrame &frame) { frame = route.Place(frame); });
	CHECK(liveBehaviour.Position == route.Point(unlocked.Position));
	render::View liveRemote;
	diagnostics.ApplyCameraFrames(liveRemote, unlocked, liveBehaviour);
	CHECK(liveRemote.VisibilityFrame.has_value());
	CHECK(liveRemote.VisibilityCameraFrame().Position == liveBehaviour.Position);
	CHECK(liveRemote.VisibilityCameraFrame().QuaternionX == liveBehaviour.QuaternionX);
	CHECK(liveRemote.VisibilityCameraFrame().QuaternionY == liveBehaviour.QuaternionY);
	CHECK(liveRemote.VisibilityCameraFrame().QuaternionZ == liveBehaviour.QuaternionZ);
	CHECK(liveRemote.VisibilityCameraFrame().QuaternionW == liveBehaviour.QuaternionW);
	render::PortalImageDemand liveDemand;
	REQUIRE(
		render::BuildPortalEyeDemand(
			core::Name("viewport-eye"), liveRemote, {.Width = 64, .Height = 64}, liveDemand
		) == render::PortalDemandStatus::Ready
	);
	CHECK(liveDemand.Request.Position[0] == liveBehaviour.Position.X);
	CHECK(liveDemand.Request.Position[1] == liveBehaviour.Position.Y);
	CHECK(liveDemand.Request.Position[2] == liveBehaviour.Position.Z);
	CHECK(liveRemote.CameraFrame.Position == unlocked.Position);
	CHECK(liveRemote.CameraFrame.LookVector() == unlocked.LookVector());

	render::View ordinary;
	diagnostics.ApplyCameraFrames(ordinary, unlocked, unlocked);
	CHECK_FALSE(ordinary.VisibilityFrame.has_value());
	CHECK(ordinary.VisibilityCameraFrame().Position == unlocked.Position);
	CHECK(ordinary.CameraFrame.QuaternionX == unlocked.QuaternionX);
	CHECK(ordinary.CameraFrame.QuaternionY == unlocked.QuaternionY);
	CHECK(ordinary.CameraFrame.QuaternionZ == unlocked.QuaternionZ);
	CHECK(ordinary.CameraFrame.QuaternionW == unlocked.QuaternionW);
}

TEST_CASE(
	"staged engine textures are resident in Studio and its Play client",
	"[studio][viewports][content][render][gpu]"
) {
	studio::Editor editor;
	REQUIRE(studio::ViewportCameraProbe::Initialise(editor, 1));
	CHECK(studio::ViewportCameraProbe::HasPackagedFlipbook(editor));

	const engine::scene::FlipbookFacts author = studio::ViewportCameraProbe::Flipbook(editor, editor.Active);
	CHECK(author.Side == 8);
	CHECK(author.Frames == 64);
	CHECK(author.FrameRate > 23.0f);
	CHECK(author.FrameRate < 25.0f);

	const WorldId replica = studio::ViewportCameraProbe::StartPlay(editor);
	const engine::scene::FlipbookFacts client = studio::ViewportCameraProbe::Flipbook(editor, replica);
	CHECK(client.Side == 8);
	CHECK(client.Frames == 64);
	CHECK(client.FrameRate > 23.0f);
	CHECK(client.FrameRate < 25.0f);
}

TEST_CASE(
	"Studio first person hides only the displayed player's body",
	"[studio][viewports][camera][first-person-body][gpu][.]"
) {
	using namespace engine;
	const SavedGameScratch scratch;
	struct ConfigRestore {
		std::filesystem::path Previous = studio::ConfigRoot();
		~ConfigRestore() {
			studio::SetConfigRoot(Previous);
		}
	} restore;
	studio::SetConfigRoot(scratch.Root);
	studio::Editor editor;
	REQUIRE(studio::ViewportCameraProbe::Initialise(editor, 1));
	const auto authority = editor.Universe->Create({.Name = core::Name("FirstPersonBody")});
	editor.PrepareWorldIn(authority);
	editor.Universe->Enter(authority, [&](ecs::Store &store) {
		const auto observer = scene::AddPlayer(store, "observer", false, 91);
		REQUIRE(observer != ecs::NULL_ENTITY);
		const auto model = scene::LoadCharacter(store, observer);
		const auto *character = store.Get<scene::Character>(model);
		REQUIRE(character != nullptr);
		store.Set(character->Root, scene::Transform{core::CFrame(core::Vector3{0, 0, -3})});
	});
	studio::ViewportCameraProbe::Show(editor, authority);
	const WorldId replica = studio::ViewportCameraProbe::StartPlay(editor);
	studio::ViewportCameraProbe::TickPlay(editor);
	studio::ViewportCameraProbe::Show(editor, replica);
	std::vector<uint64_t> body;
	std::vector<uint64_t> otherBody;
	const auto setMode = [&](scene::CameraMode mode) {
		editor.Universe->Enter(replica, [&](ecs::Store &store) {
			(void)scene::FollowOwnCharacter(store);
			const auto *local = store.Resource<scene::LocalPlayer>();
			REQUIRE(local != nullptr);
			const auto *character = store.Get<scene::Character>(scene::CharacterOf(store, local->Instance));
			REQUIRE(character != nullptr);
			const auto *active = store.Resource<scene::ActiveCamera>();
			REQUIRE(active != nullptr);
			REQUIRE(scene::CameraSubjectRoot(store, active->Entity) == character->Root);
			auto *control = store.ResourceMutable<scene::CameraController>();
			REQUIRE(control != nullptr);
			control->Angles = {-.8f, 0};
			control->Mode = mode;
			control->Distance = mode == scene::CameraMode::LockFirstPerson ? 0 : 5;
			control->OccludedDistance = -1;
			REQUIRE(scene::PlaceCamera(store));
		});
	};
	setMode(scene::CameraMode::Classic);
	const auto classic = studio::ViewportCameraProbe::BodyVisibility(editor);
	REQUIRE(classic.Valid);
	editor.Universe->Enter(replica, [&](const ecs::Store &store) {
		const auto *active = store.Resource<scene::ActiveCamera>();
		REQUIRE(active != nullptr);
		const auto root = scene::CameraSubjectRoot(store, active->Entity);
		REQUIRE(root != ecs::NULL_ENTITY);
		const auto *drawn = store.Resource<render::DrawList>();
		REQUIRE(drawn != nullptr);
		const auto observer = store.FindFirstChild(scene::PlayersOf(store), "observer");
		const auto *other = store.Get<scene::Character>(scene::CharacterOf(store, observer));
		REQUIRE(other != nullptr);
		for (const auto &row : drawn->Instances) {
			if (row.Variant != 0) continue;
			if (row.Rig == root.Id || row.Source == root.Id) body.push_back(row.Source);
			if (row.Rig == other->Root.Id || row.Source == other->Root.Id) otherBody.push_back(row.Source);
		}
	});
	REQUIRE_FALSE(body.empty());
	REQUIRE_FALSE(otherBody.empty());
	const auto submittedBody = [&](const render::VisibilitySnapshot &snapshot,
								   const std::vector<uint64_t> &entities) {
		size_t count = 0;
		for (const auto &row : snapshot.Observations) {
			if (std::find(entities.begin(), entities.end(), row.Entity) == entities.end()) continue;
			if (row.State == render::VisibilityState::SubmittedOpaqueOrMasked ||
				row.State == render::VisibilityState::SubmittedBlended ||
				row.State == render::VisibilityState::GpuIndirectCandidate)
				++count;
		}
		return count;
	};
	CHECK(submittedBody(classic, body) > 0);
	setMode(scene::CameraMode::LockFirstPerson);
	const auto first = studio::ViewportCameraProbe::BodyVisibility(editor);
	REQUIRE(first.Valid);
	CHECK(submittedBody(first, body) == 0);
	CHECK(submittedBody(first, otherBody) > 0);
	setMode(scene::CameraMode::Classic);
	const auto restored = studio::ViewportCameraProbe::BodyVisibility(editor);
	REQUIRE(restored.Valid);
	CHECK(submittedBody(restored, body) > 0);
}

TEST_CASE(
	"overlay projection stays with the camera that made its texture",
	"[studio][viewports][camera][render][gpu]"
) {
	studio::Editor editor;
	REQUIRE(studio::ViewportCameraProbe::Initialise(editor, 1));
	studio::ViewportCameraProbe::Present(editor, 0);
	studio::ViewportCameraProbe::SetOverlayRectangle(editor, 0, 100.0f, 50.0f, 800.0f, 400.0f, 1600, 800);

	const studio::PanelProjection recorded = studio::ViewportCameraProbe::Projection(editor, 0);
	glm::vec2 recordedPoint{};
	REQUIRE(recorded.WorldToPanel({1.0f, 0.0f, -10.0f}, recordedPoint));
	CHECK(recorded.ImageMin.x == 100.0f);
	CHECK(recorded.ImageMin.y == 50.0f);
	CHECK(recorded.ImageSize.x == 800.0f);
	CHECK(recorded.ImageSize.y == 400.0f);
	CHECK(recorded.RenderSize.x == 1600.0f);
	CHECK(recorded.RenderSize.y == 800.0f);

	studio::ViewportCameraProbe::SetViewerCamera(
		editor, 0, engine::core::CFrame(engine::core::Vector3{4.0f, 0.0f, 0.0f}), 0.61f
	);
	const studio::PanelProjection overlay = studio::ViewportCameraProbe::Projection(editor, 0);
	glm::vec2 overlayPoint{};
	REQUIRE(overlay.WorldToPanel({1.0f, 0.0f, -10.0f}, overlayPoint));

	CHECK(overlay.Eye == recorded.Eye);
	CHECK(overlayPoint.x == recordedPoint.x);
	CHECK(overlayPoint.y == recordedPoint.y);
}

TEST_CASE(
	"a replica viewport retains its local camera field of view", "[studio][viewports][camera][render][gpu]"
) {
	studio::Editor editor;
	REQUIRE(studio::ViewportCameraProbe::Initialise(editor, 1));
	const WorldId replica = studio::ViewportCameraProbe::StartPlay(editor);
	studio::ViewportCameraProbe::Show(editor, replica);

	// Presenting a client view creates an authority viewer for its surface
	// cameras. Its lens must not replace the replica's runtime camera lens when
	// the authority is the visual world.
	studio::ViewportCameraProbe::Present(editor, 0);
	studio::ViewportCameraProbe::SetReplicaFieldOfView(editor, replica, 0.61f);
	studio::ViewportCameraProbe::SetFieldOfView(editor, 0, 1.19f);

	studio::ViewportCameraProbe::Present(editor, 0);

	CHECK(studio::ViewportCameraProbe::PresentedFieldOfView(editor, 0) == 0.61f);
	CHECK(studio::ViewportCameraProbe::PresentedWorld(editor, 0) == replica);

	// The client image remains on screen until its next round-robin turn. Its
	// overlay must keep the replica camera that produced that image, even after
	// the live replica camera has moved and changed its lens.
	studio::ViewportCameraProbe::SetOverlayRectangle(editor, 0, 40.0f, 20.0f, 800.0f, 400.0f, 1600, 800);
	const studio::PanelProjection recorded = studio::ViewportCameraProbe::Projection(editor, 0);
	glm::vec2 recordedPoint{};
	REQUIRE(recorded.WorldToPanel({1.0f, 0.0f, -10.0f}, recordedPoint));

	studio::ViewportCameraProbe::SetReplicaCamera(
		editor, replica, engine::core::CFrame(engine::core::Vector3{4.0f, 0.0f, 0.0f}), 1.19f
	);
	const studio::PanelProjection overlay = studio::ViewportCameraProbe::Projection(editor, 0);
	glm::vec2 overlayPoint{};
	REQUIRE(overlay.WorldToPanel({1.0f, 0.0f, -10.0f}, overlayPoint));
	CHECK(overlay.Eye == recorded.Eye);
	CHECK(overlayPoint.x == recordedPoint.x);
	CHECK(overlayPoint.y == recordedPoint.y);
}

TEST_CASE("viewport target ceilings preserve the panel aspect", "[studio][viewports][render]") {
	const studio::ViewportTargetSize wide = ResolveViewportTargetSize(3200, 1200, 0, 0, 1920, 1080);
	CHECK(wide.Width == 1920);
	CHECK(wide.Height == 720);

	const studio::ViewportTargetSize tall = ResolveViewportTargetSize(900, 2400, 0, 0, 1920, 1080);
	CHECK(tall.Width == 405);
	CHECK(tall.Height == 1080);

	const studio::ViewportTargetSize unbounded =
		ResolveViewportTargetSize(UINT32_MAX, UINT32_MAX, 0, 0, 0, 0);
	CHECK(unbounded.Width == UINT32_MAX);
	CHECK(unbounded.Height == UINT32_MAX);
}

TEST_CASE("wireframe toggle sits below the viewport direction gizmo", "[studio][viewports][overlay]") {
	const studio::ViewportDirectionControls controls =
		ResolveViewportDirectionControls(100.0f, 50.0f, 800.0f, 1.0f);

	CHECK(controls.CentreX == 852.0f);
	CHECK(controls.CentreY == 98.0f);
	CHECK(controls.WireframeTop > controls.CentreY + controls.Radius);
	CHECK((controls.WireframeLeft + controls.WireframeRight) * 0.5f == controls.CentreX);
	CHECK(controls.WireframeRight - controls.WireframeLeft == 88.0f);
	CHECK(controls.WireframeBottom - controls.WireframeTop == 24.0f);
	CHECK(controls.WireframeContains(controls.CentreX, controls.WireframeTop));
	CHECK_FALSE(controls.WireframeContains(controls.WireframeRight + 1.0f, controls.WireframeTop));
}

TEST_CASE("wireframe toggle follows Studio interface scale", "[studio][viewports][overlay]") {
	const studio::ViewportDirectionControls controls =
		ResolveViewportDirectionControls(0.0f, 0.0f, 1000.0f, 2.0f);

	CHECK(controls.Radius == 56.0f);
	CHECK(controls.WireframeRight - controls.WireframeLeft == 176.0f);
	CHECK(controls.WireframeBottom - controls.WireframeTop == 48.0f);
}

TEST_CASE("crossing either viewport ceiling scales both axes together", "[studio][viewports][render]") {
	const studio::ViewportTargetSize horizontal = ResolveViewportTargetSize(2400, 720, 0, 0, 1920, 1080);
	CHECK(horizontal.Width == 1920);
	CHECK(horizontal.Height == 576);

	const studio::ViewportTargetSize vertical = ResolveViewportTargetSize(720, 1350, 0, 0, 1920, 1080);
	CHECK(vertical.Width == 576);
	CHECK(vertical.Height == 1080);
}

TEST_CASE("explicit camera image sizes remain one bounded pair", "[studio][viewports][render]") {
	const studio::ViewportTargetSize capture = ResolveViewportTargetSize(800, 600, 4000, 1000, 1920, 1080);
	CHECK(capture.Width == 1920);
	CHECK(capture.Height == 480);

	// An incomplete override is ignored as a pair, matching Camera's contract.
	const studio::ViewportTargetSize incomplete = ResolveViewportTargetSize(800, 600, 4000, 0, 1920, 1080);
	CHECK(incomplete.Width == 800);
	CHECK(incomplete.Height == 600);
}

TEST_CASE("each viewport resolves its own target extent", "[studio][viewports][render]") {
	const studio::ViewportTargetSize left = ResolveViewportTargetSize(1050, 507, 0, 0, 1920, 1080);
	const studio::ViewportTargetSize right = ResolveViewportTargetSize(729, 674, 0, 0, 1920, 1080);

	CHECK(left.Width == 1050);
	CHECK(left.Height == 507);
	CHECK(right.Width == 729);
	CHECK(right.Height == 674);
}

TEST_CASE("a world already on screen is found rather than opened twice", "[studio][viewports]") {
	const std::array<PanelView, 2> panels{PanelView{CLIENT, true}, PanelView{OTHER, true}};

	// The extra that is pinned to it. Index 1 here is panel 2, which is the
	// numbering `Editor::ExtraAt` uses and the one an off-by-one would swap.
	CHECK(ChooseViewportFor(CLIENT, SCENE, true, panels) == 1);
	CHECK(ChooseViewportFor(OTHER, SCENE, true, panels) == 2);

	// The main panel, which is not pinned to anything - it draws the active
	// scene, so "showing SCENE" is a fact about `mainWorld` rather than about
	// the panel.
	CHECK(ChooseViewportFor(SCENE, SCENE, true, panels) == 0);

	// **Closed and it is still the panel this world belongs in.** A server view
	// shut from its title bar comes back as the main panel rather than as an
	// extra pinned over the top of it, which is the whole reason the closed case
	// is separate from the free-panel search below.
	CHECK(ChooseViewportFor(SCENE, SCENE, false, panels) == 0);

	// And nothing has a panel for a world that is not one.
	CHECK(ChooseViewportFor(WorldId{}, SCENE, true, panels) == NO_VIEWPORT);
}

TEST_CASE("a panel somebody pinned is never taken", "[studio][viewports]") {
	// One free panel: closed. A closed panel is free whatever it is pinned to -
	// nobody is looking at it.
	const std::array<PanelView, 3> panels{
		PanelView{OTHER, true}, PanelView{CLIENT, false}, PanelView{SCENE, true}
	};

	// **Not panel 1 and not panel 3**, which are showing scenes of their own,
	// and not the main panel, which is showing a third.
	CHECK(ChooseViewportFor(WorldId{7}, SCENE, true, panels) == 2);

	// An open panel following the active scene rather than pinned to one is
	// free too: nobody has said it must show anything in particular.
	const std::array<PanelView, 2> loose{PanelView{OTHER, true}, PanelView{WorldId{}, true}};
	CHECK(ChooseViewportFor(CLIENT, SCENE, true, loose) == 2);

	// With every panel spoken for the honest answer is "make one" rather than
	// evicting somebody - `Editor::ShowWorldInViewport` is what mints it.
	const std::array<PanelView, 2> full{PanelView{OTHER, true}, PanelView{CLIENT, true}};
	CHECK(ChooseViewportFor(WorldId{7}, SCENE, true, full) == NO_VIEWPORT);

	// Including when there are no extras at all, which is what a fresh editor
	// with one panel open looks like.
	CHECK(ChooseViewportFor(CLIENT, SCENE, true, std::span<const PanelView>{}) == NO_VIEWPORT);
}

TEST_CASE("new worlds start above the scene looking at the origin", "[studio][viewports][camera]") {
	const ViewportCameraPose pose = DefaultViewportCamera();
	CHECK(pose.Frame.Position == engine::core::Vector3{0.0f, 30.0f, 30.0f});
	CHECK(pose.Frame.LookVector().Dot((engine::core::Vector3::Zero - pose.Frame.Position).Unit()) > 0.9999f);
	const engine::core::CFrame rebuilt = engine::core::CFrame::Angles(pose.Pitch, pose.Yaw, 0.0f);
	CHECK(rebuilt.LookVector().Dot(pose.Frame.LookVector()) > 0.9999f);
}

TEST_CASE("fly movement uses the camera's full basis", "[studio][viewports][camera]") {
	const engine::core::CFrame rotation = engine::core::CFrame::Angles(0.65f, -0.4f, 0.0f);

	CHECK(CameraRelativeMovement(rotation, 1.0f, 0.0f, 0.0f) == rotation.LookVector());
	CHECK(CameraRelativeMovement(rotation, 0.0f, 1.0f, 0.0f) == rotation.RightVector());
	CHECK(CameraRelativeMovement(rotation, 0.0f, 0.0f, 1.0f) == rotation.UpVector());
	CHECK(CameraRelativeMovement(rotation, 0.0f, 0.0f, -1.0f) == rotation.UpVector() * -1.0f);

	const engine::core::Vector3 combined = CameraRelativeMovement(rotation, 1.0f, -1.0f, 1.0f);
	CHECK(combined == rotation.LookVector() - rotation.RightVector() + rotation.UpVector());
}

TEST_CASE("a free viewport camera is carried through a portal", "[studio][viewports][camera][portal]") {
	using engine::core::CFrame;
	using engine::core::Name;
	using engine::core::Vector3;
	using engine::ecs::Entity;
	using engine::ecs::Store;

	engine::scene::RegisterSceneClasses();
	Store store("viewport-portal");

	engine::scene::PartDesc nearDesc;
	nearDesc.Size = Vector3{8.0f, 8.0f, 0.4f};
	nearDesc.Frame = CFrame(Vector3::Zero);
	nearDesc.Simulated = false;
	const Entity nearPane = engine::scene::MakePart(store, nearDesc);

	engine::scene::PartDesc farDesc = nearDesc;
	farDesc.Frame =
		CFrame(Vector3{100.0f, 0.0f, 0.0f}) * CFrame::Angles(0.0f, std::numbers::pi_v<float> / 2.0f, 0.0f);
	const Entity farPane = engine::scene::MakePart(store, farDesc);

	const Entity hole = store.CreateInstance(engine::ecs::Classes::Find(Name("SurfaceCamera")), "Hole");
	engine::scene::SurfaceCamera surface;
	surface.Face = engine::scene::NormalId::Back;
	surface.Surface = 0;
	store.Set(hole, surface);
	store.Set(hole, engine::scene::Portal{farPane});
	REQUIRE(store.SetParent(hole, nearPane));

	const CFrame previous = CFrame::LookAt(Vector3{0.0f, 0.0f, 1.0f}, Vector3{0.0f, 0.0f, 0.0f});
	ViewportCameraPose pose;
	pose.Frame = CFrame::LookAt(Vector3{0.0f, 0.0f, 0.1f}, Vector3{0.0f, 0.0f, -1.0f});

	engine::scene::SeamTransform through;
	REQUIRE(engine::scene::PortalCrossing(store, previous.Position, pose.Frame.Position, through));
	const CFrame mapped = through.Place(pose.Frame);

	REQUIRE(CarryViewportCamera(store, previous, pose));
	CHECK((pose.Frame.Position - mapped.Position).Magnitude() < 0.05f);
	CHECK(pose.Frame.LookVector().Dot(mapped.LookVector()) > 0.9999f);
	CHECK(pose.Frame.Position.X > 90.0f);

	const CFrame rebuilt = CFrame::Angles(pose.Pitch, pose.Yaw, 0.0f);
	CHECK(rebuilt.LookVector().Dot(pose.Frame.LookVector()) > 0.9999f);
}

TEST_CASE("selecting the current direction gizmo axis flips the view", "[studio][viewports][camera]") {
	using engine::core::CFrame;
	using engine::core::Vector3;

	const Vector3 position{4.0f, 5.0f, 6.0f};
	const std::array<Vector3, 3> axes{Vector3::XAxis, Vector3::YAxis, Vector3::ZAxis};
	const std::array<Vector3, 3> upDirections{Vector3::YAxis, Vector3::ZAxis, Vector3::YAxis};

	for (size_t index = 0; index < axes.size(); index++) {
		const CFrame positive = CFrame::LookAt(position, position + axes[index], upDirections[index]);
		const CFrame negative = SnapViewportCameraDirection(positive, axes[index]);
		CHECK(negative.Position == position);
		CHECK(negative.LookVector().Dot(-axes[index]) > 0.9999f);
		CHECK(negative.UpVector().Dot(positive.UpVector()) > 0.9999f);

		const CFrame positiveAgain = SnapViewportCameraDirection(negative, axes[index]);
		CHECK(positiveAgain.Position == position);
		CHECK(positiveAgain.LookVector().Dot(axes[index]) > 0.9999f);
		CHECK(positiveAgain.UpVector().Dot(negative.UpVector()) > 0.9999f);
	}
}

TEST_CASE("each viewport remembers an independent camera per world", "[studio][viewports][camera]") {
	ViewportCameraMemory left;
	ViewportCameraMemory right;
	ViewportCameraPose leftPose = DefaultViewportCamera();
	ViewportCameraPose rightPose = DefaultViewportCamera();
	left.Use(SCENE, leftPose);
	right.Use(SCENE, rightPose);

	leftPose.Frame.Position = {1.0f, 2.0f, 3.0f};
	leftPose.Yaw = 0.25f;
	left.Use(OTHER, leftPose);
	CHECK(leftPose.Frame.Position == engine::core::Vector3{0.0f, 30.0f, 30.0f});

	rightPose.Frame.Position = {40.0f, 50.0f, 60.0f};
	right.Use(OTHER, rightPose);
	left.Use(SCENE, leftPose);
	right.Use(SCENE, rightPose);

	CHECK(leftPose.Frame.Position == engine::core::Vector3{1.0f, 2.0f, 3.0f});
	CHECK(leftPose.Yaw == 0.25f);
	CHECK(rightPose.Frame.Position == engine::core::Vector3{40.0f, 50.0f, 60.0f});

	// Closing a panel has no world to use, but it must retain the pose of the
	// world that was visible. Reopening the same world restores that pose from
	// session memory rather than starting a new camera.
	leftPose.Frame.Position = {7.0f, 8.0f, 9.0f};
	left.Use(WorldId{}, leftPose);
	leftPose = DefaultViewportCamera();
	left.Use(SCENE, leftPose);
	CHECK(leftPose.Frame.Position == engine::core::Vector3{7.0f, 8.0f, 9.0f});
}

TEST_CASE("GUI preview profiles resolve viewer-local screen metrics", "[studio][viewports]") {
	studio::GuiPreviewSettings phone;
	phone.Profile = studio::GuiPreviewProfile::Phone;
	phone.InterfaceScale = 1.5f;
	phone.TextScale = 1.25f;
	const engine::gui::Screen screen = studio::ResolveGuiPreviewScreen(phone, 800.0f, 600.0f);
	CHECK(screen.Width == 390.0f);
	CHECK(screen.Height == 844.0f);
	CHECK(screen.SafeArea.Top == 47.0f);
	CHECK(screen.InterfaceScale == 1.5f);
	CHECK(screen.TextScale == 1.25f);
	CHECK(phone.State == studio::GuiPreviewState::None);
	studio::GuiPreviewSettings secondViewport;
	secondViewport.State = studio::GuiPreviewState::Pressed;
	CHECK(phone.State == studio::GuiPreviewState::None);
	CHECK(secondViewport.State == studio::GuiPreviewState::Pressed);
}

TEST_CASE("GUI preview controls start hidden and claim viewport input", "[studio][viewports][gui][input]") {
	GuiContext context;
	studio::Editor editor;
	CHECK_FALSE(studio::ViewportGuiControlsProbe::VisibleByDefault(editor));

	studio::ViewportGuiControlsProbe::SetVisible(editor, true);
	DrawGuiPreviewControlFrame(editor, false);
	const GuiPreviewControlFrame pressed = DrawGuiPreviewControlFrame(editor, true);
	CHECK(pressed.ControlsCapture);
	CHECK_FALSE(pressed.SurfaceActive);
	DrawGuiPreviewControlFrame(editor, false);
}

TEST_CASE("GUI preview controls follow the rendered image rectangle", "[studio][viewports]") {
	studio::PanelProjection panel;
	panel.ImageMin = {403.0f, 140.0f};
	panel.ImageSize = {810.0f, 464.0f};

	const glm::vec2 controls = studio::GuiPreviewControlsPosition(panel.ImageMin);
	CHECK(controls.x == 411.0f);
	CHECK(controls.y == 148.0f);
}

TEST_CASE("opening a saved game refocuses Worlds after Live Instances", "[studio][viewports][worlds][gpu]") {
	SavedGameScratch scratch;
	studio::Editor editor;
	REQUIRE(studio::ViewportCameraProbe::Initialise(editor, 1));
	const std::filesystem::path game = scratch.Root / "worlds.agame";
	REQUIRE(editor.SaveGame(game));

	studio::ViewportCameraProbe::LeaveLiveInstancesSelected(editor);
	REQUIRE(editor.OpenGame(game));
	CHECK(studio::ViewportCameraProbe::WorldsFocusedForEditing(editor));

	editor.Shutdown();
}

TEST_CASE(
	"Studio demo import mounts a portable client camera companion", "[studio][viewports][demo-camera][gpu]"
) {
	using namespace engine;
	studio::Editor editor;
	REQUIRE(studio::ViewportCameraProbe::Initialise(editor, 1));
	const auto demo = examples::DemosLoader().Find(examples::DemoKind::Script, "Shaders.luau");
	REQUIRE(demo.has_value());
	REQUIRE(studio::ViewportCameraProbe::AddDemo(editor, *demo));
	editor.Universe->Enter(editor.Active, [&](const ecs::Store &store) {
		const auto starter = store.FindFirstRoot("StarterPlayer");
		const auto templates = store.FindFirstChild(starter, "StarterPlayerScripts");
		const auto companion = store.FindFirstChild(templates, "DemoCamera");
		REQUIRE(companion != ecs::NULL_ENTITY);
		CHECK(store.ClassOf(companion) == script::LocalScriptClass());
		CHECK_FALSE(ecs::Store::IsPredicted(companion));
		const core::Name source("examples/scripts/client/DemoCameras/Shaders.client.luau");
		CHECK(script::ActiveSourceOf(store, companion) == source);
		const auto *cache = store.Resource<script::SourceCache>();
		REQUIRE(cache != nullptr);
		const auto *text = cache->Find(source);
		REQUIRE(text != nullptr);
		CHECK(text->find("PublishedCamera") != std::string::npos);
	});
	const WorldId replica = studio::ViewportCameraProbe::StartPlay(editor);
	studio::ViewportCameraProbe::TickPlay(editor);
	const auto &link = *editor.Runs.back().Links.front();
	editor.Universe->Enter(link.AuthorityWorld(), [&](const ecs::Store &store) {
		const auto starter = store.FindFirstRoot("StarterPlayer");
		const auto templates = store.FindFirstChild(starter, "StarterPlayerScripts");
		const auto source = store.FindFirstChild(templates, "DemoCamera");
		const auto scripts = store.FindFirstChild(link.Player(), "PlayerScripts");
		const auto copy = store.FindFirstChild(scripts, "DemoCamera");
		for (const auto companion : {source, copy}) {
			REQUIRE(companion != ecs::NULL_ENTITY);
			ecs::AttributeValue name;
			REQUIRE(ecs::GetAttribute(store, companion, core::Name("Name"), name));
			CHECK(name.Type == ecs::PropertyType::String);
			CHECK(name.String == "ShaderCamera");
			ecs::AttributeValue pose;
			REQUIRE(ecs::GetAttribute(store, companion, core::Name("CFrame"), pose));
			CHECK(pose.Type == ecs::PropertyType::CFrame);
		}
	});
	editor.Universe->Enter(replica, [&](const ecs::Store &store) {
		const auto *local = store.Resource<scene::LocalPlayer>();
		REQUIRE(local != nullptr);
		const auto scripts = store.FindFirstChild(local->Instance, "PlayerScripts");
		const auto companion = store.FindFirstChild(scripts, "DemoCamera");
		REQUIRE(companion != ecs::NULL_ENTITY);
		ecs::AttributeValue name;
		REQUIRE(ecs::GetAttribute(store, companion, core::Name("Name"), name));
		CHECK(name.Type == ecs::PropertyType::String);
		CHECK(name.String == "ShaderCamera");
		ecs::AttributeValue pose;
		REQUIRE(ecs::GetAttribute(store, companion, core::Name("CFrame"), pose));
		CHECK(pose.Type == ecs::PropertyType::CFrame);
		const auto camera = studio::RuntimeCameraOf(store);
		REQUIRE(camera != ecs::NULL_ENTITY);
		CHECK(store.InstanceNameOf(camera) == core::Name("ShaderCamera"));
		CHECK(ecs::Store::IsPredicted(camera));
		CHECK(ecs::IsClientLocalInstance(store, camera));
		CHECK(store.ParentOf(camera) == scene::WorkspaceOf(store));
	});
	studio::ViewportCameraProbe::Stop(editor);
	editor.Shutdown();
}

TEST_CASE("stopping the last Play run refocuses Worlds", "[studio][viewports][worlds][gpu]") {
	studio::Editor editor;
	REQUIRE(studio::ViewportCameraProbe::Initialise(editor, 1));
	REQUIRE(studio::ViewportCameraProbe::StartPlay(editor).IsValid());
	CHECK_FALSE(studio::ViewportCameraProbe::WorldsFocusedForEditing(editor));

	studio::ViewportCameraProbe::Stop(editor);
	CHECK(studio::ViewportCameraProbe::WorldsFocusedForEditing(editor));
	studio::ViewportCameraProbe::Stop(editor);
	CHECK(studio::ViewportCameraProbe::WorldsFocusedForEditing(editor));

	editor.Shutdown();
}

TEST_CASE(
	"repeated Play restores authored content after viewport camera handles remint",
	"[studio][viewports][camera][worlds][gpu]"
) {
	using namespace engine;
	studio::Editor editor;
	REQUIRE(studio::ViewportCameraProbe::Initialise(editor, 1));
	const core::Name worldName = editor.Universe->NameOf(editor.Active);
	studio::ViewportCameraProbe::CreateTwo(editor, editor.Active);

	// Authored children and the world camera survive Stop's store rebuild.
	editor.Universe->Enter(editor.Active, [&](ecs::Store &store) {
		for (int index = 0; index < 24; ++index) {
			const auto part = store.CreateInstance(scene::PartClass(), "Authored" + std::to_string(index));
			REQUIRE(part != ecs::NULL_ENTITY);
			REQUIRE(store.SetParent(part, scene::WorkspaceOf(store)));
		}
	});

	for (int cycle = 0; cycle < 3; ++cycle) {
		INFO("Play/Stop cycle " << cycle);
		REQUIRE(studio::ViewportCameraProbe::StartPlay(editor).IsValid());
		editor.Universe->Enter(editor.Active, [&](ecs::Store &store) {
			size_t cameras = 0;
			store.Each<const scene::Camera>([&](ecs::Entity entity, const scene::Camera &) {
				if (store.ClassOf(entity) != scene::WorldCameraClass()) return;
				++cameras;
				CHECK(store.ParentOf(entity) == scene::WorkspaceOf(store));
			});
			CHECK(cameras == 1);
		});
		studio::ViewportCameraProbe::Stop(editor);
		const WorldId restored = editor.Universe->Find(worldName);
		REQUIRE(restored.IsValid());
		studio::ViewportCameraProbe::Show(editor, restored);
		const auto cameras = studio::ViewportCameraProbe::CreateTwo(editor, restored);
		editor.Universe->Enter(restored, [&](const ecs::Store &store) {
			for (const auto camera : cameras) {
				REQUIRE(store.Get<scene::Camera>(camera) != nullptr);
				CHECK_FALSE(store.Has<scene::TransientComponent>(camera));
				CHECK(store.ParentOf(camera) == scene::WorkspaceOf(store));
			}
			CHECK(RuntimeCameraOf(store) == cameras[1]);
			for (int index = 0; index < 24; ++index) {
				const auto part =
					store.FindFirstChild(scene::WorkspaceOf(store), "Authored" + std::to_string(index));
				REQUIRE(part != ecs::NULL_ENTITY);
				CHECK(store.Get<scene::Camera>(part) == nullptr);
			}
		});
	}
	editor.Shutdown();
}

TEST_CASE(
	"retained viewport opportunities collect cache hits with diagnostics hidden",
	"[studio][viewports][cache][gpu]"
) {
	SavedGameScratch scratch;
	const auto previous = studio::ConfigRoot();
	struct Restore {
		std::filesystem::path Previous;
		~Restore() {
			studio::SetConfigRoot(Previous);
		}
	} restore{previous};
	studio::SetConfigRoot(scratch.Root);
	studio::Editor editor;
	REQUIRE(studio::ViewportCameraProbe::Initialise(editor, 1));
	(void)studio::ViewportCameraProbe::BodyVisibility(editor);
	studio::ViewportCameraProbe::RetainCacheImages(editor);
	const auto before = studio::ViewportCameraProbe::CacheProfile(editor);
	for (size_t opportunity = 0; opportunity < 4; opportunity++)
		studio::ViewportCameraProbe::Present(editor, 0);
	const auto hidden = studio::ViewportCameraProbe::CacheProfile(editor);
	CHECK_FALSE(hidden.LastDamage().Any());
	for (size_t index = 0; index < hidden.Activities().size(); index++) {
		if (hidden.Activities()[index].Last != engine::render::PresentationCacheActivity::Decision::Hit)
			continue;
		CHECK(hidden.Activities()[index].Hits == before.Activities()[index].Hits + 4);
		CHECK(hidden.Activities()[index].Writes == before.Activities()[index].Writes);
	}
	studio::ViewportCameraProbe::CachePage(editor, true);
	studio::ViewportCameraProbe::Present(editor, 0);
	const auto visible = studio::ViewportCameraProbe::CacheProfile(editor);
	const size_t scene = static_cast<size_t>(engine::render::PresentationCacheLayer::SceneImage);
	CHECK(visible.Activities()[scene].Hits == hidden.Activities()[scene].Hits + 1);
	CHECK(visible.Activities()[scene].Writes == hidden.Activities()[scene].Writes);
}

TEST_CASE(
	"Studio imported procedural demos run in their authored VM and stop cleanly",
	"[studio][scripts][language][gpu]"
) {
	using namespace engine;
	const SavedGameScratch scratch;
	struct ConfigRestore {
		std::filesystem::path Previous = studio::ConfigRoot();
		~ConfigRestore() {
			studio::SetConfigRoot(Previous);
		}
	} restore;
	studio::SetConfigRoot(scratch.Root);
	for (const std::string_view file : {"ForestVillage.luau", "ForestVillage.js"}) {
		DYNAMIC_SECTION(file) {
			studio::Editor editor;
			REQUIRE(studio::ViewportCameraProbe::Initialise(editor, 1));
			const auto demo = examples::DemosLoader().Find(examples::DemoKind::Script, file);
			REQUIRE(demo.has_value());
			REQUIRE(studio::ViewportCameraProbe::AddDemo(editor, *demo));
			const auto language = script::LanguageOf(file);
			editor.Universe->Enter(editor.Active, [&](const ecs::Store &store) {
				const auto service = store.FindFirstRoot("ServerScriptService");
				const auto instance = store.FindFirstChild(service, "ForestVillageScene");
				REQUIRE(instance != ecs::NULL_ENTITY);
				CHECK(script::ActiveLanguageOf(store, instance) == language);
				CHECK(
					script::ActiveSourceOf(store, instance) ==
					core::Name(std::string("examples/scripts/") + std::string(file))
				);
			});
			for (size_t run = 0; run < 2; ++run) {
				REQUIRE(studio::ViewportCameraProbe::StartServer(editor));
				REQUIRE(editor.Runs.size() == 1);
				REQUIRE(editor.Runs.front().Runtime != nullptr);
				CHECK(editor.Runs.front().Runtime->Which() == language);
				CHECK(editor.Runs.front().Runtime->LastError().empty());
				editor.Universe->Enter(editor.Active, [&](ecs::Store &store) {
					const auto *published = store.Resource<script::ProceduralPublished>();
					REQUIRE(published != nullptr);
					CHECK(published->Regions.size() == 2);
					CHECK(store.CountMatching<script::ProceduralSceneRegion>() == 2);
				});
				studio::ViewportCameraProbe::Stop(editor);
				CHECK(editor.Runs.empty());
				editor.Universe->Enter(editor.Active, [&](ecs::Store &store) {
					CHECK(store.CountMatching<script::ProceduralSceneRegion>() == 0);
				});
			}
		}
	}
}

TEST_CASE("Studio refuses a mixed script world before recording a run", "[studio][scripts][language][gpu]") {
	using namespace engine;
	const SavedGameScratch scratch;
	struct ConfigRestore {
		std::filesystem::path Previous = studio::ConfigRoot();
		~ConfigRestore() {
			studio::SetConfigRoot(Previous);
		}
	} restore;
	studio::SetConfigRoot(scratch.Root);
	studio::Editor editor;
	REQUIRE(studio::ViewportCameraProbe::Initialise(editor, 1));
	const auto demo = examples::DemosLoader().Find(examples::DemoKind::Script, "ForestVillage.js");
	REQUIRE(demo.has_value());
	REQUIRE(studio::ViewportCameraProbe::AddDemo(editor, *demo));
	size_t cameras = 0;
	editor.Universe->Enter(editor.Active, [&](ecs::Store &store) {
		const auto extra = script::MakeScript(store, "mixed-extra.luau", "MixedExtra");
		REQUIRE(extra != ecs::NULL_ENTITY);
		REQUIRE(store.SetParent(extra, store.FindFirstRoot("ServerScriptService")));
		auto cache = *store.Resource<script::SourceCache>();
		cache.Set(core::Name("mixed-extra.luau"), "error('must not execute')");
		store.SetResource(cache);
		store.RemoveResource<scene::ActiveCamera>();
		cameras = store.CountMatching<scene::Camera>();
	});
	CHECK_FALSE(studio::ViewportCameraProbe::StartServer(editor));
	CHECK(editor.Runs.empty());
	editor.Universe->Enter(editor.Active, [&](ecs::Store &store) {
		CHECK(store.Resource<scene::ActiveCamera>() == nullptr);
		CHECK(store.CountMatching<scene::Camera>() == cameras);
		CHECK(store.CountMatching<script::ProceduralSceneRegion>() == 0);
		const auto *published = store.Resource<script::ProceduralPublished>();
		CHECK((published == nullptr || published->Regions.empty()));
	});
}

TEST_CASE(
	"Studio reopening a game retains the procedural image through its referencing interface frame",
	"[studio][procedural][preview-lifetime][gpu]"
) {
	using namespace engine;
	const SavedGameScratch scratch;
	struct ConfigRestore {
		std::filesystem::path Previous = studio::ConfigRoot();
		~ConfigRestore() {
			studio::SetConfigRoot(Previous);
		}
	} restore;
	studio::SetConfigRoot(scratch.Root);
	studio::Editor editor;
	REQUIRE(studio::ViewportCameraProbe::Initialise(editor, 1));
	const auto image = studio::ViewportCameraProbe::CaptureProceduralPreview(editor);
	void *const handle = editor.Renderer.TextureHandle(image);
	REQUIRE(handle != nullptr);
	const auto path = scratch.Root / "procedural-preview.agame";
	REQUIRE(studio::ViewportCameraProbe::Save(editor, path));

	editor.Interface.Begin(1.0f / 60.0f);
	studio::ViewportCameraProbe::DrawProceduralPreview(editor);
	// The modal opens after panels have emitted images, before this frame records.
	REQUIRE(studio::ViewportCameraProbe::Open(editor, path));
	CHECK(editor.Renderer.TextureHandle(image) == handle);
	CHECK(studio::ViewportCameraProbe::RetiredProceduralTexture(editor, image));
	REQUIRE(studio::ViewportCameraProbe::Open(editor, path));
	CHECK(editor.Renderer.TextureHandle(image) == handle);
	CHECK(studio::ViewportCameraProbe::RetiredProceduralTexture(editor, image));
	editor.Interface.End();
	const auto referencesImage = [texture = ImTextureRef(handle)._TexID] {
		const auto *draw = ImGui::GetDrawData();
		if (!draw) return false;
		for (int index = 0; index < draw->CmdListsCount; ++index)
			for (const auto &command : draw->CmdLists[index]->CmdBuffer)
				if (command.ElemCount && command.TexRef._TexData == nullptr &&
					command.TexRef._TexID == texture)
					return true;
		return false;
	};
	REQUIRE(referencesImage());
	studio::ViewportCameraProbe::Present(editor, 0);
	CHECK(editor.Renderer.TextureHandle(image) == handle);

	// A full replacement frame retires the old image after every interface consumer.
	studio::ViewportCameraProbe::PresentFrame(editor);
	CHECK_FALSE(referencesImage());
	CHECK(editor.Renderer.TextureHandle(image) == nullptr);
	CHECK_FALSE(studio::ViewportCameraProbe::RetiredProceduralTexture(editor, image));
}

TEST_CASE(
	"Studio refresh and reopen retain both procedural images until replacement interface presentation",
	"[studio][procedural][preview-lifetime][gpu]"
) {
	using namespace engine;
	const SavedGameScratch scratch;
	struct ConfigRestore {
		std::filesystem::path Previous = studio::ConfigRoot();
		~ConfigRestore() {
			studio::SetConfigRoot(Previous);
		}
	} restore;
	studio::SetConfigRoot(scratch.Root);
	studio::Editor editor;
	REQUIRE(studio::ViewportCameraProbe::Initialise(editor, 1));
	const auto first = studio::ViewportCameraProbe::CaptureProceduralPreview(editor);
	void *const firstHandle = editor.Renderer.TextureHandle(first);
	REQUIRE(firstHandle != nullptr);
	const auto path = scratch.Root / "procedural-refresh.agame";
	REQUIRE(studio::ViewportCameraProbe::Save(editor, path));

	editor.Interface.Begin(1.0f / 60.0f);
	studio::ViewportCameraProbe::DrawProceduralPreview(editor);
	editor.Interface.End();
	const auto second = studio::ViewportCameraProbe::RefreshProceduralPreview(editor);
	CHECK(second != first);
	void *const secondHandle = editor.Renderer.TextureHandle(second);
	REQUIRE(secondHandle != nullptr);
	CHECK(secondHandle != firstHandle);
	CHECK(editor.Renderer.TextureHandle(first) == firstHandle);
	CHECK(studio::ViewportCameraProbe::RetiredProceduralTexture(editor, first));
	const auto lastGood = studio::ViewportCameraProbe::ProceduralImageState(editor);
	CHECK_FALSE(studio::ViewportCameraProbe::TryRefreshProceduralPreview(editor));
	CHECK(studio::ViewportCameraProbe::ProceduralImageState(editor) == lastGood);
	CHECK(editor.Renderer.TextureHandle(first) == firstHandle);
	CHECK(editor.Renderer.TextureHandle(second) == secondHandle);
	bool referencesFirst = false;
	const auto *draw = ImGui::GetDrawData();
	REQUIRE(draw != nullptr);
	for (int index = 0; index < draw->CmdListsCount; ++index)
		for (const auto &command : draw->CmdLists[index]->CmdBuffer)
			if (command.ElemCount && command.TexRef._TexData == nullptr &&
				command.TexRef._TexID == ImTextureRef(firstHandle)._TexID)
				referencesFirst = true;
	REQUIRE(referencesFirst);

	// Platform windows still consume the earlier frame after the preview capture.
	CHECK(studio::ViewportCameraProbe::Open(editor, path));
	CHECK(studio::ViewportCameraProbe::RetiredProceduralTexture(editor, first));
	CHECK(studio::ViewportCameraProbe::RetiredProceduralTexture(editor, second));
	CHECK(editor.Renderer.TextureHandle(first) == firstHandle);
	CHECK(editor.Renderer.TextureHandle(second) == secondHandle);
	CHECK(studio::ViewportCameraProbe::Open(editor, path));
	CHECK(editor.Renderer.TextureHandle(first) == firstHandle);
	CHECK(editor.Renderer.TextureHandle(second) == secondHandle);

	studio::ViewportCameraProbe::PresentFrame(editor);
	CHECK(editor.Renderer.TextureHandle(first) == nullptr);
	CHECK(editor.Renderer.TextureHandle(second) == nullptr);
	CHECK_FALSE(studio::ViewportCameraProbe::RetiredProceduralTexture(editor, first));
	CHECK_FALSE(studio::ViewportCameraProbe::RetiredProceduralTexture(editor, second));
}
