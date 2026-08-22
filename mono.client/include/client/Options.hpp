#pragma once

// What one run of the client was asked to do.
//
// **Its own header because a setting is not a client.** `client/Settings.hpp`
// exists to name the defaults for the command line, and it needs exactly this
// struct - but it used to include the whole of `client/Client.hpp` to get it,
// which is a 1,200-line header with a 186-file include closure behind it, for a
// struct of plain values. `docs/ARCH_REVIEW.md` E3.
//
// **`render::ProfilerTab` is the only engine type in it, and it now has its own
// header.** `--profiler-tab` names a tab and a default member initialiser needs
// the complete type, so the enum has to be visible - but reaching it through
// `render/DebugPanels.hpp` dragged `core::FrameGraph`, `core::HeapProfile` and
// `core::Metrics` in behind it, which is 92,484 preprocessed lines for one
// `uint8_t` enum. `render/ProfilerTab.hpp` is what that split left.
//
// @client

#include <engine/render/ProfilerTab.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace client {

	// Command-line configuration copied into Client during Initialise.
	struct Options {
		// Window width in logical pixels, before any display scaling.
		int Width = 1280;

		// Window height in logical pixels. The window is resizable, so this is
		// where it starts and not where it stays.
		int Height = 720;

		// How many cubes the demo scene builds, per world.
		uint32_t Entities = 2048;

		// How many worlds to simulate and composite.
		//
		// Each is a whole world with its own clock, its own store and its own
		// view channel, and the compositor places them side by side because two
		// worlds' coordinates do not mean the same thing. One is the ordinary
		// case; more than one is what makes the compositing path a path rather
		// than a plan.
		uint32_t Worlds = 1;

		// World units between adjacent views.
		//
		// A compositor's decision, not a simulation's - which is why it is a
		// client option and not a world setting.
		float ViewSpacing = 40.0f;

		// Simulation ticks per second, independent of the frame rate. The
		// frame runs as fast as it can; the simulation advances in fixed steps
		// and the render interpolates between them.
		double TickRate = 60.0;

		// -1 runs until the window is closed. A frame budget is what makes the
		// client usable from a test or a CI job.
		int64_t MaximumFrames = -1;

		// Levels of surface-seen-in-surface to resolve, or 0 to let the world
		// and the renderer decide.
		//
		// **An override and not a setting, which is what changed at v0.15.** The
		// depth is the world's - `workspace.SurfaceBounces` - and where a world
		// says nothing the renderer measures the frame it just drew and follows
		// it. Both are better answers than a session-wide constant, because how
		// deep a chain of mirrors goes is a fact about the scene.
		//
		// What this is still for is measuring: pinning the number is how two
		// depths are compared on one scene, and the run that did that is what
		// found the knob had never had a caller at all. Each level multiplies
		// the passes rather than adding one - `panes × (panes - 1) ^ (levels -
		// 1)` - so it is a number worth pinning deliberately and not by default.
		int SurfaceBounces = 0;

		// Open the F3 statistics panel at startup, rather than waiting for
		// somebody to press F3.
		bool ShowStatistics = false;

		// Open the F4 network panel at startup.
		//
		// **Ignored without `--connect`.** There is no link to report on, and
		// the panel refuses to draw zeroes rather than claiming an idle one.
		bool ShowNetwork = false;

		// Open the F5 frame graph at startup.
		//
		// Worth setting deliberately: collection only runs while the panel is
		// open, so a run that meant to record a graph and never opened one has
		// nothing to show for it afterwards.
		bool ShowFrameGraph = false;

		// Which frame-graph view the panel opens on. Naming one opens the panel,
		// because asking to see something is not a separate request from showing
		// it.
		engine::render::ProfilerTab Tab = engine::render::ProfilerTab::Frame;

		// Present without waiting for vblank, to measure the frame rather than
		// the display.
		bool Uncapped = false;

		// How many frames the CPU may queue ahead of the GPU, 1 to 3.
		//
		// **A latency choice rather than a throughput one**, and
		// `render::Renderer::Initialise` carries the argument. It is a flag
		// because the cost is a thing to *feel*: at one,
		// `SDL_SubmitGPUCommandBuffer` blocks until the GPU has finished the
		// previous frame, so the frame rate is the GPU's and the wait shows up
		// as time in the `submit` span; at two the CPU runs ahead and the
		// picture is a frame further behind the mouse.
		//
		// **What one costs is worst on a scene that is not busy at all.** With
		// nothing queued behind it, a frame that misses a vblank by a hair waits
		// out the whole next interval - so a scene doing a quarter of a
		// millisecond of work reads as a steady stream of 34 ms frames with a
		// 0.08 ms median underneath, which is a stutter and not a bottleneck.
		// Measured on `examples/Cube.luau`, which is one spinning cube.
		//
		// The default matches the studio's, so the two programs feel the same
		// until somebody asks otherwise. `mono.studio` has had this flag since
		// v0.14; this is the same one, on the program the demo scenes are
		// actually run with.
		//
		// @since v0.18
		int FramesInFlight = 1;

		// The frame rate to hold, or zero for none.
		//
		// **The other half of `Uncapped`, not a contradiction of it.** Turning
		// off the vblank wait is what lets a frame finish early; without a
		// limiter the loop then spins as fast as the GPU allows, which on a
		// scene that costs 2 ms a frame is 500 fps of heat for a display that
		// shows 165. This is how a run says "do not wait for the display, and
		// do not run away from it either" - which is what a variable-refresh
		// monitor wants, and what a like-for-like measurement between two
		// machines needs.
		//
		// Ignored when the vblank wait is on: the display is already the
		// limiter, and a second one fighting it produces judder rather than a
		// lower number.
		uint32_t MaximumFrameRate = 0;

		// Seconds to wait for a Tracy profiler to attach before starting.
		// Zero means do not wait. Tracy is on-demand, so a short run with
		// nothing attached records nothing at all - which looks identical to a
		// broken profiler until you know.
		double ProfilerWaitSeconds = 0.0;

		// Run for this long, then exit. Zero means run until closed.
		double ProfileSeconds = 0.0;

		// Write a frame-graph snapshot here when the run ends. Empty writes
		// none.
		//
		// **The studio has had this and the client has not, which is backwards.**
		// Every stress scene this repository ships is run through
		// `client --script`, so the program with no way to dump a profile was
		// the one every profile is taken in - and reading a flame graph off a
		// screenshot of an overlay is not reading it. Same flag name as the
		// editor's, because a second spelling for one idea is a second thing to
		// remember.
		//
		// Turning it on turns the frame graph on: recording every span is real
		// work and `--graph` is otherwise the only thing that asks for it.
		std::filesystem::path ProfileSnapshot;

		// Write a heap report here when the run ends. Empty writes none.
		//
		// Turning it on turns heap sampling on, for the reason
		// `ProfileSnapshot` turns frame collection on: a run given the flag and
		// nothing else would retain no readings and then report that it had
		// nothing to fit a slope to.
		//
		// @since v0.18
		std::filesystem::path HeapReport;

		// Fail the run when a tag climbs faster than this, in bytes a second.
		// Zero checks nothing.
		//
		// **What makes a leak a build failure rather than a thing somebody
		// notices.** The run exits `EXIT_RUNAWAY_HEAP` and names every tag that
		// tripped it, so a soak across several scenes is one command with an
		// exit code rather than five reports for a person to read.
		//
		// Turning it on turns heap sampling on.
		//
		// @since v0.18
		double HeapGrowthLimit = 0.0;

		// Seconds at the start of the run left out of the growth fit.
		//
		// **A level loading is a step, and a step at the start of a window drags
		// a straight line through everything after it.** Ten seconds is enough
		// for the demo scenes here to have fetched their content and settled;
		// a scene that takes longer needs a longer warm-up rather than a higher
		// limit.
		//
		// @since v0.18
		double HeapWarmupSeconds = 10.0;

		// Read staged data from here instead of from beside the binary.
		std::filesystem::path AssetsDirectory;

		// The scene script to build the world from. Empty means `Rings.luau`.
		//
		// The extension picks the VM, so this is the only place either one is
		// chosen - and since v0.6 it is the *only* way this program gets a
		// world. The C++ scene it used to fall back to is deleted, because two
		// ways to build one and only one of them exercising the bindings meant
		// the bindings were the untested half.
		std::string ScriptPath;

		// A game file to play, single-player. Empty means the demo scene.
		//
		// A game file is single-player content, not a hosted server, and takes
		// precedence over `--script`.
		std::filesystem::path GameFile;

		// `host:port` of a server to replicate from. Empty means run the local
		// demo alone.
		//
		// Adds a read-only replica world beside the local worlds.
		std::string ConnectAddress;

		// Whether to look for a session on the local subnet instead of being
		// told an address.
		//
		// **The point of the whole discovery layer, seen from a player**: the
		// host runs a server and says nothing, and their friends on the same
		// switch find it. `ConnectAddress` still wins when both are given,
		// because an address somebody typed is a decision and a browse result
		// is a guess.
		//
		// @since v0.13
		bool Browse = false;

		// How long to look before giving up, in seconds.
		//
		// **A blocking wait, and the only one in this program.** It happens
		// once, before the loop starts, in the same place as binding a socket -
		// which is the one moment where waiting is not a stall in a tick. A
		// beacon announces once a second, so anything under two is a coin flip.
		//
		// @since v0.13
		double BrowseSeconds = 3.0;

		// Join the session with this name, rather than the first one found.
		//
		// @since v0.13
		std::string SessionName;

		// Join the session with this id - 32 hex characters.
		//
		// Also what reaches a session through a rendezvous point, because a
		// private one is never listed and possession of its id is what stands
		// in for a listing.
		//
		// @since v0.13
		std::string SessionIdText;

		// The pre-shared secret for a private session: 64 hex characters, or a
		// passphrase.
		//
		// @since v0.13
		std::string SessionSecret;

		// A rendezvous point to reach a session through, as `host:port`.
		//
		// @since v0.13
		std::string RendezvousAddress;

		// Content origins, in priority order - the first one that answers wins.
		//
		// Sources are tried in order; `dir:` selects a local store.
		std::vector<std::string> ContentSources;

		// Where verified content is kept between runs. Empty disables it.
		std::filesystem::path ContentCache;

		// The publisher key this client trusts, as 64 hex characters.
		//
		// An unset key disables delivery; manifests must be authenticated. A
		// server may name one for a client that pinned none - see
		// `Client::AdoptContentDirectory` - and it never replaces one that was
		// pinned here.
		std::string ContentPublisherKey;

		// The hosts a server-named content origin may live on. Empty allows all.
		//
		// **Empty is right for a list a person typed and wrong for one that
		// arrived over a wire**, which is `delivery/AGENTS.md`'s sentence and the
		// reason this setting exists at all: *a client that is told to fetch from
		// an arbitrary host is a request-forgery primitive.* Filling it in is how
		// a deployment says which machines a server is allowed to point its
		// players at.
		//
		// @since v0.16
		std::vector<std::string> ContentAllowedHosts;

		// A `.wav` to play on a loop once the world is up.
		//
		std::filesystem::path SoundPath;

		// The server's Ed25519 public key, as 64 hex characters, or empty to
		// accept any server.
		//
		// Optional server identity pin; encryption alone does not authenticate it.
		std::string ServerKey;

		// Run with no window at all.
		//
		// **What makes a shipped client drivable by something that is not a
		// person**. The studio has had `--headless` since v0.7 and is therefore
		// checkable by `just studio-smoke`, while the *client* - the binary a
		// game actually ships - could only be checked by somebody looking at it
		// until v0.15. That is the worst place for a gap to be, because the bugs
		// it hides are the ones that work in the editor and not in the game.
		// `just client-smoke` is what this makes possible.
		//
		// The renderer already had the mode: `Renderer::Initialise` takes a null
		// window and puts itself into it, which is how the editor works. What
		// this adds is a client that does not create one, and the handful of
		// window calls guarded so that a run without one does not reach them.
		//
		// **Needs `--frames`**, because a headless run has no window to close
		// and would otherwise never stop.
		//
		// @since v0.15
		bool Headless = false;

		// Click the interface element with this name, once, mid-run.
		//
		// **A diagnostic beside `--capture`, and it exists for one reason:
		// nothing could press a button in a shipped client.**
		// `Client::InterfaceRouter` was constructed, read and never `Update`d
		// until v0.15, so a `TextButton` in a game never lit and its `Activated`
		// never fired while the same tree worked in the editor. The fix was one
		// line, and no check anywhere could have said it was missing. On this
		// flag's own first run it found a second: `examples::LoadScene` kept the
		// only reference to the VM it created, so every gui event a `--script`
		// world produced was delivered nowhere.
		//
		// **By name and not by coordinates**, because a coordinate is a second
		// statement of where the layout put something and would go stale the
		// first time a padding changed. `gui::Resolved` is the one answer to
		// where an element is - `gui/AGENTS.md` refuses a second - so this asks
		// it and presses the middle of what it says.
		//
		// The press is synthesised into `input::Translator` as an ordinary SDL
		// event, so it travels the path a real click travels: the same
		// translator, the same `scene::InputState`, the same `gui::Router`, the
		// same `Runtime::DeliverGuiEvents`. A click that took a shortcut past any
		// of those would be a check of the shortcut.
		//
		// Empty presses nothing, which is every run that is not a smoke test.
		//
		// @since v0.15
		std::string ClickElement;

		// Type this into whichever `TextBox` has the keyboard, once, mid-run.
		//
		// **`ClickElement`'s diagnostic for the keyboard, and it needs that flag
		// to have run first**: only a press moves the focus, so
		// `--click Entry --type hello` is the pair, and this does nothing until
		// something is focused. Nothing is typed twice.
		//
		// **Synthesised as an `SDL_EVENT_TEXT_INPUT` into `input::Translator`**,
		// for the reason the press is synthesised as a real button event: the
		// string then travels the path a real keystroke travels - the same
		// translator, the same `Translator::TypedText`, the same `gui::Type` -
		// and a shortcut past any of it would be a check of the shortcut.
		//
		// **What it cannot check is `SDL_StartTextInput`**, which is the other
		// half of the same wiring: this injects the event SDL would have sent, so
		// a client that never asked SDL for text would still pass. The log line
		// the toggle writes is what says the call was made, and a real keyboard
		// is the only thing that proves the platform answered it.
		//
		// @since v0.15
		std::string TypedText;

		// Where to write a BMP of the scene, or empty for none.
		//
		// **A diagnostic rather than a feature**, and it changes how the frame
		// is rendered: a capture needs an offscreen target, so asking for one
		// puts the scene into a texture and presents the window from it. A game
		// pays nothing for it because the flag is off.
		std::filesystem::path Capture;
	};
}
