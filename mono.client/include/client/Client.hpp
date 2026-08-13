#pragma once

// Client-owned window, renderer, event loop and frame state.

#include <engine/audio/Device.hpp>
#include <engine/core/Clock.hpp>
#include <engine/delivery/Client.hpp>
#include <engine/delivery/IntakeBudget.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Compile.hpp>
#include <engine/gui/Input.hpp>
#include <engine/input/Actions.hpp>
#include <engine/input/Translate.hpp>
#include <engine/net/Transport.hpp>
#include <engine/render/DebugPanels.hpp>
#include <engine/render/FrameStatistics.hpp>
#include <engine/render/InterfacePass.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/ShaderLibrary.hpp>
#include <engine/render/SpatialCanvas.hpp>
#include <engine/replication/Connector.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Input.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/world/Universe.hpp>

#include <chrono>
#include <client/Compositor.hpp>
#include <client/Scene.hpp>
#include <client/Sounds.hpp>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <network/Presence.hpp>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct SDL_Window;

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
		// A compositor's decision, not a simulation's — which is why it is a
		// client option and not a world setting.
		float ViewSpacing = 40.0f;

		// Simulation ticks per second, independent of the frame rate. The
		// frame runs as fast as it can; the simulation advances in fixed steps
		// and the render interpolates between them.
		double TickRate = 60.0;

		// -1 runs until the window is closed. A frame budget is what makes the
		// client usable from a test or a CI job.
		int64_t MaximumFrames = -1;

		// Levels of surface-seen-in-surface the renderer resolves, or 0 for its
		// own default of two.
		//
		// **`Renderer::SetSurfaceBounces` had no caller anywhere until v0.15**,
		// so every scene resolved exactly two levels whatever it was built from
		// — and two is a mirror seen in a mirror and nothing past it. A pane
		// deeper than that draws as its own tint, because a slot that never
		// rendered is not `Ready` and the shader falls back rather than showing
		// a wrong picture. Each level costs a full scene pass per visible
		// surface, which is why it is asked for rather than assumed.
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

		// The frame rate to hold, or zero for none.
		//
		// **The other half of `Uncapped`, not a contradiction of it.** Turning
		// off the vblank wait is what lets a frame finish early; without a
		// limiter the loop then spins as fast as the GPU allows, which on a
		// scene that costs 2 ms a frame is 500 fps of heat for a display that
		// shows 165. This is how a run says "do not wait for the display, and
		// do not run away from it either" — which is what a variable-refresh
		// monitor wants, and what a like-for-like measurement between two
		// machines needs.
		//
		// Ignored when the vblank wait is on: the display is already the
		// limiter, and a second one fighting it produces judder rather than a
		// lower number.
		uint32_t MaximumFrameRate = 0;

		// Seconds to wait for a Tracy profiler to attach before starting.
		// Zero means do not wait. Tracy is on-demand, so a short run with
		// nothing attached records nothing at all — which looks identical to a
		// broken profiler until you know.
		double ProfilerWaitSeconds = 0.0;

		// Run for this long, then exit. Zero means run until closed.
		double ProfileSeconds = 0.0;

		// Read staged data from here instead of from beside the binary.
		std::filesystem::path AssetsDirectory;

		// The scene script to build the world from. Empty means `Rings.luau`.
		//
		// The extension picks the VM, so this is the only place either one is
		// chosen — and since v0.6 it is the *only* way this program gets a
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
		// once, before the loop starts, in the same place as binding a socket —
		// which is the one moment where waiting is not a stall in a tick. A
		// beacon announces once a second, so anything under two is a coin flip.
		//
		// @since v0.13
		double BrowseSeconds = 3.0;

		// Join the session with this name, rather than the first one found.
		//
		// @since v0.13
		std::string SessionName;

		// Join the session with this id — 32 hex characters.
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

		// Content origins, in priority order — the first one that answers wins.
		//
		// Sources are tried in order; `dir:` selects a local store.
		std::vector<std::string> ContentSources;

		// Where verified content is kept between runs. Empty disables it.
		std::filesystem::path ContentCache;

		// The publisher key this client trusts, as 64 hex characters.
		//
		// An unset key disables delivery; manifests must be authenticated.
		std::string ContentPublisherKey;

		// A `.wav` to play on a loop once the world is up.
		//
		std::filesystem::path SoundPath;

		// The server's Ed25519 public key, as 64 hex characters, or empty to
		// accept any server.
		//
		// Optional server identity pin; encryption alone does not authenticate it.
		std::string ServerKey;

		// Where to write a BMP of the scene, or empty for none.
		//
		// **A diagnostic rather than a feature**, and it changes how the frame
		// is rendered: a capture needs an offscreen target, so asking for one
		// puts the scene into a texture and presents the window from it. A game
		// pays nothing for it because the flag is off.
		std::filesystem::path Capture;
	};

	// The window, the renderer and the frame loop over one world.
	//
	// Initialise, then Run until it returns, then Shutdown. Non-copyable because
	// it owns an SDL window and a graphics device, and there is no sensible
	// meaning for a second object holding the same two.
	class Client {
	  public:
		Client() = default;
		~Client();

		Client(const Client &) = delete;
		Client &operator=(const Client &) = delete;

		// Applies `options`, opens the window, starts the renderer and builds
		// the demo world.
		//
		// @param options Parsed command line. Copied, not referenced.
		// @return False if SDL, the window or the renderer would not start. The
		//         reason is logged before returning, because the caller has no
		//         way to say anything more useful about it than that.
		bool Initialise(const Options &options);

		// Tears the window and renderer down, in that order for a reason the
		// implementation explains, and stops the job system. Safe to call
		// whether or not Initialise got as far as opening anything.
		void Shutdown();

		// Runs until the window is closed, the frame budget is spent, or the
		// profile duration elapses. Returns the process exit code.
		int Run();

		// The connection to a server, when `--connect` was given.
		//
		// For a test, which is the only way to prove a join happened rather than
		// a socket being opened.
		//
		// @return The connector, or null when running the demo alone.
		engine::replication::Connector *Server() {
			return Connection.get();
		}

	  private:
		// Builds the demo worlds `--script`/`--worlds` describe.
		//
		// @return `false` when a world could not be created or its scene script
		//         failed. A client that presented a black screen instead would
		//         leave somebody wondering why.
		bool BuildDemoWorlds();

		// Loads `--game` and starts its scripts in both roles.
		//
		// @return `false` when the file would not load or holds no worlds.
		bool LoadGameFile();

		// Everything after the worlds exist: the profiler wait, and the flag
		// that says the frame loop may begin.
		//
		// **Split out when `--game` arrived**, because two ways of building the
		// worlds sharing one tail is one tail; two copies of it is where the
		// profiler wait ends up on one path and not the other.
		//
		// @return `true` when the client is ready to run.
		bool FinishStartup();

		void PumpEvents();
		void Step();
		void WriteSnapshot();

		// Opens the socket, creates the replica world and joins.
		//
		// @return `false` when the address will not parse or the socket will not
		//         open. A client that meant to connect and silently did not is
		//         a client that looks like a server bug.
		bool BeginConnecting();

		// Finds a session to connect to, and fills in `ConnectAddress`.
		//
		// **The one blocking wait in this program**, and it is before the loop
		// rather than inside it — the same place as binding a socket. A beacon
		// announces once a second, so a search has to be able to sit still for
		// longer than that or it is a coin flip.
		//
		// **Punches on `Socket`, which is why the socket is opened first.** A
		// router's NAT mapping belongs to a port: a hole punched on some other
		// socket punches a hole to a socket the server will never send to. The
		// connector then drains that socket and routes rendezvous traffic back
		// through the presence — `replication::Connector::SetForeign`.
		//
		// @return `false` when nothing was found, or when what was given does
		//         not parse. A client that meant to find a session and silently
		//         ran single-player is one that looks like a broken server.
		bool FindSession();

		// Builds the delivery client and fetches the catalogue.
		//
		// **Reports what is reachable rather than loading anything**, because
		// there is nothing yet to load it into: mesh and texture importing are
		// `ROADMAP.md` v0.9's and land beside this. What this does earn today is
		// real — it proves the configured origins answer, that their manifest
		// verifies against the publisher key, and what is available by kind. A
		// content misconfiguration then shows up at start-up with a reason,
		// instead of as missing geometry much later.
		//
		// @return `false` only when sources were configured and are unusable.
		//         No configuration at all is not a failure.
		bool BeginContentDelivery();

		// Advances content delivery and registers whatever arrived.
		//
		// Called once a frame, before the simulation and outside every render
		// pass — the two constraints that decide where this can go at all.
		void PumpContent();

		// Asks for everything the simulated worlds name and has not asked for.
		//
		// **Demand rather than by kind**, which is what makes a large store
		// usable at all — `client/ContentDemand.hpp` carries the two failures
		// that forced it.
		// Hands every world the mesh names the store published.
		//
		// **Names, not content**, and the only way a scene can discover what
		// there is to name — see `scene/PublishedCatalogue.hpp`. Naming one is
		// still what fetches it.
		void OfferPublishedContent();

		void RequestWantedContent();

		// Asks for one asset, once.
		//
		// @param texture The name. An invalid one, or one already asked for, is
		//        ignored.
		void RequestAsset(const engine::core::Name &texture);

		// Brings the mixer into line with every simulated world's `Sound` rows.
		//
		// After the tick and before presentation, so what a script set this
		// frame is heard this frame. Runs whether or not a device opened: a
		// null device drains the queue like a real one, which is what keeps the
		// path exercised on a machine with no output rather than only on one
		// with speakers.
		void PumpSounds();

		// Opens the audio device and builds the mixer's routing.
		//
		// @return `false` only when a sound was asked for and cannot be played.
		//         **No device is not a failure**: a CI container and a machine
		//         with its output disabled both run quietly.
		bool BeginAudio();

		// Takes one tick's worth of what the server sent.
		//
		// @param nowSeconds The current time.
		void PollServer(double nowSeconds);

		// Copies this frame's keyboard and pointer onto a world's `InputState`.
		//
		// **One function because two worlds want it**, and they are not the same
		// kind of world: every simulated world takes it so that a script polling
		// `UserInputService` gets the same answer wherever it runs, and the
		// replicated world takes it because since v0.14 it holds this client's
		// camera and character.
		//
		// @param store The world to write into. Does nothing when it has no
		//        `InputState` resource.
		void WriteInput(engine::ecs::Store &store);

		// Sends what the player is asking their character to do.
		//
		// **The intent and never the result**, which is the division
		// `Server::ApplyInputs` already states for shooting: a client says which
		// way it is walking and the host decides where that puts it. The
		// alternative — simulating the character here and submitting its
		// transform — puts a physics step on both ends and hands a client the
		// ability to state where its own body is.
		//
		// @param nowSeconds The current time.
		void SubmitMove(double nowSeconds);

		// Gathers what the F4 panel shows, and moves the rate window on.
		//
		// **Not `const`, and not idempotent.** A byte *rate* is a derivative and
		// `net` only keeps the integral, so this reads the cumulative counters,
		// subtracts what it read last time, and remembers the new reading —
		// calling it twice in one frame would report the second call's window as
		// zero seconds long. Called once per panel redraw and nowhere else.
		//
		// @return The panel's view of the link, all zero and `Connected` false
		//         when this client was never given `--connect`.
		engine::render::NetworkStatistics SampleNetwork();

		// Logs what the replicated world received, drew and interpolated.
		//
		// Nothing when this client never connected. See the body for why it is
		// four numbers rather than one — "joined and drew nothing" and "never
		// joined" look identical from outside, and telling them apart is the
		// whole reason this exists.
		void ReportReplica();

		Options Settings;

		SDL_Window *Window = nullptr;
		engine::render::Renderer Renderer;

		// **What a `Material.Shader` name resolves to**, and the one thing on
		// this class that holds a GLSL compiler. One per client rather than one
		// per world, for the reason `ShaderLibrary` gives: it caches over
		// process-wide names, and `Refresh` takes whichever world is being
		// drawn.
		engine::render::ShaderLibrary Shaders;

		engine::render::OverlayImage Overlay;

		// **What draws a `ScreenGui` in a shipped client.** `mono.client` does
		// not link `Engine::ui` and must not — that is what keeps Dear ImGui out
		// of a game binary — so the interface hook here is the engine's own
		// renderer over `gui::DrawList` rather than the editor's.
		//
		// One per client rather than one per world, because a client draws one
		// interface however many worlds it composites: a `ScreenGui` belongs to
		// the viewer, and `--worlds N` places four *views* rather than four
		// overlays.
		engine::render::InterfacePass Interface;

		// Whether the interface pass has been given its image resolver. Set
		// once; see the call site for why it is not done at start-up.
		bool InterfaceImagesReady = false;

		// How long this session has been drawing, in seconds.
		//
		// **What animation is played against**, and it is accumulated from the
		// frame delta rather than read from a wall clock: a run paused or ended
		// holds its animations where they were, and two runs of one recording
		// show the same frames.
		double AnimationSeconds = 0.0;

		// The compiled list the pass draws, kept across frames so its signature
		// can be compared. Holding one per frame would compute a signature, find
		// nothing to compare it against and rebuild every time — every cost of
		// the design and none of its benefit.
		engine::gui::Compiled InterfaceList;

		// Where the pointer is, for the world's own interface. Long-lived: it
		// holds the hover and the press across frames.
		engine::gui::Router InterfaceRouter;
		engine::render::FrameStatistics Statistics;

		engine::input::Actions Actions;

		// This frame's raw input, before any world has been told about it.
		//
		// **Beside `Actions` rather than inside it**, because the two answer
		// different questions about one keyboard: `Actions` is "did the player ask
		// for the frame graph", and this is "is W held". `input/Translate.hpp`
		// carries the split.
		engine::input::Translator Input;

		// What a script last asked the pointer to do, and what the window was last
		// told.
		//
		// **Two fields rather than one, so the window call is made on the frame it
		// changes and no other.** `SDL_SetWindowRelativeMouseMode` is a
		// window-manager round trip on some platforms, and a client that made it
		// every frame would pay for it every frame to say what it already said.
		engine::scene::MouseBehavior PointerMode = engine::scene::MouseBehavior::Default;
		engine::scene::MouseBehavior AppliedPointerMode = engine::scene::MouseBehavior::Default;
		engine::core::FrameClock Clock;

		// The world, and the only place simulation state lives. Everything
		// below this line is the *program* — window, frame budget, panel
		// scroll — which is not world state and does not belong in the store.
		// The universe this client drives, and the world it draws.
		//
		// A client renders one world while the rest keep simulating, which is
		// why presentation is a separate call from the tick. Today there is one
		// world; nothing above this line assumes so.
		//
		// Held by pointer because a universe binds its driver thread on
		// construction, and that thread is decided in Initialise rather than
		// wherever this object was declared.
		std::unique_ptr<engine::world::Universe> Universe_;

		// One VM per world, while a game file is being played. Held here as
		// well as by each world's scheduler, for the reason
		// `game::StartWorldScripts` gives: the scheduler's copy is a capture
		// inside a lambda and nothing else names it.
		std::vector<std::shared_ptr<engine::script::Runtime>> Runtimes;

		// The world the panels report on, and the first view composited.
		engine::world::WorldId Rendered;

		// TODO(render-pipeline): these two are the world-to-renderer seam.
		//
		// Kept rather than deleted so the shape of what was here survives: a
		// world change installed that world's pipelines and chose one, and the
		// render call named it. Nothing writes them now.
		//
		// Which world's saved pipelines are installed in the renderer.
		//
		// **A guard so installing happens on a world change and not per frame.**
		// `client::InstallWorldPipelines` compiles every pipeline it installs
		// and reports what is wrong with each — worth paying when the world
		// changes, and sixty complaints a second about a half-wired one if it
		// were not guarded.
		engine::world::WorldId PipelinesInstalledFor;

		// What to put in `render::View::Pipeline`, from that install.
		//
		// Invalid means the world named no `main` pipeline, which is the
		// ordinary case and draws the renderer's standard frame.
		engine::core::Name PipelineSelected;

		// Every world this client simulates, in creation order.
		std::vector<engine::world::WorldId> Simulated;

		// The socket and the connection to a server. Both null unless
		// `--connect` was given, which is what keeps a single-player run from
		// opening a port it has no use for.
		std::unique_ptr<engine::net::Transport> Socket;
		std::unique_ptr<engine::replication::Connector> Connection;

		// How this client finds a session, when it was not told an address.
		// Null unless `--browse` or `--rendezvous` was given.
		//
		// **Kept for the life of the run rather than dropped after the search.**
		// A rendezvous registration has to keep repeating or the router's
		// mapping expires underneath the session it opened, and the punch that
		// established the address is on the same socket the connector uses.
		std::unique_ptr<network::Presence> Discovery;

		// This tick's time, for the foreign-datagram handler — see
		// `server::Server::DiscoveryNow`, which has the same problem for the
		// same reason.
		double DiscoveryNow = 0.0;

		// The delivery client, when content sources were configured.
		std::unique_ptr<engine::delivery::AssetClient> Content;

		// Requests issued for meshes and textures and not yet taken.
		//
		// **A list rather than a count**, because a request that failed has to
		// be dropped from it and a count could not say which.
		std::vector<engine::delivery::RequestId> ContentPending;

		// How much delivered content this frame will decode and upload.
		//
		// Held across frames rather than made in the loop so the allowance is
		// one object with one meaning; `Begin` is what resets it.
		engine::delivery::IntakeBudget ContentBudget;

		// Whether the catalogue has arrived and the requests have been issued.
		// Once, not per frame — see `PumpContent`.
		bool ContentRequested = false;
		bool ContentReported = false;

		// Requests made while `ContentPending` was being walked, moved into it
		// afterwards. See `Client::RequestTexture`.
		std::vector<engine::delivery::RequestId> ContentIssued;

		// Which texture names have been asked for, by `core::Name::Id`.
		//
		// **Asked once, whatever happened**, so a misspelled name costs one
		// failed request rather than one per pump forever.
		std::unordered_set<uint32_t> ContentAsked;

		size_t ContentMeshes = 0;
		size_t ContentTextures = 0;
		size_t ContentMaterials = 0;
		size_t ContentSounds = 0;

		// The decoded audio this client has, by the name the manifest published
		// it under. **Converted to the device's format once, here** — the graph
		// must never resample on the callback thread, and a buffer converted
		// per voice would do it once per part playing a footstep.
		SoundCatalogue Audible;

		// The voices standing in for each world's `Sound` rows, one stage per
		// world. Node ids are minted per mixer and an entity is only unique
		// inside its own store, so a single stage across two worlds would
		// collide on both counts.
		std::unordered_map<uint32_t, SoundStage> Stages;

		// The busiest frame's counters, for the run's closing line. See where
		// they are folded for why the *last* frame's would be zero.
		uint64_t PeakTriangles = 0;
		uint32_t PeakDrawCalls = 0;

		// When the next frame is due, for the `--max-fps` limiter. A default
		// value means "not started"; the first limited frame sets it.
		std::chrono::steady_clock::time_point NextFrameAt{};

		// The audio device, when one opened. Null runs silently.
		std::unique_ptr<engine::audio::Device> Sound;

		// The world the server owns. Invalid when not connected.
		engine::world::WorldId Replicated;

		// Whether the join has been reported, and therefore whether the
		// replicated world has a view channel yet. A client logs "joined" once,
		// not every frame at six hundred a second.
		bool ReportedJoin = false;

		// Whether the key exchange's outcome has been reported.
		//
		// Separate from the join because the two now fail differently: an
		// exchange that never completes and a world that never finishes
		// arriving both look like "nothing happened" from outside, and they want
		// completely different investigations.
		bool ReportedAdmission = false;

		// Whether F4 has already said there is no network to show. Once, not
		// once per press.
		bool ReportedNoNetwork = false;

		// The previous reading of the link's cumulative counters, and when it
		// was taken. `SampleNetwork` differences against these to turn totals
		// into rates — see its declaration for why the differencing lives here
		// rather than in `net`.
		bool NetworkSampled = false;
		double NetworkSampledAt = 0.0;
		uint64_t NetworkLastReceivedBytes = 0;
		uint64_t NetworkLastSentBytes = 0;
		uint64_t NetworkLastReceivedPackets = 0;
		uint64_t NetworkLastSentPackets = 0;

		// The camera the rendered world placed this frame.
		//
		// Held on the client rather than read twice because the replicated
		// world is drawn through it and has no camera of its own: a camera is
		// an entity, and a replica may not mint one until the predicted-entity
		// index range exists.
		// The surface camera this frame renders an offscreen view from, and
		// whether the world had one. See `FindSurfaceCamera`.
		// **Every surface camera the drawn world holds, rebuilt each frame.**
		// A vector rather than one view and a flag: the pipeline renders a
		// surface per index since v0.8, and rebuilding is also how a mirror that
		// was deleted stops being drawn — a list assembled from what is in the
		// world cannot outlive what is in the world.
		std::vector<engine::render::SurfaceView> Surfaces;

		// **The same-world holes, which are not surfaces and have their own
		// pass.** Rebuilt each frame for `Surfaces`' reason, and gathered
		// *before* it: a slot a portal owns is one `CollectSurfaceViews` must
		// leave alone, or the pane would be drawn twice from two different
		// viewpoints. See `render::PortalView`.
		std::vector<engine::render::PortalView> Portals;

		// Another world's rows, for a pane that shows one.
		//
		// **The standalone client did not assemble these at all until v0.15**,
		// which meant a `Portal::DestinationWorld` pane worked in the studio and
		// showed its own world here — a mirror where a window was authored, with
		// nothing in any log to say so. `client::AttachForeignSurfaces` fills it
		// and points the pane's `SurfaceView` at a range of it.
		std::vector<engine::scene::DrawInstance> Foreign;

		// This world's rows with the far side of anybody standing in one of its
		// cross-world panes appended.
		//
		// **A copy, and only taken when there is a cross-world pane in the
		// world.** `Views` owns the published list and hands out a `const` span
		// — rightly, since it is the buffer the renderer reads — so the return
		// leg has nowhere to be appended without one. That copy is a memcpy of
		// the whole draw list, which is worth paying for a world with a window
		// in it and not worth paying for every other world, so `Windowed` below
		// decides.
		std::vector<engine::scene::DrawInstance> Drawn;

		// Whether the drawn world holds a pane that names another world.
		//
		// Set while the world is open, because that is the only place the
		// question can be asked cheaply, and read after every `Enter` has closed
		// — `Universe::Enter` is not re-entrant and attaching enters the far
		// world.
		bool Windowed = false;

		// This frame's particle batches, one per emitter with something alive.
		//
		// **A member rather than a local, for `Surfaces`' reason**: the vector's
		// capacity survives from frame to frame, so a steady scene stops
		// allocating after the first one. At a hundred thousand emitters that is
		// the difference between one allocation and one a frame.
		//
		// **Only the rendered world fills it.** A batch is a span into that
		// world's pool, and a second world's pool is a different allocation — so
		// mixing two worlds' batches in one list would hand the renderer spans
		// with nothing in common but a type.
		std::vector<engine::render::ParticleBatch> Particles;

		// This frame's beams and trails, as spans into the drawn world's buffer.
		//
		// **Spans and not vectors, unlike `Particles`.** A particle batch has to
		// be assembled — the shared half comes off the emitter and the particles
		// come out of the pool — so there is a list to own. A ribbon buffer is
		// already exactly what the renderer takes, so copying it would be moving
		// the vertices twice to gain nothing.
		//
		// Valid for the frame and only for the frame: `BuildRibbons` clears and
		// refills the buffer in the next `PreRender`, which runs after `Render`.
		std::span<const engine::effects::RibbonVertex> RibbonVertices;
		std::span<const engine::effects::RibbonRun> RibbonRuns;

		// The point and spot lights nearest this frame's eye.
		//
		// A vector rather than a span, unlike the ribbons: the set is *chosen*
		// from the world rather than taken whole, so there is a list to own. Its
		// capacity survives the frame for `Surfaces`' reason.
		std::vector<engine::render::SceneLight> Lights;

		engine::core::CFrame ComposedFrame;
		engine::scene::Camera ComposedCamera;

		// N views in, one frame out. The renderer draws what this composed
		// rather than reaching into a store that somebody else is writing.
		Compositor Views;

		std::vector<engine::render::SystemTiming> SystemTimings;

		bool Running = false;
		int64_t FramesDrawn = 0;

		// A one-second window over ticks, so the panel can show the rate the
		// simulation actually achieved rather than the one it was asked for.
		double TickWindowStarted = 0.0;
		uint64_t TicksAtWindowStart = 0;
		float MeasuredTicksPerSecond = 0.0f;
		int ProfilerScroll = 0;
		// How deep the flamegraph is drawn, on - and =. Starts at everything
		// that was recorded; a deep tree is unreadable and a shallow one hides
		// the answer, so which it is has to be adjustable while looking at it.
		uint32_t ProfilerDepth = engine::core::FrameGraph::MAXIMUM_DEPTH;

		// When the panels were last drawn, and what they were showing at the
		// time.
		//
		// The overlay texture holds the last image drawn into it and is
		// presented from there every frame, so the panels are regenerated on a
		// clock rather than per frame — a person cannot read a number that
		// changes a thousand times a second, and rasterising one costs the same
		// whether they can or not.
		//
		// The rest is what forces an early redraw. A tab change or a scroll that
		// waited for the next tick would read as a dropped key press.
		double PanelsDrawn = 0.0;
		bool PanelsShown = false;
		engine::render::ProfilerTab PanelTab = engine::render::ProfilerTab::Frame;
		int PanelScroll = 0;
		uint32_t PanelDepth = engine::core::FrameGraph::MAXIMUM_DEPTH;
		int PanelWidth = 0;
		int PanelHeight = 0;

		engine::render::FrameResult LastFrame;
	};
}
