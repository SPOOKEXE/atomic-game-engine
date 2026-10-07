#include "../src/ImageGraphTransform3DResident.hpp"
#include "GpuHeap.hpp"
#include "RenderFixture.hpp"

#include <engine/render/SourceCamera3D.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <cstring>

TEST_SUITE_ID("engine.render.sourcecamera3d_gpu")
TEST_DEPENDS("engine.render.fixtures")
TEST_DEPENDS("engine.render.sourcecamera3d")
namespace {
	engine::render::imagegraph::SourceCamera3DRequest Triangle() {
		using namespace engine;
		render::imagegraph::SourceCamera3DRequest request;
		request.Width = request.Height = 4;
		request.CullMode = 0;
		request.AmbientLight = {1, 1, 1, 1};
		const glm::mat4 identity(1);
		std::copy_n(glm::value_ptr(identity), 16, request.View.begin());
		std::copy_n(glm::value_ptr(identity), 16, request.Projection.begin());
		imagegraph::MeshValue3D mesh;
		auto &data = mesh.Data.emplace();
		data.LocalTransforms.emplace_back();
		data.Materials.emplace_back();
		imagegraph::MeshPart3D part;
		for (const imagegraph::Vector3 position :
			 {imagegraph::Vector3{-1, -1, .5},
			  imagegraph::Vector3{3, -1, .5},
			  imagegraph::Vector3{-1, 3, .5}})
			part.Vertices.push_back({position, {0, 0, -1}, {0, 0}, {255, 0, 0, 255}});
		data.Parts.push_back(std::move(part));
		request.Scene.Data.emplace().Objects.push_back({std::move(mesh)});
		return request;
	}
	std::array<uint8_t, 4>
	Pixel(engine::render::Renderer &renderer, engine::core::Name owner, engine::core::Name name) {
		auto *device = static_cast<SDL_GPUDevice *>(renderer.Backend().Device);
		auto *texture = static_cast<SDL_GPUTexture *>(
			engine::render::test_support::TransformImage3DResidentTestAccess::PublishedTexture(
				renderer, owner, name
			)
		);
		REQUIRE(texture);
		SDL_GPUTransferBufferCreateInfo info{};
		info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
		info.size = 64;
		auto *transfer = engine::render::gpu::CreateTransferBuffer(device, &info);
		REQUIRE(transfer);
		auto *command = SDL_AcquireGPUCommandBuffer(device);
		REQUIRE(command);
		auto *copy = SDL_BeginGPUCopyPass(command);
		REQUIRE(copy);
		SDL_GPUTextureRegion from{};
		from.texture = texture;
		from.w = from.h = 4;
		from.d = 1;
		SDL_GPUTextureTransferInfo to{};
		to.transfer_buffer = transfer;
		to.pixels_per_row = to.rows_per_layer = 4;
		SDL_DownloadFromGPUTexture(copy, &from, &to);
		SDL_EndGPUCopyPass(copy);
		auto *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
		REQUIRE(fence);
		REQUIRE(SDL_WaitForGPUFences(device, true, &fence, 1));
		SDL_ReleaseGPUFence(device, fence);
		auto *mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
		REQUIRE(mapped);
		std::array<uint8_t, 4> pixel{};
		std::memcpy(pixel.data(), mapped, 4);
		SDL_UnmapGPUTransferBuffer(device, transfer);
		engine::render::gpu::ReleaseTransferBuffer(device, transfer);
		return pixel;
	}
}
TEST_CASE(
	"one source camera pass publishes all seven real GPU outputs with tint and numeric formats",
	"[render][gpu][imagegraph]"
) {
	using namespace engine;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	const core::Name owner("source-camera-gpu");
	for (uint32_t output = 0; output < 7; ++output) {
		auto request = Triangle();
		request.Output = static_cast<render::imagegraph::SourceCamera3DOutput>(output);
		const auto status = fixture.Render.QueueSourceCamera3D(
			{owner, core::Name("source-camera-gpu-" + std::to_string(output)), output + 1, std::move(request)}
		);
		REQUIRE(
			status == (output == 0 ? render::imagegraph::TransformImage3DQueueResult::Queued
								   : render::imagegraph::TransformImage3DQueueResult::Replaced)
		);
	}
	REQUIRE(render::test_support::TransformImage3DResidentTestAccess::RecordAndSubmit(fixture.Render));
	REQUIRE(SDL_WaitForGPUIdle(static_cast<SDL_GPUDevice *>(fixture.Render.Backend().Device)));
	render::test_support::TransformImage3DResidentTestAccess::Poll(fixture.Render);
	std::array<std::array<uint8_t, 4>, 7> pixels;
	for (size_t output = 0; output < 7; ++output) {
		const core::Name name("source-camera-gpu-" + std::to_string(output));
		REQUIRE(
			render::test_support::TransformImage3DResidentTestAccess::PublishedGeneration(
				fixture.Render, owner, name
			) == output + 1
		);
		assets::TextureFormat format{};
		REQUIRE(
			render::test_support::TransformImage3DResidentTestAccess::PublishedFormat(
				fixture.Render, owner, name, format
			)
		);
		CHECK(format == assets::TextureFormat::RGBA8_LINEAR);
		pixels[output] = Pixel(fixture.Render, owner, name);
	}
	CHECK(pixels[0] == std::array<uint8_t, 4>{255, 0, 0, 255});
	CHECK(pixels[1] == pixels[0]);
	CHECK(pixels[2][0] >= 127);
	CHECK(pixels[2][0] <= 128);
	CHECK(pixels[2][2] == 0);
	CHECK(pixels[2][3] == 255);
	CHECK(pixels[3][2] == 255);
	CHECK(pixels[3][3] == 255);
	CHECK(pixels[4][0] >= 134);
	CHECK(pixels[4][0] <= 135);
	CHECK(pixels[4][0] == pixels[4][1]);
	CHECK(pixels[4][3] == 255);
	CHECK(pixels[5] == std::array<uint8_t, 4>{0, 0, 0, 255});
	CHECK(pixels[6] == std::array<uint8_t, 4>{0, 0, 0, 0});
}
TEST_CASE(
	"synchronous source camera export preserves authored packed and floating layouts",
	"[render][gpu][imagegraph]"
) {
	using namespace engine;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto request = Triangle();
	request.Output = render::imagegraph::SourceCamera3DOutput::Diffuse;
	request.Format = assets::TextureFormat::RGBA4_UNORM;
	render::imagegraph::SourceCamera3DResult result;
	REQUIRE(
		render::imagegraph::ExecuteSourceCamera3D(fixture.Render, request, result) ==
		render::imagegraph::SourceCamera3DStatus::Ok
	);
	REQUIRE(result.Pixels.size() == 32);
	CHECK(result.Pixels[0] == std::byte{15});
	CHECK(result.Pixels[1] == std::byte{240});
	request.Format = assets::TextureFormat::RGBA32_FLOAT;
	REQUIRE(
		render::imagegraph::ExecuteSourceCamera3D(fixture.Render, request, result) ==
		render::imagegraph::SourceCamera3DStatus::Ok
	);
	REQUIRE(result.Pixels.size() == 256);
	float red = 0;
	std::memcpy(&red, result.Pixels.data(), 4);
	CHECK(red == 1);
}

TEST_CASE(
	"source instances apply object matrix before instance scale and retain raw tint",
	"[render][gpu][imagegraph]"
) {
	using namespace engine;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto request = Triangle();
	request.Output = render::imagegraph::SourceCamera3DOutput::Diffuse;
	auto &mesh = *std::get<imagegraph::MeshValue3D>(request.Scene.Data->Objects[0].Data).Data;
	mesh.Instanced = true;
	mesh.InstanceObjectTransform.Position = {100, 0, 0};
	imagegraph::MeshInstance3D instance;
	instance.Fields[0] = -100;
	instance.Fields[8] = instance.Fields[9] = instance.Fields[10] = 1;
	mesh.Instances.push_back(instance);
	// Source instancing ignores the imported part matrix and packed instance RGB.
	std::array<double, 16> matrix{};
	matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1;
	matrix[12] = 100;
	mesh.Parts[0].LocalMatrix = matrix;
	render::imagegraph::SourceCamera3DResult result;
	REQUIRE(
		render::imagegraph::ExecuteSourceCamera3D(fixture.Render, request, result) ==
		render::imagegraph::SourceCamera3DStatus::Ok
	);
	REQUIRE(result.Pixels.size() == 64);
	CHECK(result.Pixels[0] == std::byte{255});
	CHECK(result.Pixels[1] == std::byte{0});
	mesh.Instances[0].Fields[0] = 100;
	REQUIRE(
		render::imagegraph::ExecuteSourceCamera3D(fixture.Render, request, result) ==
		render::imagegraph::SourceCamera3DStatus::Ok
	);
	CHECK(result.Pixels[0] == std::byte{0});
}

TEST_CASE(
	"source particle camera preserves floating RGBA and drops inactive instances",
	"[render][gpu][imagegraph][particle3d]"
) {
	using namespace engine;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto request = Triangle();
	request.Output = render::imagegraph::SourceCamera3DOutput::Diffuse;
	request.Format = assets::TextureFormat::RGBA32_FLOAT;
	auto &mesh = *std::get<imagegraph::MeshValue3D>(request.Scene.Data->Objects[0].Data).Data;
	mesh.Instanced = mesh.ParticleInstanced = true;
	mesh.ParticleBlend = imagegraph::ParticleBlend3D::Alpha;
	mesh.ParticleTransparent = true;
	imagegraph::MeshInstance3D transform;
	transform.Fields[8] = transform.Fields[9] = transform.Fields[10] = 1;
	mesh.Instances.push_back(transform);
	imagegraph::ParticleRecord3D particle;
	particle.Active = 1;
	particle.Colour = {1.5f, .25f, .125f, .5f};
	mesh.ParticleRecords.push_back(particle);
	for (auto &vertex : mesh.Parts[0].Vertices)
		vertex.Tint = {255, 255, 255, 255};
	render::imagegraph::SourceCamera3DResult result;
	REQUIRE(
		render::imagegraph::ExecuteSourceCamera3D(fixture.Render, request, result) ==
		render::imagegraph::SourceCamera3DStatus::Ok
	);
	REQUIRE(result.Pixels.size() == 4 * 4 * 16);
	std::array<float, 4> pixel;
	std::memcpy(pixel.data(), result.Pixels.data(), sizeof(pixel));
	CHECK(pixel[0] == 1.5f);
	CHECK(pixel[1] == .25f);
	CHECK(pixel[2] == .125f);
	CHECK(pixel[3] == .5f);
	mesh.ParticleRecords[0].Active = 0;
	REQUIRE(
		render::imagegraph::ExecuteSourceCamera3D(fixture.Render, request, result) ==
		render::imagegraph::SourceCamera3DStatus::Ok
	);
	std::memcpy(pixel.data(), result.Pixels.data(), sizeof(pixel));
	CHECK(pixel == std::array<float, 4>{0, 0, 0, 0});
}
