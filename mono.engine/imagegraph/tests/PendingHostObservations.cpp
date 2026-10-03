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

TEST_CASE(
	"pending notification receipts preserve ordered messages without repeated visible effects",
	"[imagegraph][pending-host-observations]"
) {
	using namespace engine::imagegraph;
	struct MessageHost final : HostNodeProvider {
		size_t Calls = 0;
		bool Capture(const HostNodeInvocation &, HostNodeCapture &, std::string &) override {
			return false;
		}
		bool PcxMessages(std::string_view, std::span<const PcxMessage>, std::string &) override {
			++Calls;
			return true;
		}
	} host;
	PendingHostObservations observations;
	EvaluationRequest request{.Tick = 4, .Seed = 3};
	std::vector<PcxMessage> messages{{"first", false}, {"warning", true}};
	std::string failure;
	constexpr uint64_t budget = 1024 * 1024;
	observations.BeginAttempt();
	REQUIRE(observations.ForwardMessages(request, "node", messages, host, budget, failure));
	REQUIRE(observations.RetainedPayloadBytes());
	CHECK(host.Calls == 1);
	observations.BeginAttempt();
	REQUIRE(observations.ForwardMessages(request, "node", messages, host, budget, failure));
	CHECK(host.Calls == 1);
	observations.BeginAttempt();
	messages[1].Warning = false;
	CHECK_FALSE(observations.ForwardMessages(request, "node", messages, host, budget, failure));
	CHECK(host.Calls == 1);
	observations.Clear();
	observations.BeginAttempt();
	CHECK_FALSE(observations.ForwardMessages(request, "node", messages, host, 1, failure));
	CHECK(host.Calls == 1);
	CHECK(observations.MessageReceipts.empty());
	REQUIRE(observations.ForwardMessages(request, "node", messages, host, budget, failure));
	observations.BeginAttempt();
	++request.Tick;
	CHECK_FALSE(observations.ForwardMessages(request, "node", messages, host, budget, failure));
	CHECK(host.Calls == 2);
}

TEST_CASE(
	"sequenced host receipts distinguish processor rows and reject changed callback order",
	"[imagegraph][pending-host-observations]"
) {
	using namespace engine::imagegraph;
	Node node{"reader", "pc.text_file_read", "", {}, {}};
	EvaluationRequest request;
	std::vector<AuthoredValue> inputs{{"path", std::string{"row-a.txt"}}};
	HostNodeInvocation call{node, request, inputs, {}, 1024 * 1024};
	PendingHostObservations observations;
	CountingCapability capability;
	HostNodeCapture output;
	std::string failure;
	observations.BeginAttempt();
	REQUIRE(observations.CaptureSequenced(call, capability, output, failure));
	inputs[0].Data = std::string{"row-b.txt"};
	REQUIRE(observations.CaptureSequenced(call, capability, output, failure));
	CHECK(capability.Calls == 2);
	REQUIRE(observations.Captures.size() == 2);
	observations.BeginAttempt();
	inputs[0].Data = std::string{"row-a.txt"};
	REQUIRE(observations.CaptureSequenced(call, capability, output, failure));
	CHECK(std::get<std::string>(output.Outputs[0].Data) == "1");
	inputs[0].Data = std::string{"row-b.txt"};
	REQUIRE(observations.CaptureSequenced(call, capability, output, failure));
	CHECK(std::get<std::string>(output.Outputs[0].Data) == "2");
	CHECK(capability.Calls == 2);
	const auto prior = output;
	observations.BeginAttempt();
	CHECK_FALSE(observations.CaptureSequenced(call, capability, output, failure));
	CHECK(capability.Calls == 2);
	CHECK(output.Inputs == prior.Inputs);
	observations.BeginAttempt();
	node.Id = "different node";
	CHECK_FALSE(observations.CaptureSequenced(call, capability, output, failure));
	CHECK(capability.Calls == 2);
	CHECK(output.Inputs == prior.Inputs);
	REQUIRE(observations.RetainedPayloadBytes());
	CHECK(*observations.RetainedPayloadBytes() >= sizeof(PendingHostObservations) + observations.Bytes);
}

TEST_CASE(
	"sequenced pending invocation identity refuses changed queued work before another capability call",
	"[imagegraph][pending-host-observations]"
) {
	using namespace engine::imagegraph;
	struct PendingProvider : HostNodeProvider {
		size_t Calls = 0;
		bool Capture(const HostNodeInvocation &, HostNodeCapture &, std::string &failure) override {
			++Calls;
			failure = "queued fixture work";
			return false;
		}
	} provider;
	Node node{"pending", "pc.text_file_read", "", {}, {}};
	EvaluationRequest request;
	std::vector<AuthoredValue> controls{{"path", std::string{"fixed.txt"}}};
	HostNodeInvocation call{node, request, controls, {}, 1024 * 1024};
	PendingHostObservations observations;
	HostNodeCapture output;
	output.Failure = "prior receipt";
	std::string failure;
	observations.BeginAttempt();
	CHECK_FALSE(observations.CaptureSequenced(call, provider, output, failure));
	REQUIRE(observations.PendingInput);
	CHECK(provider.Calls == 1);
	observations.BeginAttempt();
	controls[0].Data = std::string{"different.txt"};
	CHECK_FALSE(observations.CaptureSequenced(call, provider, output, failure));
	CHECK(provider.Calls == 1);
	CHECK(output.Failure == "prior receipt");
	CHECK(std::get<std::string>(observations.PendingInput->Inputs[0].Data) == "fixed.txt");
}

TEST_CASE(
	"host retry contexts preserve exact timeline format and interpolation before another side effect",
	"[imagegraph][pending-host-observations]"
) {
	using namespace engine::imagegraph;
	struct Capability final : HostNodeProvider {
		CountingCapability Complete;
		size_t Calls = 0;
		bool Pending = false;
		bool Capture(const HostNodeInvocation &call, HostNodeCapture &out, std::string &failure) override {
			++Calls;
			if (Pending) {
				failure = "queued exact context";
				return false;
			}
			return Complete.Capture(call, out, failure);
		}
	} host;
	Node node{"context", "pc.text_file_read", "", {}, {{"path", std::string{"fixed.txt"}}}};
	EvaluationRequest request;
	TimelineSettings timeline{8, 0, 7, "loop", 30};
	HostNodeInvocation call{node, request, node.Values, {}, 1024 * 1024};
	call.Timeline = &timeline;
	call.OutputFormat = SurfaceFormat::RGBA16Float;
	PendingHostObservations observations;
	HostNodeCapture output;
	output.Failure = "previous";
	std::string failure;
	bool queued = false;
	SECTION("completed sequenced receipt") {}
	SECTION("pending sequenced receipt") {
		queued = true;
	}
	host.Pending = queued;
	observations.BeginAttempt();
	CHECK(observations.CaptureSequenced(call, host, output, failure) == !queued);
	REQUIRE(host.Calls == 1);
	const auto prior = output;
	const auto retained = observations.RetainedPayloadBytes();
	REQUIRE(retained);
	CHECK(
		*retained >= sizeof(PendingHostObservations) + observations.Bytes + timeline.Playback.capacity() + 1
	);
	const auto refuseChanged = [&] {
		observations.BeginAttempt();
		CHECK_FALSE(observations.CaptureSequenced(call, host, output, failure));
		CHECK(host.Calls == 1);
		CHECK(output.Inputs == prior.Inputs);
		CHECK(output.Failure == prior.Failure);
		CHECK(observations.RetainedPayloadBytes() == retained);
	};
	call.OutputFormat = SurfaceFormat::RGBA32Float;
	refuseChanged();
	call.OutputFormat = SurfaceFormat::RGBA16Float;
	call.Interpolation = 0;
	refuseChanged();
	call.Interpolation = 1;
	++timeline.FramesPerSecond;
	refuseChanged();
	--timeline.FramesPerSecond;
	timeline.Playback = "once";
	refuseChanged();
	timeline.Playback = "loop";
	call.Timeline = nullptr;
	refuseChanged();
	call.Timeline = &timeline;
	call.OutputFormat.reset();
	refuseChanged();
	call.OutputFormat = SurfaceFormat::RGBA16Float;
	host.Pending = false;
	observations.BeginAttempt();
	REQUIRE(observations.CaptureSequenced(call, host, output, failure));
	CHECK(host.Calls == (queued ? 2 : 1));
}
TEST_CASE(
	"unsequenced host receipts also bind invocation context and admit timeline backing before capture",
	"[imagegraph][pending-host-observations]"
) {
	using namespace engine::imagegraph;
	Node node{"context", "pc.text_file_read", "", {}, {{"path", std::string{"fixed.txt"}}}};
	EvaluationRequest request;
	TimelineSettings timeline{2, 0, 1, "loop", 30};
	HostNodeInvocation call{node, request, node.Values, {}, 1024 * 1024};
	call.Timeline = &timeline;
	CountingCapability host;
	PendingHostObservations observations;
	HostNodeCapture output;
	std::string failure;
	REQUIRE(observations.Capture(call, host, output, failure));
	timeline.Last = 0;
	CHECK_FALSE(observations.Capture(call, host, output, failure));
	CHECK(host.Calls == 1);
	observations.Clear();
	timeline.Playback.assign(Limits::MaximumTextBytes + 1, 'x');
	CHECK_FALSE(observations.Capture(call, host, output, failure));
	CHECK(host.Calls == 1);
	CHECK(observations.Captures.empty());
	CHECK_FALSE(observations.Active);
}
