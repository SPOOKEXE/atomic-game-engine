#include <engine/render/Renderer.hpp>
#include <engine/render/SourceSkyboxGroup.hpp>
#include <engine/render/SourceTransformImage3D.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.render.sourcetransformimage3d")

namespace {
	namespace source = engine::imagegraph;
	struct Inputs {
		source::Node Node{"transform", "pc.3_d_transform_image", "", {}, {}};
		source::EvaluationRequest Clock{.Tick = 17};
		source::Image Front{3, 2, std::vector<uint8_t>(24, 64), 0};
		std::vector<source::AuthoredValue> Values{
			{"position", source::Vector3{1, 2, 3}},
			{"anchor", source::Vector3{.1, .2, .3}},
			{"rotation", source::Quaternion{}},
			{"scale", source::Vector3{2, 3, 4}},
			{"texture_tiling", source::Vector2{2, 1}},
			{"projection", source::EnumValue{1}},
			{"fov", 45.},
			{"view_range", source::Vector2{.001, 10}},
			{"depth_range", source::Vector2{0, 1}},
			{"interpolate", source::EnumValue{0}}
		};
		std::array<source::HostResolvedImage, 1> Images{{{"surface", &Front}}};
		source::HostNodeInvocation Invocation(uint64_t maximum = 64ull * 1024 * 1024) {
			return {Node, Clock, Values, Images, maximum, nullptr, source::SurfaceFormat::RGBA8Unorm, 2};
		}
	};
}
TEST_CASE(
	"source Transform borrowed inputs preserve exact plane winding materials and aspect",
	"[render][source-transform-image3d]"
) {
	using namespace engine::render::imagegraph;
	Inputs inputs;
	TransformImage3DRequest request;
	source::MeshValue3D mesh;
	std::string error;
	REQUIRE(BuildSourceTransformImage3DRequest(inputs.Invocation(), request, mesh, error));
	REQUIRE(mesh.Data);
	REQUIRE(mesh.Data->Parts.size() == 2);
	REQUIRE(mesh.Data->Materials.size() == 2);
	CHECK(request.SourcePlane);
	CHECK(request.LinearFilter);
	CHECK(request.Scale[1] == 3);
	CHECK(request.Back.Pixels == request.Front.Pixels);
	const auto &front = mesh.Data->Parts[0].Vertices;
	const auto &back = mesh.Data->Parts[1].Vertices;
	REQUIRE(front.size() == 6);
	REQUIRE(back.size() == 6);
	CHECK(front[0].Position == source::Vector3{-.5, -.5, 0});
	CHECK(front[0].UV == source::Vector2{0, 0});
	for (size_t i = 0; i < 6; ++i) {
		CHECK(back[i].Position == front[5 - i].Position);
		CHECK(back[i].UV == front[5 - i].UV);
		CHECK(back[i].Normal.Z == -1);
	}
	CHECK(mesh.Data->Edges.size() == 4);
	inputs.Front.Pixels[0] = 255;
	CHECK(mesh.Data->Materials[0].Get().Surface->Pixels[0] == 64);
	CHECK(request.Front.Pixels[0] == std::byte(64));
	inputs.Values[5].Data = source::EnumValue{0};
	REQUIRE(BuildSourceTransformImage3DRequest(inputs.Invocation(), request, mesh, error));
	CHECK(request.Scale[1] == 4.5);
	CHECK(mesh.Data->LocalTransforms[0].Scale.Y == 4.5);
}
TEST_CASE(
	"source Transform replacement refuses budget and invalid controls atomically",
	"[render][source-transform-image3d]"
) {
	using namespace engine::render::imagegraph;
	Inputs inputs;
	TransformImage3DRequest request;
	source::MeshValue3D mesh;
	std::string error;
	REQUIRE(BuildSourceTransformImage3DRequest(inputs.Invocation(), request, mesh, error));
	const auto old = request.Front.Pixels;
	const auto *meshIdentity = &*mesh.Data;
	CHECK_FALSE(BuildSourceTransformImage3DRequest(inputs.Invocation(1), request, mesh, error));
	CHECK(request.Front.Pixels == old);
	CHECK(&*mesh.Data == meshIdentity);
	inputs.Values[5].Data = source::EnumValue{0};
	inputs.Values[6].Data = -10.;
	CHECK_FALSE(BuildSourceTransformImage3DRequest(inputs.Invocation(), request, mesh, error));
	CHECK(request.Front.Pixels == old);
	CHECK(&*mesh.Data == meshIdentity);
}
TEST_CASE(
	"source Transform uses real queue cancellation and explicit no-device capture refusal",
	"[render][source-transform-image3d]"
) {
	using namespace engine::render;
	Inputs inputs;
	imagegraph::TransformImage3DRequest request;
	source::MeshValue3D mesh;
	std::string error;
	REQUIRE(imagegraph::BuildSourceTransformImage3DRequest(inputs.Invocation(), request, mesh, error));
	Renderer renderer;
	engine::core::Name owner("transform.owner"), name("transform.capture");
	using Status = imagegraph::TransformImage3DQueueResult;
	CHECK(
		renderer.QueueTransformImage3D(
			{owner, name, 1, imagegraph::TransformImage3DOutput::Rendered, std::move(request)}
		) == Status::Queued
	);
	CHECK(renderer.SourceOutputStatus(owner, name, 1) == SourceTextureStatus::Pending);
	CHECK_FALSE(renderer.CancelTransformImage3D(owner, name, 2));
	CHECK(renderer.CancelTransformImage3D(owner, name, 1));
	CHECK(renderer.SourceOutputStatus(owner, name, 1) == SourceTextureStatus::Absent);
	source::HostNodeCapture capture;
	capture.Failure = "retained";
	bool pending = true;
	CHECK_FALSE(
		renderer.CaptureTransformImage3DAsync(inputs.Invocation(), owner, name, capture, error, &pending)
	);
	CHECK_FALSE(pending);
	CHECK(capture.Failure == "retained");
	CHECK(error.find("renderer device") != std::string::npos);
}
TEST_CASE(
	"source Transform owned typed plane travels through real graph receipt into mesh consumer",
	"[render][source-transform-image3d]"
) {
	using namespace engine::imagegraph;
	Inputs inputs;
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"image",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t(3)}, {"height", int64_t(2)}, {"colour", Colour{64, 64, 64, 64}}}},
		inputs.Node,
		{"wrapper", "pc.3_d_transform", "", {}, {{"position", Vector3{9, 0, 0}}}}
	};
	document.Nodes[1].Values = inputs.Values;
	document.Links = {{"image", "image", "transform", "surface"}, {"transform", "mesh", "wrapper", "mesh"}};
	document.Outputs = {{"mesh", "wrapper", "mesh"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationSnapshot snapshot;
	REQUIRE(EvaluateNodeInputs(document, plan, "transform", {}, snapshot, diagnostic) == Status::Ok);
	std::vector<AuthoredValue> values;
	for (const auto &v : snapshot.Values())
		values.push_back({v.Port, v.Data});
	std::vector<HostResolvedImage> images;
	for (const auto &v : snapshot.Images())
		images.push_back({v.Port, &v.Data});
	EvaluationRequest clock;
	HostNodeInvocation invocation{document.Nodes[1], clock, values, images, 64ull * 1024 * 1024};
	engine::render::imagegraph::TransformImage3DRequest request;
	MeshValue3D plane;
	std::string failure;
	REQUIRE(
		engine::render::imagegraph::BuildSourceTransformImage3DRequest(invocation, request, plane, failure)
	);
	HostNodeCapture receipt;
	uint64_t bytes = 0;
	REQUIRE(
		PrepareResolvedHostCapture(invocation, 64ull * 1024 * 1024, receipt, bytes, diagnostic) == Status::Ok
	);
	receipt.Outputs.push_back({"mesh", std::move(plane)});
	// Explicit recorded image observations satisfy the three-output receipt;
	// this fixture checks mesh routing and makes no claim about raster pixels.
	Image recorded{3, 2, std::vector<uint8_t>(24, 0), 0};
	receipt.Images = {{"rendered", recorded}, {"depth", recorded}};
	clock.HostCaptures = std::span<const HostNodeCapture>(&receipt, 1);
	EvaluatedValue result;
	const auto status = EvaluateValue(document, plan, "mesh", clock, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	const auto *mesh = std::get_if<MeshValue3D>(&result.Data);
	REQUIRE(mesh);
	REQUIRE(mesh->Data);
	CHECK(mesh->Data->Parts.size() == 2);
	REQUIRE(mesh->Data->LocalTransforms.size() == 2);
	CHECK(mesh->Data->LocalTransforms.front().Position == Vector3{9, 0, 0});
	CHECK(mesh->Data->Materials[0].Get().Surface->Pixels[0] == 64);
}
TEST_CASE(
	"native Transform host profile retains native plane and numeric image format separately",
	"[render][source-transform-image3d]"
) {
	Inputs inputs;
	inputs.Node.Type = "image.transform_3d";
	inputs.Front.Format = source::SurfaceFormat::RGBA16Float;
	inputs.Front.Pixels.assign(3 * 2 * 8, 0);
	engine::render::imagegraph::TransformImage3DRequest request;
	source::MeshValue3D mesh;
	std::string error;
	REQUIRE(
		engine::render::imagegraph::BuildSourceTransformImage3DRequest(
			inputs.Invocation(), request, mesh, error
		)
	);
	CHECK_FALSE(request.SourcePlane);
	CHECK(request.Front.Format == engine::assets::TextureFormat::RGBA16_FLOAT);
	REQUIRE(mesh.Data);
	REQUIRE(mesh.Data->Parts.size() == 1);
	CHECK(mesh.Data->Parts[0].Vertices[0].Position == source::Vector3{-1, -1, 0});
	CHECK(mesh.Data->Parts[0].Vertices[0].UV == source::Vector2{0, 1});
}
TEST_CASE(
	"source Transform accepts independent back texture dimensions without widening native profile",
	"[render][source-transform-image3d]"
) {
	Inputs inputs;
	source::Image back{1, 1, {12, 34, 56, 255}, 0};
	std::array<source::HostResolvedImage, 2> images{{{"surface", &inputs.Front}, {"back_surface", &back}}};
	auto invocation = inputs.Invocation();
	invocation.Images = images;
	engine::render::imagegraph::TransformImage3DRequest request;
	source::MeshValue3D mesh;
	std::string error;
	REQUIRE(engine::render::imagegraph::BuildSourceTransformImage3DRequest(invocation, request, mesh, error));
	CHECK(request.Front.Width == 3);
	CHECK(request.Back.Width == 1);
	CHECK(mesh.Data->Materials[1].Get().Surface->Height == 1);
	CHECK(
		engine::render::imagegraph::ValidateTransformImage3D(request) ==
		engine::render::imagegraph::TransformImage3DStatus::Ok
	);
	inputs.Node.Type = "image.transform_3d";
	CHECK_FALSE(
		engine::render::imagegraph::BuildSourceTransformImage3DRequest(invocation, request, mesh, error)
	);
	CHECK(request.SourcePlane);
	CHECK(request.Back.Width == 1);
}
