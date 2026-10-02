#include <engine/imagegraphexport/GraphFileHost.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <string>
TEST_SUITE_ID("engine.imagegraphexport.graph_file_host")
TEST_DEPENDS("engine.imagegraph.host_capture")
TEST_DEPENDS("engine.bake.layered_image")

TEST_CASE("File host requires exact node path and operation grants", "[assetc][imagegraph]") {
	using namespace engine::imagegraph;
	const auto directory = std::filesystem::temp_directory_path() / "atomic-graph-file-host-test";
	std::error_code error;
	std::filesystem::remove_all(directory, error);
	std::filesystem::create_directories(directory);
	struct Cleanup {
		std::filesystem::path Path;
		~Cleanup() {
			std::error_code error;
			std::filesystem::remove_all(Path, error);
		}
	} cleanup{directory};
	const auto file = directory / "content.txt";
	{
		std::ofstream stream(file);
		stream << "exact file bytes\n";
	}
	std::array<engine::imagegraphexport::GraphFileGrant, 1> grants{{{"read", file, false}}};
	engine::imagegraphexport::GraphFileHost host(grants, {});
	Document document;
	document.Nodes.push_back({"read", "pc.text_file_read", "", {}, {{"path", file.string()}}});
	document.Outputs.push_back({"text", "read", "content"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	request.HostProvider = &host;
	EvaluatedValue value;
	REQUIRE(EvaluateValue(document, plan, "text", request, value, diagnostic) == Status::Ok);
	CHECK(std::get<std::string>(value.Data) == "exact file bytes\n");
	grants[0].NodeId = "other";
	CHECK(EvaluateValue(document, plan, "text", request, value, diagnostic) == Status::UnsupportedExecution);
	grants[0].NodeId = "read";
	grants[0].File = directory / "different";
	CHECK(EvaluateValue(document, plan, "text", request, value, diagnostic) == Status::UnsupportedExecution);
	grants[0].File = file;
	grants[0].Write = true;
	CHECK(EvaluateValue(document, plan, "text", request, value, diagnostic) == Status::UnsupportedExecution);
}

TEST_CASE("File writer publishes typed input without truncating on refusal", "[assetc][imagegraph]") {
	using namespace engine::imagegraph;
	const auto directory = std::filesystem::temp_directory_path() / "atomic-graph-file-write-test";
	std::error_code error;
	std::filesystem::remove_all(directory, error);
	std::filesystem::create_directories(directory);
	struct Cleanup {
		std::filesystem::path Path;
		~Cleanup() {
			std::error_code error;
			std::filesystem::remove_all(Path, error);
		}
	} cleanup{directory};
	const auto file = directory / "content.bin";
	std::array<engine::imagegraphexport::GraphFileGrant, 1> grants{{{"write", file, true}}};
	engine::imagegraphexport::GraphFileHost host(grants, {});
	Node node;
	node.Id = "write";
	node.Type = "pc.byte_file_write";
	std::array<AuthoredValue, 2> inputs{{{"path", file.string()}, {"input_1", BufferValue{{0, 1, 255}}}}};
	EvaluationRequest request;
	HostNodeCapture capture;
	std::string failure;
	REQUIRE(host.Capture({node, request, inputs, {}, 1024}, capture, failure));
	std::ifstream stream(file, std::ios::binary);
	std::string bytes{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
	REQUIRE(bytes.size() == 3);
	CHECK(static_cast<unsigned char>(bytes[2]) == 255);
	stream.close();
	CHECK_FALSE(host.Capture({node, request, inputs, {}, 1}, capture, failure));
	REQUIRE(std::filesystem::file_size(file) == 3);
	CHECK(std::filesystem::is_empty(directory) == false);
	size_t count = 0;
	for (const auto &entry : std::filesystem::directory_iterator(directory)) {
		(void)entry;
		count++;
	}
	CHECK(count == 1);
}

TEST_CASE("CSV source reader returns whole lines and compiled numeric arrays", "[assetc][imagegraph]") {
	using namespace engine::imagegraph;
	const auto directory = std::filesystem::temp_directory_path() / "atomic-graph-csv-test";
	std::filesystem::create_directories(directory);
	struct Cleanup {
		std::filesystem::path Path;
		~Cleanup() {
			std::error_code e;
			std::filesystem::remove_all(Path, e);
		}
	} cleanup{directory};
	const auto file = directory / "data.csv";
	{
		std::ofstream stream(file);
		stream << "12\r\n3.5\ninvalid\n";
	}
	std::array<engine::imagegraphexport::GraphFileGrant, 1> grants{{{"read", file, false}}};
	engine::imagegraphexport::GraphFileHost host(grants, {});
	Document document;
	document.Nodes.push_back(
		{"read", "pc.csv_file_read", "", {}, {{"path", file.string()}, {"convert_to_number", true}}}
	);
	document.Outputs.push_back({"lines", "read", "content"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	request.HostProvider = &host;
	EvaluatedValue result;
	REQUIRE(EvaluateValue(document, plan, "lines", request, result, diagnostic) == Status::Ok);
	const auto &lines = std::get<ArrayValue>(result.Data);
	REQUIRE(lines.Elements.size() == 3);
	CHECK(lines.ElementType == ValueType::Scalar);
	CHECK(std::get<double>(lines.Elements[0]) == 12);
	CHECK(std::get<double>(lines.Elements[1]) == 3.5);
	CHECK(std::get<double>(lines.Elements[2]) == 0);
	document.Nodes[0].Values[1].Data = false;
	{
		std::ofstream stream(file);
		stream << "one,two\nthree,four\n";
	}
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(EvaluateValue(document, plan, "lines", request, result, diagnostic) == Status::Ok);
	CHECK(std::get<std::string>(std::get<ArrayValue>(result.Data).Elements[0]) == "one,two");
}

TEST_CASE("JSON and XML file adapters preserve owned source structures", "[assetc][imagegraph]") {
	using namespace engine::imagegraph;
	const auto directory = std::filesystem::temp_directory_path() / "atomic-graph-doc-test";
	std::filesystem::create_directories(directory);
	struct Cleanup {
		std::filesystem::path Path;
		~Cleanup() {
			std::error_code e;
			std::filesystem::remove_all(Path, e);
		}
	} cleanup{directory};
	const auto jsonFile = directory / "data.json", xmlFile = directory / "data.xml";
	{
		std::ofstream s(jsonFile);
		s << "{\"value\":12,\"nested\":[1,2]}";
	}
	{
		std::ofstream s(xmlFile);
		s << "<?xml version=\"1.0\"?><item key=\"raw&amp;\">value</item>";
	}
	std::array<engine::imagegraphexport::GraphFileGrant, 1> grants{{{"read", jsonFile, false}}};
	engine::imagegraphexport::GraphFileHost host(grants, {});
	Node node;
	node.Id = "read";
	node.Type = "pc.json_file_read";
	std::array<AuthoredValue, 1> inputs{{{"path", jsonFile.string()}}};
	EvaluationRequest request;
	HostNodeCapture capture;
	std::string failure;
	REQUIRE(host.Capture({node, request, inputs, {}, 1048576}, capture, failure));
	REQUIRE(std::get<StructValue>(capture.Outputs[0].Data).Data);
	CHECK(std::get<StructValue>(capture.Outputs[0].Data).Data->Fields.size() == 2);
	node.Type = "pc.xml_file_read";
	grants[0].File = xmlFile;
	inputs[0].Data = xmlFile.string();
	REQUIRE(host.Capture({node, request, inputs, {}, 1048576}, capture, failure));
	const auto &documents = std::get<ArrayValue>(capture.Outputs[0].Data);
	REQUIRE(documents.Elements.size() == 1);
	const auto root = std::get<StructValue>(documents.Elements[0]);
	node.Id = "write";
	node.Type = "pc.xml_file_write";
	grants[0] = {"write", xmlFile, true};
	std::array<AuthoredValue, 2> writeInputs{{{"path", xmlFile.string()}, {"struct", root}}};
	REQUIRE(host.Capture({node, request, writeInputs, {}, 1048576}, capture, failure));
	std::ifstream stream(xmlFile);
	std::string written{std::istreambuf_iterator<char>(stream), {}};
	CHECK(written.find("key=\"raw&\"") != std::string::npos);
	CHECK(written.find(">value</item>") != std::string::npos);
	const auto size = std::filesystem::file_size(xmlFile);
	CHECK_FALSE(host.Capture({node, request, writeInputs, {}, 16}, capture, failure));
	CHECK(std::filesystem::file_size(xmlFile) == size);
}
