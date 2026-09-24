#include "../src/ImageGraphTransform3D.hpp"
#include "../src/ImageGraphTransform3DResident.hpp"
#include "GpuHeap.hpp"
#include "RenderFixture.hpp"

#include <engine/testing/Suite.hpp>

#include <SDL3/SDL_gpu.h>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <string>

TEST_SUITE_ID("engine.render.imagegraph_transform_3d_gpu")
TEST_DEPENDS("engine.render.fixtures")

namespace {
	using namespace engine;
	using namespace engine::render;
	using namespace engine::render::imagegraph;

	TransformImage3DSurface Surface(std::array<uint8_t, 16> pixels) {
		TransformImage3DSurface surface;
		surface.Width = 2;
		surface.Height = 2;
		surface.Rgba8.resize(pixels.size());
		for (size_t index = 0; index < pixels.size(); index++)
			surface.Rgba8[index] = std::byte{pixels[index]};
		return surface;
	}

	TransformImage3DSurface SolidSurface(uint32_t width, uint32_t height, uint8_t value) {
		TransformImage3DSurface surface;
		surface.Width = width;
		surface.Height = height;
		surface.Rgba8.assign(uint64_t(width) * height * 4, std::byte{value});
		return surface;
	}

	TransformImage3DLiveRequest LiveRequest(
		core::Name owner, core::Name name, uint64_t generation, uint32_t width = 2, uint32_t height = 2
	) {
		TransformImage3DLiveRequest request;
		request.Owner = owner;
		request.Name = name;
		request.Generation = generation;
		request.Request.Front = SolidSurface(width, height, 17);
		return request;
	}

	size_t FindSlot(
		const std::array<test_support::TransformImage3DQueueSlotSnapshot, 4> &slots,
		core::Name owner,
		core::Name name
	) {
		for (size_t index = 0; index < slots.size(); index++)
			if (slots[index].Owner == owner && slots[index].Name == name) return index;
		return slots.size();
	}
}

TEST_CASE("Transform Image 3D draws uploaded back pixels and reads both depth outputs", "[render][gpu]") {
	test::FixtureDevice fixture;
	fixture.Initialise();
	auto *device = static_cast<SDL_GPUDevice *>(fixture.Render.Backend().Device);
	REQUIRE(device != nullptr);
	const GpuMemoryStatistics before = gpu::MemoryStatistics(device);
	TransformImage3DRequest request;
	request.Front = Surface({255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255});
	request.Back = Surface({11, 22, 33, 255, 44, 55, 66, 255, 77, 88, 99, 255, 111, 122, 133, 255});
	request.Position = {0, 0, -1};
	TransformImage3DResult backResult;
	TransformImage3DResult result;
	{
		TransformImage3DGpuPass pass(device);
		TransformImage3DRequest invalid;
		invalid.Front.Width = 2;
		invalid.Front.Height = 2;
		REQUIRE(pass.Run(invalid, result) == TransformImage3DStatus::InvalidSurface);
		CHECK(gpu::MemoryStatistics(device).LiveBytes == before.LiveBytes);

		REQUIRE(pass.Run(request, backResult) == TransformImage3DStatus::Ok);
		CHECK(backResult.RenderedRgba8 == request.Back.Rgba8);

		// The second run has no back surface and uses the perspective branch. Reusing
		// the pass proves its old fence and textures are released before replacement.
		request.Back = {};
		request.Front = Surface({9, 18, 27, 255, 36, 45, 54, 255, 63, 72, 81, 255, 90, 99, 108, 255});
		request.Projection = TransformImage3DProjection::Perspective;
		request.FieldOfViewDegrees = 120;
		REQUIRE(pass.Run(request, result) == TransformImage3DStatus::Ok);
		CHECK(result.RenderedRgba8 == request.Front.Rgba8);

		// The pinned Pixel Composer transform applies anchor before rotation and scale.
		// A uniform source keeps this asymmetric anchor and rotation check independent
		// of sampling location while still requiring the transformed plane to cover the target.
		request.Back = Surface({17, 34, 51, 255, 17, 34, 51, 255, 17, 34, 51, 255, 17, 34, 51, 255});
		request.Position = {0, 0, -1};
		request.Anchor = {.05f, -.05f, 0};
		request.Rotation = {0, 0, .17364818f, .98480775f};
		request.Scale = {2, 2, 1};
		request.Projection = TransformImage3DProjection::Orthographic;
		REQUIRE(pass.Run(request, result) == TransformImage3DStatus::Ok);
		CHECK(result.RenderedRgba8 == request.Back.Rgba8);
	}
	TransformImage3DResult adapterResult;
	REQUIRE(ExecuteTransformImage3D(fixture.Render, request, adapterResult) == TransformImage3DStatus::Ok);
	CHECK(adapterResult.RenderedRgba8 == request.Back.Rgba8);
	CHECK(adapterResult.Mesh.Positions[0] == -1);
	CHECK(adapterResult.Mesh.Positions[16] == 1);
	CHECK(adapterResult.Mesh.TextureCoordinates[11] == 0);
	CHECK(result.Width == 2);
	CHECK(result.Height == 2);
	REQUIRE(result.RenderedRgba8.size() == request.Front.Rgba8.size());
	CHECK(result.RenderedRgba8 == request.Back.Rgba8);
	REQUIRE(backResult.DepthRgba8.size() == request.Front.Rgba8.size());
	for (size_t index = 0; index < backResult.DepthRgba8.size(); index += 4) {
		CHECK(std::to_integer<uint8_t>(backResult.DepthRgba8[index]) >= 225);
		CHECK(std::to_integer<uint8_t>(backResult.DepthRgba8[index]) <= 232);
		CHECK(std::to_integer<uint8_t>(backResult.DepthRgba8[index + 3]) == 255);
	}
	REQUIRE(backResult.Depth.size() == 4);
	for (float depth : backResult.Depth)
		CHECK((depth > .09f && depth < .11f));
	const GpuMemoryStatistics after = gpu::MemoryStatistics(device);
	CHECK(after.LiveBytes == before.LiveBytes);
}

TEST_CASE("Transform Image 3D refuses invalid source data before GPU allocation", "[render][gpu]") {
	TransformImage3DRequest request;
	request.Front.Width = 2;
	request.Front.Height = 2;
	CHECK(ValidateTransformImage3D(request) == TransformImage3DStatus::InvalidSurface);
	request.Front = Surface({0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255});
	request.DepthRange = {1, 1};
	CHECK(ValidateTransformImage3D(request) == TransformImage3DStatus::InvalidControl);
}

TEST_CASE("Transform Image 3D queue preserves controls and in-flight ownership", "[render]") {
	Renderer renderer;
	const core::Name owner("imagegraph.transform.owner");
	const core::Name name("imagegraph.transform.name");
	auto first = LiveRequest(owner, name, 1);
	first.Request.Position = {1, 2, 3};
	first.Request.Anchor = {4, 5, 6};
	first.Request.Rotation = {0, 0, .5f, .8660254f};
	first.Request.Scale = {7, 8, 9};
	first.Request.TextureTiling = {2, 3};
	first.Request.Projection = TransformImage3DProjection::Perspective;
	first.Request.FieldOfViewDegrees = 70;
	first.Request.ViewRange = {.1f, 20};
	first.Request.DepthRange = {.2f, .8f};
	const auto expected = first.Request;
	REQUIRE(renderer.QueueTransformImage3D(std::move(first)) == TransformImage3DQueueResult::Queued);
	auto slots = test_support::TransformImage3DResidentTestAccess::Slots(renderer);
	const size_t firstSlot = FindSlot(slots, owner, name);
	REQUIRE(firstSlot < slots.size());
	CHECK(slots[firstSlot].Request.Position == expected.Position);
	CHECK(slots[firstSlot].Request.Anchor == expected.Anchor);
	CHECK(slots[firstSlot].Request.Rotation == expected.Rotation);
	CHECK(slots[firstSlot].Request.Scale == expected.Scale);
	CHECK(slots[firstSlot].Request.TextureTiling == expected.TextureTiling);
	CHECK(slots[firstSlot].Request.Projection == expected.Projection);
	CHECK(slots[firstSlot].Request.FieldOfViewDegrees == expected.FieldOfViewDegrees);
	CHECK(slots[firstSlot].Request.ViewRange == expected.ViewRange);
	CHECK(slots[firstSlot].Request.DepthRange == expected.DepthRange);
	CHECK(slots[firstSlot].Request.Front.Rgba8 == expected.Front.Rgba8);

	REQUIRE(
		test_support::TransformImage3DResidentTestAccess::SetPhase(
			renderer, firstSlot, test_support::TransformImage3DQueuePhase::Recorded
		)
	);
	for (uint64_t generation = 1; generation <= 3; generation++)
		REQUIRE(
			renderer.QueueTransformImage3D(LiveRequest(
				owner, core::Name("imagegraph.transform.full" + std::to_string(generation)), generation
			)) == TransformImage3DQueueResult::Queued
		);
	CHECK(renderer.QueueTransformImage3D(LiveRequest(owner, name, 2)) == TransformImage3DQueueResult::Full);
	slots = test_support::TransformImage3DResidentTestAccess::Slots(renderer);
	CHECK(slots[firstSlot].Phase == test_support::TransformImage3DQueuePhase::Recorded);
	CHECK_FALSE(slots[firstSlot].Cancelled);
	CHECK(slots[firstSlot].Generation == 1);
	CHECK(renderer.CancelTransformImage3D(owner, name, 1));
	slots = test_support::TransformImage3DResidentTestAccess::Slots(renderer);
	CHECK(slots[firstSlot].Phase == test_support::TransformImage3DQueuePhase::Recorded);
	CHECK(slots[firstSlot].Cancelled);
	renderer.DropTransformImage3DOwner(owner);
	slots = test_support::TransformImage3DResidentTestAccess::Slots(renderer);
	CHECK(slots[firstSlot].Phase == test_support::TransformImage3DQueuePhase::Recorded);
	CHECK(slots[firstSlot].Cancelled);
}

TEST_CASE("Transform Image 3D queue cancels older in-flight work after queued replacement", "[render]") {
	Renderer renderer;
	const core::Name owner("imagegraph.transform.stale.owner");
	const core::Name name("imagegraph.transform.stale.name");
	REQUIRE(
		renderer.QueueTransformImage3D(LiveRequest(owner, name, 1)) == TransformImage3DQueueResult::Queued
	);
	auto slots = test_support::TransformImage3DResidentTestAccess::Slots(renderer);
	const size_t oldSlot = FindSlot(slots, owner, name);
	REQUIRE(oldSlot < slots.size());
	REQUIRE(
		test_support::TransformImage3DResidentTestAccess::SetPhase(
			renderer, oldSlot, test_support::TransformImage3DQueuePhase::Recorded
		)
	);
	REQUIRE(
		renderer.QueueTransformImage3D(LiveRequest(owner, name, 2)) == TransformImage3DQueueResult::Queued
	);
	slots = test_support::TransformImage3DResidentTestAccess::Slots(renderer);
	const size_t queuedSlot = FindSlot(slots, owner, name);
	REQUIRE(queuedSlot < slots.size());
	REQUIRE(queuedSlot != oldSlot);
	REQUIRE(test_support::TransformImage3DResidentTestAccess::SetCancelled(renderer, oldSlot, false));
	CHECK(
		renderer.QueueTransformImage3D(LiveRequest(owner, name, 3)) == TransformImage3DQueueResult::Replaced
	);
	slots = test_support::TransformImage3DResidentTestAccess::Slots(renderer);
	CHECK(slots[oldSlot].Phase == test_support::TransformImage3DQueuePhase::Recorded);
	CHECK(slots[oldSlot].Cancelled);
	CHECK(slots[queuedSlot].Generation == 3);
}

TEST_CASE("Transform Image 3D queue replaces queued sources by net budget", "[render]") {
	Renderer renderer;
	const core::Name owner("imagegraph.transform.budget.owner");
	const core::Name name("imagegraph.transform.budget.name");
	constexpr uint32_t side = 1296;
	for (uint64_t generation = 1; generation <= 4; generation++) {
		auto request = LiveRequest(
			owner,
			generation == 1 ? name : core::Name("imagegraph.transform.budget" + std::to_string(generation)),
			generation,
			side,
			side
		);
		request.Request.Back = SolidSurface(side, side, 31);
		REQUIRE(renderer.QueueTransformImage3D(std::move(request)) == TransformImage3DQueueResult::Queued);
	}
	const uint64_t held = test_support::TransformImage3DResidentTestAccess::SourceBytes(renderer);
	CHECK(held > imagegraph::MAXIMUM_TRANSFORM_IMAGE_3D_OUTPUT_BYTES - uint64_t(side) * side * 8);
	auto replacement = LiveRequest(owner, name, 5, side, side);
	replacement.Request.Back = SolidSurface(side, side, 47);
	CHECK(renderer.QueueTransformImage3D(std::move(replacement)) == TransformImage3DQueueResult::Replaced);
	CHECK(test_support::TransformImage3DResidentTestAccess::SourceBytes(renderer) == held);
	auto slots = test_support::TransformImage3DResidentTestAccess::Slots(renderer);
	const size_t replacementSlot = FindSlot(slots, owner, name);
	REQUIRE(replacementSlot < slots.size());
	CHECK(slots[replacementSlot].Phase == test_support::TransformImage3DQueuePhase::Queued);
	CHECK(slots[replacementSlot].Generation == 5);
}

TEST_CASE("Transform Image 3D live queue records one fenced resident GPU pass", "[render][gpu]") {
	test::FixtureDevice fixture;
	fixture.Initialise();
	const core::Name owner("imagegraph.transform.live.owner");
	const core::Name name("imagegraph.transform.live.name");
	auto request = LiveRequest(owner, name, 1);
	request.Request.Back = SolidSurface(2, 2, 93);
	request.Request.Position = {0, 0, -1};
	request.Request.Anchor = {.25f, -.5f, 0};
	request.Request.Rotation = {0, 0, .5f, .8660254f};
	request.Request.Scale = {2, 3, 1};
	request.Request.TextureTiling = {2, 4};
	request.Request.DepthRange = {.2f, .7f};
	request.Request.ViewRange = {.1f, 20};
	request.Request.Projection = TransformImage3DProjection::Perspective;
	request.Request.FieldOfViewDegrees = 70;
	const auto expected = request.Request;
	CHECK(
		test_support::TransformImage3DResidentTestAccess::PublishedTexture(fixture.Render, owner, name) ==
		nullptr
	);
	REQUIRE(fixture.Render.QueueTransformImage3D(std::move(request)) == TransformImage3DQueueResult::Queued);
	REQUIRE(test_support::TransformImage3DResidentTestAccess::RecordAndSubmit(fixture.Render));
	const auto slots = test_support::TransformImage3DResidentTestAccess::Slots(fixture.Render);
	const size_t slot = FindSlot(slots, owner, name);
	REQUIRE(slot < slots.size());
	CHECK(slots[slot].Phase == test_support::TransformImage3DQueuePhase::Submitted);
	CHECK(slots[slot].ScratchBytes > 0);
	CHECK(slots[slot].Request.Position == expected.Position);
	CHECK(slots[slot].Request.Anchor == expected.Anchor);
	CHECK(slots[slot].Request.Rotation == expected.Rotation);
	CHECK(slots[slot].Request.Scale == expected.Scale);
	CHECK(slots[slot].Request.TextureTiling == expected.TextureTiling);
	CHECK(slots[slot].Request.DepthRange == expected.DepthRange);
	CHECK(slots[slot].Request.ViewRange == expected.ViewRange);
	CHECK(slots[slot].Request.Projection == expected.Projection);
	CHECK(slots[slot].Request.FieldOfViewDegrees == expected.FieldOfViewDegrees);
	REQUIRE(SDL_WaitForGPUIdle(static_cast<SDL_GPUDevice *>(fixture.Render.Backend().Device)));
	test_support::TransformImage3DResidentTestAccess::Poll(fixture.Render);
	CHECK(
		test_support::TransformImage3DResidentTestAccess::Slots(fixture.Render)[slot].Phase ==
		test_support::TransformImage3DQueuePhase::Free
	);
	void *first =
		test_support::TransformImage3DResidentTestAccess::PublishedTexture(fixture.Render, owner, name);
	REQUIRE(first != nullptr);
	CHECK(
		test_support::TransformImage3DResidentTestAccess::PublishedGeneration(fixture.Render, owner, name) ==
		1
	);
	CHECK(
		test_support::TransformImage3DResidentTestAccess::PublishedTexture(
			fixture.Render, core::Name("imagegraph.transform.other.owner"), name
		) == nullptr
	);

	REQUIRE(
		fixture.Render.QueueTransformImage3D(LiveRequest(owner, name, 2)) ==
		TransformImage3DQueueResult::Queued
	);
	REQUIRE(test_support::TransformImage3DResidentTestAccess::RecordAndSubmit(fixture.Render));
	REQUIRE(SDL_WaitForGPUIdle(static_cast<SDL_GPUDevice *>(fixture.Render.Backend().Device)));
	test_support::TransformImage3DResidentTestAccess::Poll(fixture.Render);
	void *second =
		test_support::TransformImage3DResidentTestAccess::PublishedTexture(fixture.Render, owner, name);
	REQUIRE(second != nullptr);
	CHECK(second != first);
	CHECK(
		test_support::TransformImage3DResidentTestAccess::PublishedGeneration(fixture.Render, owner, name) ==
		2
	);

	REQUIRE(
		fixture.Render.QueueTransformImage3D(LiveRequest(owner, name, 3)) ==
		TransformImage3DQueueResult::Queued
	);
	REQUIRE(test_support::TransformImage3DResidentTestAccess::RecordAndSubmit(fixture.Render));
	REQUIRE(fixture.Render.CancelTransformImage3D(owner, name, 3));
	REQUIRE(SDL_WaitForGPUIdle(static_cast<SDL_GPUDevice *>(fixture.Render.Backend().Device)));
	test_support::TransformImage3DResidentTestAccess::Poll(fixture.Render);
	CHECK(
		test_support::TransformImage3DResidentTestAccess::PublishedTexture(fixture.Render, owner, name) ==
		second
	);
	CHECK(
		test_support::TransformImage3DResidentTestAccess::PublishedGeneration(fixture.Render, owner, name) ==
		2
	);
}
