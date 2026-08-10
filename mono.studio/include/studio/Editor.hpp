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

#include <engine/control/Server.hpp>
#include <engine/control/Surface.hpp>
#include <engine/core/Clock.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/game/Game.hpp>

#include <studio/Config.hpp>
#include <studio/Plugins.hpp>
// TODO(render-pipeline): `<engine/nodeview/Editor.hpp>` and `State.hpp` were
// included here. `Engine::nodeview` was the node-canvas module the Render and
// Assets Pipeline panels were built on; it is removed. See the member and
// method markers below.
#include <engine/assets/AssetKind.hpp>
#include <engine/delivery/Client.hpp>
#include <engine/delivery/IntakeBudget.hpp>
#include <engine/delivery/Uploader.hpp>
#include <engine/gui/Compile.hpp>
#include <engine/gui/Input.hpp>
#include <engine/render/DebugPanels.hpp>
#include <engine/render/FrameStatistics.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scene/Components.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/ui/Interface.hpp>
#include <engine/ui/Theme.hpp>
#include <engine/world/Universe.hpp>

#include <array>
#include <cdn/LocalStore.hpp>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <span>
#include <string>
#include <studio/Commands.hpp>
#include <studio/ContentSources.hpp>
#include <studio/Hierarchy.hpp>
#include <studio/Operators.hpp>
#include <studio/PlayLink.hpp>
#include <studio/Preview.hpp>
#include <studio/Projection.hpp>
#include <studio/TeamCreate.hpp>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct SDL_Window;
struct ImGuiInputTextCallbackData;

// **Forward-declared rather than including imgui here.** `Editor.hpp` is
// included by every panel and by the tests, and dragging imgui in to name one
// parameter type would make a headless suite compile a UI library.
struct ImGuiTableSortSpecs;

// **Forward-declared rather than including imgui here.**  is
// included by every panel and by the tests, and dragging imgui into all of them
// to name one parameter type would make a headless suite compile a UI library.
struct ImGuiTableSortSpecs;

namespace studio {

	using engine::ecs::Entity;
	using engine::world::WorldId;

	// Bytes a second in each direction, over a measured window.
	//
	// @since v0.10
	struct NetworkRates {
		// The measured rates, in bytes a second.
		//@{
		double DownPerSecond = 0.0;
		double UpPerSecond = 0.0;
		//@}

		// How long the window actually was. Zero means there is not one yet,
		// which the panel says rather than drawing a zero that looks like idle.
		double WindowSeconds = 0.0;
	};

	// What crossed the wire over the last few seconds, and when.
	//
	// **A short ring, because a total cannot say whether a transfer is
	// moving.** Bytes-since-start divided by editor-uptime is a number that
	// only falls, and it is useless at the moment somebody actually asks — "is
	// this download progressing". `cdn::Dashboard` makes the same decision at a
	// minute's resolution for a terminal; this is the same arithmetic at the
	// resolution a panel is read at.
	//
	// **Holds no clock.** Every sample is stamped with a time the caller passes
	// in, which is `net`'s and `assets::Grant`'s standing rule and is what lets
	// a suite state ten seconds of traffic rather than wait them out.
	//
	// @since v0.10
	struct NetworkSamples {
		// How many seconds of history the ring holds, at one sample a second.
		static constexpr size_t CAPACITY = 8;

		// The shortest gap between two samples.
		static constexpr double INTERVAL = 1.0;

		// One reading of the running totals.
		struct Sample {
			// When it was taken, and the running totals at that moment.
			//
			// **Totals rather than deltas**, so a rate is a subtraction between
			// any two samples — which is what lets the window be resized without
			// the history meaning something different.
			//@{
			double Seconds = 0.0;
			uint64_t Down = 0;
			uint64_t Up = 0;
			//@}
		};

		// The ring of readings, oldest to newest by way of `At`.
		std::array<Sample, CAPACITY> Points{};

		// Where the newest sample is.
		size_t At = 0;

		// How many of the ring's entries hold a reading.
		size_t Filled = 0;

		// Records the running totals, if enough time has passed.
		//
		// @param nowSeconds Seconds since the editor started.
		// @param down Bytes fetched over this client's life.
		// @param up Bytes uploaded over this uploader's life.
		void Observe(double nowSeconds, uint64_t down, uint64_t up);

		// What crossed per second across the whole window.
		//
		// The oldest sample against the newest rather than the last two: a pair
		// one interval apart is one frame's noise, and what the panel is being
		// read for is whether a transfer is moving.
		//
		// @return The rates, with a zero window when there are fewer than two
		//         samples.
		NetworkRates Rates() const;
	};

	// One asset the worlds actually use, and what uses it.
	//
	// **One entry per asset rather than per instance**, which is the whole shape
	// of the gallery: a texture on forty parts is one tile with `x40` on it.
	//
	// @since v0.10
	struct GalleryEntry {
		// The name the property holds.
		engine::core::Name Asset;

		// What kind of content that name is, from the property that named it.
		engine::assets::AssetKind Kind = engine::assets::AssetKind::Unknown;

		// **Which property is using it — the caption that carries the
		// information.** Four tiles reading "SurfaceAppearance" say nothing;
		// "ColorMap" and "NormalMap" say everything.
		std::string Property;

		// Where the first user lives, so a click can select in that world.
		engine::world::WorldId World;

		// The first instance found using it.
		Entity First;

		// How many instances use it.
		size_t Uses = 0;
	};

	// An asset asked for and not yet arrived.
	//
	// @since v0.10
	struct PendingDownload {
		// What was asked for, and the handle to ask about it.
		//
		// **The name is kept even though the id is the handle**, because a panel
		// showing what is in flight has to name the asset and a `RequestId` is a
		// number nobody can read.
		//@{
		std::string Name;
		engine::delivery::RequestId Id;
		//@}
	};

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

	// Every panel that can be given colours of its own.
	//
	// **The titles imgui keys the windows on**, which is also what the layout
	// ini and `Prefs.PanelColours` use — one name for a panel everywhere rather
	// than a display name beside an identifier that has to be kept in step.
	//
	// **This list and the `Skinned` calls in `DrawInterface` have to agree**, and
	// nothing enforces it: a panel added to one and not the other is a panel the
	// settings page offers to colour and nothing colours, or the reverse. Both
	// live in `Interface.cpp`, next to each other, for that reason. A plugin's
	// dock widget is deliberately absent — a plugin colours its own, from Luau,
	// and a list of them here would be a list that changes as plugins load.
	//
	// @return The titles, valid for the lifetime of the process.
	// @since v0.13
	std::span<const char *const> SkinnablePanels();

	// One line in the output panel.
	//
	// @since v0.7
	struct Message {
		// What it says.
		std::string Text;

		// Whether it is an error, a warning or ordinary progress. Drawn as a
		// colour, which is the only thing the panel does with it.
		engine::core::LogLevel Level = engine::core::LogLevel::Info;

		// A number that only ever goes up, and never repeats.
		//
		// **What a selection is made of, because an index is not stable.** The
		// list is a deque trimmed from the *front* at `OUTPUT_LIMIT`, so the
		// line at index 40 is a different line a moment later — a selection
		// held as indices would slide up the log while somebody read it.
		//
		// @since v0.13
		uint64_t Serial = 0;
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

		// The frame graph, which is the second of the two panels above.
		bool ShowFrameGraph = false;

		// The assets manager, open at startup.
		//
		// **The same reason the two above take flags**, said in their own
		// comment: it makes the panel reachable without a keyboard, which is
		// what lets a capture prove it draws. A panel that had only ever been
		// compiled is what the roadmap's own `[~]` on this one was about.
		//
		// @since v0.10
		bool ShowAssetsPanel = false;

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

		// Draw as fast as the GPU allows: no vblank and no ceiling.
		//
		// **The client's flag, and it removes a different thing in each.** The
		// client is paced by the display until this is passed. The editor never
		// is — `Editor::VerticalSync` is off from the start, because the hands
		// doing the work feel every millisecond between the mouse and the
		// viewport — so what is left to remove here is `Editor::FrameCap`, the
		// 120 fps ceiling that keeps a still scene off a laptop's fans.
		//
		// Which makes this a benchmark's flag rather than a comfort one: pass it
		// when the number being read is the frame's cost, and the sleep that
		// pads every frame out to 8.3 ms would be measured as that cost. See
		// `render::Renderer::SetVerticalSync`.
		bool Uncapped = false;

		// Write the world the viewport is showing to this file, then carry on.
		//
		// **Taken on the last frame of a `--frames` budget**, so a run that asks
		// for both produces exactly one image of a known frame. On its own it is
		// taken once, a few frames in — early enough to be useful and late
		// enough that the layout and the first scene have settled.
		std::filesystem::path Capture;

		// Which world the capture should be of, or empty for whichever the
		// active viewport is showing.
		//
		// **Because a count is not a picture, and every check this editor could
		// run headlessly was a count.** A change to how geometry is scaled,
		// centred or culled produces an identical "13 placed, 7 meshes, 16
		// textures, 0 unresolved" whether the models are right, squashed,
		// inside-out or invisible. Naming the world lets a smoke test shoot the
		// one scene built to show content.
		//
		// A name that no world answers to leaves the capture on the active
		// viewport rather than failing: a capture is a diagnostic, and one that
		// aborted a run because a scene was renamed would be worse than one that
		// photographs the wrong thing.
		//
		// @since v0.10
		std::string CaptureWorld;
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

	// What the Find panel was asked for.
	//
	// **Every field is optional and they are combined with "and".** An empty
	// field does not filter, which is what makes "every Part" and "anything with
	// Transparency above zero" the same control rather than two modes.
	//
	// @since v0.10
	struct FindQuery {
		// An `IsA` filter, not an exact class name — "BasePart" finds every
		// part. Empty matches every class.
		std::string Class;

		// A substring of the instance's name. Empty matches every name.
		std::string Name;

		// A substring of a property's name. Empty means "any property", which
		// only matters when `Value` is filled in.
		std::string Property;

		// What the property's value must contain, or equal when `Exact`.
		std::string Value;

		// Whether `Value` is compared through the property's own type rather
		// than through its rendered text. See `Editor::InstanceMatches`.
		bool Exact = false;
	};

	// One instance the query matched.
	//
	// Names are copied rather than held as handles because the panel draws them
	// outside `Universe::Enter`, where the store that owns them is not reachable.
	//
	// @since v0.10
	struct FindResult {
		// Which world it is in.
		WorldId World;

		// The instance itself, for selecting it.
		Entity Instance;

		// Its name at the moment of the walk.
		std::string Name;

		// Its class's name.
		std::string Class;

		// The property and value that matched, for the row. Empty when the
		// query had no property or value in it.
		std::string Matched;
	};

	// Whether one instance satisfies a query, and what matched.
	//
	// **A free function rather than a method, so a test can reach it.** The
	// predicate is the half of Find that can be silently wrong — a filter that
	// quietly matches nothing looks exactly like a scene that contains nothing —
	// and everything else about the panel needs a window, a device and an imgui
	// frame. `Operators.hpp` records what it costs when the testable half is
	// only reachable through an `Editor`.
	//
	// @param store    The world the instance is in.
	// @param instance The instance to test.
	// @param query    What was asked for.
	// @param matched  Filled with the property and value that matched, for the
	//                 result row. Cleared when the query named neither.
	// @return `true` when it satisfies every filled-in field of the query.
	bool MatchesQuery(
		const engine::ecs::Store &store, Entity instance, const FindQuery &query, std::string &matched
	);

	// Which side of a comparison a line came from.
	//
	// @since v0.10
	enum class DiffKind : uint8_t {
		// In both, unchanged.
		Same,

		// In the live game and not in the file.
		Added,

		// In the file and not in the live game.
		Removed,
	};

	// One line of a comparison.
	//
	// @since v0.10
	struct DiffLine {
		// Which side it came from.
		DiffKind Kind = DiffKind::Same;

		// The line itself, without its newline.
		std::string Text;
	};

	// Compares two documents line by line.
	//
	// **A free function with its own suite**, because a comparator that reports
	// no changes looks exactly like a clean tree and nothing about the panel
	// would say which it was.
	//
	// Common prefix and suffix are trimmed first — two saves of one game are
	// almost identical — and the differing middle is aligned by longest common
	// subsequence. Past `DIFF_CELL_LIMIT` cells the alignment is abandoned and
	// the whole middle is reported as removed-then-added, which is still true
	// and much coarser.
	//
	// @param before The document to compare against, normally the file on disk.
	// @param after  The document as it stands now.
	// @param coarse Set when the alignment was abandoned. Optional.
	// @return One entry per line of the comparison, in order.
	std::vector<DiffLine> DiffText(std::string_view before, std::string_view after, bool *coarse = nullptr);

	// The largest alignment table `DiffText` will build.
	//
	// The table is O(n·m) and this is a bound on the work, not on the answer —
	// past it the diff degrades rather than failing, and says so.
	inline constexpr size_t DIFF_CELL_LIMIT = 4u * 1000u * 1000u;

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

		// The breakpoint column beside a script's text.
		//
		// **A sibling of the code rather than part of it**, because
		// `CodeField` is an `InputTextMultiline` and owns its own scrolling
		// child — so the column is drawn next to it and told where that child
		// has scrolled to.
		//
		// @param tab The script being edited.
		// @return How wide the column drew, so the caller can lay out beside it.
		float DrawScriptGutter(const OpenScript &tab);

		// The breakpoint on a line of a script, or null.
		//
		// @param path The script's source path.
		// @param line The 1-based line.
		// @return The breakpoint, or null when there is none.
		const engine::script::Breakpoint *BreakpointAt(engine::core::Name path, int line) const;

		// Adds a breakpoint to a line, or takes the one there away.
		//
		// **Writes the editor's list and every live runtime's**, in that order.
		// The editor's is the one that survives a Stop; a runtime's is a copy it
		// was handed when the run began, so writing only those would make a
		// breakpoint disappear the next time somebody pressed Stop.
		//
		// @param path The script's source path.
		// @param line The 1-based line.
		void ToggleBreakpoint(engine::core::Name path, int line);
		void DrawOutput();

		// Whether a line is inside the output's selected span.
		//
		// @param serial The line's serial.
		// @return Whether it is selected.
		// @since v0.13
		bool OutputSelected(uint64_t serial) const;

		// Puts the selected lines on the clipboard, oldest first.
		//
		// **Only the lines the filter is showing.** A copy that included
		// hidden lines would hand somebody text they cannot see on screen, and
		// the reason they filtered was to be rid of it.
		//
		// @return How many lines were copied.
		// @since v0.13
		size_t CopyOutputSelection();

		// The zoom control a text panel puts beside what it scales.
		//
		// **Written once because two panels have it**, and a second copy of
		// the step and the clamp would be two panels that disagree about what
		// a zoom level is the first time either is tuned.
		//
		// Draws the percentage and the three buttons, and applies Ctrl+wheel
		// and Ctrl+= / Ctrl+- for the window it is called in. The keys are
		// guarded on the window being focused so that zooming one panel does
		// not zoom another.
		//
		// @param zoom What to adjust. Clamped in place.
		// @param what What the tooltip calls the thing being zoomed.
		// @since v0.13
		void DrawZoomControl(float &zoom, const char *what);

		// Applies Ctrl+wheel to a zoom, for the item just drawn.
		//
		// Separate from the control because the wheel belongs over the *text*
		// and the buttons belong in the toolbar, and putting both in one call
		// would tie where a panel shows the control to where it reads the
		// wheel.
		//
		// @param zoom What to adjust. Clamped in place.
		// @since v0.13
		void ApplyZoomWheel(float &zoom);

		// The editor's own settings, in pages.
		//
		// **A panel rather than a modal**, because choosing a theme means
		// looking at the editor while it changes — and a modal is a rectangle
		// over the thing being judged.
		void DrawSettings();

		// The theme picker and the interface scale.
		void DrawAppearanceSettings();

		// The seven colours, chosen over whichever palette is selected.
		//
		// @since v0.13
		void DrawThemeColours();

		// The same seven, for one panel rather than for the editor.
		//
		// @since v0.13
		void DrawPanelColours();

		// The Preferences page that is not about looks: the world lifecycle
		// and how frames are paced.
		void DrawGeneralSettings();

		// The searchable list of what every key does, and where they are
		// changed. See `studio::Keybinds` — this page edits that table
		// directly, so it cannot drift from what the keys actually do.
		void DrawKeybindSettings();

		// The Preferences page that says where content comes from.
		//
		// A reorderable list, because the order *is* the policy: a delivery
		// client walks it and stops at the first source that answers, so "local
		// cache first, then the origin next door" is what the list says rather
		// than something the engine decides.
		void DrawContentSettings();

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

		// The explorer's state for one world: what is open in it, and the tree
		// compiled from it.
		//
		// **Per world, and it has to be.** Expansion used to be one set of
		// entity ids for the whole universe, and an `ecs::Entity` is an index
		// into one store — so entity 7 of World A and entity 7 of World B are
		// the same number. Opening a model in one scene opened whatever
		// happened to be the seventh instance in every other.
		//
		// Expansion is a *state* here rather than the consume-once request it
		// used to be. That changed because the rows are ours now: imgui owned
		// what was open while the panel recursed through `TreeNodeEx`, and a
		// flat list that can be clipped cannot ask imgui which of a thousand
		// rows it is holding open. Still not world state — a set of handles in
		// the editor, exactly as the selection is.
		struct WorldTree {
			WorldId World;

			// Instances the author has opened, by handle. A handle that no
			// longer names anything is harmless: the compile does not find it
			// and it is never asked about again.
			std::vector<Entity> Open;

			// What to draw, and the signature that says whether it is stale.
			HierarchyView View;
		};

		// One world's instances, as a flat clipped list.
		//
		// **Flat rather than recursive, which is what lets it be clipped.**
		// `HierarchyView` has already worked out which rows exist and in what
		// order, so this submits only the ones the scroll position can show —
		// a thousand-instance scene draws the thirty on screen instead of
		// submitting a thousand tree nodes to lay out and throw away.
		//
		// @param store The world, already entered.
		// @param tree  Its explorer state, already rebuilt this frame.
		void DrawInstanceRows(engine::ecs::Store &store, WorldTree &tree);

		// The explorer's per-world state, made on first sight of the world.
		//
		// @param world The world to look up.
		// @return Its record, valid until `Trees` next grows.
		WorldTree &TreeFor(WorldId world);

		// Starts typing a new name over an instance's row in the tree.
		//
		// @param instance The instance to rename. Ignored when it is null.
		void BeginRename(Entity instance);

		// Opens the path down to an instance, so the tree can show it.
		//
		// @param world    The world it lives in.
		// @param instance The instance to make reachable.
		void OpenPathTo(WorldId world, Entity instance);

		// Selects every row between two instances, as shift-click means.
		//
		// **Over the drawn order, not over the tree.** A range in a tree view
		// is what the eye sees between two rows, which is the flattened order
		// with the closed subtrees left out — so it is `HierarchyView`'s row
		// indices and not an ancestor walk.
		//
		// @param world  The world both rows are in.
		// @param view   The compiled tree they were drawn from.
		// @param anchor Where the range starts, from the last plain click.
		// @param to     Where it ends, being the row just clicked.
		// @param add    Whether to keep what was already selected.
		void SelectRange(WorldId world, const HierarchyView &view, Entity anchor, Entity to, bool add);

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

		// Draws the grid, the axes and the selection outline into every panel
		// that drew this frame.
		//
		// **Called from `DrawInterface` immediately after `DriveCamera`**, for
		// the reason `OverlaySlot` gives at length. Moving this call earlier
		// reintroduces the one-frame swim silently — it still draws, and it
		// still looks right whenever the camera is still.
		void DrawViewportOverlays();

		// Compiles and paints the world's own `ScreenGui` tree over one panel.
		//
		// **Over the world image and under the editor's chrome**, which is
		// where a player would see it: it is the game's interface, so it
		// belongs on top of the game and beneath the tools looking at the game.
		//
		// Runs from `DrawViewportOverlays` rather than from `DrawViewport` for
		// the reason `OverlaySlot` gives about the gizmo — the panel rectangle
		// is only settled once every panel has drawn.
		//
		// @param index Which viewport panel.
		void DrawViewportGui(size_t index);

		// How one panel maps between the world and itself, this frame.
		//
		// Built from the camera `PresentWorld` is about to use, so the two
		// agree. See `studio::PanelProjection`.
		//
		// **Call it once per panel per frame.** It resolves a camera — a matrix
		// inverse, a projection and a product, plus a `Universe::Enter` when the
		// panel follows a camera instance — so it is not the sort of thing to
		// ask twice for the same answer. `DrawViewportOverlays` resolves every
		// panel up front and hands the result to the passes that need it, which
		// is also what makes the gizmo and the pick adjudicate one click against
		// one matrix.
		//
		// @param viewport 0 is the main panel, 1..EXTRA_VIEWPORTS the others.
		// @return The mapping, invalid when that panel did not draw.
		PanelProjection ProjectionFor(size_t viewport);

		// Selects whatever is under a panel point.
		//
		// **Builds a `spatial::HashGrid` per click rather than reading the
		// physics broadphase.** A part with no collider is not in the physics
		// index and is still something an author can click on; and in Edit mode
		// the physics world is not stepping, so its index is whatever the last
		// run left. A grid built from the drawables at the moment of the click
		// is correct for both, and a click is not a per-frame cost.
		//
		// **Takes the projection rather than resolving its own.** The click has
		// already been adjudicated against one — the gizmo pass decided it was
		// not a handle grab — and a second `ProjectionFor` would judge the two
		// halves of one click by two matrices that are only equal by argument.
		//
		// @param viewport Which panel.
		// @param point    Where, in screen coordinates.
		// @param add      Whether to add to the selection rather than replace.
		// @param panel    That panel's mapping for this frame.
		void PickInViewport(size_t viewport, float x, float y, bool add, const PanelProjection &panel);

		// Fills `Operators` in. Called once, from `Start`, after the universe
		// exists — several polls read it.
		//
		// **One place that knows what the editor can do.** See `Operators.hpp`
		// for why the menus ask this table rather than writing their enable
		// conditions out again.
		void RegisterOperators();

		// The command palette, over `Operators`.
		//
		// **A modal over the whole window rather than a panel**, because it is
		// reached, used and dismissed within one gesture — a dockable palette
		// would be a panel somebody has to find a home for and then look at
		// forever.
		void DrawPalette();

		// The undo stack, as a list somebody can read and click.
		//
		// **Undo is only trustworthy if you can see what it will do.** The Edit
		// menu names the next one; this names all of them, and clicking an entry
		// walks to that point rather than making somebody press Ctrl+Z and count.
		void DrawHistory();

		// What is in the local content store, and how to put things in it.
		//
		// **A view over `cdn::LocalStore` and nothing of its own.** The folder is
		// the index, so this reads the log and calls `ImportFile` — it keeps no
		// list and caches nothing it cannot rebuild. `Assets.cpp` says why the
		// upload is a typed path rather than a file dialog.
		void DrawAssets();

		// The Render Pipeline and Assets Pipeline node editors.
		//
		// **Drawn with an ImGui draw list rather than as a `gui` tree**, which is
		// `DEFERRED.md` D00041's decision: the canvas machinery — layout, edges,
		// hit-testing, selection — is `graph::PipelineView` and
		// `nodeview::CanvasState` and is shared either way, but a `gui` subtree
		// cannot live inside an `ImGui::Begin` block. The engine's own tree would
		// need a render target per open editor; boxes and lines on a draw list need
		// none, and this panel is the editor's chrome rather than a game's UI.
		//@{
		// TODO(render-pipeline): `void DrawRenderPipeline();` drew the Render Pipeline node editor.

		// The pipeline as a grid: passes across, resources down, what each does
		// to each where they meet.
		//
		// **A second view of the same graph, not a second graph.** The canvas
		// shows what somebody wired; this shows what will run, in what order,
		// touching what, and how much memory is live while it does — which is a
		// different question and wants a different shape.
		// `docs/PIPELINE_NODES.md` §7 argues the point; `graph::PipelineProfile`
		// is the arithmetic and this is only the drawing.
		// TODO(render-pipeline): `void DrawPipelineProfile();` drew the profile grid — passes across the top,
		// resources down the side.

		// The picture and histogram under the access grid. See `ProfileWatched`.
		// TODO(render-pipeline): `void DrawProfileWatch();` drew the picture and channel histogram under the
		// profile grid.

		// The selected node's own settings, under the canvas.
		//
		// **Without it the parameters are unreachable.** Which shader a `raster`
		// runs is a node's own business, and a canvas that could only wire
		// things could describe the shape of a frame and nothing about it.
		// TODO(render-pipeline): `DrawNodeParameters` edited a node's parameters,
		// including the multi-line GLSL box a `raster` node's shader was typed into.

		// TODO(render-pipeline): `DrawChannelHistogram` drew one channel's
		// distribution as sixteen bars and a range, over `render::ChannelHistogram`.
		// TODO(render-pipeline): `void DrawAssetsPipeline();` drew the Assets Pipeline node editor.

		//@}

		// Brings one file, or every file under one folder, into the store.
		//
		// @param given What the person chose or dropped.
		void ImportAssetPath(const std::string &given);

		// What a dropped path does.
		//
		// **Opens the panel as well as importing.** A drop onto a closed one
		// would otherwise be silent: the file lands, the status line updates,
		// and nothing the person can see says so.
		//
		// @param path One dropped file or folder. SDL delivers one event per
		//        path, so a multi-file drop is several calls.
		void DropAssetPath(const std::string &path);

		// The published manifest, as a table somebody can filter and copy from.
		void DrawPublishedList();

		// How long this editor has been drawing, in seconds.
		//
		// **What animation is played against**, accumulated from the frame delta
		// rather than read from a wall clock — see `Renderer::SetAnimationTime`.
		// A world paused in the editor still animates its interface, which is
		// why this advances with the *frame* and not with a world's tick.
		double AnimationSeconds = 0.0;

		// What is sitting in `raw/`, waiting to be published.
		void DrawRawList();

		// One asset's picture, or a box of the same size when there is none.
		//
		// **The same size either way**, so a row does not change height when its
		// picture arrives — a list that reflows while it loads is one nobody can
		// click in.
		//
		// @param name The asset's name, which is its path under `raw/`.
		// @param side How wide to draw it.
		// @param kind What it is, so an empty box can say something useful.
		void DrawPreview(const std::string &name, float side, engine::assets::AssetKind kind);

		// The same picture, painted at a screen position and reserving nothing.
		//
		// **For a caller that has already reserved the space**, which a content
		// list's row has: the row is one `Selectable` and everything over it is
		// painted, so a preview submitted as an item there would be a second hit
		// target and a second copy of the row's geometry.
		// `studio/AssetRow.hpp` carries the whole rule.
		//
		// **Two floats rather than an `ImVec2`**, for the reason this header
		// forward-declares `ImGuiTableSortSpecs` instead of including imgui: it
		// is included by every panel and by the tests, and dragging a UI library
		// in to name one parameter type is a cost every one of them pays. A
		// forward declaration will not do here — the type is passed by value.
		//
		// @param cornerX Left edge, in screen space.
		// @param cornerY Top edge, in screen space.
		// @param side    How wide and tall to paint it.
		// @param name    The asset's name, which is its path under `raw/`.
		// @param kind    What it is, for the glyph when there is no picture.
		void PaintPreview(
			float cornerX, float cornerY, float side, const std::string &name, engine::assets::AssetKind kind
		);

		// Orders the published view by whatever headers were clicked.
		//
		// **The view and never `PickerContents`.** That is what the manifest
		// says, in the order it says it, and every picker reads it — a header
		// click must not reorder what another panel is looking at.
		void SortPublished(std::vector<const cdn::PublishedEntry *> &rows, const ImGuiTableSortSpecs *specs);

		// The same for the raw view.
		void SortRaw(std::vector<const cdn::RawEntry *> &rows, const ImGuiTableSortSpecs *specs);

		// Re-reads both halves of the store.
		//
		// **Called when the panel opens and after anything changes it**, never
		// per frame: listing `raw/` stats every file and reading the manifest
		// parses one.
		void RefreshStoreContents();

		// A small picture of an asset, for a row in a list.
		//
		// **Asked for while drawing and built between frames.** Returning null
		// is the ordinary answer the first time a row appears — the caller draws
		// its placeholder and the picture arrives a frame or two later. See
		// `Thumbnails.cpp` for why it is not built here.
		//
		// @param name The asset's name, which is also its path under `raw/`.
		// @return The backend handle to draw, or nullptr when there is none.
		void *ThumbnailFor(const std::string &name);

		// The same, and why there is no picture when there is not.
		//
		// **Three outcomes rather than two**, which is the correction
		// `Preview.hpp` opens with: a file refused for its size and a file that
		// would not decode are different situations and a person can act on
		// only one of them.
		//
		// @param name  The asset's name.
		// @param state Filled in with what happened.
		// @return The handle, or nullptr.
		void *ThumbnailFor(const std::string &name, PreviewState &state);

		// Builds a bounded number of queued thumbnails and evicts old ones.
		void PumpThumbnails();

		// Drives the preview slot for a rendered row nobody is pointing at.
		//
		// **The fix for a material that only appeared when hovered.** A mesh and
		// a material have no bitmap — their picture is a render — and nothing
		// asked for one until the cursor arrived, so a store full of materials
		// drew a grid of dashes until somebody swept the mouse over it.
		//
		// One at a time, because there is one slot and the round robin gives it
		// one turn in N. `RenderPreviewSlot` caches what it drew, so a row that
		// has had its turn keeps its picture and the queue moves on — a list
		// fills in over the next few frames and then costs nothing.
		//
		// **Hover still wins.** This runs before `DrawHoverPreview`, which
		// overwrites the request with whatever is under the cursor: the row
		// somebody is looking at is worth more than the one that happened to be
		// next.
		void PumpRenderedPreviews();

		// Rows drawn this frame whose picture is a render and is not cached yet.
		//
		// Recorded by `PaintPreview` while the list is drawn, so it is exactly
		// what was on screen — a queue built from the whole store would spend
		// the editor's frames on rows nobody had scrolled to.
		std::vector<std::string> PreviewQueue;

		// What one asset's preview is registered under in the texture table.
		//
		// **Prefixed, so a preview can never be sampled as content**: the
		// renderer resolves a part's texture out of the same table by name, and
		// a 64-pixel thumbnail under the real name would replace the real one.
		static std::string ThumbnailTextureName(const std::string &name);

	  private:
		// Decodes, scales and uploads one. Null when there is nothing to show.
		void *BuildThumbnail(const std::string &name, PreviewState &state);

		// One asset's preview, and when its row was last drawn.
		struct Entry {
			// Null for an asset with no preview — a mesh, an archive, a file
			// from another machine. **Cached as null rather than retried**, or
			// its row would queue a decode on every frame it is visible.
			void *Handle = nullptr;

			// Why, when there is no handle.
			PreviewState State = PreviewState::Pending;

			uint64_t LastSeen = 0;
		};

		std::unordered_map<std::string, Entry> Thumbnails;

		// Asked for while drawing, built by `PumpThumbnails`.
		std::vector<std::string> ThumbnailQueue;

		// Frames, for the eviction order. Not a clock — nothing here needs one.
		uint64_t ThumbnailClock = 0;

		// Keeps what the preview slot just drew, so a row can show it later.
		//
		// **This is what makes a rendered preview outlive the cursor.** Without
		// it a mesh or a material has a picture only while it is hovered, which
		// is `DEFERRED.md` D00033. Cheap and idempotent: an asset already
		// holding a handle is left alone, so the turntable does not recreate a
		// texture every frame.
		//
		// @param name The asset whose preview is in the slot.
		void CachePreviewThumbnail(const std::string &name);

		// Decodes, uploads and measures one mesh.
		PreviewState BuildPreviewMesh(const std::string &name);

		// Reads a material and uploads its colour map for the preview sphere.
		//
		// **Ready with an invalid `texture` is a real outcome, not a failure**: a
		// material that names no colour map previews as a bare sphere, which is
		// what that material actually puts on a part. A material that names one
		// this machine does not have is `Unavailable` instead, because a grey
		// ball would be a picture of something the material is not.
		//
		// @param name    The `.amat`'s published name.
		// @param texture Filled with the name the sheet was registered under, and
		//                left alone when there is nothing to sample.
		// @return Whether a preview can be drawn.
		PreviewState BuildPreviewMaterial(const std::string &name, engine::core::Name &texture);

		// What a preview mesh is registered under, prefixed so it can never be
		// drawn as content.
		static std::string PreviewMeshName(const std::string &name);

		// A mesh's true extent, for framing the camera on it.
		struct PreviewBounds {
			engine::core::Vector3 Centre;
			float Radius = 1.0f;

			// The texture the preview samples, or an invalid name for none.
			//
			// **Set for a material and left empty for a mesh**, which is the
			// whole of the difference between the two previews: a material is
			// the engine's sphere wearing its colour map, and a mesh is its own
			// geometry wearing nothing. Both are one instance in one slot, so
			// carrying the texture on the record rather than branching in
			// `RenderPreviewSlot` keeps that function about the camera.
			engine::core::Name Texture;
		};

		// Which meshes have been tried, and how each went.
		std::unordered_map<std::string, PreviewState> PreviewMeshes;

		// The bounds of the ones that loaded.
		std::unordered_map<std::string, PreviewBounds> PreviewMeshBounds;

		// What the preview slot should draw on its next turn, or empty.
		std::string PreviewWanted;

		// Whose picture is in the preview slot right now.
		//
		// **One slot, so one row.** `PaintPreview` draws this texture in place of
		// the kind glyph for the row it belongs to, which is what makes a hovered
		// mesh turn in its own 48-pixel square instead of staying an `M`. A row
		// that drew the slot without comparing names would show the hovered
		// mesh's picture in every mesh row on screen.
		std::string PreviewShowing;

		// The row the cursor is over this frame, before the delay.
		std::string HoverCandidate;

		// What kind that row's asset is.
		engine::assets::AssetKind HoverKind = engine::assets::AssetKind::Unknown;

		// Whether any row claimed the cursor this frame.
		bool HoverSeen = false;

		// The row the delay is counting for. **The token from the reference, as
		// state rather than a deferred callback** — a different row resets it,
		// so a superseded hover cannot land a quarter second after the cursor
		// left.
		std::string HoverPending;

		// How long it has been counted for.
		double HoverElapsed = 0.0;

		// What is actually on screen, or empty.
		std::string HoverShowing;

		// How wide that target should be.
		uint32_t PreviewSide = 132;

	  public:
		// Publishes `raw/` into `processed/`.
		//
		// @param hexSeed 64 hex characters of Ed25519 seed. Not stored.
		void PublishAssets(const std::string &hexSeed);

		// A modal listing the store's published assets of one kind.
		//
		// **What replaces typing a mesh name and finding out later.** An unknown
		// name is a part drawn with the missing-mesh marker, which is also what
		// a mesh that has not streamed in yet looks like — so the old text field
		// had no failure a person could see. See `AssetPicker.cpp`.
		//
		// @param title  The popup id, which `OpenPopup` was given.
		// @param kind   Which assets to list.
		// @param chosen The selection, in and out. Emptied by Clear.
		// @return `true` on the frame it is confirmed.
		bool DrawAssetPicker(const char *title, engine::assets::AssetKind kind, std::string &chosen);

		// Loads a mesh into the renderer so a preview can draw it.
		//
		// **Read from `raw/<name>` and decoded as `assets::Mesh`**, the same
		// path a thumbnail takes and for the same reason — no delivery client,
		// no chunk reassembly. A source format that has not been baked is
		// `Unavailable`, and the caption says to run `assetc`.
		//
		// @param name The asset's name.
		// @return Whether it can be previewed, and why not when it cannot.
		PreviewState LoadPreviewMesh(const std::string &name);

		// Asks the render rotation to draw this mesh into the preview slot.
		//
		// @param name The mesh, already through `LoadPreviewMesh`.
		// @param side How wide the target should be.
		// @return Whether the mesh is known well enough to draw.
		bool DrawPreviewViewport(const std::string &name, float side);

		// Draws whatever `DrawPreviewViewport` last asked for.
		//
		// **Called from `PresentWorld`'s rotation and never twice a frame**:
		// `Renderer::Render` owns the swapchain and the present, so a second
		// call a frame would be a second present. See `MeshPreview.cpp`.
		//
		// @return Whether it drew, so the rotation knows it spent its turn.
		bool RenderPreviewSlot();

		// Offers the item just drawn as the hover preview's subject.
		//
		// **Called right after the row's widget**, because it reads
		// `IsItemHovered`. A quarter-second delay and a token keep dragging the
		// cursor down a list from opening three hundred previews — see
		// `HoverPreview.cpp`, which takes both from `explorer-plus`.
		//
		// @param name The asset the row is about.
		// @param kind What it is, which decides whether the preview is a picture
		//        or a render.
		void HoverPreview(const std::string &name, engine::assets::AssetKind kind);

		// Settles the hover state once a list has finished drawing.
		//
		// **After the loop and never inside it**: every row calling this would
		// have each one cancelling the next.
		//
		// @param frameSeconds How long the last frame took, for the delay.
		void EndHoverPreview(double frameSeconds);

		// Draws the preview panel, if the delay has elapsed.
		void DrawHoverPreview();

		// Every asset the worlds name, one tile each. See `Gallery.cpp`.
		void DrawGallery();

		// Walks every world for every content-naming property.
		//
		// **Rebuilt on demand and never per frame**: this is the one thing in
		// the assets panel that scales with the scene rather than with the
		// store.
		void RebuildGallery();

		// Whether the gallery has been walked since it was last invalidated.
		//
		// **Separate from `Gallery.empty()`, which is what it used to be gated
		// on.** A world that names no assets produces an empty gallery, and
		// reading that as "not scanned yet" walked every entity of every world
		// on every frame the panel was open.
		bool GalleryScanned = false;

		// Selects every instance using one asset.
		//
		// The gesture the gallery exists for — "where is this texture actually
		// used" is the question somebody has before they change one.
		void SelectGalleryUsers(const GalleryEntry &entry);

		// Re-reads the store's manifest into `PickerContents`.
		//
		// **Called when a picker opens and by its Refresh button, never per
		// frame.** Reading a manifest is opening and parsing a file.
		void RefreshPickerContents();

		// Whether the picker is showing `raw/` rather than the manifest.
		bool PickerShowRaw = false;

		// Draws the raw half of the picker: unbaked sources, baked when picked.
		void DrawRawPickerRows(engine::assets::AssetKind kind, std::string &chosen, bool &confirmed);

		// The Use/Cancel/Clear row and the popup's close. Shared by both tabs.
		bool FinishAssetPicker(std::string &chosen, bool confirmed);

		// A raw entry's path relative to `raw/`, which is what a baker takes.
		static std::string RawRelativePath(const cdn::RawEntry &entry);

		// Bakes one source out of `raw/` into `baked/`, now.
		//
		// **On demand, because a whole-store bake is minutes.** A picker that
		// republished to make one file selectable would be one nobody waits for
		// — `assetc::Settings::Only` carries why it is a filter on the real walk
		// rather than a second baker.
		//
		// @param relative The source's path under `raw/`.
		// @param baked    Set to the baked name, which is what a scene writes.
		// @return `false` when it could not be baked. `AssetStatus` says why.
		bool BakeRawAsset(const std::string &relative, std::string &baked);

		// Hands a freshly baked file to this editor's renderer.
		//
		// **So a picked asset appears in the viewport before any publish**,
		// which is the whole point of baking on demand. Textures and meshes
		// only; everything else reaches a runtime through a publish.
		void RegisterBakedAsset(const std::filesystem::path &path, const std::string &name);

		// What is moving between this editor and its origins.
		//
		// **The panel that makes `ContentSources` observable.** The settings
		// were saved and loaded since v0.9 and nothing ever built a client from
		// them, so a wrong key and a working one looked identical. See
		// `Network.cpp`.
		void DrawNetwork();

		// Who else is editing, and how to invite them. `TeamCreate.cpp`.
		void DrawTeamCreate();

		// The colours a panel was given, by the title imgui identifies it with.
		//
		// @param panel The panel's title.
		// @return Its override, or an empty one when it has none. The reference
		//         is valid until `Prefs.PanelColours` is next changed.
		// @since v0.13
		const engine::ui::ThemeColours &PanelColoursFor(const char *panel) const;

		// Draws one panel with whatever colours it was given.
		//
		// **The push has to bracket `Begin` and `End`, not sit inside them** —
		// a window's background is read at `Begin`, so an override pushed within
		// the window colours everything except the window. Which is why this
		// wraps the call rather than living in each panel: one place to get it
		// right, and a panel author cannot get it wrong.
		//
		// @param panel The panel's title, as the settings panel lists it.
		// @param body  The draw call.
		// @since v0.13
		template <typename Body>
		void Skinned(const char *panel, Body &&body) {
			const engine::ui::ScopedColours skin(PanelColoursFor(panel));
			body();
		}

		// A prompt that runs a line of Luau against the scene being edited.
		// `CommandBar.cpp`.
		//
		// @since v0.13
		void DrawCommandBar();

		// Runs one command, bracketed by a history recording.
		//
		// **The whole run is one waypoint**, which is the point: a line that
		// moves forty parts is one press of Ctrl+Z, not forty. A run that
		// changed nothing commits nothing, so an empty step never appears in
		// the Edit menu for somebody who typed a query.
		//
		// @param source The Luau to run.
		// @return Whether it compiled and ran without raising.
		// @since v0.13
		bool RunCommand(const std::string &source);

		// Walks the command history for the input's arrow keys.
		//
		// **Public because imgui's callback is a free function** and the only
		// way it reaches an editor is through the user pointer it carries.
		//
		// @param data The callback's data.
		// @return Zero, which is what imgui expects of a history callback.
		// @since v0.13
		int WalkCommandHistory(ImGuiInputTextCallbackData *data);

		// The control surface: whether it is listening, and what it offers.
		//
		// **A panel rather than a log line, because the interesting facts about
		// it change.** Whether a client is attached, how many requests have been
		// answered and which tools this program declares are all things somebody
		// checks while debugging an agent, and the startup line said the first
		// two once and the third never.
		void DrawControl();

		// Starts the control server, or stops it.
		//
		// **One button rather than a setting that needs a restart.** The port is
		// a setting; whether the socket is open is a decision somebody makes
		// while working, and an editor that had to be relaunched to answer an
		// agent is one nobody would use that way.
		void ToggleControl();

		// Discovers, starts and beats the plugins. See `studio/Plugins.hpp`.
		//@{
		void LoadPlugins();
		void PumpPlugins(float delta);
		void DrawPlugins();

		// Draws every open plugin panel, each in its own window.
		void DrawPluginWidgets();

		// Calls one of a plugin's handlers, counting a raise as a fault.
		//
		// @param plugin   Whose handler.
		// @param callback What to call.
		// @param drawing  Whether the widget calls are legal inside it.
		void InvokePlugin(
			LoadedPlugin &plugin,
			engine::script::HostCallback callback,
			bool drawing,
			engine::script::HostArguments arguments = {}
		);

		// Points the command log at the plugins and, when one is open, at the
		// team-create edit stream.
		//
		// **One watcher, fanned out here.** `CommandLog` has one seam because a
		// log that knew what a plugin was would be a log that knows what an
		// editor is; deciding who hears about a waypoint is this class's job
		// and nobody else's.
		//
		// @since v0.13
		void InstallHistoryWatcher();

		//@}

		// Publishes the selection into the world as `studio.Selected`.
		//
		// **The bridge a plugin reads the selection through**, and the reason
		// there is no selection *API*: a selection is per-entity state about the
		// world, which is what a component is for. Called once a frame from the
		// plugin pump, and only when something changed — a tag written every
		// frame would move `Store::ChangeVersion` every frame and defeat the
		// gate the physics broad phase reads.
		void PublishSelection();

		// **The plugin surface is a friend rather than a widening of this
		// class.** It is the editor's own half of the seam and lives in
		// `PluginSurface.cpp`; making `Select` public so one file could call it
		// would offer it to every other file too, which is the opposite of what
		// a plugin API is for.
		friend class PluginSurface;

		// The active world's name, for a plugin that wants to say which scene it
		// is looking at.
		//
		// @return The name, or empty when there is none.
		std::string ActiveWorldName() const;

		// Whether there is a scene to act on.
		//
		// @return `true` when a world is active.
		bool HasActiveWorld() const;

		// Runs `body` against the world the selection belongs to.
		//
		// **One entry point, because a plugin call is not allowed to guess.**
		// Every host call that touches storage goes through this, so a call made
		// when nothing is open is a no-op rather than a crash — and there is one
		// place that knows which world "the world" means.
		//
		// @param body What to run. Not called when there is no world.
		void WithSelectionWorld(const std::function<void(engine::ecs::Store &)> &body);

		// What was published last, so the frame that changed nothing does
		// nothing. Sorted, because it is compared against a sorted selection.
		std::vector<Entity> PublishedSelection;

		// Reads the config folder, and moves anything found beside the binary.
		//
		// **The move is one-way and happens once.** `studio-content.ini` and
		// `studio-keybinds.ini` lived in `Paths::Base()`, which is the build
		// directory for anybody working on the engine — so a `just build`
		// against another preset read as a fresh install and deleting the build
		// directory threw somebody's keybinds away. Reading the old location
		// when the new one has nothing is what stops this change from being that
		// same loss one more time.
		void LoadConfiguration();

		// Writes the config folder, on the way out.
		//
		// **Not on every edit**, which is `Keybinds::Save`'s own rule: the
		// preferences page changes a value as somebody drags a slider, and a
		// file rewritten per frame is a file that records a half-finished drag.
		void SaveConfiguration();

		// Builds the delivery client and the uploader from the current sources.
		//
		// Called at start-up and whenever the Content page is edited, because a
		// role or an address changed there decides which of the two a row
		// belongs to.
		void RebuildContentClients();

		// Drives both, and samples what they have moved.
		//
		// @param frameSeconds How long the last frame took.
		void PumpContent(double frameSeconds);

		// Takes what the delivery client has finished and registers it.
		//
		// **The editor fetches content, which it did not before at all.** Its
		// delivery client existed and nothing ever asked it for anything, so a
		// `MeshPart` drew the fallback cube however good its `MeshId` was.
		void DrainContent();

		// Reshapes every part naming this mesh to the mesh's own proportions,
		// keeping the size each part already has along its longest axis.
		//
		// **Because `Size` is a box the mesh is stretched into.** A part whose
		// box is the wrong shape distorts whatever is put in it, and only the
		// geometry knows the right shape. Idempotent, so it is safe to run
		// whenever a mesh arrives.
		//
		// @param mesh   The mesh that arrived.
		// @param extent Its own half-extent, from the renderer's table.
		void FitPartsToMesh(const engine::core::Name &mesh, const engine::core::Vector3 &extent);

		// Rebuilds `PublishedMeshNames` from the signed manifest.
		//
		// **Names, not content.** It is what makes
		// `ContentService:GetPublishedMeshes()` answerable, and naming one of them
		// is still what fetches it — see `scene/PublishedCatalogue.hpp`.
		void PublishManifestNames();

		// Makes sure every open world holds `PublishedMeshNames`.
		//
		// **Every pump, because worlds appear after the catalogue does** — Play
		// mints a server world and a client replica, and both run scripts.
		// Guarded on the count, so the steady case is a comparison.
		void OfferPublishedNames();

		// The mesh names the store published, filtered to what a runtime reads.
		std::vector<engine::core::Name> PublishedMeshNames;

		// Asks for what the open worlds name and has not been asked for, a
		// bounded number of them per pump.
		//
		// **Bounded because the unit that travels is a bundle**, and asking for
		// everything at once resolves, verifies and decompresses all of it on
		// this thread — which is what made the editor freeze for half a minute
		// on start-up before anything was demand-driven.
		void RequestShownContent();

		// Asks for one asset, once.
		//
		// @return Whether this call issued a request, so the caller can bound
		//         how many it starts in one pump.
		bool RequestContentAsset(const engine::core::Name &asset);

		// Runs `body` inside every open world.
		void EachOpenWorld(const std::function<void(engine::ecs::Store &)> &body);

		// Whether the catalogue has been seen to open.
		//
		// **Not "whether the requests have been issued", which is what this used
		// to mean.** Nothing is requested up front any more — a world names an
		// asset or it is not fetched — so this only gates `RequestShownContent`
		// on there being a catalogue to ask.
		bool ContentRequested = false;

		// The total the summary last reported, so the next one is logged only
		// when something has actually arrived since.
		//
		// **A total rather than a flag**, because an editor keeps naming content
		// after start-up — see `DrainContent`, where a once-only report meant
		// every asset chosen from a picker loaded without a word.
		size_t ContentReportedTotal = 0;

		// How much of each kind has been registered into the renderer.
		//
		// Reported together, because "7 meshes, 16 textures" is what says a
		// scene arrived and a bare total is not — a store that delivered every
		// texture and no mesh reads as working until somebody looks at it.
		//@{
		size_t ContentMeshes = 0;
		size_t ContentTextures = 0;

		// How many shader modules the content store delivered.
		//
		// **Counted, because a pipeline naming a shader that never arrived draws
		// nothing and says so once.** A number beside the mesh and texture
		// counts is how somebody tells "the pipeline is wrong" from "the content
		// has not landed".
		//
		// @since v0.11
		size_t ContentShaders = 0;
		size_t ContentMaterials = 0;
		//@}

		// Fetches still in flight.
		std::vector<engine::delivery::RequestId> ContentPending;

		// How much delivered content this frame will decode and upload.
		//
		// Held across frames rather than made in the loop so the allowance is
		// one object with one meaning; `Begin` is what resets it.
		engine::delivery::IntakeBudget ContentBudget;

		// Requests made while `ContentPending` was being walked.
		std::vector<engine::delivery::RequestId> ContentIssued;

		// Which texture names have been asked for, by `core::Name::Id`.
		std::unordered_set<uint32_t> ContentAsked;

		// Queues every file in the local store's `raw/` for every write source.
		void UploadStore();

		// Asks the delivery client for one asset by name.
		//
		// @param name The content name, as the signed manifest spells it.
		void DownloadAsset(const std::string &name);

		// Files what has arrived into the local store.
		void CollectDownloads();

		// What crosses between worlds, which is the one thing they share.
		//
		// **The rule that nothing crossing a world boundary is a pointer is what
		// makes this panel possible at all.** Every crossing is already a copy
		// with a topic and a payload, so this reads what is there rather than
		// instrumenting a path.
		void DrawBus();

		// Instances matching a class and a property predicate.
		//
		// **Generic over properties for the same reason the properties panel
		// is**: `PropertyDescriptor` is data, so this names no property and
		// gains one the day any module declares it.
		void DrawFindInstances();

		// Rebuilds `FindResults` from `Find` across every world.
		void RunFind();

		// Which script is spending the tick.
		//
		// Reads the interrupt counter the step budget already maintains, so the
		// number is one the VM was counting anyway.
		void DrawScriptProfile();

		// What has changed since the file on disk was written.
		void DrawDiff();

		// Re-reads the file and re-writes the live game, then compares them.
		void RefreshDiff();

		// Builds a Rojo project's tree into the active world.
		//
		// **Into the active world rather than a new one**, because a project is
		// how somebody lays out a game they are already working on — and a sync
		// that made a world of its own would leave them with the scene in one
		// place and the scripts in another.
		//
		// @param project The path to `default.project.json`.
		void SyncRojo(const std::filesystem::path &project);

		// Builds every world a `main.universe.json` names.
		//
		// **The universe counterpart of `SyncRojo`, and a separate command
		// rather than a mode of it.** One project file is one world, which is
		// what an author editing a scene wants; a universe file is a whole game
		// laid out as a folder of them, which is what an author opening a
		// checkout wants. Folding the two into one path would mean guessing
		// which they meant from the file's contents.
		//
		// Every world syncs on its own, so a project file with a typo costs its
		// own world and nothing else — the report names which.
		//
		// @param universe The path to `main.universe.json`.
		void SyncRojoWorlds(const std::filesystem::path &universe);

		// Breakpoints, the paused stack, and stepping.
		void DrawDebugger();

		// One frame's locals or upvalues, as a two-column table.
		//
		// @param values What to show, or null when no frame is chosen.
		// @param empty  What to say when there are none, in words that give the
		//               reason rather than only the absence.
		void DrawDebugValues(const std::vector<engine::script::DebugLocal> *values, const char *empty);

		// Which capture the debugger panel is showing.
		//
		// **An index rather than a pointer, because the hit log is bounded and
		// rolls.** A pointer into it would dangle the moment a loop pushed the
		// sixty-fifth capture; an index that has gone past the end reads as
		// "pick a capture", which is the honest answer.
		//@{
		uint32_t SelectedWorld = 0;
		size_t SelectedHit = 0;
		size_t SelectedFrame = 0;
		//@}

		// Reverses the last edit, and tells the author what was reversed.
		//
		// **The selection is cleared rather than repointed.** Undoing a delete
		// gives the subtree a new root handle, so whatever the selection held is
		// either dead or about to be misleading — and a selection pointing at a
		// handle that has been recycled is the failure `CommandLog` is written
		// to avoid, reintroduced one layer up. Clearing is honest; guessing is
		// not.
		void UndoEdit();

		// Reapplies the last undone edit. See `UndoEdit` for the selection.
		void RedoEdit();

		// Whether an instance is in the current selection.
		//
		// @param instance The instance to test.
		// @return `true` when it is selected.
		bool IsSelected(Entity instance) const;

		// --- actions ----------------------------------------------------------

		// Replaces everything with an empty game.
		//
		// **Refused while anything is running**, for the reason the Worlds panel
		// states: the snapshot Stop restores was taken before the run began.
		void NewGame();

		// Loads a game file, adopting its path.
		//
		// @param path The `.agame` to read.
		// @return `false` when it could not be read or is not one.
		bool OpenGame(const std::filesystem::path &path);

		// Writes the whole game, adopting the path.
		//
		// **Adopting is what separates this from `ExportActiveWorld`**: the
		// title changes, the modified marker clears, and Ctrl+S writes here from
		// then on.
		//
		// @param path Where to write.
		// @return `false` when it could not be written.
		bool SaveGame(const std::filesystem::path &path);

		// Writes the active world alone, without adopting the path.
		//
		// @param path Where to write.
		// @return `false` when it could not be written.
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
		// Adds one world file to this universe, keeping what is here.
		//
		// A world whose name is taken arrives under a suffixed one rather than
		// being refused — an import should never silently replace a scene.
		//
		// @param path The world file to read.
		// @return `false` when nothing could be imported.
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
		void
		InstallExampleScript(engine::ecs::Store &store, std::string_view file, std::string_view instanceName);

		// Marks every instance in a world as expanded in the explorer.
		//
		// **A request rather than a state, which is what `Expanded` already
		// is.** The set is consumed the first time the tree draws, so this says
		// "open these once" and imgui owns what is open from then on — a tree
		// that re-expanded every frame could not be collapsed.
		//
		// @param world The world whose instances to open.
		void ExpandWorldTree(WorldId world);

		// Adds an empty world to the universe.
		//
		// @param name What to call it.
		// @return The new world, or an invalid id when the name is taken.
		WorldId AddWorld(engine::core::Name name);

		// Removes a world and everything in it.
		//
		// **Never the last one**, for `UpdateWorldLifecycle`'s reason: a
		// universe with nothing in it is a game that has stopped without saying
		// so.
		//
		// @param world The world to remove.
		void RemoveWorld(WorldId world);

		// Creates one instance and selects it.
		//
		// @param world  Which world.
		// @param klass  What to create.
		// @param parent Where to put it, or a null entity for the root.
		// @return The new instance, or a null entity when the class is unknown.
		Entity InsertInstance(WorldId world, engine::ecs::ClassId klass, Entity parent);

		// Destroys everything selected, as one undoable edit.
		//
		// **One edit and not one per instance**, so undoing a multi-selection
		// delete brings all of it back rather than the last one.
		void DeleteSelection();

		// Clones everything selected and selects the copies.
		void DuplicateSelection();

		// Opens a script in a tab, or focuses the tab it is already in.
		//
		// @param world    Which world it lives in.
		// @param instance The `Script` to open.
		void OpenScriptTab(WorldId world, Entity instance);

		// Writes a tab's text back into its instance.
		//
		// **Into the instance rather than to disk**, because a script *is* the
		// game file's content — saving the game is what puts it on disk.
		//
		// @param tab The tab to save.
		void SaveScriptTab(OpenScript &tab);

		// Closes a tab, discarding nothing — the text is already in the
		// instance if it was saved, and a tab is a view rather than a document.
		//
		// @param index Which tab.
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
		// Records that the game differs from what is on disk.
		//
		// **Every path that edits the world calls this**, which is why it is a
		// method rather than a flag somebody sets: a missed call is a game that
		// closes without offering to save.
		void MarkModified();

		// The window title: the game's name, its path, and a marker when it is
		// modified.
		//
		// @return The title to set.
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

		// What this editor was started with.
		Options Settings;

		// Where this editor fetches content from, in priority order.
		ContentSources Content;

		// Beside the binary with the layout ini and the keybinds, so a
		// launcher's working directory cannot move somebody's configuration.
		std::filesystem::path ContentSourcesPath;

		// What was configured, and what is remembered between sessions.
		//
		// **The file is the source and this is the copy the frame reads.** Every
		// field here is written back on the way out from whatever the interface
		// left it at, which is why the panel toggles are read off the live flags
		// rather than out of this — see `SaveConfiguration`.
		Preferences Prefs;

		// The last five games opened, most recent first. See `Config.hpp`.
		RecentProjects Recent;

		// The listener and the table. Held by value and started only when asked;
		// a server that was never started costs a thread that was never spawned.
		engine::control::Server ControlServer;
		// What the control server exposes, and how it describes itself to
		// whatever connected.
		engine::control::Surface ControlSurface{
			"atomic-studio",
			"The editor of the atomic game engine, live. `engine_info` and `world_list` are the two "
			"worth calling first: the universe is the game and each world under it is a scene, which "
			"is the same mapping a script sees through `game` and `workspace`. Worlds run "
			"independently — `world_run` starts one without starting the rest, and stopping restores "
			"the snapshot taken when it started."
		};

		// The window and the things that draw into it.
		//
		// **Held by value and in this order**, because destruction runs
		// backwards: the clock and the interface go before the renderer, and the
		// renderer before the window it borrowed its device from.
		//@{
		SDL_Window *Window = nullptr;
		engine::render::Renderer Renderer;
		engine::render::OverlayImage Overlay;
		engine::ui::Interface Interface;
		engine::core::FrameClock Clock;
		//@}

		// Held by pointer because a universe binds its driver thread on
		// construction, and that thread is decided in `Initialise` rather than
		// wherever this object was declared. Same reason `client::Client` holds
		// its own that way.
		std::unique_ptr<engine::world::Universe> Universe;

		// Undo and redo. Held the same way and for a narrower version of the
		// same reason: it binds to the universe above, so it cannot exist before
		// there is one to bind to.
		//
		// **Not world state**, per the note at the top of this file — see
		// `Commands.hpp` for why a log that travelled with a world would be
		// restored by Stop describing edits the restore had discarded.
		std::unique_ptr<CommandLog> Commands;

		// Every command this editor offers. Built by `RegisterOperators`.
		OperatorTable Operators;

		// Whether the palette is up, and what has been typed into it.
		//
		// **Not saved in the layout**, unlike a panel's open flag: a palette
		// that came back open on start-up would be a modal in front of the
		// window before anybody had asked for one.
		bool ShowPalette = false;

		// What has been typed into it.
		std::string PaletteQuery;

		// Which row the arrow keys have moved to. Reset whenever the query
		// changes, because the row that was second for one query is not
		// meaningfully the second for the next.
		int PaletteCursor = 0;

		// The game's name and where it came from. An empty path is a game that
		// has never been saved, which is what makes Save fall through to Save
		// As rather than writing somewhere arbitrary.
		//@{
		engine::core::Name GameName;
		std::filesystem::path GamePath;
		bool Modified = false;
		//@}

		// The world the viewport draws and the properties panel edits.
		WorldId Active;

		// What is selected, and which world it is in. Cleared whenever `Active`
		// changes, because a selection in a world nobody is looking at is a
		// delete somebody does not see.
		//@{
		WorldId SelectionWorld;
		std::vector<Entity> Selection;
		//@}

		// One record per world the explorer has drawn. See `WorldTree`.
		std::vector<WorldTree> Trees;

		// Worlds the explorer has been told to expand, by world index.
		//
		// **Apart from `WorldTree::Open` because the key spaces are
		// different.** A world is identified by a small index and an instance
		// by a 64-bit handle, and putting both in one set would have world 3
		// open whatever happened to be entity 3.
		std::vector<uint32_t> ExpandedWorlds;

		// The instance whose name is being typed in the tree, and what has been
		// typed so far.
		//
		// **The buffer is the editor's rather than the store's**, for the
		// reason every panel here reads the store every frame: a half-typed
		// name is not world state, and writing each keystroke through would put
		// one undo entry on the stack per character.
		//@{
		Entity Renaming;
		std::string RenameBuffer;
		//@}

		// Whether the rename field still has to be given the keyboard.
		//
		// One frame's worth of request. imgui's `SetKeyboardFocusHere` applies
		// to the *next* item submitted, so it has to be asked for on the frame
		// the field first appears and not afterwards, or every frame steals
		// focus back and the caret can never be moved.
		bool RenameFocus = false;

		// A rename the context menu asked for.
		//
		// A `Pending*` like the rest: the menu is drawn inside `Universe::Enter`
		// and `BeginRename` enters the world to read the name it seeds with.
		Entity PendingRenameStart;

		// Where a shift-click measures its range from.
		//
		// The last row clicked without shift. Roblox's explorer works the same
		// way, and so does every file list: shift extends from the last plain
		// click rather than from whichever end of the selection is nearer.
		Entity SelectionAnchor;

		// Whether the explorer should open the path to the selection and scroll
		// to it, once.
		//
		// **Set by whatever selected from outside the panel** — a viewport
		// click, a Find result, an undo — and never by the tree itself, because
		// scrolling the row somebody just clicked on to the middle of the panel
		// moves the thing under their cursor.
		bool RevealSelection = false;

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

		// TODO(render-pipeline): `PipelineSelected` mapped world index to the
		// pipeline key installed for it — a map rather than one name, because two
		// viewports can show two worlds in the same frame, which is exactly what
		// `InstallWorldPipelines` qualified its keys for. An absent entry meant
		// "install on the next frame", and saving a pipeline erased the entry,
		// which is what made a save visible in the viewport.

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
			//
			// **Several, because one client cannot show a disagreement.** The
			// bugs a play test is for — an entity that arrives on one client and
			// not another, a value that replicates late to the second joiner —
			// are invisible with a single replica, because there is nothing for
			// it to disagree *with*. Each entry is an independent client with
			// its own world, its own `Replica` and its own report; N is a loop
			// over the machinery one already needed.
			std::vector<std::unique_ptr<PlayLink>> Links;
		};

		// Every world currently running. Worlds absent from this are in edit.
		std::vector<WorldRun> Runs;

		// This world's run, or null when it is being edited.
		//
		// @param world The scene to ask about.
		// @return The record, or null.
		//@{
		WorldRun *RunOf(WorldId world);
		const WorldRun *RunOf(WorldId world) const;
		//@}

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

		// The open script tabs, and which one is in front.
		//
		// **An index rather than a pointer**, because closing a tab moves the
		// rest — a pointer would outlive the element it named by exactly one
		// erase. -1 is no tab open.
		//@{
		std::vector<OpenScript> Scripts;
		int ActiveScript = -1;
		//@}

		// How much bigger the code is drawn than the interface around it.
		//
		// **One zoom for the panel rather than one per tab.** Somebody who
		// wants larger code wants it in every script they open, and a
		// per-tab zoom is a setting that appears to reset itself every time
		// they switch file.
		float ScriptZoom = 1.0f;

		// The same, for the output panel.
		//
		// **Its own number rather than the script editor's.** They are read in
		// different situations — code somebody is writing, and a log somebody
		// is squinting at — and one setting would mean making a stack trace
		// legible also made every script enormous.
		//
		// @since v0.13
		float OutputZoom = 1.0f;

		// The selected span of the output, as the serials at each end.
		//
		// **An anchor and a head rather than a first and a last**, so a drag
		// that goes upwards selects what it crosses rather than nothing: the
		// anchor is where the mouse went down and the head is where it is now,
		// and either may be the larger.
		//
		// Equal and zero means nothing is selected, which is safe because a
		// serial is never zero.
		//
		// @since v0.13
		//@{
		uint64_t OutputAnchor = 0;
		uint64_t OutputHead = 0;
		//@}

		// What the next line's serial will be. Never zero.
		uint64_t NextOutputSerial = 1;

		// The output panel's lines, oldest first.
		//
		// A deque because it is trimmed from the front at `OUTPUT_LIMIT` and
		// appended at the back, which is the one shape a vector is bad at.
		std::deque<Message> Output;

		// The engine log, teed into that panel. See `PanelSink` in `Editor.cpp`:
		// a script's `print` and the error that stopped it are the two things an
		// author actually wants there, and both arrive through the logger rather
		// than through this class.
		std::shared_ptr<class PanelSink> Sink;

		// --- the viewport ------------------------------------------------------

		// Puts this panel's own camera in the world it is showing, and names it
		// the world's live one.
		//
		// **The camera is the viewer's, not the game's.** The editor makes one
		// to show its point of view, a client makes one for its player, and when
		// several people edit one game they each make their own — so it carries
		// `scene::TransientComponent` and is never written into a game file.
		// Everything else about it is ordinary: it is in `Workspace`, the
		// explorer shows it, the properties panel edits it, and a script sees it
		// where it expects the current camera to be.
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

		// The main viewport's eye.
		//
		// **Yaw and pitch are kept beside the frame rather than derived from
		// it.** Recovering them from a rotation is ambiguous at the poles, and
		// the pitch clamp needs an angle it can compare — which is why the
		// camera never rolls.
		//@{
		engine::core::CFrame CameraFrame;
		float CameraYaw = 0.0f;
		float CameraPitch = 0.0f;
		float CameraSpeed = 24.0f;
		//@}

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

			// The texture this panel's world is drawn into.
			engine::render::SceneTarget Target;

			// This panel's own eye, for the main viewport's reason.
			//@{
			engine::core::CFrame Frame;
			float Yaw = 0.0f;
			float Pitch = 0.0f;
			float Speed = 24.0f;
			//@}

			// What the pointer is doing to this panel.
			//
			// A drag holds `Active` or `Panning` until the button is released
			// even when the pointer leaves — a look that stopped at the edge
			// would be unusable exactly when somebody is turning quickly.
			//@{
			bool Hovered = false;
			bool Active = false;
			bool Panning = false;
			//@}

			// Whether the panel exists at all.
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
		// What one viewport panel handed the overlay pass this frame.
		//
		// **The overlay is deferred rather than drawn where the panel is, and
		// the frame order is why.** `DrawViewport` runs at the top of
		// `DrawInterface`; `DriveCamera` runs near the bottom, because it needs
		// the hover and active state the viewport's `InvisibleButton` produces;
		// and `PresentWorld` renders the world *after* the whole imgui frame,
		// with the camera `DriveCamera` just set.
		//
		// So an overlay projected while the panel drew is one `DriveCamera`
		// behind the pixels it sits on, and the grid swims against the ground
		// whenever the camera moves. Keeping the panel's draw list and appending
		// to it after `DriveCamera` — still inside the same imgui frame — costs
		// nothing and removes the lag rather than sharing it.
		//
		// @since v0.7
		struct OverlaySlot {
			// The panel's own draw list, valid for the rest of this frame.
			struct ImDrawList *List = nullptr;

			// The panel-space rectangle the world image occupies, in screen
			// coordinates because that is what a draw list takes.
			//@{
			float X = 0.0f;
			float Y = 0.0f;
			float Width = 0.0f;
			float Height = 0.0f;
			//@}

			// Whether this panel drew at all this frame. A closed panel returns
			// early and leaves this false.
			bool Drawn = false;
		};

		// How many viewport panels there are beyond the main one.
		//
		// A fixed number rather than a growable list, because each costs a scene
		// target — device memory that exists whether the panel is open or not.
		static constexpr size_t EXTRA_VIEWPORTS = 3;

		// **The slot the asset preview owns, past every viewport panel.**
		// Sharing one with a viewport would make the preview and that panel
		// overwrite each other's texture — `PresentWorld` keeps a slot per panel
		// precisely so two things of different sizes do not reallocate one
		// target twice a frame.
		static constexpr size_t PREVIEW_SLOT = 1 + EXTRA_VIEWPORTS;
		// The extra panels, whether or not they are open.
		std::array<ViewportState, EXTRA_VIEWPORTS> Extras;

		// A panel's own camera instance, and the world it was minted in.
		//
		// **The world is held beside the entity because an entity handle alone
		// cannot say where it lives.** A panel can be repointed at another
		// world, and the camera it made in the old one has to be destroyed
		// there — `Universe::Enter` needs the id to do it, and by then the panel
		// is already showing something else.
		struct ViewerCamera {
			// The camera and the world it was minted in.
			//@{
			WorldId World;
			Entity Instance;
			//@}
		};

		// Indexed the way `DrawingViewport` is: 0 is the main panel, 1.. are the
		// extras, so a panel index is a subscript rather than a branch.
		std::array<ViewerCamera, 1 + EXTRA_VIEWPORTS> Viewers;

		// What each panel handed this frame's overlay pass. Cleared as each
		// panel draws, so a closed one contributes nothing.
		std::array<OverlaySlot, 1 + EXTRA_VIEWPORTS> Overlays;

		// The game's own UI, compiled per panel and kept between frames.
		//
		// **One per panel and not one per editor**, because a panel is a canvas:
		// two viewports showing the same world at different sizes resolve every
		// `UDim2` differently, and a shared compile would give whichever panel
		// drew second the other one's rectangles.
		//
		// Kept across frames deliberately — that is the whole of what
		// `gui::Compiled` is for. A fresh one per frame would compute a
		// signature, find nothing to compare it against and rebuild every time.
		std::array<engine::gui::Compiled, 1 + EXTRA_VIEWPORTS> GuiLists;

		// The hover and press state behind those lists, per panel for the same
		// reason. Editor state, not world state: nobody replicates where a
		// mouse is.
		std::array<engine::gui::Router, 1 + EXTRA_VIEWPORTS> GuiRouters;

		// A click in a viewport, waiting to be turned into a selection.
		//
		// A `Pending*` like every other action a panel offers, for the reason
		// the group of them gives: picking enters a store, and a panel draws
		// from inside `Universe::Enter` and acts from outside it.
		//
		// @since v0.7
		struct PendingPickAction {
			// Which panel the click was in, and where in it.
			//@{
			size_t Viewport = 0;
			float X = 0.0f;
			float Y = 0.0f;
			//@}

			// Whether to add to the selection rather than replace it.
			bool Add = false;

			// Whether there is a click waiting at all.
			//
			// **Separate from the coordinates, because (0, 0) is a real
			// place.** A sentinel position would make the top-left corner
			// unclickable.
			bool Wanted = false;
		};

		// The click waiting to become a selection, if any.
		PendingPickAction PendingPick;

		// Which manipulator the viewport is offering.
		//
		// **A mode rather than three gizmos drawn at once.** Studio and every
		// other editor does it this way for one reason: three sets of handles
		// over one object is a target nobody can hit, because the ones you are
		// not using are in front of the one you are.
		//
		// @since v0.7
		enum class ToolMode : uint8_t {
			// No handles. A click selects and nothing else, which is what you
			// want while placing the camera or reading a scene.
			Select,

			// Drag an axis to move along it.
			Move,

			// Drag a ring to turn about its axis.
			Rotate,

			// Drag an axis to grow along it.
			Scale,
		};

		// Which manipulator is offered right now.
		ToolMode CurrentTool = ToolMode::Select;

		// Which faces a scale drag moves. See `studio::ScaleSide`.
		//
		// **Mirrors `Prefs.Sides`, like the snap fields beside it**, and for the
		// same reason the root `AGENTS.md` gives: the frame reads this and the
		// file is written from it on the way out.
		//
		// @since v0.13
		ScaleSide ScaleSides = ScaleSide::Side;

		// Snap steps, and whether they are on.
		//
		// **Off by default.** Snapping is a constraint somebody turns on for a
		// job, and an editor that quietly rounded every drag would be an editor
		// that cannot place anything where it was asked to.
		//@{
		bool SnapEnabled = false;
		float SnapDistance = 1.0f;
		float SnapDegrees = 15.0f;
		//@}

		// Whether the handles edit the pivot rather than the placement.
		//
		// **A mode over the same handles, which is Roblox's "Edit Pivot" and is
		// the right shape.** A pivot is a `CFrame` in the part's own space, so
		// moving one is the same drag arithmetic pointed at `PivotOffset`
		// instead of at `Transform::Frame` — a second set of gizmos would be a
		// second set of handles over one object, which the `ToolMode` comment
		// above already rules out for the same reason.
		//
		// **The part does not move while this is on.** That is the whole point:
		// a door whose hinge is in the wrong place is fixed by moving the hinge,
		// and an editor that moved the door with it would leave the author
		// exactly where they started.
		//
		// @since v0.12
		bool PivotEditing = false;

		// Puts every selected instance's pivot back at its centre.
		//
		// **A button rather than typing zeroes into three fields**, which is
		// what it replaced: `PivotOffset` is a `CFrame` and the properties panel
		// spells it as a position and an orientation, so undoing a pivot edit by
		// hand is six numbers and a chance to get one wrong.
		//
		// Recorded as one command, so it undoes in one press.
		void ResetSelectionPivot();

		// Sets a boolean property on everything selected.
		//
		// **One function for `Anchored` and `Locked`**, because the two toolbar
		// buttons differ only in the name they write — and a second copy of "walk
		// the selection, write a bool, record a command" is the duplicate that
		// drifts the first time one of them learns about mixed selections.
		//
		// @param property What to write.
		// @param value    What to write to it.
		// @param label    What the undo entry is called.
		void SetSelectionFlag(const char *property, bool value, const char *label);

		// Whether every selected instance already reads `true` for a property.
		//
		// **Used to decide what a toggle button does next.** A mixed selection
		// answers `false`, so the first press turns everything on rather than
		// half of it off — which is what a person pressing one button means.
		//
		// @param property What to read.
		// @return `true` when everything selected has it set.
		bool SelectionFlag(const char *property) const;

		// The ribbon's four strips, one per tab. `Tools.cpp`.
		//
		// **Called by `DrawToolbar`, not by a panel.** Each is one row of the
		// pinned strip under the menu bar; the tab bar that chooses between them
		// is on the row above. Split so that each is a list of controls rather
		// than a list of controls with a `BeginTabItem` between every fourth
		// one.
		//@{
		void DrawHomeTools();
		void DrawModelTools();
		void DrawScriptTools();
		void DrawViewTools();
		//@}

		// A button that runs a registered command, greyed with its reason.
		//
		// @param id    Which command.
		// @param label What the button says.
		void OperatorButton(Action id, const char *label);

		// A translate gizmo, mid-drag.
		//
		// **One command per drag, not one per frame.** A drag that recorded on
		// every frame it moved would fill the undo stack with a hundred entries
		// describing one motion, and reaching back past it would take a hundred
		// presses. The `Before` frames are captured on grab and the command is
		// recorded on release.
		//
		// @since v0.7
		struct GizmoDrag {
			// Which axis is held: 0 is X, 1 is Y, 2 is Z. Negative is none.
			int Axis = -1;

			// Which panel the drag started in, so turning to another viewport
			// mid-drag does not retarget it.
			size_t Viewport = 0;

			// Where along the axis the cursor was when it was grabbed. The
			// difference from this is the translation, which is what stops the
			// selection jumping to the cursor on the first frame.
			float Grabbed = 0.0f;

			// Where the selection was before any of this, for the undo entry
			// and for applying an absolute rather than accumulated delta.
			//@{
			std::vector<engine::core::CFrame> Before;
			std::vector<Entity> Instances;
			//@}

			// The half-extents before the drag, for a scale. Parallel to
			// `Instances`; an entry is zero for anything with no `Bounds`.
			std::vector<engine::core::Vector3> BeforeSize;

			// The pivot offsets before the drag, for a pivot edit. Parallel to
			// `Instances`; the identity for anything with no `Pivot`.
			//
			// **Captured on grab like the frames beside it**, and for the same
			// reason: a pivot edit applies an absolute delta from where things
			// were, so accumulating onto the live value would drift by one
			// frame's rounding every frame.
			std::vector<engine::core::CFrame> BeforePivot;

			// Whether this drag is editing pivots rather than placements.
			//
			// Captured on grab rather than read live, exactly as `Mode` is:
			// leaving the mode mid-drag must not turn a pivot edit into a move
			// halfway through it.
			bool Pivots = false;

			// Which manipulation this drag is. Captured on grab rather than
			// read live, so changing tool mid-drag cannot turn a move into a
			// rotation halfway through it.
			ToolMode Mode = ToolMode::Move;

			// Which faces this scale drag moves. Captured on grab for the same
			// reason `Mode` is.
			//
			// @since v0.13
			ScaleSide Sides = ScaleSide::Side;

			// Which end of the axis was grabbed: `1` for the positive arm and
			// `-1` for the negative one.
			//
			// **It decides which face grows and nothing else.** A move drag
			// measures along the axis either way, so both arms move the
			// selection identically and the sign only lights the right one; a
			// scale drag in `ScaleSide::Side` has to know which face was taken
			// hold of, because that is the one that is meant to move.
			//
			// @since v0.13
			int Sign = 1;

			// Where the selection was centred when the drag began.
			//
			// **The whole drag is measured from here, and it is not the live
			// centre.** `Grabbed` is a distance along the axis from the centre
			// at grab time; reading the centre live means it has already moved
			// by the delta being applied, so the next frame measures a delta of
			// zero, puts everything back, and the frame after that measures the
			// full delta again. That is a part flickering between where it was
			// and where it is being dragged to — visible, reported, and fixed
			// by capturing this.
			//
			// The handles are still *drawn* at the live centre, because a
			// manipulator that stayed behind while the part moved would be a
			// second thing that looks broken.
			//
			// @since v0.13
			engine::core::Vector3 Centre;

			// Where on the rotation plane the drag began, for a rotate.
			engine::core::Vector3 GrabbedPoint;

			// Whether anything actually moved. A click on a handle that never
			// dragged is not an edit and must not reach the log.
			// Whether the drag actually moved anything, so a click that grabbed
			// a handle and released without moving records no command.
			bool Moved = false;
		};

		// The drag in flight, if any.
		GizmoDrag Dragging;

		// Draws the translate gizmo and runs a drag. Called from the overlay.
		//
		// @param viewport Which panel.
		// @param panel    That panel's mapping.
		// @return `true` when the pointer is over a handle, so the click that
		//         would otherwise pick is swallowed.
		bool DrawGizmo(size_t viewport, const PanelProjection &panel);

		// Where the selection is, as a gizmo needs it.
		//
		// @param world  The scene.
		// @param centre Filled in with the average position.
		// @return `false` when nothing selected has a place in the world.
		bool SelectionCentre(WorldId world, engine::core::Vector3 &centre);

		// How frames are paced, as the Preferences page sets them.
		//
		// **Live rather than start-up-only, unlike `Options::Uncapped`.** The
		// flag is what a launcher passes; these are what somebody changes while
		// looking at the frame graph, which is the only time the question comes
		// up. `Options::Uncapped` clears `FrameCap` and is not read again.
		//
		// **Off by default, and the pair is one decision.** An editor is a
		// program with hands on it, and vertical sync puts the display's refresh
		// between the mouse and the viewport — 16.7 ms on a 60 Hz panel before
		// the compositor takes its turn. The cost of turning it off is a
		// viewport spinning as fast as the GPU allows, which `FrameCap` is there
		// to answer; the two ship together because either one alone is a worse
		// default than what they are now.
		bool VerticalSync = false;

		// A ceiling on frames per second while vertical sync is off.
		//
		// **Zero is no ceiling**, which is what `--uncapped` means and what a
		// benchmark wants. Anything else is a soft cap applied by sleeping out
		// the rest of the frame — worth having because an editor that renders
		// nine hundred frames a second to show a still scene is an editor that
		// spins a laptop's fans for nothing.
		//
		// 120 by default: high enough that the cap is not what anybody feels on
		// a 60, 75 or 120 Hz panel, and low enough that a still scene stops
		// costing a laptop its fans. It is deliberately *not* tied to the
		// display's refresh — being unpaced by the display is the point.
		float FrameCap = 120.0f;

		// The script editor's find bar: whether it is up, and what is in it.
		//
		// **One bar for every tab rather than one per tab.** Somebody hunting a
		// name is hunting it across the scripts they have open, and a find box
		// that emptied itself every time they switched tab would be a find box
		// they retype.
		//@{
		bool ShowFind = false;
		std::string FindText;
		std::string ReplaceText;
		//@}

		// What the output panel is showing, and what it is searching for.
		//
		// **Three flags rather than a minimum level**, because "just the
		// warnings" is a question a threshold cannot answer. See `DrawOutput`.
		//@{
		bool ShowInfo = true;
		bool ShowWarnings = true;
		bool ShowErrors = true;
		std::string OutputFilter;
		//@}

		// How far a pick ray reaches, in metres.
		//
		// Bounded rather than infinite because `spatial::Raycast` takes a
		// distance and an unbounded one would walk every cell in the grid to
		// find nothing.
		static constexpr float PICK_REACH = 5000.0f;

		// Whether the ground grid and the origin axes are drawn.
		//
		// **On by default, and it is not decoration.** An empty world with no
		// grid is a black rectangle: no scale, no horizon, and no way to tell
		// where the origin is or which way is up.
		bool ShowGrid = true;

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

		//@{
		bool AskingSaveAs = false;
		bool AskingOpen = false;
		//@}

		// Whether the Rojo project picker is up. See `SyncRojo`.
		bool AskingRojo = false;

		// Whether the Rojo universe picker is up. See `SyncRojoWorlds`.
		bool AskingRojoUniverse = false;
		// Which of the file modals is up. At most one at a time.
		//@{
		bool AskingExport = false;
		bool AskingExportUniverse = false;
		bool AskingImport = false;
		bool AskingImportUniverse = false;
		bool AskingNewWorld = false;
		//@}
		// What the modal's two text fields hold.
		//
		// **Shared across the dialogs rather than one pair each**, because only
		// one is ever open — and a field per dialog is a field somebody forgets
		// to clear, so the New World box opens holding the last export path.
		//@{
		std::string PathBuffer;
		std::string NameBuffer;
		//@}

		// What the explorer's, the properties panel's and the keybind page's
		// filter boxes hold.
		//@{
		std::string ExplorerFilter;
		std::string PropertyFilter;
		std::string KeybindFilter;
		//@}

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

		// An insert the explorer asked for, applied outside `Universe::Enter`.
		struct PendingInsertAction {
			// Where it goes, what it is, and what it hangs off.
			//@{
			WorldId World;
			engine::ecs::ClassId Class;
			Entity Parent;
			//@}
		};

		// A rename the tree asked for, applied outside `Universe::Enter`.
		struct PendingRenameInstanceAction {
			// Which instance, and what to call it.
			//@{
			WorldId World;
			Entity Instance;
			std::string To;
			//@}
		};

		// A move within one world, applied outside `Universe::Enter`.
		struct PendingReparentAction {
			// Which world it happens in.
			WorldId World;

			// Everything to move, which is one row for a menu action and the
			// whole selection for a drag that started on a selected row.
			//
			// **A list rather than a single handle**, because dragging a
			// multi-selection and having one part of it move is worse than
			// either outcome: the author sees the gesture work and finds out
			// later that most of it did not.
			std::vector<Entity> Instances;

			// What to parent them to, or null for a root.
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
			// Where it comes from and where it goes.
			//@{
			WorldId Source;
			WorldId Target;
			Entity Instance;
			//@}

			// What to parent it to in `Target`, or null for a root there.
			Entity Parent;
		};

		// A script the explorer asked to open, applied outside `Universe::Enter`.
		struct PendingScriptAction {
			// Which script, and where it lives.
			//@{
			WorldId World;
			Entity Instance;
			//@}
		};

		// Whether the pointer is over the viewport panel, and whether a drag
		// that started there is still in flight. Two questions rather than one:
		// a look that leaves the rectangle is still that look, and a camera
		// that stopped at the edge would be unusable exactly when somebody is
		// turning quickly.
		bool ViewportHovered = false;

		// Whether a look-drag is in flight.
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
		//@{
		bool ShowViewport = true;
		bool ShowExplorer = true;
		bool ShowWorlds = true;
		bool ShowProperties = true;
		bool ShowScripts = true;
		bool ShowOutput = true;
		//@}

		// Closed by default: it is a panel somebody opens to change one thing.
		bool ShowSettings = false;

		// --- v0.10's panels ----------------------------------------------------
		//
		// All closed by default. Each of these answers a question somebody has
		// occasionally and none of them earns permanent space in the layout —
		// which is what the View menu and the palette are for.

		// The undo stack as a list. See `DrawHistory`.
		bool ShowHistory = false;

		// The local content store. See `DrawAssets`.
		bool ShowAssets = false;

		// Whether each node editor is open. Closed by default: they are for
		// somebody editing a pipeline, and every other session should not pay a
		// panel for it.
		//@{
		bool ShowRenderPipeline = false;
		bool ShowAssetsPipeline = false;

		// The frame as a grid rather than as a canvas: every pass across the
		// top, every resource down the side. See `Editor::DrawPipelineProfile`.
		bool ShowPipelineProfile = false;

		// Which resource the profile panel is showing a picture of.
		//
		// **The grid says who wrote what; this says what they wrote.** Unset is
		// the panel being a grid, which is what it was before and what it should
		// go back to when nobody is looking at anything in particular — a
		// readback costs a download every frame and should not run because
		// somebody left a window open.
		//
		// @since v0.11
		engine::core::Name ProfileWatched;
		//@}

		// TODO(render-pipeline): the Render Pipeline editor's state lived here.
		//
		// Roughly a dozen members: two `nodeview::CanvasState`s (selection and
		// scroll, held **outside** the canvas because they outlive a rebuild), an
		// `EditorGraph` model, the world it was loaded for, a dirty flag, the
		// canvas view, what was selected, what was being dragged and from where,
		// the half-drawn wire, and where the add-node menu was opened.
		//
		// **The one that mattered was the load guard.** The graph was rebuilt
		// from the world's document only when the world changed or the graph was
		// empty — a panel that rebuilt every frame would throw away a wire on the
		// frame it was dragged and have nowhere to put a node somebody moved.

		// Where the add-file and add-folder dialogs are looking.
		//
		// **A `std::string` and no longer a fixed buffer**, because it is no
		// longer typed into: `FilePrompt` and `FolderPrompt` own the text field
		// and hand back what was browsed to.
		std::string AssetBrowsePath;

		// What the person typed into the store list's filter.
		std::string AssetFilter;

		// One entry per asset the worlds use. Rebuilt on demand.
		std::vector<GalleryEntry> Gallery;

		// The signing seed, for a publish.
		//
		// **Never saved, and the buffer is cleared with the editor.** A key kept
		// in the preferences file is a key that signs anything anybody drops in
		// the content folder — `cdn::PublishLocal` carries the argument.
		char AssetSigningKey[128] = {};

		// What the last import or publish said.
		std::string AssetStatus;

		// What the store has published, as of the last refresh.
		//
		// Held rather than re-read, for `RefreshPickerContents`' reason. Shared
		// by every picker and by the Assets panel, because they are looking at
		// one store and two copies would disagree the moment one refreshed.
		std::vector<cdn::PublishedEntry> PickerContents;

		// What is sitting in `raw/`, as of the last refresh.
		std::vector<cdn::RawEntry> PickerRaw;

		// What the person typed into a picker's filter.
		std::string PickerFilter;

		// Whether a `...` button was pressed this frame.
		//
		// **The open is deferred because of imgui's id stack.** A property's
		// widget is inside a `PushID`, so an `OpenPopup` there computes a
		// different id than the `BeginPopupModal` at the window's root — and the
		// modal would silently never appear.
		bool PickerWanted = false;

		// Which kind the open picker is listing.
		engine::assets::AssetKind PickerKind = engine::assets::AssetKind::Unknown;

		// Which property it was opened for, so the frame it is confirmed knows
		// what to write.
		//
		// **A name rather than a descriptor pointer.** A modal spans frames and
		// the class table is rebuilt by registration — rule 3, and the reason
		// every handle in this editor is a value.
		engine::core::Name PickerProperty;

		// What that property's type is, carried across the frames the modal is
		// open.
		//
		// **Because `game::WriteProperty` refuses a value whose `Type` does not
		// match the descriptor's, and a default-constructed `PropertyValue` is
		// `Opaque`.** The confirm path used to build one from nothing and set
		// only `Name` on it, so every choice made in this picker — mesh,
		// texture, material, sound, image — was refused by the one line at the
		// top of `WriteProperty` and nothing anywhere said so. The property
		// stayed blank, the part kept the fallback cube, and typing the same
		// string into the field beside it worked, because that path starts from
		// the value it read and keeps its type.
		//
		// Carried rather than assumed to be `Name`: `ContentKindOfProperty` only
		// happens to match `Name` properties today, and an assumption that holds
		// by coincidence is how this broke in the first place.
		engine::ecs::PropertyType PickerType = engine::ecs::PropertyType::Opaque;

		// The engine's own meshes, offered beside the store's.
		//
		// **Separate from `PickerContents` because it means something else.**
		// That one is the published manifest and its emptiness is a message
		// somebody acts on; these six exist in every process whether or not
		// anything has ever been published, so folding them in would make
		// "nothing published" unsayable.
		std::vector<cdn::PublishedEntry> PickerBuiltins;

		// The name a picker is currently offering.
		std::string PickerChoice;

		// What is moving to and from the origins. See `DrawNetwork`.
		bool ShowNetwork = false;

		// The control surface's own panel. See `DrawControl`.
		bool ShowControl = false;

		// Team create's panel. See `DrawTeamCreate`.
		bool ShowTeamCreate = false;

		// Which panel the per-panel colour rows are editing.
		//
		// **Not saved.** It is where somebody's cursor was in a settings page,
		// not a preference — and a restored one would open the page on a panel
		// they have no memory of choosing. Indexes `SkinnablePanels`.
		//
		// @since v0.13
		int ColourPanel = 0;

		// The command bar. See `DrawCommandBar`.
		//
		// @since v0.13
		bool ShowCommandBar = false;

		// What is typed into it.
		//
		// A character buffer for `InputText`'s sake, like the team-create
		// fields: this repository's imgui has no `imgui_stdlib` on its link
		// line.
		char CommandField[1024] = {};

		// What has been run, oldest first, so the arrows can walk back through
		// it.
		//
		// **Kept even when a command failed.** A command with a typo in it is
		// exactly the one somebody wants back to fix rather than retype.
		std::vector<std::string> CommandHistory;

		// Where the arrows have walked to, or -1 for "at the prompt".
		int CommandCursor = -1;

		// The runtime commands are run in, and the scene it was built against.
		//
		// **Kept between commands, so globals persist.** A bar that threw its
		// VM away after every line would make `local helper = ...` on one line
		// and using it on the next impossible, which is most of what a person
		// wants a prompt for. Rebuilt when the active scene changes, because a
		// runtime is bound to one store.
		//
		// **Its own runtime rather than a plugin's**, so a command cannot spend
		// a plugin's step budget or see its globals — the same reason plugins
		// get one each.
		LoadedPlugin CommandHost;
		engine::world::WorldId CommandWorld;

		// This editor's presence among the others, and the edits between them.
		// Idle — and holding no socket — until somebody opens the panel and
		// asks it to look.
		//
		// Held by pointer because it borrows the command log and the universe,
		// and both are built during `Initialise` rather than at construction.
		std::unique_ptr<TeamCreate> Team;

		// What the team-create fields hold while somebody is editing them.
		// Kept on the editor rather than static inside the draw, for
		// `ControlPortField`'s reason: a panel that is closed and reopened
		// should not forget what was half typed into it.
		//
		// Character buffers rather than strings, because that is the imgui this
		// repository vendors — there is no `imgui_stdlib` on the link line and
		// adding one for three fields would widen what the editor pulls in.
		char TeamNameField[64] = {};
		char TeamKeyField[80] = {};
		char TeamPointField[64] = {};

		// The plugins panel. See `DrawPlugins`.
		bool ShowPlugins = false;

		// What `DiscoverPlugins` found, in folder order.
		std::vector<LoadedPlugin> Plugins;

		// What the port field holds while somebody is editing it.
		//
		// **Separate from `Settings.ControlPort`, which is what the editor
		// starts with.** A half-typed number must not be the port a restart
		// would use, and `Settings.ControlPort` is negative for "do not listen"
		// — a state a text field cannot express while it is being typed in.
		int ControlPortField = 8720;

		// The editor's own delivery client, built from `Content`.
		//
		// **Absent when the settings are not valid**, which is an ordinary
		// state and not an error: an editor with no publisher key configured
		// has a source list and no trust, and nothing should be fetched through
		// it — `MakeAssetClient` is the one that refuses.
		std::unique_ptr<engine::delivery::AssetClient> ContentClient;

		// The other direction. Absent when no source has the write role.
		//
		// **Built even when `ContentClient` is not.** An uploader verifies nothing,
		// so it needs no publisher key — and seeding an origin is exactly the
		// case where no manifest has been signed yet.
		std::unique_ptr<engine::delivery::Uploader> ContentUploads;

		// Seconds since the editor started, for the sample ring's timestamps.
		//
		// Accumulated from frame times rather than read from a clock, which is
		// `NetworkSamples`' rule and is what lets a suite state a window.
		double ContentSeconds = 0.0;

		// The last few seconds of transfer, for the rate rows.
		NetworkSamples ContentSamples;

		// What the last upload, download or rebuild said.
		std::string ContentStatus;

		// Assets asked for and not yet arrived.
		std::vector<PendingDownload> Downloads;

		// What the person typed into the fetch field.
		char DownloadName[256] = {};

		// How many files the last upload queued, for the progress line.
		size_t UploadQueued = 0;

		// How many of them did not arrive.
		size_t UploadFailures = 0;

		// What crosses between worlds. See `DrawBus`.
		bool ShowBus = false;

		// Find instances by class and property. See `DrawFindInstances`.
		bool ShowFindInstances = false;

		// What the Find panel was asked for. Every field is optional and an
		// empty one is "do not filter on this" rather than "match nothing".
		FindQuery Find;

		// What matched, rebuilt every frame the panel is open.
		std::vector<FindResult> FindResults;

		// Whether the walk stopped early. Reported, never silent.
		bool FindTruncated = false;

		// The most results the panel will collect in one pass.
		//
		// A predicate matching everything in a large place would otherwise build
		// a list nobody can scroll and spend a frame doing it. The number is a
		// bound on the work, not a claim about how many exist — which is why the
		// panel says it stopped.
		static constexpr size_t FIND_LIMIT = 500;

		// Which script is spending the tick. See `DrawScriptProfile`.
		bool ShowScriptProfile = false;

		// What has changed since the file was written. See `DrawDiff`.
		bool ShowDiff = false;

		// The last comparison. Rebuilt on demand rather than every frame: it
		// re-serialises the whole game, which is not a per-frame cost.
		std::vector<DiffLine> DiffRows;

		// Why the comparison could not be made, or empty.
		std::string DiffError;

		// Whether the alignment was abandoned for size. Reported, never silent.
		bool DiffCoarse = false;

		// Breakpoints and the captured stacks. See `DrawDebugger`.
		bool ShowDebugger = false;

		// What the debugger's add-breakpoint row holds.
		//@{
		std::string BreakSource;
		int BreakLine = 1;
		//@}

		// Whether a new breakpoint ends the script or lets it carry on.
		bool BreakStops = false;

		// The breakpoints, held here rather than on a runtime.
		//
		// **A Stop destroys every runtime, and breakpoints must not go with
		// them.** They belong to the person debugging, not to the VM that
		// happens to be alive — and re-typing every line number after each Play
		// is how a debugger stops being used. `BeginRun` hands this list to each
		// new runtime; the hits stay per-run, because a hit describes one
		// execution and showing an old one against a new run would be a lie
		// about when it happened.
		//
		// Not written to disk. A breakpoint is a today problem, and a file of
		// stale ones pointing at lines that have moved is worse than none.
		engine::script::Debugger Breakpoints;

		// How many clients a Play run admits.
		//
		// **One is the default because one is what a scene author wants**, and
		// every extra client is a whole world: its own store, its own replica
		// and its own turn in the render round robin. Two is what somebody
		// reaches for when a bug only happens with company, which is most
		// replication bugs.
		int PlayClients = 1;

		// The most clients Play will admit at once.
		//
		// A bound rather than a slider to infinity: each client costs a world,
		// and the failure mode of asking for forty is an editor that stops
		// responding rather than an error.
		static constexpr int MAXIMUM_PLAY_CLIENTS = 4;

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
		//@{
		bool ShowStatistics = false;
		bool ShowFrameGraph = false;
		//@}

		// Frame times, sampled every frame so the panel has history the moment
		// it is opened rather than starting empty.
		engine::render::FrameStatistics Statistics;

		// Whether the next frame rebuilds the default arrangement. Set from the
		// View menu and acted on at the top of the frame, because rearranging a
		// dockspace's nodes from inside a menu is rearranging the tree that is
		// being walked.
		bool ResetLayout = false;

		// What the panels asked for this frame, applied by
		// `ApplyPendingActions` once every `Universe::Enter` has been left.
		//
		// **A slot each rather than a queue**, because two of the same action in
		// one frame is a double click rather than two intentions — and the last
		// one is the one somebody meant.
		//@{
		PendingInsertAction PendingInsert;
		PendingReparentAction PendingReparent;
		PendingRenameInstanceAction PendingRenameInstance;
		PendingMoveAction PendingMove;
		PendingScriptAction PendingOpenScript;
		WorldId PendingRemoveWorld;
		//@}
		// Where the keybind table is read from and written back to.
		//
		// Beside the binary with the layout ini, so a launcher's working
		// directory cannot move somebody's keys. See `Keybinds::Load`.
		std::filesystem::path KeybindPath;

		// A world the Worlds panel asked to make active, applied after it draws.
		WorldId PendingActivate;

		// A run change asked for from the Worlds panel, applied after it draws.
		//
		// **Queued because `SetRunMode` rebuilds the world.** Starting or stopping
		// a scene from inside the popup drawn for its own row would destroy the
		// world while the loop listing it still held its handle.
		//@{
		WorldId PendingRunWorld;
		RunMode PendingRunMode = RunMode::Edit;
		WorldId PendingDuplicateWorld;
		WorldId PendingTeleport;
		//@}

		// A world whose state a menu asked to change, and what to. Both, so
		// that Open and Close are one queued action rather than two flags that
		// can disagree.
		//@{
		WorldId PendingWorldState;
		engine::world::WorldState PendingWorldStateTo = engine::world::WorldState::Active;
		//@}

		// A world a menu asked to rename, and what to.
		//@{
		WorldId PendingRenameWorld;
		std::string PendingRenameTo;
		//@}
		// A camera to look through, and whether the menu asked at all — the two
		// are separate because "look through nothing" is a real request.
		//@{
		Entity PendingLookThrough;
		bool PendingLookThroughSet = false;
		//@}

		// Whether the selection is to be duplicated or destroyed after the
		// panels finish drawing.
		//
		// Flags rather than actions because the subject is already known — it is
		// whatever is selected, and reading it here would be reading it a frame
		// early.
		//@{
		bool PendingDuplicate = false;
		bool PendingDelete = false;
		//@}

		// The scene the rename prompt is for, and whether it is open. Held
		// apart from `PendingRenameWorld` because the prompt spans frames and
		// the action is applied on exactly one.
		//@{
		WorldId RenamingWorld;
		bool AskingRenameWorld = false;
		//@}

		// The explorer's recursion buffer, shared by every level of one walk.
		//
		// One cached instance count per scene. See `InstanceCountOf`.
		struct InstanceCount {
			// The scene it counted.
			WorldId World;

			// How many instances it held.
			size_t Count = 0;

			// When the count was taken, on the frame clock.
			double Taken = 0.0;
		};

		// One row per world, refreshed on a timer rather than every frame — a
		// count is a walk, and nobody reads it sixty times a second.
		std::vector<InstanceCount> InstanceCounts;

		// --- the world lifecycle ----------------------------------------------

		// When a world last had a reason to be running.
		struct WorldLife {
			// Which world this is about.
			WorldId World;

			// Frame-clock time of the last activity: an arrival, an occupant,
			// or somebody looking at it.
			double LastActivity = 0.0;
		};

		// One record per world the lifecycle is watching.
		std::vector<WorldLife> Lives;

		// Whether worlds close themselves when nobody is in them.
		//
		// Off would mean a universe of subareas ticking all of them forever,
		// which is the thing `WorldState::Suspended` exists to prevent — but it
		// is a policy, and an author debugging a world that keeps closing under
		// them needs a way to stop it.
		// Whether the idle policy may suspend and resume worlds at all.
		//
		// Off is what an author wants while debugging one scene: a world that
		// closed under them mid-investigation would look like the bug.
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

		// Whether the last `Simulate` reached `Universe::Tick`.
		//
		// **The universe cannot be asked this and no world's state carries
		// it.** `SyncWorldStates` leaves every world `Active` when nothing is
		// running — deliberately, so an author back in Edit does not find their
		// scenes marked stopped — while `Simulate` returns before the tick.
		// `Present` needs to tell those apart to pick an interpolation alpha,
		// and `studio::PresentationAlpha` is where that is decided.
		bool Advancing = false;

		// Whether the editor's own loop is still going, and what the last frame
		// through it did.
		//@{
		bool Running = false;
		int64_t FramesDrawn = 0;
		engine::render::FrameResult LastFrame;
		//@}
	};
}
