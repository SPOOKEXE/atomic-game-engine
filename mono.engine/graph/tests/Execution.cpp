#include <engine/graph/Execution.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>

TEST_SUITE_ID("engine.graph.execution")
TEST_DEPENDS("engine.graph.nodeschema")

using engine::core::Name;
using namespace engine::graph;

namespace {
	NodeSchema PassSchema(std::string id, std::string valueType, int *evaluations = nullptr) {
		return {
			.Id = std::move(id),
			.Title = "Pass",
			.Ports =
				{
					{.Id = "value",
					 .ValueType = valueType,
					 .Direction = NodePortDirection::Input,
					 .DefaultValue = 0.0},
					{.Id = "value", .ValueType = valueType, .Direction = NodePortDirection::Output},
				},
			.BypassMappings = {{.Input = "value", .Output = "value"}},
			.Evaluate = [evaluations](
							const NodeSchema &, const NodeValues &inputs, NodeValues &outputs, std::string &
						) {
				if (evaluations != nullptr) {
					(*evaluations)++;
				}
				outputs["value"] = inputs.at("value");
				return true;
			},
		};
	}

	NodeRuntime TwoPassRuntime(NodeSchemaRegistry &schemas, int *evaluations = nullptr) {
		REQUIRE(schemas.Add(PassSchema("test.pass", "number", evaluations)) == NodeSchemaStatus::Ok);
		NodeRuntime runtime(schemas);
		REQUIRE(
			runtime.AddNode({.Id = Name("source"), .Schema = Name("test.pass")}) == NodeExecutionStatus::Ok
		);
		REQUIRE(
			runtime.AddNode({.Id = Name("target"), .Schema = Name("test.pass")}) == NodeExecutionStatus::Ok
		);
		REQUIRE(
			runtime.Connect(
				{.FromNode = Name("source"), .FromPort = "value", .ToNode = Name("target"), .ToPort = "value"}
			) == NodeExecutionStatus::Ok
		);
		return runtime;
	}

	class PendingDispatcher final : public NodeExecutionDispatcher {
	  public:
		NodeDispatchResult
		Dispatch(const NodeExecutionRequest &request, NodeValues &, std::string &) override {
			Requests.push_back(request);
			return NodeDispatchResult::Pending;
		}

		std::vector<NodeExecutionRequest> Requests;
	};
}

TEST_CASE("node execution caches outputs and dirties only dependent nodes", "[graph][execution]") {
	NodeSchemaRegistry schemas;
	int evaluations = 0;
	NodeRuntime runtime = TwoPassRuntime(schemas, &evaluations);

	REQUIRE(runtime.SetInput(Name("source"), "value", 4.0) == NodeExecutionStatus::Ok);
	REQUIRE(runtime.Evaluate() == NodeExecutionStatus::Ok);
	CHECK(evaluations == 2);
	CHECK(runtime.Find(Name("source"))->State == NodeExecutionState::Complete);
	CHECK(std::get<double>(runtime.Find(Name("target"))->Outputs.at("value")) == 4.0);

	REQUIRE(runtime.Evaluate() == NodeExecutionStatus::Ok);
	CHECK(evaluations == 2);

	REQUIRE(runtime.SetEnabled(Name("target"), false) == NodeExecutionStatus::Ok);
	REQUIRE(runtime.SetInput(Name("source"), "value", 9.0) == NodeExecutionStatus::Ok);
	REQUIRE(runtime.Evaluate() == NodeExecutionStatus::Ok);
	CHECK(evaluations == 3);
	CHECK(std::get<double>(runtime.Find(Name("target"))->Outputs.at("value")) == 9.0);

	REQUIRE(runtime.SetEnabled(Name("target"), true) == NodeExecutionStatus::Ok);
	REQUIRE(runtime.Evaluate() == NodeExecutionStatus::Ok);
	CHECK(evaluations == 4);

	REQUIRE(runtime.SetBypassMode(Name("target"), NodeBypassMode::Bypass) == NodeExecutionStatus::Ok);
	REQUIRE(runtime.SetInput(Name("source"), "value", 12.0) == NodeExecutionStatus::Ok);
	REQUIRE(runtime.Evaluate() == NodeExecutionStatus::Ok);
	CHECK(evaluations == 5);
	CHECK(std::get<double>(runtime.Find(Name("target"))->Outputs.at("value")) == 12.0);
}

TEST_CASE(
	"runtime rejects a dynamic any output that does not match its typed destination", "[graph][execution]"
) {
	NodeSchemaRegistry schemas;
	NodeSchema anySource = PassSchema("test.any-source", "any");
	anySource.Ports[0].DefaultValue = std::string("not a number");
	REQUIRE(schemas.Add(std::move(anySource)) == NodeSchemaStatus::Ok);
	REQUIRE(schemas.Add(PassSchema("test.number", "number")) == NodeSchemaStatus::Ok);

	NodeRuntime runtime(schemas);
	REQUIRE(
		runtime.AddNode({.Id = Name("any-source"), .Schema = Name("test.any-source")}) ==
		NodeExecutionStatus::Ok
	);
	REQUIRE(
		runtime.AddNode({.Id = Name("number-target"), .Schema = Name("test.number")}) ==
		NodeExecutionStatus::Ok
	);
	REQUIRE(
		runtime.Connect({
			.FromNode = Name("any-source"),
			.FromPort = "value",
			.ToNode = Name("number-target"),
			.ToPort = "value",
		}) == NodeExecutionStatus::Ok
	);

	CHECK(runtime.Evaluate() == NodeExecutionStatus::EvaluationFailed);
	const NodeExecutionRecord *source = runtime.Find(Name("any-source"));
	const NodeExecutionRecord *target = runtime.Find(Name("number-target"));
	REQUIRE(source != nullptr);
	REQUIRE(target != nullptr);
	CHECK(source->State == NodeExecutionState::Error);
	CHECK(source->Error.find("expects number") != std::string::npos);
	CHECK(target->State == NodeExecutionState::Error);
}

TEST_CASE(
	"an async dispatcher returns copied work and completes through one runtime door", "[graph][execution]"
) {
	NodeSchemaRegistry schemas;
	REQUIRE(schemas.Add(PassSchema("test.pass", "number")) == NodeSchemaStatus::Ok);
	NodeRuntime runtime(schemas);
	REQUIRE(runtime.AddNode({.Id = Name("async"), .Schema = Name("test.pass")}) == NodeExecutionStatus::Ok);
	REQUIRE(runtime.SetInput(Name("async"), "value", 7.0) == NodeExecutionStatus::Ok);

	PendingDispatcher dispatcher;
	CHECK(runtime.Evaluate(&dispatcher) == NodeExecutionStatus::Pending);
	REQUIRE(runtime.Find(Name("async"))->State == NodeExecutionState::Running);
	REQUIRE(dispatcher.Requests.size() == 1);
	CHECK(dispatcher.Requests.front().Node == Name("async"));
	CHECK(std::get<double>(dispatcher.Requests.front().Inputs.at("value")) == 7.0);

	CHECK(
		runtime.CompleteAsync(Name("async"), dispatcher.Requests.front().Revision, {{"value", 7.0}}) ==
		NodeExecutionStatus::Ok
	);
	CHECK(runtime.Find(Name("async"))->State == NodeExecutionState::Complete);
	CHECK(std::get<double>(runtime.Find(Name("async"))->Outputs.at("value")) == 7.0);
}

TEST_CASE("an invalidated async request cannot publish stale output", "[graph][execution]") {
	NodeSchemaRegistry schemas;
	REQUIRE(schemas.Add(PassSchema("test.pass", "number")) == NodeSchemaStatus::Ok);
	NodeRuntime runtime(schemas);
	REQUIRE(runtime.AddNode({.Id = Name("async"), .Schema = Name("test.pass")}) == NodeExecutionStatus::Ok);
	REQUIRE(runtime.SetInput(Name("async"), "value", 7.0) == NodeExecutionStatus::Ok);

	PendingDispatcher dispatcher;
	REQUIRE(runtime.Evaluate(&dispatcher) == NodeExecutionStatus::Pending);
	REQUIRE(dispatcher.Requests.size() == 1);
	const NodeExecutionRequest first = dispatcher.Requests.front();

	REQUIRE(runtime.SetInput(Name("async"), "value", 11.0) == NodeExecutionStatus::Ok);
	CHECK(runtime.Evaluate(&dispatcher) == NodeExecutionStatus::Pending);
	CHECK(
		runtime.CompleteAsync(Name("async"), first.Revision, {{"value", 7.0}}) ==
		NodeExecutionStatus::StaleCompletion
	);
	REQUIRE(runtime.Find(Name("async"))->State == NodeExecutionState::Queued);

	REQUIRE(runtime.Evaluate(&dispatcher) == NodeExecutionStatus::Pending);
	REQUIRE(dispatcher.Requests.size() == 2);
	const NodeExecutionRequest &second = dispatcher.Requests.back();
	CHECK(second.Revision != first.Revision);
	CHECK(std::get<double>(second.Inputs.at("value")) == 11.0);
	CHECK(
		runtime.CompleteAsync(Name("async"), second.Revision, {{"value", 11.0}}) == NodeExecutionStatus::Ok
	);
	CHECK(std::get<double>(runtime.Find(Name("async"))->Outputs.at("value")) == 11.0);
}

TEST_CASE("an async completion must satisfy the node output contract", "[graph][execution]") {
	NodeSchemaRegistry schemas;
	REQUIRE(schemas.Add(PassSchema("test.pass", "number")) == NodeSchemaStatus::Ok);
	NodeRuntime runtime(schemas);
	REQUIRE(runtime.AddNode({.Id = Name("async"), .Schema = Name("test.pass")}) == NodeExecutionStatus::Ok);

	PendingDispatcher dispatcher;
	REQUIRE(runtime.Evaluate(&dispatcher) == NodeExecutionStatus::Pending);
	REQUIRE(dispatcher.Requests.size() == 1);
	CHECK(
		runtime.CompleteAsync(Name("async"), dispatcher.Requests.front().Revision, {{"unexpected", 3.0}}) ==
		NodeExecutionStatus::EvaluationFailed
	);
	REQUIRE(runtime.Find(Name("async"))->State == NodeExecutionState::Error);
	CHECK(runtime.Find(Name("async"))->Error.find("undeclared") != std::string::npos);
}

TEST_CASE("a link exceeding an input cardinality and an unbroken cycle are refused", "[graph][execution]") {
	NodeSchemaRegistry schemas;
	NodeRuntime runtime = TwoPassRuntime(schemas);
	REQUIRE(
		runtime.AddNode({.Id = Name("second-source"), .Schema = Name("test.pass")}) == NodeExecutionStatus::Ok
	);
	CHECK(
		runtime.Connect({
			.FromNode = Name("second-source"),
			.FromPort = "value",
			.ToNode = Name("target"),
			.ToPort = "value",
		}) == NodeExecutionStatus::InvalidConnection
	);
	CHECK(
		runtime.Connect(
			{.FromNode = Name("target"), .FromPort = "value", .ToNode = Name("source"), .ToPort = "value"}
		) == NodeExecutionStatus::Ok
	);
	CHECK(runtime.Evaluate() == NodeExecutionStatus::Cycle);
}

TEST_CASE("an explicit delay boundary feeds a feedback cycle from its cached output", "[graph][execution]") {
	NodeSchemaRegistry schemas;
	REQUIRE(schemas.Add(PassSchema("test.pass", "number")) == NodeSchemaStatus::Ok);
	NodeRuntime runtime(schemas);
	REQUIRE(
		runtime.AddNode({.Id = Name("delay"), .Schema = Name("test.pass"), .BreaksCycle = true}) ==
		NodeExecutionStatus::Ok
	);
	REQUIRE(
		runtime.AddNode({.Id = Name("feedback"), .Schema = Name("test.pass")}) == NodeExecutionStatus::Ok
	);
	REQUIRE(
		runtime.Connect(
			{.FromNode = Name("delay"), .FromPort = "value", .ToNode = Name("feedback"), .ToPort = "value"}
		) == NodeExecutionStatus::Ok
	);
	REQUIRE(runtime.SetInput(Name("delay"), "value", 3.0) == NodeExecutionStatus::Ok);
	REQUIRE(runtime.Evaluate() == NodeExecutionStatus::Ok);

	REQUIRE(
		runtime.Connect(
			{.FromNode = Name("feedback"), .FromPort = "value", .ToNode = Name("delay"), .ToPort = "value"}
		) == NodeExecutionStatus::Ok
	);
	CHECK(runtime.Evaluate() == NodeExecutionStatus::Ok);
	CHECK(std::get<double>(runtime.Find(Name("delay"))->Outputs.at("value")) == 3.0);
	CHECK(std::get<double>(runtime.Find(Name("feedback"))->Outputs.at("value")) == 3.0);
}
