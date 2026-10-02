// Analytical lattice positions and graph replay follow the pinned source constructor and loop order.

#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>

TEST_SUITE_ID("engine.imagegraph.point_nodes")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;

namespace {
	constexpr std::string_view LATTICE = "pc.scatter_point_lattice_3_d";
	ArrayValue Points(std::initializer_list<Vector3> positions) {
		ArrayValue result{ValueType::Scalar, {}};
		for (const Vector3 point : positions)
			result.Nested.push_back({point.X, point.Y, point.Z});
		return result;
	}
	Value Replay(Document document) {
		Document parsed;
		Diagnostic diagnostic;
		Plan plan;
		const auto read = Read(Write(document), parsed, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(read == Status::Ok);
		CHECK(parsed == document);
		const auto compiled = Compile(parsed, plan, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(compiled == Status::Ok);
		EvaluatedValue result;
		const auto evaluated = EvaluateValue(parsed, plan, "points", {}, result, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << " " << diagnostic.Message);
		REQUIRE(evaluated == Status::Ok);
		CHECK(parsed == document);
		return result.Data;
	}
}

TEST_CASE("Lattice Point 3D defaults replay the 27 x-fast source positions", "[imagegraph][point_nodes]") {
	Document document;
	document.Nodes.push_back({"lattice", std::string(LATTICE), "", {}, {}});
	document.Outputs.push_back({"points", "lattice", "points"});
	const auto actual = std::get<ArrayValue>(Replay(document));
	ArrayValue expected{ValueType::Scalar, {}};
	for (double z : {-1., 0., 1.})
		for (double y : {-1., 0., 1.})
			for (double x : {-1., 0., 1.})
				expected.Nested.push_back({x, y, z});
	CHECK(actual == expected);
}

TEST_CASE(
	"Lattice Point 3D asymmetric corners retain order independently of seed", "[imagegraph][point_nodes]"
) {
	const auto expected =
		Points({{1, 2, 3}, {3, 2, 3}, {1, 6, 3}, {3, 6, 3}, {1, 2, 9}, {3, 2, 9}, {1, 6, 9}, {3, 6, 9}});
	for (double seed : {0., 913.}) {
		const auto run = RunNode(
			LATTICE,
			{},
			{{"center", Vector3{2, 4, 6}},
			 {"half_size", Vector3{1, 2, 3}},
			 {"subdivision", Vector3{1, 1, 1}},
			 {"seed", seed}}
		);
		REQUIRE(run.Ok);
		CHECK(*run.OutputValue("points") == Value{expected});
	}
	const auto reversed = RunNode(
		LATTICE,
		{},
		{{"center", Vector3{2, 4, 6}}, {"half_size", Vector3{-1, 2, 3}}, {"subdivision", Vector3{1, 1, 1}}}
	);
	REQUIRE(reversed.Ok);
	const auto &points = std::get<ArrayValue>(*reversed.OutputValue("points"));
	CHECK(points.Nested.front() == Points({{3, 2, 3}}).Nested.front());
	CHECK(points.Nested.back() == Points({{1, 6, 9}}).Nested.front());
}

TEST_CASE(
	"Lattice Point 3D singleton axes use the midpoint and negative axes empty the grid",
	"[imagegraph][point_nodes]"
) {
	const auto singleton = RunNode(
		LATTICE,
		{},
		{{"center", Vector3{2, 4, 6}}, {"half_size", Vector3{1, 2, 3}}, {"subdivision", Vector3{0, 0, 0}}}
	);
	REQUIRE(singleton.Ok);
	CHECK(*singleton.OutputValue("points") == Value{Points({{2, 4, 6}})});
	const auto broadcast = RunNode(
		LATTICE,
		{},
		{{"center", 2.0},
		 {"half_size", int64_t{3}},
		 {"subdivision", ArrayValue{ValueType::Integer, {int64_t{0}, int64_t{0}, int64_t{0}}}}}
	);
	REQUIRE(broadcast.Ok);
	CHECK(*broadcast.OutputValue("points") == Value{Points({{2, 2, 2}})});
	const auto mixed = RunNode(
		LATTICE,
		{},
		{{"center", Vector3{2, 4, 6}}, {"half_size", Vector3{1, 2, 3}}, {"subdivision", Vector3{0, 1, 0}}}
	);
	REQUIRE(mixed.Ok);
	CHECK(*mixed.OutputValue("points") == Value{Points({{2, 2, 6}, {2, 6, 6}})});
	for (const Vector3 subdivisions : {Vector3{-1, 2, 2}, Vector3{2, -2, 2}, Vector3{1e20, 2, -1}}) {
		const auto empty = RunNode(LATTICE, {}, {{"subdivision", subdivisions}});
		REQUIRE(empty.Ok);
		CHECK(std::get<ArrayValue>(*empty.OutputValue("points")).Nested.empty());
	}
}

TEST_CASE("Lattice Point 3D consumes a persisted real Vector3 link", "[imagegraph][point_nodes]") {
	Document document;
	// Authored Vector3 values require the v6 document grammar.
	document.FormatVersion = 6;
	document.Nodes = {
		{"center", "pc.vector3", "", {}, {{"x", 2.0}, {"y", 4.0}, {"z", 6.0}}},
		{"lattice", std::string(LATTICE), "", {}, {{"subdivision", Vector3{0, 0, 0}}}}
	};
	document.Links.push_back({"center", "vector", "lattice", "center"});
	document.Outputs.push_back({"points", "lattice", "points"});
	const Value actual = Replay(document);
	CHECK(actual == Value{Points({{2, 4, 6}})});
}

TEST_CASE(
	"Lattice Point 3D refuses unproved fractional sizing and bounded allocation overflow",
	"[imagegraph][point_nodes]"
) {
	for (const auto &[subdivision, status] : std::array{
			 std::pair{Vector3{.5, 1, 1}, Status::UnsupportedExecution},
			 std::pair{Vector3{100, 100, 100}, Status::LimitExceeded},
			 std::pair{Vector3{std::numeric_limits<double>::infinity(), 1, 1}, Status::InvalidValue}
		 }) {
		const auto run = RunNode(LATTICE, {}, {{"subdivision", subdivision}});
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == status);
		CHECK(run.Port == "subdivision");
		CHECK(run.Values.empty());
	}
	for (const Value &invalid :
		 {Value{std::string("not a vector")},
		  Value{ArrayValue{ValueType::Scalar, {1.0, 2.0}}},
		  Value{ArrayValue{ValueType::Text, {std::string("x"), std::string("y"), std::string("z")}}}}) {
		const auto run = RunNode(LATTICE, {}, {{"center", invalid}});
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::TypeMismatch);
		CHECK(run.Port == "center");
		CHECK(run.Values.empty());
	}
	const auto overflow =
		RunNode(LATTICE, {}, {{"center", Vector3{1e308, 0, 0}}, {"half_size", Vector3{1e308, 1, 1}}});
	CHECK_FALSE(overflow.Ok);
	CHECK(overflow.Code == Status::InvalidValue);
	CHECK(overflow.Port == "points");
	CHECK(overflow.Values.empty());
}

TEST_CASE(
	"Lattice Point 3D output admission includes previous retained data",
	"[imagegraph][point_nodes][allocation_ledger]"
) {
	const auto *entry = FindCatalogueEntry(LATTICE);
	const auto executor = detail::FindExecutor(LATTICE);
	REQUIRE(entry);
	REQUIRE(executor);
	const Node authored{"lattice", std::string(LATTICE), "", {}, {}};
	const EvaluationRequest request;
	uint64_t peak = 0;
	const auto run = [&](uint64_t maximum, bool accepted) {
		detail::EvaluationBudget ledger(maximum);
		{
			auto previousCharge = ledger.Reserve(8 * sizeof(double));
			REQUIRE(previousCharge);
			std::vector<double> previous(8, 5);
			{
				detail::NodeContext context(authored, *entry, request, ledger);
				context.ByteBudget = maximum;
				CHECK(executor(context) == accepted);
				if (accepted) {
					REQUIRE(context.FailureCode == Status::Ok);
					CHECK(std::get<ArrayValue>(context.OutputValues.front().Data).Nested.size() == 27);
					peak = ledger.Peak();
				} else {
					CHECK(context.FailureCode == Status::LimitExceeded);
					CHECK(context.OutputValues.empty());
				}
				CHECK(previous.front() == 5);
			}
			CHECK(ledger.Used() == previousCharge->Bytes());
		}
		CHECK(ledger.Used() == 0);
	};
	run(Limits::MaximumEvaluationBytes, true);
	REQUIRE(peak > 0);
	run(peak - 1, false);
}
