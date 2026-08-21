#include <engine/assets/ChunkStore.hpp>
#include <engine/assets/Grant.hpp>
#include <engine/assets/Signature.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/core/FrameGraph.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Paths.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/delivery/Relay.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/game/CollisionContent.hpp>
#include <engine/game/Content.hpp>
#include <engine/game/Game.hpp>
#include <engine/game/Play.hpp>
#include <engine/gui/Services.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/parallel/Process.hpp>
#include <engine/parallel/Settings.hpp>
#include <engine/physics/Query.hpp>
#include <engine/replication/Defaults.hpp>
#include <engine/replication/Priority.hpp>
#include <engine/replication/SnapshotBuffer.hpp>
#include <engine/scene/Awake.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/CollisionShapes.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Ownership.hpp>
#include <engine/scene/Services.hpp>
#include <engine/world/Lifecycle.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cdn/Origin.hpp>
#include <cdn/Service.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <server/ContentRelay.hpp>
#include <server/Server.hpp>
#include <server/Simulation.hpp>
#include <thread>

namespace server {

	namespace {
		// Parses an exact-length hexadecimal value into `out`.
		bool ParseHex(std::string_view text, std::span<std::byte> out) {
			if (text.size() != out.size() * 2) {
				return false;
			}
			for (size_t index = 0; index < out.size(); index++) {
				const std::string byte(text.substr(index * 2, 2));
				char *end = nullptr;
				const long value = std::strtol(byte.c_str(), &end, 16);
				if (end != byte.c_str() + 2) {
					return false;
				}
				out[index] = static_cast<std::byte>(value);
			}
			return true;
		}

		// Turns one `--content-sources` entry into a source.
		//
		// **The client's spelling, on purpose.** `NAME=LOCATION` names the entry
		// and `dir:` marks a published tree; anything else is an address. A name
		// is what a log line and an operator's configuration refer to, which is
		// AGENTS.md rule 4 - so an entry that gives none is named after the
		// location it came from rather than after its position in the list.
		// Wall time, in whole seconds.
		//
		// **A grant is checked against wall time because whoever checks it uses
		// wall time.** `cdn`'s own main says the same in as many words: the origin
		// on the other side of a relay is a different process and possibly a
		// different machine, and a steady clock's epoch is neither.
		// How long a relay's own upstream grant is good for.
		//
		// Longer than a client's, because it is reissued only when this process
		// restarts and a relay that stopped being admitted mid-session would look
		// exactly like an origin that went down.
		constexpr uint64_t RELAY_GRANT_SECONDS = 24 * 60 * 60;

		// The session a relay's own grant is issued under.
		//
		// **Not zero, because zero is not a session** - `GrantScope::IsValid`
		// refuses it, so a grant issued under it is never minted at all and the
		// upstream refuses every bundle with nothing anywhere saying why. And
		// not a small number either: a client's is `ClientId::Index + 1`, and
		// this fetch is this process's own rather than any player's.
		constexpr uint64_t RELAY_SESSION = ~uint64_t{0};

		uint64_t WallSeconds() {
			using namespace std::chrono;
			return static_cast<uint64_t>(
				duration_cast<seconds>(system_clock::now().time_since_epoch()).count()
			);
		}

		engine::delivery::Source ParseContentSource(const std::string &entry) {
			std::string_view text(entry);
			std::string name;

			if (const size_t equals = text.find('='); equals != std::string_view::npos && equals > 0) {
				// A `dir:` prefix carries a colon and a Windows path carries a
				// backslash, so the half in front of the `=` is only read as a name
				// when it holds neither - otherwise `C:\\store` would be a name.
				const std::string_view head = text.substr(0, equals);
				if (head.find('/') == std::string_view::npos && head.find('\\') == std::string_view::npos &&
					head.find(':') == std::string_view::npos) {
					name = std::string(head);
					text = text.substr(equals + 1);
				}
			}

			const bool directory = text.starts_with("dir:");
			engine::delivery::Source source;
			source.Name = name.empty() ? std::string(text) : name;
			source.Kind =
				directory ? engine::delivery::SourceKind::Directory : engine::delivery::SourceKind::Http;
			source.Location = directory ? std::string(text.substr(4)) : std::string(text);
			source.Enabled = true;
			// **Read, because a server relays and never publishes on a client's
			// behalf.** A write origin is invisible to a fetch, which is exactly
			// what this list is for - see `delivery::SourceRole`.
			source.Role = engine::delivery::SourceRole::Read;
			return source;
		}
	}

	const char *Describe(ContentMode mode) {
		switch (mode) {
		case ContentMode::Relay:
			return "relay";
		case ContentMode::Redirect:
			return "redirect";
		}
		return "unknown";
	}

	std::optional<ContentMode> ContentModeOf(std::string_view text) {
		if (text == "relay") {
			return ContentMode::Relay;
		}
		if (text == "redirect") {
			return ContentMode::Redirect;
		}
		return std::nullopt;
	}

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

		// What one confirmed hit takes off a character.
		//
		// **A quarter of a default `Humanoid::MaxHealth`, chosen against that
		// number rather than tuned.** Four hits to a kill is the smallest figure
		// that makes a death reachable in a session and still leaves the health
		// bar saying something on the way there; a game with an opinion sets its
		// own on the humanoid it spawns.
		//
		// **A constant and never a roll.** A damage drawn from a random number is
		// a recording that does not replay, which is the rule `scene::FindSpawn`
		// already keeps about picking a pad in tree order - and `just
		// replay-check` would report it a long way from here.
		constexpr float SHOT_DAMAGE = 25.0f;

		// This executable, for spawning hosts of itself.
		//
		// `Paths::Base()` is the directory the binary sits in, which is where
		// it is staged. Deriving it rather than taking it from `argv[0]` means
		// a server started through a shell alias or a symlink still finds
		// itself.
		std::filesystem::path ThisProgram() {
			return engine::core::Paths::Base() / engine::core::Paths::Program("server");
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

	// Out of line so CDN types remain incomplete in the public header.
	Server::~Server() {
		ContentShapes.reset();
		ContentLink.reset();
		ContentService.reset();
		ContentOrigin.reset();
		ContentGrantSecret.reset();
	}

	void Server::InstallCollisionShapes(engine::ecs::Store &store) {
		engine::game::RecordBuiltinCollisionShapes(store);
		if (ContentShapes) {
			engine::game::MergeCollisionShapes(store, *ContentShapes);
		}
	}

	bool Server::BeginServingContent() {
		if (Settings.ContentStore.empty()) {
			return true;
		}

		auto store = engine::assets::ChunkStore::Open(Settings.ContentStore, false);
		if (!store) {
			ENGINE_ERROR("server: no content store at {}", Settings.ContentStore.string());
			ENGINE_ERROR(
				"server: publish one first - cdn --publish DIR --store {} --signing-key HEX",
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

		// The grant secret is deployment-provided; this path must not invent one.
		std::array<std::byte, engine::assets::GrantKey::BYTES> secret{};
		if (Settings.ContentGrantKey.size() != secret.size() * 2) {
			ENGINE_ERROR(
				"server: --content-store needs --content-grant-key, {} hex characters", secret.size() * 2
			);
			ENGINE_ERROR(
				"server: the secret is the deployment's to supply - this engine has no "
				"cryptographic generator to invent one with"
			);
			return false;
		}
		if (!ParseHex(Settings.ContentGrantKey, secret)) {
			ENGINE_ERROR("server: --content-grant-key is not hex");
			return false;
		}

		auto key = engine::assets::GrantKey::FromSecret(secret);
		if (!key) {
			return false;
		}

		// Keep separate key objects for the issuing and origin roles.
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

		// **Here, because this is the last point that holds both the store and
		// the manifest.** The publication takes the manifest and the service
		// takes the store, so a bake after these two lines has nothing to read.
		//
		// The worlds already exist - `StartHosts` and the primary world are both
		// built above this - so the table is pushed into them below rather than
		// waited for. Nothing has ticked yet: `Running` is still false.
		ContentShapes = std::make_unique<engine::scene::CollisionShapes>();
		const size_t shapes = engine::game::AddCollisionShapesFrom(*ContentShapes, *store, *manifest);
		if (shapes != 0) {
			ENGINE_INFO("server: baked collision geometry for {} mesh(es)", shapes);
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

		// **Into the worlds that were built before this ran.** A world created
		// after this point picks the table up from `InstallCollisionShapes`;
		// these were made during startup, above, when there was nothing to give
		// them. Without this a `--content-store` server's meshes would collide
		// as their bounds for the life of the process.
		for (const engine::world::WorldId id : Worlds().Worlds()) {
			if (Worlds().IsRemote(id)) {
				continue;
			}
			Worlds().Enter(id, [this](engine::ecs::Store &store) {
				engine::game::MergeCollisionShapes(store, *ContentShapes);
			});
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
		// bound - game content is not secret, it ships to everyone who plays.
		scope.ByteBudget = 1024ull * 1024ull * 1024ull;

		const auto grant = engine::assets::Grant::Issue(scope, *ContentGrantSecret);
		if (!grant) {
			return std::nullopt;
		}
		return grant->Encode();
	}

	const ContentRelayStatistics *Server::ContentRelayStats() const {
		return ContentLink ? &ContentLink->Stats() : nullptr;
	}

	std::vector<engine::delivery::Source> Server::ConfiguredContentSources() const {
		std::vector<engine::delivery::Source> sources;
		for (const std::string &entry : Settings.ContentSources) {
			if (entry.empty()) {
				continue;
			}
			sources.push_back(ParseContentSource(entry));
		}

		if (sources.empty() && !Settings.ContentStore.empty()) {
			// **One flag rather than two saying the same thing.** A server told to
			// serve a store is a server whose content source is that store, and
			// making an operator write it twice is two places to get it wrong.
			engine::delivery::Source own;
			own.Name = "server-store";
			own.Kind = engine::delivery::SourceKind::Directory;
			own.Location = Settings.ContentStore.string();
			own.Role = engine::delivery::SourceRole::Read;
			sources.push_back(std::move(own));
		}
		return sources;
	}

	bool Server::BeginRelayingContent() {
		// **Said once at start-up, because the symptom says nothing at all.** A
		// client is told the publisher key at admission and fetches nothing until
		// it has one, so a server with content and no key leaves every client
		// sitting on "waiting for the server to name a publisher key" for the
		// length of the session - a session that joins the world, draws it, and
		// never loads an asset, with no error anywhere. This is `ContentRoot::
		// Mount`'s rule: report the misconfiguration where it was made rather than
		// as a silence somebody else has to explain.
		if (!ConfiguredContentSources().empty() && Settings.ContentPublisherKey.empty()) {
			ENGINE_WARN(
				"server: content is configured with no --content-publisher-key, so clients are told no "
				"publisher to trust and will fetch nothing"
			);
		}

		if (Settings.ContentDelivery != ContentMode::Relay) {
			ENGINE_INFO("server: content mode {}", Describe(Settings.ContentDelivery));
			return true;
		}

		const std::vector<engine::delivery::Source> sources = ConfiguredContentSources();
		if (sources.empty()) {
			// Nothing was configured. Not a failure: a game whose content ships
			// inside its game file needs no origin at all, and building a relay
			// with nowhere to fetch from would refuse every route.
			return true;
		}

		engine::delivery::DeliverySettings settings;
		settings.Sources = sources;
		if (!Settings.ContentPublisherKey.empty()) {
			const auto key = engine::assets::PublicKey::FromHex(Settings.ContentPublisherKey);
			if (!key) {
				ENGINE_ERROR("server: --content-publisher-key is not 64 hex characters");
				return false;
			}
			settings.Publisher = *key;
		}

		std::unique_ptr<engine::delivery::RouteFetcher> fetcher =
			engine::delivery::MakeRouteFetcher(settings);
		if (!fetcher) {
			ENGINE_ERROR("server: content mode relay names no usable source");
			return false;
		}

		ContentLink = std::make_unique<ContentRelay>(std::move(fetcher));

		// **The grant this server presents to its own upstreams**, which is the
		// half of CDN.md §4 a relay sits on: a client presents nothing because it
		// reaches no origin, and this process presents what it issued itself. A
		// `dir:` source admits nobody and needs none, which is why an absent grant
		// is not an error here.
		bool upstreamNeedsGrant = false;
		for (const engine::delivery::Source &source : sources) {
			upstreamNeedsGrant = upstreamNeedsGrant || source.Kind == engine::delivery::SourceKind::Http;
		}

		if (const std::optional<std::vector<std::byte>> token =
				IssueContentGrant(RELAY_SESSION, WallSeconds(), RELAY_GRANT_SECONDS)) {
			ContentLink->UseGrant(*token);
		} else if (upstreamNeedsGrant) {
			// **Said out loud, because the symptom is a manifest that arrives
			// and a bundle that never does.** An origin admits a fetch against a
			// grant and nothing else, so a relay with an HTTP source and no
			// grant to present serves its clients a catalogue of content it
			// cannot get - which reads as content corruption a long way from
			// here.
			ENGINE_WARN(
				"server: relaying from an origin needs --content-store and --content-grant-key so this "
				"server can issue itself a grant - bundles will be refused without one"
			);
		}

		ENGINE_INFO(
			"server: content mode relay - {} source(s), first is '{}'", sources.size(), sources.front().Name
		);
		return true;
	}

	void Server::OfferContentDirectory(engine::replication::ClientId client) {
		if (Replication == nullptr) {
			return;
		}

		engine::game::ContentDirectory directory;

		// **Endpoints only in `Redirect`, and the publisher key in both.** A
		// relaying server has nowhere to point a client at - it *is* where the
		// content comes from - but a client still needs a root of trust, because
		// relayed bytes are verified exactly as fetched ones are. Naming origins
		// in relay mode would hand a client a second way to get content that this
		// deployment deliberately did not give it.
		if (Settings.ContentDelivery == ContentMode::Redirect) {
			for (const engine::delivery::Source &source : ConfiguredContentSources()) {
				if (directory.Endpoints.size() >= engine::game::MAXIMUM_CONTENT_ENDPOINTS) {
					break;
				}
				directory.Endpoints.push_back(
					engine::game::ContentEndpoint{
						.Name = source.Name,
						.Kind = source.Kind == engine::delivery::SourceKind::Directory ? "dir" : "http",
						.Location = source.Location,
					}
				);
			}

			if (const std::optional<std::vector<std::byte>> token =
					IssueContentGrant(client.Index + 1u, WallSeconds())) {
				// **The grant is what lets `cdn::Gate` admit this client without
				// the origin knowing anything about sessions or players.** The
				// origin decides nothing about who; this process does, and it says
				// so once.
				directory.Grant = *token;
			}
		}
		directory.PublisherKey = Settings.ContentPublisherKey;

		if (directory.Endpoints.empty() && directory.PublisherKey.empty()) {
			// Nothing to say. Saying it anyway would make a client rebuild its
			// delivery client for no reason.
			return;
		}

		const std::vector<std::byte> message = engine::game::EncodeContentDirectory(directory);
		if (!Replication->SendTo(client, message, PollNow)) {
			// **Not fatal, and said out loud.** A client that never hears where
			// content is falls back to whatever it was configured with, which on a
			// client configured with nothing is a world of missing textures.
			ENGINE_WARN("server: could not tell client {} where content is", client.Index);
		}
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

		const unsigned processes =
			Settings.Processes > 0 ? Settings.Processes : 1u + static_cast<unsigned>(PlannedHosts());

		// The configured count wins, and zero means work it out from how many
		// processes are sharing the machine - see `parallel::ConfiguredWorkers`.
		const unsigned configured = engine::parallel::ConfiguredWorkers();
		engine::parallel::Jobs::Start(
			configured != 0 ? configured : engine::parallel::WorkersPerHost(processes)
		);

		// Register names before any snapshot or world is built.
		RegisterPlaceholderComponents();

		engine::world::UniverseSettings universe;

		// Federated hosts do not own shared bus state.
		universe.Federated = IsHost();

		engine::world::DriverSettings driver;
		driver.Universe = universe;
		// A listening host is one replication authority and one UDP endpoint.
		// Keeping one world in it makes the process, physics state, ECS store,
		// and listener share the same failure and restart boundary.
		driver.Hosts.WorldsPerHost = Settings.Listening ? 1u : Settings.WorldsPerHost;

		driver.Hosts.Program = Settings.HostProgram.empty() ? ThisProgram() : Settings.HostProgram;

		driver.Hosts.Arguments = {
			"--unpaced",
			"--entities",
			std::to_string(Settings.Entities),
			"--tick-rate",
			std::to_string(Settings.TickRate),

			// Carried to the child, so a world hosted in another process steps
			// and publishes at the rate this one was asked for. Without these a
			// federated universe would run the same world at two rates
			// depending on which process picked it up.
			"--physics-tick-rate",
			std::to_string(Settings.PhysicsTickRate),
			"--replication-tick-rate",
			std::to_string(Settings.ReplicationTickRate),

			"--processes",
			std::to_string(1u + PlannedHosts()),
		};

		if (Settings.Chatter) {
			driver.Hosts.Arguments.emplace_back("--chatter");
		}
		if (Settings.Listening) {
			// Every child asks the OS for its own free port. The bound value comes
			// back in its Ready frame, so no process races another over a guessed
			// range and the driver can publish the exact endpoint.
			driver.Hosts.Arguments.emplace_back("--listen");
			driver.Hosts.Arguments.emplace_back("0");
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

			// Restore state, then install the systems for this executable.
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

			// Preserve replay recording so byte-for-byte replay checks remain valid.
			if (!BeginRecording()) {
				return false;
			}

			Running = true;

			ENGINE_INFO(
				"replaying {} barrier(s) from {}", Replayer_->Barriers(), Settings.ReplayPath.string()
			);
			return true;
		}

		if (IsGameFile(Settings.GamePath)) {
			if (!HostGameFile()) {
				return false;
			}
		} else {
			engine::world::WorldSettings world;
			world.Name = engine::core::Name(PRIMARY);
			world.TickRate = Settings.TickRate;
			world.PhysicsTickRate = Settings.PhysicsTickRate;
			world.ReplicationTickRate = Settings.ReplicationTickRate;

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

		// After the listening socket, because the port it announces is the one
		// that was bound rather than the one that was asked for.
		if (!BeginAnnouncing()) {
			return false;
		}

		if (!BeginServingContent()) {
			return false;
		}

		// After the origin, because a relay presents the grant this process
		// issues and there is nothing to issue one against until it exists.
		if (!BeginRelayingContent()) {
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
		// Replication is one world per connection today - `Session` binds a
		// client to a world - so a game of several scenes simulates all of them
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

			// Read before the borrow, so the settings lookup is not made from
			// inside the world it is asking about.
			const double physicsTickRate = Worlds().SettingsOf(id).PhysicsTickRate;

			std::string failure;
			Worlds().Enter(
				id,
				[this, &limits, &failure, id, physicsTickRate](
					engine::ecs::Store &store, engine::ecs::Scheduler &systems
				) {
					// **Before the scripts, because a script may create a
					// part.** `PreparePhysicsWorld` calls `Store::Observe`,
					// which moves every row already carrying the component
					// into an archetype with somewhere to put the change
					// bits - a structural change, and one that is free on an
					// empty world and a re-shuffle on a populated one.
					PrepareSimulation(store, systems, physicsTickRate);

					// **Beside the physics, because it is what the physics
					// resolves a mesh collider through.** See `ContentShapes`.
					InstallCollisionShapes(store);

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
			"hosting '{}' - {} world(s), serving '{}'",
			info.Name.IsValid() ? info.Name.Text() : "game",
			worlds.size(),
			Worlds().NameOf(PrimaryWorld).Text()
		);
		return true;
	}

	bool Server::BuildWorld(engine::ecs::Store &store, engine::ecs::Scheduler &scheduler) {
		if (Settings.GamePath.empty()) {
			// **No physics on the placeholder, deliberately.** It is a
			// benchmark scene rather than a game: its entities carry no
			// `Part` and so no rigid body, its own `Integrate` and `Bounce`
			// are what it exists to measure, and `--entities 512 --ticks 200`
			// is the shape `just determinism` and `just replay-check` compare.
			// Preparing it would add a resource and two systems that find
			// nothing to do, and put them inside the thing being measured.
			BuildPlaceholderWorld(store, scheduler, Settings.Entities);
			return true;
		}

		// The same loader the client calls, over the same file. A server that
		// authored the scene differently would be disagreeing with its own
		// replicas once per tick, and every side would look self-consistent.
		//
		// What it does *not* install is the client's half - there is no camera
		// and no draw list here, because a server draws nothing. That split is
		// the reason the loader stops where it does.
		PrepareSimulation(store, scheduler, Settings.PhysicsTickRate);
		InstallCollisionShapes(store);

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

		Socket = engine::net::MakeUdpTransport(Settings.ListenPort);
		if (Socket == nullptr) {
			ENGINE_ERROR("could not bind UDP port {}", Settings.ListenPort);
			return false;
		}

		// **The anti-entropy audit is on for a real server and off by default in
		// the library**, and the split is deliberate rather than timid. What it
		// finds is a client quietly disagreeing with the world on a value
		// nothing is moving - which no delta reports and which, on a game
		// server, nobody is standing over the logs to notice. The cost is a
		// small message every eight ticks per client. See `Audit.hpp` for why
		// the default cannot simply be on: it is the one thing that speaks on a
		// world at rest, and a caller measuring an idle link has to have said
		// yes to that.
		engine::replication::ListenerSettings streaming;
		streaming.Authority.Audit.Enabled = true;
		streaming.MaximumClients = Settings.MaximumClients;

		Replication = std::make_unique<engine::replication::Listener>(*Socket, streaming);

		if (!Replication->Admitting()) {
			// The admission challenge is drawn from operating system entropy and
			// there was none. Refusing to start beats listening on a port that
			// turns every client away, which reads from outside exactly like a
			// firewall problem.
			ENGINE_ERROR("the admission challenge could not be seeded, so this server can admit nobody.");
			return false;
		}

		// **One table, and it is `replication`'s.** This was written out here,
		// in `mono.unified_tests` and in `mono.studio`, and D00018 said
		// all three agreed - which was true of this one and the studio's and
		// not of the harness, whose own comment claimed it was duplicated from
		// here. Nothing in the build compared them. `DefaultReplicatedComponents`
		// is where the pairing lives now, and it carries the argument for every
		// row: which are written every tick and therefore observed, which are
		// written once by a script and therefore signed, and why getting one
		// wrong is silent in both directions.
		//
		// Still opt-in: a host declares these and may declare more. What is
		// gone is three chances to declare them differently.
		for (const engine::replication::ReplicatedComponent &component :
			 engine::replication::DefaultReplicatedComponents()) {
			Replication->Authority().Replicate(engine::core::Name(component.Name), component.Detection);

			// After `Replicate`, which is what declares the slot this names.
			if (!component.Suppressor.empty()) {
				Replication->Authority().SuppressWhenTagged(
					engine::core::Name(component.Name), engine::core::Name(component.Suppressor)
				);
			}
		}

		// **The priority score, and it is the first thing ever to fill this
		// hook in.** `Authority::SetPriority` has existed since v0.4 with
		// nothing supplying it, so the rotation has been in sole charge and the
		// order a plain round robin - which is fine for a world that fits and
		// wrong for one that does not, because the thing a player is standing
		// next to updates no more often than the thing across the map.
		//
		// **The arithmetic is `replication`'s and the lookup is this
		// program's**, which is the division `SetPriority`'s own documentation
		// asks for: that module carries named components and has no idea which
		// of them is a position. `DistancePriority` supplies the falloff and the
		// guards; the two lambdas below are the part that knows what a
		// `scene::Transform` is.
		{
			engine::replication::DistancePriority score;

			score.Position = [this](engine::ecs::Entity entity, engine::core::Vector3 &out) {
				return PositionOf(entity, out);
			};

			// **Where a client is looking from, which this server now knows.**
			// It did not until v0.15: the comment here said the placeholder
			// world was cubes with nobody in it, and it was right - nothing
			// called `SetClientViewpoint`, so `DistancePriority` fell back to
			// the round robin it was written to replace, on every connection,
			// for three versions. `UpdateClientViewpoints` points each client at
			// its own character once a tick.
			//
			// A client nothing has placed still scores everything the same, and
			// that is still the honest answer rather than pretending every
			// client is at the origin: between a join and a spawn there is
			// genuinely nowhere it is standing.
			score.Viewpoint = [this](engine::replication::ClientId client, engine::core::Vector3 &out) {
				return ViewpointOf(client, out);
			};

			// **What a client cannot see sorts below what it can**, and the
			// query is this program's for the reason the two above are: it
			// needs a broad phase and a game's idea of what occludes, and
			// neither is `replication`'s.
			//
			// Cast from the viewpoint towards the entity: anything the ray meets
			// noticeably nearer than the entity itself is between them. The
			// entity is hit by its own ray, which is why the comparison has a
			// margin rather than being "did it hit anything".
			score.Blocked = [this](engine::replication::ClientId client, engine::ecs::Entity entity) {
				engine::core::Vector3 eye;
				engine::core::Vector3 at;
				if (!ViewpointOf(client, eye) || !PositionOf(entity, at)) {
					return false;
				}

				const engine::core::Vector3 gap = at - eye;
				const float distance = gap.Magnitude();
				if (distance <= 0.01f) {
					return false;
				}

				// `Publishing` is non-null here because `PositionOf` answered,
				// and that is the only way it does.
				const engine::core::Ray ray(eye, gap.Unit());
				const auto hit = engine::physics::Raycast(*Publishing, ray, distance);

				// **A margin, because the entity is hit by its own ray.** Ten
				// centimetres is well inside anything a wall could be and well
				// outside the floating-point noise of a cast that lands on the
				// target's own surface.
				return hit.has_value() && hit->Owner != entity && hit->Distance < distance - 0.1f;
			};

			// **`ReplicatedFirst` outranks the distance in the *stream*, which
			// is half of what that container is for.** Roblox replicates it
			// ahead of the rest of the tree so a loading screen is up before the
			// world it covers arrives; here it was an ordinary root with an
			// ordinary score, so a screen in it landed somewhere in the middle
			// of the scene it was meant to hide. The other half is the join, and
			// no score reaches that - see `SetPreface` below.
			//
			// **Above the falloff's range rather than inside it.**
			// `DistancePriority` answers zero to one, so any number above one
			// sorts every one of its answers below this - and the starvation
			// rotation still outranks *this*, which is right: a value that has
			// waited its deadline goes first whatever it is about, or the
			// bound `AGENTS.md` calls "a bound rather than a hope" would stop
			// being one.
			//
			// **A world entry per candidate, which is `PositionOf`'s cost and
			// is paid the same way.** The alternative is a set rebuilt per
			// publish, which is a second record of a containment the tree
			// already states.
			// Named out here rather than inside the scorer because both hooks
			// below have to agree about it: one grants the rank and the other has
			// to recognise it in order to leave it alone.
			constexpr float REPLICATED_FIRST = 2.0f;

			Replication->Authority().SetPriority(
				[this, score](engine::replication::ClientId client, engine::ecs::Entity entity) {
					// The publish's own store, for `PositionOf`'s reason: this runs
					// once per candidate per client and re-entering the world to
					// walk one entity's ancestry was the second of the two entries
					// this path was paying per call.
					const bool first =
						Publishing != nullptr && engine::scene::InReplicatedFirst(*Publishing, entity);
					return first ? REPLICATED_FIRST : score(client, entity);
				}
			);

			// **The occlusion raycast, asked only about the rows in
			// contention.** `SetPriorityRefinement` is consulted for what a
			// tick could carry rather than for every entity in the world, which
			// is the difference between two hundred casts a client and two
			// thousand. `Authority` clamps the answer to the score it was given,
			// so this can only ever push a row back.
			//
			// **`ReplicatedFirst` is exempt, exactly as it is in the score.**
			// Its rank is deliberately above everything the falloff can produce,
			// and a quarter of it is not - so demoting a loading screen because
			// a wall is in front of it would put the screen behind the world it
			// exists to hide.
			Replication->Authority().SetPriorityRefinement(
				[score](engine::replication::ClientId client, engine::ecs::Entity entity, float hint) {
					if (hint >= REPLICATED_FIRST) {
						return hint;
					}
					return score.Refine(client, entity, hint);
				}
			);

			// **And the same rule where a client actually meets it first, which
			// is the join and not the stream.** A score orders the values a
			// running world produces; a join is one `Save` chunked across ticks
			// in archetype order, and the line above has never touched it - so a
			// loading screen was ranked first for every tick after the moment it
			// was needed. `D00122`, and closing it is a second snapshot rather
			// than a cleverer score.
			//
			// **No world entry, unlike the score above.** `SetPreface` is handed
			// the store it is walking, exactly as `SetInterest` is and for the
			// same reason: it runs inside the snapshot build, and re-entering
			// the world from inside a walk of it would be entering it twice.
			Replication->Authority().SetPreface([](engine::ecs::Entity entity,
												   const engine::ecs::Store &store) {
				return engine::scene::InReplicatedFirst(store, entity);
			});
		}

		// **A `Player` per connection, which nothing in this engine was
		// making.** `scene::AddPlayer` had no production caller at all - every
		// world that ever ran had a `Players` service with nobody in it - and
		// the consequence was not cosmetic: ownership is assigned to a player,
		// so a server with no players had nobody to assign it to and
		// `SetNetworkOwner` had no argument a script could obtain.
		//
		// **What a client says about content, and the only thing it may say.**
		// The payload is opaque to `replication` by design, so this is where it
		// stops being opaque - and it stops being opaque behind a rate limit, an
		// outstanding bound and a closed list of routes, because a client is
		// untrusted and every one of those has to be held on this side.
		Replication->OnUserMessage(
			[this](engine::replication::ClientId client, std::span<const std::byte> payload) {
				if (ContentLink != nullptr) {
					ContentLink->Receive(client, payload, PollNow);
				}
			}
		);

		Replication->OnAdmitted([this](engine::replication::ClientId client) {
			// Apply the world delay before the join notice and first snapshot are
			// sent. Placeholder worlds have no Player row but still use this value.
			Replication->SetSimulatedLatency(
				client, Worlds().SettingsOf(PrimaryWorld).GlobalSimulatedNetworkLatency
			);

			Worlds().Enter(PrimaryWorld, [this, client](engine::ecs::Store &store) {
				// **A world with no `Players` service gets no player, quietly.**
				// That is not a misconfiguration to warn about - it is the
				// placeholder world, which is furnished by nobody and is what
				// `--entities` builds. A game file has services and gets one.
				if (engine::scene::PlayersOf(store) == engine::ecs::NULL_ENTITY) {
					return;
				}

				// Named for the slot rather than for anything the client said.
				// A name a client chose is a field of an inbound message, and
				// this one ends up in the world tree where scripts index by it.
				const std::string name = "Player" + std::to_string(client.Index + 1);
				const engine::ecs::Entity player = engine::scene::AddPlayer(store, name);
				if (player == engine::ecs::NULL_ENTITY) {
					// **A world at `Players.MaxPlayers` is the case worth
					// saying out loud.** The transport admitted the socket -
					// `ListenerSettings::MaximumClients` is a different number
					// and a different question - so a silent return here is a
					// connected client watching a world it can never enter,
					// with nothing anywhere saying why.
					ENGINE_WARN("server: '{}' got no player - the world is full or has no Players", name);
					return;
				}

				Players[client.Index] = Occupant{player, client.Generation};

				// **And a body, which is the other half of admitting somebody.**
				// A `Player` with no character is a row in a service that nothing
				// draws and nothing can move - every world this repository shipped
				// before now was in exactly that state, so `--listen` produced a
				// scene a client could watch and never enter.
				//
				// Ownership of the root goes to this player inside `LoadCharacter`,
				// which is what makes the client's own movement authoritative and
				// nobody else's.
				// **The player's own interface, copied from the world's
				// template.** `StarterGui` is a template and what a player sees
				// is their copy - see `gui::ResetPlayerGui`, which also carries
				// why a `ResetOnSpawn = false` collector survives a death. The
				// copies are ordinary world content on this authority, so they
				// replicate to exactly one client: `SetInterest` above hides
				// what is under a player from everybody else.
				//
				// **Before the character, not after.** A `ScreenGui` a script
				// reaches for from a spawn handler has to exist by the time the
				// handler runs, and `LoadCharacter` is what a game hangs that
				// handler on.
				(void)engine::gui::ResetPlayerGui(store, player);

				// **Unless the game says it spawns its own occupants.**
				// `Players.CharacterAutoLoads` is what a lobby sets to false,
				// and a host that ignored it would hand everybody a body the
				// game then has to destroy - which is a frame of a character
				// standing in the world before a script can stop it.
				//
				// The same flag is what `scene::UpdateRespawns` reads for every
				// life after this one, so the join and the respawn are one
				// decision rather than two that can disagree.
				const engine::scene::PlayersServiceComponent *settings =
					store.Get<engine::scene::PlayersServiceComponent>(engine::scene::PlayersOf(store));
				if (settings != nullptr && !settings->CharacterAutoLoads) {
					ENGINE_INFO("server: {} joined without a body - CharacterAutoLoads is off", name);
				} else if (engine::scene::LoadCharacter(store, player) == engine::ecs::NULL_ENTITY) {
					ENGINE_WARN("server: '{}' has no character - the world has no Workspace", name);
				}

				// **Which player is theirs, over the user channel.** It cannot be
				// replicated: `scene::LocalPlayer` is one resource per world and
				// the answer differs per client, so it travels as a per-client
				// message - `game/Join.hpp` carries the whole argument.
				const std::vector<std::byte> notice =
					engine::game::EncodeJoinNotice(engine::game::JoinNotice{player});
				if (!Replication->SendTo(client, notice, PollNow)) {
					// **Not fatal, and said out loud.** A client that never
					// learns its player watches the world and cannot move in it,
					// which is a symptom with no other explanation attached.
					ENGINE_WARN("server: could not tell '{}' which player is theirs", name);
				}

				ENGINE_INFO("server: {} joined as '{}'", name, Worlds().NameOf(PrimaryWorld).Text());
			});

			// **Where content is, said once at admission and outside the world.**
			// It is per-client - the grant names this session - so it cannot be
			// replicated, and it is nothing to do with whether the world had a
			// `Players` service to put anybody in. In relay mode it names no
			// origin - there is nowhere to point a client at - and still carries
			// the publisher key, because relayed bytes are verified too.
			OfferContentDirectory(client);
		});

		// **And destroyed when they leave, which is what makes the reclaim
		// fire.** `scene.ownership` gives back every body owned by an entity
		// that is no longer alive; destroying the player is what makes that
		// entity not alive. Leaving it behind would be a body owned for ever by
		// somebody who has gone.
		Replication->OnDropped([this](engine::replication::ClientId client) {
			if (ContentLink != nullptr) {
				// **Before the player, because a slot is reused immediately.** A
				// relay session left behind would hand the next client on this slot
				// the previous one's allowance, and a half-sent group nobody is
				// waiting for.
				ContentLink->Forget(client);
			}

			const auto found = Players.find(client.Index);
			if (found == Players.end() || found->second.Generation != client.Generation) {
				return;
			}

			const engine::ecs::Entity player = found->second.Instance;
			Players.erase(found);

			// **And where they were standing, which is a second map keyed on
			// the same slot.** A slot is reused the moment somebody else joins,
			// and `Viewpoint::Generation` is what stops the new client
			// inheriting it - but an entry left behind is one the next client on
			// this slot sorts by until its own first tick lands, which is a
			// world ordered by where a stranger stood.
			ForgetClientViewpoint(client);

			Worlds().Enter(PrimaryWorld, [player](engine::ecs::Store &store) {
				store.DestroyInstance(player);
			});
		});

		// **Who may write what, and it is the only thing between a client's
		// delta and this world.** The predicate is the division
		// `SetOwnership` asks for: that module carries entity handles and has
		// no idea what a player is, and this is the part that knows a
		// `NetworkOwner` names one.
		//
		// Absent means the server owns it, so an unowned entity refuses - which
		// is every entity in every world this repository ships until a script
		// hands one over.
		Replication->Authority().SetOwnership([this](
												  engine::replication::ClientId client,
												  engine::ecs::Entity entity,
												  const engine::ecs::Store &store
											  ) {
			const auto found = Players.find(client.Index);
			if (found == Players.end() || found->second.Generation != client.Generation) {
				return false;
			}

			// The world it was handed, not one this looks up: `ApplySubmitted`
			// runs inside a world the host has already entered.
			return engine::scene::NetworkOwnerOf(store, entity) == found->second.Instance;
		});

		// **What a client may be *shown*, which until v0.15 was everything.**
		// `ServiceComponent::Scope` had said since v0.7 that
		// `ServerScriptService` and `ServerStorage` are the server's, it
		// round-tripped through save files, it showed in the properties panel -
		// and nothing read it, so a game's server scripts and its unreleased
		// content went down the wire to every client that joined.
		//
		// **Two rules, and they are different questions.** Scope is per world: a
		// server-scoped service is hidden from *every* client and the answer does
		// not depend on who is asking. Ownership of a player's own subtree is per
		// client: `Players` is `Shared` because both halves need the list of who
		// is connected, and what is under one player's row is that player's
		// alone - a second client shown somebody else's `PlayerGui` gets an
		// interface it cannot interact with.
		//
		// **The rules live in `scene` and the plumbing lives here**, which is the
		// same split `SetOwnership` makes one block up: `mono.engine/replication`
		// does not depend on `scene` and must not, because the wire's job is to
		// move components and it has no business knowing what a service is.
		Replication->Authority().SetInterest(
			[this](
				engine::replication::ClientId client, engine::ecs::Entity entity, const engine::ecs::Store &
			) {
				// **Two lookups into what `SurveyVisibility` worked out this tick,
				// rather than two walks up the tree.** Both of those questions are
				// about the world rather than about who is asking, and this runs once
				// per entity per client - see `HiddenFromClients`.
				if (std::binary_search(HiddenFromClients.begin(), HiddenFromClients.end(), entity.Id)) {
					return false;
				}

				// **Almost everything is nobody's**, so this answers first and the
				// player lookup below is paid only by the few rows under a player.
				const auto owned = std::lower_bound(
					OwnedByPlayer.begin(),
					OwnedByPlayer.end(),
					entity.Id,
					[](const std::pair<uint64_t, engine::ecs::Entity> &row, uint64_t id) {
						return row.first < id;
					}
				);
				if (owned == OwnedByPlayer.end() || owned->first != entity.Id) {
					return true;
				}

				const engine::ecs::Entity owner = owned->second;

				// **The `Player` row itself goes to everybody; what is *under* it
				// does not.** Roblox draws the line in the same place and for the
				// same reason: `Players:GetPlayers()` is how a game knows who is in
				// it, and a client shown only its own row would think it was alone.
				// `server.replication`'s "a client is told which player is theirs"
				// case is what caught this - the first version of this predicate hid
				// every player from every other client, which reads as a lobby that
				// never fills.
				if (entity == owner) {
					return true;
				}

				// **A client this server has forgotten sees nothing under any
				// player**, rather than everything: the map is the only statement of
				// who a connection is, and a handle it does not answer for is a
				// connection that has gone. Failing open here would show a departing
				// client every player's interface on its way out.
				const auto occupant = Players.find(client.Index);
				if (occupant == Players.end() || occupant->second.Generation != client.Generation) {
					return false;
				}
				return owner == occupant->second.Instance;
			}
		);

		// A delta is the third reader of the dirty bits, so the components that
		// travel *and change every tick* have to be observed or nothing ever
		// looks changed. The two above are signed instead - a hash of a value
		// that moves every tick is a pass over the world to learn what a bit
		// already knew.
		Worlds().Enter(PrimaryWorld, [](engine::ecs::Store &store) {
			store.Observe<engine::scene::Transform>();
			store.Observe<engine::scene::Motion>();
		});

		// **The identity, and the log line says which mode this server is in.**
		// A deployment that meant to sign and typo'd the flag would otherwise
		// look identical to one that meant not to.
		if (!Settings.IdentityKey.empty()) {
			std::array<std::byte, engine::assets::SigningKey::SEED_BYTES> seed{};
			if (!ParseHex(Settings.IdentityKey, seed)) {
				ENGINE_ERROR("server: --identity-key is not {} hex characters", seed.size() * 2);
				return false;
			}

			Identity = engine::assets::SigningKey::FromSeed(seed);
			if (!Identity.has_value()) {
				ENGINE_ERROR("server: --identity-key is not a usable Ed25519 seed");
				return false;
			}

			Replication->SetIdentity(&*Identity);
			ENGINE_INFO("replication identity {}", Identity->Public().ToHex());
		} else {
			ENGINE_WARN(
				"replication: no --identity-key, so the exchange authenticates nobody. "
				"It is encrypted against a listener and open to a relay."
			);
		}

		ENGINE_INFO("replication listening on {}", Socket->Local().Text());
		return true;
	}

	void Server::SurveyVisibility(engine::ecs::Store &store) {
		ENGINE_PROFILE_CAT("Server::SurveyVisibility", engine::core::ProfileCategory::Network);

		HiddenFromClients.clear();
		OwnedByPlayer.clear();

		// **One walk for the world, where the predicate it feeds is one walk per
		// entity per client.** `EachEntity` visits in archetype order, so both
		// lists are sorted afterwards rather than assumed to be - the predicate
		// reaches them by binary search.
		store.EachEntity([this, &store](engine::ecs::Entity entity) {
			if (!engine::scene::VisibleToClients(store, entity)) {
				HiddenFromClients.push_back(entity.Id);
				return;
			}

			const engine::ecs::Entity owner = engine::scene::PlayerOwning(store, entity);
			if (owner != engine::ecs::NULL_ENTITY) {
				OwnedByPlayer.emplace_back(entity.Id, owner);
			}
		});

		std::sort(HiddenFromClients.begin(), HiddenFromClients.end());
		std::sort(OwnedByPlayer.begin(), OwnedByPlayer.end(), [](const auto &left, const auto &right) {
			return left.first < right.first;
		});
	}

	bool Server::PositionOf(engine::ecs::Entity entity, engine::core::Vector3 &out) const {
		// **The store the publish is already inside, not a fresh entry into it.**
		// This is asked once per candidate per client, so on a full server it is
		// the most-called function in the process - and `Universe::Enter` is a
		// world lookup, a thread check, a rebind and two indirect calls before
		// it reaches the component. Reading `Publishing` is the same store by
		// the same pointer the caller above already holds.
		if (Publishing == nullptr) {
			return false;
		}

		const auto *placement = Publishing->Get<engine::scene::Transform>(entity);
		if (placement == nullptr) {
			return false;
		}

		out = placement->Frame.Position;
		return true;
	}

	bool Server::ViewpointOf(engine::replication::ClientId client, engine::core::Vector3 &out) const {
		const auto found = Viewpoints.find(client.Index);

		// **The generation is checked, not just the slot.** A slot is reused
		// when a client leaves and another joins, so keying on the index alone
		// would hand the new client the old one's viewpoint - and it would sort
		// *its* world by where somebody else had been standing.
		if (found == Viewpoints.end() || found->second.Generation != client.Generation) {
			return false;
		}
		out = found->second.At;
		return true;
	}

	void Server::ApplyMove(engine::replication::ClientId client, const engine::game::MoveInput &move) {
		const auto found = Players.find(client.Index);
		if (found == Players.end() || found->second.Generation != client.Generation) {
			// A move from a connection this server has no player for. Ordinary
			// during the tick between a drop and the next poll, so it is
			// dropped without a count.
			return;
		}

		const engine::ecs::Entity player = found->second.Instance;

		// **The write itself is `game`'s**, because the studio applies the same
		// move from a `PlayLink` with no socket in the middle - two copies of
		// "which field does a move touch" is the shape that drifts, and drifts
		// first in the editor.
		Worlds().Enter(PrimaryWorld, [player, &move](engine::ecs::Store &store) {
			(void)engine::game::ApplyMoveInput(store, player, move);
		});
	}

	void Server::ApplyInputs() {
		using engine::replication::Rewind;

		// **The one place this engine is server-authoritative about something a
		// client did.** A client sends where it aimed, never what it hit: a
		// client that decided what it hit would be a client nothing downstream
		// can second-guess, and no amount of validation afterwards recovers
		// from that.
		for (const auto &submission : Replication->Inputs()) {
			const float latency = Replication->RoundTripMilliseconds(submission.Client);

			for (const engine::replication::Input &input : submission.Inputs) {
				// **Movement first, because it is the tagged one.** A shot is
				// seven untagged floats, so the order has to be "try the message
				// that identifies itself, then the one that does not" - the
				// reverse would eventually read a move as a shot at whatever
				// three of its bytes happened to spell.
				engine::game::MoveInput move;
				if (engine::game::DecodeMoveInput(input.Bytes, move)) {
					ApplyMove(submission.Client, move);
					continue;
				}

				engine::examples::Shot shot;
				if (!engine::examples::DecodeShot(input.Bytes, shot)) {
					// A client sending something this is not is either running
					// a different game or probing. Counted and dropped; there
					// is nothing to answer.
					Dropped++;
					continue;
				}

				// **Rewound to what that client was looking at**, which is its
				// input's tick less the interpolation delay it renders behind
				// and the half round trip the snapshot took to reach it.
				double seen = Rewind::TickSeenBy(
					input.Tick,
					engine::replication::InterpolationSettings{}.DelayTicks,
					latency,
					Settings.TickRate
				);

				// **A tick outside the window is resolved against the present,
				// and the case that forces it is a client standing still.** A
				// client stamps its input with the newest tick it has applied,
				// and a tick only reaches it when something *changed* - so in a
				// quiet world its idea of the server's clock stops advancing
				// while the server's does not. Left alone, `Each` is asked for a
				// tick that fell out of the ring, answers nothing, and every
				// shot in a still scene misses with no error anywhere.
				//
				// **The present rather than the oldest frame held**, which is
				// the answer that follows from *why* a tick goes stale: it goes
				// stale because nothing has been changing, and a world that has
				// not changed looks the same now as it did then. Falling back to
				// the oldest frame would rewind half a second for a client that
				// is not behind at all.
				//
				// **And it cannot be gamed**, which is the other half of
				// choosing this direction. Rewinding is the favourable answer
				// for a laggy shooter, so a client that wanted more of it would
				// claim an older tick - and claiming one this server no longer
				// remembers buys the *least* favourable resolution there is,
				// not the most. A tick inside the window is honoured exactly as
				// before; `RewindSettings::HistoryTicks` is the bound, and that
				// header already calls the depth a fairness decision.
				if (History.Depth() == 0) {
					continue;
				}
				if (seen < static_cast<double>(History.Oldest()) ||
					seen > static_cast<double>(History.Newest())) {
					seen = static_cast<double>(History.Newest());
				}

				// The candidates are the *history's*, not the world's, and that
				// is the whole reason a hit test uses one: an entity destroyed
				// between the tick the client saw and now is still a legitimate
				// thing to have shot at, and the world no longer has it.
				Candidates.clear();
				History.Each(seen, [this](engine::ecs::Entity entity, const engine::core::Vector3 &at) {
					engine::examples::Target target;
					target.Entity = entity;
					target.At = at;
					Candidates.push_back(target);
				});

				const engine::examples::Hit hit = engine::examples::NearestHit(shot, Candidates);
				if (!hit.Struck) {
					continue;
				}

				// Two effects, and they answer different questions: the recolour
				// says *something* was hit and the damage says *somebody* was.
				// `scene.Visual` and `scene.Humanoid` both replicate, so every
				// client sees the server's verdict rather than the shooter's.
				//
				// **This is the consequence `docs/retired/DEFERRED.md` D00121 was
				// waiting for.** A hit test with no consequence did not need "dead" to
				// mean anything; the moment one subtracts something it does, and
				// `scene::TakeDamage` is the door - including its refusal to run
				// in a replica, which is why a client running this same code
				// against its own copy could not kill anybody.
				Worlds().Enter(PrimaryWorld, [&hit](engine::ecs::Store &store) {
					if (auto *visual = store.GetMutable<engine::scene::Visual>(hit.Entity)) {
						visual->Tint = engine::core::Color3{1.0f, 0.2f, 0.1f};
					}

					// **The rewind history holds parts, and a `Character` sits on
					// the model above one.** A limb is intangible and never
					// enters the history, so what can be struck on a person is
					// the root - whose parent is the model. Anything else struck
					// is scenery and has no humanoid to lose.
					const auto *rig = store.Get<engine::scene::Character>(store.ParentOf(hit.Entity));
					if (rig == nullptr) {
						return;
					}

					if (engine::scene::TakeDamage(store, rig->Humanoid, SHOT_DAMAGE)) {
						ENGINE_INFO("server: a shot killed a character; the respawn clock has started");
					}
				});
				Struck++;
			}
		}
	}

	void Server::UpdateClientViewpoints() {
		if (Players.empty()) {
			return;
		}

		// **One world entry for every client rather than one each.** Entering a
		// world takes it - `PositionOf` pays that cost per candidate and says
		// so - and this runs every tick for every connection, where the walk
		// inside is a component read per player.
		Worlds().Enter(PrimaryWorld, [this](engine::ecs::Store &store) {
			for (const auto &[slot, occupant] : Players) {
				// **The character's root and not the `Player` instance.** A
				// player is a row in a service and has no position at all; what
				// a client is looking *from* is the body it was given, which is
				// what `scene::CharacterOf` answers and what moves.
				const engine::ecs::Entity character = engine::scene::CharacterOf(store, occupant.Instance);
				if (character == engine::ecs::NULL_ENTITY) {
					// Between a join and a spawn, and between a death and a
					// respawn. A client with no entry scores everything the
					// same, which is the round robin and the honest answer -
					// rather than sorting its world by wherever its corpse fell.
					ForgetClientViewpoint(engine::replication::ClientId{slot, occupant.Generation});
					continue;
				}

				const auto *placement = store.Get<engine::scene::Transform>(character);
				if (placement == nullptr) {
					continue;
				}

				// **The root's origin and not the eye.** Where a client's
				// *camera* is depends on its zoom and its pitch, and neither
				// crosses the wire - `CameraController` is a resource on
				// whichever host is looking. The body is what the server knows,
				// it is within a couple of metres of the eye in every camera
				// mode, and the score it feeds is a falloff over tens of metres.
				SetClientViewpoint(
					engine::replication::ClientId{slot, occupant.Generation}, placement->Frame.Position
				);
			}
		});
	}

	void Server::SetClientViewpoint(engine::replication::ClientId client, const engine::core::Vector3 &at) {
		if (!client.IsValid()) {
			return;
		}
		Viewpoints[client.Index] = Viewpoint{at, client.Generation};
	}

	void Server::ForgetClientViewpoint(engine::replication::ClientId client) {
		const auto found = Viewpoints.find(client.Index);
		if (found != Viewpoints.end() && found->second.Generation == client.Generation) {
			Viewpoints.erase(found);
		}
	}

	engine::net::Endpoint Server::ListeningOn() const {
		return Socket == nullptr ? engine::net::Endpoint{} : Socket->Local();
	}

	bool Server::BeginAnnouncing() {
		if (!Settings.Advertise && Settings.RendezvousAddress.empty()) {
			return true;
		}
		if (Socket == nullptr) {
			// Nothing to announce. A server that broadcast a port it never
			// bound would send every client somewhere nothing is listening,
			// which reads from the other end as a firewall problem.
			ENGINE_WARN("--advertise and --rendezvous need --listen; nothing is being announced.");
			return true;
		}

		std::optional<network::SessionKey> key;
		if (!Settings.SessionSecret.empty()) {
			// Hex when it is exactly that, words otherwise. A person who was
			// given words types words, and a launcher that generated a key
			// passes the key.
			key = network::SessionKey::FromText(Settings.SessionSecret);
			if (!key) {
				key = network::SessionKey::FromPassphrase(Settings.SessionSecret);
			}
			if (!key) {
				ENGINE_ERROR("--session-key is neither 64 hex characters nor a passphrase.");
				return false;
			}
		}

		Announcement.Session = network::SessionId::Draw();
		if (!Announcement.Session.IsValid()) {
			ENGINE_ERROR("no entropy for a session id, so this server cannot be announced.");
			return false;
		}
		Announcement.Use = network::Purpose::Game;
		Announcement.Admits = key ? network::Access::Private : network::Access::Public;
		Announcement.Protocol = engine::replication::PROTOCOL_VERSION;
		// The port that was bound rather than the one that was asked for:
		// `--listen 0` binds an ephemeral one. The address is left as the
		// socket has it - usually the wildcard - because the browser at the
		// other end resolves that against where the datagram came from, which
		// is the one route known to work. `network::Listing::Dial`.
		Announcement.At = Socket->Local();
		Announcement.Name =
			Settings.SessionName.empty() ? std::string("atomic server") : Settings.SessionName;
		Announcement.Detail = Settings.GamePath.empty()
								  ? std::string()
								  : std::filesystem::path(Settings.GamePath).stem().string();
		Announcement.PeerLimit =
			static_cast<uint16_t>(engine::replication::ListenerSettings{}.MaximumClients);

		network::PresenceSettings presence;
		presence.Announce = Settings.Advertise;
		// A dedicated server does not browse. It has nothing to look for and a
		// listener on the well-known port would be a port it did not need to
		// take.
		presence.Discover = false;
		presence.RendezvousAddress = Settings.RendezvousAddress;
		presence.Protocol = engine::replication::PROTOCOL_VERSION;
		presence.Use = network::Purpose::Game;

		// **The listening socket, not a socket of its own.** A router's NAT
		// mapping belongs to a port: a hole punched on some other socket leaves
		// this one exactly as unreachable as it was, so the rendezvous traffic
		// has to travel over the port clients will connect to. The listener
		// drains that socket and hands anything foreign back here.
		Discovery = network::Presence::Open(presence, Announcement, std::move(key), Socket.get());
		if (Replication != nullptr) {
			Replication->SetForeign(
				[this](std::span<const std::byte> datagram, const engine::net::Endpoint &from) {
					return Discovery != nullptr && Discovery->Deliver(datagram, from, DiscoveryNow);
				}
			);
		}
		if (Discovery->Fault() != network::PresenceFault::None) {
			// Recorded rather than fatal. A machine with no route to the subnet
			// broadcast address still wants its rendezvous registration, and a
			// server that refused to start over discovery would be refusing
			// over a feature nobody asked to be essential.
			ENGINE_WARN("discovery: {}", network::Describe(Discovery->Fault()));
		}
		if (Discovery->Announcing()) {
			ENGINE_INFO(
				"announcing session {} ({}) on the local subnet",
				Announcement.Session.Text(),
				network::Describe(Announcement.Admits)
			);
		}
		if (Discovery->Rendezvousing()) {
			ENGINE_INFO(
				"registering session {} with {}", Announcement.Session.Text(), Settings.RendezvousAddress
			);
		}
		return true;
	}

	void Server::ServeDiscovery(double nowSeconds) {
		if (Discovery == nullptr) {
			return;
		}

		// The clock the foreign-datagram handler reads. It is set here, before
		// `Listener::Poll` runs in the same tick, so a rendezvous message
		// routed out of that drain is stamped with this tick's time rather than
		// the previous one's.
		DiscoveryNow = nowSeconds;

		// The count changes as people join and leave, and it is the one field
		// somebody reads before deciding to click. Set every tick and sent on
		// the beacon's own interval, so a server whose count churns does not
		// broadcast every tick.
		const auto peers = static_cast<uint16_t>(Replication == nullptr ? 0 : Replication->Count());
		if (peers != Announcement.Peers) {
			Announcement.Peers = peers;
			Discovery->SetAdvert(Announcement);
		}
		Discovery->Pump(nowSeconds);
	}

	void Server::UpdateWorldLifecycle(double nowSeconds) {
		// **Off is the default and the check is first**, so a server nobody
		// asked for this costs one comparison a tick and behaves exactly as it
		// did - which is what keeps `just determinism` and `just replay-check`
		// comparing the program they were written against.
		if (Settings.IdleCloseSeconds <= 0.0) {
			return;
		}

		for (const engine::world::WorldId world : Worlds().Worlds()) {
			// A world somebody else hosts is somebody else's to suspend.
			if (Worlds().IsRemote(world)) {
				continue;
			}

			engine::world::LifecycleInputs inputs;
			inputs.State = Worlds().StateOf(world);
			inputs.IdleLimit = Settings.IdleCloseSeconds;
			inputs.LastWorld = Worlds().Count() <= 1;

			// **Occupancy is a player standing in it, and players live in the
			// primary world.** That is the whole of what a headless server can
			// mean by "somebody is using this" - the studio's other two answers,
			// the active scene and a viewport showing it, are questions only an
			// editor can ask.
			inputs.Occupied = world == PrimaryWorld && !Players.empty();

			// **And whatever the game says is still happening.** A host can see
			// players and nothing else, so a world of NPCs on a route looks
			// abandoned from here - `scene::AwakeWorld` is the game's answer and
			// this is the only place a server can ask it. See `scene/Awake.hpp`.
			if (!inputs.Occupied) {
				engine::core::Name reason;
				bool held = false;
				Worlds().Enter(world, [&held, &reason](engine::ecs::Store &store) {
					held = engine::scene::WorldIsHeldAwake(store, &reason);
				});
				if (held) {
					inputs.Occupied = true;

					// Once, on the tick the claim starts holding it up, rather
					// than every tick - a line a second per world is a log
					// nobody reads and a claim is usually long-lived.
					if (inputs.State == engine::world::WorldState::Active && !WasHeldAwake(world)) {
						ENGINE_INFO(
							"server: '{}' is empty but held awake by '{}'",
							Worlds().NameOf(world).Text(),
							reason.Text()
						);
					}
				}
				SetHeldAwake(world, held);
			} else {
				SetHeldAwake(world, false);
			}

			// A suspended world's inbox is the one queue nothing drains, which
			// is what makes reading it reliable rather than a race with the
			// tick. Read only while suspended, for that reason.
			if (inputs.State == engine::world::WorldState::Suspended) {
				Worlds().Enter(world, [&inputs](engine::ecs::Store &store) {
					if (const auto *inbox = store.Resource<engine::world::Inbox>()) {
						inputs.InboxWaiting = !inbox->Arrived.empty();
					}
				});
			}

			// Only an active world has an idle clock and only it needs one.
			if (inputs.State == engine::world::WorldState::Active) {
				const auto found = std::find_if(Lives.begin(), Lives.end(), [world](const WorldLife &life) {
					return life.World == world;
				});

				if (found == Lives.end()) {
					// First seen. Its clock starts now rather than at zero, so a
					// world created mid-run is not immediately eligible.
					Lives.push_back(WorldLife{world, nowSeconds});
					continue;
				}
				inputs.IdleSeconds = nowSeconds - found->LastOccupied;
			}

			const engine::world::LifecycleAction action = engine::world::DecideLifecycle(inputs);

			if (action == engine::world::LifecycleAction::Resume) {
				Worlds().SetState(world, engine::world::WorldState::Active);
				TouchWorld(world, nowSeconds);
				ENGINE_INFO("server: resumed '{}' - something arrived for it", Worlds().NameOf(world).Text());
				continue;
			}

			if (action == engine::world::LifecycleAction::Leave) {
				// An occupied world's clock is restarted rather than merely not
				// read, which is what makes `IdleSeconds` mean "since the last
				// player left" on the next pass instead of "since it was made".
				if (inputs.Occupied) {
					TouchWorld(world, nowSeconds);
				}
				continue;
			}

			Worlds().SetState(world, engine::world::WorldState::Suspended);
			ENGINE_INFO(
				"server: suspended '{}' - empty for {:.4g}s",
				Worlds().NameOf(world).Text(),
				Settings.IdleCloseSeconds
			);
		}
	}

	bool Server::WasHeldAwake(engine::world::WorldId world) const {
		for (const WorldLife &life : Lives) {
			if (life.World == world) {
				return life.HeldAwake;
			}
		}
		return false;
	}

	void Server::SetHeldAwake(engine::world::WorldId world, bool held) {
		for (WorldLife &life : Lives) {
			if (life.World == world) {
				life.HeldAwake = held;
				return;
			}
		}
	}

	void Server::TouchWorld(engine::world::WorldId world, double nowSeconds) {
		for (WorldLife &life : Lives) {
			if (life.World == world) {
				life.LastOccupied = nowSeconds;
				return;
			}
		}
		Lives.push_back(WorldLife{world, nowSeconds});
	}

	void Server::ServeClients(double nowSeconds) {
		ENGINE_PROFILE_CAT("Server::ServeClients", engine::core::ProfileCategory::Network);

		ServeDiscovery(nowSeconds);

		if (Replication == nullptr) {
			return;
		}

		// Recorded before the poll, because `OnAdmitted` fires from inside it
		// and the join notice it sends has to be stamped with this tick's clock.
		PollNow = nowSeconds;

		// Inbound first, so an acknowledgement that arrived this tick is counted
		// before this tick's delta is built against it.
		Replication->Poll(nowSeconds);

		// **Where everybody is, before the delta is built against it.** The
		// score sorts a client's world by distance from its viewpoint, so a
		// viewpoint refreshed *after* the publish would order every tick by
		// where that client stood on the previous one - invisible while a
		// player stands still and exactly wrong while they run.
		UpdateClientViewpoints();

		// **Asked once per frame, and asking is what clears it.** A world with
		// no replication rate answers `true` after every tick, which is what
		// this program did before rates existed. A world at 20 Hz inside a 60 Hz
		// simulation answers `true` on one tick in three, and the world has been
		// holding its change bits across the other two so that the delta built
		// below covers all three - see `World::Tick`.
		const bool publishDue = Worlds().TakeReplicationTick(PrimaryWorld);

		Worlds().Enter(PrimaryWorld, [this, nowSeconds, publishDue](engine::ecs::Store &store) {
			{
				ENGINE_PROFILE_CAT("Server::NetworkLatency", engine::core::ProfileCategory::Network);
				const double global = Worlds().SettingsOf(PrimaryWorld).GlobalSimulatedNetworkLatency;
				for (const auto &[slot, occupant] : Players) {
					if (!store.Alive(occupant.Instance)) {
						continue;
					}
					const auto *network = store.Get<engine::scene::PlayerNetworkComponent>(occupant.Instance);
					const double local = network != nullptr ? network->LocalSimulatedNetworkLatency : 0.0;
					Replication->SetSimulatedLatency(
						engine::replication::ClientId{slot, occupant.Generation}, global + local
					);
				}
			}

			// **What the clients that own something said, applied before this
			// tick is published.** Applying after would publish a delta built
			// from last tick's value and send the owner's own state back to it
			// one tick stale, which is a client fighting its own echo.
			//
			// What gets through is `Authority::SetOwnership`'s business - see
			// the predicate in `BeginListening`. A world where nothing has been
			// handed over, which is every world this repository ships, sees an
			// empty loop.
			Replication->ApplyOwnedState(store);

			// **The rewind history, recorded beside the delta and from the same
			// state.** Both answer questions about this tick, so taking them at
			// two different moments is how a hit test starts disagreeing with
			// what was actually sent.
			//
			// Only where something *can* move, which keeps the static geometry
			// out of a ring whose rows would all carry the same answer.
			//
			// **`RigidBody` and not `Motion`, and the difference is a bug that
			// made a standing player unhittable.** This walked `Motion` until
			// v0.15, on the reading that a `Motion` is what makes a placement
			// worth remembering - and `physics` takes a row's `Motion` *away*
			// when it puts the body to sleep, deliberately, so that the solver's
			// query never visits a resting row. `physics/AGENTS.md` carries that
			// decision and `physics/tests/Solver.cpp` states it in one line.
			//
			// So the history held whatever happened to be awake. A player
			// standing still is asleep within a second or so, which meant they
			// could not be shot - and nothing reported it, because a hit test
			// against an empty candidate list is an ordinary miss.
			//
			// `scene::Simulated` is the question actually being asked, and since
			// v0.18 it is asked positively: a part the world may move carries
			// the tag, so this records exactly those and skips the static
			// geometry the old predicate was aiming at. `Static` is skipped for
			// the same reason one layer in: it is a body that does not move.
			//
			// **A sleeping body keeps the tag**, which is the whole of the fix
			// above surviving the polarity change: it loses `scene::Motion` and
			// nothing else, so a player standing still is still in the rewind
			// history and can still be shot.
			//
			// **It was the absence of `RigidBody` until v0.15**, when that
			// component became the author's numbers rather than the world's
			// decision and every part started carrying one. Left as it was, this
			// would have recorded a rewind history for every wall in the map.
			{
				ENGINE_PROFILE_CAT("Server::Rewind", engine::core::ProfileCategory::Network);
				if (History.Begin(store.Time().Tick)) {
					store.Query<const engine::scene::Transform, const engine::scene::RigidBody>()
						.With<engine::scene::Simulated>()
						.Each([this](
								  engine::ecs::Entity entity,
								  const engine::scene::Transform &placement,
								  const engine::scene::RigidBody &body
							  ) {
							if (body.Kind == engine::scene::BodyKind::Static) {
								return;
							}
							History.Record(entity, placement.Frame.Position);
						});
				}
			}

			// Before `ClearChanges`, which the world does at the start of its
			// next tick - the bits are the delta source and reading them after
			// they are cleared is how a tick's worth of movement goes missing.
			//
			// The borrow is opened and closed around exactly this call, so the
			// priority callbacks read the store this walk is in rather than
			// entering the world again per candidate. See `Publishing`.
			//
			// **The visibility survey immediately before it, inside the same
			// entry**, because what it works out is only true of the world as it
			// stands now - and nothing between the two mutates one.
			//
			// **The survey and the publish are skipped together on a tick this
			// world does not publish.** What the survey works out is only true
			// of the world as it stands, so running it for a delta nobody is
			// building is its whole cost for nothing.
			if (!publishDue) {
				// **The wire is still pumped.** `Listener::Flush` exists for
				// exactly this caller: acknowledgements that never leave fill
				// the far side's reliable window, and a link that is working
				// perfectly then gives up. What waits for the next published
				// tick is the queued outgoing messages, which is what a
				// replication rate is asking for.
				Replication->Flush(nowSeconds);
				return;
			}

			SurveyVisibility(store);
			Publishing = &store;
			Replication->Publish(store, store.Time().Tick, nowSeconds);
			Publishing = nullptr;
		});

		// **After the world, and that ordering is the whole of "content must not
		// starve the simulation".** `Publish` has already spent what this tick's
		// delta needed out of each link's budget, so what a relayed chunk is
		// offered is whatever is left - and `SendTo` refusing is ordinary
		// backpressure, so the piece goes out on the next tick from where it
		// stopped rather than being lost.
		if (ContentLink != nullptr) {
			ENGINE_PROFILE_CAT("Server::ContentRelay", engine::core::ProfileCategory::Network);
			ContentLink->Pump([this](
								  engine::replication::ClientId client, std::span<const std::byte> payload
							  ) { return Replication->SendTo(client, payload, PollNow); });
		}

		{
			ENGINE_PROFILE_CAT("Server::ApplyInputs", engine::core::ProfileCategory::Simulation);
			ApplyInputs();
		}
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

		return engine::world::PlanHosts(remote, Settings.Listening ? 1u : Settings.WorldsPerHost).size();
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
			world.PhysicsTickRate = Settings.PhysicsTickRate;
			world.ReplicationTickRate = Settings.ReplicationTickRate;
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
			world.PhysicsTickRate = Settings.PhysicsTickRate;
			world.ReplicationTickRate = Settings.ReplicationTickRate;

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

		if (!BeginListening()) {
			return false;
		}

		engine::world::HostFrame ready;
		ready.Signal = engine::world::HostSignal::Ready;
		ready.Port = ListeningOn().Port;
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

		if (Discovery != nullptr) {
			// Best effort and unacknowledged: a process shutting down has
			// nothing to wait for, and the rendezvous point's expiry is what
			// makes a host that crashed indistinguishable from one that was
			// killed. This only makes the common case tidy.
			Discovery->Withdraw(0.0);
			Discovery.reset();
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

		// Every tick's cost, for the percentiles below. Four bytes a tick, so an
		// hour at sixty a second is under a megabyte and a run long enough to
		// matter is bounded by how long somebody left it running.
		std::vector<float> tickMilliseconds;
		size_t droppedSpans = 0;

		const auto ticksSoFar = [this] { return Worlds().StatisticsOf(PrimaryWorld).Ticks; };

		// Collection has to be on for there to be a tree to fold, and asking for
		// the profile is what says so - `--graph` is the other way in and the
		// two do not have to be given together.
		if (!Settings.ProfilePath.empty()) {
			engine::core::FrameGraph::SetEnabled(true);
			engine::core::FrameGraph::SetFoldingEnabled(true);
		}

		// From the first tick rather than from when somebody asks, because a
		// slope is only as good as the window under it and a host's first
		// minute is when it decides what it is going to hold.
		if (!Settings.HeapReport.empty()) {
			engine::core::HeapProfile::SetSamplingEnabled(true);
			if (!engine::core::HeapProfile::IsCompiledIn()) {
				ENGINE_WARN(
					"--heap-report was given and this build has no allocator hooks. Configure with "
					"MONO_HEAP_PROFILE=ON, the dev preset, or the -dev archive of this release."
				);
			}
		}

		if (Settings.ControlPort >= 0) {
			ControlSurface.AddUniverseTools(Worlds());
			if (ControlServer.Start(static_cast<uint16_t>(Settings.ControlPort))) {
				ENGINE_INFO(
					"control: listening on 127.0.0.1:{} - {} tools",
					ControlServer.Port(),
					ControlSurface.Count()
				);
			}
		}

		// Once, and here rather than in the constructor: the game file is
		// loaded and the socket is bound by now, and both are what the first
		// card says.
		StartDiscord();

		while (Running && !StopRequested.load()) {
			const uint64_t tickStarted = engine::core::Clock::Nanoseconds();

			engine::core::FrameGraph::BeginFrame();

			// **Between the frame's start and its work**, which is where the
			// editor pumps it too: a tool that writes a property is doing what a
			// hand on the mouse does, so it lands at the same point in the loop.
			if (ControlServer.IsRunning()) {
				ControlServer.Pump([this](const std::string &line) { return ControlSurface.Answer(line); });
			}

			// Beside the control surface and for its reason: telling Discord
			// how many people are on changes nothing a recorded run has to
			// reproduce, so it belongs where the frame is bookkept rather than
			// where the world is simulated.
			PumpDiscord(engine::core::Clock::Seconds());

			// Beside the control surface, and for the same reason it is here:
			// content delivery is not part of the tick. A fetch that completes
			// between two ticks changes nothing a recorded run would have to
			// reproduce - CDN.md §3 - so it is pumped where the frame is
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
			// such choice: `PreRender` is where deriving what to send lives -
			// the same shape as deriving what to draw - so it runs every tick,
			// with an alpha of zero because nothing here interpolates.
			Worlds().Present(PrimaryWorld, delta, 0.0f);

			// After presentation, because that is the phase this comment has
			// always said replication extraction belongs to, and before the next
			// tick clears the change bits it reads.
			//
			// A replay does not serve clients: a recording reproduces a run, and
			// a run that also streamed would depend on whether anybody was
			// connected - which is exactly the kind of input that stops a replay
			// being byte-identical.
			if (!Replayer_) {
				ServeClients(static_cast<double>(tickStarted) / 1e9);

				// **After serving, so a client that joined this tick counts as
				// occupancy before anything decides the world is empty.** Not on
				// the replay path for the same reason `ServeClients` is not: a
				// recording reproduces a run, and suspending against wall time
				// would make it depend on how long the replay took.
				UpdateWorldLifecycle(static_cast<double>(tickStarted) / 1e9);
			}

			engine::core::FrameGraph::EndFrame();
			ENGINE_PROFILE_FRAME();

			// **Counted, because a partial flamegraph must not look complete.**
			// The collector's per-frame budgets are `MAXIMUM_SPANS` and
			// `MAXIMUM_DEPTH`, and a host with hundreds of clients on it is
			// exactly the run that can reach the first. A capture that quietly
			// lost a subtree is a capture that reports the wrong hot path.
			droppedSpans += engine::core::FrameGraph::Dropped();

			const uint64_t tickEnded = engine::core::Clock::Nanoseconds();
			const auto spent = static_cast<double>(tickEnded - tickStarted) / 1e9;

			totalTickSeconds += spent;
			tickMilliseconds.push_back(static_cast<float>(spent * 1000.0));
			summary.SlowestTickMilliseconds =
				std::max(summary.SlowestTickMilliseconds, static_cast<float>(spent * 1000.0));

			// From the world, which counted the tick it just ran. The loop does
			// not keep its own tally.
			const uint64_t ticks = ticksSoFar();

			// **A cumulative sample, not a second collector.** `WriteFolded` reads
			// the running total without resetting it, so this is the same call the
			// shutdown block below makes, just made early and repeatedly. Two
			// samples N ticks apart subtract into that window's folded stacks -
			// `scripts/flamegraph.py --average` does the subtracting - so nothing
			// here has to track a window's spans on its own.
			if (Settings.ProfileWindowTicks > 0 && !Settings.ProfilePath.empty() &&
				ticks % Settings.ProfileWindowTicks == 0) {
				const std::filesystem::path window =
					Settings.ProfilePath.parent_path() /
					(Settings.ProfilePath.stem().string() + ".window" + std::to_string(ticks) +
					 Settings.ProfilePath.extension().string());
				engine::core::FrameGraph::WriteFolded(window);
			}

			// After the tick, so a reading covers whole ticks. Sampling inside
			// one would catch every scratch buffer the simulation holds for the
			// length of a tick and report the sawtooth as the shape of the heap.
			engine::core::HeapProfile::SampleIfDue();

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

		// Nearest-rank on a sorted copy, for `FrameGraph`'s reason: with
		// thousands of readings an interpolated percentile reports a tick that
		// never happened.
		if (!tickMilliseconds.empty()) {
			std::sort(tickMilliseconds.begin(), tickMilliseconds.end());
			const auto at = [&tickMilliseconds](double fraction) {
				const auto rank =
					static_cast<size_t>(fraction * static_cast<double>(tickMilliseconds.size() - 1) + 0.5);
				return tickMilliseconds[std::min(rank, tickMilliseconds.size() - 1)];
			};
			summary.TickP50Milliseconds = at(0.50);
			summary.TickP95Milliseconds = at(0.95);
			summary.TickP99Milliseconds = at(0.99);
		}

		ENGINE_INFO(
			"{} tick(s) over {:.2f}s · mean {:.3f} ms · slowest {:.3f} ms · {} overrun(s)",
			summary.Ticks,
			summary.Seconds,
			summary.MeanTickMilliseconds,
			summary.SlowestTickMilliseconds,
			summary.Overruns
		);

		// **A separate line, and machine-readable on purpose.** `just stress`
		// scrapes it: the harness measures what a client saw and cannot see a
		// server's own tick cost, and the alternative was a second control
		// channel for a number this process already has.
		ENGINE_INFO(
			"tick ms p50 {:.3f} · p95 {:.3f} · p99 {:.3f}",
			summary.TickP50Milliseconds,
			summary.TickP95Milliseconds,
			summary.TickP99Milliseconds
		);

		// **Two counters that nothing read until now.** `Struck` and `Dropped`
		// have been incremented since v0.9 and printed nowhere, which made them
		// exactly the kind of number this file's own comment warns about: the
		// reason they are two rather than one is that a payload which did not
		// decode is a client running a different game or probing, and burying
		// that in a hit count is how an unusual number stops being noticed.
		// Neither could be noticed at all while neither was reported.
		//
		// Said only when something happened, so a run with no clients in it
		// does not print a line of zeroes.
		if (Struck > 0 || Dropped > 0) {
			ENGINE_INFO("server: {} shot(s) struck, {} input(s) did not decode", Struck, Dropped);
		}

		// **The relay's own counters, and they are separate on purpose.** A
		// client asking too fast, a route nothing could produce and a link that
		// was full are three incidents with three different fixes, and one
		// number for them would bury whichever mattered. Said only when
		// something happened, for the line above's reason.
		if (const ContentRelayStatistics *const relayed = ContentRelayStats();
			relayed != nullptr && relayed->Requests > 0) {
			ENGINE_INFO(
				"server: content relay served {} route(s) in {} bytes · {} refused · {} dropped for rate · "
				"{} client(s) flagged · {} chunk(s) deferred",
				relayed->Served,
				relayed->SentBytes,
				relayed->Refused,
				relayed->Dropped,
				relayed->Flagged,
				relayed->Deferred
			);
		}

		if (!Settings.HeapReport.empty()) {
			const engine::core::HeapTotals heap = engine::core::HeapProfile::Totals();
			ENGINE_INFO(
				"heap: {:.1f} MiB live in {} block(s), {:.1f} MiB peak, {} tag(s)",
				static_cast<double>(heap.LiveBytes) / (1024.0 * 1024.0),
				heap.LiveBlocks,
				static_cast<double>(heap.PeakBytes) / (1024.0 * 1024.0),
				heap.Nodes
			);
			if (engine::core::HeapProfile::WriteReport(Settings.HeapReport)) {
				ENGINE_INFO("heap: report written to '{}'", Settings.HeapReport.string());
			} else {
				ENGINE_ERROR("heap: nothing to write to '{}'", Settings.HeapReport.string());
			}
		}

		if (!Settings.ProfilePath.empty()) {
			engine::core::FrameGraph::SetFoldingEnabled(false);
			const size_t folded = engine::core::FrameGraph::FoldedFrames();
			if (engine::core::FrameGraph::WriteFolded(Settings.ProfilePath)) {
				ENGINE_INFO("profile: {} frame(s) folded into {}", folded, Settings.ProfilePath.string());
				if (droppedSpans > 0) {
					ENGINE_WARN(
						"profile: {} scope(s) were dropped, so the capture is incomplete - see "
						"FrameGraph::MAXIMUM_SPANS",
						droppedSpans
					);
				}
			} else {
				ENGINE_ERROR("profile: nothing to write to '{}'", Settings.ProfilePath.string());
			}
		}

		return summary;
	}
}
