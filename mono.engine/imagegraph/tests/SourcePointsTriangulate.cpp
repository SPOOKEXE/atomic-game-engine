#include "NodeExecutors.hpp"
#include "nodes/SourcePolygon2D.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <limits>
#include <set>

TEST_SUITE_ID("engine.imagegraph.source_points_triangulate")
using namespace engine::imagegraph;
namespace {
	using Triangle = std::array<std::array<double, 3>, 3>;
	std::set<Triangle> Coordinates(const ArrayValue &array) {
		CHECK(array.ElementType == ValueType::Any);
		std::set<Triangle> triangles;
		for (const auto &item : array.Items) {
			const auto &corners = std::get<std::vector<SourceArrayItem>>(item.Data);
			REQUIRE(corners.size() == 3);
			Triangle triangle;
			for (size_t corner = 0; corner < 3; ++corner) {
				const auto &coordinates = std::get<std::vector<SourceArrayItem>>(corners[corner].Data);
				REQUIRE(coordinates.size() == 3);
				for (size_t component = 0; component < 3; ++component)
					triangle[corner][component] =
						std::get<double>(std::get<ElementValue>(coordinates[component].Data));
				CHECK(triangle[corner][2] == 1);
			}
			std::sort(triangle.begin(), triangle.end());
			triangles.insert(triangle);
		}
		return triangles;
	}
	Triangle Expected(std::array<double, 3> a, std::array<double, 3> b, std::array<double, 3> c) {
		Triangle triangle{a, b, c};
		std::sort(triangle.begin(), triangle.end());
		return triangle;
	}
	ArrayValue Points(std::initializer_list<Vector2> points) {
		ArrayValue input{ValueType::Vector2, {}};
		for (const auto &point : points)
			input.Elements.emplace_back(point);
		return input;
	}
	EvaluatedValue Run(Value input) {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {{"triangulate", "pc.points_triangulate", "", {}, {{"points", std::move(input)}}}};
		document.Outputs = {{"out", "triangulate", "triangles"}};
		Plan plan;
		Diagnostic diagnostic;
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		EvaluatedValue output;
		const auto status = EvaluateValue(document, plan, "out", {}, output, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		REQUIRE(std::holds_alternative<ArrayValue>(output.Data));
		return output;
	}
}
TEST_CASE(
	"Triangulate retains weighted numeric corners and excludes no concave hull region",
	"[imagegraph][triangulate]"
) {
	const auto input = Points({{0, 0}, {4, 0}, {1, 1}, {4, 4}, {0, 4}});
	const auto output = Run(input);
	const std::set<Triangle> expected{
		Expected({0, 0, 1}, {4, 0, 1}, {1, 1, 1}),
		Expected({4, 0, 1}, {4, 4, 1}, {1, 1, 1}),
		Expected({4, 4, 1}, {0, 4, 1}, {1, 1, 1}),
		Expected({0, 4, 1}, {0, 0, 1}, {1, 1, 1})
	};
	CHECK(Coordinates(std::get<ArrayValue>(output.Data)) == expected);
	REQUIRE(output.Domain);
	CHECK(output.Domain->Type == ValueType::Scalar);
	CHECK(output.Domain->Kind == SourceSocketKind::Float);
	CHECK(output.Domain->Display == SourceValueDisplay::Vector);
	const std::array<Vector2, 5> polygon{{{0, 0}, {4, 0}, {1, 1}, {4, 4}, {0, 4}}};
	std::vector<std::array<uint32_t, 3>> filtered;
	REQUIRE(detail::polygon2d::Delaunay(polygon, filtered));
	CHECK(filtered.size() == 3);
}
TEST_CASE(
	"Triangulate normalizes coordinate rows and preserves source empty degeneracies",
	"[imagegraph][triangulate]"
) {
	ArrayValue rows{ValueType::Scalar, {}};
	rows.Nested = {{0., 0.}, {4., 0.}, {0., 4.}};
	const auto output = Run(rows);
	CHECK(
		Coordinates(std::get<ArrayValue>(output.Data)) ==
		std::set<Triangle>{Expected({0, 0, 1}, {4, 0, 1}, {0, 4, 1})}
	);
	for (Value input :
		 {Value{Vector2{2, 3}},
		  Value{ArrayValue{ValueType::Scalar, {2., 3.}}},
		  Value{Points({})},
		  Value{Points({{0, 0}, {1, 0}, {2, 0}})},
		  Value{Points({{0, 0}, {0, 0}, {0, 0}})}}) {
		const auto result = Run(std::move(input));
		CHECK(std::get<ArrayValue>(result.Data).Items.empty());
	}
	const auto duplicates = Run(Points({{0, 0}, {4, 0}, {0, 4}, {0, 0}}));
	CHECK(
		Coordinates(std::get<ArrayValue>(duplicates.Data)) ==
		std::set<Triangle>{Expected({0, 0, 1}, {4, 0, 1}, {0, 4, 1})}
	);
}
TEST_CASE("Triangulate admits byte and whole batch work before publication", "[imagegraph][triangulate]") {
	const Node node{"triangulate", "pc.points_triangulate", "", {}, {}};
	const auto *entry = FindCatalogueEntry(node.Type);
	const auto executor = detail::FindExecutor(node.Type);
	REQUIRE(entry);
	REQUIRE(executor);
	EvaluationRequest request;
	for (int refusal = 0; refusal < 4; ++refusal) {
		detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
		detail::NodeContext context(node, *entry, request, budget);
		context.ByteBudget = refusal == 0 ? 1 : Limits::MaximumEvaluationBytes;
		context.ProcessorCount = refusal == 1 ? 1000000 : 1;
		auto points = Points({{0, 0}, {4, 0}, {0, 4}});
		if (refusal == 2) points.Elements[0] = Vector2{std::numeric_limits<double>::infinity(), 0};
		if (refusal == 3) points.Elements[0] = Vector2{double(INT32_MAX), 0};
		context.Values = {{"points", std::move(points)}};
		CHECK_FALSE(executor(context));
		CHECK(context.FailureCode == (refusal == 2 ? Status::InvalidValue : Status::LimitExceeded));
		CHECK(context.OutputValues.empty());
		CHECK(budget.Used() == 0);
	}
}
TEST_CASE(
	"Triangulate weighted corners cross an Array Get link without vector truncation",
	"[imagegraph][triangulate]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"triangulate", "pc.points_triangulate", "", {}, {{"points", Points({{0, 0}, {4, 0}, {0, 4}})}}},
		{"triangle", "pc.array_get", "", {}, {{"index", int64_t{0}}}},
		{"corner", "pc.array_get", "", {}, {{"index", int64_t{0}}}}
	};
	document.Links = {
		{"triangulate", "triangles", "triangle", "array"}, {"triangle", "value", "corner", "array"}
	};
	document.Outputs = {{"out", "corner", "value"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluatedValue output;
	const auto status = EvaluateValue(document, plan, "out", {}, output, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	const auto &coordinates = std::get<ArrayValue>(output.Data);
	REQUIRE(coordinates.Elements.size() == 3);
	CHECK(std::get<double>(coordinates.Elements[2]) == 1);
}
