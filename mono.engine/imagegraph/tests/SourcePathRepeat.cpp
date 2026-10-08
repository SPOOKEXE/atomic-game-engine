#include "nodes/Path.hpp"

#include <engine/imagegraph/Catalogue.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <limits>

TEST_SUITE_ID("engine.imagegraph.source_path_repeat")
using namespace engine::imagegraph;
namespace source_repeat_test {
	Path2D Line(double length = 10, double y = 0, double weight = 3) {
		Path2D path;
		path.Anchors = {{{0, y, 0, 0, 0, 0}, 0}, {{length, y, 0, 0, 0, 0}, 0}};
		path.Weights = {{0, weight}, {100, weight}};
		return path;
	}
	struct Graph {
		Document Doc;
		Plan Compiled;
		Diagnostic Error;
		Graph() {
			Doc.FormatVersion = 9;
			Doc.Nodes = {
				{"repeat", "pc.path_repeat", "", {}, {{"path", Line()}, {"amount", int64_t{3}}}},
				{"sample", "pc.path_sample", "", {}, {{"ratio", .5}}}
			};
			for (const auto *port :
				 {"center_unit", "radius_unit", "position_unit", "shift_position_unit", "anchor_unit"})
				Set(port, EnumValue{0});
			Doc.Links = {{"repeat", "path", "sample", "path"}};
			Doc.Outputs = {
				{"path", "repeat", "path"}, {"point", "sample", "position"}, {"weight", "sample", "weight"}
			};
		}
		void Set(std::string_view port, Value value) {
			for (auto &input : Doc.Nodes[0].Values)
				if (input.Port == port) {
					input.Data = std::move(value);
					return;
				}
			Doc.Nodes[0].Values.push_back({std::string(port), std::move(value)});
		}
		void Sample(double ratio, int64_t line = 0, int64_t mode = 0) {
			Doc.Nodes[1].Values = {{"ratio", ratio}, {"path_index", line}, {"type", EnumValue{mode}}};
		}
		Value Run(std::string_view output = "path", EvaluationRequest request = {}) {
			const auto compiled = Compile(Doc, Compiled, Error);
			INFO(Error.Message);
			REQUIRE(compiled == Status::Ok);
			EvaluatedValue value;
			const auto status = EvaluateValue(Doc, Compiled, std::string(output), request, value, Error);
			INFO(Error.Message);
			REQUIRE(status == Status::Ok);
			return value.Data;
		}
	};
	struct Runtime {
		Node Authored{"sample", "pc.path_sample", "", {}, {}};
		EvaluationRequest Request;
		detail::NodeContext Context{Authored, *FindCatalogueEntry("pc.path_sample"), Request};
		Path2D Owned;
		detail::PathRuntime Path;
		explicit Runtime(const Path2D &path) : Owned(path) {
			Context.ByteBudget = Limits::MaximumEvaluationBytes;
			const auto initialized = Path.Init(Context, Owned);
			INFO(Context.FailureMessage);
			REQUIRE(initialized);
		}
	};
	void Near(const Value &value, double x, double y) {
		const auto &point = std::get<Vector2>(value);
		CHECK(point.X == Catch::Approx(x).margin(1e-8));
		CHECK(point.Y == Catch::Approx(y).margin(1e-8));
	}
	void Refused(Graph &graph, Status expected) {
		const auto compiled = Compile(graph.Doc, graph.Compiled, graph.Error);
		INFO(graph.Error.Message << " " << graph.Error.NodeId << ":" << graph.Error.Port);
		REQUIRE(compiled == Status::Ok);
		EvaluatedValue output;
		output.Data = std::string{"retained"};
		const auto status = EvaluateValue(graph.Doc, graph.Compiled, "path", {}, output, graph.Error);
		INFO(graph.Error.Message);
		CHECK(status == expected);
		CHECK(std::get<std::string>(output.Data) == "retained");
	}
}
using namespace source_repeat_test;

TEST_CASE("Repeat applies rotation before nonuniform scale around its anchor", "[imagegraph][path_repeat]") {
	Graph graph;
	graph.Set("position", Vector2{7, 11});
	graph.Set("shift_position", Vector2{2, -3});
	graph.Set("rotation", 90.);
	graph.Set("shift_rotation", 90.);
	graph.Set("anchor", Vector2{1, 2});
	graph.Set("scale", Vector2{2, 3});
	graph.Set("shift_scale", Vector2{-2, .5});
	graph.Sample(.5, 0);
	Near(graph.Run("point"), 4, 1);
	graph.Sample(.5, 1);
	Near(graph.Run("point"), 26, 13);
	graph.Sample(.5, 2);
	Near(graph.Run("point"), 28, 10);
	CHECK(std::get<double>(graph.Run("weight")) == 3);
	const auto value = std::get<Path2D>(graph.Run());
	REQUIRE(value.SourceOperation);
	CHECK(value.SourceOperation->Kind == SourcePathOperationKind::Repeat);
	CHECK(value.Anchors.empty());
}

TEST_CASE("Circular repeat uses negative screen Y and optional rotation along", "[imagegraph][path_repeat]") {
	Graph graph;
	graph.Set("amount", int64_t{4});
	graph.Set("pattern", EnumValue{1});
	graph.Set("center", Vector2{20, 30});
	graph.Set("radius", Vector2{10, 6});
	graph.Set("position", Vector2{1, 2});
	const std::array<Vector2, 4> expected{{{36, 32}, {21, 21}, {6, 32}, {21, 43}}};
	for (int64_t line = 0; line < 4; ++line) {
		graph.Sample(.5, line);
		Near(graph.Run("point"), expected[line].X, expected[line].Y);
	}
	graph.Set("rotate_along", false);
	graph.Sample(.5, 1);
	Near(graph.Run("point"), 26, 26);
	graph.Set("shift_position", Vector2{2, 3});
	graph.Set("shift_rotation", 90.);
	graph.Sample(.5, 1);
	Near(graph.Run("point"), 23, 24);
}

TEST_CASE("Repeat keeps repeat major multiline order and source metrics", "[imagegraph][path_repeat]") {
	Path2D combined;
	auto &operation = combined.SourceOperation.emplace();
	operation.Kind = SourcePathOperationKind::Combine;
	operation.Inputs = {Line(10, 0, 3), Line(20, 4, 7)};
	Graph graph;
	graph.Set("path", combined);
	graph.Set("amount", int64_t{2});
	graph.Set("shift_position", Vector2{100, 0});
	graph.Set("scale", Vector2{3, -2});
	const std::array<Vector2, 4> expected{{{15, 0}, {30, -8}, {115, 0}, {130, -8}}};
	for (int64_t line = 0; line < 4; ++line) {
		graph.Sample(.5, line);
		Near(graph.Run("point"), expected[line].X, expected[line].Y);
		CHECK(std::get<double>(graph.Run("weight")) == (line % 2 ? 7 : 3));
	}
	Runtime runtime(std::get<Path2D>(graph.Run()));
	CHECK(runtime.Path.LineCount() == 4);
	for (size_t line = 0; line < 4; ++line) {
		const double length = line % 2 ? 20 : 10;
		CHECK(runtime.Path.Length(line) == Catch::Approx(length));
		CHECK(runtime.Path.SegmentCount(line) == 1);
		CHECK(runtime.Path.AccumulatedCount(line) == 1);
		CHECK(runtime.Path.AccumulatedAt(0, line) == Catch::Approx(length));
	}
	CHECK(runtime.Path.MinX == 0);
	CHECK(runtime.Path.MinY == 0);
	CHECK(runtime.Path.MaxX == 1);
	CHECK(runtime.Path.MaxY == 1);
	const auto firstDistance = runtime.Path.PointDistance(5, 1);
	CHECK(firstDistance.X == Catch::Approx(15));
	CHECK(firstDistance.Y == Catch::Approx(-8));
	const auto secondDistance = runtime.Path.PointDistance(5, 3);
	CHECK(secondDistance.X == Catch::Approx(115));
	CHECK(secondDistance.Y == Catch::Approx(-8));
	SourcePathPointBuffer distanceBuffer;
	runtime.Path.PointDistanceInto(5, 3, distanceBuffer);
	CHECK(distanceBuffer.Position.X == Catch::Approx(115));
	CHECK(distanceBuffer.Position.Y == Catch::Approx(-8));
	CHECK(distanceBuffer.Weight == 7);
	SourcePathPointBuffer retained;
	retained.Position = {123, 456};
	retained.Weight = 8;
	const auto before = retained;
	CHECK(runtime.Path.PointRatioInto(.5, 4, retained) == before);
	CHECK(retained == before);
	CHECK(runtime.Path.PointDistanceInto(5, 4, retained) == before);
	CHECK(retained == before);
}

TEST_CASE("Repeat zero amount and missing path retain an empty source getter", "[imagegraph][path_repeat]") {
	for (bool missing : {false, true}) {
		Graph graph;
		if (missing)
			graph.Doc.Nodes[0].Values.erase(graph.Doc.Nodes[0].Values.begin());
		else
			graph.Set("amount", int64_t{0});
		Runtime runtime(std::get<Path2D>(graph.Run()));
		CHECK(runtime.Path.LineCount() == 0);
		CHECK(runtime.Path.Length() == 0);
		CHECK(runtime.Path.SegmentCount() == 0);
		CHECK(runtime.Path.AccumulatedCount() == 0);
		SourcePathPointBuffer supplied;
		supplied.Position = {9, 12};
		supplied.Weight = 7;
		const auto before = supplied;
		CHECK(runtime.Path.PointRatioInto(.5, 0, supplied) == before);
		CHECK(supplied == before);
		Near(graph.Run("point"), 0, 0);
	}
}

TEST_CASE("Repeat processor arrays produce independent complete paths", "[imagegraph][path_repeat]") {
	Graph graph;
	graph.Set("amount", ArrayValue{ValueType::Integer, {int64_t{1}, int64_t{3}}});
	graph.Set("shift_position", ArrayValue{ValueType::Vector2, {Vector2{2, 0}, Vector2{7, 0}}});
	const auto rows = std::get<ArrayValue>(graph.Run());
	REQUIRE(rows.Elements.size() == 2);
	const auto &first = std::get<Path2D>(rows.Elements[0]);
	const auto &second = std::get<Path2D>(rows.Elements[1]);
	Runtime a(first), b(second);
	CHECK(a.Path.LineCount() == 1);
	CHECK(b.Path.LineCount() == 3);
	CHECK(a.Path.PointRatio(.5).X == 5);
	CHECK(b.Path.PointRatio(.5, 2).X == 19);
	auto copied = second;
	REQUIRE(copied.SourceOperation);
	copied.SourceOperation->Inputs.clear();
	CHECK(b.Path.LineCount() == 3);
}

TEST_CASE(
	"Repeat rejects invalid counts and finite arithmetic overflow atomically", "[imagegraph][path_repeat]"
) {
	Graph graph;
	SECTION("negative source allocation") {
		graph.Set("amount", int64_t{-1});
		Refused(graph, Status::UnsupportedExecution);
	}
	SECTION("count limit") {
		graph.Set("amount", int64_t{Limits::MaximumArrayElements + 1});
		Refused(graph, Status::LimitExceeded);
	}
	SECTION("position overflow") {
		graph.Set("position", Vector2{std::numeric_limits<double>::max(), 0});
		graph.Set("shift_position", Vector2{std::numeric_limits<double>::max(), 0});
		Refused(graph, Status::InvalidValue);
	}
	SECTION("scale overflow") {
		graph.Set("shift_scale", Vector2{std::numeric_limits<double>::max(), 1});
		Refused(graph, Status::InvalidValue);
	}
	SECTION("rotation overflow") {
		graph.Set("rotation", std::numeric_limits<double>::max());
		graph.Set("shift_rotation", std::numeric_limits<double>::max());
		Refused(graph, Status::InvalidValue);
	}
	SECTION("caller byte budget") {
		const auto &node = graph.Doc.Nodes[0];
		const auto *entry = FindCatalogueEntry(node.Type);
		const auto executor = detail::FindExecutor(node.Type);
		REQUIRE(entry);
		REQUIRE(executor);
		EvaluationRequest request;
		detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
		{
			detail::NodeContext context(node, *entry, request, budget);
			context.ByteBudget = 1;
			context.InputProvenanceResolved = true;
			for (const auto &input : node.Values)
				context.Values.emplace_back(input.Port, input.Data);
			CHECK_FALSE(executor(context));
			CHECK(context.FailureCode == Status::LimitExceeded);
			CHECK(context.OutputValues.empty());
		}
		CHECK(budget.Used() == 0);
	}
}

TEST_CASE("Repeat bounds the complete owned tree before publishing", "[imagegraph][path_repeat]") {
	Graph graph;
	SECTION("depth") {
		auto path = Line();
		for (size_t depth = 0; depth < Limits::MaximumArrayDepth - 1; ++depth) {
			Path2D parent;
			auto &operation = parent.SourceOperation.emplace();
			operation.Kind = SourcePathOperationKind::Reverse;
			operation.Inputs.push_back(std::move(path));
			path = std::move(parent);
		}
		graph.Set("path", path);
	}
	SECTION("line product") {
		Path2D path;
		auto &operation = path.SourceOperation.emplace();
		operation.Kind = SourcePathOperationKind::Combine;
		operation.Inputs.assign(100, Line());
		graph.Set("path", path);
		graph.Set("amount", int64_t{100});
	}
	SECTION("node product with few lines") {
		auto path = Line();
		for (size_t depth = 0; depth < 40; ++depth) {
			Path2D parent;
			auto &operation = parent.SourceOperation.emplace();
			operation.Kind = SourcePathOperationKind::Reverse;
			operation.Inputs.push_back(std::move(path));
			path = std::move(parent);
		}
		graph.Set("path", path);
		graph.Set("amount", int64_t{200});
	}
	SECTION("whole processor batch work") {
		Path2D dense;
		for (size_t i = 0; i < Limits::MaximumPathAnchors; ++i)
			dense.Anchors.push_back({{double(i), 0, 0, 0, 0, 0}, 0});
		graph.Set("path", dense);
		ArrayValue amounts{ValueType::Integer, {}};
		amounts.Elements.assign(64, int64_t{32});
		graph.Set("amount", amounts);
	}
	Refused(graph, Status::LimitExceeded);
}

TEST_CASE(
	"Repeat Reference units scale authored and numeric linked vectors by each project axis",
	"[imagegraph][path_repeat]"
) {
	const std::array<std::string_view, 5> controls{
		"center", "radius", "position", "shift_position", "anchor"
	};
	for (const auto port : controls) {
		for (int kind = 0; kind < 3; ++kind) {
			INFO(port << " input kind " << kind);
			Graph graph;
			graph.Doc.Project.emplace();
			graph.Doc.Project->SurfaceWidth = 20;
			graph.Doc.Project->SurfaceHeight = 40;
			graph.Set("amount", int64_t{4});
			graph.Set("center", Vector2{0, 0});
			graph.Set("radius", Vector2{0, 0});
			graph.Set(std::string(port) + "_unit", EnumValue{1});
			graph.Set(port, Vector2{.25, .75});
			if (kind != 0) {
				graph.Doc.Junctions = {
					{"control",
					 "",
					 kind == 1 ? ValueType::Vector2 : ValueType::Scalar,
					 kind == 1 ? Value{Vector2{.25, .75}} : Value{.25}}
				};
				graph.Doc.Links.push_back({"control", "value", "repeat", std::string(port)});
			}
			const double convertedY = kind == 2 ? 10 : 30;
			if (port == "center" || port == "radius") {
				graph.Set("pattern", EnumValue{1});
				graph.Set("rotate_along", false);
			}
			if (port == "anchor") {
				graph.Set("scale", Vector2{2, 3});
				Near(graph.Run("point"), 5, -2 * convertedY);
			} else if (port == "radius") {
				graph.Sample(.5, 0);
				Near(graph.Run("point"), 10, 0);
				graph.Sample(.5, 1);
				Near(graph.Run("point"), 5, -convertedY);
			} else {
				graph.Sample(.5, port == "shift_position" ? 1 : 0);
				Near(graph.Run("point"), 10, convertedY);
			}
		}
	}
}

TEST_CASE(
	"Repeat path linked vector controls sample pixel coordinates before Reference units",
	"[imagegraph][path_repeat]"
) {
	Path2D driver;
	driver.Anchors = {{{20, 6, 0, 0, 0, 0}, 0}, {{40, 6, 0, 0, 0, 0}, 0}};
	for (const auto port :
		 {"center", "radius", "position", "shift_position", "anchor", "scale", "shift_scale"}) {
		INFO(port);
		Graph graph;
		graph.Doc.Project.emplace();
		graph.Doc.Project->SurfaceWidth = 20;
		graph.Doc.Project->SurfaceHeight = 40;
		graph.Set("amount", int64_t{4});
		graph.Set("center", Vector2{0, 0});
		graph.Set("radius", Vector2{0, 0});
		graph.Set(port, Vector2{.25, .75});
		if (std::string_view(port) != "scale" && std::string_view(port) != "shift_scale")
			graph.Set(std::string(port) + "_unit", EnumValue{1});
		graph.Doc.Junctions = {{"driver", "", ValueType::Any, driver}};
		graph.Doc.Links.push_back({"driver", "value", "repeat", port});
		const std::string_view control = port;
		if (control == "center" || control == "radius") {
			graph.Set("pattern", EnumValue{1});
			graph.Set("rotate_along", false);
		}
		if (control == "scale" || control == "shift_scale") {
			graph.Set("path", Line(10, 2));
			graph.Sample(.5, control == "shift_scale" ? 1 : 0);
			Near(graph.Run("point"), 125, 12);
		} else if (control == "anchor") {
			graph.Set("scale", Vector2{2, 3});
			Near(graph.Run("point"), -15, -12);
		} else if (control == "radius") {
			graph.Sample(.5, 0);
			Near(graph.Run("point"), 30, 0);
			graph.Sample(.5, 1);
			Near(graph.Run("point"), 5, -6);
		} else {
			graph.Sample(.5, control == "shift_position" ? 1 : 0);
			Near(graph.Run("point"), 30, 6);
		}
	}
}

TEST_CASE(
	"Repeat refuses fractional pattern and unit choices without publishing", "[imagegraph][path_repeat]"
) {
	for (const auto port :
		 {"pattern", "center_unit", "radius_unit", "position_unit", "shift_position_unit", "anchor_unit"}) {
		INFO(port);
		Graph graph;
		graph.Doc.Junctions = {{"fractional", "", ValueType::Scalar, .5}};
		graph.Doc.Links.push_back({"fractional", "value", "repeat", port});
		Refused(graph, Status::UnsupportedExecution);
		CHECK(graph.Error.Port == port);
	}
}

TEST_CASE(
	"Repeat path driver uses local animated ratio through a group at fractional frames",
	"[imagegraph][path_repeat]"
) {
	Graph graph;
	graph.Doc.Project.emplace();
	graph.Doc.Project->SurfaceWidth = 20;
	graph.Doc.Project->SurfaceHeight = 40;
	graph.Doc.Timeline = TimelineSettings{11, 0, 10, "loop", 11};
	graph.Set("position", Vector2{.123, .987});
	graph.Set("position_unit", EnumValue{1});
	for (auto &node : graph.Doc.Nodes)
		node.GroupId = "group";
	Node input{
		"input",
		"pc.group_input",
		"group",
		{},
		{{"input_type", EnumValue{11}}, {"subtype", EnumValue{0}}, {"vector_size", EnumValue{0}}}
	};
	graph.Doc.Nodes.push_back(std::move(input));
	Group group{"group", "Group"};
	group.Ports = {{"input", "input/parent", PortDirection::Input, "input"}};
	graph.Doc.Groups = {group};
	Path2D driver;
	driver.Anchors = {{{20, 6, 0, 0, 0, 0}, 0}, {{40, 6, 0, 0, 0, 0}, 0}};
	graph.Doc.Junctions = {{"input/parent", "group", ValueType::Any, driver}};
	graph.Doc.Links.push_back({"input/parent", "value", "input", "parent_value"});
	graph.Doc.Links.push_back({"input", "value", "repeat", "position"});
	graph.Doc.Nodes[0].SourceAnimatedInputs = {"position"};
	SECTION("tuple animator") {
		graph.Doc.Keyframes = {
			{"repeat", "position", 0, Vector2{.25, 100}, "linear"},
			{"repeat", "position", 10, Vector2{.75, -100}, "linear"}
		};
	}
	SECTION("separated X animator overrides dormant tuple and Y") {
		graph.Doc.Keyframes = {{"repeat", "position", 0, Vector2{.99, .99}, "linear"}};
		auto &separated = graph.Doc.Nodes[0].SourceSeparatedVec2Animators.emplace();
		separated.Inputs.push_back({"position", {}});
		auto &axes = separated.Inputs[0].Axes;
		axes[0].Keys = {
			{"repeat", "position", 0, .25, "source", KeyframeEase{}},
			{"repeat", "position", 10, .75, "source", KeyframeEase{}}
		};
		axes[1].Keys = {
			{"repeat", "position", 0, 100., "source", KeyframeEase{}},
			{"repeat", "position", 10, -100., "source", KeyframeEase{}}
		};
	}
	EvaluationRequest request;
	request.Tick = 4;
	request.Subframe = .5;
	Near(graph.Run("point", request), 34.5, 6);
	request.Tick = 0;
	request.Subframe = 0;
	Near(graph.Run("point", request), 30, 6);
	request.Tick = 4;
	request.Subframe = .5;
	Near(graph.Run("point", request), 34.5, 6);
}

TEST_CASE(
	"Repeat rounds a fractional numeric link through its source Int getter", "[imagegraph][path_repeat]"
) {
	Graph graph;
	graph.Set("shift_position", Vector2{7, 0});
	graph.Doc.Junctions = {{"fractional", "", ValueType::Scalar, 1.5}};
	graph.Doc.Links.push_back({"fractional", "value", "repeat", "amount"});
	Runtime runtime(std::get<Path2D>(graph.Run()));
	CHECK(runtime.Path.LineCount() == 2);
	graph.Sample(.5, 1);
	Near(graph.Run("point"), 12, 0);
	SourcePathPointBuffer supplied;
	supplied.Position = {9, 12};
	const auto before = supplied;
	CHECK(runtime.Path.PointRatioInto(.5, 2, supplied) == before);
	CHECK(supplied == before);
}

TEST_CASE(
	"Repeat path linked controls preserve each missing constructor raw ratio", "[imagegraph][path_repeat]"
) {
	Path2D driver;
	driver.Anchors = {{{20, 6, 0, 0, 0, 0}, 0}, {{40, 6, 0, 0, 0, 0}, 0}};
	for (const auto port :
		 {"center", "radius", "position", "shift_position", "anchor", "scale", "shift_scale"}) {
		INFO(port);
		Graph graph;
		graph.Doc.Project.emplace();
		graph.Doc.Project->SurfaceWidth = 20;
		graph.Doc.Project->SurfaceHeight = 40;
		graph.Set("amount", int64_t{4});
		graph.Set("center", Vector2{0, 0});
		graph.Set("radius", Vector2{0, 0});
		const std::string_view control = port;
		std::erase_if(graph.Doc.Nodes[0].Values, [&](const auto &value) { return value.Port == control; });
		if (control != "scale" && control != "shift_scale")
			graph.Set(std::string(port) + "_unit", EnumValue{1});
		graph.Doc.Junctions = {{"driver", "", ValueType::Any, driver}};
		graph.Doc.Links.push_back({"driver", "value", "repeat", port});
		if (control == "center" || control == "radius") {
			graph.Set("pattern", EnumValue{1});
			graph.Set("rotate_along", false);
		}
		if (control == "scale" || control == "shift_scale") {
			graph.Set("path", Line(10, 2));
			graph.Sample(.5, control == "shift_scale" ? 1 : 0);
			Near(graph.Run("point"), 199.99, 12);
		} else if (control == "anchor") {
			graph.Set("scale", Vector2{2, 3});
			Near(graph.Run("point"), -10, -12);
		} else if (control == "radius") {
			Near(graph.Run("point"), 35, 0);
			graph.Sample(.5, 1);
			Near(graph.Run("point"), 5, -6);
		} else {
			graph.Sample(.5, control == "shift_position" ? 1 : 0);
			Near(graph.Run("point"), control == "center" ? 35 : 25, 6);
		}
	}
}

TEST_CASE("Repeat refuses an Any spatial path vector payload atomically", "[imagegraph][path_repeat]") {
	Graph graph;
	PathValue3D spatial;
	spatial.Data.emplace().Anchors = {{{0, 0, 3, 0, 0, 0, 0, 0, 0}, 0}, {{10, 0, 3, 0, 0, 0, 0, 0, 0}, 0}};
	graph.Doc.Junctions = {{"unsupported", "", ValueType::Any, spatial}};
	graph.Doc.Links.push_back({"unsupported", "value", "repeat", "position"});
	Refused(graph, Status::UnsupportedExecution);
	CHECK(graph.Error.Port == "position");
}

TEST_CASE(
	"Repeat refuses a valid runtime struct vector payload before publication", "[imagegraph][path_repeat]"
) {
	Graph graph;
	StructValue structure;
	structure.Data.emplace().Fields = {{"x", 7.}, {"y", 9.}};
	REQUIRE(detail::ValidValuePayload(Value{structure}, true));
	const auto &node = graph.Doc.Nodes[0];
	const auto *entry = FindCatalogueEntry(node.Type);
	const auto executor = detail::FindExecutor(node.Type);
	REQUIRE(entry);
	REQUIRE(executor);
	EvaluationRequest request;
	detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	{
		detail::NodeContext context(node, *entry, request, budget);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		context.InputProvenanceResolved = true;
		for (const auto &input : node.Values)
			context.Values.emplace_back(input.Port, input.Data);
		context.Values.emplace_back("position", structure);
		CHECK_FALSE(executor(context));
		CHECK(context.FailureCode == Status::UnsupportedExecution);
		CHECK(context.FailurePort == "position");
		CHECK(context.OutputValues.empty());
	}
	CHECK(budget.Used() == 0);
}

TEST_CASE(
	"Repeat surface linked vectors use physical dimensions for all seven controls",
	"[imagegraph][path_repeat]"
) {
	for (const auto port :
		 {"center", "radius", "position", "shift_position", "anchor", "scale", "shift_scale"}) {
		INFO(port);
		Graph graph;
		graph.Doc.Project.emplace();
		graph.Doc.Project->SurfaceWidth = 20;
		graph.Doc.Project->SurfaceHeight = 40;
		graph.Set("amount", int64_t{4});
		graph.Set("center", Vector2{0, 0});
		graph.Set("radius", Vector2{0, 0});
		const std::string_view control = port;
		if (control != "scale" && control != "shift_scale")
			graph.Set(std::string(port) + "_unit", EnumValue{1});
		graph.Doc.Nodes.push_back(
			{"surface",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{3}}, {"height", int64_t{7}}, {"colour", Colour{255, 255, 255, 255}}}}
		);
		graph.Doc.Links.push_back({"surface", "image", "repeat", port});
		if (control == "center" || control == "radius") {
			graph.Set("pattern", EnumValue{1});
			graph.Set("rotate_along", false);
		}
		if (control == "scale" || control == "shift_scale") {
			graph.Set("path", Line(10, 2));
			graph.Sample(.5, control == "shift_scale" ? 1 : 0);
			Near(graph.Run("point"), 15, 14);
		} else if (control == "anchor") {
			graph.Set("scale", Vector2{2, 3});
			Near(graph.Run("point"), 7, -14);
		} else if (control == "radius") {
			Near(graph.Run("point"), 8, 0);
			graph.Sample(.5, 1);
			Near(graph.Run("point"), 5, -7);
		} else {
			graph.Sample(.5, control == "shift_position" ? 1 : 0);
			Near(graph.Run("point"), 8, 7);
		}
	}
}
