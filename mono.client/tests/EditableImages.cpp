// Converting `scene::EditableImage`'s raw pixels into `assets::TextureData`.
//
// **The device-free half** - `client/tests/EditableMeshes.cpp`'s own header
// carries the full argument for why this and not `EditableImageUploader::
// Refresh` is what gets a unit suite.

#include <engine/assets/Texture.hpp>
#include <engine/scene/EditableImage.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <client/EditableImages.hpp>

TEST_SUITE_ID("client.editableimages")
TEST_DEPENDS("engine.scene.editableimage")

using engine::scene::EditableImage;

TEST_CASE("a fresh EditableImage converts to a valid, matching TextureData", "[client][editableimages]") {
	EditableImage image;

	const engine::assets::TextureData built = client::BuildTextureData(image);

	CHECK(built.Width == image.Width);
	CHECK(built.Height == image.Height);
	CHECK(built.Format == engine::assets::TextureFormat::RGBA8);
	REQUIRE(built.Pixels.size() == image.Pixels.size());
	CHECK(built.IsValid());
}

TEST_CASE("the pixel bytes cross unchanged", "[client][editableimages]") {
	EditableImage image;
	image.Width = 2;
	image.Height = 1;
	image.Pixels.assign(2 * 1 * 4, 0);
	image.Pixels[0] = 200; // R of the first pixel
	image.Pixels[7] = 77;  // A of the second pixel

	const engine::assets::TextureData built = client::BuildTextureData(image);

	REQUIRE(built.Pixels.size() == image.Pixels.size());
	CHECK(static_cast<uint8_t>(built.Pixels[0]) == 200);
	CHECK(static_cast<uint8_t>(built.Pixels[7]) == 77);
}
