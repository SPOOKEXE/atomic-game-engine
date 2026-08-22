// What is moving between this editor and the origins it is configured against.
//
// **The editor had the configuration and never used it.** `ContentSources` was
// saved, loaded and edited since v0.9, and nothing in the studio ever built a
// `delivery::AssetClient` from it - so a publisher key could be wrong, an origin
// could be down and an address could be a host name that never resolves, and the
// preferences page would look exactly the same either way. This file is the half
// that makes the settings do something and then says what happened.
//
// **A model over counters, and no clock of its own.** `cdn::Dashboard` is the
// same shape on the origin's side and for the same reasons - everything here is
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
// it - which is the same decision `Dashboard`'s minute buckets make, at the
// resolution an interactive panel is read at.

#include <engine/assets/Builtin.hpp>
#include <engine/assets/ContentForm.hpp>
#include <engine/assets/ContentPolicy.hpp>
#include <engine/assets/Manifest.hpp>
#include <engine/assets/Material.hpp>
#include <engine/assets/Mesh.hpp>
#include <engine/assets/Texture.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/core/FrameGraph.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/delivery/Client.hpp>
#include <engine/delivery/Uploader.hpp>
#include <engine/game/CollisionContent.hpp>
#include <engine/scene/Materials.hpp>
#include <engine/scene/MeshCatalogue.hpp>
#include <engine/scene/PublishedCatalogue.hpp>
#include <engine/ui/Theme.hpp>

#include <algorithm>
#include <cdn/LocalStore.hpp>
#include <client/ContentDemand.hpp>
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
		// the only lever is how much is asked for at once - and a place naming
		// five hundred assets has to become five hundred assets arriving over a
		// second, not one frame that never returns.
		constexpr size_t REQUESTS_PER_PUMP = 4;
	}

	namespace {
		// A byte count somebody can read at a glance.
		//
		// **Powers of 1024 under decimal names**, matching `cdn::ReadableRate` and
		// every tool an operator already has open. Being right about the prefix
		// and alone in it helps nobody comparing this against `df`.
		std::string ReadableRate(uint64_t bytes) {
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
			return ReadableRate(static_cast<uint64_t>(std::max(0.0, bytes))) + "/s";
		}

		// One labelled number in the two-column table both halves use.
		void NetworkRow(const char *label, const std::string &value, const char *note = nullptr) {
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

		// **Everything the old client's answers were recorded in goes with it.**
		// `ContentAsked` is what stops a name being requested twice, and it was
		// keyed to a client that no longer exists - so saving the Content page
		// mid-session left every asset already named permanently unfetchable
		// against the new sources, which reads as the new origin being empty.
		// `ContentPending` and `ContentIssued` hold request ids belonging to the
		// dead client and would be asked about a live one. `mono.client` clears
		// its own set for this reason when it rebuilds; the editor did not.
		ContentAsked.clear();
		ContentPending.clear();
		ContentIssued.clear();
		ContentRequested = false;
		ContentReportedTotal = 0;

		if (settings.IsValid()) {
			ContentClient = engine::delivery::MakeAssetClient(settings);
		} else {
			// Said once, here, rather than as a failed fetch later. The two
			// reasons are worth separating because they are fixed on different
			// pages: no key is a trust problem and no source is an address one.
			ContentStatus =
				Content.PublisherKey.empty()
					? "no publisher key - nothing can be fetched, because nothing could be verified"
					: "no usable read source - check the addresses and that a row is enabled";
		}

		// **The assets panel's tabs are the same list read a different way**, so
		// they are rebuilt with the clients rather than waiting for somebody to
		// reopen the panel. An origin added on the Content page and no tab for
		// it reads as the row not having been saved.
		//
		// **Rebuilt without asking the origins**, unlike `RefreshStoreContents`.
		// This runs at start-up and on every save of the Content page, and
		// `MakeOriginLister` waits for an answer with a ceiling on the wait - a
		// mistyped address with a key beside it would then be a stall on a path
		// nobody chose. The tab says it has not been asked, and Refresh asks.
		AssetTabs = BuildCatalogue(Content);
		AssetTabsRevision++;

		// **Built even when delivery is not.** An uploader verifies nothing, so
		// it does not need a publisher key - and an editor being used to *seed*
		// an origin is exactly the case where no manifest has been signed yet
		// and `DeliverySettings::IsValid` is false.
		ContentUploads = engine::delivery::MakeUploader(settings);
	}

	void Editor::PumpContent(double frameSeconds) {
		ContentSeconds += frameSeconds;

		// **Five spans rather than one, because "content costs 0.1 ms in an idle
		// editor" is not an answer.** The things under here are a delivery
		// client polling a socket, a demand scan over every world's instances, a
		// decode-and-upload of whatever arrived, a walk over parts waiting for a
		// mesh to size them against, and an upload queue - and in an editor with
		// nothing downloading they cost very different amounts for very
		// different reasons. One bar labelled `content` could only say that the
		// total was small and non-zero, which is exactly the reading that
		// prompts somebody to go looking and find nothing.
		//
		// `content.deliver` was three of those at once, and the reading it gave
		// on an idle frame was almost entirely the demand scan rather than the
		// client it was named after. The names now match `mono.client`'s, so a
		// figure from one program means the same thing in the other.
		if (ContentClient) {
			{
				ENGINE_PROFILE_CAT("content.deliver", engine::core::ProfileCategory::Assets);
				ContentClient->Pump();
			}
			DrainContent();
		}

		// **Outside the `ContentClient` guard on purpose.** A part can meet an
		// already-loaded mesh in a process with no delivery client at all - a
		// built-in, a duplicate, an undo - and those are exactly the cases the
		// arrival-driven fit never saw.
		{
			ENGINE_PROFILE_CAT("content.fit", engine::core::ProfileCategory::Assets);
			FitPendingParts();
		}

		if (ContentUploads) {
			ENGINE_PROFILE_CAT("content.upload", engine::core::ProfileCategory::Assets);
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
		// open - a server's beside a client's during Play - and a mesh
		// registered into the renderer is registered for all of them, so a
		// catalogue filled for one would leave `TrianglesCount` answering zero
		// in the others for no reason anybody could see.
		// `EachWorld` rather than `Worlds`, which returns the list by value: this
		// runs several times a frame between the content pump and the fit pass,
		// and each call was a heap allocation for a list it walked once.
		Universe->EachWorld([this, &body](engine::world::WorldId world) {
			Universe->Enter(world, [&body](engine::ecs::Store &store) { body(store); });
		});
	}

	void Editor::DrainContent() {
		// **The editor fetches content, which it did not before at all.** Its
		// delivery client existed and nothing ever asked it for anything, so a
		// `MeshPart` in a viewport drew the fallback cube however good its
		// `MeshId` was - the renderer had never been handed a mesh. This is
		// `Client::PumpContent`'s policy, one program over.
		if (!ContentRequested && ContentClient->Ready()) {
			ContentRequested = true;
			ENGINE_INFO("assets: catalogue ready - content is fetched as the worlds name it");

			// **The list of what there is, handed to the worlds once.** Not the
			// content - the *names*. A scene has no other way to find out what a
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
			// **Named apart from the delivery client**, because they answer
			// different questions and only one of them is "is anything
			// arriving". An idle editor's `content.deliver` reading was almost
			// entirely this: a walk of every world for the names its instances
			// carry, recomputing a bit-identical answer at the frame rate. One
			// bar covering both could only say the total was small and non-zero,
			// which is exactly the reading that sends somebody looking in the
			// delivery client and finding nothing.
			ENGINE_PROFILE_CAT("content.demand", engine::core::ProfileCategory::Assets);
			OfferPublishedNames();
			RequestShownContent();
		}

		// **How much decoding and uploading one frame will do**, and the same
		// allowance the client's own intake uses. Content arrives in bursts - a
		// scene names forty meshes at once and the origin answers them together
		// - and this loop used to drain every completed request in the frame
		// that noticed them, which is a third of a second in one frame and an
		// editor that stops responding while somebody's model set lands.
		//
		// `IntakeBudget` says why it is bytes rather than a count, why what does
		// not fit is deferred rather than dropped, and why the first arrival of
		// a frame is admitted however large it is.
		// **The decode-and-upload half, named apart from the fetch.** Everything
		// below reads bytes that have already arrived and turns them into
		// meshes, textures and collision shapes - which is where a burst of
		// arrivals actually costs a frame, and it is not the delivery client.
		// The same split the client makes, so the two programs' frame graphs can
		// be read against each other.
		ENGINE_PROFILE_CAT("content.intake", engine::core::ProfileCategory::Assets);

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
			// name is what has to be unmarked - the half `D00107` warned about,
			// where unmarking only on arrival leaves a misspelled sheet expected
			// for ever and the marker never appears for the one case it exists
			// for.
			const engine::core::Name asked(ContentClient->NameOf(id));

			std::optional<engine::delivery::Asset> asset = ContentClient->Take(id);

			// **On the request finishing, not on it succeeding**, and above
			// every `continue` below so no branch can forget. An arrival needs
			// no call - `AddTexture` clears it - but doing it here as well costs
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
				// readable - they live inside the mesh file, so the demand pass
				// cannot see them.
				for (const engine::assets::Submesh &submesh : mesh.Submeshes) {
					if (!submesh.Texture.empty()) {
						(void)RequestContentAsset(engine::core::Name(submesh.Texture));
					}
				}

				if (Renderer.AddMesh(name, mesh)) {
					ContentMeshes++;

					// **Every part naming it, now that its shape is known.** A
					// `MeshId` can be set long before the geometry arrives - that
					// is the ordinary case, since naming it is what fetches it -
					// so the fit cannot happen at assignment alone.
					FitPartsToMesh(name, engine::core::Vector3{(mesh.Maximum - mesh.Minimum) * 0.5f});

					// **The sheets its submeshes name, recorded where they are
					// readable.** They live inside the mesh file, so this is the
					// one point anything can learn them - and without them a
					// script that wants to swap a model's texture has no way to
					// find out what it is wearing or what to put back. Duplicates
					// and order are kept: which run wears which is a fact, and
					// collapsing it here would lose it for good.
					std::vector<engine::core::Name> sheets;
					sheets.reserve(mesh.Submeshes.size());
					for (const engine::assets::Submesh &submesh : mesh.Submeshes) {
						sheets.emplace_back(submesh.Texture);
					}

					// **Triangle counts are world data**, so
					// `MeshPart.TrianglesCount` answers in an edited world too -
					// which is how somebody checks a mesh actually arrived.
					const auto triangles = static_cast<uint32_t>(mesh.Indices.size() / 3);

					// **The hull and the soup, baked once here rather than once
					// per world.** Quickhull over a model is not free and the
					// answer is a function of the mesh alone, so four viewports
					// on four worlds would otherwise run it four times for one
					// arrival.
					engine::scene::CollisionShapes arrived;
					engine::game::AddCollisionShapes(arrived, name, mesh);

					// **Kept as well as given out, because a world can arrive
					// after a mesh does** - the same argument the mesh facts
					// above make, and `PrepareWorld` is where it is spent.
					engine::game::MergeCollisionShapes(ContentShapes, arrived);

					EachOpenWorld([&name, triangles, &sheets, &arrived](engine::ecs::Store &store) {
						engine::scene::RecordMesh(store, name, triangles, sheets);
						engine::game::MergeCollisionShapes(store, arrived);
					});

					// **Kept, because a world can arrive after a mesh does.**
					// The line above tells the worlds that are open now; one
					// created or opened later has parts naming this mesh and a
					// catalogue that has never heard of it. `FitPendingParts`
					// is what tells it, out of this.
					ContentMeshFacts[name.Id()] = RegisteredMesh{triangles, sheets};
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
				// - what somebody publishes is what they wrote - and
				// `IsRuntimeReadable` is what says it has not been baked yet.
				// TODO(shader-assets): register baked modules in the graph shader
				// store with their stage metadata. `Renderer::AddShader` is only the
				// material fragment-variant path and cannot safely accept compute or
				// vertex modules routed through this common asset kind.
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

		// Appended after the walk, never during it - a mesh names its own
		// sheets while this vector is being drained, and pushing to a container
		// being iterated is what cost the client a whole debugging round.
		ContentPending.insert(ContentPending.end(), ContentIssued.begin(), ContentIssued.end());
		ContentIssued.clear();

		// **Said whenever the queue empties on a different total than last time,
		// where it used to be said exactly once.** An editor is not a client: a
		// client names its content at load and then stops, so one line at the end
		// of the first drain described the whole session. An editor's whole job is
		// to name content *later* - somebody picks a mesh, and that is the moment
		// they want to know whether it arrived.
		//
		// The once-only version reported `0 mesh(es)` on the frame the catalogue
		// opened and then never spoke again, so every asset chosen after start-up
		// loaded in silence. When the picker beside it was also dropping every
		// choice, the two failures were indistinguishable from one: nothing
		// changed on screen and nothing was written down.
		//
		// **Gated on the counts rather than on the queue**, so a pump that drains
		// nothing new says nothing - otherwise this would be a line per frame for
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
			store.Each<engine::scene::Visual, engine::scene::Bounds>([&](engine::ecs::Entity,
																		 engine::scene::Visual &visual,
																		 engine::scene::Bounds &bounds) {
				// **Only when the mesh changed, which is what `Visual::Fitted`
				// records.** This runs whenever geometry arrives - a republish,
				// a reopened place, another part pulling the same mesh in - and
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
				//     box the mesh is *stretched* into - a character in a cubic
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
				const float span = std::max({bounds.HalfExtent.X, bounds.HalfExtent.Y, bounds.HalfExtent.Z});
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
			});
		});
	}

	void Editor::FitPendingParts() {
		if (Universe == nullptr) {
			return;
		}

		// **Gathered first, applied second.** `MeshExtentOf` is the renderer's
		// and `Each` is inside `Universe::Enter`, so asking the renderer from
		// within the walk would be reaching out of a scoped store - the rule at
		// the top of `Editor.hpp`. It is also a walk that writes, and the names
		// are what decide whether anything is written at all.
		std::vector<engine::core::Name> waiting;

		EachOpenWorld([&waiting](engine::ecs::Store &store) {
			store.Each<const engine::scene::Visual>(
				[&waiting, &store](engine::ecs::Entity, const engine::scene::Visual &visual) {
					if (!visual.Mesh.IsValid()) {
						return;
					}

					// **Two reasons a mesh is pending, and the second is not the
					// first.** A part that has never been fitted needs the shape; a
					// world whose catalogue has never heard of the mesh needs the
					// facts. They come apart when a world is loaded from a file -
					// `Visual::Fitted` is saved with the part, so a reopened place
					// is fully fitted and knows no triangle counts at all.
					const bool unfitted = visual.Fitted != visual.Mesh;
					const bool unknown = engine::scene::TrianglesOf(store, visual.Mesh) == 0;
					if (!unfitted && !unknown) {
						return;
					}

					if (std::find(waiting.begin(), waiting.end(), visual.Mesh) == waiting.end()) {
						waiting.push_back(visual.Mesh);
					}
				}
			);
		});

		for (const engine::core::Name &mesh : waiting) {
			// **Only a mesh the renderer holds.** A part naming one that has not
			// arrived - or never will - is left alone rather than fitted to
			// nothing, which is what keeps a misspelled `MeshId` a fallback cube
			// instead of a part collapsed to zero.
			engine::core::Vector3 extent;
			if (!Renderer.MeshExtentOf(mesh, extent)) {
				continue;
			}

			FitPartsToMesh(mesh, extent);

			// **And tell any world that has not heard of it.** The catalogue is
			// written at intake into the worlds that were open then, so a world
			// created or opened afterwards holds parts naming a mesh it knows
			// nothing about: `TrianglesCount` reads zero for ever while the
			// geometry draws perfectly, which is the properties panel appearing
			// never to update.
			//
			// Guarded on the count rather than written unconditionally, so this
			// is a lookup per pending mesh rather than a write per frame - and
			// so a republish, which *does* go through the intake path, is not
			// overwritten here with what this cached.
			const auto known = ContentMeshFacts.find(mesh.Id());
			if (known == ContentMeshFacts.end()) {
				continue;
			}

			EachOpenWorld([&mesh, &known](engine::ecs::Store &store) {
				if (engine::scene::TrianglesOf(store, mesh) != 0) {
					return;
				}
				engine::scene::RecordMesh(store, mesh, known->second.Triangles, known->second.Sheets);
			});
		}
	}

	void Editor::PublishManifestNames() {
		const engine::assets::Manifest *catalogue = ContentClient ? ContentClient->Catalogue() : nullptr;
		if (catalogue == nullptr) {
			return;
		}

		// **Runtime-readable only.** A `.pmx` and a `.amesh` are both
		// `AssetKind::Mesh` and only the second is something the runtime decodes,
		// so offering both would put names in a scene's list that can be named,
		// fetched and then refused - a cell drawing the fallback cube with a
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
		// and stayed at six through the whole play session - which reads exactly
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
		// mean here.** `delivery/Client.hpp` forbids a background thread - a
		// completion arriving at a moment scheduling chose would be a desync -
		// so `Pump` does its work on this thread, and the unit it fetches is a
		// *bundle*. Issuing five hundred requests at once therefore asks for
		// five hundred bundles' worth of decompression before the next frame.
		//
		// Issuing a few per pump turns the same load into content appearing over
		// a second or two, which is what an editor opening a large place should
		// look like. The collection is idempotent, so what is not issued this
		// pump is simply issued on the next - there is no queue to keep in step.
		// **A member cleared rather than a local rebuilt.** This runs every pump
		// and the list is discarded every pump, so a local was one allocation
		// and a geometric regrowth per frame for an answer that is almost always
		// the same one.
		std::vector<engine::core::Name> &wanted = WantedContent;
		wanted.clear();
		EachOpenWorld([&wanted](engine::ecs::Store &store) { client::CollectWantedContent(store, wanted); });

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
		// **The already-asked probe first, and the order is the whole cost of
		// this function on an idle frame.** Every name a world carries reaches
		// here every pump, and almost all of them have been asked for already -
		// so the cheapest question that can end the call has to be the first
		// one. `Name::Text()` takes a shared lock on a process-wide table and
		// `BuiltinFromName` then compares six strings, both of which used to run
		// before the hash probe that already knew the answer. The shipped client
		// has always had these the right way round; the editor had not.
		//
		// A built-in now enters `ContentAsked` on its first sight, which is six
		// extra ids and no change in what is fetched: the check below still
		// refuses it before anything is requested.
		if (!ContentClient || !asset.IsValid() || !ContentAsked.insert(asset.Id()).second) {
			return false;
		}

		// **A built-in is never fetched, because there is nothing to fetch.**
		// `engine.Cube` and its five siblings are generated by
		// `assets::MakeBuiltin` and registered by `MeshTable::Initialise` before
		// any delivery client exists, so they resolve in every process with no
		// store at all. No manifest names one, so a request for it can only come
		// back as a miss - a warning per built-in in the log of anybody using
		// the picker's default rows, describing content that is already there.
		if (engine::assets::BuiltinMesh ignored; engine::assets::BuiltinFromName(asset.Text(), ignored)) {
			return false;
		}

		// **The same door the shipped client has**, and it is here rather than
		// only there because a rule that holds in a game binary and not in the
		// editor is one an author meets for the first time after shipping.
		if (const engine::assets::ContentForm form = engine::assets::FormOfName(asset.Text());
			!engine::assets::ContentPolicy::Process(engine::assets::ContentVerb::Handle).Allows(form)) {
			ContentStatus = "not asking for " + std::string(asset.Text()) + " - " +
							engine::assets::Describe(form) + " content is turned off";
			ENGINE_INFO("content: {}", ContentStatus);
			return false;
		}

		ContentIssued.push_back(ContentClient->Request(asset.Text()));

		// **Marked before the answer, which is the whole point.** Until this
		// request finishes, a part naming this sheet draws as the default
		// material rather than as the purple marker - so a scene load looks like
		// untextured parts becoming textured instead of a purple shimmer. See
		// `render::ChooseTexture`; the unmark is in `DrainContent`, on the
		// request finishing rather than on it succeeding.
		Renderer.ExpectTexture(asset);
		return true;
	}

	void Editor::UploadStore() {
		if (!ContentUploads) {
			ContentStatus = "no write source - give a row the write role and an ingest key";
			return;
		}

		// **`raw/` and not `processed/`.** What an origin's inbox wants is
		// content, and `processed/` is a *published* store - chunks under hash
		// names plus a signed manifest, which would arrive as several thousand
		// unrecognisable files. The far end publishes what it receives; sending
		// it something already published would be publishing twice.
		const cdn::LocalPaths paths = cdn::DefaultLocalPaths();

		std::error_code failure;
		if (!std::filesystem::is_directory(paths.Raw, failure)) {
			ContentStatus = "nothing to upload - the content store is empty";
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
			ContentStatus = "delivery is not configured - see the Content page in Preferences";
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
							paths.Raw /
							(asset->Root.ToHex() + std::filesystem::path(asset->Name).extension().string());
						std::ofstream out(stored, std::ios::binary | std::ios::trunc);
						out.write(
							reinterpret_cast<const char *>(asset->Bytes.data()),
							static_cast<std::streamsize>(asset->Bytes.size())
						);
						ContentStatus = out.good() ? pending.Name + " - " +
														 ReadableRate(asset->Bytes.size()) + " into the store"
												   : pending.Name + " - could not be written";
					}
				}
			} else {
				ContentStatus = pending.Name + " - every source was tried and none answered";
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

			NetworkRow(
				"Down",
				PerSecond(rates.DownPerSecond),
				"compressed bytes off the wire, over the last few seconds"
			);
			NetworkRow("Up", PerSecond(rates.UpPerSecond), "bytes sent to write origins");

			if (ContentClient) {
				NetworkRow("In flight", std::to_string(ContentClient->Outstanding()) + " request(s)");
			}
			if (ContentUploads) {
				NetworkRow("Queued", std::to_string(ContentUploads->Remaining()) + " upload(s)");
			}
			ImGui::EndTable();
		}

		if (rates.WindowSeconds <= 0.0) {
			ImGui::TextDisabled("no window yet - rates appear after a second of samples");
		}

		// --- downloading ------------------------------------------------------

		ImGui::SeparatorText("Downloading");

		if (!ContentClient) {
			ImGui::TextColored(
				ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled),
				"not configured - Preferences > Content needs a publisher key and a readable source"
			);
		} else {
			const engine::delivery::DeliveryCounters &counters = ContentClient->Counters();

			if (ImGui::BeginTable("##down", 2, ImGuiTableFlags_SizingStretchProp)) {
				ImGui::TableSetupColumn("##what", ImGuiTableColumnFlags_WidthStretch, 0.45f);
				ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch, 0.55f);

				NetworkRow("Manifest", ContentClient->Ready() ? "verified" : "waiting");
				NetworkRow(
					"Cache hits",
					std::to_string(counters.CacheHits),
					"assets served without touching a source"
				);
				NetworkRow("Cache misses", std::to_string(counters.CacheMisses));
				NetworkRow("Bundles", std::to_string(counters.Bundles));
				NetworkRow(
					"Transferred", ReadableRate(counters.TransferredBytes), "as it travelled - compressed"
				);
				NetworkRow("Expanded", ReadableRate(counters.ExpandedBytes), "what those became");

				// **The pair is what answers 'did this travel compressed'**, and
				// it is a question about the wire - so it is measured at it
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
					NetworkRow("Compression", ratio);
				}

				NetworkRow(
					"Source failures",
					std::to_string(counters.SourceFailures),
					"times a source was passed over"
				);

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
				ImGui::BulletText("%s - waiting", pending.Name.c_str());
			}
		}

		// --- uploading --------------------------------------------------------

		ImGui::SeparatorText("Uploading");

		if (!ContentUploads) {
			ImGui::TextColored(
				ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled),
				"no write source - give a row the write role and an ingest key in Preferences > Content"
			);
		} else {
			const engine::delivery::UploadCounters &counters = ContentUploads->Counters();

			if (ImGui::BeginTable("##up", 2, ImGuiTableFlags_SizingStretchProp)) {
				ImGui::TableSetupColumn("##what", ImGuiTableColumnFlags_WidthStretch, 0.45f);
				ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch, 0.55f);

				NetworkRow("Destinations", std::to_string(ContentUploads->Destinations().size()));
				NetworkRow("Stored", std::to_string(counters.Stored));
				NetworkRow(
					"Already there",
					std::to_string(counters.Skipped),
					"the probe that makes a re-upload cheap"
				);
				NetworkRow("Sent", ReadableRate(counters.SentBytes));
				NetworkRow(
					"Refused",
					std::to_string(counters.Refused),
					"wrong key, or an origin that takes no writes"
				);
				NetworkRow("Failed", std::to_string(counters.Failed));
				ImGui::EndTable();
			}

			for (const engine::delivery::Source &target : ContentUploads->Destinations()) {
				ImGui::BulletText("%s - %s", target.Name.c_str(), target.Location.c_str());
			}

			ImGui::BeginDisabled(ContentUploads->Remaining() > 0);
			if (ImGui::Button("Upload the content store")) {
				UploadStore();
			}
			ImGui::EndDisabled();

			if (ContentUploads->Remaining() > 0 && UploadQueued > 0) {
				const size_t done = UploadQueued > ContentUploads->Remaining()
										? UploadQueued - ContentUploads->Remaining()
										: 0;
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
