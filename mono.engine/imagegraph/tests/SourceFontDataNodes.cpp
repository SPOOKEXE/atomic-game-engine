#include "FontPath.hpp"

#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/SourceFont.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.source_font_data")
using namespace engine::imagegraph;
namespace {
	Document FontPathGraph(std::string path = "font-alias") {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"font", "pc.font_data", "", {}, {{"font", std::move(path)}}}, {"pin", "pc.pin", "", {}, {}}
		};
		document.Links = {{"font", "font", "pin", "in"}};
		document.Outputs = {{"out", "pin", "out"}};
		return document;
	}
	Plan FontPathPlan(const Document &document) {
		Plan plan;
		Diagnostic diagnostic;
		const auto status = Compile(document, plan, diagnostic);
		INFO(diagnostic.NodeId << ':' << diagnostic.Port << ':' << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return plan;
	}
	SourceFontContext FontNamespace() {
		SourceFontContext context;
		context.AliasMapKnown = true;
		context.Aliases = {{"font-alias", "/older/face.ttf"}, {"font-alias", "/granted/face.ttf"}};
		context.Directory = "/source/";
		context.ApplicationLocation = "/application/";
		context.ProjectPath = "/project/scenes/project.pxc";
		return context;
	}
}
TEST_CASE(
	"Font Data preserves source alias order through a persisted graph and Any forwarding",
	"[source_font_data]"
) {
	const auto document = FontPathGraph();
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	REQUIRE(restored == document);
	const auto plan = FontPathPlan(restored);
	const auto context = FontNamespace();
	EvaluationRequest request;
	request.SourceFonts = &context;
	EvaluatedValue output;
	const auto status = EvaluateValue(restored, plan, "out", request, output, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	CHECK(output.Data == Value{std::string{"/granted/face.ttf"}});
}
TEST_CASE(
	"Font Data prefix and relative path projection require explicit source namespace facts",
	"[source_font_data]"
) {
	const auto context = FontNamespace();
	EvaluationRequest request;
	request.SourceFonts = &context;
	for (const auto &[authored, expected] :
		 {std::pair{"%DIR%/fonts/face.ttf", "/source/fonts/face.ttf"},
		  std::pair{"%APP%/fonts/face.ttf", "/application/fonts/face.ttf"},
		  std::pair{"./fonts/face.ttf", "/project/scenes/fonts/face.ttf"},
		  std::pair{"../fonts/face.ttf", "/project/fonts/face.ttf"},
		  std::pair{"C:\\fonts\\face.ttf", "C:/fonts/face.ttf"}}) {
		const auto document = FontPathGraph(authored);
		const auto plan = FontPathPlan(document);
		EvaluatedValue output;
		Diagnostic diagnostic;
		const auto status = EvaluateValue(document, plan, "out", request, output, diagnostic);
		INFO(authored << ':' << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		CHECK(output.Data == Value{std::string{expected}});
	}
	const auto document = FontPathGraph("./face.ttf");
	const auto plan = FontPathPlan(document);
	auto incomplete = context;
	incomplete.ProjectPath.reset();
	request.SourceFonts = &incomplete;
	EvaluatedValue output;
	output.Data = int64_t{93};
	const auto before = output;
	Diagnostic diagnostic;
	CHECK(EvaluateValue(document, plan, "out", request, output, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.NodeId == "font");
	CHECK(output == before);
}
TEST_CASE(
	"Font Data empty default distinguishes known empty namespace from unavailable host state",
	"[source_font_data]"
) {
	auto document = FontPathGraph();
	document.Nodes[0].Values.clear();
	const auto plan = FontPathPlan(document);
	EvaluationRequest request;
	EvaluatedValue output;
	output.Data = std::string{"old"};
	const auto before = output;
	Diagnostic diagnostic;
	CHECK(EvaluateValue(document, plan, "out", request, output, diagnostic) == Status::UnsupportedExecution);
	CHECK(output == before);
	SourceFontContext context;
	context.AliasMapKnown = true;
	request.SourceFonts = &context;
	REQUIRE(EvaluateValue(document, plan, "out", request, output, diagnostic) == Status::Ok);
	CHECK(output.Data == Value{std::string{}});
}
TEST_CASE(
	"Font Data real array producer uses source root mapping when processing is disabled", "[source_font_data]"
) {
	auto document = FontPathGraph();
	document.Nodes[0].Values = {{"attribute_process", false}};
	Node array{"paths", "pc.array", "", {}, {{"type", EnumValue{4}}}};
	array.DynamicInputs = {
		{"input_0", ValueType::Text, std::string{"font-alias"}},
		{"input_1", ValueType::Text, std::string{"./face.ttf"}}
	};
	document.Nodes.push_back(std::move(array));
	document.Links.push_back({"paths", "array", "font", "font"});
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	const auto plan = FontPathPlan(restored);
	const auto context = FontNamespace();
	EvaluationRequest request;
	request.SourceFonts = &context;
	EvaluatedValue output;
	const auto status = EvaluateValue(restored, plan, "out", request, output, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	const auto *arrayOutput = std::get_if<ArrayValue>(&output.Data);
	REQUIRE(arrayOutput);
	REQUIRE(arrayOutput->Elements.size() == 2);
	CHECK(arrayOutput->Elements[0] == ElementValue{std::string{"/granted/face.ttf"}});
	CHECK(arrayOutput->Elements[1] == ElementValue{std::string{"/project/scenes/face.ttf"}});
}
TEST_CASE("Font path workspace failure cannot replace a previous lexical result", "[source_font_data]") {
	const auto context = FontNamespace();
	std::string output = "prior", failure;
	CHECK(detail::ResolveSourceFontPath("./face.ttf", &context, 1, output, failure) == Status::LimitExceeded);
	CHECK(output == "prior");
	CHECK_FALSE(failure.empty());
}
