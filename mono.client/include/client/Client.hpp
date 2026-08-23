#pragma once

// Client-owned window, renderer, event loop and frame state.

#include <engine/assets/ContentPolicy.hpp>
#include <engine/audio/Device.hpp>
#include <engine/control/Server.hpp>
#include <engine/control/Surface.hpp>
#include <engine/core/Clock.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/delivery/Client.hpp>
#include <engine/delivery/IntakeBudget.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/game/Content.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/gui/Compile.hpp>
#include <engine/gui/Input.hpp>
#include <engine/input/Translate.hpp>
#include <engine/net/Transport.hpp>
#include <engine/net/Wire.hpp>
#include <engine/render/DebugPanels.hpp>
#include <engine/render/EditableImages.hpp>
#include <engine/render/EditableMeshes.hpp>
#include <engine/render/FrameStatistics.hpp>
#include <engine/render/InterfacePass.hpp>
#include <engine/render/PresentationSchedule.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/ShaderLibrary.hpp>
#include <engine/render/SpatialCanvas.hpp>
#include <engine/render/ViewportFrames.hpp>
#include <engine/render/WorldPresentation.hpp>
#include <engine/replication/Connector.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Input.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/world/Universe.hpp>

#include <client/Actions.hpp>
#include <client/Compositor.hpp>
#include <client/ContentLink.hpp>
#include <client/Options.hpp>
#include <client/Scene.hpp>
#include <client/Sounds.hpp>
#include <cstdint>
#include <discord/Link.hpp>
#include <filesystem>
#include <memory>
#include <network/Presence.hpp>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct SDL_Window;

namespace client {

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
		// Exit code for a run whose heap kept climbing, and for one that was
		// asked to check and could not.
		//
		// Distinct from 1 and 2, which a client already uses for a failed
		// startup and a bad command line: a soak recipe has to be able to tell
		// "this build leaks" from "this build would not start".
		static constexpr int EXIT_RUNAWAY_HEAP = 3;

		// Below this many seconds of readings a slope is noise. Six, because a
		// least-squares fit over five points is as answerable to one outlier as
		// to the trend.
		static constexpr double MINIMUM_GROWTH_WINDOW_SECONDS = 6.0;

		// Reports whether any tag outran `Options::HeapGrowthLimit`, naming each
		// one. Returns the process exit code.
		int CheckHeapGrowth();

		// Rebuilds the heap tree, history and growth report the F5 panel draws.
		//
		// Called when a reading is taken - once a second - rather than when the
		// panel repaints, which is twenty times a second.
		void RefreshHeapReport();

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
		// rather than inside it - the same place as binding a socket. A beacon
		// announces once a second, so a search has to be able to sit still for
		// longer than that or it is a coin flip.
		//
		// **Punches on `Socket`, which is why the socket is opened first.** A
		// router's NAT mapping belongs to a port: a hole punched on some other
		// socket punches a hole to a socket the server will never send to. The
		// connector then drains that socket and routes rendezvous traffic back
		// through the presence - `replication::Connector::SetForeign`.
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
		// real - it proves the configured origins answer, that their manifest
		// verifies against the publisher key, and what is available by kind. A
		// content misconfiguration then shows up at start-up with a reason,
		// instead of as missing geometry much later.
		//
		// @return `false` only when sources were configured and are unusable.
		//         No configuration at all is not a failure.
		bool BeginContentDelivery();

		// Builds the delivery client out of everything currently known.
		//
		// **One place rather than two**, because the source list is assembled
		// twice - once at start-up from this client's own configuration, and again
		// when a server says where content is - and two assemblers would drift the
		// day one of them learned about a new kind of source.
		//
		// @return `false` when sources were configured and are unusable.
		bool BuildContentClient();

		// Whether a server may still say where content is.
		//
		// **What makes a missing publisher key "not yet" rather than "never".**
		// A client that was given an address and nothing else is exactly who
		// redirect mode is for, and refusing to start would put it out of reach.
		bool ExpectsServerContent() const;

		// Takes what a server said about content.
		//
		// **This client's own configuration wins and is tried first.** RUNNING.md
		// decides precedence by where a value came from: a source in a config file
		// or on a command line was stated by the person running this program, and
		// one that arrived over a wire was stated by a server they connected to.
		// So the server's list is *appended* rather than substituted - the order is
		// the policy, `delivery/AGENTS.md`, and appending is how "mine first, then
		// whatever the host knows about" is expressed without a policy flag.
		//
		// The same rule one field along: a publisher key pinned here is kept, and
		// a server's is adopted only when none was pinned. A pinned client refuses
		// rather than downgrades.
		//
		// **Every endpoint is untrusted.** One on a host `ContentAllowedHosts`
		// does not admit is dropped, and one naming a host *name* rather than an
		// address is dropped with one warning - `net::Endpoint::Parse` refuses a
		// name on purpose, and a stream of failures at fetch time would say the
		// same thing far less usefully.
		//
		// @param directory What the server said.
		// @since v0.16
		void AdoptContentDirectory(const engine::game::ContentDirectory &directory);

		// Advances content delivery and registers whatever arrived.
		//
		// Called once a frame, before the simulation and outside every render
		// pass - the two constraints that decide where this can go at all.
		void PumpContent();

		// Hands every world the mesh names the store published.
		//
		// **Names, not content**, and the only way a scene can discover what
		// there is to name - see `scene/PublishedCatalogue.hpp`. Naming one is
		// still what fetches it.
		void OfferPublishedContent();

		// Asks for everything the simulated worlds name and has not asked for.
		//
		// **Demand rather than by kind**, which is what makes a large store
		// usable at all - `client/ContentDemand.hpp` carries the two failures
		// that forced it.
		//
		// **And only for a world that has moved since it was last asked.** The
		// collection is eight walks of a store and it ran on every world on
		// every frame; `ScanWantedContent` carries the gate and why it is the
		// world's tick counter rather than the ECS change channel.
		void RequestWantedContent();

		// Collects one world's content demand, if its tick has moved since the
		// last time it was collected.
		//
		// @param world The world to scan. A world with no valid id is skipped.
		void ScanWantedContent(engine::world::WorldId world);

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

		// Tells Discord what is being played, and keeps that connection alive.
		//
		// Beside `PumpSounds` and bounded the same way: cheap when nothing is
		// configured, and safe every frame because `discord::Link` sends only
		// what changed and only as often as the protocol allows.
		//
		// @param nowSeconds This process's monotonic clock.
		// @since v0.17
		void PumpDiscord(double nowSeconds);

		// Makes the link, if the flags asked for one.
		//
		// Called once, from `Run`. Nothing is allocated and no socket is opened
		// for a program nobody configured this for, which is every default
		// install.
		//
		// @since v0.17
		void StartDiscord();

		// What the Discord templates can name, filled from this frame.
		//
		// @return The tokens and what they resolve to.
		// @since v0.17
		discord::Facts DiscordFacts() const;

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
		// alternative - simulating the character here and submitting its
		// transform - puts a physics step on both ends and hands a client the
		// ability to state where its own body is.
		//
		// @param nowSeconds The current time.
		void SubmitMove(double nowSeconds);

		// Gathers what the F4 panel shows, and moves the rate window on.
		//
		// **Not `const`, and not idempotent.** A byte *rate* is a derivative and
		// `net` only keeps the integral, so this reads the cumulative counters,
		// subtracts what it read last time, and remembers the new reading -
		// calling it twice in one frame would report the second call's window as
		// zero seconds long. Called once per panel redraw and nowhere else.
		//
		// @return The panel's view of the link, all zero and `Connected` false
		//         when this client was never given `--connect`.
		engine::render::NetworkStatistics SampleNetwork();

		// Logs what the replicated world received, drew and interpolated.
		//
		// Nothing when this client never connected. See the body for why it is
		// four numbers rather than one - "joined and drew nothing" and "never
		// joined" look identical from outside, and telling them apart is the
		// whole reason this exists.
		void ReportReplica();

		Options Settings;

		// The connection to Discord, or null when nothing is configured. See
		// `client/Settings.hpp` for where the `discord` flags come from.
		//
		// @since v0.17
		std::unique_ptr<discord::Link> DiscordLink;

		// When this process started, as a unix epoch second, for the elapsed
		// timer. Read once: `discord::Link` takes monotonic seconds and Discord
		// wants epoch ones, and this is the one place the two meet.
		//
		// @since v0.17
		int64_t DiscordStartedUnixSeconds = 0;

		SDL_Window *Window = nullptr;
		engine::render::Renderer Renderer;

		// **What a `Material.Shader` name resolves to**, and the one thing on
		// this class that holds a GLSL compiler. One per client rather than one
		// per world, for the reason `ShaderLibrary` gives: it caches over
		// process-wide names, and `Refresh` takes whichever world is being
		// drawn.
		engine::render::ShaderLibrary Shaders;

		// **What `Renderer::PostProcessShaderName` was last set to from this
		// world's own `scene::PostProcessShaderOf`.** `ShaderLibrary::
		// Changed` only fires when a *compiled module* moves, so a world
		// switching between two shaders it had already asked for once - both
		// already compiled - needs a second signal, and this is it: compared
		// every frame against what the world currently names, independent of
		// whether anything recompiled.
		engine::core::Name LastPostProcessShader;

		// **What an `EditableMesh` a script built converts into and
		// uploads**, one per client for `Shaders`' own reason: the ledger of
		// last-uploaded revisions is process-wide state, not world state.
		engine::render::EditableMeshUploader EditableMeshes;

		// The identical ledger, for `EditableImage`.
		engine::render::EditableImageUploader EditableImages;

		engine::render::OverlayImage Overlay;

		// **What draws a `ScreenGui` in a shipped client.** `mono.client` does
		// not link `Engine::ui` and must not - that is what keeps Dear ImGui out
		// of a game binary - so the interface hook here is the engine's own
		// renderer over `gui::DrawList` rather than the editor's.
		//
		// One per client rather than one per world, because a client draws one
		// interface however many worlds it composites: a `ScreenGui` belongs to
		// the viewer, and `--worlds N` places four *views* rather than four
		// overlays.
		engine::render::InterfacePass Interface;
		engine::render::ViewportFrames ViewportImages;

		// Whether the interface pass has been given its image resolver. Set
		// once; see the call site for why it is not done at start-up.
		bool InterfaceImagesReady = false;

		// Synthesises the `--click` press, once the interface has been laid out.
		//
		// **After a compile rather than at a frame number**, because a name can
		// only be resolved once something has resolved it: `gui::Resolved` is
		// written by the layout pass, so the earliest frame this can work on is
		// the first one that produced a draw list.
		//
		// @param store The drawn world, already laid out.
		void PressNamedElement(engine::ecs::Store &store);

		// How many frames the synthetic press has left to run.
		//
		// **Three states in one counter**: not started, pressed and waiting to
		// release, done. A press and a release on one frame is a click no
		// `gui::Router` can see - it holds the press across frames on purpose,
		// because that is what a press *is*.
		int ClickFrames = -1;

		// Whether `--type`'s string has been sent. Once per run, like the click.
		bool TypedTextSent = false;

		// How long this session has been drawing, in seconds.
		//
		// **What animation is played against**, and it is accumulated from the
		// frame delta rather than read from a wall clock: a run paused or ended
		// holds its animations where they were, and two runs of one recording
		// show the same frames.
		double AnimationSeconds = 0.0;

		// Device particle time owed since the last rendered image. Simulation may
		// run several updates between presentation deadlines, so handing the
		// renderer only the latest update's delta would slow particles by that
		// ratio.
		float ParticleDeltaSeconds = 0.0f;

		// Time owed to the next PreRender pass. This is separate from the device
		// delta because an unchanged prepared image can consume one without
		// submitting the other.
		float PresentationDeltaSeconds = 0.0f;

		// The compiled list the pass draws, kept across frames so its signature
		// can be compared. Holding one per frame would compute a signature, find
		// nothing to compare it against and rebuild every time - every cost of
		// the design and none of its benefit.
		engine::gui::Compiled InterfaceList;

		// Where the pointer is, for the world's own interface. Long-lived: it
		// holds the hover and the press across frames.
		engine::gui::Router InterfaceRouter;
		engine::render::FrameStatistics Statistics;

		client::Actions Actions;

		// This frame's raw input, before any world has been told about it.
		//
		// **Beside `Actions` rather than inside it**, because the two answer
		// different questions about one keyboard: `Actions` is "did the player ask
		// for the frame graph", and this is "is W held". `input/Translate.hpp`
		// carries the split.
		engine::input::Translator Input;

		// What the window was last told about the pointer.
		//
		// **The record of a system call, and deliberately not a copy of what a
		// script asked for.** `UserInputService.MouseBehavior` and
		// `MouseIconEnabled` are `scene::InputState` fields owned by a *world*,
		// and this client drives several - so a member holding "what a script
		// wants" is one fact per process for a fact there is one of per world,
		// and until v0.19 that is exactly what it was: `WriteInput` wrote it
		// once per world per frame and whichever world was entered last won.
		// Root `AGENTS.md` rule 2. What a script wants is read at the point of
		// use from `InterfaceWorld()`, which is the one world that owns the
		// pointer this frame.
		//
		// **These two stay, because nothing else knows them.**
		// `SDL_SetWindowRelativeMouseMode` and `SDL_ShowCursor` are
		// window-manager round trips on some platforms, so they are made on the
		// frame the answer changes and no other, and the previous answer is not
		// in any store.
		// arch-waiver ecs-copy: the record of a system call, not a copy of a world's
		// state. What a script asked for is read from `InterfaceWorld()` at the
		// point of use; nothing but SDL knows what the window was last told, and
		// SDL has no getter for it.
		engine::scene::MouseBehavior AppliedPointerMode = engine::scene::MouseBehavior::Default;
		bool AppliedPointerIcon = true;

		// Whether the window has been told somebody is typing.
		//
		// **The pointer pair's arrangement with one field instead of two**,
		// because what a text box wants is what the window was last told and
		// there is no second question to ask: `gui::FocusedTextBox` is the
		// authority and this is only the record of the last call made about it.
		// `SDL_StartTextInput` is a round trip for the same reason
		// `SDL_SetWindowRelativeMouseMode` is - it raises an on-screen keyboard
		// on a phone and starts an input method's composition on a desktop - so
		// it is made on the frame the answer changes and no other.
		bool TextInputActive = false;
		engine::core::FrameClock Clock;

		// The world, and the only place simulation state lives. Everything
		// below this line is the *program* - window, frame budget, panel
		// scroll - which is not world state and does not belong in the store.
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

		// The loopback MCP surface. It registers nothing and opens no socket when
		// `Options::ControlPort` is negative.
		engine::control::Server ControlServer;
		engine::control::Surface ControlSurface{
			"atomic-client",
			"A shipped atomic game client. Its universe contains local scenes and may also contain a "
			"replica received from a dedicated server. Replica writes are refused by their store."
		};

		// One VM per world, while a game file is being played. Held here as
		// well as by each world's scheduler, for the reason
		// `game::StartWorldScripts` gives: the scheduler's copy is a capture
		// inside a lambda and nothing else names it.
		//
		// **Keyed by world rather than parallel to `Simulated`**, because the
		// drawn world's VM is what `DeliverGuiEvents` needs and the two vectors
		// only line up on the path that fills both. `BuildDemoWorlds` fills one
		// of them, so an index into the other would have been right until
		// somebody ran the client without `--game`.
		std::vector<std::pair<engine::world::WorldId, std::shared_ptr<engine::script::Runtime>>> Runtimes;

		// The VM for one world, or null when it runs no scripts.
		//
		// @param world Which world.
		// @return The runtime, or null.
		engine::script::Runtime *RuntimeOf(engine::world::WorldId world);

		// Which world holds the input focus: the 2D tree that is laid out,
		// routed and typed into, and the one whose pointer settings the window
		// obeys.
		//
		// **The one the local player is standing in, which is not always the one
		// in front.** A connected client draws its local scene beside the
		// server's, and a person's `PlayerGui` is a subtree of their own
		// `Player` - a row in the replica. Compiling `Rendered` and delivering
		// the press there is what made every button in a replicated world
		// silent: the router picked the right element and the event went to the
		// wrong VM.
		//
		// **There is exactly one answer per frame and that is the point.**
		// `UserInputService.MouseBehavior` and `MouseIconEnabled` are per-world
		// facts with one window between them; every simulated world is told
		// what the pointer *did*, and only this one gets to say what it should
		// do. Anything else is a client whose cursor depends on which world was
		// entered last.
		//
		// @return The replicated world once the join has completed, and the
		//         drawn world otherwise.
		engine::world::WorldId InterfaceWorld() const;

		// The world the panels report on, and the first view composited.
		engine::world::WorldId Rendered;

		// The one rendering profile library loaded with this universe.
		// arch-waiver ecs-copy: the authored library, which is a property of the
		// universe rather than of any world in it. A world holds the *name* it
		// selected, so one graph edit reaches every world using that profile - the
		// copy the store would hold is the one this exists to avoid.
		engine::graph::PipelineSet RenderingProfiles;

		// Which world's selected profile is installed in the renderer.
		//
		// **A guard so installing happens on a world change and not per frame.**
		// `render::InstallWorldPipeline` compiles every profile it installs
		// and reports what is wrong with each - worth paying when the world
		// changes, and sixty complaints a second about a half-wired one if it
		// were not guarded.
		engine::world::WorldId ProfilesInstalledFor;

		// The selection used for that install. A replicated WorldSettings change
		// keeps the same WorldId, so the world id alone cannot invalidate the
		// selected runtime key.
		engine::core::Name ProfileInstalledSelection;

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

		// This tick's time, for the foreign-datagram handler - see
		// `server::Server::DiscoveryNow`, which has the same problem for the
		// same reason.
		double DiscoveryNow = 0.0;

		// What the advert this client is about to dial said the server accepts.
		//
		// **A hint that saves one round trip and decides nothing.** The server
		// chooses the transport; this only lets a client that already heard the
		// answer open on the right stack instead of being refused first. Empty
		// for an address somebody typed, which is the case the fallback exists
		// for.
		std::optional<engine::net::WireMode> Advertised;

		// The delivery client, when content sources were configured.
		std::unique_ptr<engine::delivery::AssetClient> Content;

		// How a relayed route travels, when this client is connected to a server.
		//
		// Built with the connection and outliving every delivery client made over
		// it, because a rebuild replaces the fetcher and must not replace the
		// routes already in flight underneath one.
		std::unique_ptr<ContentLink> ContentRelay;

		// The origins a server named, in the order it named them.
		//
		// Kept apart from `Settings::ContentSources` rather than merged into it,
		// because the two have different provenance and the precedence between
		// them is decided by exactly that - see `AdoptContentDirectory`.
		std::vector<engine::delivery::Source> OfferedContentSources;

		// The publisher a server named, as 64 hex characters, or empty.
		std::string OfferedPublisherKey;

		// The grant a server issued this client, or empty.
		std::vector<std::byte> OfferedContentGrant;

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
		// Once, not per frame - see `PumpContent`.
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

		// The world tick each world's content demand was last collected at, by
		// `world::WorldId::Index`. Absent means never.
		//
		// **A watermark rather than a copy**, which is the distinction root
		// `AGENTS.md` rule 2 is about: nothing here is a second statement of
		// something a world holds, it is a record of how far *this* reader has
		// got. `ContentAsked` above is the same kind of memo.
		//
		// The number it is compared against is `ecs::WorldTime::Tick`, and that
		// is the whole of the gate - see `RequestWantedContent`.
		std::unordered_map<uint32_t, uint64_t> ContentScannedAtTick;

		// Scratch for the demand scan, reused so a pump allocates nothing.
		std::vector<engine::core::Name> ContentWanted;

		size_t ContentMeshes = 0;
		size_t ContentTextures = 0;
		size_t ContentMaterials = 0;
		size_t ContentSounds = 0;

		// The decoded audio this client has, by the name the manifest published
		// it under. **Converted to the device's format once, here** - the graph
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

		// Instance-stream chunks offered and rewritten, summed over the run.
		//
		// **The resident-row result, not the old upload-order estimate.** Draw order
		// and visibility are uint indices now, so only a new or changed entity row
		// contributes to the dirty side of this ratio.
		//
		// Summed rather than peaked, unlike the two above: the question is what a
		// typical frame costs, and a peak would report the one frame a world
		// streamed in on.
		//
		// @since v0.19
		//@{
		uint64_t TotalInstanceChunks = 0;
		uint64_t DirtyInstanceChunks = 0;
		uint64_t TotalInstanceRows = 0;
		uint64_t DirtyInstanceRows = 0;
		//@}

		// Presentation is a consumer of updated state, not the clock that drives
		// it. This deadline is observed after the update and never sleeps it.
		engine::render::PresentationSchedule Presentations;

		// Scene, game UI, and viewport geometry are retained independently. A
		// scene-only update does not rebuild the interface image, and a restored
		// window can present the retained composition without rebuilding either.
		engine::render::PresentationDamageTracker PresentationDamage;
		uint32_t PresentedImages = 0;

		// Content and shader uploads can change pixels without changing a draw row.
		// Set by those upload doors and cleared only after a rendered image.
		bool VisualResourcesChanged = true;

		// The window system can invalidate a presented image without changing any
		// scene input, for example after restore or expose.
		bool PresentationInvalidated = true;

		// The scheduler's observable result. These are session counters rather
		// than per-frame metrics because most update iterations intentionally end
		// without a frame whose metric buffer could carry them.
		uint64_t UpdateIterations = 0;
		uint64_t PresentationOpportunities = 0;
		uint64_t UnchangedPresentationsSkipped = 0;

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
		// into rates - see its declaration for why the differencing lives here
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
		// was deleted stops being drawn - a list assembled from what is in the
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
		// showed its own world here - a mirror where a window was authored, with
		// nothing in any log to say so. `client::AttachForeignSurfaces` fills it
		// and points the pane's `SurfaceView` at a range of it.
		std::vector<engine::scene::DrawInstance> Foreign;

		// This world's rows with the far side of anybody standing in one of its
		// cross-world panes appended.
		//
		// **A copy, and only taken when there is a cross-world pane in the
		// world.** `Views` owns the published list and hands out a `const` span
		// - rightly, since it is the buffer the renderer reads - so the return
		// leg has nowhere to be appended without one. That copy is a memcpy of
		// the whole draw list, which is worth paying for a world with a window
		// in it and not worth paying for every other world, so `Windowed` below
		// decides.
		std::vector<engine::scene::DrawInstance> Drawn;

		// Whether the drawn world holds a pane that names another world.
		//
		// Set while the world is open, because that is the only place the
		// question can be asked cheaply, and read after every `Enter` has closed
		// - `Universe::Enter` is not re-entrant and attaching enters the far
		// world.
		bool Windowed = false;

		// This frame's particle batches, one per emitter with something alive.
		//
		// **A member rather than a local, for `Surfaces`' reason**: the vector's
		// capacity survives from frame to frame, so a steady scene stops
		// allocating after the first one. At a hundred thousand emitters that is
		// the difference between one allocation and one a frame.
		//
		// **Only the rendered world fills it.** A batch points at a block of that
		// world's pool and a birth names a slot of it, and a second world's pool
		// is a different allocation - so mixing two worlds in one frame would
		// hand the renderer indices into a buffer they do not belong to.
		engine::render::ParticleFrame Particles;

		// This frame's beams and trails, as spans into the drawn world's buffer.
		//
		// **Spans and not vectors, unlike `Particles`.** A particle batch has to
		// be assembled - the shared half comes off the emitter and the particles
		// come out of the pool - so there is a list to own. A ribbon buffer is
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
		// arch-waiver ecs-copy: composed from N views for one frame out. No world
		// holds this camera, because it is not any world's camera.
		engine::scene::Camera ComposedCamera;

		// N views in, one frame out. The renderer draws what this composed
		// rather than reaching into a store that somebody else is writing.
		Compositor Views;

		std::vector<engine::render::SystemTiming> SystemTimings;

		// The heap tree and growth report, refreshed when a reading is taken
		// rather than when the panel repaints.
		//
		// **Because fitting a slope is a pass over the whole retained window.**
		// The panel repaints twenty times a second and a reading is taken once,
		// so doing this on the drawing path would run the fit four hundred times
		// for every sample it had to fit - and the heap panel would then be the
		// most expensive thing in the frame it is reporting on.
		std::vector<engine::core::HeapTreeRow> HeapRows;
		std::vector<engine::core::HeapGrowth> HeapGrowth;
		std::vector<engine::core::HeapSample> HeapHistory;
		engine::core::HeapTotals HeapTotals;
		std::vector<uint64_t> GpuHeapHistory;
		engine::render::GpuMemoryStatistics GpuHeapTotals;

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
		// clock rather than per frame - a person cannot read a number that
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
