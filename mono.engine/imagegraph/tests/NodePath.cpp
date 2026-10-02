// Path family fixtures.

#include "../src/nodes/Path.hpp"

#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.node_path")

using namespace engine::imagegraph;

namespace {
	Value Anchor(std::initializer_list<double> numbers) {
		ArrayValue array{ValueType::Scalar, {}};
		for (const double number : numbers)
			array.Elements.emplace_back(number);
		return array;
	}

	Vector2 SamplePath(Value first, Value second, double ratio) {
		Document document;
		document.FormatVersion = 7;
		document.Nodes.push_back(
			{"path",
			 "pc.path",
			 "",
			 {},
			 {{"sample_path", ratio}},
			 {{"anchor_0", ValueType::Array, first}, {"anchor_1", ValueType::Array, second}}}
		);
		document.Outputs.push_back({"point", "path", "position_out"});
		Plan plan;
		Diagnostic diagnostic;
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		EvaluatedValue value;
		INFO(diagnostic.Message);
		REQUIRE(EvaluateValue(document, plan, "point", {0, 0}, value, diagnostic) == Status::Ok);
		return std::get<Vector2>(value.Data);
	}
}

TEST_CASE("Path samples a straight segment by length ratio", "[imagegraph][node_path]") {
	const Vector2 middle = SamplePath(Anchor({0, 0, 0, 0, 0, 0, 0}), Anchor({10, 0, 0, 0, 0, 0, 0}), 0.5);
	CHECK(middle == Vector2{5.0, 0.0});
}

TEST_CASE("Path evaluates a Bezier segment through its handles", "[imagegraph][node_path]") {
	// Out handle (0, 10) on the first anchor and in handle (0, 10) on the second; t = 0.5 on one segment.
	const Vector2 middle = SamplePath(Anchor({0, 0, 0, 0, 0, 10, 0}), Anchor({10, 0, 0, 10, 0, 0, 0}), 0.5);
	CHECK(middle == Vector2{5.0, 7.5});
}

TEST_CASE(
	"Path runtime replacement admits old and new storage together",
	"[imagegraph][node_path][evaluation_budget]"
) {
	Path2D first;
	first.Anchors = {{{0, 0, 0, 0, 0, 0}, 0}, {{10, 0, 0, 0, 0, 0}, 0}};
	first.Weights = {{0, 1}, {100, 1}};
	Path2D second = first;
	second.Anchors.push_back({{20, 0, 0, 0, 0, 0}, 0});
	const auto firstBytes = detail::PathRuntime::StorageBytes(first);
	const auto secondBytes = detail::PathRuntime::StorageBytes(second);
	REQUIRE(firstBytes);
	REQUIRE(secondBytes);
	const auto *entry = FindCatalogueEntry("pc.path");
	REQUIRE(entry);
	const Node node{"path", "pc.path", "", {}, {}};
	const EvaluationRequest request;
	for (const uint64_t limit : {*firstBytes + *secondBytes - 1, *firstBytes + *secondBytes}) {
		detail::EvaluationBudget budget(limit);
		{
			detail::NodeContext context(node, *entry, request, budget);
			context.ByteBudget = limit;
			detail::PathRuntime runtime;
			REQUIRE(runtime.Init(context, first));
			CHECK(runtime.LengthTotal == 10.0);
			if (limit < *firstBytes + *secondBytes) {
				CHECK_FALSE(runtime.Init(context, second));
				CHECK(context.FailureCode == Status::LimitExceeded);
				CHECK(runtime.LengthTotal == 10.0);
				CHECK(runtime.PointRatio(.5).X == 5.0);
				CHECK(budget.Used() == *firstBytes);
			} else {
				REQUIRE(runtime.Init(context, second));
				CHECK(runtime.LengthTotal == 20.0);
				CHECK(runtime.PointRatio(.5).X == 10.0);
				CHECK(budget.Used() == *secondBytes);
				CHECK(budget.Peak() == *firstBytes + *secondBytes);
			}
			CHECK(first.Anchors.size() == 2);
			CHECK(first.Anchors.back().Controls[0] == 10.0);
		}
		CHECK(budget.Used() == 0);
	}
}

TEST_CASE(
	"Path publishes sorted anchors under one shared live budget", "[imagegraph][node_path][evaluation_budget]"
) {
	const auto *entry = FindCatalogueEntry("pc.path");
	const auto executor = detail::FindExecutor("pc.path");
	REQUIRE(entry);
	REQUIRE(executor);
	const Node node{
		"path",
		"pc.path",
		"",
		{},
		{{"sample_path", .5}, {"sample_mode", int64_t{0}}, {"loop", false}, {"round_anchor", false}},
		{{"anchor_1", ValueType::Array, Anchor({10, 0, 0, 0, 0, 0, 0})},
		 {"anchor_0", ValueType::Array, Anchor({0, 0, 0, 0, 0, 0, 0})}}
	};
	const auto original = node.DynamicInputs;
	const EvaluationRequest request;
	uint64_t peak = 0;
	for (int pass = 0; pass < 2; ++pass) {
		const uint64_t limit = pass == 0 ? Limits::MaximumEvaluationBytes : peak - 1;
		detail::EvaluationBudget budget(limit);
		auto previous = budget.Reserve(64);
		REQUIRE(previous);
		{
			detail::NodeContext context(node, *entry, request, budget);
			context.ByteBudget = limit;
			for (const auto &value : node.Values)
				context.ValueViews.emplace_back(value.Port, &value.Data);
			for (const auto &value : node.DynamicInputs)
				context.ValueViews.emplace_back(value.Id, &*value.Default);
			const bool ok = executor(context);
			if (pass == 0) {
				REQUIRE(ok);
				REQUIRE(context.FailureCode == Status::Ok);
				REQUIRE(context.OutputValues.size() == 4);
				CHECK(std::get<Vector2>(context.OutputValues[0].Data) == Vector2{5, 0});
				const auto &path = std::get<Path2D>(context.OutputValues[1].Data);
				CHECK(path.Anchors.front().Controls[0] == 0);
				CHECK(path.Anchors.back().Controls[0] == 10);
				const auto &anchors = std::get<ArrayValue>(context.OutputValues[2].Data);
				CHECK(anchors.Elements.size() == 14);
				CHECK(std::get<double>(anchors.Elements.front()) == 0);
				CHECK(std::get<double>(anchors.Elements[7]) == 10);
				CHECK(
					std::get<ArrayValue>(context.OutputValues[3].Data) ==
					ArrayValue{ValueType::Scalar, {0.0, 1.0, 100.0, 1.0}}
				);
				peak = budget.Peak();
			} else {
				CHECK_FALSE(ok);
				CHECK(context.FailureCode == Status::LimitExceeded);
				CHECK(context.OutputValues.empty());
			}
			CHECK(node.DynamicInputs == original);
			CHECK(budget.Peak() <= limit);
		}
		CHECK(budget.Used() == 64);
	}
}

TEST_CASE(
	"Path runtime rejects excessive borrowed counts before allocation",
	"[imagegraph][node_path][evaluation_budget]"
) {
	const auto *entry = FindCatalogueEntry("pc.path");
	REQUIRE(entry);
	const Node node{"path", "pc.path", "", {}, {}};
	const EvaluationRequest request;
	for (const bool excessiveWeights : {false, true}) {
		Path2D borrowed;
		if (excessiveWeights)
			borrowed.Weights.resize(Limits::MaximumPathWeights + 1);
		else
			borrowed.Anchors.resize(Limits::MaximumPathAnchors + 1);
		detail::EvaluationBudget budget(0);
		detail::NodeContext context(node, *entry, request, budget);
		detail::PathRuntime runtime;
		CHECK_FALSE(runtime.Init(context, borrowed));
		CHECK(context.FailureCode == Status::LimitExceeded);
		CHECK(context.FailurePort == "path");
		CHECK(runtime.Anchors.empty());
		CHECK(runtime.Weights.empty());
		CHECK(budget.Peak() == 0);
	}
}
