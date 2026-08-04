#include <engine/core/Log.hpp>
#include <engine/core/Paths.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ui/Theme.hpp>

#include <algorithm>
#include <cstring>
#include <imgui.h>
#include <imgui_internal.h>
#include <studio/Editor.hpp>
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
		constexpr const char *DOCKSPACE = "StudioDockSpace.v2";

		constexpr const char *VIEWPORT = "Viewport";
		constexpr const char *EXPLORER = "Explorer";
		constexpr const char *PROPERTIES = "Properties";
		constexpr const char *WORLDS = "Worlds";
		constexpr const char *SCRIPTS = "Script Editor";
		constexpr const char *OUTPUT = "Output";

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

			ImGui::DockBuilderFinish(dockspace);
		}
	}

	void Editor::DrawInterface() {
		const ImGuiViewport *viewport = ImGui::GetMainViewport();

		// **`PassthruCentralNode` is what makes the viewport a hole rather than
		// a panel.** The dockspace host covers the whole window; without this
		// flag its central node is drawn opaque and paints over the world the
		// editor exists to show. `ui::Theme` keeps `DockingEmptyBg` transparent
		// for the same reason, and `ui/tests/Theme.cpp` checks it.
		const ImGuiID dockspace = ImGui::DockSpaceOverViewport(
			ImGui::GetID(DOCKSPACE), viewport, ImGuiDockNodeFlags_PassthruCentralNode
		);

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
		DrawStatusBar();
		DrawDialogs();

		DriveCamera();
		DrawShortcuts();
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
		// **A window with no background, and that is the whole trick.** The
		// world is already in the swapchain by the time imgui's draw lists are
		// recorded — `render::Pass::Interface` is last — so a panel that paints
		// nothing lets it through. What that buys over the bare hole this used
		// to be is everything a panel has: a name, a tab, a dock target, a
		// rectangle somebody can drag, and somewhere to put the readout below.
		//
		// The alternative is rendering the world into a texture and showing it
		// as an image. That is the honest way to have several viewports at once
		// and it is the render-node system's job — this pipeline draws one
		// screen pass, and building a second path for it here would be the
		// small-version-of-a-big-thing `render/Renderer.hpp` argues against.
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

		const bool open = ImGui::Begin(
			"Viewport", nullptr, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar |
									 ImGuiWindowFlags_NoScrollWithMouse
		);

		ImGui::PopStyleVar();

		if (!open) {
			// Collapsed or tabbed behind something. The world is drawn into
			// nothing rather than into wherever the panel last was — a stale
			// rectangle would leave the previous frame's image sitting under a
			// panel that is no longer there.
			WorldViewport = engine::render::Viewport{};
			ImGui::End();
			return;
		}

		// **The content rectangle, in pixels rather than in imgui's logical
		// points.** The swapchain is sized in pixels and a high-DPI display
		// makes those different numbers — a viewport handed logical points on a
		// 2x display would draw the world into the top-left quarter of the
		// panel and leave the rest showing the clear colour.
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		const ImVec2 size = ImGui::GetContentRegionAvail();
		const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
		const float horizontal = scale.x > 0.0f ? scale.x : 1.0f;
		const float vertical = scale.y > 0.0f ? scale.y : 1.0f;

		const ImGuiViewport *host = ImGui::GetMainViewport();
		WorldViewport.X = static_cast<int>((origin.x - host->Pos.x) * horizontal);
		WorldViewport.Y = static_cast<int>((origin.y - host->Pos.y) * vertical);
		WorldViewport.Width = static_cast<int>(size.x * horizontal);
		WorldViewport.Height = static_cast<int>(size.y * vertical);

		// An invisible button over the whole rectangle, so the panel has an
		// item to be hovered and a drag to be captured. Without one, imgui
		// reports the window hovered only while the cursor is over its
		// decoration — and the camera would stop turning the moment the drag
		// left the title bar.
		ImGui::InvisibleButton("##surface", size, ImGuiButtonFlags_MouseButtonRight);
		ViewportHovered = ImGui::IsItemHovered();
		ViewportActive = ImGui::IsItemActive();

		// The readout, drawn back at the top-left over the world.
		ImGui::SetCursorScreenPos(ImVec2(origin.x + 10.0f, origin.y + 8.0f));
		ImGui::BeginGroup();

		const engine::core::Name scene = Universe->NameOf(Active);
		ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
		ImGui::Text(
			"%s   %d x %d   %u draw   %llu tris   %u culled",
			scene.IsValid() ? Label(scene) : "(no scene)",
			WorldViewport.Width,
			WorldViewport.Height,
			LastFrame.DrawCalls,
			static_cast<unsigned long long>(LastFrame.Triangles),
			LastFrame.Culled
		);
		ImGui::PopStyleColor();

		if (Mode != RunMode::Edit) {
			// **The one thing that must be visible without reading anything.**
			// An author who has forgotten they are in Play will make edits that
			// Stop throws away, and "why did my change vanish" is the worst
			// question an editor can produce.
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::WarningColour());
			ImGui::Text("%s — Stop restores the scene", Describe(Mode));
			ImGui::PopStyleColor();
		}

		if (ViewportActive) {
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
			ImGui::Text("WASD / QE   %.0f u/s   wheel to change", static_cast<double>(CameraSpeed));
			ImGui::PopStyleColor();
		}

		ImGui::EndGroup();
		ImGui::End();
	}

	void Editor::DrawMenuBar() {
		if (!ImGui::BeginMainMenuBar()) {
			return;
		}

		if (ImGui::BeginMenu("File")) {
			if (ImGui::MenuItem("New Game", "Ctrl+N")) {
				NewGame();
			}
			if (ImGui::MenuItem("Open Game...", "Ctrl+O")) {
				AskingOpen = true;
				PathBuffer = GamePath.string();
			}

			ImGui::Separator();

			if (ImGui::MenuItem("Save", "Ctrl+S")) {
				if (GamePath.empty()) {
					AskingSaveAs = true;
					PathBuffer = std::string(Label(GameName)) + std::string(engine::game::GAME_EXTENSION);
				} else {
					SaveGame(GamePath);
				}
			}
			if (ImGui::MenuItem("Save As...")) {
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

			ImGui::Separator();

			if (ImGui::MenuItem("Exit", "Alt+F4")) {
				Running = false;
			}
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Edit")) {
			if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, !Selection.empty())) {
				DuplicateSelection();
			}
			if (ImGui::MenuItem("Delete", "Del", false, !Selection.empty())) {
				DeleteSelection();
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Select None", "Esc", false, !Selection.empty())) {
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

		if (ImGui::BeginMenu("Run")) {
			if (ImGui::MenuItem("Play (server + client)", "F5", Mode == RunMode::Play)) {
				SetRunMode(Mode == RunMode::Play ? RunMode::Edit : RunMode::Play);
			}
			if (ImGui::MenuItem("Run (server only)", "F6", Mode == RunMode::Server)) {
				SetRunMode(Mode == RunMode::Server ? RunMode::Edit : RunMode::Server);
			}
			if (ImGui::MenuItem("Stop", "Shift+F5", false, Mode != RunMode::Edit)) {
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

		if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) {
			if (GamePath.empty()) {
				AskingSaveAs = true;
				PathBuffer = std::string(Label(GameName, "Untitled")) +
							 std::string(engine::game::GAME_EXTENSION);
			} else {
				SaveGame(GamePath);
			}
		}

		if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_N)) {
			NewGame();
		}

		if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_D) && !Selection.empty()) {
			DuplicateSelection();
		}

		if (ImGui::IsKeyPressed(ImGuiKey_Delete) && !Selection.empty()) {
			DeleteSelection();
		}

		if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !Selection.empty()) {
			ClearSelection();
		}

		// Shift+F5 stops, F5 toggles Play — Studio's bindings, including the part
		// where Shift+F5 stops rather than doing nothing when already stopped.
		if (ImGui::IsKeyPressed(ImGuiKey_F5)) {
			const bool stopping = ImGui::IsKeyDown(ImGuiKey_LeftShift) ||
								  ImGui::IsKeyDown(ImGuiKey_RightShift) || Mode == RunMode::Play;
			SetRunMode(stopping ? RunMode::Edit : RunMode::Play);
		}

		if (ImGui::IsKeyPressed(ImGuiKey_F6)) {
			SetRunMode(Mode == RunMode::Server ? RunMode::Edit : RunMode::Server);
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
		if (!ImGui::Begin(OUTPUT)) {
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
