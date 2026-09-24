#include "RenderFixture.hpp"

#include <engine/render/LiveImagePublisher.hpp>
#include <engine/render/TextureTable.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.render.liveimagepublisher")

namespace {

	using engine::core::Name;
	using engine::render::LiveImageBinding;
	using engine::render::LiveImagePublishStatus;

	std::vector<std::byte> Pixels(uint32_t width, uint32_t height, uint8_t value) {
		return std::vector<std::byte>(static_cast<size_t>(width) * height * 4, static_cast<std::byte>(value));
	}

	LiveImagePublishStatus Publish(
		engine::render::LiveImagePublisher &publisher,
		engine::render::Renderer &renderer,
		const LiveImageBinding &binding,
		uint32_t width,
		uint32_t height,
		const std::vector<std::byte> &pixels
	) {
		return publisher.Publish(renderer, binding, width, height, std::span<const std::byte>(pixels));
	}
}

TEST_CASE("live images are isolated by owner and retire once", "[render][gpu][live-image][.]") {
	engine::render::test::FixtureDevice fixture;
	fixture.Initialise();

	engine::render::LiveImagePublisher publisher;
	const Name firstOwner("live-image:first"), secondOwner("live-image:second"),
		name("live-image:shared-output");
	const auto firstResult = publisher.BeginBinding(firstOwner, name);
	const auto secondResult = publisher.BeginBinding(secondOwner, name);
	REQUIRE(firstResult.has_value());
	REQUIRE(secondResult.has_value());
	const LiveImageBinding first = *firstResult, second = *secondResult;
	const std::vector<std::byte> firstPixels = Pixels(2, 2, 31);
	const std::vector<std::byte> secondPixels = Pixels(4, 2, 92);

	CHECK(Publish(publisher, fixture.Render, first, 2, 2, firstPixels) == LiveImagePublishStatus::Published);
	CHECK(
		Publish(publisher, fixture.Render, second, 4, 2, secondPixels) == LiveImagePublishStatus::Published
	);
	const void *const firstTexture = fixture.Render.TextureHandle(name, firstOwner);
	const void *const secondTexture = fixture.Render.TextureHandle(name, secondOwner);
	REQUIRE(firstTexture != nullptr);
	REQUIRE(secondTexture != nullptr);
	CHECK(firstTexture != secondTexture);
	uint32_t width = 0, height = 0;
	REQUIRE(fixture.Render.TextureSize(name, width, height, firstOwner));
	CHECK(width == 2);
	CHECK(height == 2);
	REQUIRE(fixture.Render.TextureSize(name, width, height, secondOwner));
	CHECK(width == 4);
	CHECK(height == 2);

	CHECK(publisher.RetireOwner(fixture.Render, firstOwner) == 1);
	CHECK(publisher.ActiveBindingCount() == 1);
	CHECK(fixture.Render.TextureHandle(name, firstOwner) == nullptr);
	CHECK(Publish(publisher, fixture.Render, first, 2, 2, firstPixels) == LiveImagePublishStatus::Stale);
	CHECK(fixture.Render.TextureHandle(name, secondOwner) == secondTexture);
	CHECK(publisher.RetireOwner(fixture.Render, firstOwner) == 0);
	CHECK(publisher.Retire(fixture.Render, second));
	CHECK_FALSE(publisher.Retire(fixture.Render, second));
	CHECK(Publish(publisher, fixture.Render, second, 4, 2, secondPixels) == LiveImagePublishStatus::Stale);
	CHECK(fixture.Render.TextureHandle(name, secondOwner) == nullptr);
	CHECK(publisher.ActiveBindingCount() == 0);
}

TEST_CASE(
	"live image generations reject stale work and keep the last good image", "[render][gpu][live-image][.]"
) {
	engine::render::test::FixtureDevice fixture;
	fixture.Initialise();

	engine::render::LiveImagePublisher publisher;
	const Name owner("live-image:generation"), name("live-image:output");
	const auto firstResult = publisher.BeginBinding(owner, name);
	REQUIRE(firstResult.has_value());
	const LiveImageBinding first = *firstResult;
	const std::vector<std::byte> firstPixels = Pixels(2, 2, 18);
	const std::vector<std::byte> reboundPixels = Pixels(4, 1, 73);
	const std::vector<std::byte> replacementPixels = Pixels(1, 1, 201);

	CHECK(Publish(publisher, fixture.Render, first, 2, 2, firstPixels) == LiveImagePublishStatus::Published);
	const auto reboundResult = publisher.BeginBinding(owner, name);
	REQUIRE(reboundResult.has_value());
	const LiveImageBinding rebound = *reboundResult;
	CHECK(
		Publish(publisher, fixture.Render, rebound, 4, 1, reboundPixels) == LiveImagePublishStatus::Published
	);
	CHECK_FALSE(publisher.Retire(fixture.Render, first));
	CHECK(Publish(publisher, fixture.Render, first, 2, 2, firstPixels) == LiveImagePublishStatus::Stale);
	uint32_t width = 0, height = 0;
	REQUIRE(fixture.Render.TextureSize(name, width, height, owner));
	CHECK(width == 4);
	CHECK(height == 1);

	const auto failedResult = publisher.BeginBinding(owner, name);
	REQUIRE(failedResult.has_value());
	const LiveImageBinding failed = *failedResult;
	// A malformed replacement is refused before the GPU table is changed.
	const std::vector<std::byte> malformed(15, std::byte{7});
	CHECK(Publish(publisher, fixture.Render, failed, 2, 2, malformed) == LiveImagePublishStatus::Invalid);
	REQUIRE(fixture.Render.TextureSize(name, width, height, owner));
	CHECK(width == 4);
	CHECK(height == 1);
	CHECK(
		Publish(publisher, fixture.Render, failed, 1, 1, replacementPixels) ==
		LiveImagePublishStatus::Published
	);
	REQUIRE(fixture.Render.TextureSize(name, width, height, owner));
	CHECK(width == 1);
	CHECK(height == 1);
	CHECK(publisher.Retire(fixture.Render, failed));
	CHECK(
		Publish(publisher, fixture.Render, failed, 1, 1, replacementPixels) == LiveImagePublishStatus::Stale
	);
	CHECK(publisher.ActiveBindingCount() == 0);
}

TEST_CASE("live image input is bounded before copying", "[render][live-image]") {
	engine::render::LiveImagePublisher publisher;
	engine::render::Renderer renderer;
	const auto result = publisher.BeginBinding(Name("live-image:bounded"), Name("live-image:oversized"));
	REQUIRE(result.has_value());
	const LiveImageBinding binding = *result;
	const std::vector<std::byte> pixels;

	CHECK(
		Publish(
			publisher, renderer, binding, engine::render::LiveImagePublisher::MAXIMUM_SIDE + 1, 1, pixels
		) == LiveImagePublishStatus::Invalid
	);
	CHECK(publisher.Retire(renderer, binding));
}

TEST_CASE("six live bindings publish in one texture generation", "[render][gpu][live-image][.]") {
	engine::render::test::FixtureDevice fixture;
	REQUIRE(fixture.VideoReady);
	REQUIRE(fixture.Render.Initialise(nullptr, 1, true));
	engine::render::LiveImagePublisher publisher;
	const Name owner("live-image:sky-owner");
	std::array<Name, 6> names{
		Name("live-image:sky-0"),
		Name("live-image:sky-1"),
		Name("live-image:sky-2"),
		Name("live-image:sky-3"),
		Name("live-image:sky-4"),
		Name("live-image:sky-5")
	};
	std::array<LiveImageBinding, 6> bindings;
	std::array<std::vector<std::byte>, 6> pixels;
	std::array<engine::render::LiveImageUpload, 6> uploads;
	for (size_t index = 0; index < names.size(); ++index) {
		const auto binding = publisher.BeginBinding(owner, names[index]);
		REQUIRE(binding.has_value());
		bindings[index] = *binding;
		pixels[index] = Pixels(1, 1, static_cast<uint8_t>(index + 20));
		uploads[index] = {bindings[index], 1, 1, pixels[index]};
	}
	CHECK(publisher.PublishBatch(fixture.Render, uploads) == LiveImagePublishStatus::Published);
	for (size_t index = 0; index < names.size(); ++index) {
		engine::assets::TextureData retained;
		REQUIRE(
			fixture.Render.CopyTexture(names[index], retained, 4, owner) ==
			engine::render::TextureCopyStatus::Copied
		);
		CHECK(retained.Pixels == pixels[index]);
	}
	const auto newer = publisher.BeginBinding(owner, names[5]);
	REQUIRE(newer.has_value());
	CHECK(publisher.PublishBatch(fixture.Render, uploads) == LiveImagePublishStatus::Stale);
	for (size_t index = 0; index < names.size(); ++index) {
		engine::assets::TextureData retained;
		REQUIRE(
			fixture.Render.CopyTexture(names[index], retained, 4, owner) ==
			engine::render::TextureCopyStatus::Copied
		);
		CHECK(retained.Pixels == pixels[index]);
	}
	uploads[5].Binding = *newer;
	CHECK(publisher.PublishBatch(fixture.Render, uploads) == LiveImagePublishStatus::Published);
	CHECK(publisher.RetireOwner(fixture.Render, owner) == 6);
}

TEST_CASE("retired world bindings free slots and reject delayed work", "[render][live-image]") {
	engine::render::LiveImagePublisher publisher;
	engine::render::Renderer renderer;
	const Name owner("live-image:reused-owner");
	const std::vector<std::byte> pixels = Pixels(1, 1, 42);

	for (size_t cycle = 0; cycle < engine::render::LiveImagePublisher::MAXIMUM_BINDINGS + 32; cycle++) {
		const Name name("live-image:world-output:" + std::to_string(cycle));
		const auto initialResult = publisher.BeginBinding(owner, name);
		REQUIRE(initialResult.has_value());
		const LiveImageBinding initial = *initialResult;
		CHECK(publisher.ActiveBindingCount() == 1);
		CHECK(publisher.RetireOwner(renderer, owner) == 1);
		CHECK(publisher.ActiveBindingCount() == 0);
		CHECK(Publish(publisher, renderer, initial, 1, 1, pixels) == LiveImagePublishStatus::Stale);

		const auto reopenedResult = publisher.BeginBinding(owner, name);
		REQUIRE(reopenedResult.has_value());
		const LiveImageBinding reopened = *reopenedResult;
		CHECK(reopened.Generation > initial.Generation);
		CHECK(Publish(publisher, renderer, initial, 1, 1, pixels) == LiveImagePublishStatus::Stale);
		CHECK(Publish(publisher, renderer, reopened, 1, 1, pixels) == LiveImagePublishStatus::UploadFailed);
		CHECK(publisher.Retire(renderer, reopened));
		CHECK(publisher.ActiveBindingCount() == 0);
	}
}
