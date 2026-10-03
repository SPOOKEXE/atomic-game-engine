#include <engine/imagegraph/PendingHostObservations.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.pendinghostobservations")

namespace {
	struct CountingCapability final : engine::imagegraph::HostNodeProvider {
		uint32_t Calls = 0;
		bool Capture(
			const engine::imagegraph::HostNodeInvocation &invocation,
			engine::imagegraph::HostNodeCapture &output,
			std::string &failure
		) override {
			using namespace engine::imagegraph;
			HostNodeCapture captured;
			Diagnostic error;
			uint64_t bytes = 0;
			if (PrepareResolvedHostCapture(
					invocation, invocation.MaximumOperationBytes, captured, bytes, error
				) != Status::Ok) {
				failure = error.Message;
				return false;
			}
			++Calls;
			captured.Outputs.push_back({"content", std::to_string(Calls)});
			output = std::move(captured);
			return true;
		}
	};
}
TEST_CASE(
	"pending host retries preserve exact capability observation and refuse changed inputs",
	"[imagegraph][pending-host-observations]"
) {
	using namespace engine::imagegraph;
	Node node{"read", "pc.text_file_read", "", {}, {{"path", std::string("captured.txt")}}};
	EvaluationRequest clock{.Tick = 17, .Seed = 9};
	std::vector<AuthoredValue> values = node.Values;
	HostNodeInvocation invocation{node, clock, values, {}, 1024 * 1024};
	PendingHostObservations observations;
	CountingCapability capability;
	HostNodeCapture result;
	std::string error;
	REQUIRE(observations.Capture(invocation, capability, result, error));
	CHECK(capability.Calls == 1);
	REQUIRE(observations.Capture(invocation, capability, result, error));
	CHECK(capability.Calls == 1);
	CHECK(std::get<std::string>(result.Outputs.front().Data) == "1");
	values.front().Data = std::string("changed.txt");
	CHECK_FALSE(observations.Capture(invocation, capability, result, error));
	CHECK(capability.Calls == 1);
	CHECK(std::get<std::string>(result.Outputs.front().Data) == "1");
	clock.Tick = 18;
	CHECK_FALSE(observations.Capture(invocation, capability, result, error));
	CHECK(capability.Calls == 1);
	observations.Clear();
	REQUIRE(observations.Capture(invocation, capability, result, error));
	CHECK(capability.Calls == 2);
}
TEST_CASE(
	"pending host budget refuses before capability side effects and retains last good output",
	"[imagegraph][pending-host-observations]"
) {
	using namespace engine::imagegraph;
	Node node{"read", "pc.text_file_read", "", {}, {{"path", std::string("captured.txt")}}};
	EvaluationRequest clock;
	HostNodeInvocation invocation{node, clock, node.Values, {}, 1};
	PendingHostObservations observations;
	CountingCapability capability;
	HostNodeCapture result;
	result.Failure = "last good";
	std::string error;
	CHECK_FALSE(observations.Capture(invocation, capability, result, error));
	CHECK(capability.Calls == 0);
	CHECK(result.Failure == "last good");
	CHECK_FALSE(observations.Active);
	invocation.MaximumOperationBytes = 1024 * 1024;
	REQUIRE(observations.Capture(invocation, capability, result, error));
	CHECK(capability.Calls == 1);
	invocation.MaximumOperationBytes = observations.Bytes;
	CHECK_FALSE(observations.Capture(invocation, capability, result, error));
	CHECK(capability.Calls == 1);
	CHECK(observations.Captures.size() == 1);
}
