#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/core/Paths.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/game/Game.hpp>
#include <engine/scene/Components.hpp>
#include <engine/ui/Fonts.hpp>
#include <engine/ui/Metrics.hpp>
#include <engine/ui/Theme.hpp>

#include <algorithm>
#include <cstring>
#include <cstdio>
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
		constexpr const char *DOCKSPACE = "StudioDockSpace.v6";

		constexpr const char *VIEWPORT = "Viewport";
		constexpr const char *VIEWPORT2 = "Viewport 2";
		constexpr const char *EXPLORER = "Explorer";
		constexpr const char *PROPERTIES = "Properties";
		constexpr const char *WORLDS = "Worlds";
		constexpr const char *SCRIPTS = "Script Editor";
		constexpr const char *OUTPUT = "Output";
		// **The title reads "Preferences" and the id stays "Studio Settings".**
		// imgui derives a window's id from its label, and the saved layout keys
		// on that id — so renaming the panel outright would leave the ini's
		// entry orphaned and the panel would come back floating in a corner for
		// everybody who had docked it. `###` pins the id to the old name while
		// the visible title changes, which is the same trick the script tabs use
		// to survive a rename. See `mono.studio/AGENTS.md`.
		constexpr const char *SETTINGS = "Preferences###Studio Settings";
		constexpr const char *STATISTICS = "Statistics";
		constexpr const char *FRAMEGRAPH = "Frame Graph";

		// The rest, which have no constant of their own because only this list
		// and their own `Begin` ever name them. Spelled here rather than
		// hoisted into a dozen more constants: a name used twice in one file is
		// not the drift a constant prevents.
		constexpr const char *SKINNABLE[]{
			VIEWPORT,
			VIEWPORT2,
			EXPLORER,
			WORLDS,
			PROPERTIES,
			SCRIPTS,
			OUTPUT,
			"Command Bar",
			SETTINGS,
			STATISTICS,
			FRAMEGRAPH,
			"History",
			"Assets",
			"Network",
			"Team Create",
			"Control (MCP)",
			"Plugins",
			"Bus",
			"Find Instances",
			"Script Profile",
			"Changes",
			"Debugger",
		};

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

	std::span<const char *const> SkinnablePanels() {
		return std::span<const char *const>(SKINNABLE, std::size(SKINNABLE));
	}

	const engine::ui::ThemeColours &Editor::PanelColoursFor(const char *panel) const {
		// **One empty set, handed out to every panel that has none.** The common
		// case by a mile — most panels are never recoloured — and it has to cost
		// a pointer rather than a construction, because this runs once per panel
		// per frame.
		static const engine::ui::ThemeColours NONE;

		if (panel == nullptr) {
			return NONE;
		}

		const auto found = Prefs.PanelColours.find(panel);
		return found == Prefs.PanelColours.end() ? NONE : found->second;
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
		// **Every bar before the dockspace, and that ordering was the bug.**
		// `DockSpaceOverViewport` fills the viewport's *work area*, and
		// `BeginMainMenuBar` and `BeginViewportSideBar` are what shrink that
		// area. Drawing the toolbar afterwards put it underneath a dockspace
		// covering the same rectangle — the strip was submitted every frame,
		// with working buttons, and could not be seen or clicked.
		//
		// **The status bar had the same bug and kept it a version longer**,
		// because it is a readout rather than a control: an unclickable toolbar
		// is noticed the first time somebody reaches for Play, and an invisible
		// readout is indistinguishable from one nobody looked at. It moved up
		// here with the other two and became a side bar in the same change.
		{
			ENGINE_PROFILE_CAT("bars", engine::core::ProfileCategory::Render);
			DrawMenuBar();
			DrawToolbar();
			DrawStatusBar();
		}

		// **Spanned, because it is not the free line it looks like.** The
		// dockspace host is a full-window `Begin` and the splitter arithmetic for
		// every node under it, and it sat in the same unmeasured gap the bars did.
		ImGuiID dockspace = 0;
		{
			ENGINE_PROFILE_CAT("dockspace", engine::core::ProfileCategory::Render);
			dockspace = ImGui::DockSpaceOverViewport(ImGui::GetID(DOCKSPACE), viewport);
		}

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

		// Reset before any panel draws: the claim is a within-frame fact, not
		// state that carries. See `ViewportClaimed`.
		ViewportClaimed = false;

		// Same reason, and the same lifetime: a draw list belongs to the frame
		// it came from, and a panel closed this frame must contribute nothing
		// rather than the rectangle it had when it was last open.
		for (OverlaySlot &slot : Overlays) {
			slot = OverlaySlot{};
		}

		// **One span per panel, so the interface bar has something under it.**
		// Without these the whole build is a single wide block and the only
		// question it can answer is "is the interface slow" — which is the one
		// question you already know the answer to by the time you have opened
		// the graph. A closed panel returns immediately and costs a span of
		// almost nothing, which is the correct reading rather than an absence.
		{
			ENGINE_PROFILE_CAT("viewports", engine::core::ProfileCategory::Render);
			for (size_t index = 0; index <= EXTRA_VIEWPORTS; index++) {
				Skinned(index == 0 ? VIEWPORT : VIEWPORT2, [&] { DrawViewport(index); });
			}
		}

		// **After every viewport, never inside one.** See the note in
		// `DrawViewport` for why asking per panel could not work.
		ResolveFocusedViewport();

		{
			ENGINE_PROFILE_CAT("explorer", engine::core::ProfileCategory::Render);
			Skinned(EXPLORER, [&] { DrawExplorer(); });
		}
		{
			ENGINE_PROFILE_CAT("worlds", engine::core::ProfileCategory::Render);
			Skinned(WORLDS, [&] { DrawWorlds(); });
		}
		{
			ENGINE_PROFILE_CAT("properties", engine::core::ProfileCategory::Render);
			Skinned(PROPERTIES, [&] { DrawProperties(); });
		}
		{
			ENGINE_PROFILE_CAT("scripts", engine::core::ProfileCategory::Render);
			Skinned(SCRIPTS, [&] { DrawScripts(); });
		}

		{
			ENGINE_PROFILE_CAT("output", engine::core::ProfileCategory::Render);
			Skinned(OUTPUT, [&] { DrawOutput(); });
			Skinned("Command Bar", [&] { DrawCommandBar(); });
		}
		{
			ENGINE_PROFILE_CAT("settings", engine::core::ProfileCategory::Render);
			Skinned(SETTINGS, [&] { DrawSettings(); });
		}
		{
			ENGINE_PROFILE_CAT("statistics", engine::core::ProfileCategory::Render);
			Skinned(STATISTICS, [&] { DrawStatistics(); });
		}
		// **The panel that reports the frame, inside the frame it reports.** It
		// draws a row per span and a bar per span, so it scales with exactly the
		// thing it is used to measure — and it was the largest of the panels with
		// no span of its own, which made it the one thing the graph could not
		// account for while being read.
		{
			ENGINE_PROFILE_CAT("frame graph", engine::core::ProfileCategory::Render);
			Skinned(FRAMEGRAPH, [&] { DrawFrameGraph(); });
		}

		// v0.10's panels. Each returns immediately when closed, which is what
		// makes a long list of them cost nothing to leave wired in.
		//
		// One span over the group rather than six. A closed panel is an early
		// return, and six spans of almost nothing crowd the graph without ever
		// distinguishing themselves; if this bar is ever wide, splitting it is
		// the next step rather than the current one.
		{
			ENGINE_PROFILE_CAT("tools", engine::core::ProfileCategory::Render);
			Skinned("History", [&] { DrawHistory(); });
			Skinned("Assets", [&] { DrawAssets(); });
			// TODO(render-pipeline): `DrawRenderPipeline()`, `DrawAssetsPipeline()`
			// and `DrawPipelineProfile()` were drawn here.
			Skinned("Network", [&] { DrawNetwork(); });
			Skinned("Team Create", [&] { DrawTeamCreate(); });
			Skinned("Control (MCP)", [&] { DrawControl(); });
			Skinned("Plugins", [&] { DrawPlugins(); });

			// **Not `Skinned`, and that is the difference between the two
			// halves of this feature.** Every panel above is the editor's and
			// takes its colours from the settings page; a plugin's dock widget
			// is the plugin's and takes them from `SetWidgetColour`, per widget,
			// inside the loop.
			DrawPluginWidgets();

			Skinned("Bus", [&] { DrawBus(); });
			Skinned("Find Instances", [&] { DrawFindInstances(); });
			Skinned("Script Profile", [&] { DrawScriptProfile(); });
			Skinned("Changes", [&] { DrawDiff(); });
			Skinned("Debugger", [&] { DrawDebugger(); });
		}

		// **After every panel, because any of them may be the one under the
		// cursor.** `EndHoverPreview` settles the delay for the whole frame and
		// `DrawHoverPreview` puts the panel on top of everything — which is why
		// neither belongs inside a list's own loop.
		EndHoverPreview(static_cast<double>(ImGui::GetIO().DeltaTime));

		// **Before the hover panel, so hover wins.** This asks for a picture
		// of whichever rendered row still has none; `DrawHoverPreview`
		// overwrites the request with the row under the cursor, which is the
		// one somebody is actually looking at.
		PumpRenderedPreviews();
		DrawHoverPreview();

		{
			ENGINE_PROFILE_CAT("dialogs", engine::core::ProfileCategory::Render);
			DrawDialogs();
			DrawPalette();
		}

		{
			ENGINE_PROFILE_CAT("camera", engine::core::ProfileCategory::Render);
			DriveCamera();
		}

		// **Immediately after the camera moves and before the frame ends.** See
		// `Editor::OverlaySlot`: this is what makes the grid sit still on the
		// ground instead of swimming across it.
		{
			ENGINE_PROFILE_CAT("overlays", engine::core::ProfileCategory::Render);
			DrawViewportOverlays();
		}

		{
			ENGINE_PROFILE_CAT("shortcuts", engine::core::ProfileCategory::Render);
			DrawShortcuts();
		}

		// **Once, here, after every panel and whether or not any of them
		// drew.** Panels are closable and every one of them returns early when
		// closed, so an action queued from a menu in one panel and applied at
		// the end of another is an action that silently does nothing the moment
		// somebody closes the wrong window.
		{
			ENGINE_PROFILE_CAT("actions", engine::core::ProfileCategory::Render);
			ApplyPendingActions();
		}
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
		// **The pointer decides first, and focus decides when the pointer is
		// nowhere.** One camera driver for every panel rather than a copy each:
		// the rules — right-drag to look, middle-drag to pan, wheel to dolly, F
		// to frame, WASD to fly — are the same in all of them.
		//
		// The order is the whole of it. A viewport under the pointer is the one a
		// mouse gesture means, whichever panel was last clicked; a viewport with
		// the keyboard in it is the one WASD means, wherever the pointer has
		// wandered off to. Asking the pointer first and falling back to focus
		// gives both without either overriding the other.
		//
		// **Which panel, resolved once, then driven once.** This was four call
		// sites of an eight-argument function spread over four early returns —
		// and two of them were provably the same call, one passing a computed
		// expression and the other a hard-coded `true` for the same value. A
		// ninth parameter would have meant four more edits, and the signature
		// grew by one the last time it was touched.
		constexpr size_t NONE = ~size_t{0};
		size_t target = NONE;

		for (size_t index = 1; index <= EXTRA_VIEWPORTS; index++) {
			const ViewportState &view = Extras[index - 1];
			if (view.Open && (view.Hovered || view.Active || view.Panning)) {
				target = index;
				break;
			}
		}

		if (target == NONE && (ViewportHovered || ViewportActive || ViewportPanning)) {
			target = 0;
		}

		// Nothing under the pointer, so the keyboard's panel gets the frame —
		// but only if the keyboard is genuinely in a viewport. `FocusedViewport`
		// alone still names one after a click into the properties panel, which is
		// right for the transport readout and wrong here. See
		// `ResolveFocusedViewport`, which runs earlier this frame.
		if (target == NONE && FocusedIsViewport) {
			ViewportState *focused = ExtraAt(FocusedViewport);
			if (focused == nullptr || focused->Open) {
				target = FocusedViewport;
			}
		}

		if (target == NONE) {
			return;
		}

		// `ExtraAt` returns null for index 0, which is the main viewport's own
		// fields — the one place the extras array does not hold the state.
		ViewportState *view = ExtraAt(target);
		const bool focused = FocusedIsViewport && FocusedViewport == target;

		if (view == nullptr) {
			DriveCameraFor(
				CameraFrame,
				CameraYaw,
				CameraPitch,
				CameraSpeed,
				ViewportHovered,
				ViewportActive,
				ViewportPanning,
				focused
			);
			return;
		}

		DriveCameraFor(
			view->Frame, view->Yaw, view->Pitch, view->Speed, view->Hovered, view->Active, view->Panning,
			focused
		);
	}

	void Editor::DriveCameraFor(
		CFrame &frame,
		float &yaw,
		float &pitch,
		float &speed,
		bool hovered,
		bool active,
		bool &panning,
		bool focused
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
		// **`WantTextInput`, not `WantCaptureKeyboard` — and that swap is the
		// bug, not a tidy-up.** The claim above it was that imgui raises
		// `WantCaptureKeyboard` "while any text field has focus". It does not.
		// `imgui.cpp` raises it while *navigation* is active:
		//
		//     else if (io.NavActive && (ConfigFlags & NavEnableKeyboard) &&
		//              io.ConfigNavCaptureKeyboard)
		//         io.WantCaptureKeyboard = true;
		//
		// `ui::Interface` sets `NavEnableKeyboard` and `ConfigNavCaptureKeyboard`
		// defaults to true, so **clicking a viewport to focus it set the very
		// flag that switched its camera off**. WASD worked until you clicked on
		// the picture, which is the first thing anybody does.
		//
		// **This is the keyboard twin of the bug fixed twelve lines above**, and
		// it survived that fix: `hovered && !io.WantCaptureMouse` could never be
		// satisfied because hovering an imgui window is what raises
		// `WantCaptureMouse`. Same mistake, same file, other device — a
		// "does imgui want this" flag used to answer "is a widget eating this".
		//
		// `WantTextInput` is the question that was meant: it is raised only by an
		// active text field, so typing `while` in the script editor still does
		// not fly the camera four ways. `DrawShortcuts` already guards on this
		// one, for the same reason.
		const bool driving = (active || hovered || focused) && !io.WantTextInput;

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
		// needs no aim at all — so it follows the keyboard rather than the
		// pointer, and carries the same `WantTextInput` correction as `driving`.
		// It had the identical `WantCaptureKeyboard` guard and was dead in
		// exactly the same circumstances.
		if ((overViewport || focused) && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F, false)) {
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
		// **Focus is not decided here, and two attempts to decide it here is why
		// it is not.** Every panel asking "am I focused?" as it draws is a race:
		// `SetWindowFocus` applies at the *end* of a frame, so a click on
		// Viewport 1 leaves Viewport 2 still reporting focus when it draws
		// moments later, and whichever panel drew last won. Guarding the query
		// with a click flag just moved which panel got stuck.
		//
		// `ResolveFocusedViewport` settles it once, after every panel has
		// drawn, from the single window imgui actually considers focused. The
		// only thing claimed here is the click, below — because a click has to
		// take effect on the frame it happened rather than the frame after.

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
		// What the overlay pass needs, kept rather than drawn now. See
		// `Editor::OverlaySlot`: the camera has not been driven yet, so
		// anything projected here would be a frame behind the pixels under it.
		if (index < Overlays.size()) {
			OverlaySlot &slot = Overlays[index];
			slot.List = ImGui::GetWindowDrawList();
			slot.X = origin.x;
			slot.Y = origin.y;
			slot.Width = size.x;
			slot.Height = size.y;
			slot.Drawn = true;
		}

		ImGui::SetCursorScreenPos(origin);

		// **Left is claimed now, and the comment below used to say it was
		// deliberately not.** It was reserved for "selecting things in the
		// world", which is this. Right and middle still drive the camera; a
		// left click picks, and ctrl-click adds — the modifier the explorer
		// already uses, because two ways to extend one selection is two things
		// to learn.
		ImGui::InvisibleButton(
			"##surface",
			size,
			ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
				ImGuiButtonFlags_MouseButtonMiddle
		);

		// Recorded rather than acted on: picking enters the store, and a panel
		// acts from outside `Universe::Enter` — the rule at the top of
		// `Editor.hpp`. `DrawViewportOverlays` runs it after the camera moves,
		// which is also when the projection it needs is correct.
		if (ImGui::IsItemDeactivated() && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
			!ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Left)) {
			const ImVec2 at = ImGui::GetIO().MousePos;
			PendingPick.Viewport = index;
			PendingPick.X = at.x;
			PendingPick.Y = at.y;
			PendingPick.Add = ImGui::GetIO().KeyCtrl;
			PendingPick.Wanted = true;
		}

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

			// Held for the rest of the frame so a later panel's stale
			// `IsWindowFocused` cannot take it back. See the note above.
			ViewportClaimed = true;
		}

		// **An extra viewport used to stop here, behind a scene dropdown drawn
		// over its own picture, and both halves of that were wrong.** The
		// dropdown duplicated the toolbar's scene selector — which now retargets
		// whichever viewport you are in, so one control does the job from a
		// place that is not covering the image. And returning early meant an
		// extra panel never reached the readout below: no scene name, no draw
		// count, no triangle count, no warning that a run is in progress. The
		// panel you opened to compare two worlds was the one that could not tell
		// you anything about either.
		//
		// Both panels fall through to the same readout now. See `DrawToolbar`.

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

		// **This panel's world, not "the" mode.** With scenes running
		// independently, a viewport showing an edited world must not warn that
		// Stop will throw the edits away — and one showing a running world must,
		// whatever the other panels are doing. An author who has forgotten which
		// of two scenes is live is exactly who this line is for.
		if (const RunMode panelMode = ModeOf(ViewportWorld(index)); panelMode != RunMode::Edit) {
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::WarningColour());
			ImGui::Text("%s - Stop restores this scene", Describe(panelMode));
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
		ImGui::MenuItem("Command Bar", nullptr, &ShowCommandBar);
		ImGui::MenuItem("Preferences", nullptr, &ShowSettings);

		ImGui::Separator();

		// **In the View menu like every other panel**, because that is this
		// program's rule: a thing that can be toggled and has no menu entry is
		// a thing somebody turns on by accident and cannot turn off. No
		// shortcuts of their own — the Keybinds page is where keys are decided
		// now, and two places to bind a key is one too many.
		ImGui::MenuItem("Statistics", nullptr, &ShowStatistics);
		ImGui::MenuItem("Frame Graph", nullptr, &ShowFrameGraph);
		ImGui::MenuItem("Script Profile", nullptr, &ShowScriptProfile);

		ImGui::Separator();

		// The inspectors. Closed by default and grouped apart from the panels
		// somebody works in all day, because these are opened to answer a
		// question and closed again.
		ImGui::MenuItem("History", nullptr, &ShowHistory);
		ImGui::MenuItem("Assets", nullptr, &ShowAssets);
		// TODO(render-pipeline): the three pipeline panels were opened from here.
		ImGui::MenuItem("Network", nullptr, &ShowNetwork);
		ImGui::MenuItem("Team Create", nullptr, &ShowTeamCreate);
		ImGui::MenuItem("Control (MCP)", nullptr, &ShowControl);
		ImGui::MenuItem("Plugins", nullptr, &ShowPlugins);
		ImGui::MenuItem("Find Instances", nullptr, &ShowFindInstances);
		ImGui::MenuItem("Bus", nullptr, &ShowBus);
		ImGui::MenuItem("Changes", nullptr, &ShowDiff);
		ImGui::MenuItem("Debugger", nullptr, &ShowDebugger);

		ImGui::Separator();

		// Not a panel, so it is below the separator rather than in the list of
		// them — but it is a thing somebody turns off and has to be able to
		// turn back on, which is the rule this menu exists for.
		ImGui::MenuItem("Ground Grid", nullptr, &ShowGrid);

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

			// **The last five, most recent first**, which is the shape a menu
			// wants: a list somebody scans rather than searches. Kept in
			// `~/Documents/atomic-game-engine/studio/recent.json` with the rest
			// of the configuration — see `studio/Config.hpp`.
			if (ImGui::BeginMenu("Open Recent", !Recent.Paths.empty())) {
				// A copy, because opening a game calls `Recent.Remember` and
				// reorders the very list this loop is walking.
				const std::vector<std::filesystem::path> listed = Recent.Paths;

				std::filesystem::path chosen;
				std::filesystem::path forget;

				for (const std::filesystem::path &path : listed) {
					std::error_code missing;
					const bool there = std::filesystem::is_regular_file(path, missing);

					// **A file that is not there is shown and disabled, not
					// hidden.** A project on a drive that is not plugged in
					// right now is still a project somebody wants to see, and a
					// menu that silently forgot it would be worse than a row
					// that says why it cannot be opened.
					ImGui::BeginDisabled(!there);
					if (ImGui::MenuItem(path.filename().string().c_str())) {
						chosen = path;
					}
					ImGui::EndDisabled();

					if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
						ImGui::SetTooltip("%s%s", path.string().c_str(), there ? "" : "\n(not found)");
					}

					// The one thing a missing row is still good for.
					if (!there && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
						forget = path;
					}
				}

				ImGui::Separator();
				if (ImGui::MenuItem("Clear List")) {
					Recent.Paths.clear();
				}

				ImGui::EndMenu();

				// **After `EndMenu`, because opening a game tears down every
				// world** — and doing that while ImGui is inside the menu it is
				// drawing is rearranging the tree being walked.
				if (!forget.empty()) {
					Recent.Forget(forget);
				}
				if (!chosen.empty()) {
					OpenGame(chosen);
				}
			}

			// **Rojo's layout, because it is the one the ecosystem already
			// writes.** A game laid out for Rojo has its scripts in folders that
			// mean something, its tooling assumes it, and every author who has
			// used Roblox knows it — asking them to convert a working project in
			// order to try this engine is the wrong side of the trade for a
			// format that costs a parser to read.
			if (ImGui::MenuItem("Sync Rojo Project...")) {
				AskingRojo = true;
				PathBuffer = GamePath.empty() ? std::string("default.project.json")
											  : (GamePath.parent_path() / "default.project.json").string();
			}

			// **A second command rather than a mode of the first.** One project
			// file is one world, which is what an author editing a scene wants;
			// a universe file is a whole game laid out as a folder of them,
			// which is what an author opening a checkout wants. Deciding which
			// they meant by looking inside the file would be a guess, and the
			// wrong guess builds a game into one world.
			if (ImGui::MenuItem("Sync Rojo Universe...")) {
				AskingRojoUniverse = true;
				PathBuffer = GamePath.empty()
								 ? std::string("main.universe.json")
								 : (GamePath.parent_path() / "main.universe.json").string();
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
			// **Named after what they would reverse, not "Undo".** A stack whose
			// top is invisible is one somebody presses hopefully; "Undo Delete
			// Wall" is the difference between reversing an edit and finding out
			// what you reversed afterwards. `CommandLog::NextUndo` carries the
			// description the recording site wrote for exactly this.
			const bool canUndo = Commands != nullptr && Commands->CanUndo();
			const bool canRedo = Commands != nullptr && Commands->CanRedo();

			const std::string undoLabel =
				canUndo ? "Undo " + std::string(Commands->NextUndo()) : std::string("Undo");
			const std::string redoLabel =
				canRedo ? "Redo " + std::string(Commands->NextRedo()) : std::string("Redo");

			// **Every item below asks the operator table whether it may run.**
			// The conditions used to be written here — `!Selection.empty()`
			// three times in this menu alone, and again in `DrawShortcuts` —
			// which is four copies of one rule with nothing comparing them. See
			// `Operators.hpp`.
			const auto item = [this](Action id, const char *label) {
				const Availability state = Operators.Available(id);
				if (ImGui::MenuItem(label, Keybinds::Of(id).Text().c_str(), false, state.Ready)) {
					Operators.Run(id);
				}

				// The reason, on hover, for a greyed row. A menu item that is
				// disabled and silent is one somebody clicks twice.
				if (!state.Ready && !state.Reason.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
					ImGui::SetTooltip("%s", state.Reason.c_str());
				}
			};

			item(Action::Undo, undoLabel.c_str());
			item(Action::Redo, redoLabel.c_str());
			ImGui::Separator();
			item(Action::Duplicate, "Duplicate");
			item(Action::Delete, "Delete");
			ImGui::Separator();
			item(Action::SelectNone, "Select None");
			ImGui::Separator();
			item(Action::CommandPalette, "Command Palette...");

			ImGui::Separator();

			// **Where every other editor puts it.** Preferences is the last
			// item in Edit on Windows and Linux, and somebody looking for the
			// theme or the frame cap looks there before they look in View —
			// which is where the panel was, filed with the panels because it is
			// one. It is still in View as well, because it is still a panel and
			// `DrawViewMenu` is the guaranteed way back to any of them.
			if (ImGui::MenuItem("Preferences...", nullptr, ShowSettings)) {
				ShowSettings = true;
				ImGui::SetWindowFocus(SETTINGS);
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
			// The scene in the viewport being worked in, exactly as the
			// toolbar's buttons. A menu that ran something else would be a
			// second answer to "which scene does Play mean".
			const WorldId scope = ViewportWorld(FocusedViewport);
			const RunMode mode = ModeOf(scope);

			if (ImGui::MenuItem(
					"Play (server + client)", Keybinds::Of(Action::Play).Text().c_str(),
					mode == RunMode::Play
				)) {
				SetRunMode(scope, mode == RunMode::Play ? RunMode::Edit : RunMode::Play);
			}
			if (ImGui::MenuItem(
					"Run (server only)", Keybinds::Of(Action::RunServer).Text().c_str(),
					mode == RunMode::Server
				)) {
				SetRunMode(scope, mode == RunMode::Server ? RunMode::Edit : RunMode::Server);
			}

			// **How many clients the next Play admits.** Disabled while
			// something is running, because the links are made at `BeginRun` and
			// a number that changed underneath a live run would describe
			// something that is not there. Stop, change it, Play again — which
			// is also how Roblox's own player count works.
			ImGui::Separator();
			ImGui::BeginDisabled(mode != RunMode::Edit);
			if (ImGui::BeginMenu("Clients")) {
				for (int count = 1; count <= MAXIMUM_PLAY_CLIENTS; count++) {
					char label[32];
					std::snprintf(label, sizeof(label), "%d client%s", count, count == 1 ? "" : "s");
					if (ImGui::MenuItem(label, nullptr, PlayClients == count)) {
						PlayClients = count;
					}
				}
				ImGui::EndMenu();
			}
			ImGui::EndDisabled();

			if (PlayClients > 1) {
				ImGui::TextDisabled("  Play opens %d clients", PlayClients);
			}
			if (ImGui::MenuItem(
					"Stop", Keybinds::Of(Action::Stop).Text().c_str(), false, mode != RunMode::Edit
				)) {
				SetRunMode(scope, RunMode::Edit);
			}
			if (ImGui::MenuItem("Stop All", nullptr, false, AnyRunning())) {
				EndAllRuns();
				Say("stopped every scene");
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

		if (Keybinds::Fired(Action::CommandPalette)) {
			ShowPalette = true;
		}

		// **Through the table, not through the method.** A shortcut that called
		// `UndoEdit` directly would be a second answer to "may this run now" —
		// the menu asks the poll and the key would not, and the two would agree
		// only for as long as nobody changed one of them.
		if (Keybinds::Fired(Action::Undo)) {
			Operators.Run(Action::Undo);
		}

		if (Keybinds::Fired(Action::Redo)) {
			Operators.Run(Action::Redo);
		}

		if (Keybinds::Fired(Action::Duplicate) && !Selection.empty()) {
			DuplicateSelection();
		}

		if (Keybinds::Fired(Action::Delete) && !Selection.empty()) {
			DeleteSelection();
		}

		// The primary selection, which is the one the tree highlights first. A
		// rename is a single-instance edit however many rows are selected —
		// there is one field and one name being typed into it.
		if (Keybinds::Fired(Action::Rename) && !Selection.empty()) {
			BeginRename(Selection.front());
		}

		if (Keybinds::Fired(Action::SelectNone) && !Selection.empty()) {
			ClearSelection();
		}

		// **Stop is tested before Play**, because their defaults share a key:
		// F5 plays and Shift+F5 stops, which is Studio's arrangement. `Fired`
		// matches modifiers exactly, so the two cannot both fire — but the
		// order says which is meant to win if somebody binds them to the same
		// chord anyway.
		// Same scene the toolbar and the Run menu act on, so a keyboard and a
		// click cannot disagree about which world F5 starts.
		const WorldId transport = ViewportWorld(FocusedViewport);
		const RunMode transportMode = ModeOf(transport);

		if (Keybinds::Fired(Action::Stop)) {
			SetRunMode(transport, RunMode::Edit);
		} else if (Keybinds::Fired(Action::Play)) {
			SetRunMode(transport, transportMode == RunMode::Play ? RunMode::Edit : RunMode::Play);
		}

		if (Keybinds::Fired(Action::RunServer)) {
			SetRunMode(transport, transportMode == RunMode::Server ? RunMode::Edit : RunMode::Server);
		}

		if (Keybinds::Fired(Action::ShowStatistics)) {
			ShowStatistics = !ShowStatistics;
		}

		if (Keybinds::Fired(Action::ShowFrameGraph)) {
			ShowFrameGraph = !ShowFrameGraph;
		}
	}

	void Editor::ResolveFocusedViewport() {
		// **Read from imgui's own idea of the focused window, once.** This is
		// the fact every per-panel `IsWindowFocused` call was trying and failing
		// to reconstruct: there is exactly one focused window, it is known here,
		// and reading it after all the panels have drawn means no panel can
		// overwrite another's answer.
		//
		// A click inside a viewport has already set `FocusedViewport` directly,
		// because `SetWindowFocus` does not land until the end of the frame and
		// the transport should not lag a click by one. From the next frame on,
		// this agrees with it.
		const ImGuiContext *context = ImGui::GetCurrentContext();
		if (context == nullptr || context->NavWindow == nullptr) {
			return;
		}

		// The panel that owns the focused window, which is the window itself
		// unless focus landed on a child of it — a combo or a popup inside the
		// viewport is still the viewport for this purpose.
		const ImGuiWindow *focused = context->NavWindow->RootWindow != nullptr
										 ? context->NavWindow->RootWindow
										 : context->NavWindow;

		// **Which panel the keyboard is in, decided in the same place and from
		// the same window.** A binding scoped to the tree must not fire while
		// the pointer is in a viewport, and this is the one function that knows
		// which panel is in front. See `Keybinds::Scope`.
		const auto isWindow = [&](const char *title) {
			const ImGuiWindow *window = ImGui::FindWindowByName(title);
			return window != nullptr && (window == focused || window == context->NavWindow);
		};

		if (isWindow(EXPLORER) || isWindow(WORLDS)) {
			Keybinds::SetScope(Scope::Tree);
		} else if (isWindow(SCRIPTS)) {
			Keybinds::SetScope(Scope::Script);
		} else {
			Keybinds::SetScope(Scope::Viewport);
		}

		static const char *const TITLES[] = {"Viewport", "Viewport 2", "Viewport 3", "Viewport 4"};

		for (size_t index = 0; index <= EXTRA_VIEWPORTS; index++) {
			const ImGuiWindow *window = ImGui::FindWindowByName(TITLES[index]);
			if (window == nullptr) {
				continue;
			}

			if (window == focused || window == context->NavWindow) {
				FocusedViewport = index;
				FocusedIsViewport = true;
				return;
			}
		}

		// **Focus somewhere else leaves the last viewport standing**, and that
		// is deliberate: clicking the explorer or a property field should not
		// blank the transport's readout. It keeps describing the viewport you
		// were last in, which is the one you are still looking at.
		//
		// **The camera must not read it that way, which is why there are two
		// facts here rather than one.** "Which viewport is being reported on"
		// survives a click into the properties panel; "is the keyboard in a
		// viewport" does not, and a camera driven by the first would fly while
		// somebody typed a number into a field. See `FocusedIsViewport`.
		FocusedIsViewport = false;
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
		//
		// **Two rows since v0.13, which is what makes it a ribbon.** The first
		// is the transport and which scene it is about — facts that are true
		// whatever somebody is doing, so they never move. The second is the
		// selected tab's controls, because what is wanted while placing geometry
		// and what is wanted while writing a script are different sets and
		// putting both on screen at once means neither is findable. Studio's
		// arrangement, and this is the reference `ROADMAP.md` points at.
		//
		// **Two item spacings rather than the one between the rows.** The tab
		// bar draws a border under itself and reserves a little of its own, and
		// the window is `NoScrollbar` — so a height that is a few pixels short
		// clips the bottom of the second row silently rather than growing a
		// scrollbar. One spacing of slack is cheaper than that.
		const ImGuiStyle &style = ImGui::GetStyle();
		const float height =
			ImGui::GetFrameHeight() * 2.0f + style.ItemSpacing.y * 2.0f + style.WindowPadding.y * 2.0f;

		constexpr ImGuiWindowFlags FLAGS = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
										   ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
										   ImGuiWindowFlags_NoScrollbar;

		if (!ImGui::BeginViewportSideBar("##toolbar", viewport, ImGuiDir_Up, height, FLAGS)) {
			ImGui::End();
			return;
		}

		// **The transport is about one scene: the one in the viewport you are
		// in.** Every button below reads and writes that world's run record, so
		// switching viewports genuinely swaps the transport rather than
		// relabelling it. A universe is a collection of scenes and each of them
		// runs, pauses and stops on its own — see `Editor::WorldRun`.
		const WorldId scope = ViewportWorld(FocusedViewport);
		const RunMode mode = ModeOf(scope);
		const bool running = mode != RunMode::Edit;
		const bool paused = IsPaused(scope);

		if (RunButton("Play", mode == RunMode::Play, engine::ui::AccentColour())) {
			SetRunMode(scope, mode == RunMode::Play ? RunMode::Edit : RunMode::Play);
		}
		ImGui::SameLine();
		if (RunButton("Run", mode == RunMode::Server, engine::ui::AccentColour())) {
			SetRunMode(scope, mode == RunMode::Server ? RunMode::Edit : RunMode::Server);
		}
		ImGui::SameLine();

		// **Pause between Run and Stop, and only while this scene runs.** It is
		// the transport a person reaches for mid-run, so it sits where a
		// transport does; disabled in Edit because there is no clock to stop —
		// a button that could be pressed and did nothing would read as a
		// pause that failed.
		ImGui::BeginDisabled(!running);
		if (RunButton(paused ? "Resume" : "Pause", paused, engine::ui::WarningColour())) {
			if (WorldRun *record = RunOf(scope); record != nullptr) {
				record->Paused = !record->Paused;
				Say(record->Paused ? "paused — the clock is stopped, the run is not" : "resumed");
			}
		}
		ImGui::SameLine();

		if (ImGui::Button("Stop")) {
			SetRunMode(scope, RunMode::Edit);
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

		// --- the second row --------------------------------------------------
		//
		// **The tab strip sits on the first row and its contents on the second**,
		// which is the arrangement of every ribbon and of the reference in
		// `ROADMAP.md`. Naming the tabs beside the transport keeps the row that
		// never changes short and gives the row that does the full width.
		//
		// **The tab bar owns which one is selected.** A member holding it would
		// be a second answer to a question imgui already answers, and the two
		// would disagree the first time anything else opened a tab.
		if (!ImGui::BeginTabBar("ribbon", ImGuiTabBarFlags_FittingPolicyScroll)) {
			ImGui::End();
			return;
		}

		// **Grouped by what somebody is doing, not by what the code is.** "Home"
		// is the loop somebody is in most of the day — pick a tool, set a step,
		// anchor the thing. "Model" is the operations on what is already
		// selected. "Script" is the programs. "View" is the furniture.
		//
		// Each strip is drawn *after* `EndTabBar` rather than inside its tab
		// item, because a tab item's contents go under the bar at the bar's own
		// width — and this row is the whole toolbar's width. So the tab item is
		// a label that answers "was I clicked", and `tab` carries the answer
		// down to the row below.
		int tab = -1;
		const auto item = [&tab](const char *label, int which) {
			if (ImGui::BeginTabItem(label)) {
				tab = which;
				ImGui::EndTabItem();
			}
		};

		item("Home", 0);
		item("Model", 1);
		item("Script", 2);
		item("View", 3);

		ImGui::EndTabBar();

		switch (tab) {
		case 0:
			DrawHomeTools();
			break;
		case 1:
			DrawModelTools();
			break;
		case 2:
			DrawScriptTools();
			break;
		case 3:
			DrawViewTools();
			break;
		default:
			// No tab is open only on the frame the bar is first submitted, and
			// a row that briefly drew nothing would be a toolbar that flickers
			// its height on startup.
			DrawHomeTools();
			break;
		}

		ImGui::End();
	}

	namespace {
		// What a zoom may be, and how far one press moves it.
		//
		// **Stated once because two panels zoom.** The script editor and the
		// output panel both scale their text, and two copies of these numbers
		// would be two panels that disagree about what a zoom level is the
		// first time either is tuned.
		//
		// The floor is where the vendored faces stop being readable and the
		// ceiling is where stretching the atlas's glyphs starts to look like a
		// mistake rather than a setting — `SetWindowFontScale` resizes what has
		// already been rasterised, so a long way from 1 goes soft in both
		// directions.
		constexpr float ZOOM_MINIMUM = 0.6f;
		constexpr float ZOOM_MAXIMUM = 3.0f;
		constexpr float ZOOM_STEP = 0.1f;
	}

	void Editor::ApplyZoomWheel(float &zoom) {
		// Guarded on hovering, so scrolling a tab bar or the panel around the
		// text does not resize it by accident.
		if (!ImGui::IsItemHovered() || !ImGui::GetIO().KeyCtrl) {
			return;
		}
		if (const float wheel = ImGui::GetIO().MouseWheel; wheel != 0.0f) {
			zoom = std::clamp(zoom + wheel * ZOOM_STEP, ZOOM_MINIMUM, ZOOM_MAXIMUM);
		}
	}

	void Editor::DrawZoomControl(float &zoom, const char *what) {
		// The zoom, beside the thing it applies to. A control nobody can find
		// is a control that only exists for people who already knew the wheel
		// did something.
		ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
		ImGui::Text("%.0f%%", static_cast<double>(zoom * 100.0f));
		ImGui::PopStyleColor();

		ImGui::SameLine();
		if (ImGui::SmallButton("-")) {
			zoom = std::clamp(zoom - ZOOM_STEP, ZOOM_MINIMUM, ZOOM_MAXIMUM);
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("+")) {
			zoom = std::clamp(zoom + ZOOM_STEP, ZOOM_MINIMUM, ZOOM_MAXIMUM);
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Reset")) {
			zoom = 1.0f;
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Ctrl+wheel over the %s, or Ctrl+= and Ctrl+-", what);
		}

		// **Focused rather than hovered, unlike the wheel.** A person reaching
		// for Ctrl+= has their hand on the keyboard and their pointer wherever
		// they left it; requiring them to hover the text as well would make the
		// shortcut work only by accident.
		if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)) {
			return;
		}

		// **Both the plus and the equals, because they are one key.** Ctrl and
		// shift-equals is what a person means by "zoom in" and what every
		// editor binds; requiring the shift would make the obvious press do
		// nothing.
		if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Equal) ||
			ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_KeypadAdd)) {
			zoom = std::clamp(zoom + ZOOM_STEP, ZOOM_MINIMUM, ZOOM_MAXIMUM);
		}
		if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Minus) ||
			ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_KeypadSubtract)) {
			zoom = std::clamp(zoom - ZOOM_STEP, ZOOM_MINIMUM, ZOOM_MAXIMUM);
		}
		if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_0)) {
			zoom = 1.0f;
		}
	}

	bool Editor::OutputSelected(uint64_t serial) const {
		if (OutputAnchor == 0 || OutputHead == 0) {
			return false;
		}
		const uint64_t first = std::min(OutputAnchor, OutputHead);
		const uint64_t last = std::max(OutputAnchor, OutputHead);
		return serial >= first && serial <= last;
	}

	size_t Editor::CopyOutputSelection() {
		std::string text;
		size_t copied = 0;

		for (const Message &message : Output) {
			if (!OutputSelected(message.Serial)) {
				continue;
			}

			// **Only what the filter is showing.** A copy that included hidden
			// lines would hand somebody text they cannot see on screen, and the
			// reason they filtered was to be rid of it.
			const bool isError = message.Level == LogLevel::Error;
			const bool isWarning = message.Level == LogLevel::Warning;
			if (isError ? !ShowErrors : isWarning ? !ShowWarnings : !ShowInfo) {
				continue;
			}
			if (!OutputFilter.empty()) {
				int score = 0;
				if (!FuzzyMatch(OutputFilter, message.Text, score)) {
					continue;
				}
			}

			if (!text.empty()) {
				text.push_back('\n');
			}
			text += message.Text;
			copied++;
		}

		if (copied > 0) {
			ImGui::SetClipboardText(text.c_str());
		}
		return copied;
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
			OutputAnchor = 0;
			OutputHead = 0;
		}

		// **Beside Clear, because a control nobody can find is one that only
		// exists for people who already knew the shortcut.** Disabled with
		// nothing picked rather than hidden, so its absence is never the reason
		// somebody thinks the panel cannot copy.
		ImGui::SameLine();
		ImGui::BeginDisabled(OutputAnchor == 0);
		if (ImGui::SmallButton("Copy")) {
			CopyOutputSelection();
		}
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Drag to select lines · Ctrl+C copies · Ctrl+A selects all");
		}

		// **Level toggles rather than a minimum level.** A minimum is the
		// obvious control and it is the wrong one: the thing somebody actually
		// wants is "errors and warnings, without the progress chatter", which a
		// threshold gives, and also "just the warnings", which it cannot.
		ImGui::SameLine();
		ImGui::TextDisabled("|");
		ImGui::SameLine();
		ImGui::Checkbox("info", &ShowInfo);
		ImGui::SameLine();
		ImGui::Checkbox("warnings", &ShowWarnings);
		ImGui::SameLine();
		ImGui::Checkbox("errors", &ShowErrors);

		ImGui::SameLine();
		ImGui::TextDisabled("|");
		ImGui::SameLine();
		DrawZoomControl(OutputZoom, "output");

		ImGui::SameLine();
		ImGui::SetNextItemWidth(-1.0f);

		// The same `FuzzyMatch` the explorer, the properties panel and the
		// palette filter with. Three notions of "matches what I typed" in one
		// program is three things to learn.
		TextField("##output-filter", OutputFilter, "filter output");

		ImGui::Separator();

		if (ImGui::BeginChild("##lines", ImVec2(0, 0), ImGuiChildFlags_None)) {
			// **Monospace, because this is a log.** A stack trace, a table of
			// numbers and a printed table all line up in one and none of them do
			// in a proportional face.
			const engine::ui::ScopedFont code(
				engine::ui::Typeface::Monospace, engine::ui::TextSize::Small
			);

			// **Scales the font rather than the interface.** `Options::Scale`
			// rebuilds every metric in the editor and needs a restart to
			// rasterise the faces at the new size; this is one panel's text,
			// and wanting a bigger stack trace is not wanting a bigger
			// properties panel.
			ImGui::SetWindowFontScale(OutputZoom);

			size_t shown = 0;

			for (const Message &message : Output) {
				const bool isError = message.Level == LogLevel::Error;
				const bool isWarning = message.Level == LogLevel::Warning;

				if (isError ? !ShowErrors : isWarning ? !ShowWarnings : !ShowInfo) {
					continue;
				}

				if (!OutputFilter.empty()) {
					int score = 0;
					if (!FuzzyMatch(OutputFilter, message.Text, score)) {
						continue;
					}
				}

				shown++;

				const unsigned int colour = isError	  ? engine::ui::ErrorColour()
											: isWarning ? engine::ui::WarningColour()
														: 0u;

				if (colour != 0u) {
					ImGui::PushStyleColor(ImGuiCol_Text, colour);
				}

				// **A `Selectable` rather than text, so a line can be picked.**
				// It spans the width of the panel, which is what a log wants
				// anyway: clicking anywhere on the row selects it rather than
				// only where the glyphs happen to reach.
				//
				// `AllowOverlap` so the drag below sees the row under the
				// pointer rather than only the one the press started on.
				ImGui::PushID(static_cast<int>(message.Serial));
				ImGui::Selectable(
					message.Text.c_str(),
					OutputSelected(message.Serial),
					ImGuiSelectableFlags_AllowOverlap
				);

				if (ImGui::IsItemClicked()) {
					// Shift extends from wherever the last press was, which is
					// what every list in every program does.
					OutputHead = message.Serial;
					if (!ImGui::GetIO().KeyShift) {
						OutputAnchor = message.Serial;
					}
				}
				if (ImGui::IsItemHovered() && ImGui::IsMouseDragging(ImGuiMouseButton_Left) &&
					OutputAnchor != 0) {
					// The drag. Moving the head rather than the anchor is what
					// lets a selection run upwards as readily as down.
					OutputHead = message.Serial;
				}
				ImGui::PopID();

				if (colour != 0u) {
					ImGui::PopStyleColor();
				}
			}

			// **Says what is hidden, not just what is shown.** A filtered log
			// that reports only its visible count looks like a log that lost
			// the line somebody is hunting for.
			if (shown != Output.size()) {
				ImGui::TextDisabled(
					"— %zu of %zu lines, %zu hidden by the filter",
					shown,
					Output.size(),
					Output.size() - shown
				);
			}

			// **Restored before the child ends.** The scale is a property of
			// the window rather than of the widget, so leaving it set would
			// zoom whatever the panel draws next as well.
			ImGui::SetWindowFontScale(1.0f);

			// Pinned to the bottom while the view is already there, and left
			// alone when it is not. Scrolling up to read an error and being
			// yanked back down by the next line is the thing that makes an
			// output panel useless.
			//
			// **Read before the wheel is consumed below.** Ctrl+wheel must not
			// also scroll the log, and imgui has already applied it to this
			// child by the time the panel sees it — so a zoom that left the
			// view where it was is one the person has to scroll back from.
			const bool pinned = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
			if (pinned) {
				ImGui::SetScrollHereY(1.0f);
			}
		}
		ImGui::EndChild();

		// The wheel belongs over the lines rather than over the toolbar, so it
		// is read against the child that holds them.
		ApplyZoomWheel(OutputZoom);

		// Ctrl+C and Ctrl+A, while the panel has focus.
		//
		// **Not guarded on hovering**, unlike the wheel: somebody who has just
		// dragged a selection has their hand on the keyboard and their pointer
		// wherever the drag ended, which may be outside the panel entirely.
		if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)) {
			if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C)) {
				CopyOutputSelection();
			}
			if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_A) && !Output.empty()) {
				OutputAnchor = Output.front().Serial;
				OutputHead = Output.back().Serial;
			}
		}

		ImGui::End();
	}

	void Editor::DrawStatusBar() {
		ImGuiViewport *viewport = ImGui::GetMainViewport();

		// **A side bar, for `DrawToolbar`'s reason and after the same bug.**
		// Positioning an ordinary window at the bottom of the work area puts it
		// under a dockspace covering that same rectangle, and
		// `NoBringToFrontOnFocus` then guarantees a docked panel wins. The strip
		// was submitted every frame, with correct numbers in it, and could not
		// be seen. `BeginViewportSideBar` reserves the row instead, so the
		// dockspace is laid out around it rather than over it.
		const float height = ImGui::GetFrameHeight();

		constexpr ImGuiWindowFlags FLAGS = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
										   ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
										   ImGuiWindowFlags_NoScrollbar;

		if (!ImGui::BeginViewportSideBar("##status", viewport, ImGuiDir_Down, height, FLAGS)) {
			ImGui::End();
			return;
		}

		// **How many scenes are live, not "the" mode.** With runs per world the
		// status bar cannot name one, and naming the focused viewport's would
		// hide a scene running behind you. A count is the honest summary; the
		// toolbar and the Worlds panel say which.
		const size_t live = Runs.size();
		const std::string state = live == 0	 ? std::string("Edit")
								  : live == 1 ? std::string(Describe(Runs.front().Mode)) + " (1 scene)"
											  : std::to_string(live) + " scenes running";

		ImGui::Text(
			"%s  |  %.0f fps  |  %u draw calls, %llu triangles, %u culled",
			state.c_str(),
			ImGui::GetIO().Framerate,
			LastFrame.DrawCalls,
			static_cast<unsigned long long>(LastFrame.Triangles),
			LastFrame.Culled
		);

		// **What is selected, which the explorer cannot say while you are
		// looking at the viewport.** It is the one fact about the current state
		// that has no other home once the eye is in the centre panel — the
		// explorer has it, and the explorer is the panel you are not reading.
		//
		// Drawn from the store every frame rather than from a cached name,
		// because a cached one is wrong for a frame after a rename and one
		// frame is enough to see.
		ImGui::SameLine();
		ImGui::TextDisabled("|");
		ImGui::SameLine();

		if (Selection.empty()) {
			ImGui::TextDisabled("nothing selected");
		} else if (Selection.size() > 1) {
			ImGui::Text("%zu selected", Selection.size());
		} else {
			const Entity only = Selection.front();
			Universe->Enter(SelectionWorld, [&](Store &store) {
				if (!store.Alive(only)) {
					ImGui::TextDisabled("selection is gone");
					return;
				}

				const engine::ecs::ClassId klass = store.ClassOf(only);
				const Name named = store.InstanceNameOf(only);
				if (klass.IsValid()) {
					ImGui::Text(
						"%s (%s)",
						named.IsValid() ? Label(named) : "?",
						Label(engine::ecs::Classes::Describe(klass).Name)
					);
				} else {
					ImGui::TextUnformatted(named.IsValid() ? Label(named) : "?");
				}
			});
		}

		ImGui::End();
	}

	namespace {
		// What each dialog lists. **From `game`'s own constants rather than
		// written out**, because the reader refuses a `<World>` where a `<Game>`
		// belongs and a browser offering the wrong one would be offering a file
		// that cannot load.
		const std::vector<std::string> GAME_FILES{std::string(engine::game::GAME_EXTENSION)};
		const std::vector<std::string> WORLD_FILES{std::string(engine::game::WORLD_EXTENSION)};

		// Rojo's own name for its project file, and the only one this opens.
		// A picker that offered every `.json` would invite somebody to point it
		// at a `package.json` and get an error rather than a filter.
		const std::vector<std::string> ROJO_FILES{".json"};
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
		if (AskingRojoUniverse) {
			ImGui::OpenPopup("Sync Rojo Universe");
		}
		if (AskingRojo) {
			ImGui::OpenPopup("Sync Rojo Project");
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

		if (FilePrompt("Save Game As", PathBuffer, "Save", GAME_FILES, false)) {
			SaveGame(std::filesystem::path(PathBuffer));
			AskingSaveAs = false;
		} else if (!ImGui::IsPopupOpen("Save Game As")) {
			AskingSaveAs = false;
		}

		if (FilePrompt("Open Game", PathBuffer, "Open", GAME_FILES, true)) {
			OpenGame(std::filesystem::path(PathBuffer));
			AskingOpen = false;
		} else if (!ImGui::IsPopupOpen("Open Game")) {
			AskingOpen = false;
		}

		if (FilePrompt("Sync Rojo Project", PathBuffer, "Sync", ROJO_FILES, true)) {
			SyncRojo(std::filesystem::path(PathBuffer));
			AskingRojo = false;
		} else if (!ImGui::IsPopupOpen("Sync Rojo Project")) {
			AskingRojo = false;
		}

		if (FilePrompt("Sync Rojo Universe", PathBuffer, "Sync", ROJO_FILES, true)) {
			SyncRojoWorlds(std::filesystem::path(PathBuffer));
			AskingRojoUniverse = false;
		} else if (!ImGui::IsPopupOpen("Sync Rojo Universe")) {
			AskingRojoUniverse = false;
		}

		if (FilePrompt("Export World", PathBuffer, "Export", WORLD_FILES, false)) {
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
		if (FilePrompt("Export Universe", PathBuffer, "Export", GAME_FILES, false)) {
			ExportUniverse(std::filesystem::path(PathBuffer));
			AskingExportUniverse = false;
		} else if (!ImGui::IsPopupOpen("Export Universe")) {
			AskingExportUniverse = false;
		}

		if (FilePrompt("Import Universe", PathBuffer, "Import", GAME_FILES, true)) {
			ImportUniverseFile(std::filesystem::path(PathBuffer));
			AskingImportUniverse = false;
		} else if (!ImGui::IsPopupOpen("Import Universe")) {
			AskingImportUniverse = false;
		}

		if (FilePrompt("Import World", PathBuffer, "Import", WORLD_FILES, true)) {
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
