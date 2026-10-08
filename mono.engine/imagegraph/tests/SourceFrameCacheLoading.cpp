#include <engine/imagegraph/FrameCacheReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>
TEST_SUITE_ID("engine.imagegraph.source_frame_cache_loading")
using namespace engine::imagegraph;
namespace {
	Node Owner(bool array = false) {
		Node node;
		node.Id = "cache";
		node.Type = array ? "pc.cache_array" : "pc.cache";
		node.SourceProperties = {{"cache", std::string("identity")}};
		return node;
	}
	DataReplayEntry Inventory(const Node &node, uint64_t count = 4) {
		DataReplayEntry row;
		row.NodeId = node.Id;
		row.Initialized = true;
		row.LoadedCacheData = "identity";
		row.SourceFrameCacheSerializedSlots = count;
		row.Values = {{0, std::string(node.Type)}, {1, int64_t{999}}, {2, int64_t{10}}, {4, int64_t{30}}};
		return row;
	}
	DataReplayEntry Begin(const Node &node, const DataReplayEntry &row) {
		Diagnostic error;
		DataReplayEntry output;
		REQUIRE(BeginSourceFrameCacheLoading(node, row, output, error) == Status::Ok);
		return output;
	}
}
TEST_CASE(
	"source loader publishes exactly one indexed slot and retains trailing inventory", "[frame_cache_loading]"
) {
	const auto node = Owner();
	const auto inventory = Inventory(node);
	auto row = Begin(node, inventory);
	REQUIRE(row.Values.size() == 2);
	REQUIRE(std::get<int64_t>(row.Values[1].Data) == -4);
	REQUIRE(row.SourceFrameCacheLoading->PendingSlots.size() == 2);
	Diagnostic error;
	bool completed = true;
	REQUIRE(StepSourceFrameCacheLoading(node, 2, row, row, completed, error) == Status::Ok);
	REQUIRE_FALSE(completed);
	REQUIRE(row.SourceFrameCacheLoading->NextSlot == 1);
	REQUIRE(row.Values.back().Frame == 2);
	REQUIRE(std::get<int64_t>(row.Values.back().Data) == 10);
	REQUIRE(StepSourceFrameCacheLoading(node, 2, row, row, completed, error) == Status::Ok);
	REQUIRE(completed);
	REQUIRE_FALSE(row.SourceFrameCacheLoading->Loading);
	REQUIRE(std::get<int64_t>(row.Values.back().Data) == -4);
	REQUIRE(row.SourceFrameCacheLoading->PendingSlots.size() == 1);
	const auto stopped = row;
	REQUIRE(StepSourceFrameCacheLoading(node, 99, row, row, completed, error) == Status::Ok);
	REQUIRE_FALSE(completed);
	REQUIRE(row == stopped);
}
TEST_CASE("array loader completion uses original width including holes", "[frame_cache_loading]") {
	const auto node = Owner(true);
	auto row = Begin(node, Inventory(node));
	Diagnostic error;
	bool completed = false;
	REQUIRE(std::get<ArrayValue>(row.Values[1].Data).Items.empty());
	for (uint64_t index = 0; index < 4; ++index) {
		REQUIRE(StepSourceFrameCacheLoading(node, 1, row, row, completed, error) == Status::Ok);
		REQUIRE(completed == (index == 3));
		REQUIRE(row.SourceFrameCacheLoading->NextSlot == index + 1);
	}
	REQUIRE(row.Values.size() == 6);
	REQUIRE(row.SourceFrameCacheLoading->PendingSlots.empty());
	REQUIRE(std::get<int64_t>(row.Values.back().Data) == -4);
}
TEST_CASE("clear preserves loading cursor original width and pending values", "[frame_cache_loading]") {
	const auto node = Owner();
	auto row = Begin(node, Inventory(node));
	Diagnostic error;
	bool completed = false;
	REQUIRE(StepSourceFrameCacheLoading(node, 4, row, row, completed, error) == Status::Ok);
	DataReplayState source;
	source.Entries = {row};
	DataReplayState cleared;
	REQUIRE(ClearSourceFrameCacheReplay(node, source, cleared, error) == Status::Ok);
	REQUIRE(cleared.Entries[0].Values.size() == 2);
	REQUIRE(cleared.Entries[0].SourceFrameCacheLoading == row.SourceFrameCacheLoading);
	REQUIRE(cleared.Entries[0].SourceFrameCacheSerializedSlots == row.SourceFrameCacheSerializedSlots);
	REQUIRE(
		StepSourceFrameCacheLoading(node, 4, cleared.Entries[0], cleared.Entries[0], completed, error) ==
		Status::Ok
	);
	REQUIRE(cleared.Entries[0].Values.back().Frame == 3);
}
TEST_CASE("loader refuses malformed unknown and undefined slots atomically", "[frame_cache_loading]") {
	const auto node = Owner();
	auto inventory = Inventory(node);
	Diagnostic error;
	DataReplayEntry output;
	output.NodeId = "sentinel";
	const auto before = output;
	inventory.SourceFrameCacheSerializedSlots.reset();
	REQUIRE(BeginSourceFrameCacheLoading(node, inventory, output, error) == Status::UnsupportedExecution);
	REQUIRE(output == before);
	auto row = Begin(node, Inventory(node));
	bool completed = true;
	REQUIRE(StepSourceFrameCacheLoading(node, 4, row, output, completed, error, 1) == Status::LimitExceeded);
	REQUIRE(output == before);
	REQUIRE(completed);
	row.SourceFrameCacheLoading->NextSlot = 4;
	row.SourceFrameCacheLoading->PendingSlots.clear();
	REQUIRE(
		StepSourceFrameCacheLoading(node, 5, row, output, completed, error) == Status::UnsupportedExecution
	);
	REQUIRE(output == before);
	REQUIRE(completed);
	DataReplayState invalid;
	invalid.Entries = {row};
	invalid.Entries[0].SourceFrameCacheLoading->PendingSlots = {{6, int64_t{3}}};
	REQUIRE(ValidateDataReplay(invalid, Limits::MaximumEvaluationBytes, error) == Status::InvalidValue);
}
TEST_CASE(
	"pending payload residency and guaranteed array publication are admitted before cloning",
	"[frame_cache_loading]"
) {
	const auto node = Owner(true);
	auto inventory = Inventory(node);
	Diagnostic error;
	DataReplayEntry output;
	inventory.SourceFrameCacheSerializedSlots = Limits::MaximumArrayElements;
	REQUIRE(BeginSourceFrameCacheLoading(node, inventory, output, error) == Status::LimitExceeded);
	auto row = Begin(node, Inventory(node));
	const auto bytes = RetainedDataReplayEntryBytes(row);
	auto minimal = row;
	minimal.SourceFrameCacheLoading->PendingSlots.clear();
	REQUIRE(bytes > RetainedDataReplayEntryBytes(minimal));
	DataReplayState invalid;
	invalid.Entries = {row};
	invalid.Entries[0].SourceFrameCacheLoading->PendingSlots[0].Data =
		std::numeric_limits<double>::quiet_NaN();
	REQUIRE(ValidateDataReplay(invalid, Limits::MaximumEvaluationBytes, error) == Status::InvalidValue);
}

TEST_CASE(
	"active loading rejects future published slots and completion permits runtime captures",
	"[frame_cache_loading]"
) {
	const auto node = Owner();
	auto row = Begin(node, Inventory(node));
	Diagnostic error;
	row.Values.push_back({4, int64_t{30}});
	DataReplayState invalid;
	invalid.Entries = {row};
	REQUIRE(ValidateDataReplay(invalid, Limits::MaximumEvaluationBytes, error) == Status::InvalidValue);
	DataReplayEntry output;
	output.NodeId = "sentinel";
	const auto before = output;
	bool completed = true;
	REQUIRE(StepSourceFrameCacheLoading(node, 2, row, output, completed, error) == Status::InvalidValue);
	REQUIRE(output == before);
	REQUIRE(completed);
	row.SourceFrameCacheLoading->NextSlot = 1;
	row.SourceFrameCacheLoading->Loading = false;
	row.SourceFrameCacheLoading->PendingSlots = {{4, int64_t{30}}};
	row.Values.push_back({100, int64_t{90}});
	invalid.Entries = {row};
	REQUIRE(ValidateDataReplay(invalid, Limits::MaximumEvaluationBytes, error) == Status::Ok);
}
