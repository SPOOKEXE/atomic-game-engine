#include <engine/core/Log.hpp>
#include <engine/core/Paths.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/ui/Fonts.hpp>
#include <engine/ui/Metrics.hpp>
#include <engine/ui/Theme.hpp>

#include <algorithm>
#include <cstring>
#include <imgui.h>
#include <imgui_internal.h>
#include <studio/Editor.hpp>
#include <studio/Keybinds.hpp>
#include <studio/Widgets.hpp>

namespace studio {

	using engine::core::CFrame;
	using engine::core::LogLevel;
	using engine::core::Name;
	using engine::core::Vector3;
	using engine::ecs::Entity;
	using engine::ecs::Store;

	namespace {
		// **The dockspace's id carries a version, and bumping it is how a new
		// panel gets a home.**
		//
		// imgui writes the layout to an ini and owns it from then on, which is
		// right — an editor that threw away wherever somebody dragged a panel to
		// would be unusable. But it also means a panel added in a later build is
		// a window the saved layout has never heard of, so it opens floating in
		// the corner. That is exactly what `Viewport` and `Worlds` did.
		//
		// A version in the id makes the old node unfindable, so the default
		// layout is rebuilt once and then owned by the ini again. **Bump this
		// when a panel is added or the arrangement changes**, and not otherwise
		// — every bump costs everybody their layout.
		// **v3 because Settings is a panel the saved layout has never heard
		// of**, and a panel a layout does not know about opens floating in a
		// corner. Bumping costs everybody the arrangement they dragged into
		// place, which is why `mono.studio/AGENTS.md` says to do it when a
		// panel is added and not otherwise.
		constexpr const char *DOCKSPACE = "StudioDockSpace.v5";

		constexpr const char *VIEWPORT = "Viewport";
		constexpr const char *VIEWPORT2 = "Viewport 2";
		constexpr const char *EXPLORER = "Explorer";
		constexpr const char *PROPERTIES = "Properties";
		constexpr const char *WORLDS = "Worlds";
		constexpr const char *SCRIPTS = "Script Editor";
		constexpr const char *OUTPUT = "Output";
		constexpr const char *SETTINGS = "Studio Settings";
		constexpr const char *STATISTICS = "Statistics";
		constexpr const char *FRAMEGRAPH = "Frame Graph";

		// The first-run layout, built once and then owned by the ini file.
		//
		// **Only when imgui has no layout of its own.** Rebuilding every run
		// would throw away wherever somebody dragged a panel to, which is the
		// single most annoying thing an editor can do.
		void BuildDefaultLayout(ImGuiID dockspace) {
			ImGui::DockBuilderRemoveNode(dockspace);
			ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
			ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->Size);

			// Studio's arrangement, and it is Studio's for a reason worth
			// stating: the tree and the properties are one conversation — you
			// click a thing on the left and edit it below — so they share an
			// edge. Splitting them across the window makes every edit a
			// diagonal mouse journey.
			ImGuiID centre = dockspace;
			const ImGuiID right = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.24f, nullptr, &centre);
			const ImGuiID bottom =
				ImGui::DockBuilderSplitNode(centre, ImGuiDir_Down, 0.28f, nullptr, &centre);

			ImGuiID rightLower = right;
			const ImGuiID rightUpper =
				ImGui::DockBuilderSplitNode(rightLower, ImGuiDir_Up, 0.45f, nullptr, &rightLower);

			// **A split, not the same node.** Docking both viewports into
			// `centre` makes them *tabs*, so the second is a background window
			// — `ImGui::Begin` returns false for it, the panel drops its target
			// and the renderer never draws it. That is not a subtle failure
			// either: the second view is simply never there, and the first
			// looks exactly as it always did.
			//
			// Two views stacked as tabs would also be one view you have to
			// click between, which is the thing having two of them is for.
			ImGuiID rightHalf = centre;
			const ImGuiID leftHalf =
				ImGui::DockBuilderSplitNode(rightHalf, ImGuiDir_Left, 0.5f, nullptr, &rightHalf);

			ImGui::DockBuilderDockWindow(VIEWPORT, leftHalf);
			ImGui::DockBuilderDockWindow(VIEWPORT2, rightHalf);

			// Three and four share the halves rather than splitting further:
			// four quarters of a centre pane are four pictures too small to
			// judge anything by, and a panel can be dragged wherever somebody
			// actually wants it.
			ImGui::DockBuilderDockWindow("Viewport 3", leftHalf);
			ImGui::DockBuilderDockWindow("Viewport 4", rightHalf);
			ImGui::DockBuilderDockWindow(EXPLORER, rightUpper);
			ImGui::DockBuilderDockWindow(WORLDS, rightUpper);
			ImGui::DockBuilderDockWindow(PROPERTIES, rightLower);
			ImGui::DockBuilderDockWindow(SCRIPTS, bottom);
			ImGui::DockBuilderDockWindow(OUTPUT, bottom);

			// Beside the properties rather than in the centre: they are panels
			// somebody opens, reads or changes one thing in, and leaves — and
			// the centre belongs to the world.
			ImGui::DockBuilderDockWindow(SETTINGS, rightLower);
			ImGui::DockBuilderDockWindow(STATISTICS, rightLower);
			ImGui::DockBuilderDockWindow(FRAMEGRAPH, bottom);

			ImGui::DockBuilderFinish(dockspace);
		}
	}

	void Editor::DrawInterface() {
		const ImGuiViewport *viewport = ImGui::GetMainViewport();

		// **No `PassthruCentralNode`, and that flag is why the viewport used to
		// be a hole.** It punches a transparent rectangle through the dockspace
		// so the swapchain shows through — but `imgui.cpp`'s `central_node_hole`
		// requires the central node to be *empty*, so docking a panel into it
		// fills the whole dockspace with `ImGuiCol_WindowBg` instead. The world
		// vanished the moment the viewport was docked, which is exactly what it
		// looked like.
		//
		// The world goes into a texture now and the viewport is an ordinary
		// panel showing it, so there is nothing to see through and no hole to
		// punch. See `render::SceneTarget`.
		// **Both bars before the dockspace, and that ordering was the bug.**
		// `DockSpaceOverViewport` fills the viewport's *work area*, and
		// `BeginMainMenuBar` and `BeginViewportSideBar` are what shrink that
		// area. Drawing the toolbar afterwards put it underneath a dockspace
		// covering the same rectangle — the strip was submitted every frame,
		// with working buttons, and could not be seen or clicked.
		DrawMenuBar();
		DrawToolbar();

		const ImGuiID dockspace = ImGui::DockSpaceOverViewport(ImGui::GetID(DOCKSPACE), viewport);

		if (ResetLayout) {
			ResetLayout = false;
			BuildDefaultLayout(dockspace);
		}

		static bool built = false;
		if (!built) {
			built = true;
			if (ImGui::DockBuilderGetNode(dockspace) == nullptr ||
				ImGui::DockBuilderGetNode(dockspace)->IsLeafNode()) {
				BuildDefaultLayout(dockspace);
			}
		}

		for (size_t index = 0; index <= EXTRA_VIEWPORTS; index++) {
			DrawViewport(index);
		}
		DrawExplorer();
		DrawWorlds();
		DrawProperties();
		DrawScripts();
		DrawOutput();
		DrawSettings();
		DrawStatistics();
		DrawFrameGraph();
		DrawStatusBar();
		DrawDialogs();

		DriveCamera();
		DrawShortcuts();

		// **Once, here, after every panel and whether or not any of them
		// drew.** Panels are closable and every one of them returns early when
		// closed, so an action queued from a menu in one panel and applied at
		// the end of another is an action that silently does nothing the moment
		// somebody closes the wrong window.
		ApplyPendingActions();
	}

	void Editor::FocusSelection(Vector3 &position, float yaw, float pitch) {
		if (Selection.empty() || !SelectionWorld.IsValid()) {
			return;
		}

		// The selection's centre and how far it reaches, from the transforms
		// themselves. Bounds would be better and `scene::Bounds` is on
		// `BasePart` rather than on `Instance` — so a folder of parts would
		// frame from nothing, and a position is the fact every instance has.
		Vector3 centre;
		size_t counted = 0;

		const std::vector<Entity> selected = Selection;
		Universe->Enter(SelectionWorld, [&](Store &store) {
			for (const Entity instance : selected) {
				if (!store.Alive(instance)) {
					continue;
				}
				if (const auto *transform = store.Get<engine::scene::Transform>(instance)) {
					centre = centre + transform->Frame.Position;
					counted++;
				}
			}
		});

		if (counted == 0) {
			// Selected, but nothing in it has a place in the world — a script,
			// a service. Silent rather than a message: F over a folder is a
			// keypress somebody makes by accident.
			return;
		}

		centre = centre * (1.0f / static_cast<float>(counted));

		// Backed off along the way the camera is already pointing. The distance
		// is a constant rather than a function of the selection's size for the
		// same reason the centre is a mean of positions: without bounds there
		// is nothing to measure, and a fixed step is honest about that.
		const CFrame rotation = CFrame::Angles(pitch, yaw, 0.0f);
		position = centre - rotation.LookVector() * 18.0f;
	}

	void Editor::DriveCamera() {
		// **Whichever viewport the pointer is over.** One camera driver for two
		// panels rather than two copies: the rules — right-drag to look,
		// middle-drag to pan, wheel to dolly, F to frame — are the same in
		// both, and a second copy is a second place for them to drift.
		for (ViewportState &view : Extras) {
			if (view.Open && (view.Hovered || view.Active || view.Panning)) {
				DriveCameraFor(
					view.Frame, view.Yaw, view.Pitch, view.Speed, view.Hovered, view.Active, view.Panning
				);
				return;
			}
		}

		DriveCameraFor(CameraFrame, CameraYaw, CameraPitch, CameraSpeed, ViewportHovered, ViewportActive,
					   ViewportPanning);
	}

	void Editor::DriveCameraFor(
		CFrame &frame, float &yaw, float &pitch, float &speed, bool hovered, bool active, bool &panning
	) {
		ImGuiIO &io = ImGui::GetIO();

		// **Right button held, and only over the viewport panel.** A camera that
		// moved on a left click would fight every selection, and one that moved
		// whenever the mouse was not over a panel would spin the view while
		// somebody dragged a splitter.
		//
		// `ViewportActive` rather than `ViewportHovered` is what keeps a look
		// going once it has started: a drag that leaves the panel is still that
		// drag, and a camera that stopped at the edge of the rectangle would be
		// unusable at exactly the moment somebody is turning quickly.
		// **`hovered` alone, not `hovered && !io.WantCaptureMouse`.** The
		// viewport is an imgui window, so hovering it sets `WantCaptureMouse` —
		// the second half was therefore false exactly when the first was true,
		// and the two together could never be satisfied. `IsItemHovered`
		// already refuses when a popup or another window is over the panel,
		// which is the case that test was reaching for.
		const bool looking = (active || hovered) && ImGui::IsMouseDown(ImGuiMouseButton_Right);

		// **Flying detaches from a followed camera.** Otherwise a right-drag
		// would turn a camera nobody asked it to turn — or worse, appear to do
		// nothing because the scene's camera keeps overriding the eye every
		// frame. Discoverable without a menu: you fly, you are flying.
		if (looking && FollowCamera != engine::ecs::NULL_ENTITY) {
			FollowCamera = engine::ecs::NULL_ENTITY;
			Say("back to the editor camera");
		}

		if (looking) {
			const float sensitivity = 0.0035f;
			yaw -= io.MouseDelta.x * sensitivity;
			pitch -= io.MouseDelta.y * sensitivity;

			// Clamped short of straight up and straight down. At exactly
			// vertical the yaw axis and the view direction are parallel and the
			// frame flips, which reads as the camera snapping.
			constexpr float LIMIT = 1.5533f;
			pitch = std::clamp(pitch, -LIMIT, LIMIT);
		}

		// The scroll wheel changes how fast, not how far. A wheel that dollied
		// the camera would make the speed control something you cannot find.
		if (looking && io.MouseWheel != 0.0f) {
			speed = std::clamp(speed * (io.MouseWheel > 0.0f ? 1.25f : 0.8f), 1.0f, 4096.0f);
		}

		const CFrame rotation = CFrame::Angles(pitch, yaw, 0.0f);
		const Vector3 forward = rotation.LookVector();
		const Vector3 right = rotation.RightVector();

		Vector3 position = frame.Position;
		Vector3 move;

		// **WASD without holding a mouse button, which is what an author
		// expects.** Flying used to require the right button down — the
		// aim-while-you-move arrangement Unreal and Roblox use — so tapping W
		// over the picture did nothing at all and read as a broken camera.
		// Turning still needs the right button, because a viewport that swung
		// whenever the pointer crossed it would be unusable; only the
		// translation is freed here.
		//
		// **`WantCaptureKeyboard` is what keeps this out of the script
		// editor.** imgui raises it while any text field has focus, so typing
		// `while` in a script does not fly the camera four ways. That guard was
		// already here and is why this is safe to widen.
		const bool driving = (active || hovered) && !io.WantCaptureKeyboard;

		if (driving) {
			if (ImGui::IsKeyDown(ImGuiKey_W)) {
				move = move + forward;
			}
			if (ImGui::IsKeyDown(ImGuiKey_S)) {
				move = move - forward;
			}
			if (ImGui::IsKeyDown(ImGuiKey_D)) {
				move = move + right;
			}
			if (ImGui::IsKeyDown(ImGuiKey_A)) {
				move = move - right;
			}
			if (ImGui::IsKeyDown(ImGuiKey_E)) {
				move = move + Vector3{0.0f, 1.0f, 0.0f};
			}
			if (ImGui::IsKeyDown(ImGuiKey_Q)) {
				move = move - Vector3{0.0f, 1.0f, 0.0f};
			}
		}

		// --- pan, dolly and focus -------------------------------------------
		//
		// **The three ways of getting somewhere that are not flying.** The
		// right-drag flythrough above is how you *look*; it is a poor way to
		// approach a specific part, which is what an editor is mostly for.
		// These are Studio's, and they work whether or not the right button is
		// down.

		const bool overViewport = hovered || active;

		// Middle-drag slides the camera across its own plane, so the thing
		// under the pointer stays roughly under the pointer.
		if ((overViewport || panning) && ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
			panning = true;

			// Scaled by distance-independent speed rather than by depth: there
			// is no picked point to measure against, and a pan that changed
			// pace with whatever happened to be in front of it is worse than
			// one that is merely constant.
			const float pace = speed * 0.0016f;
			position = position - right * (io.MouseDelta.x * pace);
			position = position + rotation.UpVector() * (io.MouseDelta.y * pace);
		} else if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
			panning = false;
		}

		// **The wheel dollies when not looking and changes speed when it is.**
		// Both are what somebody means by the wheel at those two moments, and
		// the `looking` branch above already claimed the second — so this is
		// the other half rather than a conflict.
		if (overViewport && !looking && io.MouseWheel != 0.0f) {
			position = position + forward * (io.MouseWheel * speed * 0.12f);
		}

		// F frames the selection, which is the one navigation command that
		// needs no aim at all.
		if (overViewport && !io.WantCaptureKeyboard && ImGui::IsKeyPressed(ImGuiKey_F, false)) {
			FocusSelection(position, yaw, pitch);
		}

		const float length = std::sqrt(move.X * move.X + move.Y * move.Y + move.Z * move.Z);
		if (length > 0.0001f) {
			// **Wall time, and this is the one place in the program where that
			// is right.** An editor camera is not simulation: it must move at
			// the same rate whether or not the game is running, and tying it to
			// a fixed tick would freeze it in Edit mode where nothing ticks at
			// all.
			const float step = speed * io.DeltaTime / length;
			position = position + move * step;
		}

		frame = CFrame(position, rotation.Rotation());
	}

	void Editor::DrawViewport(size_t index) {
		ViewportState *extra = ExtraAt(index);
		const bool second = extra != nullptr;

		// imgui remembers a window by its title, so the titles are fixed rather
		// than built per call — a name that changed would be a panel the saved
		// layout has never heard of.
		static const char *const TITLES[] = {"Viewport", "Viewport 2", "Viewport 3", "Viewport 4"};

		// **The second panel is a different world by default and says which.**
		// Two viewports both showing the active world is one view drawn twice;
		// the reason to open the second is to watch the server's world beside
		// the client's, or one subarea beside another while both tick.
		const char *title = TITLES[index < 4 ? index : 0];
		bool *open = second ? &extra->Open : &ShowViewport;
		engine::render::SceneTarget &target = second ? extra->Target : WorldTarget;

		if (!*open) {
			// Nothing asks for a texture, so the renderer releases the one it
			// had. A closed panel should not go on costing its pixels.
			target = engine::render::SceneTarget{};
			return;
		}

		// No padding, so the image is the panel rather than a picture inside it.
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		const bool shown = ImGui::Begin(
			title, open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
		);
		ImGui::PopStyleVar();

		if (!shown) {
			// Collapsed or behind another tab. The target is dropped rather than
			// left at its last size — rendering a texture nobody shows is a
			// frame's work thrown away every frame.
			target = engine::render::SceneTarget{};
			ImGui::End();
			return;
		}

		// **What the toolbar reports on, claimed here rather than from a click
		// on the image.** Focus is true for the panel a person is working in
		// whether they got there by clicking the picture, the tab or the title
		// bar — a click on the image alone would leave the transport describing
		// a panel nobody is in, which is the whole failure this is fixing. See
		// `FocusedViewport`.
		//
		// **`ChildWindows` and emphatically not `RootAndChildWindows`.** A
		// docked window's *root* is the dockspace host, which every docked
		// panel shares — so the root-walking flag is true for all four
		// viewports at once and the last one drawn wins. It was written that
		// way first and the toolbar reported "Viewport 2" no matter which panel
		// was clicked. This flag stays inside the panel and its own children.
		if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)) {
			FocusedViewport = index;
		}

		// **The size in pixels, which is not the size in imgui's points.** The
		// texture is real pixels and a high-DPI display makes those different
		// numbers — a target sized in points on a 2x display is a quarter-scale
		// image stretched back up, which reads as a blurry renderer.
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		const ImVec2 size = ImGui::GetContentRegionAvail();
		const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
		const float horizontal = scale.x > 0.0f ? scale.x : 1.0f;
		const float vertical = scale.y > 0.0f ? scale.y : 1.0f;

		target.Width = static_cast<uint32_t>(std::max(size.x, 1.0f) * horizontal);
		target.Height = static_cast<uint32_t>(std::max(size.y, 1.0f) * vertical);

		// **The texture the renderer holds now, and it is usually this frame's
		// picture rather than the last one's.** imgui records its draw lists
		// before the renderer runs, so what is bound here is whatever texture
		// exists at this moment — but the world pass and the interface pass go
		// into the same command buffer with the world first, so a texture that
		// keeps its identity across the frame is written before it is sampled.
		// Targets are allocated in blocks precisely so that identity survives a
		// resize; see `render::SceneExtent`.
		//
		// **Sampled to its extent rather than whole.** The texture is rounded up
		// to a block and the world fills the corner, so drawing all of it would
		// show the unwritten border down two edges.
		if (void *texture = Renderer.SceneTexture(index); texture != nullptr) {
			const engine::render::SceneExtent extent = Renderer.SceneTextureExtent(index);
			ImGui::Image(
				reinterpret_cast<ImTextureID>(texture),
				size,
				ImVec2(0.0f, 0.0f),
				ImVec2(extent.U, extent.V)
			);
		} else {
			// The first frame, and any frame after a resize the renderer has not
			// caught up with. Nothing to show yet; the button below still makes
			// the panel drivable.
			ImGui::Dummy(size);
		}

		// **The thing the camera is actually driven from, and its absence was
		// why the camera could not be driven at all.** `ImGui::Image` is not an
		// interactive item: it has no id, it is never hovered *as an item* in a
		// way that survives, and `IsItemActive` is false for it forever. So the
		// look condition — "the viewport is active, or hovered and imgui does
		// not want the mouse" — could only ever be satisfied by the fallback
		// path that ran on the first frame after a resize.
		//
		// A button laid over the image gives the panel an id, so a right-drag
		// *captures*: `IsItemActive` stays true while the button is held even
		// when the pointer leaves the panel, which is what makes a fast turn
		// keep turning instead of stopping at the edge.
		//
		// Right and middle only. Left is deliberately not claimed — it belongs
		// to selecting things in the world, and a button that swallowed it
		// would be in the way of the first feature added here.
		ImGui::SetCursorScreenPos(origin);
		ImGui::InvisibleButton(
			"##surface", size, ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle
		);

		if (second) {
			extra->Hovered = ImGui::IsItemHovered();
			extra->Active = ImGui::IsItemActive();
		} else {
			ViewportHovered = ImGui::IsItemHovered();
			ViewportActive = ImGui::IsItemActive();
		}

		// **This panel's hover, not the first panel's.** Reading
		// `ViewportHovered` here made an extra viewport's claim below depend on
		// whether the *main* one was hovered, so a right-drag over Viewport 2
		// was handed to whatever sat behind it and the camera did not turn.
		const bool hovered = second ? extra->Hovered : ViewportHovered;

		// **The image is not a button, so a click over it has to be claimed
		// explicitly** or the panel behind would get it. Right was always
		// claimed, because a right-drag is how the camera is aimed. Left is
		// claimed for one more reason: clicking a picture is how a person says
		// "this is the viewport I am working in", and imgui does not focus a
		// window from a click on a non-interactive item — so without this the
		// toolbar went on describing whichever panel imgui happened to focus
		// last, which is exactly what it did. See `FocusedViewport`.
		if (hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
						ImGui::IsMouseClicked(ImGuiMouseButton_Left))) {
			ImGui::SetWindowFocus();
			FocusedViewport = index;
		}

		// **The second panel picks its own world.** Without this both viewports
		// show the active one, which is one view drawn twice at half the rate —
		// strictly worse than a single viewport. In Play the two are naturally
		// the server's world and the world a client is standing in.
		if (second) {
			ImGui::SetCursorScreenPos(ImVec2(origin.x + 10.0f, origin.y + 8.0f));
			ImGui::SetNextItemWidth(engine::ui::Scaled(170.0f));

			const Name chosen = Universe->NameOf(extra->World.IsValid() ? extra->World : Active);
			if (ImGui::BeginCombo("##view", chosen.IsValid() ? Label(chosen) : "(no scene)")) {
				for (const WorldId id : Universe->Worlds()) {
					const Name name = Universe->NameOf(id);
					if (ImGui::Selectable(name.IsValid() ? Label(name) : "?", id == extra->World)) {
						extra->World = id;
					}
				}
				ImGui::EndCombo();
			}

			ImGui::End();
			return;
		}

		// The readout, drawn back over the top-left of the image.
		ImGui::SetCursorScreenPos(ImVec2(origin.x + 10.0f, origin.y + 8.0f));

		// **Scoped to the readout and not to the function.** A pushed font that
		// is still pushed when `ImGui::End` runs is imgui's "Missing PopFont()"
		// assertion — which fires at the *end of the frame*, naming neither the
		// window nor the font, and was exactly what this cost once.
		ImGui::BeginGroup();
		{
			const engine::ui::ScopedFont small(
				engine::ui::Typeface::Interface, engine::ui::TextSize::Small
			);

		const engine::core::Name scene =
			Universe->NameOf(second ? (extra->World.IsValid() ? extra->World : Active) : Active);
		ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
		ImGui::Text(
			"%s   %u x %u   %u draw   %llu tris   %u culled",
			scene.IsValid() ? Label(scene) : "(no scene)",
			target.Width,
			target.Height,
			LastFrame.DrawCalls,
			static_cast<unsigned long long>(LastFrame.Triangles),
			LastFrame.Culled
		);
		ImGui::PopStyleColor();

		if (Mode != RunMode::Edit) {
			// **The one thing that must be visible without reading anything.**
			// An author who has forgotten they are in Play will make edits Stop
			// throws away, and "why did my change vanish" is the worst question
			// an editor can produce.
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::WarningColour());
			ImGui::Text("%s - Stop restores the scene", Describe(Mode));
			ImGui::PopStyleColor();
		}

		if (ViewportActive) {
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
			ImGui::Text("WASD / QE   %.0f u/s   wheel to change", static_cast<double>(CameraSpeed));
			ImGui::PopStyleColor();
		}
		}

		ImGui::EndGroup();
		ImGui::End();
	}

	void Editor::DrawViewMenu() {
		ImGui::MenuItem("Viewport", nullptr, &ShowViewport);

		// **The second view, and it is what "server beside client" is made
		// of.** Off by default: a second viewport halves the refresh rate of
		// both, so it is a thing somebody opens when they want it rather than
		// a cost everybody pays.
		for (size_t index = 0; index < EXTRA_VIEWPORTS; index++) {
			const char *names[] = {"Viewport 2", "Viewport 3", "Viewport 4"};
			ImGui::MenuItem(names[index], nullptr, &Extras[index].Open);
		}
		ImGui::MenuItem("Explorer", nullptr, &ShowExplorer);
		ImGui::MenuItem("Worlds", nullptr, &ShowWorlds);
		ImGui::MenuItem("Properties", nullptr, &ShowProperties);
		ImGui::MenuItem("Script Editor", nullptr, &ShowScripts);
		ImGui::MenuItem("Output", nullptr, &ShowOutput);
		ImGui::MenuItem("Settings", nullptr, &ShowSettings);

		ImGui::Separator();

		// **In the View menu like every other panel**, because that is this
		// program's rule: a thing that can be toggled and has no menu entry is
		// a thing somebody turns on by accident and cannot turn off. No
		// shortcuts of their own — the Keybinds page is where keys are decided
		// now, and two places to bind a key is one too many.
		ImGui::MenuItem("Statistics", nullptr, &ShowStatistics);
		ImGui::MenuItem("Frame Graph", nullptr, &ShowFrameGraph);

		ImGui::Separator();

		if (ImGui::MenuItem("Show Every Panel")) {
			ShowViewport = ShowExplorer = ShowWorlds = true;
			ShowProperties = ShowScripts = ShowOutput = true;
		}

		if (ImGui::MenuItem("Reset Layout")) {
			// **Rebuilt on the next frame rather than here**, because the
			// dockspace is mid-frame and rearranging its nodes from inside a
			// menu is rearranging the tree that is being walked.
			ResetLayout = true;
			ShowViewport = ShowExplorer = ShowWorlds = true;
			ShowProperties = ShowScripts = ShowOutput = true;
		}
	}

	void Editor::DrawMenuBar() {
		if (!ImGui::BeginMainMenuBar()) {
			return;
		}

		if (ImGui::BeginMenu("File")) {
			if (ImGui::MenuItem("New Game", Keybinds::Of(Action::NewGame).Text().c_str())) {
				NewGame();
			}
			if (ImGui::MenuItem("Open Game...", Keybinds::Of(Action::OpenGame).Text().c_str())) {
				AskingOpen = true;
				PathBuffer = GamePath.string();
			}

			ImGui::Separator();

			if (ImGui::MenuItem("Save", Keybinds::Of(Action::Save).Text().c_str())) {
				if (GamePath.empty()) {
					AskingSaveAs = true;
					PathBuffer = std::string(Label(GameName)) + std::string(engine::game::GAME_EXTENSION);
				} else {
					SaveGame(GamePath);
				}
			}
			if (ImGui::MenuItem("Save As...", Keybinds::Of(Action::SaveAs).Text().c_str())) {
				AskingSaveAs = true;
				PathBuffer =
					GamePath.empty()
						? std::string(Label(GameName)) + std::string(engine::game::GAME_EXTENSION)
						: GamePath.string();
			}

			ImGui::Separator();

			if (ImGui::MenuItem("Import World...", nullptr, false, true)) {
				AskingImport = true;
				PathBuffer.clear();
			}

			// **Import rather than Open, and they are different operations.**
			// Open replaces this universe with the file's; this adds the
			// file's worlds to what is already here, renaming any whose name
			// is taken. See `game::ImportUniverse`.
			if (ImGui::MenuItem("Import Universe...", nullptr, false, true)) {
				AskingImportUniverse = true;
				PathBuffer.clear();
			}
			if (ImGui::MenuItem("Export Active World...", nullptr, false, Active.IsValid())) {
				AskingExport = true;
				PathBuffer = std::string(Label(Universe->NameOf(Active))) +
							 std::string(engine::game::WORLD_EXTENSION);
			}

			// **Beside the world export rather than beside Save As**, because
			// the pair an author is choosing between is "this scene" and "all
			// of them" — not "write it" and "write it somewhere else". The
			// extension is what tells them apart afterwards.
			if (ImGui::MenuItem("Export Universe...", nullptr, false, Universe->Count() > 0)) {
				AskingExportUniverse = true;
				PathBuffer =
					std::string(Label(GameName, "Game")) + std::string(engine::game::GAME_EXTENSION);
			}

			ImGui::Separator();

			if (ImGui::MenuItem("Exit", "Alt+F4")) {
				Running = false;
			}
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Edit")) {
			if (ImGui::MenuItem("Duplicate", Keybinds::Of(Action::Duplicate).Text().c_str(), false, !Selection.empty())) {
				DuplicateSelection();
			}
			if (ImGui::MenuItem("Delete", Keybinds::Of(Action::Delete).Text().c_str(), false, !Selection.empty())) {
				DeleteSelection();
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Select None", Keybinds::Of(Action::SelectNone).Text().c_str(), false, !Selection.empty())) {
				ClearSelection();
			}
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Insert", Active.IsValid())) {
			ImGui::TextDisabled("into %s", Selection.empty() ? "the world" : "the selection");
			ImGui::Separator();

			if (const engine::ecs::ClassId chosen = DrawClassPicker("insert-menu"); chosen.IsValid()) {
				InsertInstance(
					Active, chosen, Selection.empty() ? engine::ecs::NULL_ENTITY : Selection.front()
				);
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("World")) {
			if (ImGui::MenuItem("New World...")) {
				AskingNewWorld = true;
				NameBuffer = "World " + std::to_string(Universe->Count() + 1);
			}
			if (ImGui::MenuItem("Remove Active World", nullptr, false, Universe->Count() > 1)) {
				RemoveWorld(Active);
			}

			ImGui::Separator();
			ImGui::TextDisabled("scenes");

			for (const WorldId id : Universe->Worlds()) {
				const Name name = Universe->NameOf(id);
				const bool selected = id == Active;
				if (ImGui::MenuItem(name.IsValid() ? Label(name) : "?", nullptr, selected)) {
					Active = id;
					SelectionWorld = id;
					ClearSelection();
				}
			}
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("View")) {
			DrawViewMenu();
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Run")) {
			if (ImGui::MenuItem("Play (server + client)", Keybinds::Of(Action::Play).Text().c_str(), Mode == RunMode::Play)) {
				SetRunMode(Mode == RunMode::Play ? RunMode::Edit : RunMode::Play);
			}
			if (ImGui::MenuItem("Run (server only)", Keybinds::Of(Action::RunServer).Text().c_str(), Mode == RunMode::Server)) {
				SetRunMode(Mode == RunMode::Server ? RunMode::Edit : RunMode::Server);
			}
			if (ImGui::MenuItem("Stop", Keybinds::Of(Action::Stop).Text().c_str(), false, Mode != RunMode::Edit)) {
				SetRunMode(RunMode::Edit);
			}
			ImGui::EndMenu();
		}

		// The title, right-aligned. A window title bar is the platform's and
		// says "atomic studio"; what is *open* belongs where the eye already is.
		{
			const std::string title = TitleText();
			const float width = ImGui::CalcTextSize(title.c_str()).x;
			ImGui::SameLine(ImGui::GetWindowWidth() - width - ImGui::GetStyle().ItemSpacing.x * 3.0f);
			ImGui::TextUnformatted(title.c_str());
		}

		ImGui::EndMainMenuBar();
	}

	void Editor::DrawShortcuts() {
		// **At the end of the frame, and that is the whole reason this is its own
		// function.** `io.WantTextInput` is cleared by `NewFrame` and set by
		// whatever field turns out to be active *during* the frame — so reading
		// from the menu bar, which draws first, always saw false. The symptom was
		// Ctrl+S inside the script editor saving the script *and* opening the Save
		// Game As dialog at the same time.
		if (ImGui::GetIO().WantTextInput) {
			return;
		}

		// **Every key comes from `Keybinds`, and none is spelled out here.**
		// That table is what the Keybinds page edits, so a binding changed
		// there changes what this does on the next frame — and the menus print
		// their shortcut labels from the same rows. Three copies of "F5" was
		// what this looked like before, and two of them were comments.

		if (Keybinds::Fired(Action::Save)) {
			if (GamePath.empty()) {
				AskingSaveAs = true;
				PathBuffer = std::string(Label(GameName, "Untitled")) +
							 std::string(engine::game::GAME_EXTENSION);
			} else {
				SaveGame(GamePath);
			}
		}

		if (Keybinds::Fired(Action::SaveAs)) {
			AskingSaveAs = true;
			PathBuffer = GamePath.empty() ? std::string(Label(GameName, "Untitled")) +
												std::string(engine::game::GAME_EXTENSION)
										  : GamePath.string();
		}

		if (Keybinds::Fired(Action::NewGame)) {
			NewGame();
		}

		if (Keybinds::Fired(Action::OpenGame)) {
			AskingOpen = true;
			PathBuffer = GamePath.string();
		}

		if (Keybinds::Fired(Action::Duplicate) && !Selection.empty()) {
			DuplicateSelection();
		}

		if (Keybinds::Fired(Action::Delete) && !Selection.empty()) {
			DeleteSelection();
		}

		if (Keybinds::Fired(Action::SelectNone) && !Selection.empty()) {
			ClearSelection();
		}

		// **Stop is tested before Play**, because their defaults share a key:
		// F5 plays and Shift+F5 stops, which is Studio's arrangement. `Fired`
		// matches modifiers exactly, so the two cannot both fire — but the
		// order says which is meant to win if somebody binds them to the same
		// chord anyway.
		if (Keybinds::Fired(Action::Stop)) {
			SetRunMode(RunMode::Edit);
		} else if (Keybinds::Fired(Action::Play)) {
			SetRunMode(Mode == RunMode::Play ? RunMode::Edit : RunMode::Play);
		}

		if (Keybinds::Fired(Action::RunServer)) {
			SetRunMode(Mode == RunMode::Server ? RunMode::Edit : RunMode::Server);
		}

		if (Keybinds::Fired(Action::ShowStatistics)) {
			ShowStatistics = !ShowStatistics;
		}

		if (Keybinds::Fired(Action::ShowFrameGraph)) {
			ShowFrameGraph = !ShowFrameGraph;
		}
	}

	void Editor::DrawToolbar() {
		ImGuiViewport *viewport = ImGui::GetMainViewport();

		// **A side bar, not a window placed where a side bar would go.**
		// `BeginViewportSideBar` reserves the strip out of the viewport's work
		// area, which is the only thing that stops the dockspace from being
		// laid over the top of it — the previous version positioned an
		// ordinary window at `WorkPos` and was invisible for exactly that
		// reason.
		//
		// Pinned rather than dockable, because a toolbar you can accidentally
		// drag into a corner is a toolbar somebody loses.
		const float height = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2.0f;

		constexpr ImGuiWindowFlags FLAGS = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
										   ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
										   ImGuiWindowFlags_NoScrollbar;

		if (!ImGui::BeginViewportSideBar("##toolbar", viewport, ImGuiDir_Up, height, FLAGS)) {
			ImGui::End();
			return;
		}

		const bool running = Mode != RunMode::Edit;

		if (RunButton("Play", Mode == RunMode::Play, engine::ui::AccentColour())) {
			SetRunMode(Mode == RunMode::Play ? RunMode::Edit : RunMode::Play);
		}
		ImGui::SameLine();
		if (RunButton("Run", Mode == RunMode::Server, engine::ui::AccentColour())) {
			SetRunMode(Mode == RunMode::Server ? RunMode::Edit : RunMode::Server);
		}
		ImGui::SameLine();

		// **Pause between Run and Stop, and only while something runs.** It is
		// the transport a person reaches for mid-run, so it sits where a
		// transport does; disabled in Edit because there is no clock to stop —
		// a button that could be pressed and did nothing would read as a
		// pause that failed.
		ImGui::BeginDisabled(!running);
		if (RunButton(Paused ? "Resume" : "Pause", Paused, engine::ui::WarningColour())) {
			Paused = !Paused;
			Say(Paused ? "paused — the clock is stopped, the run is not"
					   : "resumed");
		}
		ImGui::SameLine();

		if (ImGui::Button("Stop")) {
			SetRunMode(RunMode::Edit);
		}
		ImGui::EndDisabled();

		ImGui::SameLine();
		ImGui::TextDisabled("|");
		ImGui::SameLine();

		// **Everything from here reports on the viewport you are in, not on
		// "the" world.** With two panels showing two worlds ticking in
		// parallel, a transport that always described the active world was
		// describing the wrong one half the time — you would be looking at the
		// mirror and reading the skygrid's state. See `FocusedViewport`.
		const size_t reporting = FocusedViewport;
		const WorldId shown = ViewportWorld(reporting);
		ViewportState *reported = ExtraAt(reporting);

		// Which panel is being described, so the readout is never ambiguous
		// about *whose* state it is showing.
		if (reporting > 0) {
			ImGui::TextDisabled("Viewport %zu", reporting + 1);
			ImGui::SameLine();
		}

		// The scene selector, because a universe with several worlds needs one
		// click to switch and a menu is two.
		//
		// **It retargets the focused panel rather than always the active
		// world.** Picking a scene while looking at Viewport 2 moves *that*
		// panel — the alternative is a selector that appears to be about the
		// picture in front of you and silently changes a different one.
		ImGui::SetNextItemWidth(180.0f * Settings.Scale);
		const Name shownName = Universe->NameOf(shown);
		if (ImGui::BeginCombo("##scene", shownName.IsValid() ? Label(shownName) : "(no scene)")) {
			for (const WorldId id : Universe->Worlds()) {
				const Name name = Universe->NameOf(id);
				if (ImGui::Selectable(name.IsValid() ? Label(name) : "?", id == shown)) {
					if (reported != nullptr) {
						reported->World = id;
					} else {
						Active = id;
						SelectionWorld = id;
						ClearSelection();
					}
				}
			}
			ImGui::EndCombo();
		}

		ImGui::SameLine();

		// **The world's own state, which is not the same claim as the mode.**
		// The mode is the universe's — Play runs every world — while this is
		// whether *this* world is ticking, and the two disagree exactly when
		// something interesting has happened: a world suspended for being empty
		// during a run, or faulted because its tick threw. That is the reading
		// somebody switches viewports to get.
		if (shown.IsValid()) {
			const engine::world::WorldState state = Universe->StateOf(shown);
			const bool healthy = state == engine::world::WorldState::Active;
			ImGui::PushStyleColor(
				ImGuiCol_Text, healthy ? engine::ui::MutedColour() : engine::ui::WarningColour()
			);
			ImGui::TextUnformatted(engine::world::Describe(state));
			ImGui::PopStyleColor();
		} else {
			ImGui::TextDisabled("no scene");
		}

		ImGui::SameLine();
		ImGui::TextDisabled("|");
		ImGui::SameLine();

		ImGui::BeginDisabled(!Active.IsValid());
		if (ImGui::Button("Insert Object")) {
			ImGui::OpenPopup("insert-object");
		}
		ImGui::EndDisabled();

		if (ImGui::BeginPopup("insert-object")) {
			if (const engine::ecs::ClassId chosen = DrawClassPicker("insert-toolbar"); chosen.IsValid()) {
				InsertInstance(
					Active, chosen, Selection.empty() ? engine::ecs::NULL_ENTITY : Selection.front()
				);
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		ImGui::End();
	}

	void Editor::DrawOutput() {
		if (!ShowOutput) {
			return;
		}

		if (!ImGui::Begin(OUTPUT, &ShowOutput)) {
			ImGui::End();
			return;
		}

		if (ImGui::SmallButton("Clear")) {
			Output.clear();
		}
		ImGui::SameLine();
		ImGui::TextDisabled("%zu line(s)", Output.size());
		ImGui::Separator();

		if (ImGui::BeginChild("##lines", ImVec2(0, 0), ImGuiChildFlags_None)) {
			// **Monospace, because this is a log.** A stack trace, a table of
			// numbers and a printed table all line up in one and none of them do
			// in a proportional face.
			const engine::ui::ScopedFont code(
				engine::ui::Typeface::Monospace, engine::ui::TextSize::Small
			);

			for (const Message &message : Output) {
				const unsigned int colour = message.Level == LogLevel::Error ? engine::ui::ErrorColour()
										  : message.Level == LogLevel::Warning ? engine::ui::WarningColour()
																			: 0u;

				if (colour != 0u) {
					ImGui::PushStyleColor(ImGuiCol_Text, colour);
				}
				ImGui::TextUnformatted(message.Text.c_str());
				if (colour != 0u) {
					ImGui::PopStyleColor();
				}
			}

			// Pinned to the bottom while the view is already there, and left
			// alone when it is not. Scrolling up to read an error and being
			// yanked back down by the next line is the thing that makes an
			// output panel useless.
			if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
				ImGui::SetScrollHereY(1.0f);
			}
		}
		ImGui::EndChild();
		ImGui::End();
	}

	void Editor::DrawStatusBar() {
		const ImGuiViewport *viewport = ImGui::GetMainViewport();
		const float height = ImGui::GetFrameHeight();

		ImGui::SetNextWindowPos(
			ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + viewport->WorkSize.y - height)
		);
		ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, height));
		ImGui::SetNextWindowViewport(viewport->ID);

		constexpr ImGuiWindowFlags FLAGS = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
										   ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
										   ImGuiWindowFlags_NoBringToFrontOnFocus;

		if (!ImGui::Begin("##status", nullptr, FLAGS)) {
			ImGui::End();
			return;
		}

		ImGui::Text(
			"%s  |  %.0f fps  |  %u draw calls, %llu triangles, %u culled  |  camera %.0f u/s",
			Describe(Mode),
			ImGui::GetIO().Framerate,
			LastFrame.DrawCalls,
			static_cast<unsigned long long>(LastFrame.Triangles),
			LastFrame.Culled,
			static_cast<double>(CameraSpeed)
		);

		ImGui::End();
	}

	void Editor::DrawDialogs() {
		// **One modal shape for five questions, because they are one question.**
		// Every dialog here asks for a path or a name and then does something
		// with it; five hand-written popups would be five places for the Enter
		// key to behave differently.
		if (AskingSaveAs) {
			ImGui::OpenPopup("Save Game As");
		}
		if (AskingOpen) {
			ImGui::OpenPopup("Open Game");
		}
		if (AskingExport) {
			ImGui::OpenPopup("Export World");
		}
		if (AskingExportUniverse) {
			ImGui::OpenPopup("Export Universe");
		}
		if (AskingImport) {
			ImGui::OpenPopup("Import World");
		}
		if (AskingImportUniverse) {
			ImGui::OpenPopup("Import Universe");
		}
		if (AskingNewWorld) {
			ImGui::OpenPopup("New World");
		}
		if (AskingRenameWorld) {
			ImGui::OpenPopup("Rename Scene");
		}

		if (PathPrompt("Save Game As", "File", PathBuffer, "Save")) {
			SaveGame(std::filesystem::path(PathBuffer));
			AskingSaveAs = false;
		} else if (!ImGui::IsPopupOpen("Save Game As")) {
			AskingSaveAs = false;
		}

		if (PathPrompt("Open Game", "File", PathBuffer, "Open")) {
			OpenGame(std::filesystem::path(PathBuffer));
			AskingOpen = false;
		} else if (!ImGui::IsPopupOpen("Open Game")) {
			AskingOpen = false;
		}

		if (PathPrompt("Export World", "File", PathBuffer, "Export")) {
			ExportActiveWorld(std::filesystem::path(PathBuffer));
			AskingExport = false;
		} else if (!ImGui::IsPopupOpen("Export World")) {
			AskingExport = false;
		}

		// **The universe, which is a different document from a world and not a
		// bigger one.** `<Game>` and `<World>` are separate roots and the
		// reader refuses each in the other's place, so the two exports write
		// different extensions and say which they are — see
		// `game::WORLD_EXTENSION`, where the same distinction is spelled out.
		if (PathPrompt("Export Universe", "File", PathBuffer, "Export")) {
			ExportUniverse(std::filesystem::path(PathBuffer));
			AskingExportUniverse = false;
		} else if (!ImGui::IsPopupOpen("Export Universe")) {
			AskingExportUniverse = false;
		}

		if (PathPrompt("Import Universe", "File", PathBuffer, "Import")) {
			ImportUniverseFile(std::filesystem::path(PathBuffer));
			AskingImportUniverse = false;
		} else if (!ImGui::IsPopupOpen("Import Universe")) {
			AskingImportUniverse = false;
		}

		if (PathPrompt("Import World", "File", PathBuffer, "Import")) {
			ImportWorldFile(std::filesystem::path(PathBuffer));
			AskingImport = false;
		} else if (!ImGui::IsPopupOpen("Import World")) {
			AskingImport = false;
		}

		if (PathPrompt("Rename Scene", "Name", NameBuffer, "Rename")) {
			PendingRenameWorld = RenamingWorld;
			PendingRenameTo = NameBuffer;
			AskingRenameWorld = false;
		} else if (!ImGui::IsPopupOpen("Rename Scene")) {
			AskingRenameWorld = false;
		}

		if (PathPrompt("New World", "Name", NameBuffer, "Create")) {
			if (!NameBuffer.empty()) {
				const WorldId created = AddWorld(Name(NameBuffer));
				if (created.IsValid()) {
					Active = created;
					SelectionWorld = created;
					ClearSelection();
					Say("added world '" + NameBuffer + "'");
				}
			}
			AskingNewWorld = false;
		} else if (!ImGui::IsPopupOpen("New World")) {
			AskingNewWorld = false;
		}
	}
}
