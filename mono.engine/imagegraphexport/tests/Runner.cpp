#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraphexport/Runner.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <sstream>
TEST_SUITE_ID("engine.imagegraphexport.runner")
TEST_DEPENDS("engine.imagegraph.document")
TEST_CASE(
	"Live runner reads immutable graph observations without opening the context file", "[imagegraph][export]"
) {
	using namespace engine::imagegraph;
	const auto root = std::filesystem::temp_directory_path() / "atomic-live-runner-test";
	std::filesystem::create_directories(root);
	struct Cleanup {
		std::filesystem::path Path;
		~Cleanup() {
			std::error_code error;
			std::filesystem::remove_all(Path, error);
		}
	} cleanup{root};
	Document document;
	document.Nodes.push_back(
		{"solid",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{255, 0, 0, 255}}}}
	);
	document.Outputs.push_back({"preview", "solid", "image"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	request.Tick = 2;
	request.Subframe = .5;
	request.NegativeFrame = true;
	std::vector<std::string> args{
		"imagegraph",
		"--input",
		(root / "does-not-exist.graph").string(),
		"--output-id",
		"preview",
		"--output",
		(root / "image.png").string()
	};
	std::vector<char *> argv;
	for (auto &arg : args)
		argv.push_back(arg.data());
	std::ostringstream output, errors;
	REQUIRE(
		engine::imagegraphexport::runner::RunWithDocument(
			static_cast<int>(argv.size()), argv.data(), output, errors, document, plan, request
		) == 0
	);
	CHECK(std::filesystem::exists(root / "image.png"));
	CHECK(output.str().find("frame=-2.5") != std::string::npos);
	CHECK_FALSE(std::filesystem::exists(root / "does-not-exist.graph"));
	CHECK(document.Nodes[0].Values[0].Data == Value{int64_t{1}});
}
