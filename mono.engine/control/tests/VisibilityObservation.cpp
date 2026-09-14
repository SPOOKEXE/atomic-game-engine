#include <engine/control/Surface.hpp>
#include <engine/control/features/VisibilityObservation.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>

TEST_SUITE_ID("engine.control.visibilityobservation")

namespace {
	engine::control::features::VisibilitySnapshotReply Snapshot(size_t rows, size_t dropped = 0) {
		engine::control::features::VisibilitySnapshotReply snapshot;
		snapshot.Valid = true;
		snapshot.Frame = 8;
		snapshot.ViewSlot = 3;
		snapshot.World = "visibility.world";
		snapshot.Dropped = dropped;
		for (size_t index = 0; index < rows; index++) {
			snapshot.Observations.push_back({
				.World = index % 2 == 0 ? "visibility.world" : "other.world",
				.Entity = index + 1,
				.State = "submitted-opaque-or-masked",
				.Cause = "opaque-or-masked-draw",
			});
		}
		return snapshot;
	}

	const engine::control::Tool &Tool(engine::control::Surface &surface) {
		const auto found =
			std::find_if(surface.Registered().begin(), surface.Registered().end(), [](const auto &tool) {
				return tool.Name == "visibility_observations";
			});
		REQUIRE(found != surface.Registered().end());
		return *found;
	}
}

TEST_CASE(
	"visibility observation MCP feature exposes schema and snapshot identity", "[control][visibility]"
) {
	engine::control::Surface surface("visibility", "visibility test");
	surface.Enable(std::array{engine::control::features::VisibilityObservations([] { return Snapshot(2); })});
	const auto &tool = Tool(surface);
	const auto schema = tool.Schema();
	CHECK(schema["type"] == "object");
	CHECK(schema["properties"].contains("world"));
	CHECK(schema["properties"].contains("entity"));
	std::string failure;
	const auto reply = tool.Call(nlohmann::json::object(), failure);
	REQUIRE(failure.empty());
	CHECK(reply["valid"] == true);
	CHECK(reply["frame"] == 8);
	CHECK(reply["viewSlot"] == 3);
	CHECK(reply["world"] == "visibility.world");
	CHECK(reply["observations"].size() == 2);
	CHECK(reply["observations"][0]["state"] == "submitted-opaque-or-masked");
	CHECK(reply["observations"][0]["cause"] == "opaque-or-masked-draw");
	CHECK(reply["dropped"] == 0);
	CHECK(reply["dropped_exact"] == true);
}

TEST_CASE("visibility observation MCP feature filters and bounds rows", "[control][visibility]") {
	engine::control::Surface surface("visibility", "visibility test");
	surface.Enable(std::array{engine::control::features::VisibilityObservations([] {
		return Snapshot(600, 4);
	})});
	const auto &tool = Tool(surface);
	std::string failure;
	const auto filtered = tool.Call({{"world", "other.world"}, {"entity", 2}}, failure);
	REQUIRE(failure.empty());
	REQUIRE(filtered["observations"].size() == 1);
	CHECK(filtered["observations"][0]["entity"] == 2);
	const auto reply = tool.Call(nlohmann::json::object(), failure);
	REQUIRE(failure.empty());
	CHECK(reply["observations"].size() == 256);
	CHECK(reply["truncated"] == true);
	CHECK(reply["dropped"] == 4);
	CHECK(reply["dropped_exact"] == true);
}

TEST_CASE(
	"visibility observation MCP feature preserves invalid snapshots and rejects bad filters",
	"[control][visibility]"
) {
	engine::control::Surface surface("visibility", "visibility test");
	surface.Enable(std::array{engine::control::features::VisibilityObservations([] {
		return engine::control::features::VisibilitySnapshotReply{};
	})});
	const auto &tool = Tool(surface);
	std::string failure;
	const auto invalid = tool.Call(nlohmann::json::object(), failure);
	REQUIRE(failure.empty());
	CHECK(invalid["valid"] == false);
	CHECK(invalid["observations"].empty());
	CHECK(invalid["truncated"] == false);
	CHECK(tool.Call({{"entity", -1}}, failure).is_null());
	CHECK_FALSE(failure.empty());
}
