#include "RenderStageProbe.hpp"

#include "RenderFixture.hpp"

#include <engine/core/Paths.hpp>
#include <engine/testing/Suite.hpp>

#include <array>

TEST_SUITE_ID("engine.render.stageprobe")

TEST_CASE("stage probe arms once on the named display transition", "[render][stage-probe]") {
	engine::render::RenderStageProbe probe;
	probe.Directory = "stage-probe-transition-test";
	probe.TransitionFrom = "client.replica";
	probe.TransitionTo = "client.portal.approach.1";
	probe.ObserveDisplay("client.replica", 0);
	probe.ActivateView(10, "client.portal.approach.1", 0, true);
	CHECK_FALSE(probe.Enabled(10, 0));
	probe.ObserveDisplay("client.portal.approach.1", 0);
	probe.ActivateView(11, "client.portal.approach.1", 1, true);
	probe.ActivateView(11, "client.portal.approach.1", 0, false);
	CHECK_FALSE(probe.Enabled(11, 0));
	probe.ActivateView(11, "client.portal.approach.1", 0, true);
	CHECK(probe.Enabled(11, 0));
	CHECK_FALSE(probe.Enabled(11, 1));
	CHECK_FALSE(probe.Enabled(12, 0));
	CHECK(probe.Wants("gbuffer", "albedo"));
	CHECK(probe.Wants("gbuffer", "depth"));
	CHECK(probe.Wants("portal-overlay", "portaled"));
	CHECK_FALSE(probe.Wants("gbuffer", "normal"));
	probe.ObserveDisplay("client.replica", 0);
	probe.ObserveDisplay("client.portal.approach.1", 0);
	probe.ActivateView(20, "client.portal.approach.1", 0, true);
	CHECK_FALSE(probe.Enabled(20, 0));
}

TEST_CASE(
	"stage snapshots keep pixels from before an aliased target is overwritten",
	"[render][gpu][stage-probe][.]"
) {
	using namespace engine;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto *device = static_cast<SDL_GPUDevice *>(fixture.Render.Backend().Device);
	render::RenderStageProbe probe;
	probe.Directory = core::Paths::Base() / "stage-probe-lifetime";
	std::filesystem::remove_all(probe.Directory);
	std::filesystem::create_directories(probe.Directory);
	SDL_GPUTextureCreateInfo info{};
	info.type = SDL_GPU_TEXTURETYPE_2D;
	info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
	info.width = 3;
	info.height = 2;
	info.layer_count_or_depth = info.num_levels = 1;
	const auto release = [device](SDL_GPUTexture *texture) { render::gpu::ReleaseTexture(device, texture); };
	std::unique_ptr<SDL_GPUTexture, decltype(release)> texture(
		render::gpu::CreateTexture(device, &info), release
	);
	REQUIRE(texture);
	auto *command = SDL_AcquireGPUCommandBuffer(device);
	REQUIRE(command);
	for (int stage = 0; stage < 2; ++stage) {
		SDL_GPUColorTargetInfo target{};
		target.texture = texture.get();
		target.load_op = SDL_GPU_LOADOP_CLEAR;
		target.store_op = SDL_GPU_STOREOP_STORE;
		target.clear_color = stage == 0 ? SDL_FColor{0, 0, 1, 1} : SDL_FColor{1, 0, 0, 1};
		auto *pass = SDL_BeginGPURenderPass(command, &target, 1, nullptr);
		REQUIRE(pass);
		SDL_EndGPURenderPass(pass);
		probe.Record(
			device, command, 1, "\"stage\":" + std::to_string(stage), texture.get(), 3, 2, info.format
		);
	}
	REQUIRE(SDL_SubmitGPUCommandBuffer(command));
	REQUIRE(probe.Pending.size() == 2);
	probe.Flush(device);
	CHECK(probe.Pending.empty());
	CHECK(probe.PendingBytes == 0);
	for (int stage = 0; stage < 2; ++stage) {
		const auto stem = probe.Directory / ("1-" + std::to_string(stage));
		REQUIRE(std::filesystem::file_size(stem.string() + ".bin") == 512);
		std::ifstream file(stem.string() + ".bin", std::ios::binary);
		std::array<uint8_t, 512> pixels{};
		REQUIRE(bool(file.read(reinterpret_cast<char *>(pixels.data()), pixels.size())));
		for (size_t row = 0; row < 2; ++row)
			for (size_t column = 0; column < 3; ++column) {
				const auto offset = row * 256 + column * 4;
				CHECK(pixels[offset] == (stage == 0 ? 0 : 255));
				CHECK(pixels[offset + 1] == 0);
				CHECK(pixels[offset + 2] == (stage == 0 ? 255 : 0));
				CHECK(pixels[offset + 3] == 255);
			}
		REQUIRE(std::filesystem::exists(stem.string() + ".bmp"));
		REQUIRE(std::filesystem::exists(stem.string() + ".json"));
	}
	CHECK(std::filesystem::exists(probe.Directory / "index.html"));
	std::filesystem::remove_all(probe.Directory);
}
