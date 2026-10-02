#include "RigidGraphFixture.hpp"

#include <engine/imagegraph/RigidMeshAction.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/imagegraphphysics/RigidReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraphphysics.rigid_mesh_action")
using namespace engine::imagegraph;
namespace {
	void AttachTextureArray(Document &document, size_t count) {
		document.Links.erase(document.Links.begin());
		Node all{"textures", "value.array", "rigid", {}, {{"spread", true}}};
		size_t groupIndex = 0;
		while (count) {
			const auto groupSize = std::min<size_t>(Limits::MaximumDynamicInputsPerNode, count);
			const auto id = "texture_group_" + std::to_string(groupIndex++);
			Node group{id, "value.array", "rigid", {}, {{"spread", true}}};
			for (size_t index = 0; index < groupSize; ++index) {
				const auto port = "texture_" + std::to_string(index);
				group.DynamicInputs.push_back({port, ValueType::Image, std::nullopt});
				document.Links.push_back({"texture", "image", id, port});
			}
			all.DynamicInputs.push_back({id, ValueType::Array, std::nullopt});
			document.Links.push_back({id, "array", "textures", id});
			document.Nodes.push_back(std::move(group));
			count -= groupSize;
		}
		document.Nodes.push_back(std::move(all));
		document.Links.push_back({"textures", "array", "body", "texture"});
	}
}
TEST_CASE(
	"Generate Mesh captures owned action then persists polygon through authoring roundtrip",
	"[rigid][source][rigid_mesh_action]"
) {
	auto document = engine::imagegraphphysics::testing::RigidGraphFixture();
	document.Nodes[1].Values[0].Data = int64_t{5};
	document.Nodes[1].Values[1].Data = int64_t{5};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	engine::imagegraphphysics::RigidProvider provider;
	EvaluationRequest request;
	request.RigidProvider = &provider;
	EvaluationSnapshot inputs;
	REQUIRE(EvaluateNodeInputs(document, plan, "body", request, inputs, diagnostic) == Status::Ok);
	ArrayValue mesh;
	REQUIRE(PrepareRigidMeshAction(inputs, Limits::MaximumEvaluationBytes, mesh, diagnostic) == Status::Ok);
	REQUIRE(mesh.Items.size() == 1);
	CHECK(mesh.ElementType == ValueType::Any);
	const auto *points = std::get_if<std::vector<SourceArrayItem>>(&mesh.Items[0].Data);
	REQUIRE(points);
	CHECK(points->size() >= 3);
	CHECK(points->size() <= 8);
	const auto prior = mesh;
	CHECK(PrepareRigidMeshAction(inputs, 0, mesh, diagnostic) == Status::LimitExceeded);
	CHECK(mesh == prior);
	CHECK(
		PrepareRigidMeshAction(inputs, Limits::MaximumEvaluationBytes + 1, mesh, diagnostic) ==
		Status::LimitExceeded
	);
	CHECK(mesh == prior);
	CHECK(PrepareRigidMeshAction(inputs, 1, mesh, diagnostic) == Status::LimitExceeded);
	CHECK(mesh == prior);
	document.Nodes[2].Values.push_back({"attribute_mesh", mesh});
	document.Nodes[2].Values.push_back({"shape", EnumValue{2}});
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Document persisted;
	REQUIRE(Read(Write(document), persisted, diagnostic) == Status::Ok);
	CHECK(persisted.Nodes[2].Values == document.Nodes[2].Values);
	REQUIRE(Compile(persisted, plan, diagnostic) == Status::Ok);
	StatefulEvaluationResult result;
	request.RigidPlaying = true;
	request.RigidFrameProgress = true;
	const auto status = EvaluateStateful(persisted, plan, "image", request, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(result.Rigid.Owners.size() == 1);
	SourceRigidSnapshot physical;
	REQUIRE(
		provider.Replay(
			result.Rigid.Owners[0].History,
			0,
			std::nullopt,
			Limits::MaximumEvaluationBytes,
			physical,
			diagnostic
		) == Status::Ok
	);
	REQUIRE(physical.Bodies.size() == 1);
	CHECK(physical.Bodies[0].Id == "body/0/0");
	CHECK(physical.Bodies.back().Mass > 0);
}

TEST_CASE(
	"Compiled selected output keeps Render A then Force then Render B in one owned world",
	"[rigid][source][rigid_mesh_action]"
) {
	auto document = engine::imagegraphphysics::testing::RigidGraphFixture();
	document.Nodes[0].Values[3].Data = 0.;
	document.Nodes[0].Values[4].Data = false;
	document.Nodes[2].Values[2].Data = Vector2{16, 16};
	document.Nodes[3].Id = "render_a";
	document.Links[1].ToNode = "render_a";
	document.Nodes.push_back(
		{"impulse",
		 "pc.rigid_force_apply",
		 "rigid",
		 {},
		 {{"force_type", EnumValue{1}}, {"trigger", true}, {"force", Vector2{2, 0}}, {"strength", 1.}}}
	);
	Node second{"render_b", "pc.rigid_render", "rigid", {}, {{"timestep", 100.}, {"round_position", true}}};
	second.DynamicInputs = {{"object_0", ValueType::Rigid, std::nullopt}};
	document.Nodes.push_back(std::move(second));
	document.Links.push_back({"body", "object", "impulse", "object"});
	document.Links.push_back({"impulse", "object", "render_b", "object_0"});
	document.Outputs = {{"image_b", "render_b", "surface_out"}};
	document.Keyframes = {{"impulse", "trigger", 0, false, "step"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	engine::imagegraphphysics::RigidProvider provider;
	EvaluationRequest request;
	request.RigidProvider = &provider;
	request.RigidPlaying = true;
	request.RigidFrameProgress = true;
	StatefulEvaluationResult result;
	const auto status = EvaluateStateful(document, plan, "image_b", request, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(result.Rigid.Owners.size() == 1);
	const auto &history = result.Rigid.Owners[0].History;
	REQUIRE(history.Frames.size() == 1);
	const auto &events = history.Frames[0].Events;
	auto first = std::find_if(events.rbegin(), events.rend(), [](const auto &event) {
		return event.Position.ConsumerId == "render_a";
	});
	REQUIRE(first != events.rend());
	SourceRigidSnapshot before, after;
	REQUIRE(
		provider.Replay(history, 0, first->Position, Limits::MaximumEvaluationBytes, before, diagnostic) ==
		Status::Ok
	);
	REQUIRE(
		provider.Replay(history, 0, std::nullopt, Limits::MaximumEvaluationBytes, after, diagnostic) ==
		Status::Ok
	);
	REQUIRE(before.Bodies.size() == 1);
	REQUIRE(after.Bodies.size() == 1);
	CHECK(after.Bodies[0].Position.X > before.Bodies[0].Position.X);
	const auto steps = std::count_if(events.begin(), events.end(), [](const auto &event) {
		return std::holds_alternative<SourceRigidStep>(event.Command);
	});
	CHECK(steps == 2);
}

TEST_CASE(
	"Compiled rigid texture and raster bounds preserve previously published output",
	"[rigid][source][rigid_mesh_action]"
) {
	auto document = engine::imagegraphphysics::testing::RigidGraphFixture();
	Plan plan;
	Diagnostic diagnostic;
	engine::imagegraphphysics::RigidProvider provider;
	EvaluationRequest request;
	request.RigidProvider = &provider;
	request.RigidPlaying = true;
	request.RigidFrameProgress = true;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	StatefulEvaluationResult result;
	REQUIRE(EvaluateStateful(document, plan, "image", request, result, diagnostic) == Status::Ok);
	const auto priorPixels = std::get<Image>(result.Output);
	const auto priorHistory = result.Rigid;
	AttachTextureArray(document, 257);
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CHECK(EvaluateStateful(document, plan, "image", request, result, diagnostic) == Status::LimitExceeded);
	CHECK(std::get<Image>(result.Output) == priorPixels);
	CHECK(result.Rigid == priorHistory);
	for (const auto type : {"pc.rigid_render", "pc.rigid_render_id"}) {
		document = engine::imagegraphphysics::testing::RigidGraphFixture();
		document.Nodes[0].Values[0].Data = Vector2{1024, 1024};
		document.Nodes[3].Type = type;
		document.Outputs.resize(1);
		AttachTextureArray(document, 65);
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		CHECK(
			EvaluateStateful(document, plan, "image", request, result, diagnostic) == Status::LimitExceeded
		);
		CHECK(std::get<Image>(result.Output) == priorPixels);
		CHECK(result.Rigid == priorHistory);
		CHECK(diagnostic.Message.find("raster") != std::string::npos);
	}
}
