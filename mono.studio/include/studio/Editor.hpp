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
#include <engine/scene/Components.hpp>
#include <engine/render/FrameStatistics.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/ui/Interface.hpp>
#include <engine/world/Universe.hpp>
#include <nlohmann/json_fwd.hpp>
#include <engine/control/Server.hpp>
#include <engine/control/Surface.hpp>
#include <studio/PlayLink.hpp>

#include <cstdint>
#include <array>
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

		// The loopback port the control server listens on, or -1 for off.
		//
		// **Off by default, and that is the security boundary.** This surface
		// runs scripts, writes properties and saves files on behalf of whatever
		// connects to it. A port that opened itself because the editor started
		// would be one nobody chose; `--mcp-port` is the choosing.
		//
		// Zero means "any free port", which is what a launcher wants — the one
		// actually bound is logged and reported by `engine_info`.
		int ControlPort = -1;

		// Open the second viewport at start-up.
		//
		// **Reachable without a person, for `Headless`'s reason.** The panels
		// run headless and only the drawing is skipped, so a flag is what lets
		// a capture prove that two viewports really do draw two worlds — a
		// thing no screenshot can show on a machine somebody else is using.
		bool ShowSecondViewport = false;

		// Write a frame-graph snapshot here when the run ends.
		//
		// **What makes the editor's own frame profilable without a person
		// watching it.** The panel shows the frame in front of you, which is no
		// use for a cost that only appears while a window is being dragged —
		// the hands are busy and the panel is being resized along with
		// everything else. A snapshot carries the recent *worst case* per span,
		// so a run that was resized throughout says which span the resizing
		// went into.
		//
		// Implies the frame graph: collection is off unless something is
		// reading it.
		std::filesystem::path ProfileSnapshot;

		// How long a world with nobody in it keeps ticking before it closes.
		//
		// **On the command line because five minutes is untestable.** The
		// default is a person's idea of "a while"; a check that a world closes
		// at all cannot wait for it, and neither can somebody demonstrating the
		// lifecycle.
		float IdleCloseSeconds = 300.0f;

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

		// Present without waiting for vblank.
		//
		// **The client's flag, and an editor wants it for a reason a game does
		// not.** A game uncaps to measure the frame instead of the display; an
		// editor uncaps because the hands doing the work feel every millisecond
		// between the mouse and the viewport. Paced to a 60 Hz panel that floor
		// is 16.7 ms before the compositor has its turn, and on a machine whose
		// other display runs at 165 Hz the difference is the whole complaint.
		//
		// Off by default, because a viewport spinning as fast as the GPU allows
		// is a laptop with its fans up for a still picture. See
		// `render::Renderer::SetVerticalSync`.
		bool Uncapped = false;

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
		// The world, as one viewport sees it.
		//
		// @param index 0 is the main viewport, 1 the second.
		void DrawViewport(size_t index);
		void DrawExplorer();
		void DrawWorlds();

		// Decides which viewport the toolbar reports on, once per frame.
		//
		// **Called after every viewport has drawn, from `DrawInterface`.** Focus
		// cannot be resolved inside a panel: `SetWindowFocus` lands at the end
		// of a frame, so a panel drawn later still sees the old focused window
		// and overwrites whatever an earlier one concluded. See
		// `FocusedViewport`.
		void ResolveFocusedViewport();
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

		// Moves the camera back so the selection fills the view.
		//
		// **The aim is kept and only the distance changes.** Swinging the
		// camera round to face the selection as well would move two things at
		// once from one key, and the direction somebody is looking from is
		// usually the direction they meant.
		//
		// Does nothing with an empty selection: there is no honest frame for
		// "everything" that is not just the world bounds, and jumping to those
		// from a stray keypress loses the view somebody had.
		//
		// @param position The camera position to adjust, in place.
		void FocusSelection(engine::core::Vector3 &position, float yaw, float pitch);

		// The editor camera, driven from the mouse and the keyboard.
		//
		// Called from inside the imgui frame rather than beside it, because
		// whether a click belongs to the world or to a panel is a question only
		// imgui can answer and only while a frame is open.
		void DriveCamera();

		// The camera rules, over whichever viewport the pointer is in.
		//
		// **One driver rather than one per panel.** Right-drag to look,
		// middle-drag to pan, wheel to dolly, F to frame — the same in both
		// views, and a second copy is a second place for them to drift apart.
		//
		// **The mouse follows the pointer and the keyboard follows focus**, which
		// is one function and two rules because they are two different questions.
		// Turning a view the pointer is not over would be a camera swinging in a
		// panel somebody is not looking at; refusing to fly a panel that has been
		// clicked into, because the pointer has since moved off it, is the bug
		// this parameter was added for.
		//
		// @param focused Whether this is the panel the keyboard is in. Frees WASD
		//        and F from needing the pointer over the rectangle.
		void DriveCameraFor(
			engine::core::CFrame &frame,
			float &yaw,
			float &pitch,
			float &speed,
			bool hovered,
			bool active,
			bool &panning,
			bool focused
		);

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

		// Opens and closes worlds as a player moves between them.
		//
		// **Roblox's place lifecycle, in one process.** A universe of a hundred
		// subareas cannot tick all hundred: the ones nobody is in should cost
		// their storage and nothing else, and the one somebody teleports into
		// has to be running by the time they arrive. `world::WorldState` already
		// has the vocabulary — Active, Idle, Suspended — and this is the policy
		// that drives it.
		//
		// **Only while running.** In Edit nothing ticks anyway, so suspending a
		// world there would be a state change with no effect that an author
		// would then have to undo by hand.
		void UpdateWorldLifecycle();

		// Records that a world had a reason to be running, now.
		//
		// @param world The world to keep open.
		void Touch(WorldId world);

		// Moves the studio's player into a world, waking it if it is closed.
		//
		// **Wakes before it sends, not after.** A teleport routed to a
		// suspended world is delivered — the directory still knows it — and
		// then sits in an inbox nothing is draining, because a suspended world
		// does not tick. Waking first is what makes arrival mean arrival.
		//
		// @param target The world to move to.
		// @return `false` when the world is unknown or remote.
		bool TeleportPlayer(WorldId target);

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

		// Adds another game's worlds to this universe, keeping what is here.
		//
		// **Not Open.** Open replaces the universe; this merges into it, which
		// is what `ImportWorldFile` does one level down. A world whose name is
		// taken arrives under a suffixed one rather than being refused.
		//
		// @param path The `.agame` to read.
		// @return `false` when nothing could be imported.
		bool ImportUniverseFile(const std::filesystem::path &path);

		// Puts the mirror example into a new place, as a script.
		//
		// **A `Script` instance rather than a scene run at edit time.** The
		// example builds its world from code, and running it here would leave
		// the result looking like geometry somebody placed — saved into every
		// file made from a new place, with nothing in the tree to say where it
		// came from. As a script it is content: visible, editable, run by Play
		// and undone by Stop.
		//
		// @param store The world to install it into.
		// @param file The staged example's file name, e.g. `SkyGrid.luau`.
		// @param instanceName What the `Script` is called in the tree.
		void InstallExampleScript(
			engine::ecs::Store &store, std::string_view file, std::string_view instanceName
		);

		// Marks every instance in a world as expanded in the explorer.
		//
		// **A request rather than a state, which is what `Expanded` already
		// is.** The set is consumed the first time the tree draws, so this says
		// "open these once" and imgui owns what is open from then on — a tree
		// that re-expanded every frame could not be collapsed.
		//
		// @param world The world whose instances to open.
		void ExpandWorldTree(WorldId world);

		WorldId AddWorld(engine::core::Name name);
		void RemoveWorld(WorldId world);

		Entity InsertInstance(WorldId world, engine::ecs::ClassId klass, Entity parent);
		void DeleteSelection();
		void DuplicateSelection();

		void OpenScriptTab(WorldId world, Entity instance);
		void SaveScriptTab(OpenScript &tab);
		void CloseScriptTab(size_t index);

		// --- running -----------------------------------------------------------

		// Puts one world into a mode, starting or stopping it as needed.
		//
		// **The transport, and it names a world because the transport does.**
		// Play with Viewport 2 focused runs the scene Viewport 2 is showing and
		// nothing else. Passing `Edit` stops that world and restores it.
		//
		// @param world The scene to change.
		// @param mode What it should be doing.
		void SetRunMode(WorldId world, RunMode mode);

		// Takes the snapshot Stop restores, and starts this world's runtime.
		//
		// @param world The scene to start.
		// @param mode Server-only or server-and-client.
		// @return `false` when the scene could not be written, which is a
		//         refusal to start rather than a run that cannot be undone.
		bool BeginRun(WorldId world, RunMode mode);

		// Destroys this world's runtime and restores it from its snapshot.
		//
		// **The world is rebuilt rather than overwritten**, because
		// `ReadWorldDocument` creates a scene rather than restoring into one.
		// `Universe::Adopt` reuses the hole a destroy leaves, so the handle
		// survives — the same trick `RenameWorld` depends on, and the reason a
		// viewport pinned to this world still points at it afterwards.
		//
		// @param world The scene to stop. Not running is not an error.
		void EndRun(WorldId world);

		// Stops every running world. For shutdown, New Game and Open.
		void EndAllRuns();

		// Suspends the worlds that are not running, and wakes the ones that are.
		//
		// **Because `Universe::Tick` advances every world it holds.** A scene
		// left alone while another runs would settle its physics and fire its
		// heartbeats — running by any name an author would use. With nothing
		// running at all there is no tick, so everything goes back to active.
		void SyncWorldStates();

		// --- state ------------------------------------------------------------

		void Say(std::string text, engine::core::LogLevel level = engine::core::LogLevel::Info);
		void MarkModified();
		std::string TitleText() const;

		// --- control -----------------------------------------------------------
		//
		// The Model Context Protocol surface. `engine/control/Server.hpp` carries
		// why it is a socket rather than stdio and why it only ever binds
		// loopback; `Control.cpp` here registers the rows an editor has and a
		// server does not.
		//
		// Every one of these runs on the editor's own thread, called from the
		// frame loop, because `Universe::Enter` aborts on a foreign one.

		// Binds the port, if `--mcp-port` asked for one, and fills the table.
		void StartControl();

		// Answers everything the socket parked since the last frame.
		void PumpControl();

		// The editor's own tools, added on top of the shared ones.
		void RegisterControlTools();

		// The `world` argument, defaulting to the active scene.
		WorldId ControlWorld(const nlohmann::json &arguments, std::string &failure);

		// Whether a control client has asked for the frame graph.
		//
		// Mirrored from `ControlSurface.WantsProfiling()` rather than reusing
		// `ShowFrameGraph`, because that one is a *panel* — setting it would open
		// a window in an editor somebody is using, to answer a question asked
		// over a socket.
		bool ControlWantsProfile = false;

		Options Settings;

		// The listener and the table. Held by value and started only when asked;
		// a server that was never started costs a thread that was never spawned.
		engine::control::Server ControlServer;
		engine::control::Surface ControlSurface{
			"atomic-studio",
			"The editor of the atomic game engine, live. `engine_info` and `world_list` are the two "
			"worth calling first: the universe is the game and each world under it is a scene, which "
			"is the same mapping a script sees through `game` and `workspace`. Worlds run "
			"independently — `world_run` starts one without starting the rest, and stopping restores "
			"the snapshot taken when it started."};

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

		// Worlds the explorer has been told to expand, by world index.
		//
		// **Apart from `Expanded` because the key spaces are different.** A
		// world is identified by a small index and an instance by a 64-bit id,
		// and putting both in one set would have world 3 open whatever instance
		// happened to be entity 3.
		std::vector<uint32_t> ExpandedWorlds;

		// How many more frames the Worlds panel should ask for its dock tab.
		//
		// **A count rather than a flag, and one frame is not enough.** Focus is
		// an imgui operation that only means anything between `NewFrame` and
		// `Render`, so `NewGame` — reachable from a menu, a keybind or the
		// command line — has to leave a request behind. One frame's worth was
		// tried: on a first run the default layout is rebuilt during that same
		// frame, and `DockBuilderDockWindow` decides the tab order after the
		// focus request has already been spent, so the panel docked first wins
		// and the request is silently lost.
		//
		// A few frames outlast the rebuild and cost nothing afterwards — the
		// panel is already in front, so asking again changes nothing.
		int FocusWorlds = 0;

		// The scene's surface camera, when it has one.
		//
		// **What makes a mirror appear in the editor.** A `Camera` with a
		// `SurfaceSize` renders the world into a texture that a `Part` with a
		// matching `Surface` samples; the renderer only runs that pass when it
		// is handed a view, and the editor handed it nothing. Found per frame
		// rather than cached, because a script can create, move or delete the
		// camera at any point during a run. See `PresentWorld`.
		// Every surface camera the drawn world holds, rebuilt each frame. See
		// `client::CollectSurfaceViews`; a list assembled from what is in the
		// world is also what makes a deleted mirror stop being drawn.
		std::vector<engine::render::SurfaceView> Surfaces;

		// One world's run, for as long as it is running.
		//
		// **A universe is a collection of scenes, so running is per scene.**
		// This replaced a single editor-wide `RunMode` plus a `Paused` flag plus
		// a one-world scope, and the three of them together could not express
		// the thing a studio with several viewports is for: run the server's
		// world, leave the client's in edit, and author one while the other
		// plays. A global mode forced every world into the same answer, so the
		// scene you were not looking at ran too — and Stop restored a snapshot
		// over both of them.
		//
		// A world with no record is being edited. That is the whole of the
		// "stopped" state: there is no `RunMode::Edit` record, because a record
		// exists precisely to hold what a run needs to be undone.
		//
		// @since v0.7
		struct WorldRun {
			// Which scene this is the run of.
			WorldId World;

			// Server-only or server-and-client. Never `Edit` — see above.
			RunMode Mode = RunMode::Server;

			// Whether the clock is stopped for this world alone.
			//
			// **Not a third mode, and the distinction is load-bearing.** The
			// mode decides which scripts run and what `IsServer` answers;
			// pausing changes neither. It stops this world's clock and leaves
			// its runtime, its connections and its snapshot alone, so
			// unpausing carries on rather than starting again.
			bool Paused = false;

			// The scene as it was when the run started, as a world document.
			//
			// **`WriteWorldDocument`, not `Universe::Save`.** The universe
			// snapshot is every world at once, which is exactly the thing that
			// made Stop restore scenes nobody had run. A document is one
			// scene's authored content, and it is the same call Duplicate and
			// Rename already make.
			std::string Snapshot;

			// The VM, for as long as the world runs.
			//
			// **Shared rather than unique**, because the heartbeat system
			// installed into the world's scheduler captures it — the same
			// arrangement `examples::LoadScene` uses, and for the same reason:
			// a script that connects to `RunService.Heartbeat` *is* the
			// simulation for what it built, so the VM has to outlive the call
			// that started it.
			std::shared_ptr<engine::script::Runtime> Runtime;

			// The client half, for a `Play` run only.
			//
			// **Null for `RunMode::Server`, and that is the difference between
			// the two modes made real.** Run is a dedicated server: there is no
			// client anywhere, so there is nothing to replicate to. Play is both
			// halves, which until this was a claim about which scripts ran
			// rather than something a viewport could show. See `PlayLink`.
			//
			// Held by pointer because `PlayLink` owns an `Authority` and a
			// `Replica`, neither of which is movable in the way a vector of
			// runs needs — and a run is moved whenever another one starts.
			std::unique_ptr<PlayLink> Link;
		};

		// Every world currently running. Worlds absent from this are in edit.
		std::vector<WorldRun> Runs;

		// This world's run, or null when it is being edited.
		//
		// @param world The scene to ask about.
		// @return The record, or null.
		WorldRun *RunOf(WorldId world);
		const WorldRun *RunOf(WorldId world) const;

		// What a world is doing right now.
		//
		// **The replacement for reading `Mode`, and every old call site had to
		// answer a new question to use it: running *for which world*.** A menu
		// item that disabled itself during a run now disables itself during a
		// run *of the scene it would change*, which is the behaviour a studio
		// with independent scenes has to have.
		//
		// @param world The scene to ask about.
		// @return Its mode, or `Edit` when it is not running.
		RunMode ModeOf(WorldId world) const;

		// Whether a world is running at all.
		//
		// @param world The scene to ask about.
		// @return `true` when it has a run record.
		bool IsRunning(WorldId world) const;

		// Whether a world is a `Play` run's client view rather than a scene.
		//
		// **Asked by everything that treats a world as authored content**: the
		// worlds panel refuses to rename, remove, duplicate or export one, the
		// lifecycle never idle-closes one, and it is never written to a game
		// file. A replica is a *view of a run*, and it exists between Play and
		// Stop and at no other time.
		//
		// A linear scan over the runs, which is the same shape `RunOf` already
		// is and for the same reason: there is one run per scene being played
		// and a map would be a second place for that list to be wrong.
		//
		// @param world The scene to ask about.
		// @return `true` when some run owns it as a replica.
		bool IsReplicaWorld(WorldId world) const;

		// The run whose client view this world is, or null.
		//
		// @param world The scene to ask about.
		// @return The run that produced it, or null.
		const WorldRun *RunForReplica(WorldId world) const;

		// Whether a world's clock is stopped.
		//
		// @param world The scene to ask about.
		// @return `true` when it is running and paused.
		bool IsPaused(WorldId world) const;

		// Whether anything at all is running.
		//
		// **For the few places that genuinely mean the program rather than a
		// scene** — the window title, and the guard on saving a game file while
		// something is live. Everything else wants `ModeOf`.
		//
		// @return `true` when at least one world is running.
		bool AnyRunning() const;

		std::vector<OpenScript> Scripts;
		int ActiveScript = -1;

		// How much bigger the code is drawn than the interface around it.
		//
		// **One zoom for the panel rather than one per tab.** Somebody who
		// wants larger code wants it in every script they open, and a
		// per-tab zoom is a setting that appears to reset itself every time
		// they switch file.
		float ScriptZoom = 1.0f;

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
		// Makes this viewer's own camera in a world, and keeps it where the eye
		// is.
		//
		// **The camera is the viewer's, not the game's.** The editor makes one
		// to show its point of view, a client makes one for its player, and when
		// several people edit one game they each make their own — so it carries
		// `scene::TransientComponent` and is never written into a game file.
		// Everything else about it is ordinary: it is in `Workspace`, the
		// explorer shows it, the properties panel edits it, and a script sees it
		// where it expects the current camera to be.
		//
		// @param world The world being viewed.
		// @param eye   Where this viewport is looking from.
		// @param lens  Its field of view and clip planes.
		// Puts this panel's own camera in the world it is showing, and names it
		// the world's live one.
		//
		// **One per panel, not one per world, and that is the whole point.** A
		// single shared `Camera` instance was written by whichever panel drew
		// last — the studio round-robins one viewport per frame — so its `CFrame`
		// flickered between viewpoints every frame, the properties panel showed
		// a field nobody could edit, and `workspace.CurrentCamera` meant "the
		// panel that happened to draw most recently".
		//
		// **`ActiveCamera` is set every frame rather than at creation.** It used
		// to be named once, when the instance was minted, so a script assigning
		// `workspace.CurrentCamera` took the world's eye away from the editor
		// permanently — and `scene::AimSurfaceCameras` reflects through whatever
		// that names, so every mirror in the scene quietly started reflecting
		// from a camera the viewport was not looking through.
		//
		// @param viewport 0 is the main panel, 1..EXTRA_VIEWPORTS the others.
		// @param world    The world this panel is showing.
		// @param eye      Where the panel is looking from.
		// @param lens     Its field of view and clip planes.
		// @param follow   The instance being looked through, or null. Its camera
		//                 is followed rather than driven, so dragging its
		//                 `CFrame` in the properties panel is not fought.
		void EnsureViewerCamera(
			size_t viewport,
			WorldId world,
			const engine::core::CFrame &eye,
			const engine::scene::Camera &lens,
			Entity follow
		);

		// Destroys a panel's camera, if it has one.
		//
		// **Closing a panel has to take its camera with it**, or a studio that
		// has had four viewports open over a session leaves four cameras in the
		// world — none of them driven, all of them in the explorer, and one of
		// them still named as the world's active one.
		//
		// @param viewport Which panel.
		void ReleaseViewerCamera(size_t viewport);

		// A `Camera` instance the main viewport looks through, or null.
		//
		// **Because "I moved the Camera and nothing happened" is the obvious
		// complaint and the answer used to be a design note.** The editor's own
		// camera is deliberately not an entity — one that lived in the scene
		// would be saved into the game file, replicated, and reset by Stop. The
		// consequence was that the `Camera` instance in the tree looked like the
		// thing driving the viewport and drove nothing at all: an author could
		// edit its CFrame all day and watch the view sit still.
		//
		// Looking through one is the reconciliation. The free camera is still
		// what an editor flies; a followed camera is the scene's, and moving it
		// moves the view because that is what it means to look through it.
		// Right-dragging to fly detaches, which is how somebody gets back
		// without finding a menu.
		Entity FollowCamera;

		engine::core::CFrame CameraFrame;
		float CameraYaw = 0.0f;
		float CameraPitch = 0.0f;
		float CameraSpeed = 24.0f;

		// How big a texture the world is drawn into, from the viewport panel's
		// content rectangle. See `render::SceneTarget`.
		// One per viewport panel, indexed by `ViewportState::Slot`.
		//
		// **Separate targets rather than one shared**, because the two panels
		// are different sizes: a single target would be reallocated twice a
		// frame as each asked for its own dimensions.
		engine::render::SceneTarget WorldTarget;

		// What a viewport panel is looking at, and from where.
		//
		// **A camera per panel, and neither is world state.** Two panels
		// sharing one camera would be one view drawn twice; the whole point of
		// the second is to watch a different world, or the same world from
		// somewhere else, while the first stays where it was put.
		struct ViewportState {
			// Which world it draws, or invalid to follow the active one.
			WorldId World;

			engine::render::SceneTarget Target;

			engine::core::CFrame Frame;
			float Yaw = 0.0f;
			float Pitch = 0.0f;
			float Speed = 24.0f;

			bool Hovered = false;
			bool Active = false;
			bool Panning = false;

			bool Open = false;

			// A `Camera` instance this view looks through, or null for the free
			// camera. See `Editor::FollowCamera`.
			Entity Follow;
		};

		// The extra viewports. The first is the fields above, which predate them
		// and which every other panel already reads.
		//
		// **Three of them, so a universe of subareas can be watched at once.**
		// A fixed array rather than a vector: the panels are named windows imgui
		// remembers by title, so they cannot be created on demand without the
		// saved layout having a name it has never seen — and a studio with an
		// unbounded number of viewports is one where the frame rate divides by a
		// number nobody chose.
		static constexpr size_t EXTRA_VIEWPORTS = 3;
		std::array<ViewportState, EXTRA_VIEWPORTS> Extras;

		// A panel's own camera instance, and the world it was minted in.
		//
		// **The world is held beside the entity because an entity handle alone
		// cannot say where it lives.** A panel can be repointed at another
		// world, and the camera it made in the old one has to be destroyed
		// there — `Universe::Enter` needs the id to do it, and by then the panel
		// is already showing something else.
		struct ViewerCamera {
			WorldId World;
			Entity Instance;
		};

		// Indexed the way `DrawingViewport` is: 0 is the main panel, 1.. are the
		// extras, so a panel index is a subscript rather than a branch.
		std::array<ViewerCamera, 1 + EXTRA_VIEWPORTS> Viewers;

		// Which viewport a panel index refers to, or null for the main one.
		//
		// @param index 0 is the main viewport, 1..EXTRA_VIEWPORTS the others.
		// @return The state, or null when the index is the main viewport.
		ViewportState *ExtraAt(size_t index) {
			return index == 0 || index > EXTRA_VIEWPORTS ? nullptr : &Extras[index - 1];
		}

		// Which viewport the toolbar is reporting on.
		//
		// **The one you last clicked in, so the transport is about the picture
		// you are looking at.** With one viewport this is always zero and
		// nothing about the toolbar changes. With two it is the difference
		// between a scene selector that says what you are editing and one that
		// says what some other panel happens to be pinned to — and with two
		// worlds ticking in parallel, a readout naming the wrong one is worse
		// than no readout.
		//
		// Not saved: it follows the mouse and re-establishes itself on the first
		// click of a session.
		size_t FocusedViewport = 0;

		// Whether the keyboard is actually in that viewport right now.
		//
		// **Not the same question as `FocusedViewport`, and conflating them
		// flies the camera while somebody types in a property field.** That one
		// keeps naming the last viewport when focus moves to the explorer or the
		// properties panel — deliberately, so the transport readout does not
		// blank every time you click something. This one goes false, because the
		// camera must not answer to a keyboard that is somewhere else.
		//
		// Both are resolved together in `ResolveFocusedViewport`, from the same
		// window, which is what stops them disagreeing.
		bool FocusedIsViewport = false;

		// Whether a viewport claimed focus from a click during this frame.
		//
		// **A click has to outrank `IsWindowFocused`, and only for one frame.**
		// `SetWindowFocus` takes effect at the end of the frame, so every panel
		// drawn after the clicked one still sees the *old* focused window and
		// would overwrite the claim. Reset in `DrawInterface` before any panel
		// draws. See `FocusedViewport`.
		bool ViewportClaimed = false;

		// The world a viewport is showing.
		//
		// **An extra viewport with no pin follows the active world**, which is
		// what makes a freshly opened panel show something rather than nothing.
		// The main viewport is always the active world; it has no pin of its own
		// because "the world being edited" is what it means.
		//
		// @param index 0 is the main viewport, 1..EXTRA_VIEWPORTS the others.
		// @return The world it draws, which may be invalid if there is none.
		WorldId ViewportWorld(size_t index) {
			const ViewportState *extra = ExtraAt(index);
			if (extra != nullptr && extra->World.IsValid()) {
				return extra->World;
			}
			return Active;
		}

		// **Which viewport the renderer draws this frame.** `Renderer::Render`
		// owns the whole frame — it acquires the swapchain, records the
		// interface and presents — so it draws one world per call. Two panels
		// therefore take turns: each keeps its own target and its own texture,
		// and shows the most recent frame drawn into it.
		//
		// The cost is that N open viewports refresh at a *fraction* of the frame
		// rate each — a sixtieth of a second still goes by, but any one panel
		// is redrawn every N frames. That is honest for an editor watching
		// several worlds tick and it is not the end state: drawing them all in
		// one frame is a change to `Render` to take a list of views.
		size_t DrawingViewport = 0;

		// Where the rotation is up to, over the *open* panels rather than over
		// all of them.
		size_t RoundRobin = 0;

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
		bool AskingImportUniverse = false;
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

		// Whether a middle-drag pan is in flight.
		//
		// Held for `ViewportActive`'s reason: a pan that stopped the moment the
		// pointer left the panel would be unusable exactly when somebody is
		// dragging the view a long way.
		bool ViewportPanning = false;

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
		// Where the keybind table is read from and written back to.
		//
		// Beside the binary with the layout ini, so a launcher's working
		// directory cannot move somebody's keys. See `Keybinds::Load`.
		std::filesystem::path KeybindPath;

		WorldId PendingActivate;

		// A run change asked for from the Worlds panel, applied after it draws.
		//
		// **Queued because `SetRunMode` rebuilds the world.** Starting or stopping
		// a scene from inside the popup drawn for its own row would destroy the
		// world while the loop listing it still held its handle.
		WorldId PendingRunWorld;
		RunMode PendingRunMode = RunMode::Edit;
		WorldId PendingDuplicateWorld;
		WorldId PendingTeleport;

		// A world whose state a menu asked to change, and what to. Both, so
		// that Open and Close are one queued action rather than two flags that
		// can disagree.
		WorldId PendingWorldState;
		engine::world::WorldState PendingWorldStateTo = engine::world::WorldState::Active;
		WorldId PendingRenameWorld;
		std::string PendingRenameTo;
		// A camera to look through, and whether the menu asked at all — the two
		// are separate because "look through nothing" is a real request.
		Entity PendingLookThrough;
		bool PendingLookThroughSet = false;

		bool PendingDuplicate = false;
		bool PendingDelete = false;

		// The scene the rename prompt is for, and whether it is open. Held
		// apart from `PendingRenameWorld` because the prompt spans frames and
		// the action is applied on exactly one.
		WorldId RenamingWorld;
		bool AskingRenameWorld = false;

		// The explorer's recursion buffer, shared by every level of one walk.
		//
		// **A member rather than a local, because a local was a heap allocation
		// per node per frame.** `DrawTreeNode` collects a node's children before
		// walking them — it has to, since drawing one can queue a reparent — and
		// doing that with a `std::vector` inside the body meant an open tree of
		// two hundred rows allocated and freed two hundred times every frame.
		//
		// Each level appends its own run and truncates back to its mark on the
		// way out, so the buffer is empty between frames and reaches the depth of
		// the deepest open path exactly once.
		std::vector<Entity> ChildScratch;

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

		// --- the world lifecycle ----------------------------------------------

		// When a world last had a reason to be running.
		struct WorldLife {
			WorldId World;

			// Frame-clock time of the last activity: an arrival, an occupant,
			// or somebody looking at it.
			double LastActivity = 0.0;
		};

		std::vector<WorldLife> Lives;

		// Whether worlds close themselves when nobody is in them.
		//
		// Off would mean a universe of subareas ticking all of them forever,
		// which is the thing `WorldState::Suspended` exists to prevent — but it
		// is a policy, and an author debugging a world that keeps closing under
		// them needs a way to stop it.
		bool AutoManageWorlds = true;

		// How long a world with nobody in it keeps ticking before it closes.
		//
		// Five minutes by default, which is Roblox's own place-shutdown grace
		// and long enough that an author who walked away from one subarea to
		// build in another does not come back to a stopped clock. Set from
		// `Options::IdleCloseSeconds`.
		float IdleCloseSeconds = 300.0f;

		// Which world the studio's player token is in.
		//
		// **The editor's own notion of "where somebody is", and it is not a
		// character.** There is no player instance in this program; what the
		// lifecycle needs is one authoritative answer to "is this world
		// occupied", and a token the editor moves is one it cannot get wrong.
		// A script's own teleports are picked up separately, through the
		// destination's inbox.
		WorldId PlayerWorld;

		bool Running = false;
		int64_t FramesDrawn = 0;
		engine::render::FrameResult LastFrame;
	};
}
