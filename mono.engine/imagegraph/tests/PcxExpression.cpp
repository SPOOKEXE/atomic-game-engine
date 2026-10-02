#include "NodeHarness.hpp"

#include <engine/imagegraph/PcxExpression.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.pcx_expression")
using namespace engine::imagegraph;
namespace {
	Value Evaluate(std::string_view source, std::span<const AuthoredValue> parameters = {}) {
		PcxExpressionValue tree;
		Diagnostic diagnostic;
		REQUIRE(CompilePcxExpression(source, tree, diagnostic) == Status::Ok);
		EvaluationRequest request;
		PcxExecutionContext context{request, parameters, nullptr, {32, 32}, {}};
		PcxExecutionResult result;
		INFO(diagnostic.Message);
		REQUIRE(ExecutePcxExpression(tree, context, result, diagnostic) == Status::Ok);
		return result.Data;
	}
	double Number(std::string_view source) {
		return std::get<double>(Evaluate(source));
	}
}
TEST_CASE("PCX source precedence arrays and UTF8 indexing are preserved", "[imagegraph][pcx]") {
	CHECK(Number("2**3**2") == 64);
	CHECK(Number("2+3*4") == 14);
	CHECK(Number("1|2*4") == 12);
	CHECK(Number("2*0∸3") == -6);
	CHECK(Number("8/0") == 0);
	CHECK(Number("round(-2.5)") == -2);
	CHECK(Number("round(3.5)") == 4);
	CHECK(Number("[2,4][-1]") == 4);
	CHECK(std::get<std::string>(Evaluate("\"é中\"[-1]")) == "中");
	const auto range = std::get<ArrayValue>(Evaluate("3..1"));
	REQUIRE(range.Elements.size() == 3);
	CHECK(Number("length(3..1)") == 3);
	const auto sum = std::get<ArrayValue>(Evaluate("[1,2]+[3]"));
	REQUIRE(sum.Items.size() == 2);
	CHECK(std::get<double>(std::get<ElementValue>(sum.Items[0].Data)) == 4);
	CHECK(std::get<double>(std::get<ElementValue>(sum.Items[1].Data)) == 2);
	const auto inverted = std::get<ArrayValue>(Evaluate("~[0,1]"));
	REQUIRE(inverted.Items.size() == 2);
	CHECK(std::get<double>(std::get<ElementValue>(inverted.Items[0].Data)) == -1);
	CHECK(std::get<double>(std::get<ElementValue>(inverted.Items[1].Data)) == -2);
}
TEST_CASE("PCX native functions cover source numeric text colour and surfaces", "[imagegraph][pcx]") {
	const std::pair<std::string_view, double> functions[]{
		{"abs(-3)", 3},
		{"round(2.5)", 2},
		{"floor(1.9)", 1},
		{"ceil(1.1)", 2},
		{"fract(1.25)", .25},
		{"sign(-2)", -1},
		{"min(2,3)", 2},
		{"max(2,3)", 3},
		{"clamp(5,1,3)", 3},
		{"lerp(1,3,.5)", 2},
		{"sin(0)", 0},
		{"cos(0)", 1},
		{"tan(0)", 0},
		{"dsin(0)", 0},
		{"dcos(0)", 1},
		{"dtan(0)", 0},
		{"arcsin(0)", 0},
		{"arccos(1)", 0},
		{"arctan(0)", 0},
		{"arctan2(0,1)", 0},
		{"darcsin(0)", 0},
		{"darccos(1)", 0},
		{"darctan(0)", 0},
		{"darctan2(0,1)", 0},
		{"random(2,2)", 2},
		{"irandom(2,2)", 2},

		{"number(\"12\")", 12},
		{"ord(chr(233))", 233},
		{"length(range(3,1))", 3},
		{"color_rgb(255,0,0)", 255},
		{"color_hsv(0,255,255)", 255},
		{"color_hex(\"ff0000\")", 255}
	};
	for (const auto &[source, expected] : functions) {
		INFO(source);
		CHECK(Number(source) == Catch::Approx(expected));
	}
	CHECK(std::get<std::string>(Evaluate("string(12)")) == "12");
	CHECK(std::get<std::string>(Evaluate("string(1.2)")) == "1.20");
	const auto wiggle = Number("wiggle(0,1,1,0)");
	CHECK(wiggle >= 0);
	CHECK(wiggle <= 1);
	CHECK(Number("wiggle(0,1,1,0)") == wiggle);
	CHECK(Number("color_hex(\"11223344\")") == double(0x44332211));
	Image image{2, 3, std::vector<uint8_t>(24, 255)};
	const std::array values{AuthoredValue{"surface", SurfaceValue{image}}};
	CHECK(std::get<Vector2>(Evaluate("surface_get_dimension(surface)", values)) == Vector2{2, 3});
	CHECK(std::get<double>(Evaluate("surface_get_width(surface)", values)) == 2);
	CHECK(std::get<double>(Evaluate("surface_get_height(surface)", values)) == 3);
}
TEST_CASE("PCX source program branches and both for forms share bounded variables", "[imagegraph][pcx]") {
	const std::string program =
		"sum=0\nfor(i=0:i+=1:i<4){\nsum+=i\n}\nfor(index,item:[2,4]){\nsum+=index+item\n}\nif(sum==13){\nsum="
		"20\n}elseif(sum==0){\nsum=30\n}else{\nsum=40\n}\nsum";
	PcxExpressionValue tree;
	Diagnostic diagnostic;
	REQUIRE(CompilePcxProgram(program, tree, diagnostic) == Status::Ok);
	EvaluationRequest request;
	PcxExecutionContext context{request, {}, nullptr, {32, 32}, {}};
	PcxExecutionResult result;
	INFO(diagnostic.Message);
	REQUIRE(ExecutePcxExpression(tree, context, result, diagnostic) == Status::Ok);
	CHECK(std::get<double>(result.Data) == 20);
	REQUIRE(CompilePcxProgram("for(i=0:i+=1:1){\n0\n}", tree, diagnostic) == Status::Ok);
	context.MaximumWork = 100;
	CHECK(ExecutePcxExpression(tree, context, result, diagnostic) == Status::LimitExceeded);
	CHECK(std::get<double>(result.Data) == 20);
}
TEST_CASE("PCX prints retain order and draw uses an explicit bounded target", "[imagegraph][pcx]") {
	PcxExpressionValue tree;
	Diagnostic diagnostic;
	REQUIRE(
		CompilePcxProgram("print(\"a\")\nprint(\"b\",1)\ndraw(surface,0,0)", tree, diagnostic) == Status::Ok
	);
	EvaluationRequest request;
	Image target{1, 1, {0, 0, 0, 0}};
	const std::array parameters{AuthoredValue{"surface", SurfaceValue{Image{1, 1, {255, 0, 0, 255}}}}};
	PcxExecutionContext context{request, parameters, nullptr, {32, 32}, {}};
	context.Target = &target;
	PcxExecutionResult result;
	REQUIRE(ExecutePcxExpression(tree, context, result, diagnostic) == Status::Ok);
	REQUIRE(result.Messages.size() == 2);
	CHECK(result.Messages[0].Text == "a");
	CHECK_FALSE(result.Messages[0].Warning);
	CHECK(result.Messages[1].Text == "b");
	CHECK(result.Messages[1].Warning);
	CHECK(target.Pixels == std::vector<uint8_t>{255, 0, 0, 255});
	for (const auto format : {SurfaceFormat::R8Unorm, SurfaceFormat::R16Float, SurfaceFormat::R32Float}) {
		Image single;
		single.Width = single.Height = 1;
		single.Format = format;
		single.Pixels.resize(DescribeSurfaceFormat(format)->BytesPerPixel);
		REQUIRE(StoreSurfacePixel(single, 0, 0, {.5, 0, 0, 1}));
		const std::array greyParameters{AuthoredValue{"surface", SurfaceValue{single}}};
		context.Parameters = greyParameters;
		target.Pixels = {0, 0, 0, 0};
		REQUIRE(ExecutePcxExpression(tree, context, result, diagnostic) == Status::Ok);
		CHECK(target.Pixels == std::vector<uint8_t>{128, 128, 128, 255});
	}
	context.Parameters = parameters;
	context.Target = nullptr;
	CHECK(ExecutePcxExpression(tree, context, result, diagnostic) == Status::UnsupportedExecution);
}
TEST_CASE("PCX compile routes serialized internal names and animated global roots", "[imagegraph][pcx]") {
	Document document;
	document.FormatVersion = 9;
	Node consumer{"consumer", "pc.equation", "", {}, {}};
	consumer.Values = {{"equation", std::string{"source.outputs.result+answer"}}};
	Node producer{"source-node", "pc.equation", "", {}, {}};
	producer.Values = {{"equation", std::string{"3"}}};
	producer.SourceInternalName = "source";
	producer.SourceDisplayName = "Source";
	Node globals{"globals", "pc.global_scope", "", {}, {}};
	globals.DynamicInputs = {DynamicInput{"answer", ValueType::Scalar, Value{double{4}}}};
	document.Nodes = {consumer, producer, globals};
	document.ProjectGlobalNodeId = "globals";
	document.Outputs = {{"value", "consumer", "result"}};
	Plan plan;
	Diagnostic diagnostic;
	INFO(diagnostic.Message);
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(plan.NodeOrder.size() == 3);
	CHECK(plan.NodeOrder.back() == 0);
	EvaluatedValue result;
	EvaluationRequest request;
	REQUIRE(EvaluateValue(document, plan, "value", request, result, diagnostic) == Status::Ok);
	CHECK(std::get<double>(result.Data) == 7);
	const auto text = Write(document);
	Document roundtrip;
	REQUIRE(Read(text, roundtrip, diagnostic) == Status::Ok);
	CHECK(roundtrip == document);
	document.Timeline = TimelineSettings{2, 0, 1, "loop", 30};
	document.Keyframes = {
		Keyframe{"globals", "answer", 0, double{4}}, Keyframe{"globals", "answer", 1, double{8}}
	};
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	request.Tick = 1;
	REQUIRE(EvaluateValue(document, plan, "value", request, result, diagnostic) == Status::Ok);
	CHECK(std::get<double>(result.Data) == 11);
	document.Nodes[1].Values[0].Data = std::string{"source.outputs.result"};
	CHECK(Compile(document, plan, diagnostic) == Status::Cycle);
}

TEST_CASE("PCX control programs override linked defaults and retain disabled source", "[imagegraph][pcx]") {
	Document document;
	document.FormatVersion = 9;
	Node node{"number", "pc.number", "", {}, {{"value", double{3}}}};
	node.SourceDisplayName = "Number";
	node.SourceInputExpressions = {{"value", "value*2+self.value", true}, {"integer", "invalid(", false}};
	document.Nodes = {node};
	document.Outputs = {{"number", "number", "number"}};
	Plan plan;
	Diagnostic diagnostic;
	EvaluatedValue result;
	EvaluationRequest request;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(EvaluateValue(document, plan, "number", request, result, diagnostic) == Status::Ok);
	CHECK(std::get<double>(result.Data) == 9);
	Document copy;
	REQUIRE(Read(Write(document), copy, diagnostic) == Status::Ok);
	CHECK(copy == document);
	document.Nodes[0].SourceInputExpressions[0].Code = "invalid(";
	CHECK(Compile(document, plan, diagnostic) == Status::InvalidValue);
	document.Nodes[0].SourceInputExpressions[0].Enabled = false;
	document.Nodes[0].SourceInputExpressions[0].Port = "missing";
	CHECK(Compile(document, plan, diagnostic) == Status::InvalidValue);
	document.Nodes[0].SourceInputExpressions = {{"value", "0", false}, {"value", "1", false}};
	CHECK(Compile(document, plan, diagnostic) == Status::InvalidValue);
	document.Nodes[0].SourceInputExpressions = {
		{"value", std::string(Limits::MaximumTextBytes + 1, 'x'), false}
	};
	CHECK(Compile(document, plan, diagnostic) == Status::InvalidValue);
}

TEST_CASE("PCX computed equation text schedules only selected named producers", "[imagegraph][pcx]") {
	Document document;
	document.FormatVersion = 9;
	document.Timeline = TimelineSettings{2, 0, 1, "loop", 30};
	Node equation{"consumer", "pc.equation", "", {}, {{"equation", std::string{}}}};
	equation.SourceInternalName = "consumer";
	Node text{"text", "pc.string", "", {}, {{"text", std::string{}}}};
	text.SourceInputExpressions = {
		{"text", "if(Project.frame==0){\"a.outputs.result\"}else{\"b.outputs.result\"}", true}
	};
	Node a{"a", "pc.equation", "", {}, {{"equation", std::string{"7"}}}};
	a.SourceInternalName = "a";
	Node b{"b", "pc.equation", "", {}, {{"equation", std::string{}}}};
	b.SourceInternalName = "b";
	Node cycleText{"cycle-text", "pc.string", "", {}, {{"text", std::string{"consumer.outputs.result"}}}};
	document.Nodes = {equation, text, a, b, cycleText};
	document.Links = {{"text", "text", "consumer", "equation"}, {"cycle-text", "text", "b", "equation"}};
	document.Outputs = {{"value", "consumer", "result"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluatedValue result;
	EvaluationRequest request;
	INFO(diagnostic.Message);
	REQUIRE(EvaluateValue(document, plan, "value", request, result, diagnostic) == Status::Ok);
	CHECK(std::get<double>(result.Data) == 7);
	request.Tick = 1;
	CHECK(EvaluateValue(document, plan, "value", request, result, diagnostic) == Status::Cycle);
	CHECK(std::get<double>(result.Data) == 7);
}

TEST_CASE("PCX node builders compose typed instruction trees and retained bindings", "[imagegraph][pcx]") {
	const auto execute = [](const imagegraph_test::NodeRun &run, std::string_view port = "pcx") {
		INFO(run.Message);
		REQUIRE(run.Ok);
		const auto *value = run.OutputValue(port);
		REQUIRE(value);
		const auto *tree = std::get_if<PcxExpressionValue>(value);
		REQUIRE(tree);
		EvaluationRequest request;
		PcxExecutionContext context{request, {}, nullptr, {32, 32}, {}};
		PcxExecutionResult result;
		Diagnostic diagnostic;
		REQUIRE(ExecutePcxExpression(*tree, context, result, diagnostic) == Status::Ok);
		return result.Data;
	};
	const auto math = imagegraph_test::RunNode(
		"pc.pcx_fn_math", {}, {{"x", double{2}}, {"y", double{3}}, {"operator", EnumValue{0}}}
	);
	CHECK(std::get<double>(execute(math)) == 5);
	REQUIRE(math.OutputValue("pcx"));
	const auto condition = imagegraph_test::RunNode(
		"pc.pcx_condition",
		{},
		{{"condition", double{1}}, {"true", *math.OutputValue("pcx")}, {"false", double{99}}}
	);
	CHECK(std::get<double>(execute(condition)) == 5);
	ArrayValue array{ValueType::Scalar, {double{2}, double{4}}};
	const auto variable =
		imagegraph_test::RunNode("pc.pcx_var", {}, {{"name", std::string{"arr"}}, {"value", array}});
	REQUIRE(variable.Ok);
	REQUIRE(variable.OutputValue("pcx"));
	const auto setter = imagegraph_test::RunNode(
		"pc.pcx_array_set",
		{},
		{{"array", *variable.OutputValue("pcx")}, {"index", double{1}}, {"value", double{9}}}
	);
	const auto changed = std::get<ArrayValue>(execute(setter));
	REQUIRE(changed.Items.size() == 2);
	CHECK(std::get<double>(std::get<ElementValue>(changed.Items[1].Data)) == 9);
	const auto getter =
		imagegraph_test::RunNode("pc.pcx_array_get", {}, {{"array", array}, {"index", double{-1}}});
	CHECK(std::get<double>(execute(getter)) == 4);
	const auto random = imagegraph_test::RunNode(
		"pc.pcx_fn_random", {}, {{"min", double{4}}, {"max", double{4}}, {"integer", true}}
	);
	CHECK(std::get<double>(execute(random)) == 4);
	Image image{2, 3, std::vector<uint8_t>(24, 255)};
	CHECK(
		std::get<double>(
			execute(imagegraph_test::RunNode("pc.pcx_fn_surface_width", {{"surface", &image}}))
		) == 2
	);
	CHECK(
		std::get<double>(
			execute(imagegraph_test::RunNode("pc.pcx_fn_surface_height", {{"surface", &image}}))
		) == 3
	);
	const auto equation = imagegraph_test::RunNode("pc.pcx_equation", {}, {{"equation", std::string{"4*5"}}});
	CHECK(std::get<double>(execute(equation, "result")) == 20);
	const auto *entry = FindCatalogueEntry("pc.pcx_fn_var");
	REQUIRE(entry);
	Node authored{"fallback", "pc.pcx_fn_var", "", {}, {}};
	authored.SourceDisplayName = "fallback";
	EvaluationRequest request;
	detail::NodeContext context(authored, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.Values.emplace_back("default_value", double{6});
	const bool built = detail::FindExecutor(authored.Type)(context);
	INFO(context.FailureMessage);
	REQUIRE(built);
	imagegraph_test::NodeRun fallback;
	fallback.Ok = true;
	fallback.Values = std::move(context.OutputValues);
	CHECK(std::get<double>(execute(fallback)) == 6);
	PcxExpressionValue child, parent;
	Diagnostic diagnostic;
	REQUIRE(CompilePcxExpression("x+2", child, diagnostic) == Status::Ok);
	child.Data->Bindings = {{"x", double{3}}};
	REQUIRE(CompilePcxExpression("child+1", parent, diagnostic) == Status::Ok);
	parent.Data->Bindings = {{"child", child}};
	PcxExecutionContext execution{request, {}, nullptr, {32, 32}, {}};
	PcxExecutionResult result;
	REQUIRE(ExecutePcxExpression(parent, execution, result, diagnostic) == Status::Ok);
	CHECK(std::get<double>(result.Data) == 6);
}

TEST_CASE("PCX source tuples project as arrays and preserve integer numeric values", "[imagegraph][pcx]") {
	// pcx_ast.gml uses is_array for vector inputs; toNumber returns numeric carriers unchanged.
	const std::array parameters{
		AuthoredValue{"v2", Vector2{2, 3}},
		AuthoredValue{"v3", Vector3{4, 5, 6}},
		AuthoredValue{"v4", Vector4{7, 8, 9, 10}},
		AuthoredValue{"q", Quaternion{1, 2, 3, 4}},
		AuthoredValue{"integer", int64_t{9007199254740993}},
		AuthoredValue{"flag", true},
		AuthoredValue{"selection", EnumValue{5}},
		AuthoredValue{"colour", Colour{0x11, 0x22, 0x33, 0x44}}
	};
	CHECK(std::get<double>(Evaluate("v2[-1]", parameters)) == 3);
	CHECK(std::get<double>(Evaluate("v3[1]", parameters)) == 5);
	CHECK(std::get<double>(Evaluate("v4[-1]", parameters)) == 10);
	CHECK(std::get<double>(Evaluate("q[3]", parameters)) == 4);
	CHECK(std::get<double>(Evaluate("length(v2)+length(v3)+length(v4)+length(q)", parameters)) == 13);
	CHECK(std::get<double>(Evaluate("(v2+v3)[2]", parameters)) == 6);
	CHECK(std::get<double>(Evaluate("(2*v4)[-1]", parameters)) == 20);
	CHECK(std::get<double>(Evaluate("(~q)[0]", parameters)) == -2);
	CHECK(std::get<int64_t>(Evaluate("number(integer)", parameters)) == 9007199254740993);
	CHECK(std::get<bool>(Evaluate("number(flag)", parameters)));
	CHECK(std::get<int64_t>(Evaluate("number(selection)", parameters)) == 5);
	CHECK(std::get<int64_t>(Evaluate("number(colour)", parameters)) == 0x44332211);
	CHECK(std::get<double>(Evaluate("colour & 255", parameters)) == 0x11);
	CHECK(std::get<double>(Evaluate("selection+2", parameters)) == 7);
	CHECK(std::get<std::string>(Evaluate("string(integer)", parameters)) == "9007199254740993");
	CHECK(std::get<std::string>(Evaluate("string(colour)", parameters)) == "1144201745");
	CHECK(std::get<double>(Evaluate("Project.dimension[1]")) == 32);
	PcxExpressionValue tree;
	Diagnostic diagnostic;
	REQUIRE(
		CompilePcxProgram(
			"sum=0\nfor(index,item:q){sum+=index+item}\nv2[1]=9\nsum+v2[1]", tree, diagnostic
		) == Status::Ok
	);
	EvaluationRequest request;
	PcxExecutionContext context{request, parameters, nullptr, {32, 32}, {}};
	PcxExecutionResult result;
	const auto executed = ExecutePcxExpression(tree, context, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(executed == Status::Ok);
	CHECK(std::get<double>(result.Data) == 25);
	CHECK(std::get<Vector2>(parameters[0].Data) == Vector2{2, 3});
	context.MaximumBytes = 1;
	CHECK(ExecutePcxExpression(tree, context, result, diagnostic) == Status::LimitExceeded);
	CHECK(std::get<double>(result.Data) == 25);

	Document document;
	document.FormatVersion = 9;
	Node globals{"globals", "pc.global_scope", "", {}, {}};
	globals.DynamicInputs = {
		DynamicInput{"position", ValueType::Vector3, Value{Vector3{4, 5, 6}}},
		DynamicInput{"enabled", ValueType::Boolean, Value{true}},
		DynamicInput{"large", ValueType::Integer, Value{int64_t{9007199254740993}}}
	};
	Node equation{
		"equation", "pc.equation", "", {}, {{"equation", std::string{"(position+2)[-1]+length(position)"}}}
	};
	document.Nodes = {equation, globals};
	document.ProjectGlobalNodeId = "globals";
	document.Outputs = {{"value", "equation", "result"}};
	Plan plan;
	EvaluatedValue output;
	const auto compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	REQUIRE(EvaluateValue(document, plan, "value", request, output, diagnostic) == Status::Ok);
	CHECK(std::get<double>(output.Data) == 11);
	document.Nodes[0].Values[0].Data = std::string{"number(large)"};
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(EvaluateValue(document, plan, "value", request, output, diagnostic) == Status::Ok);
	CHECK(std::get<int64_t>(output.Data) == 9007199254740993);
	document.Nodes[0].Values[0].Data = std::string{"number(enabled)"};
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(EvaluateValue(document, plan, "value", request, output, diagnostic) == Status::Ok);
	CHECK(std::get<bool>(output.Data));
}
