#include "SourceMeshAvailability.hpp"

#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.source_mesh_availability")
using namespace engine::imagegraph;
namespace {
	Document BufferGraph() {
		Document d;
		d.FormatVersion = 9;
		d.Nodes = {
			{"cube", "pc.3_d_mesh_cube", "", {}, {}},
			{"uv", "pc.3_d_uv_remap", "", {}, {}},
			{"round", "pc.3_d_round_vertex", "", {}, {}},
			{"transform", "pc.3_d_transform", "", {}, {}},
			{"points", "pc.3_d_mesh_vertex_points", "", {}, {{"remove_overlap", false}}}
		};
		d.Links = {
			{"cube", "mesh", "uv", "mesh"},
			{"uv", "mesh", "round", "mesh"},
			{"uv", "mesh", "transform", "mesh"},
			{"uv", "mesh", "points", "mesh"}
		};
		d.Outputs = {
			{"buffer", "uv", "mesh"},
			{"rebuilt", "round", "mesh"},
			{"wrapper", "transform", "mesh"},
			{"positions", "points", "positions"}
		};
		return d;
	}
	Value EvaluateBufferValue(const Document &d, const Plan &plan, const std::string &output) {
		EvaluatedValue result;
		Diagnostic error;
		const auto status = EvaluateValue(d, plan, output, {}, result, error);
		INFO(error.NodeId << ":" << error.Port << ": " << error.Message);
		REQUIRE(status == Status::Ok);
		return result.Data;
	}
}
TEST_CASE(
	"Buffer only UV clone survives getters transforms and authored graph persistence",
	"[source_mesh_availability]"
) {
	auto document = BufferGraph();
	Plan plan;
	Diagnostic error;
	const auto compiled = Compile(document, plan, error);
	INFO(error.NodeId << ":" << error.Port << ": " << error.Message);
	REQUIRE(compiled == Status::Ok);
	const auto buffer = EvaluateBufferValue(document, plan, "buffer");
	const auto &mesh = std::get<MeshValue3D>(buffer);
	REQUIRE(mesh.Data);
	CHECK_FALSE(mesh.Data->CpuVerticesPresent);
	CHECK_FALSE(mesh.Data->CpuEdgesPresent);
	REQUIRE_FALSE(mesh.Data->Parts.empty());
	size_t count = 0;
	for (const auto &part : mesh.Data->Parts) {
		count += part.Vertices.size();
		CHECK_FALSE(part.LocalMatrix);
	}
	REQUIRE(count == 36);
	const auto positions = EvaluateBufferValue(document, plan, "positions");
	// Source de-duplicates all six coordinates even when position-only overlap removal is off.
	CHECK(std::get<ArrayValue>(positions).Nested.size() == 24);
	const auto wrapped = EvaluateBufferValue(document, plan, "wrapper");
	const auto &wrapper = std::get<MeshValue3D>(wrapped);
	REQUIRE(wrapper.Data);
	CHECK_FALSE(wrapper.Data->CpuVerticesPresent);
	CHECK_FALSE(wrapper.Data->CpuEdgesPresent);
	CHECK(wrapper.Data->Parts == mesh.Data->Parts);
	const auto rebuilt = EvaluateBufferValue(document, plan, "rebuilt");
	const auto &empty = std::get<MeshValue3D>(rebuilt);
	REQUIRE(empty.Data);
	CHECK(empty.Data->Parts.empty());
	CHECK(empty.Data->Edges == mesh.Data->Edges);
	Document restored;
	REQUIRE(Read(Write(document), restored, error) == Status::Ok);
	Plan restoredPlan;
	REQUIRE(Compile(restored, restoredPlan, error) == Status::Ok);
	CHECK(EvaluateBufferValue(restored, restoredPlan, "buffer") == buffer);
	CHECK(EvaluateBufferValue(restored, restoredPlan, "rebuilt") == rebuilt);
	CHECK(EvaluateBufferValue(restored, restoredPlan, "positions") == positions);
}
TEST_CASE(
	"Instancer retains draw buffers while source object modifiers and vertex getters reject its class",
	"[source_mesh_availability]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"cube", "pc.3_d_mesh_cube", "", {}, {}},
		{"instances", "pc.3_d_instancer", "", {}, {{"seed", 0.0}, {"amounts", int64_t{2}}}},
		{"round", "pc.3_d_round_vertex", "", {}, {}},
		{"subdivide", "pc.3_d_subdivide", "", {}, {}},
		{"displace", "pc.3_d_displace", "", {}, {}},
		{"points", "pc.3_d_mesh_vertex_points", "", {}, {}}
	};
	document.Links = {
		{"cube", "mesh", "instances", "mesh"},
		{"instances", "mesh", "round", "mesh"},
		{"instances", "mesh", "subdivide", "mesh"},
		{"instances", "mesh", "displace", "mesh"},
		{"instances", "mesh", "points", "mesh"}
	};
	document.Outputs = {
		{"draw", "instances", "mesh"},
		{"round", "round", "mesh"},
		{"subdivide", "subdivide", "mesh"},
		{"displace", "displace", "mesh"},
		{"positions", "points", "positions"},
		{"normals", "points", "normals"}
	};
	Plan plan;
	Diagnostic error;
	const auto compiled = Compile(document, plan, error);
	INFO(error.NodeId << ":" << error.Port << ": " << error.Message);
	REQUIRE(compiled == Status::Ok);
	const auto draw = EvaluateBufferValue(document, plan, "draw");
	const auto &mesh = std::get<MeshValue3D>(draw);
	REQUIRE(mesh.Data);
	CHECK(mesh.Data->Instanced);
	CHECK_FALSE(mesh.Data->CpuVerticesPresent);
	CHECK_FALSE(mesh.Data->CpuEdgesPresent);
	REQUIRE_FALSE(mesh.Data->Parts.empty());
	REQUIRE(mesh.Data->Instances.size() == 2);
	for (const std::string output : {"round", "subdivide", "displace"}) {
		const auto result = EvaluateBufferValue(document, plan, output);
		CHECK_FALSE(std::get<MeshValue3D>(result).Data);
	}
	for (const std::string output : {"positions", "normals"}) {
		const auto result = EvaluateBufferValue(document, plan, output);
		const auto &array = std::get<ArrayValue>(result);
		CHECK(array.Elements.empty());
		CHECK(array.Nested.empty());
		CHECK(array.Items.empty());
	}
	Document restored;
	REQUIRE(Read(Write(document), restored, error) == Status::Ok);
	Plan restoredPlan;
	REQUIRE(Compile(restored, restoredPlan, error) == Status::Ok);
	CHECK(EvaluateBufferValue(restored, restoredPlan, "draw") == draw);
}
