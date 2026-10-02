#include "GraphCsvWrite.hpp"

#include "SourceCsvNumber.hpp"

#include <engine/imagegraphexport/GraphExport.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
TEST_SUITE_ID("engine.imagegraphexport.graph_csv_write")
TEST_DEPENDS("engine.imagegraph.host_capture")
namespace {
	using namespace engine::imagegraph;
	using namespace engine::imagegraphexport;
	struct Files {
		std::filesystem::path Directory =
			std::filesystem::temp_directory_path() /
			("atomic-csv-host-" +
			 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		Files() {
			std::filesystem::create_directory(Directory);
		}
		~Files() {
			std::error_code error;
			std::filesystem::remove_all(Directory, error);
		}
	};
	std::string ReadCsv(const std::filesystem::path &file) {
		std::ifstream stream(file, std::ios::binary);
		return {(std::istreambuf_iterator<char>(stream)), {}};
	}
	SourceArrayItem Leaf(ElementValue value) {
		return {std::move(value)};
	}
}
TEST_CASE("CSV source number profile rounds the exact binary64 value", "[imagegraph][csv_write]") {
	using engine::imagegraphexport::detail::CsvNumber;
	CHECK(CsvNumber(1.5) == "1.50");
	CHECK(CsvNumber(1.125) == "1.13");
	CHECK(CsvNumber(-1.125) == "-1.13");
	CHECK(CsvNumber(2.675) == "2.67");
	CHECK(CsvNumber(1.005) == "1.00");
	CHECK(CsvNumber(1.015) == "1.01");
	CHECK(CsvNumber(0.) == "0");
	CHECK(CsvNumber(-0.) == "0");
	CHECK(CsvNumber(-.0001) == "-0.00");
	CHECK(CsvNumber(2147483647.) == "2147483647");
	CHECK(CsvNumber(2147483648.) == "2147483648.00");
	CHECK(CsvNumber(-2147483649.) == "-2147483649.00");
	CHECK(CsvNumber(1e20) == "100000000000000000000.00");
	CHECK(CsvNumber(1e21) == "1e+21");
}
TEST_CASE(
	"CSV writer traverses all native source array carriers without inventing escapes",
	"[imagegraph][csv_write]"
) {
	Files files;
	Node node;
	node.Id = "writer";
	node.Type = "pc.csv_file_write";
	const auto original = files.Directory / "literal.CSV";
	const auto final = files.Directory / "literal.CSV.csv";
	Value content;
	std::string expected;
	SECTION("flat primitive rows") {
		ArrayValue array;
		array.ElementType = ValueType::Any;
		array.Items = {
			Leaf(1.5), Leaf(1.125), Leaf(2.675), Leaf(int64_t{4}), Leaf(true), Leaf(std::string("a,\"b\"\nc"))
		};
		content = array;
		expected = "1.50, 1.13, 2.67, 4, 1, a,\"b\"\nc";
	}
	SECTION("typed nested numeric carrier") {
		ArrayValue array;
		array.ElementType = ValueType::Scalar;
		array.Nested = {{1.5, 1.125}, {2.675, 4.}};
		content = array;
		expected = "1.50, 1.13\n2.67, 4\n";
	}
	SECTION("mixed source rows preserve source leading separator quirk") {
		ArrayValue array;
		array.ElementType = ValueType::Any;
		array.Items = {
			{std::vector<SourceArrayItem>{Leaf(1.5), Leaf(std::string("left,quoted"))}},
			Leaf(true),
			{std::vector<SourceArrayItem>{}},
			{std::vector<SourceArrayItem>{
				Leaf(int64_t{-2}), {std::vector<SourceArrayItem>{Leaf(std::string("inner")), Leaf(1.125)}}
			}}
		};
		content = array;
		expected = "1.50, left,quoted\n, 1\n-2, [ \"inner\",1.13 ]\n";
	}
	SECTION("scalar source content") {
		content = 1.125;
		expected = "1.13";
	}
	SECTION("empty source array") {
		content = ArrayValue{ValueType::Any, {}};
		expected = "";
	}
	std::array<AuthoredValue, 2> inputs{{{"path", original.string()}, {"content", content}}};
	std::array<GraphFileGrant, 1> grants{{{node.Id, final, true}}};
	EvaluationRequest request;
	request.Tick = 9;
	request.Subframe = .25;
	request.NegativeFrame = true;
	HostNodeInvocation invocation{node, request, inputs, {}, 16 * 1024 * 1024};
	HostNodeCapture capture;
	std::string failure;
	const bool written = CaptureGraphCsvWrite(grants, {}, invocation, capture, failure);
	INFO(failure);
	REQUIRE(written);
	CHECK(ReadCsv(final) == expected);
	CHECK_FALSE(std::filesystem::exists(original));
	CHECK(capture.Tick == 9);
	CHECK(capture.Subframe == .25);
	CHECK(capture.NegativeFrame);
	const auto retained = capture.Inputs;
	grants[0].File = original;
	CHECK_FALSE(CaptureGraphCsvWrite(grants, {}, invocation, capture, failure));
	CHECK(ReadCsv(final) == expected);
	CHECK_FALSE(std::filesystem::exists(original));
	CHECK(capture.Inputs == retained);
	grants[0].File = final;
	grants[0].Write = false;
	CHECK_FALSE(CaptureGraphCsvWrite(grants, {}, invocation, capture, failure));
	CHECK(ReadCsv(final) == expected);
	grants[0].Write = true;
	invocation.MaximumOperationBytes = 16;
	CHECK_FALSE(CaptureGraphCsvWrite(grants, {}, invocation, capture, failure));
	CHECK(ReadCsv(final) == expected);
	invocation.MaximumOperationBytes = 16 * 1024 * 1024;
	inputs[1].Data = StructValue{};
	CHECK_FALSE(CaptureGraphCsvWrite(grants, {}, invocation, capture, failure));
	CHECK(ReadCsv(final) == expected);
	size_t entries = 0;
	for (const auto &entry : std::filesystem::directory_iterator(files.Directory)) {
		(void)entry;
		++entries;
	}
	CHECK(entries == 1);
}
TEST_CASE(
	"CSV file-backed manual callback resolves flat nested and mixed routed graph contents",
	"[imagegraph][csv_write]"
) {
	Files files;
	const auto output = files.Directory / "routed.csv";
	Value content;
	std::string expected;
	SECTION("flat") {
		ArrayValue array;
		array.ElementType = ValueType::Any;
		array.Items = {Leaf(1.5), Leaf(std::string("a,b"))};
		content = array;
		expected = "1.50, a,b";
	}
	SECTION("authored numeric row tree") {
		ArrayValue array;
		array.ElementType = ValueType::Any;
		array.Items = {
			{std::vector<SourceArrayItem>{Leaf(1.125), Leaf(2.675)}},
			{std::vector<SourceArrayItem>{Leaf(3.), Leaf(4.)}}
		};
		content = array;
		expected = "1.13, 2.67\n3, 4\n";
	}
	SECTION("mixed nested") {
		ArrayValue array;
		array.ElementType = ValueType::Any;
		array.Items = {
			Leaf(std::string("heading")),
			{std::vector<SourceArrayItem>{Leaf(1.125), Leaf(true)}},
			Leaf(std::string("last"))
		};
		content = array;
		expected = "heading1.13, 1\n, last";
	}
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"writer", "pc.csv_file_write", "", {}, {{"path", output.string()}}},
		{"preview",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{0, 0, 0, 1}}}}
	};
	document.Junctions = {{"content", "", ValueType::Any, content}};
	document.Links = {{"content", "value", "writer", "content"}};
	document.Outputs = {{"preview", "preview", "image"}};
	Plan plan;
	Diagnostic diagnostic;
	const auto status = Compile(document, plan, diagnostic);
	INFO(diagnostic.NodeId << ":" << diagnostic.Port << " " << diagnostic.Message);
	REQUIRE(status == Status::Ok);
	std::array<GraphFileGrant, 1> grants{{{"writer", output, true}}};
	GraphFileHost host(grants, {});
	EvaluationRequest request;
	request.HostProvider = &host;
	Image preview;
	REQUIRE(Evaluate(document, plan, "preview", request, preview, diagnostic) == Status::Ok);
	CHECK_FALSE(std::filesystem::exists(output));
	HostNodeCapture capture;
	std::string failure;
	REQUIRE(ExecuteGraphHostNode(document, plan, request, "writer", capture, failure));
	CHECK(ReadCsv(output) == expected);
	GraphExportSettings settings;
	settings.Input = files.Directory / "routed.graph";
	settings.HostProvider = &host;
	{
		std::ofstream file(settings.Input);
		file << Write(document);
	}
	REQUIRE(ExecuteGraphHostNode(settings, "writer", capture, failure));
	CHECK(ReadCsv(output) == expected);
	grants[0].Write = false;
	CHECK_FALSE(ExecuteGraphHostNode(settings, "writer", capture, failure));
	CHECK(ReadCsv(output) == expected);
}
