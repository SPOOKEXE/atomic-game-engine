#include <engine/core/Bytes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/scene/EditableImage.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <utility>
#include <vector>

TEST_SUITE_ID("engine.scene.editableimage.snapshot")

TEST_CASE(
	"editable image snapshot refuses mismatched declared bytes before resizing pixels",
	"[scene][editableimage][snapshot]"
) {
	using namespace engine;
	scene::RegisterSceneComponents();
	const auto &type = ecs::Components::Describe(ecs::Components::Of<scene::EditableImage>());
	CHECK(type.MaximumSerialisedBytes == scene::MAXIMUM_EDITABLE_IMAGE_PIXELS * 4 + 128);
	scene::EditableImage image;
	image.Width = image.Height = 1;
	image.Pixels = {1, 2, 3, 4};
	core::ByteWriter writer;
	writer.WriteUInt32(1);
	writer.WriteUInt32(1);
	writer.WriteUInt32(1024 * 1024);
	// A short hostile payload must never resize a valid prior image.
	core::ByteReader reader(writer.Bytes());
	type.Read(reader, &image, 1);
	CHECK(reader.Failed());
	CHECK(image.Width == 1);
	CHECK(image.Height == 1);
	CHECK(image.Pixels.size() == 4);
	CHECK((image.Pixels == std::vector<uint8_t>{1, 2, 3, 4}));
}

TEST_CASE(
	"editable image snapshots retain encoded colour space and reject unknown spaces transactionally",
	"[scene][editableimage][snapshot]"
) {
	using namespace engine;
	scene::RegisterSceneComponents();
	const auto &type = ecs::Components::Describe(ecs::Components::Of<scene::EditableImage>());
	scene::EditableImage image;
	image.Width = image.Height = 1;
	image.Pixels = {255, 64, 0, 255};
	image.Space = scene::EditableImageSpace::SRGB;
	image.Revision = 7;
	core::ByteWriter writer;
	type.Write(writer, &image, 1);
	scene::EditableImage restored;
	core::ByteReader reader(writer.Bytes());
	type.Read(reader, &restored, 1);
	REQUIRE_FALSE(reader.Failed());
	CHECK(restored.Space == scene::EditableImageSpace::SRGB);
	CHECK((restored.Pixels == image.Pixels));
	CHECK(restored.Revision == 7);

	std::vector<std::byte> invalid(writer.Bytes().begin(), writer.Bytes().end());
	invalid.back() = std::byte{'X'};
	core::ByteReader corrupt(invalid);
	type.Read(corrupt, &restored, 1);
	CHECK(corrupt.Failed());
	CHECK(restored.Space == scene::EditableImageSpace::SRGB);
	CHECK((restored.Pixels == image.Pixels));
}

TEST_CASE(
	"editable image snapshot dimensions and truncated pixels are bounded before replacement",
	"[scene][editableimage][snapshot]"
) {
	using namespace engine;
	scene::RegisterSceneComponents();
	const auto &type = ecs::Components::Describe(ecs::Components::Of<scene::EditableImage>());
	for (const auto &dimensions :
		 {std::pair{0u, 1u}, std::pair{0xffffffffu, 0xffffffffu}, std::pair{2u, 2u}}) {
		scene::EditableImage image;
		image.Width = image.Height = 1;
		image.Pixels = {1, 2, 3, 4};
		core::ByteWriter writer;
		writer.WriteUInt32(dimensions.first);
		writer.WriteUInt32(dimensions.second);
		writer.WriteUInt32(16);
		core::ByteReader reader(writer.Bytes());
		type.Read(reader, &image, 1);
		CHECK(reader.Failed());
		CHECK(image.Width == 1);
		CHECK(image.Height == 1);
		CHECK((image.Pixels == std::vector<uint8_t>{1, 2, 3, 4}));
	}
}
