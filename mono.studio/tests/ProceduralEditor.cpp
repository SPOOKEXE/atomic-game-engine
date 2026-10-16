#include <engine/ecs/Store.hpp>
#include <engine/procedural/Artifact.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/script/ProceduralScene.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <string_view>
#include <studio/ProceduralEditor.hpp>
#include <thread>
#include <vector>

TEST_SUITE_ID("studio.proceduraleditor")
TEST_DEPENDS("engine.script.proceduralscene")
TEST_DEPENDS("engine.script.proceduraljobs")

using namespace engine;
namespace {
	constexpr int64_t CELL = procedural::SUBCELLS_PER_CELL;
	constexpr std::array<procedural::RegionScope, 2> SCOPES{{{0, 0, 16}, {20, 0, 16}}};
	struct Fixture {
		ecs::Store Source{"procedural-authoring"};
		procedural::Diagnostic Diagnostic{};
		Fixture() {
			scene::RegisterSceneClasses();
			script::RegisterProceduralComponents();
			Configure(SCOPES);
		}
		void Configure(std::span<const procedural::RegionScope> scopes, std::string_view owner = "demo") {
			const std::array<script::ProceduralSource, 2> sources{
				{{procedural::SourceKind::Village, "oakstead", {8 * CELL, 8 * CELL}, ""},
				 {procedural::SourceKind::Entrance, "west-gate", {12 * CELL, 8 * CELL}, "oakstead"}}
			};
			REQUIRE(
				script::ConfigureProceduralWorld(
					Source,
					owner,
					procedural::ForestVillageDefaults("forest-village", 42),
					scopes,
					sources,
					Diagnostic
				)
			);
		}
		void MoveVillage() {
			REQUIRE(
				script::SetProceduralSource(
					Source,
					{procedural::SourceKind::Village, "oakstead", {9 * CELL, 8 * CELL}, ""},
					Diagnostic
				)
			);
		}
	};
	const script::ProceduralPublication &Accepted(const studio::ProceduralEditor &editor, size_t index) {
		const auto *published = editor.PreviewStore().Resource<script::ProceduralPublished>();
		REQUIRE(published != nullptr);
		REQUIRE(index < published->Regions.size());
		return published->Regions[index];
	}
	script::ProceduralProgress RequestProgress(const studio::ProceduralEditor &editor, uint64_t ticket) {
		const auto rows = editor.Progress();
		const auto found =
			std::find_if(rows.begin(), rows.end(), [&](const auto &row) { return row.Ticket == ticket; });
		REQUIRE(found != rows.end());
		return *found;
	}
	void
	CheckMonotonic(const script::ProceduralProgress &current, const script::ProceduralProgress &previous) {
		CHECK(current.Ticket == previous.Ticket);
		CHECK(current.CandidateCells >= previous.CandidateCells);
		CHECK(current.WorkUnits >= previous.WorkUnits);
		CHECK(current.SpatialTests >= previous.SpatialTests);
		CHECK(current.Placements >= previous.Placements);
		CHECK(current.Roads >= previous.Roads);
	}
	std::vector<uint8_t> ExpectedArtifact(
		const ecs::Store &source, procedural::RegionScope scope, procedural::Diagnostic &diagnostic
	) {
		procedural::CompiledRecipe recipe;
		procedural::RegionRequest request;
		procedural::RegionResult result;
		std::vector<uint8_t> bytes;
		REQUIRE(script::CaptureProceduralRegion(source, scope, recipe, request, diagnostic));
		REQUIRE(procedural::GenerateRegion(recipe, request, result, diagnostic) == procedural::Status::Ok);
		REQUIRE(procedural::EncodeRegionResult(result, bytes, diagnostic) == procedural::Status::Ok);
		return bytes;
	}
	size_t ReadyCount(const studio::ProceduralEditor &editor) {
		return static_cast<size_t>(
			std::count_if(editor.Progress().begin(), editor.Progress().end(), [](const auto &row) {
				return row.Status == script::ProceduralJobStatus::Ready;
			})
		);
	}
	void WaitReady(studio::ProceduralEditor &editor, size_t count, procedural::Diagnostic &diagnostic) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
		do {
			REQUIRE(editor.Pump(diagnostic));
			if (ReadyCount(editor) == count) return;
			std::this_thread::yield();
		} while (std::chrono::steady_clock::now() < deadline);
		FAIL("native procedural preview did not become ready before the deadline");
	}
	void Drain(studio::ProceduralEditor &editor, procedural::Diagnostic &diagnostic) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
		do {
			REQUIRE(editor.Pump(diagnostic));
			if (editor.ReservedBytes() == 0) return;
			std::this_thread::yield();
		} while (std::chrono::steady_clock::now() < deadline);
		FAIL("canceled procedural preview did not release its reservations before the deadline");
	}
	void Generate(studio::ProceduralEditor &editor, Fixture &fixture) {
		REQUIRE(editor.SyncSource(fixture.Source, fixture.Diagnostic));
		editor.Start();
		WaitReady(editor, 2, fixture.Diagnostic);
		REQUIRE(editor.Activate(fixture.Source, editor.PreviewStore().Time().Tick, fixture.Diagnostic));
	}
}

TEST_CASE(
	"native preview publishes only at explicit activation and leaves authoring untouched",
	"[studio][procedural]"
) {
	Fixture fixture;
	studio::ProceduralEditor editor;
	std::vector<uint8_t> before;
	REQUIRE(script::EncodeProceduralAuthority(fixture.Source, before, fixture.Diagnostic));
	REQUIRE(editor.SyncSource(fixture.Source, fixture.Diagnostic));
	CHECK(editor.Targets().size() == 2);
	for (const auto &target : editor.Targets()) {
		const auto *collider = editor.PreviewStore().Get<scene::Collider>(target.Part);
		REQUIRE(collider != nullptr);
		CHECK(collider->Trigger);
		CHECK(collider->Mask == spatial::LayerMask::None());
		CHECK_FALSE(collider->CanQuery);
		CHECK_FALSE(editor.PreviewStore().Has<scene::Simulated>(target.Part));
	}
	editor.Start();
	WaitReady(editor, 2, fixture.Diagnostic);
	CHECK(editor.PreviewStore().Resource<script::ProceduralPublished>()->Regions.empty());
	CHECK(editor.ReservedBytes() == script::ProceduralHostLimits::MaximumRetainedBytes);
	REQUIRE(editor.Activate(fixture.Source, editor.PreviewStore().Time().Tick, fixture.Diagnostic));
	CHECK(editor.ReservedBytes() == 0);
	CHECK(editor.Progress().empty());
	script::ProceduralPublication captured;
	REQUIRE(editor.CaptureAccepted(fixture.Source, SCOPES[0], captured, fixture.Diagnostic));
	CHECK_FALSE(captured.ArtifactBytes.empty());
	procedural::Digest expected;
	REQUIRE(studio::CurrentProceduralKey(fixture.Source, SCOPES[0], expected, fixture.Diagnostic));
	CHECK(captured.GenerationKey == expected);
	CHECK(fixture.Source.Resource<script::ProceduralPublished>()->Regions.empty());
	std::vector<uint8_t> after;
	REQUIRE(script::EncodeProceduralAuthority(fixture.Source, after, fixture.Diagnostic));
	CHECK(before == after);
}

TEST_CASE(
	"local source edits preserve unrelated accepted preview meshes and refuse stale capture",
	"[studio][procedural]"
) {
	Fixture fixture;
	studio::ProceduralEditor editor;
	Generate(editor, fixture);
	const auto nearDigest = Accepted(editor, 0).ArtifactDigest;
	const auto farPublication = Accepted(editor, 1);
	const auto farMesh = editor.Targets()[1].Mesh;
	const auto farRevision = editor.PreviewStore().Get<scene::EditableMesh>(farMesh)->Revision;
	const auto firstIdentity = editor.PreviewStore().Identity();
	fixture.MoveVillage();
	script::ProceduralPublication refused = farPublication;
	CHECK_FALSE(editor.CaptureAccepted(fixture.Source, SCOPES[0], refused, fixture.Diagnostic));
	CHECK(fixture.Diagnostic.Code == procedural::Status::StaleArtifact);
	CHECK(refused.ArtifactDigest == farPublication.ArtifactDigest);
	REQUIRE(editor.CaptureAccepted(fixture.Source, SCOPES[1], refused, fixture.Diagnostic));
	REQUIRE(editor.SyncSource(fixture.Source, fixture.Diagnostic));
	CHECK(editor.PreviewStore().Identity() == firstIdentity);
	CHECK(Accepted(editor, 0).ArtifactDigest == nearDigest);
	CHECK(Accepted(editor, 1).ArtifactBytes == farPublication.ArtifactBytes);
	WaitReady(editor, 1, fixture.Diagnostic);
	REQUIRE(editor.Activate(fixture.Source, editor.PreviewStore().Time().Tick, fixture.Diagnostic));
	CHECK(Accepted(editor, 0).ArtifactDigest != nearDigest);
	CHECK(Accepted(editor, 1).ArtifactDigest == farPublication.ArtifactDigest);
	CHECK(Accepted(editor, 1).ActivationTick == farPublication.ActivationTick);
	CHECK(editor.PreviewStore().Get<scene::EditableMesh>(farMesh)->Revision == farRevision);
	REQUIRE(editor.CaptureAccepted(fixture.Source, SCOPES[0], refused, fixture.Diagnostic));
}

TEST_CASE("cancel drains native workers and requires Start before resubmission", "[studio][procedural]") {
	Fixture fixture;
	studio::ProceduralEditor editor;
	Generate(editor, fixture);
	const auto before = Accepted(editor, 0);
	fixture.MoveVillage();
	REQUIRE(editor.SyncSource(fixture.Source, fixture.Diagnostic));
	REQUIRE(editor.Pump(fixture.Diagnostic));
	REQUIRE(editor.ReservedBytes() != 0);
	editor.Cancel();
	Drain(editor, fixture.Diagnostic);
	CHECK_FALSE(editor.Running());
	REQUIRE(editor.SyncSource(fixture.Source, fixture.Diagnostic));
	REQUIRE(editor.Pump(fixture.Diagnostic));
	CHECK(editor.ReservedBytes() == 0);
	CHECK(Accepted(editor, 0).ArtifactBytes == before.ArtifactBytes);
	CHECK_FALSE(editor.Activate(fixture.Source, editor.PreviewStore().Time().Tick, fixture.Diagnostic));
	editor.Start();
	WaitReady(editor, 1, fixture.Diagnostic);
	REQUIRE(editor.Activate(fixture.Source, editor.PreviewStore().Time().Tick, fixture.Diagnostic));
	CHECK(Accepted(editor, 0).GenerationKey != before.GenerationKey);
}

TEST_CASE("local synchronization preserves ready and unobserved far tickets", "[studio][procedural]") {
	Fixture fixture;
	const auto farArtifact = ExpectedArtifact(fixture.Source, SCOPES[1], fixture.Diagnostic);
	studio::ProceduralEditor editor;
	REQUIRE(editor.SyncSource(fixture.Source, fixture.Diagnostic));
	editor.Start();
	REQUIRE(editor.Pump(fixture.Diagnostic));
	REQUIRE(editor.Progress().size() == 2);
	const auto nearTicket = editor.Progress()[0].Ticket;
	const auto farTicket = editor.Progress()[1].Ticket;
	SECTION("admitted work has not been polled even if its worker has finished") {
		CHECK(RequestProgress(editor, nearTicket).Status == script::ProceduralJobStatus::Pending);
		CHECK(RequestProgress(editor, farTicket).Status == script::ProceduralJobStatus::Pending);
	}
	SECTION("ready work retains its already completed artifact") {
		WaitReady(editor, 2, fixture.Diagnostic);
		CHECK(RequestProgress(editor, farTicket).Status == script::ProceduralJobStatus::Ready);
	}
	const auto farBefore = RequestProgress(editor, farTicket);
	fixture.MoveVillage();
	REQUIRE(editor.SyncSource(fixture.Source, fixture.Diagnostic));
	CHECK(RequestProgress(editor, nearTicket).Status == script::ProceduralJobStatus::Canceled);
	const auto farAfter = RequestProgress(editor, farTicket);
	CHECK(farAfter.Status == farBefore.Status);
	CheckMonotonic(farAfter, farBefore);
	CHECK(editor.ReservedBytes() == script::ProceduralHostLimits::MaximumRetainedBytes);
	WaitReady(editor, 2, fixture.Diagnostic);
	CHECK(RequestProgress(editor, farTicket).Status == script::ProceduralJobStatus::Ready);
	CheckMonotonic(RequestProgress(editor, farTicket), farAfter);
	CHECK(std::none_of(editor.Progress().begin(), editor.Progress().end(), [&](const auto &row) {
		return row.Ticket == nearTicket;
	}));
	CHECK(editor.ReservedBytes() == script::ProceduralHostLimits::MaximumRetainedBytes);
	REQUIRE(editor.Activate(fixture.Source, editor.PreviewStore().Time().Tick, fixture.Diagnostic));
	CHECK(editor.ReservedBytes() == 0);
	CHECK(Accepted(editor, 1).ArtifactBytes == farArtifact);
	script::ProceduralPublication near;
	REQUIRE(editor.CaptureAccepted(fixture.Source, SCOPES[0], near, fixture.Diagnostic));
}

TEST_CASE("activation rereads authoritative inputs before publishing ready work", "[studio][procedural]") {
	Fixture fixture;
	studio::ProceduralEditor editor;
	REQUIRE(editor.SyncSource(fixture.Source, fixture.Diagnostic));
	editor.Start();
	WaitReady(editor, 2, fixture.Diagnostic);
	const auto nearTicket = editor.Progress()[0].Ticket;
	const auto farTicket = editor.Progress()[1].Ticket;
	const auto farArtifact = ExpectedArtifact(fixture.Source, SCOPES[1], fixture.Diagnostic);
	fixture.MoveVillage();
	REQUIRE(editor.Activate(fixture.Source, editor.PreviewStore().Time().Tick, fixture.Diagnostic));
	CHECK(editor.PreviewStore().Resource<script::ProceduralPublished>()->Regions.size() == 1);
	CHECK(Accepted(editor, 0).Scope.X == SCOPES[1].X);
	CHECK(Accepted(editor, 0).ArtifactBytes == farArtifact);
	CHECK(RequestProgress(editor, nearTicket).Status == script::ProceduralJobStatus::Canceled);
	CHECK(std::none_of(editor.Progress().begin(), editor.Progress().end(), [&](const auto &row) {
		return row.Ticket == farTicket;
	}));
	script::ProceduralPublication captured;
	CHECK_FALSE(editor.CaptureAccepted(fixture.Source, SCOPES[0], captured, fixture.Diagnostic));
	WaitReady(editor, 1, fixture.Diagnostic);
	CHECK(editor.Progress().front().Ticket != nearTicket);
	REQUIRE(editor.Activate(fixture.Source, editor.PreviewStore().Time().Tick, fixture.Diagnostic));
	REQUIRE(editor.CaptureAccepted(fixture.Source, SCOPES[0], captured, fixture.Diagnostic));
	CHECK(Accepted(editor, 1).ArtifactBytes == farArtifact);
}

TEST_CASE(
	"preview submits subsequent scopes after releasing the bounded ready batch", "[studio][procedural]"
) {
	Fixture fixture;
	const std::array<procedural::RegionScope, 3> scopes{{SCOPES[0], SCOPES[1], {40, 0, 16}}};
	fixture.Configure(scopes);
	studio::ProceduralEditor editor;
	REQUIRE(editor.SyncSource(fixture.Source, fixture.Diagnostic));
	editor.Start();
	WaitReady(editor, 2, fixture.Diagnostic);
	CHECK(editor.ReservedBytes() == script::ProceduralHostLimits::MaximumRetainedBytes);
	REQUIRE(editor.Activate(fixture.Source, editor.PreviewStore().Time().Tick, fixture.Diagnostic));
	CHECK(editor.ReservedBytes() == 0);
	WaitReady(editor, 1, fixture.Diagnostic);
	REQUIRE(editor.Activate(fixture.Source, editor.PreviewStore().Time().Tick, fixture.Diagnostic));
	CHECK(editor.PreviewStore().Resource<script::ProceduralPublished>()->Regions.size() == 3);
	CHECK(editor.ReservedBytes() == 0);
}

TEST_CASE("a removed then readded scope never revives its old ticket", "[studio][procedural]") {
	Fixture fixture;
	studio::ProceduralEditor editor;
	REQUIRE(editor.SyncSource(fixture.Source, fixture.Diagnostic));
	editor.Start();
	REQUIRE(editor.Pump(fixture.Diagnostic));
	REQUIRE(editor.Progress().size() == 2);
	const auto nearTicket = editor.Progress()[0].Ticket;
	const auto farTicket = editor.Progress()[1].Ticket;
	const auto nearMesh = editor.Targets()[0].Mesh;
	const std::array<procedural::RegionScope, 1> remaining{SCOPES[1]};
	fixture.Configure(remaining);
	REQUIRE(editor.SyncSource(fixture.Source, fixture.Diagnostic));
	CHECK(RequestProgress(editor, nearTicket).Status == script::ProceduralJobStatus::Canceled);
	CHECK(RequestProgress(editor, farTicket).Status == script::ProceduralJobStatus::Pending);
	fixture.Configure(SCOPES);
	REQUIRE(editor.SyncSource(fixture.Source, fixture.Diagnostic));
	CHECK(RequestProgress(editor, nearTicket).Status == script::ProceduralJobStatus::Canceled);
	CHECK(RequestProgress(editor, farTicket).Status == script::ProceduralJobStatus::Pending);
	CHECK_FALSE(editor.PreviewStore().Alive(nearMesh));
	WaitReady(editor, 2, fixture.Diagnostic);
	CHECK(RequestProgress(editor, farTicket).Status == script::ProceduralJobStatus::Ready);
	CHECK(std::none_of(editor.Progress().begin(), editor.Progress().end(), [&](const auto &row) {
		return row.Ticket == nearTicket;
	}));
	REQUIRE(editor.Activate(fixture.Source, editor.PreviewStore().Time().Tick, fixture.Diagnostic));
	script::ProceduralPublication captured;
	REQUIRE(editor.CaptureAccepted(fixture.Source, SCOPES[0], captured, fixture.Diagnostic));
}

TEST_CASE("a named owner change cancels every previously admitted preview ticket", "[studio][procedural]") {
	Fixture fixture;
	studio::ProceduralEditor editor;
	REQUIRE(editor.SyncSource(fixture.Source, fixture.Diagnostic));
	editor.Start();
	WaitReady(editor, 2, fixture.Diagnostic);
	const auto nearTicket = editor.Progress()[0].Ticket;
	const auto farTicket = editor.Progress()[1].Ticket;
	fixture.Configure(SCOPES, "different-owner");
	REQUIRE(editor.SyncSource(fixture.Source, fixture.Diagnostic));
	CHECK(RequestProgress(editor, nearTicket).Status == script::ProceduralJobStatus::Canceled);
	CHECK(RequestProgress(editor, farTicket).Status == script::ProceduralJobStatus::Canceled);
	CHECK_FALSE(editor.Activate(fixture.Source, editor.PreviewStore().Time().Tick, fixture.Diagnostic));
	WaitReady(editor, 2, fixture.Diagnostic);
	CHECK(std::none_of(editor.Progress().begin(), editor.Progress().end(), [&](const auto &row) {
		return row.Ticket == nearTicket || row.Ticket == farTicket;
	}));
	REQUIRE(editor.Activate(fixture.Source, editor.PreviewStore().Time().Tick, fixture.Diagnostic));
}

TEST_CASE(
	"accepted capture verifies artifact bytes and its exact current regional key", "[studio][procedural]"
) {
	Fixture fixture;
	studio::ProceduralEditor editor;
	Generate(editor, fixture);
	const auto near = Accepted(editor, 0);
	const auto far = Accepted(editor, 1);
	auto *published = editor.PreviewStore().ResourceMutable<script::ProceduralPublished>();
	published->Regions[0].ArtifactBytes.back() ^= 1;
	script::ProceduralPublication output = far;
	CHECK_FALSE(editor.CaptureAccepted(fixture.Source, SCOPES[0], output, fixture.Diagnostic));
	CHECK(fixture.Diagnostic.Field == "preview.artifact_digest");
	CHECK(output.ArtifactBytes == far.ArtifactBytes);
	published->Regions[0] = near;
	published->Regions[0].ArtifactBytes = far.ArtifactBytes;
	published->Regions[0].ArtifactDigest = far.ArtifactDigest;
	CHECK_FALSE(editor.CaptureAccepted(fixture.Source, SCOPES[0], output, fixture.Diagnostic));
	CHECK(fixture.Diagnostic.Code == procedural::Status::StaleArtifact);
	CHECK(output.ArtifactBytes == far.ArtifactBytes);
}

TEST_CASE(
	"lost source cancels admitted work and stale scene edits refuse publication", "[studio][procedural]"
) {
	Fixture fixture;
	studio::ProceduralEditor editor;
	Generate(editor, fixture);
	const auto previous = Accepted(editor, 0);
	fixture.MoveVillage();
	REQUIRE(editor.SyncSource(fixture.Source, fixture.Diagnostic));
	WaitReady(editor, 1, fixture.Diagnostic);
	const auto target = editor.Targets()[0];
	editor.PreviewStore().GetMutable<scene::Visual>(target.Part)->Transparency = 0.5f;
	CHECK_FALSE(editor.Activate(fixture.Source, editor.PreviewStore().Time().Tick, fixture.Diagnostic));
	CHECK(Accepted(editor, 0).ArtifactDigest == previous.ArtifactDigest);
	fixture.Source.RemoveResource<script::ProceduralAuthority>();
	CHECK_FALSE(editor.SyncSource(fixture.Source, fixture.Diagnostic));
	CHECK(fixture.Diagnostic.Code == procedural::Status::MissingSource);
	Drain(editor, fixture.Diagnostic);
	CHECK(editor.ReservedBytes() == 0);
	CHECK(Accepted(editor, 0).ArtifactBytes == previous.ArtifactBytes);
}

TEST_CASE(
	"native preview tears down active threaded work and refuses unsupported contexts", "[studio][procedural]"
) {
	Fixture fixture;
	{
		studio::ProceduralEditor editor;
		REQUIRE(editor.SyncSource(fixture.Source, fixture.Diagnostic));
		editor.Start();
		REQUIRE(editor.Pump(fixture.Diagnostic));
		CHECK(editor.ReservedBytes() != 0);
	}
	studio::ProceduralEditor unsupported(parallel::JobContext::Processed);
	REQUIRE(unsupported.SyncSource(fixture.Source, fixture.Diagnostic));
	unsupported.Start();
	CHECK_FALSE(unsupported.Pump(fixture.Diagnostic));
	CHECK(fixture.Diagnostic.Field == "request.context");
	CHECK_FALSE(unsupported.Running());
	CHECK(unsupported.ReservedBytes() == 0);
}
