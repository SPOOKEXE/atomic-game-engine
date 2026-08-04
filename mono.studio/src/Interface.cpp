#include <engine/core/Log.hpp>
#include <engine/core/Paths.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ui/Fonts.hpp>
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
		constexpr const char *DOCKSPACE = "StudioDockSpace.v4";

		constexpr const char *VIEWPORT = "Viewport";
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

			ImGui::DockBuilderDockWindow(VIEWPORT, centre);
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

		DrawMenuBar();
		DrawToolbar();
		DrawViewport();
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

	void Editor::DriveCamera() {
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
		const bool looking = (ViewportActive || (ViewportHovered && !io.WantCaptureMouse)) &&
							 ImGui::IsMouseDown(ImGuiMouseButton_Right);

		if (looking) {
			const float sensitivity = 0.0035f;
			CameraYaw -= io.MouseDelta.x * sensitivity;
			CameraPitch -= io.MouseDelta.y * sensitivity;

			// Clamped short of straight up and straight down. At exactly
			// vertical the yaw axis and the view direction are parallel and the
			// frame flips, which reads as the camera snapping.
			constexpr float LIMIT = 1.5533f;
			CameraPitch = std::clamp(CameraPitch, -LIMIT, LIMIT);
		}

		// The scroll wheel changes how fast, not how far. A wheel that dollied
		// the camera would make the speed control something you cannot find.
		if (looking && io.MouseWheel != 0.0f) {
			CameraSpeed = std::clamp(CameraSpeed * (io.MouseWheel > 0.0f ? 1.25f : 0.8f), 1.0f, 4096.0f);
		}

		const CFrame rotation = CFrame::Angles(CameraPitch, CameraYaw, 0.0f);
		const Vector3 forward = rotation.LookVector();
		const Vector3 right = rotation.RightVector();

		Vector3 move;
		if (looking && !io.WantCaptureKeyboard) {
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

		const float length = std::sqrt(move.X * move.X + move.Y * move.Y + move.Z * move.Z);
		Vector3 position = CameraFrame.Position;
		if (length > 0.0001f) {
			// **Wall time, and this is the one place in the program where that
			// is right.** An editor camera is not simulation: it must move at
			// the same rate whether or not the game is running, and tying it to
			// a fixed tick would freeze it in Edit mode where nothing ticks at
			// all.
			const float step = CameraSpeed * io.DeltaTime / length;
			position = position + move * step;
		}

		CameraFrame = CFrame(position, rotation.Rotation());
	}

	void Editor::DrawViewport() {
		if (!ShowViewport) {
			// Nothing asks for a texture, so the renderer releases the one it
			// had. A closed panel should not go on costing its pixels.
			WorldTarget = engine::render::SceneTarget{};
			return;
		}

		// No padding, so the image is the panel rather than a picture inside it.
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		const bool open = ImGui::Begin(
			"Viewport", &ShowViewport, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
		);
		ImGui::PopStyleVar();

		if (!open) {
			// Collapsed or behind another tab. The target is dropped rather than
			// left at its last size — rendering a texture nobody shows is a
			// frame's work thrown away every frame.
			WorldTarget = engine::render::SceneTarget{};
			ImGui::End();
			return;
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

		WorldTarget.Width = static_cast<uint32_t>(std::max(size.x, 1.0f) * horizontal);
		WorldTarget.Height = static_cast<uint32_t>(std::max(size.y, 1.0f) * vertical);

		// **Last frame's texture, and the one-frame lag is the design rather
		// than a bug.** imgui records its draw lists before the renderer runs,
		// so the only texture that exists when this executes is the one the
		// previous frame produced. `world::ViewChannel` made the same trade for
		// a hosted world and `SurfaceView` makes it for a mirror: a consumer a
		// frame behind is what removes the dependency cycle between "how big is
		// the panel" and "what is in the texture".
		if (void *texture = Renderer.SceneTexture(); texture != nullptr) {
			ImGui::Image(reinterpret_cast<ImTextureID>(texture), size);
		} else {
			// The first frame, and any frame after a resize the renderer has not
			// caught up with. An invisible button keeps the panel hoverable so
			// the camera does not stop working for a frame.
			ImGui::InvisibleButton("##surface", size, ImGuiButtonFlags_MouseButtonRight);
		}

		ViewportHovered = ImGui::IsItemHovered();
		ViewportActive = ImGui::IsItemActive();

		// The image is not a button, so a right-drag over it has to be claimed
		// explicitly or the panel behind would get it.
		if (ViewportHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
			ImGui::SetWindowFocus();
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

		const engine::core::Name scene = Universe->NameOf(Active);
		ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
		ImGui::Text(
			"%s   %u x %u   %u draw   %llu tris   %u culled",
			scene.IsValid() ? Label(scene) : "(no scene)",
			WorldTarget.Width,
			WorldTarget.Height,
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
		const ImGuiViewport *viewport = ImGui::GetMainViewport();

		// Pinned under the menu bar rather than docked, because a toolbar you
		// can accidentally drag into the corner is a toolbar somebody loses.
		ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y));
		ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, 0.0f));
		ImGui::SetNextWindowViewport(viewport->ID);

		constexpr ImGuiWindowFlags FLAGS = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
										   ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
										   ImGuiWindowFlags_NoBringToFrontOnFocus |
										   ImGuiWindowFlags_AlwaysAutoResize;

		if (!ImGui::Begin("##toolbar", nullptr, FLAGS)) {
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

		ImGui::BeginDisabled(!running);
		if (ImGui::Button("Stop")) {
			SetRunMode(RunMode::Edit);
		}
		ImGui::EndDisabled();

		ImGui::SameLine();
		ImGui::TextDisabled("|");
		ImGui::SameLine();

		// The scene selector, because a universe with several worlds needs one
		// click to switch and a menu is two.
		ImGui::SetNextItemWidth(180.0f * Settings.Scale);
		const Name activeName = Universe->NameOf(Active);
		if (ImGui::BeginCombo("##scene", activeName.IsValid() ? Label(activeName) : "(no scene)")) {
			for (const WorldId id : Universe->Worlds()) {
				const Name name = Universe->NameOf(id);
				if (ImGui::Selectable(name.IsValid() ? Label(name) : "?", id == Active)) {
					Active = id;
					SelectionWorld = id;
					ClearSelection();
				}
			}
			ImGui::EndCombo();
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
