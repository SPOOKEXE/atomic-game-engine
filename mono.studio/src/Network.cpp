// What is moving between this editor and the origins it is configured against.
//
// **The editor had the configuration and never used it.** `ContentSources` was
// saved, loaded and edited since v0.9, and nothing in the studio ever built a
// `delivery::AssetClient` from it — so a publisher key could be wrong, an origin
// could be down and an address could be a host name that never resolves, and the
// preferences page would look exactly the same either way. This file is the half
// that makes the settings do something and then says what happened.
//
// **A model over counters, and no clock of its own.** `cdn::Dashboard` is the
// same shape on the origin's side and for the same reasons — everything here is
// arithmetic over `DeliveryCounters`, `UploadCounters` and a ring of one-second
// samples, and every sample is stamped with a time passed in. What it does *not*
// share with the dashboard is a text model: an origin's operator is looking at a
// terminal and this is an imgui table, so the layout is drawn directly and the
// thing worth sharing was the arithmetic rather than the lines.
//
// **The rate is measured and not derived.** "Bytes over the run divided by how
// long the editor has been open" is a number that only falls, and is useless the
// moment somebody wants to know whether a download is moving *now*. So the
// samples are a ring of the last few seconds and the rate is what crossed inside
// it — which is the same decision `Dashboard`'s minute buckets make, at the
// resolution an interactive panel is read at.

#include <engine/scene/MeshCatalogue.hpp>
#include <engine/scene/PublishedCatalogue.hpp>
#include <engine/assets/Manifest.hpp>
#include <engine/scene/Materials.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/assets/Texture.hpp>
#include <engine/assets/Mesh.hpp>
#include <engine/assets/Material.hpp>
#include <engine/assets/Builtin.hpp>
#include <client/ContentDemand.hpp>
#include <engine/core/Log.hpp>
#include <engine/delivery/Client.hpp>
#include <engine/delivery/Uploader.hpp>
#include <engine/ui/Theme.hpp>

#include <algorithm>
#include <cdn/LocalStore.hpp>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <studio/Editor.hpp>
#include <studio/Widgets.hpp>
#include <system_error>

namespace studio {

	namespace {
		// How many new fetches an editor starts in one pump.
		//
		// **Four, because a request pulls a bundle and a bundle is decompressed
		// on this thread.** `delivery/Client.hpp` forbids a background thread, so
		// the only lever is how much is asked for at once — and a place naming
		// five hundred assets has to become five hundred assets arriving over a
		// second, not one frame that never returns.
		constexpr size_t REQUESTS_PER_PUMP = 4;
	}

	namespace {
		// A byte count somebody can read at a glance.
		//
		// **Powers of 1024 under decimal names**, matching `cdn::Readable` and
		// every tool an operator already has open. Being right about the prefix
		// and alone in it helps nobody comparing this against `df`.
		std::string Readable(uint64_t bytes) {
			static const char *UNITS[] = {"B", "KB", "MB", "GB", "TB"};

			double scaled = static_cast<double>(bytes);
			size_t unit = 0;
			while (scaled >= 1024.0 && unit + 1 < std::size(UNITS)) {
				scaled /= 1024.0;
				unit++;
			}

			char text[32];
			std::snprintf(text, sizeof(text), unit == 0 ? "%.0f %s" : "%.1f %s", scaled, UNITS[unit]);
			return text;
		}

		std::string PerSecond(double bytes) {
			return Readable(static_cast<uint64_t>(std::max(0.0, bytes))) + "/s";
		}

		// One labelled number in the two-column table both halves use.
		void Row(const char *label, const std::string &value, const char *note = nullptr) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(label);
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(value.c_str());
			if (note != nullptr && ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", note);
			}
		}
	}

	void NetworkSamples::Observe(double nowSeconds, uint64_t down, uint64_t up) {
		if (Filled == 0) {
			// **The first observation seeds and does not measure.** A ring whose
			// first sample counted every byte since the process started would
			// show one enormous spike on the frame the panel is first opened,
			// which is exactly when somebody is deciding whether to believe it.
			At = 0;
			Filled = 1;
			Points[0] = Sample{.Seconds = nowSeconds, .Down = down, .Up = up};
			return;
		}

		const Sample &last = Points[At];
		if (nowSeconds - last.Seconds < INTERVAL) {
			return;
		}

		At = (At + 1) % CAPACITY;
		Points[At] = Sample{.Seconds = nowSeconds, .Down = down, .Up = up};
		Filled = std::min<size_t>(Filled + 1, CAPACITY);
	}

	NetworkRates NetworkSamples::Rates() const {
		if (Filled < 2) {
			return {};
		}

		// The oldest sample still in the ring against the newest. Not the last
		// two: a pair one interval apart is one frame's noise, and what the
		// panel is being read for is whether a transfer is moving.
		const size_t oldest = (At + CAPACITY - (Filled - 1)) % CAPACITY;
		const Sample &first = Points[oldest];
		const Sample &last = Points[At];

		const double elapsed = last.Seconds - first.Seconds;
		if (elapsed <= 0.0) {
			return {};
		}

		NetworkRates rates;
		rates.DownPerSecond = static_cast<double>(last.Down - first.Down) / elapsed;
		rates.UpPerSecond = static_cast<double>(last.Up - first.Up) / elapsed;
		rates.WindowSeconds = elapsed;
		return rates;
	}

	void Editor::RebuildContentClients() {
		// **Both are rebuilt together and from one settings block**, because
		// they read one list: a role edited in the preferences page changes
		// which of them a row belongs to, and rebuilding one would leave the
		// other holding the previous answer.
		const engine::delivery::DeliverySettings settings = Content.ToSettings();

		ContentClient.reset();
		ContentUploads.reset();
		ContentSamples = NetworkSamples{};

		if (settings.IsValid()) {
			ContentClient = engine::delivery::MakeAssetClient(settings);
		} else {
			// Said once, here, rather than as a failed fetch later. The two
			// reasons are worth separating because they are fixed on different
			// pages: no key is a trust problem and no source is an address one.
			ContentStatus = Content.PublisherKey.empty()
								? "no publisher key — nothing can be fetched, because nothing could be verified"
								: "no usable read source — check the addresses and that a row is enabled";
		}

		// **Built even when delivery is not.** An uploader verifies nothing, so
		// it does not need a publisher key — and an editor being used to *seed*
		// an origin is exactly the case where no manifest has been signed yet
		// and `DeliverySettings::IsValid` is false.
		ContentUploads = engine::delivery::MakeUploader(settings);
	}

	void Editor::PumpContent(double frameSeconds) {
		ContentSeconds += frameSeconds;

		if (ContentClient) {
			ContentClient->Pump();
			DrainContent();
		}

		if (ContentUploads) {
			ContentUploads->Pump();
			for (const engine::delivery::UploadOutcome &outcome : ContentUploads->Take()) {
				if (!outcome.Delivered) {
					// **Failures are logged and successes are not.** A tree of
					// three hundred files would otherwise be three hundred log
					// lines nobody reads; the counters say how it went and the
					// log says what went wrong.
					ENGINE_WARN(
						"upload: {} → {}: {}",
						outcome.File.filename().string(),
						outcome.Destination,
						outcome.Detail
					);
					UploadFailures++;
				}
			}

			if (ContentUploads->Remaining() == 0 && UploadQueued > 0) {
				const engine::delivery::UploadCounters &counters = ContentUploads->Counters();
				ContentStatus = std::to_string(counters.Stored) + " stored, " +
								std::to_string(counters.Skipped) + " already there, " +
								std::to_string(counters.Refused + counters.Failed) + " failed";
				ENGINE_INFO("upload: {}", ContentStatus);
				UploadQueued = 0;
			}
		}

		uint64_t down = 0;
		if (ContentClient) {
			down = ContentClient->Counters().TransferredBytes;
		}
		const uint64_t up = ContentUploads ? ContentUploads->Counters().SentBytes : 0;
		ContentSamples.Observe(ContentSeconds, down, up);
	}

	void Editor::EachOpenWorld(const std::function<void(engine::ecs::Store &)> &body) {
		// **Every world, not only the one on screen.** An editor has several
		// open — a server's beside a client's during Play — and a mesh
		// registered into the renderer is registered for all of them, so a
		// catalogue filled for one would leave `TrianglesCount` answering zero
		// in the others for no reason anybody could see.
		for (const engine::world::WorldId world : Universe->Worlds()) {
			Universe->Enter(world, [&body](engine::ecs::Store &store) { body(store); });
		}
	}

	void Editor::DrainContent() {
		// **The editor fetches content, which it did not before at all.** Its
		// delivery client existed and nothing ever asked it for anything, so a
		// `MeshPart` in a viewport drew the fallback cube however good its
		// `MeshId` was — the renderer had never been handed a mesh. This is
		// `Client::PumpContent`'s policy, one program over.
		if (!ContentRequested && ContentClient->Ready()) {
			ContentRequested = true;
			ENGINE_INFO("assets: catalogue ready — content is fetched as the worlds name it");

			// **The list of what there is, handed to the worlds once.** Not the
			// content — the *names*. A scene has no other way to find out what a
			// store published: since v0.10 nothing is fetched by kind, so
			// `ContentService:GetMeshes()` reports only what something already
			// asked for, and a scene reading it can never be the thing that asks.
			//
			// Cheap, and worth saying how cheap: a few hundred strings, once,
			// against the 6.9 GB that asking by kind used to pull through
			// `Pump`. Naming one of these is still what fetches it.
			PublishManifestNames();
		}

		if (ContentRequested) {
			OfferPublishedNames();
			RequestShownContent();
		}

		// **How much decoding and uploading one frame will do**, and the same
		// allowance the client's own intake uses. Content arrives in bursts — a
		// scene names forty meshes at once and the origin answers them together
		// — and this loop used to drain every completed request in the frame
		// that noticed them, which is a third of a second in one frame and an
		// editor that stops responding while somebody's model set lands.
		//
		// `IntakeBudget` says why it is bytes rather than a count, why what does
		// not fit is deferred rather than dropped, and why the first arrival of
		// a frame is admitted however large it is.
		ContentBudget.Begin();

		size_t kept = 0;
		for (const engine::delivery::RequestId id : ContentPending) {
			if (ContentClient->StateOf(id) == engine::delivery::RequestState::Pending) {
				ContentPending[kept++] = id;
				continue;
			}

			// **Held rather than dropped.** An arrival this frame cannot take is
			// still an arrival; putting it back in the pending list is what
			// makes the budget a delay instead of a loss.
			if (!ContentBudget.Admits()) {
				ContentBudget.Defer();
				ContentPending[kept++] = id;
				continue;
			}

			// **Read before the take, because a take is what destroys it.** A
			// failed request answers no asset and therefore no name, and the
			// name is what has to be unmarked — the half `D00107` warned about,
			// where unmarking only on arrival leaves a misspelled sheet expected
			// for ever and the marker never appears for the one case it exists
			// for.
			const engine::core::Name asked(ContentClient->NameOf(id));

			std::optional<engine::delivery::Asset> asset = ContentClient->Take(id);

			// **On the request finishing, not on it succeeding**, and above
			// every `continue` below so no branch can forget. An arrival needs
			// no call — `AddTexture` clears it — but doing it here as well costs
			// a hash and removes the question.
			Renderer.StopExpectingTexture(asked);

			if (!asset) {
				continue;
			}

			ContentBudget.Spend(asset->Bytes.size());

			const engine::core::Name name(asset->Name);
			engine::core::ByteReader reader(asset->Bytes);

			if (asset->Kind == engine::assets::AssetKind::Mesh) {
				engine::assets::MeshData mesh;
				if (!engine::assets::Mesh::Read(reader, mesh)) {
					continue;
				}

				// A mesh's own sheets, at the one point their names are
				// readable — they live inside the mesh file, so the demand pass
				// cannot see them.
				for (const engine::assets::Submesh &submesh : mesh.Submeshes) {
					if (!submesh.Texture.empty()) {
						(void)RequestContentAsset(engine::core::Name(submesh.Texture));
					}
				}

				if (Renderer.AddMesh(name, mesh)) {
					ContentMeshes++;

					// **Every part naming it, now that its shape is known.** A
					// `MeshId` can be set long before the geometry arrives — that
					// is the ordinary case, since naming it is what fetches it —
					// so the fit cannot happen at assignment alone.
					FitPartsToMesh(
						name, engine::core::Vector3{(mesh.Maximum - mesh.Minimum) * 0.5f}
					);
					// **Triangle counts are world data**, so `MeshPart.
					// TrianglesCount` answers in an edited world too — which is
					// how somebody checks a mesh actually arrived.
					const auto triangles = static_cast<uint32_t>(mesh.Indices.size() / 3);
					EachOpenWorld([&name, triangles](engine::ecs::Store &store) {
						engine::scene::RecordMesh(store, name, triangles);
					});
				}
			} else if (asset->Kind == engine::assets::AssetKind::Texture) {
				engine::assets::TextureData image;
				if (engine::assets::Texture::Read(reader, image) && Renderer.AddTexture(name, image)) {
					ContentTextures++;
				}
			} else if (asset->Kind == engine::assets::AssetKind::Shader) {
				// **Handed over whole, not decoded.** A shader asset is a SPIR-V
				// module; there is nothing here to parse, and a renderer holding
				// a compiler for content it did not write is how a frame ends up
				// paying for one. `Renderer::AddShader` takes the bytes and a
				// `raster` or `dispatch` node names them.
				//
				// **Only what a runtime can read.** GLSL routes to this kind too
				// — what somebody publishes is what they wrote — and
				// `IsRuntimeReadable` is what says it has not been baked yet.
				// TODO(render-pipeline): `Renderer.AddShader(name, asset->Bytes)`
				// went here, inside this condition. The delivery is kept and
				// still counted, because `AssetKind::Shader` is part of the asset
				// pipeline rather than the render one — shaders publish, fetch
				// and arrive exactly as before. What is missing is the renderer
				// end: something has to take the bytes and let a node name them.
				if (engine::assets::IsRuntimeReadable(asset->Name)) {
					ContentShaders++;
				}
			} else if (asset->Kind == engine::assets::AssetKind::Material) {
				engine::assets::MaterialData material;
				if (!engine::assets::Material::Read(reader, material)) {
					continue;
				}
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
				EachOpenWorld([&name, &maps](engine::ecs::Store &store) {
					engine::scene::RecordMaterial(store, name, maps);
				});
				ContentMaterials++;
			}
		}
		ContentPending.resize(kept);

		// Appended after the walk, never during it — a mesh names its own
		// sheets while this vector is being drained, and pushing to a container
		// being iterated is what cost the client a whole debugging round.
		ContentPending.insert(ContentPending.end(), ContentIssued.begin(), ContentIssued.end());
		ContentIssued.clear();

		// **Said whenever the queue empties on a different total than last time,
		// where it used to be said exactly once.** An editor is not a client: a
		// client names its content at load and then stops, so one line at the end
		// of the first drain described the whole session. An editor's whole job is
		// to name content *later* — somebody picks a mesh, and that is the moment
		// they want to know whether it arrived.
		//
		// The once-only version reported `0 mesh(es)` on the frame the catalogue
		// opened and then never spoke again, so every asset chosen after start-up
		// loaded in silence. When the picker beside it was also dropping every
		// choice, the two failures were indistinguishable from one: nothing
		// changed on screen and nothing was written down.
		//
		// **Gated on the counts rather than on the queue**, so a pump that drains
		// nothing new says nothing — otherwise this would be a line per frame for
		// the life of the editor.
		const size_t total = ContentMeshes + ContentTextures + ContentMaterials;
		if (ContentPending.empty() && total != ContentReportedTotal) {
			ContentReportedTotal = total;
			ENGINE_INFO(
				"assets: {} mesh(es), {} texture(s) and {} material(s) registered",
				ContentMeshes,
				ContentTextures,
				ContentMaterials
			);
		}
	}

	void Editor::FitPartsToMesh(const engine::core::Name &mesh, const engine::core::Vector3 &extent) {
		// The mesh's proportions, normalised so the longest axis is one.
		const float longest = std::max({extent.X, extent.Y, extent.Z});
		if (!mesh.IsValid() || longest <= 1e-6f) {
			return;
		}

		EachOpenWorld([&mesh, &extent, longest](engine::ecs::Store &store) {
			store.Each<engine::scene::Visual, engine::scene::Bounds>(
				[&](engine::ecs::Entity, engine::scene::Visual &visual, engine::scene::Bounds &bounds) {
					// **Only when the mesh changed, which is what `Visual::Fitted`
					// records.** This runs whenever geometry arrives — a republish,
					// a reopened place, another part pulling the same mesh in — and
					// without the guard every one of those would reshape a box
					// somebody had deliberately squashed. A scene that rearranges
					// itself on load is the worst kind of surprise, because nothing
					// visibly did it.
					if (visual.Mesh != mesh || visual.Fitted == mesh) {
						return;
					}

					// **The part keeps the size it has along its longest axis and
					// gets the mesh's shape on the other two.** That is the whole
					// rule, and both halves are load-bearing:
					//
					//   * the shape has to come from the mesh, because `Size` is a
					//     box the mesh is *stretched* into — a character in a cubic
					//     box is a character squashed into a cube;
					//   * the scale has to come from the part, because somebody
					//     swapping a bad mesh for a fixed one wants the thing to
					//     stay the size they made it. Taking the mesh's own metres
					//     would resize their scene every time they corrected an
					//     asset.
					//
					// **Idempotent**, which is what lets this run whenever a mesh
					// arrives rather than only on assignment: a part whose
					// proportions already match is written the value it has.
					const float span =
						std::max({bounds.HalfExtent.X, bounds.HalfExtent.Y, bounds.HalfExtent.Z});
					if (span <= 1e-6f) {
						return;
					}

					const float unit = span / longest;
					bounds.HalfExtent = engine::core::Vector3{
						extent.X * unit,
						extent.Y * unit,
						extent.Z * unit,
					};

					// Claimed, so nothing fits this part to this mesh again.
					visual.Fitted = mesh;
				}
			);
		});
	}

	void Editor::PublishManifestNames() {
		const engine::assets::Manifest *catalogue = ContentClient ? ContentClient->Catalogue() : nullptr;
		if (catalogue == nullptr) {
			return;
		}

		// **Runtime-readable only.** A `.pmx` and a `.amesh` are both
		// `AssetKind::Mesh` and only the second is something the runtime decodes,
		// so offering both would put names in a scene's list that can be named,
		// fetched and then refused — a cell drawing the fallback cube with a
		// perfectly good string behind it. `assets::IsRuntimeReadable` is the same
		// filter the asset picker applies, and a second opinion here would be a
		// script disagreeing with the editor about what works.
		PublishedMeshNames.clear();
		for (const engine::assets::AssetEntry *entry : catalogue->OfKind(engine::assets::AssetKind::Mesh)) {
			if (entry != nullptr && engine::assets::IsRuntimeReadable(entry->Name)) {
				PublishedMeshNames.emplace_back(entry->Name);
			}
		}

		ENGINE_INFO("assets: {} published mesh(es) offered to the worlds", PublishedMeshNames.size());
		OfferPublishedNames();
	}

	void Editor::OfferPublishedNames() {
		if (PublishedMeshNames.empty()) {
			return;
		}

		// **Every pump, not once when the catalogue opened.** Worlds appear after
		// that moment and routinely: pressing Play mints a server world and a
		// client replica, and both run scripts. Offering once left those two with
		// an empty list, so the mesh grid placed its six built-ins during editing
		// and stayed at six through the whole play session — which reads exactly
		// like a store with nothing in it.
		//
		// **Guarded on the count, so the common case is a comparison.** The list
		// only ever changes on a republish, and copying a few hundred names into
		// every world every pump would be work proportional to the store on a
		// path that runs at the frame rate.
		EachOpenWorld([this](engine::ecs::Store &store) {
			const auto *held = store.Resource<engine::scene::PublishedCatalogue>();
			if (held != nullptr && held->Meshes.size() == PublishedMeshNames.size()) {
				return;
			}
			(void)engine::scene::RecordPublishedMeshes(store, PublishedMeshNames);
		});
	}

	void Editor::RequestShownContent() {
		// **Bounded per pump, which is the whole of what "asynchronously" can
		// mean here.** `delivery/Client.hpp` forbids a background thread — a
		// completion arriving at a moment scheduling chose would be a desync —
		// so `Pump` does its work on this thread, and the unit it fetches is a
		// *bundle*. Issuing five hundred requests at once therefore asks for
		// five hundred bundles' worth of decompression before the next frame.
		//
		// Issuing a few per pump turns the same load into content appearing over
		// a second or two, which is what an editor opening a large place should
		// look like. The collection is idempotent, so what is not issued this
		// pump is simply issued on the next — there is no queue to keep in step.
		std::vector<engine::core::Name> wanted;
		EachOpenWorld([&wanted](engine::ecs::Store &store) {
			client::CollectWantedContent(store, wanted);
		});

		size_t issued = 0;
		for (const engine::core::Name &name : wanted) {
			if (issued >= REQUESTS_PER_PUMP) {
				break;
			}
			if (RequestContentAsset(name)) {
				issued++;
			}
		}
	}

	bool Editor::RequestContentAsset(const engine::core::Name &asset) {
		// **A built-in is never fetched, because there is nothing to fetch.**
		// `engine.Cube` and its five siblings are generated by
		// `assets::MakeBuiltin` and registered by `MeshTable::Initialise` before
		// any delivery client exists, so they resolve in every process with no
		// store at all. No manifest names one, so a request for it can only come
		// back as a miss — a warning per built-in in the log of anybody using
		// the picker's default rows, describing content that is already there.
		if (engine::assets::BuiltinMesh ignored; engine::assets::BuiltinFromName(asset.Text(), ignored)) {
			return false;
		}

		// Asked once, whatever happened to it — a misspelled name must not
		// issue a request per frame for the life of the session.
		if (!ContentClient || !asset.IsValid() || !ContentAsked.insert(asset.Id()).second) {
			return false;
		}
		ContentIssued.push_back(ContentClient->Request(asset.Text()));

		// **Marked before the answer, which is the whole point.** Until this
		// request finishes, a part naming this sheet draws as the default
		// material rather than as the purple marker — so a scene load looks like
		// untextured parts becoming textured instead of a purple shimmer. See
		// `render::ChooseTexture`; the unmark is in `DrainContent`, on the
		// request finishing rather than on it succeeding.
		Renderer.ExpectTexture(asset);
		return true;
	}

	void Editor::UploadStore() {
		if (!ContentUploads) {
			ContentStatus = "no write source — give a row the write role and an ingest key";
			return;
		}

		// **`raw/` and not `processed/`.** What an origin's inbox wants is
		// content, and `processed/` is a *published* store — chunks under hash
		// names plus a signed manifest, which would arrive as several thousand
		// unrecognisable files. The far end publishes what it receives; sending
		// it something already published would be publishing twice.
		const cdn::LocalPaths paths = cdn::DefaultLocalPaths();

		std::error_code failure;
		if (!std::filesystem::is_directory(paths.Raw, failure)) {
			ContentStatus = "nothing to upload — the content store is empty";
			return;
		}

		size_t queued = 0;
		for (std::filesystem::recursive_directory_iterator walk(
				 paths.Raw, std::filesystem::directory_options::skip_permission_denied, failure
			 );
			 walk != std::filesystem::recursive_directory_iterator();
			 walk.increment(failure)) {
			if (failure) {
				break;
			}
			if (walk->is_regular_file(failure) && ContentUploads->Add(walk->path())) {
				queued++;
			}
		}

		UploadQueued = queued;
		UploadFailures = 0;
		ContentStatus = std::to_string(queued) + " file(s) queued to " +
						std::to_string(ContentUploads->Destinations().size()) + " destination(s)";
		ENGINE_INFO("upload: {}", ContentStatus);
	}

	void Editor::DownloadAsset(const std::string &name) {
		if (!ContentClient) {
			ContentStatus = "delivery is not configured — see the Content page in Preferences";
			return;
		}
		if (name.empty()) {
			return;
		}

		const engine::delivery::RequestId id = ContentClient->Request(name);
		if (!id.IsValid()) {
			ContentStatus = "could not ask for " + name;
			return;
		}

		Downloads.push_back(PendingDownload{.Name = name, .Id = id});
		ContentStatus = "asked for " + name;
	}

	void Editor::CollectDownloads() {
		if (!ContentClient) {
			Downloads.clear();
			return;
		}

		for (size_t index = 0; index < Downloads.size();) {
			PendingDownload &pending = Downloads[index];
			const engine::delivery::RequestState state = ContentClient->StateOf(pending.Id);

			if (state == engine::delivery::RequestState::Pending) {
				index++;
				continue;
			}

			if (state == engine::delivery::RequestState::Ready) {
				std::optional<engine::delivery::Asset> asset = ContentClient->Take(pending.Id);
				if (asset) {
					// **Into the local store, under the hash of what arrived.**
					// A download that landed somewhere else would be a second
					// place content lives, and the whole reason the store is
					// content-addressed is that there is one.
					const cdn::LocalPaths paths = cdn::DefaultLocalPaths();
					if (cdn::EnsureLocalStore(paths)) {
						const std::filesystem::path stored =
							paths.Raw / (asset->Root.ToHex() + std::filesystem::path(asset->Name).extension().string());
						std::ofstream out(stored, std::ios::binary | std::ios::trunc);
						out.write(
							reinterpret_cast<const char *>(asset->Bytes.data()),
							static_cast<std::streamsize>(asset->Bytes.size())
						);
						ContentStatus = out.good()
											? pending.Name + " — " + Readable(asset->Bytes.size()) + " into the store"
											: pending.Name + " — could not be written";
					}
				}
			} else {
				ContentStatus = pending.Name + " — every source was tried and none answered";
			}

			Downloads.erase(Downloads.begin() + static_cast<ptrdiff_t>(index));
		}
	}

	void Editor::DrawNetwork() {
		if (!ShowNetwork) {
			return;
		}

		if (!ImGui::Begin("Network", &ShowNetwork)) {
			ImGui::End();
			return;
		}

		CollectDownloads();

		const NetworkRates rates = ContentSamples.Rates();

		// --- what is moving right now -----------------------------------------
		//
		// First, and emphasised, because it is the one row somebody opens this
		// panel to see. Everything below it is a total, and a total cannot say
		// whether a transfer is progressing.
		ImGui::SeparatorText("Right now");

		if (ImGui::BeginTable("##rates", 2, ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn("##what", ImGuiTableColumnFlags_WidthStretch, 0.45f);
			ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch, 0.55f);

			Row("Down", PerSecond(rates.DownPerSecond), "compressed bytes off the wire, over the last few seconds");
			Row("Up", PerSecond(rates.UpPerSecond), "bytes sent to write origins");

			if (ContentClient) {
				Row("In flight", std::to_string(ContentClient->Outstanding()) + " request(s)");
			}
			if (ContentUploads) {
				Row("Queued", std::to_string(ContentUploads->Remaining()) + " upload(s)");
			}
			ImGui::EndTable();
		}

		if (rates.WindowSeconds <= 0.0) {
			ImGui::TextDisabled("no window yet — rates appear after a second of samples");
		}

		// --- downloading ------------------------------------------------------

		ImGui::SeparatorText("Downloading");

		if (!ContentClient) {
			ImGui::TextColored(
				ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled),
				"not configured — Preferences > Content needs a publisher key and a readable source"
			);
		} else {
			const engine::delivery::DeliveryCounters &counters = ContentClient->Counters();

			if (ImGui::BeginTable("##down", 2, ImGuiTableFlags_SizingStretchProp)) {
				ImGui::TableSetupColumn("##what", ImGuiTableColumnFlags_WidthStretch, 0.45f);
				ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch, 0.55f);

				Row("Manifest", ContentClient->Ready() ? "verified" : "waiting");
				Row("Cache hits", std::to_string(counters.CacheHits), "assets served without touching a source");
				Row("Cache misses", std::to_string(counters.CacheMisses));
				Row("Bundles", std::to_string(counters.Bundles));
				Row("Transferred", Readable(counters.TransferredBytes), "as it travelled — compressed");
				Row("Expanded", Readable(counters.ExpandedBytes), "what those became");

				// **The pair is what answers 'did this travel compressed'**, and
				// it is a question about the wire — so it is measured at it
				// rather than inferred from a setting. See `DeliveryCounters`.
				if (counters.TransferredBytes > 0) {
					char ratio[32];
					std::snprintf(
						ratio,
						sizeof(ratio),
						"%.2fx",
						static_cast<double>(counters.ExpandedBytes) /
							static_cast<double>(counters.TransferredBytes)
					);
					Row("Compression", ratio);
				}

				Row("Source failures", std::to_string(counters.SourceFailures), "times a source was passed over");

				ImGui::EndTable();
			}

			// **Verification failures get their own line and a colour**, because
			// they are not an operational event. A source that is down is
			// ordinary; a source serving bytes that do not match a signed root
			// is corruption or an attack, and burying it in a table of totals is
			// how it gets missed.
			if (counters.VerificationFailures > 0) {
				ImGui::TextColored(
					ImVec4(0.95f, 0.45f, 0.35f, 1.0f),
					"%llu payload(s) did not verify against the signed manifest",
					static_cast<unsigned long long>(counters.VerificationFailures)
				);
			}

			ImGui::SetNextItemWidth(-90.0f);
			ImGui::InputTextWithHint(
				"##fetch", "an asset name from the manifest", DownloadName, sizeof(DownloadName)
			);
			ImGui::SameLine();
			ImGui::BeginDisabled(DownloadName[0] == '\0');
			if (ImGui::Button("Fetch", ImVec2(80.0f, 0.0f))) {
				DownloadAsset(DownloadName);
			}
			ImGui::EndDisabled();

			for (const PendingDownload &pending : Downloads) {
				ImGui::BulletText("%s — waiting", pending.Name.c_str());
			}
		}

		// --- uploading --------------------------------------------------------

		ImGui::SeparatorText("Uploading");

		if (!ContentUploads) {
			ImGui::TextColored(
				ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled),
				"no write source — give a row the write role and an ingest key in Preferences > Content"
			);
		} else {
			const engine::delivery::UploadCounters &counters = ContentUploads->Counters();

			if (ImGui::BeginTable("##up", 2, ImGuiTableFlags_SizingStretchProp)) {
				ImGui::TableSetupColumn("##what", ImGuiTableColumnFlags_WidthStretch, 0.45f);
				ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch, 0.55f);

				Row("Destinations", std::to_string(ContentUploads->Destinations().size()));
				Row("Stored", std::to_string(counters.Stored));
				Row("Already there", std::to_string(counters.Skipped), "the probe that makes a re-upload cheap");
				Row("Sent", Readable(counters.SentBytes));
				Row("Refused", std::to_string(counters.Refused), "wrong key, or an origin that takes no writes");
				Row("Failed", std::to_string(counters.Failed));
				ImGui::EndTable();
			}

			for (const engine::delivery::Source &target : ContentUploads->Destinations()) {
				ImGui::BulletText("%s — %s", target.Name.c_str(), target.Location.c_str());
			}

			ImGui::BeginDisabled(ContentUploads->Remaining() > 0);
			if (ImGui::Button("Upload the content store")) {
				UploadStore();
			}
			ImGui::EndDisabled();

			if (ContentUploads->Remaining() > 0 && UploadQueued > 0) {
				const size_t done = UploadQueued > ContentUploads->Remaining() ? UploadQueued - ContentUploads->Remaining() : 0;
				ImGui::SameLine();
				ImGui::Text("%zu / %zu", done, UploadQueued);
			}
		}

		if (!ContentStatus.empty()) {
			ImGui::Separator();
			ImGui::TextWrapped("%s", ContentStatus.c_str());
		}

		ImGui::End();
	}
}
