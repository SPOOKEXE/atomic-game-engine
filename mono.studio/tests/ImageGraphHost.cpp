#include "ImageGraphHost.hpp"

#include <engine/imagegraph/FrameTime.hpp>
#include <engine/imagegraphexport/GraphExport.hpp>
#include <engine/scripthost/ComposerLua.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <fstream>
TEST_SUITE_ID("studio.imagegraph.host")
TEST_DEPENDS("studio.imagegraph")
using namespace engine::imagegraph;
namespace {
	struct Temporary {
		std::filesystem::path Path =
			std::filesystem::temp_directory_path() /
			("studio-file-host-" +
			 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		Temporary() {
			std::filesystem::create_directories(Path);
		}
		~Temporary() {
			std::error_code ignored;
			std::filesystem::remove_all(Path, ignored);
		}
	};
	void WriteRoom(const std::filesystem::path &path, bool red) {
		std::ofstream file(path);
		file
			<< R"({"resourceType":"GMRoom","roomSettings":{"Width":2,"Height":1},"layers":[{"resourceType":"GMRBackgroundLayer","visible":true,"spriteId":null,"colour":)"
			<< (red ? "4278190335" : "4294901760") << R"(,"layers":[]}]})";
	}
} // namespace
TEST_CASE(
	"Studio granted file observations retain bytes across seeks and "
	"require refresh and grants",
	"[studio][file_host]"
) {
	Temporary temp;
	const auto path = temp.Path / "room.yy";
	WriteRoom(path, false);
	Document document;
	document.FormatVersion = 9;
	Node room;
	room.Id = "room";
	room.Type = "pc.gmroom";
	document.Nodes = {room};
	document.Outputs = {{"preview", "room", "room_preview"}};
	Plan plan;
	Diagnostic error;
	REQUIRE(Compile(document, plan, error) == Status::Ok);
	std::vector<engine::imagegraphexport::GraphFileGrant> grants{{"room", path, false}};
	studio::detail::ImageGraphHost host;
	host.Grants = grants;
	EvaluationRequest request;
	request.HostProvider = &host;
	REQUIRE(SetFrameTime(request, {2, .5, true}));
	HostNodeCapture capture;
	std::string failure;
	const auto execute = [&] {
		const bool ok =
			engine::imagegraphexport::ExecuteGraphHostNode(document, plan, request, "room", capture, failure);
		INFO(failure);
		return ok;
	};
	REQUIRE(execute());
	CHECK(capture.Tick == 2);
	CHECK(capture.Subframe == .5);
	CHECK(capture.NegativeFrame);
	REQUIRE(capture.Images.size() == 1);
	const auto original = capture.Images[0].Data.Pixels;
	const auto retained = host.RetainedBytes;
	WriteRoom(path, true);
	REQUIRE(SetFrameTime(request, {4, .25, false}));
	REQUIRE(execute());
	CHECK(capture.Tick == 4);
	CHECK(capture.Subframe == .25);
	CHECK_FALSE(capture.NegativeFrame);
	CHECK(capture.Images[0].Data.Pixels == original);
	CHECK(host.RetainedBytes == retained);
	HostNodeCapture previous = capture;
	HostNodeInvocation tiny{document.Nodes[0], request, {}, {}, retained};
	CHECK_FALSE(host.Capture(tiny, capture, failure));
	CHECK(capture.Images[0].Data.Pixels == previous.Images[0].Data.Pixels);
	CHECK(host.RetainedBytes == retained);
	host.Grants = {};
	CHECK_FALSE(execute());
	CHECK(capture.Images[0].Data.Pixels == original);
	host.Grants = grants;
	host.RefreshFile("room");
	REQUIRE(execute());
	CHECK(capture.Images[0].Data.Pixels != original);
	host.ResetFiles();
	CHECK(host.RetainedBytes == sizeof(host.Files));
}
TEST_CASE("Studio composite host delegates Lua and refuses ungranted file access", "[studio][file_host]") {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"lua",
		 "pc.lua_compute",
		 "",
		 {},
		 {{"lua_code", std::string{"return 7"}},
		  {"function_name", std::string{"runFixture"}},
		  {"execute_on_frame", true}}}
	};
	document.Outputs = {{"result", "lua", "return_value"}};
	auto lua = engine::script::MakeComposerLuaHost();
	studio::detail::ImageGraphHost host;
	host.Lua = lua.get();
	Plan plan;
	Diagnostic error;
	REQUIRE(Compile(document, plan, error) == Status::Ok);
	EvaluationRequest request;
	request.HostProvider = &host;
	EvaluatedValue value;
	REQUIRE(EvaluateValue(document, plan, "result", request, value, error) == Status::Ok);
	CHECK(value.Data == Value{7.});
	Node fileNode;
	fileNode.Id = "ungranted";
	fileNode.Type = "pc.gmroom";
	HostNodeCapture capture;
	capture.Failure = "preserved";
	std::string failure;
	CHECK_FALSE(host.Capture({fileNode, request, {}, {}, 1024 * 1024}, capture, failure));
	CHECK(capture.Failure == "preserved");
	CHECK(failure.find("Grant") != std::string::npos);
}
TEST_CASE(
	"Studio GMRoom resources require exact logical grants even after a successful observation",
	"[studio][file_host]"
) {
	Temporary temp;
	const auto room = temp.Path / "room.yy", sprite = temp.Path / "sprite.yy", png = temp.Path / "red.png";
	{
		std::ofstream file(room);
		file
			<< R"({"resourceType":"GMRoom","roomSettings":{"Width":2,"Height":1},"layers":[{"resourceType":"GMRAssetLayer","visible":true,"layers":[],"assets":[{"spriteId":{"path":"sprites/red/red.yy"},"x":1,"y":0,"scaleX":1,"scaleY":1,"rotation":0,"colour":4294967295}]}]})";
	}
	{
		std::ofstream file(sprite);
		file
			<< R"({"resourceType":"GMSprite","width":1,"height":1,"sequence":{"xorigin":0,"yorigin":0},"frames":[{"name":"frame-a"}],"layers":[{"name":"layer-a"}]})";
	}
	constexpr std::array<unsigned char, 70> red{137, 80, 78, 71, 13, 10,  26,  10,	0,	 0,	  0,   13,
												73,	 72, 68, 82, 0,	 0,	  0,   1,	0,	 0,	  0,   1,
												8,	 6,	 0,	 0,	 0,	 31,  21,  196, 137, 0,	  0,   0,
												13,	 73, 68, 65, 84, 120, 156, 99,	248, 207, 192, 240,
												31,	 0,	 5,	 0,	 1,	 255, 137, 153, 61,	 29,  0,   0,
												0,	 0,	 73, 69, 78, 68,  174, 66,	96,	 130};
	{
		std::ofstream file(png, std::ios::binary);
		file.write(reinterpret_cast<const char *>(red.data()), red.size());
	}
	std::array<engine::imagegraphexport::GraphFileGrant, 3> grants{
		{{"room", room, false},
		 {"room", sprite, false, "sprites/red/red.yy"},
		 {"room", png, false, "sprites/red/layers/frame-a/layer-a.png"}}
	};
	Node node;
	node.Id = "room";
	node.Type = "pc.gmroom";
	EvaluationRequest request;
	studio::detail::ImageGraphHost host;
	host.Grants = std::span(grants).first(1);
	HostNodeCapture capture;
	capture.Failure = "preserved";
	std::string failure;
	const auto invoke = [&] { return host.Capture({node, request, {}, {}, 1024 * 1024}, capture, failure); };
	CHECK_FALSE(invoke());
	CHECK(capture.Failure == "preserved");
	host.Grants = std::span(grants).first(2);
	CHECK_FALSE(invoke());
	CHECK(capture.Failure == "preserved");
	host.Grants = grants;
	REQUIRE(invoke());
	CHECK(capture.Images[0].Data.Pixels[3] == 0);
	CHECK(capture.Images[0].Data.Pixels[4] == 255);
	CHECK(capture.Images[0].Data.Pixels[7] == 255);
	const auto retained = host.RetainedBytes;
	grants[2].Resource = "unrelated-image";
	CHECK_FALSE(invoke());
	CHECK(capture.Images[0].Data.Pixels[4] == 255);
	CHECK(host.RetainedBytes == retained);
}

TEST_CASE(
	"Studio derived ASE surfaces use owned source content at each exact frame without file grants",
	"[studio][file_host]"
) {
	Temporary temp;
	const auto path = temp.Path / "point.aseprite";
	constexpr std::array<unsigned char, 289> asePoint{
		33,	 1,	  0,  0, 224, 165, 2,	0,	10,	 0,	  10, 0,  8,  0,   3,	0,	0,	0,	 100, 0,  0, 0,	  0,
		0,	 0,	  0,  0, 0,	  0,   0,	0,	0,	 2,	  0,  1,  1,  0,   0,	0,	0,	0,	 0,	  0,  0, 0,	  0,
		0,	 0,	  0,  0, 0,	  0,   0,	0,	0,	 0,	  0,  0,  0,  0,   0,	0,	0,	0,	 0,	  0,  0, 0,	  0,
		0,	 0,	  0,  0, 0,	  0,   0,	0,	0,	 0,	  0,  0,  0,  0,   0,	0,	0,	0,	 0,	  0,  0, 0,	  0,
		0,	 0,	  0,  0, 0,	  0,   0,	0,	0,	 0,	  0,  0,  0,  0,   0,	0,	0,	0,	 0,	  0,  0, 0,	  0,
		0,	 0,	  0,  0, 0,	  0,   0,	0,	0,	 0,	  0,  0,  0,  121, 0,	0,	0,	250, 241, 3,  0, 100, 0,
		0,	 0,	  0,  0, 0,	  0,   27,	0,	0,	 0,	  4,  32, 1,  0,   0,	0,	0,	0,	 0,	  0,  0, 0,	  0,
		0,	 255, 0,  0, 0,	  3,   0,	73, 110, 107, 38, 0,  0,  0,   25,	32, 2,	0,	 0,	  0,  0, 0,	  0,
		0,	 1,	  0,  0, 0,	  0,   0,	0,	0,	 0,	  0,  0,  0,  0,   0,	0,	0,	0,	 0,	  0,  0, 36,  142,
		96,	 255, 40, 0, 0,	  0,   5,	32, 0,	 0,	  2,  0,  2,  0,   255, 2,	0,	0,	 0,	  0,  0, 0,	  0,
		0,	 5,	  0,  5, 0,	  120, 218, 99, 96,	 132, 2,  6,  52, 12,  0,	1,	29, 0,	 21,  40, 0, 0,	  0,
		250, 241, 1,  0, 100, 0,   0,	0,	0,	 0,	  0,  0,  24, 0,   0,	0,	5,	32,	 0,	  0,  0, 0,	  0,
		0,	 255, 1,  0, 0,	  0,   0,	0,	0,	 0,	  0,  0,  0
	};

	{
		std::ofstream file(path, std::ios::binary);
		file.write(reinterpret_cast<const char *>(asePoint.data()), asePoint.size());
	}
	Node fileNode;
	fileNode.Id = "file";
	fileNode.Type = "pc.ase_file_read";
	fileNode.Values = {{"path", path.string()}};
	ArrayValue loop;
	loop.ElementType = ValueType::Scalar;
	std::array<AuthoredValue, 5> inputs{
		{{"path", path.string()},
		 {"generate_layers", false},
		 {"use_cel_dimension", false},
		 {"current_tag", std::string{}},
		 {"attribute_layer_loop", loop}}
	};
	std::array<engine::imagegraphexport::GraphFileGrant, 1> grants{{{"file", path, false}}};
	studio::detail::ImageGraphHost host;
	host.Grants = grants;
	EvaluationRequest request;
	HostNodeCapture content;
	std::string failure;
	REQUIRE(host.Capture({fileNode, request, inputs, {}, 1024 * 1024}, content, failure));
	const auto object = std::find_if(content.Outputs.begin(), content.Outputs.end(), [](const auto &v) {
		return v.Port == "content";
	});
	REQUIRE(object != content.Outputs.end());
	const auto &fields = std::get<StructValue>(object->Data).Data->Fields;
	const auto names =
		std::find_if(fields.begin(), fields.end(), [](const auto &f) { return f.first == "layers"; });
	REQUIRE(names != fields.end());
	const auto layer = std::get<std::string>(std::get<ArrayValue>(names->second).Elements.front());
	Node derived;
	derived.Id = "layer";
	derived.Type = "pc.ase_layer";
	std::array<AuthoredValue, 3> layerInputs{
		{{"ase_data", object->Data}, {"layer_name", layer}, {"crop_output", true}}
	};
	HostNodeCapture capture;
	host.Grants = {};
	REQUIRE(host.Capture({derived, request, layerInputs, {}, 1024 * 1024}, capture, failure));
	CHECK(capture.Images[0].Data.Width == 5);
	CHECK(capture.Images[0].Data.Height == 5);
	const auto retained = host.RetainedBytes;
	request.Tick = 2;
	REQUIRE(host.Capture({derived, request, layerInputs, {}, 1024 * 1024}, capture, failure));
	CHECK(capture.Tick == 2);
	CHECK(capture.Images[0].Data.Width == 1);
	CHECK(capture.Images[0].Data.Pixels == std::vector<uint8_t>{0, 0, 0, 0});
	CHECK(host.RetainedBytes == retained);
}

TEST_CASE(
	"Studio raster observations respect output precision at unchanged controls", "[studio][file_host]"
) {
	Temporary temp;
	const auto path = temp.Path / "red.png";
	constexpr std::array<unsigned char, 70> red{137, 80, 78, 71, 13, 10,  26,  10,	0,	 0,	  0,   13,
												73,	 72, 68, 82, 0,	 0,	  0,   1,	0,	 0,	  0,   1,
												8,	 6,	 0,	 0,	 0,	 31,  21,  196, 137, 0,	  0,   0,
												13,	 73, 68, 65, 84, 120, 156, 99,	248, 207, 192, 240,
												31,	 0,	 5,	 0,	 1,	 255, 137, 153, 61,	 29,  0,   0,
												0,	 0,	 73, 69, 78, 68,  174, 66,	96,	 130};
	{
		std::ofstream file(path, std::ios::binary);
		file.write(reinterpret_cast<const char *>(red.data()), red.size());
	}
	Node node;
	node.Id = "image";
	node.Type = "pc.image";
	std::array<AuthoredValue, 2> inputs{{{"path", path.string()}, {"padding", Vector4{}}}};
	std::array<engine::imagegraphexport::GraphFileGrant, 1> grants{{{"image", path, false}}};
	studio::detail::ImageGraphHost host;
	host.Grants = grants;
	EvaluationRequest request;
	HostNodeCapture capture;
	std::string failure;
	HostNodeInvocation invocation{node, request, inputs, {}, 1024 * 1024};
	invocation.OutputFormat = SurfaceFormat::RGBA8Unorm;
	REQUIRE(host.Capture(invocation, capture, failure));
	REQUIRE(capture.Images.size() == 1);
	CHECK(capture.Images[0].Data.Format == SurfaceFormat::RGBA8Unorm);
	CHECK(capture.Images[0].Data.Pixels.size() == 4);
	invocation.OutputFormat = SurfaceFormat::RGBA32Float;
	REQUIRE(host.Capture(invocation, capture, failure));
	CHECK(capture.Images[0].Data.Format == SurfaceFormat::RGBA32Float);
	CHECK(capture.Images[0].Data.Pixels.size() == 16);
	SurfacePixel pixel{};
	REQUIRE(LoadSurfacePixel(capture.Images[0].Data, 0, 0, pixel));
	CHECK(pixel == SurfacePixel{1, 0, 0, 1});
	invocation.OutputFormat = SurfaceFormat::RGBA8Unorm;
	REQUIRE(host.Capture(invocation, capture, failure));
	CHECK(capture.Images[0].Data.Format == SurfaceFormat::RGBA8Unorm);
	CHECK(capture.Images[0].Data.Pixels.size() == 4);

	Node sequence;
	sequence.Id = "sequence";
	sequence.Type = "pc.image_sequence";
	ArrayValue paths;
	paths.ElementType = ValueType::Text;
	paths.Elements = {path.string()};
	std::array<AuthoredValue, 4> sequenceInputs{
		{{"paths", paths},
		 {"padding", Vector4{}},
		 {"canvas_size", EnumValue{0}},
		 {"sizing_method", EnumValue{0}}}
	};
	std::array<engine::imagegraphexport::GraphFileGrant, 1> resources{
		{{"sequence", path, false, path.string()}}
	};
	host.Grants = resources;
	REQUIRE(host.Capture(
		{sequence, request, sequenceInputs, {}, 1024 * 1024, nullptr, SurfaceFormat::RGBA32Float},
		capture,
		failure
	));
	REQUIRE(capture.ImageArrays.size() == 1);
	REQUIRE(capture.ImageArrays[0].Frames.size() == 1);
	CHECK(capture.ImageArrays[0].Frames[0].Format == SurfaceFormat::RGBA32Float);
	CHECK(capture.ImageArrays[0].Frames[0].Pixels.size() == 16);
	resources[0].Resource = "wrong-source-path";
	CHECK_FALSE(host.Capture(
		{sequence, request, sequenceInputs, {}, 1024 * 1024, nullptr, SurfaceFormat::RGBA32Float},
		capture,
		failure
	));
	CHECK(capture.ImageArrays[0].Frames[0].Pixels.size() == 16);
}

TEST_CASE(
	"Studio animated read observes signed fractional frame and exact resource grants", "[studio][file_host]"
) {
	Temporary temp;
	const auto red = temp.Path / "red.bmp", blue = temp.Path / "blue.bmp";
	const auto write = [&](const auto &path, uint8_t r, uint8_t b) {
		std::array<uint8_t, 58> bitmap{};
		bitmap[0] = 'B';
		bitmap[1] = 'M';
		bitmap[2] = 58;
		bitmap[10] = 54;
		bitmap[14] = 40;
		bitmap[18] = 1;
		bitmap[22] = 1;
		bitmap[26] = 1;
		bitmap[28] = 24;
		bitmap[34] = 4;
		bitmap[54] = b;
		bitmap[56] = r;
		std::ofstream file(path, std::ios::binary);
		file.write(reinterpret_cast<const char *>(bitmap.data()), bitmap.size());
		REQUIRE(file.good());
	};
	write(red, 255, 0);
	write(blue, 0, 255);
	Node node;
	node.Id = "animated";
	node.Type = "pc.image_animated";
	ArrayValue paths{ValueType::Text, {red.string(), blue.string()}};
	std::vector<AuthoredValue> inputs{
		{"path", paths},
		{"padding", Vector4{}},
		{"canvas_size", EnumValue{2}},
		{"loop_modes", EnumValue{0}},
		{"stretch_frame", false},
		{"start_frame", int64_t{1}},
		{"animation_speed", 1.},
		{"draw_before_start", true},
		{"custom_frame_order", false},
		{"frame", int64_t{0}},
		{"edit_in_timeline", true},
		{"set_animation_length_to_match", false}
	};
	std::array<engine::imagegraphexport::GraphFileGrant, 2> grants{
		{{node.Id, red, false, red.string()}, {node.Id, blue, false, blue.string()}}
	};
	studio::detail::ImageGraphHost host;
	host.Grants = grants;
	EvaluationRequest request;
	request.Subframe = .25;
	HostNodeCapture capture;
	std::string failure;
	HostNodeInvocation invocation{node, request, inputs, {}, 1024 * 1024};
	REQUIRE(studio::detail::ImageGraphFileReadType(node.Type));
	CHECK_FALSE(studio::detail::ImageGraphFileNeedsPrimary(node.Type));
	const auto check = [&](uint64_t tick, bool negative, SurfacePixel expected) {
		request.Tick = tick;
		request.NegativeFrame = negative;
		auto accepted = host.Capture(invocation, capture, failure);
		INFO(failure);
		REQUIRE(accepted);
		REQUIRE(capture.Images.size() == 1);
		SurfacePixel pixel{};
		REQUIRE(LoadSurfacePixel(capture.Images[0].Data, 0, 0, pixel));
		CHECK(pixel == expected);
		CHECK(capture.Tick == tick);
		CHECK(capture.Subframe == .25);
		CHECK(capture.NegativeFrame == negative);
	};
	check(0, false, {1, 0, 0, 1});
	check(1, false, {0, 0, 1, 1});
	check(0, true, {});
	check(0, false, {1, 0, 0, 1});
	const auto retained = host.RetainedBytes;
	const auto before = capture.Images[0].Data;
	grants[1].Resource = "wrong-blue-path";
	CHECK_FALSE(host.Capture(invocation, capture, failure));
	CHECK(capture.Images[0].Data == before);
	CHECK(host.RetainedBytes == retained);
}

TEST_CASE(
	"Studio directory roots preserve observed order until refresh and require exact grants",
	"[studio][file_host][directory_host]"
) {
	Temporary temp;
	constexpr std::array<uint8_t, 77> PNG_RGB{
		{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
		 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x02, 0x00, 0x00, 0x00, 0xFD, 0xD4, 0x9A,
		 0x73, 0x00, 0x00, 0x00, 0x14, 0x49, 0x44, 0x41, 0x54, 0x78, 0xDA, 0x63, 0xF8, 0xCF, 0xC0, 0xC0,
		 0x00, 0xC2, 0x0C, 0xFF, 0xFF, 0xFF, 0x67, 0x00, 0x00, 0x1E, 0xEF, 0x04, 0xFC, 0x73, 0x1C, 0x53,
		 0xCC, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82}
	};
	const auto first = temp.Path / "first.png", second = temp.Path / "second.png";
	const auto write = [&](const auto &path) {
		std::ofstream stream(path, std::ios::binary);
		stream.write(reinterpret_cast<const char *>(PNG_RGB.data()), PNG_RGB.size());
		REQUIRE(bool(stream));
	};
	write(first);
	Node node;
	node.Id = "directory";
	node.Type = "pc.directory_search";
	std::array<AuthoredValue, 4> inputs{
		{{"path", temp.Path.string()},
		 {"extensions", std::string(".png")},
		 {"type", EnumValue{0}},
		 {"recursive", false}}
	};
	std::array<engine::imagegraphexport::GraphDirectoryGrant, 1> directories{{{node.Id, temp.Path}}};
	std::array<engine::imagegraphexport::GraphFileGrant, 2> files{
		{{node.Id, first, false, first.string()}, {node.Id, second, false, second.string()}}
	};
	studio::detail::ImageGraphHost host;
	host.Grants = files;
	EvaluationRequest request;
	request.Tick = 2;
	request.Subframe = .5;
	request.NegativeFrame = true;
	HostNodeInvocation invocation{node, request, inputs, {}, 16 * 1024 * 1024};
	HostNodeCapture captured;
	std::string failure;
	CHECK_FALSE(host.Capture(invocation, captured, failure));
	host.Directories = directories;
	REQUIRE(host.Capture(invocation, captured, failure));
	REQUIRE(captured.ImageArrays[0].Frames.size() == 1);
	const auto originalPaths = captured.Outputs[0].Data;
	write(second);
	request.Tick = 10;
	request.Subframe = .25;
	request.NegativeFrame = false;
	REQUIRE(host.Capture(invocation, captured, failure));
	CHECK(captured.ImageArrays[0].Frames.size() == 1);
	CHECK(captured.Outputs[0].Data == originalPaths);
	CHECK(captured.Tick == 10);
	CHECK(captured.Subframe == .25);
	CHECK_FALSE(captured.NegativeFrame);
	host.RefreshFile(node.Id);
	REQUIRE(host.Capture(invocation, captured, failure));
	CHECK(captured.ImageArrays[0].Frames.size() == 2);
	const auto prior = captured.ImageArrays[0].Frames;
	host.Directories = {};
	CHECK_FALSE(host.Capture(invocation, captured, failure));
	CHECK(captured.ImageArrays[0].Frames == prior);
	host.Directories = directories;
	directories[0].Root = temp.Path / "other";
	CHECK_FALSE(host.Capture(invocation, captured, failure));
	CHECK(captured.ImageArrays[0].Frames == prior);
	directories[0].Root = temp.Path;
	files[0].Resource = "revoked";
	CHECK_FALSE(host.Capture(invocation, captured, failure));
	CHECK(captured.ImageArrays[0].Frames == prior);
	host.Grants = {};
	inputs[2].Data = EnumValue{1};
	REQUIRE(host.Capture(invocation, captured, failure));
	CHECK(captured.ImageArrays.empty());
	CHECK(std::get<ArrayValue>(captured.Outputs[1].Data).Elements.empty());
	invocation.MaximumOperationBytes = host.RetainedBytes;
	CHECK_FALSE(host.Capture(invocation, captured, failure));
	CHECK(captured.ImageArrays.empty());
}

TEST_CASE(
	"Studio manual file exports use explicit grants without replaying a cached write",
	"[studio][imagegraph][file_export]"
) {
	using namespace engine::imagegraphexport;
	Temporary temp;
	studio::detail::ImageGraphHost host;
	CHECK(studio::detail::ImageGraphFileWriteType("pc.csv_file_write"));
	CHECK(studio::detail::ImageGraphFileWriteType("pc.tile_tilemap_export"));
	CHECK_FALSE(studio::detail::ImageGraphFileReadType("pc.tile_tilemap_export"));
	const auto file = temp.Path / "output.csv";
	std::array<GraphFileGrant, 1> grants{{{"writer", file, true}}};
	host.Grants = grants;
	Node node;
	node.Id = "writer";
	node.Type = "pc.csv_file_write";
	std::array<AuthoredValue, 2> inputs{{{"path", file.string()}, {"content", std::string("first")}}};
	EvaluationRequest request;
	HostNodeInvocation invocation{node, request, inputs, {}, 16 * 1024 * 1024};
	HostNodeCapture capture;
	std::string failure;
	const auto retained = host.RetainedBytes;
	REQUIRE(host.Capture(invocation, capture, failure));
	const auto read = [&] {
		std::ifstream stream(file, std::ios::binary);
		return std::string((std::istreambuf_iterator<char>(stream)), {});
	};
	CHECK(read() == "first");
	CHECK(host.RetainedBytes == retained);
	inputs[1].Data = std::string("second");
	REQUIRE(host.Capture(invocation, capture, failure));
	CHECK(read() == "second");
	CHECK(host.RetainedBytes == retained);
	host.Grants = {};
	CHECK_FALSE(host.Capture(invocation, capture, failure));
	CHECK(read() == "second");
	host.Grants = grants;
	grants[0].Write = false;
	CHECK_FALSE(host.Capture(invocation, capture, failure));
	CHECK(read() == "second");
	grants[0].Write = true;
	invocation.MaximumOperationBytes = host.RetainedBytes;
	CHECK_FALSE(host.Capture(invocation, capture, failure));
	CHECK(read() == "second");
}

TEST_CASE(
	"Studio tile export composite resolves a live graph and revokes its exact write grant",
	"[studio][imagegraph][file_export]"
) {
	using namespace engine::imagegraphexport;
	Temporary temp;
	const auto file = temp.Path / "map.csv";
	TilesetValue set;
	auto &data = set.Data.emplace();
	data.Texture = {1, 1, {255, 0, 0, 255}, 0};
	data.TileSize = {16, 16};
	data.DisplayName = "Native";
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"export", "pc.tile_tilemap_export", "", {}, {{"path", file.string()}}},
		{"map",
		 "pc.solid",
		 "",
		 {},
		 {{"dimension", Vector2{2, 2}},
		  {"dimension_unit", EnumValue{0}},
		  {"color", Colour{255, 0, 0, 255}},
		  {"attribute_color_depth", EnumValue{4}}}}
	};
	document.Junctions = {{"tileset", "", ValueType::Tileset, set}};
	document.Links = {{"map", "surface_out", "export", "tilemap"}, {"tileset", "value", "export", "input_7"}};
	document.Outputs = {{"map", "map", "surface_out"}};
	Plan plan;
	Diagnostic diagnostic;
	const auto status = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	std::array<GraphFileGrant, 1> grants{{{"export", file, true}}};
	studio::detail::ImageGraphHost host;
	host.Grants = grants;
	EvaluationRequest request;
	request.HostProvider = &host;
	Image preview;
	REQUIRE(Evaluate(document, plan, "map", request, preview, diagnostic) == Status::Ok);
	CHECK_FALSE(std::filesystem::exists(file));
	HostNodeCapture capture;
	std::string failure;
	REQUIRE(ExecuteGraphHostNode(document, plan, request, "export", capture, failure));
	const auto read = [&] {
		std::ifstream stream(file, std::ios::binary);
		return std::string((std::istreambuf_iterator<char>(stream)), {});
	};
	CHECK(read() == "1,1\n1,1\n");
	host.Grants = {};
	CHECK_FALSE(ExecuteGraphHostNode(document, plan, request, "export", capture, failure));
	CHECK(read() == "1,1\n1,1\n");
}
