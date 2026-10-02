#include "GraphTileHost.hpp"

#include <engine/imagegraphexport/GraphExport.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
TEST_SUITE_ID("engine.imagegraphexport.graph_tile_host")
TEST_DEPENDS("engine.imagegraph.source_tiles")
TEST_DEPENDS("engine.imagegraph.host_capture")
namespace {
	using namespace engine::imagegraph;
	using namespace engine::imagegraphexport;
	struct Fixture {
		std::filesystem::path Directory =
			std::filesystem::temp_directory_path() /
			("atomic-tile-host-" +
			 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		Fixture() {
			std::filesystem::create_directory(Directory);
		}
		~Fixture() {
			std::error_code e;
			std::filesystem::remove_all(Directory, e);
		}
	};
	std::string Read(const std::filesystem::path &path) {
		std::ifstream file(path, std::ios::binary);
		return {(std::istreambuf_iterator<char>(file)), {}};
	}
	Image Map(uint32_t w, uint32_t h, std::initializer_list<double> ids) {
		Image result;
		result.Width = w;
		result.Height = h;
		result.Format = SurfaceFormat::RGBA16Float;
		result.Pixels.resize(size_t(w) * h * 8);
		REQUIRE(ids.size() == size_t(w) * h);
		size_t i = 0;
		for (double red : ids) {
			REQUIRE(StoreSurfacePixel(result, uint32_t(i % w), uint32_t(i / w), {red, 51, 52, 1}));
			++i;
		}
		return result;
	}
	TilesetValue Set() {
		TilesetValue result;
		auto &data = result.Data.emplace();
		data.Texture = {1, 1, {255, 0, 0, 255}, 0};
		data.TileSize = {16, 32};
		data.DisplayName = "NativeSet";
		return result;
	}
	const Value &Field(const StructValue &object, std::string_view name) {
		REQUIRE(object.Data);
		for (const auto &[key, value] : object.Data->Fields)
			if (key == name) return value;
		FAIL("missing JSON field " << name);
		static const Value absent = UndefinedValue{};
		return absent;
	}
	double Numeric(const Value &value) {
		if (const auto *v = std::get_if<double>(&value)) return *v;
		return double(std::get<int64_t>(value));
	}
	StructValue ParseRoom(const std::string &bytes) {
		Document document;
		document.Nodes = {{"parse", "pc.struct_json_parse", "", {}, {{"json_string", bytes}}}};
		document.Outputs = {{"value", "parse", "struct"}};
		Plan plan;
		Diagnostic diagnostic;
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		EvaluatedValue parsed;
		EvaluationRequest request;
		REQUIRE(EvaluateValue(document, plan, "value", request, parsed, diagnostic) == Status::Ok);
		return std::get<StructValue>(std::move(parsed.Data));
	}

} // namespace
TEST_CASE(
	"Tile CSV preserves dimensions and verified half-float string formatting", "[imagegraph][tile_host]"
) {
	Fixture f;
	Node node;
	node.Id = "export";
	node.Type = "pc.tile_tilemap_export";
	const auto original = f.Directory / "tiles.original";
	const auto destination = f.Directory / "tiles.csv";
	std::vector<AuthoredValue> inputs{
		{"input_7", Set()}, {"path", original.string()}, {"format", EnumValue{0}}
	};
	auto map = Map(3, 2, {1.5, 1.125, -1.125, .125, -.125, 65504});
	const std::array<HostResolvedImage, 1> images{{{"tilemap", &map}}};
	std::vector<GraphFileGrant> grants{{node.Id, destination, true}};
	EvaluationRequest request;
	request.Tick = 7;
	request.Subframe = .5;
	request.NegativeFrame = true;
	HostNodeInvocation call{node, request, inputs, images, 16 * 1024 * 1024};
	HostNodeCapture capture;
	std::string failure;
	REQUIRE(CaptureGraphTileFile(call, grants, {}, capture, failure));
	const auto expected = std::string("1.50,1.13,-1.13\n0.13,-0.13,65504\n");
	CHECK(Read(destination) == expected);
	CHECK_FALSE(std::filesystem::exists(original));
	CHECK(capture.Outputs.empty());
	REQUIRE(capture.InputImages.size() == 1);
	CHECK(capture.InputImages[0].Port == "tilemap");
	CHECK(capture.InputImages[0].Hash == SurfaceHash(map));
	CHECK(capture.Tick == 7);
	CHECK(capture.Subframe == .5);
	CHECK(capture.NegativeFrame);
	const auto prior = capture;
	SECTION("exact changed-extension grant") {
		grants[0].File = original;
	}
	SECTION("duplicate primary grant") {
		grants.push_back(grants[0]);
	}
	SECTION("read grant cannot publish") {
		grants[0].Write = false;
	}
	SECTION("wrong source surface format") {
		map.Format = SurfaceFormat::RGBA8Unorm;
	}
	SECTION("nonfinite source sample") {
		map.Pixels[0] = 0;
		map.Pixels[1] = 0x7c;
	}
	SECTION("bounded request") {
		call.MaximumOperationBytes = 32;
	}
	SECTION("missing owned tileset") {
		inputs[0].Data = TilesetValue{};
	}
	CHECK_FALSE(CaptureGraphTileFile(call, grants, {}, capture, failure));
	CHECK(capture.Authored == prior.Authored);
	CHECK(capture.Inputs == prior.Inputs);
	CHECK(capture.Tick == prior.Tick);
	CHECK(capture.Subframe == prior.Subframe);
	CHECK(capture.NegativeFrame == prior.NegativeFrame);
	CHECK(Read(destination) == expected);
	size_t entries = 0;
	for (const auto &entry : std::filesystem::directory_iterator(f.Directory)) {
		(void)entry;
		++entries;
	}
	CHECK(entries == 1);
}
TEST_CASE(
	"Tile room export projects the explicitly granted template and "
	"retains unknown fields",
	"[imagegraph][tile_host]"
) {
	Fixture f;
	Node node;
	node.Id = "room";
	node.Type = "pc.tile_tilemap_export";
	const auto original = f.Directory / "tiles.original";
	const auto destination = f.Directory / "tiles.yy";
	const auto templateFile = f.Directory / "template.yy";
	const std::string templateBytes =
		R"({"parent":{"name":"old","path":"old","extra":7},"layers":[{"name":"old","gridX":1,"gridY":1,"tilesetId":{"name":"old","path":"old"},"tiles":{"SerialiseWidth":1,"SerialiseHeight":1,"TileSerialiseData":[],"unknown":9},"other":true}],"kept":"native"})";
	{
		std::ofstream file(templateFile);
		file << templateBytes;
	}
	std::vector<AuthoredValue> inputs{
		{"input_7", Set()},
		{"path", original.string()},
		{"format", EnumValue{1}},
		{"gm_export_type", EnumValue{0}},
		{"gm_room_name", std::string("Room")},
		{"gm_layer_name", std::string("Tiles")},
		{"gm_room", std::string("intentionally-unused.yy")}
	};
	auto map = Map(2, 2, {1, 2, 3, 4});
	const std::array<HostResolvedImage, 1> images{{{"tilemap", &map}}};
	std::vector<GraphFileGrant> grants{
		{node.Id, destination, true}, {node.Id, templateFile, false, "tileset_gamemaker2_room.yy"}
	};
	EvaluationRequest request;
	HostNodeInvocation call{node, request, inputs, images, 16 * 1024 * 1024};
	HostNodeCapture capture;
	std::string failure;
	REQUIRE(CaptureGraphTileFile(call, grants, {}, capture, failure));
	const auto bytes = Read(destination);
	const auto parsed = ParseRoom(bytes);
	CHECK(std::get<std::string>(Field(parsed, "kept")) == "native");
	const auto &parent = std::get<StructValue>(Field(parsed, "parent"));
	CHECK(Numeric(Field(parent, "extra")) == 7);
	CHECK(std::get<std::string>(Field(parent, "name")) == "Room");
	CHECK(std::get<std::string>(Field(parent, "path")) == "folders/Room.yy");
	const auto &layers = std::get<ArrayValue>(Field(parsed, "layers"));
	REQUIRE(layers.Items.size() == 1);
	const auto &layer = std::get<StructValue>(std::get<ElementValue>(layers.Items[0].Data));
	CHECK(std::get<std::string>(Field(layer, "name")) == "Tiles");
	CHECK(Numeric(Field(layer, "gridX")) == 8);
	CHECK(Numeric(Field(layer, "gridY")) == 16);
	const auto &set = std::get<StructValue>(Field(layer, "tilesetId"));
	CHECK(std::get<std::string>(Field(set, "name")) == "NativeSet");
	CHECK(std::get<std::string>(Field(set, "path")) == "tilesets/NativeSet/NativeSet.yy");
	const auto &tiles = std::get<StructValue>(Field(layer, "tiles"));
	CHECK(Numeric(Field(tiles, "unknown")) == 9);
	CHECK(Numeric(Field(tiles, "SerialiseWidth")) == 2);
	CHECK(Numeric(Field(tiles, "SerialiseHeight")) == 2);
	const auto &ids = std::get<ArrayValue>(Field(tiles, "TileSerialiseData"));
	REQUIRE(ids.Items.size() == 4);
	for (size_t i = 0; i < ids.Items.size(); ++i) {
		const Value value = std::visit(
			[](const auto &leaf) -> Value { return leaf; }, std::get<ElementValue>(ids.Items[i].Data)
		);
		CHECK(Numeric(value) == double(i + 1));
	}
	const auto prior = capture;
	SECTION("template read grant required") {
		grants.pop_back();
	}
	SECTION("wrong template operation refused") {
		grants[1].Write = true;
	}
	SECTION("duplicate template grant refused") {
		grants.push_back(grants[1]);
	}
	SECTION("source nonsquare indexing refused") {
		map = Map(3, 2, {1, 2, 3, 4, 5, 6});
	}
	SECTION("malformed template refused") {
		std::ofstream file(templateFile);
		file << "{";
	}
	SECTION("missing source template structures refused") {
		std::ofstream file(templateFile);
		file << "{}";
	}
	SECTION("native output is bounded") {
		call.MaximumOperationBytes = 64;
	}
	CHECK_FALSE(CaptureGraphTileFile(call, grants, {}, capture, failure));
	CHECK(capture.Authored == prior.Authored);
	CHECK(capture.Inputs == prior.Inputs);
	CHECK(capture.Tick == prior.Tick);
	CHECK(capture.Subframe == prior.Subframe);
	CHECK(capture.NegativeFrame == prior.NegativeFrame);
	CHECK(Read(destination) == bytes);
	CHECK_FALSE(std::filesystem::exists(original));
}
TEST_CASE(
	"Tile export executes only the explicitly selected manual graph callback", "[imagegraph][tile_host]"
) {
	Fixture f;
	const auto output = f.Directory / "manual.csv";
	Document document;
	document.FormatVersion = 9;
	Node exporter;
	exporter.Id = "export";
	exporter.Type = "pc.tile_tilemap_export";
	exporter.Values = {{"path", output.string()}};
	document.Nodes = {exporter, {"map", "image.captured", "", {}, {{"source_id", std::string("map")}}}};
	document.Junctions = {{"tileset", "", ValueType::Tileset, Set()}};
	document.Links = {{"map", "image", "export", "tilemap"}, {"tileset", "value", "export", "input_7"}};
	document.Outputs = {{"preview", "map", "image"}};
	Plan plan;
	Diagnostic diagnostic;
	const auto status = Compile(document, plan, diagnostic);
	INFO(diagnostic.NodeId << ":" << diagnostic.Port << " " << diagnostic.Message);
	REQUIRE(status == Status::Ok);
	std::array<RequestImageSource, 1> sources{{{"map", Map(2, 2, {1, 2, 3, 4})}}};
	std::array<GraphFileGrant, 1> grants{{{"export", output, true}}};
	GraphFileHost host(grants, {});
	EvaluationRequest request;
	request.ImageSources = sources;
	request.HostProvider = &host;
	Image preview;
	REQUIRE(Evaluate(document, plan, "preview", request, preview, diagnostic) == Status::Ok);
	CHECK_FALSE(std::filesystem::exists(output));
	HostNodeCapture capture;
	std::string failure;
	REQUIRE(ExecuteGraphHostNode(document, plan, request, "export", capture, failure));
	CHECK(Read(output) == "1,2\n3,4\n");
	CHECK(capture.Authored.Id == "export");
	// Actual serialized graph reloading executes the same manual capability with
	// a native half surface.
	document.Nodes[1] = {
		"map",
		"pc.solid",
		"",
		{},
		{{"dimension", Vector2{2, 2}},
		 {"dimension_unit", EnumValue{0}},
		 {"color", Colour{255, 0, 0, 255}},
		 {"attribute_color_depth", EnumValue{4}}}
	};
	document.Links[0].FromPort = "surface_out";
	document.Outputs[0].Port = "surface_out";
	GraphExportSettings settings;
	settings.Input = f.Directory / "manual.graph";
	settings.HostProvider = &host;
	{
		std::ofstream file(settings.Input);
		file << Write(document);
	}
	REQUIRE(ExecuteGraphHostNode(settings, "export", capture, failure));
	CHECK(Read(output) == "1,1\n1,1\n");
}
