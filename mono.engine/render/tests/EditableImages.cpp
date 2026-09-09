// Host conversion and device upload checks for editable resource ownership.

#include "RenderFixture.hpp"

#include <engine/assets/Texture.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/render/EditableImages.hpp>
#include <engine/scene/EditableImage.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.render.editableimages")
TEST_DEPENDS("engine.scene.editableimage")

using engine::scene::EditableImage;

TEST_CASE("editable image uploads follow store and owner lifetimes", "[render][gpu][editable-owner][.]") {
	using namespace engine;
	scene::RegisterSceneClasses();
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	ecs::Store first("editable.first"), second("editable.second");
	const auto firstImage = first.CreateInstance(scene::EditableImageClass(), "image");
	const auto secondImage = second.CreateInstance(scene::EditableImageClass(), "image");
	REQUIRE(firstImage == secondImage);
	EditableImage small, large;
	small.Width = small.Height = 2;
	small.Pixels.assign(16, 255);
	large.Width = large.Height = 4;
	large.Pixels.assign(64, 127);
	first.Set(firstImage, small);
	second.Set(secondImage, large);
	const core::Name firstOwner("editable:first"), secondOwner("editable:second");
	const auto name = scene::EditableImageContentName(first, firstImage);
	CHECK(name == scene::EditableImageContentName(second, secondImage));
	render::EditableImageUploader uploader;
	CHECK(uploader.Refresh(first, fixture.Render, firstOwner) == 1);
	CHECK(uploader.Refresh(second, fixture.Render, secondOwner) == 1);
	const auto secondHandle = fixture.Render.TextureHandle(name, secondOwner);
	REQUIRE(secondHandle != nullptr);
	CHECK(fixture.Render.TextureHandle(name, firstOwner) != secondHandle);
	CHECK(uploader.Refresh(first, fixture.Render, firstOwner) == 0);
	CHECK(uploader.Refresh(second, fixture.Render, secondOwner) == 0);
	uint32_t width = 0, height = 0;
	REQUIRE(fixture.Render.TextureSize(name, width, height, firstOwner));
	CHECK(width == 2);
	REQUIRE(fixture.Render.TextureSize(name, width, height, secondOwner));
	CHECK(width == 4);
	CHECK(fixture.Render.TextureHandle(name) == nullptr);
	REQUIRE(scene::ResizeEditableImage(first, firstImage, 8, 8));
	CHECK(uploader.Refresh(first, fixture.Render, firstOwner) == 1);
	CHECK(fixture.Render.TextureHandle(name, secondOwner) == secondHandle);
	uploader.ForgetWorld(first.Identity());
	CHECK(uploader.Refresh(first, fixture.Render, firstOwner) == 1);
	CHECK(uploader.Refresh(second, fixture.Render, secondOwner) == 0);
	fixture.Render.DropContentOwner(firstOwner);
	uploader.ForgetOwner(firstOwner);
	CHECK(uploader.Refresh(first, fixture.Render, firstOwner) == 1);
	CHECK(uploader.Refresh(second, fixture.Render, secondOwner) == 0);
	// A store can also move to a new residency owner without changing its pixels.
	CHECK(uploader.Refresh(first, fixture.Render, core::Name("editable:rebound")) == 1);
	CHECK(uploader.Refresh(first, fixture.Render, firstOwner) == 0);
}

TEST_CASE("a fresh EditableImage converts to a valid, matching TextureData", "[render][editableimages]") {
	EditableImage image;

	const engine::assets::TextureData built = engine::render::BuildTextureData(image);

	CHECK(built.Width == image.Width);
	CHECK(built.Height == image.Height);
	CHECK(built.Format == engine::assets::TextureFormat::RGBA8);
	REQUIRE(built.Pixels.size() == image.Pixels.size());
	CHECK(built.IsValid());
}

TEST_CASE("the pixel bytes cross unchanged", "[render][editableimages]") {
	EditableImage image;
	image.Width = 2;
	image.Height = 1;
	image.Pixels.assign(2 * 1 * 4, 0);
	image.Pixels[0] = 200; // R of the first pixel
	image.Pixels[7] = 77;  // A of the second pixel

	const engine::assets::TextureData built = engine::render::BuildTextureData(image);

	REQUIRE(built.Pixels.size() == image.Pixels.size());
	CHECK(static_cast<uint8_t>(built.Pixels[0]) == 200);
	CHECK(static_cast<uint8_t>(built.Pixels[7]) == 77);
}
