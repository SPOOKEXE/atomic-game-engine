#include <engine/assets/ContentHash.hpp>
#include <engine/assets/ProceduralDescription.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/procedural/Geometry.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/script/BakedProceduralScene.hpp>
#include <engine/script/ProceduralInputs.hpp>
#include <engine/testing/Bench.hpp>

#include <algorithm>
#include <array>
#include <assetc/Procedural.hpp>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

TEST_SUITE_ID("tool.assetc.bench.procedural-pipeline")

namespace procedural_pipeline_bench {
	using namespace engine;
	using Clock = std::chrono::steady_clock;
	namespace fs = std::filesystem;
	constexpr int64_t CELL = procedural::SUBCELLS_PER_CELL;

	template <class Text> void Require(bool condition, const Text &message) {
		if (!condition) throw std::runtime_error(std::string(message));
	}

	void Accepted(procedural::Status status, const procedural::Diagnostic &diagnostic) {
		if (status != procedural::Status::Ok)
			throw std::runtime_error(
				"procedural B0 refusal: " + diagnostic.Field +
				" code=" + std::to_string(static_cast<unsigned>(status))
			);
	}

	void Accepted(bool success, const procedural::Diagnostic &diagnostic) {
		if (!success) Accepted(diagnostic.Code, diagnostic);
		Require(success, "procedural B0 call returned false without a diagnostic");
	}

	size_t TrialCount() {
		const char *text = std::getenv("MONO_PROCEDURAL_B0_TRIALS");
		if (!text) return 128;
		size_t count = 0;
		const std::string_view value(text);
		const auto parsed = std::from_chars(value.data(), value.data() + value.size(), count);
		Require(
			parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && count >= 1 &&
				count <= 4096,
			"MONO_PROCEDURAL_B0_TRIALS must be 1..4096"
		);
		return count;
	}

	enum class Stage : size_t {
		CaptureKey,
		Kernel,
		Encode,
		SubmitReady,
		PublishScene,
		UnchangedKey,
		UnchangedRegenerate,
		SourceEdit,
		SplineEdit,
		OverrideMove,
		OverrideDelete,
		StaleRefusal,
		AtomicRefusal,
		CancelJoin,
		AdmissionRefusal,
		Cook,
		PublishAssets,
		ReadAssets,
		InstallAssets,
		Cleanup,
		SourceRoundTrip,
		SplineRoundTrip,
		MoveRoundTrip,
		DeleteRoundTrip,
		Count
	};
	constexpr std::array<std::string_view, static_cast<size_t>(Stage::Count)> STAGE_NAMES{
		"capture_key",
		"kernel_generate",
		"artifact_encode",
		"submit_to_ready",
		"scene_publish",
		"unchanged_capture_key",
		"unchanged_resubmit_regenerates",
		"source_edit",
		"spline_edit",
		"override_move",
		"override_delete",
		"source_stale_refusal",
		"atomic_target_refusal",
		"cancel_to_observe_join",
		"admission_refusal",
		"ordinary_cook",
		"ordinary_asset_publish",
		"ordinary_asset_read",
		"ordinary_baked_install",
		"completion_cleanup",
		"source_edit_to_publish_excluding_oracle",
		"spline_edit_to_publish_excluding_oracle",
		"override_move_to_publish_excluding_oracle",
		"override_delete_to_publish_excluding_oracle"
	};

	struct Measurement {
		std::vector<uint64_t> Nanoseconds{};
		bool MeasuredPhase = false;
		bool HasHeapCounters = false;
		uint64_t AllocatedBytes = 0;
		uint64_t AllocatedBlocks = 0;
		int64_t ObservedProcessPeak = 0;
		uint64_t WorkUnits = 0, CandidateCells = 0, SpatialTests = 0;
		int64_t MaximumLiveDelta = std::numeric_limits<int64_t>::min();
	};

	struct Report {
		std::array<Measurement, static_cast<size_t>(Stage::Count)> Stages{};
		uint64_t CandidateCells = 0, WorkUnits = 0, SpatialTests = 0;
		uint64_t GeneratedPlacements = 0, GeneratedRoads = 0, ArtifactBytes = 0;
		size_t PeakRetainedRequests = 0, PeakReservedBytes = 0;
		int64_t PeakProcessBytes = 0;
		int64_t MaximumPostTrialLiveDelta = std::numeric_limits<int64_t>::min();
		explicit Report(size_t trials) {
			for (auto &stage : Stages)
				stage.Nanoseconds.reserve(trials * 8);
		}
		template <class Function> void Measure(Stage stage, Function &&function) {
			// Totals stay monotonic while a retained worker may still be exiting.
			const auto before = core::HeapProfile::Totals();
			const auto started = Clock::now();
			function();
			const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started);
			const auto after = core::HeapProfile::Totals();
			auto &row = Stages[static_cast<size_t>(stage)];
			row.MeasuredPhase = true;
			row.HasHeapCounters = core::HeapProfile::IsCompiledIn();
			row.Nanoseconds.push_back(static_cast<uint64_t>(elapsed.count()));
			row.AllocatedBytes += after.TotalBytes - before.TotalBytes;
			row.AllocatedBlocks += after.TotalBlocks - before.TotalBlocks;
			row.ObservedProcessPeak = std::max(row.ObservedProcessPeak, after.PeakBytes);
			row.MaximumLiveDelta = std::max(row.MaximumLiveDelta, after.LiveBytes - before.LiveBytes);
		}
		void Observe(Stage stage, const script::ProceduralProgress &progress, size_t bytes) {
			auto &row = Stages[static_cast<size_t>(stage)];
			row.WorkUnits += progress.WorkUnits;
			row.CandidateCells += progress.CandidateCells;
			row.SpatialTests += progress.SpatialTests;
			CandidateCells += progress.CandidateCells;
			WorkUnits += progress.WorkUnits;
			SpatialTests += progress.SpatialTests;
			GeneratedPlacements += progress.Placements;
			GeneratedRoads += progress.Roads;
			ArtifactBytes += bytes;
		}
	};

	struct Scenario {
		std::string_view Name;
		uint16_t Side = 16;
		uint16_t Villages = 0, Roads = 0, Splines = 0, Overrides = 0;
	};
	constexpr std::array<Scenario, 4> SCENARIOS{
		{{"empty", 16, 0, 0, 0, 0},
		 {"simple", 16, 1, 1, 0, 0},
		 {"busy", 32, 8, 8, 0, 0},
		 {"complex", 64, 32, 64, 8, 32}}
	};

	struct FixtureSummary {
		uint32_t CandidateCells = 0, WorkUnits = 0, SpatialTests = 0;
		size_t Placements = 0, Roads = 0, Objects = 0, KeyBytes = 0, ArtifactBytes = 0, UniqueMeshes = 0;
		uint32_t Vertices = 0, Indices = 0;
	};

	struct LatestSummary {
		uint64_t CompletedCohorts = 0;
		std::string Text{};
	};

	struct SummarySink {
		// Warmup and measured bodies run fully; only the latest complete cohort is retained.
		std::array<LatestSummary, SCENARIOS.size() * 2> Latest{};
		~SummarySink() {
			for (const auto &entry : Latest) {
				if (entry.CompletedCohorts == 0) continue;
				std::cout
					<< "procedural-b0-summary report_scope=latest-completed-body-cohort completed_cohorts="
					<< entry.CompletedCohorts << " aggregated_across_cohorts=0" << '\n'
					<< entry.Text;
			}
			std::cout.flush();
		}
	};

	SummarySink &Summaries() {
		static SummarySink sink;
		return sink;
	}

	script::ProceduralSceneTarget Target(ecs::Store &store, procedural::RegionScope scope) {
		const auto mesh = store.CreateInstance(scene::EditableMeshClass(), "B0RegionMesh");
		scene::PartDesc description;
		description.Class = ecs::Classes::Find(core::Name("MeshPart"));
		description.Mesh = scene::EditableMeshContentName(store, mesh);
		const auto part = scene::MakePart(store, description);
		Require(part != ecs::NULL_ENTITY, "procedural B0 MeshPart creation failed");
		auto *collider = store.GetMutable<scene::Collider>(part);
		Require(collider != nullptr, "procedural B0 collider missing");
		collider->Mask = spatial::LayerMask::None();
		collider->CanQuery = false;
		script::ProceduralSceneTarget target;
		procedural::Diagnostic diagnostic;
		Accepted(script::BindProceduralSceneTarget(store, scope, mesh, part, target, diagnostic), diagnostic);
		return target;
	}

	struct Oracle {
		procedural::CompiledRecipe Recipe{};
		procedural::RegionRequest Request{};
		procedural::RegionResult Result{};
		std::vector<uint8_t> Artifact{};
	};

	Oracle SerialOracle(const ecs::Store &store, procedural::RegionScope scope) {
		Oracle oracle;
		procedural::Diagnostic diagnostic;
		Accepted(
			script::CaptureProceduralRegion(store, scope, oracle.Recipe, oracle.Request, diagnostic),
			diagnostic
		);
		Accepted(
			procedural::GenerateRegion(oracle.Recipe, oracle.Request, oracle.Result, diagnostic), diagnostic
		);
		Accepted(procedural::EncodeRegionResult(oracle.Result, oracle.Artifact, diagnostic), diagnostic);
		return oracle;
	}

	std::vector<uint8_t> Key(const ecs::Store &store, procedural::RegionScope scope) {
		procedural::CompiledRecipe recipe;
		procedural::RegionRequest request;
		procedural::Diagnostic diagnostic;
		Accepted(script::CaptureProceduralRegion(store, scope, recipe, request, diagnostic), diagnostic);
		std::vector<uint8_t> key;
		Accepted(procedural::BuildRegionKey(recipe, request, key, diagnostic), diagnostic);
		return key;
	}

	struct Fixture {
		ecs::Store World{"procedural-b0"};
		std::array<procedural::RegionScope, 3> Scopes{};
		std::array<script::ProceduralSceneTarget, 3> Targets{};
		std::array<Oracle, 3> Oracles{};
		procedural::Diagnostic Diagnostic{};
		explicit Fixture(const Scenario &scenario) {
			scene::RegisterSceneComponents();
			script::RegisterProceduralComponents();
			Scopes = {{{0, 0, scenario.Side}, {20, 0, scenario.Side}, {40, 0, scenario.Side}}};
			auto definition =
				procedural::ForestVillageDefaults("procedural-b0-" + std::string(scenario.Name), 91);
			if (scenario.Villages == 0)
				for (auto &node : definition.Nodes)
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
			Accepted(
				script::ConfigureProceduralWorld(World, "b0-owner", definition, Scopes, sources, Diagnostic),
				Diagnostic
			);
			for (uint16_t index = 0; index < scenario.Splines; ++index) {
				const auto anchor = villages[index % scenario.Villages];
				Accepted(
					script::SetProceduralSpline(
						World,
						{"bend-" + std::to_string(index),
						 "gate-" + std::to_string(index),
						 {anchor.X + 2 * CELL, anchor.Y + CELL}},
						Diagnostic
					),
					Diagnostic
				);
			}
			for (uint16_t index = 0; index < scenario.Overrides; ++index)
				Accepted(
					script::SetProceduralOverride(
						World,
						{"tree/" + std::to_string(index) + "/3",
						 procedural::OverrideAction::Replace,
						 {index * CELL + 1, 3 * CELL + 1},
						 0,
						 0,
						 0,
						 ""},
						Diagnostic
					),
					Diagnostic
				);
			for (size_t index = 0; index < Scopes.size(); ++index) {
				Targets[index] = Target(World, Scopes[index]);
				Oracles[index] = SerialOracle(World, Scopes[index]);
				procedural::RegionGeometry geometry;
				Accepted(
					procedural::BuildRegionGeometry(
						Oracles[index].Result, Scopes[index], geometry, Diagnostic
					),
					Diagnostic
				);
			}
		}
		void Rebind() {
			for (auto &target : Targets)
				Accepted(
					script::BindProceduralSceneTarget(
						World, target.Scope, target.Mesh, target.Part, target, Diagnostic
					),
					Diagnostic
				);
		}
	};

	void Await(script::ProceduralJobs &jobs, size_t count) {
		const auto deadline = Clock::now() + std::chrono::seconds(60);
		while (Clock::now() < deadline) {
			jobs.Poll();
			if (jobs.Completions().size() == count) return;
			std::this_thread::yield();
		}
		throw std::runtime_error("procedural B0 worker deadline exceeded");
	}

	bool SameScope(procedural::RegionScope left, procedural::RegionScope right) {
		return left.X == right.X && left.Y == right.Y && left.CellsPerSide == right.CellsPerSide;
	}

	const script::ProceduralPublication &Publication(const Fixture &fixture, size_t index) {
		const auto *published = fixture.World.Resource<script::ProceduralPublished>();
		Require(published != nullptr, "procedural B0 missing publication resource");
		const auto found =
			std::find_if(published->Regions.begin(), published->Regions.end(), [&](const auto &row) {
				return SameScope(row.Scope, fixture.Scopes[index]);
			});
		Require(found != published->Regions.end(), "procedural B0 missing regional publication");
		return *found;
	}

	void SameMesh(const scene::EditableMesh &left, const scene::EditableMesh &right) {
		Require(
			left.Positions == right.Positions && left.Normals == right.Normals && left.UVs == right.UVs &&
				left.Colours == right.Colours && left.Alphas == right.Alphas && left.Indices == right.Indices,
			"procedural B0 visible geometry changed unexpectedly"
		);
	}

	void SameReceipt(const script::ProceduralPublication &left, const script::ProceduralPublication &right) {
		Require(
			SameScope(left.Scope, right.Scope) && left.GenerationKey == right.GenerationKey &&
				left.ArtifactDigest == right.ArtifactDigest && left.ArtifactBytes == right.ArtifactBytes &&
				left.AcceptedRevision == right.AcceptedRevision &&
				left.ActivationTick == right.ActivationTick,
			"procedural B0 refusal changed an accepted publication receipt"
		);
	}

	void SameRanges(const script::ProceduralSceneRegion &left, const script::ProceduralSceneRegion &right) {
		Require(
			SameScope(left.Scope, right.Scope) && left.ArtifactDigest == right.ArtifactDigest &&
				left.Objects.size() == right.Objects.size(),
			"procedural B0 scene identity table changed"
		);
		for (size_t index = 0; index < left.Objects.size(); ++index) {
			const auto &a = left.Objects[index];
			const auto &b = right.Objects[index];
			Require(
				a.Id == b.Id && a.Kind == b.Kind && a.FirstIndex == b.FirstIndex &&
					a.IndexCount == b.IndexCount && a.Material == b.Material && a.Variant == b.Variant,
				"procedural B0 scene object range changed"
			);
		}
	}

	void CheckFar(const Fixture &fixture, const std::vector<uint8_t> &key, const scene::EditableMesh &mesh) {
		Require(Key(fixture.World, fixture.Scopes[2]) == key, "procedural B0 distant key changed");
		Require(
			Publication(fixture, 2).ArtifactBytes == fixture.Oracles[2].Artifact,
			"procedural B0 distant artifact changed"
		);
		const auto *actual = fixture.World.Get<scene::EditableMesh>(fixture.Targets[2].Mesh);
		Require(actual && actual->Revision == mesh.Revision, "procedural B0 distant revision changed");
		SameMesh(*actual, mesh);
	}

	std::vector<uint64_t> SubmitReady(
		Fixture &fixture,
		script::ProceduralJobs &jobs,
		std::span<const size_t> indices,
		parallel::JobContext context,
		Report &report,
		Stage stage
	) {
		std::vector<uint64_t> tickets;
		tickets.reserve(indices.size());
		report.Measure(stage, [&] {
			for (size_t index : indices) {
				const auto ticket = jobs.Submit(fixture.World, fixture.Scopes[index], context);
				Require(ticket != 0, "procedural B0 submission refused");
				tickets.push_back(ticket);
			}
			Await(jobs, indices.size());
		});
		report.PeakRetainedRequests = std::max(report.PeakRetainedRequests, jobs.RetainedCount());
		report.PeakReservedBytes = std::max(report.PeakReservedBytes, jobs.ReservedBytes());
		for (size_t position = 0; position < indices.size(); ++position) {
			const auto &completion = jobs.Completions()[position];
			Require(
				completion.Status == script::ProceduralJobStatus::Ready &&
					completion.Ticket == tickets[position],
				"procedural B0 completion order/status mismatch"
			);
			Require(
				completion.ArtifactBytes == fixture.Oracles[indices[position]].Artifact,
				"procedural B0 worker differs from exact serial artifact oracle"
			);
			script::ProceduralProgress progress;
			Require(jobs.Progress(tickets[position], progress), "procedural B0 progress missing");
			report.Observe(stage, progress, completion.ArtifactBytes.size());
			const auto &expected = fixture.Oracles[indices[position]].Result.Facts;
			Require(
				progress.CandidateCells == expected.CandidateCells &&
					progress.WorkUnits == expected.WorkUnits &&
					progress.SpatialTests == expected.SpatialTests,
				"procedural B0 worker work differs from serial oracle"
			);
		}
		return tickets;
	}

	void Publish(
		Fixture &fixture,
		script::ProceduralJobs &jobs,
		std::span<const size_t> indices,
		std::span<const uint64_t> tickets,
		Report &report
	) {
		std::vector<script::ProceduralSceneTarget> targets;
		for (size_t index : indices)
			targets.push_back(fixture.Targets[index]);
		report.Measure(Stage::PublishScene, [&] {
			Require(
				jobs.PublishSceneBatch(fixture.World, tickets, targets, fixture.World.Time().Tick),
				"procedural B0 scene publication refused"
			);
		});
		for (size_t index : indices) {
			Require(
				Publication(fixture, index).ArtifactBytes == fixture.Oracles[index].Artifact,
				"procedural B0 publication differs from exact serial oracle"
			);
			const auto *region =
				fixture.World.Get<script::ProceduralSceneRegion>(fixture.Targets[index].Mesh);
			Require(
				region && region->ArtifactDigest == Publication(fixture, index).ArtifactDigest,
				"procedural B0 scene/canonical publication diverged"
			);
		}
		report.Measure(Stage::Cleanup, [&] { jobs.ClearCompletions(); });
		Require(
			jobs.RetainedCount() == 0 && jobs.ReservedBytes() == 0, "procedural B0 work reservation leaked"
		);
		fixture.Rebind();
	}

	struct Scratch {
		fs::path Root;
		explicit Scratch(std::string_view scenario, std::string_view mode) {
			const char *configured = std::getenv("MONO_PROCEDURAL_B0_ROOT");
			Require(
				configured && *configured, "MONO_PROCEDURAL_B0_ROOT must name the selected build directory"
			);
			const fs::path build = fs::canonical(configured);
			Require(
				fs::is_regular_file(build / "CMakeCache.txt"),
				"procedural B0 scratch root is not a configured build"
			);
			Require(
				!fs::is_symlink(build / "procedural-b0-assets"),
				"procedural B0 scratch directory cannot be a symlink"
			);
			Root = build / "procedural-b0-assets" / (std::string(scenario) + "-" + std::string(mode));
			fs::remove_all(Root);
			fs::create_directories(Root);
		}
		~Scratch() {
			std::error_code error;
			fs::remove_all(Root, error);
		}
		Scratch(const Scratch &) = delete;
		Scratch &operator=(const Scratch &) = delete;
	};

	std::vector<std::byte> ReadAsset(const fs::path &path) {
		std::ifstream input(path, std::ios::binary | std::ios::ate);
		Require(input.good(), "procedural B0 ordinary asset read failed");
		const auto length = input.tellg();
		Require(length >= 0 && length <= 32 * 1024 * 1024, "procedural B0 ordinary asset byte limit");
		std::vector<std::byte> bytes(static_cast<size_t>(length));
		input.seekg(0);
		input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		Require(input.good(), "procedural B0 ordinary asset truncated");
		return bytes;
	}

	void
	OrdinaryAssets(Fixture &fixture, Report &report, const Scratch &scratch, core::ByteWriter &oracleBytes) {
		std::array<bake::ProceduralBake, 2> cooked;
		std::string failure;
		report.Measure(Stage::Cook, [&] {
			for (size_t index = 0; index < cooked.size(); ++index)
				Require(
					assetc::CookProceduralArtifact(
						Publication(fixture, index).ArtifactBytes,
						fixture.Scopes[index],
						cooked[index],
						failure
					),
					failure
				);
		});
		for (size_t index = 0; index < cooked.size(); ++index) {
			oracleBytes.WriteUInt32(static_cast<uint32_t>(cooked[index].DescriptionBytes.size()));
			oracleBytes.WriteRaw(
				cooked[index].DescriptionBytes.data(), cooked[index].DescriptionBytes.size()
			);
			oracleBytes.WriteUInt32(static_cast<uint32_t>(cooked[index].Meshes.size()));
			for (const auto &mesh : cooked[index].Meshes) {
				oracleBytes.WriteString(mesh.Name);
				oracleBytes.WriteUInt32(static_cast<uint32_t>(mesh.Bytes.size()));
				oracleBytes.WriteRaw(mesh.Bytes.data(), mesh.Bytes.size());
			}
			bake::ProceduralBake repeated;
			Require(
				assetc::CookProceduralArtifact(
					fixture.Oracles[index].Artifact, fixture.Scopes[index], repeated, failure
				),
				failure
			);
			Require(
				cooked[index].DescriptionBytes == repeated.DescriptionBytes &&
					cooked[index].Meshes.size() == repeated.Meshes.size(),
				"procedural B0 cook byte oracle mismatch"
			);
			for (size_t mesh = 0; mesh < repeated.Meshes.size(); ++mesh)
				Require(
					cooked[index].Meshes[mesh].Name == repeated.Meshes[mesh].Name &&
						cooked[index].Meshes[mesh].Bytes == repeated.Meshes[mesh].Bytes,
					"procedural B0 ordinary mesh byte oracle mismatch"
				);
		}
		report.Measure(Stage::PublishAssets, [&] {
			for (size_t index = 0; index < cooked.size(); ++index)
				Require(
					assetc::PublishProceduralBake(
						scratch.Root, "region-" + std::to_string(index) + ".aproc", cooked[index], failure
					),
					failure
				);
		});
		std::array<std::vector<std::byte>, 2> descriptions;
		std::array<std::vector<std::vector<std::byte>>, 2> meshBytes;
		report.Measure(Stage::ReadAssets, [&] {
			for (size_t index = 0; index < cooked.size(); ++index) {
				descriptions[index] =
					ReadAsset(scratch.Root / ("region-" + std::to_string(index) + ".aproc"));
				for (const auto &mesh : cooked[index].Meshes)
					meshBytes[index].push_back(ReadAsset(scratch.Root / mesh.Name));
			}
		});
		ecs::Store loaded{"procedural-b0-baked-runtime"};
		std::array<std::vector<script::BakedProceduralMesh>, 2> views;
		std::array<script::BakedProceduralRegion, 2> regions;
		for (size_t index = 0; index < regions.size(); ++index) {
			Require(
				descriptions[index] == cooked[index].DescriptionBytes,
				"procedural B0 published description mismatch"
			);
			for (size_t mesh = 0; mesh < meshBytes[index].size(); ++mesh) {
				Require(
					meshBytes[index][mesh] == cooked[index].Meshes[mesh].Bytes,
					"procedural B0 published mesh mismatch"
				);
				views[index].push_back({cooked[index].Meshes[mesh].Name, meshBytes[index][mesh]});
			}
			regions[index] = {
				Target(loaded, fixture.Scopes[index]),
				assets::Hasher::Of(descriptions[index]),
				descriptions[index],
				views[index]
			};
		}
		Require(
			!loaded.HasResource<script::ProceduralAuthority>() &&
				!loaded.HasResource<script::ProceduralPublished>(),
			"procedural B0 baked runtime unexpectedly owns generator authority"
		);
		report.Measure(Stage::InstallAssets, [&] {
			Accepted(
				script::InstallBakedProceduralSceneBatch(loaded, regions, fixture.Diagnostic),
				fixture.Diagnostic
			);
		});
		for (size_t index = 0; index < regions.size(); ++index) {
			SameMesh(
				*loaded.Get<scene::EditableMesh>(regions[index].Target.Mesh),
				*fixture.World.Get<scene::EditableMesh>(fixture.Targets[index].Mesh)
			);
			Require(
				loaded.Get<scene::Transform>(regions[index].Target.Part)->Frame.Position ==
					fixture.World.Get<scene::Transform>(fixture.Targets[index].Part)->Frame.Position,
				"procedural B0 baked/live world placement mismatch"
			);
		}
	}

	template <class Function>
	void Edit(
		Fixture &fixture,
		script::ProceduralJobs &jobs,
		parallel::JobContext context,
		Report &report,
		Stage stage,
		Function &&mutate
	) {
		std::array<std::vector<uint8_t>, 3> previousKeys;
		for (size_t index = 0; index < previousKeys.size(); ++index)
			previousKeys[index] = Key(fixture.World, fixture.Scopes[index]);
		const auto started = Clock::now();
		report.Measure(stage, mutate);
		const auto oracleStarted = Clock::now();
		std::vector<size_t> changed;
		for (size_t index = 0; index < fixture.Scopes.size(); ++index) {
			if (Key(fixture.World, fixture.Scopes[index]) == previousKeys[index]) {
				Require(
					Publication(fixture, index).ArtifactBytes == fixture.Oracles[index].Artifact,
					"procedural B0 unchanged region identity changed"
				);
				continue;
			}
			Require(index != 2, "procedural B0 local edit reached distant region");
			changed.push_back(index);
			fixture.Oracles[index] = SerialOracle(fixture.World, fixture.Scopes[index]);
		}
		Require(!changed.empty(), "procedural B0 edit did not change any region key");
		const auto oracleDuration = Clock::now() - oracleStarted;
		const auto tickets = SubmitReady(fixture, jobs, changed, context, report, Stage::SubmitReady);
		Publish(fixture, jobs, changed, tickets, report);
		Stage total;
		switch (stage) {
		case Stage::SourceEdit:
			total = Stage::SourceRoundTrip;
			break;
		case Stage::SplineEdit:
			total = Stage::SplineRoundTrip;
			break;
		case Stage::OverrideMove:
			total = Stage::MoveRoundTrip;
			break;
		case Stage::OverrideDelete:
			total = Stage::DeleteRoundTrip;
			break;
		default:
			throw std::runtime_error("procedural B0 unknown edit stage");
		}
		const auto elapsed =
			std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started - oracleDuration);
		report.Stages[static_cast<size_t>(total)].Nanoseconds.push_back(
			static_cast<uint64_t>(elapsed.count())
		);
	}

	void
	Refusals(Fixture &fixture, script::ProceduralJobs &jobs, parallel::JobContext context, Report &report) {
		constexpr std::array<size_t, 2> active{0, 1};
		const auto before0 = *fixture.World.Get<scene::EditableMesh>(fixture.Targets[0].Mesh);
		const auto before1 = *fixture.World.Get<scene::EditableMesh>(fixture.Targets[1].Mesh);
		const auto published0 = Publication(fixture, 0);
		const auto published1 = Publication(fixture, 1);
		const auto ranges0 = *fixture.World.Get<script::ProceduralSceneRegion>(fixture.Targets[0].Mesh);
		const auto ranges1 = *fixture.World.Get<script::ProceduralSceneRegion>(fixture.Targets[1].Mesh);
		auto tickets = SubmitReady(fixture, jobs, active, context, report, Stage::SubmitReady);
		auto invalid = std::array{fixture.Targets[0], fixture.Targets[1]};
		++invalid[1].MeshRevision;
		report.Measure(Stage::AtomicRefusal, [&] {
			Require(
				!jobs.PublishSceneBatch(fixture.World, tickets, invalid, fixture.World.Time().Tick),
				"procedural B0 stale later target incorrectly published"
			);
		});
		SameMesh(before0, *fixture.World.Get<scene::EditableMesh>(fixture.Targets[0].Mesh));
		SameMesh(before1, *fixture.World.Get<scene::EditableMesh>(fixture.Targets[1].Mesh));
		Require(
			fixture.World.Get<scene::EditableMesh>(fixture.Targets[0].Mesh)->Revision == before0.Revision &&
				fixture.World.Get<scene::EditableMesh>(fixture.Targets[1].Mesh)->Revision == before1.Revision,
			"procedural B0 failed batch changed a mesh revision"
		);
		SameReceipt(Publication(fixture, 0), published0);
		SameReceipt(Publication(fixture, 1), published1);
		SameRanges(*fixture.World.Get<script::ProceduralSceneRegion>(fixture.Targets[0].Mesh), ranges0);
		SameRanges(*fixture.World.Get<script::ProceduralSceneRegion>(fixture.Targets[1].Mesh), ranges1);
		jobs.ClearCompletions();
		constexpr std::array<size_t, 1> near{0};
		tickets = SubmitReady(fixture, jobs, near, context, report, Stage::SubmitReady);
		const auto *authority = fixture.World.Resource<script::ProceduralAuthority>();
		const auto source =
			std::find_if(authority->Sources.begin(), authority->Sources.end(), [](const auto &row) {
				return row.Kind == procedural::SourceKind::Village && row.Name == "village-0";
			});
		Require(source != authority->Sources.end(), "procedural B0 stale source missing");
		const script::ProceduralSource previous = *source;
		auto moved = previous;
		++moved.Anchor.Y;
		Accepted(script::SetProceduralSource(fixture.World, moved, fixture.Diagnostic), fixture.Diagnostic);
		report.Measure(Stage::StaleRefusal, [&] {
			Require(
				!jobs.PublishSceneBatch(
					fixture.World, tickets, std::span(&fixture.Targets[0], 1), fixture.World.Time().Tick
				),
				"procedural B0 source-stale result incorrectly published"
			);
		});
		Accepted(
			script::SetProceduralSource(fixture.World, previous, fixture.Diagnostic), fixture.Diagnostic
		);
		jobs.ClearCompletions();
		SameMesh(before0, *fixture.World.Get<scene::EditableMesh>(fixture.Targets[0].Mesh));
		SameReceipt(Publication(fixture, 0), published0);
		Require(
			fixture.World.Get<scene::EditableMesh>(fixture.Targets[0].Mesh)->Revision == before0.Revision,
			"procedural B0 source-stale refusal changed a mesh revision"
		);
		const auto first = jobs.Submit(fixture.World, fixture.Scopes[0], context);
		const auto second = jobs.Submit(fixture.World, fixture.Scopes[1], context);
		Require(first && second, "procedural B0 cancellation submissions refused");
		const auto reserved = jobs.ReservedBytes();
		report.Measure(Stage::CancelJoin, [&] {
			Require(jobs.Cancel(first) && jobs.Cancel(second), "procedural B0 cancellation refused");
			Require(
				jobs.RetainedCount() == 2 && jobs.ReservedBytes() == reserved,
				"procedural B0 cancellation released reservation before observation"
			);
			Await(jobs, 2);
			for (const auto &completion : jobs.Completions())
				Require(
					completion.Status == script::ProceduralJobStatus::Canceled,
					"procedural B0 cancel status mismatch"
				);
			for (uint64_t ticket : {first, second}) {
				script::ProceduralProgress progress;
				Require(jobs.Progress(ticket, progress), "procedural B0 canceled progress missing");
				report.Observe(Stage::CancelJoin, progress, 0);
			}
			jobs.ClearCompletions();
		});
		Require(jobs.RetainedCount() == 0 && jobs.ReservedBytes() == 0, "procedural B0 canceled work leaked");
		// Serial completion is retained, making admission refusal independent of worker timing.
		const auto retained0 = jobs.Submit(fixture.World, fixture.Scopes[0], parallel::JobContext::Serial);
		const auto retained1 = jobs.Submit(fixture.World, fixture.Scopes[1], parallel::JobContext::Serial);
		Require(retained0 && retained1, "procedural B0 admission fixture failed");
		report.Measure(Stage::AdmissionRefusal, [&] {
			Require(
				jobs.Submit(fixture.World, fixture.Scopes[2], context) == 0 &&
					jobs.LastDiagnostic().Code == procedural::Status::LimitExceeded,
				"procedural B0 third retained request was not refused"
			);
		});
		jobs.Poll();
		jobs.ClearCompletions();
		Require(jobs.ReservedBytes() == 0, "procedural B0 admission cleanup leaked");
	}

	void RunTrial(
		const Scenario &scenario,
		parallel::JobContext context,
		Report &report,
		const Scratch &scratch,
		std::string &oracleHash,
		std::array<FixtureSummary, 2> &fixtureSummaries
	) {
		Fixture fixture(scenario);
		const bool firstTrial = oracleHash.empty();
		script::ProceduralJobs jobs(fixture.World);
		constexpr std::array<size_t, 2> active{0, 1};
		constexpr std::array<size_t, 1> far{2};
		const auto farTickets =
			SubmitReady(fixture, jobs, far, parallel::JobContext::Serial, report, Stage::SubmitReady);
		Publish(fixture, jobs, far, farTickets, report);
		const auto farKey = Key(fixture.World, fixture.Scopes[2]);
		const auto farMesh = *fixture.World.Get<scene::EditableMesh>(fixture.Targets[2].Mesh);
		core::ByteWriter oracleBytes;
		for (size_t index : active) {
			oracleBytes.WriteUInt32(static_cast<uint32_t>(fixture.Oracles[index].Artifact.size()));
			oracleBytes.WriteRaw(
				fixture.Oracles[index].Artifact.data(), fixture.Oracles[index].Artifact.size()
			);
			bake::ProceduralBake bakedOracle;
			std::string failure;
			Require(
				assetc::CookProceduralArtifact(
					fixture.Oracles[index].Artifact, fixture.Scopes[index], bakedOracle, failure
				),
				failure
			);
			oracleBytes.WriteUInt32(static_cast<uint32_t>(bakedOracle.DescriptionBytes.size()));
			oracleBytes.WriteRaw(bakedOracle.DescriptionBytes.data(), bakedOracle.DescriptionBytes.size());
			oracleBytes.WriteUInt32(static_cast<uint32_t>(bakedOracle.Meshes.size()));
			for (const auto &mesh : bakedOracle.Meshes) {
				oracleBytes.WriteString(mesh.Name);
				oracleBytes.WriteUInt32(static_cast<uint32_t>(mesh.Bytes.size()));
				oracleBytes.WriteRaw(mesh.Bytes.data(), mesh.Bytes.size());
			}
			if (firstTrial) {
				procedural::RegionGeometry geometry;
				Accepted(
					procedural::BuildRegionGeometry(
						fixture.Oracles[index].Result, fixture.Scopes[index], geometry, fixture.Diagnostic
					),
					fixture.Diagnostic
				);
				const auto &result = fixture.Oracles[index].Result;
				fixtureSummaries[index] = {
					result.Facts.CandidateCells,
					result.Facts.WorkUnits,
					result.Facts.SpatialTests,
					result.Placements.size(),
					result.Roads.size(),
					geometry.Objects.size(),
					result.KeyBytes.size(),
					fixture.Oracles[index].Artifact.size(),
					bakedOracle.Meshes.size(),
					geometry.VertexCount,
					geometry.IndexCount
				};
			}
			procedural::CompiledRecipe recipe;
			procedural::RegionRequest request;
			std::vector<uint8_t> key;
			report.Measure(Stage::CaptureKey, [&] {
				Accepted(
					script::CaptureProceduralRegion(
						fixture.World, fixture.Scopes[index], recipe, request, fixture.Diagnostic
					),
					fixture.Diagnostic
				);
				Accepted(
					procedural::BuildRegionKey(recipe, request, key, fixture.Diagnostic), fixture.Diagnostic
				);
			});
			Require(
				key == fixture.Oracles[index].Result.KeyBytes,
				"procedural B0 capture/key byte oracle mismatch"
			);
			procedural::RegionResult result;
			report.Measure(Stage::Kernel, [&] {
				Accepted(
					procedural::GenerateRegion(recipe, request, result, fixture.Diagnostic),
					fixture.Diagnostic
				);
			});
			auto &kernel = report.Stages[static_cast<size_t>(Stage::Kernel)];
			kernel.CandidateCells += result.Facts.CandidateCells;
			kernel.WorkUnits += result.Facts.WorkUnits;
			kernel.SpatialTests += result.Facts.SpatialTests;
			std::vector<uint8_t> artifact;
			report.Measure(Stage::Encode, [&] {
				Accepted(
					procedural::EncodeRegionResult(result, artifact, fixture.Diagnostic), fixture.Diagnostic
				);
			});
			Require(
				artifact == fixture.Oracles[index].Artifact,
				"procedural B0 cold generation byte oracle mismatch"
			);
		}
		auto tickets = SubmitReady(fixture, jobs, active, context, report, Stage::SubmitReady);
		Publish(fixture, jobs, active, tickets, report);
		report.Measure(Stage::UnchangedKey, [&] {
			for (size_t index : active)
				Require(
					Key(fixture.World, fixture.Scopes[index]) == fixture.Oracles[index].Result.KeyBytes,
					"procedural B0 unchanged key differs"
				);
		});
		tickets = SubmitReady(fixture, jobs, active, context, report, Stage::UnchangedRegenerate);
		Publish(fixture, jobs, active, tickets, report);
		Edit(fixture, jobs, context, report, Stage::SourceEdit, [&] {
			if (scenario.Villages == 0) {
				Accepted(
					script::SetProceduralSource(
						fixture.World,
						{procedural::SourceKind::Village, "village-0", {4 * CELL, 4 * CELL}, ""},
						fixture.Diagnostic
					),
					fixture.Diagnostic
				);
				Accepted(
					script::SetProceduralSource(
						fixture.World,
						{procedural::SourceKind::Entrance, "gate-0", {8 * CELL, 4 * CELL}, "village-0"},
						fixture.Diagnostic
					),
					fixture.Diagnostic
				);
			} else {
				Accepted(
					script::SetProceduralSource(
						fixture.World,
						{procedural::SourceKind::Village, "village-0", {4 * CELL, 4 * CELL + 64}, ""},
						fixture.Diagnostic
					),
					fixture.Diagnostic
				);
			}
		});
		CheckFar(fixture, farKey, farMesh);
		for (size_t index : active) {
			oracleBytes.WriteUInt32(static_cast<uint32_t>(fixture.Oracles[index].Artifact.size()));
			oracleBytes.WriteRaw(
				fixture.Oracles[index].Artifact.data(), fixture.Oracles[index].Artifact.size()
			);
		}
		Edit(fixture, jobs, context, report, Stage::SplineEdit, [&] {
			Accepted(
				script::SetProceduralSpline(
					fixture.World,
					{scenario.Splines ? "bend-0" : "edited-bend", "gate-0", {6 * CELL, 5 * CELL + 16}},
					fixture.Diagnostic
				),
				fixture.Diagnostic
			);
		});
		CheckFar(fixture, farKey, farMesh);
		for (size_t index : active) {
			oracleBytes.WriteUInt32(static_cast<uint32_t>(fixture.Oracles[index].Artifact.size()));
			oracleBytes.WriteRaw(
				fixture.Oracles[index].Artifact.data(), fixture.Oracles[index].Artifact.size()
			);
		}
		Edit(fixture, jobs, context, report, Stage::OverrideMove, [&] {
			Accepted(
				script::SetProceduralOverride(
					fixture.World,
					{"tree/1/1",
					 procedural::OverrideAction::Replace,
					 {(20 * scenario.Side + 2) * CELL, 2 * CELL},
					 1,
					 0,
					 0,
					 ""},
					fixture.Diagnostic
				),
				fixture.Diagnostic
			);
		});
		CheckFar(fixture, farKey, farMesh);
		for (size_t index : active) {
			oracleBytes.WriteUInt32(static_cast<uint32_t>(fixture.Oracles[index].Artifact.size()));
			oracleBytes.WriteRaw(
				fixture.Oracles[index].Artifact.data(), fixture.Oracles[index].Artifact.size()
			);
		}
		Edit(fixture, jobs, context, report, Stage::OverrideDelete, [&] {
			Accepted(
				script::SetProceduralOverride(fixture.World, {"tree/1/1"}, fixture.Diagnostic),
				fixture.Diagnostic
			);
		});
		CheckFar(fixture, farKey, farMesh);
		for (size_t index : active) {
			oracleBytes.WriteUInt32(static_cast<uint32_t>(fixture.Oracles[index].Artifact.size()));
			oracleBytes.WriteRaw(
				fixture.Oracles[index].Artifact.data(), fixture.Oracles[index].Artifact.size()
			);
		}
		Refusals(fixture, jobs, context, report);
		CheckFar(fixture, farKey, farMesh);
		for (size_t index : active) {
			oracleBytes.WriteUInt32(static_cast<uint32_t>(fixture.Oracles[index].Artifact.size()));
			oracleBytes.WriteRaw(
				fixture.Oracles[index].Artifact.data(), fixture.Oracles[index].Artifact.size()
			);
		}
		OrdinaryAssets(fixture, report, scratch, oracleBytes);
		const auto hash = assets::Hasher::Of(oracleBytes.Bytes()).ToHex();
		if (oracleHash.empty()) oracleHash = hash;
		Require(oracleHash == hash, "procedural B0 canonical pipeline oracle changed between trials");
	}

	uint64_t Quantile(const std::vector<uint64_t> &sorted, size_t percent) {
		return sorted[(sorted.size() * percent + 99) / 100 - 1];
	}

	void Run(size_t scenarioIndex, parallel::JobContext context) {
		Require(
			std::getenv("MONO_PROCEDURAL_B0") != nullptr,
			"set MONO_PROCEDURAL_B0=1 to run the full procedural baseline"
		);
		const auto &scenario = SCENARIOS[scenarioIndex];
		const std::string_view mode =
			context == parallel::JobContext::Serial ? "authoritative-serial" : "threaded-preview";
		const auto trials = TrialCount();
		Scratch scratch(scenario.Name, mode);
		Report report(trials);
		std::string oracleHash;
		std::array<FixtureSummary, 2> fixtureSummaries{};
		const auto beforeTrials = core::HeapProfile::Totals();
		for (size_t trial = 0; trial < trials; ++trial) {
			// No worker from a preceding trial survives this point.
			core::HeapProfile::ResetTotals();
			const auto before = core::HeapProfile::Totals();
			RunTrial(scenario, context, report, scratch, oracleHash, fixtureSummaries);
			const auto after = core::HeapProfile::Totals();
			report.PeakProcessBytes = std::max(report.PeakProcessBytes, after.PeakBytes);
			report.MaximumPostTrialLiveDelta =
				std::max(report.MaximumPostTrialLiveDelta, after.LiveBytes - before.LiveBytes);
		}
		const std::array<const char *, 4> oracleEnvs{
			"MONO_PROCEDURAL_B0_EMPTY_ORACLE",
			"MONO_PROCEDURAL_B0_SIMPLE_ORACLE",
			"MONO_PROCEDURAL_B0_BUSY_ORACLE",
			"MONO_PROCEDURAL_B0_COMPLEX_ORACLE"
		};
		const char *expected = std::getenv(oracleEnvs[scenarioIndex]);
		Require(!expected || oracleHash == expected, "procedural B0 pinned artifact oracle digest mismatch");
		const auto afterTrials = core::HeapProfile::Totals();
		std::ostringstream summary;
		for (size_t index = 0; index < fixtureSummaries.size(); ++index) {
			const auto &fixture = fixtureSummaries[index];
			summary << "procedural-b0-fixture scenario=" << scenario.Name << " mode=" << mode
					<< " region=" << index << " candidate_cells=" << fixture.CandidateCells
					<< " work_units=" << fixture.WorkUnits << " spatial_tests=" << fixture.SpatialTests
					<< " placements=" << fixture.Placements << " roads=" << fixture.Roads
					<< " geometry_objects=" << fixture.Objects << " vertices=" << fixture.Vertices
					<< " indices=" << fixture.Indices << " key_bytes=" << fixture.KeyBytes
					<< " artifact_bytes=" << fixture.ArtifactBytes
					<< " unique_mesh_assets=" << fixture.UniqueMeshes << '\n';
		}
		summary
			<< "procedural-b0 scenario=" << scenario.Name << " mode=" << mode << " trials=" << trials
			<< " side=" << scenario.Side << " villages=" << scenario.Villages << " roads=" << scenario.Roads
			<< " splines=" << scenario.Splines << " overrides=" << scenario.Overrides
			<< " canonical_pipeline_oracle=" << oracleHash << " oracle_pinned=" << (expected != nullptr)
			<< " heap_hooks=" << core::HeapProfile::IsCompiledIn() << " generator_result_cache=absent"
			<< " poll_strategy=owner-yield filesystem_cache=unspecified peak_scope=whole-trial-with-oracles"
			<< " candidate_cells=" << report.CandidateCells << " work_units=" << report.WorkUnits
			<< " spatial_tests=" << report.SpatialTests
			<< " generated_placements=" << report.GeneratedPlacements
			<< " generated_roads=" << report.GeneratedRoads << " artifact_bytes=" << report.ArtifactBytes
			<< " peak_retained_requests=" << report.PeakRetainedRequests
			<< " peak_reserved_bytes=" << report.PeakReservedBytes
			<< " process_heap_counters=" << (core::HeapProfile::IsCompiledIn() ? "measured" : "unavailable");
		if (core::HeapProfile::IsCompiledIn())
			summary << " process_live_bytes=" << afterTrials.LiveBytes
					<< " process_live_blocks=" << afterTrials.LiveBlocks
					<< " process_peak_bytes=" << report.PeakProcessBytes
					<< " profiler_overhead_bytes=" << afterTrials.OverheadBytes
					<< " post_trials_live_delta=" << afterTrials.LiveBytes - beforeTrials.LiveBytes
					<< " max_post_trial_live_delta=" << report.MaximumPostTrialLiveDelta;
		summary << '\n';
		for (size_t index = 0; index < report.Stages.size(); ++index) {
			auto &row = report.Stages[index];
			if (row.Nanoseconds.empty()) continue;
			std::sort(row.Nanoseconds.begin(), row.Nanoseconds.end());
			summary << "procedural-b0-stage scenario=" << scenario.Name << " mode=" << mode
					<< " stage=" << STAGE_NAMES[index] << " n=" << row.Nanoseconds.size()
					<< " tail_insufficient=" << (row.Nanoseconds.size() < 100)
					<< " p50_ns=" << Quantile(row.Nanoseconds, 50)
					<< " p95_ns=" << Quantile(row.Nanoseconds, 95)
					<< " p99_ns=" << Quantile(row.Nanoseconds, 99) << " max_ns=" << row.Nanoseconds.back()
					<< " heap_counters="
					<< (row.HasHeapCounters ? "measured"
											: (row.MeasuredPhase ? "unavailable" : "see-phase-rows"));
			if (row.HasHeapCounters)
				summary << " allocated_bytes=" << row.AllocatedBytes
						<< " allocated_blocks=" << row.AllocatedBlocks
						<< " observed_process_peak_bytes=" << row.ObservedProcessPeak
						<< " max_live_delta_bytes=" << row.MaximumLiveDelta;
			if (row.MeasuredPhase)
				summary << " work_units=" << row.WorkUnits << " candidate_cells=" << row.CandidateCells
						<< " spatial_tests=" << row.SpatialTests;
			summary << '\n';
		}
		auto text = summary.str();
		Require(text.size() <= 65536, "procedural B0 summary exceeded its bounded storage");
		const size_t slot = scenarioIndex * 2 + (context == parallel::JobContext::Threaded ? 1 : 0);
		auto &latest = Summaries().Latest[slot];
		latest.Text = std::move(text);
		++latest.CompletedCohorts;
	}
}

BENCH("procedural B0 empty | authoritative serial", 1) {
	procedural_pipeline_bench::Run(0, engine::parallel::JobContext::Serial);
}
BENCH("procedural B0 simple | authoritative serial", 1) {
	procedural_pipeline_bench::Run(1, engine::parallel::JobContext::Serial);
}
BENCH("procedural B0 busy | authoritative serial", 1) {
	procedural_pipeline_bench::Run(2, engine::parallel::JobContext::Serial);
}
BENCH("procedural B0 complex | authoritative serial", 1) {
	procedural_pipeline_bench::Run(3, engine::parallel::JobContext::Serial);
}
BENCH("procedural B0 empty | threaded preview", 1) {
	procedural_pipeline_bench::Run(0, engine::parallel::JobContext::Threaded);
}
BENCH("procedural B0 simple | threaded preview", 1) {
	procedural_pipeline_bench::Run(1, engine::parallel::JobContext::Threaded);
}
BENCH("procedural B0 busy | threaded preview", 1) {
	procedural_pipeline_bench::Run(2, engine::parallel::JobContext::Threaded);
}
BENCH("procedural B0 complex | threaded preview", 1) {
	procedural_pipeline_bench::Run(3, engine::parallel::JobContext::Threaded);
}
