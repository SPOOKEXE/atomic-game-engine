#include "RenderFixture.hpp"
#include "VulkanTimestamps.hpp"

#include <engine/testing/Suite.hpp>

#include <SDL3/SDL_gpu.h>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <memory>

TEST_SUITE_ID("engine.render.vulkantimestampsgpu")

TEST_CASE("Vulkan timestamps retain the measured eight-view mark workload", "[render][gpu][timestamps]") {
	using engine::render::VulkanTimestamps;

	engine::render::test::FixtureDevice fixture;
	fixture.Initialise();
	if (fixture.Render.BackendName() != "vulkan") SKIP("Vulkan timestamps require the Vulkan backend.");

	auto *device = static_cast<SDL_GPUDevice *>(fixture.Render.Backend().Device);
	VulkanTimestamps timestamps;
	REQUIRE(timestamps.Probe(device));
	struct DrainDevice {
		SDL_GPUDevice *Device;
		~DrainDevice() {
			// A failed assertion must retire submitted commands before the query pools.
			(void)SDL_WaitForGPUIdle(Device);
		}
	} drain{device};
	const auto cancel = [](SDL_GPUCommandBuffer *command) { (void)SDL_CancelGPUCommandBuffer(command); };

	std::unique_ptr<SDL_GPUCommandBuffer, decltype(cancel)> command(
		SDL_AcquireGPUCommandBuffer(device), cancel
	);
	REQUIRE(command != nullptr);
	const uint32_t slot = timestamps.Begin(command.get());
	REQUIRE(slot < VulkanTimestamps::SLOTS);
	constexpr uint32_t REQUIRED_MARKS = 202;
	for (uint32_t index = 0; index < REQUIRED_MARKS; ++index) {
		const uint32_t mark = timestamps.Mark(command.get());
		REQUIRE(mark < VulkanTimestamps::MARKS);
		REQUIRE(mark == index);
	}
	for (uint32_t index = REQUIRED_MARKS; index < VulkanTimestamps::MARKS; ++index)
		REQUIRE(timestamps.Mark(command.get()) == index);
	CHECK(timestamps.Mark(command.get()) == VulkanTimestamps::MARKS);
	REQUIRE(SDL_SubmitGPUCommandBuffer(command.release()));
	timestamps.Submitted(slot);
	REQUIRE(timestamps.Pending(slot));
	REQUIRE(SDL_WaitForGPUIdle(device));

	std::array<double, VulkanTimestamps::MARKS> times{};
	uint32_t count = 0;
	REQUIRE(timestamps.Collect(slot, times.data(), count));
	REQUIRE(count == VulkanTimestamps::MARKS);
	CHECK(times[0] == 0.0);
	for (uint32_t index = 1; index < count; ++index)
		CHECK(times[index] >= times[index - 1]);
	CHECK_FALSE(timestamps.Pending(slot));
	CHECK_FALSE(timestamps.Collect(slot, times.data(), count));

	std::unique_ptr<SDL_GPUCommandBuffer, decltype(cancel)> cancelled(
		SDL_AcquireGPUCommandBuffer(device), cancel
	);
	REQUIRE(cancelled != nullptr);
	const uint32_t otherSlot = timestamps.Begin(cancelled.get(), slot);
	REQUIRE(otherSlot < VulkanTimestamps::SLOTS);
	REQUIRE(otherSlot != slot);
	REQUIRE(timestamps.Mark(cancelled.get()) == 0);
	REQUIRE(SDL_CancelGPUCommandBuffer(cancelled.release()));
	timestamps.Abandon(otherSlot);
	CHECK_FALSE(timestamps.Pending(otherSlot));

	std::unique_ptr<SDL_GPUCommandBuffer, decltype(cancel)> reused(
		SDL_AcquireGPUCommandBuffer(device), cancel
	);
	REQUIRE(reused != nullptr);
	const uint32_t reusedSlot = timestamps.Begin(reused.get(), slot);
	REQUIRE(reusedSlot < VulkanTimestamps::SLOTS);
	REQUIRE(reusedSlot != slot);
	REQUIRE(timestamps.Mark(reused.get()) == 0);
	REQUIRE(SDL_SubmitGPUCommandBuffer(reused.release()));
	timestamps.Submitted(reusedSlot);
	REQUIRE(SDL_WaitForGPUIdle(device));
	count = 0;
	REQUIRE(timestamps.Collect(reusedSlot, times.data(), count));
	CHECK(count == 1);
}
