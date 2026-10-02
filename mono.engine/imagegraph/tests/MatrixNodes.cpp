// Source-derived fixtures for deterministic matrix nodes.

#include "NodeHarness.hpp"

#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <numbers>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.matrix_nodes")

using namespace engine::imagegraph;
using imagegraph_test::RunNode;

TEST_CASE("2D transform matrix follows the pinned affine and linear formulas", "[imagegraph]") {
	const double halfPi = std::numbers::pi / 2.0;
	const auto affine = RunNode(
		"pc.matrix_transform_2_d",
		{},
		{{"position", Vector2{7, -3}}, {"rotation", halfPi}, {"scale", Vector2{2, 3}}}
	);
	REQUIRE(affine.Ok);
	const auto &matrix = std::get<MatrixValue>(*affine.OutputValue("matrix"));
	REQUIRE(matrix.Columns == 3);
	REQUIRE(matrix.Rows == 3);
	CHECK(std::abs(matrix.Values[0]) < 1e-15);
	CHECK(matrix.Values[1] == -3.0);
	CHECK(matrix.Values[3] == 2.0);
	CHECK(std::abs(matrix.Values[4]) < 1e-15);
	CHECK(matrix.Values[6] == 7.0);
	CHECK(matrix.Values[7] == -3.0);
	CHECK(matrix.Values[8] == 1.0);

	const auto linear = RunNode(
		"pc.matrix_transform_2_d",
		{},
		{{"affine", false}, {"position", Vector2{7, -3}}, {"rotation", 0.0}, {"scale", Vector2{2, 3}}}
	);
	REQUIRE(linear.Ok);
	CHECK(*linear.OutputValue("matrix") == Value{MatrixValue{3, 3, {2, 0, 0, 0, 3, 0, 0, 0, 0}}});
}

TEST_CASE("matrix math resolves source menu indices including its separator", "[imagegraph]") {
	const MatrixValue left{2, 2, {1, 2, 3, 4}};
	const MatrixValue right{2, 2, {5, 6, 7, 8}};
	const auto run = [&](int64_t operation, double scalar = 2.0) {
		return RunNode(
			"pc.matrix_math",
			{},
			{{"matrix_1", left}, {"matrix_2", right}, {"operation", EnumValue{operation}}, {"scala", scalar}}
		);
	};
	const auto add = run(0);
	REQUIRE(add.Ok);
	CHECK(*add.OutputValue("matrix") == Value{MatrixValue{2, 2, {6, 8, 10, 12}}});
	const auto subtract = run(1);
	REQUIRE(subtract.Ok);
	CHECK(*subtract.OutputValue("matrix") == Value{MatrixValue{2, 2, {-4, -4, -4, -4}}});
	const auto multiplyScalar = run(2);
	REQUIRE(multiplyScalar.Ok);
	CHECK(*multiplyScalar.OutputValue("matrix") == Value{MatrixValue{2, 2, {2, 4, 6, 8}}});
	const auto divideScalar = run(3, 2.0);
	REQUIRE(divideScalar.Ok);
	CHECK(*divideScalar.OutputValue("matrix") == Value{MatrixValue{2, 2, {0.5, 1, 1.5, 2}}});
	const auto multiplyMatrix = run(5);
	REQUIRE(multiplyMatrix.Ok);
	CHECK(*multiplyMatrix.OutputValue("matrix") == Value{MatrixValue{2, 2, {19, 22, 43, 50}}});
	const auto separator = run(4);
	CHECK_FALSE(separator.Ok);
	CHECK(separator.Code == Status::UnsupportedExecution);
	const auto zeroDivisor = run(3, 0.0);
	CHECK_FALSE(zeroDivisor.Ok);
	CHECK(zeroDivisor.Code == Status::InvalidValue);
}

TEST_CASE("matrix math keeps source dimension mismatch behavior", "[imagegraph]") {
	const MatrixValue left{2, 3, {1, 2, 3, 4, 5, 6}};
	const MatrixValue right{2, 2, {7, 8, 9, 10}};
	const auto product =
		RunNode("pc.matrix_math", {}, {{"matrix_1", left}, {"matrix_2", right}, {"operation", EnumValue{5}}});
	REQUIRE(product.Ok);
	CHECK(*product.OutputValue("matrix") == Value{left});
}

TEST_CASE("matrix crop applies zero repeat and clamp overflow", "[imagegraph]") {
	const MatrixValue input{3, 2, {1, 2, 3, 4, 5, 6}};
	const auto crop = [&](int64_t overflow) {
		return RunNode(
			"pc.matrix_crop",
			{},
			{{"matrix", input},
			 {"size", Vector2{3, 2}},
			 {"offset", Vector2{-1, -1}},
			 {"overflow", EnumValue{overflow}}}
		);
	};
	const auto zero = crop(0);
	REQUIRE(zero.Ok);
	CHECK(*zero.OutputValue("matrix") == Value{MatrixValue{3, 2, {0, 0, 0, 0, 1, 2}}});
	const auto repeat = crop(1);
	REQUIRE(repeat.Ok);
	CHECK(*repeat.OutputValue("matrix") == Value{MatrixValue{3, 2, {6, 4, 5, 3, 1, 2}}});
	const auto clamp = crop(2);
	REQUIRE(clamp.Ok);
	CHECK(*clamp.OutputValue("matrix") == Value{MatrixValue{3, 2, {1, 1, 2, 1, 1, 2}}});
	const auto tooLarge = RunNode("pc.matrix_crop", {}, {{"matrix", input}, {"size", Vector2{4097, 1}}});
	CHECK_FALSE(tooLarge.Ok);
	CHECK(tooLarge.Code == Status::LimitExceeded);
}

TEST_CASE("matrix get and set preserve typed shape and authored input", "[imagegraph]") {
	const MatrixValue input{3, 2, {1, 2, 3, 4, 5, 6}};
	const auto one = RunNode("pc.matrix_get", {}, {{"matrix", input}, {"position", Vector2{1, 1}}});
	REQUIRE(one.Ok);
	CHECK(*one.OutputValue("output") == Value{5.0});
	const ArrayValue positions{ValueType::Vector2, {Vector2{2, 0}, Vector2{0, 1}}};
	const auto many = RunNode("pc.matrix_get", {}, {{"matrix", input}, {"position", positions}});
	REQUIRE(many.Ok);
	CHECK(*many.OutputValue("output") == Value{ArrayValue{ValueType::Scalar, {3.0, 4.0}}});
	const auto missing = RunNode("pc.matrix_get", {}, {{"matrix", input}, {"position", Vector2{-1, 0}}});
	CHECK_FALSE(missing.Ok);
	CHECK(missing.Code == Status::UnsupportedExecution);

	const auto set =
		RunNode("pc.matrix_set", {}, {{"matrix", input}, {"position", Vector2{1, 1}}, {"value", 20.0}});
	REQUIRE(set.Ok);
	CHECK(*set.OutputValue("matrix") == Value{MatrixValue{3, 2, {1, 2, 3, 4, 20, 6}}});
	CHECK(input == MatrixValue{3, 2, {1, 2, 3, 4, 5, 6}});
	const auto setMany = RunNode(
		"pc.matrix_set",
		{},
		{{"matrix", input}, {"position", positions}, {"value", ArrayValue{ValueType::Scalar, {30.0, 40.0}}}}
	);
	REQUIRE(setMany.Ok);
	CHECK(*setMany.OutputValue("matrix") == Value{MatrixValue{3, 2, {1, 2, 30, 40, 5, 6}}});
}

TEST_CASE("matrix vector getters and setters preserve source row column and diagonal order", "[imagegraph]") {
	const MatrixValue input{3, 2, {1, 2, 3, 4, 5, 6}};
	const auto get = [&](int64_t direction, Value position) {
		return RunNode(
			"pc.matrix_get_vector",
			{},
			{{"matrix", input}, {"direction", EnumValue{direction}}, {"position", std::move(position)}}
		);
	};
	const auto row = get(0, int64_t{1});
	REQUIRE(row.Ok);
	CHECK(*row.OutputValue("matrix") == Value{ArrayValue{ValueType::Scalar, {4.0, 5.0, 6.0}}});
	const auto column = get(1, int64_t{2});
	REQUIRE(column.Ok);
	CHECK(*column.OutputValue("matrix") == Value{ArrayValue{ValueType::Scalar, {3.0, 6.0}}});
	const auto diagonal = get(2, int64_t{0});
	REQUIRE(diagonal.Ok);
	CHECK(*diagonal.OutputValue("matrix") == Value{ArrayValue{ValueType::Scalar, {1.0, 5.0}}});
	const auto inverseDiagonal = get(3, int64_t{0});
	REQUIRE(inverseDiagonal.Ok);
	CHECK(*inverseDiagonal.OutputValue("matrix") == Value{ArrayValue{ValueType::Scalar, {3.0, 5.0}}});
	const auto rows = get(0, ArrayValue{ValueType::Integer, {int64_t{0}, int64_t{1}}});
	REQUIRE(rows.Ok);
	CHECK(
		*rows.OutputValue("matrix") ==
		Value{ArrayValue{ValueType::Scalar, {}, {{1.0, 2.0, 3.0}, {4.0, 5.0, 6.0}}}}
	);

	const auto setRow = RunNode(
		"pc.matrix_set_vector",
		{},
		{{"matrix", input},
		 {"direction", EnumValue{0}},
		 {"position", int64_t{1}},
		 {"vector", ArrayValue{ValueType::Scalar, {7.0, 8.0, 9.0}}}}
	);
	REQUIRE(setRow.Ok);
	CHECK(*setRow.OutputValue("matrix") == Value{MatrixValue{3, 2, {1, 2, 3, 7, 8, 9}}});
	const auto setColumn = RunNode(
		"pc.matrix_set_vector",
		{},
		{{"matrix", input},
		 {"direction", EnumValue{1}},
		 {"position", int64_t{2}},
		 {"vector", ArrayValue{ValueType::Scalar, {70.0, 80.0}}}}
	);
	REQUIRE(setColumn.Ok);
	CHECK(*setColumn.OutputValue("matrix") == Value{MatrixValue{3, 2, {1, 2, 70, 4, 5, 80}}});
	const auto setDiagonal = RunNode(
		"pc.matrix_set_vector",
		{},
		{{"matrix", input},
		 {"direction", EnumValue{2}},
		 {"position", int64_t{2}},
		 {"vector", ArrayValue{ValueType::Scalar, {10.0, 50.0}}}}
	);
	REQUIRE(setDiagonal.Ok);
	CHECK(*setDiagonal.OutputValue("matrix") == Value{MatrixValue{3, 2, {10, 2, 3, 4, 50, 6}}});
	const auto setInverse = RunNode(
		"pc.matrix_set_vector",
		{},
		{{"matrix", input},
		 {"direction", EnumValue{3}},
		 {"position", int64_t{0}},
		 {"vector", ArrayValue{ValueType::Scalar, {11.0, 12.0}}}}
	);
	REQUIRE(setInverse.Ok);
	CHECK(*setInverse.OutputValue("matrix") == Value{MatrixValue{3, 2, {1, 2, 11, 4, 12, 6}}});
}

TEST_CASE("matrix vector getter rejects oversized aggregate output before publishing", "[imagegraph]") {
	const MatrixValue input{4096, 1, std::vector<double>(4096, 1.0)};
	const ArrayValue positions{ValueType::Integer, std::vector<ElementValue>(4096, int64_t{0})};
	const auto result = RunNode(
		"pc.matrix_get_vector", {}, {{"matrix", input}, {"direction", EnumValue{0}}, {"position", positions}}
	);
	CHECK_FALSE(result.Ok);
	CHECK(result.Code == Status::LimitExceeded);
	CHECK(result.OutputValue("matrix") == nullptr);

	const MatrixValue oversizedEmpty{std::numeric_limits<uint32_t>::max(), 0, {}};
	const auto invalidDimensions = RunNode(
		"pc.matrix_get_vector",
		{},
		{{"matrix", oversizedEmpty}, {"direction", EnumValue{0}}, {"position", int64_t{0}}}
	);
	CHECK_FALSE(invalidDimensions.Ok);
	CHECK(invalidDimensions.Code == Status::LimitExceeded);
}

TEST_CASE("matrix vector input scratch is admitted before its index allocation", "[imagegraph]") {
	const auto *entry = FindCatalogueEntry("pc.matrix_get_vector");
	REQUIRE(entry);
	Node node{"bounded", "pc.matrix_get_vector", "", {}, {}};
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = sizeof(int64_t) - 1;
	context.Values = {
		{"matrix", MatrixValue{2, 2, {1, 2, 3, 4}}},
		{"direction", EnumValue{0}},
		{"position", int64_t{0}}
	};
	const auto executor = detail::FindExecutor(node.Type);
	REQUIRE(executor);
	CHECK_FALSE(executor(context));
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.FailurePort == "position");
	CHECK(context.OutputValues.empty());
}

TEST_CASE("matrix vector arrays execute through compiled document links", "[imagegraph]") {
	Document document;
	document.FormatVersion = 8;
	document.Nodes = {
		{"source",
		 "pc.matrix",
		 "",
		 {},
		 {{"size", Vector2{3, 2}}, {"data", MatrixValue{3, 2, {1, 2, 3, 4, 5, 6}}}}},
		{"get_rows",
		 "pc.matrix_get_vector",
		 "",
		 {},
		 {{"direction", EnumValue{0}},
		  {"position", ArrayValue{ValueType::Integer, {int64_t{0}, int64_t{1}}}}}},
		{"set_columns",
		 "pc.matrix_set_vector",
		 "",
		 {},
		 {{"direction", EnumValue{1}},
		  {"position", ArrayValue{ValueType::Integer, {int64_t{0}, int64_t{1}}}}}},
	};
	document.Links = {
		{"source", "matrix", "get_rows", "matrix"},
		{"source", "matrix", "set_columns", "matrix"},
		{"get_rows", "matrix", "set_columns", "vector"},
	};
	document.Outputs.push_back({"out", "set_columns", "matrix"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluatedValue value;
	const Status evaluated = EvaluateValue(document, plan, "out", EvaluationRequest{}, value, diagnostic);
	INFO(diagnostic.NodeId << " " << diagnostic.Port << " " << diagnostic.Message);
	REQUIRE(evaluated == Status::Ok);
	CHECK(value.Data == Value{MatrixValue{3, 2, {1, 4, 3, 2, 5, 6}}});
}
