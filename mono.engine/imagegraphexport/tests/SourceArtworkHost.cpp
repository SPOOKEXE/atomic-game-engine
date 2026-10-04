#include "fixtures/AsepritePoint.hpp"

#include <engine/imagegraphexport/GraphFileHost.hpp>
#include <engine/imagegraphio/SourceArtworkEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>

TEST_SUITE_ID("engine.imagegraphexport.source_artwork_host")
TEST_DEPENDS("engine.imagegraphexport.graph_sprite_host")
namespace {
	// Independent ZIP/tile bytes also exercised by the bake layered-image decoder suite.
	constexpr std::array<unsigned char, 834> KritaRawPaint{
		80,	 75,  3,   4,	20,	 0,	  0,   0,	0,	 0,	  4,   128, 66,	 93,  48,  175, 80,	 214, 17,  0,
		0,	 0,	  17,  0,	0,	 0,	  8,   0,	0,	 0,	  109, 105, 109, 101, 116, 121, 112, 101, 97,  112,
		112, 108, 105, 99,	97,	 116, 105, 111, 110, 47,  120, 45,	107, 114, 97,  80,	75,	 3,	  4,   20,
		0,	 0,	  0,   8,	0,	 4,	  128, 66,	93,	 80,  6,   236, 219, 68,  0,   0,	0,	 72,  0,   0,
		0,	 15,  0,   0,	0,	 109, 101, 114, 103, 101, 100, 105, 109, 97,  103, 101, 46,	 112, 110, 103,
		235, 12,  240, 115, 231, 229, 146, 226, 98,	 96,  96,  224, 245, 244, 112, 9,	2,	 210, 76,  64,
		204, 200, 193, 6,	36,	 191, 40,  213, 119, 1,	  41,  126, 79,	 23,  199, 144, 138, 57,  201, 63,
		206, 31,  248, 32,	207, 33,  205, 32,	80,	 201, 92,  87,	155, 124, 238, 58,	80,	 138, 193, 211,
		213, 207, 101, 157, 83,	 66,  19,  0,	80,	 75,  3,   4,	20,	 0,	  0,   0,	8,	 0,	  4,   128,
		66,	 93,  155, 222, 143, 127, 214, 0,	0,	 0,	  48,  1,	0,	 0,	  11,  0,	0,	 0,	  109, 97,
		105, 110, 100, 111, 99,	 46,  120, 109, 108, 53,  142, 205, 110, 131, 48,  16,	132, 95,  101, 187,
		247, 120, 73,  143, 21,	 16,  37,  1,	69,	 81,  127, 18,	69,	 105, 165, 28,	45,	 112, 193, 170,
		99,	 35,  99,  21,	120, 251, 44,  4,	46,	 59,  210, 236, 236, 183, 19,  111, 250, 187, 129, 127,
		229, 91,  237, 108, 130, 107, 17,  225, 38,	 141, 95,  178, 211, 254, 122, 59,	231, 192, 10,  231,
		239, 221, 199, 113, 15,	 184, 34,  122, 207, 114, 162, 236, 154, 193, 159, 215, 65,	 194, 171, 136,
		136, 242, 47,  4,	172, 67,  104, 222, 136, 186, 174, 19,	133, 52,  70,  87,	94,	 10,  231, 171,
		49,	 75,  83,  118, 197, 89,  81,  134, 18,	 211, 120, 100, 182, 131, 13,  178, 255, 89,  222, 242,
		146, 23,  199, 207, 237, 33,  7,   43,	239, 42,  65,  233, 3,	 66,  225, 140, 243, 109, 35,  11,
		245, 52,  47,  135, 221, 22,  161, 211, 101, 168, 249, 6,	161, 86,  186, 170, 3,	 183, 230, 99,
		35,	 7,	  166, 205, 58,	 67,  26,  169, 45,	 99,  172, 43,	85,	 24,  154, 197, 152, 18,  8,   191,
		218, 204, 216, 201, 88,	 35,  244, 9,	70,	 8,	  195, 56,	41,	 141, 105, 33,	210, 212, 139, 149,
		139, 167, 15,  80,	75,	 3,	  4,   20,	0,	 0,	  0,   8,	0,	 4,	  128, 66,	93,	 181, 21,  19,
		243, 107, 0,   0,	0,	 71,  64,  0,	0,	 17,  0,   0,	0,	 97,  114, 116, 47,	 108, 97,  121,
		101, 114, 115, 47,	108, 97,  121, 101, 114, 49,  237, 199, 49,	 14,  130, 48,	0,	 0,	  192, 206,
		125, 69,  31,  192, 0,	 138, 196, 149, 132, 42,  77,  136, 26,	 33,  106, 120, 141, 63,  199, 232,
		39,	 92,  238, 182, 123, 228, 251, 92,	174, 151, 180, 139, 75,	 153, 242, 179, 12,	 203, 152, 186,
		246, 151, 49,  151, 243, 184, 124, 119, 43,	 175, 60,  205, 101, 205, 169, 141, 67,	 191, 244, 169,
		137, 117, 85,  87,	211, 122, 170, 154, 110, 127, 60,  196, 16,	 194, 182, 133, 45,	 188, 3,   0,
		0,	 0,	  0,   0,	0,	 0,	  0,   0,	0,	 0,	  0,   0,	0,	 0,	  240, 55,	31,	 80,  75,  1,
		2,	 20,  3,   20,	0,	 0,	  0,   0,	0,	 4,	  128, 66,	93,	 48,  175, 80,	214, 17,  0,   0,
		0,	 17,  0,   0,	0,	 8,	  0,   0,	0,	 0,	  0,   0,	0,	 0,	  0,   0,	0,	 128, 1,   0,
		0,	 0,	  0,   109, 105, 109, 101, 116, 121, 112, 101, 80,	75,	 1,	  2,   20,	3,	 20,  0,   0,
		0,	 8,	  0,   4,	128, 66,  93,  80,	6,	 236, 219, 68,	0,	 0,	  0,   72,	0,	 0,	  0,   15,
		0,	 0,	  0,   0,	0,	 0,	  0,   0,	0,	 0,	  0,   128, 1,	 55,  0,   0,	0,	 109, 101, 114,
		103, 101, 100, 105, 109, 97,  103, 101, 46,	 112, 110, 103, 80,	 75,  1,   2,	20,	 3,	  20,  0,
		0,	 0,	  8,   0,	4,	 128, 66,  93,	155, 222, 143, 127, 214, 0,	  0,   0,	48,	 1,	  0,   0,
		11,	 0,	  0,   0,	0,	 0,	  0,   0,	0,	 0,	  0,   0,	128, 1,	  168, 0,	0,	 0,	  109, 97,
		105, 110, 100, 111, 99,	 46,  120, 109, 108, 80,  75,  1,	2,	 20,  3,   20,	0,	 0,	  0,   8,
		0,	 4,	  128, 66,	93,	 181, 21,  19,	243, 107, 0,   0,	0,	 71,  64,  0,	0,	 17,  0,   0,
		0,	 0,	  0,   0,	0,	 0,	  0,   0,	0,	 128, 1,   167, 1,	 0,	  0,   97,	114, 116, 47,  108,
		97,	 121, 101, 114, 115, 47,  108, 97,	121, 101, 114, 49,	80,	 75,  5,   6,	0,	 0,	  0,   0,
		4,	 0,	  4,   0,	235, 0,	  0,   0,	65,	 2,	  0,   0,	0,	 0
	};
	using namespace engine::imagegraph;
	uint32_t Load(const std::vector<uint8_t> &bytes, size_t at, size_t count) {
		uint32_t value = 0;
		for (size_t i = 0; i < count; ++i)
			value |= uint32_t(bytes.at(at + i)) << (8 * i);
		return value;
	}
	void Store(std::vector<uint8_t> &bytes, size_t at, uint32_t value, size_t count) {
		for (size_t i = 0; i < count; ++i)
			bytes.at(at + i) = uint8_t(value >> (8 * i));
	}
	void Append(std::vector<uint8_t> &bytes, uint32_t value, size_t count) {
		for (size_t i = 0; i < count; ++i)
			bytes.push_back(uint8_t(value >> (8 * i)));
	}
	std::vector<uint8_t> Tags(std::string_view name, uint16_t first) {
		std::vector<uint8_t> bytes(6, 0);
		Append(bytes, 1, 2);
		bytes.resize(bytes.size() + 8);
		Append(bytes, first, 2);
		Append(bytes, first, 2);
		Append(bytes, 0, 1);
		Append(bytes, 0, 2);
		bytes.resize(bytes.size() + 6);
		Append(bytes, 0x563412, 3);
		Append(bytes, 0, 1);
		Append(bytes, name.size(), 2);
		bytes.insert(bytes.end(), name.begin(), name.end());
		Store(bytes, 0, bytes.size(), 4);
		Store(bytes, 4, 0x2018, 2);
		return bytes;
	}
	std::vector<uint8_t> DuplicateLayerSource() {
		std::vector<uint8_t> bytes(AsePoint.begin(), AsePoint.end());
		size_t at = 144;
		while (Load(bytes, at + 4, 2) != 0x2004)
			at += Load(bytes, at, 4);
		const auto length = Load(bytes, at, 4);
		std::vector<uint8_t> additions(bytes.begin() + at, bytes.begin() + at + length);
		const auto ignored = Tags("discarded", 0);
		additions.insert(additions.end(), ignored.begin(), ignored.end());
		auto selected = Tags("Run", 0);
		const auto second = Tags("Run", 1);
		selected.insert(selected.end(), second.begin() + 16, second.end());
		Store(selected, 6, 2, 2);
		Store(selected, 0, selected.size(), 4);
		additions.insert(additions.end(), selected.begin(), selected.end());
		const auto oldFrameBytes = Load(bytes, 128, 4);
		bytes.insert(bytes.begin() + 128 + oldFrameBytes, additions.begin(), additions.end());
		Store(bytes, 128, oldFrameBytes + additions.size(), 4);
		Store(bytes, 134, Load(bytes, 134, 2) + 3, 2);
		if (Load(bytes, 140, 4)) Store(bytes, 140, Load(bytes, 140, 4) + 3, 4);
		Store(bytes, 0, bytes.size(), 4);
		return bytes;
	}
	struct Temporary {
		static inline std::atomic<uint64_t> Serial{0};
		std::filesystem::path Root =
			std::filesystem::temp_directory_path() /
			("atomic-artwork-host-" +
			 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
			 std::to_string(Serial.fetch_add(1)));
		Temporary() {
			std::filesystem::create_directory(Root);
		}
		~Temporary() {
			std::error_code error;
			std::filesystem::remove_all(Root, error);
		}
	};
	const Value &ObservedOutput(const HostNodeCapture &capture, std::string_view port) {
		const auto found =
			std::find_if(capture.Outputs.begin(), capture.Outputs.end(), [&](const auto &item) {
				return item.Port == port;
			});
		REQUIRE(found != capture.Outputs.end());
		return found->Data;
	}
}
TEST_CASE(
	"Granted ASE host exposes source duplicate aliases and derives the normalized layer and tag",
	"[imagegraph][artwork][sprite_host]"
) {
	Temporary temporary;
	const auto path = temporary.Root / "duplicate.aseprite";
	const auto bytes = DuplicateLayerSource();
	{
		std::ofstream file(path, std::ios::binary);
		file.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
	}
	const engine::imagegraphexport::GraphFileGrant grant{"file", path, false};
	engine::imagegraphexport::GraphFileHost host(std::span(&grant, 1), {});
	Node file{"file", "pc.ase_file_read", {}, {}, {{"path", path.string()}}};
	const AuthoredValue inputs[] = {
		{"path", path.string()}, {"current_tag", std::string{}}, {"use_cel_dimension", false}
	};
	EvaluationRequest request;
	HostNodeCapture captured;
	std::string failure;
	const bool read = host.Capture({file, request, inputs, {}, 4 * 1024 * 1024}, captured, failure);
	INFO(failure);
	REQUIRE(read);
	const auto &layers = std::get<ArrayValue>(ObservedOutput(captured, "layers"));
	REQUIRE(layers.Elements.size() == 2);
	const auto name = std::get<std::string>(layers.Elements[0]);
	CHECK(std::get<std::string>(layers.Elements[1]) == name + "_1");
	const auto &tags = std::get<ArrayValue>(ObservedOutput(captured, "tags"));
	REQUIRE(tags.Elements.size() == 2);
	CHECK(std::get<std::string>(tags.Elements[0]) == "Run");
	CHECK(std::get<std::string>(tags.Elements[1]) == "Run_1");
	Node layer{"layer", "pc.ase_layer", {}, {}, {}};
	const AuthoredValue layerInputs[] = {
		{"ase_data", ObservedOutput(captured, "content")},
		{"layer_name", name},
		{"crop_output", false},
		{"loop", false},
		{"apply_opacity", true}
	};
	HostNodeCapture isolated;
	REQUIRE(host.Capture({layer, request, layerInputs, {}, 4 * 1024 * 1024}, isolated, failure));
	REQUIRE(isolated.Images.size() == 1);
	CHECK(
		std::all_of(
			isolated.Images[0].Data.Pixels.begin(), isolated.Images[0].Data.Pixels.end(), [](uint8_t value) {
				return value == 0;
			}
		)
	);
	std::vector<AuthoredValue> aliasInputs(std::begin(layerInputs), std::end(layerInputs));
	aliasInputs[1].Data = name + "_1";
	const auto priorPixels = isolated.Images[0].Data.Pixels;
	CHECK_FALSE(host.Capture({layer, request, aliasInputs, {}, 4 * 1024 * 1024}, isolated, failure));
	CHECK(failure.find("raw-name map") != std::string::npos);
	CHECK(isolated.Images[0].Data.Pixels == priorPixels);
	Node tag{"tag", "pc.ase_tag", {}, {}, {}};
	const AuthoredValue tagInputs[] = {
		{"ase_data", ObservedOutput(captured, "content")},
		{"tag", std::string("Run_1")},
		{"crop_output", false},
		{"apply_opacity", true}
	};
	HostNodeCapture selected;
	REQUIRE(host.Capture({tag, request, tagInputs, {}, 4 * 1024 * 1024}, selected, failure));
	CHECK(std::get<Vector2>(ObservedOutput(selected, "frame_range")) == Vector2{1, 1});
	REQUIRE(selected.Images.size() == 1);
	REQUIRE(captured.Images.size() == 1);
	REQUIRE(selected.Images[0].Data.Pixels.size() == 400);
	REQUIRE(captured.Images[0].Data.Pixels.size() == 400);
	// The fixture's indexed cel is 5x5, green palette index 1, transparent index 0.
	// Frame 1 links that cel at (0,0); the file preview reads frame 0 at (2,2).
	const auto golden = [](size_t offset) {
		std::vector<uint8_t> pixels(400, 0);
		constexpr std::string_view rows[]{"01111", "11110", "11101", "11011", "10111"};
		for (size_t y = 0; y < 5; ++y)
			for (size_t x = 0; x < 5; ++x)
				if (rows[y][x] == '1') {
					const size_t at = ((y + offset) * 10 + x + offset) * 4;
					pixels[at] = 36;
					pixels[at + 1] = 142;
					pixels[at + 2] = 96;
					pixels[at + 3] = 255;
				}
		return pixels;
	};
	CHECK(selected.Images[0].Data.Width == 10);
	CHECK(selected.Images[0].Data.Height == 10);
	CHECK(captured.Images[0].Data.Width == 10);
	CHECK(captured.Images[0].Data.Height == 10);
	CHECK(selected.Images[0].Data.Pixels == golden(0));
	CHECK(captured.Images[0].Data.Pixels == golden(2));
	engine::imagegraphio::SourceArtworkMetadata metadata;
	Diagnostic diagnostic;
	REQUIRE(engine::imagegraphio::ReadSourceArtworkMetadata(captured, metadata, diagnostic) == Status::Ok);
	CHECK(metadata.Layers[1].Name == name + "_1");
	CHECK(metadata.Tags[1].Name == "Run_1");
}

TEST_CASE(
	"Granted Krita host maps source layerDat to normalized layerData authoring content",
	"[imagegraph][artwork][layered_host]"
) {
	Temporary temporary;
	const auto path = temporary.Root / "paint.kra";
	{
		std::ofstream file(path, std::ios::binary);
		file.write(reinterpret_cast<const char *>(KritaRawPaint.data()), KritaRawPaint.size());
	}
	const engine::imagegraphexport::GraphFileGrant grant{"file", path, false};
	engine::imagegraphexport::GraphFileHost host(std::span(&grant, 1), {});
	Node file{"file", "pc.krita_file_read", {}, {}, {{"path", path.string()}}};
	const AuthoredValue inputs[] = {{"path", path.string()}};
	HostNodeCapture captured;
	EvaluationRequest request;
	std::string failure;
	REQUIRE(host.Capture({file, request, inputs, {}, 8 * 1024 * 1024}, captured, failure));
	const auto &content = std::get<StructValue>(ObservedOutput(captured, "content"));
	REQUIRE(content.Data);
	CHECK(std::any_of(content.Data->Fields.begin(), content.Data->Fields.end(), [](const auto &field) {
		return field.first == "layerData";
	}));
	CHECK(std::none_of(content.Data->Fields.begin(), content.Data->Fields.end(), [](const auto &field) {
		return field.first == "layerDat";
	}));
	engine::imagegraphio::SourceArtworkMetadata metadata;
	Diagnostic diagnostic;
	REQUIRE(engine::imagegraphio::ReadSourceArtworkMetadata(captured, metadata, diagnostic) == Status::Ok);
	REQUIRE(metadata.Layers.size() == 1);
	CHECK(metadata.Layers[0].Name == "paint");
	Document document;
	document.FormatVersion = 9;
	document.Nodes.push_back(file);
	GroupReplayState empty, initialized, replay;
	REQUIRE(RebindGroupReplay(document, empty, 7, initialized, diagnostic) == Status::Ok);
	REQUIRE(BindGroupReplay(document, {}, initialized, 7, replay, diagnostic) == Status::Ok);
	Document candidate;
	GroupReplayState next;
	REQUIRE(
		engine::imagegraphio::ApplySourceArtworkEdit(
			document,
			captured,
			replay,
			{engine::imagegraphio::SourceArtworkAction::GenerateLayers, true, true, 7, 8, std::nullopt},
			candidate,
			next,
			diagnostic
		) == Status::Ok
	);
	REQUIRE(candidate.Nodes.size() == 2);
	CHECK(candidate.Nodes[1].Type == "pc.krita_layer");
	CHECK(candidate.Nodes[1].SourceDisplayName == "paint");
	REQUIRE(candidate.Links.size() == 1);
	CHECK(candidate.Links[0].FromNode == "file");
	CHECK(candidate.Links[0].ToNode == candidate.Nodes[1].Id);
	candidate.Outputs.push_back({"layer", candidate.Nodes[1].Id, "surface_out"});
	Plan plan;
	const auto compiled = Compile(candidate, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	request.HostProvider = &host;
	Image evaluated;
	const auto evaluatedStatus = Evaluate(candidate, plan, "layer", request, evaluated, diagnostic);
	INFO(diagnostic.NodeId);
	INFO(diagnostic.Port);
	INFO(diagnostic.Message);
	REQUIRE(evaluatedStatus == Status::Ok);
	REQUIRE(captured.Images.size() == 1);
	CHECK(evaluated.Width == captured.Images[0].Data.Width);
	CHECK(evaluated.Height == captured.Images[0].Data.Height);
	CHECK(evaluated.Pixels == captured.Images[0].Data.Pixels);
	CHECK(evaluated.Hash == captured.Images[0].Data.Hash);
}
