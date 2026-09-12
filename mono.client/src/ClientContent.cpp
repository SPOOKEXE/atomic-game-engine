#include <engine/assets/Animation.hpp>
#include <engine/assets/ContentForm.hpp>
#include <engine/assets/Manifest.hpp>
#include <engine/assets/Material.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/game/CollisionContent.hpp>
#include <engine/render/Animation.hpp>
#include <engine/render/AutomaticMeshLod.hpp>
#include <engine/scene/CollisionShapes.hpp>
#include <engine/scene/Materials.hpp>
#include <engine/scene/MeshCatalogue.hpp>
#include <engine/scene/PublishedCatalogue.hpp>
#include <engine/scene/TextureCatalogue.hpp>

#include <client/Client.hpp>
#include <client/ContentDemand.hpp>

namespace client {
	static uint64_t ContentRequestKey(engine::core::Name owner, engine::core::Name asset) {
		return (uint64_t(owner.Id()) << 32) | asset.Id();
	}

	void Client::RefreshContentBindings() {
		ContentBindings.clear();
		Universe_->EachWorld([&](engine::world::WorldId world) {
			if (Universe_->IsRemote(world)) return;
			const auto owner = Universe_->NameOf(world);
			ContentBindings.push_back({owner, owner});
			Universe_->Enter(world, [&](engine::ecs::Store &store) {
				const auto *replica = store.Resource<engine::world::Replica>();
				if (replica && replica->Of.IsValid() && replica->Of != owner)
					ContentBindings.push_back({replica->Of, owner});
			});
		});
		if (!PortalImages) return;
		Universe_->EachWorld([&](engine::world::WorldId world) {
			if (!Universe_->IsRemote(world))
				PortalImages->SetContentOwner(world, Universe_->NameOf(world), ContentBindings);
		});
	}

	void Client::PumpContent() {
		RefreshContentBindings();
		ContentBudget.Begin();
		ContentWorlds.assign(Simulated.begin(), Simulated.end());
		if (ReportedJoin) ContentWorlds.push_back(Replicated);
		PumpContent(*ContentState, ContentWorlds);
		if (PortalPrevious) PumpContent(*PortalPrevious->Content, std::span(&PortalPrevious->World, 1));
		if (PortalNext && PortalNext->Content && PortalNext->World.IsValid())
			PumpContent(*PortalNext->Content, std::span(&PortalNext->World, 1));
	}

	bool Client::BuildContentClient(ContentSession &content) {
		engine::delivery::DeliverySettings settings;
		settings.CachePath = Settings.ContentCache;
		settings.AllowedHosts = Settings.ContentAllowedHosts;
		settings.Sources = MergeContentSources(
			Settings.ContentSources,
			content.OfferedSources,
			content.Relay ? std::string_view(content.RelayName) : std::string_view()
		);

		if (settings.Sources.empty()) {
			// Nobody named anything and there is no link. The origin on this
			// machine is the historical default and stays it.
			settings.Sources = engine::delivery::DeliverySettings::Default(Settings.ContentCache).Sources;
		}

		// **A key pinned here is kept and a server's is only ever a fallback.** A
		// pinned client refuses rather than downgrades, which is the same position
		// `ConnectorSettings::ServerIdentity` takes one module along.
		const std::string &publisher =
			Settings.ContentPublisherKey.empty() ? content.PublisherKey : Settings.ContentPublisherKey;
		if (const auto key = engine::assets::PublicKey::FromHex(publisher)) {
			settings.Publisher = *key;
		} else {
			ENGINE_ERROR("content delivery needs --publisher-key, 64 hex characters");
			ENGINE_ERROR("a client that accepted an unsigned manifest would have no trust boundary");
			return false;
		}

		std::unique_ptr<engine::delivery::AssetClient> built =
			engine::delivery::MakeAssetClient(settings, content.Relay.get());
		if (!built) {
			return false;
		}
		if (!content.Grant.empty()) {
			built->UseGrant(content.Grant);
		}

		content.Client = std::move(built);

		// **Everything asked for is asked for again.** A rebuilt client holds none
		// of the previous one's requests, and the demand walk only asks for what
		// it has not asked for - so a set left behind would leave every texture
		// this client already wanted permanently unrequested.
		content.Requested = false;
		content.Reported = false;
		content.Pending.clear();
		content.Issued.clear();
		content.Asked.clear();
		content.ScannedAtRevision.clear();

		ENGINE_INFO(
			"content: {} source(s), first is '{}'", settings.Usable().size(), settings.Usable().front().Name
		);
		return true;
	}

	void
	Client::AdoptContentDirectory(ContentSession &content, const engine::game::ContentDirectory &directory) {
		const OfferedContent accepted = AcceptOfferedContent(directory, Settings.ContentAllowedHosts);
		if (accepted.RefusedByAllowList != 0) {
			ENGINE_WARN(
				"content: this client's allow-list refused {} origin(s) the server named",
				accepted.RefusedByAllowList
			);
		}
		if (accepted.UnresolvedNames != 0) {
			// **One warning rather than a stream of failures.** `Endpoint::Parse`
			// refuses a host *name* on purpose - resolving one blocks on a network
			// service and nothing on the fetch path may block - so a server that
			// named one has a configuration problem, and saying it once at the
			// door is where it is useful.
			ENGINE_WARN(
				"content: the server named {} origin(s) by host name rather than address - a name has to be "
				"resolved before it gets here, so they were skipped",
				accepted.UnresolvedNames
			);
		}

		content.OfferedSources = accepted.Permitted;
		content.PublisherKey = directory.PublisherKey;
		content.Grant = directory.Grant;

		if (!BuildContentClient(content)) {
			ENGINE_WARN("content: what the server named could not be used");
		}
	}

	void Client::PumpContent(ContentSession &content, std::span<const engine::world::WorldId> worlds) {
		std::vector<engine::world::WorldId> addedWorlds;
		for (const auto world : worlds) {
			const auto owner = Universe_->NameOf(world);
			if (std::find(content.Owners.begin(), content.Owners.end(), owner) == content.Owners.end())
				addedWorlds.push_back(world);
		}
		if (content.Client) {
			for (const auto owner : content.Owners) {
				if (std::any_of(worlds.begin(), worlds.end(), [&](auto world) {
						return Universe_->NameOf(world) == owner;
					}))
					continue;
				for (const auto id : content.Pending)
					Renderer.StopExpectingTexture(engine::core::Name(content.Client->NameOf(id)), owner);
				for (const auto id : content.Issued)
					Renderer.StopExpectingTexture(engine::core::Name(content.Client->NameOf(id)), owner);
				std::erase_if(content.Asked, [owner](uint64_t key) { return key >> 32 == owner.Id(); });
			}
		}
		content.Owners.clear();
		for (const auto world : worlds)
			content.Owners.push_back(Universe_->NameOf(world));
		if (!addedWorlds.empty()) {
			// New worlds request their own missing names without replaying existing worlds.
			for (const auto world : addedWorlds)
				content.ScannedAtRevision.erase(world.Index);
			if (content.Requested) OfferPublishedContent(content, addedWorlds);
			if (content.Client) {
				const auto keepPending = [&](engine::delivery::RequestId id) {
					const engine::core::Name name(content.Client->NameOf(id));
					for (const auto owner : content.Owners) {
						content.Asked.insert(ContentRequestKey(owner, name));
						Renderer.ExpectTexture(name, owner);
					}
				};
				for (const auto id : content.Pending)
					keepPending(id);
				for (const auto id : content.Issued)
					keepPending(id);
			}
		}
		if (!content.Client) {
			return;
		}

		// **Named in pieces rather than as one bar, because "content costs
		// 3 seconds" is not an answer.** The three things under here are a
		// delivery client resolving and verifying bundles, a demand scan over
		// the worlds, and a decode-and-upload of whatever finished - and they
		// stall for entirely different reasons. The whole of this used to have
		// no span at all, so the frame a game finished loading in read as a
		// three-second hole between `pump events` and `simulation`, which is
		// exactly the shape a missing span has and exactly how it was reported.
		ENGINE_PROFILE_CAT("content", engine::core::ProfileCategory::Assets);

		if (!content.Requested && content.Client->Ready()) {
			content.Requested = true;

			// **Nothing is requested by kind, which is what v0.10 ended.** The
			// unit that travels is a *bundle*, so asking for every mesh and
			// every material asks for essentially every bundle in the store -
			// and `AssetClient::Pump` resolves, verifies and decompresses all of
			// it synchronously, because the contract forbids a background
			// thread. On this repository's own store that was 6.9 GB through one
			// function on the frame the client started.
			//
			// `client/ContentDemand.hpp` carries both failures this replaces.
			ENGINE_INFO("content: catalogue ready - assets are fetched as the world names them");

			// **The manifest's mesh names, handed to the world once.** Names, not
			// content - a few hundred strings against the 6.9 GB above. It is the
			// only way a scene can find out what there is to name, because the
			// catalogue it can otherwise read holds what has already been asked
			// for. `scene/PublishedCatalogue.hpp` carries the whole argument, and
			// naming one of these is still what fetches it.
			OfferPublishedContent(content, worlds);
		}

		// Transform and clock updates leave the content-reference revision unchanged.

		if (content.Requested) {
			ENGINE_PROFILE_CAT("content.demand", engine::core::ProfileCategory::Assets);
			{
				ENGINE_PROFILE_CAT("content.demand.references", engine::core::ProfileCategory::Assets);
				RequestWantedContent(content, worlds);
			}
		}

		{
			// Apply completions between frames, outside render passes.
			ENGINE_PROFILE_CAT("content.deliver", engine::core::ProfileCategory::Assets);
			content.Client->Pump();
		}

		// **How much decoding and uploading one frame will do**, and the same
		// allowance the studio's own intake uses for the same reason: content
		// arrives in bursts, and draining every completed request in the frame
		// that noticed them is a frame that takes a third of a second when
		// somebody walks into a room full of new models. `IntakeBudget` says why
		// it is bytes rather than a count and why the first arrival is always
		// admitted.

		ENGINE_PROFILE_CAT("content.intake", engine::core::ProfileCategory::Assets);

		size_t kept = 0;
		for (const engine::delivery::RequestId id : content.Pending) {
			const engine::delivery::RequestState state = content.Client->StateOf(id);
			if (state == engine::delivery::RequestState::Pending) {
				content.Pending[kept++] = id;
				continue;
			}

			// Held rather than dropped: an arrival this frame cannot take is
			// still an arrival, and putting it back is what makes the budget a
			// delay instead of a loss.
			if (!ContentBudget.Admits()) {
				ContentBudget.Defer();
				content.Pending[kept++] = id;
				continue;
			}

			// **Read before the take, because a take is what destroys it.** A
			// failed request answers no asset and therefore no name, and the
			// name is what has to be unmarked - see `render::ChooseTexture`.
			const engine::core::Name asked(content.Client->NameOf(id));

			std::optional<engine::delivery::Asset> asset = content.Client->Take(id);

			// **On the request finishing, not on it succeeding**, and above
			// every `continue` below so no branch can forget. Unmarking only on
			// arrival would leave a misspelled sheet expected for ever, which is
			// precisely the case the purple marker exists for.
			for (const auto owner : content.Owners)
				Renderer.StopExpectingTexture(asked, owner);

			if (!asset) {
				// Failed, or already taken. Either way there is nothing more to
				// wait for; `delivery` has already counted it.
				continue;
			}

			ContentBudget.Spend(asset->Bytes.size());

			// **The name is published as-is, extension included.** A
			// `SurfaceAppearance` naming `characters/skin.atex` and a manifest
			// carrying `characters/skin.atex` have to be the same string or the
			// lookup misses - and the one place that could diverge is here.
			const engine::core::Name name(asset->Name);
			engine::core::ByteReader reader(asset->Bytes);

			if (asset->Kind == engine::assets::AssetKind::Mesh) {
				engine::assets::MeshData mesh;
				{
					ENGINE_PROFILE_CAT("mesh decode", engine::core::ProfileCategory::Assets);
					if (!engine::assets::Mesh::Read(reader, mesh)) {
						ENGINE_WARN("content: {} is not a mesh this engine reads", asset->Name);
						continue;
					}
				}
				// **A mesh's own sheets, asked for at the one point their names
				// are readable.** `Submesh::Texture` lives in the mesh file, so
				// `CollectWantedTextures` cannot see it - an imported model's
				// twenty sheets would otherwise never be fetched at all now that
				// textures are demand-driven.
				for (const engine::assets::Submesh &submesh : mesh.Submeshes) {
					if (!submesh.Texture.empty()) {
						RequestAsset(content, engine::core::Name(submesh.Texture));
					}
				}

				// The upload, which had no span while its texture sibling did -
				// so a slow mesh looked like time `content` spent on nothing.
				ENGINE_PROFILE_CAT("mesh upload", engine::core::ProfileCategory::Render);
				bool uploaded = false;
				for (const auto owner : content.Owners)
					uploaded = Renderer.AddMesh(name, mesh, owner) || uploaded;
				if (uploaded) {
					VisualResourcesChanged = true;
					ContentMeshes++;

					const auto generated =
						engine::render::BuildAutomaticMeshLods(*Universe_, worlds, name, mesh);
					if (engine::render::PublishAutomaticMeshLods(
							*Universe_, Renderer, content.Owners, generated
						) > 0) {
						VisualResourcesChanged = true;
					}

					// **The sheets its submeshes name, recorded where they are
					// readable.** They live inside the mesh file, so this is the
					// one point anything can learn them - and without them a
					// script that wants to swap a model's texture has no way to
					// find out what it is wearing or what to put back.
					std::vector<engine::core::Name> sheets;
					sheets.reserve(mesh.Submeshes.size());
					for (const engine::assets::Submesh &submesh : mesh.Submeshes) {
						sheets.emplace_back(submesh.Texture);
					}

					// Mesh metadata is world data, not renderer state.
					const auto triangles = static_cast<uint32_t>(mesh.Indices.size() / 3);

					// **The collision geometry, baked once here rather than per
					// world.** A hull and a triangle soup are a function of the
					// mesh alone, so building them inside the loop below would
					// be the same quickhull run four times for four worlds.
					//
					// **Through `game` rather than inline**, because a headless
					// server bakes the same two shapes out of the store it
					// serves: a second copy of the conversion here is how the
					// two would come to disagree about a hull, which reads as a
					// client and a server disagreeing about where a player is
					// standing.
					engine::scene::CollisionShapes arrived;
					{
						ENGINE_PROFILE_CAT("mesh collision", engine::core::ProfileCategory::Assets);
						engine::game::AddCollisionShapes(arrived, name, mesh);
					}

					const auto record = [&name, triangles, &sheets, &arrived](engine::ecs::Store &store) {
						engine::scene::RecordMesh(store, name, triangles, sheets);
						engine::game::MergeCollisionShapes(store, arrived);
					};

					for (const engine::world::WorldId id : worlds) {
						Universe_->Enter(id, record);
					}
				}
			} else if (asset->Kind == engine::assets::AssetKind::Texture) {
				engine::assets::TextureData image;
				{
					ENGINE_PROFILE_CAT("texture decode", engine::core::ProfileCategory::Assets);
					if (!engine::assets::Texture::Read(reader, image)) {
						ENGINE_WARN("content: {} is not a texture this engine reads", asset->Name);
						continue;
					}
				}
				{
					ENGINE_PROFILE_CAT("texture upload", engine::core::ProfileCategory::Assets);
					bool uploaded = false;
					for (const auto owner : content.Owners)
						uploaded = Renderer.AddTexture(name, image, owner) || uploaded;
					if (uploaded) {
						VisualResourcesChanged = true;
						ContentTextures++;
					}
				}

				// **Flipbook layout is world data, not renderer state**, exactly
				// as the triangle count above is. A 4x4 animation sheet and a 4x4
				// tile atlas are the same pixels, so the grid, the frame count
				// and the authored rate are what tell an emitter how to play one
				// - and without them every scene using a GIF would have to state
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
					for (const engine::world::WorldId id : worlds) {
						Universe_->Enter(id, [&name, &facts](engine::ecs::Store &store) {
							engine::scene::RecordTexture(store, name, facts);
						});
					}
				}
			} else if (asset->Kind == engine::assets::AssetKind::Material) {
				engine::assets::MaterialData material;
				// The one decode on this path with no span of its own.
				ENGINE_PROFILE_CAT("material decode", engine::core::ProfileCategory::Engine);
				if (!engine::assets::Material::Read(reader, material)) {
					ENGINE_WARN("content: {} is not a material this engine reads", asset->Name);
					continue;
				}

				// **World data, not renderer state**, exactly as the triangle
				// count and the flipbook layout above are - and for the sharper
				// version of the same reason: the renderer never sees a material
				// at all. `ResolveMaterials` turns one into a texture name on a
				// part, in `shared`, so a headless server resolves the same
				// materials the client does.
				// **All seven, built once and recorded together.** A material is
				// one thing; recording its colour and forgetting its normals
				// would draw a part textured and flat, which reads as the normal
				// map being broken rather than absent.
				const engine::scene::MaterialMaps maps{
					.Colour = engine::core::Name(material.ColourMap),
					.Normal = engine::core::Name(material.NormalMap),
					.Roughness = engine::core::Name(material.RoughnessMap),
					.Occlusion = engine::core::Name(material.OcclusionMap),
					.Height = engine::core::Name(material.HeightMap),
					.Metalness = engine::core::Name(material.MetalnessMap),
					.Emissive = engine::core::Name(material.EmissiveMap),
				};

				// **Deliberately not asked for here**, unlike a mesh's sheets, and
				// the asymmetry is the point. Every material in the catalogue
				// arrives whether anything uses it or not - 295 of them on this
				// store - so fetching each one's sheet on arrival is requesting
				// every texture by kind again, one indirection later, and it
				// refuses 160 of them exactly as before.
				//
				// A material reaches a *part* through `ResolveMaterials`, which
				// writes this name into that part's `SurfaceAppearance::ColourMap`
				// - and that is a field `CollectWantedTextures` already reads. So
				// the demand path needs no special case: the next pump asks for
				// the sheets of the materials something is actually made of.
				for (const engine::world::WorldId id : worlds) {
					Universe_->Enter(id, [&name, &maps](engine::ecs::Store &store) {
						engine::scene::RecordMaterial(store, name, maps);
					});
				}
				ContentMaterials++;
			} else if (asset->Kind == engine::assets::AssetKind::Animation) {
				engine::assets::AnimationData animation;
				if (!engine::assets::Animation::Read(reader, animation)) {
					ENGINE_WARN("content: {} is not an animation this engine reads", asset->Name);
					continue;
				}
				const auto record = [&name, &animation](engine::ecs::Store &store) {
					(void)engine::render::RecordAnimation(store, name, animation);
				};
				for (const engine::world::WorldId id : worlds) {
					Universe_->Enter(id, record);
				}
				ContentAnimations++;
			} else if (asset->Kind == engine::assets::AssetKind::Audio) {
				// **Decoded and converted here, once.** The graph must never resample
				// on the device thread, and a buffer converted per voice would pay
				// for it again for every part playing a footstep. `DecodeAudio` picks
				// its decoder from the bytes rather than from the name - Sounds.hpp.
				ENGINE_PROFILE_CAT("audio decode", engine::core::ProfileCategory::Assets);
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
		content.Pending.resize(kept);

		// Appended after the walk, never during it. See `RequestTexture`.
		content.Pending.insert(content.Pending.end(), content.Issued.begin(), content.Issued.end());
		content.Issued.clear();

		if (content.Requested && content.Pending.empty() && !content.Reported) {
			content.Reported = true;
			ENGINE_INFO(
				"content: {} mesh(es), {} texture(s), {} material(s), {} animation(s) and {} sound(s) "
				"registered",
				ContentMeshes,
				ContentTextures,
				ContentMaterials,
				ContentAnimations,
				ContentSounds
			);
		}
	}

	void
	Client::OfferPublishedContent(ContentSession &content, std::span<const engine::world::WorldId> worlds) {
		// **Named, because it lands in `content`'s self time and is not small.**
		// It walks every entry of the delivery catalogue - nearly two thousand
		// on a filled store - and enters every world to ask what each wants, on
		// a path that runs whenever content has been requested. Unprofiled, that
		// is a chunk of `content` with nothing in it to say what it was.
		ENGINE_PROFILE_CAT("offer published content", engine::core::ProfileCategory::Engine);

		const engine::assets::Manifest *catalogue = content.Client ? content.Client->Catalogue() : nullptr;
		if (catalogue == nullptr) {
			return;
		}

		// **Runtime-readable only.** A `.pmx` and a `.amesh` are both
		// `AssetKind::Mesh` and only the second is one this process decodes, so
		// offering both would hand a scene names it can set, fetch and then fail
		// to draw - a part on the fallback cube with a perfectly good string
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

		// A catalogue belongs only to the worlds served by this content session.
		for (const engine::world::WorldId id : worlds) {
			Universe_->Enter(id, [&meshes](engine::ecs::Store &store) {
				(void)engine::scene::RecordPublishedMeshes(store, meshes);
			});
		}

		ENGINE_INFO("content: {} published mesh(es) offered to the world", meshes.size());
	}

	void Client::ScanWantedContent(ContentSession &content, engine::world::WorldId world) {
		if (!world.IsValid()) {
			return;
		}

		Universe_->Enter(world, [this, &content, world](engine::ecs::Store &store) {
			// **The gate, and it is a small fixed set of integer compares.**
			// `CollectWantedContent`
			// is several walks of the store, and this used to run all of them on
			// every world on every frame - `docs/ARCH_REVIEW.md` F1, which is
			// explicit that this is work that should not happen rather than
			// work to parallelise. `WantedContentRevision` watches only columns
			// that can carry an asset name. Its monotonic component versions survive
			// `ClearChanges`, and live row counts cover removals, so the reader
			// cannot miss a write between pumps. Particle simulation, transforms,
			// ticks, and additional cameras leave it unchanged.
			const uint64_t revision = WantedContentRevision(store);
			const auto scanned = content.ScannedAtRevision.find(world.Index);
			if (scanned != content.ScannedAtRevision.end() && scanned->second == revision) {
				return;
			}
			content.ScannedAtRevision[world.Index] = revision;

			CollectWantedContent(store, content.Wanted);
		});
	}

	void
	Client::RequestWantedContent(ContentSession &content, std::span<const engine::world::WorldId> worlds) {
		// Reused rather than made per pump: the steady-state answer is empty and
		// an allocation per world per frame to produce nothing is the same kind
		// of waste the gate below removes.
		content.Wanted.clear();

		for (const engine::world::WorldId id : worlds) {
			content.Wanted.clear();
			ScanWantedContent(content, id);
			for (const engine::core::Name &name : content.Wanted) {
				RequestAsset(content, name, Universe_->NameOf(id));
			}
		}
	}

	void Client::RequestAsset(
		ContentSession &content, const engine::core::Name &texture, engine::core::Name requestingOwner
	) {
		if (!content.Client || !texture.IsValid()) return;
		// Attempts are remembered per owner, including failures. A later world can
		// demand an already-seen name without making old worlds fetch it again.
		const auto alreadyAsked = [&](engine::core::Name owner) {
			return content.Asked.contains(ContentRequestKey(owner, texture));
		};
		if (requestingOwner.IsValid()
				? alreadyAsked(requestingOwner)
				: std::all_of(content.Owners.begin(), content.Owners.end(), alreadyAsked))
			return;
		for (const auto owner : content.Owners)
			content.Asked.insert(ContentRequestKey(owner, texture));
		const auto pending = [&](engine::delivery::RequestId id) {
			return content.Client->NameOf(id) == texture.Text();
		};
		if (std::any_of(content.Pending.begin(), content.Pending.end(), pending) ||
			std::any_of(content.Issued.begin(), content.Issued.end(), pending)) {
			for (const auto owner : content.Owners)
				Renderer.ExpectTexture(texture, owner);
			return;
		}

		// **Refused before the request and not on arrival, so the bytes never
		// cross.** A form this deployment has turned off is one nothing here
		// will decode, and fetching it anyway would spend the link on something
		// destined for a `continue`. Logged once - the insert above is what
		// makes it once - because a name that silently never arrives is exactly
		// the failure the settings layer exists to make legible.
		if (const engine::assets::ContentForm form = engine::assets::FormOfName(texture.Text());
			!engine::assets::ContentPolicy::Process(engine::assets::ContentVerb::Handle).Allows(form)) {
			ENGINE_INFO(
				"content: not asking for {} - {} content is turned off",
				texture.Text(),
				engine::assets::Describe(form)
			);
			return;
		}

		// **Queued rather than appended, because this is called from inside the
		// walk over `content.Pending`.** A mesh names its own sheets and a
		// material names its colour map, and both are read while draining that
		// vector - pushing to it there is a range-for over a container being
		// grown, which is what it looks like: the walk lost its place and one
		// texture out of the several hundred asked for arrived.
		content.Issued.push_back(content.Client->Request(texture.Text()));

		// **Marked before the answer, which is the whole point.** Until this
		// request finishes, a part naming this sheet draws as the default
		// material rather than as the purple marker - so a scene load looks like
		// untextured parts becoming textured instead of a purple shimmer across
		// every imported model. See `render::ChooseTexture`.
		for (const auto owner : content.Owners)
			Renderer.ExpectTexture(texture, owner);
	}

}
