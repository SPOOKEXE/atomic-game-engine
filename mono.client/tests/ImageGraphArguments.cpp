#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/SourceArgumentHost.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <client/ImageGraphRuntime.hpp>
#include <filesystem>
#include <fstream>

TEST_SUITE_ID("client.imagegraph_arguments")
TEST_DEPENDS("engine.imagegraph.source_argument")
namespace {
	using namespace engine::imagegraph;
	struct GraphFile {
		std::filesystem::path Directory =
			std::filesystem::temp_directory_path() / "atomic-client-argument-graph";
		engine::core::Name Graph{"argument"}, Output{"image"};
		GraphFile() {
			std::filesystem::remove_all(Directory);
			std::filesystem::create_directories(Directory / "imagegraphs");
			Document document;
			document.FormatVersion = 9;
			document.Nodes = {
				{"argument",
				 "pc.argument",
				 "",
				 {},
				 {{"tag", std::string{"width"}}, {"type", EnumValue{1}}, {"default_value", 2.}}},
				{"dimension", "pc.vector2", "", {}, {{"x", 2.0}, {"y", 1.0}}},
				{"solid",
				 "pc.solid",
				 "",
				 {},
				 {{"dimension", Vector2{2, 1}},
				  {"dimension_unit", EnumValue{0}},
				  {"color", Colour{31, 47, 59, 255}},
				  {"attribute_color_depth", EnumValue{3}}}}
			};
			document.Links = {
				{"argument", "value", "dimension", "x"}, {"dimension", "vector", "solid", "dimension"}
			};
			document.Outputs = {{"image", "solid", "surface_out"}};
			std::ofstream file(client::ImageGraphDocumentPath(Directory, Graph));
			file << Write(document);
		}
		~GraphFile() {
			std::error_code error;
			std::filesystem::remove_all(Directory, error);
		}
	};
}
TEST_CASE(
	"Client CPU graph arguments are explicit and repeated same-tick replacements take effect",
	"[client][source_argument]"
) {
	GraphFile file;
	const auto initial = client::LoadImageGraphFrame(file.Directory, file.Graph, file.Output, 0);
	INFO(initial.Diagnostic.Message);
	REQUIRE(initial.Status == Status::Ok);
	CHECK(initial.Image.Width == 2);
	CHECK(initial.Image.Height == 1);
	CHECK(initial.Image.Pixels == std::vector<uint8_t>{31, 47, 59, 255, 31, 47, 59, 255});
	SourceArgumentHost host;
	Diagnostic diagnostic;
	const std::string_view integer[] = {"width=3"};
	REQUIRE(
		host.PrepareOptions({{}, {}, integer, {}}, Limits::MaximumEvaluationBytes, diagnostic) == Status::Ok
	);
	const auto assigned = client::LoadImageGraphFrame(file.Directory, file.Graph, file.Output, 0, 0, &host);
	INFO(assigned.Diagnostic.Message);
	REQUIRE(assigned.Status == Status::Ok);
	CHECK(assigned.Image.Width == 3);
	const std::string_view updated[] = {"width=4"};
	REQUIRE(
		host.PrepareOptions({{}, {}, updated, {}}, Limits::MaximumEvaluationBytes, diagnostic) == Status::Ok
	);
	const auto replaced = client::LoadImageGraphFrame(file.Directory, file.Graph, file.Output, 0, 0, &host);
	REQUIRE(replaced.Status == Status::Ok);
	CHECK(replaced.Image.Width == 4);
	const std::string_view malformed[] = {"width=4tail"};
	CHECK(
		host.PrepareOptions({{}, {}, malformed, {}}, Limits::MaximumEvaluationBytes, diagnostic) ==
		Status::InvalidValue
	);
	const auto preserved = client::LoadImageGraphFrame(file.Directory, file.Graph, file.Output, 0, 0, &host);
	REQUIRE(preserved.Status == Status::Ok);
	CHECK(preserved.Image == replaced.Image);
}
TEST_CASE(
	"Client table generation changes only after a successful bounded replacement", "[client][source_argument]"
) {
	client::ImageGraphRuntime runtime;
	engine::render::Renderer renderer;
	Diagnostic diagnostic;
	const auto initial = runtime.ArgumentGeneration();
	const std::string_view integer[] = {"width=3"};
	REQUIRE(runtime.PrepareArguments({{}, {}, integer, {}}, renderer, diagnostic) == Status::Ok);
	CHECK(runtime.ArgumentGeneration() == initial + 1);
	const std::string_view malformed[] = {"width=3.5"};
	CHECK(runtime.PrepareArguments({{}, {}, malformed, {}}, renderer, diagnostic) == Status::InvalidValue);
	CHECK(runtime.ArgumentGeneration() == initial + 1);
	CHECK(runtime.PrepareArguments({{}, {}, integer, {}}, renderer, diagnostic, 1) == Status::LimitExceeded);
	CHECK(runtime.ArgumentGeneration() == initial + 1);
}

TEST_CASE("Renderer export CPU fallback uses the same explicit argument table", "[client][source_argument]") {
	GraphFile file;
	engine::render::Renderer renderer;
	const auto defaults =
		client::LoadImageGraphRenderExportFrame(file.Directory, file.Graph, file.Output, renderer, 0);
	INFO(defaults.Diagnostic.Message);
	REQUIRE(defaults.Status == Status::Ok);
	CHECK(defaults.Image.Width == 2);
	SourceArgumentHost host;
	Diagnostic diagnostic;
	const std::string_view integer[] = {"width=3"};
	REQUIRE(
		host.PrepareOptions({{}, {}, integer, {}}, Limits::MaximumEvaluationBytes, diagnostic) == Status::Ok
	);
	const auto assigned = client::LoadImageGraphRenderExportFrame(
		file.Directory, file.Graph, file.Output, renderer, 0, 0, &host
	);
	INFO(assigned.Diagnostic.Message);
	REQUIRE(assigned.Status == Status::Ok);
	CHECK(assigned.Image.Width == 3);
	const std::string_view replacement[] = {"width=4"};
	REQUIRE(
		host.PrepareOptions({{}, {}, replacement, {}}, Limits::MaximumEvaluationBytes, diagnostic) ==
		Status::Ok
	);
	const auto updated = client::LoadImageGraphRenderExportFrame(
		file.Directory, file.Graph, file.Output, renderer, 0, 0, &host
	);
	REQUIRE(updated.Status == Status::Ok);
	CHECK(updated.Image.Width == 4);
	CHECK(assigned.Image.Width == 3);
}
