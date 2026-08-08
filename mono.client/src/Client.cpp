#include <engine/assets/Manifest.hpp>
#include <engine/assets/Material.hpp>
#include <engine/audio/Wav.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Paths.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/effects/Ribbon.hpp>
#include <engine/game/Game.hpp>
#include <engine/gui/Layout.hpp>
#include <engine/input/Translate.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/parallel/Process.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Input.hpp>
#include <engine/scene/Materials.hpp>
#include <engine/scene/MeshCatalogue.hpp>
#include <engine/scene/PublishedCatalogue.hpp>
#include <engine/scene/TextureCatalogue.hpp>
#include <engine/world/Postbox.hpp>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <chrono>
#include <client/Client.hpp>
#include <client/ContentDemand.hpp>
#include <client/Replicated.hpp>
#include <fstream>
#include <thread>

namespace client {

	using engine::core::FrameGraph;
	using engine::core::Metrics;
	using engine::input::Action;
	using engine::render::ProfilerTab;

	Client::~Client() {
		Shutdown();
	}

	bool Client::Initialise(const Options &options) {
		Settings = options;

		// Before anything reads a file. Changing it later would leave whatever
		// had already loaded pointing at the old tree.
		if (!Settings.AssetsDirectory.empty()) {
			engine::core::Paths::SetAssetsOverride(Settings.AssetsDirectory);
			ENGINE_INFO("assets from {}", Settings.AssetsDirectory.string());
		}

		if (!Settings.ScriptPath.empty()) {
			ENGINE_INFO("scene from {}", Settings.ScriptPath);
		}

		if (!SDL_Init(SDL_INIT_VIDEO)) {
			ENGINE_ERROR("SDL_Init: {}", SDL_GetError());
			return false;
		}

		Window = SDL_CreateWindow(
			"atomic", Settings.Width, Settings.Height, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY
		);
		if (!Window) {
			ENGINE_ERROR("SDL_CreateWindow: {}", SDL_GetError());
			return false;
		}

		if (!Renderer.Initialise(Window)) {
			return false;
		}

		// **The interface pass, built against the renderer's own device.** A
		// failure here is not fatal and must not be: a client that refused to
		// start because a shader or a typeface was missing would be worse than
		// one that draws a world with no interface over it, and the log says
		// which happened.
		{
			const engine::render::BackendHandles backend = Renderer.Backend();
			if (!Interface.Initialise(backend.Device, backend.ColourFormat)) {
				ENGINE_WARN("no interface pass; a ScreenGui will not be drawn");
			}
		}

		if (Settings.Uncapped && !Renderer.SetVerticalSync(false)) {
			ENGINE_WARN("--uncapped had no effect; frames stay paced by the display");
		}

		engine::parallel::Jobs::Start(engine::parallel::WorkersPerHost(1));

		ENGINE_INFO("simulation at {:.0f} Hz, rendering unlocked from it", Settings.TickRate);

		Universe_ = std::make_unique<engine::world::Universe>();

		if (!Settings.GameFile.empty()) {
			if (!LoadGameFile()) {
				return false;
			}
		} else if (!BuildDemoWorlds()) {
			return false;
		}

		// The first is the one the panels report on and the one the composed
		// camera comes from. A client draws one world's worth of camera however
		// many it composites.
		Rendered = Simulated.front();

		if (!BeginConnecting()) {
			return false;
		}

		if (Simulated.size() > 1) {
			ENGINE_INFO("compositing {} worlds, {:.0f} units apart", Simulated.size(), Settings.ViewSpacing);
		}

		FrameGraph::SetEnabled(Settings.ShowFrameGraph);
		return FinishStartup();
	}

	bool Client::LoadGameFile() {
		engine::game::GameInfo info;
		std::string error;

		if (!engine::game::LoadGame(*Universe_, Settings.GameFile, info, error)) {
			ENGINE_ERROR("--game '{}' failed: {}", Settings.GameFile.string(), error);
			return false;
		}

		const auto worlds = Universe_->Worlds();
		if (worlds.empty()) {
			ENGINE_ERROR("--game '{}' holds no worlds", Settings.GameFile.string());
			return false;
		}

		// **Both halves, in one process.** A single-player run is a server and
		// a client at once — `HostRole::OfBoth` says so and `RunService.hpp`
		// argues that both being true is a legal answer rather than a bug — so
		// a game's `Script` and its `LocalScript` both run here.
		engine::script::RuntimeLimits limits;
		limits.Role = engine::script::HostRole::OfBoth();

		for (const engine::world::WorldId id : worlds) {
			std::string failure;

			Universe_->Enter(id, [&](engine::ecs::Store &store, engine::ecs::Scheduler &systems) {
				InstallPresentation(store, systems, Settings.Entities);

				// The scripts before the camera, so a scene that aimed one of
				// its own keeps it — see `InstallDefaultCamera`.
				Runtimes.push_back(engine::game::StartWorldScripts(store, systems, limits, failure));
				InstallDefaultCamera(store, systems);
			});

			if (!failure.empty()) {
				ENGINE_ERROR("world '{}': {}", Universe_->NameOf(id).Text(), failure);
			}

			Views.Track(id, Universe_->NameOf(id), Settings.Entities);
			Simulated.push_back(id);
		}

		ENGINE_INFO(
			"playing '{}' — {} world(s)", info.Name.IsValid() ? info.Name.Text() : "game", worlds.size()
		);
		return true;
	}

	bool Client::BuildDemoWorlds() {
		const uint32_t worlds = std::max(1u, Settings.Worlds);
		for (uint32_t index = 0; index < worlds; index++) {
			engine::world::WorldSettings world;

			// Named rather than numbered, because a name is what a bus
			// envelope, a snapshot and a view header all carry. The first keeps
			// the name it has always had, so nothing that referred to it has to
			// learn a new one.
			world.Name = engine::core::Name(
				index == 0 ? std::string("client.world") : "client.world." + std::to_string(index)
			);
			world.TickRate = Settings.TickRate;

			const engine::world::WorldId id = Universe_->Create(world);
			if (!id.IsValid()) {
				ENGINE_ERROR("could not create the world to render");
				return false;
			}

			// The scripted path is the only demo-world implementation.
			const std::string scenePath = Settings.ScriptPath.empty()
											  ? engine::examples::ExamplePath("Rings.luau")
											  : Settings.ScriptPath;

			bool scripted = true;
			Universe_->Enter(
				id,
				[this, &scripted, &scenePath](engine::ecs::Store &store, engine::ecs::Scheduler &systems) {
					// Do not present a partially built world.
					scripted = BuildScriptedWorld(store, systems, scenePath, Settings.Entities);
				}
			);

			if (!scripted) {
				ENGINE_ERROR("the scene script failed, so there is nothing to render");
				return false;
			}

			// Size once so publishing does not allocate per frame.
			Views.Track(id, world.Name, Settings.Entities);
			Simulated.push_back(id);
		}

		return true;
	}

	bool Client::FinishStartup() {
		// Tracy is on-demand: it collects nothing until a profiler attaches, so
		// a short run with nothing listening produces an empty capture. Waiting
		// makes that an explicit choice rather than a surprise.
		if (Settings.ProfilerWaitSeconds > 0.0 && !ENGINE_PROFILE_ATTACHED()) {
			ENGINE_INFO("waiting up to {:.1f}s for a Tracy profiler", Settings.ProfilerWaitSeconds);

			const uint64_t started = engine::core::Clock::Nanoseconds();
			while (!ENGINE_PROFILE_ATTACHED()) {
				const double waited = static_cast<double>(engine::core::Clock::Nanoseconds() - started) / 1e9;
				if (waited >= Settings.ProfilerWaitSeconds) {
					ENGINE_INFO("no profiler attached; continuing without one");
					break;
				}
				SDL_Delay(50);
			}
			if (ENGINE_PROFILE_ATTACHED()) {
				ENGINE_INFO("profiler attached");
			}
		}

		if (!BeginContentDelivery()) {
			return false;
		}

		if (!BeginAudio()) {
			return false;
		}

		Running = true;
		return true;
	}

	bool Client::BeginContentDelivery() {
		if (Settings.ContentSources.empty() && Settings.ContentPublisherKey.empty()) {
			// Nothing was asked for. Not a failure — a game with its content
			// beside it in a game file needs no origin at all.
			return true;
		}

		engine::delivery::DeliverySettings settings =
			engine::delivery::DeliverySettings::Default(Settings.ContentCache);
		settings.CachePath = Settings.ContentCache;

		if (!Settings.ContentSources.empty()) {
			settings.Sources.clear();
			for (const std::string &source : Settings.ContentSources) {
				// `dir:` names a published store on this machine; anything else
				// is an address. One flag rather than two, because the priority
				// order is a single list and splitting it across two flags
				// would make "local first, then remote" unexpressible.
				const bool directory = source.starts_with("dir:");
				settings.Sources.push_back(
					engine::delivery::Source{
						.Name = source,
						.Kind = directory ? engine::delivery::SourceKind::Directory
										  : engine::delivery::SourceKind::Http,
						.Location = directory ? source.substr(4) : source,
						.Enabled = true,
					}
				);
			}
		}

		if (const auto key = engine::assets::PublicKey::FromHex(Settings.ContentPublisherKey)) {
			settings.Publisher = *key;
		} else {
			ENGINE_ERROR("content delivery needs --publisher-key, 64 hex characters");
			ENGINE_ERROR("a client that accepted an unsigned manifest would have no trust boundary");
			return false;
		}

		Content = engine::delivery::MakeAssetClient(settings);
		if (!Content) {
			return false;
		}

		ENGINE_INFO(
			"content: {} source(s), first is '{}'", settings.Usable().size(), settings.Usable().front().Name
		);
		return true;
	}

	void Client::PumpContent() {
		if (!Content) {
			return;
		}

		if (!ContentRequested && Content->Ready()) {
			ContentRequested = true;

			// **Nothing is requested by kind, which is what v0.10 ended.** The
			// unit that travels is a *bundle*, so asking for every mesh and
			// every material asks for essentially every bundle in the store —
			// and `AssetClient::Pump` resolves, verifies and decompresses all of
			// it synchronously, because the contract forbids a background
			// thread. On this repository's own store that was 6.9 GB through one
			// function on the frame the client started.
			//
			// `client/ContentDemand.hpp` carries both failures this replaces.
			ENGINE_INFO("content: catalogue ready — assets are fetched as the world names them");

			// **The manifest's mesh names, handed to the world once.** Names, not
			// content — a few hundred strings against the 6.9 GB above. It is the
			// only way a scene can find out what there is to name, because the
			// catalogue it can otherwise read holds what has already been asked
			// for. `scene/PublishedCatalogue.hpp` carries the whole argument, and
			// naming one of these is still what fetches it.
			OfferPublishedContent();
		}

		// **Every pump, and it is a diff rather than a walk of the catalogue.**
		// A world's content names change when a scene is authored, loaded or
		// replicated, none of which this can observe cheaply — so the names are
		// collected and everything already asked for is dropped. What survives is
		// almost always nothing.
		if (ContentRequested) {
			RequestWantedContent();
		}

		// Apply completions between frames, outside render passes.
		Content->Pump();

		size_t kept = 0;
		for (const engine::delivery::RequestId id : ContentPending) {
			const engine::delivery::RequestState state = Content->StateOf(id);
			if (state == engine::delivery::RequestState::Pending) {
				ContentPending[kept++] = id;
				continue;
			}

			std::optional<engine::delivery::Asset> asset = Content->Take(id);
			if (!asset) {
				// Failed, or already taken. Either way there is nothing more to
				// wait for; `delivery` has already counted it.
				continue;
			}

			// **The name is published as-is, extension included.** A
			// `SurfaceAppearance` naming `characters/skin.atex` and a manifest
			// carrying `characters/skin.atex` have to be the same string or the
			// lookup misses — and the one place that could diverge is here.
			const engine::core::Name name(asset->Name);
			engine::core::ByteReader reader(asset->Bytes);

			if (asset->Kind == engine::assets::AssetKind::Mesh) {
				engine::assets::MeshData mesh;
				if (!engine::assets::Mesh::Read(reader, mesh)) {
					ENGINE_WARN("content: {} is not a mesh this engine reads", asset->Name);
					continue;
				}
				// **A mesh's own sheets, asked for at the one point their names
				// are readable.** `Submesh::Texture` lives in the mesh file, so
				// `CollectWantedTextures` cannot see it — an imported model's
				// twenty sheets would otherwise never be fetched at all now that
				// textures are demand-driven.
				for (const engine::assets::Submesh &submesh : mesh.Submeshes) {
					if (!submesh.Texture.empty()) {
						RequestAsset(engine::core::Name(submesh.Texture));
					}
				}

				if (Renderer.AddMesh(name, mesh)) {
					ContentMeshes++;

					// Mesh metadata is world data, not renderer state.
					const auto triangles = static_cast<uint32_t>(mesh.Indices.size() / 3);
					for (const engine::world::WorldId id : Simulated) {
						Universe_->Enter(id, [&name, triangles](engine::ecs::Store &store) {
							engine::scene::RecordMesh(store, name, triangles);
						});
					}
					if (ReportedJoin) {
						Universe_->Enter(Replicated, [&name, triangles](engine::ecs::Store &store) {
							engine::scene::RecordMesh(store, name, triangles);
						});
					}
				}
			} else if (asset->Kind == engine::assets::AssetKind::Texture) {
				engine::assets::TextureData image;
				if (!engine::assets::Texture::Read(reader, image)) {
					ENGINE_WARN("content: {} is not a texture this engine reads", asset->Name);
					continue;
				}
				if (Renderer.AddTexture(name, image)) {
					ContentTextures++;
				}

				// **Flipbook layout is world data, not renderer state**, exactly
				// as the triangle count above is. A 4x4 animation sheet and a 4x4
				// tile atlas are the same pixels, so the grid, the frame count
				// and the authored rate are what tell an emitter how to play one
				// — and without them every scene using a GIF would have to state
				// numbers the file already holds. See `scene::TextureCatalogue`.
				//
				// Recorded whether or not the upload succeeded: a headless run
				// has no device and still knows what it read.
				if (image.IsFlipbook()) {
					const engine::scene::FlipbookFacts facts{
						.Side = image.FlipbookSide,
						.Frames = image.FlipbookFrames,
						.FrameRate = image.FlipbookFrameRate,
					};
					for (const engine::world::WorldId id : Simulated) {
						Universe_->Enter(id, [&name, &facts](engine::ecs::Store &store) {
							engine::scene::RecordTexture(store, name, facts);
						});
					}
					if (ReportedJoin) {
						Universe_->Enter(Replicated, [&name, &facts](engine::ecs::Store &store) {
							engine::scene::RecordTexture(store, name, facts);
						});
					}
				}
			} else if (asset->Kind == engine::assets::AssetKind::Material) {
				engine::assets::MaterialData material;
				if (!engine::assets::Material::Read(reader, material)) {
					ENGINE_WARN("content: {} is not a material this engine reads", asset->Name);
					continue;
				}

				// **World data, not renderer state**, exactly as the triangle
				// count and the flipbook layout above are — and for the sharper
				// version of the same reason: the renderer never sees a material
				// at all. `ResolveMaterials` turns one into a texture name on a
				// part, in `shared`, so a headless server resolves the same
				// materials the client does.
				const engine::core::Name colour(material.ColourMap);

				// **Deliberately not asked for here**, unlike a mesh's sheets, and
				// the asymmetry is the point. Every material in the catalogue
				// arrives whether anything uses it or not — 295 of them on this
				// store — so fetching each one's sheet on arrival is requesting
				// every texture by kind again, one indirection later, and it
				// refuses 160 of them exactly as before.
				//
				// A material reaches a *part* through `ResolveMaterials`, which
				// writes this name into that part's `SurfaceAppearance::ColourMap`
				// — and that is a field `CollectWantedTextures` already reads. So
				// the demand path needs no special case: the next pump asks for
				// the sheets of the materials something is actually made of.
				for (const engine::world::WorldId id : Simulated) {
					Universe_->Enter(id, [&name, &colour](engine::ecs::Store &store) {
						engine::scene::RecordMaterial(store, name, colour);
					});
				}
				if (ReportedJoin) {
					Universe_->Enter(Replicated, [&name, &colour](engine::ecs::Store &store) {
						engine::scene::RecordMaterial(store, name, colour);
					});
				}
				ContentMaterials++;
			} else if (asset->Kind == engine::assets::AssetKind::Audio) {
				// **Decoded and converted here, once.** The graph must never resample
				// on the device thread, and a buffer converted per voice would pay
				// for it again for every part playing a footstep. `DecodeAudio` picks
				// its decoder from the bytes rather than from the name — Sounds.hpp.
				std::optional<engine::audio::SampleBuffer> samples = DecodeAudio(asset->Bytes);
				if (!samples) {
					ENGINE_WARN("content: {} is not audio this engine decodes", asset->Name);
					continue;
				}

				// The device's format when there is one, and the graph's default when
				// there is not. A machine with no output still registers its sounds,
				// so a headless run exercises everything but the last hop.
				const engine::audio::AudioFormat target =
					Sound ? Sound->Format() : engine::audio::AudioFormat{};
				auto ready = std::make_shared<const engine::audio::SampleBuffer>(samples->ConvertTo(target));

				if (Audible.Add(name, ready)) {
					ContentSounds++;
					ENGINE_INFO(
						"content: {} decoded ({:.1f}s, {} Hz, {} channel(s))",
						asset->Name,
						ready->Seconds(),
						target.SampleRate,
						target.Channels
					);
				}
			}
		}
		ContentPending.resize(kept);

		// Appended after the walk, never during it. See `RequestTexture`.
		ContentPending.insert(ContentPending.end(), ContentIssued.begin(), ContentIssued.end());
		ContentIssued.clear();

		if (ContentRequested && ContentPending.empty() && !ContentReported) {
			ContentReported = true;
			ENGINE_INFO(
				"content: {} mesh(es), {} texture(s), {} material(s) and {} sound(s) registered",
				ContentMeshes,
				ContentTextures,
				ContentMaterials,
				ContentSounds
			);
		}
	}

	void Client::OfferPublishedContent() {
		const engine::assets::Manifest *catalogue = Content ? Content->Catalogue() : nullptr;
		if (catalogue == nullptr) {
			return;
		}

		// **Runtime-readable only.** A `.pmx` and a `.amesh` are both
		// `AssetKind::Mesh` and only the second is one this process decodes, so
		// offering both would hand a scene names it can set, fetch and then fail
		// to draw — a part on the fallback cube with a perfectly good string
		// behind it.
		std::vector<engine::core::Name> meshes;
		for (const engine::assets::AssetEntry *entry : catalogue->OfKind(engine::assets::AssetKind::Mesh)) {
			if (entry != nullptr && engine::assets::IsRuntimeReadable(entry->Name)) {
				meshes.emplace_back(entry->Name);
			}
		}

		// Every simulated world, and the replica when there is one — a scene is a
		// scene wherever it came from, and a list offered to only some of them
		// would be a service that answers differently depending on which world a
		// script happens to be in.
		for (const engine::world::WorldId id : Simulated) {
			Universe_->Enter(id, [&meshes](engine::ecs::Store &store) {
				(void)engine::scene::RecordPublishedMeshes(store, meshes);
			});
		}
		if (ReportedJoin) {
			Universe_->Enter(Replicated, [&meshes](engine::ecs::Store &store) {
				(void)engine::scene::RecordPublishedMeshes(store, meshes);
			});
		}

		ENGINE_INFO("content: {} published mesh(es) offered to the world", meshes.size());
	}

	void Client::RequestWantedContent() {
		std::vector<engine::core::Name> wanted;
		for (const engine::world::WorldId id : Simulated) {
			Universe_->Enter(id, [&wanted](engine::ecs::Store &store) {
				CollectWantedContent(store, wanted);
			});
		}
		if (ReportedJoin) {
			Universe_->Enter(Replicated, [&wanted](engine::ecs::Store &store) {
				CollectWantedContent(store, wanted);
			});
		}

		for (const engine::core::Name &name : wanted) {
			RequestAsset(name);
		}
	}

	void Client::RequestAsset(const engine::core::Name &texture) {
		// **Asked once and never again, whatever happened to it.** A name that
		// failed — not in the manifest, refused by the table — must not be
		// retried, or a scene naming one misspelled asset issues a request per
		// pump for the life of the process.
		if (!Content || !texture.IsValid() || !ContentAsked.insert(texture.Id()).second) {
			return;
		}

		// **Queued rather than appended, because this is called from inside the
		// walk over `ContentPending`.** A mesh names its own sheets and a
		// material names its colour map, and both are read while draining that
		// vector — pushing to it there is a range-for over a container being
		// grown, which is what it looks like: the walk lost its place and one
		// texture out of the several hundred asked for arrived.
		ContentIssued.push_back(Content->Request(texture.Text()));
	}

	void Client::PumpSounds() {
		// **Runs with or without a device.** A null device drains the queue like
		// a real one, so a machine with no output still exercises every hop but
		// the last — which is the property that keeps this path from only working
		// on the developer's laptop.
		if (Sound == nullptr) {
			return;
		}

		// The listener is the composed camera's position, which is **last
		// frame's**. One frame of latency on an ear is inaudible; reading a live
		// camera out of a store here would be reading something a world is
		// writing, which is the whole reason the compositor exists.
		const engine::core::Vector3 ear = Views.CameraFrame().Position;
		const uint32_t rate = Sound->Format().SampleRate;

		const auto sync = [&](engine::world::WorldId id) {
			if (!id.IsValid()) {
				return;
			}
			SoundStage &stage = Stages[id.Index];
			Universe_->Enter(id, [&](engine::ecs::Store &store) {
				stage.Sync(store, Sound->Mixer(), Audible, ear, rate);
			});
		};

		for (const engine::world::WorldId id : Simulated) {
			sync(id);
		}

		// The server's world too, so a `Sound` the authority replicated is heard
		// on the client that received it. Nothing here distinguishes the two —
		// a replicated `Sound` row and a locally created one are the same rows,
		// which is what makes "made by a LocalScript, heard by that player alone"
		// fall out of replication rather than out of an audio rule.
		if (ReportedJoin) {
			sync(Replicated);
		}
	}

	bool Client::BeginAudio() {
		// Validate before opening the device so headless runs report file errors.
		std::shared_ptr<const engine::audio::SampleBuffer> decoded;
		if (!Settings.SoundPath.empty()) {
			std::ifstream file(Settings.SoundPath, std::ios::binary | std::ios::ate);
			if (!file) {
				ENGINE_ERROR("audio: cannot read {}", Settings.SoundPath.string());
				return false;
			}
			const std::streamoff size = file.tellg();
			file.seekg(0);
			std::vector<std::byte> bytes(static_cast<size_t>(std::max<std::streamoff>(0, size)));
			if (!bytes.empty()) {
				file.read(reinterpret_cast<char *>(bytes.data()), size);
			}

			// **The same decoder the delivery path uses**, picked from the
			// bytes rather than from the extension. A second decode path here
			// would be a second opinion about what a file is, and the two would
			// disagree the day one of them learned a format.
			const auto samples = DecodeAudio(bytes);
			if (!samples) {
				ENGINE_ERROR("audio: {} is not audio this engine decodes", Settings.SoundPath.string());
				return false;
			}
			decoded = std::make_shared<const engine::audio::SampleBuffer>(*samples);
		}

		// Opened whether or not a sound was asked for: a world's own scripts
		// will want one, and a device that is only opened when a flag is passed
		// is a device nothing exercises.
		Sound = engine::audio::OpenDevice({});
		if (Sound == nullptr) {
			// Not a failure. A CI container has no sound server and a laptop
			// may have its output disabled; a game that refused to start
			// because it could not make a noise would be worse than a quiet
			// one.
			if (decoded) {
				ENGINE_WARN(
					"audio: '{}' decoded ({:.2f}s) but there is no output on this machine",
					Settings.SoundPath.string(),
					decoded->Seconds()
				);
			}
			return true;
		}
		if (!decoded) {
			return true;
		}

		// Convert once at load time, never on the device thread.
		auto &mixer = Sound->Mixer();
		const auto sample =
			std::make_shared<const engine::audio::SampleBuffer>(decoded->ConvertTo(Sound->Format()));

		// Keep a fader node for future master-volume control.
		const engine::audio::NodeId player = mixer.Commands().Allocate();
		const engine::audio::NodeId fader = mixer.Commands().Allocate();

		engine::audio::Command command;
		command.Kind = engine::audio::CommandKind::AddNode;
		command.Target = player;
		command.Node = engine::audio::NodeKind::Player;
		mixer.Commands().Post(command);

		command.Target = fader;
		command.Node = engine::audio::NodeKind::Fader;
		mixer.Commands().Post(command);

		command = {};
		command.Kind = engine::audio::CommandKind::Connect;
		command.Target = player;
		command.Second = fader;
		mixer.Commands().Post(command);

		command.Target = fader;
		command.Second = mixer.Graph().Output();
		mixer.Commands().Post(command);

		command = {};
		command.Kind = engine::audio::CommandKind::SetSound;
		command.Target = player;
		command.Sound = sample;
		mixer.Commands().Post(command);

		command = {};
		command.Kind = engine::audio::CommandKind::SetLooping;
		command.Target = player;
		command.Flag = true;
		mixer.Commands().Post(command);

		command = {};
		command.Kind = engine::audio::CommandKind::Play;
		command.Target = player;
		// Schedule against the sample clock for deterministic start timing.
		command.AtSample = mixer.Clock() + Sound->Format().SampleRate / 10;
		mixer.Commands().Post(command);

		ENGINE_INFO("audio: playing {} ({:.2f}s, looping)", Settings.SoundPath.string(), sample->Seconds());
		return true;
	}

	void Client::Shutdown() {
		// Stop dependants before renderer and SDL teardown.
		Sound.reset();
		Content.reset();

		Connection.reset();
		if (Socket != nullptr) {
			Socket->Close();
			Socket.reset();
		}

		if (Window) {
			Renderer.Shutdown();
			SDL_DestroyWindow(Window);
			Window = nullptr;
			SDL_Quit();
		}
		engine::parallel::Jobs::Stop();
	}

	bool Client::BeginConnecting() {
		if (Settings.ConnectAddress.empty()) {
			return true;
		}

		const std::optional<engine::net::Endpoint> server =
			engine::net::Endpoint::Parse(Settings.ConnectAddress);
		if (!server.has_value()) {
			ENGINE_ERROR("--connect '{}' is not a host:port", Settings.ConnectAddress);
			return false;
		}

		// Let the OS choose the client port so multiple clients can coexist.
		Socket = engine::net::MakeUdpTransport(0);
		if (Socket == nullptr) {
			ENGINE_ERROR("could not open a socket to connect from");
			return false;
		}

		engine::world::WorldSettings world;
		world.Name = engine::core::Name("client.replica");
		world.TickRate = Settings.TickRate;

		Replicated = Universe_->Create(world);
		if (!Replicated.IsValid()) {
			ENGINE_ERROR("could not create the replicated world");
			return false;
		}

		Universe_->Enter(Replicated, [this](engine::ecs::Store &store, engine::ecs::Scheduler &systems) {
			// The v0.2 refusal, used for what it was reserved for. A replica
			// that published to a bus would be telling the universe
			// something the server never said; the inbox still delivers,
			// which is how it receives.
			store.SetResource(engine::world::Replica{true});

			// A replicated world runs no *simulation* system: everything in
			// it arrived, and simulating it here would be this process
			// disagreeing with the authority once per tick. What this
			// installs is the `PreRender` half — the draw list and the
			// system that fills it — which derives what to draw and writes
			// no component.
			//
			// **The rate the snapshot buffer measures its delay against is the
			// server's, and nothing on the wire carries it.** What is passed is
			// the rate this process was told to run at, which is the same
			// default both programs take. A disagreement is absorbed by the
			// buffer's own correction up to a few percent, and past that shows
			// as `replica.stalls` rather than as something mysterious.
			engine::replication::InterpolationSettings interpolation;
			interpolation.TickRate = Settings.TickRate;

			BuildReplicatedWorld(store, systems, interpolation);
		});

		engine::replication::ConnectorSettings connector;
		if (!Settings.ServerKey.empty()) {
			connector.ServerIdentity = engine::assets::PublicKey::FromHex(Settings.ServerKey);
			if (!connector.ServerIdentity.has_value()) {
				ENGINE_ERROR("client: --server-key is not 64 hex characters");
				return false;
			}
		}

		Connection = std::make_unique<engine::replication::Connector>(
			*Socket, *server, engine::core::Clock::Seconds(), connector
		);

		ENGINE_INFO("connecting to {} from {}", server->Text(), Socket->Local().Text());
		return true;
	}

	void Client::PollServer(double nowSeconds) {
		if (Connection == nullptr) {
			return;
		}

		Universe_->Enter(Replicated, [this, nowSeconds](engine::ecs::Store &store) {
			Connection->Poll(store, nowSeconds);

			// **Here, not in the render pass.** This instant is the one where
			// the store holds the tick the server described; a pass that only
			// ran when a frame was drawn would miss a received tick whenever
			// the frame rate dipped below the tick rate, and the buffer would
			// then be interpolating across gaps the network never produced.
			RecordReplicatedTick(store, Connection->Applied());
		});

		// The exchange, before the world. A client that sat there with an empty
		// scene used to have one explanation; it now has two, and the log has to
		// say which — the handshake never finished, or it finished and the
		// snapshot has not arrived.
		if (!ReportedAdmission && (Connection->Admitted() || Connection->Rejected())) {
			ReportedAdmission = true;
			if (Connection->Admitted()) {
				ENGINE_INFO("admitted by {}, waiting for the world", Settings.ConnectAddress);
			} else {
				ENGINE_ERROR("the server at {} did not admit this client", Settings.ConnectAddress);
			}
		}

		// Once, on the tick it becomes true. A client that logged this every
		// frame would write six hundred lines a second saying the same thing.
		//
		// This is also where the replicated world starts being drawn, and the
		// reason it waits for the join is the channel's size: a view channel
		// allocates its slots once so that publishing never allocates, and the
		// only number this process has to size one with is what actually
		// arrived. Doubled, so a world that grows a little afterwards still
		// publishes; past that `Compositor::Publish` refuses and says so, which
		// beats a frame with holes in it.
		if (!ReportedJoin && Connection->Joined()) {
			ReportedJoin = true;

			size_t entities = 0;
			Universe_->Enter(Replicated, [&entities](engine::ecs::Store &store) {
				store.EachEntity([&entities](engine::ecs::Entity) { entities++; });
			});

			Views.Track(Replicated, engine::core::Name("client.replica"), entities * 2);

			ENGINE_INFO("joined: {} entities at tick {}", entities, Connection->Applied());
		}

		Connection->Advance(nowSeconds);
	}

	void Client::PumpEvents() {
		ENGINE_PROFILE("pump events");

		// Cleared before the pump, not after: an action fired during this
		// frame's events has to survive until something reads it.
		Actions.BeginFrame();

		// **The raw state beside the action layer, not instead of it.** `Actions`
		// maps a key to an *intent* the client acts on — F5 toggles the frame
		// graph — and this records what is *held*, which is what a game's own
		// scripts read. Two questions about one keyboard, and both want every
		// event.
		Input.BeginFrame();

		{
			// **The pump on its own.** `pump events` covered the poll and every
			// key it then acted on, and the poll is the half that can block on
			// the compositor — so a frame stalled by the window system and a
			// frame stalled by a keybinding doing work read as one number.
			// There is nothing worth a span inside `input` itself: `Actions` is
			// a bitset and a switch, and this is the cost of reaching it.
			ENGINE_PROFILE("poll events");

			SDL_Event event;
			while (SDL_PollEvent(&event)) {
				Actions.HandleEvent(event);

				// **Both, unconditionally, and neither consumes for the other.**
				// `Actions::HandleEvent` reports whether it took an event and that
				// answer is about the *client's* bindings — a script watching F5
				// should still see F5. Gating this on that return would make the
				// engine's own keybindings invisible to the game, which is a rule
				// nobody asked for.
				Input.HandleEvent(event);
			}
		}

		// **The pointer mode a script asked for, applied once it changes.**
		// `SDL_SetWindowRelativeMouseMode` is a system call and a window-manager
		// round trip on some platforms, so setting it every frame is a per-frame
		// cost to say what it already says. The compare is what makes it free.
		if (Window != nullptr && PointerMode != AppliedPointerMode) {
			const bool relative = PointerMode == engine::scene::MouseBehavior::LockCenter;
			SDL_SetWindowRelativeMouseMode(Window, relative);

			// `LockCurrentPosition` hides the pointer without warping it, which is
			// what a drag-to-rotate wants: the pointer is back where it started
			// when the drag ends. Relative mode would warp it to the centre.
			if (PointerMode == engine::scene::MouseBehavior::LockCurrentPosition) {
				SDL_HideCursor();
			} else if (!relative) {
				SDL_ShowCursor();
			}

			AppliedPointerMode = PointerMode;
		}

		if (Actions.Fired(Action::Quit)) {
			Running = false;
		}

		if (Actions.Fired(Action::ToggleStatistics)) {
			Settings.ShowStatistics = !Settings.ShowStatistics;
		}

		if (Actions.Fired(Action::ToggleNetwork)) {
			// **Refused rather than toggled when there is nothing to show.**
			// A client run without `--connect` has no link, and a network panel
			// full of zeroes reads as a link that is up and idle. Saying so once
			// beats a key that silently does nothing, which reads as a broken
			// binding.
			if (Connection == nullptr) {
				if (!ReportedNoNetwork) {
					ReportedNoNetwork = true;
					ENGINE_INFO("F4: no network panel — this client was not given --connect");
				}
			} else {
				Settings.ShowNetwork = !Settings.ShowNetwork;
			}
		}

		if (Actions.Fired(Action::ToggleFrameGraph)) {
			Settings.ShowFrameGraph = !Settings.ShowFrameGraph;
			// Collection is off until something asks for it, so opening the
			// panel is what turns it on.
			FrameGraph::SetEnabled(Settings.ShowFrameGraph);
			ProfilerScroll = 0;
		}

		const auto tabCount = static_cast<int>(ProfilerTab::Count);
		if (Actions.Fired(Action::NextProfilerTab)) {
			Settings.Tab = static_cast<ProfilerTab>((static_cast<int>(Settings.Tab) + 1) % tabCount);
			ProfilerScroll = 0;
		}
		if (Actions.Fired(Action::PreviousProfilerTab)) {
			Settings.Tab =
				static_cast<ProfilerTab>((static_cast<int>(Settings.Tab) + tabCount - 1) % tabCount);
			ProfilerScroll = 0;
		}

		if (Actions.Fired(Action::ScrollProfilerUp)) {
			ProfilerScroll = std::max(0, ProfilerScroll - 4);
		}
		if (Actions.Fired(Action::ScrollProfilerDown)) {
			ProfilerScroll += 4;
		}

		// Clamped to what was recorded. Offering a depth past MAXIMUM_DEPTH
		// would suggest there is something deeper to reveal, and there is not:
		// nothing below it was ever stored.
		if (Actions.Fired(Action::DecreaseProfilerDepth) && ProfilerDepth > 0) {
			ProfilerDepth--;
		}
		if (Actions.Fired(Action::IncreaseProfilerDepth) && ProfilerDepth < FrameGraph::MAXIMUM_DEPTH) {
			ProfilerDepth++;
		}

		if (Actions.Fired(Action::WriteProfilerSnapshot)) {
			WriteSnapshot();
		}
	}

	void Client::WriteSnapshot() {
		// Beside the binary rather than in the working directory, which is
		// wherever the launcher happened to be.
		const auto path = engine::core::Paths::Base() / "frame-graph-snapshot.txt";

		if (!FrameGraph::WriteSnapshot(path)) {
			// The overwhelmingly likely reason, and the one worth naming:
			// collection only retains while the panel is open, so F8 with F5
			// closed has nothing to write.
			ENGINE_WARN(
				"no snapshot written to {} — nothing retained. The frame graph only records "
				"while it is open, so press F5 first.",
				path.string()
			);
			return;
		}

		ENGINE_INFO(
			"snapshot: {} frame(s) over {:.2f}s written to {}",
			FrameGraph::HistoryFrames(),
			FrameGraph::HistorySeconds(),
			path.string()
		);
	}

	void Client::Step() {
		// **The limiter sleeps here, before anything else in the frame.** Ahead
		// of the swapchain wait because the two are alternatives rather than a
		// sequence: with vblank on, the display paces the loop and this does
		// nothing; with it off, this is the pacing and the wait below returns
		// immediately.
		//
		// Against a deadline that advances by a fixed step, not by sleeping a
		// computed amount each frame. The difference is drift: a sleep
		// overshoots by whatever the scheduler felt like, and a loop that
		// recomputed from "now" every frame would accumulate every one of those
		// overshoots and settle below the rate it was asked for.
		if (Settings.Uncapped && Settings.MaximumFrameRate > 0) {
			using namespace std::chrono;
			const auto period = nanoseconds(1'000'000'000ull / Settings.MaximumFrameRate);
			const auto now = steady_clock::now();

			if (NextFrameAt.time_since_epoch().count() == 0) {
				NextFrameAt = now;
			} else if (now < NextFrameAt) {
				std::this_thread::sleep_until(NextFrameAt);
			} else if (now - NextFrameAt > period * 4) {
				// A stall — a resize, a hitch, a debugger. Catching up on four
				// frames of deficit by running four frames flat out is worse
				// than the stall was, so the deadline is reset rather than
				// chased.
				NextFrameAt = now;
			}
			NextFrameAt += period;
		}

		// **The display is waited for before the input is read**, for the reason
		// `Editor::Run` gives at length: the swapchain wait is most of a frame
		// with vertical sync on, and doing it after the pump means every frame is
		// drawn from input that is already a frame old. The client has the same
		// shape as the studio and had the same frame of delay in it.
		//
		// It costs a frame of nothing when it fails — minimised, or mid-resize —
		// and `Render` reaches the same conclusion for itself below.
		Renderer.WaitForFrame();

		const float delta = Clock.Tick();

		// Everything from here to EndFrame is one frame's worth of spans. The
		// panels below draw the *previous* frame's, because this one has not
		// finished being measured.
		FrameGraph::BeginFrame();

		PumpEvents();

		// **Before the simulation and outside every pass.** Content becoming
		// visible mid-tick is `AGENTS.md` rule 5's desync, and content
		// registering mid-frame is an upload into a buffer a render pass may be
		// reading.
		PumpContent();

		// Simulation and rendering advance at different rates, and this is
		// where they separate. The frame runs as fast as the display and the
		// GPU allow; the simulation runs a whole number of fixed steps, which
		// is often zero on a fast machine and several after a stall.
		//
		// A system therefore never sees a variable delta, so the same scene
		// behaves identically at 30 fps and 600 — and a recorded run replays.
		// RENDER_PIPELINE.md §14.
		{
			// The universe owns the accumulator now, so the world runs however
			// many fixed ticks it owes and the client no longer keeps a second
			// copy of the rate it is running at.
			ENGINE_PROFILE_CAT("simulation", engine::core::ProfileCategory::Simulation);
			Universe_->Tick(delta);
		}

		{
			// After the tick and before presentation, the same place the server
			// publishes from — so what the replica applied this frame is what
			// the frame draws, rather than being one frame stale for no reason.
			//
			// `Network` and not `ECS`: this is the socket being drained and
			// what came out of it being applied. It was ECS time because it
			// writes to a store, which put the link's cost in the same bar as
			// the systems and made a bad connection read as a slow game.
			ENGINE_PROFILE_CAT("replication", engine::core::ProfileCategory::Network);
			PollServer(engine::core::Clock::Seconds());
		}

		// After the tick and the replica's apply, so what a script set this
		// frame is heard this frame rather than next. Before presentation
		// because presentation is where the frame stops being about state.
		PumpSounds();

		{
			// Once per frame, and separate from the tick because a client draws
			// one world while the rest keep simulating. This is the phase that
			// turns simulation state into something to draw, so it is the one
			// that interpolates.
			ENGINE_PROFILE_CAT("pre-render", engine::core::ProfileCategory::Simulation);

			// Every world, not only the one whose camera is used. A world that
			// is composited but not presented would publish the frame it built
			// last time it was, which is a world that appears frozen for a
			// reason nothing reports.
			// **Cleared once, before any world is asked.** `CollectSurfaceViews`
			// clears as well, but only the drawn world reaches it — a world that
			// returns early for want of a camera would otherwise leave the
			// previous frame's mirrors in the list, and the surface pass would
			// go on rendering a camera that is no longer in the scene.
			Surfaces.clear();

			// **Cleared with the surfaces and for a sharper version of the same
			// reason.** A stale `SurfaceView` renders a camera that has gone; a
			// stale `ParticleBatch` is a span into a pool that has been stepped
			// since, so its `Live` prefix may now be shorter than the span says.
			// That is a read past the live particles rather than a wrong picture.
			Particles.clear();
			Lights.clear();
			RibbonVertices = {};
			RibbonRuns = {};

			for (const engine::world::WorldId id : Simulated) {
				// **Written before `Present`, so this frame's `PreRender` sees
				// this frame's input.** A camera controller reads the state and
				// places the camera in the same pass; writing afterwards would
				// aim every camera at where the mouse was one frame ago, which is
				// the lag nobody can find by reading the controller.
				//
				// **Every simulated world, not only the drawn one.** A world the
				// player is not looking at still ticks its scripts, and a script
				// polling `UserInputService` there should get the same answer —
				// the alternative is input that works in one world and silently
				// does not in another.
				Universe_->Enter(id, [this](engine::ecs::Store &store) {
					if (auto *state = store.ResourceMutable<engine::scene::InputState>()) {
						// **The behaviour is read back, not overwritten.** A
						// script sets `UserInputService.MouseBehavior` and the
						// client applies it to the window; copying the whole
						// translator over the resource would throw that away every
						// frame. `scene::InputState` is the seam in both
						// directions.
						const engine::scene::MouseBehavior wanted = state->Behaviour;
						*state = Input.State();
						state->Behaviour = wanted;
						PointerMode = wanted;
					}
				});

				Universe_->Present(id, delta, Universe_->AlphaOf(id));

				// Published from inside the world, straight after its PreRender
				// phase filled the draw list. The camera and the list stay
				// where they were produced; what leaves is a copy in a buffer
				// the renderer owns the other end of.
				Universe_->Enter(id, [this, id](engine::ecs::Store &store) {
					const auto *active = store.Resource<engine::scene::ActiveCamera>();
					const auto *list = store.Resource<DrawList>();
					if (active == nullptr || list == nullptr) {
						return;
					}

					// The live camera is a row: `ActiveCamera` names which
					// entity it is and the placement and the lens are the
					// components on it.
					const auto *placement = store.Get<engine::scene::Transform>(active->Entity);
					const auto *lens = store.Get<engine::scene::Camera>(active->Entity);
					if (placement == nullptr || lens == nullptr) {
						return;
					}

					if (id == Rendered) {
						// Kept for the replicated view below, which has no
						// camera of its own.
						ComposedFrame = placement->Frame;
						ComposedCamera = *lens;

						// **The surface cameras, read from the world that owns
						// them.** All of them: the pipeline renders one offscreen
						// view per surface index since v0.8, so a room of
						// mirrored walls gets a working mirror per wall rather
						// than one wall's image projected across all four.
						(void)CollectSurfaceViews(store, Surfaces);

						// **The particles, from the world being drawn and only
						// that one.** A batch is a span into this world's pool;
						// see `Client.hpp` for why a second world's cannot be
						// appended to the same list.
						(void)CollectParticleBatches(store, Particles);

						// **The ribbons are taken as spans rather than copied**,
						// which is safe for the frame and only for the frame:
						// `BuildRibbons` clears and refills the buffer in the next
						// `PreRender`, and `Render` is called before that.
						RibbonVertices = engine::effects::RibbonStream(store);
						RibbonRuns = engine::effects::RibbonRuns(store);

						// **Ordered from the eye, which is why this needs the
						// camera and the two above do not.** The renderer takes
						// sixteen lights and a world may hold any number; which
						// sixteen is a scene question and distance is the answer
						// that is right more often than it is wrong.
						(void)CollectLights(store, placement->Frame.Position, Lights);
					}

					Views.Publish(
						id, placement->Frame, *lens, list->Instances, store.Time().Tick, store.Time().Alpha
					);
				});
			}

			// The replicated world, once it has joined and been given a
			// channel. Presented like any other world — `PreRender` is where
			// deriving what to draw belongs, whoever owns the simulation.
			//
			// **It is looked at through this client's own camera**, because a
			// replica has none: a camera is an entity, and an authoritative
			// entity minted in a replica collides exactly with one the server
			// minted, which is what `Store::SetAdoptOnly` refuses. A local row
			// in a replicated world is safe — `Store::CreatePredicted` mints
			// from a range the server never allocates from — and since v0.8
			// something does own it: `AimReplicaViewer` puts a predicted camera
			// in the replica and names it `ActiveCamera`.
			//
			// **Before `Present`, because `PreRender` is where the mirrors are
			// aimed.** `aim-surface-cameras` reads `ActiveCamera` and reflects
			// through it, so setting the eye afterwards would aim every mirror
			// at where the client stood last frame.
			if (ReportedJoin) {
				Universe_->Enter(Replicated, [this](engine::ecs::Store &store) {
					(void)AimReplicaViewer(store, ComposedFrame, ComposedCamera);
				});

				Universe_->Present(Replicated, delta, Universe_->AlphaOf(Replicated));

				Universe_->Enter(Replicated, [this](engine::ecs::Store &store) {
					const auto *list = store.Resource<DrawList>();
					if (list == nullptr) {
						return;
					}
					Views.Publish(
						Replicated,
						ComposedFrame,
						ComposedCamera,
						list->Instances,
						store.Time().Tick,
						store.Time().Alpha
					);
				});
			}
		}

		Statistics.Record(Clock.Now(), delta);

		// **Advanced with the frame it will be drawn against.** Accumulated from
		// the frame delta rather than read from a wall clock, so a run that
		// stalls does not skip an animation forward and two runs of one
		// recording show the same frames — `Renderer::SetAnimationTime`.
		AnimationSeconds += delta;

		// Ticks actually achieved, over a one-second window. It matches the
		// configured rate until the machine cannot keep up, and the gap is the
		// number worth seeing.
		if (Clock.Now() - TickWindowStarted >= 1.0) {
			const auto elapsed = static_cast<float>(Clock.Now() - TickWindowStarted);
			const uint64_t ticksNow = Universe_->StatisticsOf(Rendered).Ticks;
			MeasuredTicksPerSecond = static_cast<float>(ticksNow - TicksAtWindowStart) / elapsed;
			TickWindowStarted = Clock.Now();
			TicksAtWindowStart = ticksNow;
		}

		int pixelWidth = 0;
		int pixelHeight = 0;
		SDL_GetWindowSizeInPixels(Window, &pixelWidth, &pixelHeight);

		{
			ENGINE_PROFILE_CAT("debug panels", engine::core::ProfileCategory::Render);

			Overlay.Resize(pixelWidth, pixelHeight);

			// The panels are redrawn on a clock of their own, and presented on
			// every frame from the texture they were last drawn into.
			//
			// They are read by a person, and a person cannot read a number that
			// changes a thousand times a second — past about twenty updates a
			// second the extra work buys a blur. Rasterising the glyphs and
			// pushing the image across were together the largest thing in the
			// frame, and at 1000 fps this is fifty times less of both.
			//
			// The *collection* is untouched: FrameGraph and Metrics still record
			// every frame, so nothing is missed. Only the drawing is throttled,
			// and RMAX still reports the worst frame in the window rather than
			// the worst frame that happened to be drawn.
			constexpr double PANEL_UPDATE_SECONDS = 1.0 / 20.0;

			// Anything a key press changed has to appear at once, or the panel
			// feels broken: pressing F6 and waiting fifty milliseconds for the
			// tab to change reads as a dropped input.
			const bool settingsChanged =
				PanelsShown != (Settings.ShowStatistics || Settings.ShowNetwork || Settings.ShowFrameGraph) ||
				PanelTab != Settings.Tab || PanelScroll != ProfilerScroll || PanelDepth != ProfilerDepth ||
				PanelWidth != pixelWidth || PanelHeight != pixelHeight;

			const bool redraw = settingsChanged || Clock.Now() - PanelsDrawn >= PANEL_UPDATE_SECONDS;

			if (redraw) {
				PanelsDrawn = Clock.Now();
				PanelsShown = Settings.ShowStatistics || Settings.ShowNetwork || Settings.ShowFrameGraph;
				PanelTab = Settings.Tab;
				PanelScroll = ProfilerScroll;
				PanelDepth = ProfilerDepth;
				PanelWidth = pixelWidth;
				PanelHeight = pixelHeight;
			}

			if (!redraw) {
				// Counters accumulate whether or not anyone is looking, and a
				// frame that does not draw them still has to drain them or the
				// next panel shows several frames added together.
				Metrics::Clear();
			} else if (Settings.ShowStatistics || Settings.ShowNetwork || Settings.ShowFrameGraph) {
				SystemTimings.clear();
				Universe_->Enter(Rendered, [this](engine::ecs::Store &, engine::ecs::Scheduler &systems) {
					for (const auto &timing : systems.Timings()) {
						SystemTimings.push_back(
							engine::render::SystemTiming{timing.Name, timing.Milliseconds}
						);
					}
				});

				const auto counters = Metrics::Drain();

				engine::render::DebugPanelData panels;
				panels.ShowStatistics = Settings.ShowStatistics;
				panels.ShowNetwork = Settings.ShowNetwork;
				panels.ShowFrameGraph = Settings.ShowFrameGraph;
				panels.Tab = Settings.Tab;
				panels.Scroll = ProfilerScroll;
				panels.DepthLimit = ProfilerDepth;
				panels.HistorySeconds = FrameGraph::HistorySeconds();
				panels.TracyAttached = ENGINE_PROFILE_ATTACHED();
				panels.Statistics = &Statistics;
				panels.Spans = FrameGraph::Spans();
				panels.FrameMilliseconds = FrameGraph::FrameMilliseconds();
				panels.UnmarkedMilliseconds = FrameGraph::UnmarkedMilliseconds();
				panels.IdleMilliseconds =
					FrameGraph::CategoryMilliseconds(engine::core::ProfileCategory::Idle);
				panels.DroppedSpans = FrameGraph::Dropped();
				panels.Systems = SystemTimings;
				panels.Counters = counters;
				// Asked of the world, not read back off the command line. The
				// number that matters is what the world actually holds, and
				// the day something spawns or destroys an entity those two
				// stop being the same.
				panels.Entities = 0;
				for (const engine::world::WorldId id : Simulated) {
					Universe_->Enter(id, [&panels](engine::ecs::Store &store) {
						panels.Entities +=
							store.CountMatching<engine::scene::Transform, engine::scene::Visual>();
					});
				}
				if (ReportedJoin) {
					// The replica counts too. A number that ignored it would
					// say the client is drawing fewer things than it is.
					Universe_->Enter(Replicated, [&panels](engine::ecs::Store &store) {
						panels.Entities +=
							store.CountMatching<engine::scene::Transform, engine::scene::Visual>();
					});
				}
				panels.TickRate = Settings.TickRate;
				panels.TicksPerSecond = MeasuredTicksPerSecond;
				panels.DroppedTicks = Universe_->StatisticsOf(Rendered).DroppedTicks;
				panels.DrawCalls = LastFrame.DrawCalls;
				panels.Triangles = LastFrame.Triangles;
				panels.Backend = Renderer.BackendName();
				panels.Network = SampleNetwork();
				// One logical pixel of the font per two physical, so the panels
				// stay the same apparent size on a high-DPI display.
				panels.Scale = pixelWidth >= 2400 ? 3 : 2;

				engine::render::DrawDebugPanels(Overlay, panels);
			} else {
				// Nothing drawn means nothing uploaded and no overlay pass.
				Overlay.Clear();
				// Counters accumulate whether or not anyone is looking, so
				// they still have to be drained.
				Metrics::Clear();
			}
		}

		// Drawn from what the compositor took off the view channels, not from a
		// store. The camera was placed by a system and the draw list was filled
		// by one in PreRender — but the renderer runs at the display's rate and
		// the worlds run at their own, so reaching into a store here would be
		// reading something somebody else is writing. Between them sits three
		// slots and an atomic index, which is what lets a slow frame drop
		// rather than throttle a simulation.
		// Counted rather than read off the option, because `--connect` adds a
		// view the option does not know about. Two views drawn on top of each
		// other is two scenes inside one, which reads as a rendering fault.
		Views.Compose(Views.Count() > 1 ? Settings.ViewSpacing : 0.0f);
		// **The world's own interface, compiled here.** The studio does this
		// per viewport panel because a panel *is* a canvas; a client has one
		// canvas, which is the window.
		//
		// **An instance is an entity and a class is a set of components**, so
		// the `ScreenGui` this walks is the same storage a system iterates —
		// there is no second tree to keep in step, which is what makes one
		// `Layout` pass over the store the whole of it.
		// **The image resolver, set once and never unset.** `InterfacePass` takes
		// a hook rather than a texture table because `render` has no business
		// resolving a game's content names — `gui::DrawCommand::Image` is a name
		// precisely so that whoever owns the textures decides. Nothing supplied
		// one, so every `ImageLabel` in the engine drew the atlas's white texel
		// and read as a missing image whatever had loaded. The seam was right and
		// one end of it was never connected.
		//
		// Set here rather than at start-up because the renderer's table is what
		// answers, and a lambda capturing `this` outlives the frame it is set in.
		if (!InterfaceImagesReady) {
			InterfaceImagesReady = true;
			Interface.SetImageSource(
				[this](const engine::core::Name &name, engine::render::FlipbookCell &cell) -> void * {
					void *const handle = Renderer.TextureHandle(name);
					if (handle != nullptr) {
						cell = Renderer.TextureCell(name, AnimationSeconds);
					}
					return handle;
				}
			);
		}

		engine::render::InterfacePass *hook = nullptr;
		if (Rendered.IsValid()) {
			engine::gui::CompileRequest request;
			request.Display.Width = static_cast<float>(Settings.Width);
			request.Display.Height = static_cast<float>(Settings.Height);

			// Fed back from the previous frame's routing, deliberately: the
			// hover comes from the list a compile produced, so a compile
			// reading this frame's hover would depend on its own output.
			request.Hovered = InterfaceRouter.Hovered();
			request.Pressed = InterfaceRouter.Pressed();

			Universe_->Enter(Rendered, [&](engine::ecs::Store &store) {
				// **Before the layout, and that order is the whole of it.** A
				// `SurfaceGui` sized in pixels-per-stud and a `BillboardGui`
				// sized in studs both need numbers `gui` cannot reach — the
				// adornee's stud extent and the live camera's distance — so
				// this resolves them into `gui::SpatialCanvas` and the layout
				// reads that instead of the authored pixels. Running it *after*
				// would draw every billboard at the previous frame's size,
				// which on one a player is walking towards is a visible lag on
				// everything inside it.
				engine::render::ResolveSpatialCanvases(store, request.Display);
				engine::gui::Layout(store, request.Display);
				InterfaceList.Rebuild(store, request);
			});

			if (!InterfaceList.Commands().Commands.empty()) {
				Interface.Submit(
					InterfaceList.Commands(),
					engine::core::Vector2{request.Display.Width, request.Display.Height}
				);
				hook = &Interface;
			}
		}

		// **A capture needs an offscreen target, so asking for one turns the
		// client into a two-step render.** The scene goes into a texture, the
		// texture is what gets copied back, and the window is presented from
		// it. A game does not want that cost, which is why it happens only when
		// `--capture` was passed — and why the flag is a diagnostic rather than
		// a feature.
		engine::render::SceneTarget target{};
		const engine::render::SceneTarget *sceneTarget = nullptr;
		if (!Settings.Capture.empty()) {
			target.Width = static_cast<uint32_t>(Settings.Width);
			target.Height = static_cast<uint32_t>(Settings.Height);
			sceneTarget = &target;
		}

		// **Accumulated from the frame's own delta rather than read from a
		// clock**, so a paused editor or a stopped run holds its animations
		// where they were and a recorded session replays them identically —
		// `Renderer::SetAnimationTime` carries the rule.
		Renderer.SetAnimationTime(AnimationSeconds);

		// One view, which is what a game is. The span is of a single local
		// rather than a member: nothing is retained past the call, so there is
		// nothing for the client to own.
		//
		// **Assigned rather than brace-initialised**, because a `View` has
		// fields this caller has nothing to say about and a designated
		// initialiser list that skips one is a warning per skipped field. The
		// defaults are the answer for those, and saying so by leaving them
		// alone is clearer than repeating them.
		engine::render::View view;
		view.CameraFrame = Views.CameraFrame();
		view.Camera = Views.Camera();
		view.Instances = Views.Instances();
		view.Surfaces = Surfaces;
		view.Target = sceneTarget;
		view.Particles = Particles;
		view.RibbonVertices = RibbonVertices;
		view.RibbonRuns = RibbonRuns;
		view.Lights = Lights;

		LastFrame = Renderer.Render({&view, 1}, Overlay, hook);

		// **After the frame rather than before it**, so the capture is of a
		// frame whose scene texture exists — the studio's own capture states
		// the same thing for the same reason. One frame before the last, so the
		// request is made on one frame and written by the next while the run
		// still ends when it was told to.
		if (!Settings.Capture.empty() && Settings.MaximumFrames > 1 &&
			FramesDrawn == Settings.MaximumFrames - 2) {
			Renderer.RequestSceneCapture(Settings.Capture);
		}

		FrameGraph::EndFrame();
		ENGINE_PROFILE_FRAME();

		if (LastFrame.Presented) {
			FramesDrawn++;
		}

		// **What was actually drawn, as counters.** Both are per frame and both
		// are the numbers that say whether the mesh path is doing anything: a
		// world of cubes is twelve triangles an instance and a world of imported
		// meshes is thousands, so a run whose triangle count did not move is one
		// where every `MeshId` resolved to the fallback.
		using engine::core::Metrics;
		Metrics::Count("render.triangles", static_cast<double>(LastFrame.Triangles));
		Metrics::Count("render.draw-calls", static_cast<double>(LastFrame.DrawCalls));

		// **The peak rather than the latest.** The frame a run exits on is
		// often one that presented nothing — the window is going away — so the
		// last frame's counters are zero on almost every bounded run, which
		// makes them useless as the one number a log can carry.
		PeakTriangles = std::max(PeakTriangles, LastFrame.Triangles);
		PeakDrawCalls = std::max<uint32_t>(PeakDrawCalls, LastFrame.DrawCalls);
	}

	int Client::Run() {
		if (!Running) {
			return 1;
		}

		while (Running) {
			Step();

			if (Settings.MaximumFrames >= 0 && FramesDrawn >= Settings.MaximumFrames) {
				ENGINE_INFO("frame budget of {} reached", Settings.MaximumFrames);
				break;
			}

			if (Settings.ProfileSeconds > 0.0 && Clock.Now() >= Settings.ProfileSeconds) {
				// **The pass counts are here because nothing else can report
				// them.** A shadow pass or a surface pass that silently did not
				// run looks exactly like one that ran and changed nothing, and
				// neither has a unit test — `AGENTS.md` names the GPU exception
				// and refuses a mock renderer to close it. The last frame's
				// draw calls are the cheapest honest evidence that the passes
				// are being submitted at all.
				ENGINE_INFO(
					"profiled for {:.1f}s over {} frames · {} draw call(s), {} culled, {} surfaced, "
					"{} surface pass(es)",
					Clock.Now(),
					FramesDrawn,
					LastFrame.DrawCalls,
					LastFrame.Culled,
					LastFrame.SurfaceInstances,
					LastFrame.SurfacePasses
				);
				break;
			}
		}

		if (Statistics.HasSamples()) {
			ENGINE_INFO(
				"{} frames · {:.1f} avg · {:.1f} min · {:.1f} max FPS",
				FramesDrawn,
				Statistics.Average(),
				Statistics.Minimum(),
				Statistics.Maximum()
			);

			// **What the last frame actually drew.** A world of cubes is twelve
			// triangles an instance; a world of imported meshes is tens of
			// thousands. A run whose triangle count did not move from the first
			// is one where every `MeshId` resolved to the fallback, and that is
			// the only cheap way to tell from a log.
			ENGINE_INFO(
				"{} triangle(s) in {} draw call(s) at the busiest frame", PeakTriangles, PeakDrawCalls
			);
			ENGINE_INFO(
				"{} tick(s) at {:.0f} Hz · {:.1f} achieved · {} dropped",
				Universe_->StatisticsOf(Rendered).Ticks,
				Settings.TickRate,
				Clock.Now() > 0.0 ? static_cast<double>(Universe_->StatisticsOf(Rendered).Ticks) / Clock.Now()
								  : 0.0,
				Universe_->StatisticsOf(Rendered).DroppedTicks
			);
		}

		ReportReplica();
		return 0;
	}

	engine::render::NetworkStatistics Client::SampleNetwork() {
		engine::render::NetworkStatistics network;
		if (Connection == nullptr) {
			// Not connected stays not connected, and every field below stays
			// zero. `DrawDebugPanels` draws nothing at all for this.
			return network;
		}

		network.Connected = true;
		network.Joined = Connection->Joined();
		network.AppliedTick = Connection->Applied();

		const engine::net::ConnectionStats &link = Connection->Link().Stats();
		network.ReceivedBytes = link.BytesReceived;
		network.SentBytes = link.BytesSent;
		network.RoundTripMilliseconds = link.RoundTripMilliseconds;
		network.PacketsLost = link.PacketsLost;
		network.PacketsStale = link.PacketsStale;
		network.SendsOverBudget = link.SendsOverBudget;

		// **Rates are a derivative and `net` only keeps the integral.** Every
		// counter above is cumulative over the connection's life, so a rate has
		// to come from two readings and the time between them. Taken here
		// rather than in `net` because a counter that had to be sampled on a
		// clock would be a counter that read one, and this module's whole
		// discipline is that it does not.
		//
		// The window is however long it has been since the last panel redraw —
		// a twentieth of a second at the panel's own rate. Short enough to
		// follow a stream that starts and stops, long enough that it is not one
		// packet's worth of noise.
		const double now = Clock.Now();
		const double elapsed = now - NetworkSampledAt;
		if (NetworkSampled && elapsed > 0.0) {
			network.ReceivedBytesPerSecond =
				static_cast<double>(link.BytesReceived - NetworkLastReceivedBytes) / elapsed;
			network.SentBytesPerSecond = static_cast<double>(link.BytesSent - NetworkLastSentBytes) / elapsed;
			network.ReceivedPacketsPerSecond =
				static_cast<double>(link.PacketsReceived - NetworkLastReceivedPackets) / elapsed;
			network.SentPacketsPerSecond =
				static_cast<double>(link.PacketsSent - NetworkLastSentPackets) / elapsed;
		}

		NetworkSampled = true;
		NetworkSampledAt = now;
		NetworkLastReceivedBytes = link.BytesReceived;
		NetworkLastSentBytes = link.BytesSent;
		NetworkLastReceivedPackets = link.PacketsReceived;
		NetworkLastSentPackets = link.PacketsSent;

		const engine::replication::Replica::Statistics &replica = Connection->ReplicaStats();
		network.Snapshots = replica.Snapshots;
		network.Deltas = replica.Deltas;
		network.Structures = replica.Structures;
		network.Malformed = replica.Malformed;
		network.Stale = replica.Stale;

		// The interpolation half, and the row count that says whether any of it
		// reached a draw list. Only once the world exists — before the join
		// there is no replicated store to enter.
		if (ReportedJoin) {
			Universe_->Enter(Replicated, [&network](engine::ecs::Store &store) {
				network.Entities = store.CountMatching<engine::scene::Transform>();

				if (const auto *drawList = store.Resource<DrawList>()) {
					network.Drawn = drawList->Instances.size();
				}
				if (const auto *buffer = store.Resource<engine::replication::SnapshotBuffer>()) {
					network.TickRate = buffer->MeasuredTickRate();
					network.BehindTicks = buffer->Behind();
					network.Stalls = buffer->Stats().Stalls;
					network.Interpolated = buffer->Stats().Interpolated;
					network.Held = buffer->Stats().Held;
				}
			});
		}

		return network;
	}

	void Client::ReportReplica() {
		if (Connection == nullptr) {
			return;
		}

		// **A replica that joined and drew nothing reads from outside exactly
		// like a replica that never joined**, and the difference is four
		// numbers this process already has. Printed at exit rather than left in
		// the F3 panel, because the reported symptom — "nothing appears in the
		// scene" — is one somebody hits on a machine where they are looking at
		// the window rather than at a counter, and a run with `--frames`
		// produces no window to look at at all.
		//
		// Read in this order, and the first one that is wrong is the answer:
		// rows arrived, rows were drawn, the world moved between ticks.
		size_t entities = 0;
		size_t drawn = 0;
		double behind = 0.0;
		uint64_t stalls = 0;
		uint64_t interpolated = 0;
		uint64_t held = 0;
		double rate = 0.0;

		Universe_->Enter(Replicated, [&](engine::ecs::Store &store) {
			store.EachEntity([&entities](engine::ecs::Entity) { entities++; });

			if (const auto *drawList = store.Resource<DrawList>()) {
				drawn = drawList->Instances.size();
			}
			if (const auto *buffer = store.Resource<engine::replication::SnapshotBuffer>()) {
				behind = buffer->Behind();
				stalls = buffer->Stats().Stalls;
				interpolated = buffer->Stats().Interpolated;
				held = buffer->Stats().Held;
				rate = buffer->MeasuredTickRate();
			}
		});

		ENGINE_INFO(
			"replica: {} entities · {} drawn · {:.2f} ticks behind · {} stall(s) · {} interpolated / {} held "
			"· {:.1f} Hz measured",
			entities,
			drawn,
			behind,
			stalls,
			interpolated,
			held,
			rate
		);

		// **Drawn and never seen is the third case, and it is a framing
		// problem rather than a replication one.** The composited camera is the
		// demo world's: it is placed from *that* world's bounds and its far
		// plane follows the same distance, so a replicated world larger than
		// the demo is drawn outside a frustum sized for something else.
		// `mono.client/AGENTS.md` records the camera of its own that fixes it.
		if (drawn > 0) {
			ENGINE_INFO(
				"replica: drawn through the demo world's camera — `--view-spacing 0` overlays the two if the "
				"replicated world is not on screen"
			);
		}
	}
}
