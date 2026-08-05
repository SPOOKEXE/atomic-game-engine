#include <engine/assets/ChunkStore.hpp>
#include <engine/assets/Grant.hpp>
#include <engine/assets/Signature.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/core/FrameGraph.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Paths.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/game/Game.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/parallel/Process.hpp>
#include <engine/scene/Components.hpp>

#include <algorithm>
#include <atomic>
#include <cdn/Origin.hpp>
#include <cdn/Service.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <server/Server.hpp>
#include <server/Simulation.hpp>
#include <thread>

namespace server {

	namespace {
		// Read between ticks by the loop, written by Stop from anywhere.
		std::atomic<bool> StopRequested{false};

		// The primary world's name.
		//
		// A name rather than an index, because that is what a bus envelope, a
		// snapshot and a supervisor all carry. An index means something
		// different in every process that reads it.
		constexpr const char *PRIMARY = "server.world";

		// The one topic every chattering world uses.
		constexpr const char *CHATTER_TOPIC = "placeholder.chatter";

		// This executable, for spawning hosts of itself.
		//
		// `Paths::Base()` is the directory the binary sits in, which is where
		// it is staged. Deriving it rather than taking it from `argv[0]` means
		// a server started through a shell alias or a symlink still finds
		// itself.
		std::filesystem::path ThisProgram() {
#if defined(_WIN32)
			return engine::core::Paths::Base() / "server.exe";
#else
			return engine::core::Paths::Base() / "server";
#endif
		}

		bool ReadFile(const std::filesystem::path &path, std::vector<std::byte> &bytes) {
			std::ifstream file(path, std::ios::binary | std::ios::ate);
			if (!file) {
				return false;
			}

			const auto size = static_cast<size_t>(file.tellg());
			bytes.resize(size);
			file.seekg(0);
			file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size));
			return static_cast<bool>(file);
		}

		bool WriteFile(const std::filesystem::path &path, std::span<const std::byte> bytes) {
			std::ofstream file(path, std::ios::binary | std::ios::trunc);
			if (!file) {
				return false;
			}
			file.write(
				reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())
			);
			return static_cast<bool>(file);
		}
	}

	Server::Server() = default;

	// Out of line, so `cdn::Origin` and `cdn::Service` may be incomplete in the
	// header. The order matters: the service borrows the origin, so it is
	// destroyed first — which is what member declaration order already gives,
	// stated here because it is the kind of thing a later edit reorders.
	Server::~Server() {
		ContentService.reset();
		ContentOrigin.reset();
		ContentGrantSecret.reset();
	}

	bool Server::BeginServingContent() {
		if (Settings.ContentStore.empty()) {
			return true;
		}

		auto store = engine::assets::ChunkStore::Open(Settings.ContentStore, false);
		if (!store) {
			ENGINE_ERROR("server: no content store at {}", Settings.ContentStore.string());
			ENGINE_ERROR(
				"server: publish one first — cdn --publish DIR --store {} --signing-key HEX",
				Settings.ContentStore.string()
			);
			return false;
		}

		engine::assets::SignatureBytes signature;
		auto manifest = store->ReadManifest(signature);
		if (!manifest) {
			ENGINE_ERROR("server: {} holds no manifest", Settings.ContentStore.string());
			return false;
		}

		// The secret shared with whoever issues grants.
		//
		// **Required rather than invented, and that is not friction for its own
		// sake.** A grant key is HMAC material, and `assets/AGENTS.md` is
		// explicit that `core::Random` is not a cryptographic generator and
		// must never be used for one. The alternatives here are to reach for a
		// generator this engine does not have, or to say plainly that a secret
		// is the deployment's to supply — which is what `Grant.hpp` already
		// says and what `cdn --grant-key` already requires.
		//
		// A key minted from a weak generator would look exactly like a strong
		// one and would be discovered by somebody else, not by us.
		std::array<std::byte, engine::assets::GrantKey::BYTES> secret{};
		if (Settings.ContentGrantKey.size() != secret.size() * 2) {
			ENGINE_ERROR(
				"server: --content-store needs --content-grant-key, {} hex characters", secret.size() * 2
			);
			ENGINE_ERROR(
				"server: the secret is the deployment's to supply — this engine has no "
				"cryptographic generator to invent one with"
			);
			return false;
		}
		for (size_t index = 0; index < secret.size(); ++index) {
			const std::string byte = Settings.ContentGrantKey.substr(index * 2, 2);
			char *end = nullptr;
			const long value = std::strtol(byte.c_str(), &end, 16);
			if (end != byte.c_str() + 2) {
				ENGINE_ERROR("server: --content-grant-key is not hex");
				return false;
			}
			secret[index] = static_cast<std::byte>(value);
		}

		auto key = engine::assets::GrantKey::FromSecret(secret);
		if (!key) {
			return false;
		}

		// One secret, two keys, because there are two roles — see the member's
		// comment. Built from the same bytes rather than shared, since a
		// `GrantKey` zeroes itself and is deliberately not copyable.
		auto issuing = engine::assets::GrantKey::FromSecret(secret);
		if (!issuing) {
			return false;
		}
		ContentGrantSecret = std::make_unique<engine::assets::GrantKey>(std::move(*issuing));
		ContentOrigin = std::make_unique<cdn::Origin>(std::move(*key));

		auto mounted = cdn::ContentRoot::Mount(Settings.ContentStore);
		if (!mounted) {
			return false;
		}
		if (!ContentOrigin->Publish(
				std::make_shared<const cdn::Publication>(*mounted, std::move(*manifest))
			)) {
			ENGINE_ERROR("server: could not publish the content store");
			return false;
		}

		cdn::ServiceSettings service;
		service.Port = Settings.ContentPort;
		ContentService = cdn::Serve(*ContentOrigin, std::move(*store), service);
		if (!ContentService) {
			ENGINE_ERROR("server: could not serve content on port {}", Settings.ContentPort);
			return false;
		}

		ENGINE_INFO(
			"server: serving content from {} on port {}",
			Settings.ContentStore.string(),
			ContentService->Local().Port
		);
		return true;
	}

	std::optional<uint16_t> Server::ContentPort() const {
		if (!ContentService) {
			return std::nullopt;
		}
		return ContentService->Local().Port;
	}

	std::optional<std::vector<std::byte>>
	Server::IssueContentGrant(uint64_t session, uint64_t nowSeconds, uint64_t lifetimeSeconds) const {
		if (!ContentOrigin || !ContentGrantSecret) {
			return std::nullopt;
		}
		const std::shared_ptr<const cdn::Publication> published = ContentOrigin->Current();
		if (!published) {
			return std::nullopt;
		}

		engine::assets::GrantScope scope;
		scope.Session = session;
		for (const engine::assets::BundleEntry &bundle : published->Contents().Bundles()) {
			scope.Bundles.push_back(bundle.Root);
		}
		scope.ExpiresAtSeconds = nowSeconds + lifetimeSeconds;
		// What an operator will accept being billed for rather than a secrecy
		// bound — game content is not secret, it ships to everyone who plays.
		scope.ByteBudget = 1024ull * 1024ull * 1024ull;

		const auto grant = engine::assets::Grant::Issue(scope, *ContentGrantSecret);
		if (!grant) {
			return std::nullopt;
		}
		return grant->Encode();
	}

	bool Server::Initialise(const Options &options) {
		Settings = options;

		if (!Settings.AssetsDirectory.empty()) {
			engine::core::Paths::SetAssetsOverride(Settings.AssetsDirectory);
			ENGINE_INFO("assets from {}", Settings.AssetsDirectory.string());
		}

		if (!Settings.GamePath.empty()) {
			ENGINE_INFO("hosting the scene in {}", Settings.GamePath);
		}

		if (Settings.TickRate <= 0.0) {
			ENGINE_ERROR("--tick-rate must be greater than zero");
			return false;
		}

		// How many processes will share this machine, this one included. A
		// driver works it out from the hosts it is about to spawn and passes
		// the answer down, because it is the only process that knows.
		const unsigned processes =
			Settings.Processes > 0 ? Settings.Processes : 1u + static_cast<unsigned>(PlannedHosts());

		engine::parallel::Jobs::Start(engine::parallel::WorkersPerHost(processes));

		// Before anything builds or loads a world. A snapshot names its
		// components, so a process that has not registered them cannot resolve
		// them — and registration order fixes iteration order, so it belongs at
		// startup rather than wherever a type is first touched.
		RegisterPlaceholderComponents();

		engine::world::UniverseSettings universe;

		// A host owns worlds and no bus backend. The topics, the MemoryStore
		// map and the DataStore records belong to the driver, because two
		// processes each holding a copy would be two answers to the same key.
		universe.Federated = IsHost();

		engine::world::DriverSettings driver;
		driver.Universe = universe;
		driver.Hosts.WorldsPerHost = Settings.WorldsPerHost;

		// A host is not a different executable. Defaulting to this one is what
		// makes the grouping a deployment decision rather than an engine one —
		// there is no `mono.host` to keep in step.
		driver.Hosts.Program = Settings.HostProgram.empty() ? ThisProgram() : Settings.HostProgram;

		// Passed through so a host builds the same scene this process would
		// have. A game file replaces this at v0.5.
		driver.Hosts.Arguments = {
			"--unpaced",
			"--entities",
			std::to_string(Settings.Entities),
			"--tick-rate",
			std::to_string(Settings.TickRate),

			// The worker budget, worked out once by the process that knows how
			// many there will be.
			"--processes",
			std::to_string(1u + PlannedHosts()),
		};

		if (Settings.Chatter) {
			driver.Hosts.Arguments.emplace_back("--chatter");
		}

		Driver_ = std::make_unique<engine::world::Driver>(driver);
		StopRequested.store(false);

		if (IsHost()) {
			return InitialiseHost();
		}

		if (!Settings.ReplayPath.empty()) {
			std::vector<std::byte> bytes;
			if (!ReadFile(Settings.ReplayPath, bytes)) {
				ENGINE_ERROR("could not read the recording at '{}'", Settings.ReplayPath.string());
				return false;
			}

			Replayer_ = std::make_unique<engine::world::Replayer>();
			engine::core::ByteReader reader(bytes);
			if (!Replayer_->Load(reader)) {
				ENGINE_ERROR("'{}' is not a recording this build reads", Settings.ReplayPath.string());
				return false;
			}

			// A snapshot carries state, never code, so the systems are
			// registered again here — exactly as this program does on a normal
			// start, because it *is* the same program.
			const bool restored =
				Replayer_->Restore(Worlds(), [](engine::world::Universe &into, engine::world::WorldId id) {
					into.Enter(id, [](engine::ecs::Store &store, engine::ecs::Scheduler &systems) {
						RegisterPlaceholderSystems(store, systems);
					});
				});

			if (!restored) {
				ENGINE_ERROR("the recording's snapshot could not be restored");
				return false;
			}

			PrimaryWorld = Worlds().Find(engine::core::Name(PRIMARY));

			// Recording a *replay* is what proves the replay path reproduces
			// the run rather than merely surviving it: the two files are
			// compared byte for byte by `just replay-check`. Without this the
			// flag combination was accepted and silently ignored, which is the
			// worst of the three possible behaviours.
			if (!BeginRecording()) {
				return false;
			}

			Running = true;

			ENGINE_INFO(
				"replaying {} barrier(s) from {}", Replayer_->Barriers(), Settings.ReplayPath.string()
			);
			return true;
		}

		// **A game file brings its own worlds, and there may be several.** This
		// is the promise `Options::GamePath` has carried since v0.3 — the flag
		// was named for the file it would eventually point at rather than being
		// renamed twice — and v0.7 is when it comes due.
		if (IsGameFile(Settings.GamePath)) {
			if (!HostGameFile()) {
				return false;
			}
		} else {
			engine::world::WorldSettings world;
			world.Name = engine::core::Name(PRIMARY);
			world.TickRate = Settings.TickRate;

			PrimaryWorld = Worlds().Create(world);
			if (!PrimaryWorld.IsValid()) {
				ENGINE_ERROR("could not create the primary world");
				return false;
			}

			Worlds().Enter(PrimaryWorld, [this](engine::ecs::Store &store, engine::ecs::Scheduler &systems) {
				if (!BuildWorld(store, systems)) {
					return;
				}
				if (Settings.Chatter) {
					store.SetResource(Chatter{engine::core::Name(CHATTER_TOPIC)});
				}
			});
		}

		if (!StartHosts()) {
			return false;
		}

		if (!BeginRecording()) {
			return false;
		}

		if (!BeginListening()) {
			return false;
		}

		if (!BeginServingContent()) {
			return false;
		}

		Running = true;
		ENGINE_INFO("server ready at {:.1f} Hz", Settings.TickRate);
		return true;
	}

	bool Server::IsGameFile(const std::string &path) {
		// **By extension, and that is the whole discriminator.** `--game` has
		// accepted a scene *script* since v0.3 and every recipe and test that
		// uses it still passes one; refusing those to make room for the new
		// format would break the working half to add the missing one. A
		// `.agame` is a universe of worlds and anything else is one scene, and
		// `game::GAME_EXTENSION` is the single place that spelling lives.
		if (path.empty()) {
			return false;
		}
		return std::filesystem::path(path).extension() == engine::game::GAME_EXTENSION;
	}

	bool Server::HostGameFile() {
		engine::game::GameInfo info;
		std::string error;

		if (!engine::game::LoadGame(Worlds(), Settings.GamePath, info, error)) {
			ENGINE_ERROR("--game '{}' failed: {}", Settings.GamePath, error);
			return false;
		}

		const auto worlds = Worlds().Worlds();
		if (worlds.empty()) {
			ENGINE_ERROR("--game '{}' holds no worlds", Settings.GamePath);
			return false;
		}

		// **Every world runs, and the first one is what a client joins.**
		// Replication is one world per connection today — `Session` binds a
		// client to a world — so a game of several scenes simulates all of them
		// and streams one. Said in the log rather than left to be discovered:
		// a player who joined and saw the lobby instead of the arena has no way
		// to tell which world they got.
		PrimaryWorld = worlds.front();

		engine::script::RuntimeLimits limits;
		limits.Role = engine::script::HostRole::OfServer();

		for (const engine::world::WorldId id : worlds) {
			if (Worlds().IsRemote(id)) {
				continue;
			}

			std::string failure;
			Worlds().Enter(
				id,
				[this, &limits, &failure, id](engine::ecs::Store &store, engine::ecs::Scheduler &systems) {
					// The same call the studio's Play makes. What "running a
					// game" means is one function, or the two drift and the
					// first thing to drift is the heartbeat's delta.
					Runtimes.push_back(engine::game::StartWorldScripts(store, systems, limits, failure));

					if (id == PrimaryWorld && Settings.Chatter) {
						store.SetResource(Chatter{engine::core::Name(CHATTER_TOPIC)});
					}
				}
			);

			if (!failure.empty()) {
				// Reported and not fatal, for the reason `RunWorldScripts`
				// runs every script even when one fails: a game where half the
				// scripts silently did not start is a bug report with nothing
				// in it, and refusing to host at all is worse than hosting a
				// game with one broken script in it.
				ENGINE_ERROR("world '{}': {}", Worlds().NameOf(id).Text(), failure);
			}
		}

		ENGINE_INFO(
			"hosting '{}' — {} world(s), serving '{}'",
			info.Name.IsValid() ? info.Name.Text() : "game",
			worlds.size(),
			Worlds().NameOf(PrimaryWorld).Text()
		);
		return true;
	}

	bool Server::BuildWorld(engine::ecs::Store &store, engine::ecs::Scheduler &scheduler) {
		if (Settings.GamePath.empty()) {
			BuildPlaceholderWorld(store, scheduler, Settings.Entities);
			return true;
		}

		// The same loader the client calls, over the same file. A server that
		// authored the scene differently would be disagreeing with its own
		// replicas once per tick, and every side would look self-consistent.
		//
		// What it does *not* install is the client's half — there is no camera
		// and no draw list here, because a server draws nothing. That split is
		// the reason the loader stops where it does.
		std::string error;
		if (!engine::examples::LoadScene(store, scheduler, Settings.GamePath, error)) {
			ENGINE_ERROR("--game '{}' failed:\n{}", Settings.GamePath, error);
			return false;
		}
		return true;
	}

	bool Server::BeginListening() {
		if (!Settings.Listening) {
			return true;
		}

		if (IsHost()) {
			// A driver is the authority for every world in the universe,
			// including the ones a host holds. A host that also streamed would
			// be a second answer to the same world, which is the split brain the
			// federated universe exists to prevent.
			ENGINE_ERROR("--listen is for a driver. A host serves its driver, not clients.");
			return false;
		}

		Socket = engine::net::MakeUdpTransport(Settings.ListenPort);
		if (Socket == nullptr) {
			ENGINE_ERROR("could not bind UDP port {}", Settings.ListenPort);
			return false;
		}

		Replication = std::make_unique<engine::replication::Listener>(*Socket);

		if (!Replication->Admitting()) {
			// The admission challenge is drawn from operating system entropy and
			// there was none. Refusing to start beats listening on a port that
			// turns every client away, which reads from outside exactly like a
			// firewall problem.
			ENGINE_ERROR("the admission challenge could not be seeded, so this server can admit nobody.");
			return false;
		}

		// **Opt in, by name.** A world holds components no client has any
		// business receiving, and a default of "everything" makes leaking one
		// the consequence of forgetting rather than of deciding. These four are
		// the placeholder scene; a game file names its own at v0.5.
		//
		// They are `scene`'s names, which are the same strings a client
		// registers. They used to be `server.Position` and `server.Velocity`,
		// and the client declared a matching pair of its own to receive them.
		Replication->Authority().Replicate(engine::core::Name("scene.Transform"));
		Replication->Authority().Replicate(engine::core::Name("scene.Motion"));

		// **Signed rather than observed, and until v0.7 they crossed once and
		// never again.** The placeholder world resizes and recolours nothing,
		// so observing them would buy a dirty column paid for every tick and
		// read never — which is why they were left unobserved. The cost of that
		// only appeared once a *script* could write one: a part recoloured at
		// runtime kept its old colour on every client for ever, because no
		// delta could carry a component with no dirty bits, and nothing said
		// so. `ChangeDetection::Signature` notices the write itself instead of
		// waiting to be told about it.
		//
		// They are here at all because a client that received a position and no
		// size has nothing to draw.
		using engine::replication::ChangeDetection;
		Replication->Authority().Replicate(engine::core::Name("scene.Bounds"), ChangeDetection::Signature);
		Replication->Authority().Replicate(engine::core::Name("scene.Visual"), ChangeDetection::Signature);

		// The mirror and the tree that says what it is a mirror of. See the
		// same three lines in `studio/PlayLink.cpp` for why the *aim* is not
		// among them: a reflection is of the viewer, and every client has its
		// own. `client::AimReplicaViewer` is the other half.
		Replication->Authority().Replicate(
			engine::core::Name("scene.SurfaceCamera"), ChangeDetection::Signature
		);
		Replication->Authority().Replicate(engine::core::Name("scene.Camera"), ChangeDetection::Signature);
		Replication->Authority().Replicate(engine::core::Name("ecs.Hierarchy"), ChangeDetection::Signature);

		// A delta is the third reader of the dirty bits, so the components that
		// travel *and change every tick* have to be observed or nothing ever
		// looks changed. The two above are signed instead — a hash of a value
		// that moves every tick is a pass over the world to learn what a bit
		// already knew.
		Worlds().Enter(PrimaryWorld, [](engine::ecs::Store &store) {
			store.Observe<engine::scene::Transform>();
			store.Observe<engine::scene::Motion>();
		});

		ENGINE_INFO("replication listening on {}", Socket->Local().Text());
		return true;
	}

	engine::net::Endpoint Server::ListeningOn() const {
		return Socket == nullptr ? engine::net::Endpoint{} : Socket->Local();
	}

	void Server::ServeClients(double nowSeconds) {
		if (Replication == nullptr) {
			return;
		}

		// Inbound first, so an acknowledgement that arrived this tick is counted
		// before this tick's delta is built against it.
		Replication->Poll(nowSeconds);

		Worlds().Enter(PrimaryWorld, [this, nowSeconds](engine::ecs::Store &store) {
			// Before `ClearChanges`, which the world does at the start of its
			// next tick — the bits are the delta source and reading them after
			// they are cleared is how a tick's worth of movement goes missing.
			Replication->Publish(store, store.Time().Tick, nowSeconds);
		});

		// Nothing consumes inputs yet: the placeholder scene has no notion of a
		// player, so an input has nowhere to be applied. Dropped rather than
		// left to accumulate, which would be a slow leak per connected client
		// for a feature that does not exist. A game file at v0.5 is what turns
		// this into an apply.
		Replication->ClearInputs();

		Replication->Advance(nowSeconds);
	}

	size_t Server::PlannedHosts() const {
		if (Settings.RemoteWorlds.empty()) {
			return 0;
		}

		// The same planner the driver uses, run early for its count alone. Two
		// different answers to "how many hosts" is how a worker budget stops
		// matching the processes it was budgeted for.
		std::vector<engine::world::WorldSettings> remote;
		remote.reserve(Settings.RemoteWorlds.size());
		for (const std::string &name : Settings.RemoteWorlds) {
			engine::world::WorldSettings world;
			world.Name = engine::core::Name(name);
			remote.push_back(world);
		}

		return engine::world::PlanHosts(remote, Settings.WorldsPerHost).size();
	}

	bool Server::StartHosts() {
		if (Settings.RemoteWorlds.empty()) {
			return true;
		}

		std::vector<engine::world::WorldSettings> remote;
		remote.reserve(Settings.RemoteWorlds.size());

		for (const std::string &name : Settings.RemoteWorlds) {
			engine::world::WorldSettings world;
			world.Name = engine::core::Name(name);
			world.TickRate = Settings.TickRate;
			remote.push_back(world);
		}

		const size_t started = Driver_->Start(remote);
		if (started == 0) {
			ENGINE_ERROR("no host could be started for {} world(s)", remote.size());
			return false;
		}

		ENGINE_INFO("started {} host(s) for {} world(s)", started, remote.size());
		return true;
	}

	bool Server::InitialiseHost() {
		auto channel = engine::parallel::AdoptInheritedChannel();
		if (channel == nullptr) {
			// A host with no driver is a process that would tick worlds nobody
			// asked for and answer to nobody. Refusing is the honest outcome,
			// and it is also what stops `--host` from looking like a way to run
			// the server with a nicer name.
			ENGINE_ERROR("--host was given but this process was not started with a channel to a driver");
			return false;
		}

		Link = std::make_unique<engine::world::HostLink>(
			std::move(channel), engine::core::Name(Settings.HostName)
		);

		if (Settings.HostWorlds.empty()) {
			ENGINE_ERROR("host '{}' was granted no worlds", Settings.HostName);
			return false;
		}

		for (const std::string &name : Settings.HostWorlds) {
			engine::world::WorldSettings world;
			world.Name = engine::core::Name(name);
			world.TickRate = Settings.TickRate;

			const engine::world::WorldId id = Worlds().Create(world);
			if (!id.IsValid()) {
				ENGINE_ERROR("host '{}' could not create world '{}'", Settings.HostName, name);
				return false;
			}

			Worlds().Enter(id, [this](engine::ecs::Store &store, engine::ecs::Scheduler &systems) {
				if (!BuildWorld(store, systems)) {
					return;
				}
				if (Settings.Chatter) {
					store.SetResource(Chatter{engine::core::Name(CHATTER_TOPIC)});
				}
			});

			// The first world granted is the one `Enter` reaches. A host holds
			// several and a caller outside it addresses them by name, so this
			// is a convenience rather than a distinction the host makes.
			if (!PrimaryWorld.IsValid()) {
				PrimaryWorld = id;
			}
		}

		engine::world::HostFrame ready;
		ready.Signal = engine::world::HostSignal::Ready;
		Link->Send(ready);

		Running = true;
		ENGINE_INFO(
			"host '{}' holding {} world(s) at {:.1f} Hz",
			Settings.HostName,
			Settings.HostWorlds.size(),
			Settings.TickRate
		);
		return true;
	}

	bool Server::ServiceLink() {
		if (Link == nullptr) {
			return true;
		}

		Frames.clear();
		Link->Receive(Frames);

		bool asked = false;
		for (const engine::world::HostFrame &frame : Frames) {
			switch (frame.Signal) {
			case engine::world::HostSignal::Stop:
				// Noted rather than acted on here: the tick that is already
				// underway finishes, and what this host owes goes up with it.
				asked = true;
				break;

			case engine::world::HostSignal::Deliveries:
				for (const engine::world::HostDelivery &delivery : frame.Deliveries) {
					if (!Worlds().Deliver(delivery.World, delivery.Message)) {
						ENGINE_WARN(
							"host '{}': a delivery for '{}', which is not one of mine.",
							Settings.HostName,
							delivery.World.Text()
						);
					}
				}
				break;

			case engine::world::HostSignal::Traffic:
			case engine::world::HostSignal::Ready:
			case engine::world::HostSignal::Heartbeat:
			case engine::world::HostSignal::Faulted:
				// Host to driver only. A driver sending one is a driver with a
				// bug, and acting on it would hide that.
				ENGINE_WARN(
					"host '{}': a driver sent a {} frame, which only a host sends.",
					Settings.HostName,
					engine::world::Describe(frame.Signal)
				);
				break;
			}
		}

		if (!Link->Connected()) {
			// The driver is gone. Continuing would leave worlds ticking with
			// nobody to answer their bus requests and nobody to stop them,
			// which is the orphan a supervisor exists to prevent.
			ENGINE_WARN("host '{}': the driver went away.", Settings.HostName);
			return false;
		}

		return !asked;
	}

	bool Server::BeginRecording() {
		if (Settings.RecordPath.empty()) {
			return true;
		}

		Recorder_ = std::make_unique<engine::world::Recorder>();
		if (!Recorder_->Begin(Worlds())) {
			ENGINE_ERROR("the world cannot be snapshotted, so it cannot be recorded");
			return false;
		}

		ENGINE_INFO("recording to {}", Settings.RecordPath.string());
		return true;
	}

	void Server::Shutdown() {
		if (Recorder_ && Recorder_->Recording_() && !Settings.RecordPath.empty()) {
			engine::core::ByteWriter writer;
			if (Recorder_->Write(writer) && WriteFile(Settings.RecordPath, writer.Bytes())) {
				ENGINE_INFO("wrote {} barrier(s) to {}", Recorder_->Barriers(), Settings.RecordPath.string());
			} else {
				ENGINE_ERROR("could not write the recording to '{}'", Settings.RecordPath.string());
			}
		}

		if (Link != nullptr) {
			Link->Close();
			Link.reset();
		}

		Running = false;
		Recorder_.reset();
		Replayer_.reset();

		// The listener before the socket it borrows, and both before the driver
		// whose worlds it reads. A listener outliving its transport is a
		// dangling reference in a destructor, which is the least debuggable
		// place for one.
		Replication.reset();
		if (Socket != nullptr) {
			Socket->Close();
			Socket.reset();
		}

		Driver_.reset();
		engine::parallel::Jobs::Stop();
	}

	void Server::Stop() {
		StopRequested.store(true);
	}

	bool Server::Enter(const std::function<void(engine::ecs::Store &)> &body) {
		if (!Driver_ || !PrimaryWorld.IsValid()) {
			return false;
		}
		return Worlds().Enter(PrimaryWorld, body) == engine::world::WorldStatus::Ok;
	}

	RunSummary Server::Run() {
		RunSummary summary;
		if (!Running) {
			return summary;
		}

		const double budget = 1.0 / Settings.TickRate;
		const auto budgetNanoseconds = static_cast<uint64_t>(budget * 1e9);
		const auto delta = static_cast<float>(budget);

		const uint64_t started = engine::core::Clock::Nanoseconds();
		uint64_t nextTickAt = started;
		double totalTickSeconds = 0.0;

		const auto ticksSoFar = [this] { return Worlds().StatisticsOf(PrimaryWorld).Ticks; };

		if (Settings.ControlPort >= 0) {
			ControlSurface.AddUniverseTools(Worlds());
			if (ControlServer.Start(static_cast<uint16_t>(Settings.ControlPort))) {
				ENGINE_INFO(
					"control: listening on 127.0.0.1:{} — {} tools",
					ControlServer.Port(),
					ControlSurface.Count()
				);
			}
		}

		while (Running && !StopRequested.load()) {
			const uint64_t tickStarted = engine::core::Clock::Nanoseconds();

			engine::core::FrameGraph::BeginFrame();

			// **Between the frame's start and its work**, which is where the
			// editor pumps it too: a tool that writes a property is doing what a
			// hand on the mouse does, so it lands at the same point in the loop.
			if (ControlServer.IsRunning()) {
				ControlServer.Pump([this](const std::string &line) { return ControlSurface.Answer(line); });
			}

			// Beside the control surface, and for the same reason it is here:
			// content delivery is not part of the tick. A fetch that completes
			// between two ticks changes nothing a recorded run would have to
			// reproduce — CDN.md §3 — so it is pumped where the frame is
			// bookkept rather than where the world is simulated, and
			// `just determinism` is unaffected by whether anyone is fetching.
			if (ContentService) {
				ContentService->Pump(
					static_cast<uint64_t>(engine::core::Clock::Nanoseconds() / 1'000'000'000ull)
				);
			}

			if (Replayer_) {
				// The recording decides the frame time as well as the traffic:
				// replaying with a different one would run a different number
				// of ticks and stop being the same run.
				if (!Replayer_->Step(Worlds())) {
					engine::core::FrameGraph::EndFrame();
					break;
				}
				if (Recorder_) {
					// The recorded delta, not this run's. Capturing a measured
					// one would write a file that differs from its source in
					// every barrier for a reason that has nothing to do with
					// whether the replay was faithful.
					Recorder_->Capture(Worlds(), Replayer_->LastFrameSeconds());
				}
			} else {
				// Before the tick, so what the driver decided last barrier is
				// in the inboxes by the time the systems that read them run.
				if (!ServiceLink()) {
					engine::core::FrameGraph::EndFrame();
					break;
				}

				// A fixed delta, not the measured one. A tick is a function of
				// its state and its inbox; feeding it real elapsed time would
				// make a recorded run unreplayable and every physics result
				// machine-dependent.
				// The driver's barrier rather than the universe's, so worlds
				// held by a host route through the same buses in the same
				// order as the ones held here. With no hosts the two are the
				// same call plus five lines of nothing, which is why there is
				// only one path.
				Driver_->Tick(delta, static_cast<double>(engine::core::Clock::Nanoseconds()) / 1e9);
				if (Recorder_) {
					Recorder_->Capture(Worlds(), delta);
				}

				if (Link != nullptr) {
					// A federated universe collects and orders its worlds'
					// requests without applying them, so this is exactly what a
					// driver's barrier would have had.
					Link->SendTraffic(Worlds().LastTraffic());

					// The world's own cumulative count, not the universe's
					// per-barrier one. A driver watching this for a host that
					// is wedged rather than dead needs a number that only ever
					// goes up.
					// The tick cost travels with the count. A driver cannot
					// time another address space, so what it graphs for this
					// host is the number measured here and sent.
					Link->Heartbeat(
						Worlds().StatisticsOf(PrimaryWorld).Ticks,
						Worlds().StatisticsOf(PrimaryWorld).LastTickMilliseconds
					);
				}
			}

			// Presentation is a separate call because a *client* renders one
			// world while the rest keep simulating. A headless server has no
			// such choice: `PreRender` is where deriving what to send lives —
			// the same shape as deriving what to draw — so it runs every tick,
			// with an alpha of zero because nothing here interpolates.
			Worlds().Present(PrimaryWorld, delta, 0.0f);

			// After presentation, because that is the phase this comment has
			// always said replication extraction belongs to, and before the next
			// tick clears the change bits it reads.
			//
			// A replay does not serve clients: a recording reproduces a run, and
			// a run that also streamed would depend on whether anybody was
			// connected — which is exactly the kind of input that stops a replay
			// being byte-identical.
			if (!Replayer_) {
				ServeClients(static_cast<double>(tickStarted) / 1e9);
			}

			engine::core::FrameGraph::EndFrame();
			ENGINE_PROFILE_FRAME();

			const uint64_t tickEnded = engine::core::Clock::Nanoseconds();
			const auto spent = static_cast<double>(tickEnded - tickStarted) / 1e9;

			totalTickSeconds += spent;
			summary.SlowestTickMilliseconds =
				std::max(summary.SlowestTickMilliseconds, static_cast<float>(spent * 1000.0));

			// From the world, which counted the tick it just ran. The loop does
			// not keep its own tally.
			const uint64_t ticks = ticksSoFar();

			if (Settings.MaximumTicks >= 0 && ticks >= static_cast<uint64_t>(Settings.MaximumTicks)) {
				break;
			}

			const double elapsed = static_cast<double>(tickEnded - started) / 1e9;
			if (Settings.Seconds > 0.0 && elapsed >= Settings.Seconds) {
				break;
			}

			if (Settings.Unpaced || Replayer_) {
				// A replay is not paced: it reproduces a run rather than
				// performing one, and sleeping between barriers would only make
				// it take as long as the original did.
				continue;
			}

			// Pace against an absolute schedule rather than sleeping for
			// "budget minus spent". The latter accumulates the sleep's own
			// overshoot, so a server drifts slower than its stated tick rate
			// and nothing in the numbers says why.
			nextTickAt += budgetNanoseconds;
			const uint64_t now = engine::core::Clock::Nanoseconds();
			if (now < nextTickAt) {
				std::this_thread::sleep_for(std::chrono::nanoseconds(nextTickAt - now));
			} else {
				summary.Overruns++;
				// Far behind: give up on catching the missed ticks rather than
				// spiralling. A server that tries to make up a lost second by
				// running thirty ticks back to back falls further behind.
				if (now - nextTickAt > budgetNanoseconds * 4) {
					nextTickAt = now;
				}
			}
		}

		summary.Ticks = ticksSoFar();
		summary.Seconds = static_cast<double>(engine::core::Clock::Nanoseconds() - started) / 1e9;
		summary.MeanTickMilliseconds =
			summary.Ticks > 0
				? static_cast<float>(totalTickSeconds / static_cast<double>(summary.Ticks) * 1000.0)
				: 0.0f;

		ENGINE_INFO(
			"{} tick(s) over {:.2f}s · mean {:.3f} ms · slowest {:.3f} ms · {} overrun(s)",
			summary.Ticks,
			summary.Seconds,
			summary.MeanTickMilliseconds,
			summary.SlowestTickMilliseconds,
			summary.Overruns
		);

		return summary;
	}
}
