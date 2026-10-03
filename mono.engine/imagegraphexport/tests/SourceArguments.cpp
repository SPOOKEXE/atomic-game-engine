#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/SourceArgumentHost.hpp>
#include <engine/imagegraphexport/Runner.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

TEST_SUITE_ID("engine.imagegraphexport.source_arguments")
TEST_DEPENDS("engine.imagegraph.source_argument")
namespace {
	using namespace engine::imagegraph;
	Document Graph() {
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
		document.Outputs = {{"image", "solid", "surface_out"}, {"number", "argument", "value"}};
		return document;
	}
	struct Files {
		std::filesystem::path Directory =
			std::filesystem::temp_directory_path() / "atomic-source-arguments-runner";
		Files() {
			std::filesystem::remove_all(Directory);
			std::filesystem::create_directories(Directory);
		}
		~Files() {
			std::error_code error;
			std::filesystem::remove_all(Directory, error);
		}
	};
	int
	Run(std::vector<std::string> args,
		std::ostringstream &output,
		std::ostringstream &errors,
		const Document *document = nullptr,
		const Plan *plan = nullptr,
		const EvaluationRequest *request = nullptr) {
		std::vector<char *> argv;
		for (auto &arg : args)
			argv.push_back(arg.data());
		return document ? engine::imagegraphexport::runner::RunWithDocument(
							  static_cast<int>(argv.size()),
							  argv.data(),
							  output,
							  errors,
							  *document,
							  *plan,
							  *request
						  )
						: engine::imagegraphexport::runner::Run(
							  static_cast<int>(argv.size()), argv.data(), output, errors
						  );
	}
	uint32_t PngWidth(const std::filesystem::path &path) {
		std::ifstream file(path, std::ios::binary);
		file.seekg(16);
		uint32_t value = 0;
		for (size_t i = 0; i < 4; ++i)
			value = (value << 8) | static_cast<uint8_t>(file.get());
		return value;
	}
}
TEST_CASE(
	"Ordinary runner arguments reach a real saved graph without extra grants", "[source_argument][export]"
) {
	Files files;
	const auto input = files.Directory / "argument.graph", target = files.Directory / "image.png";
	{
		std::ofstream file(input);
		file << Write(Graph());
	}
	std::vector<std::string> args{
		"imagegraph", "--input", input.string(), "--output-id", "image", "--output", target.string()
	};
	std::ostringstream output, errors;
	REQUIRE(Run(args, output, errors) == 0);
	CHECK(PngWidth(target) == 2);
	for (const auto option : {"--graph-argument", "--graph-argument-integer", "--graph-argument-real"}) {
		auto explicitArgs = args;
		explicitArgs.insert(explicitArgs.end(), {option, "width=3"});
		output.str({});
		errors.str({});
		const auto status = Run(explicitArgs, output, errors);
		INFO(errors.str());
		REQUIRE(status == 0);
		CHECK(PngWidth(target) == 3);
	}
	args.insert(args.end(), {"--graph-argument-bool", "width=true"});
	REQUIRE(Run(args, output, errors) == 0);
	CHECK(PngWidth(target) == 1);
}
TEST_CASE(
	"Runner rejects malformed argument replacement before touching prior pixels", "[source_argument][export]"
) {
	Files files;
	const auto target = files.Directory / "prior.png";
	{
		std::ofstream file(target);
		file << "prior";
	}
	std::ostringstream output, errors;
	CHECK(
		Run({"imagegraph",
			 "--input",
			 "unused.graph",
			 "--output-id",
			 "image",
			 "--output",
			 target.string(),
			 "--graph-argument",
			 "width=3",
			 "--graph-argument-bool",
			 "width=false"},
			output,
			errors) == 2
	);
	CHECK(errors.str().find("repeats a name") != std::string::npos);
	std::ifstream prior(target);
	std::string text;
	prior >> text;
	CHECK(text == "prior");
}
TEST_CASE(
	"Live runner keeps frozen caller argument observations and refuses CLI substitution",
	"[source_argument][export]"
) {
	Files files;
	auto document = Graph();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	SourceArgumentHost host;
	const std::string_view real[] = {"width=5"};
	REQUIRE(
		host.PrepareOptions({{}, {}, {}, real}, Limits::MaximumEvaluationBytes, diagnostic) == Status::Ok
	);
	EvaluationRequest request;
	request.HostProvider = &host;
	std::vector<std::string> args{
		"imagegraph", "--input", "unused.graph", "--output-id", "number", "--value"
	};
	std::ostringstream output, errors;
	REQUIRE(Run(args, output, errors, &document, &plan, &request) == 0);
	CHECK(output.str().find("value=5") != std::string::npos);
	args.insert(args.end(), {"--graph-argument-real", "width=9"});
	CHECK(Run(args, output, errors, &document, &plan, &request) == 2);
	CHECK(errors.str().find("frozen document arguments cannot be replaced") != std::string::npos);
	CHECK(request.HostProvider == &host);
}

TEST_CASE(
	"Runner retains an injected argument provider unless explicit options replace it",
	"[source_argument][export]"
) {
	Files files;
	const auto document = Graph();
	const auto input = files.Directory / "argument.graph";
	{
		std::ofstream file(input);
		file << Write(document);
	}
	SourceArgumentHost host;
	Diagnostic diagnostic;
	const std::string_view real[] = {"width=5"};
	REQUIRE(
		host.PrepareOptions({{}, {}, {}, real}, Limits::MaximumEvaluationBytes, diagnostic) == Status::Ok
	);
	std::vector<std::string> args{
		"imagegraph", "--input", input.string(), "--output-id", "number", "--value"
	};
	const auto run = [&]() {
		std::vector<char *> argv;
		for (auto &arg : args)
			argv.push_back(arg.data());
		std::ostringstream output, errors;
		const auto status = engine::imagegraphexport::runner::RunWithHostInputs(
			static_cast<int>(argv.size()), argv.data(), output, errors, {}, {}, &host
		);
		INFO(errors.str());
		REQUIRE(status == 0);
		return output.str();
	};
	CHECK(run().find("value=5") != std::string::npos);
	args.insert(args.end(), {"--graph-argument-real", "width=9"});
	CHECK(run().find("value=9") != std::string::npos);
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	request.HostProvider = &host;
	EvaluatedValue output;
	REQUIRE(EvaluateValue(document, plan, "number", request, output, diagnostic) == Status::Ok);
	CHECK(output.Data == Value{5.});
}
