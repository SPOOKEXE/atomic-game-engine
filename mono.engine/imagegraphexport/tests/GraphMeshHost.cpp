#include <engine/imagegraphexport/GraphExport.hpp>
#include <engine/imagegraphexport/GraphMeshHost.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
TEST_SUITE_ID("engine.imagegraphexport.graph_mesh_host")
TEST_DEPENDS("engine.bake.composer_model")
TEST_DEPENDS("engine.imagegraphexport.graph_file_host")
namespace {
	std::string Text(const std::filesystem::path &path) {
		std::ifstream stream(path, std::ios::binary);
		return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
	}
}
TEST_CASE(
	"Granted native model nodes import typed mesh and atomically publish OBJ with texture",
	"[assetc][imagegraph]"
) {
	using namespace engine::imagegraph;
	const auto directory = std::filesystem::temp_directory_path() / "atomic-graph-mesh-host-test";
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
	const auto input = directory / "in.obj", target = directory / "out.obj";
	{
		std::ofstream stream(input);
		stream << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
	}
	std::array<engine::imagegraphexport::GraphFileGrant, 1> grants{{{"obj", input, false}}};
	Node node;
	node.Id = "obj";
	node.Type = "pc.3_d_mesh_obj";
	std::vector<AuthoredValue> inputs{
		{"file_path", input.string()},
		{"import_scale", 1.0},
		{"axis", EnumValue{0}},
		{"flip_uv", false},
		{"position", Vector3{1, 2, 3}},
		{"anchor", Vector3{}},
		{"scale", Vector3{1, 1, 1}},
		{"rotation", Quaternion{}}
	};
	EvaluationRequest request;
	HostNodeCapture capture;
	std::string failure;
	REQUIRE(
		engine::imagegraphexport::CaptureGraphMeshFile(
			grants, {}, {node, request, inputs, {}, Limits::MaximumEvaluationBytes}, capture, failure
		)
	);
	REQUIRE(capture.Outputs.size() == 1);
	auto mesh = std::get<MeshValue3D>(capture.Outputs[0].Data);
	REQUIRE(mesh.Data);
	REQUIRE(mesh.Data->Parts.size() == 1);
	CHECK(mesh.Data->Parts[0].Vertices.size() == 3);
	Image image;
	image.Width = image.Height = 1;
	image.Pixels = {255, 0, 0, 255};
	image.Hash = SurfaceHash(image);
	mesh.Data->Materials[0].Edit().Surface = image;
	node.Id = "export";
	node.Type = "pc.3_d_mesh_export";
	grants[0] = {"export", target, true};
	inputs = {
		{"paths", target.string()},
		{"mesh", mesh},
		{"export_texture", true},
		{"invert_uv", false},
		{"invert_yz_axis", false},
		{"apply_transform", true}
	};
	REQUIRE(
		engine::imagegraphexport::CaptureGraphMeshFile(
			grants, {}, {node, request, inputs, {}, Limits::MaximumEvaluationBytes}, capture, failure
		)
	);
	const auto obj = Text(target);
	CHECK(obj.find("mtllib out.mtl") != std::string::npos);
	CHECK(obj.find("v 0.75000 1.75000 3.00000") != std::string::npos);
	CHECK(obj.find("f 1/1/1 2/2/2 3/3/3") != std::string::npos);
	CHECK(Text(directory / "out.mtl").find("map_Kd out_texture1.png") != std::string::npos);
	CHECK(Text(directory / "out_texture1.png").starts_with("\x89PNG"));
	inputs[5].Data = std::string{"invalid"};
	CHECK_FALSE(
		engine::imagegraphexport::CaptureGraphMeshFile(
			grants, {}, {node, request, inputs, {}, Limits::MaximumEvaluationBytes}, capture, failure
		)
	);
	CHECK(Text(target) == obj);
}
TEST_CASE("Explicit graph host execution reaches a zero-output writer", "[assetc][imagegraph]") {
	using namespace engine::imagegraph;
	const auto directory = std::filesystem::temp_directory_path() / "atomic-graph-zero-output-test";
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
	const auto output = directory / "result.txt";
	Document document;
	document.Nodes.push_back(
		{"writer",
		 "pc.text_file_write",
		 "",
		 {},
		 {{"path", output.string()}, {"content", std::string{"native zero output"}}}}
	);
	document.Nodes.push_back(
		{"preview",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{0, 0, 0, 255}}}}
	);
	document.Outputs.push_back({"preview", "preview", "image"});
	engine::imagegraphexport::GraphExportSettings settings;
	settings.Input = directory / "writer.graph";
	{
		std::ofstream stream(settings.Input);
		stream << Write(document);
	}
	std::array<engine::imagegraphexport::GraphFileGrant, 1> grants{{{"writer", output, true}}};
	engine::imagegraphexport::GraphFileHost host(grants, {});
	settings.HostProvider = &host;
	HostNodeCapture capture;
	std::string failure;
	const bool executed =
		engine::imagegraphexport::ExecuteGraphHostNode(settings, "writer", capture, failure);
	INFO(failure);
	REQUIRE(executed);
	CHECK(Text(output) == "native zero output");
	CHECK(capture.Outputs.empty());
}
