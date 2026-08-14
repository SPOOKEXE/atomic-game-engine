#include <engine/assets/ContentForm.hpp>
#include <engine/assets/Manifest.hpp>
#include <engine/assets/Material.hpp>
#include <engine/audio/Wav.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Paths.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/effects/Ribbon.hpp>
#include <engine/examples/Shooting.hpp>
#include <engine/game/Game.hpp>
#include <engine/game/Play.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Layout.hpp>
#include <engine/gui/Services.hpp>
#include <engine/gui/Typing.hpp>
#include <engine/input/Translate.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/parallel/Process.hpp>
#include <engine/parallel/Settings.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Input.hpp>
#include <engine/scene/Materials.hpp>
#include <engine/scene/MeshCatalogue.hpp>
#include <engine/scene/PublishedCatalogue.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Sunlight.hpp>
#include <engine/scene/SurfaceCameras.hpp>
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

		// **Said at startup rather than at the first refusal**, so "my texture
		// never arrives" and "this client was told not to fetch GIFs" are one
		// line apart in the same log. `main` has already applied and frozen the
		// settings, so this is the complete answer.
		if (const std::string refused =
				engine::assets::ContentPolicy::Process(engine::assets::ContentVerb::Handle).RefusedText();
			!refused.empty()) {
			ENGINE_INFO("content: turned off — {}", refused);
		}

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

		if (!Settings.Headless) {
			Window = SDL_CreateWindow(
				"atomic",
				Settings.Width,
				Settings.Height,
				SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY
			);
			if (!Window) {
				ENGINE_ERROR("SDL_CreateWindow: {}", SDL_GetError());
				return false;
			}
		}

		// Null when headless, which is what puts the renderer in that mode — the
		// same call the editor makes, for the same reason.
		if (!Renderer.Initialise(Window)) {
			return false;
		}

		// **Said once here and applied every frame, beside the sun.** The depth
		// is the world's now — `workspace.SurfaceBounces`, or the renderer's own
		// measurement when nothing says — so a value pushed once at startup would
		// be overwritten by the first world drawn. What this line is for is the
		// log: a run that pins the number has done something the scene did not
		// ask for, and that is worth one line rather than a silent override.
		if (Settings.SurfaceBounces > 0) {
			ENGINE_INFO(
				"surfaces: {} bounce(s), overriding whatever the world asks for", Settings.SurfaceBounces
			);
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

		// **Only with a window**, because a present mode belongs to a swapchain
		// and a headless run has none.
		if (Settings.Uncapped && Window != nullptr && !Renderer.SetVerticalSync(false)) {
			ENGINE_WARN("--uncapped had no effect; frames stay paced by the display");
		}

		// **The configured count wins, and zero means work it out.** A machine
		// running a client beside something else is the case that wants to say
		// so, and `WorkersPerHost(1)` cannot know about the something else.
		const unsigned configured = engine::parallel::ConfiguredWorkers();
		engine::parallel::Jobs::Start(configured != 0 ? configured : engine::parallel::WorkersPerHost(1));

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
				Runtimes.emplace_back(id, engine::game::StartWorldScripts(store, systems, limits, failure));
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
			std::shared_ptr<engine::script::Runtime> runtime;
			Universe_->Enter(
				id,
				[this, &scripted, &scenePath, &runtime](
					engine::ecs::Store &store, engine::ecs::Scheduler &systems
				) {
					// Do not present a partially built world.
					scripted = BuildScriptedWorld(store, systems, scenePath, Settings.Entities, &runtime);
				}
			);

			if (!scripted) {
				ENGINE_ERROR("the scene script failed, so there is nothing to render");
				return false;
			}

			// **Recorded against the world, which is what `DeliverGuiEvents`
			// needs and what this path never did.** The scheduler holds the last
			// reference and drops it with the world — that is still true and is
			// still the lifetime — but nothing could *name* the VM, so a
			// `TextButton` in a `--script` scene fired nothing in a shipped
			// client while the same tree worked in the editor. The `--game` path
			// has filled this vector since v0.7; the demo path had not.
			if (runtime != nullptr) {
				Runtimes.emplace_back(id, std::move(runtime));
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

		// **How much decoding and uploading one frame will do**, and the same
		// allowance the studio's own intake uses for the same reason: content
		// arrives in bursts, and draining every completed request in the frame
		// that noticed them is a frame that takes a third of a second when
		// somebody walks into a room full of new models. `IntakeBudget` says why
		// it is bytes rather than a count and why the first arrival is always
		// admitted.
		ContentBudget.Begin();

		size_t kept = 0;
		for (const engine::delivery::RequestId id : ContentPending) {
			const engine::delivery::RequestState state = Content->StateOf(id);
			if (state == engine::delivery::RequestState::Pending) {
				ContentPending[kept++] = id;
				continue;
			}

			// Held rather than dropped: an arrival this frame cannot take is
			// still an arrival, and putting it back is what makes the budget a
			// delay instead of a loss.
			if (!ContentBudget.Admits()) {
				ContentBudget.Defer();
				ContentPending[kept++] = id;
				continue;
			}

			// **Read before the take, because a take is what destroys it.** A
			// failed request answers no asset and therefore no name, and the
			// name is what has to be unmarked — see `render::ChooseTexture`.
			const engine::core::Name asked(Content->NameOf(id));

			std::optional<engine::delivery::Asset> asset = Content->Take(id);

			// **On the request finishing, not on it succeeding**, and above
			// every `continue` below so no branch can forget. Unmarking only on
			// arrival would leave a misspelled sheet expected for ever, which is
			// precisely the case the purple marker exists for.
			Renderer.StopExpectingTexture(asked);

			if (!asset) {
				// Failed, or already taken. Either way there is nothing more to
				// wait for; `delivery` has already counted it.
				continue;
			}

			ContentBudget.Spend(asset->Bytes.size());

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

					// **The sheets its submeshes name, recorded where they are
					// readable.** They live inside the mesh file, so this is the
					// one point anything can learn them — and without them a
					// script that wants to swap a model's texture has no way to
					// find out what it is wearing or what to put back.
					std::vector<engine::core::Name> sheets;
					sheets.reserve(mesh.Submeshes.size());
					for (const engine::assets::Submesh &submesh : mesh.Submeshes) {
						sheets.emplace_back(submesh.Texture);
					}

					// Mesh metadata is world data, not renderer state.
					const auto triangles = static_cast<uint32_t>(mesh.Indices.size() / 3);
					for (const engine::world::WorldId id : Simulated) {
						Universe_->Enter(id, [&name, triangles, &sheets](engine::ecs::Store &store) {
							engine::scene::RecordMesh(store, name, triangles, sheets);
						});
					}
					if (ReportedJoin) {
						Universe_->Enter(Replicated, [&name, triangles, &sheets](engine::ecs::Store &store) {
							engine::scene::RecordMesh(store, name, triangles, sheets);
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
				// **All five, built once and recorded together.** A material is
				// one thing; recording its colour and forgetting its normals
				// would draw a part textured and flat, which reads as the normal
				// map being broken rather than absent.
				const engine::scene::MaterialMaps maps{
					.Colour = engine::core::Name(material.ColourMap),
					.Normal = engine::core::Name(material.NormalMap),
					.Roughness = engine::core::Name(material.RoughnessMap),
					.Occlusion = engine::core::Name(material.OcclusionMap),
					.Height = engine::core::Name(material.HeightMap),
					.Emissive = engine::core::Name(material.EmissiveMap),
				};

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
					Universe_->Enter(id, [&name, &maps](engine::ecs::Store &store) {
						engine::scene::RecordMaterial(store, name, maps);
					});
				}
				if (ReportedJoin) {
					Universe_->Enter(Replicated, [&name, &maps](engine::ecs::Store &store) {
						engine::scene::RecordMaterial(store, name, maps);
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
		//
		// **And forms this deployment turned off, for the same reason one step
		// further out.** A name a scene can set and this process will refuse to
		// fetch is the same untextured part with a perfectly good string behind
		// it, arrived at by a different route.
		const engine::assets::ContentPolicy &allowed =
			engine::assets::ContentPolicy::Process(engine::assets::ContentVerb::Handle);

		std::vector<engine::core::Name> meshes;
		for (const engine::assets::AssetEntry *entry : catalogue->OfKind(engine::assets::AssetKind::Mesh)) {
			if (entry != nullptr && engine::assets::IsRuntimeReadable(entry->Name) &&
				allowed.AllowsName(entry->Name)) {
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

		// **Refused before the request and not on arrival, so the bytes never
		// cross.** A form this deployment has turned off is one nothing here
		// will decode, and fetching it anyway would spend the link on something
		// destined for a `continue`. Logged once — the insert above is what
		// makes it once — because a name that silently never arrives is exactly
		// the failure the settings layer exists to make legible.
		if (const engine::assets::ContentForm form = engine::assets::FormOfName(texture.Text());
			!engine::assets::ContentPolicy::Process(engine::assets::ContentVerb::Handle).Allows(form)) {
			ENGINE_INFO(
				"content: not asking for {} — {} content is turned off",
				texture.Text(),
				engine::assets::Describe(form)
			);
			return;
		}

		// **Queued rather than appended, because this is called from inside the
		// walk over `ContentPending`.** A mesh names its own sheets and a
		// material names its colour map, and both are read while draining that
		// vector — pushing to it there is a range-for over a container being
		// grown, which is what it looks like: the walk lost its place and one
		// texture out of the several hundred asked for arrived.
		ContentIssued.push_back(Content->Request(texture.Text()));

		// **Marked before the answer, which is the whole point.** Until this
		// request finishes, a part naming this sheet draws as the default
		// material rather than as the purple marker — so a scene load looks like
		// untextured parts becoming textured instead of a purple shimmer across
		// every imported model. See `render::ChooseTexture`.
		Renderer.ExpectTexture(texture);
	}

	void Client::PressNamedElement(engine::ecs::Store &store) {
		if (ClickFrames == 0) {
			// Done. Nothing further is synthesised, so a run may carry on and a
			// person may still use the mouse.
			return;
		}

		if (ClickFrames > 0) {
			// **The release, two frames after the press.** `gui::Router` turns a
			// press into an `Activated` on the *release* over the same element,
			// which is what a button is — so a synthetic click that never let go
			// would light the button and never fire it.
			if (--ClickFrames == 0) {
				SDL_Event released{};
				released.type = SDL_EVENT_MOUSE_BUTTON_UP;
				released.button.button = SDL_BUTTON_LEFT;
				released.button.down = false;
				released.button.x = Input.State().MousePosition.X;
				released.button.y = Input.State().MousePosition.Y;
				Input.HandleEvent(released);
				ENGINE_INFO("click: released on frame {}", FramesDrawn);
			}
			return;
		}

		// **`gui::Resolved` and not a coordinate somebody wrote down.** The
		// layout pass is what decides where an element is, and asking anything
		// else would be a second answer that goes stale the first time a padding
		// moves.
		const engine::core::Name wanted(Settings.ClickElement);
		engine::core::Vector2 centre;
		bool found = false;

		store.Each<const engine::gui::Resolved>([&](engine::ecs::Entity element,
													const engine::gui::Resolved &resolved) {
			if (found || store.InstanceNameOf(element) != wanted) {
				return;
			}
			if (resolved.AbsoluteSize.X <= 0.0f || resolved.AbsoluteSize.Y <= 0.0f) {
				// Laid out to nothing. Pressing its centre would press
				// whatever is behind it and report a success that is not one.
				return;
			}
			centre = engine::core::Vector2{
				resolved.AbsolutePosition.X + resolved.AbsoluteSize.X * 0.5f,
				resolved.AbsolutePosition.Y + resolved.AbsoluteSize.Y * 0.5f,
			};
			found = true;
		});

		if (!found) {
			return;
		}

		// Moved and then pressed, as a real pointer is: the router admits a
		// press on the element it is already over, so a press with no motion
		// before it is a press at wherever the pointer happened to be.
		SDL_Event moved{};
		moved.type = SDL_EVENT_MOUSE_MOTION;
		moved.motion.x = centre.X;
		moved.motion.y = centre.Y;
		Input.HandleEvent(moved);

		SDL_Event pressed{};
		pressed.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
		pressed.button.button = SDL_BUTTON_LEFT;
		pressed.button.down = true;
		pressed.button.x = centre.X;
		pressed.button.y = centre.Y;
		Input.HandleEvent(pressed);

		ENGINE_INFO(
			"click: pressed '{}' at {}, {} on frame {}",
			Settings.ClickElement,
			centre.X,
			centre.Y,
			FramesDrawn
		);
		ClickFrames = 2;
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

	bool Client::FindSession() {
		std::optional<network::SessionKey> key;
		if (!Settings.SessionSecret.empty()) {
			key = network::SessionKey::FromText(Settings.SessionSecret);
			if (!key) {
				key = network::SessionKey::FromPassphrase(Settings.SessionSecret);
			}
			if (!key) {
				ENGINE_ERROR("--session-key is neither 64 hex characters nor a passphrase");
				return false;
			}
		}

		std::optional<network::SessionId> wanted;
		if (!Settings.SessionIdText.empty()) {
			wanted = network::SessionId::Parse(Settings.SessionIdText);
			if (!wanted) {
				ENGINE_ERROR("--session-id is not 32 hex characters");
				return false;
			}
		}

		network::PresenceSettings presence;
		presence.Announce = false;
		presence.Discover = Settings.Browse;
		presence.RendezvousAddress = Settings.RendezvousAddress;
		presence.Protocol = engine::replication::PROTOCOL_VERSION;
		presence.Use = network::Purpose::Game;

		// The connector's socket, handed over so the punch lands on the port
		// the session will use. See `FindSession`'s declaration.
		Discovery = network::Presence::Open(presence, {}, std::nullopt, Socket.get());
		if (Discovery->Fault() != network::PresenceFault::None) {
			ENGINE_WARN("discovery: {}", network::Describe(Discovery->Fault()));
		}
		if (key) {
			// The directory holds the key so a private session on the subnet
			// verifies and lists as joinable; the reach below takes its own
			// copy for the poke.
			auto forTable = network::SessionKey::FromText(key->Text());
			if (forTable) {
				Discovery->Seen().Trust(std::move(*forTable));
			}
		}

		const bool reaching = Discovery->Rendezvousing() && wanted.has_value();
		if (reaching) {
			Discovery->Reach(*wanted, std::move(key), engine::core::Clock::Seconds());
		} else if (Discovery->Rendezvousing()) {
			// No id, so ask the point what it has. A private session is never
			// in that answer — reaching one needs its id, which is what the
			// host handed over with the key.
			Discovery->Browse(engine::core::Clock::Seconds());
		}
		if (!Settings.Browse && !Discovery->Rendezvousing()) {
			ENGINE_ERROR("--browse needs a subnet or --rendezvous needs an address; neither is usable");
			return false;
		}

		// The one blocking wait in this program, before the loop rather than
		// inside it. The connector does not exist yet, so nothing else is
		// draining the socket and the presence may take it whole.
		const double startedAt = engine::core::Clock::Seconds();
		const double deadline = startedAt + std::max(Settings.BrowseSeconds, 0.25);
		std::vector<std::byte> datagram;

		while (engine::core::Clock::Seconds() < deadline) {
			const double now = engine::core::Clock::Seconds();

			for (;;) {
				const engine::net::Transport::Inbound inbound = Socket->Receive(datagram);
				if (inbound.Status != engine::net::TransportStatus::Ok) {
					break;
				}
				// Anything that is not the discovery module's is dropped. There
				// is no server yet to have sent one, so a packet here is a
				// stray from a previous run or somebody probing the port.
				Discovery->Deliver(datagram, inbound.From, now);
			}
			Discovery->Pump(now);

			if (reaching) {
				if (Discovery->Reaching() == network::ReachState::Reached) {
					Settings.ConnectAddress = Discovery->Reached().Text();
					ENGINE_INFO("reached session {} at {}", wanted->Text(), Settings.ConnectAddress);
					return true;
				}
				if (Discovery->Reaching() == network::ReachState::Failed) {
					ENGINE_ERROR(
						"could not reach session {} through {}", wanted->Text(), Settings.RendezvousAddress
					);
					return false;
				}
			} else {
				for (const network::Listing &row : Discovery->Seen().Listings()) {
					if (!row.Joinable() || !row.Dial().IsValid()) {
						continue;
					}
					if (wanted && row.Session.Session != *wanted) {
						continue;
					}
					if (!Settings.SessionName.empty() && row.Session.Name != Settings.SessionName) {
						continue;
					}
					Settings.ConnectAddress = row.Dial().Text();
					ENGINE_INFO(
						"found \"{}\" ({}, {}) at {}",
						row.Session.Name,
						network::Describe(row.Via),
						network::Describe(row.Session.Admits),
						Settings.ConnectAddress
					);
					return true;
				}
			}

			// A tenth of the beacon's interval: short enough that the first
			// announcement is acted on without waiting out a poll, long enough
			// that this is not a spin.
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}

		// Said out loud rather than falling through to single-player. A client
		// that meant to join and quietly ran the demo alone looks exactly like
		// a server that is broken.
		if (Discovery->Seen().Listings().empty()) {
			ENGINE_ERROR("no session found in {:.1f}s", Settings.BrowseSeconds);
		} else {
			ENGINE_ERROR(
				"{} session(s) found in {:.1f}s and none was joinable",
				Discovery->Seen().Listings().size(),
				Settings.BrowseSeconds
			);
		}
		return false;
	}

	bool Client::BeginConnecting() {
		const bool searching =
			Settings.ConnectAddress.empty() && (Settings.Browse || !Settings.RendezvousAddress.empty());
		if (Settings.ConnectAddress.empty() && !searching) {
			return true;
		}

		// Let the OS choose the client port so multiple clients can coexist.
		//
		// **Opened before the search, not after it.** The search punches on
		// this socket, and a hole punched on any other one is a hole to a port
		// the server will never send to.
		Socket = engine::net::MakeUdpTransport(0);
		if (Socket == nullptr) {
			ENGINE_ERROR("could not open a socket to connect from");
			return false;
		}

		if (searching && !FindSession()) {
			return false;
		}

		const std::optional<engine::net::Endpoint> server =
			engine::net::Endpoint::Parse(Settings.ConnectAddress);
		if (!server.has_value()) {
			ENGINE_ERROR("--connect '{}' is not a host:port", Settings.ConnectAddress);
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

		std::shared_ptr<engine::script::Runtime> replicaScripts;

		Universe_->Enter(
			Replicated, [this, &replicaScripts](engine::ecs::Store &store, engine::ecs::Scheduler &systems) {
				// The v0.2 refusal, used for what it was reserved for. A replica
				// that published to a bus would be telling the universe
				// something the server never said; the inbox still delivers,
				// which is how it receives.
				store.SetResource(engine::world::Replica{true});

				// **Said here rather than left to the first `Connector::Poll`,
				// because something now builds instances between the two.**
				// `BuildReplicatedWorld` opens a VM and installs `GuiService`,
				// and both of those ask the store whether minting is legal — an
				// authoritative index taken in the window before the connector
				// spoke would be one the server is also handing out. The
				// connector still sets it, idempotently, for a store it was
				// handed by some other route.
				store.SetAdoptOnly(true);

				// A replicated world simulates nothing: everything in it
				// arrived, and stepping it here would be this process
				// disagreeing with the authority once per tick. What this
				// installs is the `PreRender` half — the draw list and the
				// system that fills it — and, since v0.15, a VM whose scripts
				// are the ones this client may run out of somebody else's
				// world.
				//
				// **The rate the snapshot buffer measures its delay against is
				// the server's, and nothing on the wire carries it.** What is
				// passed is the rate this process was told to run at, which is
				// the same default both programs take. A disagreement is
				// absorbed by the buffer's own correction up to a few percent,
				// and past that shows as `replica.stalls` rather than as
				// something mysterious.
				engine::replication::InterpolationSettings interpolation;
				interpolation.TickRate = Settings.TickRate;

				replicaScripts = BuildReplicatedWorld(store, systems, interpolation);
			}
		);

		// **Named here, which is the whole of what `DeliverGuiEvents` needs.**
		// The scheduler holds a reference and drops it with the world — that is
		// the lifetime and it has not changed — but a VM nothing can *name* is
		// one no press can be handed to, which is the shape the demo path had
		// until v0.14 and the replicated path had until now.
		if (replicaScripts != nullptr) {
			Runtimes.emplace_back(Replicated, std::move(replicaScripts));
		}

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

		// **Which player is this client's, which nothing else can tell it.**
		// `scene::LocalPlayer` cannot be replicated — a resource is one row and
		// the answer differs per client — so the host says it once over the user
		// channel. `game/Play.hpp` carries the whole argument.
		Connection->OnUserMessage([this](std::span<const std::byte> message) {
			engine::game::JoinNotice notice;
			if (!engine::game::DecodeJoinNotice(message, notice)) {
				// Somebody else's message on a shared channel. Ignored rather
				// than counted: the tag exists so that this is a non-event.
				return;
			}

			Universe_->Enter(Replicated, [notice](engine::ecs::Store &store) {
				store.SetResource(engine::scene::LocalPlayer{notice.Player});
			});

			ENGINE_INFO("client: this is player {}", notice.Player.Id);
		});

		if (Discovery != nullptr) {
			// The connector owns the drain from here. Rendezvous traffic keeps
			// arriving on this socket — the registration this client's peer is
			// repeating, and the pokes that keep the mapping alive — so it is
			// routed back rather than counted as a refusal.
			Connection->SetForeign(
				[this](std::span<const std::byte> datagram, const engine::net::Endpoint &from) {
					return Discovery != nullptr && Discovery->Deliver(datagram, from, DiscoveryNow);
				}
			);
		}

		ENGINE_INFO("connecting to {} from {}", server->Text(), Socket->Local().Text());
		return true;
	}

	engine::world::WorldId Client::InterfaceWorld() const {
		// Gated on the join rather than on the connection, because a `PlayerGui`
		// is a subtree of a `Player` and neither exists until the world has
		// arrived. Before that there is nothing there to lay out, and the local
		// scene's own interface is still the only one a person can press.
		if (ReportedJoin && Replicated.IsValid()) {
			return Replicated;
		}
		return Rendered;
	}

	engine::script::Runtime *Client::RuntimeOf(engine::world::WorldId world) {
		for (const auto &[id, runtime] : Runtimes) {
			if (id == world) {
				return runtime.get();
			}
		}
		return nullptr;
	}

	void Client::WriteInput(engine::ecs::Store &store) {
		auto *state = store.ResourceMutable<engine::scene::InputState>();
		if (state == nullptr) {
			return;
		}

		// **The behaviour is read back, not overwritten.** A script sets
		// `UserInputService.MouseBehavior` and the client applies it to the
		// window; copying the whole translator over the resource would throw
		// that away every frame. `scene::InputState` is the seam in both
		// directions.
		// **Three fields survive the overwrite, for two different reasons.** The
		// pointer mode and whether the cursor is drawn travel the other way — a
		// script writes them and the window obeys — and the tap latch is *older
		// than this frame* by design: it is what a key pressed between two ticks
		// is remembered in, and `Input.State()` only knows about the frame it
		// just read.
		const engine::scene::MouseBehavior wanted = state->Behaviour;
		const bool wantedIcon = state->MouseIconEnabled;
		const engine::scene::KeyBits taps = state->Pressed;

		// **The button latch survives for the key latch's reason**, and it was
		// the half that did not exist until something fired a shot: the
		// translator builds a state from the frame it just read, so a click
		// latched two frames ago and not yet consumed by a tick is in the
		// resource and nowhere else.
		const uint8_t buttonTaps = state->PressedButtons;

		*state = Input.State();
		state->Behaviour = wanted;
		state->MouseIconEnabled = wantedIcon;
		state->Pressed = taps;
		state->PressedButtons = buttonTaps;
		PointerMode = wanted;
		PointerIconEnabled = wantedIcon;

		// Latched here, where every frame is seen. This used to set a private
		// `PendingJump` on the client, which `SubmitMove` then had to merge back
		// in by hand — jump was the one key that did not travel through
		// `scene::InputState`. `LatchPresses` is that latch for every key, in
		// the state both the client and the studio already share.
		state->LatchPresses();
	}

	void Client::SubmitMove(double nowSeconds) {
		if (Connection == nullptr || !Connection->Admitted()) {
			return;
		}

		engine::game::MoveInput move;
		engine::scene::AimIntent aim;
		uint64_t tick = 0;

		Universe_->Enter(Replicated, [this, &move, &aim, &tick](engine::ecs::Store &store) {
			// **Only once there is a body**, because until the join notice
			// arrives and the character replicates there is nothing for a move
			// to mean — and a host that received one would look up a player,
			// find no character, and do the work of deciding that every tick.
			if (const auto *local = store.Resource<engine::scene::LocalPlayer>();
				local == nullptr ||
				engine::scene::CharacterOf(store, local->Instance) == engine::ecs::NULL_ENTITY) {
				return;
			}

			// The same arithmetic a single-player character uses, which is what
			// `scene::ReadMoveIntent` was split out for: W has to mean "away
			// from the camera" on a client exactly as it does everywhere else,
			// and the camera it is relative to is this world's.
			const engine::scene::MoveIntent intent = engine::scene::ReadMoveIntent(store);
			move.Direction = intent.Direction;
			move.Jump = intent.Jump;

			// **Read in the same scope as the move, from the same tick.** Two
			// entries would sample the camera on one tick and the keys on
			// another, so a shot fired while turning would be aimed a tick
			// behind where the player was looking — which on a fast flick is
			// several degrees and reads as the server cheating.
			aim = engine::scene::ReadAimIntent(store);

			tick = store.Time().Tick;
		});

		if (tick == 0) {
			return;
		}

		// **The aim, and never the result.** `Server::ApplyInputs` states the
		// division and this is the half that had no caller: `examples::
		// EncodeShot` existed with a complete server-side rewind and hit test
		// behind it, and nothing in the tree had ever sent one — `D00109`
		// records that as two finished halves connected to nothing.
		//
		// Sent before the move because the two share a channel and a shot is
		// the one a dropped tick cannot be re-derived from: a move repeats every
		// tick by design, and a click happens once.
		if (aim.Aimed && aim.Fired) {
			engine::examples::Shot shot;
			shot.Aim = aim.Ray;

			// **The engine's ceiling and not a game's choice.** A client picks
			// its own range on the wire and `DecodeShot` refuses anything past
			// `MAXIMUM_SHOT_RANGE`, so sending the ceiling is sending the most
			// an honest client is allowed to — a game with a shorter weapon
			// shortens it here and the host still bounds it.
			shot.Range = engine::examples::MAXIMUM_SHOT_RANGE;

			if (Connection->Submit(tick, engine::examples::EncodeShot(shot), nowSeconds)) {
				// **Cleared once the click is actually on the wire**, and
				// separately from the jump below: a shot refused by the send
				// budget while the move went through would otherwise forget a
				// click nobody was ever told about.
				Universe_->Enter(Replicated, [](engine::ecs::Store &store) {
					if (auto *input = store.ResourceMutable<engine::scene::InputState>(); input != nullptr) {
						input->ConsumeButtonTaps();
					}
				});
			}
		}

		// **Sent every tick, including the still ones.** A client that only
		// spoke when its keys changed would leave a character walking for ever
		// after a dropped release — an input channel is unreliable by design,
		// and "still walking" is the failure a state-change protocol produces.
		if (Connection->Submit(tick, engine::game::EncodeMoveInput(move), nowSeconds)) {
			// **Cleared once the tap is actually on the wire**, and not when it
			// was read. A submission that failed has not told the server
			// anything, and forgetting the jump there is the dropped press this
			// latch exists to prevent.
			Universe_->Enter(Replicated, [](engine::ecs::Store &store) {
				if (auto *input = store.ResourceMutable<engine::scene::InputState>(); input != nullptr) {
					input->ConsumeKeyTaps();
				}
			});
		}
	}

	void Client::PollServer(double nowSeconds) {
		if (Discovery != nullptr) {
			// Before the connector's drain, so a rendezvous message routed out
			// of it is stamped with this tick rather than the previous one.
			DiscoveryNow = nowSeconds;
			Discovery->Pump(nowSeconds);
		}

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
		if (Window != nullptr &&
			(PointerMode != AppliedPointerMode || PointerIconEnabled != AppliedPointerIcon)) {
			const bool relative = PointerMode == engine::scene::MouseBehavior::LockCenter;
			SDL_SetWindowRelativeMouseMode(Window, relative);

			// **`LockCurrentPosition` hides the pointer whatever
			// `MouseIconEnabled` says, and relative mode owns it outright.** The
			// two properties overlap and the mode is the stronger claim: a
			// drag-to-rotate wants the pointer gone and back where it started when
			// the drag ends, which is why it hides without warping where relative
			// mode would warp to the centre. `MouseIconEnabled` is what decides
			// every other case, which is the one Roblox uses it for — a cutscene
			// with the pointer free and invisible.
			const bool hidden = relative ||
								PointerMode == engine::scene::MouseBehavior::LockCurrentPosition ||
								!PointerIconEnabled;
			if (hidden) {
				SDL_HideCursor();
			} else {
				SDL_ShowCursor();
			}

			AppliedPointerMode = PointerMode;
			AppliedPointerIcon = PointerIconEnabled;
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

			// **After the poll, so a move is stamped with the tick this client
			// has just finished receiving** — a submission tagged with a tick
			// the server has not reached is one it rewinds against nothing.
			SubmitMove(engine::core::Clock::Seconds());
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
				Universe_->Enter(id, [this](engine::ecs::Store &store) { WriteInput(store); });

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
						// **The holes first, because they claim slots the
						// surfaces then leave alone.** A same-world portal is
						// drawn by the recursive pass from a camera derived from
						// this one; a surface camera aimed at the same pane
						// would be a second answer taken from the eye.
						(void)CollectPortalViews(store, Portals);
						(void)CollectSurfaceViews(store, Surfaces, Portals);

						// **Whether any pane here names another world**, asked
						// while the world is open because that is the only place
						// it is cheap. What it gates is a whole copy of the draw
						// list, and a scene with no window in it must not pay
						// for one.
						Windowed = false;
						store.Each<const engine::scene::Portal>(
							[this](engine::ecs::Entity, const engine::scene::Portal &portal) {
								Windowed = Windowed || portal.DestinationWorld.IsValid();
							}
						);

						// TODO(render-pipeline): the world's own pipeline was
						// installed here, on a world change rather than per
						// frame. `PipelinesInstalledFor` guarded that and
						// `PipelineSelected` carried the answer to the render
						// call below — both members are still declared and both
						// are marked. See `client::InstallWorldPipelines`.

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
					// **The replica takes this frame's input like a simulated
					// world does**, because since v0.14 it has a camera and a
					// character of its own to drive. It is not in `Simulated` —
					// it never will be, nothing here is stepped — so it is
					// written here rather than by widening that loop to mean
					// something other than what it says.
					WriteInput(store);

					(void)AimReplicaViewer(store, ComposedFrame, ComposedCamera);
				});

				Universe_->Present(Replicated, delta, Universe_->AlphaOf(Replicated));

				Universe_->Enter(Replicated, [this](engine::ecs::Store &store) {
					const auto *list = store.Resource<DrawList>();
					if (list == nullptr) {
						return;
					}

					// **Read back rather than assumed**, which folds two cases
					// into one: with no character the replica's camera *is*
					// `ComposedFrame`, because that is what `AimReplicaViewer`
					// just put there, and with one it is where the player is
					// standing. Publishing the composed frame unconditionally
					// was the version that showed the local scene's viewpoint
					// over the server's world.
					engine::core::CFrame frame = ComposedFrame;
					engine::scene::Camera lens = ComposedCamera;

					if (const auto *active = store.Resource<engine::scene::ActiveCamera>()) {
						const auto *placement = store.Get<engine::scene::Transform>(active->Entity);
						const auto *found = store.Get<engine::scene::Camera>(active->Entity);
						if (placement != nullptr && found != nullptr) {
							frame = placement->Frame;
							lens = *found;
						}
					}

					Views.Publish(
						Replicated, frame, lens, list->Instances, store.Time().Tick, store.Time().Alpha
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

		// **The requested size when there is no window to ask.** A headless run
		// still lays the panels out and still renders into an offscreen target
		// of this size, so the numbers have to come from somewhere — and
		// `--width`/`--height` is what a caller asked for.
		int pixelWidth = Settings.Width;
		int pixelHeight = Settings.Height;
		if (Window != nullptr) {
			SDL_GetWindowSizeInPixels(Window, &pixelWidth, &pixelHeight);
		}

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

		// **Whether the window should be listening for text, decided from the
		// world.** Read inside the `Enter` below and applied outside it, because
		// the answer is the world's and the call is the window's.
		bool wantsTextInput = false;

		// **Whose interface this is, which is not always the world in front.** A
		// connected client draws its local scene *and* the server's; the
		// `PlayerGui` a person can press lives under their own `Player`, which is
		// a row in the replica. Compiling the local world and routing the press
		// into it is what made every button in a replicated world silent — the
		// press was picked correctly, produced the right event, and was handed to
		// a VM that was not the one the button's script was in.
		const engine::world::WorldId interfaceWorld = InterfaceWorld();

		if (interfaceWorld.IsValid()) {
			engine::gui::CompileRequest request;
			std::span<const engine::gui::GuiEvent> interfaceEvents;

			// **What Return did, which the router cannot produce.** Releasing the
			// focus with a key is not a press, so nothing is picked and
			// `Router::Update` names no element — `gui::TypeResult::Released` is
			// the only record of it, and a script's `FocusLost` is owed one
			// however the focus went away.
			std::vector<engine::gui::GuiEvent> typedEvents;

			request.Display.Width = static_cast<float>(Settings.Width);
			request.Display.Height = static_cast<float>(Settings.Height);

			// Fed back from the previous frame's routing, deliberately: the
			// hover comes from the list a compile produced, so a compile
			// reading this frame's hover would depend on its own output.
			request.Hovered = InterfaceRouter.Hovered();
			request.Pressed = InterfaceRouter.Pressed();

			Universe_->Enter(interfaceWorld, [&](engine::ecs::Store &store) {
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

				// **This frame's characters, and before the press that may move
				// the focus.** They were produced by a keyboard aimed at whichever
				// box held it when they arrived; routing first would post them
				// into the box the person is only now clicking on.
				//
				// **The synthetic keystroke, and it goes in before the frame reads
				// what was typed.** `--type` is `--click`'s diagnostic for the
				// keyboard and needs it to have run first, because only a press
				// moves the focus. It enters as an SDL event so the translator
				// stays the only thing in the engine that decodes one —
				// `Options::TypedText` says what this does and does not cover.
				if (!Settings.TypedText.empty() && !TypedTextSent &&
					engine::gui::FocusedTextBox(store) != engine::ecs::NULL_ENTITY) {
					TypedTextSent = true;

					SDL_Event typedEvent{};
					typedEvent.type = SDL_EVENT_TEXT_INPUT;
					typedEvent.text.text = Settings.TypedText.c_str();
					Input.HandleEvent(typedEvent);

					ENGINE_INFO("type: sent '{}' on frame {}", Settings.TypedText, FramesDrawn);
				}

				// **Straight from the translator into `Label::Text`, with nothing
				// in between.** `Translate.hpp` keeps the string on the translator
				// rather than on `scene::InputState` because that resource is
				// trivially copyable and a `std::string` on it buys a hand-written
				// serialiser, so this is the one hop it makes — into the world,
				// where rule 2 says the text lives.
				engine::gui::Typing typing;
				typing.Text = Input.TypedText();
				typing.Backspace = Input.State().WasKeyPressed(engine::scene::KeyCode::Backspace);
				typing.Submit = Input.State().WasKeyPressed(engine::scene::KeyCode::Return);
				typing.Extend = Input.State().IsKeyDown(engine::scene::KeyCode::LeftShift) ||
								Input.State().IsKeyDown(engine::scene::KeyCode::RightShift);

				// **One step per press and not one per repeat**, because
				// `input::Translator` drops SDL's key repeats deliberately — a
				// held arrow moves the caret once until something records how many
				// times it fired.
				if (Input.State().WasKeyPressed(engine::scene::KeyCode::Left)) {
					typing.Caret = -1;
				} else if (Input.State().WasKeyPressed(engine::scene::KeyCode::Right)) {
					typing.Caret = 1;
				}

				if (const engine::gui::TypeResult typed = engine::gui::Type(store, typing); typed.Released) {
					engine::gui::GuiEvent released;
					released.Kind = engine::gui::EventKind::FocusReleased;
					released.Instance = typed.Instance;
					released.Entered = true;
					typedEvents.push_back(released);
				}

				// **The hit test, which a shipped client did not do at all.** The
				// router was constructed, read for `Hovered` and `Pressed`, and
				// never `Update`d — so a `TextButton` in a game never lit up and
				// its `Activated` never fired, while the same tree worked in the
				// editor because `studio::Overlay` drives a router of its own.
				// A button that does nothing in the shipped build and works in
				// the editor is the worst version of this bug, because it is
				// invisible to whoever is authoring.
				//
				// **Against the list that was just compiled**, and the hover it
				// fed that compile is deliberately the previous frame's — see
				// `Router::Hovered`, which explains why the one-frame loop is the
				// alternative to a compile that depends on its own output.
				engine::gui::Pointer pointer;
				pointer.Position = Input.State().MousePosition;
				pointer.Down = Input.State().IsButtonDown(engine::scene::MouseButton::Left);

				// **Focus and not a rectangle test.** The position is already in
				// window pixels, and a pointer that has left the window stops
				// producing motion — what it does *not* do is end the hover,
				// which is what this flag is for.
				pointer.Inside = Input.State().Focused;

				// **Before the router reads the pointer, so the press is this
				// frame's.** Synthesising after would put the click one frame
				// behind the hover that admits it, which is the one-frame loop
				// `Router::Hovered` already warns about arriving from the other
				// direction.
				if (!Settings.ClickElement.empty() && !InterfaceList.Commands().Commands.empty()) {
					PressNamedElement(store);
					pointer.Position = Input.State().MousePosition;
					pointer.Down = Input.State().IsButtonDown(engine::scene::MouseButton::Left);
				}

				interfaceEvents = InterfaceRouter.Update(store, InterfaceList.Commands(), pointer);

				// **After the router, because this frame's press is what may have
				// changed it.** A click that lands on a box has to reach the
				// window on the frame it happened, or the first character somebody
				// types is the one SDL was never asked for.
				wantsTextInput = engine::gui::FocusedTextBox(store) != engine::ecs::NULL_ENTITY;
			});

			// **Outside the world's lock, because it reaches a VM.** The span
			// points into the router's own vector, which is a member and is only
			// rewritten by the next `Update`; `DeliverGuiEvents` copies.
			if (!typedEvents.empty() || !interfaceEvents.empty()) {
				if (engine::script::Runtime *runtime = RuntimeOf(interfaceWorld); runtime != nullptr) {
					// Typing first, because it happened first — the keystroke is
					// applied above the routing for the reason stated there.
					if (!typedEvents.empty()) {
						runtime->DeliverGuiEvents(typedEvents);
					}
					if (!interfaceEvents.empty()) {
						runtime->DeliverGuiEvents(interfaceEvents);
					}
				}
			}

			if (!InterfaceList.Commands().Commands.empty()) {
				Interface.Submit(
					InterfaceList.Commands(),
					engine::core::Vector2{request.Display.Width, request.Display.Height}
				);
				hook = &Interface;
			}
		}

		// **SDL is asked for text only while a `TextBox` has the keyboard, and
		// that is a decision rather than a saving.** Text input is not a stream
		// somebody switches on to receive characters — it is what raises the
		// on-screen keyboard on a phone and opens an input method's composition
		// window on a desktop, and both of those cover the game. A client that
		// started it once and left it on would be a game with a keyboard over it
		// on every platform that has one, which is why SDL makes a host ask at
		// all.
		//
		// **`gui::FocusedTextBox` is the switch, and being a lookup is exactly
		// what makes it usable as one.** The fact rests in the world, one module
		// decides it and this reads it — no flag here mirrors it, which is the
		// same refusal `WriteInput`'s three surviving fields are about.
		//
		// **Compared before it is called**, for the reason the pointer mode above
		// is: this is a window-manager round trip on some platforms and a client
		// that made it every frame would pay for it every frame to say what it
		// already said.
		if (Window != nullptr && wantsTextInput != TextInputActive) {
			TextInputActive = wantsTextInput;

			// **Logged because nothing else can see it.** Whether the platform
			// accepted the request is SDL's answer and there is no test that can
			// ask — a headless run has no input method to raise — so the one
			// record that the call was made on the frame the focus changed is
			// this line. `--type` synthesises the event SDL would have sent and
			// therefore proves the hop after this one, never this one.
			const bool accepted = wantsTextInput ? SDL_StartTextInput(Window) : SDL_StopTextInput(Window);
			ENGINE_INFO(
				"text input: {} — SDL {}", wantsTextInput ? "on" : "off", accepted ? "ok" : SDL_GetError()
			);
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

		// **The worlds a pane looks into, and the bodies standing in both mouths
		// of it.** This is the step the standalone client never had: the studio
		// has called `AttachForeignSurfaces` since cross-world panes existed and
		// this loop handed the renderer an empty foreign span, so a
		// `DestinationWorld` pane fell back to showing its own world — a mirror
		// where a window was authored.
		//
		// **Outside every `Enter`, because it enters other worlds** and
		// `Universe::Enter` is not re-entrant. The studio's own call carries the
		// same note for the same reason.
		//
		// **And no `PresentPortalDestinations` beside it, unlike the studio.**
		// That step exists there because a panel presents only the world it
		// shows, so a far world's draw list was whatever it held last time
		// somebody looked at it. This loop presents *every* simulated world
		// already — a world the player is not looking at still ticks — so the
		// far list is this frame's by the time we get here.
		Foreign.clear();
		std::span<const engine::scene::DrawInstance> drawn = Views.Instances();

		if (Windowed) {
			// The copy `Drawn`'s comment argues for: the published list is
			// `const` and the return leg has to go somewhere.
			Drawn.assign(drawn.begin(), drawn.end());
			(void)AttachForeignSurfaces(*Universe_, Rendered, Drawn, Foreign, Surfaces);
			drawn = Drawn;
		}

		// **The world's sun, pushed before the frame that shades with it.** It is
		// a knob rather than an argument for `SetPortalDepth`'s reason, so this is
		// where a world's `scene::Sun` reaches the renderer — every frame, because
		// a resource a script may write is one a script may write at any time and
		// two floats compared per frame is cheaper than anything that would notice
		// when it changed.
		//
		// **`SunOf` rather than the resource**, so a world that has never set one
		// draws with the numbers this engine has always drawn with rather than
		// with black.
		// **And how deep its mirrors go, for the sun's reason exactly.** It is a
		// property of the world being drawn rather than of the process, so it
		// arrives here rather than at startup: two worlds in one session are two
		// different scenes, and the corridor of facing panes that wants three
		// levels is not the room with one mirror in it that wants one.
		//
		// **`--surface-bounces` wins where it was given.** A session pinning the
		// number is somebody measuring or comparing, and a world quietly taking
		// it back on the next frame is the shape of an afternoon lost.
		Universe_->Enter(Rendered, [this](engine::ecs::Store &lit, engine::ecs::Scheduler &) {
			const engine::scene::Sun sun = engine::scene::SunOf(lit);
			Renderer.SetSun(sun.Direction, sun.Ambient);

			const int32_t bounces = Settings.SurfaceBounces > 0
										? static_cast<int32_t>(Settings.SurfaceBounces)
										: engine::scene::SurfaceBouncesOf(lit);
			Renderer.SetSurfaceBounces(static_cast<uint32_t>(std::max(bounces, 0)));
		});

		// **The shaders this world's materials name, resolved before the frame
		// that draws with them.** Beside the sun and for the same reason: a
		// script may select one at any time, so this runs every frame — and on a
		// world nobody is editing it is one walk over the materials and an
		// integer compare per distinct shader, which is what
		// `ShaderSource::Revision` exists to make possible.
		//
		// **Only for the world being drawn.** A shader is a pipeline, and a
		// pipeline built for a world nothing is presenting is video memory held
		// for a frame that is not being rendered.
		Universe_->Enter(Rendered, [this](engine::ecs::Store &shaded, engine::ecs::Scheduler &) {
			if (Shaders.Refresh(shaded) == 0) {
				return;
			}

			// **Only what moved.** `Changed` is the whole reason a refresh
			// returns a count rather than a bool: rebuilding every pipeline
			// every frame is what this loop exists not to do.
			for (const engine::core::Name &name : Shaders.Changed()) {
				const engine::render::ShaderModule *module = Shaders.Find(name);

				// Null is a name nothing asks for any more, so whatever was
				// built for it is released. The instances that named it are
				// already gone or already name something else.
				if (module == nullptr) {
					(void)Renderer.DropShader(name);
					continue;
				}

				// **A warning and not a fatal, and the part goes on drawing with
				// the engine's own shader.** `render/AGENTS.md` is explicit that
				// a user shader failing is a diagnostic string — the built-in
				// ones fail the build instead, which is where that belongs.
				if (!module->Error.empty()) {
					ENGINE_WARN("shader '{}': {}", name.Text(), module->Error);
					continue;
				}

				(void)Renderer.AddShader(name, module->SpirV);
			}
		});

		// TODO(render-pipeline): this call took a `render::View` per camera.
		//
		// The old system's `Render` takes one camera's worth of arguments
		// positionally; the one being replaced took `std::span<const View>`, so a
		// frame could carry several cameras and each could name **its own
		// pipeline** — `view.World` and `view.Pipeline` were set together here,
		// because the pipeline key a world installs is qualified by the world id
		// and a view naming one without the other asks for a pipeline nothing
		// installed.
		//
		// **The two members that fed it are still on this class**, unused, and
		// marked: `PipelinesInstalledFor` and `PipelineSelected`. They are where
		// a world's saved pipelines were installed and which one this view
		// selected. See `client::InstallWorldPipelines` in `Scene.hpp`.
		LastFrame = Renderer.Render(
			Views.CameraFrame(),
			Views.Camera(),
			drawn,
			Overlay,
			Surfaces,
			hook,
			sceneTarget,
			0,
			Particles,
			RibbonVertices,
			RibbonRuns,
			Lights,
			Foreign,
			Portals
		);

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

		// **Presented, or simply drawn when there is nowhere to present.** A
		// headless renderer never presents by design, so counting presents alone
		// would leave `--frames` unreachable and a run that nobody is watching
		// would never end — which is the one failure mode a build server cannot
		// recover from. The editor makes the same allowance for the same reason.
		if (LastFrame.Presented || Settings.Headless) {
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

		// **The three that only mean anything as a series.** A batch count says
		// whether the interface is being rebuilt into more draws than it needs;
		// the two compositor figures are the ones that say a run is heading for
		// trouble rather than in it — drops climbing is a compositor behind its
		// producers, and growths climbing is a draw list with no upper edge.
		// Counted rather than merely available, because a number nobody reads
		// answers no question.
		Metrics::Count("ui.batches", static_cast<double>(Interface.LastBatchCount()));
		Metrics::Count("view.drops", static_cast<double>(Views.Dropped()));
		Metrics::Count("view.growths", static_cast<double>(Views.Growths()));

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
					"{} surface pass(es), {} portal pass(es)",
					Clock.Now(),
					FramesDrawn,
					LastFrame.DrawCalls,
					LastFrame.Culled,
					LastFrame.SurfaceInstances,
					LastFrame.SurfacePasses,
					LastFrame.PortalPasses
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
