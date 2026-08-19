// The pixel buffer a script paints into, and the three primitives that
// reach it.
//
// **What is pinned here is the storage `client::UpdateEditableImages`
// converts from - that half is L12 and this is what it consumes.** Blending
// is the one piece of arithmetic worth a direct test: a transparent draw
// over an existing pixel must darken it towards the new colour rather than
// replace it outright, which is the whole difference between "painting" and
// "stamping."

#include <engine/ecs/Store.hpp>
#include <engine/scene/EditableImage.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.scene.editableimage")

using Catch::Approx;
using engine::core::Color3;
using engine::core::Name;
using engine::core::Vector2;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;
using engine::scene::DrawCircle;
using engine::scene::DrawLine;
using engine::scene::DrawRectangle;
using engine::scene::EditableImage;
using engine::scene::EditableImageClass;
using engine::scene::EditableImageContentName;
using engine::scene::ResizeEditableImage;

namespace {
	Entity MakeEditableImage(Store &store) {
		return store.CreateInstance(EditableImageClass(), "Image");
	}

	// The four bytes at one pixel, or all-zero for one out of range.
	std::array<uint8_t, 4> PixelAt(const EditableImage &image, uint32_t x, uint32_t y) {
		if (x >= image.Width || y >= image.Height) {
			return {0, 0, 0, 0};
		}
		const size_t offset = (static_cast<size_t>(y) * image.Width + x) * 4;
		return {
			image.Pixels[offset], image.Pixels[offset + 1], image.Pixels[offset + 2], image.Pixels[offset + 3]
		};
	}
}

TEST_CASE("a fresh EditableImage is the default size and fully transparent", "[scene][editableimage]") {
	Store store("editableimage.fresh");
	const Entity image = MakeEditableImage(store);

	const EditableImage *held = store.Get<EditableImage>(image);
	REQUIRE(held != nullptr);
	CHECK(held->Width == engine::scene::DEFAULT_EDITABLE_IMAGE_SIZE);
	CHECK(held->Height == engine::scene::DEFAULT_EDITABLE_IMAGE_SIZE);
	CHECK(held->Pixels.size() == static_cast<size_t>(held->Width) * held->Height * 4);
	for (const uint8_t byte : held->Pixels) {
		CHECK(byte == 0);
	}
}

TEST_CASE("Resize reallocates and clears, and refuses an absurd size", "[scene][editableimage]") {
	Store store("editableimage.resize");
	const Entity image = MakeEditableImage(store);

	REQUIRE(DrawRectangle(store, image, Vector2{0, 0}, Vector2{10, 10}, Color3{1.0f, 1.0f, 1.0f}));
	REQUIRE(ResizeEditableImage(store, image, 8, 4));

	const EditableImage *held = store.Get<EditableImage>(image);
	CHECK(held->Width == 8);
	CHECK(held->Height == 4);
	CHECK(held->Pixels.size() == 8 * 4 * 4);
	CHECK(PixelAt(*held, 0, 0)[3] == 0); // The old rectangle did not survive.

	CHECK_FALSE(ResizeEditableImage(store, image, 1u << 20, 1u << 20));
}

TEST_CASE("DrawRectangle fills exactly its own footprint, clipped to the image", "[scene][editableimage]") {
	Store store("editableimage.rectangle");
	const Entity image = MakeEditableImage(store);
	REQUIRE(ResizeEditableImage(store, image, 16, 16));

	REQUIRE(DrawRectangle(store, image, Vector2{4, 4}, Vector2{4, 4}, Color3{1.0f, 0.0f, 0.0f}));

	const EditableImage *held = store.Get<EditableImage>(image);
	CHECK(PixelAt(*held, 4, 4)[0] == 255);
	CHECK(PixelAt(*held, 7, 7)[0] == 255);
	CHECK(PixelAt(*held, 8, 8)[3] == 0); // Just outside the rectangle.
	CHECK(PixelAt(*held, 3, 4)[3] == 0); // Just outside the other edge.

	// Clipped rather than refused: a rectangle that runs off the image draws
	// what fits.
	REQUIRE(DrawRectangle(store, image, Vector2{14, 14}, Vector2{10, 10}, Color3{0.0f, 1.0f, 0.0f}));
	CHECK(PixelAt(*held, 15, 15)[1] == 255);
}

TEST_CASE(
	"a transparent draw blends towards the new colour rather than replacing it", "[scene][editableimage]"
) {
	Store store("editableimage.blend");
	const Entity image = MakeEditableImage(store);
	REQUIRE(ResizeEditableImage(store, image, 4, 4));

	REQUIRE(DrawRectangle(store, image, Vector2{0, 0}, Vector2{4, 4}, Color3{1.0f, 1.0f, 1.0f}));
	REQUIRE(DrawRectangle(store, image, Vector2{0, 0}, Vector2{4, 4}, Color3{0.0f, 0.0f, 0.0f}, 0.5f));

	const EditableImage *held = store.Get<EditableImage>(image);
	const auto pixel = PixelAt(*held, 1, 1);
	// Half white blended with half black lands at half grey, not at black -
	// a `transparency` of 1 would draw nothing at all, and 0.5 draws half
	// as much of the new colour as it would at full strength.
	CHECK(static_cast<int>(pixel[0]) == Approx(127).margin(2));
	CHECK(static_cast<int>(pixel[3]) == 255); // Coverage is still full.
}

TEST_CASE("DrawLine reaches both endpoints", "[scene][editableimage]") {
	Store store("editableimage.line");
	const Entity image = MakeEditableImage(store);
	REQUIRE(ResizeEditableImage(store, image, 10, 10));

	REQUIRE(DrawLine(store, image, Vector2{0, 0}, Vector2{9, 9}, Color3{1.0f, 1.0f, 1.0f}));

	const EditableImage *held = store.Get<EditableImage>(image);
	CHECK(PixelAt(*held, 0, 0)[3] == 255);
	CHECK(PixelAt(*held, 9, 9)[3] == 255);
	// A 45-degree line over a square canvas passes through the middle.
	CHECK(PixelAt(*held, 5, 5)[3] == 255);
}

TEST_CASE("DrawCircle fills the disc and leaves the corners of its box untouched", "[scene][editableimage]") {
	Store store("editableimage.circle");
	const Entity image = MakeEditableImage(store);
	REQUIRE(ResizeEditableImage(store, image, 20, 20));

	REQUIRE(DrawCircle(store, image, Vector2{10, 10}, 5.0f, Color3{1.0f, 0.5f, 0.0f}));

	const EditableImage *held = store.Get<EditableImage>(image);
	CHECK(PixelAt(*held, 10, 10)[3] == 255); // The centre.
	CHECK(PixelAt(*held, 10, 5)[3] == 255);	 // On the radius, straight up.
	CHECK(PixelAt(*held, 0, 0)[3] == 0);	 // Corner of the bounding box, outside the disc.
}

TEST_CASE("every door refuses an instance that is not an EditableImage", "[scene][editableimage]") {
	Store store("editableimage.wrongtype");
	const Entity notAnImage = store.Create();

	CHECK_FALSE(ResizeEditableImage(store, notAnImage, 8, 8));
	CHECK_FALSE(DrawRectangle(store, notAnImage, Vector2{}, Vector2{1, 1}, Color3{}));
	CHECK_FALSE(DrawLine(store, notAnImage, Vector2{}, Vector2{1, 1}, Color3{}));
	CHECK_FALSE(DrawCircle(store, notAnImage, Vector2{}, 1.0f, Color3{}));
	CHECK_FALSE(EditableImageContentName(store, notAnImage).IsValid());
	CHECK_FALSE(EditableImageContentName(store, NULL_ENTITY).IsValid());
}

TEST_CASE("the content name is stable and distinct per instance", "[scene][editableimage]") {
	Store store("editableimage.contentname");
	const Entity a = MakeEditableImage(store);
	const Entity b = MakeEditableImage(store);

	const Name nameA = EditableImageContentName(store, a);
	const Name nameB = EditableImageContentName(store, b);
	REQUIRE(nameA.IsValid());
	REQUIRE(nameB.IsValid());
	CHECK(nameA != nameB);
	CHECK(EditableImageContentName(store, a) == nameA);
}
