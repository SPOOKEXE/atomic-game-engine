#include "../src/ImageGraphTransform3DResident.hpp"
#include "RenderFixture.hpp"

#include <engine/render/SourceSkyboxGroup.hpp>
#include <engine/render/TextureTable.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.render.sourceskyboxgroup_gpu")
TEST_DEPENDS("engine.render.fixtures")
TEST_DEPENDS("engine.render.sourceskyboxgroup")
namespace {
	using namespace engine;
	namespace ri = render::imagegraph;
	using Access = render::test_support::TransformImage3DResidentTestAccess;
	assets::TextureData Solid(uint8_t red) {
		assets::TextureData texture;
		texture.Width = texture.Height = 8;
		texture.Format = assets::TextureFormat::RGBA8;
		texture.Pixels.resize(256);
		for (size_t i = 0; i < 256; i += 4) {
			texture.Pixels[i] = std::byte{red};
			texture.Pixels[i + 3] = std::byte{255};
		}
		return texture;
	}
	ri::SourceCamera3DRequest Camera() {
		ri::SourceCamera3DRequest request;
		request.Width = request.Height = 8;
		request.Format = assets::TextureFormat::RGBA8;
		request.Output = ri::SourceCamera3DOutput::Diffuse;
		request.CullMode = 0;
		request.View = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
		request.Projection = request.View;
		imagegraph::MeshValue3D mesh;
		auto &data = mesh.Data.emplace();
		data.LocalTransforms.emplace_back();
		data.Materials.emplace_back();
		imagegraph::MeshPart3D part;
		for (const imagegraph::Vector3 point :
			 {imagegraph::Vector3{-1, -1, .5},
			  imagegraph::Vector3{3, -1, .5},
			  imagegraph::Vector3{-1, 3, .5}})
			part.Vertices.push_back({point, {0, 0, -1}, {0, 0}, {255, 0, 0, 255}});
		data.Parts.push_back(std::move(part));
		request.Scene.Data.emplace().Objects.push_back({std::move(mesh)});
		return request;
	}
	ri::SourceSdfRequest Sphere() {
		ri::SourceSdfRequest request;
		request.Width = request.Height = 8;
		request.Format = assets::TextureFormat::RGBA8;
		request.CameraRotation = {0, 0, 0};
		request.UseLight = false;
		auto &data = request.Object.Data.emplace();
		data.Shapes.emplace_back();
		auto &shape = data.Shapes.front();
		shape.Shape = 200;
		shape.Identity = "skybox-sphere";
		shape.Radius = 1;
		shape.Diffuse = {255, 0, 0, 255};
		data.Operations.push_back({0, 0});
		return request;
	}
	ri::SourceSkyboxRequest Request(core::Name owner, core::Name staging, uint64_t generation, bool mixed) {
		ri::SourceSkyboxRequest request;
		request.Owner = owner;
		request.StagingOwner = staging;
		for (size_t i = 0; i < 6; ++i) {
			request.Targets[i] = {core::Name("skybox-device-face-" + std::to_string(i)), generation};
			request.Faces[i] = Solid(uint8_t(20 + i));
		}
		if (mixed) {
			for (size_t i = 0; i < 5; ++i) {
				auto camera = Camera();
				camera.CameraPosition[0] = static_cast<float>(i);
				request.Faces[i] = std::move(camera);
			}
			request.Faces[5] = Sphere();
		}
		return request;
	}
	std::array<std::byte, 256> Download(render::Renderer &renderer, core::Name owner, core::Name name) {
		auto *device = static_cast<SDL_GPUDevice *>(renderer.Backend().Device);
		auto *texture = static_cast<SDL_GPUTexture *>(renderer.TextureHandle(name, owner));
		REQUIRE(texture);
		SDL_GPUTransferBufferCreateInfo info{};
		info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
		info.size = 256;
		auto *transfer = render::gpu::CreateTransferBuffer(device, &info);
		REQUIRE(transfer);
		struct Transfer {
			SDL_GPUDevice *Device;
			SDL_GPUTransferBuffer *Value;
			~Transfer() {
				render::gpu::ReleaseTransferBuffer(Device, Value);
			}
		} retained{device, transfer};
		auto *command = SDL_AcquireGPUCommandBuffer(device);
		REQUIRE(command);
		auto *copy = SDL_BeginGPUCopyPass(command);
		REQUIRE(copy);
		SDL_GPUTextureRegion region{};
		region.texture = texture;
		region.w = region.h = 8;
		region.d = 1;
		SDL_GPUTextureTransferInfo destination{};
		destination.transfer_buffer = transfer;
		destination.pixels_per_row = destination.rows_per_layer = 8;
		SDL_DownloadFromGPUTexture(copy, &region, &destination);
		SDL_EndGPUCopyPass(copy);
		auto *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
		REQUIRE(fence);
		struct Fence {
			SDL_GPUDevice *Device;
			SDL_GPUFence *Value;
			~Fence() {
				SDL_ReleaseGPUFence(Device, Value);
			}
		} pending{device, fence};
		const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!SDL_QueryGPUFence(device, fence) && std::chrono::steady_clock::now() < end)
			SDL_Delay(1);
		REQUIRE(SDL_QueryGPUFence(device, fence));
		auto *mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
		REQUIRE(mapped);
		std::array<std::byte, 256> result{};
		std::memcpy(result.data(), mapped, result.size());
		SDL_UnmapGPUTransferBuffer(device, transfer);
		return result;
	}
	void CheckGeneration(render::Renderer &renderer, core::Name owner, uint64_t generation) {
		for (size_t i = 0; i < 6; ++i)
			CHECK(
				renderer.SourceOutputStatus(
					owner, core::Name("skybox-device-face-" + std::to_string(i)), generation
				) == render::SourceTextureStatus::Ready
			);
	}
}
TEST_CASE(
	"mixed camera and SDF skybox publishes six faces atomically and cancels submitted replacement",
	"[render][gpu][source-skybox-group-gpu][.]"
) {
	using namespace engine;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	const core::Name owner("skybox-device-owner"), staging("skybox-device-staging");
	ri::SourceSkyboxGroup group;
	std::string diagnostic;
	REQUIRE(
		ri::BeginSourceSkybox(fixture.Render, group, Request(owner, staging, 1, false), diagnostic) ==
		ri::SourceSkyboxStatus::Pending
	);
	REQUIRE(ri::RefreshSourceSkybox(fixture.Render, group, diagnostic) == ri::SourceSkyboxStatus::Published);
	CheckGeneration(fixture.Render, owner, 1);
	REQUIRE(
		ri::BeginSourceSkybox(fixture.Render, group, Request(owner, staging, 2, true), diagnostic) ==
		ri::SourceSkyboxStatus::Pending
	);
	REQUIRE(ri::RefreshSourceSkybox(fixture.Render, group, diagnostic) == ri::SourceSkyboxStatus::Pending);
	// Six independent passes exceed the four retained slots. Old faces remain one complete generation.
	CHECK(std::count(group.Admitted.begin(), group.Admitted.end(), true) == 4);
	CheckGeneration(fixture.Render, owner, 1);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	auto status = ri::SourceSkyboxStatus::Pending;
	while (status == ri::SourceSkyboxStatus::Pending && std::chrono::steady_clock::now() < deadline) {
		Access::Poll(fixture.Render);
		REQUIRE(Access::RecordAndSubmit(fixture.Render));
		status = ri::RefreshSourceSkybox(fixture.Render, group, diagnostic);
		INFO(diagnostic);
		REQUIRE(status != ri::SourceSkyboxStatus::Failed);
		if (status == ri::SourceSkyboxStatus::Pending) {
			CheckGeneration(fixture.Render, owner, 1);
			SDL_Delay(1);
		}
	}
	REQUIRE(status == ri::SourceSkyboxStatus::Published);
	CheckGeneration(fixture.Render, owner, 2);
	for (size_t i = 0; i < 6; ++i) {
		const auto pixels =
			Download(fixture.Render, owner, core::Name("skybox-device-face-" + std::to_string(i)));
		const size_t center = (4 * 8 + 4) * 4;
		CHECK(pixels[center] == std::byte{255});
		CHECK(pixels[center + 1] == std::byte{0});
		CHECK(pixels[center + 2] == std::byte{0});
		CHECK(pixels[center + 3] == std::byte{255});
	}
	REQUIRE(
		ri::BeginSourceSkybox(fixture.Render, group, Request(owner, staging, 3, true), diagnostic) ==
		ri::SourceSkyboxStatus::Pending
	);
	REQUIRE(ri::RefreshSourceSkybox(fixture.Render, group, diagnostic) == ri::SourceSkyboxStatus::Pending);
	REQUIRE(Access::RecordAndSubmit(fixture.Render));
	ri::CancelSourceSkybox(fixture.Render, group);
	CheckGeneration(fixture.Render, owner, 2);
	// Wait only in the fixture. A late cancelled fence must not publish generation three.
	REQUIRE(SDL_WaitForGPUIdle(static_cast<SDL_GPUDevice *>(fixture.Render.Backend().Device)));
	Access::Poll(fixture.Render);
	CheckGeneration(fixture.Render, owner, 2);
	for (size_t i = 0; i < 6; ++i)
		CHECK(
			fixture.Render.SourceOutputStatus(
				owner, core::Name("skybox-device-face-" + std::to_string(i)), 3
			) == render::SourceTextureStatus::Absent
		);
	fixture.Render.ForgetWorld(1, owner);
	for (size_t i = 0; i < 6; ++i)
		CHECK(
			fixture.Render.SourceOutputStatus(
				owner, core::Name("skybox-device-face-" + std::to_string(i)), 2
			) == render::SourceTextureStatus::Absent
		);
}
