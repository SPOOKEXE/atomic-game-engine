#include <engine/imagegraph/DataReplay.hpp>
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
