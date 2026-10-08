#include "SourceCommonExecution.hpp"

#include <engine/imagegraph/SourceCommonRuntime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.source_common_admission")
TEST_DEPENDS("engine.imagegraph.source_common_runtime")

using namespace engine::imagegraph;
namespace {
	Document Authored() {
		Document document;
		document.FormatVersion = 11;
		document.Nodes = {{"owner", "pc.boolean", {}, {}, {{"value", true}}}};
		document.Outputs = {{"value", "owner", "boolean"}};
		SourceCommonOwnerRecord owner;
		owner.SourceOwnerId = "owner";
		owner.SourceType = "Node_Boolean";
		owner.NativeOwnerId = "owner";
		owner.UpdateAnimatorOwnerId = "owner";
		owner.UpdateAnimatorPort = "native:animator:1";
		document.SourceCommonOwners = {owner};
		document.SourceAnimators.emplace();
		DetachedSourceAnimator writer;
		writer.OwnerId = "owner";
		writer.Id = "native:animator:1";
		writer.OriginalPort = "pxcx.update_in_trigger";
		writer.Type = ValueType::Boolean;
		document.SourceAnimators->Detached = {writer};
		GroupSubtypeOverlay payload;
		payload.NodeId = writer.OwnerId;
		payload.Port = writer.Id;
		payload.Fixed = false;
		document.SourceAnimators->DetachedValues = {payload};
		return document;
	}
	Plan Checked(const Document &document) {
		Plan plan;
		Diagnostic diagnostic;
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		return plan;
	}
	GroupRenderSession Initialized(const Document &document, const Plan &plan) {
		GroupRenderSession session;
		Diagnostic diagnostic;
		const auto status = InitializeNativeSourceCommonRuntime(
			document, plan, {}, SourceNodeInitialState::Loaded, session, diagnostic
		);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return session;
	}
} // namespace

TEST_CASE(
	"common admission imports canonical journals even when common state "
	"is explicitly cold"
) {
	const auto document = Authored();
	const auto plan = Checked(document);
	auto prior = Initialized(document, plan);
	REQUIRE(prior.Nodes.size() == 1);
	REQUIRE(prior.SourceCommonWrites.empty());
	prior.Nodes.front().Rendered = true;
	REQUIRE(prior.CommonAnimators.Detached.size() == 1);
	REQUIRE(prior.CommonAnimators.DetachedValues.size() == 1);
	SourceCommonAnimatorResetReceipt receipt;
	Diagnostic resetDiagnostic;
	auto &writerPayload = prior.CommonAnimators.DetachedValues.front();
	REQUIRE(
		ResetSourceCommonAnimator(
			prior.CommonAnimators.Detached.front(), writerPayload, {}, writerPayload, receipt, resetDiagnostic
		) == Status::Ok
	);
	// The setter returns effects separately from its payload; retain its actual receipt for cold retirement.
	prior.SourceCommonWrites.push_back(
		{"owner", "native:animator:1", receipt.Modified, receipt.ForceDynamic, receipt.Edited}
	);
	RandomReplayState random;
	RandomReplayEntry entry;
	entry.NodeId = "recorded-random";
	entry.Initialized = true;
	entry.StoredSeed = 71;
	entry.PreviousOutput = 0.125;
	random.Entries = {entry};
	SurfaceFrameReplayState surfaces;
	surfaces.Initialized = true;
	surfaces.Tick = 9;
	SimulationReplayState simulation;
	SimulationReplayEntry simulated;
	simulated.NodeId = "recorded-simulation";
	simulated.State.Initialized = true;
	simulated.State.Tick = 5;
	simulation.Entries = {simulated};
	DataReplayState data;
	DataReplayEntry delayed;
	delayed.NodeId = "recorded-data";
	delayed.Initialized = true;
	delayed.PreviousValue = 0.75;
	data.Entries = {delayed};
	RigidReplayState rigid;
	RigidOwnerReplayState physics;
	physics.History.OwnerId = "recorded-physics";
	physics.AuthoringRevision = 81;
	rigid.Owners = {physics};
	EvaluationRequest request;
	request.RandomReplay = &random;
	request.SimulationReplay = &simulation;
	request.DataReplay = &data;
	request.RigidReplay = &rigid;
	request.SurfaceReplay = &surfaces;
	detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	auto priorCharge = budget.Reserve(RetainedGroupRenderSessionBytes(prior));
	REQUIRE(priorCharge);
	detail::SourceCommonAdmission admission;
	detail::AllocationReservation candidateCharge;
	GroupRenderSession candidate;
	Diagnostic diagnostic;
	const auto status = detail::PrepareSourceCommonCandidate(
		document,
		plan,
		request,
		SourceNodeInitialState::Loaded,
		prior,
		candidate,
		budget,
		candidateCharge,
		admission,
		diagnostic
	);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(candidate.Replay.Random == random);
	REQUIRE(candidate.Replay.Surfaces == surfaces);
	REQUIRE(candidate.Replay.Simulation == simulation);
	REQUIRE(candidate.Replay.Data == data);
	REQUIRE(candidate.Replay.Rigid == rigid);
	REQUIRE(candidate.Nodes.size() == 1);
	REQUIRE_FALSE(candidate.Nodes.front().Rendered);
	REQUIRE(candidate.SourceCommonWrites.empty());
	REQUIRE(candidate.SourceCommonInputs.size() == 1);
	REQUIRE_FALSE(candidate.SourceCommonInputs.front().Inputs.has_value());
	REQUIRE(prior.Nodes.front().Rendered);
	REQUIRE(prior.SourceCommonWrites.size() == 1);
	REQUIRE(prior.SourceCommonWrites.front().Modified);
	REQUIRE(prior.Replay.Random.Entries.empty());
	REQUIRE(candidateCharge.Bytes() == RetainedGroupRenderSessionBytes(candidate));
}

TEST_CASE(
	"common admission refuses before publishing when the prior alone "
	"fills the byte cap"
) {
	const auto document = Authored();
	const auto plan = Checked(document);
	const auto prior = Initialized(document, plan);
	const auto priorBytes = RetainedGroupRenderSessionBytes(prior);
	detail::EvaluationBudget budget(priorBytes);
	auto priorCharge = budget.Reserve(priorBytes);
	REQUIRE(priorCharge);
	detail::SourceCommonAdmission admission;
	detail::AllocationReservation candidateCharge;
	GroupRenderSession candidate;
	candidate.Nodes = {{"sentinel", true}};
	const auto held = candidate.Nodes;
	Diagnostic diagnostic;
	REQUIRE(
		detail::PrepareSourceCommonCandidate(
			document, plan, {}, std::nullopt, prior, candidate, budget, candidateCharge, admission, diagnostic
		) == Status::LimitExceeded
	);
	REQUIRE(candidate.Nodes == held);
	REQUIRE(candidateCharge.Bytes() == 0);
	REQUIRE(budget.Used() == priorBytes);
}

TEST_CASE(
	"common admission requires explicit reconciliation for reordered "
	"native readiness"
) {
	auto document = Authored();
	document.Nodes.push_back({"unregistered", "pc.boolean", {}, {}, {{"value", false}}});
	const auto plan = Checked(document);
	auto prior = Initialized(document, plan);
	prior.Nodes.front().Rendered = true;
	prior.Outputs.Nodes.front().Outputs.front().Data.reset();
	prior.Outputs.Nodes.front().Outputs.front().Refusal =
		Diagnostic{Status::UnsupportedExecution, "owner", "boolean", "held refusal"};
	std::swap(document.Nodes[0], document.Nodes[1]);
	const auto edited = Checked(document);
	detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	auto priorCharge = budget.Reserve(RetainedGroupRenderSessionBytes(prior));
	REQUIRE(priorCharge);
	detail::SourceCommonAdmission admission;
	detail::AllocationReservation candidateCharge;
	GroupRenderSession candidate;
	Diagnostic diagnostic;
	REQUIRE(
		detail::PrepareSourceCommonCandidate(
			document,
			edited,
			{},
			std::nullopt,
			prior,
			candidate,
			budget,
			candidateCharge,
			admission,
			diagnostic
		) == Status::InvalidValue
	);
	admission = {};
	const auto status = detail::PrepareSourceCommonCandidate(
		document,
		edited,
		{},
		SourceNodeInitialState::Constructed,
		prior,
		candidate,
		budget,
		candidateCharge,
		admission,
		diagnostic,
		detail::SourceCommonPreparePurpose::Reconcile
	);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(candidate.Nodes[0].NodeId == "unregistered");
	REQUIRE(candidate.Nodes[1].NodeId == "owner");
	REQUIRE(candidate.Nodes[1].Rendered);
	const auto &held = candidate.Outputs.Nodes[1].Outputs.front();
	REQUIRE_FALSE(held.Data.has_value());
	REQUIRE(held.Refusal.has_value());
	REQUIRE(held.Refusal->Message == "held refusal");
	REQUIRE(candidate.SourceCommonWrites == prior.SourceCommonWrites);
	REQUIRE(candidateCharge.Bytes() == RetainedGroupRenderSessionBytes(candidate));
}

TEST_CASE("common observation owns font residency separately and refuses foreign common views") {
	const auto document = Authored();
	const auto plan = Checked(document);
	const auto prior = Initialized(document, plan);
	detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	auto priorCharge = budget.Reserve(RetainedGroupRenderSessionBytes(prior));
	REQUIRE(priorCharge);
	detail::SourceCommonAdmission admission;
	EvaluationRequest request;
	request.SourceFontHostResidentBytes = 257;
	AudioClipSource clip;
	clip.SourceId = "whole-clip";
	clip.Data.SampleRate = 48000;
	clip.Data.Samples.resize(5000, 0.25);
	request.AudioClips = std::span<const AudioClipSource>(&clip, 1);
	Diagnostic diagnostic;
	REQUIRE(
		detail::ValidateSourceCommonObservation(
			document, plan, request, prior, budget, admission, diagnostic
		) == Status::Ok
	);
	REQUIRE(admission.FontHost.Bytes() == 257);
	REQUIRE(admission.Observer.Bytes() == 0);
	const auto borrowed = admission.Borrowed.Bytes();
	admission = {};
	request.SourceFontHostResidentBytes = 0;
	REQUIRE(
		detail::ValidateSourceCommonObservation(
			document, plan, request, prior, budget, admission, diagnostic
		) == Status::Ok
	);
	REQUIRE(admission.Borrowed.Bytes() == borrowed);
	admission = {};
	SourceCommonSocketSession unrelated;
	request.SourceCommon = &unrelated;
	REQUIRE(
		detail::ValidateSourceCommonObservation(
			document, plan, request, prior, budget, admission, diagnostic
		) == Status::InvalidValue
	);
	REQUIRE(prior.SourceCommonBindings.front().SourceOwnerId == "owner");
}
