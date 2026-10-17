#include <engine/render/PortalResidentImages.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.render.portalresidentimages")
TEST_DEPENDS("engine.render.portalexchange")

TEST_CASE(
	"resident capture reservations bind endpoint incarnations and exact requests", "[render][portal-resident]"
) {
	using namespace engine;
	using namespace engine::render;
	Renderer renderer, other;
	PortalResidentImages images(renderer);
	CHECK(images.Owns(renderer));
	CHECK_FALSE(images.Owns(other));
	const world::PresentationAddress source{"near", "replies", 1, 2};
	const world::PresentationAddress producer{"far", "requests", 3, 4};
	PortalImageRequest request;
	request.Key = {1, "Door", 2, 3};
	request.Width = request.Height = 8;
	request.PixelBudget = 64;
	PortalImageBinding binding;
	binding.WorldName = core::Name("near");
	binding.Portal = core::Name("Door");
	binding.Expected = request.Key;
	const PortalResidentImages::Time now{};
	REQUIRE(images.Reserve(source, producer, request, binding, now));
	CHECK_FALSE(images.Reserve(source, producer, request, binding, now));
	CHECK(images.Contains(source, producer, request, now));
	auto changed = request;
	changed.Position[0] = 1;
	CHECK_FALSE(images.Contains(source, producer, changed, now));
	auto recreated = producer;
	recreated.Generation++;
	CHECK_FALSE(images.Contains(source, recreated, request, now));
	images.Invalidate(recreated);
	CHECK(images.Contains(source, producer, request, now));
	images.Cancel(source, 2);
	CHECK(images.Contains(source, producer, request, now));
	images.Cancel(source, 1);
	CHECK_FALSE(images.Contains(source, producer, request, now));
	for (uint64_t id = 1; id <= MAX_IMPORTED_PORTAL_IMAGES; ++id) {
		request.Key.RequestId = id;
		binding.Expected = request.Key;
		REQUIRE(images.Reserve(source, producer, request, binding, now));
	}
	request.Key.RequestId++;
	binding.Expected = request.Key;
	CHECK_FALSE(images.Reserve(source, producer, request, binding, now));
	CHECK(images.Expire(now + std::chrono::seconds(1)));
	CHECK_FALSE(images.Expire(now));
	CHECK_FALSE(images.Expire(PortalResidentImages::Time::max()));
	REQUIRE(images.Reserve(source, producer, request, binding, now + std::chrono::seconds(1)));
	images.Invalidate(producer);
	CHECK_FALSE(images.Contains(source, producer, request, now + std::chrono::seconds(1)));
}
