#pragma once

// The editor: a universe you can see, a tree you can rearrange, and a game file
// at the end of it.
//
// **`RunService:IsStudio()` is true here and in no other program.**
// `script/Runtime.hpp` has carried that sentence since v0.6, with the default
// set to false on the grounds that editor-only behaviour must never appear in a
// shipped game because a default was optimistic. This is the thing that sets
// it.
//
// **The universe is the game and a world is a scene.** `Bindings.hpp` already
// maps `game` to `world::Universe` and `workspace` to the world a script runs
// on; the explorer draws exactly that, so what an author sees in the tree and
// what a script reaches through `game` are the same object rather than two
// models kept in step.
//
// **Nothing in this program is world state.** Selection, expansion, which
// panels are open, where the camera is — none of it enters a store, crosses a
// bus, or reaches a snapshot. That is `ui/AGENTS.md`'s rule and it is the
// reason Stop can throw the whole universe away and restore it without the
// editor losing its place.
//
// **Run and Play are Roblox's, including the part people forget.** Pressing
// Play takes a snapshot, runs the game, and pressing Stop *restores the
// snapshot* — so an author's scene is not whatever their scripts left behind.
// `world::Universe::Save` and `Load` are exactly that operation and already
// exist; this program is the first caller with a reason to use them.

#include <engine/core/Clock.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/game/Game.hpp>
#include <engine/render/DebugPanels.hpp>
#include <engine/render/FrameStatistics.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/ui/Interface.hpp>
#include <engine/world/Universe.hpp>

#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct SDL_Window;

namespace studio {

	using engine::ecs::Entity;
	using engine::world::WorldId;

	// What the editor is doing to the game right now.
	//
	// @since v0.7
	enum class RunMode : uint8_t {
		// Nothing is running. Scripts are text, the universe does not tick, and
		// every edit is authoring.
		//
		// **The universe genuinely does not tick**, which is worth stating
		// because the alternative looks harmless: a world that simulated while
		// being edited would settle physics under an author's hands, so a part
		// placed in the air would be on the floor by the time they looked away.
		Edit,

		// The server's scripts run — `Script`, not `LocalScript` — with
		// `IsServer()` true and `IsClient()` false. Roblox's "Run".
		Server,

		// Both halves in one process, the arrangement `HostRole::OfBoth`
		// describes and `mono.unified_server_client` proves. Roblox's "Play".
		Play,
	};

	// A stable name for a mode, for a menu and a log line.
	//
	// @param mode The mode.
	// @return A view valid for the lifetime of the process.
	const char *Describe(RunMode mode);

	// One line in the output panel.
	//
	// @since v0.7
	struct Message {
		// What it says.
		std::string Text;

		// Whether it is an error, a warning or ordinary progress. Drawn as a
		// colour, which is the only thing the panel does with it.
		engine::core::LogLevel Level = engine::core::LogLevel::Info;
	};

	// Everything the command line decides.
	//
	// @since v0.7
	struct Options {
		// Window width in logical pixels, before display scaling.
		int Width = 1600;

		// Window height in logical pixels. The window is resizable, so this is
		// where it starts and not where it stays.
		int Height = 900;

		// The game file to open at start-up, or empty for a new game.
		std::filesystem::path Game;

		// Read staged data from here instead of from beside the binary.
		std::filesystem::path Assets;

		// Simulation ticks per second while running.
		double TickRate = 60.0;

		// Everything imgui draws is multiplied by this. See
		// `ui::InterfaceSettings::Scale`.
		float Scale = 1.0f;

		// -1 runs until the window is closed. A frame budget is what makes the
		// editor usable from a test or a CI job — and what lets a capture be
		// taken of a known frame rather than of whenever somebody looked.
		int64_t MaximumFrames = -1;

		// Which mode to start in, rather than Edit.
		//
		// **What makes the run cycle testable without a person.** A headless run
		// that could only sit in Edit mode would exercise loading and rendering
		// and nothing else; starting in Play runs the game's scripts, ticks its
		// worlds and draws the result — which is the half a capture can check.
		RunMode StartIn = RunMode::Edit;

		// Open the statistics and frame-graph panels at start-up.
		//
		// **The client's two flags, spelled the same.** `client --stats
		// --graph` is how the demo is run with both panels open, and an editor
		// that needed a different pair of words for the same two panels would
		// be a second thing to remember. They also make the panels reachable
		// without a keyboard, which is what lets a capture prove they draw.
		bool ShowStatistics = false;
		bool ShowFrameGraph = false;

		// Run with no window at all.
		//
		// **What makes the editor drivable by something that is not a person.**
		// The whole state machine still runs — the game loads, the panels lay
		// themselves out, Play starts the scripts, the world renders — and none
		// of it needs a display, a compositor or an unattended machine to be
		// left alone. A scripted control or an agent gets the same editor and
		// reads `--capture` instead of a screen.
		//
		// Needs `--frames`, because a headless run has no window to close.
		bool Headless = false;

		// Write the world the viewport is showing to this file, then carry on.
		//
		// **Taken on the last frame of a `--frames` budget**, so a run that asks
		// for both produces exactly one image of a known frame. On its own it is
		// taken once, a few frames in — early enough to be useful and late
		// enough that the layout and the first scene have settled.
		std::filesystem::path Capture;
	};

	// One open script tab.
	//
	// @since v0.7
	struct OpenScript {
		// The world it lives in, because two worlds may hold scripts with the
		// same path and closing one must not save over the other.
		WorldId World;

		// The instance, so the tab can follow a rename and close itself when
		// the script is deleted.
		Entity Instance;

		// The asset-relative path its text is filed under.
		engine::core::Name Path;

		// The text being edited. Written back into the world's
		// `script::SourceCache` on save, and nowhere else — an editor that
		// wrote to the store on every keystroke would make undo the
		// filesystem's problem.
		std::string Text;

		// Whether the buffer differs from what the world holds.
		bool Modified = false;
	};

	// The window, the renderer, the interface and the game.
	//
	// @since v0.7
	class Editor {
	  public:
		Editor();
		~Editor();

		Editor(const Editor &) = delete;
		Editor &operator=(const Editor &) = delete;

		// Opens the window, starts the renderer and the interface, and either
		// loads `options.Game` or starts a new one.
		//
		// @param options Parsed command line. Copied, not referenced.
		// @return `false` when SDL, the window, the renderer or the interface
		//         would not start. The reason is logged.
		bool Initialise(const Options &options);

		// Tears everything down, in the reverse of the order it came up.
		void Shutdown();

		// Runs until the window is closed or the frame budget is spent.
		//
		// @return The process exit code.
		int Run();

	  private:
		// --- the frame ------------------------------------------------------

		// Which frame `--capture` is taken on. See the body.
		//
		// @return The frame number to request a capture on.
		int64_t CaptureAtFrame() const;

		void PumpEvents();
		void Simulate(float frameSeconds);
		void Present(float frameSeconds);

		// The frame rate, and where the frame went.
		//
		// **Ordinary panels rather than the client's overlay.**
		// `render::DrawDebugPanels` writes into a `render::OverlayImage`, which
		// the dockspace is drawn over — so panels written there are painted
		// out by imgui every frame. They read the same `core::FrameGraph` the
		// client's overlay does; only the drawing differs.
		void DrawStatistics();
		void DrawFrameGraph();

		// Records the frame time and turns span collection on or off.
		//
		// @param frameSeconds The frame just measured.
		void SampleFrame(float frameSeconds);

		// The half of a frame that is the world rather than the editor.
		//
		// Split out when headless arrived: a run with no window still presents
		// its worlds, collects their draw lists and renders them into a target
		// — it just has no panels to draw over the top.
		void PresentWorld(float frameSeconds);

		// --- the interface ---------------------------------------------------

		void DrawInterface();
		void DrawMenuBar();
		void DrawToolbar();
		void DrawViewport();
		void DrawExplorer();
		void DrawWorlds();
		void DrawProperties();
		void DrawScripts();
		void DrawOutput();

		// The editor's own settings, in pages.
		//
		// **A panel rather than a modal**, because choosing a theme means
		// looking at the editor while it changes — and a modal is a rectangle
		// over the thing being judged.
		void DrawSettings();

		// The theme picker and the interface scale.
		void DrawAppearanceSettings();

		// The searchable list of what every key does, and where they are
		// changed. See `studio::Keybinds` — this page edits that table
		// directly, so it cannot drift from what the keys actually do.
		void DrawKeybindSettings();

		void DrawStatusBar();
		void DrawDialogs();

		// The View menu, and the only way back to a panel somebody closed.
		//
		// **Every panel is closable and every panel is dockable**, which is what
		// makes the layout theirs rather than ours — and a closable panel with
		// no way back is a panel somebody loses permanently.
		void DrawViewMenu();

		// Keyboard shortcuts, read after every panel has drawn.
		//
		// See the body: `io.WantTextInput` is only meaningful once whatever
		// field is going to claim the keyboard has claimed it, which is why this
		// is not part of `DrawMenuBar`.
		void DrawShortcuts();

		// The editor camera, driven from the mouse and the keyboard.
		//
		// Called from inside the imgui frame rather than beside it, because
		// whether a click belongs to the world or to a panel is a question only
		// imgui can answer and only while a frame is open.
		void DriveCamera();

		// One instance and its subtree, in the explorer.
		void DrawTreeNode(engine::ecs::Store &store, WorldId world, Entity instance);

		// The right-click menu shared by the tree and the Insert menu.
		//
		// One function, because two menus offering different subsets of the
		// same actions is how "Duplicate is on one of them" happens.
		void DrawInstanceActions(engine::ecs::Store &store, WorldId world, Entity instance);

		// The searchable Insert Object submenu, for every menu that offers one.
		//
		// **Takes no `Store`, which is what lets a world's menu have one.** An
		// instance's menu is drawn from inside `Universe::Enter`; a world's row
		// and the universe root are drawn from outside it. A helper that needed
		// a store could only serve the first, and that asymmetry is exactly why
		// Insert Object used to be missing from the other two.
		//
		// @param id     Distinguishes this menu's search box from the others.
		// @param world  The world the new instance goes into.
		// @param parent What to parent it to, or a null entity for a root.
		void DrawInsertMenu(const char *id, WorldId world, Entity parent);

		// The searchable class list. Returns the class chosen, or an invalid id.
		engine::ecs::ClassId DrawClassPicker(const char *id);

		// The menu one row of the Worlds panel offers.
		//
		// @param world The scene the menu was opened on.
		void DrawWorldActions(WorldId world);

		// Applies what the Worlds panel queued, from outside `Universe::Enter`.
		void ApplyPendingWorldActions();

		// The world with this slot index, or an invalid handle.
		//
		// **A `WorldId` cannot be rebuilt from an index alone** — the handle
		// carries more than the slot, and `Universe::Adopt` reuses slots — so
		// the drag payload carries the index and this resolves it against the
		// live list rather than reconstructing one.
		//
		// @param index The slot a drag payload carried.
		// @return The handle, or an invalid one when no world holds that slot.
		WorldId WorldFor(uint32_t index) const;

		// Moves an instance and its subtree into another world.
		//
		// **Written out and rebuilt, because a handle does not cross.** The
		// subtree goes through `game::WriteInstanceDocument`, is read into the
		// destination and is destroyed in the source — so what survives is
		// exactly what a save file preserves, and a property pointing at
		// something left behind comes back at its default with a warning.
		//
		// @param source   The world it is in.
		// @param instance The subtree's root.
		// @param target   The world it is going to.
		// @param parent   What to parent it to there, or null for a root.
		// @return `false` when it could not be written, rebuilt, or either
		//         world is remote. The source is left untouched on failure.
		bool MoveInstanceToWorld(WorldId source, Entity instance, WorldId target, Entity parent);

		// How many instances a scene holds, recounted on a clock.
		//
		// @param world The scene.
		// @return The count, at most half a second old.
		size_t InstanceCountOf(WorldId world);

		// Copies a scene, under a free name derived from the original.
		//
		// @param source The scene to copy.
		// @return `false` when it could not be written or read back.
		bool DuplicateWorld(WorldId source);

		// Renames a scene by writing it out and reading it back.
		//
		// **`world::Universe` has no rename and should not grow one**: a world's
		// name is what a bus envelope, a subscription and a teleport carry, so
		// renaming a live world strands its pending traffic. In Edit mode there
		// is none, which is why this is refused while a game is running.
		//
		// @param world  The scene to rename.
		// @param wanted The new name.
		// @return `false` when the name is taken or the round trip failed.
		bool RenameWorld(WorldId world, engine::core::Name wanted);

		// Applies everything the tree queued, from outside `Universe::Enter`.
		//
		// **The explorer draws a world's tree from inside `Enter` and every
		// action it offers enters a world itself.** `Universe::Enter` aborts on
		// re-entry rather than allowing it — that is the affinity check doing
		// its job, and it is the only thing standing between a scoped store
		// reference and a data race. So a panel records what was asked for and
		// this applies it once, outside.
		void ApplyPendingActions();

		// --- selection -------------------------------------------------------

		void Select(WorldId world, Entity instance, bool add);
		void ClearSelection();
		bool IsSelected(Entity instance) const;

		// --- actions ----------------------------------------------------------

		void NewGame();
		bool OpenGame(const std::filesystem::path &path);
		bool SaveGame(const std::filesystem::path &path);
		bool ExportActiveWorld(const std::filesystem::path &path);

		// Writes the universe and every world in it, without adopting the path.
		//
		// **Separate from Save As rather than a second name for it.** Save As
		// adopts: the title changes, the modified marker clears and Ctrl+S
		// writes there from then on. An export is a copy handed to somebody
		// else, and an author who exported one and then pressed Ctrl+S would
		// otherwise have overwritten the copy instead of their own file.
		//
		// @param path Where to write.
		// @return `false` when the file would not be written.
		bool ExportUniverse(const std::filesystem::path &path);
		bool ImportWorldFile(const std::filesystem::path &path);

		WorldId AddWorld(engine::core::Name name);
		void RemoveWorld(WorldId world);

		Entity InsertInstance(WorldId world, engine::ecs::ClassId klass, Entity parent);
		void DeleteSelection();
		void DuplicateSelection();

		void OpenScriptTab(WorldId world, Entity instance);
		void SaveScriptTab(OpenScript &tab);
		void CloseScriptTab(size_t index);

		// --- running -----------------------------------------------------------

		void SetRunMode(RunMode mode);

		// Takes the snapshot Stop restores, and starts a runtime per world.
		//
		// @return `false` when the universe could not be written, which is a
		//         refusal to start rather than a run that cannot be undone.
		bool BeginRun(RunMode mode);

		// Destroys the runtimes and restores the snapshot.
		void EndRun();

		// --- state ------------------------------------------------------------

		void Say(std::string text, engine::core::LogLevel level = engine::core::LogLevel::Info);
		void MarkModified();
		std::string TitleText() const;

		Options Settings;

		SDL_Window *Window = nullptr;
		engine::render::Renderer Renderer;
		engine::render::OverlayImage Overlay;
		engine::ui::Interface Interface;
		engine::core::FrameClock Clock;

		// Held by pointer because a universe binds its driver thread on
		// construction, and that thread is decided in `Initialise` rather than
		// wherever this object was declared. Same reason `client::Client` holds
		// its own that way.
		std::unique_ptr<engine::world::Universe> Universe;

		// The game's name and where it came from. An empty path is a game that
		// has never been saved, which is what makes Save fall through to Save
		// As rather than writing somewhere arbitrary.
		engine::core::Name GameName;
		std::filesystem::path GamePath;
		bool Modified = false;

		// The world the viewport draws and the properties panel edits.
		WorldId Active;

		// What is selected, and which world it is in. Cleared whenever `Active`
		// changes, because a selection in a world nobody is looking at is a
		// delete somebody does not see.
		WorldId SelectionWorld;
		std::vector<Entity> Selection;

		// Instances the explorer has been told to expand, by entity id.
		//
		// A set of ids rather than a flag on the instance, because expansion is
		// not world state — see the header comment. An id that no longer names
		// anything is harmless and is dropped the next time the tree is walked.
		std::vector<uint64_t> Expanded;

		RunMode Mode = RunMode::Edit;

		// The universe as it was when Run was pressed. Restored by Stop.
		std::vector<std::byte> EditSnapshot;

		// One VM per world while running, in `Universe::Worlds()` order.
		//
		// **Shared rather than unique**, because the heartbeat system installed
		// into a world's scheduler captures it — the same arrangement
		// `examples::LoadScene` uses, and for the same reason: a script that
		// connects to `RunService.Heartbeat` *is* the simulation for what it
		// built, so the VM has to outlive the call that started it.
		std::vector<std::shared_ptr<engine::script::Runtime>> Runtimes;

		std::vector<OpenScript> Scripts;
		int ActiveScript = -1;

		std::deque<Message> Output;

		// The engine log, teed into that panel. See `PanelSink` in `Editor.cpp`:
		// a script's `print` and the error that stopped it are the two things an
		// author actually wants there, and both arrive through the logger rather
		// than through this class.
		std::shared_ptr<class PanelSink> Sink;

		// --- the viewport ------------------------------------------------------

		// Where the eye is. Not an entity in any world: a camera that lived in
		// the scene would be saved into the game file, replicated, and reset by
		// Stop — three things an editor camera must not do.
		engine::core::CFrame CameraFrame;
		float CameraYaw = 0.0f;
		float CameraPitch = 0.0f;
		float CameraSpeed = 24.0f;

		// How big a texture the world is drawn into, from the viewport panel's
		// content rectangle. See `render::SceneTarget`.
		engine::render::SceneTarget WorldTarget;

		// --- dialogs -----------------------------------------------------------
		//
		// Modal state, held here rather than in statics inside the drawing
		// functions. A static would be shared between two editors in one
		// process, which is a thing a test does.

		bool AskingSaveAs = false;
		bool AskingOpen = false;
		bool AskingExport = false;
		bool AskingExportUniverse = false;
		bool AskingImport = false;
		bool AskingNewWorld = false;
		std::string PathBuffer;
		std::string NameBuffer;

		// What the explorer's, the properties panel's and the keybind page's
		// filter boxes hold.
		std::string ExplorerFilter;
		std::string PropertyFilter;
		std::string KeybindFilter;

		// Which action is waiting for a key, as a `studio::Action` cast to int,
		// or -1 for none.
		//
		// An `int` rather than the enum so that `Keybinds.hpp` — and through it
		// imgui — stays out of this header.
		int RebindingAction = -1;

		// --- queued actions -------------------------------------------------
		//
		// See `ApplyPendingActions`. Every one of these is something a panel
		// asked for while it was inside `Universe::Enter` and could not do
		// there.

		struct PendingInsertAction {
			WorldId World;
			engine::ecs::ClassId Class;
			Entity Parent;
		};

		struct PendingReparentAction {
			WorldId World;
			Entity Instance;
			Entity Parent;
		};

		// A reparent whose two ends are in different worlds.
		//
		// **A separate action from `PendingReparentAction`, because it is a
		// separate operation.** Within a world a move is a `SetParent` and the
		// instance keeps its handle. Across two it cannot: an `ecs::Entity` is
		// an index into one store, so the subtree is described, rebuilt on the
		// far side and the original destroyed. The handle changes, which means
		// selection, open script tabs and anything else holding one have to be
		// told — and a single struct covering both would make that
		// "sometimes".
		struct PendingMoveAction {
			WorldId Source;
			WorldId Target;
			Entity Instance;

			// What to parent it to in `Target`, or null for a root there.
			Entity Parent;
		};

		struct PendingScriptAction {
			WorldId World;
			Entity Instance;
		};

		// Whether the pointer is over the viewport panel, and whether a drag
		// that started there is still in flight. Two questions rather than one:
		// a look that leaves the rectangle is still that look, and a camera
		// that stopped at the edge would be unusable exactly when somebody is
		// turning quickly.
		bool ViewportHovered = false;
		bool ViewportActive = false;

		// Which panels are open. Not saved here — imgui's ini remembers it, and
		// a second copy would be the drift rule 2 is about. These are the
		// current frame's answer, passed to `ImGui::Begin` and read back.
		bool ShowViewport = true;
		bool ShowExplorer = true;
		bool ShowWorlds = true;
		bool ShowProperties = true;
		bool ShowScripts = true;
		bool ShowOutput = true;

		// Closed by default: it is a panel somebody opens to change one thing.
		bool ShowSettings = false;

		// --- the debug panels -------------------------------------------------
		//
		// **Drawn into the overlay rather than as imgui windows, and reused
		// rather than rebuilt.** `render::DrawDebugPanels` is what the client
		// draws with, so the editor's frame graph is the same panel with the
		// same columns — an fps figure that meant something slightly different
		// in two programs is worse than not having one in the editor.
		//
		// **No keys of their own.** They are panels like every other panel:
		// opened from the View menu, docked where somebody wants them, closed
		// with their own button. The client's F3/F5 could not be reused here —
		// F5 is Play — and inventing a second set of function keys for the
		// editor was a decision worth not making twice.
		bool ShowStatistics = false;
		bool ShowFrameGraph = false;

		// Frame times, sampled every frame so the panel has history the moment
		// it is opened rather than starting empty.
		engine::render::FrameStatistics Statistics;

		// Whether the next frame rebuilds the default arrangement. Set from the
		// View menu and acted on at the top of the frame, because rearranging a
		// dockspace's nodes from inside a menu is rearranging the tree that is
		// being walked.
		bool ResetLayout = false;

		PendingInsertAction PendingInsert;
		PendingReparentAction PendingReparent;
		PendingMoveAction PendingMove;
		PendingScriptAction PendingOpenScript;
		WorldId PendingRemoveWorld;
		WorldId PendingActivate;
		WorldId PendingDuplicateWorld;
		WorldId PendingRenameWorld;
		std::string PendingRenameTo;
		bool PendingDuplicate = false;
		bool PendingDelete = false;

		// The scene the rename prompt is for, and whether it is open. Held
		// apart from `PendingRenameWorld` because the prompt spans frames and
		// the action is applied on exactly one.
		WorldId RenamingWorld;
		bool AskingRenameWorld = false;

		// One cached instance count per scene. See `InstanceCountOf`.
		struct InstanceCount {
			// The scene it counted.
			WorldId World;

			// How many instances it held.
			size_t Count = 0;

			// When the count was taken, on the frame clock.
			double Taken = 0.0;
		};

		std::vector<InstanceCount> InstanceCounts;

		bool Running = false;
		int64_t FramesDrawn = 0;
		engine::render::FrameResult LastFrame;
	};
}
