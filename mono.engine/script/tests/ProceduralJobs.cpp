#include "../src/ProceduralState.hpp"

#include <engine/core/Bytes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/procedural/Artifact.hpp>
#include <engine/script/ProceduralJobs.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <thread>

TEST_SUITE_ID("engine.script.proceduraljobs")
TEST_DEPENDS("engine.procedural.region")
TEST_DEPENDS("engine.procedural.artifact")

using namespace engine;

TEST_CASE(
	"procedural request and incarnation sequences remain exhausted after the last nonzero identity",
	"[procedural][host]"
) {
	std::atomic<uint64_t> next{std::numeric_limits<uint64_t>::max()};
	CHECK(script::procedural_detail::TakeSequence(next) == std::numeric_limits<uint64_t>::max());
	CHECK(script::procedural_detail::TakeSequence(next) == 0);
	CHECK(script::procedural_detail::TakeSequence(next) == 0);
	CHECK(next.load() == 0);
}

namespace {
	constexpr int64_t CELL = procedural::SUBCELLS_PER_CELL;
	const std::array<procedural::RegionScope, 3> SCOPES{{{0, 0, 16}, {20, 0, 16}, {40, 0, 16}}};

	struct ProceduralFixture {
		ecs::Store Store{"procedural-host"};
		procedural::Diagnostic Diagnostic{};
		ProceduralFixture() {
			script::RegisterProceduralComponents();
			const std::array<script::ProceduralSource, 4> sources{
				{{procedural::SourceKind::Village, "oakstead", {8 * CELL, 8 * CELL}, ""},
				 {procedural::SourceKind::Entrance, "west-gate", {12 * CELL, 8 * CELL}, "oakstead"},
				 {procedural::SourceKind::Village, "far-village", {328 * CELL, 8 * CELL}, ""},
				 {procedural::SourceKind::Entrance, "far-gate", {332 * CELL, 8 * CELL}, "far-village"}}
			};
			REQUIRE(
				script::ConfigureProceduralWorld(
					Store,
					"demo",
					procedural::ForestVillageDefaults("forest-village", 42),
					SCOPES,
					sources,
					Diagnostic
				)
			);
		}
	};

	std::vector<uint8_t> RegionKey(const ecs::Store &store, procedural::RegionScope scope) {
		procedural::CompiledRecipe compiled;
		procedural::RegionRequest request;
		procedural::Diagnostic diagnostic;
		REQUIRE(script::CaptureProceduralRegion(store, scope, compiled, request, diagnostic));
		std::vector<uint8_t> key;
		REQUIRE(procedural::BuildRegionKey(compiled, request, key, diagnostic) == procedural::Status::Ok);
		return key;
	}

	void AwaitProcedural(script::ProceduralJobs &jobs, size_t count) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
		while (std::chrono::steady_clock::now() < deadline) {
			jobs.Poll();
			if (jobs.Completions().size() == count) return;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		FAIL("procedural worker did not complete before the deadline");
	}

	const script::ProceduralPublication &Published(const ecs::Store &store, procedural::RegionScope scope) {
		const auto *published = store.Resource<script::ProceduralPublished>();
		REQUIRE(published != nullptr);
		const auto found =
			std::find_if(published->Regions.begin(), published->Regions.end(), [&](const auto &row) {
				return row.Scope.X == scope.X && row.Scope.Y == scope.Y &&
					   row.Scope.CellsPerSide == scope.CellsPerSide;
			});
		REQUIRE(found != published->Regions.end());
		return *found;
	}
}

TEST_CASE(
	"unconfigured procedural authority survives snapshots but is not an authored document",
	"[procedural][host]"
) {
	script::RegisterProceduralComponents();
	ecs::Store source("unconfigured-authority");
	source.SetResource(script::ProceduralAuthority{});
	const auto &type = ecs::Components::Describe(ecs::Components::Assigned<script::ProceduralAuthority>());
	auto partial = script::ProceduralAuthority{};
	partial.Owner = "demo";
	core::ByteWriter malformed;
	CHECK_THROWS_AS(type.Write(malformed, &partial, 1), std::length_error);
	procedural::Diagnostic diagnostic;
	std::vector<uint8_t> portable;
	CHECK_FALSE(script::EncodeProceduralAuthority(source, portable, diagnostic));

	core::ByteWriter snapshot;
	REQUIRE(source.Save(snapshot));
	core::ByteReader reader(snapshot.Bytes());
	ecs::Store restored("restored-unconfigured-authority");
	REQUIRE(restored.Load(reader));
	const auto *authority = restored.Resource<script::ProceduralAuthority>();
	REQUIRE(authority);
	CHECK(authority->Owner.empty());
	CHECK(authority->RecipeBytes.empty());
	CHECK(authority->Sources.empty());
	CHECK(authority->Splines.empty());
	CHECK(authority->Overrides.empty());
	CHECK(authority->Regions.empty());
	CHECK(authority->Incarnation == 0);
	CHECK_FALSE(script::EncodeProceduralAuthority(restored, portable, diagnostic));
}

TEST_CASE(
	"registered authority codec preserves authored bytes and rejects malformed payloads", "[procedural][host]"
) {
	ProceduralFixture fixture;
	const auto *authority = fixture.Store.Resource<script::ProceduralAuthority>();
	REQUIRE(authority);
	const auto &type = ecs::Components::Describe(ecs::Components::Assigned<script::ProceduralAuthority>());
	std::vector<uint8_t> portable;
	REQUIRE(script::EncodeProceduralAuthority(fixture.Store, portable, fixture.Diagnostic));
	core::ByteWriter expected;
	expected.WriteUInt32(static_cast<uint32_t>(portable.size()));
	expected.WriteRaw(portable.data(), portable.size());
	core::ByteWriter registered;
	type.Write(registered, authority, 1);
	CHECK(
		std::equal(
			registered.Bytes().begin(),
			registered.Bytes().end(),
			expected.Bytes().begin(),
			expected.Bytes().end()
		)
	);

	core::ByteWriter malformed;
	malformed.WriteUInt32(1);
	malformed.WriteUInt8(0);
	core::ByteReader reader(malformed.Bytes());
	auto destination = *authority;
	type.Read(reader, &destination, 1);
	CHECK(reader.Failed());
	CHECK(destination.Owner == authority->Owner);
	CHECK(destination.RecipeBytes == authority->RecipeBytes);
	CHECK(destination.Incarnation == authority->Incarnation);

	core::ByteWriter empty;
	core::ByteReader missingLength(empty.Bytes());
	destination = *authority;
	type.Read(missingLength, &destination, 1);
	CHECK(missingLength.Failed());
	CHECK(destination.Owner == authority->Owner);
	CHECK(destination.Incarnation == authority->Incarnation);
}

TEST_CASE(
	"procedural source capture proves local empty coverage and selective moved-source invalidation",
	"[procedural][host]"
) {
	ProceduralFixture fixture;
	const auto firstKey = RegionKey(fixture.Store, SCOPES[0]);
	const auto farKey = RegionKey(fixture.Store, SCOPES[1]);
	const auto emptyKey = RegionKey(fixture.Store, SCOPES[2]);
	procedural::CompiledRecipe compiled;
	procedural::RegionRequest empty;
	REQUIRE(script::CaptureProceduralRegion(fixture.Store, SCOPES[2], compiled, empty, fixture.Diagnostic));
	REQUIRE(empty.Queries.size() == 2);
	CHECK(empty.Queries[0].Complete);
	CHECK(empty.Queries[0].Matches.empty());
	CHECK(empty.Queries[0].CoverageDigest != procedural::Digest{});
	REQUIRE(
		script::SetProceduralSource(
			fixture.Store,
			{procedural::SourceKind::Village, "oakstead", {648 * CELL, 8 * CELL}, ""},
			fixture.Diagnostic
		)
	);
	CHECK(RegionKey(fixture.Store, SCOPES[0]) != firstKey);
	CHECK(RegionKey(fixture.Store, SCOPES[1]) == farKey);
	CHECK(RegionKey(fixture.Store, SCOPES[2]) != emptyKey);
	const auto *authority = fixture.Store.Resource<script::ProceduralAuthority>();
	REQUIRE(authority != nullptr);
	CHECK(authority->Regions[0].SourceRevision == 2);
	CHECK(authority->Regions[1].SourceRevision == 1);
	CHECK(authority->Regions[2].SourceRevision == 2);
}

TEST_CASE("copied procedural inputs produce identical serial and threaded artifacts", "[procedural][host]") {
	ProceduralFixture fixture;
	script::ProceduralJobs serial(fixture.Store);
	script::ProceduralJobs threaded(fixture.Store);
	REQUIRE(serial.Submit(fixture.Store, SCOPES[0], parallel::JobContext::Serial) != 0);
	const uint64_t ticket = threaded.Submit(fixture.Store, SCOPES[0], parallel::JobContext::Threaded);
	REQUIRE(ticket != 0);
	REQUIRE(
		script::SetProceduralSource(
			fixture.Store,
			{procedural::SourceKind::Entrance, "west-gate", {13 * CELL, 8 * CELL}, "oakstead"},
			fixture.Diagnostic
		)
	);
	AwaitProcedural(serial, 1);
	AwaitProcedural(threaded, 1);
	REQUIRE(serial.Completions()[0].Status == script::ProceduralJobStatus::Ready);
	REQUIRE(threaded.Completions()[0].Status == script::ProceduralJobStatus::Ready);
	CHECK(serial.Completions()[0].ArtifactBytes == threaded.Completions()[0].ArtifactBytes);
	CHECK(serial.Completions()[0].ArtifactDigest == threaded.Completions()[0].ArtifactDigest);
	CHECK_FALSE(threaded.PublishBatch(fixture.Store, std::array{ticket}, fixture.Store.Time().Tick));
	CHECK(threaded.LastDiagnostic().Code == procedural::Status::StaleArtifact);
}

TEST_CASE(
	"procedural publication preflights every output before one complete region-table swap",
	"[procedural][host]"
) {
	ProceduralFixture fixture;
	script::ProceduralJobs jobs(fixture.Store);
	std::array<uint64_t, 2> tickets{
		jobs.Submit(fixture.Store, SCOPES[0], parallel::JobContext::Serial),
		jobs.Submit(fixture.Store, SCOPES[1], parallel::JobContext::Serial)
	};
	REQUIRE(tickets[0] != 0);
	REQUIRE(tickets[1] != 0);
	AwaitProcedural(jobs, 2);
	REQUIRE(jobs.PublishBatch(fixture.Store, tickets, fixture.Store.Time().Tick));
	const auto first = Published(fixture.Store, SCOPES[0]);
	const auto far = Published(fixture.Store, SCOPES[1]);
	jobs.ClearCompletions();
	tickets = {
		jobs.Submit(fixture.Store, SCOPES[1], parallel::JobContext::Serial),
		jobs.Submit(fixture.Store, SCOPES[0], parallel::JobContext::Serial)
	};
	AwaitProcedural(jobs, 2);
	REQUIRE(
		script::SetProceduralSource(
			fixture.Store,
			{procedural::SourceKind::Entrance, "west-gate", {14 * CELL, 8 * CELL}, "oakstead"},
			fixture.Diagnostic
		)
	);
	CHECK_FALSE(jobs.PublishBatch(fixture.Store, tickets, fixture.Store.Time().Tick));
	CHECK(Published(fixture.Store, SCOPES[0]).ArtifactBytes == first.ArtifactBytes);
	CHECK(Published(fixture.Store, SCOPES[1]).ArtifactBytes == far.ArtifactBytes);
	CHECK(Published(fixture.Store, SCOPES[1]).AcceptedRevision == far.AcceptedRevision);
	CHECK(jobs.Completions()[0].Status == script::ProceduralJobStatus::Ready);
	jobs.ClearCompletions();
	const uint64_t rebuilt = jobs.Submit(fixture.Store, SCOPES[0], parallel::JobContext::Serial);
	REQUIRE(rebuilt != 0);
	AwaitProcedural(jobs, 1);
	REQUIRE(jobs.PublishBatch(fixture.Store, std::array{rebuilt}, fixture.Store.Time().Tick));
	CHECK(Published(fixture.Store, SCOPES[0]).ArtifactBytes != first.ArtifactBytes);
	CHECK(Published(fixture.Store, SCOPES[1]).ArtifactBytes == far.ArtifactBytes);
	CHECK(Published(fixture.Store, SCOPES[1]).ArtifactDigest == far.ArtifactDigest);
}

TEST_CASE(
	"unconsumed and canceled procedural records retain their queue and memory charge", "[procedural][host]"
) {
	ProceduralFixture fixture;
	script::ProceduralJobs jobs(fixture.Store);
	std::vector<uint64_t> tickets;
	constexpr size_t ADMITTED =
		script::ProceduralHostLimits::MaximumRetainedBytes / script::ProceduralHostLimits::BytesPerRequest;
	for (size_t index = 0; index < ADMITTED; ++index) {
		const auto ticket = jobs.Submit(fixture.Store, SCOPES[0], parallel::JobContext::Serial);
		REQUIRE(ticket != 0);
		tickets.push_back(ticket);
	}
	CHECK(jobs.ReservedBytes() == script::ProceduralHostLimits::MaximumRetainedBytes);
	CHECK(jobs.Submit(fixture.Store, SCOPES[0], parallel::JobContext::Serial) == 0);
	for (const auto ticket : tickets) {
		CHECK(jobs.Cancel(ticket));
		CHECK(jobs.Cancel(ticket));
	}
	AwaitProcedural(jobs, ADMITTED);
	for (const auto &completion : jobs.Completions()) {
		CHECK(completion.Status == script::ProceduralJobStatus::Canceled);
		CHECK(completion.ArtifactBytes.empty());
	}
	CHECK(jobs.Submit(fixture.Store, SCOPES[0], parallel::JobContext::Serial) == 0);
	jobs.ClearCompletions();
	CHECK(jobs.RetainedCount() == 0);
	CHECK(jobs.ReservedBytes() == 0);
	CHECK(jobs.Submit(fixture.Store, SCOPES[0], parallel::JobContext::Serial) != 0);
}

TEST_CASE(
	"procedural authority round trips through portable bytes and snapshots without reviving a queue",
	"[procedural][host]"
) {
	ProceduralFixture fixture;
	script::ProceduralJobs jobs(fixture.Store);
	const uint64_t ticket = jobs.Submit(fixture.Store, SCOPES[0], parallel::JobContext::Serial);
	REQUIRE(ticket != 0);
	AwaitProcedural(jobs, 1);
	REQUIRE(jobs.PublishBatch(fixture.Store, std::array{ticket}, fixture.Store.Time().Tick));
	const auto previous = Published(fixture.Store, SCOPES[0]);
	const auto key = RegionKey(fixture.Store, SCOPES[0]);
	std::vector<uint8_t> portable;
	REQUIRE(script::EncodeProceduralAuthority(fixture.Store, portable, fixture.Diagnostic));
	ecs::Store imported("imported");
	REQUIRE(script::DecodeProceduralAuthority(imported, portable, fixture.Diagnostic));
	CHECK(RegionKey(imported, SCOPES[0]) == key);
	std::vector<uint8_t> encodedAgain;
	REQUIRE(script::EncodeProceduralAuthority(imported, encodedAgain, fixture.Diagnostic));
	CHECK(encodedAgain == portable);
	core::ByteWriter snapshot;
	REQUIRE(fixture.Store.Save(snapshot));
	core::ByteReader restore(snapshot.Bytes());
	REQUIRE(fixture.Store.Load(restore));
	CHECK(RegionKey(fixture.Store, SCOPES[0]) == key);
	CHECK(Published(fixture.Store, SCOPES[0]).ArtifactBytes == previous.ArtifactBytes);
	CHECK_FALSE(jobs.PublishBatch(fixture.Store, std::array{ticket}, fixture.Store.Time().Tick));
	CHECK(jobs.Submit(fixture.Store, SCOPES[0], parallel::JobContext::Serial) == 0);
}

TEST_CASE(
	"procedural malformed authority import and unsupported contexts preserve accepted state",
	"[procedural][host]"
) {
	ProceduralFixture fixture;
	const auto key = RegionKey(fixture.Store, SCOPES[0]);
	std::vector<uint8_t> portable;
	REQUIRE(script::EncodeProceduralAuthority(fixture.Store, portable, fixture.Diagnostic));
	for (size_t length : {size_t{0}, size_t{4}, portable.size() - 1}) {
		CHECK_FALSE(
			script::DecodeProceduralAuthority(
				fixture.Store, std::span(portable).first(length), fixture.Diagnostic
			)
		);
		CHECK(RegionKey(fixture.Store, SCOPES[0]) == key);
	}
	portable.push_back(0);
	CHECK_FALSE(script::DecodeProceduralAuthority(fixture.Store, portable, fixture.Diagnostic));
	CHECK(RegionKey(fixture.Store, SCOPES[0]) == key);
	script::ProceduralJobs jobs(fixture.Store);
	CHECK(jobs.Submit(fixture.Store, SCOPES[0], parallel::JobContext::Processed) == 0);
	CHECK(jobs.LastDiagnostic().Field == "request.context");
	ecs::Store foreign("foreign");
	CHECK(jobs.Submit(foreign, SCOPES[0], parallel::JobContext::Serial) == 0);
	fixture.Store.SetAdoptOnly(true);
	CHECK(jobs.Submit(fixture.Store, SCOPES[0], parallel::JobContext::Serial) == 0);
	CHECK_FALSE(
		script::SetProceduralSource(
			fixture.Store, {procedural::SourceKind::Village, "other", {}, ""}, fixture.Diagnostic
		)
	);
}

TEST_CASE(
	"procedural local source additions stale empty results and failed builds publish no empty success",
	"[procedural][host]"
) {
	ProceduralFixture fixture;
	script::ProceduralJobs jobs(fixture.Store);
	const auto emptyTicket = jobs.Submit(fixture.Store, SCOPES[2], parallel::JobContext::Serial);
	REQUIRE(emptyTicket != 0);
	AwaitProcedural(jobs, 1);
	REQUIRE(
		script::SetProceduralSource(
			fixture.Store,
			{procedural::SourceKind::Village, "new-village", {648 * CELL, 8 * CELL}, ""},
			fixture.Diagnostic
		)
	);
	CHECK_FALSE(jobs.PublishBatch(fixture.Store, std::array{emptyTicket}, fixture.Store.Time().Tick));
	jobs.ClearCompletions();
	REQUIRE(
		script::RemoveProceduralSource(
			fixture.Store, procedural::SourceKind::Village, "oakstead", fixture.Diagnostic
		)
	);
	const auto failed = jobs.Submit(fixture.Store, SCOPES[0], parallel::JobContext::Serial);
	REQUIRE(failed != 0);
	AwaitProcedural(jobs, 1);
	CHECK(jobs.Completions()[0].Status == script::ProceduralJobStatus::Failed);
	CHECK(jobs.Completions()[0].Diagnostic.Code == procedural::Status::MissingSource);
	CHECK(jobs.Completions()[0].ArtifactBytes.empty());
	CHECK_FALSE(jobs.PublishBatch(fixture.Store, std::array{failed}, fixture.Store.Time().Tick));
}

TEST_CASE(
	"procedural threaded cancellation has bounded monotonic progress and joins on owner teardown",
	"[procedural][host]"
) {
	ProceduralFixture fixture;
	script::ProceduralJobs jobs(fixture.Store);
	const auto ticket = jobs.Submit(fixture.Store, SCOPES[0], parallel::JobContext::Threaded);
	REQUIRE(ticket != 0);
	script::ProceduralProgress before;
	REQUIRE(jobs.Progress(ticket, before));
	REQUIRE(jobs.Cancel(ticket));
	AwaitProcedural(jobs, 1);
	script::ProceduralProgress after;
	REQUIRE(jobs.Progress(ticket, after));
	CHECK(after.Status == script::ProceduralJobStatus::Canceled);
	CHECK(after.WorkUnits >= before.WorkUnits);
	CHECK(after.SpatialTests >= before.SpatialTests);
	CHECK(after.WorkUnits <= procedural::GenerationLimits{}.MaximumWorkUnits);
	CHECK_FALSE(jobs.PublishBatch(fixture.Store, std::array{ticket}, fixture.Store.Time().Tick));
	const auto retained = jobs.RetainedCount();
	{
		script::ProceduralJobs temporary(fixture.Store);
		REQUIRE(temporary.Submit(fixture.Store, SCOPES[0], parallel::JobContext::Threaded) != 0);
	}
	CHECK(jobs.RetainedCount() == retained);
}

TEST_CASE(
	"procedural queue follows the store owner handoff and refuses foreign tickets and ticks",
	"[procedural][host]"
) {
	ProceduralFixture fixture;
	script::ProceduralJobs jobs(fixture.Store);
	uint64_t transferredTicket = 0;
	bool published = false;
	std::thread owner([&] {
		fixture.Store.BindToCallingThread();
		transferredTicket = jobs.Submit(fixture.Store, SCOPES[0], parallel::JobContext::Serial);
		jobs.Poll();
		published =
			jobs.PublishBatch(fixture.Store, std::array{transferredTicket}, fixture.Store.Time().Tick);
	});
	owner.join();
	fixture.Store.BindToCallingThread();
	REQUIRE(transferredTicket != 0);
	REQUIRE(published);
	CHECK_FALSE(jobs.Cancel(transferredTicket));
	script::ProceduralJobs other(fixture.Store);
	const auto ticket = other.Submit(fixture.Store, SCOPES[1], parallel::JobContext::Serial);
	REQUIRE(ticket != 0);
	other.Poll();
	CHECK_FALSE(jobs.PublishBatch(fixture.Store, std::array{ticket}, fixture.Store.Time().Tick));
	CHECK_FALSE(other.PublishBatch(fixture.Store, std::array{ticket}, fixture.Store.Time().Tick + 1));
	CHECK(fixture.Store.Resource<script::ProceduralPublished>()->Regions.size() == 1);
	CHECK_FALSE(other.PublishBatch(fixture.Store, std::array{ticket, ticket}, fixture.Store.Time().Tick));
	CHECK(fixture.Store.Resource<script::ProceduralPublished>()->Regions.size() == 1);
}

TEST_CASE(
	"procedural source limits and invalid names refuse before changing authority", "[procedural][host]"
) {
	ProceduralFixture fixture;
	const auto original = RegionKey(fixture.Store, SCOPES[0]);
	auto invalid = script::ProceduralSource{procedural::SourceKind::Village, std::string(129, 'x'), {}, ""};
	CHECK_FALSE(script::SetProceduralSource(fixture.Store, invalid, fixture.Diagnostic));
	CHECK(RegionKey(fixture.Store, SCOPES[0]) == original);
	std::vector<script::ProceduralSource> excessive(script::ProceduralHostLimits::MaximumSources + 1);
	CHECK_FALSE(
		script::ConfigureProceduralWorld(
			fixture.Store,
			"demo",
			procedural::ForestVillageDefaults("forest-village", 42),
			SCOPES,
			excessive,
			fixture.Diagnostic
		)
	);
	CHECK(RegionKey(fixture.Store, SCOPES[0]) == original);
	procedural::CompiledRecipe compiled;
	procedural::RegionRequest request;
	CHECK_FALSE(
		script::CaptureProceduralRegion(fixture.Store, {500, 500, 16}, compiled, request, fixture.Diagnostic)
	);
}

TEST_CASE(
	"explicit preview synchronization retains exact local inputs and keeps load fences",
	"[procedural][host][preview]"
) {
	ProceduralFixture source;
	ProceduralFixture preview;
	script::ProceduralJobs jobs(preview.Store);
	const std::array<uint64_t, 2> tickets{
		jobs.Submit(preview.Store, SCOPES[0], parallel::JobContext::Serial),
		jobs.Submit(preview.Store, SCOPES[1], parallel::JobContext::Serial)
	};
	REQUIRE(tickets[0]);
	REQUIRE(tickets[1]);
	jobs.Poll();
	const auto before = *preview.Store.Resource<script::ProceduralAuthority>();
	std::vector<uint8_t> original;
	REQUIRE(script::EncodeProceduralAuthority(preview.Store, original, preview.Diagnostic));
	REQUIRE(
		script::SetProceduralSource(
			source.Store,
			{procedural::SourceKind::Village, "oakstead", {9 * CELL, 8 * CELL}, ""},
			source.Diagnostic
		)
	);
	std::vector<uint8_t> changed;
	REQUIRE(script::EncodeProceduralAuthority(source.Store, changed, source.Diagnostic));
	auto malformed = changed;
	malformed.front() ^= 1;
	CHECK_FALSE(jobs.SynchronizePreviewInputs(preview.Store, malformed, preview.Diagnostic));
	std::vector<uint8_t> afterRefusal;
	REQUIRE(script::EncodeProceduralAuthority(preview.Store, afterRefusal, preview.Diagnostic));
	CHECK(afterRefusal == original);
	REQUIRE(jobs.Completions().size() == 2);
	CHECK(jobs.Completions()[0].Status == script::ProceduralJobStatus::Ready);
	CHECK(jobs.Completions()[1].Status == script::ProceduralJobStatus::Ready);
	REQUIRE(jobs.SynchronizePreviewInputs(preview.Store, changed, preview.Diagnostic));
	const auto *updated = preview.Store.Resource<script::ProceduralAuthority>();
	REQUIRE(updated);
	CHECK(updated->Incarnation == before.Incarnation);
	CHECK(updated->Regions[0].SourceRevision == before.Regions[0].SourceRevision + 1);
	CHECK(updated->Regions[1].SourceRevision == before.Regions[1].SourceRevision);
	CHECK(jobs.Completions()[0].Status == script::ProceduralJobStatus::Canceled);
	CHECK(jobs.Completions()[1].Status == script::ProceduralJobStatus::Ready);
	CHECK_FALSE(jobs.PublishBatch(preview.Store, std::span(&tickets[0], 1), preview.Store.Time().Tick));
	REQUIRE(jobs.PublishBatch(preview.Store, std::span(&tickets[1], 1), preview.Store.Time().Tick));
	const auto far = Published(preview.Store, SCOPES[1]);
	jobs.ClearCompletions();
	const auto fresh = jobs.Submit(preview.Store, SCOPES[1], parallel::JobContext::Serial);
	REQUIRE(fresh);
	jobs.Poll();
	REQUIRE(script::DecodeProceduralAuthority(preview.Store, changed, preview.Diagnostic));
	CHECK(preview.Store.Resource<script::ProceduralAuthority>()->Incarnation != before.Incarnation);
	CHECK_FALSE(jobs.PublishBatch(preview.Store, std::span(&fresh, 1), preview.Store.Time().Tick));
	CHECK(Published(preview.Store, SCOPES[1]).ArtifactBytes == far.ArtifactBytes);
}

TEST_CASE(
	"preview synchronization refuses foreign ownership and exhausted revisions before canceling work",
	"[procedural][host][preview]"
) {
	ProceduralFixture source;
	ProceduralFixture preview;
	script::ProceduralJobs jobs(preview.Store);
	std::vector<uint8_t> bytes;
	REQUIRE(script::EncodeProceduralAuthority(source.Store, bytes, source.Diagnostic));
	CHECK_FALSE(jobs.SynchronizePreviewInputs(source.Store, bytes, preview.Diagnostic));
	preview.Store.SetAdoptOnly(true);
	CHECK_FALSE(jobs.SynchronizePreviewInputs(preview.Store, bytes, preview.Diagnostic));
	preview.Store.SetAdoptOnly(false);
	preview.Store.ResourceMutable<script::ProceduralAuthority>()->Regions[0].SourceRevision =
		std::numeric_limits<uint64_t>::max();
	const auto ticket = jobs.Submit(preview.Store, SCOPES[0], parallel::JobContext::Serial);
	REQUIRE(ticket);
	jobs.Poll();
	REQUIRE(
		script::SetProceduralSource(
			source.Store,
			{procedural::SourceKind::Village, "oakstead", {9 * CELL, 8 * CELL}, ""},
			source.Diagnostic
		)
	);
	REQUIRE(script::EncodeProceduralAuthority(source.Store, bytes, source.Diagnostic));
	CHECK_FALSE(jobs.SynchronizePreviewInputs(preview.Store, bytes, preview.Diagnostic));
	CHECK(preview.Diagnostic.Field == "preview.revision");
	CHECK(
		preview.Store.Resource<script::ProceduralAuthority>()->Regions[0].SourceRevision ==
		std::numeric_limits<uint64_t>::max()
	);
	CHECK(jobs.Completions().front().Status == script::ProceduralJobStatus::Ready);
	REQUIRE(jobs.PublishBatch(preview.Store, std::span(&ticket, 1), preview.Store.Time().Tick));
}
