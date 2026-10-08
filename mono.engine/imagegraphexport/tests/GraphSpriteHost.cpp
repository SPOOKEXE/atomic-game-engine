#include "fixtures/AsepritePoint.hpp"
#include "fixtures/GameMakerRoom.hpp"

#include <engine/imagegraphexport/GraphFileHost.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
TEST_SUITE_ID("engine.imagegraphexport.graph_sprite_host")
TEST_DEPENDS("engine.bake.aseprite")
TEST_DEPENDS("engine.bake.gamemaker_room")
TEST_DEPENDS("engine.imagegraph.host_capture")
using namespace engine::imagegraph;
namespace {
	struct Temporary {
		std::filesystem::path Path =
			std::filesystem::temp_directory_path() /
			("atomic-sprite-host-" +
			 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		Temporary() {
			std::filesystem::create_directory(Path);
		}
		~Temporary() {
			std::error_code e;
			std::filesystem::remove_all(Path, e);
		}
	};
}
TEST_CASE(
	"Aseprite file and derived layer nodes execute through exact grants and recorded object content",
	"[assetc][imagegraph][sprite_host]"
) {
	Temporary temp;
	const auto file = temp.Path / "point.aseprite";
	{
		std::ofstream stream(file, std::ios::binary);
		stream.write(reinterpret_cast<const char *>(AsePoint.data()), AsePoint.size());
	}
	std::array<engine::imagegraphexport::GraphFileGrant, 1> grants{{{"file", file, false}}};
	engine::imagegraphexport::GraphFileHost host(grants, {});
	Node node;
	node.Id = "file";
	node.Type = "pc.ase_file_read";
	node.Values = {{"path", file.string()}};
	ArrayValue loop;
	loop.ElementType = ValueType::Scalar;
	std::array<AuthoredValue, 5> inputs{
		{{"path", file.string()},
		 {"generate_layers", false},
		 {"use_cel_dimension", false},
		 {"current_tag", std::string{}},
		 {"attribute_layer_loop", loop}}
	};
	EvaluationRequest request;
	HostNodeCapture capture;
	std::string failure;
	REQUIRE(host.Capture({node, request, inputs, {}, 1024 * 1024}, capture, failure));
	REQUIRE(capture.Images.size() == 1);
	CHECK(capture.Images.front().Data.Width == 10);
	auto content = std::find_if(capture.Outputs.begin(), capture.Outputs.end(), [](const auto &value) {
		return value.Port == "content";
	});
	REQUIRE(content != capture.Outputs.end());
	CHECK(std::holds_alternative<StructValue>(content->Data));
	const auto &object = std::get<StructValue>(content->Data);
	auto names = std::find_if(object.Data->Fields.begin(), object.Data->Fields.end(), [](const auto &field) {
		return field.first == "layers";
	});
	REQUIRE(names != object.Data->Fields.end());
	auto layer = std::get<std::string>(std::get<ArrayValue>(names->second).Elements.front());
	const auto field = [](const StructValue &object, std::string_view name) -> const Value * {
		auto item =
			std::find_if(object.Data->Fields.begin(), object.Data->Fields.end(), [&](const auto &entry) {
				return entry.first == name;
			});
		return item == object.Data->Fields.end() ? nullptr : &item->second;
	};
	const Value *framesValue = field(object, "Frames");
	REQUIRE(framesValue);
	const auto &frames = std::get<ArrayValue>(*framesValue);
	REQUIRE(frames.Items.size() == 2);
	const auto &frame = std::get<StructValue>(std::get<ElementValue>(frames.Items[0].Data));
	const auto *chunksValue = field(frame, "Chunks");
	REQUIRE(chunksValue);
	const auto &chunks = std::get<ArrayValue>(*chunksValue);
	bool foundBuffer = false;
	for (const auto &item : chunks.Items) {
		const auto &chunk = std::get<StructValue>(std::get<ElementValue>(item.Data));
		if (const auto *buffer = field(chunk, "Buffer")) {
			foundBuffer = true;
			CHECK_FALSE(std::get<BufferValue>(*buffer).Bytes.empty());
		}
	}
	CHECK(foundBuffer);
	const auto raw = std::find_if(capture.Outputs.begin(), capture.Outputs.end(), [](const auto &value) {
		return value.Port == "raw_data";
	});
	REQUIRE(raw != capture.Outputs.end());
	CHECK(std::get<StructValue>(raw->Data) == object);
	Node derived;
	derived.Id = "layer";
	derived.Type = "pc.ase_layer";
	std::array<AuthoredValue, 3> layerInputs{
		{{"ase_data", content->Data}, {"layer_name", layer}, {"crop_output", true}}
	};
	HostNodeCapture extracted;
	REQUIRE(host.Capture({derived, request, layerInputs, {}, 1024 * 1024}, extracted, failure));
	CHECK(extracted.Images.front().Data.Width == 5);
	CHECK(extracted.Images.front().Data.Height == 5);
	CHECK(extracted.SourceUpdateOnFrame == true);
	request.Tick = 2;
	REQUIRE(host.Capture({derived, request, layerInputs, {}, 1024 * 1024}, extracted, failure));
	CHECK(extracted.Images.front().Data.Width == 1);
	CHECK(extracted.Images.front().Data.Pixels == std::vector<uint8_t>{0, 0, 0, 0});
	CHECK(extracted.SourceUpdateOnFrame == true);
	request.Tick = 0;

	Document document;
	document.Nodes = {node};
	document.Outputs = {{"preview", "file", "output"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	request.HostCaptures = {&capture, 1};
	Image image;
	const auto evaluated = Evaluate(document, plan, "preview", request, image, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(evaluated == Status::Ok);
	CHECK(image.Hash == capture.Images.front().Data.Hash);
	grants[0].NodeId = "other";
	const auto previous = capture;
	CHECK_FALSE(host.Capture({node, request, inputs, {}, 1024 * 1024}, capture, failure));
	CHECK(capture.Images.front().Data.Hash == previous.Images.front().Data.Hash);
}
TEST_CASE(
	"GameMaker room node needs an explicit primary read grant and preserves capture on missing dependencies",
	"[assetc][imagegraph][sprite_host]"
) {
	Temporary temp;
	auto file = temp.Path / "room.yy";
	{
		std::ofstream stream(file);
		stream
			<< R"({"resourceType":"GMRoom","roomSettings":{"Width":2,"Height":1},"layers":[{"resourceType":"GMRBackgroundLayer","visible":true,"spriteId":null,"colour":4294901760,"layers":[]}]})";
	}
	std::array<engine::imagegraphexport::GraphFileGrant, 1> grants{{{"room", file, false}}};
	engine::imagegraphexport::GraphFileHost host(grants, {});
	Node node;
	node.Id = "room";
	node.Type = "pc.gmroom";
	EvaluationRequest request;
	HostNodeCapture capture;
	std::string failure;
	REQUIRE(host.Capture({node, request, {}, {}, 1024 * 1024}, capture, failure));
	REQUIRE(capture.Images.size() == 1);
	CHECK(capture.Images[0].Port == "room_preview");
	CHECK(capture.Images[0].Data.Width == 2);
	CHECK(capture.Images[0].Data.Pixels[2] == 255);
	grants[0].Write = true;
	CHECK_FALSE(host.Capture({node, request, {}, {}, 1024 * 1024}, capture, failure));
	CHECK(capture.Images[0].Data.Width == 2);
}

TEST_CASE(
	"GameMaker room assets follow explicit logical YY and thumbnail grants",
	"[assetc][imagegraph][sprite_host]"
) {
	Temporary temp;
	auto room = temp.Path / "room.yy", sprite = temp.Path / "sprite.yy", png = temp.Path / "red.png";
	{
		std::ofstream stream(room);
		stream
			<< R"({"resourceType":"GMRoom","roomSettings":{"Width":2,"Height":1},"layers":[{"resourceType":"GMRAssetLayer","visible":true,"layers":[],"assets":[{"spriteId":{"path":"sprites/red/red.yy"},"x":1,"y":0,"scaleX":1,"scaleY":1,"rotation":0,"colour":4294967295}]}]})";
	}
	{
		std::ofstream stream(sprite);
		stream
			<< R"({"resourceType":"GMSprite","width":1,"height":1,"sequence":{"xorigin":0,"yorigin":0},"frames":[{"name":"frame-a"}],"layers":[{"name":"layer-a"}]})";
	}
	{
		std::ofstream stream(png, std::ios::binary);
		stream.write(reinterpret_cast<const char *>(GameMakerRed.data()), GameMakerRed.size());
	}
	std::array<engine::imagegraphexport::GraphFileGrant, 3> grants{
		{{"room", room, false},
		 {"room", sprite, false, "sprites/red/red.yy"},
		 {"room", png, false, "sprites/red/layers/frame-a/layer-a.png"}}
	};
	engine::imagegraphexport::GraphFileHost host(grants, {});
	Node node;
	node.Id = "room";
	node.Type = "pc.gmroom";
	EvaluationRequest request;
	HostNodeCapture capture;
	std::string failure;
	const bool accepted = host.Capture({node, request, {}, {}, 1024 * 1024}, capture, failure);
	INFO(failure);
	REQUIRE(accepted);
	CHECK(capture.Images[0].Data.Pixels[3] == 0);
	CHECK(capture.Images[0].Data.Pixels[4] == 255);
	CHECK(capture.Images[0].Data.Pixels[7] == 255);
	const auto hash = capture.Images[0].Data.Hash;
	grants[2].Resource = "unrelated-image";
	CHECK_FALSE(host.Capture({node, request, {}, {}, 1024 * 1024}, capture, failure));
	CHECK(capture.Images[0].Data.Hash == hash);
}
TEST_CASE(
	"GameMaker dynamic controls use durable layer bindings and recorded surfaces",
	"[assetc][imagegraph][sprite_host]"
) {
	Temporary temp;
	const auto file = temp.Path / "room.yy";
	{
		std::ofstream stream(file);
		stream
			<< R"({"resourceType":"GMRoom","roomSettings":{"Width":1,"Height":1},"layers":[{"resourceType":"GMRTileLayer","name":"foreground","visible":true,"tilesetId":null}]})";
	}
	std::array<engine::imagegraphexport::GraphFileGrant, 1> grants{{{"room", file, false}}};
	engine::imagegraphexport::GraphFileHost host(grants, {});
	Node node;
	node.Id = "room";
	node.Type = "pc.gmroom";
	DynamicInput input;
	input.Id = "data_8";
	input.Type = ValueType::Any;
	input.SourceLayerName = "foreground";
	node.DynamicInputs.push_back(input);
	StructValue content;
	content.Data.emplace();
	SurfaceValue preview;
	preview.Data.Width = 1;
	preview.Data.Height = 1;
	preview.Data.Pixels = {255, 0, 0, 255};
	preview.Data.Hash = SurfaceHash(preview.Data);
	content.Data->Fields.push_back({"preview", preview});
	const std::array<AuthoredValue, 1> inputs{{{"data_8", content}}};
	EvaluationRequest request;
	HostNodeCapture capture;
	std::string failure;
	const bool accepted = host.Capture({node, request, inputs, {}, 1024 * 1024}, capture, failure);
	INFO(failure);
	REQUIRE(accepted);
	CHECK(capture.Images[0].Data.Pixels[0] == 255);
	const auto hash = capture.Images[0].Data.Hash;
	node.DynamicInputs[0].SourceLayerName = "missing";
	CHECK_FALSE(host.Capture({node, request, inputs, {}, 1024 * 1024}, capture, failure));
	CHECK(capture.Images[0].Data.Hash == hash);
}
