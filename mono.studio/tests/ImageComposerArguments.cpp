#include "ImageGraphArguments.hpp"
#include "ImageGraphHost.hpp"

#include <engine/core/Arguments.hpp>
#include <engine/imagegraphexport/GraphAuthoredExport.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <studio/ImageComposerArguments.hpp>
TEST_SUITE_ID("studio.image_composer.arguments")
TEST_DEPENDS("studio.imagegraph.host")
namespace {
	using namespace engine::imagegraph;
	Document ArgumentGraph() {
		Document doc;
		doc.FormatVersion = 9;
		doc.Nodes = {
			{"argument",
			 "pc.argument",
			 {},
			 {},
			 {{"tag", std::string("width")}, {"type", EnumValue{1}}, {"default_value", int64_t{1}}}},
			{"dimension", "pc.vector2", "", {}, {{"x", 1.0}, {"y", 1.0}}},
			{"solid",
			 "pc.solid",
			 "",
			 {},
			 {{"dimension", Vector2{1, 1}},
			  {"dimension_unit", EnumValue{0}},
			  {"color", Colour{17, 31, 49, 255}},
			  {"attribute_color_depth", EnumValue{3}}}},
			{"export", "pc.export", {}, {}, {{"type", EnumValue{0}}}}
		};
		doc.Links = {
			{"argument", "value", "dimension", "x"},
			{"dimension", "vector", "solid", "dimension"},
			{"solid", "surface_out", "export", "surface"}
		};
		doc.Outputs = {{"argument-value", "argument", "value"}, {"image", "solid", "surface_out"}};
		return doc;
	}
}
TEST_CASE(
	"Composer owns explicit argument observations for preview and prepared export",
	"[studio][source_arguments]"
) {
	using namespace studio::detail;
	const auto directory = std::filesystem::temp_directory_path() /
						   ("pc-studio-arguments-" +
							std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	REQUIRE(std::filesystem::create_directory(directory));
	struct Cleanup {
		std::filesystem::path Path;
		~Cleanup() {
			std::error_code ignored;
			std::filesystem::remove_all(Path, ignored);
		}
	} cleanup{directory};
	ImageGraphHost host;
	Document doc = ArgumentGraph();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	EvaluationRequest request{.Tick = 4, .HostProvider = &host};
	Image image;
	REQUIRE(Evaluate(doc, plan, "image", request, image, diagnostic) == Status::Ok);
	CHECK(image.Width == 1);
	CHECK(image.Height == 1);
	CHECK(image.Pixels == std::vector<uint8_t>{17, 31, 49, 255});
	uint64_t inputRevision = 7;
	unsigned retired = 0;
	std::string assignment = "width=2";
	std::array<std::string_view, 1> integers{assignment};
	REQUIRE(PrepareImageGraphArguments(
		host.SourceArguments,
		{{}, {}, integers, {}},
		inputRevision,
		[&] {
			++retired;
			host.ResetFiles();
			host.LuaReceipts.Clear();
		},
		diagnostic
	));
	assignment = "width=9";
	CHECK(inputRevision == 8);
	CHECK(retired == 1);
	REQUIRE(Evaluate(doc, plan, "image", request, image, diagnostic) == Status::Ok);
	CHECK(image.Width == 2);
	CHECK(host.RetainedObservationBytes() > host.RetainedBytes);
	EvaluationSnapshot snapshot;
	REQUIRE(EvaluateNodeInputs(doc, plan, "export", request, snapshot, diagnostic) == Status::Ok);
	REQUIRE(snapshot.Images().size() == 1);
	CHECK(snapshot.Images()[0].Data.Width == 2);
	engine::imagegraphexport::GraphExportSettings grants;
	grants.Input = directory / "unread.graph";
	grants.Output = directory;
	std::string failure;
	REQUIRE(
		engine::imagegraphexport::ExportAuthoredGraphNode(
			doc, plan, request, grants, "export", snapshot, failure
		)
	);
	CHECK_FALSE(std::filesystem::exists(grants.Input));
	size_t pngs = 0;
	for (const auto &entry : std::filesystem::directory_iterator(directory))
		if (entry.path().extension() == ".png") ++pngs;
	CHECK(pngs == 1);
	const auto held = host.SourceArguments.RetainedBytes();
	const std::array<std::string_view, 1> invalid{"width=not-an-integer"};
	CHECK_FALSE(PrepareImageGraphArguments(
		host.SourceArguments, {{}, {}, invalid, {}}, inputRevision, [&] { ++retired; }, diagnostic
	));
	CHECK(inputRevision == 8);
	CHECK(retired == 1);
	CHECK(host.SourceArguments.RetainedBytes() == held);
	REQUIRE(Evaluate(doc, plan, "image", request, image, diagnostic) == Status::Ok);
	CHECK(image.Width == 2);
	const std::array<std::string_view, 1> replacement{"width=3"};
	CHECK_FALSE(PrepareImageGraphArguments(
		host.SourceArguments, {{}, {}, replacement, {}}, inputRevision, [&] { ++retired; }, diagnostic, 1
	));
	CHECK(inputRevision == 8);
	CHECK(retired == 1);
	REQUIRE(Evaluate(doc, plan, "image", request, image, diagnostic) == Status::Ok);
	CHECK(image.Width == 2);
	REQUIRE(PrepareImageGraphArguments(
		host.SourceArguments,
		{{}, {}, replacement, {}},
		inputRevision,
		[&] {
			++retired;
			host.ResetFiles();
			host.LuaReceipts.Clear();
		},
		diagnostic
	));
	CHECK(inputRevision == 9);
	CHECK(retired == 2);
	REQUIRE(Evaluate(doc, plan, "image", request, image, diagnostic) == Status::Ok);
	CHECK(image.Width == 3);
	inputRevision = UINT64_MAX;
	const std::array<std::string_view, 1> afterLimit{"width=4"};
	CHECK_FALSE(PrepareImageGraphArguments(
		host.SourceArguments, {{}, {}, afterLimit, {}}, inputRevision, [&] { ++retired; }, diagnostic
	));
	CHECK(diagnostic.Code == Status::LimitExceeded);
	CHECK(inputRevision == UINT64_MAX);
	CHECK(retired == 2);
	REQUIRE(Evaluate(doc, plan, "image", request, image, diagnostic) == Status::Ok);
	CHECK(image.Width == 3);
}
TEST_CASE(
	"Composer startup parses four typed option spans during synchronous ownership",
	"[studio][source_arguments]"
) {
	using namespace engine::imagegraph;
	engine::core::Arguments arguments("studio-arguments", "typed Composer inputs");
	arguments.Value(SOURCE_ARGUMENT_TEXT_OPTION, "NAME=VALUE", "text");
	arguments.Value(SOURCE_ARGUMENT_BOOLEAN_OPTION, "NAME=true|false", "boolean");
	arguments.Value(SOURCE_ARGUMENT_INTEGER_OPTION, "NAME=INT", "integer");
	arguments.Value(SOURCE_ARGUMENT_REAL_OPTION, "NAME=REAL", "real");
	std::array<std::string, 9> parameters{
		"studio-arguments",
		"--graph-argument",
		"name=hello=world",
		"--graph-argument-bool",
		"flag=true",
		"--graph-argument-integer",
		"width=2",
		"--graph-argument-real",
		"gain=-1.25"
	};
	std::array<char *, 9> argv{};
	for (size_t i = 0; i < parameters.size(); ++i)
		argv[i] = parameters[i].data();
	REQUIRE(arguments.Parse(static_cast<int>(argv.size()), argv.data()).Ok);
	const auto text = arguments.GetAll(SOURCE_ARGUMENT_TEXT_OPTION);
	const auto boolean = arguments.GetAll(SOURCE_ARGUMENT_BOOLEAN_OPTION);
	const auto integer = arguments.GetAll(SOURCE_ARGUMENT_INTEGER_OPTION);
	const auto real = arguments.GetAll(SOURCE_ARGUMENT_REAL_OPTION);
	Diagnostic diagnostic;
	REQUIRE(studio::PrepareImageComposerArguments({text, boolean, integer, real}, diagnostic));
	const std::array<std::string_view, 1> duplicate{"name=3"};
	CHECK_FALSE(studio::PrepareImageComposerArguments({text, boolean, duplicate, real}, diagnostic));
	CHECK(diagnostic.Code == Status::DuplicateId);
	REQUIRE(studio::PrepareImageComposerArguments({}, diagnostic));
}
