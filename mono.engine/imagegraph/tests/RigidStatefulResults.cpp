#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.rigid_stateful_results")
using namespace engine::imagegraph;
namespace {
	RigidReplayState RecordedOwner() {
		RigidReplayState state;
		RigidOwnerReplayState owner;
		owner.AuthoringRevision = 17;
		owner.History.OwnerId = "retained-owner";
		for (size_t i = 0; i < 2; ++i) {
			SourceRigidFrame frame;
			frame.Events.push_back({{"recorded-consumer", 0, 0}, SourceRigidEvent::Checkpoint{}});
			owner.History.Frames.push_back(std::move(frame));
		}
		state.Owners.push_back(std::move(owner));
		return state;
	}
	Document NumberGraph() {
		Document document;
		document.Nodes = {{"number", "value.number", "", {}, {{"value", 4.}}}};
		document.Outputs = {{"out", "number", "number"}};
		return document;
	}
}
TEST_CASE(
	"Stateful evaluation retains copied rigid event journals outside the selected cone",
	"[imagegraph][rigid_stateful_results]"
) {
	const auto document = NumberGraph();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	const auto prior = RecordedOwner();
	REQUIRE(ValidateRigidReplay(prior, Limits::MaximumEvaluationBytes, diagnostic) == Status::Ok);
	EvaluationRequest request;
	request.Tick = 3;
	request.RigidReplay = &prior;
	StatefulEvaluationResult result;
	REQUIRE(EvaluateStateful(document, plan, "out", request, result, diagnostic) == Status::Ok);
	CHECK(result.Rigid == prior);
	CHECK(std::get<double>(std::get<EvaluatedValue>(result.Output).Data) == 4.);
	CHECK(prior == RecordedOwner());
	const auto retained = result.Rigid;
	request.RigidReplay = &result.Rigid;
	REQUIRE(EvaluateStateful(document, plan, "out", request, result, diagnostic) == Status::Ok);
	CHECK(result.Rigid == retained);
	CHECK(EvaluateStateful(document, plan, "out", request, result, diagnostic, 1) == Status::LimitExceeded);
	CHECK(result.Rigid == retained);
	CHECK(std::get<double>(std::get<EvaluatedValue>(result.Output).Data) == 4.);
}
TEST_CASE(
	"Input and named output snapshots publish rigid journals atomically",
	"[imagegraph][rigid_stateful_results]"
) {
	auto document = NumberGraph();
	document.Nodes.push_back({"inspection", "value.number", "", {}, {{"value", 9.}}});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	const auto prior = RecordedOwner();
	EvaluationRequest request;
	request.RigidReplay = &prior;
	const std::array<std::string, 1> outputs{"out"};
	StatefulInputEvaluationResult result;
	REQUIRE(
		EvaluateStatefulNodeInputs(
			document, plan, "inspection", request, result, diagnostic, Limits::MaximumEvaluationBytes, outputs
		) == Status::Ok
	);
	CHECK(result.Rigid == prior);
	REQUIRE(result.Outputs.size() == 1);
	CHECK(result.Outputs.front().Id == "out");
	CHECK(std::get<double>(std::get<EvaluatedValue>(result.Outputs.front().Output).Data) == 4.);
	const auto retained = result.Rigid;
	CHECK(
		EvaluateStatefulNodeInputs(document, plan, "inspection", request, result, diagnostic, 1, outputs) ==
		Status::LimitExceeded
	);
	CHECK(result.Rigid == retained);
	CHECK(result.Outputs.size() == 1);
	const std::array<std::string, 1> missing{"missing"};
	CHECK(
		EvaluateStatefulNodeInputs(
			document, plan, "inspection", request, result, diagnostic, Limits::MaximumEvaluationBytes, missing
		) == Status::InvalidOutput
	);
	CHECK(result.Rigid == retained);
	CHECK(result.Outputs.size() == 1);
}
