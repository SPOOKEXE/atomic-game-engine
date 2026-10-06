#include <engine/core/FrameGraph.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/imagegraphexport/GraphExport.hpp>
#include <engine/imagegraphexport/GraphMeshHost.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
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
	CHECK(obj.find("f 1/1/1 2/1/1 3/1/1") != std::string::npos);
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

namespace {
	struct MeshIndexFixture {
		std::filesystem::path Directory, Target;
		engine::imagegraph::Node Node{"export", "pc.3_d_mesh_export", "", {}, {}};
		engine::imagegraph::EvaluationRequest Request;
		engine::imagegraph::HostNodeCapture Capture;
		std::vector<engine::imagegraph::AuthoredValue> Inputs;
		std::string Failure;
		MeshIndexFixture() {
			using namespace engine::imagegraph;
			Directory = std::filesystem::temp_directory_path() /
						("atomic-mesh-indices-" +
						 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			REQUIRE(std::filesystem::create_directory(Directory));
			Target = Directory / "out.obj";
			MeshValue3D mesh;
			auto &data = mesh.Data.emplace();
			data.LocalTransforms.emplace_back();
			data.Materials.emplace_back();
			MeshPart3D triangle;
			triangle.Vertices = {
				{{0, 0, 0}, {0, 0, 1}, {0, 0}}, {{1, 0, 0}, {0, 0, 1}, {1, 0}}, {{0, 1, 0}, {0, 0, 1}, {0, 1}}
			};
			data.Parts = {triangle, triangle};
			Inputs = {
				{"paths", Target.string()},
				{"mesh", std::move(mesh)},
				{"export_texture", false},
				{"invert_uv", false},
				{"invert_yz_axis", false},
				{"apply_transform", true}
			};
		}
		~MeshIndexFixture() {
			std::error_code error;
			std::filesystem::remove_all(Directory, error);
		}
		engine::imagegraph::MeshData3D &Mesh() {
			return *std::get<engine::imagegraph::MeshValue3D>(Inputs[1].Data).Data;
		}
		bool Export(uint64_t maximum = engine::imagegraph::Limits::MaximumEvaluationBytes) {
			std::array grants{engine::imagegraphexport::GraphFileGrant{"export", Target, true}};
			return engine::imagegraphexport::CaptureGraphMeshFile(
				grants, {}, {Node, Request, Inputs, {}, maximum}, Capture, Failure
			);
		}
		std::vector<std::string> Lines(std::string_view prefix) const {
			std::vector<std::string> result;
			std::istringstream text(Text(Target));
			for (std::string line; std::getline(text, line);)
				if (line.starts_with(prefix)) result.push_back(std::move(line));
			return result;
		}
	};
}

TEST_CASE(
	"OBJ export shares independent source indices across mesh buffers", "[assetc][imagegraph][mesh_indices]"
) {
	MeshIndexFixture fixture;
	using engine::core::FrameGraph;
	const bool enabled = FrameGraph::IsEnabled();
	struct Restore {
		bool Enabled;
		~Restore() {
			FrameGraph::SetEnabled(Enabled);
		}
	} restore{enabled};
	engine::core::Metrics::Drain();
	FrameGraph::SetEnabled(true);
	FrameGraph::BeginFrame();
	const bool exported = fixture.Export();
	FrameGraph::EndFrame();
	INFO(fixture.Failure);
	REQUIRE(exported);
	const auto spans = FrameGraph::Spans();
	CHECK(std::any_of(spans.begin(), spans.end(), [](const auto &span) {
		return span.Name == "imagegraphexport.mesh.obj";
	}));
	CHECK(FrameGraph::Dropped() == 0);
	const auto counters = engine::core::Metrics::Drain();
	const auto count = [&](std::string_view name) {
		for (const auto &counter : counters)
			if (counter.Name.Text() == name) return counter.Value;
		return -1.0;
	};
	CHECK(count("imagegraphexport.mesh.obj_vertices") == 6);
	CHECK(count("imagegraphexport.mesh.obj_positions") == 3);
	CHECK(count("imagegraphexport.mesh.obj_normals") == 1);
	CHECK(count("imagegraphexport.mesh.obj_uvs") == 3);
	CHECK(count("imagegraphexport.mesh.obj_payload_bytes") == Text(fixture.Target).size());
	CHECK(
		fixture.Lines("v ") ==
		std::vector<std::string>{
			"v 0.00000 0.00000 0.00000", "v 1.00000 0.00000 0.00000", "v 0.00000 1.00000 0.00000"
		}
	);
	CHECK(fixture.Lines("vn ") == std::vector<std::string>{"vn 0.00000 0.00000 1.00000"});
	CHECK(fixture.Lines("vt ").size() == 3);
	CHECK(fixture.Lines("f ") == std::vector<std::string>{"f 1/1/1 2/2/1 3/3/1", "f 1/1/1 2/2/1 3/3/1"});
}

TEST_CASE(
	"OBJ source formatted keys merge precision collisions and retain UV and normal seams",
	"[assetc][imagegraph][mesh_indices]"
) {
	MeshIndexFixture fixture;
	auto &vertices = fixture.Mesh().Parts[1].Vertices;
	vertices[0].Position.X = .000001;
	vertices[1].UV.X = 1.000001;
	vertices[1].Normal.X = .000001;
	vertices[2].UV = {.5, .5};
	vertices[2].Normal = {1, 0, 0};
	REQUIRE(fixture.Export());
	CHECK(fixture.Lines("v ").size() == 3);
	CHECK(fixture.Lines("vt ").size() == 4);
	CHECK(fixture.Lines("vn ").size() == 2);
	CHECK(fixture.Lines("f ") == std::vector<std::string>{"f 1/1/1 2/2/1 3/3/1", "f 1/1/1 2/2/1 3/4/2"});
}

TEST_CASE(
	"OBJ indexing follows export controls and refusal preserves output and receipt",
	"[assetc][imagegraph][mesh_indices]"
) {
	MeshIndexFixture fixture;
	fixture.Mesh().LocalTransforms[0].Position = {2, 3, 4};
	fixture.Inputs[3].Data = true;
	fixture.Inputs[4].Data = true;
	REQUIRE(fixture.Export());
	CHECK(fixture.Lines("v ").front() == "v 2.00000 4.00000 3.00000");
	CHECK(fixture.Lines("vn ") == std::vector<std::string>{"vn 0.00000 1.00000 0.00000"});
	CHECK(fixture.Lines("vt ").front() == "vt 0.00000 1.00000");
	const auto text = Text(fixture.Target);
	const auto capture = fixture.Capture;
	CHECK_FALSE(fixture.Export(1));
	CHECK(Text(fixture.Target) == text);
	CHECK(fixture.Capture.Authored == capture.Authored);
	CHECK(fixture.Capture.Inputs == capture.Inputs);
	fixture.Capture.Authored.Id.clear();
	fixture.Capture.Inputs.push_back({"prior", std::string(8192, 'x')});
	const auto anonymous = fixture.Capture;
	CHECK_FALSE(fixture.Export(8192));
	CHECK(Text(fixture.Target) == text);
	CHECK(fixture.Capture.Authored == anonymous.Authored);
	CHECK(fixture.Capture.Inputs == anonymous.Inputs);
	fixture.Capture = capture;
	fixture.Inputs[5].Data = false;
	REQUIRE(fixture.Export());
	CHECK(fixture.Lines("v ").front() == "v 0.00000 0.00000 0.00000");
	CHECK(fixture.Lines("f ").front() == "f 1/1/1 2/2/1 3/3/1");
}
