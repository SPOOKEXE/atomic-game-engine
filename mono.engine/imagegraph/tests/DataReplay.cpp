#include <engine/imagegraph/DataReplay.hpp>
#include <engine/imagegraph/FrameCacheReplay.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>
TEST_SUITE_ID("engine.imagegraph.data_replay")
using namespace engine::imagegraph;
TEST_CASE(
	"Data replay owns rows and enforces finite identities and byte bounds", "[imagegraph][data_replay]"
) {
	DataReplayEntry row;
	row.NodeId = "differential";
	row.Initialized = true;
	row.PreviousFrame = -.5;
	row.PreviousValue = 3;
	DataReplayState state{{row}};
	Diagnostic diagnostic;
	CHECK(ValidateDataReplay(state, Limits::MaximumEvaluationBytes, diagnostic) == Status::Ok);
	CHECK(RetainedDataReplayBytes(state) >= sizeof(state) + sizeof(row) + row.NodeId.size());
	CHECK(ValidateDataReplay(state, 1, diagnostic) == Status::LimitExceeded);
	auto copy = state;
	copy.Entries[0].PreviousValue = 9;
	CHECK(state.Entries[0].PreviousValue == 3);
	state.Entries.push_back(row);
	CHECK(ValidateDataReplay(state, Limits::MaximumEvaluationBytes, diagnostic) == Status::DuplicateId);
	state.Entries.back().ProcessorRow = 1;
	CHECK(ValidateDataReplay(state, Limits::MaximumEvaluationBytes, diagnostic) == Status::Ok);
	state.Entries.back().PreviousFrame = std::numeric_limits<double>::infinity();
	CHECK(ValidateDataReplay(state, Limits::MaximumEvaluationBytes, diagnostic) == Status::InvalidValue);
}
TEST_CASE(
	"Data replay owns cache-group payloads in its aggregate validation and byte cap",
	"[imagegraph][data_replay][frame_cache_groups]"
) {
	DataReplayState state;
	std::vector<CacheGroupReplayOutput> outputs{
		{"surface", Value{SurfaceValue{Image{1, 1, {1, 2, 3, 255}}}}, {}, {}}
	};
	REQUIRE(
		RetainCacheGroupReplayNode(
			state.CacheGroups, "producer", "image.solid", outputs, Limits::MaximumEvaluationBytes
		)
			.Code == Status::Ok
	);
	const auto bytes = RetainedDataReplayBytes(state);
	CHECK(
		bytes == sizeof(state) + RetainedCacheGroupReplayBytes(state.CacheGroups) - sizeof(state.CacheGroups)
	);
	Diagnostic diagnostic;
	CHECK(ValidateDataReplay(state, bytes + sizeof(size_t), diagnostic) == Status::Ok);
	CHECK(ValidateDataReplay(state, bytes + sizeof(size_t) - 1, diagnostic) == Status::LimitExceeded);
	DataReplayEntry row;
	row.NodeId = "differential";
	row.Initialized = true;
	state.Entries.push_back(row);
	const auto withRows = RetainedDataReplayBytes(state);
	CHECK(ValidateDataReplay(state, withRows + sizeof(size_t) - 1, diagnostic) == Status::LimitExceeded);
	CHECK(ValidateDataReplay(state, withRows + sizeof(size_t), diagnostic) == Status::Ok);
	auto copy = state;
	std::get<SurfaceValue>(*copy.CacheGroups.Nodes.front().Outputs.front().Data).Data.Pixels[0] = 99;
	CHECK(std::get<SurfaceValue>(*state.CacheGroups.Nodes.front().Outputs.front().Data).Data.Pixels[0] == 1);
	state.CacheGroups.Nodes.front().Outputs.front().Domain =
		SourceSocketDomain{static_cast<ValueType>(255), {}, {}};
	CHECK(ValidateDataReplay(state, Limits::MaximumEvaluationBytes, diagnostic) == Status::InvalidValue);
}
TEST_CASE(
	"Stateful evaluator transports active producer snapshots and retires type-replaced records",
	"[imagegraph][data_replay][frame_cache_groups]"
) {
	Document document;
	document.Nodes = {
		{"producer",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{1, 2, 3, 255}}}}
	};
	document.Outputs = {{"out", "producer", "image"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	DataReplayState prior;
	std::vector<CacheGroupReplayOutput> outputs{
		{"image", Value{SurfaceValue{Image{1, 1, {1, 2, 3, 255}}}}, {}, {}}
	};
	REQUIRE(
		RetainCacheGroupReplayNode(
			prior.CacheGroups, "producer", "image.solid", outputs, Limits::MaximumEvaluationBytes
		)
			.Code == Status::Ok
	);
	EvaluationRequest request;
	request.DataReplay = &prior;
	StatefulEvaluationResult result;
	REQUIRE(EvaluateStateful(document, plan, "out", request, result, diagnostic) == Status::Ok);
	REQUIRE(result.Data.CacheGroups.Nodes.size() == 1);
	CHECK(result.Data.CacheGroups.Nodes.front().RenderActive);
	CHECK(result.Data.CacheGroups.Nodes.front().OwnerId.empty());
	CHECK(
		std::get<SurfaceValue>(*result.Data.CacheGroups.Nodes.front().Outputs.front().Data).Data ==
		std::get<Image>(result.Output)
	);
	CHECK(std::get<Image>(result.Output).Pixels == std::vector<uint8_t>{1, 2, 3, 255});
	request.DataReplay = &result.Data;
	request.Tick = 1;
	REQUIRE(EvaluateStateful(document, plan, "out", request, result, diagnostic) == Status::Ok);
	REQUIRE(result.Data.CacheGroups.Nodes.size() == 1);
	CHECK(result.Data.CacheGroups.Nodes.front().RenderActive);
	CHECK(result.Data.CacheGroups.Nodes.front().OwnerId.empty());
	CHECK(
		std::get<SurfaceValue>(*result.Data.CacheGroups.Nodes.front().Outputs.front().Data).Data ==
		std::get<Image>(result.Output)
	);
	prior.CacheGroups.Nodes.front().NodeType = "pc.replaced";
	request.DataReplay = &prior;
	REQUIRE(EvaluateStateful(document, plan, "out", request, result, diagnostic) == Status::Ok);
	CHECK(result.Data.CacheGroups.Nodes.empty());
	prior.CacheGroups.Nodes.front().NodeType = "image.solid";
	prior.CacheGroups.Nodes.front().RenderActive = false;
	REQUIRE(EvaluateStateful(document, plan, "out", request, result, diagnostic) == Status::Ok);
	CHECK(result.Data.CacheGroups == prior.CacheGroups);
	CHECK(std::get<Image>(result.Output).Pixels == std::vector<uint8_t>{1, 2, 3, 255});
	Image directOutput;
	REQUIRE(Evaluate(document, plan, "out", request, directOutput, diagnostic) == Status::Ok);
	CHECK(directOutput == std::get<Image>(result.Output));
}
TEST_CASE(
	"Frame-row clear and overlay retain the target observation's separate group journal",
	"[imagegraph][data_replay][frame_cache_groups]"
) {
	Document document;
	document.Nodes = {{"cache", "pc.cache", "", {}, {}}, {"producer", "image.solid", "", {}, {}}};
	DataReplayState source;
	DataReplayEntry row;
	row.NodeId = "cache";
	row.Initialized = true;
	row.Values = {
		{0, std::string("pc.cache")}, {1, int64_t{-4}}, {2, SurfaceValue{Image{1, 1, {1, 2, 3, 255}}}}
	};
	source.Entries.push_back(row);
	std::vector<CacheGroupReplayOutput> outputs{
		{"image", Value{SurfaceValue{Image{1, 1, {1, 2, 3, 255}}}}, {}, {}}
	};
	REQUIRE(
		RetainCacheGroupReplayNode(
			source.CacheGroups, "producer", "image.solid", outputs, Limits::MaximumEvaluationBytes
		)
			.Code == Status::Ok
	);
	DataReplayState cleared;
	Diagnostic diagnostic;
	REQUIRE(
		ClearSourceFrameCacheReplay(
			document.Nodes.front(), source, cleared, diagnostic, Limits::MaximumEvaluationBytes
		) == Status::Ok
	);
	CHECK(cleared.CacheGroups == source.CacheGroups);
	CHECK(cleared.Entries.front().Values.size() == 2);
	std::get<SurfaceValue>(*cleared.CacheGroups.Nodes.front().Outputs.front().Data).Data.Pixels[0] = 77;
	const auto earlier = cleared.CacheGroups;
	REQUIRE(
		OverlaySourceFrameCacheRows(
			document,
			source,
			cleared,
			FrameCacheOutputPolicy::RetainedObservation,
			diagnostic,
			Limits::MaximumEvaluationBytes
		) == Status::Ok
	);
	CHECK(cleared.CacheGroups == earlier);
	CHECK(cleared.Entries.front().Values.size() == 3);
	const auto before = cleared;
	CHECK(
		OverlaySourceFrameCacheRows(
			document, source, cleared, FrameCacheOutputPolicy::RetainedObservation, diagnostic, 1
		) == Status::LimitExceeded
	);
	CHECK(cleared == before);
	CHECK(std::get<SurfaceValue>(*source.CacheGroups.Nodes.front().Outputs.front().Data).Data.Pixels[0] == 1);
}

TEST_CASE("Replay validation charges the largest sequential sorting workspace", "[imagegraph][data_replay]") {
	DataReplayState state;
	CHECK(DataReplayValidationWorkspaceBytes(state) == 0);
	CHECK(DataReplayValidationWorkspaceBytes(state, 1) == sizeof(size_t));
	state.Entries.resize(3);
	CacheGroupReplayNode node;
	node.Outputs.resize(17);
	state.CacheGroups.Nodes.push_back(node);
	CHECK(DataReplayValidationWorkspaceBytes(state, 1) == 17 * sizeof(size_t));
	CHECK(DataReplayValidationWorkspaceBytes(state, 20) == 23 * sizeof(size_t));
	CHECK(DataReplayValidationWorkspaceBytes(state, SIZE_MAX) == UINT64_MAX);
	state.CacheGroups.Nodes.front().Outputs.resize(
		Limits::MaximumDynamicOutputsPerNode + Limits::MaximumGroupPorts + 1
	);
	CHECK(DataReplayValidationWorkspaceBytes(state) == UINT64_MAX);
	state.CacheGroups.Nodes.clear();
	state.Entries.resize(Limits::MaximumArrayElements + 1);
	CHECK(DataReplayValidationWorkspaceBytes(state) == UINT64_MAX);
}
