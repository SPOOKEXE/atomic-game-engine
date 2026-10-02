#include <engine/imagegraphexport/GraphExport.hpp>
#include <engine/imagegraphexport/GraphFileHost.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

TEST_CASE(
	"WAV host publishes canonical PCM only through its resolved destination grant", "[imagegraph][wav]"
) {
	using namespace engine::imagegraph;
	using namespace engine::imagegraphexport;
	const auto directory = std::filesystem::temp_directory_path() / "atomic-graph-wav-host-test";
	std::filesystem::create_directories(directory);
	struct Cleanup {
		std::filesystem::path Path;
		~Cleanup() {
			std::error_code error;
			std::filesystem::remove_all(Path, error);
		}
	} cleanup{directory};
	const auto path = directory / "sample.wav";
	Node node;
	node.Id = "wav";
	node.Type = "pc.wav_file_write";
	ArrayValue channels;
	channels.ElementType = ValueType::Scalar;
	channels.Nested = {{0., 255., 127.}};
	std::vector<AuthoredValue> inputs{
		{"path", (directory / "sample").string()},
		{"audio_data", channels},
		{"sample", int64_t{44100}},
		{"bit_depth", EnumValue{0}},
		{"remap_data", false},
		{"data_range", Vector2{0, 1}}
	};
	std::array<GraphFileGrant, 1> grants{{{"wav", path, true}}};
	GraphFileHost host(grants, {});
	EvaluationRequest request;
	HostNodeInvocation invocation{node, request, inputs, {}, 1024 * 1024};
	HostNodeCapture capture;
	std::string failure;
	REQUIRE(host.Capture(invocation, capture, failure));
	std::ifstream stream(path, std::ios::binary);
	std::string bytes((std::istreambuf_iterator<char>(stream)), {});
	stream.close();
	REQUIRE(bytes.size() == 48);
	CHECK(bytes.substr(0, 4) == "RIFF");
	CHECK(bytes.substr(8, 4) == "WAVE");
	CHECK(uint8_t(bytes[44]) == 0);
	CHECK(uint8_t(bytes[45]) == 255);
	CHECK(uint8_t(bytes[46]) == 127);
	CHECK(uint8_t(bytes[47]) == 0);
	CHECK(capture.Outputs.empty());
	Document document;
	document.FormatVersion = 9;
	Node authored = node;
	authored.Values = inputs;
	ArrayValue authoredChannels;
	authoredChannels.ElementType = ValueType::Scalar;
	authoredChannels.Nested = {{0., 127., 255.}, {255., 64., 0.}};
	authored.Values[1].Data = authoredChannels;
	bool emptyDefault = false;
	SECTION("authored two-channel source controls") {}
	SECTION("unconnected source channel default") {
		emptyDefault = true;
		authored.Values.erase(authored.Values.begin() + 1);
	}
	document.Nodes = {
		authored,
		{"preview",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{0, 0, 0, 1}}}}
	};
	document.Outputs = {{"preview", "preview", "image"}};
	Plan plan;
	Diagnostic diagnostic;
	const auto status = Compile(document, plan, diagnostic);
	INFO(diagnostic.NodeId << ":" << diagnostic.Port << " " << diagnostic.Message);
	REQUIRE(status == Status::Ok);
	request.HostProvider = &host;
	std::filesystem::remove(path);
	Image preview;
	REQUIRE(Evaluate(document, plan, "preview", request, preview, diagnostic) == Status::Ok);
	CHECK_FALSE(std::filesystem::exists(path));
	REQUIRE(ExecuteGraphHostNode(document, plan, request, "wav", capture, failure));
	CHECK(std::filesystem::exists(path));
	std::ifstream exported(path, std::ios::binary);
	std::string manualBytes((std::istreambuf_iterator<char>(exported)), {});
	exported.close();
	REQUIRE(manualBytes.size() == (emptyDefault ? 44 : 50));
	CHECK(uint8_t(manualBytes[22]) == (emptyDefault ? 1 : 2));
	if (!emptyDefault) {
		const std::array<uint8_t, 6> interleaved{0, 255, 127, 64, 255, 0};
		for (size_t i = 0; i < interleaved.size(); ++i)
			CHECK(uint8_t(manualBytes[44 + i]) == interleaved[i]);
	}
	REQUIRE(host.Capture(invocation, capture, failure));

	grants[0].Write = false;
	CHECK_FALSE(host.Capture(invocation, capture, failure));
	grants[0].Write = true;
	inputs[3].Data = 0.5;
	CHECK_FALSE(host.Capture(invocation, capture, failure));
	inputs[3].Data = EnumValue{0};
	invocation.MaximumOperationBytes = 8;
	CHECK_FALSE(host.Capture(invocation, capture, failure));
	std::ifstream preserved(path, std::ios::binary);
	CHECK(std::string((std::istreambuf_iterator<char>(preserved)), {}) == bytes);
	grants[0].File = directory / "other.wav";
	invocation.MaximumOperationBytes = 1024 * 1024;
	CHECK_FALSE(host.Capture(invocation, capture, failure));
	CHECK_FALSE(std::filesystem::exists(grants[0].File));
}

TEST_CASE("Image file host admits headers and preserves exact padded pixels", "[imagegraph][raster_host]") {
	using namespace engine::imagegraph;
	using namespace engine::imagegraphexport;
	const auto file = std::filesystem::temp_directory_path() / "atomic-graph-raster-host.bmp";
	struct Cleanup {
		std::filesystem::path File;
		~Cleanup() {
			std::error_code e;
			std::filesystem::remove(File, e);
		}
	} cleanup{file};
	std::vector<uint8_t> bmp(62);
	const auto word = [&](size_t at, uint32_t value, size_t count) {
		for (size_t i = 0; i < count; ++i)
			bmp[at + i] = uint8_t(value >> (8 * i));
	};
	bmp[0] = 'B';
	bmp[1] = 'M';
	word(2, 62, 4);
	word(10, 54, 4);
	word(14, 40, 4);
	word(18, 2, 4);
	word(22, 1, 4);
	word(26, 1, 2);
	word(28, 24, 2);
	bmp[54] = 30;
	bmp[55] = 20;
	bmp[56] = 10;
	bmp[57] = 60;
	bmp[58] = 50;
	bmp[59] = 40;
	const auto write = [&] {
		std::ofstream stream(file, std::ios::binary);
		stream.write(reinterpret_cast<const char *>(bmp.data()), std::streamsize(bmp.size()));
	};
	write();
	Node node;
	node.Id = "image";
	node.Type = "pc.image";
	std::vector<AuthoredValue> inputs{{"path", file.string()}, {"padding", Vector4{1, 1, 2, 0}}};
	std::array<GraphFileGrant, 1> grants{{{"image", file, false}}};
	GraphFileHost host(grants, {});
	EvaluationRequest request;
	HostNodeInvocation invocation{node, request, inputs, {}, 1024 * 1024};
	HostNodeCapture capture;
	std::string failure;
	REQUIRE(host.Capture(invocation, capture, failure));
	REQUIRE(capture.Images.size() == 1);
	const auto &image = capture.Images[0].Data;
	CHECK(image.Width == 5);
	CHECK(image.Height == 2);
	SurfacePixel pixel;
	REQUIRE(LoadSurfacePixel(image, 2, 1, pixel));
	CHECK(pixel == SurfacePixel{10. / 255, 20. / 255, 30. / 255, 1});
	REQUIRE(LoadSurfacePixel(image, 3, 1, pixel));
	CHECK(pixel == SurfacePixel{40. / 255, 50. / 255, 60. / 255, 1});
	REQUIRE(LoadSurfacePixel(image, 0, 0, pixel));
	CHECK(pixel == SurfacePixel{});

	SECTION("ordered image array requires its own exact resource grants") {
		node.Type = "pc.image_sequence";
		ArrayValue paths;
		paths.ElementType = ValueType::Text;
		paths.Elements = {file.string(), file.string()};
		std::vector<AuthoredValue> sequenceInputs{
			{"paths", paths},
			{"padding", Vector4{}},
			{"canvas_size", EnumValue{0}},
			{"sizing_method", EnumValue{0}}
		};
		grants[0].Resource = file.string();
		HostNodeInvocation sequence{node, request, sequenceInputs, {}, 1024 * 1024};
		REQUIRE(host.Capture(sequence, capture, failure));
		REQUIRE(capture.ImageArrays.size() == 1);
		CHECK(capture.ImageArrays[0].Frames.size() == 2);
		CHECK(capture.ImageArrays[0].Frames[0].Width == 2);
		CHECK(capture.ImageArrays[0].Frames[0] == capture.ImageArrays[0].Frames[1]);

		node.Values = sequenceInputs;
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {node};
		document.Outputs = {
			{"images", node.Id, "surfaces_out"},
			{"paths", node.Id, "paths"},
			{"dimensions", node.Id, "dimensions"}
		};
		Plan plan;
		Diagnostic diagnostic;
		const auto status = Compile(document, plan, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << " " << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		request.HostProvider = &host;
		ImageArray evaluated;
		REQUIRE(EvaluateArray(document, plan, "images", request, evaluated, diagnostic) == Status::Ok);
		REQUIRE(evaluated.Images.size() == 2);
		CHECK(evaluated.Images[0].Pixels == capture.ImageArrays[0].Frames[0].Pixels);
		EvaluatedValue value;
		REQUIRE(EvaluateValue(document, plan, "paths", request, value, diagnostic) == Status::Ok);
		CHECK(std::get<ArrayValue>(value.Data).Elements.size() == 2);
		REQUIRE(ExecuteGraphHostNode(document, plan, request, node.Id, capture, failure));
		CHECK(capture.ImageArrays[0].Frames.size() == 2);
		grants[0].Resource.clear();
		CHECK_FALSE(host.Capture(sequence, capture, failure));
		return;
	}
	const auto old = capture.Images[0].Data;
	word(18, 100000, 4);
	write();
	CHECK_FALSE(host.Capture(invocation, capture, failure));
	CHECK(capture.Images[0].Data == old);
	grants[0].Write = true;
	CHECK_FALSE(host.Capture(invocation, capture, failure));
}
