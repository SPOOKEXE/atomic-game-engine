#include <engine/imagegraph/RandomReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.imagegraph.random_replay")
using namespace engine::imagegraph;
TEST_CASE(
	"Random replay validates processor identity finite state and retained byte limits",
	"[imagegraph][random_replay]"
) {
	RandomReplayState state;
	RandomReplayEntry entry;
	entry.NodeId = "random";
	entry.Initialized = true;
	entry.Kernel = {.1, .2, .3};
	state.Entries = {entry};
	Diagnostic diagnostic;
	CHECK(ValidateRandomReplay(state, Limits::MaximumEvaluationBytes, diagnostic) == Status::Ok);
	CHECK(RetainedRandomReplayBytes(state) >= sizeof(state) + sizeof(entry) + 3 * sizeof(double));
	CHECK(ValidateRandomReplay(state, 1, diagnostic) == Status::LimitExceeded);
	auto copy = state;
	copy.Entries[0].Kernel[0] = 4;
	CHECK(state.Entries[0].Kernel[0] == .1);
	state.Entries.push_back(entry);
	CHECK(ValidateRandomReplay(state, Limits::MaximumEvaluationBytes, diagnostic) == Status::DuplicateId);
	state.Entries.back().ProcessorRow = 1;
	CHECK(ValidateRandomReplay(state, Limits::MaximumEvaluationBytes, diagnostic) == Status::Ok);
	state.Entries.back().MovingAverage = std::numeric_limits<double>::infinity();
	CHECK(ValidateRandomReplay(state, Limits::MaximumEvaluationBytes, diagnostic) == Status::InvalidValue);
}
