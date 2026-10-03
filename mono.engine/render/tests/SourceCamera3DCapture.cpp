#include "SourceCamera3DCapture.hpp"

#include "ComposerCaptureIdentity.hpp"

#include <engine/imagegraph/SourceCamera3D.hpp>
#include <engine/render/SourceCamera3D.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.render.source_camera_capture")
TEST_DEPENDS("engine.imagegraph.source_camera_host")
TEST_CASE(
	"camera completion converts all seven packed surfaces atomically", "[render][source-camera-capture]"
) {
	using namespace engine::render::imagegraph;
	using namespace engine::imagegraph;
	SourceCamera3DRequest request;
	request.Width = 2;
	request.Height = 1;
	request.Format = engine::assets::TextureFormat::RGBA4_UNORM;
	std::array<std::byte, 8> bytes{
		std::byte{255},
		std::byte{0},
		std::byte{128},
		std::byte{255},
		std::byte{0},
		std::byte{255},
		std::byte{0},
		std::byte{255}
	};
	std::array<std::span<const std::byte>, 7> downloads;
	downloads.fill(bytes);
	HostNodeCapture receipt;
	receipt.Authored.Id = "camera";
	REQUIRE(CompleteSourceCamera3DCapture(request, downloads, 1 << 20, receipt));
	REQUIRE(receipt.Images.size() == 7);
	CHECK(receipt.Images[0].Data.Pixels == std::vector<uint8_t>{0x0f, 0xf8, 0xf0, 0xf0});
	CHECK(receipt.Images[6].Port == "ambient_occlusion");
	const auto old = receipt.Images;
	downloads[6] = std::span(bytes).first(4);
	CHECK_FALSE(CompleteSourceCamera3DCapture(request, downloads, 1 << 20, receipt));
	CHECK(receipt.Images.size() == old.size());
	CHECK(receipt.Images[0].Data == old[0].Data);
	downloads[6] = bytes;
	CHECK_FALSE(CompleteSourceCamera3DCapture(request, downloads, 1, receipt));
	CHECK(receipt.Images[0].Data == old[0].Data);
	request.Format = engine::assets::TextureFormat::R8;
	REQUIRE(CompleteSourceCamera3DCapture(request, downloads, 1 << 20, receipt));
	CHECK(receipt.Images[0].Data.Pixels == std::vector<uint8_t>{255, 0});
}
TEST_CASE(
	"camera request owns camera set source scene and exact receipt policy", "[render][source-camera-capture]"
) {
	using namespace engine::render::imagegraph;
	using namespace engine::imagegraph;
	Node node{"camera", "pc.3_d_camera_set", "", {}, {}};
	SceneValue3D scene;
	scene.Data.emplace();
	std::array<AuthoredValue, 3> controls{
		{{"scene", std::move(scene)}, {"dimension", Vector2{.5, .5}}, {"dimension_unit", EnumValue{1}}}
	};
	EvaluationRequest clock;
	HostNodeInvocation invocation{
		node,
		clock,
		controls,
		{},
		1 << 20,
		nullptr,
		SurfaceFormat::RGBA8Unorm,
		1,
		SourceCameraEvaluationPolicy{64, 32, 1, 1, false},
		0
	};
	SourceCamera3DRequest request;
	std::string failure;
	REQUIRE(BuildSourceCamera3DRequest(invocation, "rendered", false, request, failure));
	CHECK(request.Width == 32);
	CHECK(request.Height == 16);
	CHECK(request.Shader == 1);
	REQUIRE(request.Scene.Data);
	REQUIRE(request.Scene.Data->Objects.size() == 3);
	const auto &key = std::get<LightValue3D>(request.Scene.Data->Objects[1].Data);
	REQUIRE(key.Data);
	CHECK(key.Data->Kind == LightKind3D::Directional);
	CHECK(key.Data->Intensity == 1);
	CHECK_FALSE(key.Data->CastShadow);
	const auto previous = request;
	invocation.MaximumOperationBytes = 1;
	CHECK_FALSE(BuildSourceCamera3DRequest(invocation, "rendered", false, request, failure));
	CHECK(request == previous);
	invocation.MaximumOperationBytes = 4096;
	Image environment{1, 1, {255, 255, 255, 255}};
	environment.Pixels.reserve(1 << 20);
	const std::array<HostResolvedImage, 1> images{{{"environment_texture", &environment}}};
	invocation.Images = images;
	CHECK_FALSE(BuildSourceCamera3DRequest(invocation, "rendered", false, request, failure));
	CHECK(request == previous);
	invocation.Images = {};
	invocation.MaximumOperationBytes = 1 << 20;
	const std::vector<HostResolvedImage> excessiveImages(Limits::MaximumLinks + 1);
	invocation.Images = excessiveImages;
	CHECK_FALSE(BuildSourceCamera3DRequest(invocation, "rendered", false, request, failure));
	CHECK(failure == "camera resolved bindings exceed the bounded input limit");
	CHECK(request == previous);
	invocation.Images = {};
	const std::vector<AuthoredValue> excessiveInputs(Limits::MaximumLinks + 1);
	invocation.Inputs = excessiveInputs;
	CHECK_FALSE(BuildSourceCamera3DRequest(invocation, "rendered", false, request, failure));
	CHECK(failure == "camera resolved bindings exceed the bounded input limit");
	CHECK(request == previous);
	invocation.Inputs = controls;
	HostNodeCapture a, b;
	Diagnostic diagnostic;
	uint64_t retained = 0;
	REQUIRE(PrepareResolvedHostCapture(invocation, 1 << 20, a, retained, diagnostic) == Status::Ok);
	b = a;
	CHECK(engine::render::detail::SameComposerCaptureInputs(a, b, 1, 1));
	b.CameraPolicy->InheritedSurfaceFormat = SurfaceFormat::RGBA16Float;
	CHECK_FALSE(engine::render::detail::SameComposerCaptureInputs(a, b, 1, 1));
	b = a;
	b.CameraPolicy->ProjectShader3D = 0;
	CHECK_FALSE(engine::render::detail::SameComposerCaptureInputs(a, b, 1, 1));
}

TEST_CASE(
	"camera preparation admits exact flattened backing before vertices or instanced parts allocate",
	"[render][source-camera-capture]"
) {
	using namespace engine;
	using namespace render::imagegraph;
	SourceCamera3DRequest request;
	request.Width = request.Height = 1;
	request.View = request.Projection = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
	request.Scene.Data.emplace();
	const auto minimum = [&](const SourceCamera3DRequest &value) {
		uint64_t low = 0, high = 1 << 20;
		REQUIRE(ValidateSourceCamera3D(value, high) == SourceCamera3DStatus::Ok);
		while (low < high) {
			const auto middle = low + (high - low) / 2;
			if (ValidateSourceCamera3D(value, middle) == SourceCamera3DStatus::Ok)
				high = middle;
			else
				low = middle + 1;
		}
		REQUIRE(low > 0);
		CHECK(ValidateSourceCamera3D(value, low) == SourceCamera3DStatus::Ok);
		CHECK(ValidateSourceCamera3D(value, low - 1) == SourceCamera3DStatus::OutputLimit);
		return low;
	};
	const auto emptyBytes = minimum(request);
	imagegraph::MeshValue3D mesh;
	auto &data = mesh.Data.emplace();
	data.LocalTransforms.emplace_back();
	data.Materials.emplace_back();
	imagegraph::MeshPart3D triangle;
	triangle.Vertices = {
		{{0, 0, .5}, {0, 0, 1}, {0, 0}, {255, 255, 255, 255}},
		{{1, 0, .5}, {0, 0, 1}, {1, 0}, {255, 255, 255, 255}},
		{{0, 1, .5}, {0, 0, 1}, {0, 1}, {255, 255, 255, 255}}
	};
	data.Parts.push_back(std::move(triangle));
	request.Scene.Data->Objects.emplace_back(imagegraph::SceneObject3D{std::move(mesh)});
	const auto vertexBytes = minimum(request);
	// Colour and shadow each flatten three vertices of fifteen float components.
	CHECK(vertexBytes >= emptyBytes + 6 * 15 * sizeof(float));
	auto &instanced = *std::get<imagegraph::MeshValue3D>(request.Scene.Data->Objects[0].Data).Data;
	instanced.Parts.clear();
	instanced.Parts.resize(32);
	instanced.Instanced = true;
	instanced.Instances.resize(64);
	for (auto &instance : instanced.Instances)
		instance.Fields[8] = instance.Fields[9] = instance.Fields[10] = 1;
	const auto partBytes = minimum(request);
	CHECK(partBytes > vertexBytes);
	CHECK(ValidateSourceCamera3D(request, vertexBytes) == SourceCamera3DStatus::OutputLimit);
	// Source accounting also takes this allocation-free traversal, so it can reject expansion directly.
	instanced.Instances.resize(4096);
	instanced.Parts.resize(1024);
	CHECK(ValidateSourceCamera3D(request, 1 << 20) == SourceCamera3DStatus::OutputLimit);
}
