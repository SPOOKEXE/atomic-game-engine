// Signed C06 content crosses the production client pump with an empty then populated disk cache.
// Authored setup and full byte/scene oracles stay outside each complete client-lifecycle phase.
#include <engine/assets/ChunkStore.hpp>
#include <engine/assets/ContentHash.hpp>
#include <engine/assets/Manifest.hpp>
#include <engine/assets/Signature.hpp>
#include <engine/bake/Procedural.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/core/Name.hpp>
#include <engine/delivery/Cache.hpp>
#include <engine/delivery/Client.hpp>
#include <engine/delivery/IntakeBudget.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/procedural/Region.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/script/BakedProceduralReference.hpp>
#include <engine/script/ProceduralInputs.hpp>
#include <engine/script/ProceduralJobs.hpp>
#include <engine/script/ProceduralScene.hpp>
#include <engine/testing/Bench.hpp>

#include <algorithm>
#include <array>
#include <assetc/Procedural.hpp>
#include <cdn/Publisher.hpp>
#include <chrono>
#include <client/ProceduralContent.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

TEST_SUITE_ID("unified.bench.procedural-delivery")

namespace {
	using namespace engine;
	using Clock = std::chrono::steady_clock;
	namespace fs = std::filesystem;
	constexpr int64_t CELL = procedural::SUBCELLS_PER_CELL;
	constexpr size_t REGIONS = 2, MAXIMUM_PUMPS = 2000;
	// grug bound diagnostic encoding above the two regions' checked geometry/asset budgets.
	constexpr size_t MAXIMUM_PROOF_BYTES = 128u * 1024u * 1024u;
	struct Scenario {
		std::string_view Name;
		uint16_t Side, Villages, Roads, Splines, Overrides;
	};
	constexpr std::array<Scenario, 4> SCENARIOS{
		{{"empty", 16, 0, 0, 0, 0},
		 {"simple", 16, 1, 1, 0, 0},
		 {"busy", 32, 8, 8, 0, 0},
		 {"complex", 64, 32, 64, 8, 32}}
	};
	void Require(bool accepted, std::string_view message) {
		if (!accepted) throw std::runtime_error(std::string(message));
	}
	bool Enabled(const char *name) {
		const char *value = std::getenv(name);
		return value && value[0] == '1' && value[1] == '\0';
	}
	struct Scratch {
		fs::path Root;
		Scratch() {
			const char *parent = std::getenv("MONO_PROCEDURAL_DELIVERY_ROOT");
			Require(parent && *parent, "MONO_PROCEDURAL_DELIVERY_ROOT must name a build scratch directory");
			fs::create_directories(parent);
			const auto nonce = Clock::now().time_since_epoch().count();
			for (size_t attempt = 0; attempt < 64; ++attempt) {
				auto candidate =
					fs::path(parent) / ("c06-" + std::to_string(nonce) + "-" + std::to_string(attempt));
				if (fs::create_directory(candidate)) {
					Root = std::move(candidate);
					return;
				}
			}
			throw std::runtime_error("procedural delivery could not claim an isolated scratch directory");
		}
		Scratch(const Scratch &) = delete;
		Scratch &operator=(const Scratch &) = delete;
		~Scratch() {
			std::error_code ignored;
			fs::remove_all(Root, ignored);
		}
	};
	script::ProceduralSceneTarget Target(ecs::Store &world, procedural::RegionScope scope, bool bind) {
		const auto mesh = world.CreateInstance(scene::EditableMeshClass(), "C06Region");
		scene::PartDesc description;
		description.Class = ecs::Classes::Find(core::Name("MeshPart"));
		description.Mesh = scene::EditableMeshContentName(world, mesh);
		const auto part = scene::MakePart(world, description);
		Require(part != ecs::NULL_ENTITY, "procedural delivery target refused");
		auto *collider = world.GetMutable<scene::Collider>(part);
		Require(collider != nullptr, "procedural delivery collider missing");
		collider->CanQuery = false;
		if (bind)
			collider->Mask = spatial::LayerMask::None();
		else
			collider->Trigger = true;
		script::ProceduralSceneTarget target;
		procedural::Diagnostic diagnostic;
		Require(
			bind ? script::BindProceduralSceneTarget(world, scope, mesh, part, target, diagnostic)
				 : script::CaptureProceduralSceneTarget(world, scope, mesh, part, target, diagnostic),
			"procedural delivery target capture refused"
		);
		return target;
	}

	struct Fixture {
		Scratch Files;
		ecs::Store World{"c06-delivery-authority"};
		std::array<procedural::RegionScope, REGIONS> Scopes{};
		std::array<script::ProceduralSceneTarget, REGIONS> Targets{};
		std::array<bake::ProceduralBake, REGIONS> Baked{};
		std::map<std::string, std::vector<std::byte>> Assets;
		assets::Manifest Manifest;
		cdn::PublishReport Publication;
		delivery::DeliverySettings Settings;
		size_t Requests = REGIONS, ArtifactBytes = 0;
		uint64_t WorkUnits = 0, CandidateCells = 0, SpatialTests = 0;
		explicit Fixture(const Scenario &scenario) {
			scene::RegisterSceneComponents();
			script::RegisterProceduralComponents();
			Scopes = {{{0, 0, scenario.Side}, {20, 0, scenario.Side}}};
			auto recipe =
				procedural::ForestVillageDefaults("procedural-b0-" + std::string(scenario.Name), 91);
			if (scenario.Villages == 0)
				for (auto &node : recipe.Nodes)
					for (auto &parameter : node.Parameters)
						if (parameter.Name == "density_ppm") parameter.Value = uint64_t{0};
			std::vector<script::ProceduralSource> sources;
			std::vector<procedural::Point> villages;
			for (uint16_t index = 0; index < scenario.Villages; ++index) {
				const int64_t region = scenario.Villages > 1 && index >= scenario.Villages / 2 ? 1 : 0;
				const int64_t local = scenario.Villages > 1 ? index % (scenario.Villages / 2) : 0;
				const int64_t step = scenario.Side / 4;
				const procedural::Point anchor{
					(region * 20 * scenario.Side + 4 + local % 4 * step) * CELL, (4 + local / 4 * step) * CELL
				};
				villages.push_back(anchor);
				sources.push_back(
					{procedural::SourceKind::Village, "village-" + std::to_string(index), anchor, ""}
				);
			}
			for (uint16_t index = 0; index < scenario.Roads; ++index) {
				const uint16_t village = index % scenario.Villages;
				const auto anchor = villages[village];
				sources.push_back(
					{procedural::SourceKind::Entrance,
					 "gate-" + std::to_string(index),
					 {anchor.X + 4 * CELL, anchor.Y + (index >= scenario.Villages ? CELL : 0)},
					 "village-" + std::to_string(village)}
				);
			}
			procedural::Diagnostic diagnostic;
			Require(
				script::ConfigureProceduralWorld(World, "b0-owner", recipe, Scopes, sources, diagnostic),
				"procedural delivery authority refused"
			);
			for (uint16_t index = 0; index < scenario.Splines; ++index) {
				const auto anchor = villages[index % scenario.Villages];
				Require(
					script::SetProceduralSpline(
						World,
						{"bend-" + std::to_string(index),
						 "gate-" + std::to_string(index),
						 {anchor.X + 2 * CELL, anchor.Y + CELL}},
						diagnostic
					),
					"procedural delivery spline refused"
				);
			}
			for (uint16_t index = 0; index < scenario.Overrides; ++index)
				Require(
					script::SetProceduralOverride(
						World,
						{"tree/" + std::to_string(index) + "/3",
						 procedural::OverrideAction::Replace,
						 {index * CELL + 1, 3 * CELL + 1},
						 0,
						 0,
						 0,
						 ""},
						diagnostic
					),
					"procedural delivery override refused"
				);
			script::ProceduralJobs jobs(World);
			std::array<uint64_t, REGIONS> tickets{};
			for (size_t index = 0; index < REGIONS; ++index) {
				Targets[index] = Target(World, Scopes[index], true);
				tickets[index] = jobs.Submit(World, Scopes[index], parallel::JobContext::Serial);
				Require(tickets[index] != 0, "procedural delivery generation refused");
			}
			jobs.Poll();
			Require(
				jobs.PublishSceneBatch(World, tickets, Targets, World.Time().Tick),
				"procedural delivery scene publish refused"
			);
			for (auto ticket : tickets) {
				script::ProceduralProgress progress;
				Require(jobs.Progress(ticket, progress), "procedural delivery progress missing");
				WorkUnits += progress.WorkUnits;
				CandidateCells += progress.CandidateCells;
				SpatialTests += progress.SpatialTests;
			}
			const auto *published = World.Resource<script::ProceduralPublished>();
			Require(
				published && published->Regions.size() == REGIONS,
				"procedural delivery publication incomplete"
			);
			std::string failure;
			for (size_t index = 0; index < REGIONS; ++index) {
				const auto &bytes = published->Regions[index].ArtifactBytes;
				ArtifactBytes += bytes.size();
				Require(assetc::CookProceduralArtifact(bytes, Scopes[index], Baked[index], failure), failure);
				const auto name = "region-" + std::to_string(index) + ".aproc";
				Require(
					assetc::PublishProceduralBake(Files.Root / "content", name, Baked[index], failure),
					failure
				);
				Assets.emplace(name, Baked[index].DescriptionBytes);
				Requests += Baked[index].Meshes.size();
				for (const auto &mesh : Baked[index].Meshes) {
					const auto [found, inserted] = Assets.emplace(mesh.Name, mesh.Bytes);
					Require(
						inserted || found->second == mesh.Bytes, "procedural delivery mesh name collision"
					);
				}
			}
			auto key = assets::SigningKey::FromSeed(std::array<std::byte, 32>{});
			Require(key.has_value(), "procedural delivery signing key refused");
			cdn::PublishSettings publish;
			publish.TrainDictionary = false;
			auto report = cdn::Publish(Files.Root / "content", Files.Root / "store", *key, publish);
			Require(report.has_value() && report->Refused == 0, "procedural delivery signed publish refused");
			Publication = std::move(*report);
			auto store = assets::ChunkStore::Open(Files.Root / "store", false);
			Require(store.has_value(), "procedural delivery store missing");
			assets::SignatureBytes signature;
			auto manifest = store->ReadManifest(signature);
			Require(
				manifest.has_value() &&
					assets::VerifyManifestRoot(manifest->Root(), signature, key->Public()),
				"procedural delivery manifest signature refused"
			);
			Manifest = std::move(*manifest);
			Settings.Publisher = key->Public();
			Settings.CachePath = Files.Root / "cache";
			Settings.Sources.push_back(
				{.Name = "signed-store",
				 .Kind = delivery::SourceKind::Directory,
				 .Location = (Files.Root / "store").string(),
				 .Enabled = true,
				 .Role = delivery::SourceRole::Read}
			);
		}
	};

	struct Recipient {
		ecs::Store World{"c06-delivery-recipient"};
		std::array<script::ProceduralSceneTarget, REGIONS> Targets{};
		explicit Recipient(const Fixture &fixture) {
			procedural::Diagnostic diagnostic;
			for (size_t index = 0; index < REGIONS; ++index) {
				Targets[index] = Target(World, fixture.Scopes[index], false);
				const auto &target = Targets[index];
				Require(
					script::SetBakedProceduralReference(
						World,
						target.Scope,
						target.Mesh,
						target.Part,
						"region-" + std::to_string(index) + ".aproc",
						diagnostic
					),
					"procedural delivery reference refused"
				);
			}
		}
	};
	struct Phase {
		delivery::DeliveryCounters Delivery{};
		client::ProceduralContentCounters Content{};
		size_t Pumps = 0, PeakOutstanding = 0, Absorbed = 0, Deferred = 0;
		core::HeapTotals Before{}, After{};
		uint64_t Nanoseconds = 0;
	};
	Phase Deliver(const Fixture &fixture, Recipient &recipient, bool memory) {
		Phase report;
		if (memory) {
			core::HeapProfile::ResetTotals();
			report.Before = core::HeapProfile::Totals();
		}
		const auto started = Clock::now();
		{
			auto delivery = delivery::MakeAssetClient(fixture.Settings);
			Require(delivery != nullptr, "procedural delivery client refused");
			client::ProceduralContent content;
			delivery::IntakeBudget budget;
			while (report.Pumps < MAXIMUM_PUMPS) {
				++report.Pumps;
				delivery->Pump();
				budget.Begin();
				content.BeginFrame();
				content.PumpWorld(recipient.World, *delivery, budget);
				content.EndFrame(*delivery);
				report.PeakOutstanding = std::max(report.PeakOutstanding, delivery->Outstanding());
				report.Absorbed += budget.Absorbed();
				report.Deferred += budget.Deferred();
				if (content.Counters().Installed == REGIONS || content.Counters().Refused) break;
			}
			report.Delivery = delivery->Counters();
			report.Content = content.Counters();
			Require(
				delivery->Ready() && delivery->Catalogue()->Root() == fixture.Manifest.Root(),
				"procedural delivery catalogue changed"
			);
			Require(
				report.Content.Installed == REGIONS && report.Content.Refused == 0 &&
					report.Content.Pending == 0 && delivery->Outstanding() == 0,
				"procedural delivery did not finish cleanly within pump bound"
			);
		}
		report.Nanoseconds = static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count()
		);
		if (memory) report.After = core::HeapProfile::Totals();
		return report;
	}

	std::string CheckBytes(const Fixture &fixture) {
		auto cache =
			delivery::ContentCache::Open(fixture.Settings.CachePath, fixture.Settings.CacheCapacityBytes);
		Require(cache.has_value(), "procedural delivery cache missing");
		core::ByteWriter canonical(0, MAXIMUM_PROOF_BYTES);
		canonical.WriteUInt64(fixture.Assets.size());
		for (const auto &[name, expected] : fixture.Assets) {
			const auto *entry = fixture.Manifest.Find(name);
			Require(
				entry && assets::VerifyAsset(*entry, expected),
				"procedural delivery publication byte oracle failed"
			);
			auto actual = cache->Find(*entry);
			Require(
				actual && *actual == expected, "procedural delivery cached bytes differ from authored bake"
			);
			canonical.WriteString(name);
			canonical.WriteUInt64(actual->size());
			canonical.WriteRaw(actual->data(), actual->size());
		}
		return assets::Hasher::Of(canonical.Bytes()).ToHex();
	}
	std::string CheckScene(const Fixture &fixture, const Recipient &recipient) {
		Require(
			!recipient.World.HasResource<script::ProceduralAuthority>() &&
				!recipient.World.HasResource<script::ProceduralPublished>(),
			"procedural delivery installed generator authority"
		);
		core::ByteWriter canonical(0, MAXIMUM_PROOF_BYTES);
		for (size_t index = 0; index < REGIONS; ++index) {
			const auto &target = recipient.Targets[index];
			const auto *actual = recipient.World.Get<scene::EditableMesh>(target.Mesh);
			const auto *region = recipient.World.Get<script::ProceduralSceneRegion>(target.Mesh);
			const auto *expected = fixture.World.Get<scene::EditableMesh>(fixture.Targets[index].Mesh);
			const auto *expectedRegion =
				fixture.World.Get<script::ProceduralSceneRegion>(fixture.Targets[index].Mesh);
			Require(
				actual && region && expected && expectedRegion, "procedural delivery installed scene missing"
			);
			Require(
				actual->Positions == expected->Positions && actual->Normals == expected->Normals &&
					actual->UVs == expected->UVs && actual->Colours == expected->Colours &&
					actual->Alphas == expected->Alphas && actual->Indices == expected->Indices,
				"procedural delivery full mesh parity failed"
			);
			Require(
				region->Scope.X == fixture.Scopes[index].X && region->Scope.Y == fixture.Scopes[index].Y &&
					region->Scope.CellsPerSide == fixture.Scopes[index].CellsPerSide &&
					region->ArtifactDigest.Bytes ==
						assets::Hasher::Of(fixture.Baked[index].DescriptionBytes).Digest &&
					region->Objects.size() == expectedRegion->Objects.size(),
				"procedural delivery scope/digest/object count changed"
			);
			const auto position = recipient.World.Get<scene::Transform>(target.Part)->Frame.Position;
			Require(
				position ==
						fixture.World.Get<scene::Transform>(fixture.Targets[index].Part)->Frame.Position &&
					recipient.World.Get<scene::Visual>(target.Part)->Fitted == target.Content &&
					recipient.World.Get<scene::Collider>(target.Part)->Mask == spatial::LayerMask::None(),
				"procedural delivery placement/binding changed"
			);
			canonical.WriteInt64(region->Scope.X);
			canonical.WriteInt64(region->Scope.Y);
			canonical.WriteUInt16(region->Scope.CellsPerSide);
			canonical.WriteRaw(region->ArtifactDigest.Bytes.data(), region->ArtifactDigest.Bytes.size());
			canonical.WriteFloat(position.X);
			canonical.WriteFloat(position.Y);
			canonical.WriteFloat(position.Z);
			canonical.WriteUInt64(actual->Positions.size());
			for (const auto &value : actual->Positions) {
				canonical.WriteFloat(value.X);
				canonical.WriteFloat(value.Y);
				canonical.WriteFloat(value.Z);
			}
			canonical.WriteUInt64(actual->Normals.size());
			for (const auto &value : actual->Normals) {
				canonical.WriteFloat(value.X);
				canonical.WriteFloat(value.Y);
				canonical.WriteFloat(value.Z);
			}
			canonical.WriteUInt64(actual->UVs.size());
			for (const auto &value : actual->UVs) {
				canonical.WriteFloat(value.X);
				canonical.WriteFloat(value.Y);
			}
			canonical.WriteUInt64(actual->Colours.size());
			for (const auto &value : actual->Colours) {
				canonical.WriteFloat(value.R);
				canonical.WriteFloat(value.G);
				canonical.WriteFloat(value.B);
			}
			canonical.WriteUInt64(actual->Alphas.size());
			for (float value : actual->Alphas)
				canonical.WriteFloat(value);
			canonical.WriteUInt64(actual->Indices.size());
			for (auto value : actual->Indices)
				canonical.WriteUInt32(value);
			canonical.WriteUInt64(region->Objects.size());
			for (size_t object = 0; object < region->Objects.size(); ++object) {
				const auto &actualObject = region->Objects[object];
				const auto &expectedObject = expectedRegion->Objects[object];
				Require(
					actualObject.Id == expectedObject.Id && actualObject.Kind == expectedObject.Kind &&
						actualObject.FirstIndex == expectedObject.FirstIndex &&
						actualObject.IndexCount == expectedObject.IndexCount &&
						actualObject.Material == expectedObject.Material &&
						actualObject.Variant == expectedObject.Variant,
					"procedural delivery full object parity failed"
				);
				canonical.WriteString(actualObject.Id);
				canonical.WriteString(actualObject.Kind);
				canonical.WriteUInt32(actualObject.FirstIndex);
				canonical.WriteUInt32(actualObject.IndexCount);
				canonical.WriteUInt16(actualObject.Material);
				canonical.WriteUInt16(actualObject.Variant);
			}
		}
		return assets::Hasher::Of(canonical.Bytes()).ToHex();
	}
	void Print(
		const Scenario &scenario,
		std::string_view phase,
		const Phase &report,
		bool memory,
		std::string_view bytesHash,
		std::string_view sceneHash
	) {
		const auto &counts = report.Delivery;
		Require(
			counts.SourceFailures == 0 && counts.VerificationFailures == 0,
			"procedural delivery source/verification failure"
		);
		std::cout << "# c06 delivery\t" << scenario.Name << '\t' << phase
				  << "\ttiming_valid=0\tphase_ns=" << report.Nanoseconds
				  << "\tcache_hits=" << counts.CacheHits << "\tcache_misses=" << counts.CacheMisses
				  << "\tsource_bundle_payload_bytes=" << counts.TransferredBytes
				  << "\texpanded_bundle_payload_bytes=" << counts.ExpandedBytes
				  << "\tbundles=" << counts.Bundles << "\tsource_failures=" << counts.SourceFailures
				  << "\tverification_failures=" << counts.VerificationFailures
				  << "\tinstalled=" << report.Content.Installed << "\trefused=" << report.Content.Refused
				  << "\tpending=" << report.Content.Pending << "\tpumps=" << report.Pumps
				  << "\tpeak_outstanding=" << report.PeakOutstanding
				  << "\tassets_absorbed=" << report.Absorbed << "\tarrivals_deferred=" << report.Deferred
				  << "\tfull_mesh_object_parity=1\tcanonical_asset_hash=" << bytesHash
				  << "\tscene_hash=" << sceneHash;
		if (memory)
			std::cout << "\tallocated_bytes=" << report.After.TotalBytes - report.Before.TotalBytes
					  << "\tallocated_blocks=" << report.After.TotalBlocks - report.Before.TotalBlocks
					  << "\tretained_bytes=" << report.After.LiveBytes - report.Before.LiveBytes
					  << "\tbaseline_live_bytes=" << report.Before.LiveBytes
					  << "\tprocess_live_bytes=" << report.After.LiveBytes
					  << "\tprocess_peak_since_reset_bytes=" << report.After.PeakBytes
					  << "\tprocess_peak_above_entry_live_bytes="
					  << report.After.PeakBytes - report.Before.LiveBytes
					  << "\tlive_blocks=" << report.After.LiveBlocks
					  << "\theader_overhead_model_bytes=" << report.After.OverheadBytes
					  << "\tblock_overhead_bytes=" << core::HeapProfile::BlockOverhead()
					  << "\tdropped_scopes_delta=" << report.After.DroppedScopes - report.Before.DroppedScopes
					  << "\tforeign_frees_delta=" << report.After.ForeignFrees - report.Before.ForeignFrees;
		std::cout << '\n';
	}
	// grug catch forces owned scratch cleanup before an unhandled benchmark error terminates the process.
	void Run(size_t shape) try {
		if (!Enabled("MONO_PROCEDURAL_DELIVERY_B0")) return;
		const bool memory = Enabled("MONO_BENCH_MEMORY");
		Require(
			!memory || core::HeapProfile::IsCompiledIn(),
			"procedural delivery memory mode requires MONO_HEAP_PROFILE"
		);
		const auto &scenario = SCENARIOS[shape];
		Fixture fixture(scenario);
		std::cout
			<< "# c06 delivery "
			   "basis\tsource=directory\tnetwork_bytes=not_applicable\tfilesystem_page_cache=unspecified"
			<< "\tmanifest_reads_excluded_from_payload_counters=1\twarm_manifest_read_verified=1\tfresh_"
			   "client_each_phase=1"
			<< "\tphase=client_construct_manifest_pump_install_destroy\tsetup_oracles_excluded=1\tintake_"
			   "bytes="
			<< delivery::IntakeBudget::FRAME_BYTES << "\tmaximum_pumps=" << MAXIMUM_PUMPS
			<< "\theap_enabled=" << memory
			<< "\theap=tracked_cpp_requested_payload_only\tmalloc_gpu_excluded=1\tprocess_peak=single_call_"
			   "since_reset\tprepared_"
			   "fixtures_resident=1\tpeak_above_entry=additional_payload_not_rss\n";
		std::string coldBytes, coldScene;
		for (bool warm : {false, true}) {
			if (!warm)
				Require(
					!fs::exists(fixture.Settings.CachePath), "procedural delivery cold cache already exists"
				);
			Recipient recipient(fixture);
			const Phase report = Deliver(fixture, recipient, memory);
			const auto &counts = report.Delivery;
			Require(
				counts.CacheHits + counts.CacheMisses == fixture.Requests &&
					report.Absorbed == fixture.Requests,
				"procedural delivery request/absorption totals changed"
			);
			Require(
				warm ? counts.CacheHits == fixture.Requests && counts.CacheMisses == 0 &&
						   counts.Bundles == 0 && counts.TransferredBytes == 0 && counts.ExpandedBytes == 0
					 : counts.CacheMisses > 0 && counts.Bundles > 0 && counts.TransferredBytes > 0 &&
						   counts.ExpandedBytes == counts.TransferredBytes,
				"procedural delivery cold/warm cache behavior changed"
			);
			const auto bytesHash = CheckBytes(fixture), sceneHash = CheckScene(fixture, recipient);
			if (warm)
				Require(
					bytesHash == coldBytes && sceneHash == coldScene,
					"procedural delivery cold/warm hashes changed"
				);
			else {
				coldBytes = bytesHash;
				coldScene = sceneHash;
			}
			Print(scenario, warm ? "warm_cache" : "empty_cache", report, memory, bytesHash, sceneHash);
		}
		size_t vertices = 0, indices = 0, objects = 0;
		for (const auto &target : fixture.Targets) {
			const auto *mesh = fixture.World.Get<scene::EditableMesh>(target.Mesh);
			vertices += mesh->Positions.size();
			indices += mesh->Indices.size();
			objects += fixture.World.Get<script::ProceduralSceneRegion>(target.Mesh)->Objects.size();
		}
		std::cout << "# c06 fixture\t" << scenario.Name << "\tseed=91\tregions=" << REGIONS
				  << "\tside=" << scenario.Side << "\tvillages=" << scenario.Villages
				  << "\troads=" << scenario.Roads << "\tsplines=" << scenario.Splines
				  << "\toverrides=" << scenario.Overrides << "\tartifact_bytes=" << fixture.ArtifactBytes
				  << "\tpublished_assets=" << fixture.Publication.Assets
				  << "\tpublished_bytes=" << fixture.Publication.ContentBytes
				  << "\tfixture_work_units=" << fixture.WorkUnits
				  << "\tfixture_candidate_cells=" << fixture.CandidateCells
				  << "\tfixture_spatial_tests=" << fixture.SpatialTests << "\tobjects=" << objects
				  << "\tvertices=" << vertices << "\tindices=" << indices << '\n';
	} catch (...) {
		throw;
	}
}
BENCH("C06 signed delivery empty cache pair", 1) {
	Run(0);
}
BENCH("C06 signed delivery simple cache pair", 1) {
	Run(1);
}
BENCH("C06 signed delivery busy cache pair", 1) {
	Run(2);
}
BENCH("C06 signed delivery complex cache pair", 1) {
	Run(3);
}
