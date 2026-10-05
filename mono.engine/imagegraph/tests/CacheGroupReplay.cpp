#include "nodes/Path3D.hpp"

#include <engine/imagegraph/CacheGroupReplay.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <limits>

TEST_SUITE_ID("engine.imagegraph.cache_group_replay")
using namespace engine::imagegraph;
namespace {
	constexpr auto BYTE_BUDGET = Limits::MaximumEvaluationBytes;
	CacheGroupReplayState Journal() {
		CacheGroupReplayState state;
		for (const auto *id : {"cache-a", "cache-b", "producer", "other"}) {
			auto result = RetainCacheGroupReplayNode(state, id, "fixture", {}, BYTE_BUDGET);
			INFO(result.Error.Message);
			REQUIRE(result.Code == Status::Ok);
		}
		return state;
	}
	CacheGroupReplayChange Refresh(
		CacheGroupReplayState &state,
		std::string_view owner,
		std::span<const std::string> members,
		bool serialize = true
	) {
		return ApplyCacheGroupReplay(
			state, {CacheGroupReplayAction::RefreshOwner, owner, {}, members, serialize}, BYTE_BUDGET
		);
	}
	CacheGroupReplayOperation Disable(std::string_view owner = "cache-a") {
		CacheGroupReplayOperation operation;
		operation.Action = CacheGroupReplayAction::Disable;
		operation.OwnerId = owner;
		operation.Playing = true;
		operation.Project = {{9, .5, false}, 10, false, false};
		return operation;
	}
}
TEST_CASE(
	"Cache-group journal retains complete named outputs with independent owned pixels",
	"[imagegraph][frame_cache_groups]"
) {
	CacheGroupReplayState state;
	Image image{1, 1, {2, 3, 4, 255}};
	ArrayValue tree{ValueType::Any, {}};
	tree.Items = {{std::vector<SourceArrayItem>{{image}, {ElementValue{std::string("leaf")}}}}};
	std::vector<CacheGroupReplayOutput> outputs = {
		{"surface",
		 Value{SurfaceValue{image}},
		 SourceSocketDomain{ValueType::Image, {}, SourceSocketKind::Surface},
		 {}},
		{"nested", Value{tree}, SourceSocketDomain{ValueType::Array, {}, SourceSocketKind::Any}, {}},
		{"refused",
		 {},
		 SourceSocketDomain{ValueType::Font, {}, SourceSocketKind::Font},
		 Diagnostic{Status::UnsupportedExecution, "producer", "refused", "host font unavailable"}},
		{"constructor", Value{int64_t{-4}}, {}, {}}
	};
	auto retained = RetainCacheGroupReplayNode(state, "producer", "pc.fixture", outputs, BYTE_BUDGET);
	REQUIRE(retained.Code == Status::Ok);
	CHECK(state.Nodes.front().Outputs == outputs);
	Diagnostic diagnostic;
	REQUIRE(ValidateCacheGroupReplay(state, BYTE_BUDGET, diagnostic) == Status::Ok);
	auto copy = state;
	std::get<SurfaceValue>(*outputs[0].Data).Data.Pixels[0] = 77;
	std::get<Image>(
		std::get<std::vector<SourceArrayItem>>(std::get<ArrayValue>(*outputs[1].Data).Items.front().Data)
			.front()
			.Data
	)
		.Pixels[0] = 88;
	std::get<SurfaceValue>(*copy.Nodes.front().Outputs[0].Data).Data.Pixels[0] = 99;
	CHECK(std::get<SurfaceValue>(*state.Nodes.front().Outputs[0].Data).Data.Pixels[0] == 2);
	const auto &savedTree = std::get<ArrayValue>(*state.Nodes.front().Outputs[1].Data);
	CHECK(
		std::get<Image>(std::get<std::vector<SourceArrayItem>>(savedTree.Items.front().Data).front().Data)
			.Pixels[0] == 2
	);
	CHECK(*state.Nodes.front().Outputs[3].Data == Value{int64_t{-4}});
	CHECK(state.Nodes.front().Outputs[2].Refusal->Message == "host font unavailable");
	// Borrowed identifiers and outputs may refer into the state being atomically replaced.
	const auto &node = state.Nodes.front();
	REQUIRE(
		RetainCacheGroupReplayNode(state, node.NodeId, node.NodeType, node.Outputs, BYTE_BUDGET).Code ==
		Status::Ok
	);
	CHECK(state.Nodes.front().Outputs[2].Domain->Kind == SourceSocketKind::Font);
}
TEST_CASE(
	"Loaded cache groups preserve overlapping lists missing ids and last owner pointers",
	"[imagegraph][frame_cache_groups]"
) {
	auto state = Journal();
	std::vector<std::string> first{"producer", "missing", "producer"};
	std::vector<std::string> second{"producer", "other"};
	REQUIRE(Refresh(state, "cache-a", first).Code == Status::Ok);
	REQUIRE(state.Owners.front().Members == std::vector<std::string>{"producer", "producer"});
	REQUIRE(Refresh(state, "cache-b", second).Code == Status::Ok);
	CHECK(state.Nodes[2].OwnerId == "cache-b");
	REQUIRE(ApplyCacheGroupReplay(state, Disable(), BYTE_BUDGET).Code == Status::Ok);
	CHECK_FALSE(CacheGroupReplayShouldRun(state, "producer"));
	CHECK(CacheGroupReplayShouldRun(state, "other"));
	CHECK(CacheGroupReplayShouldRun(state, "producer", true));
	CHECK(CacheGroupReplayShouldRun(state, "untracked"));
	// A stale loaded list still toggles the shared activity flag despite the newer owner pointer.
	const auto enabled =
		ApplyCacheGroupReplay(state, {CacheGroupReplayAction::Enable, "cache-a"}, BYTE_BUDGET);
	CHECK(enabled.Code == Status::Ok);
	CHECK(enabled.ClearCapturedFrames);
	CHECK(CacheGroupReplayShouldRun(state, "producer"));
	CHECK(state.Nodes[2].OwnerId == "cache-b");
	REQUIRE(ApplyCacheGroupReplay(state, Disable("cache-b"), BYTE_BUDGET).Code == Status::Ok);
	CHECK_FALSE(CacheGroupReplayShouldRun(state, "other"));
	// Refresh does not wake a member or erase an earlier owner's persisted list.
	REQUIRE(Refresh(state, "cache-a", first).Code == Status::Ok);
	CHECK_FALSE(CacheGroupReplayShouldRun(state, "producer"));
	CHECK(state.Nodes[2].OwnerId == "cache-a");
	CHECK(state.Owners[1].Members == second);
}
TEST_CASE(
	"Interactive cache-group transfer wakes the old owner's member and guarded removal preserves overlaps",
	"[imagegraph][frame_cache_groups]"
) {
	auto state = Journal();
	std::array<std::string, 1> members{"producer"};
	REQUIRE(Refresh(state, "cache-a", members).Code == Status::Ok);
	REQUIRE(Refresh(state, "cache-b", {}).Code == Status::Ok);
	REQUIRE(ApplyCacheGroupReplay(state, Disable(), BYTE_BUDGET).Code == Status::Ok);
	REQUIRE_FALSE(CacheGroupReplayShouldRun(state, "producer"));
	REQUIRE(
		ApplyCacheGroupReplay(
			state, {CacheGroupReplayAction::TransferMember, "cache-b", "producer"}, BYTE_BUDGET
		)
			.Code == Status::Ok
	);
	CHECK(state.Owners[0].Members.empty());
	CHECK(state.Owners[1].Members == std::vector<std::string>{"producer"});
	CHECK(CacheGroupReplayShouldRun(state, "producer"));
	REQUIRE(ApplyCacheGroupReplay(state, Disable("cache-b"), BYTE_BUDGET).Code == Status::Ok);
	const auto frozen = state;
	REQUIRE(
		ApplyCacheGroupReplay(
			state, {CacheGroupReplayAction::RemoveMember, "cache-a", "producer"}, BYTE_BUDGET
		)
			.Code == Status::Ok
	);
	CHECK(state == frozen);
	REQUIRE(
		ApplyCacheGroupReplay(
			state, {CacheGroupReplayAction::TransferMember, "cache-b", "producer"}, BYTE_BUDGET
		)
			.Code == Status::Ok
	);
	CHECK(state == frozen);
	REQUIRE(
		ApplyCacheGroupReplay(
			state, {CacheGroupReplayAction::RemoveMember, "cache-b", "producer"}, BYTE_BUDGET
		)
			.Code == Status::Ok
	);
	CHECK(state.Nodes[2].OwnerId.empty());
	CHECK(CacheGroupReplayShouldRun(state, "producer"));
}
TEST_CASE(
	"Cache-group gates preserve frozen outputs and Serialize changes alone do not wake producers",
	"[imagegraph][frame_cache_groups]"
) {
	auto state = Journal();
	std::array<std::string, 1> members{"producer"};
	std::array<CacheGroupReplayOutput, 1> outputs{{{"value", Value{double{17}}, {}, {}}}};
	REQUIRE(
		RetainCacheGroupReplayNode(state, "producer", "fixture", outputs, BYTE_BUDGET).Code == Status::Ok
	);
	REQUIRE(Refresh(state, "cache-a", members).Code == Status::Ok);
	for (bool loading : {false, true})
		for (bool appending : {false, true})
			for (bool serialize : {false, true})
				for (bool playing : {false, true}) {
					auto trial = state;
					REQUIRE(
						ApplyCacheGroupReplay(
							trial,
							{CacheGroupReplayAction::SetSerialize, "cache-a", {}, {}, serialize},
							BYTE_BUDGET
						)
							.Code == Status::Ok
					);
					auto operation = Disable();
					operation.Playing = playing;
					operation.Project.ProjectLoading = loading;
					operation.Project.ProjectAppending = appending;
					const auto result = ApplyCacheGroupReplay(trial, operation, BYTE_BUDGET);
					REQUIRE(result.Code == Status::Ok);
					CHECK_FALSE(result.ClearCapturedFrames);
					CHECK(
						CacheGroupReplayShouldRun(trial, "producer") ==
						(loading || appending || !serialize || !playing)
					);
				}
	REQUIRE(ApplyCacheGroupReplay(state, Disable(), BYTE_BUDGET).Code == Status::Ok);
	REQUIRE(
		ApplyCacheGroupReplay(
			state, {CacheGroupReplayAction::SetSerialize, "cache-a", {}, {}, false}, BYTE_BUDGET
		)
			.Code == Status::Ok
	);
	CHECK_FALSE(CacheGroupReplayShouldRun(state, "producer"));
	auto enable = ApplyCacheGroupReplay(state, {CacheGroupReplayAction::Enable, "cache-a"}, BYTE_BUDGET);
	CHECK(enable.Code == Status::Ok);
	CHECK_FALSE(enable.ClearCapturedFrames);
	CHECK_FALSE(CacheGroupReplayShouldRun(state, "producer"));
	REQUIRE(
		ApplyCacheGroupReplay(
			state, {CacheGroupReplayAction::SetSerialize, "cache-a", {}, {}, true}, BYTE_BUDGET
		)
			.Code == Status::Ok
	);
	for (bool loading : {false, true})
		for (bool appending : {false, true}) {
			auto trial = state;
			CacheGroupReplayOperation operation{CacheGroupReplayAction::Enable, "cache-a"};
			operation.Project.ProjectLoading = loading;
			operation.Project.ProjectAppending = appending;
			const auto result = ApplyCacheGroupReplay(trial, operation, BYTE_BUDGET);
			REQUIRE(result.Code == Status::Ok);
			CHECK(result.ClearCapturedFrames == (!loading && !appending));
			CHECK(CacheGroupReplayShouldRun(trial, "producer") == (!loading && !appending));
			CHECK(trial.Nodes[2].Outputs == state.Nodes[2].Outputs);
		}
	for (double endpoint : {9., 10., 10.5}) {
		auto trial = state;
		REQUIRE(
			ApplyCacheGroupReplay(trial, {CacheGroupReplayAction::Enable, "cache-a"}, BYTE_BUDGET).Code ==
			Status::Ok
		);
		auto operation = Disable();
		operation.Project.ProjectLastFrame = endpoint;
		REQUIRE(ApplyCacheGroupReplay(trial, operation, BYTE_BUDGET).Code == Status::Ok);
		CHECK(CacheGroupReplayShouldRun(trial, "producer") == (endpoint != 10));
	}
}
TEST_CASE(
	"Cache owner destruction respects old Serialize loading and appending gates",
	"[imagegraph][frame_cache_groups]"
) {
	for (bool loading : {false, true})
		for (bool appending : {false, true})
			for (bool serialize : {false, true}) {
				auto state = Journal();
				std::array<std::string, 1> members{"producer"};
				REQUIRE(Refresh(state, "cache-a", members).Code == Status::Ok);
				REQUIRE(ApplyCacheGroupReplay(state, Disable(), BYTE_BUDGET).Code == Status::Ok);
				REQUIRE(
					ApplyCacheGroupReplay(
						state,
						{CacheGroupReplayAction::SetSerialize, "cache-a", {}, {}, serialize},
						BYTE_BUDGET
					)
						.Code == Status::Ok
				);
				CacheGroupReplayOperation operation{CacheGroupReplayAction::DestroyOwner, "cache-a"};
				operation.Project.ProjectLoading = loading;
				operation.Project.ProjectAppending = appending;
				const auto result = ApplyCacheGroupReplay(state, operation, BYTE_BUDGET);
				REQUIRE(result.Code == Status::Ok);
				CHECK(result.ClearCapturedFrames == (serialize && !loading && !appending));
				CHECK(CacheGroupReplayShouldRun(state, "producer") == (serialize && !loading && !appending));
				CHECK(state.Owners.empty());
				CHECK(state.Nodes[2].OwnerId.empty());
			}
}
TEST_CASE(
	"Cache-group invalid snapshots and bounded mutation failures leave the journal intact",
	"[imagegraph][frame_cache_groups]"
) {
	auto state = Journal();
	std::array<std::string, 1> members{"producer"};
	REQUIRE(Refresh(state, "cache-a", members).Code == Status::Ok);
	const auto before = state;
	std::vector<CacheGroupReplayOutput> outputs{{"port", Value{double{1}}, {}, {}}};
	outputs.push_back(outputs.front());
	CHECK(
		RetainCacheGroupReplayNode(state, "producer", "fixture", outputs, BYTE_BUDGET).Code ==
		Status::DuplicateId
	);
	outputs.pop_back();
	outputs.front().Data = std::numeric_limits<double>::infinity();
	CHECK(
		RetainCacheGroupReplayNode(state, "producer", "fixture", outputs, BYTE_BUDGET).Code ==
		Status::InvalidValue
	);
	outputs.front().Data = double{1};
	outputs.front().Domain = SourceSocketDomain{static_cast<ValueType>(255), {}, {}};
	CHECK(
		RetainCacheGroupReplayNode(state, "producer", "fixture", outputs, BYTE_BUDGET).Code ==
		Status::InvalidValue
	);
	outputs.front().Domain.reset();
	outputs.front().Refusal = Diagnostic{};
	CHECK(
		RetainCacheGroupReplayNode(state, "producer", "fixture", outputs, BYTE_BUDGET).Code ==
		Status::InvalidValue
	);
	outputs.front().Refusal.reset();
	CHECK(
		RetainCacheGroupReplayNode(state, "producer", "other-type", outputs, BYTE_BUDGET).Code ==
		Status::InvalidValue
	);
	CHECK(
		RetainCacheGroupReplayNode(
			state, "producer", "fixture", outputs, RetainedCacheGroupReplayBytes(state)
		)
			.Code == Status::LimitExceeded
	);
	CHECK(
		ApplyCacheGroupReplay(state, Disable(), RetainedCacheGroupReplayBytes(state)).Code ==
		Status::LimitExceeded
	);
	CHECK(
		ApplyCacheGroupReplay(state, {CacheGroupReplayAction::Enable, "missing"}, BYTE_BUDGET).Code ==
		Status::InvalidValue
	);
	auto operation = Disable();
	operation.Project.ProjectFrame.Subframe = 1;
	CHECK(ApplyCacheGroupReplay(state, operation, BYTE_BUDGET).Code == Status::InvalidValue);
	CHECK(state == before);
	Diagnostic diagnostic;
	CHECK(ValidateCacheGroupReplay(state, RetainedCacheGroupReplayBytes(state), diagnostic) == Status::Ok);
	CHECK(
		ValidateCacheGroupReplay(state, RetainedCacheGroupReplayBytes(state) - 1, diagnostic) ==
		Status::LimitExceeded
	);
	auto invalid = state;
	invalid.Nodes.push_back(invalid.Nodes.front());
	CHECK(ValidateCacheGroupReplay(invalid, BYTE_BUDGET, diagnostic) == Status::DuplicateId);
	invalid = state;
	invalid.Nodes[2].OwnerId = "missing";
	CHECK(ValidateCacheGroupReplay(invalid, BYTE_BUDGET, diagnostic) == Status::InvalidValue);
	invalid = state;
	invalid.Owners.front().Members = {"missing"};
	CHECK(ValidateCacheGroupReplay(invalid, BYTE_BUDGET, diagnostic) == Status::InvalidValue);
	invalid = state;
	invalid.Nodes.front().Outputs.resize(
		Limits::MaximumDynamicOutputsPerNode + Limits::MaximumGroupPorts + 1
	);
	CHECK(ValidateCacheGroupReplay(invalid, BYTE_BUDGET, diagnostic) == Status::LimitExceeded);
}
TEST_CASE(
	"Cache-group snapshots retain the full native dynamic output range without changing port order",
	"[imagegraph][frame_cache_groups]"
) {
	CacheGroupReplayState state;
	std::vector<CacheGroupReplayOutput> outputs;
	outputs.reserve(Limits::MaximumDynamicOutputsPerNode + 1);
	outputs.push_back({"declared", Value{int64_t{1}}, {}, {}});
	for (size_t index = Limits::MaximumDynamicOutputsPerNode; index > 0; --index)
		outputs.push_back({"output_" + std::to_string(index), Value{double(index)}, {}, {}});
	REQUIRE(
		RetainCacheGroupReplayNode(state, "split", "pc.array_split", outputs, BYTE_BUDGET).Code == Status::Ok
	);
	CHECK(state.Nodes.front().Outputs == outputs);
	const auto before = state;
	outputs.push_back(outputs[2]);
	CHECK(
		RetainCacheGroupReplayNode(state, "split", "pc.array_split", outputs, BYTE_BUDGET).Code ==
		Status::DuplicateId
	);
	CHECK(state == before);
}
TEST_CASE(
	"Cache-group repeated long-name scans are refused before comparison work",
	"[imagegraph][frame_cache_groups]"
) {
	CacheGroupReplayState state;
	state.Nodes.reserve(128);
	const std::string prefix(8192, 'a');
	for (size_t index = 0; index < 128; ++index)
		state.Nodes.push_back({prefix + std::to_string(index), "fixture", {}, true, {}});
	REQUIRE(RetainedCacheGroupReplayBytes(state) < BYTE_BUDGET);
	Diagnostic diagnostic;
	CHECK(ValidateCacheGroupReplay(state, BYTE_BUDGET, diagnostic) == Status::LimitExceeded);
	CHECK(diagnostic.Message.find("comparison work") != std::string::npos);
	const auto before = state;
	CHECK(RetainCacheGroupReplayNode(state, "new", "fixture", {}, BYTE_BUDGET).Code == Status::LimitExceeded);
	CHECK(
		ApplyCacheGroupReplay(
			state, {CacheGroupReplayAction::RefreshOwner, state.Nodes.front().NodeId}, BYTE_BUDGET
		)
			.Code == Status::LimitExceeded
	);
	CHECK(state == before);
}
TEST_CASE(
	"Cache-group typed retirement wakes surviving members only through admitted owner destruction",
	"[imagegraph][frame_cache_groups]"
) {
	auto state = Journal();
	std::array<std::string, 2> members{"producer", "other"};
	REQUIRE(Refresh(state, "cache-a", members).Code == Status::Ok);
	REQUIRE(ApplyCacheGroupReplay(state, Disable(), BYTE_BUDGET).Code == Status::Ok);
	Document document;
	document.Nodes = {{"producer", "fixture", "", {}, {}}, {"other", "replacement", "", {}, {}}};
	Diagnostic diagnostic;
	CacheGroupReplayState target = state;
	CHECK(
		ReconcileCacheGroupReplay(document, state, target, {}, BYTE_BUDGET, diagnostic) ==
		Status::UnsupportedExecution
	);
	CHECK(target == state);
	for (bool loading : {false, true})
		for (bool appending : {false, true}) {
			std::optional<SourceFrameCacheProjectObservation> project{{{}, 10, loading, appending}};
			REQUIRE(
				ReconcileCacheGroupReplay(document, state, target, project, BYTE_BUDGET, diagnostic) ==
				Status::Ok
			);
			REQUIRE(target.Nodes.size() == 1);
			CHECK(target.Nodes.front().NodeId == "producer");
			CHECK(target.Nodes.front().OwnerId.empty());
			CHECK(target.Nodes.front().RenderActive == (!loading && !appending));
			CHECK(target.Owners.empty());
		}
	REQUIRE(
		ApplyCacheGroupReplay(
			state, {CacheGroupReplayAction::SetSerialize, "cache-a", {}, {}, false}, BYTE_BUDGET
		)
			.Code == Status::Ok
	);
	REQUIRE(ReconcileCacheGroupReplay(document, state, target, {}, BYTE_BUDGET, diagnostic) == Status::Ok);
	CHECK_FALSE(target.Nodes.front().RenderActive);
	document.Nodes.push_back({"cache-a", "fixture", "", {}, {}});
	REQUIRE(ReconcileCacheGroupReplay(document, state, target, {}, BYTE_BUDGET, diagnostic) == Status::Ok);
	REQUIRE(target.Owners.size() == 1);
	CHECK(target.Owners.front().Members == std::vector<std::string>{"producer"});
	const auto producer = std::find_if(target.Nodes.begin(), target.Nodes.end(), [](const auto &node) {
		return node.NodeId == "producer";
	});
	REQUIRE(producer != target.Nodes.end());
	CHECK(producer->OwnerId == "cache-a");
	const auto before = target;
	CHECK(ReconcileCacheGroupReplay(document, state, target, {}, 1, diagnostic) == Status::LimitExceeded);
	CHECK(target == before);
	REQUIRE(ReconcileCacheGroupReplay(document, target, target, {}, BYTE_BUDGET, diagnostic) == Status::Ok);
	CHECK(target == before);
}

namespace {
	DataReplayState
	FrozenNode(std::string id, std::string type, std::vector<CacheGroupReplayOutput> outputs) {
		DataReplayState state;
		REQUIRE(
			RetainCacheGroupReplayNode(state.CacheGroups, id, type, outputs, BYTE_BUDGET).Code == Status::Ok
		);
		state.CacheGroups.Nodes.front().RenderActive = false;
		return state;
	}
}
TEST_CASE(
	"Frozen getters prune hidden failures while independent output roots still execute",
	"[imagegraph][frame_cache_groups]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"bad", "value.math", "", {}, {{"mode", int64_t{19}}, {"a", 1.}, {"b", 1.}}},
		{"frozen", "value.math", "", {}, {{"mode", int64_t{0}}, {"b", 100.}}},
		{"consumer", "value.math", "", {}, {{"mode", int64_t{0}}, {"b", 2.}}}
	};
	document.Links = {{"bad", "result", "frozen", "a"}, {"frozen", "result", "consumer", "a"}};
	document.Outputs = {{"out", "consumer", "result"}, {"bad-out", "bad", "result"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	auto prior = FrozenNode("frozen", "value.math", {{"result", Value{7.}, {}, {}}});
	const auto original = prior;
	EvaluationRequest request;
	request.DataReplay = &prior;
	EvaluatedValue value;
	REQUIRE(EvaluateValue(document, plan, "out", request, value, diagnostic) == Status::Ok);
	CHECK(std::get<double>(value.Data) == 9.);
	StatefulEvaluationResult result;
	REQUIRE(EvaluateStateful(document, plan, "out", request, result, diagnostic) == Status::Ok);
	CHECK(std::get<double>(std::get<EvaluatedValue>(result.Output).Data) == 9.);
	CHECK(result.Data.CacheGroups == prior.CacheGroups);
	const auto previous = result.Output;
	CHECK(EvaluateStateful(document, plan, "bad-out", request, result, diagnostic) != Status::Ok);
	CHECK(std::get<EvaluatedValue>(result.Output).Data == std::get<EvaluatedValue>(previous).Data);
	CHECK(prior == original);
	StatefulOutputEvaluationResult batch;
	const std::vector<std::string> ids{"out", "bad-out"};
	CHECK(EvaluateStatefulOutputs(document, plan, ids, request, batch, diagnostic) != Status::Ok);
	CHECK(batch.Outputs.empty());
}
TEST_CASE(
	"Frozen named ports preserve raw domains nested arrays and precise refused siblings",
	"[imagegraph][frame_cache_groups]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {{"frozen", "value.color_data", "", {}, {}}};
	document.Outputs = {{"good", "frozen", "red"}, {"refused", "frozen", "alpha"}};
	ArrayValue array{ValueType::Any, {}};
	array.Items = {
		{std::vector<SourceArrayItem>{{Image{1, 1, {8, 7, 6, 255}}}, {ElementValue{std::string{"raw"}}}}}
	};
	SourceSocketDomain domain{ValueType::Array, SourceValueDisplay::Vector, SourceSocketKind::Surface};
	auto prior = FrozenNode(
		"frozen",
		"value.color_data",
		{{"red", Value{array}, domain, {}},
		 {"alpha",
		  {},
		  {},
		  Diagnostic{Status::UnsupportedExecution, "frozen", "alpha", "original source refusal"}}}
	);
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	request.DataReplay = &prior;
	EvaluatedValue value;
	REQUIRE(EvaluateValue(document, plan, "good", request, value, diagnostic) == Status::Ok);
	CHECK(value.Data == Value{array});
	CHECK(value.Domain == domain);
	const auto previous = value;
	CHECK(
		EvaluateValue(document, plan, "refused", request, value, diagnostic) == Status::UnsupportedExecution
	);
	CHECK(diagnostic.Message == "original source refusal");
	CHECK(diagnostic.Port == "alpha");
	CHECK(value.Data == previous.Data);
	StatefulOutputEvaluationResult batch;
	const std::vector<std::string> ids{"good"};
	REQUIRE(EvaluateStatefulOutputs(document, plan, ids, request, batch, diagnostic) == Status::Ok);
	CHECK(std::get<EvaluatedValue>(batch.Outputs.front().Output).Domain == domain);
	CHECK(std::get<EvaluatedValue>(batch.Outputs.front().Output).Data == Value{array});
}
TEST_CASE(
	"Frozen surfaces feed image kernels without changing caller pixels and refuse absent ports",
	"[imagegraph][frame_cache_groups]"
) {
	Document document;
	document.Nodes = {
		{"frozen",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{1, 2, 3, 255}}}},
		{"consumer", "image.passthrough", "", {}, {}}
	};
	document.Links = {{"frozen", "image", "consumer", "image"}};
	document.Outputs = {{"out", "consumer", "image"}, {"direct", "frozen", "image"}};
	auto prior = FrozenNode(
		"frozen", "image.solid", {{"image", Value{SurfaceValue{Image{1, 1, {4, 5, 6, 255}}}}, {}, {}}}
	);
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	request.DataReplay = &prior;
	Image image;
	REQUIRE(Evaluate(document, plan, "out", request, image, diagnostic) == Status::Ok);
	CHECK(image.Pixels == std::vector<uint8_t>{4, 5, 6, 255});
	image.Pixels[0] = 99;
	CHECK(std::get<SurfaceValue>(*prior.CacheGroups.Nodes.front().Outputs.front().Data).Data.Pixels[0] == 4);
	prior.CacheGroups.Nodes.front().Outputs.clear();
	const auto previous = image;
	CHECK(Evaluate(document, plan, "direct", request, image, diagnostic) == Status::InvalidOutput);
	CHECK(image == previous);
}
TEST_CASE(
	"Frozen restore rejects aggregate residency before publication and retires changed types",
	"[imagegraph][frame_cache_groups]"
) {
	Document document;
	document.Nodes = {
		{"frozen",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{1, 2, 3, 255}}}}
	};
	document.Outputs = {{"out", "frozen", "image"}};
	auto prior = FrozenNode(
		"frozen",
		"image.solid",
		{{"image", Value{SurfaceValue{Image{32, 32, std::vector<uint8_t>(4096, 8)}}}, {}, {}}}
	);
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	request.DataReplay = &prior;
	Image image{1, 1, {1, 2, 3, 255}};
	const auto previous = image;
	request.MaximumImageDimension = 1;
	CHECK(Evaluate(document, plan, "out", request, image, diagnostic) == Status::LimitExceeded);
	CHECK(image == previous);
	CHECK(diagnostic.Port == "image");
	request.MaximumImageDimension = Limits::MaximumDimension;

	CHECK(
		Evaluate(document, plan, "out", request, image, diagnostic, RetainedDataReplayBytes(prior)) ==
		Status::LimitExceeded
	);
	CHECK(image == previous);
	StatefulEvaluationResult result;
	CHECK(
		EvaluateStateful(
			document, plan, "out", request, result, diagnostic, RetainedDataReplayBytes(prior)
		) == Status::LimitExceeded
	);
	CHECK(result.Data.CacheGroups.Nodes.empty());
	prior.CacheGroups.Nodes.front().NodeType = "retired.type";
	REQUIRE(Evaluate(document, plan, "out", request, image, diagnostic) == Status::Ok);
	CHECK(image.Width == 1);
}

TEST_CASE(
	"Frozen producers satisfy static and computed PCX routes without waiting on cut inputs",
	"[imagegraph][frame_cache_groups][pcx]"
) {
	Document document;
	document.FormatVersion = 9;
	Node producer{"frozen", "pc.equation", "", {}, {{"equation", std::string{"1/0"}}}};
	producer.SourceInternalName = "frozen";
	Node consumer{"consumer", "pc.equation", "", {}, {{"equation", std::string{"frozen.outputs.result+2"}}}};
	consumer.SourceInternalName = "consumer";
	document.Nodes = {producer, consumer};
	document.Outputs = {{"out", "consumer", "result"}};
	auto prior = FrozenNode("frozen", "pc.equation", {{"result", Value{7.}, {}, {}}});
	EvaluationRequest request;
	request.DataReplay = &prior;
	Plan plan;
	Diagnostic diagnostic;
	EvaluatedValue value;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(EvaluateValue(document, plan, "out", request, value, diagnostic) == Status::Ok);
	CHECK(std::get<double>(value.Data) == 9.);
	document.Nodes[1].Values = {{"equation", std::string{}}};
	Node text{"code", "pc.string", "", {}, {{"text", std::string{"frozen.outputs.result+3"}}}};
	document.Nodes.push_back(text);
	document.Links = {{"code", "text", "consumer", "equation"}};
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(EvaluateValue(document, plan, "out", request, value, diagnostic) == Status::Ok);
	CHECK(std::get<double>(value.Data) == 10.);
}
TEST_CASE(
	"Frozen native image arrays restore indexed leaves and nested shape for array consumers",
	"[imagegraph][frame_cache_groups]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"hidden",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{99, 99, 99, 255}}}},
		{"frozen", "value.array", "", {}, {}, {{"input", ValueType::Image, std::nullopt}}},
		{"pick", "value.array_get", "", {}, {{"index", int64_t{1}}}}
	};
	document.Links = {{"hidden", "image", "frozen", "input"}, {"frozen", "array", "pick", "array"}};
	document.Outputs = {{"out", "pick", "image"}, {"array", "frozen", "array"}};
	ArrayValue array{ValueType::Any, {}};
	array.Items = {
		{std::vector<SourceArrayItem>{{Image{1, 1, {1, 2, 3, 255}}}}}, {Image{1, 1, {4, 5, 6, 255}}}
	};
	auto prior = FrozenNode("frozen", "value.array", {{"array", Value{array}, {}, {}}});
	EvaluationRequest request;
	request.DataReplay = &prior;
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image image;
	REQUIRE(Evaluate(document, plan, "out", request, image, diagnostic) == Status::Ok);
	CHECK(image.Pixels == std::vector<uint8_t>{4, 5, 6, 255});
	ImageArray restored;
	REQUIRE(EvaluateArray(document, plan, "array", request, restored, diagnostic) == Status::Ok);
	REQUIRE(restored.Items.size() == 2);
	REQUIRE(restored.Images.size() == 2);
	CHECK(std::get<std::vector<ImageArrayItem>>(restored.Items[0].Data).size() == 1);
	CHECK(std::get<size_t>(restored.Items[1].Data) == 1);
	CHECK(restored.Images[0].Pixels == std::vector<uint8_t>{1, 2, 3, 255});
	std::get<Image>(
		std::get<ArrayValue>(*prior.CacheGroups.Nodes.front().Outputs.front().Data).Items[1].Data
	) = Image{32, 32, std::vector<uint8_t>(4096, 8)};
	request.MaximumImageDimension = 1;
	const auto previous = image;
	CHECK(Evaluate(document, plan, "out", request, image, diagnostic) == Status::LimitExceeded);
	CHECK(image == previous);
}

TEST_CASE(
	"Live instances retain animator reads from a frozen base in either declaration order",
	"[imagegraph][frame_cache_groups]"
) {
	for (const bool baseFirst : {false, true}) {
		Document document;
		document.FormatVersion = 9;
		document.Timeline = TimelineSettings{11, 0, 10, "loop", 30};
		Node base{"base", "pc.number_simple", "", {}, {{"value", 2.}}};
		Node instance{"instance", "pc.number_simple", "", {}, {}};
		instance.InstanceBase = "base";
		base.SourceAnimatedInputs = {"value"};
		instance.SourceAnimatedInputs = {"value"};
		document.Nodes = baseFirst ? std::vector<Node>{base, instance} : std::vector<Node>{instance, base};
		document.Keyframes = {{"base", "value", 0, 2., "linear"}, {"base", "value", 10, 8., "linear"}};
		document.Outputs = {{"out", "instance", "number"}, {"base-out", "base", "number"}};
		auto prior = FrozenNode("base", "pc.number_simple", {{"number", Value{100.}, {}, {}}});
		EvaluationRequest request;
		request.DataReplay = &prior;
		request.Tick = 10;
		Plan plan;
		Diagnostic diagnostic;
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		EvaluatedValue value;
		REQUIRE(EvaluateValue(document, plan, "out", request, value, diagnostic) == Status::Ok);
		CHECK(std::get<double>(value.Data) == 8.);
		REQUIRE(EvaluateValue(document, plan, "base-out", request, value, diagnostic) == Status::Ok);
		CHECK(std::get<double>(value.Data) == 100.);
	}
}

TEST_CASE(
	"Active cache-group getters refresh values preserve uncomputed ports and freeze the latest observation",
	"[imagegraph][frame_cache_groups]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"value", "pc.number_simple", "", {}, {{"value", 7.}}}, {"owner", "pc.cache", "", {}, {}}
	};
	document.Outputs = {{"out", "value", "number"}};
	auto prior = FrozenNode(
		"value",
		"pc.number_simple",
		{{"number", Value{1.}, {}, {}},
		 {"constructor", Value{std::string{"retained"}}, {}, {}},
		 {"uncomputed",
		  {},
		  {},
		  Diagnostic{Status::UnsupportedExecution, "value", "uncomputed", "saved refusal"}}}
	);
	prior.CacheGroups.Nodes.front().RenderActive = true;
	REQUIRE(
		RetainCacheGroupReplayNode(prior.CacheGroups, "owner", "pc.cache", {}, BYTE_BUDGET).Code == Status::Ok
	);
	const std::array<std::string, 1> members{"value"};
	REQUIRE(Refresh(prior.CacheGroups, "owner", members).Code == Status::Ok);
	const auto original = prior;
	EvaluationRequest request;
	request.DataReplay = &prior;
	Plan plan;
	Diagnostic diagnostic;
	StatefulEvaluationResult result;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	INFO(diagnostic.Message);
	REQUIRE(EvaluateStateful(document, plan, "out", request, result, diagnostic) == Status::Ok);
	const auto &captured = result.Data.CacheGroups.Nodes.front();
	REQUIRE(captured.Outputs.size() == 3);
	CHECK(captured.RenderActive);
	CHECK(captured.OwnerId == "owner");
	CHECK(result.Data.CacheGroups.Owners == original.CacheGroups.Owners);
	CHECK(*captured.Outputs[0].Data == Value{7.});
	CHECK(captured.Outputs[0].Domain == std::get<EvaluatedValue>(result.Output).Domain);
	CHECK(captured.Outputs[1] == original.CacheGroups.Nodes.front().Outputs[1]);
	CHECK(captured.Outputs[2] == original.CacheGroups.Nodes.front().Outputs[2]);
	CHECK(prior == original);
	document.Nodes.front().Values.front().Data = 11.;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	request.DataReplay = &result.Data;
	request.Tick = 1;
	REQUIRE(EvaluateStateful(document, plan, "out", request, result, diagnostic) == Status::Ok);
	CHECK(*result.Data.CacheGroups.Nodes.front().Outputs.front().Data == Value{11.});
	result.Data.CacheGroups.Nodes.front().RenderActive = false;
	document.Nodes.front().Values.front().Data = 99.;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(EvaluateStateful(document, plan, "out", request, result, diagnostic) == Status::Ok);
	CHECK(std::get<EvaluatedValue>(result.Output).Data == Value{11.});
	CHECK_FALSE(result.Data.CacheGroups.Nodes.front().RenderActive);
}
TEST_CASE(
	"Live native surface snapshots exceed nested-value bounds and refuse insufficient residency atomically",
	"[imagegraph][frame_cache_groups]"
) {
	Document document;
	document.Nodes = {
		{"image",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1025}}, {"height", int64_t{1024}}, {"colour", Colour{8, 7, 6, 255}}}}
	};
	document.Outputs = {{"out", "image", "image"}};
	auto prior = FrozenNode("image", "image.solid", {});
	prior.CacheGroups.Nodes.front().RenderActive = true;
	EvaluationRequest request;
	request.DataReplay = &prior;
	Plan plan;
	Diagnostic diagnostic;
	StatefulEvaluationResult result;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	const auto evaluated = EvaluateStateful(document, plan, "out", request, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(evaluated == Status::Ok);
	const auto &surface =
		std::get<SurfaceValue>(*result.Data.CacheGroups.Nodes.front().Outputs.front().Data).Data;
	CHECK(surface.Pixels.size() > Limits::MaximumArrayBytes);
	CHECK(surface == std::get<Image>(result.Output));
	CHECK(ValidateDataReplay(result.Data, BYTE_BUDGET, diagnostic) == Status::Ok);
	const auto saved = result.Data;
	std::get<Image>(result.Output).Pixels.front() = 99;
	CHECK(surface.Pixels.front() == 8);
	result.Data.CacheGroups.Nodes.front().RenderActive = false;
	request.DataReplay = &result.Data;
	REQUIRE(EvaluateStateful(document, plan, "out", request, result, diagnostic) == Status::Ok);
	CHECK(std::get<Image>(result.Output).Pixels.front() == 8);
	const auto before = result.Data;
	CHECK(
		EvaluateStateful(
			document, plan, "out", request, result, diagnostic, RetainedDataReplayBytes(before)
		) == Status::LimitExceeded
	);
	CHECK(result.Data == before);
	CHECK(saved.CacheGroups.Nodes.front().RenderActive);
	CHECK(prior.CacheGroups.Nodes.front().Outputs.empty());
}
TEST_CASE(
	"Live cache-group refusal replaces stale pixels while usable sibling getters freeze independently",
	"[imagegraph][frame_cache_groups]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"grid",
		 "pc.grid",
		 "",
		 {},
		 {{"dimension", Vector2{4, 4}},
		  {"dimension_unit", EnumValue{0}},
		  {"seed", 1.},
		  {"grid_size", Vector2{2, 2}},
		  {"grid_size_unit", EnumValue{0}},
		  {"render_type", EnumValue{1}},
		  {"gap_width", 1.},
		  {"tile_color", Gradient{0, {{0, {255, 255, 255, 255}}}}}}}
	};
	document.Outputs = {{"colour", "grid", "surface_out"}, {"height", "grid", "heightmap"}};
	auto prior = FrozenNode(
		"grid", "pc.grid", {{"heightmap", Value{SurfaceValue{Image{1, 1, {8, 7, 6, 255}}}}, {}, {}}}
	);
	prior.CacheGroups.Nodes.front().RenderActive = true;
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	request.DataReplay = &prior;
	StatefulEvaluationResult result;
	const auto evaluated = EvaluateStateful(document, plan, "colour", request, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(evaluated == Status::Ok);
	const auto &ports = result.Data.CacheGroups.Nodes.front().Outputs;
	REQUIRE(ports.front().Port == "heightmap");
	CHECK_FALSE(ports.front().Data);
	REQUIRE(ports.front().Refusal);
	const auto refusal = *ports.front().Refusal;
	result.Data.CacheGroups.Nodes.front().RenderActive = false;
	request.DataReplay = &result.Data;
	REQUIRE(EvaluateStateful(document, plan, "colour", request, result, diagnostic) == Status::Ok);
	const auto before = result.Data;
	CHECK(EvaluateStateful(document, plan, "height", request, result, diagnostic) == refusal.Code);
	CHECK(diagnostic == refusal);
	CHECK(result.Data == before);
	CHECK(prior.CacheGroups.Nodes.front().Outputs.front().Data.has_value());
}
TEST_CASE(
	"Live native image arrays retain nested indexed shape through frozen getters",
	"[imagegraph][frame_cache_groups]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"a",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{1, 2, 3, 255}}}},
		{"b",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{4, 5, 6, 255}}}},
		{"inner", "value.array", "", {}, {}, {{"input", ValueType::Image, std::nullopt}}},
		{"outer",
		 "value.array",
		 "",
		 {},
		 {},
		 {{"nested", ValueType::Array, std::nullopt}, {"leaf", ValueType::Image, std::nullopt}}}
	};
	document.Links = {
		{"a", "image", "inner", "input"},
		{"inner", "array", "outer", "nested"},
		{"b", "image", "outer", "leaf"}
	};
	document.Outputs = {{"out", "outer", "array"}};
	auto prior = FrozenNode("outer", "value.array", {});
	prior.CacheGroups.Nodes.front().RenderActive = true;
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	request.DataReplay = &prior;
	StatefulEvaluationResult result;
	const auto evaluated = EvaluateStateful(document, plan, "out", request, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(evaluated == Status::Ok);
	const auto &array = std::get<ArrayValue>(*result.Data.CacheGroups.Nodes.front().Outputs.front().Data);
	REQUIRE(array.Items.size() == 2);
	CHECK(std::get<std::vector<SourceArrayItem>>(array.Items.front().Data).size() == 1);
	CHECK(std::get<Image>(array.Items.back().Data).Pixels == std::vector<uint8_t>{4, 5, 6, 255});
	const auto live = std::get<ImageArray>(result.Output);
	result.Data.CacheGroups.Nodes.front().RenderActive = false;
	request.DataReplay = &result.Data;
	REQUIRE(EvaluateStateful(document, plan, "out", request, result, diagnostic) == Status::Ok);
	CHECK(std::get<ImageArray>(result.Output).Images == live.Images);
	CHECK(std::get<ImageArray>(result.Output).Items == live.Items);
}

namespace {
	ArrayValue GroupIds(std::initializer_list<std::string> ids) {
		ArrayValue value{ValueType::Text, {}};
		for (const auto &id : ids)
			value.Elements.push_back(id);
		return value;
	}
	Document AuthoredGroups() {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"cache-a", "pc.cache", "", {}, {}},
			{"cache-b", "pc.cache_array", "", {}, {}},
			{"number", "pc.number_simple", "", {}, {{"value", 99.}}},
			{"path", "pc.path_join", "", {}, {}},
			{"unrelated", "pc.string", "", {}, {}}
		};
		document.Nodes[0].SourceProperties = {
			{"cache_group", GroupIds({"number", "missing", "number", "path"})}
		};
		document.Nodes[1].SourceProperties = {{"cache_group", GroupIds({"number"})}, {"serialize", false}};
		document.Outputs = {{"number", "number", "number"}};
		return document;
	}
}
TEST_CASE(
	"Authored cache-group load preserves source owner order overlaps and exact cold literals",
	"[imagegraph][frame_cache_groups]"
) {
	const auto document = AuthoredGroups();
	CacheGroupReplayState source, loaded;
	Diagnostic diagnostic;
	const auto initialized =
		InitializeAuthoredCacheGroupReplay(document, source, loaded, BYTE_BUDGET, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(initialized == Status::Ok);
	REQUIRE(loaded.Nodes.size() == 4);
	REQUIRE(loaded.Owners.size() == 2);
	CHECK(loaded.Owners[0].Members == std::vector<std::string>{"number", "number", "path"});
	CHECK(loaded.Owners[1].Members == std::vector<std::string>{"number"});
	CHECK(loaded.Owners[0].Serialize);
	CHECK_FALSE(loaded.Owners[1].Serialize);
	const auto number = std::find_if(loaded.Nodes.begin(), loaded.Nodes.end(), [](const auto &node) {
		return node.NodeId == "number";
	});
	REQUIRE(number != loaded.Nodes.end());
	CHECK(number->OwnerId == "cache-b");
	CHECK(number->RenderActive);
	REQUIRE(number->Outputs.size() == 1);
	CHECK(*number->Outputs.front().Data == Value{0.});
	const auto cache = std::find_if(loaded.Nodes.begin(), loaded.Nodes.end(), [](const auto &node) {
		return node.NodeId == "cache-a";
	});
	REQUIRE(cache != loaded.Nodes.end());
	CHECK(*cache->Outputs.front().Data == Value{int64_t{-4}});
	const auto array = std::find_if(loaded.Nodes.begin(), loaded.Nodes.end(), [](const auto &node) {
		return node.NodeId == "cache-b";
	});
	REQUIRE(array != loaded.Nodes.end());
	CHECK(*array->Outputs.front().Data == Value{ArrayValue{ValueType::Any, {}}});
	CHECK(source.Nodes.empty());
	CHECK(
		document.Nodes[0].SourceProperties[0].Data == Value{GroupIds({"number", "missing", "number", "path"})}
	);
	CHECK(ValidateCacheGroupReplay(loaded, BYTE_BUDGET, diagnostic) == Status::Ok);
	std::swap(loaded.Nodes[0], loaded.Nodes[3]);
	const auto original = loaded;
	REQUIRE(
		InitializeAuthoredCacheGroupReplay(document, loaded, loaded, BYTE_BUDGET, diagnostic) == Status::Ok
	);
	CHECK(loaded == original);
}
TEST_CASE(
	"Authored load preserves frozen latest getters and restores cold values without executing producers",
	"[imagegraph][frame_cache_groups]"
) {
	auto document = AuthoredGroups();
	auto prior = FrozenNode("number", "pc.number_simple", {{"number", Value{17.}, {}, {}}});
	Diagnostic diagnostic;
	CacheGroupReplayState loaded;
	REQUIRE(
		InitializeAuthoredCacheGroupReplay(document, prior.CacheGroups, loaded, BYTE_BUDGET, diagnostic) ==
		Status::Ok
	);
	REQUIRE_FALSE(loaded.Nodes.front().RenderActive);
	CHECK(*loaded.Nodes.front().Outputs.front().Data == Value{17.});
	CHECK(loaded.Nodes.front().OwnerId == "cache-b");
	prior.CacheGroups = loaded;
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	request.DataReplay = &prior;
	EvaluatedValue value;
	REQUIRE(EvaluateValue(document, plan, "number", request, value, diagnostic) == Status::Ok);
	CHECK(value.Data == Value{17.});
	CacheGroupReplayState empty;
	REQUIRE(
		InitializeAuthoredCacheGroupReplay(document, empty, loaded, BYTE_BUDGET, diagnostic) == Status::Ok
	);
	prior.CacheGroups = loaded;
	for (auto &node : prior.CacheGroups.Nodes)
		if (node.NodeId == "number") node.RenderActive = false;
	REQUIRE(EvaluateValue(document, plan, "number", request, value, diagnostic) == Status::Ok);
	CHECK(value.Data == Value{0.});
}
TEST_CASE(
	"Unknown source cold constructors retain precise refusals rather than fabricated resource values",
	"[imagegraph][frame_cache_groups]"
) {
	auto document = AuthoredGroups();
	document.Nodes[3].Type = "pc.path_builder";
	CacheGroupReplayState empty, loaded;
	Diagnostic diagnostic;
	REQUIRE(
		InitializeAuthoredCacheGroupReplay(document, empty, loaded, BYTE_BUDGET, diagnostic) == Status::Ok
	);
	const auto path = std::find_if(loaded.Nodes.begin(), loaded.Nodes.end(), [](const auto &node) {
		return node.NodeId == "path";
	});
	REQUIRE(path != loaded.Nodes.end());
	REQUIRE(path->Outputs.size() == 1);
	CHECK_FALSE(path->Outputs.front().Data);
	REQUIRE(path->Outputs.front().Refusal);
	const auto refused = *path->Outputs.front().Refusal;
	CHECK(refused.Message.find("self") != std::string::npos);
	DataReplayState prior;
	prior.CacheGroups = loaded;
	for (auto &node : prior.CacheGroups.Nodes)
		if (node.NodeId == "path") node.RenderActive = false;
	document.Outputs = {{"out", "path", "path"}};
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	request.DataReplay = &prior;
	EvaluatedValue value;
	CHECK(EvaluateValue(document, plan, "out", request, value, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic == refused);
}
TEST_CASE(
	"Authored cache-group load rejects malformed metadata type replacement and resource bounds atomically",
	"[imagegraph][frame_cache_groups]"
) {
	const auto document = AuthoredGroups();
	CacheGroupReplayState empty, loaded;
	Diagnostic diagnostic;
	REQUIRE(
		InitializeAuthoredCacheGroupReplay(document, empty, loaded, BYTE_BUDGET, diagnostic) == Status::Ok
	);
	const auto original = loaded;
	for (const auto malformed :
		 {Value{int64_t{3}},
		  Value{GroupIds({""})},
		  Value{ArrayValue{ValueType::Any, {std::string{"number"}}}}}) {
		auto bad = document;
		bad.Nodes.front().SourceProperties.front().Data = malformed;
		CHECK(
			InitializeAuthoredCacheGroupReplay(bad, empty, loaded, BYTE_BUDGET, diagnostic) ==
			Status::InvalidValue
		);
		CHECK(diagnostic.Port == "cache_group");
		CHECK(loaded == original);
	}
	auto duplicate = document;
	duplicate.Nodes.front().SourceProperties.push_back(duplicate.Nodes.front().SourceProperties.front());
	CHECK(
		InitializeAuthoredCacheGroupReplay(duplicate, empty, loaded, BYTE_BUDGET, diagnostic) ==
		Status::InvalidValue
	);
	CHECK(loaded == original);
	auto replaced = document;
	replaced.Nodes[2].Type = "pc.string";
	CHECK(
		InitializeAuthoredCacheGroupReplay(replaced, original, loaded, BYTE_BUDGET, diagnostic) ==
		Status::InvalidValue
	);
	CHECK(loaded == original);
	CHECK(
		InitializeAuthoredCacheGroupReplay(
			document, original, loaded, RetainedCacheGroupReplayBytes(original), diagnostic
		) == Status::LimitExceeded
	);
	CHECK(loaded == original);
	auto repeated = document;
	auto &ids = std::get<ArrayValue>(repeated.Nodes.front().SourceProperties.front().Data).Elements;
	ids.assign(Limits::MaximumNodes, std::string{"number"});
	repeated.Nodes[1].SourceProperties.front().Data = repeated.Nodes[0].SourceProperties.front().Data;
	Node third = repeated.Nodes.front();
	third.Id = "cache-c";
	repeated.Nodes.push_back(third);
	CHECK(
		InitializeAuthoredCacheGroupReplay(repeated, empty, loaded, BYTE_BUDGET, diagnostic) ==
		Status::LimitExceeded
	);
	CHECK(loaded == original);
}

TEST_CASE(
	"Fresh stateful observations initialize authored groups once then capture current producer outputs",
	"[imagegraph][frame_cache_groups]"
) {
	const auto document = AuthoredGroups();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	StatefulEvaluationResult result;
	EvaluationRequest request;
	const auto evaluated = EvaluateStateful(document, plan, "number", request, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(evaluated == Status::Ok);
	REQUIRE(result.Data.CacheGroups.Owners.size() == 2);
	CHECK(result.Data.CacheGroups.Owners[0].Members == std::vector<std::string>{"number", "number", "path"});
	CHECK(std::get<EvaluatedValue>(result.Output).Data == Value{99.});
	auto number = std::find_if(
		result.Data.CacheGroups.Nodes.begin(), result.Data.CacheGroups.Nodes.end(), [](const auto &node) {
			return node.NodeId == "number";
		}
	);
	REQUIRE(number != result.Data.CacheGroups.Nodes.end());
	CHECK(number->OwnerId == "cache-b");
	CHECK(*number->Outputs.front().Data == Value{99.});
	const std::array<std::string, 1> members{"number"};
	REQUIRE(Refresh(result.Data.CacheGroups, "cache-a", members).Code == Status::Ok);
	number = std::find_if(
		result.Data.CacheGroups.Nodes.begin(), result.Data.CacheGroups.Nodes.end(), [](const auto &node) {
			return node.NodeId == "number";
		}
	);
	number->RenderActive = false;
	const auto before = result.Data.CacheGroups;
	request.DataReplay = &result.Data;
	request.Tick = 1;
	REQUIRE(EvaluateStateful(document, plan, "number", request, result, diagnostic) == Status::Ok);
	CHECK(result.Data.CacheGroups == before);
	CHECK(std::get<EvaluatedValue>(result.Output).Data == Value{99.});
}

TEST_CASE(
	"Authored load admits source sorting beside destination and replaces owner strings without geometric "
	"growth",
	"[imagegraph][frame_cache_groups]"
) {
	CacheGroupReplayState source;
	std::vector<CacheGroupReplayOutput> outputs;
	for (size_t index = 0; index < 1000; ++index)
		outputs.push_back({"output_" + std::to_string(index), Value{double(index)}, {}, {}});
	REQUIRE(
		RetainCacheGroupReplayNode(source, "number", "pc.number_simple", outputs, BYTE_BUDGET).Code ==
		Status::Ok
	);
	auto destination = Journal();
	const auto before = destination;
	Diagnostic diagnostic;
	const auto cap = RetainedCacheGroupReplayBytes(source) + RetainedCacheGroupReplayBytes(destination) +
					 outputs.size() * sizeof(size_t) - 1;
	CHECK(
		InitializeAuthoredCacheGroupReplay(Document{}, source, destination, cap, diagnostic) ==
		Status::LimitExceeded
	);
	CHECK(diagnostic.Message.find("source validation") != std::string::npos);
	CHECK(destination == before);
	const std::string oldId(100, 'a'), newId(101, 'b');
	Document document;
	document.Nodes = {
		{oldId, "pc.cache", "", {}, {}},
		{newId, "pc.cache_array", "", {}, {}},
		{"number", "pc.number_simple", "", {}, {}}
	};
	document.Nodes[1].SourceProperties = {{"cache_group", GroupIds({"number"})}};
	CacheGroupReplayState old;
	for (const auto &node : document.Nodes)
		REQUIRE(RetainCacheGroupReplayNode(old, node.Id, node.Type, {}, BYTE_BUDGET).Code == Status::Ok);
	const std::array<std::string, 1> members{"number"};
	REQUIRE(Refresh(old, oldId, members).Code == Status::Ok);
	const auto prior = old;
	REQUIRE(
		InitializeAuthoredCacheGroupReplay(document, old, destination, BYTE_BUDGET, diagnostic) == Status::Ok
	);
	CHECK(destination.Nodes[2].OwnerId == newId);
	CHECK(destination.Nodes[2].OwnerId.capacity() < 2 * oldId.size());
	CHECK(old == prior);
}

TEST_CASE(
	"Cold native cache group members retain named runtime refusals", "[imagegraph][cache_group][constructor]"
) {
	Document document;
	document.Nodes = {
		{"number", "value.number", "", {}, {{"value", 7.0}}}, {"owner", "pc.cache", "", {}, {}}
	};
	document.Nodes[1].SourceProperties = {{"cache_group", GroupIds({"number"})}};
	CacheGroupReplayState result;
	Diagnostic diagnostic;
	REQUIRE(InitializeAuthoredCacheGroupReplay(document, {}, result, BYTE_BUDGET, diagnostic) == Status::Ok);
	const auto member = std::find_if(result.Nodes.begin(), result.Nodes.end(), [](const auto &node) {
		return node.NodeId == "number";
	});
	REQUIRE(member != result.Nodes.end());
	REQUIRE(member->Outputs.size() == 1);
	CHECK(member->Outputs[0].Port == "number");
	CHECK_FALSE(member->Outputs[0].Data);
	REQUIRE(member->Outputs[0].Refusal);
	CHECK(member->Outputs[0].Refusal->Code == Status::UnsupportedExecution);
	CHECK(member->Outputs[0].Refusal->NodeId == "number");
	CHECK(member->Outputs[0].Refusal->Port == "number");
}

TEST_CASE(
	"Frozen cold constructors retain zero matrices colours curves and gradients without node updates",
	"[imagegraph][cache_group][constructor]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"matrix", "pc.matrix_identity", "", {}, {}},
		{"projection", "pc.matrix_projection", "", {}, {}},
		{"colour", "pc.color", "", {}, {}},
		{"curve", "pc.curve_function", "", {}, {}},
		{"gradient", "pc.gradient_out", "", {}, {}},
		{"samples", "pc.gradient_sample", "", {}, {}},
		{"owner", "pc.cache", "", {}, {}}
	};
	document.Nodes.back().SourceProperties = {
		{"cache_group", GroupIds({"matrix", "projection", "colour", "curve", "gradient", "samples"})}
	};
	document.Outputs = {
		{"matrix", "matrix", "matrix"},
		{"projection", "projection", "matrix"},
		{"colour", "colour", "color"},
		{"curve", "curve", "curve"},
		{"gradient", "gradient", "gradient"},
		{"samples", "samples", "colors"}
	};
	DataReplayState prior;
	Diagnostic diagnostic;
	REQUIRE(
		InitializeAuthoredCacheGroupReplay(document, {}, prior.CacheGroups, BYTE_BUDGET, diagnostic) ==
		Status::Ok
	);
	for (auto &node : prior.CacheGroups.Nodes)
		node.RenderActive = false;
	const auto original = prior;
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	request.DataReplay = &prior;
	for (const auto &output : document.Outputs) {
		INFO(output.Id);
		EvaluatedValue result;
		REQUIRE(EvaluateValue(document, plan, output.Id, request, result, diagnostic) == Status::Ok);
		if (output.Id == "matrix" || output.Id == "projection") {
			const auto &matrix = std::get<MatrixValue>(result.Data);
			const uint32_t size = output.Id == "matrix" ? 3 : 4;
			CHECK(matrix.Rows == size);
			CHECK(matrix.Columns == size);
			CHECK(matrix.Values == std::vector<double>(size * size, 0));
		} else if (output.Id == "colour") {
			CHECK(result.Data == Value{Colour{255, 255, 255, 255}});
		} else if (output.Id == "curve") {
			const auto &curve = std::get<Curve>(result.Data);
			CHECK(curve.Header == std::array<double, 6>{0, 1, 0, 0, 1, 0});
			REQUIRE(curve.Anchors.size() == 2);
			CHECK(curve.Anchors[0] == std::array<double, 6>{0, 0, 0, 0, 1. / 3, 1. / 3});
			CHECK(curve.Anchors[1] == std::array<double, 6>{-1. / 3, -1. / 3, 1, 1, 0, 0});
		} else if (output.Id == "gradient") {
			CHECK(result.Data == Value{Gradient{0, {{0, {255, 255, 255, 255}}}}});
		} else {
			const auto &samples = std::get<ArrayValue>(result.Data);
			REQUIRE(samples.Items.size() == 1);
			CHECK(std::get<ElementValue>(samples.Items[0].Data) == ElementValue{Colour{0, 0, 0, 255}});
		}
		CHECK(prior == original);
	}
	CacheGroupReplayState destination = prior.CacheGroups;
	CHECK(
		InitializeAuthoredCacheGroupReplay(document, {}, destination, 1, diagnostic) == Status::LimitExceeded
	);
	CHECK(destination == prior.CacheGroups);
}

TEST_CASE(
	"Frozen source path constructors preserve cold ownership and reusable point getter semantics",
	"[imagegraph][cache_group][constructor][path]"
) {
	const std::array<std::pair<std::string_view, std::string_view>, 5> paths{
		{{"pc.path_join", "joined_path"},
		 {"pc.path_array", "combined_path"},
		 {"pc.path_shift", "path"},
		 {"pc.path_weight_adjust", "path"},
		 {"pc.path_3_d", "path_data"}}
	};
	for (const auto &[type, port] : paths) {
		INFO(type);
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {{"path", std::string(type), "", {}, {}}, {"owner", "pc.cache", "", {}, {}}};
		document.Nodes.back().SourceProperties = {{"cache_group", GroupIds({"path"})}};
		document.Outputs = {{"out", "path", std::string(port)}};
		DataReplayState prior;
		Diagnostic diagnostic;
		REQUIRE(
			InitializeAuthoredCacheGroupReplay(document, {}, prior.CacheGroups, BYTE_BUDGET, diagnostic) ==
			Status::Ok
		);
		for (auto &node : prior.CacheGroups.Nodes)
			node.RenderActive = false;
		const auto original = prior;
		Plan plan;
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		EvaluationRequest request;
		request.DataReplay = &prior;
		EvaluatedValue output;
		REQUIRE(EvaluateValue(document, plan, "out", request, output, diagnostic) == Status::Ok);
		CHECK(prior == original);
		const auto *entry = FindCatalogueEntry(type);
		REQUIRE(entry);
		detail::NodeContext context(document.Nodes.front(), *entry, request);
		if (type == "pc.path_3_d") {
			const auto &spatial = std::get<PathValue3D>(output.Data);
			REQUIRE(spatial.Data);
			CHECK(spatial.Data->SourcePresent);
			CHECK_FALSE(spatial.Data->Loop);
			CHECK(spatial.Data->Anchors.empty());
			detail::PathRuntime3D runtime(*spatial.Data, &context);
			CHECK(runtime.LineCount() == 1);
			CHECK(runtime.SourceSegmentCount() == 0);
			CHECK(runtime.Length() == 0);
			CHECK(runtime.SourceBoundary() == std::optional<Vector4>{{-4, -4, -4, -4}});
			CHECK(runtime.Ratio(.5).Position == Vector3{});
		} else {
			const auto &path = std::get<Path2D>(output.Data);
			REQUIRE(path.SourceOperation);
			CHECK(path.SourceOperation->Inputs.empty());
			CHECK(path.SourceOperation->EvaluationMemoId == 0);
			if (type == "pc.path_weight_adjust") {
				CHECK(path.SourceOperation->WeightCurve.empty());
				for (const bool partialCurve : {false, true}) {
					auto malformed = prior.CacheGroups;
					auto member =
						std::find_if(malformed.Nodes.begin(), malformed.Nodes.end(), [](const auto &node) {
							return node.NodeId == "path";
						});
					REQUIRE(member != malformed.Nodes.end());
					auto &operation = std::get<Path2D>(*member->Outputs.front().Data).SourceOperation;
					if (partialCurve)
						operation->WeightCurve = {0};
					else
						operation->WeightValue = 1;
					CHECK(
						ValidateCacheGroupReplay(malformed, BYTE_BUDGET, diagnostic) == Status::InvalidValue
					);
				}
			}
			detail::PathRuntime runtime;
			REQUIRE(runtime.Init(context, path));
			const bool compound = type == "pc.path_join" || type == "pc.path_array";
			CHECK(runtime.LineCount() == (compound ? 0 : 1));
			CHECK(runtime.Length() == 0);
			CHECK(runtime.AccumulatedCount() == 0);
			CHECK(
				Vector4{runtime.MinX, runtime.MinY, runtime.MaxX, runtime.MaxY} ==
				(compound ? Vector4{-4, -4, -4, -4} : Vector4{0, 0, 1, 1})
			);
			const SourcePathPointBuffer initial{SourcePathPointClass::Spatial, {7, 9}, 11, .4};
			for (const bool distance : {false, true}) {
				auto supplied = initial;
				const auto sampled = distance ? runtime.PointDistanceInto(.5, 0, supplied)
											  : runtime.PointRatioInto(.5, 0, supplied);
				if (type == "pc.path_join") {
					CHECK(sampled == initial);
					CHECK(supplied == initial);
				} else if (type == "pc.path_array") {
					CHECK(sampled == SourcePathPointBuffer{});
					CHECK(supplied == initial);
				} else {
					auto expected = initial;
					expected.Position = {};
					CHECK(sampled == expected);
					CHECK(supplied == expected);
				}
			}
		}
		CacheGroupReplayState destination = prior.CacheGroups;
		CHECK(
			InitializeAuthoredCacheGroupReplay(document, {}, destination, 1, diagnostic) ==
			Status::LimitExceeded
		);
		CHECK(destination == prior.CacheGroups);
	}
}

TEST_CASE(
	"Project globals execute despite retained cache group activity flags",
	"[imagegraph][frame_cache_groups][pcx]"
) {
	Document document;
	document.FormatVersion = 9;
	Node globals{"globals", "pc.global_scope", "", {}, {}};
	globals.DynamicInputs = {{"answer", ValueType::Scalar, Value{4.0}}};
	document.Nodes = {globals, {"consumer", "pc.equation", "", {}, {{"equation", std::string{"answer+1"}}}}};
	document.ProjectGlobalNodeId = "globals";
	document.Timeline = TimelineSettings{2, 0, 1, "loop", 24};
	document.Keyframes = {{"globals", "answer", 0, 4.0, "step"}, {"globals", "answer", 1, 8.0, "step"}};
	document.Outputs = {{"out", "consumer", "result"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	auto prior = FrozenNode("globals", "pc.global_scope", {});
	const auto original = prior;
	CHECK(CacheGroupReplayShouldRun(prior.CacheGroups, "globals"));
	EvaluationRequest request;
	request.DataReplay = &prior;
	for (const uint64_t tick : {0, 1}) {
		request.Tick = tick;
		EvaluatedValue value;
		const auto status = EvaluateValue(document, plan, "out", request, value, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		CHECK(std::get<double>(value.Data) == (tick ? 9.0 : 5.0));
		StatefulEvaluationResult result;
		REQUIRE(EvaluateStateful(document, plan, "out", request, result, diagnostic) == Status::Ok);
		CHECK(std::get<double>(std::get<EvaluatedValue>(result.Output).Data) == (tick ? 9.0 : 5.0));
		CHECK_FALSE(result.Data.CacheGroups.Nodes.front().RenderActive);
	}
	CHECK(prior == original);
}

TEST_CASE(
	"Loaded owner refresh keeps unselected interactive lists and frozen getters",
	"[imagegraph][cache_group][load]"
) {
	auto document = AuthoredGroups();
	CacheGroupReplayState state;
	Diagnostic error;
	REQUIRE(InitializeAuthoredCacheGroupReplay(document, {}, state, BYTE_BUDGET, error) == Status::Ok);
	REQUIRE(
		ApplyCacheGroupReplay(
			state, {CacheGroupReplayAction::TransferMember, "cache-a", "number"}, BYTE_BUDGET
		)
			.Code == Status::Ok
	);
	REQUIRE(ApplyCacheGroupReplay(state, Disable(), BYTE_BUDGET).Code == Status::Ok);
	const auto old = state;
	auto checkpoint = state;
	document.Nodes[1].SourceProperties[0].Data = GroupIds({"path"});
	document.Nodes.push_back({"loaded", "pc.cache_array", "", {}, {}});
	document.Nodes.back().SourceProperties = {
		{"cache_group", GroupIds({"number", "new-number", "missing", "number"})}, {"serialize", false}
	};
	document.Nodes.push_back({"new-number", "pc.number_simple", "", {}, {{"value", 99.0}}});
	const std::array<std::string_view, 1> owners{"loaded"};
	const std::array journals{&state, &checkpoint};
	REQUIRE(RefreshLoadedCacheGroupReplay(document, owners, journals, BYTE_BUDGET, error) == Status::Ok);
	CHECK(state == checkpoint);
	CHECK(state.Owners[0] == old.Owners[0]);
	CHECK(state.Owners[1] == old.Owners[1]);
	CHECK_FALSE(state.Owners.back().Serialize);
	CHECK(state.Owners.back().Members == std::vector<std::string>{"number", "new-number", "number"});
	const auto number = std::find_if(state.Nodes.begin(), state.Nodes.end(), [](const auto &node) {
		return node.NodeId == "number";
	});
	const auto priorNumber = std::find_if(old.Nodes.begin(), old.Nodes.end(), [](const auto &node) {
		return node.NodeId == "number";
	});
	REQUIRE(number != state.Nodes.end());
	CHECK(number->OwnerId == "loaded");
	CHECK(number->Outputs == priorNumber->Outputs);
	CHECK(number->RenderActive == priorNumber->RenderActive);
	const auto cold = std::find_if(state.Nodes.begin(), state.Nodes.end(), [](const auto &node) {
		return node.NodeId == "new-number";
	});
	REQUIRE(cold != state.Nodes.end());
	REQUIRE(cold->Outputs.front().Data);
	CHECK(std::get<double>(*cold->Outputs.front().Data) == 0.0);
}

TEST_CASE(
	"Loaded cache owner callback order and two-journal refusals are atomic", "[imagegraph][cache_group][load]"
) {
	auto document = AuthoredGroups();
	document.Nodes.push_back({"loaded-a", "pc.cache", "", {}, {}});
	document.Nodes.back().SourceProperties = {{"cache_group", GroupIds({"number"})}};
	document.Nodes.push_back({"loaded-b", "pc.cache_array", "", {}, {}});
	document.Nodes.back().SourceProperties = {{"cache_group", GroupIds({"number"})}};
	CacheGroupReplayState state, checkpoint;
	Diagnostic error;
	const std::array<std::string_view, 2> owners{"loaded-b", "loaded-a"};
	const std::array journals{&state, &checkpoint};
	REQUIRE(RefreshLoadedCacheGroupReplay(document, owners, journals, BYTE_BUDGET, error) == Status::Ok);
	CHECK(state == checkpoint);
	CHECK(state.Owners[0].NodeId == "loaded-b");
	CHECK(state.Owners[1].NodeId == "loaded-a");
	CHECK(std::find_if(state.Nodes.begin(), state.Nodes.end(), [](const auto &node) {
			  return node.NodeId == "number";
		  })->OwnerId == "loaded-a");
	std::find_if(checkpoint.Nodes.begin(), checkpoint.Nodes.end(), [](const auto &node) {
		return node.NodeId == "number";
	})->NodeType = "pc.number";
	const auto old = state, start = checkpoint;
	CHECK(
		RefreshLoadedCacheGroupReplay(document, owners, journals, BYTE_BUDGET, error) == Status::InvalidValue
	);
	CHECK(state == old);
	CHECK(checkpoint == start);
	CHECK(RefreshLoadedCacheGroupReplay(document, owners, journals, 1, error) == Status::LimitExceeded);
	CHECK(state == old);
	CHECK(checkpoint == start);
	const std::array<std::string_view, 2> duplicate{"loaded-a", "loaded-a"};
	CHECK(
		RefreshLoadedCacheGroupReplay(document, duplicate, journals, BYTE_BUDGET, error) ==
		Status::DuplicateId
	);
	const std::array<std::string_view, 1> absent{"absent"};
	CHECK(
		RefreshLoadedCacheGroupReplay(document, absent, journals, BYTE_BUDGET, error) == Status::UnknownNode
	);
	const std::array alias{&state, &state};
	CHECK(RefreshLoadedCacheGroupReplay(document, owners, alias, BYTE_BUDGET, error) == Status::InvalidValue);
	CHECK(state == old);
	CHECK(checkpoint == start);
}

TEST_CASE("Loaded cache journals share one comparison work cap", "[imagegraph][cache_group][load][bounds]") {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {{"cache", "pc.cache", "", {}, {}}};
	const auto id = std::string(20000, 'n');
	document.Nodes.push_back({id, "pc.number_simple", "", {}, {}});
	for (size_t i = 2; i < 64; ++i)
		document.Nodes.push_back({"other-" + std::to_string(i), "pc.number_simple", "", {}, {}});
	document.Nodes[0].SourceProperties = {{"cache_group", GroupIds({id})}};
	CacheGroupReplayState single, state, checkpoint;
	Diagnostic error;
	REQUIRE(InitializeAuthoredCacheGroupReplay(document, {}, single, BYTE_BUDGET, error) == Status::Ok);
	const std::array<std::string_view, 1> owners{"cache"};
	const std::array journals{&state, &checkpoint};
	CHECK(
		RefreshLoadedCacheGroupReplay(document, owners, journals, BYTE_BUDGET, error) == Status::LimitExceeded
	);
	CHECK(state == CacheGroupReplayState{});
	CHECK(checkpoint == CacheGroupReplayState{});
}

TEST_CASE(
	"Authored membership clicks transfer and remove without enabling whole groups",
	"[imagegraph][cache_group][membership]"
) {
	auto document = AuthoredGroups();
	CacheGroupReplayState state;
	Diagnostic error;
	REQUIRE(InitializeAuthoredCacheGroupReplay(document, {}, state, BYTE_BUDGET, error) == Status::Ok);
	REQUIRE(ApplyCacheGroupReplay(state, Disable(), BYTE_BUDGET).Code == Status::Ok);
	auto checkpoint = state;
	const auto before = state;
	const std::array journals{&state, &checkpoint};
	REQUIRE(
		ToggleAuthoredCacheGroupMember(document, journals, "cache-b", "path", BYTE_BUDGET, error) ==
		Status::Ok
	);
	CHECK(state == checkpoint);
	CHECK(
		std::get<ArrayValue>(document.Nodes[0].SourceProperties[0].Data) ==
		GroupIds({"number", "missing", "number"})
	);
	CHECK(std::get<ArrayValue>(document.Nodes[1].SourceProperties[0].Data) == GroupIds({"number", "path"}));
	const auto find = [&](std::string_view id) -> const CacheGroupReplayNode & {
		const auto found = std::find_if(state.Nodes.begin(), state.Nodes.end(), [&](const auto &node) {
			return node.NodeId == id;
		});
		REQUIRE(found != state.Nodes.end());
		return *found;
	};
	CHECK(find("path").OwnerId == "cache-b");
	CHECK(find("path").RenderActive);
	CHECK_FALSE(find("number").RenderActive);
	CHECK(
		find("path").Outputs == std::find_if(before.Nodes.begin(), before.Nodes.end(), [](const auto &node) {
									return node.NodeId == "path";
								})->Outputs
	);
	CHECK_FALSE(state.Owners[1].Serialize);
	REQUIRE(
		ToggleAuthoredCacheGroupMember(document, journals, "cache-b", "path", BYTE_BUDGET, error) ==
		Status::Ok
	);
	CHECK(find("path").OwnerId.empty());
	CHECK(find("path").RenderActive);
	CHECK_FALSE(find("number").RenderActive);
	CHECK(state == checkpoint);
	REQUIRE(
		ToggleAuthoredCacheGroupMember(document, journals, "cache-b", "unrelated", BYTE_BUDGET, error) ==
		Status::Ok
	);
	CHECK(find("unrelated").OwnerId == "cache-b");
	CHECK(find("unrelated").RenderActive);
	REQUIRE_FALSE(find("unrelated").Outputs.empty());
	REQUIRE(find("unrelated").Outputs.front().Data);
	CHECK(std::get<std::string>(*find("unrelated").Outputs.front().Data).empty());
	Document roundtrip;
	REQUIRE(Read(Write(document), roundtrip, error) == Status::Ok);
	CHECK(roundtrip == document);
}

TEST_CASE(
	"Loaded overlap membership clicks refresh ownership even when removal does nothing",
	"[imagegraph][cache_group][membership]"
) {
	auto document = AuthoredGroups();
	CacheGroupReplayState state;
	Diagnostic error;
	REQUIRE(InitializeAuthoredCacheGroupReplay(document, {}, state, BYTE_BUDGET, error) == Status::Ok);
	REQUIRE(ApplyCacheGroupReplay(state, Disable(), BYTE_BUDGET).Code == Status::Ok);
	auto checkpoint = state;
	const auto oldDocument = document;
	const auto old = state;
	const std::array journals{&state, &checkpoint};
	REQUIRE(
		ToggleAuthoredCacheGroupMember(document, journals, "cache-a", "number", BYTE_BUDGET, error) ==
		Status::Ok
	);
	CHECK(document == oldDocument);
	CHECK(state.Owners == old.Owners);
	const auto number = std::find_if(state.Nodes.begin(), state.Nodes.end(), [](const auto &node) {
		return node.NodeId == "number";
	});
	REQUIRE(number != state.Nodes.end());
	CHECK(number->OwnerId == "cache-a");
	CHECK_FALSE(number->RenderActive);
	CHECK(state == checkpoint);
	REQUIRE(
		ToggleAuthoredCacheGroupMember(document, journals, "cache-a", "number", BYTE_BUDGET, error) ==
		Status::Ok
	);
	CHECK(std::get<ArrayValue>(document.Nodes[0].SourceProperties[0].Data) == GroupIds({"missing", "path"}));
	CHECK(state.Owners[1] == old.Owners[1]);
	const auto removed = std::find_if(state.Nodes.begin(), state.Nodes.end(), [](const auto &node) {
		return node.NodeId == "number";
	});
	REQUIRE(removed != state.Nodes.end());
	CHECK(removed->OwnerId.empty());
	CHECK(removed->RenderActive);
	CHECK(state == checkpoint);
}

TEST_CASE(
	"Authored membership refusal keeps document and both journals unchanged",
	"[imagegraph][cache_group][membership][bounds]"
) {
	auto document = AuthoredGroups();
	CacheGroupReplayState state;
	Diagnostic error;
	REQUIRE(InitializeAuthoredCacheGroupReplay(document, {}, state, BYTE_BUDGET, error) == Status::Ok);
	auto checkpoint = state;
	const auto oldDocument = document;
	const auto old = state;
	const std::array journals{&state, &checkpoint};
	CHECK(
		ToggleAuthoredCacheGroupMember(document, journals, "cache-b", "path", 1, error) ==
		Status::LimitExceeded
	);
	CHECK(
		ToggleAuthoredCacheGroupMember(document, journals, "cache-a", "cache-a", BYTE_BUDGET, error) ==
		Status::InvalidValue
	);
	CHECK(
		ToggleAuthoredCacheGroupMember(document, journals, "cache-a", "absent", BYTE_BUDGET, error) ==
		Status::UnknownNode
	);
	CHECK(document == oldDocument);
	CHECK(state == old);
	CHECK(checkpoint == old);
	const auto number = std::find_if(checkpoint.Nodes.begin(), checkpoint.Nodes.end(), [](const auto &node) {
		return node.NodeId == "number";
	});
	number->OwnerId = "cache-a";
	const auto different = checkpoint;
	CHECK(
		ToggleAuthoredCacheGroupMember(document, journals, "cache-b", "number", BYTE_BUDGET, error) ==
		Status::InvalidValue
	);
	CHECK(document == oldDocument);
	CHECK(state == old);
	CHECK(checkpoint == different);

	Document longDocument;
	longDocument.FormatVersion = 9;
	longDocument.Nodes = {{"cache", "pc.cache", "", {}, {}}};
	const auto id = std::string(20000, 'n');
	longDocument.Nodes.push_back({id, "pc.number_simple", "", {}, {}});
	for (size_t i = 2; i < 64; ++i)
		longDocument.Nodes.push_back({"other-" + std::to_string(i), "pc.number_simple", "", {}, {}});
	longDocument.Nodes[0].SourceProperties = {{"cache_group", GroupIds({id})}};
	CacheGroupReplayState loaded;
	REQUIRE(InitializeAuthoredCacheGroupReplay(longDocument, {}, loaded, BYTE_BUDGET, error) == Status::Ok);
	auto single = loaded;
	auto singleDocument = longDocument;
	const std::array one{&single};
	REQUIRE(
		ToggleAuthoredCacheGroupMember(singleDocument, one, "cache", id, BYTE_BUDGET, error) == Status::Ok
	);
	state = checkpoint = loaded;
	const std::array two{&state, &checkpoint};
	CHECK(
		ToggleAuthoredCacheGroupMember(longDocument, two, "cache", id, BYTE_BUDGET, error) ==
		Status::LimitExceeded
	);
	CHECK(longDocument.Nodes[0].SourceProperties[0].Data == Value{GroupIds({id})});
	CHECK(state == loaded);
	CHECK(checkpoint == loaded);
	CacheGroupReplayState oversized;
	const std::array invalid{&oversized};
	const std::array<std::string_view, 1> selected{"cache-a"};
	for (unsigned shape = 0; shape < 3; ++shape) {
		oversized = {};
		if (shape == 0) oversized.Nodes.resize(Limits::MaximumNodes + 1);
		if (shape == 1) {
			oversized.Nodes.resize(1);
			oversized.Nodes.front().Outputs.resize(
				Limits::MaximumDynamicOutputsPerNode + Limits::MaximumGroupPorts + 1
			);
		}
		if (shape == 2) {
			oversized.Owners.resize(1);
			oversized.Owners.front().Members.resize(Limits::MaximumLinks + 1);
		}
		const auto prior = oversized;
		CHECK(
			ToggleAuthoredCacheGroupMember(document, invalid, "cache-a", "path", BYTE_BUDGET, error) ==
			Status::LimitExceeded
		);
		CHECK(
			RefreshLoadedCacheGroupReplay(document, selected, invalid, BYTE_BUDGET, error) ==
			Status::LimitExceeded
		);
		CHECK(oversized == prior);
		CHECK(document == oldDocument);
	}
}

TEST_CASE(
	"Membership adds preserve unowned activity and keep nested cache membership direct",
	"[imagegraph][cache_group][membership]"
) {
	auto document = AuthoredGroups();
	CacheGroupReplayState state;
	Diagnostic error;
	REQUIRE(InitializeAuthoredCacheGroupReplay(document, {}, state, BYTE_BUDGET, error) == Status::Ok);
	REQUIRE(ApplyCacheGroupReplay(state, Disable(), BYTE_BUDGET).Code == Status::Ok);
	const auto pathBefore = *std::find_if(state.Nodes.begin(), state.Nodes.end(), [](const auto &node) {
		return node.NodeId == "path";
	});
	auto checkpoint = state;
	const std::array journals{&state, &checkpoint};
	REQUIRE(
		ToggleAuthoredCacheGroupMember(document, journals, "cache-b", "cache-a", BYTE_BUDGET, error) ==
		Status::Ok
	);
	CHECK(*std::find_if(state.Nodes.begin(), state.Nodes.end(), [](const auto &node) {
		return node.NodeId == "path";
	}) == pathBefore);
	CHECK(std::find_if(state.Nodes.begin(), state.Nodes.end(), [](const auto &node) {
			  return node.NodeId == "cache-a";
		  })->OwnerId == "cache-b");
	CHECK(state == checkpoint);

	Document cold;
	cold.FormatVersion = 9;
	cold.Nodes = {{"cache", "pc.cache", "", {}, {}}, {"text", "pc.string", "", {}, {}}};
	CacheGroupReplayState orphan;
	const std::array<CacheGroupReplayOutput, 1> outputs{{{"text", Value{std::string{"held"}}, {}, {}}}};
	REQUIRE(RetainCacheGroupReplayNode(orphan, "text", "pc.string", outputs, BYTE_BUDGET).Code == Status::Ok);
	orphan.Nodes.front().RenderActive = false;
	auto start = orphan;
	const std::array coldJournals{&orphan, &start};
	REQUIRE(
		ToggleAuthoredCacheGroupMember(cold, coldJournals, "cache", "text", BYTE_BUDGET, error) == Status::Ok
	);
	CHECK(orphan == start);
	CHECK_FALSE(orphan.Nodes.front().RenderActive);
	CHECK(orphan.Nodes.front().Outputs == std::vector<CacheGroupReplayOutput>{outputs.front()});
	CHECK(orphan.Nodes.front().OwnerId == "cache");
	CHECK(std::get<ArrayValue>(cold.Nodes.front().SourceProperties.front().Data) == GroupIds({"text"}));
}

TEST_CASE(
	"Prepared membership owns candidates without publishing borrowed source journals",
	"[imagegraph][cache_group][membership][prepare]"
) {
	auto document = AuthoredGroups();
	CacheGroupReplayState state;
	Diagnostic error;
	REQUIRE(InitializeAuthoredCacheGroupReplay(document, {}, state, BYTE_BUDGET, error) == Status::Ok);
	REQUIRE(ApplyCacheGroupReplay(state, Disable(), BYTE_BUDGET).Code == Status::Ok);
	auto checkpoint = state;
	const auto originalDocument = document;
	const auto originalState = state;
	const std::array<const CacheGroupReplayState *, 2> sources{&state, &checkpoint};
	auto prepared = PrepareAuthoredCacheGroupMember(document, sources, "cache-b", "path", BYTE_BUDGET, error);
	REQUIRE(prepared);
	CHECK(error.Code == Status::Ok);
	CHECK(prepared->JournalCount == 2);
	CHECK(document == originalDocument);
	CHECK(state == originalState);
	CHECK(checkpoint == originalState);
	const std::array journals{&state, &checkpoint};
	REQUIRE(
		ToggleAuthoredCacheGroupMember(document, journals, "cache-b", "path", BYTE_BUDGET, error) ==
		Status::Ok
	);
	CHECK(document == prepared->Authored);
	CHECK(state == prepared->Journals[0]);
	CHECK(checkpoint == prepared->Journals[1]);
	document = {};
	state = {};
	checkpoint = {};
	CHECK(prepared->Authored != document);
	CHECK_FALSE(prepared->Journals[0].Nodes.empty());
	CHECK(prepared->Journals[0] == prepared->Journals[1]);
}
