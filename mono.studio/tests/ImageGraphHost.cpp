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
