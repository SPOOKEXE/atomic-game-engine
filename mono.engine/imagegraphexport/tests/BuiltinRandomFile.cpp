#include <engine/imagegraph/BuiltinRandomCaptureCodec.hpp>
#include <engine/imagegraphexport/BuiltinRandomFile.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <fstream>

TEST_SUITE_ID("engine.imagegraphexport.builtin_random_file")
namespace {
	using namespace engine::imagegraph;
	using namespace engine::imagegraphexport;
	std::string ReadBytes(const std::filesystem::path &path) {
		std::ifstream stream(path, std::ios::binary);
		return {std::istreambuf_iterator<char>(stream), {}};
	}
	Document Fixture() {
		Document document;
		document.FormatVersion = 9;
		ArrayValue palette{
			ValueType::Colour,
			{Colour{0, 0, 0, 255},
			 Colour{64, 64, 64, 255},
			 Colour{128, 128, 128, 255},
			 Colour{192, 192, 192, 255},
			 Colour{255, 255, 255, 255}}
		};
		document.Nodes = {
			{"palette",
			 "pc.palette_shrink",
			 "",
			 {},
			 {{"palette_in", palette}, {"amount", int64_t{2}}, {"sample_type", EnumValue{1}}, {"seed", 42.}}}
		};
		document.Outputs = {{"colours", "palette", "palette"}};
		return document;
	}
} // namespace
TEST_CASE(
	"Named observed draws reject malformed text without changing caller state", "[builtin_random_file]"
) {
	SourceBuiltinRandomDraw draw{SourceBuiltinRandomOperation::CRand, 0, 0, 42};
	const auto before = draw;
	std::string failure;
	for (auto text : {"0:0:1:.1", "random:0:1:nan", "random:0:1:.1:2", "random:0:1", "random:0::1"}) {
		CHECK_FALSE(ParseBuiltinRandomDraw(text, draw, failure));
		CHECK(draw == before);
	}
	CHECK(ParseBuiltinRandomDraw("random:0:1:0.1", draw, failure));
	CHECK(draw == SourceBuiltinRandomDraw{SourceBuiltinRandomOperation::Random, 0, 1, .1});
}
TEST_CASE(
	"Prepared filesystem recordings replay actual clusters and preserve "
	"files on failure",
	"[builtin_random_file]"
) {
	const auto directory = std::filesystem::temp_directory_path() / "atomic-builtin-random-files";
	std::filesystem::remove_all(directory);
	std::filesystem::create_directories(directory);
	struct Cleanup {
		std::filesystem::path Directory;
		~Cleanup() {
			std::error_code e;
			std::filesystem::remove_all(Directory, e);
		}
	} cleanup{directory};
	GraphExportSettings settings;
	settings.Input = directory / "palette.graph";
	const auto document = Fixture();
	{
		std::ofstream file(settings.Input);
		file << Write(document);
	}
	const auto destination = directory / "observed.rng";
	const std::array draws{
		SourceBuiltinRandomDraw{SourceBuiltinRandomOperation::Random, 0, 1, .1},
		SourceBuiltinRandomDraw{SourceBuiltinRandomOperation::Random, 0, 1, .9}
	};
	std::string failure;
	REQUIRE(PrepareBuiltinRandomCaptureFile(settings, "palette", destination, draws, failure));
	const auto bytes = ReadBytes(destination);
	REQUIRE_FALSE(bytes.empty());
	std::vector<SourceBuiltinRandomCapture> captures;
	REQUIRE(LoadBuiltinRandomCaptureFile(destination, settings.Content, captures, failure));
	REQUIRE(captures.size() == 1);
	CHECK(captures[0].Authored == document.Nodes[0]);
	CHECK(captures[0].Draws == std::vector(draws.begin(), draws.end()));
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	request.BuiltinRandomCaptures = captures;
	EvaluatedValue output;
	REQUIRE(EvaluateValue(document, plan, "colours", request, output, diagnostic) == Status::Ok);
	const auto &palette = std::get<ArrayValue>(output.Data);
	REQUIRE(palette.Elements.size() == 2);
	CHECK(std::get<Colour>(palette.Elements[0]) == Colour{0, 0, 0, 255});
	CHECK(std::get<Colour>(palette.Elements[1]) == Colour{192, 192, 192, 255});
	CHECK_FALSE(PrepareBuiltinRandomCaptureFile(settings, "missing", destination, draws, failure));
	CHECK(ReadBytes(destination) == bytes);
	CHECK_FALSE(PrepareBuiltinRandomCaptureFile(settings, "palette", destination, draws, failure, 1));
	CHECK(ReadBytes(destination) == bytes);
	auto denied = settings;
	denied.Content.Allow(engine::assets::ContentForm::Unknown, false);
	CHECK_FALSE(PrepareBuiltinRandomCaptureFile(denied, "palette", destination, draws, failure));
	CHECK(ReadBytes(destination) == bytes);
	const auto previous = captures;
	CHECK_FALSE(LoadBuiltinRandomCaptureFile(destination, settings.Content, captures, failure, 1));
	CHECK(captures == previous);
	{
		std::ofstream file(destination);
		file << "malformed";
	}
	CHECK_FALSE(LoadBuiltinRandomCaptureFile(destination, settings.Content, captures, failure));
	CHECK(captures == previous);
	CHECK_FALSE(PrepareBuiltinRandomCaptureFile(settings, "palette", settings.Input, draws, failure));
	CHECK(ReadBytes(settings.Input) == Write(document));
}
